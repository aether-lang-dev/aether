#!/bin/sh
# Regression: a cross `ae build --emit=lib --target=<triple> --lib <dir>` of a
# program importing a bare-name module found ONLY through the `--lib` search dir
# must NOT print a spurious `unresolved import`, and must build.
#
# The cross path runs a diagnostic prepass (cross_uses_unsupported_module ->
# aetherc --emit=inspect) to warn about library-backed stdlib modules that a
# sysroot-less cross link stubs out. That prepass was invoked WITHOUT the
# caller's `--lib` dirs, so a bare-name `--lib`-backed import (e.g. selaenium's
# `resolve`, `browser`, `cft` under `--lib drivermgr`) came back unresolved and
# printed `error: unresolved import 'resolve'` to stderr — even though the REAL
# compile, which does pass `--lib`, resolved it and the build succeeded. On a
# FreeBSD cross (which then failed to LINK for an unrelated sysroot-header
# reason) the two errors read as one story and looked like `--lib` being dropped
# for freebsd. The prepass now forwards the same `--lib` search path as the real
# compile (tools/ae.c: tc_lib_flags / aetherc_capture_stdout).
#
# Asserts, on x86_64-linux (a Tier-A self-contained zig cross, no sysroot):
#   - the build succeeds and produces the .so
#   - stderr carries NO `unresolved import` line

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if ! command -v zig >/dev/null 2>&1; then
    echo "  [SKIP] cross_lib_no_spurious_unresolved: zig not on PATH"
    exit 0
fi

TMPDIR_T="$(mktemp -d)"
cleanup() { rm -rf "$TMPDIR_T"; }
trap cleanup EXIT INT TERM

# A clean module cache so a prior run's artifact can't mask a prepass regression.
rm -rf "$HOME/.aether/cache" 2>/dev/null || true

log="$TMPDIR_T/build.log"
if ! (cd "$SCRIPT_DIR" && "$AE" build --emit=lib --target=x86_64-linux \
        --lib mylib embed.ae -o "$TMPDIR_T/out.so") > "$log" 2>&1; then
    echo "  [FAIL] cross_lib_no_spurious_unresolved: cross --emit=lib with --lib did not build"
    sed 's/^/    /' "$log" | head -15
    exit 1
fi

if [ ! -f "$TMPDIR_T/out.so" ]; then
    echo "  [FAIL] cross_lib_no_spurious_unresolved: no out.so produced"
    exit 1
fi

if grep -q "unresolved import" "$log"; then
    echo "  [FAIL] cross_lib_no_spurious_unresolved: spurious 'unresolved import' on a --lib-backed cross build"
    grep "unresolved import" "$log" | sed 's/^/    /' | head -3
    echo "         (the inspect prepass ran without --lib — the FreeBSD red herring)"
    exit 1
fi

echo "  [PASS] cross --lib-backed import resolves in the prepass; no spurious unresolved-import"
echo "PASS: cross_lib_no_spurious_unresolved"
exit 0
