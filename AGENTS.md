# AGENTS.md

Durable instructions for AI coding agents working in this repository.
Keep this file free of transient progress notes and benchmark numbers.

## Project structure

- `src/backend/` — **Go backend (current production implementation).**
  Module `magitrickle`, entry `cmd/magitrickled/main.go`. It is the
  reference implementation for the ongoing C rewrite and MUST NOT be
  removed or behaviourally changed until the rewrite's Phase 9 gate
  (see `docs/c-rewrite/migration-plan.md`).
- `src/backend-c/` — C backend (appears from Phase 1 of the rewrite).
- `src/frontend/` — Svelte 5 + TypeScript WebUI (Vite build, Deno mock
  backend in `dev/`, Playwright e2e in `tests/`). Not being rewritten;
  only confirmed compatibility fixes are allowed, no redesigns.
- `config/entware/*.config`, `config/openwrt/*.config` — the build target
  matrix. This is the single source of truth for supported targets; never
  hand-edit a hardcoded target list elsewhere, never drop a target
  silently.
- `files/` — packaging payload (init scripts, default config, hooks) for
  common/entware/entware_kn/openwrt.
- `tools/bench/` — benchmark tooling (dnsstub, dnsload, genconfig,
  configbench, run_baseline.sh). Raw results live in
  `docs/c-rewrite/baseline-raw/`.
- `docs/c-rewrite/` — C rewrite audit, contracts, plans, decisions.
  `docs/swagger.yaml` — HTTP API reference.

## Build

```sh
cp .config.example .config   # choose PLATFORM/TARGET
make build_backend           # Go binary (later: C binary)
make build_frontend          # Svelte dist/
make package                 # .ipk (Entware/OpenWrt) / .apk (OpenWrt)
make clean | make clear
```

Outputs: `.build/<PLATFORM>_<TARGET>/`, packages in `.build/`.
Do not remove or rename the user-facing make targets
(`all`, `build`, `build_backend`, `build_frontend`, `package`, `clean`,
`clear`).

## Test

```sh
cd src/backend
go vet ./...
go test ./...                 # main suite
go test -tags testing ./utils/iptables/   # fake-executable iptables tests
go test -tags entware_kn ./internal/interfaces/  # Keenetic-specific tests

cd src/frontend
npm run check                 # svelte-check + tsc
npm run format:check          # prettier (CI)
npm run test:unit             # Deno unit tests
npm run test:e2e              # Playwright (needs built/mocked backend)
```

Benchmarks: `sh tools/bench/run_baseline.sh` (root; see script header for
requirements and env vars).

## Formatting / style

- Go: gofmt (implicit); do not reformat unrelated files.
- Frontend: Prettier via `npm run format`.
- C (from Phase 1): C11; warning set and `make sanitize`/
  `make static_analysis` defined in `src/backend-c/`; GNU/Linux extensions
  only inside the platform layer.

## Target platforms & compatibility constraints

- Entware (Keenetic etc.): **glibc**, `/opt` prefix, kernels ≥3.2/3.4,
  targets incl. big-endian mips and softfloat arm. `_kn` targets add the
  `entware_kn` build tag (Keenetic RCI, ignored interfaces, ndm hook,
  socat dep).
- OpenWrt: **musl**, procd init, `.ipk` (opkg, ≤24.10) and `.apk`
  (≥25.12) from the same rootfs.
- Never link Entware binaries against musl or host glibc; never build
  OpenWrt binaries against host glibc; no `-march=native`; no static
  glibc.
- Wire formats (DNS, netlink) must use explicit byte-order access — BE
  mips targets are first-class.
- Binary/package size is NOT an optimization goal.

## C rewrite ground rules

1. Phase order is defined in `docs/c-rewrite/migration-plan.md`; do not
   skip phases or start later-phase work early without updating the plan.
2. The Go backend is the behavioural oracle. Behaviour contracts live in
   `docs/c-rewrite/compatibility-contract.md`; check them before changing
   anything observable, and record intentional divergences in
   `docs/c-rewrite/decisions.md` — never diverge silently.
3. **Do not delete the Go backend, its tests, or its CI** until every
   condition in migration-plan Phase 9 / spec §23 is met; removal is its
   own separate change.
4. No performance claims without `tools/bench` measurements attached.
5. Config YAML field names, defaults, API routes/status codes, ipset/chain
   naming (`mt_`/`MT_` prefixes), and file paths are frozen contracts.

## Git etiquette

- Run `git status` before changes; never overwrite uncommitted user work;
  never rewrite history; small, logically complete commits.
