# MagiTrickle C backend

Phase 1 foundation of the Go→C migration
(план и статус: `docs/c-rewrite/migration-plan.md`; контракты:
`docs/c-rewrite/compatibility-contract.md`). The Go backend in
`src/backend/` remains the production implementation and behavioural
oracle until Phase 9.

## Layout

```
include/magitrickle/   public module headers (err, log, queue, lifecycle, loop)
src/
  main/                daemon entry (skeleton: loop + signals + logging)
  core/                lifecycle (partial-init unwinding)
  logging/             leveled console logger (zerolog-compatible levels)
  platform/            Linux-only layer: epoll/timerfd/signalfd/eventfd loop
  util/                error model, bounded queue
tests/
  unit/                greatest.h-based unit tests
  vendor/              vendored test framework (greatest.h, ISC)
  differential/        run_diff.sh — Go↔C parity suites (grows per phase)
spikes/
  regex_corpus/        dlclark/regexp2 vs PCRE2 corpus (+known divergences)
  yaml_emit/           go-yaml v2 vs libyaml byte-shape check
```

## Commands

```sh
make                   # host build -> build/host/magitrickled-c
make test              # unit tests
make sanitize          # ASan+UBSan test run
make static_analysis   # clang-tidy (.clang-tidy) + cppcheck
make CROSS_COMPILE=mipsel-linux-gnu- [SYSROOT=...]   # cross skeleton
sh tests/differential/run_diff.sh                    # parity suites
```

CI (`.github/workflows/check-c.yml`) builds with `-Werror`, runs tests,
sanitizers, static analysis, differential suites and a mipsel cross build.

## Rules

- C11; `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2 -Wundef
  -Wstrict-prototypes -Wmissing-prototypes`; POSIX.1-2008 API project-wide.
- GNU/Linux-specific APIs live only under `src/platform/` (D-14).
- Every queue/cache is bounded with an explicit overflow policy and a
  dropped-counter (см. `include/magitrickle/queue.h`).
- Init/destroy симметричны; частичная инициализация разматывается через
  `mt_lifecycle` (см. `include/magitrickle/lifecycle.h`).
- Никакой работы с DNS/netfilter здесь до соответствующих фаз плана.
