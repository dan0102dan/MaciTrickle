# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

MagiTrickle is a DNS-based traffic routing utility for routers (Entware/OpenWrt). It intercepts DNS queries via a MITM proxy, caches resolved IPs, matches them against domain rules, and programs iptables/ipset to route matched traffic through a specified network interface.

## Build System

The project uses a Makefile with stamp-based incremental builds. Before building, copy and configure `.config.example` to `.config`:

```sh
cp .config.example .config
# Edit PLATFORM, TARGET, CROSS_COMPILE, SYSROOT etc.
```

Key make targets:
```sh
make all              # download deps + build + package
make build            # build backend and frontend only
make build_backend    # C daemon binary only
make build_frontend   # Svelte frontend only
make package          # create .ipk / .apk packages
make clean            # remove all build artifacts
make clear            # remove build dir for current PLATFORM/TARGET + frontend dist
```

Build outputs land in `.build/<PLATFORM>_<TARGET>/`. The final packages appear in `.build/`.

Two supported platforms (set via `PLATFORM` in `.config`):
- `entware` — Keenetic and similar; targets like `mipsel-3.4`, `aarch64-3.10`. The `_kn` suffix targets set `ENTWARE_KN=1` (Keenetic-specific ignored-interfaces list) and require `socat`.
- `openwrt` — targets like `aarch64_cortex-a53`.

`CROSS_COMPILE` (toolchain prefix, e.g. `mipsel-linux-gnu-`) and `SYSROOT` are empty by default, which builds natively for the host. Real per-target cross-toolchains + sysroots are not yet provisioned in CI (see `docs/c-rewrite/toolchains.md` and `decisions.md` D-45) — `build.yml`'s packaging matrix explicitly marks every target "not yet automated" rather than risk shipping a mislabeled host binary.

## Backend

**Language**: C11  
**Entry point**: `src/backend-c/src/main/main.c`  
**Build**: `src/backend-c/Makefile` (standalone; invoked by the root Makefile via `build_backend`)

Run tests (from `src/backend-c/`):
```sh
make test              # unit tests (greatest.h), one binary per module
make sanitize           # unit tests under ASan+UBSan
make static_analysis    # clang-tidy + cppcheck
make fuzz               # libFuzzer targets (DNS wire, YAML load, HTTP request line)
sudo -E env "PATH=$PATH" sh tests/differential/run_diff.sh   # regression suites vs golden/ snapshots
```

### Architecture

`src/backend-c/src/main/main.c` wires up the DNS MITM proxy, netfilter engine, HTTP/Unix API servers, and the netlink watcher — the C equivalent of the old Go `App`.

| Directory | Purpose |
|---|---|
| `src/core/` | Lifecycle state machine shared by long-running components |
| `src/platform/` | epoll-based event loop |
| `src/config/` | `duration`, `id`, `models` (Group/Rule/Subscription/AppConfig), YAML load/save via libyaml |
| `src/rules/` | Rule matching (`domain`/`namespace`/`wildcard`/`regex` via PCRE2/`subnet`/`subnet6`) + rule-set snapshot aggregation |
| `src/dns/` | DNS wire parser/packer + MITM proxy transport + pipeline (cache + rule matching hookup) |
| `src/dns_cache/` | In-memory DNS A/AAAA/CNAME cache with TTL cleanup |
| `src/iptables/` | iptables Rule/chain-patch/chain-override/chain-delete engine + real fork/exec executable |
| `src/netfilter/` | ipset CRUD via libmnl, ipset-to-link wiring, port remap (53→3553), cleanup |
| `src/netlink/` | rtnetlink link/address watcher |
| `src/api/` | JSON helpers (cJSON), bounded HTTP/1.1 server + router + Unix socket, auth/JWT, group/rule/subscription CRUD handlers, static skin serving |
| `src/crypto/` | MD5/SHA-256/SHA-512, crypt, HMAC, JWT, base64 |
| `src/subscriptions/` | List parsing/validation, libcurl-based fetch, sync flows, auto-update scheduler |
| `src/tools/` | `mt-configtool`/`mt-dnstool`/`mt-cachetool` — standalone CLI helpers used by tests and the differential suite |

### DNS flow

1. iptables `nat PREROUTING` redirects port 53 → 3553 (configurable, can be disabled)
2. The DNS proxy forwards queries to upstream (default `127.0.0.1:53`) and intercepts responses
3. The response pipeline processes A/AAAA/CNAME records
4. Matching IPs are added to the group's ipset with TTL = DNS TTL + `AdditionalTTL` (default 3600s)
5. iptables routes packets from the ipset through the group's configured interface

### Rule types

`domain` (exact), `namespace` (domain + subdomains), `wildcard` (`*`/`?`), `regex` (PCRE2, with a handful of documented divergences from the original dlclark/regexp2 behavior — see `docs/c-rewrite/decisions.md` D-07), `subnet` (IPv4 CIDR), `subnet6` (IPv6 CIDR).

## Frontend

**Location**: `src/frontend/`  
**Stack**: Svelte 5, TypeScript, Vite, Prettier

```sh
cd src/frontend
npm install
npm run dev:frontend    # Vite dev server (needs separate backend or mock)
npm run dev:backend     # Deno mock backend (API mock for local UI dev)
npm run build           # production build → dist/
npm run check           # svelte-check + tsc type check
npm run format          # Prettier format
npm run format:check    # Prettier check (CI)
npm run test:e2e        # Playwright end-to-end tests
npm run test:unit       # Deno unit tests
```

For local development, run both `dev:backend` (Deno mock at `dev/backend-mock.ts`) and `dev:frontend` in separate terminals.

Built frontend is placed into the package at `usr/share/magitrickle/skins/default/` and served by the HTTP server. The active skin is set by `HTTPWeb.Skin` config (default: `"default"`).

## Configuration

Runtime config is YAML; filesystem paths (share dir, state dir, socket, passwd/shadow) are in `src/backend-c/include/magitrickle/paths.h`, overridable via `-D`. Defaults set in `src/backend-c/src/config/models.c`:
- DNS proxy listens on `[::]`:3553, upstream `127.0.0.1:53`
- HTTP WebUI at `[::]`:8080
- iptables chain prefix `MT_`, ipset prefix `mt_`
- Default monitored interface: `br0`

## CI

- `.github/workflows/check.yml` — frontend unit + e2e tests
- `.github/workflows/check-c.yml` — C backend build (warnings-as-errors), unit tests, sanitizers, static analysis, fuzz smoke, differential/regression suites, mipsel cross-build skeleton
- `.github/workflows/build.yml` — builds a matrix of all configs under `config/*/`; packaging is currently gated on a real per-target cross-toolchain + sysroot being provisioned (not yet done — see `docs/c-rewrite/toolchains.md` and `decisions.md` D-45), so every target is explicitly marked "not yet automated" rather than risk a mislabeled binary. To add a new target, add a `.config` file in the appropriate `config/<platform>/` directory.

## History

This project was originally implemented in Go; the Go backend (`src/backend/`) was fully rewritten in C11 (`src/backend-c/`) and removed once parity was verified. See `docs/c-rewrite/` for the full migration plan, phase-by-phase decisions log, and parity checklist.