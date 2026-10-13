#!/bin/sh
# #2889: closures a `try` body binds before a panic are what the local
# holds after the catch, on the path without statement expressions.
#
# There (AETHER_GCC_COMPAT 0) a capturing closure literal was a call that
# returned the closure struct through a hidden slot, and gcc at -O1 and -O2
# inlined it into the volatile local the `try` body writes and dropped the
# store. The .ae sweep builds the regression program on the path its C
# compiler picks, which is the GCC one under gcc and clang; this builds it
# on the other path, at the default -O2, and runs it.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -f "$AE.exe" ] && AE="$AE.exe"
CC_BIN="${CC:-cc}"
command -v "$CC_BIN" >/dev/null 2>&1 || CC_BIN=gcc
SRC="$ROOT/tests/regression/test_issue2889_closure_rebound_in_try.ae"

if [ ! -f "$AE" ]; then
    echo "  [SKIP] closure_rebound_in_try_nogcc: toolchain not built"
    exit 0
fi
if ! command -v "$CC_BIN" >/dev/null 2>&1; then
    echo "  [SKIP] closure_rebound_in_try_nogcc: no C compiler"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

fail() {
    echo "  [FAIL] closure_rebound_in_try_nogcc: $1"
    exit 1
}

[ -f "$SRC" ] || fail "$SRC is missing"
if ! AE_CC="$CC_BIN -DAETHER_GCC_COMPAT=0" "$AE" build "$SRC" -o "$tmp/main" \
        >"$tmp/build.log" 2>&1; then
    sed 's/^/    /' "$tmp/build.log" | head -20
    fail "ae build failed with AETHER_GCC_COMPAT=0"
fi
out="$("$tmp/main" 2>&1 | tr -d '\r')"
[ "$out" = "ok" ] || fail "got '$out', want 'ok'"

echo "  [PASS] closure_rebound_in_try_nogcc: closures a try body binds hold after the catch without statement expressions"
