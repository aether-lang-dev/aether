#!/usr/bin/env bash
#
# Cross-compilation smoke test for `ae build --target=<triple>` (#1105).
#
# Verifies the zig cc backend: each supported target produces a binary of
# the right object format, and the two error paths (unknown target, a
# program importing an external-lib module) report cleanly. Cross builds
# for a foreign OS are not runnable on the build host, so this checks the
# emitted object format via file(1) rather than executing them; the two
# host-runnable targets are additionally executed.
#
# Requires zig on PATH. When zig is absent (e.g. CI without the zig
# toolchain) the whole script SKIPS with exit 0 so it never breaks a gate.
#
# Run with: make test-cross   (or: bash tests/scripts/cross_compile.sh)

set -u
cd "$(dirname "$0")/../.." || exit 1
ROOT="$(pwd)"
AE="$ROOT/build/ae"

pass=0; fail=0
ok()   { printf '  \033[0;32mOK\033[0m    %s\n' "$1"; pass=$((pass+1)); }
bad()  { printf '  \033[0;31mFAIL\033[0m  %s\n' "$1"; fail=$((fail+1)); }

if [ ! -x "$AE" ]; then
    echo "SKIP: build/ae not found; run 'make ae' first."
    exit 0
fi

# --- FreeBSD cross-LINK platform libs (asks/freebsd-cross-link-platform-libs.md)
# These run WITHOUT a real zig (they provide a stub `zig` that echoes its argv)
# so they execute even on hosts lacking the toolchain — hence before the zig
# SKIP guard below. They assert the emitted LINK COMMAND, not a produced binary:
#   casper libs ALWAYS (aether_casper.o is unconditionally in libaether.a and
#   calls cap_getpwnam/cap_sysctlbyname/…), openssl/nghttp2/zlib/pcre2 ONLY when
#   CROSSBUILD_SYSROOT is provided. A minimal fake base sysroot satisfies the
#   path checks.
_fbtmp="$(mktemp -d)"
STUB="$_fbtmp/stubzig"; mkdir -p "$STUB"
cat > "$STUB/zig" <<'STUBEOF'
#!/bin/sh
case "$1" in
  version) echo "0.16.0" ;;
  cc)  echo "ZIGCC: $*" >&2
       out=""; prev=""; for a in "$@"; do [ "$prev" = "-o" ] && out="$a"; prev="$a"; done
       [ -n "$out" ] && : > "$out"; exit 0 ;;
  ar)  shift; exec ar "$@" ;;
  *) exit 0 ;;
esac
STUBEOF
chmod +x "$STUB/zig"
FB="$_fbtmp/fbbase"; mkdir -p "$FB/usr/lib" "$FB/lib" "$FB/usr/include"
touch "$FB/usr/lib/crt1.o" "$FB/usr/lib/crti.o" "$FB/usr/lib/crtn.o" "$FB/lib/libc.so.7"
XB="$_fbtmp/xbuild"; mkdir -p "$XB/lib" "$XB/include"
# The Tier-2 append is per-lib-probed (ae.c links only libs actually staged),
# so the fake sysroot must carry the .a files for them to appear on the line.
for _l in ssl crypto nghttp2 z pcre2-8; do : > "$XB/lib/lib$_l.a"; done
_HELLO="examples/basics/hello.ae"

# (a) base sysroot only → casper libs present, openssl absent.
PATH="$STUB:$PATH" AETHER_SYSROOT="$FB" \
    "$AE" build --target=x86_64-freebsd "$_HELLO" -o "$_fbtmp/lk" 2>"$_fbtmp/zc" >/dev/null || true
_la="$(grep 'ZIGCC:' "$_fbtmp/zc" | grep -F 'libaether.a' | tail -1)"
if printf '%s' "$_la" | grep -q -- "-lcasper -lcap_pwd -lcap_sysctl -lcap_grp -lcap_dns"; then
    ok "x86_64-freebsd link appends casper libs (always)"
else
    bad "x86_64-freebsd link missing casper libs"; echo "        $_la"
fi
# libthr.so.3 (POSIX threads) must be linked BY PATH, next to libc.so.7 — the
# runtime/scheduler + std.http server call pthread_create, and -lthr/-lpthread
# does NOT resolve under zig-lld + -nostdlib against the split base sysroot.
if printf '%s' "$_la" | grep -q -- "/lib/libthr.so.3"; then
    ok "x86_64-freebsd link includes libthr.so.3 by path"
else
    bad "x86_64-freebsd link missing libthr.so.3 (pthread_create would be undefined)"; echo "        $_la"
fi
if printf '%s' "$_la" | grep -q -- "-lssl"; then
    bad "x86_64-freebsd link added openssl WITHOUT CROSSBUILD_SYSROOT"
else
    ok "x86_64-freebsd link omits openssl without CROSSBUILD_SYSROOT"
fi

# (b) with CROSSBUILD_SYSROOT → casper + openssl/nghttp2/zlib/pcre2.
PATH="$STUB:$PATH" AETHER_SYSROOT="$FB" CROSSBUILD_SYSROOT="$XB" \
    "$AE" build --target=x86_64-freebsd "$_HELLO" -o "$_fbtmp/lk2" 2>"$_fbtmp/zc2" >/dev/null || true
_lb="$(grep 'ZIGCC:' "$_fbtmp/zc2" | grep -F 'libaether.a' | tail -1)"
if printf '%s' "$_lb" | grep -q -- "-lcasper" && \
   printf '%s' "$_lb" | grep -q -- "-lssl -lcrypto -lnghttp2 -lz -lpcre2-8"; then
    ok "x86_64-freebsd link adds Tier-2 libs with CROSSBUILD_SYSROOT"
else
    bad "x86_64-freebsd link missing Tier-2 libs under CROSSBUILD_SYSROOT"; echo "        $_lb"
fi

# --- Windows (Tier A, self-contained — no sysroot). Same stub zig. ---
# (c) recognized + emits .exe + NO casper (casper is FreeBSD-only) + NO Tier-2
#     libs without a sysroot.
PATH="$STUB:$PATH" "$AE" build --target=x86_64-windows "$_HELLO" -o "$_fbtmp/w" 2>"$_fbtmp/wc" >/dev/null || true
if grep -q "Unknown target" "$_fbtmp/wc"; then
    bad "x86_64-windows: reported Unknown target"
else
    ok "x86_64-windows recognized as a valid target"
fi
_lw="$(grep 'ZIGCC:' "$_fbtmp/wc" | grep -F 'libaether.a' | tail -1)"
if printf '%s' "$_lw" | grep -q -- "-lcasper"; then
    bad "x86_64-windows link added casper (FreeBSD-only)"
else
    ok "x86_64-windows link omits casper"
fi
if printf '%s' "$_lw" | grep -qE -- "-o [^ ]*\.exe"; then
    ok "x86_64-windows output named .exe"
else
    bad "x86_64-windows output not .exe: $_lw"
fi
# Windows system libs — ALWAYS: zig bundles the mingw CRT but not these -l
# names. The runtime + static openssl reference them (BCryptGenRandom needs
# bcrypt; winsock needs ws2_32; etc.), so the cross link must append them or a
# real socket/RNG program fails to link. Same always-on shape as freebsd casper.
if printf '%s' "$_lw" | grep -q -- "-lws2_32" && \
   printf '%s' "$_lw" | grep -q -- "-lbcrypt"; then
    ok "x86_64-windows link appends Windows system libs (ws2_32/bcrypt/...)"
else
    bad "x86_64-windows link missing Windows system libs: $_lw"
fi
# (d) windows WITH CROSSBUILD_SYSROOT → Tier-2 libs (per-lib probed), no casper.
PATH="$STUB:$PATH" CROSSBUILD_SYSROOT="$XB" \
    "$AE" build --target=x86_64-windows "$_HELLO" -o "$_fbtmp/w2" 2>"$_fbtmp/wc2" >/dev/null || true
_lw2="$(grep 'ZIGCC:' "$_fbtmp/wc2" | grep -F 'libaether.a' | tail -1)"
if printf '%s' "$_lw2" | grep -q -- "-lssl -lcrypto" && \
   printf '%s' "$_lw2" | grep -q -- "-lpcre2-8" && \
   ! printf '%s' "$_lw2" | grep -q -- "-lcasper"; then
    ok "x86_64-windows link adds Tier-2 libs (no casper) with CROSSBUILD_SYSROOT"
else
    bad "x86_64-windows Tier-2 wiring wrong: $_lw2"
fi
# (e) per-lib probe: a sysroot with ONLY pcre2 links -lpcre2-8 but NOT -lssl.
XP="$_fbtmp/xpart"; mkdir -p "$XP/lib"; : > "$XP/lib/libpcre2-8.a"
PATH="$STUB:$PATH" CROSSBUILD_SYSROOT="$XP" \
    "$AE" build --target=x86_64-windows "$_HELLO" -o "$_fbtmp/w3" 2>"$_fbtmp/wc3" >/dev/null || true
_lw3="$(grep 'ZIGCC:' "$_fbtmp/wc3" | grep -F 'libaether.a' | tail -1)"
if printf '%s' "$_lw3" | grep -q -- "-lpcre2-8" && ! printf '%s' "$_lw3" | grep -q -- "-lssl"; then
    ok "CROSSBUILD_SYSROOT per-lib probe links only staged libs"
else
    bad "per-lib probe wrong (linked an absent lib): $_lw3"
fi
rm -rf "$_fbtmp"

if ! command -v zig >/dev/null 2>&1; then
    echo "SKIP: rest of cross-compilation test requires a real zig on PATH."
    echo ""
    echo "  $pass passed, $fail failed"
    [ "$fail" -eq 0 ]
    exit $?
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
HELLO="examples/basics/hello.ae"
ACTOR="examples/actors/counter.ae"
HTTP="examples/actors/site-poller.ae"

echo "Cross-compilation targets ($(zig version)):"

# target triple | file(1) substring that must appear | runnable-here flag
check_target() {
    local triple="$1" want="$2" run="$3" src="$4"
    local out="$TMP/out_${triple}"
    if ! "$AE" build --target="$triple" "$src" -o "$out" >/dev/null 2>"$TMP/err"; then
        bad "$triple: build failed"; sed 's/^/        /' "$TMP/err"; return
    fi
    local desc; desc="$(file -b "$out")"
    if ! printf '%s' "$desc" | grep -q "$want"; then
        bad "$triple: expected '$want', got '$desc'"; return
    fi
    if [ "$run" = "run" ]; then
        if ! "$out" >/dev/null 2>&1; then bad "$triple: built but did not run"; return; fi
        ok "$triple (built + ran): $desc"
    else
        ok "$triple: $desc"
    fi
}

# arch/OS format checks. macos targets run on this host if it is macOS.
host_os="$(uname -s)"
mac_run="nofile"; [ "$host_os" = "Darwin" ] && mac_run="run"
check_target "x86_64-linux"   "ELF 64-bit"          "nofile"   "$HELLO"
check_target "aarch64-linux"  "ELF 64-bit"          "nofile"   "$HELLO"
check_target "aarch64-macos"  "Mach-O 64-bit"       "$mac_run" "$ACTOR"
check_target "x86_64-macos"   "Mach-O 64-bit"       "$mac_run" "$ACTOR"

# Regression: a program defining a top-level function named like an
# unreferenced runtime global (e.g. `describe` in aether_host.c) must
# cross-build, exactly as it builds natively. Guards the archive-based
# link that gives on-demand object pulling. sum-types.ae defines `describe`.
if [ -f "examples/basics/sum-types.ae" ]; then
    if "$AE" build --target=x86_64-linux examples/basics/sum-types.ae -o "$TMP/st" \
         >/dev/null 2>"$TMP/err"; then
        ok "symbol-collision program (defines 'describe') cross-builds"
    else
        bad "symbol-collision program failed to cross-build"; sed 's/^/        /' "$TMP/err"
    fi
fi

# Error path: --emit=lib with a cross target is rejected up front.
if "$AE" build --target=x86_64-linux --emit=lib "$HELLO" -o "$TMP/xl" >/dev/null 2>"$TMP/err"; then
    bad "--emit=lib cross: build unexpectedly succeeded"
elif grep -q "executables only" "$TMP/err"; then
    ok "--emit=lib cross rejected"
else
    bad "--emit=lib cross: wrong error"; sed 's/^/        /' "$TMP/err"
fi

# Error path: unknown target (no zig needed, but exercised here anyway).
if "$AE" build --target=sparc-solaris "$HELLO" -o "$TMP/x" >/dev/null 2>"$TMP/err"; then
    bad "unknown target: build unexpectedly succeeded"
elif grep -q "Unknown target" "$TMP/err"; then
    ok "unknown target rejected"
else
    bad "unknown target: wrong error"; sed 's/^/        /' "$TMP/err"
fi

# FreeBSD (Tier B): the triple is RECOGNIZED (not "Unknown target") — this
# guards the target-list wiring even where zig is absent. A freebsd target
# needs a base sysroot zig cc doesn't bundle, so without AETHER_SYSROOT it
# must report the guided sysroot error, NOT "Unknown target".
env -u AETHER_SYSROOT "$AE" build --target=x86_64-freebsd "$HELLO" -o "$TMP/fb" \
    >/dev/null 2>"$TMP/err"
if grep -q "Unknown target" "$TMP/err"; then
    bad "x86_64-freebsd: reported Unknown target (target-list wiring missing)"
else
    ok "x86_64-freebsd recognized as a valid target"
fi
# The sysroot-unset guided error lives behind the zig-presence gate (it's
# raised inside run_cross_build), so only assert it when zig is available —
# without zig the build errors on zig-not-found first, which is also correct.
if env -u AETHER_SYSROOT "$AE" build --target=x86_64-freebsd "$HELLO" -o "$TMP/fb" \
     >/dev/null 2>"$TMP/err"; then
    bad "x86_64-freebsd without AETHER_SYSROOT: build unexpectedly succeeded"
elif grep -q "needs a FreeBSD base sysroot" "$TMP/err"; then
    ok "x86_64-freebsd without AETHER_SYSROOT: guided sysroot error"
elif grep -q "zig not found" "$TMP/err"; then
    # Cannot reach the sysroot check without zig — the SKIP guard at the top
    # means this branch is unreachable here, but keep it explicit for clarity.
    ok "x86_64-freebsd without AETHER_SYSROOT: zig absent (sysroot check gated behind zig)"
else
    bad "x86_64-freebsd without AETHER_SYSROOT: wrong error"; sed 's/^/        /' "$TMP/err"
fi

# If a real FreeBSD base sysroot is available (AETHER_SYSROOT points at one),
# build for real and assert a FreeBSD ELF. Skipped in ordinary CI, which has
# no sysroot; exercised on a box provisioned via aether-crossbuild.
if [ -n "${AETHER_SYSROOT:-}" ] && [ -f "${AETHER_SYSROOT}/lib/libc.so.7" ]; then
    if "$AE" build --target=x86_64-freebsd "$HELLO" -o "$TMP/fbsd" \
         >/dev/null 2>"$TMP/err"; then
        desc="$(file -b "$TMP/fbsd")"
        if printf '%s' "$desc" | grep -qi "FreeBSD"; then
            ok "x86_64-freebsd (real sysroot): $desc"
        else
            bad "x86_64-freebsd (real sysroot): expected FreeBSD ELF, got '$desc'"
        fi
    else
        bad "x86_64-freebsd (real sysroot): build failed"; sed 's/^/        /' "$TMP/err"
    fi
else
    echo "  SKIP  x86_64-freebsd real build (no AETHER_SYSROOT base sysroot present)"
fi

# A program using a library-backed module (std.http) still cross-builds,
# with a note that those features report unavailable at runtime (matching a
# native build without OpenSSL/zlib/nghttp2/PCRE2).
if [ -f "$HTTP" ]; then
    if "$AE" build --target=x86_64-linux "$HTTP" -o "$TMP/xhttp" >/dev/null 2>"$TMP/err" \
         && [ -f "$TMP/xhttp" ]; then
        if grep -q "built without OpenSSL" "$TMP/err"; then
            ok "library-backed program cross-builds with an availability note"
        else
            bad "library-backed program built but emitted no note"; sed 's/^/        /' "$TMP/err"
        fi
    else
        bad "library-backed program failed to cross-build"; sed 's/^/        /' "$TMP/err"
    fi
fi

echo ""
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
