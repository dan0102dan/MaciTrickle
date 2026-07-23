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
