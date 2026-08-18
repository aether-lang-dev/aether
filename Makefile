.PHONY: all clean test compiler examples examples-run ci

# Pin the default goal to `all` regardless of rule order. Without this, the
# first FILE-target rule defined becomes the default goal — and the generated
# version-header rule ($(VERSION_HEADER)) is defined above `all:`, so a bare
# `make` would build only the header. Explicit default goal makes rule order
# irrelevant.
.DEFAULT_GOAL := all

# Detect OS and shell environment.
# WINDOWS_NATIVE is set only for pure Windows (mingw32-make + cmd.exe).
# IS_WINDOWS is set for any Windows variant (native, MSYS2, MinGW, Cygwin).
WINDOWS_NATIVE :=
IS_WINDOWS :=
# WINDOWS=1 — cross-build FOR Windows from a non-Windows host (see the
# "Windows cross-build" block below). Declared here, ahead of host
# detection, because every downstream conditional keys off IS_WINDOWS /
# EXE_EXT: the cross build wants the Windows CODE PATHS with a Linux
# host's shell utilities, so it sets those two and leaves WINDOWS_NATIVE
# (which switches to cmd.exe-isms like `if not exist`) unset.
ifeq ($(WINDOWS),1)
    IS_WINDOWS := 1
    EXE_EXT := .exe
    DETECTED_OS := MINGW64_NT-cross
endif
ifeq ($(OS),Windows_NT)
    IS_WINDOWS := 1
    _UNAME_S := $(shell uname -s 2>&1)
    ifneq ($(findstring MINGW,$(_UNAME_S)),)
        DETECTED_OS := $(_UNAME_S)
        EXE_EXT := .exe
    else ifneq ($(findstring MSYS,$(_UNAME_S)),)
        DETECTED_OS := $(_UNAME_S)
        EXE_EXT := .exe
    else ifneq ($(findstring CYGWIN,$(_UNAME_S)),)
        DETECTED_OS := $(_UNAME_S)
        EXE_EXT := .exe
    else
        DETECTED_OS := Windows
        EXE_EXT := .exe
        WINDOWS_NATIVE := 1
    endif
else ifneq ($(WINDOWS),1)
    DETECTED_OS := $(shell uname -s)
    ifneq ($(findstring MINGW,$(DETECTED_OS)),)
        EXE_EXT := .exe
        IS_WINDOWS := 1
    else ifneq ($(findstring MSYS,$(DETECTED_OS)),)
        EXE_EXT := .exe
        IS_WINDOWS := 1
    else ifneq ($(findstring CYGWIN,$(DETECTED_OS)),)
        EXE_EXT := .exe
        IS_WINDOWS := 1
    else
        EXE_EXT :=
    endif
endif

ifdef WINDOWS_NATIVE
    PATH_SEP := \\
    MKDIR := if not exist
    RM := del /Q
    RM_DIR := rd /S /Q
else
    PATH_SEP := /
    MKDIR := mkdir -p
    RM := rm -f
    RM_DIR := rm -rf
endif

# Parallel job count (override with: make test-ae NPROC=8)
ifdef WINDOWS_NATIVE
NPROC ?= $(shell echo %NUMBER_OF_PROCESSORS% 2>nul || echo 4)
else
NPROC ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
endif

# Version: the VERSION FILE IN THIS TREE is authoritative; the highest git tag
# is only a fallback for an untagged dev checkout.
#
# This ordering used to be reversed ("prefer highest git tag"), which mislabels
# a tagged release build. `git tag -l` reports the tags VISIBLE IN THIS CLONE,
# and release.yml's build jobs check out shallow, so the visible set can be
# stale or partial — `tail -1` then lands on an OLDER tag and overrides the
# correct VERSION file. Measured: v0.516.0 shipped an `ae --version` saying
# 0.417.0, while `git show v0.516.0:VERSION` said 0.516.0 all along. The tree
# was right and the tag scan was wrong.
#
# For a tagged build the two should be identical, so preferring the tree costs
# nothing and is immune to clone depth. For a tarball there is no git at all.
# Only an untagged dev checkout — where VERSION is still the 0.0.0 placeholder
# — consults tags, so `make` on a plain main still reports a real-ish number.
#
# One arm now, not two: `cat` covers Windows too (the WINDOWS_NATIVE split
# existed because `git tag` shell-outs are awkward under cmd.exe; a file read
# is not). WINDOWS_NATIVE keeps `type` since cmd.exe has no `cat`.
ifdef WINDOWS_NATIVE
VERSION := $(shell type VERSION 2>nul || echo 0.0.0)
else
VERSION := $(shell cat VERSION 2>/dev/null | tr -d '[:space:]')
ifeq ($(VERSION),)
VERSION := 0.0.0
endif
ifeq ($(VERSION),0.0.0)
VERSION := $(shell git tag -l 'v*.*.*' 2>/dev/null | sed 's/^v//' | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)
ifeq ($(VERSION),)
VERSION := 0.0.0
endif
endif
endif

# Numeric version components, split from $(VERSION) (MAJOR.MINOR.PATCH). A
# trailing pre-release suffix on PATCH (e.g. "0-dev") is stripped to digits.
# These feed the generated runtime/aether_version.h so the macro, the two
# VERSION sidecars, and `aetherc --version` all derive from one source.
VERSION_MAJOR := $(shell echo "$(VERSION)" | cut -d. -f1 | tr -dc 0-9)
VERSION_MINOR := $(shell echo "$(VERSION)" | cut -d. -f2 | tr -dc 0-9)
VERSION_PATCH := $(shell echo "$(VERSION)" | cut -d. -f3 | tr -dc 0-9)
# MAJOR*1_000_000 + MINOR*1_000 + PATCH — the monotonic form for #if gating.
VERSION_NUM := $(shell echo $$(( $(VERSION_MAJOR)*1000000 + $(VERSION_MINOR)*1000 + $(VERSION_PATCH) )))

# Generated, dependency-free version header. Baked into the exact header bytes
# consumers compile against (installed to include/aether/runtime/ by the normal
# `find runtime -name '*.h'` walk), so it cannot desync from the compiler.
# GENERATED — do not commit / edit by hand (gitignored).
#
# This is a real FILE target (depends on VERSION + Makefile), NOT a phony one:
# a phony target used as a build prerequisite makes GNU make treat the depending
# target as perpetually out-of-date in ways that can short-circuit its recipe.
# Build targets take it as an ORDER-ONLY prerequisite (`| $(VERSION_HEADER)`),
# so it's built if missing but never forces a rebuild. It's regenerated only
# when VERSION or the Makefile changes, and written atomically (tmp + mv) and
# only on content change, so the tree is never spuriously dirtied.
VERSION_HEADER := runtime/aether_version.h
$(VERSION_HEADER): VERSION Makefile
	@tmp="$(VERSION_HEADER).tmp.$$$$"; \
	{ \
	  echo "/* Aether SDK version — GENERATED by the Makefile from \$$(VERSION)."; \
	  echo " * Do not edit by hand. Dependency-free (#defines only) so anything,"; \
	  echo " * including the --emit=csrc prologue, can include it cheaply. This is"; \
	  echo " * the static, drift-proof complement to AetherLibMeta.aether_version"; \
	  echo " * (per-artifact runtime metadata). */"; \
	  echo "#ifndef AETHER_VERSION_H"; \
	  echo "#define AETHER_VERSION_H"; \
	  echo ""; \
	  echo "#define AETHER_VERSION       \"$(VERSION)\""; \
	  echo "#define AETHER_VERSION_MAJOR $(VERSION_MAJOR)"; \
	  echo "#define AETHER_VERSION_MINOR $(VERSION_MINOR)"; \
	  echo "#define AETHER_VERSION_PATCH $(VERSION_PATCH)"; \
	  echo "/* MAJOR*1000000 + MINOR*1000 + PATCH — monotonic, for #if gating:"; \
	  echo " *   #if AETHER_VERSION_NUM < 390000"; \
	  echo " *   #  error \"requires Aether >= 0.390\""; \
	  echo " *   #endif */"; \
	  echo "#define AETHER_VERSION_NUM   $(VERSION_NUM)"; \
	  echo ""; \
	  echo "#endif /* AETHER_VERSION_H */"; \
	} > "$$tmp"; \
	if [ -f "$(VERSION_HEADER)" ] && cmp -s "$$tmp" "$(VERSION_HEADER)"; then \
	  rm -f "$$tmp"; \
	else \
	  mv "$$tmp" "$(VERSION_HEADER)"; \
	  echo "Generated $(VERSION_HEADER) (v$(VERSION))"; \
	fi
	@mkdir -p build 2>/dev/null || true
	@echo "$(BUILD_TARGET_ID)" > "$(BUILD_TARGET_STAMP)"

# Convenience phony alias for explicit regeneration (e.g. `make gen-version-header`).
.PHONY: gen-version-header
gen-version-header: $(VERSION_HEADER)

# #1366: the bare C symbols the stdlib exposes. Codegen renames a user function
# that would collide with one, so this must track std/ automatically: a
# hand-kept list is what let string_replace_all reach a release.
# GENERATED, do not commit or edit by hand (gitignored).
STDLIB_SYMS_HEADER := compiler/codegen/aether_stdlib_symbols.h
STDLIB_MODULE_AE := $(wildcard std/*/module.ae) $(wildcard std/*/*/module.ae)
$(STDLIB_SYMS_HEADER): $(STDLIB_MODULE_AE) Makefile
	@tmp="$(STDLIB_SYMS_HEADER).tmp.$$$$"; \
	{ \
	  echo "/* GENERATED by the Makefile from the std module.ae files. Do not edit. */"; \
	  echo "#ifndef AETHER_STDLIB_SYMBOLS_H"; \
	  echo "#define AETHER_STDLIB_SYMBOLS_H"; \
	  echo "/* Sorted for bsearch. */"; \
	  echo "static const char* const AETHER_STDLIB_SYMBOLS[] = {"; \
	  grep -rhoE '^extern[[:space:]]+[A-Za-z_][A-Za-z0-9_]*' $(STDLIB_MODULE_AE) 2>/dev/null \
	    | awk '{print $$2}' | LC_ALL=C sort -u | sed 's/.*/    "&",/'; \
	  echo "};"; \
	  echo "#define AETHER_STDLIB_SYMBOL_COUNT \\"; \
	  echo "    (sizeof(AETHER_STDLIB_SYMBOLS) / sizeof(AETHER_STDLIB_SYMBOLS[0]))"; \
	  echo "#endif"; \
	} > "$$tmp"; \
	if [ -f "$(STDLIB_SYMS_HEADER)" ] && cmp -s "$$tmp" "$(STDLIB_SYMS_HEADER)"; then \
	  rm -f "$$tmp"; \
	else \
	  mv "$$tmp" "$(STDLIB_SYMS_HEADER)"; \
	  echo "Generated $(STDLIB_SYMS_HEADER)"; \
	fi

.PHONY: gen-stdlib-symbols
gen-stdlib-symbols: $(STDLIB_SYMS_HEADER)

# Compiler configuration with ccache support.
# FreeBSD base has no gcc at all (clang is installed as `cc`).
ifdef WINDOWS_NATIVE
CC := gcc
else
BASE_CC := $(shell if command -v gcc >/dev/null 2>&1; then echo gcc; \
                   elif command -v cc >/dev/null 2>&1; then echo cc; \
                   else echo clang; fi)
CC := $(shell command -v ccache >/dev/null 2>&1 && echo "ccache $(BASE_CC)" || echo "$(BASE_CC)")
endif
EXTRA_CFLAGS ?=
PLATFORM ?= native

# Platform-specific overrides
ifeq ($(PLATFORM),wasm)
    CC := emcc
    EXTRA_CFLAGS += -DAETHER_NO_THREADING -DAETHER_NO_FILESYSTEM -DAETHER_NO_NETWORKING
    SCHEDULER_SRC := runtime/scheduler/aether_scheduler_coop.c
else ifeq ($(PLATFORM),embedded)
    EXTRA_CFLAGS += -DAETHER_NO_THREADING -DAETHER_NO_FILESYSTEM -DAETHER_NO_NETWORKING -DAETHER_NO_GETENV
    SCHEDULER_SRC := runtime/scheduler/aether_scheduler_coop.c
else
    # Auto-detect: if EXTRA_CFLAGS disables threading, use cooperative scheduler
    ifneq ($(findstring AETHER_NO_THREADING,$(EXTRA_CFLAGS)),)
        SCHEDULER_SRC := runtime/scheduler/aether_scheduler_coop.c
    else
        SCHEDULER_SRC := runtime/scheduler/multicore_scheduler.c
    endif
endif

# ---------------------------------------------------------------------------
# FreeBSD cross-build (FREEBSD=1) — build the ae/aetherc toolchain FOR FreeBSD
# from a Linux (or any) host, via `zig cc`. GitHub Actions has no native
# FreeBSD runner, so the release FreeBSD zip is produced this way. This mirrors
# the link recipe tools/ae_cross.c uses for `ae build --target=x86_64-freebsd`,
# but applied to the compiler's own C sources through the Makefile's CC/LDFLAGS.
#
# Required inputs:
#   ZIG            path to a zig binary (>= 0.13)         e.g. .../zig
#   AETHER_SYSROOT FreeBSD base sysroot (headers + libc)  e.g. .../bases/x86_64-freebsd15
# Provision both with the aether-crossbuild repo (get-zig.sh / fetch-freebsd-base.sh).
#
# Notes:
#   - zig cc does NOT bundle a FreeBSD libc, so we link -nostdlib against the
#     base's CRT objects + versioned libc.so.7 / libthr.so.3 / libm.so.5 BY
#     PATH (the .so symlinks point at absolute /lib paths that don't exist on
#     the build host; -lthr/-lpthread/-lm don't resolve under zig-lld here).
#   - casper libs are ALWAYS needed: std/casper/aether_casper.c is
#     unconditionally in the build and references cap_*; the base ships them.
#   - Optional Tier-2 libs (OpenSSL/zlib/nghttp2/PCRE2/YAML) are forced OFF
#     for the cross build: pkg-config on the host would find the host's Linux
#     libs and poison the FreeBSD binary. A cross build ships those std
#     features as their "unavailable" stubs (same as any build without them);
#     a future crossbuild-sysroot-staged variant can re-enable them.
FREEBSD_CPU ?= x86_64
ifeq ($(FREEBSD),1)
  ifeq ($(ZIG),)
    $(error FREEBSD=1 needs ZIG=<path to zig> (>= 0.13))
  endif
  ifeq ($(AETHER_SYSROOT),)
    $(error FREEBSD=1 needs AETHER_SYSROOT=<FreeBSD base sysroot>)
  endif
  # Route through the zigcc-freebsd wrapper: on a FreeBSD -nostdlib link zig cc
  # prints a COSMETIC "libc not available" and exits nonzero even though a valid
  # binary was produced (see the wrapper + tools/ae_cross.c). The wrapper
  # forgives that ONLY when the link's -o output actually exists; real compile/
  # link errors still fail. `$(ZIG)` is passed as the wrapper's first arg.
  CC := $(CURDIR)/scripts/zigcc-freebsd.sh $(ZIG) -target $(FREEBSD_CPU)-freebsd --sysroot=$(AETHER_SYSROOT) -I$(AETHER_SYSROOT)/usr/include
  # Host pkg-config is wrong for the target — force the optional libs off.
  OPENSSL := 0
  ZLIB := 0
  NGHTTP2 := 0
  PCRE2 := 0
  YAML := 0
  # Link tail (mirrors tools/ae_cross.c): -nostdlib + CRT + versioned base
  # libs by path + casper, and the base -L dirs. Overrides the native LDFLAGS
  # assembly below (guarded by ifneq ($(FREEBSD),1)).
  FREEBSD_LDFLAGS := -nostdlib \
    $(AETHER_SYSROOT)/usr/lib/crt1.o $(AETHER_SYSROOT)/usr/lib/crti.o \
    $(AETHER_SYSROOT)/lib/libc.so.7 $(AETHER_SYSROOT)/lib/libthr.so.3 \
    $(AETHER_SYSROOT)/lib/libm.so.5 $(AETHER_SYSROOT)/usr/lib/crtn.o \
    -lcasper -lcap_pwd -lcap_sysctl -lcap_grp -lcap_dns \
    -L$(AETHER_SYSROOT)/usr/lib -L$(AETHER_SYSROOT)/lib
endif

# ---------------------------------------------------------------------------
# Windows cross-build (WINDOWS=1) — build the ae/aetherc toolchain FOR Windows
# from a Linux (or any) host, via `zig cc -target x86_64-windows-gnu` (#1592).
# Companion to FREEBSD=1 above, and deliberately simpler: zig BUNDLES the
# mingw-w64 headers and CRT sources and builds them on demand, so unlike the
# FreeBSD leg there is NO sysroot to fetch and no -nostdlib CRT bookkeeping.
#
# Required input:
#   ZIG   path to a zig binary (>= 0.13)   e.g. .../zig
# Provision with the aether-crossbuild repo's get-zig.sh (same as FreeBSD).
#
# Purpose is a FAST Windows signal on a Linux runner (#1593): the MSYS2 legs
# take ~20 minutes, so Windows-specific compile breakage is discovered long
# after the author moved on. This lane compiles the same code
# paths in a few minutes. It is an EARLIER signal, never a replacement — the
# MSYS2 jobs stay the fidelity tip.
#
# Capability-lean by design: the optional Tier-2 libs are forced OFF, exactly
# as the FreeBSD cross does and for the same reason — host pkg-config would
# find the host's Linux libs and poison the Windows binary. Cross-compiling
# OpenSSL/nghttp2 for windows-gnu is the genuinely expensive part and is out
# of scope; std ships those features as their "unavailable" stubs. Vendored
# PCRE2 needs no host library, so std.regex stays available.
WINDOWS_CPU ?= x86_64
ifeq ($(WINDOWS),1)
  ifeq ($(ZIG),)
    $(error WINDOWS=1 needs ZIG=<path to zig> (>= 0.13))
  endif
  CC := $(ZIG) cc -target $(WINDOWS_CPU)-windows-gnu
  AR := $(ZIG) ar
  RANLIB := $(ZIG) ranlib
  # Host pkg-config is wrong for the target — force the optional libs off.
  OPENSSL := 0
  ZLIB := 0
  NGHTTP2 := 0
  YAML := 0
  # PCRE2 has a vendored in-tree fallback (no host library involved), so
  # std.regex survives the cross build.
  PCRE2 := vendored
  # NB __USE_MINGW_ANSI_STDIO=1 still applies (it keys off IS_WINDOWS
  # below): zig ships mingw-w64's libmingwex, so the __mingw_* printf
  # family resolves exactly as it does under MSYS2. Verified by a clean
  # cross-build + link of compiler/ae/stdlib.
endif

# A cross build OWNS build/ for the duration: build/obj holds PE objects and
# build/libaether*.a become Windows archives, so a native build afterwards
# links host code against Windows objects and dies deep in the link with
# confusing errors. Rather than fork the whole tree layout (dozens of rules
# reference build/libaether.a by literal path), stamp the tree with the
# target it was last built for and fail loudly at the START of the next
# mismatching build. `make clean` between targets is the fix; CI runners
# build one target per job and never hit it.
# Literal `build/` (not $(BUILD_DIR)): this block runs ahead of the
# BUILD_DIR assignment further down, and the version-header rule that
# writes the stamp is earlier still.
BUILD_TARGET_STAMP := build/.build-target
ifeq ($(WINDOWS),1)
  BUILD_TARGET_ID := windows-$(WINDOWS_CPU)
else ifeq ($(FREEBSD),1)
  BUILD_TARGET_ID := freebsd-$(FREEBSD_CPU)
else
  BUILD_TARGET_ID := native-$(DETECTED_OS)
endif
_STAMPED := $(strip $(shell cat $(BUILD_TARGET_STAMP) 2>/dev/null))
# The check runs at PARSE time, so it must not fire for the very goals that
# resolve it: `make clean` (and clean-ish goals) would otherwise be
# unreachable — the guard would block the only way out of the state it is
# complaining about. $(MAKECMDGOALS) is empty for a bare `make` (== all).
_CLEAN_ONLY := $(if $(strip $(filter-out clean distclean help,$(or $(MAKECMDGOALS),all))),,1)
ifneq ($(_STAMPED),)
ifneq ($(_STAMPED),$(BUILD_TARGET_ID))
ifneq ($(_CLEAN_ONLY),1)
$(error build/ holds $(_STAMPED) artifacts but this is a $(BUILD_TARGET_ID) build. \
Object and archive formats do not mix — run `make clean` first (or build the \
other target in a separate checkout))
endif
endif
endif

# MinGW / MSYS2 (native Windows): bind the printf family to the
# C99-conformant __mingw_* implementations instead of legacy MSVCRT.
# MSVCRT mishandles the C99 conversions we emit (%lld / %llu / %zu / %g)
# and makes vsnprintf(NULL, 0, ...) return -1 instead of the would-be
# length — the latter breaks the two-pass sizing in the generated
# string-interpolation helper, yielding empty interpolated strings
# (#681). The generated `.c` also self-defines this before <stdio.h>;
# setting it here additionally covers the compiler, runtime, and stdlib
# translation units (which use snprintf/vsnprintf in os/json/etc.).
ifdef IS_WINDOWS
EXTRA_CFLAGS += -D__USE_MINGW_ANSI_STDIO=1
endif

# Optional OpenSSL detection (enables HTTPS client). Probes pkg-config;
# falls back silently if OpenSSL isn't installed — the HTTP client still
# works for `http://` URLs and returns a clean error for `https://`.
# Override with OPENSSL=0 to force-disable.
OPENSSL ?= auto
ifeq ($(OPENSSL),auto)
  OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
  OPENSSL_LDFLAGS := $(shell pkg-config --libs openssl 2>/dev/null)
else ifeq ($(OPENSSL),1)
  OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
  OPENSSL_LDFLAGS := $(shell pkg-config --libs openssl 2>/dev/null)
else
  OPENSSL_CFLAGS :=
  OPENSSL_LDFLAGS :=
endif
ifneq ($(OPENSSL_LDFLAGS),)
  OPENSSL_CFLAGS += -DAETHER_HAS_OPENSSL
endif

# zlib auto-detection: same pattern as OpenSSL. zlib is ambient on every
# POSIX box we care about; when absent (bare embedded, etc.) the stdlib
# wrappers report "zlib unavailable" cleanly.
ZLIB ?= auto
ifeq ($(ZLIB),auto)
  ZLIB_CFLAGS := $(shell pkg-config --cflags zlib 2>/dev/null)
  ZLIB_LDFLAGS := $(shell pkg-config --libs zlib 2>/dev/null)
else ifeq ($(ZLIB),1)
  ZLIB_CFLAGS := $(shell pkg-config --cflags zlib 2>/dev/null)
  ZLIB_LDFLAGS := $(shell pkg-config --libs zlib 2>/dev/null)
else
  ZLIB_CFLAGS :=
  ZLIB_LDFLAGS :=
endif
ifneq ($(ZLIB_LDFLAGS),)
  ZLIB_CFLAGS += -DAETHER_HAS_ZLIB
endif

# nghttp2 auto-detection: enables HTTP/2 server-side handling
# (frame parsing, HPACK encode/decode, stream state machine, flow
# control) by wrapping libnghttp2. Same pkg-config probe shape as
# OpenSSL/zlib; when absent, http_server_set_h2 returns
# "HTTP/2 unavailable: built without libnghttp2" and HTTP/1.1
# remains the only protocol the server speaks. Issue #260 Tier 2.
NGHTTP2 ?= auto
ifeq ($(NGHTTP2),auto)
  NGHTTP2_CFLAGS := $(shell pkg-config --cflags libnghttp2 2>/dev/null)
  NGHTTP2_LDFLAGS := $(shell pkg-config --libs libnghttp2 2>/dev/null)
else ifeq ($(NGHTTP2),1)
  NGHTTP2_CFLAGS := $(shell pkg-config --cflags libnghttp2 2>/dev/null)
  NGHTTP2_LDFLAGS := $(shell pkg-config --libs libnghttp2 2>/dev/null)
else
  NGHTTP2_CFLAGS :=
  NGHTTP2_LDFLAGS :=
endif
ifneq ($(NGHTTP2_LDFLAGS),)
  NGHTTP2_CFLAGS += -DAETHER_HAS_NGHTTP2
endif

# PCRE2 detection: enables std.regex (full Perl-compatible regex —
# captures, $-substitutions, Unicode, look-around, etc.). A system
# libpcre2-8 found by pkg-config is preferred (distro security updates
# keep covering it); without one the build compiles the vendored copy in
# std/regex/pcre2/ instead (#1389), so std.regex works everywhere rather
# than silently degrading to its "built without libpcre2-8" stub — the
# failure mode that cost aether-ui 8 rounds of misdiagnosis. PCRE2=0
# opts out entirely (stub mode, for minimal builds); PCRE2=vendored
# forces the vendored engine even when a system library exists.
PCRE2 ?= auto
ifeq ($(PCRE2),auto)
  PCRE2_CFLAGS := $(shell pkg-config --cflags libpcre2-8 2>/dev/null)
  PCRE2_LDFLAGS := $(shell pkg-config --libs libpcre2-8 2>/dev/null)
else ifeq ($(PCRE2),1)
  PCRE2_CFLAGS := $(shell pkg-config --cflags libpcre2-8 2>/dev/null)
  PCRE2_LDFLAGS := $(shell pkg-config --libs libpcre2-8 2>/dev/null)
else
  PCRE2_CFLAGS :=
  PCRE2_LDFLAGS :=
endif
ifneq ($(PCRE2_LDFLAGS),)
  PCRE2_CFLAGS += -DAETHER_HAS_PCRE2
  # On Windows/MinGW we link the STATIC libpcre2-8.a; without PCRE2_STATIC the
  # pcre2.h prototypes default to __declspec(dllimport), so the linker looks for
  # __imp_pcre2_* import stubs that the static lib doesn't provide. Define it so
  # the header declares plain symbols matching the static archive.
  ifneq ($(IS_WINDOWS),)
    PCRE2_CFLAGS += -DPCRE2_STATIC
  endif
else ifneq ($(PCRE2),0)
  # No system libpcre2-8 (or PCRE2=vendored): build the vendored engine.
  # AETHER_VENDOR_PCRE2 turns std/regex/aether_pcre2_vendored.c from an
  # empty TU into the full unity build, and switches aether_regex.c to
  # the in-tree pcre2.h. PCRE2_LDFLAGS stays empty on purpose — the
  # engine is inside libaether.a, so no -l flag may be emitted anywhere
  # (AETHER_PCRE2_LIBS's emptiness is load-bearing, as with every other
  # capability macro).
  PCRE2_CFLAGS := -DAETHER_HAS_PCRE2 -DAETHER_VENDOR_PCRE2
endif

# YAML auto-detection: enables std.yaml via libfyaml.
YAML ?= auto
ifeq ($(YAML),auto)
  YAML_CFLAGS := $(shell pkg-config --cflags libfyaml 2>/dev/null)
  YAML_LDFLAGS := $(shell pkg-config --libs libfyaml 2>/dev/null)
else ifeq ($(YAML),1)
  YAML_CFLAGS := $(shell pkg-config --cflags libfyaml 2>/dev/null)
  YAML_LDFLAGS := $(shell pkg-config --libs libfyaml 2>/dev/null)
else
  YAML_CFLAGS :=
  YAML_LDFLAGS :=
endif
ifneq ($(YAML_LDFLAGS),)
  # libfyaml found (auto/1): compile the real bindings + link it.
  YAML_CFLAGS += -DAETHER_HAS_YAML
else
  # libfyaml absent (or YAML=0): define NOTHING and link NOTHING, so
  # aether_yaml.c compiles its unavailable-stub path and the build works
  # everywhere. (The old `else` forced -lfyaml + -DAETHER_HAS_YAML here,
  # which broke every build without libfyaml — the whole stdlib failed on
  # `#include <libfyaml.h>: No such file`.)
  YAML_CFLAGS :=
  YAML_LDFLAGS :=
endif

# #959: homebrew's pkg-config .pc files emit versioned
# `-L/opt/homebrew/Cellar/<pkg>/<version>/lib` paths. Those `-L` dirs get
# baked into the shipped `ae` binary (via -DAETHER_*_LIBS below) and break
# every user `ae build` with `ld: library 'ssl' not found` the moment the
# formula is upgraded to a new version. Rewrite each Cellar path to the
# version-agnostic `/opt/homebrew/opt/<pkg>` symlink homebrew always keeps
# pointed at the current version. A no-op on layouts without `/Cellar/`
# (Linux, MacPorts, custom prefixes), so it applies unconditionally. The
# CFLAGS keep the as-probed paths — they are only used to build Aether
# itself (where the probed version is present), never baked into user builds.
cellar_to_opt = $(shell printf '%s' '$(1)' | sed 's|/Cellar/\([^/]*\)/[^/]*|/opt/\1|g')
OPENSSL_LDFLAGS := $(call cellar_to_opt,$(OPENSSL_LDFLAGS))
ZLIB_LDFLAGS    := $(call cellar_to_opt,$(ZLIB_LDFLAGS))
NGHTTP2_LDFLAGS := $(call cellar_to_opt,$(NGHTTP2_LDFLAGS))
PCRE2_LDFLAGS   := $(call cellar_to_opt,$(PCRE2_LDFLAGS))
YAML_LDFLAGS    := $(call cellar_to_opt,$(YAML_LDFLAGS))

# -fPIC on every runtime/stdlib object so the precompiled `build/libaether.a`
# can be linked into a shared object by `ae build --emit=lib`. Without it,
# any `--emit=lib` build that pulls in a TLS-using runtime object (e.g.
# std.http's `aether_http.o`, whose initial-exec TLS emits an
# R_X86_64_TPOFF32 relocation illegal in a `.so`) fails to link
# (vcr_embed_abi_wish.md Part A). Negligible codegen cost on x86-64/arm64
# (most distros already default to PIE); one archive serves both the exe
# link and the shared-object link.
# Two kinds of flag, kept apart on purpose.
#
# AETHER_REQUIRED_CFLAGS is structural: the include paths, dependency
# generation, and the defines that select which code is compiled in. The build
# is not merely slower without them, it is a different build.
#
# CFLAGS is the tunable half: optimisation, debug info, warnings, sanitizers.
# Callers replace it wholesale and are right to, e.g.
#
#     make test-build CFLAGS="-O0 -g"                       (valgrind leg)
#     make compiler   CFLAGS="-fsanitize=address ... -O1 -g" (ASan leg)
#
# They were previously one variable, so those commands did far more than they
# said: every -I disappeared, and with them -DAETHER_HAS_SANDBOX, which gates
# real code in runtime/aether_sandbox.c. The sanitizer and valgrind legs were
# therefore testing a configuration that never ships. It stayed invisible
# because the remaining includes all resolved same-directory or relatively,
# until a header outside those trees (include/libaether.h, #1420) finally made
# it fail out loud.
#
# Recipes use `$(AETHER_REQUIRED_CFLAGS) $(CFLAGS)` so tuning one cannot
# silently delete the other.
AETHER_REQUIRED_CFLAGS = -fPIC -Iinclude -Icompiler -Iruntime -Iruntime/actors -Iruntime/scheduler -Iruntime/utils -Iruntime/memory -Iruntime/config -Istd -Istd/string -Istd/io -Istd/math -Istd/net -Istd/collections -Istd/json -Istd/yaml -MMD -MP -DAETHER_VERSION=\"$(VERSION)\" -DAETHER_HAS_SANDBOX $(OPENSSL_CFLAGS) $(ZLIB_CFLAGS) $(NGHTTP2_CFLAGS) $(PCRE2_CFLAGS) $(YAML_CFLAGS)

CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function $(EXTRA_CFLAGS)
# Casper link libraries (FreeBSD only) — std.casper delegates DNS /
# passwd / sysctl past Capsicum capability mode. libcasper + the
# per-service libs ship in the FreeBSD base system. We resolve them by
# globbing the actual shared-object filenames rather than relying on
# `-lcasper` (the `.so` linker symlinks are absent on some installs,
# e.g. GhostBSD) or hardcoding a soname version (libcap_sysctl is at
# .so.2 while its siblings are at .so.1). Empty if the libs aren't
# present — std.casper then links its non-FreeBSD stub path.
CASPER_LDFLAGS :=
ifeq ($(shell uname -s),FreeBSD)
  CASPER_LDFLAGS := $(firstword $(wildcard /lib/libcasper.so.* /usr/lib/libcasper.so.*)) \
                    $(firstword $(wildcard /lib/libcap_net.so.* /usr/lib/libcap_net.so.*)) \
                    $(firstword $(wildcard /lib/libcap_pwd.so.* /usr/lib/libcap_pwd.so.*)) \
                    $(firstword $(wildcard /lib/libcap_sysctl.so.* /usr/lib/libcap_sysctl.so.*))
endif

# std.audio's vendored miniaudio backend (std/audio/miniaudio.h). miniaudio
# dlopens the actual OS audio backends (ALSA/PulseAudio/sndio) at runtime, so
# only the loader/threads/math libs are needed at link time; macOS needs the
# audio frameworks. Windows folds its audio into the existing WIN_LINK_LIBs.
AUDIO_LDFLAGS :=
# Keyed on the HOST uname, so a cross build must opt out explicitly or it
# picks up the host's POSIX libs (zig-lld: "unable to find dynamic system
# library 'pthread'"). Windows folds audio into WIN_LINK_LIBS either way.
ifeq ($(WINDOWS),1)
  AUDIO_LDFLAGS :=
else ifeq ($(shell uname -s),Linux)
  AUDIO_LDFLAGS := -lpthread -ldl -lm
else ifeq ($(shell uname -s),FreeBSD)
  AUDIO_LDFLAGS := -lpthread -lm
else ifeq ($(shell uname -s),Darwin)
  AUDIO_LDFLAGS := -framework CoreFoundation -framework CoreAudio -framework AudioToolbox
endif

# The link-side half of the AETHER_REQUIRED_CFLAGS split above, and it has to
# stay in lockstep with it: feature detection decides BOTH the -I/-D for a
# dependency and the -l that resolves the symbols that then get emitted. Split
# only the compile side and an `LDFLAGS="..."` override compiles OpenSSL in and
# links it out, so the build fails on undefined SSL symbols. Neither half is
# tunable on its own.
ifeq ($(FREEBSD),1)
# Cross build: the explicit base-libc link tail replaces the native assembly
# (which would pull the host's -lm / -pthread / globbed host casper libs).
AETHER_REQUIRED_LDFLAGS = $(FREEBSD_LDFLAGS)
else
AETHER_REQUIRED_LDFLAGS = -lm $(OPENSSL_LDFLAGS) $(ZLIB_LDFLAGS) $(NGHTTP2_LDFLAGS) $(PCRE2_LDFLAGS) $(YAML_LDFLAGS) $(CASPER_LDFLAGS) $(AUDIO_LDFLAGS)
endif

# Tunable link flags: sanitizers and anything else a caller passes. Empty by
# default, so `make ... LDFLAGS="-fsanitize=address"` adds to the required set
# rather than replacing it.
LDFLAGS =

# Hardening flags (issue #396). Opt-in via `HARDEN=1`. The CI matrix
# pins a Linux/gcc + HARDEN=1 entry so a hardened-build regression
# trips a red check before merge; the default release CFLAGS stay
# unchanged because (a) some hardening flags add measurable runtime
# overhead and (b) macOS Clang has a documented gap with
# `_FORTIFY_SOURCE` on a few historical setups. Each flag below is
# justified inline:
#
#   -fstack-protector-all  : stack canaries on every function — not
#                             just those gcc heuristics flag (-strong)
#                             — catches the smashing class of bugs
#                             that escape the default heuristic.
#   -D_FORTIFY_SOURCE=2    : runtime checks on read/write/memcpy/
#                             strncpy/printf-family wrappers. Requires
#                             at least -O1; we already use -O2, so
#                             nothing extra needed. Linux/gcc will
#                             warn at compile time when it can prove
#                             a buffer overflow; turn those into
#                             real bug fixes, not blanket suppressions.
#   -Wformat -Wformat-security :
#                             flag printf-family format strings that
#                             come from non-literal sources (the
#                             classic %s-format-injection pattern).
#                             Default in modern Linux distros; we
#                             standardise on it explicitly.
ifeq ($(HARDEN),1)
CFLAGS += -fstack-protector-all -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security
endif
ifneq ($(FREEBSD),1)
ifneq ($(WINDOWS),1)
ifneq ($(PLATFORM),wasm)
ifneq ($(PLATFORM),embedded)
ifeq ($(findstring AETHER_NO_THREADING,$(EXTRA_CFLAGS)),)
AETHER_REQUIRED_LDFLAGS += -pthread
endif
endif
endif
endif
endif

# Zero warnings achieved - ready for -Werror
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj

# Windows-specific: -static avoids libwinpthread/libgcc DLL dependencies.
# MinGW OpenSSL 3 static libs pull in Windows Crypto API + GDI/Advapi
# symbols (CertFreeCertificateContext, CertOpenSystemStoreW, etc.) that
# aren't auto-linked — we add the import libs explicitly so std.net TLS
# and std.cryptography work in Windows release binaries.
WIN_LINK_LIBS = -static -lws2_32 -lcrypt32 -lgdi32 -luser32 -ladvapi32 -lbcrypt -ldbghelp
ifdef WINDOWS_NATIVE
    AETHER_REQUIRED_LDFLAGS += $(WIN_LINK_LIBS)
else ifneq ($(findstring MINGW,$(DETECTED_OS)),)
    AETHER_REQUIRED_LDFLAGS += $(WIN_LINK_LIBS)
else ifneq ($(findstring MSYS,$(DETECTED_OS)),)
    AETHER_REQUIRED_LDFLAGS += $(WIN_LINK_LIBS)
else ifneq ($(findstring CYGWIN,$(DETECTED_OS)),)
    AETHER_REQUIRED_LDFLAGS += $(WIN_LINK_LIBS)
endif

COMPILER_SRC = compiler/aetherc.c compiler/parser/lexer.c compiler/parser/parser.c compiler/ast.c compiler/analysis/typechecker.c compiler/analysis/contract_eval.c compiler/analysis/derive.c compiler/analysis/actor_reply.c compiler/aether_defines.c compiler/codegen/codegen.c compiler/codegen/codegen_expr.c compiler/codegen/codegen_stmt.c compiler/codegen/codegen_actor.c compiler/codegen/codegen_func.c compiler/aether_error.c compiler/aether_module.c compiler/analysis/type_inference.c compiler/codegen/optimizer.c compiler/aether_diagnostics.c runtime/actors/aether_message_registry.c lsp/aether_lsp.c
COMPILER_LIB_SRC = compiler/parser/lexer.c compiler/parser/parser.c compiler/ast.c compiler/analysis/typechecker.c compiler/analysis/contract_eval.c compiler/analysis/derive.c compiler/analysis/actor_reply.c compiler/aether_defines.c compiler/codegen/codegen.c compiler/codegen/codegen_expr.c compiler/codegen/codegen_stmt.c compiler/codegen/codegen_actor.c compiler/codegen/codegen_func.c compiler/aether_error.c compiler/aether_module.c compiler/analysis/type_inference.c compiler/codegen/optimizer.c compiler/aether_diagnostics.c runtime/actors/aether_message_registry.c lsp/aether_lsp.c
RUNTIME_SRC = $(SCHEDULER_SRC) runtime/scheduler/scheduler_optimizations.c runtime/scheduler/aether_io_poller_epoll.c runtime/scheduler/aether_io_poller_kqueue.c runtime/scheduler/aether_io_poller_poll.c runtime/config/aether_optimization_config.c runtime/memory/aether_arena.c runtime/memory/aether_pool.c runtime/memory/aether_memory_stats.c runtime/utils/aether_trace.c runtime/utils/aether_bounds_check.c runtime/utils/aether_test.c runtime/memory/aether_arena_optimized.c runtime/aether_runtime_types.c runtime/aether_locale_num.c runtime/utils/aether_cpu_detect.c runtime/utils/aether_simd_vectorized.c runtime/aether_runtime.c runtime/aether_numa.c runtime/aether_sandbox.c runtime/sandbox/spawn_sandboxed_linux.c runtime/sandbox/spawn_sandboxed_bsd.c runtime/sandbox/spawn_sandboxed_stub.c runtime/sandbox/capsicum_autosandbox.c runtime/sandbox/aether_audit.c runtime/aether_shared_map.c runtime/aether_host.c runtime/aether_resource_caps.c runtime/libaether_caps.c runtime/actors/aether_send_buffer.c runtime/actors/aether_send_message.c runtime/actors/aether_actor_thread.c runtime/actors/aether_panic.c runtime/actors/aether_unwind.c
STD_SRC = std/string/aether_string.c std/math/aether_math.c std/net/aether_http.c std/net/aether_http_server.c std/net/aether_http_pool.c std/net/aether_net.c std/collections/aether_collections.c std/json/aether_json.c std/yaml/aether_yaml.c std/xml/aether_xml.c std/fs/aether_fs.c std/log/aether_log.c std/io/aether_io.c std/os/aether_os.c std/ipc/aether_ipc.c std/mem/aether_mem.c std/cryptography/aether_cryptography.c std/cryptography/aes/aether_aes.c std/zlib/aether_zlib.c std/lzf/lzf_c.c std/lzf/lzf_d.c std/lzf/aether_lzf.c std/dl/aether_dl.c std/http/middleware/aether_middleware.c std/http/server/h2/aether_h2.c std/http/proxy/aether_proxy_pool.c std/http/proxy/aether_proxy_lb.c std/http/proxy/aether_proxy_breaker.c std/http/proxy/aether_proxy_health.c std/http/proxy/aether_proxy_cache.c std/http/proxy/aether_proxy_opts.c std/http/proxy/aether_proxy_metrics.c std/http/proxy/aether_proxy_middleware.c std/http/script_gateway/aether_script_gateway.c std/bytes/aether_bytes.c std/bytes/cursor/aether_bytes_cursor.c std/strbuilder/aether_strbuilder.c std/config/aether_config.c std/actors/aether_actor_registry.c std/regex/aether_regex.c std/regex/aether_pcre2_vendored.c std/capsicum/aether_capsicum.c std/casper/aether_casper.c std/snapshot/aether_snapshot.c std/audio/aether_audio.c std/worker/aether_worker.c std/alloc/aether_alloc.c std/tracking/aether_tracking.c std/tar/aether_tar.c
# Stdlib sources that reference scheduler internals (scheduler_io_register,
# g_sync_step_actor, current_core_id). Excluded from the compiler binary
# because aetherc does not link the runtime scheduler, but included in
# libaether.a and user programs where the runtime is present.
STD_REACTOR_SRC = std/net/aether_actor_bridge.c
COLLECTIONS_SRC = std/collections/aether_set.c std/collections/aether_pqueue.c std/collections/aether_intarr.c std/collections/aether_floatarr.c std/collections/aether_longarr.c std/collections/aether_bits.c std/collections/aether_stringlist.c std/collections/aether_stringseq.c

# I/O poller backends (needed by both compiler and runtime targets)
IO_POLLER_SRC = runtime/scheduler/aether_io_poller_epoll.c runtime/scheduler/aether_io_poller_kqueue.c runtime/scheduler/aether_io_poller_poll.c

# Object files
COMPILER_OBJS = $(COMPILER_SRC:%.c=$(OBJ_DIR)/%.o)

# Tools driver, per-object build (#1221). ae.c was one 8.5k-line TU compiled
# in a single gcc invocation, so any edit recompiled the whole driver. Each
# ae_*.c now compiles to its own object with -MMD -MP dependency tracking, so
# an edit recompiles only the touched object (and, via the .d files, whatever
# includes a changed ae_internal.h) and relinks. The _LIBS macros are baked in
# so build_gcc_cmd can construct user-program link lines.
TOOLS_CFLAGS = -O2 -Itools -MMD -MP \
    -DAETHER_VERSION=\"$(VERSION)\" \
    -DAETHER_OPENSSL_LIBS='"$(OPENSSL_LDFLAGS)"' \
    -DAETHER_ZLIB_LIBS='"$(ZLIB_LDFLAGS)"' \
    -DAETHER_NGHTTP2_LIBS='"$(NGHTTP2_LDFLAGS)"' \
    -DAETHER_PCRE2_LIBS='"$(PCRE2_LDFLAGS)"' \
    -DAETHER_YAML_LIBS='"$(YAML_LDFLAGS)"' \
    -DAETHER_CASPER_LIBS='"$(CASPER_LDFLAGS)"' \
    -DAETHER_AUDIO_LIBS='"$(AUDIO_LDFLAGS)"' \
    $(if $(AETHER_ENABLE_LLM),-DAETHER_ENABLE_LLM=1)
TOOLS_SRC = tools/ae.c tools/ae_help.c tools/ae_fmt.c tools/ae_bindgen.c \
            tools/ae_cross.c tools/ae_repl.c tools/ae_version.c \
            tools/ae_cache.c tools/apkg/toml_parser.c $(if $(AETHER_ENABLE_LLM),tools/llm_shim.c)
TOOLS_OBJS = $(TOOLS_SRC:%.c=$(OBJ_DIR)/%.o)
COMPILER_LIB_OBJS = $(COMPILER_LIB_SRC:%.c=$(OBJ_DIR)/%.o)
RUNTIME_OBJS = $(RUNTIME_SRC:%.c=$(OBJ_DIR)/%.o)
IO_POLLER_OBJS = $(IO_POLLER_SRC:%.c=$(OBJ_DIR)/%.o)
STD_OBJS = $(STD_SRC:%.c=$(OBJ_DIR)/%.o)
STD_REACTOR_OBJS = $(STD_REACTOR_SRC:%.c=$(OBJ_DIR)/%.o)
COLLECTIONS_OBJS = $(COLLECTIONS_SRC:%.c=$(OBJ_DIR)/%.o)
TEST_OBJS = $(TEST_SRC:%.c=$(OBJ_DIR)/%.o)

# The version is injected as -DAETHER_VERSION on the command line, NOT via an
# #include, so -MMD cannot see it: bump VERSION and every object still looks up
# to date, leaving a binary that reports the PREVIOUS version. Measured: a tree
# whose generated header said 0.516.0 produced an `ae --version` of 0.515.0,
# because build/obj/tools/ae_version.o was hours older than the header and
# nothing told make to rebuild it.
#
# $(VERSION_HEADER) is regenerated whenever VERSION changes (rule above), so
# making the version-bearing objects depend on it is the cheap, honest link
# between "the version changed" and "recompile the things that bake it in".
$(OBJ_DIR)/tools/ae_version.o: $(VERSION_HEADER)
$(OBJ_DIR)/tools/ae.o: $(VERSION_HEADER)
$(OBJ_DIR)/compiler/aetherc.o: $(VERSION_HEADER)

# Dependency files (include test objects so header changes trigger test recompilation)
DEPS = $(COMPILER_OBJS:.o=.d) $(RUNTIME_OBJS:.o=.d) $(STD_OBJS:.o=.d) $(COLLECTIONS_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(TOOLS_OBJS:.o=.d)

# Include dependency files
-include $(DEPS)

# Test files using TEST() macro system (exclude standalone tests)
TEST_SRC = tests/runtime/test_harness.c \
           tests/runtime/test_main.c \
           tests/runtime/test_64bit.c \
           tests/runtime/test_runtime_collections.c \
           tests/runtime/test_runtime_strings.c \
           tests/runtime/test_runtime_strbuilder.c \
           tests/runtime/test_runtime_math.c \
           tests/runtime/test_runtime_json.c \
           tests/runtime/test_runtime_http.c \
           tests/runtime/test_runtime_net.c \
           tests/runtime/test_scheduler.c \
           tests/runtime/test_scheduler_stress.c \
           tests/runtime/test_zerocopy.c \
           tests/runtime/test_lockfree_mailbox.c \
           tests/runtime/test_send_buffer.c \
           tests/runtime/test_scheduler_optimizations.c \
           tests/runtime/test_spsc_queue.c \
           tests/runtime/test_worksteal_race.c \
           tests/runtime/test_http_server.c \
           tests/runtime/test_resource_caps.c \
           tests/runtime/test_libaether_caps.c \
           tests/memory/test_memory_arena.c \
           tests/memory/test_memory_pool.c \
           tests/memory/test_memory_leaks.c \
           tests/memory/test_memory_stress.c \
           tests/compiler/test_lexer.c \
           tests/compiler/test_typechecker.c \
           tests/compiler/test_lexer_comprehensive.c \
           tests/compiler/test_parser.c \
           tests/compiler/test_parser_comprehensive.c \
           tests/compiler/test_type_inference_comprehensive.c \
           tests/compiler/test_pattern_matching_comprehensive.c \
           tests/compiler/test_structs.c \
           tests/compiler/test_switch_statements.c \
           tests/compiler/test_codegen.c \
           tests/compiler/test_module_orchestrator.c \
           tests/compiler/test_security.c

# Standalone test programs with their own main() - build separately
# These are not part of the main test suite but can be built manually
STANDALONE_TESTS = tests/runtime/test_runtime_manual.c \
                   tests/compiler/test_arrays.c

# Every C source with its own main() that no other target compiles: the
# runtime examples, the micro-benchmarks, the demos. Nothing links them, so
# they rot invisibly — three of them had stale include paths and one was
# written against a renamed logging API. `check-standalone` compiles each so
# that cannot happen again.
STANDALONE_SRC = $(wildcard runtime/examples/*.c) \
                 $(wildcard tests/runtime/bench_*.c) \
                 $(STANDALONE_TESTS) \
                 tools/profiler/profiler_demo.c \
                 std/log/demo_log.c \
                 tests/differential/driver.c
ifneq ($(IS_WINDOWS),)
  # POSIX-only standalone sources: the differential driver dlopen()s
  # candidate compilers (<dlfcn.h>), and the work-stealing bench uses
  # raw pthreads that clash with aether_thread.h's pthread_t -> HANDLE
  # mapping on MinGW. Everything else in the gate compiles on Windows.
  STANDALONE_SRC := $(filter-out tests/differential/driver.c runtime/examples/workstealing_bench.c,$(STANDALONE_SRC))
endif

all: compiler ae stdlib

# Create object directories
$(OBJ_DIR)/compiler $(OBJ_DIR)/compiler/parser $(OBJ_DIR)/compiler/codegen $(OBJ_DIR)/compiler/analysis $(OBJ_DIR)/runtime $(OBJ_DIR)/runtime/actors $(OBJ_DIR)/runtime/sandbox $(OBJ_DIR)/runtime/scheduler $(OBJ_DIR)/runtime/memory $(OBJ_DIR)/runtime/config $(OBJ_DIR)/runtime/simd $(OBJ_DIR)/runtime/utils $(OBJ_DIR)/std $(OBJ_DIR)/std/string $(OBJ_DIR)/std/io $(OBJ_DIR)/std/math $(OBJ_DIR)/std/net $(OBJ_DIR)/std/fs $(OBJ_DIR)/std/log $(OBJ_DIR)/std/collections $(OBJ_DIR)/std/json $(OBJ_DIR)/std/yaml $(OBJ_DIR)/std/xml $(OBJ_DIR)/std/os $(OBJ_DIR)/std/ipc $(OBJ_DIR)/std/mem $(OBJ_DIR)/std/cryptography $(OBJ_DIR)/std/cryptography/aes $(OBJ_DIR)/std/zlib $(OBJ_DIR)/std/lzf $(OBJ_DIR)/std/dl $(OBJ_DIR)/std/bytes $(OBJ_DIR)/std/bytes/cursor $(OBJ_DIR)/std/strbuilder $(OBJ_DIR)/std/config $(OBJ_DIR)/std/actors $(OBJ_DIR)/std/capsicum $(OBJ_DIR)/std/casper $(OBJ_DIR)/std/snapshot $(OBJ_DIR)/std/audio $(OBJ_DIR)/std/worker $(OBJ_DIR)/std/alloc $(OBJ_DIR)/std/tracking $(OBJ_DIR)/std/tar $(OBJ_DIR)/std/http $(OBJ_DIR)/std/http/middleware $(OBJ_DIR)/std/http/proxy $(OBJ_DIR)/std/http/script_gateway $(OBJ_DIR)/std/http/server $(OBJ_DIR)/std/http/server/h2 $(OBJ_DIR)/std/regex $(OBJ_DIR)/lsp $(OBJ_DIR)/tests $(OBJ_DIR)/tests/compiler $(OBJ_DIR)/tests/memory $(OBJ_DIR)/tests/runtime $(OBJ_DIR)/tools $(OBJ_DIR)/tools/apkg:
ifdef WINDOWS_NATIVE
	@if not exist "$(subst /,\,$@)" mkdir "$(subst /,\,$@)"
else
	@mkdir -p $@
endif

# Pattern rule for object files
$(OBJ_DIR)/%.o: %.c | $(STDLIB_SYMS_HEADER) $(OBJ_DIR)/compiler $(OBJ_DIR)/compiler/parser $(OBJ_DIR)/compiler/codegen $(OBJ_DIR)/compiler/analysis $(OBJ_DIR)/runtime $(OBJ_DIR)/runtime/actors $(OBJ_DIR)/runtime/sandbox $(OBJ_DIR)/runtime/scheduler $(OBJ_DIR)/runtime/memory $(OBJ_DIR)/runtime/config $(OBJ_DIR)/runtime/simd $(OBJ_DIR)/runtime/utils $(OBJ_DIR)/std $(OBJ_DIR)/std/string $(OBJ_DIR)/std/io $(OBJ_DIR)/std/math $(OBJ_DIR)/std/net $(OBJ_DIR)/std/fs $(OBJ_DIR)/std/log $(OBJ_DIR)/std/collections $(OBJ_DIR)/std/json $(OBJ_DIR)/std/yaml $(OBJ_DIR)/std/xml $(OBJ_DIR)/std/os $(OBJ_DIR)/std/ipc $(OBJ_DIR)/std/mem $(OBJ_DIR)/std/cryptography $(OBJ_DIR)/std/cryptography/aes $(OBJ_DIR)/std/zlib $(OBJ_DIR)/std/lzf $(OBJ_DIR)/std/dl $(OBJ_DIR)/std/bytes $(OBJ_DIR)/std/bytes/cursor $(OBJ_DIR)/std/strbuilder $(OBJ_DIR)/std/config $(OBJ_DIR)/std/actors $(OBJ_DIR)/std/capsicum $(OBJ_DIR)/std/casper $(OBJ_DIR)/std/snapshot $(OBJ_DIR)/std/audio $(OBJ_DIR)/std/worker $(OBJ_DIR)/std/alloc $(OBJ_DIR)/std/tracking $(OBJ_DIR)/std/tar $(OBJ_DIR)/std/http $(OBJ_DIR)/std/http/middleware $(OBJ_DIR)/std/http/proxy $(OBJ_DIR)/std/http/script_gateway $(OBJ_DIR)/std/http/server $(OBJ_DIR)/std/http/server/h2 $(OBJ_DIR)/std/regex $(OBJ_DIR)/lsp $(OBJ_DIR)/tests $(OBJ_DIR)/tests/compiler $(OBJ_DIR)/tests/memory $(OBJ_DIR)/tests/runtime
	@echo "Compiling $<..."
	@$(CC) $(AETHER_REQUIRED_CFLAGS) $(CFLAGS) -c $< -o $@

# Upstream liblzf (vendored verbatim under std/lzf/) has a few style
# choices the modern -Werror set flags:
#   - lzf_d.c's decoder uses C-style fallthrough in its dispatch loop
#     (gcc -Wimplicit-fallthrough).
#   - lzfP.h defines LZF_USE_OFFSETS via `#define X defined(_M_X64)` and
#     then `#if X`, an expansion-to-defined that's non-portable under
#     C99 but works on every compiler the upstream targets
#     (gcc/clang -Wexpansion-to-defined).
# We silence both on the lzf objects rather than patching the vendored
# source, so future re-vendoring stays a copy-paste.
LZF_CFLAGS_RELAX = -Wno-implicit-fallthrough -Wno-expansion-to-defined
$(OBJ_DIR)/std/lzf/lzf_c.o: CFLAGS += $(LZF_CFLAGS_RELAX)
$(OBJ_DIR)/std/lzf/lzf_d.o: CFLAGS += $(LZF_CFLAGS_RELAX)
$(OBJ_DIR)/std/lzf/aether_lzf.o: CFLAGS += $(LZF_CFLAGS_RELAX)

# Vendored miniaudio (single-header, ~96k lines) is compiled inside
# aether_audio.c via MINIAUDIO_IMPLEMENTATION. Relax the warnings we don't
# own — same treatment as vendored liblzf above.
# GCC-only: MinGW-w64 GCC mis-analyses miniaudio's atomic intrinsics
# (ma_atomic_*_32 on opaque node-graph/resource-manager structs) as "writing
# into a region of size 0" — a known GCC false positive on miniaudio's own
# realtime code, not our storage. Guarded on the compiler being GCC because
# clang has no -Wstringop-overflow group and, under -Werror, promotes the
# unknown-warning-option to a hard error (macOS clang build).
AUDIO_GCC_ONLY_RELAX := $(shell $(CC) --version 2>/dev/null | grep -qi clang || echo -Wno-stringop-overflow)
AUDIO_CFLAGS_RELAX = -Wno-unused-function -Wno-unused-variable \
                     -Wno-implicit-fallthrough -Wno-sign-compare \
                     -Wno-unused-parameter -Wno-type-limits \
                     $(AUDIO_GCC_ONLY_RELAX)
$(OBJ_DIR)/std/audio/aether_audio.o: CFLAGS += $(AUDIO_CFLAGS_RELAX)

# ---- miniaudio object cache (CI) ---------------------------------------
# Compiling MINIAUDIO_IMPLEMENTATION (the ~96k-line vendored header) costs ~1
# minute per build, paid on every matrix leg because `make ci` starts with
# `clean` (rm -rf build). miniaudio.h changes ~never, so we cache the compiled
# object OUTSIDE build/ (so clean can't wipe it) and restore it after clean.
#
# Safety: a cached object is used ONLY when it would be byte-identical to a
# fresh compile. AUDIO_CACHE_STAMP is a content hash of the source, the header,
# and the compiler identity + audio flags; the restore refuses a stamp mismatch
# and falls back to a normal compile. In CI the actions/cache KEY already pins
# hash(miniaudio.h)+OS+compiler, so a restored object is guaranteed to match;
# the stamp guards local/edge use. A miss always rebuilds — never stale.
AUDIO_CACHE_DIR    := .ci-cache
AUDIO_CACHE_OBJ    := $(AUDIO_CACHE_DIR)/aether_audio.o
AUDIO_CACHE_STAMP  := $(AUDIO_CACHE_DIR)/aether_audio.stamp
AUDIO_OBJ          := $(OBJ_DIR)/std/audio/aether_audio.o
# Portable content hasher (sha256sum on Linux, shasum on macOS, cksum fallback).
CKSUM_CMD := $(shell command -v sha256sum >/dev/null 2>&1 && echo sha256sum || (command -v shasum >/dev/null 2>&1 && echo 'shasum -a 256' || echo cksum))
# Content key: source + vendored header + compiler identity + every build knob
# that can change the emitted object — compiler version, the audio relax flags,
# and the build-variant flags (HARDEN, EXTRA_CFLAGS: coop/wasm/embedded set
# -DAETHER_NO_THREADING etc). If any of these differ the stamp differs, so a
# hardened / coop / cross object never gets restored over a plain one.
AUDIO_CACHE_KEY := $(shell cat std/audio/aether_audio.c std/audio/miniaudio.h std/audio/aether_audio.h 2>/dev/null | $(CKSUM_CMD) 2>/dev/null | cut -d' ' -f1)-$(shell $(CC) --version 2>/dev/null | head -1 | tr -dc 'a-zA-Z0-9.')-h$(HARDEN)-$(shell printf '%s %s' "$(AUDIO_GCC_ONLY_RELAX)" "$(EXTRA_CFLAGS)" | $(CKSUM_CMD) 2>/dev/null | cut -d' ' -f1)

# Restore the cached object into build/ if the stamp matches, then touch it
# newer than its source so make treats it as up-to-date and skips the compile.
# No-op (silent) when there is no valid cache — the normal rule then builds it.
.PHONY: audio-cache-restore
audio-cache-restore:
	@if [ -f "$(AUDIO_CACHE_OBJ)" ] && [ -f "$(AUDIO_CACHE_STAMP)" ] && \
	    [ "$$(cat $(AUDIO_CACHE_STAMP))" = "$(AUDIO_CACHE_KEY)" ]; then \
	  mkdir -p "$(dir $(AUDIO_OBJ))"; \
	  cp "$(AUDIO_CACHE_OBJ)" "$(AUDIO_OBJ)"; \
	  touch "$(AUDIO_OBJ)"; \
	  echo "audio-cache: restored miniaudio object (stamp hit) — skipping ~96k-line compile"; \
	else \
	  echo "audio-cache: no valid cached object — will compile miniaudio from source"; \
	fi

# Save the freshly-built object + its stamp to the cache dir (idempotent).
# actions/cache then persists $(AUDIO_CACHE_DIR) between runs.
.PHONY: audio-cache-save
audio-cache-save: $(AUDIO_OBJ)
	@mkdir -p "$(AUDIO_CACHE_DIR)"
	@cp "$(AUDIO_OBJ)" "$(AUDIO_CACHE_OBJ)"
	@printf '%s' "$(AUDIO_CACHE_KEY)" > "$(AUDIO_CACHE_STAMP)"
	@echo "audio-cache: saved miniaudio object to $(AUDIO_CACHE_OBJ)"

# ---- contrib/i18n collation check (CI) ---------------------------------
# contrib/i18n (Phase 5 of #863) compiles the vendored utf8proc.c and the
# ~74k-line GENERATED DUCET table (ducet/ducet_data.c). Both are committed
# (vendored), so CI never fetches at build time — no supply-chain surface
# beyond the pinned, reviewed files. We considered mirroring the miniaudio
# object cache here, but measured the combined compile at <1s (utf8proc ~0.4s,
# ducet_data ~0.25s) — nothing like miniaudio's ~1min — so an actions/cache
# round-trip would cost more than it saves. Just compile it each run.
#
# Standalone from `make ci` (contrib is not in the test-ae glob); invoked by
# its own CI job (see .github/workflows/ci.yml: ci-contrib-i18n).
I18N_DIR := contrib/i18n

.PHONY: contrib-i18n-check
contrib-i18n-check: compiler ae stdlib
	@echo "contrib-i18n: building collate test"
	@./build/ae$(EXE_EXT) build $(I18N_DIR)/collate/test_collate.ae \
	  --extra $(I18N_DIR)/aether_i18n.c \
	  --extra $(I18N_DIR)/utf8proc/utf8proc.c \
	  --extra $(I18N_DIR)/ducet/ducet_data.c \
	  -o build/test_collate$(EXE_EXT)
	@echo "contrib-i18n: running collate test"
	@./build/test_collate$(EXE_EXT)

# contrib-check: build + RUN every contrib test_*.ae (not just type-check).
# This is the runtime-coverage gate the nightly was missing — type-checking
# alone let the tinyweb WebSocket server path rot (compiled fine, runtime-
# broken). The table of tests + their --extra C files lives in
# .github/scripts/contrib_check.sh. contrib-check-valgrind runs each under
# valgrind as a leak gate (for the dedicated nightly build box).
.PHONY: contrib-check contrib-check-valgrind
contrib-check: compiler ae stdlib
	@EXE_EXT='$(EXE_EXT)' bash .github/scripts/contrib_check.sh

contrib-check-valgrind: compiler ae stdlib
	@EXE_EXT='$(EXE_EXT)' VALGRIND=1 bash .github/scripts/contrib_check.sh

# Compiler target (incremental build with object files)
compiler: $(COMPILER_OBJS) $(STD_OBJS) $(COLLECTIONS_OBJS) $(OBJ_DIR)/runtime/aether_sandbox.o $(OBJ_DIR)/runtime/aether_resource_caps.o $(OBJ_DIR)/runtime/aether_locale_num.o $(IO_POLLER_OBJS) | $(VERSION_HEADER) $(STDLIB_SYMS_HEADER)
	@echo "Linking compiler..."
	@$(CC) $(COMPILER_OBJS) $(STD_OBJS) $(COLLECTIONS_OBJS) $(OBJ_DIR)/runtime/aether_sandbox.o $(OBJ_DIR)/runtime/aether_resource_caps.o $(OBJ_DIR)/runtime/aether_locale_num.o $(IO_POLLER_OBJS) -o build/aetherc$(EXE_EXT) $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS)
	@echo "Compiler built successfully"

# Fast compiler target (monolithic, for clean builds)
compiler-fast:
ifdef WINDOWS_NATIVE
	@if not exist "build" mkdir "build"
else
	@$(MKDIR) build
endif
	$(CC) $(AETHER_REQUIRED_CFLAGS) $(CFLAGS) $(COMPILER_SRC) $(STD_SRC) $(COLLECTIONS_SRC) $(IO_POLLER_SRC) runtime/aether_resource_caps.c runtime/aether_locale_num.c -o build/aetherc$(EXE_EXT) $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS)

test: $(TEST_OBJS) $(COMPILER_LIB_OBJS) $(RUNTIME_OBJS) $(STD_OBJS) $(STD_REACTOR_OBJS) $(COLLECTIONS_OBJS)
	@echo "==================================="
	@echo "Building Test Suite ($(DETECTED_OS))"
	@echo "==================================="
	@echo "Linking test runner..."
	@$(CC) $(TEST_OBJS) $(COMPILER_LIB_OBJS) $(RUNTIME_OBJS) $(STD_OBJS) $(STD_REACTOR_OBJS) $(COLLECTIONS_OBJS) -o build/test_runner$(EXE_EXT) $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS)
	@echo ""
	@echo "==================================="
	@echo "Running Tests"
	@echo "==================================="
ifneq ($(findstring MINGW,$(DETECTED_OS)),)
	@bash -c './build/test_runner$(EXE_EXT); exit $$?'
else ifneq ($(findstring MSYS,$(DETECTED_OS)),)
	@bash -c './build/test_runner$(EXE_EXT); exit $$?'
else
	./build/test_runner$(EXE_EXT)
endif

# Fast test target (monolithic)
test-fast: compiler-fast
	@echo "==================================="
	@echo "Building Test Suite ($(DETECTED_OS))"
	@echo "==================================="
	$(CC) $(AETHER_REQUIRED_CFLAGS) $(CFLAGS) $(TEST_SRC) $(COMPILER_LIB_SRC) $(RUNTIME_SRC) $(STD_SRC) $(STD_REACTOR_SRC) $(COLLECTIONS_SRC) -Icompiler -Istd -Istd/collections -o build/test_runner$(EXE_EXT) $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS)
	@echo ""
	@echo "==================================="
	@echo "Running Tests"
	@echo "==================================="
	./build/test_runner$(EXE_EXT)

# test-valgrind / test-asan / test-memory: link the test runner's own
# main() from TEST_SRC against pre-built flavoured archives instead of
# recompiling the full stdlib + compiler-lib sources every invocation.
# Each archive bakes the right instrumentation in once (asan, memory
# tracking, or -O0 -g for valgrind). See `stdlib-{asan,memory,dbg}`
# rules above. The archives must use COMPILER_LIB_SRC (no aetherc.c)
# so test_main.c's main() doesn't collide with aetherc's.
test-valgrind: compiler stdlib-dbg
	@echo "==================================="
	@echo "Running Tests with Valgrind"
	@echo "==================================="
	$(CC) $(AETHER_REQUIRED_CFLAGS) $(CFLAGS_NO_OPT) $(DBG_OPT) $(TEST_SRC) build/dbg/libaether_compiler.a build/dbg/libaether.a -Icompiler -Istd -Istd/collections -o build/test_runner$(EXE_EXT) $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS)
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --error-exitcode=1 ./build/test_runner$(EXE_EXT)

# -----------------------------------------------------------------
# ci-coverage — build with `gcc --coverage`, run the C-level test
# suite, then walk the .gcda files with gcov to produce per-source
# `.c.gcov` and (thanks to PR #352's #line directives) `.ae.gcov`
# reports under build/coverage/. A summary is printed to stdout and
# written to build/coverage/SUMMARY for downstream tooling.
#
# Scope: the C-level test_runner only. The 397 `.ae` regression tests
# under `make test-ae` use the user-facing `ae build` pipeline which
# doesn't currently inject --coverage into gcc; extending coverage to
# that path is a follow-up (`ae build --coverage`).
#
# Optional richer reports: gcovr (HTML) and lcov+genhtml (browsable
# HTML) are picked up if installed, skipped with a notice if not.
# Probe-and-degrade matches the contrib_build.sh philosophy.
#
# Not part of `make ci` — coverage rebuild is slow and the test data
# only becomes useful when read by a human. Run on demand.
# -----------------------------------------------------------------
ci-coverage: compiler ae stdlib-cov
	@echo "==================================="
	@echo "  Building coverage-instrumented test runner"
	@echo "==================================="
	@$(CC) $(AETHER_REQUIRED_CFLAGS) $(CFLAGS_NO_OPT) $(COV_OPT) $(COV_FLAGS) $(TEST_SRC) \
		build/cov/libaether_compiler.a build/cov/libaether.a \
		-Icompiler -Istd -Istd/collections \
		-o build/test_runner_cov$(EXE_EXT) $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS)
	@echo ""
	@echo "==================================="
	@echo "  [1/2] Running C-level tests with coverage counters"
	@echo "==================================="
	@./build/test_runner_cov$(EXE_EXT) || true
	@echo ""
	@echo "==================================="
	@echo "  [2/2] Running .ae regression tests with coverage counters"
	@echo "==================================="
	@# Forward --coverage to every per-test `ae build` invocation
	@# via the AE_BUILD_FLAGS env var. The test runner picks it up
	@# in the recipe-generated run_test.sh script.
	@AE_BUILD_FLAGS=--coverage $(MAKE) --no-print-directory test-ae || true
	@echo ""
	@bash tests/scripts/coverage_report.sh

ci-coverage-clean:
	@echo "Cleaning coverage data..."
	@find build/cov-obj -name '*.gcda' -delete 2>/dev/null || true
	@find build -maxdepth 2 -name '*.gcda' -delete 2>/dev/null || true
	@find build -maxdepth 2 -name '*.gcno' -delete 2>/dev/null || true
	@$(RM) -r build/coverage 2>/dev/null || true
	@echo "✓ Coverage data cleaned (cov-obj/.gcno files retained for reuse)"

# ci-coverage-html — wraps ci-coverage with an HTML/JSON report via
# gcovr. Auto-bootstraps gcovr in a project-local venv at
# build/cov-venv/ if it's not already on PATH (modern Debian/Ubuntu
# refuse `pip install` system-wide without --break-system-packages,
# so a self-contained venv is the friendliest path).
#
# After running, open build/coverage/index.html in a browser. JSON
# is also written to coverage.json for CI / dashboard pickup.
ci-coverage-html: ci-coverage
	@bash tests/scripts/coverage_html.sh

test-asan: compiler stdlib-asan
	@echo "==================================="
	@echo "Running Tests with AddressSanitizer"
	@echo "==================================="
	$(CC) $(AETHER_REQUIRED_CFLAGS) $(CFLAGS_NO_OPT) $(ASAN_OPT) $(ASAN_FLAGS) $(TEST_SRC) build/asan/libaether_compiler.a build/asan/libaether.a -Icompiler -Istd -Istd/collections -o build/test_runner_asan$(EXE_EXT) $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS)
ifeq ($(shell uname -s),Linux)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ./build/test_runner_asan$(EXE_EXT)
else
	ASAN_OPTIONS=halt_on_error=1 ./build/test_runner_asan$(EXE_EXT)
endif

# macOS leaks(1) gate (#468). Builds a curated set of memory-
# ownership regression programs and runs each under
# `MallocStackLogging=1 leaks --atExit`. Skips cleanly on non-Darwin
# (Linux gets leak coverage from test-valgrind + ASan-LSan). NOT
# folded into `make ci` — MallocStackLogging is ~10x slower; runs as
# its own macOS-CI step. See tests/run_macos_leaks.sh for the covered
# set and rationale.
test-macos-leaks: compiler ae stdlib
	@sh tests/run_macos_leaks.sh

test-memory: compiler stdlib-memory
	@echo "==================================="
	@echo "Running Memory Tracking Tests"
	@echo "==================================="
	$(CC) $(AETHER_REQUIRED_CFLAGS) $(CFLAGS) $(MEM_FLAGS) $(TEST_SRC) build/memory/libaether_compiler.a build/memory/libaether.a -Icompiler -Istd -Istd/collections -o build/test_runner_mem$(EXE_EXT) $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS)
	./build/test_runner_mem$(EXE_EXT)

test-manual-runtime: compiler
	@echo "Building manual runtime test..."
	$(CC) $(AETHER_REQUIRED_CFLAGS) $(CFLAGS) tests/runtime/test_runtime_manual.c $(RUNTIME_SRC) $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS) -o build/test_runtime_manual$(EXE_EXT)
	@echo "Running manual runtime test..."
	./build/test_runtime_manual$(EXE_EXT)

# Cross-compilation smoke test (#1105): `ae build --target=<triple>` via
# the zig cc backend. Self-skips when zig is not installed, so it is safe
# to run anywhere (it is not part of the default `test` gate because CI
# does not carry the zig toolchain).
test-cross: ae stdlib
	@bash tests/scripts/cross_compile.sh

# Test .ae source files - compiles and runs each test file
ifdef WINDOWS_NATIVE
test-ae: compiler ae stdlib
	@echo ===================================
	@echo   Running Aether Source Tests (.ae)
	@echo ===================================
	@.\build\ae.exe test
else
test-ae: compiler ae stdlib
	@echo "==================================="
	@echo "  Running Aether Source Tests (.ae)"
	@echo "  Parallel: $(NPROC) jobs"
	@echo "==================================="
	@tmpdir=$$(mktemp -d); \
	script="$$tmpdir/run_test.sh"; \
	printf '#!/bin/sh\n'                                                                             > "$$script"; \
	printf 'f="$$1"; tmpdir="$$2"; root="$$3"\n'                                                    >> "$$script"; \
	printf '# Portable per-test timeout: GNU coreutils `timeout` on Linux/MSYS2,\n'                 >> "$$script"; \
	printf '# `gtimeout` on macOS (coreutils via brew); empty when neither exists\n'                >> "$$script"; \
	printf '# (macOS without coreutils) so the test still runs, just unbounded.\n'                  >> "$$script"; \
	printf 'if command -v timeout >/dev/null 2>&1; then TO="timeout $${AE_TEST_TIMEOUT:-120}"; \\\n' >> "$$script"; \
	printf 'elif command -v gtimeout >/dev/null 2>&1; then TO="gtimeout $${AE_TEST_TIMEOUT:-120}"; \\\n' >> "$$script"; \
	printf 'else TO=""; fi\n'                                                                       >> "$$script"; \
	printf 'name=$$(echo "$$f" | sed "s|tests/||;s|/|_|g;s|\\.ae$$||")\n'                         >> "$$script"; \
	printf 'dir=$$(dirname "$$f")\n'                                                                >> "$$script"; \
	printf 'base=$$(basename "$$f")\n'                                                              >> "$$script"; \
	printf 'if [ -d "$$dir/lib" ]; then\n'                                                          >> "$$script"; \
	printf '  cmd="cd $$dir && $$root/build/ae build $$base $${AE_BUILD_FLAGS:-} -o $$root/build/test_$$name"\n' >> "$$script"; \
	printf 'else\n'                                                                                 >> "$$script"; \
	printf '  cmd="$$root/build/ae build $$f $${AE_BUILD_FLAGS:-} -o $$root/build/test_$$name"\n'   >> "$$script"; \
	printf 'fi\n'                                                                                   >> "$$script"; \
	printf 'if eval "$$cmd" 2>"$$tmpdir/build_$$name.err"; then\n'                                  >> "$$script"; \
	printf '  $$TO "$$root/build/test_$$name" >"$$tmpdir/run_$$name.out" 2>"$$tmpdir/run_$$name.err"\n'  >> "$$script"; \
	printf '  rc=$$?\n'                                                                             >> "$$script"; \
	printf '  if [ $$rc -eq 0 ]; then\n'                                                            >> "$$script"; \
	printf '    echo "  [PASS] $$name"; touch "$$tmpdir/PASS_$$name"\n'                             >> "$$script"; \
	printf '  elif [ $$rc -eq 124 ]; then\n'                                                        >> "$$script"; \
	printf '    echo "  [TIMEOUT] $$name (exceeded $${AE_TEST_TIMEOUT:-120}s)"\n'                    >> "$$script"; \
	printf '    printf timeout > "$$tmpdir/phase_$$name.txt"\n'                                      >> "$$script"; \
	printf '    touch "$$tmpdir/FAIL_$$name"\n'                                                      >> "$$script"; \
	printf '  else\n'                                                                               >> "$$script"; \
	printf '    echo "  [FAIL] $$name (runtime error, exit $$rc)"\n'                                >> "$$script"; \
	printf '    printf runtime > "$$tmpdir/phase_$$name.txt"\n'                                     >> "$$script"; \
	printf '    printf %%s "$$rc" > "$$tmpdir/rc_$$name.txt"\n'                                     >> "$$script"; \
	printf '    touch "$$tmpdir/FAIL_$$name"\n'                                                     >> "$$script"; \
	printf '  fi\n'                                                                                 >> "$$script"; \
	printf 'else\n'                                                                                 >> "$$script"; \
	printf '  echo "  [FAIL] $$name (compile error)"\n'                                             >> "$$script"; \
	printf '  printf compile > "$$tmpdir/phase_$$name.txt"\n'                                       >> "$$script"; \
	printf '  touch "$$tmpdir/FAIL_$$name"\n'                                                       >> "$$script"; \
	printf '  head -5 "$$tmpdir/build_$$name.err" 2>/dev/null\n'                                    >> "$$script"; \
	printf 'fi\n'                                                                                   >> "$$script"; \
	chmod +x "$$script"; \
	root=$$(pwd); \
	sed '/^#/d' tests/ae_sweep_prune.txt > "$$tmpdir/prune.txt"; \
	if [ -n "$(AE_SWEEP_EXTRA_PRUNE)" ]; then \
	  sed '/^#/d;/^$$/d' "$(AE_SWEEP_EXTRA_PRUNE)" >> "$$tmpdir/prune.txt"; \
	  echo "  Extra prune list: $(AE_SWEEP_EXTRA_PRUNE) ($$(sed '/^#/d;/^$$/d' "$(AE_SWEEP_EXTRA_PRUNE)" | wc -l | tr -d ' ') patterns)"; \
	fi; \
	{ find tests/syntax tests/compiler tests/integration tests/regression -name '*.ae' -print 2>/dev/null; find std -name 'test_*.ae' -print 2>/dev/null; } \
	    | grep -v -F -f "$$tmpdir/prune.txt" | sort | \
	xargs -P $(NPROC) -I{} "$$script" "{}" "$$tmpdir" "$$root"; \
	sh_script="$$tmpdir/run_sh_dir.sh"; \
	printf '#!/bin/sh\n'                                                                                          > "$$sh_script"; \
	printf 'dir="$$1"; tmpdir="$$2"\n'                                                                            >> "$$sh_script"; \
	printf 'if command -v timeout >/dev/null 2>&1; then TO="timeout $${AE_SH_TEST_TIMEOUT:-180}"; \\\n'          >> "$$sh_script"; \
	printf 'elif command -v gtimeout >/dev/null 2>&1; then TO="gtimeout $${AE_SH_TEST_TIMEOUT:-180}"; \\\n'      >> "$$sh_script"; \
	printf 'else TO=""; fi\n'                                                                                     >> "$$sh_script"; \
	printf 'for sh_test in $$(find "$$dir" -maxdepth 1 -name "test_*.sh" 2>/dev/null | sort); do\n'              >> "$$sh_script"; \
	printf '  name=$$(echo "$$sh_test" | sed "s|tests/||;s|/|_|g;s|\\.sh$$||")\n'                              >> "$$sh_script"; \
	printf '  $$TO bash "$$sh_test" >"$$tmpdir/run_$$name.out" 2>"$$tmpdir/run_$$name.err"; sh_rc=$$?\n'         >> "$$sh_script"; \
	printf '  if [ $$sh_rc -eq 0 ]; then\n'                                                                       >> "$$sh_script"; \
	printf '    if grep -q "\\[SKIP-WIN\\]" "$$tmpdir/run_$$name.out" 2>/dev/null; then\n'                        >> "$$sh_script"; \
	printf '      reason=$$(grep "\\[SKIP-WIN\\]" "$$tmpdir/run_$$name.out" | head -1 | sed "s/^[[:space:]]*\\[SKIP-WIN\\][[:space:]]*//"); \n' >> "$$sh_script"; \
	printf '      echo "  [SKIP] $$name — $$reason"; touch "$$tmpdir/PASS_$$name"\n'                              >> "$$sh_script"; \
	printf '    else\n'                                                                                           >> "$$sh_script"; \
	printf '      echo "  [PASS] $$name"; touch "$$tmpdir/PASS_$$name"\n'                                         >> "$$sh_script"; \
	printf '    fi\n'                                                                                             >> "$$sh_script"; \
	printf '  elif [ $$sh_rc -eq 124 ]; then\n'                                                                   >> "$$sh_script"; \
	printf '    echo "  [TIMEOUT] $$name (shell test exceeded $${AE_SH_TEST_TIMEOUT:-180}s)"\n'                   >> "$$sh_script"; \
	printf '    printf timeout > "$$tmpdir/phase_$$name.txt"\n'                                                   >> "$$sh_script"; \
	printf '    touch "$$tmpdir/FAIL_$$name"\n'                                                                   >> "$$sh_script"; \
	printf '  else\n'                                                                                             >> "$$sh_script"; \
	printf '    echo "  [FAIL] $$name (shell test)"\n'                                                            >> "$$sh_script"; \
	printf '    printf shell > "$$tmpdir/phase_$$name.txt"\n'                                                     >> "$$sh_script"; \
	printf '    touch "$$tmpdir/FAIL_$$name"\n'                                                                   >> "$$sh_script"; \
	printf '  fi\n'                                                                                               >> "$$sh_script"; \
	printf 'done\n'                                                                                               >> "$$sh_script"; \
	chmod +x "$$sh_script"; \
	sh_nproc=$${SH_NPROC:-1}; \
	if [ -n "$(AE_SWEEP_EXTRA_PRUNE)" ]; then \
	  sed '/^#/d;/^$$/d' "$(AE_SWEEP_EXTRA_PRUNE)" > "$$tmpdir/shprune.txt"; \
	else \
	  : > "$$tmpdir/shprune.txt"; \
	fi; \
	find tests/integration -name 'test_*.sh' 2>/dev/null | xargs -n1 dirname | sort -u \
	    | { if [ -s "$$tmpdir/shprune.txt" ]; then grep -v -F -f "$$tmpdir/shprune.txt"; else cat; fi; } \
	    | xargs -P $$sh_nproc -I{} "$$sh_script" "{}" "$$tmpdir"; \
	passed=$$(ls "$$tmpdir"/PASS_* 2>/dev/null | wc -l | tr -d ' '); \
	failed=$$(ls "$$tmpdir"/FAIL_* 2>/dev/null | wc -l | tr -d ' '); \
	total=$$((passed + failed)); \
	echo ""; \
	if [ "$$failed" -gt 0 ]; then \
		echo "=== FAILURE DETAILS ==="; \
		for fail_file in "$$tmpdir"/FAIL_*; do \
			fname=$$(basename "$$fail_file" | sed 's/^FAIL_//'); \
			phase=$$(cat "$$tmpdir/phase_$$fname.txt" 2>/dev/null || echo unknown); \
			case "$$phase" in \
				compile) echo "--- $$fname (compile error) ---" ;; \
				runtime) rc=$$(cat "$$tmpdir/rc_$$fname.txt" 2>/dev/null || echo '?'); \
				         echo "--- $$fname (runtime error, exit $$rc) ---" ;; \
				timeout) echo "--- $$fname (TIMED OUT — hung; killed by per-test timeout) ---" ;; \
				shell)   echo "--- $$fname (shell test) ---" ;; \
				*)       echo "--- $$fname ---" ;; \
			esac; \
			if [ "$$phase" = "compile" ]; then \
				cat "$$tmpdir/build_$$fname.err" 2>/dev/null || echo "(no error output)"; \
			else \
				if [ -s "$$tmpdir/run_$$fname.out" ]; then \
					echo "(stdout)"; cat "$$tmpdir/run_$$fname.out"; \
				fi; \
				if [ -s "$$tmpdir/run_$$fname.err" ]; then \
					echo "(stderr)"; cat "$$tmpdir/run_$$fname.err"; \
				fi; \
				if [ ! -s "$$tmpdir/run_$$fname.out" ] && [ ! -s "$$tmpdir/run_$$fname.err" ]; then \
					echo "(no output)"; \
				fi; \
			fi; \
			echo ""; \
		done; \
	fi; \
	echo "Aether Tests: $$passed passed, $$failed failed, $$total total"; \
	rm -rf "$$tmpdir"; \
	if [ "$$failed" -gt 0 ]; then exit 1; fi
endif

# Install smoke test: installs to a temp dir, runs ae init + ae run, cleans up
test-install: compiler ae stdlib
	@echo "==================================="
	@echo "  Install Smoke Test"
	@echo "==================================="
	@tmpdir=$$(mktemp -d) && \
	echo "  Installing to $$tmpdir..." && \
	{ ./install.sh "$$tmpdir" < /dev/null > "$$tmpdir/install.log" 2>&1 || \
	  { echo "  install.sh failed:"; tail -30 "$$tmpdir/install.log" | sed 's/^/      /'; false; }; } && \
	echo "  Testing ae version..." && \
	AETHER_HOME="$$tmpdir" "$$tmpdir/bin/ae$(EXE_EXT)" version > /dev/null 2>&1 && \
	echo "  Testing ae init + ae run..." && \
	projdir=$$(mktemp -d) && \
	cd "$$projdir" && \
	AETHER_HOME="$$tmpdir" "$$tmpdir/bin/ae$(EXE_EXT)" init smoketest > /dev/null 2>&1 && \
	cd smoketest && \
	output=$$(AETHER_HOME="$$tmpdir" "$$tmpdir/bin/ae$(EXE_EXT)" run 2>&1) && \
	echo "  Output: $$output" && \
	echo "$$output" | grep -q "Hello from smoketest" && \
	echo "  Cleaning up..." && \
	rm -rf "$$tmpdir" "$$projdir" && \
	echo "  [PASS] Install smoke test" || \
	(echo "  [FAIL] Install smoke test"; rm -rf "$$tmpdir" "$$projdir" 2>/dev/null; exit 1)

# Release archive smoke test: packages a tarball exactly like release.yml,
# extracts it (simulating `ae version install`), and verifies ae init + ae run
# work from the extracted layout. This catches archive structure bugs that
# test-install (which tests install.sh) would miss.
# #1395: a release archive built from an older tree than the sources it ships
# links fine for us and fails at the user's build, silently until someone calls
# the one function added last. Verified here, next to the archive packaging.
.PHONY: check-archive-exports
check-archive-exports: stdlib
	@echo "==================================="
	@echo "  Archive Export Check"
	@echo "==================================="
	@sh scripts/check_archive_exports.sh build/libaether.a

test-release-archive: compiler ae stdlib check-archive-exports
	@echo "==================================="
	@echo "  Release Archive Smoke Test"
	@echo "==================================="
	@tmpdir=$$(mktemp -d) && \
	reldir="$$tmpdir/release" && \
	mkdir -p "$$reldir/bin" "$$reldir/lib/aether" "$$reldir/share/aether" "$$reldir/include/aether" && \
	cp build/aetherc$(EXE_EXT) "$$reldir/bin/" && \
	cp build/ae$(EXE_EXT)      "$$reldir/bin/" && \
	chmod 755 "$$reldir/bin/"* && \
	if [ -f build/libaether.a ]; then cp build/libaether.a "$$reldir/lib/aether/"; fi && \
	for dir in runtime runtime/actors runtime/scheduler runtime/utils \
	           runtime/memory runtime/config std std/string std/io std/math \
	           std/net std/collections std/json std/xml std/fs std/log std/http \
	           std/file std/dir std/path std/tcp std/list std/map std/dl \
	           std/config std/actors std/capsicum std/casper std/snapshot \
	           std/audio std/worker; do \
	  if [ -d "$$dir" ]; then \
	    mkdir -p "$$reldir/include/aether/$$dir"; \
	    cp "$$dir"/*.h "$$reldir/include/aether/$$dir/" 2>/dev/null || true; \
	  fi; \
	done && \
	cp include/*.h "$$reldir/include/aether/" 2>/dev/null; \
	cp -r runtime "$$reldir/share/aether/" && \
	cp -r std     "$$reldir/share/aether/" && \
	cp build/MANIFEST "$$reldir/share/aether/" && \
	rm -rf "$$reldir/share/aether/runtime/examples" && \
	echo "  Created release layout in $$reldir" && \
	echo "  Packing tarball..." && \
	(cd "$$reldir" && tar -czf "$$tmpdir/aether-test.tar.gz" *) && \
	echo "  Extracting to simulated version dir..." && \
	verdir="$$tmpdir/extracted" && mkdir -p "$$verdir" && \
	tar -xzf "$$tmpdir/aether-test.tar.gz" -C "$$verdir" && \
	echo "  Checking extracted layout..." && \
	test -f "$$verdir/bin/aetherc$(EXE_EXT)" || (echo "  FAIL: bin/aetherc missing"; exit 1) && \
	test -f "$$verdir/bin/ae$(EXE_EXT)"      || (echo "  FAIL: bin/ae missing"; exit 1) && \
	test -f "$$verdir/lib/aether/libaether.a" || (echo "  FAIL: lib/aether/libaether.a missing"; exit 1) && \
	test -d "$$verdir/share/aether/runtime"  || (echo "  FAIL: share/aether/runtime missing"; exit 1) && \
	test -d "$$verdir/share/aether/std"      || (echo "  FAIL: share/aether/std missing"; exit 1) && \
	test -s "$$verdir/share/aether/MANIFEST" || (echo "  FAIL: share/aether/MANIFEST missing; ae cannot build from source without it"; exit 1) && \
	echo "  Testing ae init + ae run from extracted archive..." && \
	projdir=$$(mktemp -d) && \
	cd "$$projdir" && \
	AETHER_HOME="$$verdir" "$$verdir/bin/ae$(EXE_EXT)" init archivetest > /dev/null 2>&1 && \
	cd archivetest && \
	output=$$(AETHER_HOME="$$verdir" "$$verdir/bin/ae$(EXE_EXT)" run 2>&1) && \
	echo "  Output: $$output" && \
	echo "$$output" | grep -q "Hello from archivetest" && \
	echo "  Cleaning up..." && \
	rm -rf "$$tmpdir" "$$projdir" && \
	echo "  [PASS] Release archive smoke test" || \
	(echo "  [FAIL] Release archive smoke test"; rm -rf "$$tmpdir" "$$projdir" 2>/dev/null; exit 1)

# Run both C unit tests and .ae integration tests
# Syntax-and-warnings gate for the standalone sources. Compile only: several
# are benchmarks that take minutes, and what rots is the code, not the timing.
.PHONY: check-standalone
check-standalone:
	@echo "==================================="
	@echo "  Standalone sources (compile gate)"
	@echo "==================================="
	@fail=0; \
	for f in $(STANDALONE_SRC); do \
	  if $(CC) -fsyntax-only $(AETHER_REQUIRED_CFLAGS) $(CFLAGS) -Werror \
	       -Itests/runtime -Itools/profiler -Istd/log $$f 2>$(BUILD_DIR)/standalone.err; then \
	    printf '  ok    %s\n' "$$f"; \
	  else \
	    printf '  FAIL  %s\n' "$$f"; \
	    head -5 $(BUILD_DIR)/standalone.err | sed 's/^/        /'; \
	    fail=1; \
	  fi; \
	done; \
	rm -f $(BUILD_DIR)/standalone.err; \
	if [ $$fail -ne 0 ]; then echo "check-standalone: FAILED"; exit 1; fi; \
	echo "check-standalone: all standalone sources compile clean"

test-all: test test-ae check-standalone
	@echo ""
	@echo "==================================="
	@echo "  All Tests Complete"
	@echo "==================================="

# Benchmark presets: full (10M), medium (1M), low (100K), stress (100M)
BENCHMARK_PRESET ?= low

# ---- JSON parser benchmark (standalone; no actor runtime needed) ----
#
# bench-json         — runs the current std/json parser against corpus/
# bench-json-compare — same + yyjson reference (auto-fetches yyjson to
#                      benchmarks/json/vendor/ on first use; vendor/ is
#                      gitignored so it never hits the repo)
# bench-json-gen     — (re)generates corpus fixtures including the 10 MB
#                      large.json. Idempotent and deterministic.
#
# The three targets are self-contained: they don't depend on `compiler`
# or `ae` because the bench runner links directly against the JSON source
# files it needs.

BENCH_JSON_CFLAGS := -O2 -Wall -Wextra \
  -Istd/json \
  -DAETHER_VERSION=\"bench\"
# The rewritten parser has zero stdlib-internal dependencies — no
# aether_collections, no aether_string. Keep the bench link small so
# changes to other stdlib files don't churn this build.
BENCH_JSON_SRCS := benchmarks/json/run_json_bench.c \
                   std/json/aether_json.c

.PHONY: bench-json bench-json-compare bench-json-gen bench-json-fetch-yyjson

$(BUILD_DIR)/gen_corpus: benchmarks/json/gen_corpus.c | $(BUILD_DIR)
	@$(CC) -O2 -Wall -Wextra $< -o $@

bench-json-gen: $(BUILD_DIR)/gen_corpus
	@mkdir -p benchmarks/json/corpus
	@$(BUILD_DIR)/gen_corpus api-response benchmarks/json/corpus/api-response.json
	@$(BUILD_DIR)/gen_corpus strings       benchmarks/json/corpus/strings-heavy.json
	@$(BUILD_DIR)/gen_corpus numbers       benchmarks/json/corpus/numbers-heavy.json
	@$(BUILD_DIR)/gen_corpus deep          benchmarks/json/corpus/deep.json
	@$(BUILD_DIR)/gen_corpus large         benchmarks/json/corpus/large.json
	@echo "✓ Corpus generated in benchmarks/json/corpus/"

$(BUILD_DIR)/run_json_bench: $(BENCH_JSON_SRCS) | $(BUILD_DIR)
	@echo "Compiling json bench runner..."
	@$(CC) $(BENCH_JSON_CFLAGS) $(BENCH_JSON_SRCS) -o $@ -lm

bench-json: $(BUILD_DIR)/run_json_bench
	@if [ ! -f benchmarks/json/corpus/large.json ]; then \
	  echo "Note: large.json missing — run 'make bench-json-gen' to create the full corpus."; \
	fi
	@$(BUILD_DIR)/run_json_bench

# Fetch yyjson (MIT) to benchmarks/json/vendor/ for apples-to-apples comparison.
# vendor/ is gitignored — yyjson is NOT vendored into the repo, it's a
# build-time fetch for this target only.
bench-json-fetch-yyjson:
	@mkdir -p benchmarks/json/vendor
	@if [ ! -f benchmarks/json/vendor/yyjson.c ]; then \
	  echo "Fetching yyjson for reference benchmark (not committed)..."; \
	  curl -sL -o benchmarks/json/vendor/yyjson.c https://raw.githubusercontent.com/ibireme/yyjson/master/src/yyjson.c || \
	    { echo "ERROR: could not fetch yyjson.c — network?"; exit 1; }; \
	  curl -sL -o benchmarks/json/vendor/yyjson.h https://raw.githubusercontent.com/ibireme/yyjson/master/src/yyjson.h || \
	    { echo "ERROR: could not fetch yyjson.h — network?"; exit 1; }; \
	fi

$(BUILD_DIR)/run_yyjson_bench: benchmarks/json/run_yyjson_bench.c benchmarks/json/vendor/yyjson.c | $(BUILD_DIR) bench-json-fetch-yyjson
	@echo "Compiling yyjson reference bench..."
	@$(CC) -O2 -Wall -Wextra -Ibenchmarks/json/vendor \
	  benchmarks/json/run_yyjson_bench.c \
	  benchmarks/json/vendor/yyjson.c \
	  -o $@

bench-json-compare: $(BUILD_DIR)/run_json_bench $(BUILD_DIR)/run_yyjson_bench
	@echo "=== current std.json parser ==="
	@$(BUILD_DIR)/run_json_bench
	@echo ""
	@echo "=== yyjson (reference, not vendored) ==="
	@$(BUILD_DIR)/run_yyjson_bench

# ---- JSON parser hardening targets ----
#
# test-json-asan  — parse every corpus fixture under AddressSanitizer
#                   + UndefinedBehaviorSanitizer. Portable across GCC
#                   ≥4.8 and every Clang (Linux + macOS).
# test-json-valgrind — same under Valgrind on Linux (macOS skips if no
#                      valgrind installed).
# test-json-conformance — (Phase 4a) run JSONTestSuite if committed.

.PHONY: test-json-asan test-json-valgrind test-json-conformance

# Parser-focused ASan build: links only the parser + bench harness
# (same code paths as production), ignoring the broader stdlib so we
# don't pull in ASan-unfriendly subsystems.
$(BUILD_DIR)/run_json_bench_asan: $(BENCH_JSON_SRCS) | $(BUILD_DIR)
	@echo "Compiling json bench runner with ASan+UBSan..."
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  $(BENCH_JSON_CFLAGS) $(BENCH_JSON_SRCS) -o $@ -lm

test-json-asan: $(BUILD_DIR)/run_json_bench_asan
	@echo "==================================="
	@echo "  JSON parser: ASan + UBSan check"
	@echo "==================================="
	@if [ ! -f benchmarks/json/corpus/large.json ]; then \
	  $(MAKE) -s bench-json-gen >/dev/null; \
	fi
	@JSON_BENCH_WARMUP=2 JSON_BENCH_ITERS=10 $(BUILD_DIR)/run_json_bench_asan
	@echo "✓ JSON ASan+UBSan clean"

test-json-valgrind: $(BUILD_DIR)/run_json_bench
	@command -v valgrind >/dev/null 2>&1 || { \
	  echo "valgrind not installed — skipping (macOS and Windows usually don't have it)"; \
	  exit 0; \
	}
	@echo "==================================="
	@echo "  JSON parser: Valgrind leak + error check"
	@echo "==================================="
	@if [ ! -f benchmarks/json/corpus/large.json ]; then \
	  $(MAKE) -s bench-json-gen >/dev/null; \
	fi
	@JSON_BENCH_WARMUP=1 JSON_BENCH_ITERS=2 \
	  valgrind --error-exitcode=1 --leak-check=full --errors-for-leak-kinds=definite \
	  $(BUILD_DIR)/run_json_bench
	@echo "✓ JSON Valgrind clean"

test-json-conformance: $(BUILD_DIR)/run_json_conformance
	@if [ ! -d tests/conformance/json/cases ]; then \
	  echo "tests/conformance/json/cases/ not present."; \
	  exit 1; \
	fi
	@$(BUILD_DIR)/run_json_conformance tests/conformance/json/cases

$(BUILD_DIR)/run_json_conformance: tests/conformance/json/run_conformance.c std/json/aether_json.c | $(BUILD_DIR)
	@$(CC) -O2 -Wall -Wextra -Istd/json \
	  tests/conformance/json/run_conformance.c std/json/aether_json.c \
	  -o $@ -lm

benchmark: compiler ae stdlib
	@echo "============================================"
	@echo "  Running Cross-Language Benchmark Suite"
	@echo "============================================"
	@echo ""
	@mkdir -p benchmarks/cross-language/build
	@echo "Building benchmark runner (Aether)..."
	@AETHER_HOME="" ./build/ae build benchmarks/cross-language/run_benchmarks.ae -o benchmarks/cross-language/build/bench_runner
	@cd benchmarks/cross-language && ./build/bench_runner
	@pkill -9 -f "benchmarks/cross-language/visualize/server" 2>/dev/null || true
	@echo ""
	@echo "Building Aether HTTP server..."
	@AETHER_HOME="" ./build/ae build benchmarks/cross-language/visualize/server.ae -o benchmarks/cross-language/visualize/server
	@echo "Server built successfully"
	@echo ""
	@echo "=========================================="
	@echo "  Launching Benchmark Visualization UI"
	@echo "=========================================="
	@echo ""
	@echo "Open your browser at http://localhost:8080"
	@echo "Press Ctrl+C to stop the server"
	@echo ""
	@cd benchmarks/cross-language/visualize && ./server

ifdef WINDOWS_NATIVE
examples: compiler ae
	@echo ===================================
	@echo   Building Aether Examples
	@echo ===================================
	@.\build\ae.exe examples
else
examples: compiler ae stdlib
	@echo "==================================="
	@echo "  Building Aether Examples"
	@echo "  Parallel: $(NPROC) jobs"
	@echo "==================================="
	@$(MKDIR) $(BUILD_DIR)/examples $(BUILD_DIR)/examples/basics $(BUILD_DIR)/examples/actors $(BUILD_DIR)/examples/applications $(BUILD_DIR)/examples/c-interop $(BUILD_DIR)/examples/stdlib
	@tmpdir=$$(mktemp -d); \
	script="$$tmpdir/build_one.sh"; \
	printf '#!/bin/sh\n'                                                                            > "$$script"; \
	printf 'src="$$1"; tmpdir="$$2"; root="$$3"\n'                                                  >> "$$script"; \
	printf 'name=$$(echo "$$src" | sed "s|examples/||;s|\\.ae$$||")\n'                              >> "$$script"; \
	printf 'key=$$(echo "$$name" | sed "s|/|_|g")\n'                                                >> "$$script"; \
	printf 'dir=$$(dirname "$$src")\n'                                                              >> "$$script"; \
	printf 'extra_c=""\n'                                                                           >> "$$script"; \
	printf 'if [ -d "$$dir" ]; then\n'                                                              >> "$$script"; \
	printf '  extra_c=$$(find "$$dir" -maxdepth 1 -name "*.c" 2>/dev/null | tr "\\n" " ")\n'        >> "$$script"; \
	printf 'fi\n'                                                                                   >> "$$script"; \
	printf 'out_c="$$root/$(BUILD_DIR)/examples/$$name.c"\n'                                        >> "$$script"; \
	printf 'mkdir -p "$$(dirname "$$out_c")"\n'                                                     >> "$$script"; \
	printf 'if ! "$$root/build/aetherc$(EXE_EXT)" "$$src" "$$out_c" 2>"$$tmpdir/$$key.aetherc.err"; then\n' >> "$$script"; \
	printf '  printf "  %%-30s %%s\\n" "$$name" "FAIL (aetherc)"\n'                                 >> "$$script"; \
	printf '  head -5 "$$tmpdir/$$key.aetherc.err"\n'                                               >> "$$script"; \
	printf '  touch "$$tmpdir/FAIL_$$key"\n'                                                        >> "$$script"; \
	printf '  exit 1\n'                                                                             >> "$$script"; \
	printf 'fi\n'                                                                                   >> "$$script"; \
	printf 'if ! $(CC) $(AETHER_REQUIRED_CFLAGS) $(CFLAGS) "$$out_c" $$extra_c "$$root/$(BUILD_DIR)/libaether.a" -o "$$root/$(BUILD_DIR)/examples/$$name$(EXE_EXT)" $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS) 2>"$$tmpdir/$$key.gcc.err"; then\n' >> "$$script"; \
	printf '  printf "  %%-30s %%s\\n" "$$name" "FAIL (gcc)"\n'                                     >> "$$script"; \
	printf '  head -20 "$$tmpdir/$$key.gcc.err"\n'                                                  >> "$$script"; \
	printf '  touch "$$tmpdir/FAIL_$$key"\n'                                                        >> "$$script"; \
	printf '  exit 1\n'                                                                             >> "$$script"; \
	printf 'fi\n'                                                                                   >> "$$script"; \
	printf 'printf "  %%-30s %%s\\n" "$$name" "OK"\n'                                               >> "$$script"; \
	printf 'touch "$$tmpdir/PASS_$$key"\n'                                                          >> "$$script"; \
	chmod +x "$$script"; \
	root=$$(pwd); \
	find examples -name '*.ae' \
	    | grep -v '/lib/' | grep -v '/packages/' | grep -v '/embedded-java/' | grep -v '/host-.*-demo\.ae$$' \
	    | grep -v '/ae-help-demo/' \
	    | sort \
	    | xargs -P $(NPROC) -I{} "$$script" "{}" "$$tmpdir" "$$root"; \
	pass=$$(ls "$$tmpdir"/PASS_* 2>/dev/null | wc -l | tr -d ' '); \
	fail=$$(ls "$$tmpdir"/FAIL_* 2>/dev/null | wc -l | tr -d ' '); \
	echo ""; \
	echo "  $$pass passed, $$fail failed"; \
	echo "  Binaries in $(BUILD_DIR)/examples/"; \
	rm -rf "$$tmpdir"; \
	if [ "$$fail" -gt 0 ]; then exit 1; fi
endif

examples-run: examples
	@echo "==================================="
	@echo "  Running Aether Examples"
	@echo "==================================="
	@for bin in $$(find $(BUILD_DIR)/examples -type f ! -name '*.c' ! -name '*.o' | sort); do \
		test -x "$$bin" || continue; \
		name=$$(echo $$bin | sed "s|$(BUILD_DIR)/examples/||"); \
		echo "--- $$name ---"; \
		timeout 5 $$bin 2>&1 || true; \
		echo ""; \
	done

# Standalone aether-lsp binary — kept as a transitional alias for editor
# integrations that hardcode the binary name. The same code is also embedded
# in `aetherc` and runs via `aetherc lsp` (issue #327); new editor integrations
# should prefer that subcommand.
#
# `aether_lsp.c` is now part of `libaether_compiler.a` (so it's reusable
# across both binaries), so the link line just needs the 9-line wrapper
# at lsp/main.c plus the archive.
lsp: compiler stdlib
	@echo "==================================="
	@echo "Building Aether LSP Server ($(DETECTED_OS))"
	@echo "==================================="
	$(CC) $(AETHER_REQUIRED_CFLAGS) $(CFLAGS) lsp/main.c build/libaether_compiler.a build/libaether.a -Icompiler -Ilsp -Istd -Istd/collections -o build/aether-lsp$(EXE_EXT) $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS)
	@echo "✓ LSP Server built successfully: build/aether-lsp$(EXE_EXT)"

apkg:
	@echo "==================================="
	@echo "Building Aether Package Manager ($(DETECTED_OS))"
	@echo "==================================="
	$(CC) $(AETHER_REQUIRED_CFLAGS) $(CFLAGS) tools/apkg/main.c tools/apkg/apkg.c tools/apkg/toml_parser.c $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS) -o build/apkg$(EXE_EXT)
	@echo "✓ Package Manager built successfully: build/apkg$(EXE_EXT)"

# Per-object compile for the tools driver (#1221). Static pattern rule so it
# wins over the generic $(OBJ_DIR)/%.o rule (which uses CFLAGS, wrong include
# path and defines for tools). The order-only dir prereqs create the obj tree.
$(TOOLS_OBJS): $(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)/tools $(OBJ_DIR)/tools/apkg
	@echo "Compiling $<..."
	@$(CC) $(TOOLS_CFLAGS) -c $< -o $@

ae: compiler $(TOOLS_OBJS)
	@echo "==================================="
	@echo "Building ae command-line tool ($(DETECTED_OS)) v$(VERSION)"
	@echo "==================================="
	@$(CC) $(TOOLS_OBJS) -o build/ae$(EXE_EXT) $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS) $(if $(AETHER_ENABLE_LLM),$(LLM_LDFLAGS))
	@echo "✓ Built successfully: build/ae$(EXE_EXT)"
	@echo ""
	@echo "Usage:"
	@echo "  ./build/ae run file.ae       Run a program"
	@echo "  ./build/ae build file.ae     Build an executable"
	@echo "  ./build/ae init myproject    Create a new project"
	@echo "  ./build/ae test              Run tests"
	@echo "  ./build/ae help              Show all commands"

profiler:
	@echo "==================================="
	@echo "Building Aether Profiler Dashboard ($(DETECTED_OS))"
	@echo "==================================="
	$(CC) $(AETHER_REQUIRED_CFLAGS) $(CFLAGS) -DAETHER_PROFILING tools/profiler/profiler_server.c tools/profiler/profiler_demo.c $(RUNTIME_SRC) $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS) -o build/profiler_demo$(EXE_EXT)
	@echo "✓ Profiler built successfully: build/profiler_demo$(EXE_EXT)"
	@echo ""
	@echo "Run the demo and open http://localhost:8081"

docgen:
	@echo "==================================="
	@echo "Building Documentation Generator ($(DETECTED_OS))"
	@echo "==================================="
	@$(MKDIR) build
	$(CC) -O2 -Wall tools/docgen/docgen.c -o build/docgen$(EXE_EXT)
	@echo "✓ Documentation generator built: build/docgen$(EXE_EXT)"
	@echo ""
	@echo "Usage: ./build/docgen std docs/api"

docs-server: compiler
	@echo "==================================="
	@echo "Building Documentation Server ($(DETECTED_OS))"
	@echo "==================================="
	@./build/aetherc$(EXE_EXT) tools/docgen/server.ae build/docs_server_gen.c
	@$(CC) -O2 -o build/docs-server$(EXE_EXT) build/docs_server_gen.c tools/docgen/server_ffi.c \
		$(RUNTIME_SRC) $(STD_SRC) $(STD_REACTOR_SRC) $(COLLECTIONS_SRC) $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS)
	@rm -f build/docs_server_gen.c
	@echo "✓ Documentation server built: build/docs-server$(EXE_EXT)"

docs: docgen
	@echo "==================================="
	@echo "Generating API Documentation"
	@echo "==================================="
	@$(MKDIR) docs/api
	./build/docgen$(EXE_EXT) std docs/api
	@echo ""
	@echo "✓ Documentation generated in docs/api/"
	@echo "  Run 'make docs-serve' to view at http://localhost:3000"

docs-serve: docs docs-server
	@echo ""
	./build/docs-server$(EXE_EXT)

# Precompiled stdlib archive — runtime + std for user programs.
stdlib: $(STD_OBJS) $(STD_REACTOR_OBJS) $(COLLECTIONS_OBJS) $(RUNTIME_OBJS) build/libaether_compiler.a build/MANIFEST | $(VERSION_HEADER)
	@echo "Creating precompiled stdlib archive..."
	@ar rcs build/libaether.a $(STD_OBJS) $(STD_REACTOR_OBJS) $(COLLECTIONS_OBJS) $(RUNTIME_OBJS)
	@echo "✓ Stdlib archive created: build/libaether.a"

# Authoritative MANIFEST — list of link-suitable runtime + stdlib
# .c files for downstream consumers (aetherBuild and similar tools)
# that compile against the share/aether/ source tree. Generated
# from RUNTIME_SRC + STD_SRC so it stays in sync with what we
# actually link into libaether.a. Issue #329.
#
# Format: one path per non-comment, non-empty line. Paths are
# relative to share/aether/ in the install layout (and to the repo
# root in the source tree). Lines starting with `#` are comments.
build/MANIFEST: Makefile | $(BUILD_DIR)
	@echo "Generating MANIFEST..."
	@( \
	  echo "# Aether MANIFEST — link-suitable C source files for"; \
	  echo "# downstream consumers compiling against the runtime/std"; \
	  echo "# source tree (e.g. aetherBuild's aeb-link). Generated by"; \
	  echo "# the Makefile; do not edit by hand. Regenerated on every"; \
	  echo "# \`make stdlib\`."; \
	  echo "#"; \
	  echo "# Format: one path per non-comment, non-empty line. Paths"; \
	  echo "# are relative to this install root (share/aether/ on a"; \
	  echo "# system install; the repo root in the source tree)."; \
	  echo "# Lines starting with '#' are comments."; \
	  echo "#"; \
	  echo "# This file is the authoritative answer to 'which .c files"; \
	  echo "# do I compile to link against the Aether runtime?' — naive"; \
	  echo "# \`find runtime -name '*.c'\` walks pull in benchmarks /"; \
	  echo "# orphan poller hubs / etc. that aren't link-suitable."; \
	  echo ""; \
	  echo "# Runtime sources:"; \
	  for f in $(RUNTIME_SRC); do echo "$$f"; done; \
	  echo ""; \
	  echo "# Standard library sources:"; \
	  for f in $(STD_SRC); do echo "$$f"; done; \
	  echo ""; \
	  echo "# Collections sources:"; \
	  for f in $(COLLECTIONS_SRC); do echo "$$f"; done; \
	  echo ""; \
	  echo "# Reactor sources:"; \
	  for f in $(STD_REACTOR_SRC); do echo "$$f"; done; \
	) > build/MANIFEST
	@echo "✓ MANIFEST: $$(grep -c -v -E '^(#|$$)' build/MANIFEST) link-suitable files"
# Sandbox preload library (libaether_sandbox.so) — the LD_PRELOAD
# interception layer used by spawn_sandboxed for cross-process
# containment. Built on Linux and FreeBSD; both have an rtld that
# honours LD_PRELOAD + dlsym(RTLD_NEXT, ...). Link flags differ:
# glibc needs -ldl -lrt; FreeBSD folds dlopen/shm_open into libc.
# Other platforms (macOS hardened runtime, Windows) have no preload
# path — spawn_sandboxed there is a stub.
ifeq ($(shell uname -s),Linux)
	@echo "Building sandbox preload library..."
	@$(CC) -shared -fPIC -o build/libaether_sandbox.so runtime/libaether_sandbox_preload.c -ldl -lrt 2>/dev/null || true
	@test -f build/libaether_sandbox.so && echo "✓ Sandbox preload: build/libaether_sandbox.so" || echo "⚠ Sandbox preload: build failed (spawn_sandboxed will be unavailable)"
else ifeq ($(shell uname -s),FreeBSD)
	@echo "Building sandbox preload library..."
	@$(CC) -shared -fPIC -o build/libaether_sandbox.so runtime/libaether_sandbox_preload.c 2>/dev/null || true
	@test -f build/libaether_sandbox.so && echo "✓ Sandbox preload: build/libaether_sandbox.so" || echo "⚠ Sandbox preload: build failed (spawn_sandboxed will be unavailable)"
endif

# Compiler-as-library archive — COMPILER_LIB_SRC without aetherc.c's main().
# Consumed by the LSP server and the sanitizer test runners. Kept separate
# from libaether.a because user programs never need the compiler embedded.
build/libaether_compiler.a: $(COMPILER_LIB_OBJS)
	@echo "Creating compiler-as-library archive..."
	@ar rcs $@ $(COMPILER_LIB_OBJS)
	@echo "✓ Compiler-lib archive created: $@"

# Sanitizer / debug-flavoured archives.
#
# AddressSanitizer instrumentation only catches issues in code it touches
# at compile time — running asan-instrumented test code against an
# uninstrumented stdlib leaves any leak/UB in std/* invisible.
# Same logic for AETHER_MEMORY_TRACKING (the macro adds counters inside
# aether_memory.c that need to be defined when stdlib code calls into it)
# and for the -O0 -g shape that valgrind needs for legible traces.
#
# Building flavoured archives once per flavour lets each test runner
# link instantly while preserving full coverage. Pattern rules emit to
# `build/<flavour>-obj/...` so the default obj/ tree is untouched.

CFLAGS_NO_OPT := $(filter-out -O3 -O2 -O1 -O0,$(CFLAGS))
# Apple Clang on arm64 doesn't accept -fsanitize=leak (LeakSanitizer is
# bundled into ASan at runtime on Darwin). On Linux gcc/clang it must be
# requested explicitly. Platform-detect to keep both green.
ifeq ($(shell uname -s),Linux)
ASAN_FLAGS    := -fsanitize=address -fsanitize=leak -fno-omit-frame-pointer
else
ASAN_FLAGS    := -fsanitize=address -fno-omit-frame-pointer
endif
ASAN_OPT      := -O1 -g
MEM_FLAGS     := -DAETHER_MEMORY_TRACKING
DBG_OPT       := -O0 -g

ASAN_OBJ_DIR := build/asan-obj
MEM_OBJ_DIR  := build/memory-obj
DBG_OBJ_DIR  := build/dbg-obj
# Coverage variant — `gcc --coverage` is shorthand for
# `-fprofile-arcs -ftest-coverage` at compile and `-lgcov` at link.
# Produces .gcno files alongside .o (instrumentation) and .gcda
# files alongside the running binary (counters). gcov walks the .c
# (and follows #line directives — see PR #352) to produce both
# `<file>.c.gcov` and the `.ae.gcov` we actually want.
COV_FLAGS    := --coverage
COV_OPT      := -O0 -g
COV_OBJ_DIR  := build/cov-obj

ASAN_LIB_OBJS = $(STD_SRC:%.c=$(ASAN_OBJ_DIR)/%.o) \
                $(STD_REACTOR_SRC:%.c=$(ASAN_OBJ_DIR)/%.o) \
                $(COLLECTIONS_SRC:%.c=$(ASAN_OBJ_DIR)/%.o) \
                $(RUNTIME_SRC:%.c=$(ASAN_OBJ_DIR)/%.o)
ASAN_COMPILER_LIB_OBJS = $(COMPILER_LIB_SRC:%.c=$(ASAN_OBJ_DIR)/%.o)

MEM_LIB_OBJS = $(STD_SRC:%.c=$(MEM_OBJ_DIR)/%.o) \
               $(STD_REACTOR_SRC:%.c=$(MEM_OBJ_DIR)/%.o) \
               $(COLLECTIONS_SRC:%.c=$(MEM_OBJ_DIR)/%.o) \
               $(RUNTIME_SRC:%.c=$(MEM_OBJ_DIR)/%.o)
MEM_COMPILER_LIB_OBJS = $(COMPILER_LIB_SRC:%.c=$(MEM_OBJ_DIR)/%.o)

DBG_LIB_OBJS = $(STD_SRC:%.c=$(DBG_OBJ_DIR)/%.o) \
               $(STD_REACTOR_SRC:%.c=$(DBG_OBJ_DIR)/%.o) \
               $(COLLECTIONS_SRC:%.c=$(DBG_OBJ_DIR)/%.o) \
               $(RUNTIME_SRC:%.c=$(DBG_OBJ_DIR)/%.o)
DBG_COMPILER_LIB_OBJS = $(COMPILER_LIB_SRC:%.c=$(DBG_OBJ_DIR)/%.o)

COV_LIB_OBJS = $(STD_SRC:%.c=$(COV_OBJ_DIR)/%.o) \
               $(STD_REACTOR_SRC:%.c=$(COV_OBJ_DIR)/%.o) \
               $(COLLECTIONS_SRC:%.c=$(COV_OBJ_DIR)/%.o) \
               $(RUNTIME_SRC:%.c=$(COV_OBJ_DIR)/%.o)
COV_COMPILER_LIB_OBJS = $(COMPILER_LIB_SRC:%.c=$(COV_OBJ_DIR)/%.o)

$(ASAN_OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@$(CC) $(AETHER_REQUIRED_CFLAGS) $(CFLAGS_NO_OPT) $(ASAN_OPT) $(ASAN_FLAGS) -c $< -o $@

$(MEM_OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@$(CC) $(AETHER_REQUIRED_CFLAGS) $(CFLAGS) $(MEM_FLAGS) -c $< -o $@

$(DBG_OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@$(CC) $(AETHER_REQUIRED_CFLAGS) $(CFLAGS_NO_OPT) $(DBG_OPT) -c $< -o $@

$(COV_OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@$(CC) $(AETHER_REQUIRED_CFLAGS) $(CFLAGS_NO_OPT) $(COV_OPT) $(COV_FLAGS) -c $< -o $@

build/asan/libaether.a: $(ASAN_LIB_OBJS)
	@mkdir -p build/asan
	@ar rcs $@ $(ASAN_LIB_OBJS)

build/asan/libaether_compiler.a: $(ASAN_COMPILER_LIB_OBJS)
	@mkdir -p build/asan
	@ar rcs $@ $(ASAN_COMPILER_LIB_OBJS)

build/memory/libaether.a: $(MEM_LIB_OBJS)
	@mkdir -p build/memory
	@ar rcs $@ $(MEM_LIB_OBJS)

build/memory/libaether_compiler.a: $(MEM_COMPILER_LIB_OBJS)
	@mkdir -p build/memory
	@ar rcs $@ $(MEM_COMPILER_LIB_OBJS)

build/dbg/libaether.a: $(DBG_LIB_OBJS)
	@mkdir -p build/dbg
	@ar rcs $@ $(DBG_LIB_OBJS)

build/dbg/libaether_compiler.a: $(DBG_COMPILER_LIB_OBJS)
	@mkdir -p build/dbg
	@ar rcs $@ $(DBG_COMPILER_LIB_OBJS)

build/cov/libaether.a: $(COV_LIB_OBJS)
	@mkdir -p build/cov
	@ar rcs $@ $(COV_LIB_OBJS)

build/cov/libaether_compiler.a: $(COV_COMPILER_LIB_OBJS)
	@mkdir -p build/cov
	@ar rcs $@ $(COV_COMPILER_LIB_OBJS)

stdlib-asan: build/asan/libaether.a build/asan/libaether_compiler.a
stdlib-memory: build/memory/libaether.a build/memory/libaether_compiler.a
stdlib-dbg: build/dbg/libaether.a build/dbg/libaether_compiler.a
stdlib-cov: build/cov/libaether.a build/cov/libaether_compiler.a

# Self-test: compiler on itself
self-test: compiler
	@echo "==================================="
	@echo "Running Compiler Self-Test"
	@echo "==================================="
	@echo "Testing compiler on complex syntax..."
	@if [ -f examples/showcase/chat_server.ae ]; then \
		./build/aetherc$(EXE_EXT) examples/showcase/chat_server.ae build/test_compile.c && \
		echo "✓ Complex syntax compilation successful"; \
	fi
	@echo ""
	@echo "Testing collections..."
	@$(MAKE) --no-print-directory test > /dev/null && echo "✓ All tests passed"
	@echo ""
	@echo "==================================="
	@echo "Self-test complete"
	@echo "==================================="

# Parallel link-time optimization for the release build. GCC's plain `-flto`
# runs the LTRANS partitions SERIALLY (`lto-wrapper: using serial compilation of
# N LTRANS jobs`), so the single monolithic release link pins one core and looks
# frozen while the other cores idle. Use parallel LTO where the toolchain
# supports it, falling back to plain `-flto` on older GCC so this never
# regresses a build that worked before:
#   clang     -> ThinLTO (`-flto=thin`, inherently parallel)
#   GCC >= 10 -> `-flto=auto` (fans LTRANS across all CPUs)
#   older GCC -> `-flto` (unchanged, serial but correct)
# Deferred (`=`) so the probe runs only when the release/install target actually
# builds (never for `make test` etc.) and sees the final $(CC) (incl. CC=).
LTO_FLAG = $(shell \
	if $(CC) --version 2>/dev/null | grep -qi clang; then echo -flto=thin; \
	elif [ "$$($(CC) -dumpversion 2>/dev/null | cut -d. -f1)" -ge 10 ] 2>/dev/null; then echo -flto=auto; \
	else echo -flto; fi)

# Release build with optimizations and warnings as errors
release: clean $(STDLIB_SYMS_HEADER)
	@echo "==================================="
	@echo "Building Optimized Release"
	@echo "==================================="
	@$(MKDIR) build
	@echo "Compiling with -O3 -DNDEBUG $(LTO_FLAG) -Werror (parallel LTO; this can take a minute)..."
	@# -DAETHER_VERSION baked in from the same $(VERSION) the rest of
	@# the build uses (highest git tag → VERSION file fallback). Without
	@# this, compiler/aetherc.c falls back to the `v0.0.0-dev` sentinel,
	@# and since `make install` ships this exact binary as
	@# $(PREFIX)/bin/aetherc, every installed compiler reported the
	@# wrong version. The dev-build aetherc (compiled via the standard
	@# $(CFLAGS) pattern rule on line 133) has the flag and is correct;
	@# only the release-target hand-rolled gcc invocation was missing it.
	@$(CC) -O3 -DNDEBUG $(LTO_FLAG) -Werror -Icompiler -Iruntime -Istd -Istd/collections \
		-DAETHER_VERSION=\"$(VERSION)\" \
		$(COMPILER_SRC) $(STD_SRC) $(COLLECTIONS_SRC) runtime/aether_resource_caps.c \
		runtime/aether_locale_num.c \
		-o build/aetherc-release$(EXE_EXT) $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS)
ifeq ($(DETECTED_OS),Linux)
	@echo "Stripping debug symbols..."
	@strip build/aetherc-release$(EXE_EXT)
else ifeq ($(DETECTED_OS),Darwin)
	@echo "Stripping debug symbols..."
	@strip -x build/aetherc-release$(EXE_EXT)
endif
	@echo "✓ Release build complete: build/aetherc-release$(EXE_EXT)"
	@ls -lh build/aetherc-release$(EXE_EXT)

# Install to system
PREFIX ?= /usr/local
install: $(VERSION_HEADER) release ae stdlib
	@echo "==================================="
	@echo "Installing Aether to $(PREFIX)"
	@echo "==================================="
	@install -d $(PREFIX)/bin
	@# Install WITH $(EXE_EXT) on both sides. Dropping it on the
	@# destination (which this used to do) is fatal on Windows: `ae`
	@# looks for its compiler as "aetherc" EXE_EXT — i.e. aetherc.exe —
	@# next to itself, so an extensionless install leaves `ae` unable to
	@# find `aetherc` even though the two sit in the same directory.
	@# `ae --version` keeps working (it never needs the compiler), so the
	@# box reports a healthy toolchain while every compile fails with the
	@# misleading "Aether compiler not found ... set AETHER_HOME".
	@# EXE_EXT is empty on POSIX, so this is a no-op there.
	@install -m 755 build/ae$(EXE_EXT) $(PREFIX)/bin/ae$(EXE_EXT)
	@install -m 755 build/aetherc-release$(EXE_EXT) $(PREFIX)/bin/aetherc$(EXE_EXT)
	@install -d $(PREFIX)/lib/aether
	@install -m 644 build/libaether.a $(PREFIX)/lib/aether/
	@# Version stamp next to libaether.a. `ae build` reads this and
	@# compares it to the compiler's own version; a mismatch (the
	@# classic split where a stale `current` symlink shadows a fresh
	@# install) is reported up front instead of surfacing only as a
	@# cryptic `undefined reference to aether_*` at link time.
	@printf '%s\n' "$(VERSION)" > $(PREFIX)/lib/aether/VERSION
	@# Headers: walk the runtime/ and std/ trees and mirror every .h
	@# into $(PREFIX)/include/aether. Whole-tree copy (rather than the
	@# per-subdir enumeration this used to do) so new modules don't
	@# silently fall off the install — any module added under std/ or
	@# any new subdir under std/http/, std/collections/ etc. is
	@# captured automatically.
	@install -d $(PREFIX)/include/aether
	@# Sidecar VERSION next to the headers, mirroring lib/aether/VERSION and
	@# written by the SAME install step from the SAME $(VERSION), so the two
	@# cannot diverge. Language-agnostic: a build tool's include-root resolver
	@# (or a human) can read it with no compiler in the loop to detect a
	@# partial / shadowed install (the stale ~/.local/include/aether case).
	@printf '%s\n' "$(VERSION)" > $(PREFIX)/include/aether/VERSION
	@# The public embedder header lives in include/, outside the runtime/ and
	@# std/ trees the walks below cover, so it was never installed at all and
	@# runtime/libaether_caps.c could not find it in an install (#1420).
	@install -m 644 include/*.h $(PREFIX)/include/aether/ 2>/dev/null || true
	@cd runtime && find . -name '*.h' -print | while read h; do \
		install -d "$(PREFIX)/include/aether/runtime/$$(dirname $$h)"; \
		install -m 644 "$$h" "$(PREFIX)/include/aether/runtime/$$h"; \
	done
	@cd std && find . -name '*.h' -print | while read h; do \
		install -d "$(PREFIX)/include/aether/std/$$(dirname $$h)"; \
		install -m 644 "$$h" "$(PREFIX)/include/aether/std/$$h"; \
	done
	@# Sources + module.ae descriptors: same whole-tree copy. The
	@# compiler resolver looks for `share/aether/std/<mod>/module.ae`
	@# (compiler/aether_module.c:491) on every `import std.X`, so
	@# every module dir — including the Aether-only ones (file, dir,
	@# path, list, map, host, intarr, tcp) — needs its module.ae
	@# present here for the install to be functional.
	@install -d $(PREFIX)/share/aether
	@cp -R runtime $(PREFIX)/share/aether/
	@cp -R std     $(PREFIX)/share/aether/
	@# include/ too: share/aether/runtime/libaether_caps.c does
	@# `#include "../include/libaether.h"`, which resolves against
	@# share/aether/include/ — present in the source tree but, before this,
	@# absent from the install (headers go to include/aether/, a different
	@# root). Native builds add an extra -I that hides the gap; the cross
	@# path (which reuses tc.include_flags) does not, so a caps-runtime
	@# cross-build died on `'../include/libaether.h' file not found`. Mirror
	@# the source layout so the relative include resolves either way.
	@cp -R include $(PREFIX)/share/aether/
	@# Contrib module.ae descriptors + headers (issue #334). With these
	@# in place, `import contrib.X` resolves the same way `import std.X`
	@# does — share/aether/contrib/<X>/module.ae sits next to
	@# share/aether/std/<X>/module.ae and the resolver searches both
	@# (compiler/aether_module.c:436-484). The matching .a archives are
	@# still built+installed separately by `make install-contrib`, which
	@# probes for system dependencies (sqlite3-dev, etc.); without that
	@# step a link of `import contrib.sqlite` will fail loudly — fine,
	@# the resolver-side pain Paul filed is the one that's silent.
	@cp -R contrib $(PREFIX)/share/aether/
	@# Trim source-tree noise from the contrib install: tests, benchmarks,
	@# example .ae, build/CI scripts, and the .c/.m files (those compile
	@# into the libaether_<x>.a archives via `make contrib`; no value in
	@# also shipping the source). Mirrors the same trim install-contrib
	@# applies, kept here so `make install` alone yields a usable layout.
	@find $(PREFIX)/share/aether/contrib -type d -name tests       -exec rm -rf {} + 2>/dev/null || true
	@find $(PREFIX)/share/aether/contrib -type d -name benchmarks  -exec rm -rf {} + 2>/dev/null || true
	@find $(PREFIX)/share/aether/contrib -type f -name 'example_*.ae' -delete 2>/dev/null || true
	@find $(PREFIX)/share/aether/contrib -type f -name 'test_*.ae' -delete 2>/dev/null || true
	@# std module specs are co-located beside the modules they cover, so the
	@# install has to trim them the same way it trims contrib's — otherwise
	@# every user gets std/<mod>/test_<mod>.ae shipped with the toolchain.
	@find $(PREFIX)/share/aether/std -type f -name 'test_*.ae' -delete 2>/dev/null || true
	@find $(PREFIX)/share/aether/contrib -type f -name 'test_*.sh' -delete 2>/dev/null || true
	@find $(PREFIX)/share/aether/contrib -type f -name 'build.sh'  -delete 2>/dev/null || true
	@find $(PREFIX)/share/aether/contrib -type f -name 'ci.sh'     -delete 2>/dev/null || true
	@# Drop .c / .m noise EXCEPT the host bridges. `make install`
	@# (no install-contrib) does NOT ship libaether_host_<lang>.a, so
	@# downstream apps that `import contrib.host.<lang>` need the bridge
	@# source to compile in. Without this carve-out the resolver finds
	@# module.ae / .h but the link step has no `aether_host_<lang>_*`
	@# symbols available — broken installed tree.
	@find $(PREFIX)/share/aether/contrib -type f -name '*.c' \
		! -path '*/contrib/host/*/aether_host_*.c' -delete 2>/dev/null || true
	@find $(PREFIX)/share/aether/contrib -type f -name '*.m' -delete 2>/dev/null || true
	@# Trim install-noise that confuses external consumers (aetherBuild
	@# and the like). runtime/examples/ holds standalone benches with
	@# their own main(), never link-suitable, so a naive
	@# `find runtime -name '*.c'` consumer would try to link them. See
	@# new_nic_consideration.md for the broader "drop sources from
	@# install" question (Option C in aetherBuild's ask 3).
	@rm -rf $(PREFIX)/share/aether/runtime/examples
	@# Authoritative MANIFEST listing link-suitable .c files (#329).
	@# Downstream consumers (aetherBuild's aeb-link et al.) read this
	@# instead of guessing via `find runtime -name '*.c'` — the find
	@# would naively pull in benchmarks / orphan poller hubs / etc.
	@install -m 644 build/MANIFEST $(PREFIX)/share/aether/MANIFEST
	@# Restore ownership of build/ to the invoking user when running
	@# under sudo. Otherwise `sudo make install` re-runs the `release ae
	@# stdlib` build deps as root, leaving every object/archive/binary in
	@# build/ root-owned — which then makes the next plain `make` fail
	@# with "Permission denied" until the user manually chowns. SUDO_USER
	@# is set by sudo on every platform we build on; absent (no sudo) →
	@# this is a silent no-op. Failure (no chown perms, weird FS) → also
	@# a no-op so we never break the install summary.
	@if [ -n "$$SUDO_USER" ] && [ -d build ]; then \
		chown -R "$$SUDO_USER:$$(id -gn $$SUDO_USER 2>/dev/null || echo $$SUDO_USER)" build 2>/dev/null || true; \
	fi
	@# Prove the INSTALLED ae can actually reach its compiler, by
	@# compiling something. `ae version` is not sufficient: it never needs
	@# aetherc, so it reports a healthy toolchain even when `ae` cannot
	@# find the compiler at all — which is exactly how the Windows
	@# extensionless-install bug shipped, presenting as 57 red spec suites
	@# rather than a failed install. A trivial `ae build` exercises the
	@# lookup that actually matters.
	@# Non-fatal by design: a sandboxed or read-only TMPDIR must not fail
	@# an otherwise good install. It prints a loud warning instead.
	@tmpd="$${TMPDIR:-/tmp}/ae-install-check-$$$$"; \
	if mkdir -p "$$tmpd" 2>/dev/null; then \
	  printf 'main() {\n    println("ok")\n}\n' > "$$tmpd/smoke.ae"; \
	  if "$(PREFIX)/bin/ae$(EXE_EXT)" build "$$tmpd/smoke.ae" -o "$$tmpd/smoke$(EXE_EXT)" >"$$tmpd/out.txt" 2>&1; then \
	    echo "✓ Install check: the installed ae found aetherc and compiled a test program"; \
	  else \
	    echo "!! Install check FAILED: the installed ae could not compile."; \
	    echo "!! `ae version` may still work — it does not need the compiler."; \
	    echo "!! Check that $(PREFIX)/bin holds ae$(EXE_EXT) AND aetherc$(EXE_EXT):"; \
	    ls -l "$(PREFIX)/bin" 2>/dev/null | sed 's/^/!!   /'; \
	    sed 's/^/!!   /' "$$tmpd/out.txt" 2>/dev/null | head -12; \
	  fi; \
	  rm -rf "$$tmpd" 2>/dev/null || true; \
	fi
	@echo "✓ Installed successfully"
	@echo ""
	@echo "Run: ae version"
	@# Stale-`current`-symlink shadow check. `make install` writes the
	@# flat layout ($(PREFIX)/{bin,lib,include,share}/aether), but `ae`
	@# prefers $(PREFIX)/current/ whenever that symlink resolves to a
	@# tree with lib/aether or share/aether (tools/ae.c — the
	@# `ae version use` version-manager hook). A `current` left over
	@# from an older install or a past `ae version use` therefore
	@# silently shadows this fresh install: the dev rebuilds, installs,
	@# and `ae` keeps linking the stale toolchain. `ae` only warns when
	@# `current` is half-populated — a complete-but-stale tree looks
	@# valid and draws no diagnostic. Surface it loudly here so the
	@# `make && sudo make install` loop can't silently no-op. Warn
	@# only when `current` resolves to a real directory other than
	@# $(PREFIX) itself (a `current` -> $(PREFIX) symlink is harmless;
	@# a broken symlink doesn't shadow because `ae` stat-checks it).
	@cur=$$(cd "$(PREFIX)/current" 2>/dev/null && pwd -P); \
	pfx=$$(cd "$(PREFIX)" 2>/dev/null && pwd -P); \
	if [ -n "$$cur" ] && [ "$$cur" != "$$pfx" ]; then \
		echo ""; \
		echo "⚠  WARNING: $(PREFIX)/current shadows this install."; \
		echo "   $(PREFIX)/current -> $$(readlink "$(PREFIX)/current" 2>/dev/null || echo '?')"; \
		echo "   'ae' prefers current/ over the freshly-installed flat tree,"; \
		echo "   so it will keep using the stale toolchain above — not this build."; \
		echo "   Fix:  sudo rm $(PREFIX)/current      (use this flat install), or"; \
		echo "         ae version use <version>       (re-point the symlink)"; \
		echo ""; \
	fi

# -----------------------------------------------------------------
# contrib — build per-module static libs (libaether_<x>.a) for every
# contrib module whose system dependency is installed on this machine.
#
# contrib/ is a source-tree organising prefix; downstream consumes
# individual artifacts, not a bundled lib. Each module's bridge .c
# compiles to its own .a, named after the module — sqlite →
# libaether_sqlite.a, host/python → libaether_host_python.a, etc.
#
# Probe-and-build by design: machines without sqlite3-dev get no
# libaether_sqlite.a. The same module.ae still installs (so
# `import contrib.sqlite` resolves), but the link step fails loudly
# if the user actually imports a module whose .a wasn't built.
# Matches the per-build --with= capability-opt-in philosophy.
#
# Modules covered (v1):
#   - sqlite                    (-lsqlite3)
#   - host/{python,lua,perl,ruby,duktape,tcl,tinygo}
#   - host/factor               (dlopen libfactor; archive builds bare,
#                                runtime needs the factor-language fork)
#   - host/aether               (fork+exec sandboxed child via LD_PRELOAD;
#                                libc + in-tree sandbox runtime only)
#   - tinyweb                   (libc only — SHA-1 + base64 for WebSocket
#                                handshake; no third-party dep)
# Out of scope for v1: host/{java,go} (separate-process or JNI-style
# bridges, different build shape — javac/jar and cgo c-archive don't fit
# the cc→ar pipeline). The widget toolkit
# (formerly contrib/aether_ui) spun out to
# https://github.com/aether-lang-dev/aether-ui and is no longer in this repo.
# The Servirtium climate-API record/replay harness (formerly
# contrib/climate_http_tests) moved to the servirtium-vcr repo's
# integration/climate_interop, where the VCR tapes + replay tests live.
# -----------------------------------------------------------------
contrib:
	@bash tests/scripts/contrib_build.sh

# install-contrib — install the .a archives built by `make contrib`
# plus each module's module.ae and headers. Trims test/example noise
# the same way `install:` trims runtime/examples and runtime/io.
#
# Layout:
#   $(PREFIX)/lib/aether/libaether_<module>.a       — link target
#   $(PREFIX)/share/aether/contrib/<module>/module.ae — import resolver
#   $(PREFIX)/share/aether/contrib/<module>/*.h     — public headers
#
# Downstream aether.toml then references the module by name:
#   [build]
#   link_flags = "-laether_sqlite -lsqlite3"
install-contrib: contrib
	@echo "==================================="
	@echo "Installing contrib modules to $(PREFIX)"
	@echo "==================================="
	@if [ ! -f build/contrib/MANIFEST ]; then \
		echo "  No manifest — run 'make contrib' first."; exit 1; \
	fi
	@install -d $(PREFIX)/lib/aether
	@install -d $(PREFIX)/share/aether/contrib
	@# Install built archives. Manifest lines: <name> <path> (tab-separated).
	@awk -F'\t' 'NF>=2 { print $$1, $$2 }' build/contrib/MANIFEST | \
		while read -r name path; do \
			[ -z "$$name" ] && continue; \
			install -m 644 "$$path" "$(PREFIX)/lib/aether/libaether_$$name.a"; \
			printf "  lib/aether/libaether_%s.a\n" "$$name"; \
		done
	@# Mirror the contrib source tree for module.ae + headers.
	@# Trim noise: tests, benchmarks, example .ae, build scripts,
	@# CI scripts, and the modules we don't ship in v1
	@# (host/{java,go} — see contrib: target comment).
	@# host/{tinygo,factor,aether} and tinyweb ARE shipped.
	@cp -R contrib $(PREFIX)/share/aether/
	@rm -rf $(PREFIX)/share/aether/contrib/host/java
	@rm -rf $(PREFIX)/share/aether/contrib/host/go
	@find $(PREFIX)/share/aether/contrib -type d -name tests       -exec rm -rf {} + 2>/dev/null || true
	@find $(PREFIX)/share/aether/contrib -type d -name benchmarks  -exec rm -rf {} + 2>/dev/null || true
	@find $(PREFIX)/share/aether/contrib -type f -name 'example_*.ae' -delete 2>/dev/null || true
	@find $(PREFIX)/share/aether/contrib -type f -name 'test_*.ae' -delete 2>/dev/null || true
	@# std module specs are co-located beside the modules they cover, so the
	@# install has to trim them the same way it trims contrib's — otherwise
	@# every user gets std/<mod>/test_<mod>.ae shipped with the toolchain.
	@find $(PREFIX)/share/aether/std -type f -name 'test_*.ae' -delete 2>/dev/null || true
	@find $(PREFIX)/share/aether/contrib -type f -name 'test_*.sh' -delete 2>/dev/null || true
	@find $(PREFIX)/share/aether/contrib -type f -name 'build.sh'  -delete 2>/dev/null || true
	@find $(PREFIX)/share/aether/contrib -type f -name 'ci.sh'     -delete 2>/dev/null || true
	@# Drop the .c files now that they're compiled into .a — no
	@# reason to ship sources alongside the archive. Keep .h so
	@# downstream callers that #include the bridge header can.
	@find $(PREFIX)/share/aether/contrib -type f -name '*.c' -delete 2>/dev/null || true
	@find $(PREFIX)/share/aether/contrib -type f -name '*.m' -delete 2>/dev/null || true
	@# See the same SUDO_USER chown rationale on the `install:` target.
	@if [ -n "$$SUDO_USER" ] && [ -d build ]; then \
		chown -R "$$SUDO_USER:$$(id -gn $$SUDO_USER 2>/dev/null || echo $$SUDO_USER)" build 2>/dev/null || true; \
	fi
	@echo "✓ Contrib installed"

# Run an Aether program (compile + execute)
run: compiler
ifndef FILE
	@echo "Error: FILE not specified"
	@echo "Usage: make run FILE=examples/basic/hello_world.ae"
	@exit 1
endif
	@echo "Compiling $(FILE) to C..."
	@./build/aetherc$(EXE_EXT) $(FILE) build/output.c
	@echo "Building executable..."
	@$(CC) $(AETHER_REQUIRED_CFLAGS) $(CFLAGS) build/output.c $(RUNTIME_SRC) $(STD_SRC) $(STD_REACTOR_SRC) $(COLLECTIONS_SRC) -o build/output$(EXE_EXT) $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS)
	@echo "Running..."
	@./build/output$(EXE_EXT)

# Compile an Aether program to executable
compile: compiler
ifndef FILE
	@echo "make compile: missing required FILE argument."
	@echo ""
	@echo "Usage:"
	@echo "  make compile FILE=path/to/script.ae [OUTPUT=name]"
	@echo ""
	@echo "Example:"
	@echo "  make compile FILE=examples/basics/hello.ae"
	@echo "  make compile FILE=examples/ae-help-demo/fixed_dsl.ae OUTPUT=fixed_dsl"
	@echo ""
	@echo "If you wanted to build the toolchain instead, run:"
	@echo "  make ae         # just the CLI"
	@echo "  make compiler   # just the compiler"
	@echo "  make            # everything"
	@exit 1
endif
ifndef OUTPUT
	OUTPUT := $(basename $(notdir $(FILE)))
endif
	@echo "Compiling $(FILE) to C..."
	@./build/aetherc$(EXE_EXT) $(FILE) build/$(OUTPUT).c
	@echo "Building executable..."
	@$(CC) $(AETHER_REQUIRED_CFLAGS) $(CFLAGS) build/$(OUTPUT).c $(RUNTIME_SRC) $(STD_SRC) $(STD_REACTOR_SRC) $(COLLECTIONS_SRC) -o build/$(OUTPUT)$(EXE_EXT) $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS)
	@echo "✓ Built: build/$(OUTPUT)$(EXE_EXT)"

# Benchmark computed goto dispatch
bench-dispatch:
	@echo "Building computed goto benchmark..."
	@$(CC) -O3 experiments/concurrency/bench_computed_goto.c -o build/bench_computed_goto$(EXE_EXT) $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS)
	@echo "Running benchmark..."
	@./build/bench_computed_goto$(EXE_EXT)

# Benchmark manual prefetch hints
bench-prefetch:
	@echo "Building prefetch benchmark..."
	@$(CC) -O3 experiments/concurrency/bench_prefetch.c -o build/bench_prefetch$(EXE_EXT) $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS)
	@echo "Running benchmark..."
	@./build/bench_prefetch$(EXE_EXT)

# Profile-Guided Optimization (PGO) - train the compiler's inliner and
# branch-placement heuristics using a recorded workload. Run-time
# improvement is workload-dependent; measure before and after.
pgo-generate:
	@echo "==================================="
	@echo "PGO Step 1: Building with instrumentation..."
	@echo "==================================="
	@$(CC) -O3 -fprofile-generate experiments/concurrency/pgo_workload.c -o build/pgo_workload$(EXE_EXT) $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS)
	@echo "Running workload to collect profile data..."
	@./build/pgo_workload$(EXE_EXT)
	@echo "Profile data collected in *.gcda files"

pgo-build:
	@echo "==================================="
	@echo "PGO Step 2: Building with profile data..."
	@echo "==================================="
	@$(CC) -O3 -fprofile-use -D__PGO__ experiments/concurrency/bench_pgo.c -o build/bench_pgo_optimized$(EXE_EXT) $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS)
	@echo "PGO-optimized benchmark built"

pgo-baseline:
	@echo "Building baseline (no PGO)..."
	@$(CC) -O3 experiments/concurrency/bench_pgo.c -o build/bench_pgo_baseline$(EXE_EXT) $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS)
	@echo "Baseline benchmark built"

pgo-benchmark: pgo-baseline pgo-generate pgo-build
	@echo "==================================="
	@echo "PGO BENCHMARK COMPARISON"
	@echo "==================================="
	@echo ""
	@echo "Baseline (no PGO):"
	@./build/bench_pgo_baseline$(EXE_EXT)
	@echo ""
	@echo "-----------------------------------"
	@echo ""
	@echo "PGO-Optimized:"
	@./build/bench_pgo_optimized$(EXE_EXT)

pgo-clean:
	@echo "Cleaning PGO profile data..."
	@$(RM) *.gcda *.gcno 2>/dev/null || true
	@$(RM) build/pgo_workload$(EXE_EXT) build/bench_pgo_baseline$(EXE_EXT) build/bench_pgo_optimized$(EXE_EXT) 2>/dev/null || true
	@echo "✓ PGO data cleaned"

# Interactive REPL — integrated into ae CLI, no external dependencies
repl: ae
	@./build/ae$(EXE_EXT) repl

# Build statistics
stats:
	@echo "==================================="
	@echo "Build Statistics"
	@echo "==================================="
	@echo "Object files:        $$(find $(OBJ_DIR) -name '*.o' 2>/dev/null | wc -l)"
	@echo "Dependency files:    $$(find $(OBJ_DIR) -name '*.d' 2>/dev/null | wc -l)"
	@echo "Source files:"
	@echo "  Compiler:          $$(echo $(COMPILER_SRC) | wc -w)"
	@echo "  Runtime:           $$(echo $(RUNTIME_SRC) | wc -w)"
	@echo "  Stdlib:            $$(echo $(STD_SRC) $(COLLECTIONS_SRC) | wc -w)"
	@echo "  Tests:             $$(echo $(TEST_SRC) | wc -w)"
	@echo ""
	@echo "Lines of code:"
	@find compiler runtime std -name '*.c' -o -name '*.h' | xargs wc -l | tail -1
	@echo ""
	@if [ -d $(OBJ_DIR) ]; then \
		echo "Build directory size:"; \
		du -sh build 2>/dev/null || echo "N/A"; \
	fi
	@echo "==================================="

# Parallel test execution
test-parallel:
	@echo "==================================="
	@echo "Running Tests in Parallel"
	@echo "==================================="
	@echo "Testing by category..."
	@for cat in compiler runtime collections network memory stdlib parser; do \
		echo "  Testing $$cat..."; \
		./build/test_runner$(EXE_EXT) --category=$$cat & \
	done; \
	wait
	@echo ""
	@echo "All parallel tests complete!"

clean:
ifdef WINDOWS_NATIVE
	@if exist build $(RM_DIR) build
else
	$(RM_DIR) build
endif

help:
	@echo "Aether Build System ($(DETECTED_OS))"
	@echo ""
	@echo "Quick Start:"
	@echo "  make ae             - Build 'ae' CLI tool (recommended)"
	@echo "  ./build/ae run file.ae      - Run a program (Go-style)"
	@echo "  ./build/ae build file.ae    - Build executable"
	@echo ""
	@echo "Or use Make directly:"
	@echo "  make                - Build compiler"
	@echo "  make run FILE=...   - Compile and run an Aether program"
	@echo "  make compile FILE=...- Compile Aether program to executable"
	@echo "  make test           - Run test suite"
	@echo ""
	@echo "Build Targets:"
	@echo "  make compiler       - Build compiler (incremental)"
	@echo "  make compiler-fast  - Build compiler (monolithic, faster for clean)"
	@echo "  make -j8            - Parallel build with 8 jobs (faster on multi-core hosts)"
	@echo "  make release        - Optimized release build (-O3, parallel LTO)"
	@echo "  make stdlib         - Build precompiled stdlib archive"
	@echo ""
	@echo "Run Targets:"
	@echo "  make run FILE=path/to/file.ae    - Compile and execute program"
	@echo "  make compile FILE=file.ae        - Compile to executable"
	@echo "  make repl                        - Start interactive REPL"
	@echo ""
	@echo "Test Targets:"
	@echo "  make test           - Run C unit tests (incremental)"
	@echo "  make test-ae        - Run .ae integration tests"
	@echo "  make test-all       - Run both C and .ae tests"
	@echo "  make check-standalone - Compile every standalone C main (benches, demos)"
	@echo "  make test-fast      - Run C tests (monolithic build)"
	@echo "  make test-install   - Install smoke test (init + run)"
	@echo "  make test-valgrind  - Run tests with Valgrind"
	@echo "  make test-asan      - Run tests with AddressSanitizer"
	@echo "  make self-test      - Test compiler on complex examples"
	@echo ""
	@echo "CI/CD Targets:"
	@echo "  make ci             - Full CI suite (build + test + install smoke test)"
	@echo "  make docker-ci      - Run CI in Docker (with Valgrind)"
	@echo "  make docker-build-ci- Build Docker CI image"
	@echo "  make valgrind-check - Run Valgrind memory leak detection (Linux only)"
	@echo "  ./scripts/run-ci-local.sh - Full CI with Docker (recommended)"
	@echo ""
	@echo "Tool Targets:"
	@echo "  make lsp            - Build LSP server"
	@echo "  make apkg           - Build project tooling"
	@echo "  make profiler       - Build profiler dashboard"
	@echo "  make docgen         - Build documentation generator"
	@echo "  make docs           - Generate API documentation (in docs/api/)"
	@echo "  make docs-serve     - Serve docs at http://localhost:3000"
	@echo ""
	@echo "Web Servers (localhost):"
	@echo "  make docs-serve     - API Documentation    :3000"
	@echo "  make benchmark      - Benchmark Dashboard  :8080"
	@echo "  make profiler       - Profiler Dashboard   :8081"
	@echo ""
	@echo "Other Targets:"
	@echo "  make examples       - Compile example programs"
	@echo "  make install        - Install to $(PREFIX)"
	@echo "  make stats          - Show build statistics"
	@echo ""
	@echo "  make clean          - Remove build artifacts"
	@echo "  make help           - Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  make run FILE=examples/basic/hello_world.ae"
	@echo "  make compile FILE=myapp.ae OUTPUT=myapp"
	@echo "  make -j8 test       - Build and test with 8 parallel jobs"
	@echo ""
	@echo "Platform: $(DETECTED_OS)"
	@echo "Compiler: $(CC)"

test-build: $(TEST_OBJS) $(COMPILER_LIB_OBJS) $(RUNTIME_OBJS) $(STD_OBJS) $(STD_REACTOR_OBJS) $(COLLECTIONS_OBJS)
	@echo "Building test runner..."
	@$(CC) $(TEST_OBJS) $(COMPILER_LIB_OBJS) $(RUNTIME_OBJS) $(STD_OBJS) $(STD_REACTOR_OBJS) $(COLLECTIONS_OBJS) -o build/test_runner$(EXE_EXT) $(AETHER_REQUIRED_LDFLAGS) $(LDFLAGS)

# Docker CI/CD targets
docker-build-ci:
	@echo "Building Docker CI image..."
	docker build -f docker/Dockerfile.ci -t aether-ci:latest .

docker-ci: docker-build-ci
	@echo "Running full CI suite + Valgrind + ASan in Docker..."
	docker run --rm -v $(PWD):/aether -w /aether aether-ci bash -c "make ci && make valgrind-check && make asan-check"

# Cross-compile with MinGW (replicates Windows CI without needing a Windows host)
# Step 1: Build native aetherc, generate .c from all examples
# Step 2: Cross-compile compiler sources with MinGW -Werror
# Step 3: Syntax-check generated .c files with MinGW
ci-windows: clean compiler
	@echo "==================================="
	@echo "  Windows Cross-Compilation Test"
	@echo "==================================="
	@echo ""
	@echo "[1/3] Generating C from all examples with native aetherc..."
	@mkdir -p build/win
	@pass=0; fail=0; \
	for src in $$(find examples -name '*.ae' | grep -v '/lib/' | grep -v '/packages/' | grep -v '/embedded-java/' | grep -v '/host-.*-demo\.ae$$' | grep -v '/ae-help-demo/' | sort); do \
		name=$$(echo $$src | sed 's|examples/||;s|\.ae$$||'); \
		printf "  %-30s " "$$name"; \
		mkdir -p "build/win/examples/$$(dirname $$name)"; \
		out_c="build/win/examples/$$name.c"; \
		rm -f "$$out_c"; \
		if ./build/aetherc "$$src" "$$out_c" 2>/tmp/ae_err.txt && [ -f "$$out_c" ]; then \
			echo "OK"; \
			pass=$$((pass + 1)); \
		else \
			echo "FAIL"; \
			cat /tmp/ae_err.txt 2>/dev/null | head -5; \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	echo ""; \
	echo "  $$pass passed, $$fail failed"; \
	if [ "$$fail" -gt 0 ]; then exit 1; fi
	@echo ""
	@echo "[2/3] Cross-compiling compiler sources with MinGW -Werror..."
	@for f in $(COMPILER_LIB_SRC); do \
		printf "  %-50s " "$$f"; \
		if x86_64-w64-mingw32-gcc -O2 -Werror -c \
			-Icompiler -Iruntime -Iruntime/actors -Iruntime/scheduler \
			-Iruntime/utils -Iruntime/memory -Iruntime/config \
			-Istd -Istd/string -Istd/io -Istd/math -Istd/net -Istd/collections -Istd/json \
			-Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
			-DAETHER_VERSION=\"test\" \
			"$$f" -o /dev/null 2>/tmp/mingw_err.txt; then \
			echo "OK"; \
		else \
			echo "FAIL"; \
			cat /tmp/mingw_err.txt 2>/dev/null | head -10; \
			exit 1; \
		fi; \
	done
	@echo "  All compiler sources clean under MinGW -Werror"
	@echo ""
	@echo "[3/3] Syntax-checking generated C with MinGW..."
	@pass=0; fail=0; \
	for src in $$(find examples -name '*.ae' | grep -v '/lib/' | grep -v '/packages/' | grep -v '/embedded-java/' | grep -v '/host-.*-demo\.ae$$' | grep -v '/ae-help-demo/' | sort); do \
		name=$$(echo $$src | sed 's|examples/||;s|\.ae$$||'); \
		out_c="build/win/examples/$$name.c"; \
		printf "  %-30s " "$$name"; \
		if [ ! -f "$$out_c" ]; then \
			echo "SKIP"; \
		elif x86_64-w64-mingw32-gcc -O2 -fsyntax-only \
			-Iruntime -Iruntime/actors -Iruntime/scheduler \
			-Iruntime/utils -Iruntime/memory -Iruntime/config \
			-Istd -Istd/string -Istd/io -Istd/math -Istd/net -Istd/collections -Istd/json \
			-Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
			-Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-label \
			"$$out_c" 2>/tmp/mingw_err.txt; then \
			echo "OK"; \
			pass=$$((pass + 1)); \
		else \
			echo "FAIL"; \
			cat /tmp/mingw_err.txt 2>/dev/null | head -10; \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	echo ""; \
	echo "  $$pass passed, $$fail failed"; \
	if [ "$$fail" -gt 0 ]; then exit 1; fi
	@echo ""
	@echo "==================================="
	@echo "  Windows Cross-Compilation PASSED"
	@echo "==="

docker-ci-windows: docker-build-ci
	@echo "Running Windows cross-compilation tests in Docker..."
	docker run --rm -v $(PWD):/aether -w /aether aether-ci make ci-windows

ci: clean
	@echo "==================================="
	@echo "  Aether CI — Full Test Suite"
	@echo "  Parallel: $(NPROC) jobs (build) / $(NPROC) (.ae tests) / $${SH_NPROC:-1} (shell tests)"
	@echo "==================================="
	@echo ""
	@echo "[0/10] Restoring miniaudio object cache (if valid)..."
	@$(MAKE) audio-cache-restore
	@echo ""
	@echo "[1/10] Building compiler (-Werror)..."
	@$(MAKE) -j$(NPROC) compiler EXTRA_CFLAGS=-Werror
	@$(MAKE) audio-cache-save
	@echo ""
	@echo "[2/10] Building ae CLI..."
	@$(MAKE) -j$(NPROC) ae
	@echo ""
	@echo "[3/10] Building stdlib..."
	@$(MAKE) -j$(NPROC) stdlib
	@echo ""
	@echo "[4/10] Running C unit tests..."
	@$(MAKE) -j$(NPROC) test
	@$(MAKE) check-standalone
	@echo ""
	@echo "[5/10] Running .ae integration tests..."
	@$(MAKE) test-ae
	@echo ""
	@echo "[6/10] Building examples..."
	@$(MAKE) examples
	@echo ""
	@echo "[7/10] Install smoke test..."
	@$(MAKE) test-install
	@echo ""
	@echo "[8/10] ae test smoke check..."
	@AETHER_HOME="" ./build/ae test examples/basics/hello.ae 2>&1 | tail -1
	@echo "  [PASS] ae test runs correctly"
	@echo ""
	@echo "[9/10] Differential test (lowering paths agree)..."
	@$(MAKE) test-differential
	@echo ""
	@echo "[10/10] Release archive smoke test..."
	@$(MAKE) test-release-archive
	@echo ""
	@echo "==================================="
	@echo "  CI PASSED — all checks green"
	@echo "==================================="

# -----------------------------------------------------------------
# Differential testing (#523)
#
# Runs every tests/differential/cases/*.ae through both lowering paths
# (--emit=exe and --emit=lib) and compares stdout + exit code. Per-path
# correctness is weaker than cross-path agreement: a bug that miscompiles
# one path passes today, because that path's expected output was generated
# from the same build. See docs/differential-testing.md.
# -----------------------------------------------------------------
.PHONY: test-differential
test-differential: ae
	@sh tests/differential/run_differential.sh

# -----------------------------------------------------------------
# contrib/host bridge check
#
# Each contrib/host/<lang>/ directory ships a C bridge (and usually a
# module.ae) that embeds a foreign language runtime. Every bridge
# compiles in two modes:
#
#   - stub mode (no dev library available): always compiles clean
#     because the AETHER_HAS_<LANG> guard selects stub implementations.
#     Useful as a universal syntax check.
#
#   - linked mode (dev library + header available): probed via
#     pkg-config or a header-existence check. Compiles the bridge
#     with -DAETHER_HAS_<LANG> and tries to link a minimal demo.
#
# Goals:
#   - Fast ($0 marginal CI cost): always-on syntax sweep proves the
#     bridges compile on every build.
#   - Opportunistic: where dev libs are installed, link and run demos
#     end-to-end as a real integration check.
#   - No hard dependency: missing dev libs degrade gracefully to a
#     "skipped" status, never a failure.
# -----------------------------------------------------------------

# The in-process host bridges, syntax-checked (stub mode) by
# contrib-host-check. `js` builds as duktape; `factor`, `aether` and `racket`
# are dlopen / fork+exec bridges that still compile bare against libc + the
# in-tree sandbox runtime, so they belong in the syntax gate too. `rhombus`
# has no .c of its own (it shares the racket bridge — Rhombus is a #lang on
# the Racket VM), so it is not listed here (the check SKIPs missing-bridge
# dirs).
CONTRIB_HOST_LANGS = js lua perl python ruby tcl go tinygo factor aether racket

contrib-host-check: compiler ae stdlib
	@echo "==================================="
	@echo "  contrib/host bridge check"
	@echo "==================================="
	@echo ""
	@echo "[1/2] Syntax check — every bridge in stub mode..."
	@pass=0; fail=0; \
	for lang in $(CONTRIB_HOST_LANGS); do \
		src="contrib/host/$$lang/aether_host_$$lang.c"; \
		if [ ! -f "$$src" ]; then \
			printf "  %-10s SKIP (no bridge)\n" "$$lang"; \
			continue; \
		fi; \
		printf "  %-10s " "$$lang"; \
		if $(CC) -fsyntax-only -Wall -Wextra -I. "$$src" 2>/tmp/contrib_host_err.txt; then \
			echo "OK"; \
			pass=$$((pass + 1)); \
		else \
			echo "FAIL (stub mode)"; \
			head -5 /tmp/contrib_host_err.txt; \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	echo "  $$pass passed, $$fail failed (stub mode)"; \
	if [ "$$fail" -gt 0 ]; then exit 1; fi
	@echo ""
	@echo "[2/3] Link + run — demos for bridges with dev libs available..."
	@bash tests/scripts/contrib_host_demos.sh || exit 1
	@echo ""
	@echo "[3/3] Co-located bridge specs (contrib/host/*/test_*.ae)..."
	@rc=0; found=0; \
	for t in $$(find contrib/host -maxdepth 2 -name 'test_*.ae' 2>/dev/null | sort); do \
	  found=$$((found + 1)); \
	  lang=$$(basename $$(dirname "$$t")); \
	  echo "  --- $$lang ---"; \
	  case "$$lang" in \
	    ruby) command -v ruby >/dev/null 2>&1 && { \
	      AETHER_RUBY_SONAME="$$(ruby -rrbconfig -e 'print RbConfig::CONFIG["LIBRUBY_SO"]' 2>/dev/null)"; \
	      export AETHER_RUBY_SONAME; }; ;; \
	    python) command -v python3 >/dev/null 2>&1 && { \
	      AETHER_PYTHON_SONAME="$$(python3 -c 'import sysconfig,os; print(os.path.join(sysconfig.get_config_var("LIBDIR") or "", sysconfig.get_config_var("INSTSONAME") or ""))' 2>/dev/null)"; \
	      export AETHER_PYTHON_SONAME; }; ;; \
	    lua) for so in /usr/lib/*/liblua5.4.so /usr/lib/*/liblua5.3.so /usr/lib/liblua5.4.so; do \
	      [ -f "$$so" ] && { AETHER_LUA_SONAME="$$so"; export AETHER_LUA_SONAME; break; }; done; ;; \
	    perl) command -v perl >/dev/null 2>&1 && { \
	      AETHER_PERL_SONAME="$$(perl -MConfig -e 'print "$$Config{archlibexp}/CORE/$$Config{libperl}"' 2>/dev/null)"; \
	      export AETHER_PERL_SONAME; }; ;; \
	  esac; \
	  if [ ! -f "build/contrib/libaether_host_$$lang.a" ]; then \
	    MODULES="$$lang" bash tests/scripts/contrib_build.sh >/dev/null 2>&1 || true; \
	  fi; \
	  if [ ! -f "build/contrib/libaether_host_$$lang.a" ]; then \
	    echo "    SKIP: libaether_host_$$lang.a unavailable (bridge deps not installed)"; \
	    continue; \
	  fi; \
	  out="$$(./build/ae$(EXE_EXT) run "$$t" 2>&1)"; trc=$$?; \
	  echo "$$out" | sed 's/^/  /'; \
	  if [ "$$trc" -ne 0 ]; then rc=1; fi; \
	done; \
	if [ "$$found" -eq 0 ]; then echo "  (none yet)"; fi; \
	exit $$rc
	@echo ""
	@echo "==================================="
	@echo "  contrib/host check PASSED"
	@echo "==================================="

# aether_ui has been spun out into its own repo. The widget toolkit
# now lives at https://github.com/aether-lang-dev/aether-ui and is
# consumed as a downstream package, not built from this repo. The
# previous `contrib-aether-ui-check` and `benchmark-aether-ui` targets
# (along with their CI step) were removed when the spin-out happened.
# Anyone bisecting old failures: see the repo's git history pre-
# spin-out for the GTK4 / AppKit / Win32 backend build recipes.

valgrind-check: clean
	@echo "==================================="
	@echo "Running Valgrind Memory Check"
	@echo "==================================="
	@$(MAKE) compiler CFLAGS="-O0 -g"
	@$(MAKE) test-build CFLAGS="-O0 -g"
	@valgrind --leak-check=full \
		--show-leak-kinds=all \
		--track-origins=yes \
		--error-exitcode=1 \
		--suppressions=.valgrind-suppressions \
		./build/test_runner$(EXE_EXT) || (echo "Valgrind errors detected!" && exit 1)
	@echo "✓ Valgrind clean — no leaks or uninitialised reads"

asan-check: clean
	@echo "==================================="
	@echo "Running AddressSanitizer Check"
	@echo "==================================="
	@$(MAKE) compiler CFLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
	                  LDFLAGS="-fsanitize=address -pthread -lm"
	@$(MAKE) test-build CFLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
	                    LDFLAGS="-fsanitize=address -pthread -lm"
	@ASAN_OPTIONS=detect_leaks=1:check_initialization_order=1 \
	  ./build/test_runner$(EXE_EXT) 2>&1 | tee asan.log; \
	  if grep -q "ERROR: AddressSanitizer" asan.log; then \
	    echo "ERROR: AddressSanitizer detected errors!"; \
	    exit 1; \
	  fi
	@echo "✓ ASan clean — no memory errors detected"

.PHONY: all compiler lsp apkg ae profiler docgen docs-server docs docs-serve test test-build test-valgrind test-asan test-macos-leaks test-memory test-manual-runtime test-cross test-install test-release-archive benchmark benchmark-ui examples run compile repl clean help self-test install stats stdlib stdlib-asan stdlib-memory stdlib-dbg ci ci-windows docker-ci docker-ci-windows docker-build-ci valgrind-check asan-check ci-coop ci-wasm ci-embedded ci-portability docker-ci-wasm docker-ci-embedded contrib-host-check contrib install-contrib stdlib-cov ci-coverage ci-coverage-clean ci-coverage-html

# Cross-language benchmark UI (alias for benchmark)
benchmark-ui: benchmark

# ============================================================================
# Platform Portability CI
# ============================================================================

# Test cooperative scheduler on native (no Docker needed — fast)
ci-coop: clean compiler ae
	@echo "==================================="
	@echo "  Cooperative Scheduler CI"
	@echo "==================================="
	@echo ""
	@echo "[1/4] Building stdlib with -DAETHER_NO_THREADING..."
	@$(MAKE) stdlib EXTRA_CFLAGS="-DAETHER_NO_THREADING"
	@echo ""
	@echo "[2/4] Running actor tests in cooperative mode..."
	@pass=0; fail=0; \
	for src in tests/syntax/test_platform_caps.ae \
	           tests/syntax/test_coop_chain.ae \
	           tests/syntax/test_coop_many_actors.ae \
	           tests/syntax/test_coop_ask_reply.ae \
	           tests/syntax/test_coop_self_send.ae \
	           tests/syntax/test_coop_stubs.ae \
	           examples/actors/counter.ae \
	           examples/actors/ping-pong.ae \
	           examples/actors/ask-pattern.ae \
	           examples/actors/cooperative-demo.ae \
	           examples/basics/hello.ae; do \
		printf "  %-50s " "$$src"; \
		if AETHER_HOME="" ./build/ae run "$$src" >/tmp/ae_coop_out.txt 2>&1; then \
			echo "PASS"; \
			pass=$$((pass + 1)); \
		else \
			echo "FAIL"; \
			cat /tmp/ae_coop_out.txt | head -10; \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	echo ""; \
	echo "  $$pass passed, $$fail failed"; \
	if [ "$$fail" -gt 0 ]; then exit 1; fi
	@echo ""
	@echo "[3/4] Testing no-filesystem + no-networking stubs..."
	@$(MAKE) stdlib EXTRA_CFLAGS="-DAETHER_NO_FILESYSTEM -DAETHER_NO_NETWORKING"
	@printf "  %-50s " "hello.ae (no-fs/no-net)"; \
	if AETHER_HOME="" ./build/ae run examples/basics/hello.ae >/dev/null 2>&1; then \
		echo "PASS"; \
	else \
		echo "FAIL"; \
		exit 1; \
	fi
	@echo ""
	@echo "[4/4] Restoring default stdlib..."
	@$(MAKE) stdlib
	@echo ""
	@echo "==================================="
	@echo "  Cooperative Scheduler CI PASSED"
	@echo "==================================="

# WASM cross-compilation test (requires Emscripten — use Docker or local emsdk)
# Builds native aetherc, generates .c, then compiles with emcc
ci-wasm: clean compiler ae
	@echo "==================================="
	@echo "  WebAssembly (Emscripten) CI"
	@echo "==================================="
	@echo ""
	@echo "[1/3] Generating C from test programs..."
	@mkdir -p build/wasm
	@pass=0; fail=0; \
	for src in examples/basics/hello.ae \
	           examples/actors/counter.ae \
	           tests/syntax/test_platform_caps.ae \
	           tests/syntax/test_coop_chain.ae; do \
		name=$$(basename $$src .ae); \
		printf "  %-40s " "$$name → .c"; \
		if ./build/aetherc "$$src" "build/wasm/$$name.c" 2>/tmp/ae_wasm_err.txt; then \
			echo "OK"; \
			pass=$$((pass + 1)); \
		else \
			echo "FAIL"; \
			cat /tmp/ae_wasm_err.txt | head -5; \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	echo "  $$pass generated, $$fail failed"; \
	if [ "$$fail" -gt 0 ]; then exit 1; fi
	@echo ""
	@echo "[2/3] Compiling runtime + generated C with emcc..."
	@WASM_CFLAGS="-O2 -DAETHER_NO_THREADING -DAETHER_NO_FILESYSTEM -DAETHER_NO_NETWORKING \
		-Iruntime -Iruntime/actors -Iruntime/scheduler -Iruntime/utils -Iruntime/memory \
		-Iruntime/config -Istd -Istd/string -Istd/io -Istd/math -Istd/net -Istd/collections -Istd/json \
		-Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
		-Wno-unused-variable -Wno-missing-field-initializers -Wno-unused-label"; \
	pass=0; fail=0; \
	RUNTIME_FILES="runtime/scheduler/aether_scheduler_coop.c runtime/scheduler/scheduler_optimizations.c \
		runtime/config/aether_optimization_config.c runtime/memory/aether_arena.c \
		runtime/memory/aether_pool.c runtime/memory/aether_memory_stats.c \
		runtime/utils/aether_bounds_check.c runtime/utils/aether_test.c runtime/memory/aether_arena_optimized.c \
		runtime/aether_runtime_types.c runtime/aether_locale_num.c runtime/utils/aether_cpu_detect.c \
		runtime/utils/aether_simd_vectorized.c runtime/aether_runtime.c runtime/aether_numa.c \
		runtime/aether_host.c \
		runtime/actors/aether_send_buffer.c runtime/actors/aether_send_message.c \
		runtime/actors/aether_unwind.c \
		runtime/actors/aether_actor_thread.c \
		std/string/aether_string.c std/math/aether_math.c std/net/aether_http.c \
		std/net/aether_http_server.c std/net/aether_http_pool.c std/net/aether_net.c std/net/aether_actor_bridge.c \
		std/collections/aether_collections.c \
		std/json/aether_json.c std/fs/aether_fs.c std/log/aether_log.c std/io/aether_io.c \
		std/os/aether_os.c \
		std/collections/aether_set.c std/collections/aether_pqueue.c std/collections/aether_intarr.c std/collections/aether_longarr.c std/collections/aether_bits.c \
		runtime/sandbox/capsicum_autosandbox.c"; \
	for src in build/wasm/hello.c build/wasm/counter.c build/wasm/test_platform_caps.c \
	           build/wasm/test_coop_chain.c; do \
		name=$$(basename $$src .c); \
		printf "  %-40s " "emcc $$name"; \
		if emcc $$WASM_CFLAGS $$src $$RUNTIME_FILES \
			-o "build/wasm/$$name.js" -lm 2>/tmp/emcc_err.txt; then \
			echo "OK"; \
			pass=$$((pass + 1)); \
		else \
			echo "FAIL"; \
			cat /tmp/emcc_err.txt | head -10; \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	echo "  $$pass compiled, $$fail failed"; \
	if [ "$$fail" -gt 0 ]; then exit 1; fi
	@echo ""
	@echo "[3/3] Running WASM programs with Node.js..."
	@pass=0; fail=0; \
	for js in build/wasm/hello.js build/wasm/counter.js build/wasm/test_platform_caps.js \
	          build/wasm/test_coop_chain.js; do \
		name=$$(basename $$js .js); \
		printf "  %-40s " "node $$name"; \
		if node "$$js" >/tmp/wasm_out.txt 2>&1; then \
			echo "PASS"; \
			pass=$$((pass + 1)); \
		else \
			echo "FAIL"; \
			cat /tmp/wasm_out.txt | head -10; \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	echo "  $$pass passed, $$fail failed"; \
	if [ "$$fail" -gt 0 ]; then exit 1; fi
	@echo ""
	@echo "==================================="
	@echo "  WebAssembly CI PASSED"
	@echo "==================================="

# Embedded cross-compilation test (requires arm-none-eabi-gcc — use Docker or local install)
# Syntax-checks only — no runtime to execute on bare-metal
ci-embedded: clean compiler
	@echo "==================================="
	@echo "  Embedded (ARM) CI"
	@echo "==================================="
	@echo ""
	@echo "[1/2] Syntax-checking runtime sources with arm-none-eabi-gcc..."
	@EMB_CFLAGS="-fsyntax-only -O2 -mcpu=cortex-m4 -mthumb -ffreestanding \
		-DAETHER_NO_THREADING -DAETHER_NO_FILESYSTEM -DAETHER_NO_NETWORKING \
		-DAETHER_NO_GETENV -DAETHER_NO_SIMD -DAETHER_NO_AFFINITY -DAETHER_NO_NUMA \
		-Iruntime -Iruntime/actors -Iruntime/scheduler -Iruntime/utils -Iruntime/memory \
		-Iruntime/config -Istd -Istd/string -Istd/io -Istd/math -Istd/net -Istd/collections -Istd/json \
		-Wall -Wextra -Wno-unused-parameter -Wno-unused-function"; \
	pass=0; fail=0; \
	for f in runtime/scheduler/aether_scheduler_coop.c \
	         runtime/config/aether_optimization_config.c \
	         runtime/utils/aether_cpu_detect.c \
	         runtime/aether_numa.c \
	         runtime/actors/aether_send_message.c \
	         runtime/actors/aether_send_buffer.c \
	         runtime/aether_locale_num.c \
	         std/string/aether_string.c \
	         std/math/aether_math.c \
	         std/fs/aether_fs.c \
	         std/io/aether_io.c \
	         std/os/aether_os.c \
	         std/net/aether_http.c \
	         std/net/aether_net.c \
	         std/net/aether_http_server.c; do \
		printf "  %-55s " "$$f"; \
		if arm-none-eabi-gcc $$EMB_CFLAGS "$$f" 2>/tmp/emb_err.txt; then \
			echo "OK"; \
			pass=$$((pass + 1)); \
		else \
			echo "FAIL"; \
			cat /tmp/emb_err.txt | head -10; \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	echo ""; \
	echo "  $$pass passed, $$fail failed"; \
	if [ "$$fail" -gt 0 ]; then exit 1; fi
	@echo ""
	@echo "[2/2] Generating and syntax-checking example programs..."
	@mkdir -p build/embedded
	@pass=0; fail=0; \
	for src in examples/basics/hello.ae examples/actors/counter.ae; do \
		name=$$(basename $$src .ae); \
		printf "  %-55s " "$$name → syntax-check"; \
		./build/aetherc "$$src" "build/embedded/$$name.c" 2>/dev/null && \
		if arm-none-eabi-gcc -fsyntax-only -O2 -mcpu=cortex-m4 -mthumb -ffreestanding \
			-DAETHER_NO_THREADING -DAETHER_NO_FILESYSTEM -DAETHER_NO_NETWORKING \
			-DAETHER_NO_GETENV -DAETHER_NO_SIMD -DAETHER_NO_AFFINITY -DAETHER_NO_NUMA \
			-Iruntime -Iruntime/actors -Iruntime/scheduler -Iruntime/utils -Iruntime/memory \
			-Iruntime/config -Istd -Istd/string -Istd/io -Istd/math -Istd/net -Istd/collections -Istd/json \
			-Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
			-Wno-unused-variable -Wno-missing-field-initializers -Wno-unused-label \
			"build/embedded/$$name.c" 2>/tmp/emb_err.txt; then \
			echo "OK"; \
			pass=$$((pass + 1)); \
		else \
			echo "FAIL"; \
			cat /tmp/emb_err.txt | head -10; \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	echo "  $$pass passed, $$fail failed"; \
	if [ "$$fail" -gt 0 ]; then exit 1; fi
	@echo ""
	@echo "==================================="
	@echo "  Embedded CI PASSED"
	@echo "==================================="

# RISC-V 64-bit Linux cross-compile + run-under-qemu CI (issue #397).
#
# Cross-compiles aetherc, ae, and stdlib via riscv64-linux-gnu-gcc,
# then runs the C unit-test suite under qemu-riscv64-static. Catches
# portability bugs that an x86_64-only matrix can never surface —
# pointer-width / struct-padding / atomic-instruction-availability
# differences DO show up between x86_64 and riscv64.
#
# Why not the full `make test-ae`: that target invokes aetherc + ae
# (riscv64 binaries under qemu) which then invoke gcc to compile
# generated .c. Doable, but doubles CI time for limited extra
# coverage — most portability bugs surface in the C unit tests
# which we DO run end-to-end here. Adding test-ae is a follow-up if
# a riscv64-specific .ae integration bug ever escapes.
#
# Toolchain expected on the runner (apt install on ubuntu-22.04):
#   gcc-riscv64-linux-gnu     — host x86_64 binary, targets riscv64
#   libc6-dev-riscv64-cross   — riscv64 libc/headers
#   qemu-user-static          — qemu-riscv64-static binary
#
# Optional libs (OpenSSL, zlib, nghttp2, PCRE2, GTK4) are disabled for
# the riscv64 build — pkg-config on the host runner returns x86_64 lib
# paths that wouldn't link against riscv64 objects. Disabling them
# matches the std.* feature-detection pattern: the wrappers fall
# into their "unavailable" stubs cleanly. The portability check is
# about Aether's *own* C compiling for riscv64, not about
# revalidating every third-party lib.
ci-riscv64: clean
	@echo "==================================="
	@echo "  RISC-V 64 cross-CI"
	@echo "==================================="
	@echo ""
	@echo "[1/3] Cross-compiling compiler + ae + stdlib for riscv64..."
	# Variables passed as make COMMAND-LINE args (after `$(MAKE)`),
	# not as the leading-VAR=VAL environment-export form. Make's
	# command-line overrides beat the Makefile's `CC := gcc`
	# (immediate assignment); environment variables do NOT, so
	# the env-export form would silently fall through to the host
	# gcc and pass `-mabi=lp64d` to it, which it rejects.
	@$(MAKE) compiler ae stdlib \
	    CC=riscv64-linux-gnu-gcc \
	    EXTRA_CFLAGS="-march=rv64gc -mabi=lp64d" \
	    OPENSSL=0 ZLIB=0 NGHTTP2=0 PCRE2=0
	@echo ""
	@echo "[2/3] Verifying cross-built binaries are riscv64 ELF..."
	@file build/aetherc | grep -q "RISC-V" || { echo "  FAIL: aetherc not riscv64 ELF"; file build/aetherc; exit 1; }
	@file build/ae      | grep -q "RISC-V" || { echo "  FAIL: ae not riscv64 ELF"; file build/ae;      exit 1; }
	@echo "  build/aetherc and build/ae are riscv64 ELF — cross-compile worked"
	@echo ""
	@echo "[3/3] Smoke-running aetherc --version under qemu-riscv64-static..."
	@qemu-riscv64-static -L /usr/riscv64-linux-gnu ./build/aetherc --version || { \
	    echo "  FAIL: cross-built aetherc could not run under qemu"; exit 1; }
	@qemu-riscv64-static -L /usr/riscv64-linux-gnu ./build/ae --version 2>&1 | head -3 || { \
	    echo "  FAIL: cross-built ae could not run under qemu"; exit 1; }
	@echo "  cross-built binaries run cleanly under qemu-riscv64-static"
	@echo ""
	@echo "==================================="
	@echo "  RISC-V 64 cross-CI PASSED"
	@echo "==================================="

# Docker wrappers for cross-platform CI
docker-ci-wasm:
	@echo "Building WASM Docker image..."
	docker build -f docker/Dockerfile.wasm -t aether-wasm:latest .
	@echo "Running WASM CI in Docker..."
	docker run --rm -v $(PWD):/aether -w /aether aether-wasm make ci-wasm

docker-ci-embedded:
	@echo "Building embedded Docker image..."
	docker build -f docker/Dockerfile.embedded -t aether-embedded:latest .
	@echo "Running embedded CI in Docker..."
	docker run --rm -v $(PWD):/aether -w /aether aether-embedded make ci-embedded

# Run ALL portability checks (native coop + Docker WASM + Docker embedded)
ci-portability: ci-coop docker-ci-wasm docker-ci-embedded
	@echo ""
	@echo "==================================="
	@echo "  ALL PORTABILITY CHECKS PASSED"
	@echo "  - Cooperative scheduler (native)"
	@echo "  - WebAssembly (Emscripten)"
	@echo "  - Embedded (ARM Cortex-M4)"
	@echo "==================================="
