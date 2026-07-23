# Decision log (C rewrite)

Format: ID, status (proposed/accepted/superseded), context, decision,
consequences. Phase 0 entries are **proposed** unless marked otherwise —
final acceptance happens when the implementing phase starts.

## D-01 — Source layout `src/backend-c/`

Status: proposed. Adopt the spec's suggested tree (include/magitrickle/ +
src/<module> + tests/{unit,integration,differential,fuzz,benchmarks}).
Go backend stays at `src/backend/` untouched until Phase 9. Makefile gains
parallel `build_backend_c` until switchover.

## D-02 — Event-driven core, epoll, small fixed thread count

Status: proposed. Single epoll loop thread for DNS UDP/TCP + timers +
signals; a small worker pool (N=2 default, configurable) only for
blocking/slow work: iptables fork+exec, libcurl transfers, shadow-file
crypt (SHA-crypt with high rounds is CPU-heavy — must not stall DNS).
Netfilter ipset adds via netlink are non-blocking-fast; measure first
(Phase 5) before moving them off-loop. Justification vs alternatives to be
re-validated by Phase 1 spike per spec §12 (not chosen "because the spec
said epoll": the workload is thousands of tiny independent exchanges —
thread-per-request is the Go model we're explicitly replacing to cut RSS).
Every queue bounded with drop counters; overflow policy per queue
documented in code and in `current` docs.

## D-03 — DNS parser: own minimal implementation

Status: proposed. See dependencies.md. Raw pass-through preserved for the
common path (bytes in → bytes out), parse only what hooks need — mirrors
Go behaviour incl. "hooks force parse" semantics. Full fuzz coverage
mandatory before Phase 3 exit.

## D-04 — YAML via libyaml with Go-yaml-v2 compatibility shims

Status: **accepted** (Phase 1 spike, `src/backend-c/spikes/yaml_emit/`).
libyaml event API with indent=2, width=-1 воспроизводит вывод yaml.v2
**байт-в-байт** (895-байтовый config-fixture идентичен), включая стили:
plain для обычных скаляров, single-quoted для строк с `[`/`#`/`*`,
flow `[]` для пустых списков. Требуемые shims: явный порядок ключей,
duration-строки (`5s`, `1h0m0s`), выбор quote-стиля по правилам yaml.v2
(нужен небольшой классификатор скаляров в Phase 2). Duration parsing
(строки + int=наносекунды + legacy ms/s нормализация) — на стороне load.

## D-05 — JSON via cJSON

Status: **accepted** (Phase 6). Dynamically linked (`libcjson`, MIT,
confirmed available in both OpenWrt and Entware feeds per
dependencies.md), same pattern as libyaml/PCRE2/libmnl — not vendored.
`include/magitrickle/json.h` wraps it with two helpers matching
`api/utils/helpers.go`'s shape: `mt_json_error()` builds `{"error":"..."}`
(Go's `types.ErrorRes`), `mt_json_dump()` serializes compact (no
whitespace), matching Go's default un-indented `json.Marshal` output.
Byte-identical serialization with Go (e.g. Go's default HTML-escaping of
`<`/`>`/`&` in strings) is explicitly NOT a goal — the API compatibility
contract is verified by structural/normalized comparison (parse both
sides, compare values), per migration-plan.md's Phase 6 description; a
JSON parser reads an escaped and a literal `<` identically, so this only
affects wire bytes, never observable behaviour.

## D-06 — HTTP server: own bounded HTTP/1.1 on the shared loop

Status: proposed, pending Phase 1 spike vs libmicrohttpd. If the spike
shows >2 weeks of hardening effort, fall back to libmicrohttpd (LGPL,
in feeds). Hard limits (header 8 KB, body 1 MB, conns 64, timeouts) —
documented hardening, covered by tests.

## D-07 — Regex: PCRE2 with corpus-proven parity + explicit failure mode

Status: **accepted** (Phase 1 spike, `src/backend-c/spikes/regex_corpus/`).
Corpus of 47 cases (repo tests + user-style patterns + .NET constructs):
44/47 identical with `PCRE2_CASELESS|PCRE2_UTF|PCRE2_UCP` vs
`regexp2.IgnoreCase`. Divergences (frozen in `known_divergences.tsv`):
1. POSIX classes `[[:alpha:]]` — regexp2 не поддерживает (тихо не матчит),
   PCRE2 матчит корректно. Итог: С-версия «чинит» ранее сломанные паттерны.
2. Possessive quantifiers (`a*+`) — regexp2 compile_error, PCRE2 работает.
   Ранее нерабочие правила начнут работать.
3. .NET balancing groups (`(?<-name>…)`) — regexp2 работает, PCRE2
   compile_error. Единственная реальная потеря; для доменного матчинга
   экзотика. Политика: явная ошибка при загрузке, без автопереписывания.
Lookahead/lookbehind, backrefs, named groups (обе формы `(?<n>`/`(?'n'`),
inline options, `\A/\z/\Z`, `\p{L}`, atomic groups, conditionals — parity
подтверждён. Match/depth limits включены (1e6/1e4) как hardening.
Phase 2: прогнать корпус, расширенный реальными пользовательскими списками.

## D-08 — ipset/route/link via libmnl; iptables via save/restore exec

Status: proposed. Keeps the proven Go architecture (contract §7); fake
executable seam retained for tests; transcripts from
`utils/iptables/iptables_test.go` become shared fixtures.

## D-09 — HTTP client via libcurl

Status: proposed. https subscriptions need real TLS; manual redirect loop
to preserve exact 301/302-only, ≤5, loop-detect semantics. Response size
bound added (hardening).

## D-10 — Config snapshot swap instead of Go's unlocked config mutation

Status: proposed. Go mutates `App.config` fields during SIGHUP reload with
no lock (data race, benign today). C uses immutable config snapshot +
atomic pointer swap; observable behaviour unchanged.

## D-11 — Process exit codes

Status: proposed. Go exits 0 even when `Start` fails (error only logged).
Init systems (rc.func, procd respawn) treat death as restartable either
way. C will exit non-zero on startup failure — divergence flagged for
review in Phase 1 (procd respawn behaviour must be re-checked; if it causes
respawn storms on permanent config errors, keep exit 0 for parity).

## D-12 — Rule-set snapshots (RCU-light) for the match path

Status: proposed. Match path reads an immutable snapshot (refcounted);
writers build+swap. No lock-free structures beyond atomic pointer + rc.

## D-13 — UPX decision deferred to measurements

Status: accepted (spec). Not applied automatically to C builds; Phase 8
measures startup/RSS impact.

## D-14 — Language/base: C11 + documented GNU/Linux extensions in platform layer

Status: accepted (spec). Extensions (epoll, timerfd, signalfd, pktinfo,
accept4) live under `src/platform/` and are listed in its header docs.

## D-15 — `subnet`/`subnet6` stay out of DNS matching

Status: accepted (verified in code). They are materialized into ipset by
sync only; `IsMatch` returns false. The C rule engine keeps this split
(contract §6).

## D-16 — Benchmark scope on shared CI host

Status: accepted. 1000-concurrency and on-device cells deferred (noise /
no hardware); documented in benchmark-methodology.md; device runs are a
Phase 5/8 obligation, scripts already parameterized (`GROUP_ENABLE=1`).

## D-17 — No locking in the cache/matching hot path (single event-loop thread)

Status: accepted (Phase 4). D-02/D-12 anticipated RCU-style snapshots for
a *possibly* multi-threaded reader set. In the implemented architecture
(D-02) the DNS proxy, the records cache, and rule matching all run
exclusively on the single `mt_loop` thread — there are no concurrent
readers to guard against, so `dns_cache` and `rulesnap` take no locks and
the "atomic swap" of a rule-set snapshot is just a plain pointer store
from that same thread. This satisfies the Phase 4 exit criterion ("no
global lock in match path") by construction rather than by adding
synchronization primitives that would have nothing to protect against
yet. When Phase 7 introduces a worker thread for subscription fetch
(libcurl calls, D-02), the *build* step may run there, but the resulting
snapshot's *publication* must be marshalled onto the loop thread via
`mt_loop_post` before any reader sees it — at that point this decision
gets revisited with real cross-thread publication (refcounting or a
generation counter), not before.

## D-18 — Rule-set snapshot matches per-group as an aggregate OR, not Go's per-rule loop

Status: accepted (Phase 4). Go's `dns.go` processes each group's rules in
config order with two different early-exit idioms: A/AAAA records use
`break Rule` (stop the whole group after the first matching rule),
CNAME records use `continue Rule` (keep checking every rule in the group,
independently deciding per rule whether to act). Both idioms feed the
*same* action inputs regardless of which specific rule fired — A/AAAA
always adds the same (IP, TTL) pair, CNAME always adds the same cached
address set with the same per-address remaining TTL — so the *final
observable state* (what ends up in a group's ipset) is identical whether
you evaluate "did any enabled rule in this group match any candidate
name" once, or replay Go's rule-by-rule loop. The C rule-set snapshot
(`rulesnap.h`) therefore builds one aggregate `mt_matcher_t` per enabled
group (OR over its enabled rules) and asks it once per (group, DNS
record) pair.

The one thing this changes: Go's CNAME path can invoke `AddIPv4Subnet`/
`AddIPv6Subnet` multiple times for a single response when several rules
in the same group independently match different aliases (each call is
idempotent — same subnet, same TTL, `Replace: true` — so it is pure
redundant netlink traffic, explicitly called out as an optimization
target in spec §16 "уменьшить... количество netlink round trips"). The C
path collapses this to one call. No difference in resulting ipset
contents; fewer redundant operations. Documented here per the "не
исправляй несовместимое поведение молча" rule rather than left as a
silent divergence.

Disabled groups are excluded from the snapshot entirely (not just
skipped at match time): Go's `RuleSet.AddIPv4Subnet` no-ops whenever the
group's runtime/model enable flag is off, so a disabled group can never
produce an observable action regardless of whether its rules match —
there is nothing to gain from building or querying an index for it.

Subscription-derived synthetic groups are not part of the snapshot yet
(subscription sync lands in Phase 7); only `mt_config_t.groups` (user
groups) participate today.

## D-19 — Netfilter mutation is single-threaded; no per-object locking; insertion-ordered iteration

Status: accepted (Phase 5). Go's `iptables.IPTables`, `netfilterTools.IPSet`
and `IPSetToLink` each guard every method with `sync.Mutex` (some also
`atomic.Bool`) because multiple goroutines can share one instance
(HTTP API handlers, the DNS hot path, and `start.go`'s init loop all call
into the same objects concurrently in Go). The C backend instead commits
to running all netfilter mutation — iptables engine commits, ipset
create/add/del, ipset-to-link enable/disable, rtnetlink rule/route calls
— on the single event-loop thread (extending D-17's DNS-hot-path
reasoning to the whole netfilter layer); `mt_ipt_t`, `mt_ipset_t`,
`mt_ipset_to_link_t`, `mt_ruleset_t` and `mt_rtnl_t` therefore carry no
internal lock. Callers must not share one instance across threads without
external synchronization; when a future phase adds worker threads (e.g.
Phase 7's subscription fetch), calls into these objects must be
marshalled onto the loop thread via `mt_loop_post`, not called directly
from the worker.

A related, purely representational difference: Go stores per-table
per-chain registrations in `map[string]map[string]chain`, whose iteration
order is randomized by the Go runtime on every `Commit()`; `mt_ipt_t`
(engine.c) uses insertion-ordered arrays instead. This is not an
observable behavioural difference — within one priority bucket a given
chain's own compiled commands stay contiguous, and the relative order
between *different* chains/tables in the iptables-restore transcript is
insignificant to iptables-restore (distinct named chains/tables are
independent) — but it is called out here per the "no silent incompatible
fix" rule, matching the pattern of D-18.

## D-20 — ipset: dedicated per-group netlink socket; wire protocol pinned to version 6

Status: accepted (Phase 5). Two related choices in `netfilter/ipset.c` /
`ipset_nl_real.c`:

1. Go's `netfilterTools.Helper` hands every `IPSet` the same package-level
   netlink handle (`vishvananda/netlink`'s internal `pkgHandle`), so all
   groups share one socket. The C port instead gives each `mt_ipset_t`
   its own dedicated socket via `mt_ipset_nl_real_new()` (one per
   enabled group, since `mt_ruleset_t` owns its `mt_ipset_t`). This keeps
   `mt_ipset_t`'s existing (already-tested) ownership contract simple —
   `mt_ipset_free` owns and closes exactly the transport it was
   constructed with — at the cost of one extra file descriptor per group
   versus Go. On any device this backend targets, group counts are small
   (tens, not thousands), so the extra fd count is not expected to be
   material; this is an accepted, documented inefficiency rather than a
   behavioural divergence.
2. The NFNETLINK wire format (attribute set, flag bits, `hash:net`
   revision 0) is ported from what `github.com/vishvananda/netlink`
   actually emits, which pins `IPSET_PROTOCOL` to `6` — not the current
   kernel UAPI header's default of `7`. This is deliberate: the C backend
   must speak the same wire protocol the Go binary already sends in
   production, not whatever a newer kernel header advertises as current.

Both choices are untestable against a real kernel in the CI/dev sandbox
(no `ip_set` module available, confirmed Phase 0 and reconfirmed Phase 5);
only the fake in-memory transport (`tests/unit/fake_ipset_nl.c`) exercises
`mt_ipset_t`'s logic today. On-device validation remains a prerequisite
before this is trusted on a real router (see phase-5-report.md).

## D-21 — rtnetlink: verify functional equivalence on real kernel state, not byte-mirroring

Status: accepted (Phase 5). Unlike ipset (kernel module unavailable in
every sandbox tried so far) and iptables (fully differential-tested
against Go's fake-executable transcripts), plain rtnetlink — `ip rule`,
`ip route`, `ip link`, `ip addr` — works in this sandbox once `iproute2`
is installed. `rtnl.c` (rule add/del, blackhole route add/del, interface
route add/del with gateway diffing, link-by-name, gateway-for-iface,
mark/table allocation) is therefore built as idiomatic, standard
rtnetlink requests rather than byte-for-byte mirroring every attribute
`vishvananda/netlink` happens to send — including some that look like
incidental zero-values (e.g. an always-present `RTA_OIF=0` attribute on
non-interface routes) that reflect library internals more than protocol
requirements. Correctness was instead verified by exercising the real
kernel: creating rules/routes/links via the C code and independently
inspecting the resulting state with `ip rule show` / `ip route show
table N` / `ip link show`, confirming it matches what the equivalent `ip
rule add` / `ip route add` commands would produce. This is a narrower
compatibility claim than byte-mirroring (spec: "не заявляй о
совместимости только на основании визуального сходства кода") — it is a
claim about observed kernel state, backed by the real-kernel runs
recorded in phase-5-report.md, not about wire-format identity with Go's
netlink library.

One genuine behavioural gotcha found this way (not a Go-compatibility
issue, a C-side bug caught by this testing approach): a single-reply
`RTM_GETLINK` "get" request must use `NLM_F_REQUEST` only. Adding
`NLM_F_ACK` (as an early draft did) makes the kernel send an extra
trailing `NLMSG_ERROR` ack that a non-dump "read one reply, stop" loop
never consumes, permanently desynchronizing every subsequent read on that
socket. Fixed in `rtnl.c`'s `mt_rtnl_link_by_name()`; `NLM_F_ACK` stays
reserved for mutating (add/del) and dump requests.

## D-22 — Ruleset sync(): linear-scan subnet sets instead of a hash map

Status: accepted (Phase 5). Go's `RuleSet.sync()` (rule_set.go) builds
`map[IPv4Subnet]IPSetTimeout` / `map[IPv6Subnet]IPSetTimeout` to
dedupe/diff the desired ipset contents against the current one, giving
O(1) average lookup. `mt_ruleset_sync()` (netfilter/ruleset.c) uses a
plain growable array with linear-scan lookup for both the "new desired
subnets" list and the diff against the current ipset listing, making a
full sync O(n²) in the number of distinct subnets touched (config
subnet/subnet6 rules plus resolved domain addresses in the records
cache). This is an accepted scalability tradeoff, not a claimed
improvement (spec: "не заявляй об улучшении производительности без
измерений") — `sync()` only runs at daemon startup and after API-driven
group/rule mutations (Phase 6), never per-DNS-response, and expected
group/domain counts on the router hardware this backend targets are
small (tens to low hundreds), so the quadratic factor is not expected to
be material in practice. It has not been benchmarked at scale; if a
future phase needs `sync()` to handle large domain counts (e.g. a
subscription-derived group with thousands of rules, Phase 7), this
should be revisited with a real hash table rather than assumed fine.

## D-23 — Own vendored MD5/SHA-256/SHA-512/HMAC/JWT (auth crypto)

Status: accepted (Phase 6), confirms the "Own vendored single-file
implementations" verdict already recorded in dependencies.md. `api/auth/
crypt.go` and `api/auth/jwt.go` are themselves already from-scratch
reimplementations in Go (not calls into a system crypt(3) or a JWT
library) — Poul-Henning Kamp's MD5-crypt and Ulrich Drepper's SHA-256/512-
crypt reference algorithms, plus a minimal hand-rolled HS256 JWT. The C
port (`src/crypto/{md5,sha256,sha512,hmac,base64,crypt,jwt}.c`,
`include/magitrickle/{hash,crypt,jwt}.h`) follows the Go source closely
— this is the one case in the whole rewrite where a near line-by-line
port is the *correct* choice rather than a spec violation: these are
fixed, previously-specified cryptographic algorithms where byte-exact
arithmetic is the entire point (RFC 1321 MD5, FIPS 180-4 SHA-2, RFC 2104
HMAC), not incidental control-flow that should be reworked idiomatically.
No OpenSSL/mbedtls dependency is introduced for these three primitives,
matching the explicit rejection of that option in dependencies.md ("linking
the whole app to a TLS lib for 3 primitives").

Verified two ways: (1) unit tests against published NIST/RFC test
vectors for the raw primitives (empty string, "abc", the two-block NIST
SHA-256 vector, RFC 4231 HMAC test case 1); (2) a **differential** check
against the real Go implementation — `tests/unit/test_crypt.c` and
`tests/unit/test_jwt.c` use vectors captured by temporarily adding a
`TestGenVectors`/`TestGenJWTVectors` test file to `api/auth` in the Go
tree, running it with `go test`, and copying the printed output/tokens
into the C test's expected strings (the temporary Go test file was never
committed). All vectors match byte-for-byte, including the JWT tokens
(satisfying migration-plan.md's explicit "JWT byte-compatible" bar) and
crypt(3) hashes for both default and custom `rounds=N` salts.

Hardening beyond Go (spec: bounded memory): `mt_crypt_password` caps the
password length at `MAX_PASSWORD_LEN` (4096 bytes) before the SHA-crypt
pseq/sseq scratch buffers are allocated; Go imposes no such bound, but no
real password reaches anywhere near this size, so behaviour is unchanged
for every real input.

## D-06 (revisited) — Own bounded HTTP/1.1 server, confirmed

Status: **accepted** (Phase 6), resolving the "proposed, pending Phase 1
spike" note left on the original D-06 entry. `src/api/httpd.c` /
`include/magitrickle/httpd.h` implement a small, purpose-built HTTP/1.1
server on the shared `mt_loop` (epoll) rather than adopting libmicrohttpd
— the API surface is fixed and small (~25 routes under `/api/v1`, JSON
bodies "≤100 KB" per dependencies.md, plus whole-file static serving),
so a general-purpose HTTP library added little beyond what a few hundred
lines of purpose-built parsing/routing already covers, and building it in
means the connection state machine reuses the exact non-blocking
accept/partial-read/partial-write pattern already established for the
DNS proxy's TCP path (`src/dns/proxy.c`) instead of introducing a second
concurrency model.

Supports HTTP/1.1 keep-alive (one request read to completion, response
written, then the next request read on the same connection) and
`Connection: close`/HTTP/1.0 single-request mode; does **not** support
chunked request bodies (every real client here — the frontend's
fetch/axios calls, and the contract tests — sends fixed `Content-Length`
JSON, never chunked transfer-encoding).

Bounded hardening explicitly allowed by compatibility-contract.md §2
("C version may add bounded limits... keep normal-size behaviour
identical"): `MT_HTTPD_MAX_HEADER_BYTES` (8 KiB), `MT_HTTPD_MAX_BODY_BYTES`
(1 MiB), `MT_HTTPD_MAX_CONNS` (64, matching the number originally proposed
for D-06), and a 30 s per-connection idle timeout — none of which Go's
zero-value `http.Server` has, and none of which any real request from the
frontend or the differential contract tests come close to.

One deliberate, documented behavioural broadening vs. chi (Go's router):
a path pattern like `/api/v1/groups/{groupID}` matches both
`/api/v1/groups/{groupID}` and a trailing-slash variant, because segment
splitting ignores leading/trailing/duplicate slashes on both the pattern
and the incoming path before comparing. Strict chi (no `RedirectSlashes`
middleware configured in `api/v1/router.go`) would 404 a mismatched
trailing slash where this server accepts it. This can only ever accept a
request Go would reject, never the reverse, so no legitimate client can
be broken by the difference — flagged here per the "no silent
incompatible fix" rule rather than left undocumented.

Verified with a real client/server integration test
(`tests/unit/test_httpd.c`): runs the actual epoll loop on a background
pthread while the test thread drives it with plain blocking sockets over
both a TCP listener and a Unix socket listener, covering query-string
parsing, path-param extraction, POST body round-trip, the not-found
fallback, middleware short-circuiting (401), keep-alive across multiple
requests on one connection, and Unix-socket routing parity — all under
ASan/UBSan with zero findings.

## D-24 — Auth: platform paths module, and calendar-correct JWT expiry without timegm(3)

Status: accepted (Phase 6). Two small additions supporting
`src/api/auth.c` (port of `api/auth/{passwd,secret,password,middleware,
handlers}.go`):

1. `include/magitrickle/paths.h` — the C backend had no equivalent of
   Go's build-tag-conditional `constant/path_{default,entware,openwrt}.go`
   yet (only `main.c`'s single `MT_CONFIG_PATH` `#ifndef` override
   existed). Added `MT_APP_SHARE_DIR`/`MT_APP_STATE_DIR`/`MT_SOCK_PATH`/
   `MT_PASSWD_FILE`/`MT_SHADOW_FILE` with defaults matching
   `path_default.go` and the same `#ifndef`-override hook, so Phase 8
   packaging can wire per-platform `-D` flags exactly like it will for
   `MT_CONFIG_PATH`. Until then every C build uses the non-Entware/
   non-OpenWrt defaults.
2. Go's `issueToken` computes the JWT expiry via
   `issuedAt.AddDate(jwtYears, 0, 0)` — 20 *calendar* years, correctly
   handling leap years (e.g. Feb 29 rolling to March 1 when the target
   year isn't a leap year). Reproducing this without pulling in `timegm(3)`
   (a GNU/BSD extension; project convention confines GNU extensions to
   `src/platform/`, and auth.c isn't there) uses Howard Hinnant's
   public-domain `days_from_civil`/`civil_from_days` integer arithmetic
   (`mt_auth_add_years_utc`, exposed for testing). Verified against six
   vectors captured from the real Go `time` package
   (`tests/unit/test_auth.c`), including the Feb-29-2080-to-March-1-2100
   non-leap-target-year edge case — byte-identical results.

Also carries forward the shared-primitive extraction already implied by
D-23: `mt_id_random()` (config/id.c) and the new secret generator both
need cryptographically random bytes, so the `/dev/urandom` read-loop was
pulled out into `src/util/rand.c`/`include/magitrickle/rand.h` (a
behaviour-preserving refactor of already-tested code, not a new
decision in itself, noted here for traceability).

Two internal functions (`mt_auth_load_password_hash`,
`mt_auth_authenticate`, `mt_auth_verify_token`) each have a
`..._from(shadow_path, passwd_path, ...)` sibling taking explicit file
paths, used only by `tests/unit/test_auth.c` to point at temp fixture
files instead of mutating the sandbox's real `/etc/shadow` — the
production entry points always pass the real `MT_SHADOW_FILE`/
`MT_PASSWD_FILE`. The in-memory app-secret cache
(`mt_auth_load_secret`) is loaded once per process lifetime (matching
Go's `sync.Once`), which is why a test exercising "two different
`state_dir`s in one process get two different secrets" isn't
meaningful here — Go itself never has more than one `state_dir` per
process either.

## D-25 — App layer: cfg->groups as the live group registry, not a Go-style reconstruction

Status: accepted (Phase 6). `include/magitrickle/app.h` / `src/api/app.c`
port the group/interface/config-save slice of `app.go`'s `App` struct
(`UserGroups`/`AddGroup`/`ClearGroups`/`RemoveGroupByIndex`/
`RemoveGroupByID`/`ListInterfaces`/`SaveConfig`/`ForceCommitIPTables`),
promoting what main.c (Phase 5) built ad hoc into something Phase 6's
HTTP handlers can drive at runtime.

Go's `models.AppConfig` has no `Groups` field at all — group data lives
solely in `a.userRuleSets` (each `*RuleSet` wraps a `*models.Group` via
`spec.Model`), and `SaveConfig()` reconstructs a fresh `[]*models.Group`
from `rs.Model()` on every save. The C port instead keeps `cfg->groups`
(an array of pointers, so appending/removing entries reallocates the
pointer array but never invalidates the pointees) as the single live,
mutable group registry: `mt_ruleset_t` already stores a `const
mt_group_t *` pointing straight at a `cfg->groups` entry (this was
already true from Phase 5), so `mt_app_add_group`/`mt_app_remove_group_*`
mutate `cfg->groups` and the app's parallel `mt_ruleset_t*` array
together, index-for-index, and `mt_app_save_config` is then just a
direct call to the existing `mt_config_save_file` — no Go-style
reconstruction step needed. Two new `mt_config_t` helpers back this:
`mt_config_remove_group_by_index` and `mt_config_clear_groups`.

Faithfully ports two behavioural asymmetries visible in `app.go` that are
easy to miss: `ClearGroups` calls `Disable()` on every group before
clearing, but `RemoveGroupByIndex`/`RemoveGroupByID` do **not** — the Go
callers (e.g. `handlers.go`'s `DeleteGroup`) call `Disable()` themselves
first when the group is enabled. `mt_app_clear_groups` disables;
`mt_app_remove_group_by_index`/`by_id` don't — Phase 6's Groups/Rules
handlers (next task) must disable before removing, exactly like Go's
handlers do.

`ListInterfaces` is ported via `getifaddrs()` deduped by name (matching
`net.Interfaces()`), filtered by `IFF_POINTOPOINT` unless
`show_all_interfaces` is set (matching `interfaces.filterManaged`) — the
default/non-`entware_kn` `IgnoredInterfaces` list is empty in Go, so
there's nothing further to filter on this platform. The Keenetic-RCI
friendly-name lookup (`entware_kn`-only, HTTP calls to `127.0.0.1:79`)
is out of scope here: every interface's `name` field is empty, matching
Go's `DummyRouterSpecificAPI` (the same fallback every non-`entware_kn`
build already uses) — deferred to Phase 8 packaging alongside the rest
of the `entware_kn` build-tag surface.

Verified in `tests/unit/test_app.c`, including a rollback test that
exercises `mt_app_add_group`'s real failure path: adding an
`enable:true` group while the app is marked "running" attempts a real
`ipset create` via libmnl, which fails in this sandbox (no `ip_set`
kernel module — the same Phase-0/Phase-5 finding), and the test confirms
the group is removed from both the ruleset list and `cfg.groups` after
the rollback, not left half-added.

## D-26: Groups/Rules HTTP handlers (`groups.h`/`groups.c`)

Ports `api/v1/handlers.go`'s group+rule handlers and
`api/v1/converters.go` (compatibility-contract.md §2's groups/rules row)
onto the Phase 6 `mt_httpd_t` + `mt_app_t` layers. DTO JSON is
built/parsed directly with cJSON inside `groups.c` rather than a separate
public type — mirrors `converters.go` living in the same Go package as
`handlers.go`; nothing outside this module needs the wire shape.

**Mutable access to a live group.** Go's `RuleSet.Model()` returns
`*models.Group`, the *same* object the caller mutates in place (e.g.
`PutGroup`'s `GroupFromReq(req, groupWrapper.Model())`). `mt_ruleset_t`
stores its group as `const mt_group_t *` (ruleset.c is read-only over
it), so a new accessor, `mt_ruleset_group_mut()`, was added to
`ruleset.h`/`ruleset.c` — it returns the same borrowed pointer without
the `const` qualifier. This is a legitimate cast, not a const-away hack:
the pointee is genuinely mutable (owned by `cfg->groups`, per D-25), only
`ruleset.c`'s own accessor was const for its internal safety. Used by
`handle_put_group`/`handle_put_rule`/etc. to edit a group/rule's fields
in place, preserving its `cfg->groups[i]` identity — critical, since
every `mt_ruleset_t` borrows that exact pointer and swapping it out from
under the ruleset would require re-pointing the ruleset too.

**Building vs. mutating in place.** Unlike Go, `group_from_req()` in
`groups.c` always returns a brand-new `mt_group_t*` rather than mutating
`existing` directly (matching Go's signature `GroupFromReq(req,
existing)` in spirit, not by aliasing the same memory). For `PutGroup`,
where the live group's identity must be preserved, the new object is
then transplanted onto the live one field-by-field
(`group_move_into()`, which frees the live group's old contents, moves
the built object's pointers over, and frees the now-empty built shell
with a plain `free()` — not `mt_group_free()`, which would double-free
the pointers just moved). For `CreateGroup`/`PutGroups`, the freshly
built object becomes the live one directly via `mt_app_add_group`
(which always takes ownership).

**Lenient vs. strict rule-ID reuse — a real Go distinction, not a
divergence.** Go has two different ID-matching code paths for rules,
faithfully kept as two separate C functions:
- `rule_from_req()` (lenient, mirrors `RuleFromReq`): used for
  `CreateRule`, and for a `GroupReq`'s nested `"rules"` array
  (`CreateGroup`/`PutGroup`/`PutGroups`). An `"id"` that doesn't match
  any baseline rule is *not* an error — Go silently assigns a fresh
  random ID instead of failing.
- `rule_from_req_strict()`: used only by `PutRules` (the group-level
  bulk rule replace), which has its own inline found-tracking loop in
  Go, distinct from `RuleFromReq` — an unmatched `"id"` here is
  `MT_ERR_NOENT` → HTTP 404 ("rule not found"), and a body missing the
  top-level `"rules"` key is 400 ("no rules in request").

`PutRule` (single-rule update by path) matches Go exactly by *not*
calling either converter: it mutates the path-resolved rule's fields in
place and never reads the body's own `"id"` field at all (Go: `rule :=
groupWrapper.Model().Rules[ruleIdx]; rule.Name = req.Name; ...`).

**Documented gap: subscription rule-set sync.** Go's `PutGroups` also
calls `h.app.SyncSubscriptionRuleSets()` after replacing the group list.
Subscriptions aren't wired into `mt_app_t` yet (that's Phase 6 task
#36), so this call is a no-op here for now — noted with a code comment
at the call site in `handle_put_groups`, to be revisited once
subscriptions land.

**Middleware-index smuggling not replicated.** Go's chi router resolves
`groupID`/`ruleID` path params in nested middleware and smuggles the
resulting index to inner handlers via a request header
(`r.Header.Set("groupIdx", ...)`). The C router's `{name}` path-param
support makes this unnecessary: each handler resolves `groupID`/`ruleID`
directly off the request (`resolve_group`/`resolve_rule`), producing the
same 400 (invalid hex id) / 404 (not found) behavior without the
header-passing indirection — an implementation simplification with no
observable behavior change.

Verified in `tests/unit/test_groups.c` (10 tests, HTTP-level against a
real `mt_httpd_t` + `mt_app_t` over the background-loop-thread +
blocking-client harness already established by `test_httpd.c`/
`test_auth.c`): empty list, create with color normalization and default
`enable`, invalid/unknown group and rule ids, `PutGroup` preserving rules
when the body omits `"rules"` and rejecting an ID mismatch, delete,
bulk `PutGroups` reusing group/rule IDs across a replace while dropping
unreferenced groups, full rule CRUD, and `PutRules`' strict-vs-lenient
ID validation. The app under test is never marked "running", so
enable/disable/sync are no-ops regardless of a group's `enable` field —
the real-netfilter-backed failure path through these handlers is a thin,
already-tested pass-through of `mt_ruleset_t`/`mt_app_t` return codes
(see `test_app.c`'s `add_group_while_running_rolls_back_on_failure`),
so it isn't re-exercised at the HTTP layer here.

## D-27: System endpoints, static skin serving, and main.c HTTP wiring

Ports the remaining pieces of `api/v1/handlers.go` not covered by D-26
(`ListInterfaces`, `SaveConfig`, `NetfilterDHook` — `system.h`/`system.c`)
and `http.go`'s wildcard static-file fallback (`staticfiles.h`/
`staticfiles.c`), then wires everything built across Phase 6 so far into
`main.c` — this is the first point at which the C daemon actually serves
the HTTP/Unix-socket API end to end, not just in unit tests.

**Security fix found while building this task: `mt_http_req_path()`
wasn't actually cleaning `..`/`.` segments.** httpd.h's contract
(written during task #31) already *claimed* `mt_http_req_path` returns a
"path-cleaned (matches Go's `path.Clean`)" string, but the task #31
implementation only percent-decoded the path — it never collapsed dot
segments. That gap was harmless until this task, since nothing yet
joined the request path onto a filesystem root, but `staticfiles.c`'s
skin-file serving does exactly that. A request like
`GET /../../../../etc/passwd` would have joined onto
`<skins_dir>/<skin>/../../../../etc/passwd` and, depending on nesting
depth, could have escaped the skin directory entirely. Fixed by adding
`path_clean()` to `httpd.c` (Go `path.Clean` semantics for a rooted
path: `.` segments are dropped, `..` pops the last kept segment or is
simply dropped at/above the root — since the input is always absolute,
this can never escape upward) and applying it to every parsed request
path before it's exposed via `mt_http_req_path()`. Verified directly:
`test_staticfiles.c`'s `path_traversal_is_contained_under_skin_root`
sends exactly that request against a real skin fixture directory and
asserts a plain in-skin 404, not real `/etc/passwd` contents.

**Static file serving (`staticfiles.c`)** ports `http.go`'s `r.Get("/*",
...)` handler closely, including its exact up-to-2-stat retry loop
(directory → append `/index.html` → retry once, matching Go's
`for i := 0; i < 2; i++`) and its three response shapes: normal file
(content-type by extension: html/css/js/ico/png/svg, else
`text/plain`), missing file (404 JSON `{"error":...}`), and missing
file *at `/` specifically* (404 with Go's exact
`noSkinFoundPlaceholder` HTML string). Registered only as the TCP
`mt_httpd_t` instance's not-found fallback (`mt_httpd_set_not_found`),
never on the Unix socket instance — matches `unixsocket.go`, which
mounts only the v1 API router. Non-GET requests to an unmatched path
get a plain 404 here rather than chi's default text/plain 404 page;
documented as a cosmetic, status-code-level-only difference (D-05
already established that byte-identical bodies aren't a goal).

**`main.c` refactor: `mt_app_t` replaces the ad hoc `mt_ruleset_t**`
array.** Phase 5's `main.c` built and owned its own raw ruleset array
inline. Since the Phase 6 HTTP handlers (D-26, this entry) need a real,
running `mt_app_t*` to mutate, `struct daemon`'s `rulesets`/`n_rulesets`
fields are gone, replaced by `mt_app_t *app` (plus `mt_httpd_t
*http_tcp`/`*http_unix`). `find_ruleset`/`on_link_up`/`on_addr_change`
now go through `mt_app_find_group_by_id`/`mt_app_user_group_count`/
`mt_app_user_group_at` instead of iterating the old array directly. The
startup sequence still enables+syncs every initially-configured group
itself (via `mt_app_user_group_at`) *before* calling
`mt_app_set_running(app, true)` — exactly the ordering app.h's own
header comment already prescribed (mirrors Go's `Start()` CAS happening
before that same loop), so a group added later through the HTTP API
gets its own immediate enable+sync via `mt_app_add_group`, while the
initial set is brought up directly by `main()`.

**HTTP/Unix-socket topology matches Go exactly:** the Unix socket
(`MT_SOCK_PATH`) always starts and always mounts the full v1 router
(groups/rules/system/auth — matches `unixsocket.go`, which has no
enabled-gate and never installs the auth middleware); the TCP WebUI
only starts when `cfg.app.http_web.enabled`, and only the TCP instance
gets `mt_auth_middleware` and the static-file not-found fallback
(matches `http.go`). `SaveConfig`'s path/version (needed by every
`?save=true` handler and the `/system/config/save` endpoint) come from
the same `config_path`/`MT_VERSION` `main()` already resolves at
startup — no new state.

**Verified two ways.** Unit-level: `test_system.c` (4 tests: blackhole
prepended with `name` omitted per Go's `omitempty`, config save
round-trip through a real temp file, no-op-without-a-path, netfilter.d
hook success + malformed-body 400) and `test_staticfiles.c` (7 tests
including the path-traversal-containment test above), both against a
real `mt_httpd_t` over the same background-loop-thread harness as
`test_groups.c`. End-to-end: built and ran the actual
`magitrickled-c` binary against a scratch config (remap53 disabled,
`showAllInterfaces: true`, no groups) and drove it with real `curl`
over both the TCP WebUI and the Unix socket: `GET /groups` (empty),
`POST /groups` with `enable:false` (200, persisted), the *same* POST
with the default `enable:true` (500 — the sandbox's known-missing
`ip_set` kernel module, same root cause as `test_app.c`'s rollback
test, and confirmed the failed group was *not* left in `GET /groups`
afterward), `GET /system/interfaces`, `GET /auth`, `GET /` (the
no-skin-installed HTML placeholder, since no skin is built into this
scratch environment), `POST /system/config/save`, and
`POST /system/hooks/netfilterd`; `SIGTERM` produced a clean shutdown
log and left no `MT__`-prefixed iptables chains behind afterward.

Full suite (24 unit-test binaries, up from 22), `make static_analysis`
(clang-tidy + cppcheck), and `make sanitize` (ASan+UBSan) all clean
after these changes.

## D-28: Subscriptions CRUD endpoints (non-fetch)

Ports the pure config-mutation slice of `api/v1/subscription_handlers.go`
and `subscription_converters.go`: `GET`/`PUT`/`POST
/api/v1/subscriptions` and `DELETE /api/v1/subscriptions/{id}`
(`subscriptions_api.h`/`subscriptions_api.c`), plus `mt_app_t` additions
(`mt_app_subscription_count`/`_at`/`find_by_id`,
`mt_app_add_subscription`, `mt_app_replace_subscriptions`,
`mt_app_remove_subscription_by_id`) and two `mt_config_t` helpers
(`mt_config_remove_subscription_by_index`, `mt_config_clear_subscriptions`
— the subscription-side mirrors of D-25's group equivalents).

**Deliberately not implemented (need libcurl, Phase 7 scope): `POST
/api/v1/subscriptions/{id}/sync` and `GET /api/v1/subscriptions/rules?url=`.**
Both call Go's `subscriptions.FetchList` over HTTP(S), which this C port
has no dependency for yet. Neither route is registered, so a request to
either falls through to the normal 404 path rather than being faked or
stubbed with a fake success. This was the plan's own instruction for
this task, called out again here for the historical record.

**Much simpler than group CRUD, for a real reason: subscriptions have no
runtime `mt_ruleset_t`/netfilter counterpart in the C port yet.** Go's
`App.AddSubscription`/`ReplaceSubscriptions`/`RemoveSubscriptionByID`
each rebuild `a.subscriptionRuleSets` (via
`syncSubscriptionRuleSetsLocked`) and can fail/roll back if that rebuild
fails — subscription-derived rule sets are matched during DNS resolution
exactly like user groups (`app.ruleSetSnapshot` concatenates both), and
subscriptions get real ipset/iptables state through the same
`groupruntime.BuildRuntimeRuleSet` path groups use. None of that exists
on the C side yet (only the subscription *config model* and the
subscription-list-parsing logic from Phase 2's `subparse.c` do), so
`mt_app_add_subscription`/`mt_app_replace_subscriptions`/
`mt_app_remove_subscription_by_id` are plain, unconditionally-successful
`cfg->subscriptions` array mutations with no rollback path to speak of —
a real, load-bearing gap (not yet DNS-matchable or netfilter-backed),
documented here so it isn't mistaken for parity with groups.

**No per-subscription GET/PUT and no in-place mutation, unlike
groups — because Go doesn't have them either.** `router.go`'s
`/subscriptions` route tree has no `GET`/`PUT /{subscriptionID}`; only
bulk `GET`/`PUT`, single-create `POST`, single-delete `DELETE`, plus the
two fetch-requiring routes above. Every subscription mutation in Go goes
through `SubscriptionFromReq`, which always builds a *new*
`models.Subscription` (Go's `App` doesn't expose anything like
`RuleSet.Model()` for subscriptions to mutate in place) — so unlike
`mt_ruleset_group_mut` (D-26), no mutable-accessor equivalent was needed
here at all.

**Two faithfully-preserved Go quirks, kept rather than "fixed":**
- **`?save=` defaults are the *opposite* of the groups handlers.**
  Groups: `r.URL.Query().Get("save") == "true"` (opt-in, defaults to not
  saving). Subscriptions: `r.URL.Query().Get("save") != "false"`
  (opt-out, defaults to saving). This is a real asymmetry already
  present in the Go handlers, not something introduced by this port —
  `maybe_save` in `subscriptions_api.c` implements the opt-out form
  exactly, and the header doc on `mt_subs_ctx_t` calls it out explicitly
  so a future reader doesn't "fix" it into consistency with groups.
- **`ensureUniqueSubscriptionIDs`/`ensureUniqueSubscriptionRuleIDs` are
  silent fixups, not validation.** Despite returning `error` in Go,
  neither function can actually produce one — a zero or duplicate ID
  (checked only against entries processed so far, matching Go's
  incrementally-built `dup` map) is silently replaced with a fresh random
  ID. Ported as `void`-returning fixup functions in
  `subscriptions_api.c` for the same reason. Also preserved: `Interval`
  is *never* seeded from an existing subscription during
  `SubscriptionFromReq`/`subscription_from_req` (only ever taken from the
  request body, defaulting to 0) — asymmetric next to
  `LastUpdate`/`LastCheck`, which *are* seeded from `existing`, but that
  asymmetry is genuinely how Go's converter reads, so it's kept rather
  than "corrected."

Verified in `tests/unit/test_subscriptions_api.c` (7 tests: empty list,
create requires a URL, create defaults (`enable` true, empty `rules`),
duplicate-ID create → 409, delete + 404-on-unknown + 400-on-malformed-id,
`PUT` missing-key/missing-URL → 400, and a bulk-replace test that
captures a real server-assigned rule ID from a prior `GET` -- confirming
along the way that a client-supplied nested-rule ID is silently
discarded on *creation* (no baseline to match against), exactly like
`RuleFromReq` for groups -- then reuses that captured ID plus the
subscription's own ID across the `PUT`, checks `lastUpdate` was seeded
from the pre-existing record, and confirms an unreferenced subscription
is dropped). Also smoke-tested end-to-end against the real
`magitrickled-c` binary (create, list, missing-URL 400) alongside the
groups/system smoke test from D-27.

25 unit-test binaries (up from 24), static analysis, and sanitizers all
clean.

## D-29: HTTP API contract differential suite (Go daemon vs magitrickled-c)

Adds a new differential suite (`tests/differential/http_contract/
contract.py` + `tests/differential/run_http_diff.sh`, wired into the
master `run_diff.sh` as its final step) that runs the *real* Go daemon
binary and the real `magitrickled-c` binary against byte-identical
scratch configs and drives each through the same fixed sequence of 37
HTTP/Unix-socket requests covering everything built in Phase 6 tasks
#31-36 (auth status, full group/rule CRUD including bulk `PUT`,
`system/interfaces`/`config/save`/`hooks/netfilterd`, subscriptions
CRUD, and a Unix-socket spot check), then diffs the two resulting
traces textually. This is the first suite in the project that stands up
two full, real daemon processes rather than invoking one-shot CLI
oracles — a different shape of test from every earlier phase's
differential work, so several new problems had to be solved:

**Go's config path has no CLI override.** Every earlier differential
suite drives Go through small single-purpose `oracle_go`
tools/`App.LoadConfig`+`SaveConfig` calls with paths passed as
arguments. The real Go daemon (`cmd/magitrickled`) has no such
flexibility: `cfgFileLocation` is `constant.AppStateDir +
"/config.yaml"`, a hard-coded constant, unlike the C daemon's `--config`
flag. `run_http_diff.sh` therefore backs up whatever is currently at
`/var/lib/magitrickle/config.yaml` (this sandbox already had a stale
`0.99.0` file left over from the `config` suite's own oracle runs),
overwrites it with the scratch config for the Go run, and restores the
backup (or removes the file if none existed) in a `trap ... EXIT`
cleanup — the C run, by contrast, just uses `--config` against a
separate scratch path, so it never touches the real file at all. Stale
`/var/run/magitrickle.{pid,sock}` files are also cleared before each
run (Go's `cmd/magitrickled` refuses to start with a stale PID file
pointing at another process image, and both backends need a fresh
socket path to bind).

**Random IDs required a "learn, then redact" design, not just
normalization.** A raw byte-diff between two independent backends will
never match wherever either one generates a random ID (group/rule/
subscription IDs are 4 random bytes, hex-encoded) — but simple
normalization isn't enough either, because a later request needs the
*real* ID to address the right resource (e.g. `PUT
/groups/{id}/rules/{ruleId}` needs the actual rule ID a `POST` just
returned, which is a different random value on each backend). Solved
with `contract.py`'s `Trace.step(..., learn=callback)`: the callback
inspects a step's parsed response *before* that step's own trace line is
printed, registers the real ID against a stable per-slot placeholder
(e.g. `<RULE-R2>`), and the redaction is applied everywhere that value
subsequently appears — including retroactively in the very response
that introduced it, and in the literal request path of later steps
(`GET/PUT/DELETE .../rules/<real-id>` prints as `.../rules/<RULE-R2>`).
Group- and subscription-level top-level IDs are the opposite case:
those ARE supplied explicitly and ARE expected to be honored literally
by both backends (a brand-new object's own client-supplied ID is used
as-is), so they're deliberately left unredacted — verifying that literal
echo is exactly the point.

**One request body deliberately avoids a known, already-documented,
intentional divergence rather than tripping over it.** The first draft
of this suite created a subscription with `"enable": true` and got a
real divergence: Go's `AddSubscription` builds and enables a real
netfilter-backed subscription rule set (`syncSubscriptionRuleSetsLocked`
→ the same `RuleSet` machinery groups use), which failed in this sandbox
trying to link the "eth0" ipset (a real, if incidental, environment
limitation); `mt_app_add_subscription` (D-28) has no such runtime
counterpart yet and trivially succeeds. That gap is real and already
documented in D-28 — this suite isn't the place to re-litigate it, so
the subscription in the test sequence uses `"enable": false`, keeping
both backends on the config-only path this suite is actually meant to
verify (subscription netfilter parity has no C-side implementation to
compare against yet).

**Error message *text* is normalized away, not compared.** Any
`{"error": "..."}` body has its message value redacted to a fixed
placeholder before comparison (e.g. Go's `"subscription id conflict"`
vs the C port's generic `mt_err_str(MT_ERR_EXIST)` → `"already exists"`
for the same 409) — consistent with D-05's original position that
byte-identical serialization (and, by extension, exact wording) was
never the contract; status codes and shape are.

Verified by running the full suite twice in a row (idempotent — no
leftover iptables `MT_`-prefixed chains or stray socket/PID files
between or after runs, confirmed via `iptables -L`/`-t nat -L`) and as
part of a complete `run_diff.sh` invocation covering every earlier
phase's differential suite alongside this one, all green.

## D-30: Frontend Playwright e2e suite against magitrickled-c

Adds `tests/differential/run_e2e_diff.sh` (Phase 6, task #38): builds
the production frontend (`npm run build`), serves it as the `default`
skin from a real `magitrickled-c`, and runs the *actual*
`tests/e2e/*.spec.ts` suite (45 tests, unmodified) against it via a new
`playwright.c-backend.config.ts`. All 45 pass, run twice in a row from a
clean state with no leftover `/usr/share/magitrickle`, socket/PID
files, or iptables `MT_` chains afterward.

**Why this mostly exercises `staticfiles.c`, not the HTTP API.** Nearly
every spec in `tests/e2e/` intercepts its own API calls via Playwright's
`page.route()` (see e.g. `groups.spec.ts`'s `beforeEach`), which takes
priority over whatever a real server would return — so which backend is
actually running underneath barely matters for those assertions. What
*does* matter, and what this suite genuinely exercises for the first
time against a real, unmodified, production Svelte build (rather than
the hand-written HTML/CSS/JS fixtures in `tests/unit/test_staticfiles.c`,
D-27), is: does `magitrickled-c` serve `index.html` at `/`, the JS
bundle and CSS with correct content-types, and font/image assets
correctly enough for the app to actually boot and become interactive in
a real browser. The HTTP *API* contract itself is D-29's job
(`run_http_diff.sh`), not this suite's.

**Two environment-specific fixes were needed, neither of which touched
`tests/e2e/*.spec.ts` itself:**
- **Chromium executable path.** This environment pre-installs a fixed
  Chromium revision at `/opt/pw-browsers/chromium-1194/...`, but
  `@playwright/test`'s installed version expects a different
  auto-downloaded revision path (`chromium_headless_shell-1223/...`,
  which doesn't exist here and mustn't be fetched — see the session's
  own environment notes). `playwright.c-backend.config.ts` sets
  `launchOptions.executablePath` explicitly rather than modifying
  `playwright.config.ts` (the existing dev-server config, left alone).
- **Origin-locked clipboard permission grant.** `groups.spec.ts`'s two
  clipboard tests call `context.grantPermissions([...], { origin:
  "http://localhost:5173" })` — a literal, pre-existing hardcoded origin
  in the test file. A permission grant's origin must match the page's
  actual origin exactly, so `run_e2e_diff.sh` serves the C daemon on
  `localhost:5173` (not `127.0.0.1:18099`, tried first and found to fail
  exactly these two tests) purely to land on the same origin the test
  already assumes — again, no edits to the spec itself.

Both fixes were necessary to get a correct, unmodified upstream test
suite running against a new backend in a specific sandbox, not
adaptations of the tests to the C port's behavior — no C-port-specific
behavior difference was found or needed accommodating.

**Backup/restore for the real `/usr/share/magitrickle/skins`
directory**, mirroring D-29's `/var/lib/magitrickle/config.yaml`
backup/restore pattern for the same reason: `MT_APP_SHARE_DIR` is a
compile-time default in `paths.h` (real per-platform overrides are
Phase 8 packaging work), so this suite writes to the real path rather
than a build flag, and must not clobber anything already installed
there.

## D-31: Subscription list fetch via libcurl (`sub_fetch.h`/`fetch.c`)

Ports `subscriptions/fetch.go`'s `FetchList` (Phase 7, task #40) onto
libcurl rather than hand-rolled sockets/TLS — dependencies.md already
called this out as the one place hand-rolling is out of the question
("TLS is non-negotiable for https subscription URLs"). `libcurl4-openssl-dev`
was not preinstalled in this sandbox (only the runtime `.so`); installed
via `apt-get install libcurl4-openssl-dev` to get headers for
development — the actual OpenWrt/Entware feed packages are the real
dependency target (already listed in dependencies.md's table), this was
only a local dev-environment gap.

**Exact redirect semantics via libcurl's URL API, not its own redirect
following.** `CURLOPT_FOLLOWLOCATION` is left off; the C port implements
the same manual loop Go's `FetchList` does, because libcurl's automatic
following (a) follows more status codes than Go's custom loop (which
deliberately only treats 301/302 as "redirect to follow" — any other
3xx, including 303/307/308, is treated as a plain non-2xx terminal
failure, a genuine Go quirk kept here rather than "fixed"), and (b)
doesn't expose the same loop-detection/hop-count semantics. Each hop:
`curl_url()`/`curl_url_set(CURLUPART_URL)` validates the URL and
extracts scheme+host (mirrors Go's `url.Parse` + non-empty
scheme/host check); only `http`/`https` are accepted, at the original
URL and at every redirect target. `CURLINFO_REDIRECT_URL` (queried after
a `FOLLOWLOCATION`-off request that received a redirect status) already
resolves a relative `Location` header to an absolute URL exactly like
Go's manual `parsed.ResolveReference(location)` step, so no separate
relative-URL-resolution code was needed. Loop detection uses a small
fixed-capacity array (bounded by `MT_SUB_FETCH_MAX_REDIRECTS + 1` —
tiny, so a linear scan beats introducing a hash set), seeded with the
original URL before the loop starts, matching Go's `visited` map's
initial seed. The `redirects >= maxFetchRedirects` bounds check is
ordered exactly as Go's is (checked *before* marking the new location
visited and advancing), so the boundary behavior matches precisely: up
to 5 redirects succeed, a 6th is rejected as MT_ERR_LIMIT before ever
being fetched — verified directly with two dedicated redirect chains in
`test_sub_fetch.c` (`allows_exactly_five_redirects` /
`rejects_six_redirects`).

**`MT_SUB_FETCH_MAX_BODY_BYTES` (8 MiB) is new C-side hardening, not a
behavior port.** Go's `io.ReadAll(resp.Body)` has no size limit at all;
migration-plan.md's own Phase 7 line item calls for a "size bound" as a
deliberate addition (mirrors the same "C version may add bounded
limits... document as hardening" allowance already used for the HTTP
server in Phase 6, D-06 revisited). Enforced in the libcurl write
callback (returning a short write count aborts the transfer with
`CURLE_WRITE_ERROR`, mapped to `MT_ERR_LIMIT`), verified with a
dedicated test serving a body just over the cap.

**`curl_global_init`/`curl_global_cleanup` are the caller's
responsibility** (`mt_sub_fetch_global_init`/`_cleanup`), not called
implicitly inside `mt_sub_fetch_list` — libcurl's own documented
contract requires global init to happen once, non-concurrently, before
any thread performs a transfer; main.c will call this once at startup
(wired in a later Phase 7 task alongside the rest of the daemon startup
sequence, not yet done as of this task).

Verified with `tests/unit/test_sub_fetch.c` (11 tests, against a real
local `mt_httpd_t` server acting as the stub subscription-list host —
same background-loop-thread harness as `test_httpd.c` — covering plain
200, empty body, 301, 302, redirect loop, the 5-vs-6-redirect boundary,
non-2xx, oversized body, unsupported scheme, malformed URL, and
connection-refused). Full suite (26 unit-test binaries), static
analysis, and sanitizers all clean.

## D-32: Subscription runtime rule sets, DNS-snapshot integration, and a Phase-6 DNS-matching regression fix

**Synthesis over generalization, mirroring Go's own `Spec` design.**
Go's `RuleSet` operates over `rulesets.Spec`, a small carrier type built
either directly from a `*models.Group` (user groups) or synthesized
fresh on every rebuild from a `*models.Subscription`
(`subscriptionAsRuntimeRuleSet`). C's `mt_ruleset_t` is hardwired to
`const mt_group_t *` (Phase 5, unmodified since, already tested). Rather
than generalizing `ruleset.c` into a Spec-like abstraction, the
subscription side mirrors Go's own approach: `mt_sub_runtime_group()`
(`subscriptions/runtime.c`) synthesizes an independent `mt_group_t*`
from a `mt_subscription_t` (id, name — falling back to
`"subscription:<id>"` when empty, matching Go — interface, enable
computed as `sub->enable && iface non-empty`, and one `mt_rule_t` per
subscription rule), which is then fed through the existing, unmodified
`mt_ruleset_new()` exactly like a real config-file group would be.
`color` is intentionally left NULL: only the HTTP JSON layer reads it,
and synthesized groups never reach that layer.

**Ownership: `mt_ruleset_new()` borrows its group pointer** (ruleset.h's
pre-existing documented contract — "callers keep the owning `mt_config_t`
alive for the ruleset's lifetime"), so a synthesized group can't be
freed right after building its ruleset. `mt_app_t` keeps a parallel
owned array, `sub_synth_groups`, alongside `sub_rulesets`, freed in
lockstep (`sub_rulesets_push`'s realloc only bumps `cap_sub_rulesets`
once *both* parallel array reallocs have succeeded, so a partial-realloc
failure can never leave the two arrays believing they have different
capacities). Subscription rulesets live in this array, separate from
`cfg->groups`' rulesets, because Go always rebuilds subscription rule
sets wholesale (`buildSubscriptionRuleSetsLocked` disables+recreates
every one on any subscription-list-or-rules change, never edits in
place) — `rebuild_subscription_rulesets()` in `app.c` does the same:
disable+free the old array, synthesize+build fresh for every
subscription, roll back to nothing enabled and log on failure exactly
like Go's `syncSubscriptionRuleSetsLocked` failure path.

**`mt_app_add_subscription`/`_replace_subscriptions`/
`_remove_subscription_by_id` all rebuild-wholesale and roll back the
`mt_config_t` mutation (not just the rulesets) on failure**, mirroring
Go's `AddSubscription`/`ReplaceSubscriptions`/`RemoveSubscriptionByID`
exactly: mutate `cfg`, rebuild; on failure, undo the `cfg` mutation and
rebuild again (logging if even that fails), so the app is never left
with a `cfg`/ruleset-array mismatch. `mt_app_remove_subscription_by_id`
changed signature from `bool` to `mt_err_t` + `bool *out_found` (mirrors
Go's `(bool, error)` return) so the HTTP handler can distinguish 404
(not found) from 500 (found, rebuild failed) — the one existing caller
(`subscriptions_api.c`'s `handle_delete_subscription`) was updated in
the same change.

**Real Phase-6 regression found and fixed: the DNS-matching snapshot was
never rebuilt after startup.** `mt_ruleset_snapshot_build(&cfg)` was
called exactly once, at daemon startup in `main.c`, and never again —
every Phase-6 HTTP-driven group/rule mutation updated netfilter (via
`mt_ruleset_t`'s own enable/disable/sync) but never touched DNS
matching, so a group or rule created purely through the HTTP API would
never resolve traffic into it. This was a genuine bug in already-shipped
Phase 6 code, not a hypothetical concern, found by tracing every call
site of `mt_ruleset_snapshot_build`/`mt_dns_pipeline_set_snapshot`
across `main.c`/`app.c`/`groups.c`. Fixed with a new public
`mt_app_republish_dns_snapshot(app)`: rebuilds the snapshot from
`app->cfg` and calls `mt_dns_pipeline_set_snapshot` on the pipeline
handed to `mt_app_create` via the new (optional, NULL-able)
`mt_app_deps_t.pipeline` field. Called internally by every `mt_app_t`
group/subscription mutator (`mt_app_add_group`, `mt_app_clear_groups`,
`mt_app_remove_group_by_index`, the subscription mutators above), and
explicitly by `groups.c`'s in-place-mutation HTTP handlers
(`handle_put_group`, `handle_put_rules`, `handle_create_rule`,
`handle_put_rule`, `handle_delete_rule`), which mutate a live group
in place via `mt_ruleset_group_mut` rather than going through
`mt_app_t`'s own mutators. The group side had exactly the same bug as
the subscription side, so both are fixed together rather than patching
subscriptions alone. Confirmed against Go's own `LoadConfig()` (groups
and subscription rule sets are both built, unenabled, while
`a.enabled` is still false) and `Start()` (a single flat loop over
`ruleSetSnapshot()` — groups+subscriptions together — enables and syncs
everything uniformly) that this mirrors Go's actual startup/mutation
model.

**DNS-matching snapshot extended to include subscriptions**
(`rules/snapshot.c`): a shared `append_snapshot_entry()` helper
(factored out of the existing per-group loop) is now called once per
enabled user group and once per enabled subscription (via a synthesized
`mt_sub_runtime_group()`, discarded immediately after its id/name/rules
are copied into the snapshot entry). Subscription rules of type
`subnet`/`subnet6` need no special-casing: `mt_matcher_add()` already
treats those as `RK_NEVER` (never matches a domain name), the same
semantics Go's own matcher uses, so they flow through the identical
matcher-building code as any group rule.

**`main.c` wiring**: `app_deps.pipeline` now passed through to
`mt_app_create`. `find_ruleset()` (used by the DNS match sink) now
falls back to `mt_app_find_subscription_ruleset_by_id` when a match's
`group_id` isn't a user group. `on_link_up`/`on_addr_change` now also
iterate subscription rulesets via
`mt_app_subscription_ruleset_count`/`_at` (confirmed against Go's
`netlink.go`, where `handleLink`/`handleAddr` both iterate
`ruleSetSnapshot()` — groups and subscriptions together — not just
groups). A new enable+sync loop over the initial subscription rulesets
runs right after the existing group-ruleset loop, before
`mt_app_set_running(true)`, mirroring the same "build unenabled at
config-load time, enable+sync everything in one flat pass at Start()"
sequence Go uses.

**Regression test**: `tests/unit/test_groups.c` gained
`rule_created_via_http_is_dns_matchable` — creates a group with a
domain rule purely via `POST /api/v1/groups`, then hand-builds a
synthetic `mt_dns_msg_t` with a matching A-record answer and calls
`mt_dns_pipeline_handle_message()` directly, asserting the match sink
fires with the created group's id. Before this task's fix this would
have failed (the snapshot handed to the pipeline would still have been
the empty one built from the config file at daemon startup).
`test_rulesnap.c` gained 4 tests covering subscription
inclusion/exclusion (enabled+interface, no interface, disabled, and
both groups+subscriptions participating together).

Verified: full suite (27 unit-test binaries, including the new
regression tests), `static_analysis` (clang-tidy+cppcheck, 0 warnings),
and `sanitize` (ASan+UBSan) all clean; `magitrickled-c` and
`mt-configtool` rebuild cleanly with the new `main.c`/`app.c`/`app.h`
signatures.

## D-33: Subscription sync flows (`mt_app_sync_subscription_by_id`/`mt_app_sync_due_subscriptions`) and the blocking-fetch tradeoff

**Ports `subscriptions.go`'s `SyncSubscriptionByID`/`SyncDueSubscriptions`
onto `mt_app_t`, reusing the Phase 2 parse primitives
(`mt_sub_refresh_rules`/`mt_sub_same_rules`/`mt_sub_is_due`, already
shipped and differentially tested) and the Phase 7 fetch primitive
(`mt_sub_fetch_list`) with the Phase 7 rebuild/rollback machinery
(`rebuild_subscription_rulesets`) from D-32.** `mt_app_sync_subscription_by_id`
mirrors Go field-for-field: `LastCheck`/`URL` update unconditionally on
any successful fetch, `LastUpdate`/`Rules` (and the ruleset rebuild) only
when the refreshed rules actually differ (`mt_sub_same_rules`) — matching
the observation that a URL-only change never needs a ruleset rebuild in
either language, since neither `RuleSet`/Go nor `mt_sub_runtime_group`/C
read the subscription's URL when building matcher state. On a rebuild
failure, every mutated field is rolled back to its pre-sync value and
the rulesets rebuilt again from that (rollback failure logged, not
returned, matching `mt_app_add_subscription`'s established pattern);
`*out_changed` is still set `true` in that branch, faithfully matching
Go's own literal `true` return there ("a change was attempted", even
though it was rolled back). `mt_app_sync_due_subscriptions` mirrors the
two-phase due-scan/apply split (`IsDue` filter, then per-subscription
fetch+refresh, then one shared rebuild+rollback for whatever actually
changed across the whole batch) exactly, including the detail that a
subscription's `LastCheck` still advances even when its content turned
out unchanged (so it won't be immediately re-fetched next tick), while
only an *actual* rules change anywhere in the batch triggers the shared
rebuild.

**No result-copy struct in the API.** Go's `SyncSubscriptionByID` returns
an `app.SubscriptionSyncResult{URL, LastUpdate, Rules}` value because its
caller (the HTTP handler) runs in a different goroutine and needs a
snapshot immune to concurrent mutation. This port's callers are all
on the same single event-loop thread as the sync call itself (see
below), so nothing can mutate `cfg` between `mt_app_sync_subscription_by_id`
returning and the caller's next statement — `mt_app_find_subscription_by_id`
called immediately after a successful sync already IS "target". This
avoids a whole deep-copy/free lifecycle for a value that would be stale
the instant a future worker-thread redesign made the fetch genuinely
concurrent.

**New `MT_ERR_UPSTREAM` error code**, kept deliberately distinct from
`MT_ERR_IO`/`MT_ERR_PROTO`/`MT_ERR_LIMIT` (any of which `mt_sub_fetch_list`
itself can return depending on the failure mode). Go collapses every
fetch failure into a single `app.ErrSubscriptionFetch` sentinel via
`fmt.Errorf("%w: ...")` specifically so the HTTP layer can map it to one
status (502) regardless of cause; `mt_err_t` has no wrapping, so
`mt_app_sync_subscription_by_id` does the same collapsing explicitly —
logs the underlying `mt_sub_fetch_list` error, then returns
`MT_ERR_UPSTREAM` — so a fetch failure can never be confused with a
ruleset-rebuild failure (which reuses whatever `mt_ruleset_enable`/
`_sync`/`MT_ERR_NOMEM` code the rebuild itself produced) by whatever
HTTP status-mapping code consumes this in the next Phase 7 task.

**Known, documented limitation: both sync functions perform a *blocking*
libcurl fetch on the calling thread**, and today that thread is always
the single event-loop thread — `mt_httpd_t`'s handler contract
(`handle_complete_request` in `httpd.c`) builds and writes the HTTP
response synchronously from a stack-local `mt_http_res_t` before
returning, with no support for a handler to defer its response, so an
HTTP-triggered sync (the next Phase 7 task) and the auto-update timer
callback (the task after that) will both stall DNS resolution and HTTP
serving for up to `MT_SUB_FETCH_TIMEOUT_SECONDS` on a slow or hung
upstream. This is a real architectural gap from Go's model (where
`SyncSubscriptionByID`/`SyncDueSubscriptions` block only their own
goroutine, never DNS-serving or other HTTP-request goroutines) and was
already anticipated in D-02 ("a small worker pool ... only for
blocking/slow work: ... libcurl transfers") and D-17 ("when Phase 7
introduces a worker thread for subscription fetch, the snapshot's
publication must be marshalled onto the loop thread via `mt_loop_post`").
Implementing that worker-thread + deferred-HTTP-response redesign was
scoped out of this task given the size of the `httpd.c` refactor it
would require (heap-allocating response state, extending the connection
lifecycle to survive an async completion, handling the connection
closing mid-fetch); it is called out here explicitly, as a documented
divergence, rather than silently shipped as if it matched Go's
non-blocking behavior. This mirrors the precedent already accepted for
iptables fork+exec in Phase 5/6 (also invoked synchronously on the loop
thread, also not yet moved to a worker pool) — subscription fetch simply
makes the same tradeoff far more visible, since a hung fetch can block
for the full 15-second timeout rather than a fork+exec's usual
sub-100ms cost. Left as a candidate revisit for a later phase, not
silently dropped.

Verified with a new `tests/unit/test_sub_sync.c` (9 tests, against a
real local `mt_httpd_t` server acting as the stub subscription-list
host, same background-loop-thread harness as `test_sub_fetch.c`):
first sync fetches and reports changed; a same-content resync reports
unchanged but still advances `last_check`; a different-content resync
(via `url_override`) updates both URL and rules and reports changed;
unknown subscription id is `MT_ERR_NOENT`; an empty URL with no
override is `MT_ERR_INVAL`; a fetch returning non-2xx is
`MT_ERR_UPSTREAM` and leaves the subscription untouched; due-vs-not-due
filtering in the batch sync (interval/last_check arithmetic); a
due-but-content-unchanged batch sync still advances `last_check` without
reporting a change; and a batch sync with nothing due is a no-op. A
forced-rebuild-failure/rollback test was not added at this layer (no
netfilter mock is currently wired through `mt_app_t`'s own tests to
force `rebuild_subscription_rulesets` to fail deterministically) — the
rollback code itself is structurally identical to
`mt_app_add_subscription`/`_replace_subscriptions`'s already-established
pattern from D-32. Full suite (28 unit-test binaries), `static_analysis`
(clang-tidy+cppcheck, 0 warnings), and `sanitize` (ASan+UBSan, 0
findings) all clean.

## D-34: Subscription sync/rules-preview HTTP endpoints (`POST /subscriptions/{id}/sync`, `GET /subscriptions/rules`)

**Wires `mt_app_sync_subscription_by_id` (D-33) and `mt_sub_fetch_list`/
`mt_sub_parse_rules` directly onto the two routes Phase 6/D-28 explicitly
deferred**: `POST /api/v1/subscriptions/{subscriptionID}/sync`
(`SyncSubscription`) and `GET /api/v1/subscriptions/rules?url=`
(`GetSubscriptionRules`, a preview-only endpoint — parses but never
persists). Error mapping mirrors Go's `switch { errors.Is(...) }` in
`SyncSubscription` exactly: `MT_ERR_NOENT`→404, `MT_ERR_INVAL`→400,
`MT_ERR_UPSTREAM`→502 (the new code from D-33, purpose-built for this
mapping), anything else→500. The sync response is built by re-reading
the subscription via `mt_app_find_subscription_by_id` immediately after
a successful sync rather than threading a result struct through the
API (see D-33's rationale — safe because nothing else runs between the
two calls on this single-threaded event loop). `sub_rule_to_json`/
`sub_rules_to_json_array` (already used by the non-fetch subscription
handlers) are reused as-is for both new endpoints' `rules` arrays, since
`SubscriptionRuleRes`'s JSON shape (`id`/`rule`/`type`/`enable`) is
identical for a real subscription rule and a freshly-fetched, not-yet-
saved preview rule.

**The sync endpoint's save condition differs from every other
subscriptions handler's `maybe_save()`**: Go's `SyncSubscription` only
calls `SaveConfig()` when `changed && save != "false"`, whereas
create/delete/put always save on success regardless of any "did
anything actually change" concept (they have none). Reusing the
existing `maybe_save()` helper unconditionally would have broken this
exact gate, so `handle_sync_subscription` inlines the `save`-query-
param check itself, guarded by `changed` from
`mt_app_sync_subscription_by_id`'s out-param.

**Found empirically, not just theoretically: writing
`tests/unit/test_subscriptions_api.c`'s new sync tests hit the exact
blocking-fetch hazard flagged in D-33.** The first attempt put the stub
subscription-list server on the *same* `mt_loop`/thread as the
subscriptions-API server under test (following `test_sub_fetch.c`'s
single-loop pattern). Every sync-endpoint test then hung until the
client socket's own 2-second read timeout: the sync handler blocks that
shared loop thread inside `curl_easy_perform()`, so the same thread that
would need to run `epoll_wait` to accept/service the stub server's
incoming connection from libcurl is busy — the fetch can only ever
un-block via its own `MT_SUB_FETCH_TIMEOUT_SECONDS` timeout, not by
completing. Fixed by giving the stub server its own separate
`mt_loop`/thread (matching `test_sub_fetch.c`'s and `test_sub_sync.c`'s
*actual* client/server thread separation, where the fetch is always
issued from a thread other than the one serving the stub). This is the
same hazard D-33 already named for the real daemon (HTTP handlers and
the future auto-update timer both running on the *one* production event
loop) — this test failure is direct, reproducible evidence of it, not
speculation.

**Differential suite extended** (`tests/differential/http_contract/contract.py`):
a small stdlib `http.server.HTTPServer` stub (`start_stub_sub_server`,
port 18099) now runs for the duration of one `contract.py` invocation,
serving a fixed subscription list reachable identically by the real Go
daemon and `magitrickled-c`. New steps: `GET /subscriptions/rules`
(missing url → 400; a real fetch+parse; a connection-refused → 502) and
`POST /subscriptions/{id}/sync` (unknown id → 404; a real sync via
`url` override; a re-sync of unchanged content). Two new redaction
concerns this introduced, both handled the same way the suite already
redacts learned dynamic IDs and error text (module docstring): (1) the
freshly-fetched rules' random per-backend IDs are learned and redacted
via the existing `learn=` callback mechanism (`<SUBRULE-N>`/
`<PREVIEWRULE-N>` placeholders); (2) `lastUpdate` now genuinely reaches
the live wall-clock time on a real sync (previously always 0, since no
prior step ever triggered an actual sync) — the Go run and the C run
happen minutes apart (separate process spawns), so a literal timestamp
comparison would spuriously fail on real time skew having nothing to do
with behavior. Added a recursive `_redact_live_timestamps` pass
(applied in `Trace._canonical` alongside the existing error-body
redaction) that replaces any non-zero `lastUpdate` value with a stable
placeholder, while leaving a literal `0` alone — so the suite still
verifies the "unsynced vs. synced" transition point, just not the exact
epoch value. Ran `run_http_diff.sh` end-to-end in this sandbox (root +
real iptables available): 44 steps, byte-identical trace, `HTTP
contract: OK`. Also updated the stale Phase-6-era comment explaining why
subscriptions stay `enable:false` in this suite — no longer "the C
port has no subscription-ruleset runtime" (Phase 7/D-32 built that);
now it's "avoid an `eth0`-dependent real-netfilter side effect
unrelated to what this suite checks."

Verified: full suite (28 unit-test binaries, including 7 new HTTP-level
tests in `test_subscriptions_api.c` against a real two-loop harness),
`static_analysis` (0 warnings), `sanitize` (0 findings), and the
extended `run_http_diff.sh` (44/44 steps identical, Go vs C) all clean.

## D-35: Subscription auto-update timer (timerfd, 1-minute tick) and SIGHUP config reload

**Auto-update**: a straightforward port of `StartSubscriptionAutoUpdate`
onto `mt_loop_add_timer`. Go's `time.NewTicker(time.Minute)` preceded by
one immediate `SyncDueSubscriptions` call becomes a single
`mt_loop_add_timer(d.loop, 0, MT_SUBSCRIPTION_AUTO_UPDATE_INTERVAL_MS, ...)`
— `initial_ms=0` fires almost immediately (`loop.c` already special-cases
zero to a 1ns `it_value`, since an all-zero `itimerspec` disarms a
timerfd rather than firing it) and then every 60s after, so one timer
registration reproduces "fire now, then every minute" without a separate
manual bootstrap call. The callback calls `mt_app_sync_due_subscriptions`
and — mirroring Go's `if changed { SaveConfig }` — saves the config file
only when it reports a change, via `mt_app_save_config` (the same
maybe-save shape used by the HTTP subscription handlers, since
`mt_app_sync_due_subscriptions` itself never saves — D-33). Runs on the
same event-loop thread as DNS/HTTP, so it inherits the blocking-fetch
tradeoff already named in D-33/D-34: a due subscription with a slow
upstream stalls the whole daemon for the fetch's duration, once a
minute. `mt_sub_fetch_global_init()`/`_cleanup()` (D-31 had deferred
wiring this) are now called at daemon startup/shutdown — load-bearing
for the first time in Phase 7, since this timer (and the sync HTTP
endpoints, D-34) are the first production code paths that actually
invoke `mt_sub_fetch_list`.

**SIGHUP reload**: replaces the previous "reload not implemented yet"
stub with a real port of Go's `case syscall.SIGHUP: app.LoadConfig()`.
Re-reads the config file into a scratch `mt_config_t` (fresh
`mt_config_init_defaults` + `mt_config_load_file`, exactly like
startup); a missing file is a no-op (matches Go's
`errors.Is(err, os.ErrNotExist) { return nil }`), any other parse error
is logged and also a no-op (nothing is applied) rather than leaving the
live config half-migrated.

Two things are actually applied, deliberately not everything Go's
`LoadConfig` touches:

1. **Groups and subscriptions**, gated on `mt_config_t`'s
   `groups_present`/`subscriptions_present` flags (already tracked since
   Phase 2 for exactly this "was the YAML key present at all" distinction
   — mirrors Go's `cfg.Groups != nil`/`cfg.Subscriptions != nil` checks).
   Groups: `mt_app_clear_groups` (disable + empty) then one
   `mt_app_add_group` per freshly-parsed group, transferring ownership of
   each `mt_group_t*` out of the scratch config as it's consumed (NULLing
   the source slot so the scratch config's later `mt_config_clear` can't
   double-free it) — mirrors `LoadConfig`'s own "disable all, rebuild
   fresh" loop, including stopping at the first `addGroupLocked` failure
   and leaving whatever was already re-added in place (Go doesn't roll
   those back either). Subscriptions: one `mt_app_replace_subscriptions`
   call with the whole freshly-parsed array (ownership of the array and
   every element transferred the same way) — this already IS a wholesale
   replace with its own rollback-on-rebuild-failure (D-32), unlike
   groups, which have no such primitive and are reloaded one at a time
   instead.
2. **The specific handful of app-level settings Go's own runtime code
   re-reads live from `a.config` on every DNS request/record**
   (`dns.go`): `DisableFakePTR`/`DisableDropAAAA` (checked per-message)
   and `Netfilter.IPSet.AdditionalTTL` (added to every matched record's
   TTL). Traced every `a.config.*` read site in `dns.go`/`start.go`
   specifically to answer "what does Go's own reload actually change
   live, versus just update in an already-inert copy" before deciding
   this split — `Host`/`Upstream`/`MaxIdleConns`/`MaxConcurrent`/`Timeout`,
   the netfilter chain/table prefixes, `StartMarkTableIndex`,
   `DisableIPv4`/`DisableIPv6`, the `link` list, and `LogLevel` are all
   read exactly once in `start.go` to construct already-running
   subsystems (the proxy listener, the netfilter helper, the port-53
   remap) — Go's `LoadConfig` updates `a.config`'s in-memory copies of
   these too, but nothing reads them again afterward, so they are no
   more "live" in Go than in this port. New setters
   (`mt_dnsproxy_set_disable_flags`, `mt_dns_pipeline_set_additional_ttl`)
   were added specifically to make the two genuinely-live settings behave
   identically in C — not a shortcut, a deliberate parity fix, verified
   manually (see below). Every other field is left as a documented scope
   boundary rather than silently ignored.

**Verified manually against the real daemon binary** (no dedicated
`main.c` test binary exists; this exercises code paths the unit suite
can't reach): started `magitrickled-c` with an empty config, appended a
group to the config file on disk, sent `SIGHUP`, and confirmed
`GET /api/v1/groups` immediately showed the reloaded group over HTTP;
repeated with a second reload back to empty plus a subscription add,
then a clean `SIGTERM` shutdown -- all under the sanitize build
(ASan+UBSan instrumented `magitrickled-c`), zero findings across two
reload cycles, the auto-update timer's immediate first tick, and
shutdown. Also re-ran the full unit suite (28 binaries), `static_analysis`
(0 warnings), `sanitize` (0 findings), and `run_http_diff.sh` (44/44
steps, Go vs C byte-identical, confirming `mt_sub_fetch_global_init`
wired into startup didn't change any observable behavior) — all clean.
