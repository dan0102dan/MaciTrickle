# MagiTrickle C backend — build system (Phase 1 foundation).
#
# Host build:        make            (-> build/host/magitrickled-c)
# Tests:             make test
# Sanitizers:        make sanitize   (ASan+UBSan build of tests, host only)
# Static analysis:   make static_analysis (clang-tidy + cppcheck if present)
# Cross build:       make CROSS_COMPILE=mipsel-openwrt-linux- \
#                         SYSROOT=/path/to/sysroot BUILD=cross-mipsel
#   Per-target CFLAGS/LDFLAGS come from the toolchain descriptor
#   (docs/c-rewrite/toolchains.md); never use -march=native.

CROSS_COMPILE ?=
CC       := $(CROSS_COMPILE)gcc
BUILD    ?= $(if $(CROSS_COMPILE),cross-$(patsubst %-,%,$(CROSS_COMPILE)),host)
BUILDDIR := build/$(BUILD)
MT_VERSION ?= unattached

WARNINGS := -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2 \
            -Wundef -Wstrict-prototypes -Wmissing-prototypes
# NOTE: -Werror is enabled in CI only (see .github/workflows), not forced
# on local builds yet (spec §20).
CSTD     := -std=c11
OPT      ?= -O2
CFLAGS   ?=
# POSIX.1-2008 API surface project-wide; GNU extensions only inside
# src/platform/ (which defines _GNU_SOURCE itself).
CFLAGS   += $(CSTD) $(OPT) $(WARNINGS) -Iinclude -MMD -MP \
            -D_POSIX_C_SOURCE=200809L \
            -DMT_VERSION=\"$(MT_VERSION)\" -fno-common
# CI passes CFLAGS_EXTRA=-Werror; local builds stay warning-tolerant.
CFLAGS_EXTRA ?=
CFLAGS   += $(CFLAGS_EXTRA)
LDFLAGS  ?=
LDLIBS   := -lpthread
ifneq ($(SYSROOT),)
CFLAGS  += --sysroot=$(SYSROOT)
LDFLAGS += --sysroot=$(SYSROOT)
endif

# Core: no external library dependencies (cross-buildable everywhere).
CORE_SRCS := \
    src/util/err.c \
    src/util/queue.c \
    src/core/lifecycle.c \
    src/logging/log.c \
    src/platform/loop.c

# Config/rules/subscriptions: need libyaml + libpcre2-8 (host + feed
# sysroots; the cross skeleton target builds core only until Phase 8
# toolchain descriptors provide the dep sysroots).
CONFIG_SRCS := \
    src/config/duration.c \
    src/config/id.c \
    src/config/models.c \
    src/config/yaml_scalar.c \
    src/config/yaml_load.c \
    src/config/yaml_save.c \
    src/rules/wildcard.c \
    src/rules/match.c \
    src/subscriptions/subparse.c

LIB_SRCS := $(CORE_SRCS) $(CONFIG_SRCS)

MAIN_SRC := src/main/main.c
CONFIGTOOL_SRC := src/tools/configtool.c

CORE_OBJS := $(CORE_SRCS:%.c=$(BUILDDIR)/%.o)
CONFIG_OBJS := $(CONFIG_SRCS:%.c=$(BUILDDIR)/%.o)
LIB_OBJS := $(CORE_OBJS) $(CONFIG_OBJS)
MAIN_OBJ := $(MAIN_SRC:%.c=$(BUILDDIR)/%.o)
CONFIGTOOL_OBJ := $(CONFIGTOOL_SRC:%.c=$(BUILDDIR)/%.o)

DEP_LIBS := -lyaml -lpcre2-8

TEST_SRCS := $(wildcard tests/unit/*.c)
TEST_BINS := $(TEST_SRCS:tests/unit/%.c=$(BUILDDIR)/tests/%)

.PHONY: all core clean test sanitize static_analysis

ifneq ($(CROSS_COMPILE),)
all: core
else
all: $(BUILDDIR)/magitrickled-c $(BUILDDIR)/mt-configtool
endif

core: $(BUILDDIR)/magitrickled-c

$(BUILDDIR)/magitrickled-c: $(CORE_OBJS) $(MAIN_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILDDIR)/mt-configtool: $(LIB_OBJS) $(CONFIGTOOL_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) $(DEP_LIBS)

$(BUILDDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# ---- tests ------------------------------------------------------------

$(BUILDDIR)/tests/%: tests/unit/%.c $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests/vendor $(LDFLAGS) -o $@ $< $(LIB_OBJS) $(LDLIBS) $(DEP_LIBS)

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; $$t; done

sanitize:
	$(MAKE) BUILD=sanitize \
	    OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
	    LDFLAGS="-fsanitize=address,undefined" test

# ---- analysis ---------------------------------------------------------

static_analysis:
	@if command -v clang-tidy >/dev/null; then \
	    clang-tidy --quiet $(LIB_SRCS) $(MAIN_SRC) $(CONFIGTOOL_SRC) -- $(CSTD) -Iinclude \
	        -D_POSIX_C_SOURCE=200809L -DMT_VERSION=\"$(MT_VERSION)\"; \
	else echo "clang-tidy not found (skipped)"; fi
	@if command -v cppcheck >/dev/null; then \
	    cppcheck --std=c11 --enable=warning,performance,portability \
	        --error-exitcode=1 --inline-suppr -Iinclude \
	        -D_POSIX_C_SOURCE=200809L \
	        $(LIB_SRCS) $(MAIN_SRC) $(CONFIGTOOL_SRC); \
	else echo "cppcheck not found (skipped)"; fi

clean:
	rm -rf build

-include $(LIB_OBJS:.o=.d) $(MAIN_OBJ:.o=.d)
