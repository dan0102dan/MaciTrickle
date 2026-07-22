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

Status: proposed. Duration strings + bare-int-nanoseconds parsing, legacy
ms/s normalization preserved (contract §1), canonical save order fixed by
explicit emitter sequence, unknown keys ignored. Differential tests are
the gate.

## D-05 — JSON via cJSON

Status: proposed. Payloads are small; availability in both feeds decides.

## D-06 — HTTP server: own bounded HTTP/1.1 on the shared loop

Status: proposed, pending Phase 1 spike vs libmicrohttpd. If the spike
shows >2 weeks of hardening effort, fall back to libmicrohttpd (LGPL,
in feeds). Hard limits (header 8 KB, body 1 MB, conns 64, timeouts) —
documented hardening, covered by tests.

## D-07 — Regex: PCRE2 with corpus-proven parity + explicit failure mode

Status: proposed. regexp2 (.NET) parity proven for the known corpus;
patterns that fail to compile in PCRE2 are rejected at load with a clear
log/API error (rule matches nothing — same net effect as Go's invalid-regex
behaviour), never silently rewritten. Add match/depth limits (regex DoS
hardening — Go version has none). Divergences documented in release notes.

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
