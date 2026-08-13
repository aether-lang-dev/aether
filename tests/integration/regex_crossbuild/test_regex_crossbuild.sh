#!/bin/sh
# Regression (#1389): `ae build --target ...` of a std.regex program works
# with NO CROSSBUILD_SYSROOT and no system libpcre2-8 for the target — the
# vendored engine (std/regex/pcre2/, compiled by aether_pcre2_vendored.c)
# supplies it for every zig-bundled-libc target.
#
# Before #1389 this build "succeeded" but regex.compile failed at runtime
# with "built without libpcre2-8" — the silent degradation that cost the
# aether-ui line 8 rounds of misdiagnosis (SVG paths drew nothing because a
# regex three layers down was stubbed). Hence the probe pattern here IS that
# SVG path-data tokenizer, and the assertions are:
#
#   1. the cross build succeeds with the sysroot env var explicitly unset;
#   2. no "uses std.regex ... unavailable" warning is emitted (std.regex
#      left the warn list because it is now always available);
#   3. when the target matches this host, the binary RUNS and the regex
#      matches (tokens=6) — absence of the stub, not just of the warning.
#
# zig is the cross backend; SKIP when absent (CONTRIBUTING.md portability
# pattern #2).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

if ! command -v zig >/dev/null 2>&1; then
    echo "  [PASS] regex_crossbuild: SKIP (zig not installed)"
    exit 0
fi

# Pick the cross target that matches this host so the binary can be RUN.
# Cross targets only cover linux/macos (zig-bundled libc + supported arch);
# elsewhere fall back to x86_64-linux as a build-only check.
ARCH="$(uname -m)"; OS="$(uname -s)"
RUNNABLE=1
case "$OS-$ARCH" in
    Linux-x86_64)   TARGET=x86_64-linux ;;
    Linux-aarch64)  TARGET=aarch64-linux ;;
    Darwin-x86_64)  TARGET=x86_64-macos ;;
    Darwin-arm64)   TARGET=aarch64-macos ;;
    *)              TARGET=x86_64-linux; RUNNABLE=0 ;;
esac

fail() {
    echo "  [FAIL] regex_crossbuild: $1"
    shift
    for f in "$@"; do
        [ -f "$f" ] && sed 's/^/    /' "$f" | tail -20
    done
    exit 1
}

# env -u: the assertion is "no sysroot needed", so make sure a developer's
# exported CROSSBUILD_SYSROOT can't quietly satisfy the link instead.
if ! ( cd "$TMPDIR" && env -u CROSSBUILD_SYSROOT "$ROOT/build/ae" build \
        "$SCRIPT_DIR/regex_probe.ae" --target "$TARGET" -o "$TMPDIR/probe" \
        >"$TMPDIR/build.log" 2>&1 ); then
    fail "cross build for $TARGET failed without a sysroot" "$TMPDIR/build.log"
fi

[ -f "$TMPDIR/probe" ] || fail "build produced no binary" "$TMPDIR/build.log"

# std.regex must no longer trip the unsupported-module warning.
if grep -q "uses std.regex" "$TMPDIR/build.log"; then
    fail "std.regex still warned as unavailable on cross builds" "$TMPDIR/build.log"
fi

if [ "$RUNNABLE" = "1" ]; then
    if ! "$TMPDIR/probe" >"$TMPDIR/run.log" 2>&1; then
        fail "cross-built probe failed at runtime (stubbed regex?)" "$TMPDIR/run.log"
    fi
    grep -q "tokens=6" "$TMPDIR/run.log" \
        || fail "probe output wrong" "$TMPDIR/run.log"
    echo "  [PASS] regex_crossbuild: $TARGET built without sysroot and ran (tokens=6)"
else
    echo "  [PASS] regex_crossbuild: $TARGET built without sysroot (host cannot run it)"
fi
