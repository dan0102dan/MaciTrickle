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

Status: proposed. Payloads are small; availability in both feeds decides.

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
