#!/bin/sh
# #2569: contrib.host.tinygo's owned string wrappers, without a Go toolchain.
#
# fake_cshared.c stands in for a `go build -buildmode=c-shared` library: the
# C signatures cgo generates, and string results from malloc, as C.CString's
# are. It is built here with the C compiler and loaded through
# contrib.host.tinygo by uses_owned.ae, which checks every _owned wrapper,
# the borrowed wrappers on a static result, and that owned calls leave the
# heap where it was. test_host_tinygo.sh runs against a real cgo library
# when `go` is installed.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

# The bridge archive comes from `make contrib`, which `make test-ae` does not
# run; see test_host_tinygo.sh for why this skips rather than fails.
if [ ! -f "$ROOT/build/libaether_host_tinygo.a" ] && \
   [ ! -f "$ROOT/build/contrib/libaether_host_tinygo.a" ]; then
    echo "  [SKIP] contrib.host.tinygo: bridge not built; run \`make contrib\` first"
    exit 0
fi

# `make test-ae` exports CC; run alone, take what is installed.
if [ -z "${CC:-}" ]; then
    if command -v cc >/dev/null 2>&1; then CC=cc; else CC=gcc; fi
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

case "$(uname -s)" in
    Darwin) LIB="$TMPDIR/libfake.dylib"; PIC="-fPIC" ;;
    MINGW*|MSYS*|CYGWIN*) LIB="$TMPDIR/libfake.dll"; PIC="" ;;
    *)      LIB="$TMPDIR/libfake.so"; PIC="-fPIC" ;;
esac

# shellcheck disable=SC2086
if ! $CC -shared $PIC -o "$LIB" "$SCRIPT_DIR/fake_cshared.c" 2>"$TMPDIR/cc.err"; then
    echo "  [FAIL] could not build the stand-in library with $CC:"
    head -10 "$TMPDIR/cc.err"
    exit 1
fi

ACTUAL="$TMPDIR/actual.txt"
if ! AETHER_HOME="$ROOT" TINYGO_LIB="$LIB" \
        "$AE" run "$SCRIPT_DIR/uses_owned.ae" >"$ACTUAL" 2>"$TMPDIR/err.log"; then
    echo "  [FAIL] ae run exited non-zero"
    head -20 "$TMPDIR/err.log"
    head -20 "$ACTUAL"
    exit 1
fi

if ! grep -Fxq "owned wrappers ok" "$ACTUAL"; then
    echo "  [FAIL] uses_owned.ae did not finish"
    cat "$ACTUAL"
    exit 1
fi

echo "  [PASS] contrib.host.tinygo: 6 owned string wrappers take malloc'd results over and free them; $(grep '^heap' "$ACTUAL")"
