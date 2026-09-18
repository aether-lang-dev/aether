#!/bin/sh
# Issue #261: contrib/host/tinygo end-to-end smoke test.
#
# Skips cleanly when the Go toolchain is not on PATH (matches
# contrib/host/go's pattern — host bridges should never break CI on
# machines that don't have the toolchain installed). When `go` IS
# available, the test builds a c-shared .so, loads it via
# contrib.host.tinygo, and exercises every wrapper signature.
#
# NOTE on the build command: this used `tinygo build -buildmode=c-shared`
# historically, but TinyGo 0.34.0 restricts that buildmode to wasm
# targets ("buildmode c-shared is only supported on wasm at the moment").
# The bridge is a runtime dlopen — it doesn't care which compiler
# produced the .so, only that it's a valid c-shared with the exported
# symbols. We use standard `go build -buildmode=c-shared`, which works
# natively on linux/darwin and produces a binary-compatible .so. The
# "tinygo" name on the bridge is retained as the brand; the toolchain
# choice is a build-time detail.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if ! command -v go >/dev/null 2>&1; then
    echo "  [SKIP] go not on PATH — install via https://go.dev/dl/"
    exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

case "$(uname -s)" in
    Darwin) LIB_EXT=dylib ;;
    Linux)  LIB_EXT=so ;;
    MINGW*|MSYS*|CYGWIN*) LIB_EXT=dll ;;
    *)      LIB_EXT=so ;;
esac

LIB_PATH="$TMPDIR/libgreet.$LIB_EXT"
# The bridge archive is built by `make contrib`, which neither `make test-ae`
# nor `make ci` runs — so in a plain checkout it is simply absent, and `ae run`
# below fails at LINK with `undefined reference to tinygo_call_*` rather than
# anything to do with this test's subject. Skip on the missing prerequisite,
# the same way the `go` check above does, instead of reporting a link error
# that reads like a bug in the bridge. Both layouts, per tools/ae.c: install
# puts it beside libaether.a, a dev tree under contrib/.
if [ ! -f "$ROOT/build/libaether_host_tinygo.a" ] && \
   [ ! -f "$ROOT/build/contrib/libaether_host_tinygo.a" ]; then
    echo "  [SKIP] contrib.host.tinygo: bridge not built — run \`make contrib\` first"
    exit 0
fi

GO_SRC="$ROOT/contrib/host/tinygo/examples/greet.go"

# `go build -buildmode=c-shared` requires CGO (the source uses `import "C"`)
# and writes the source's directory into the binary, so build from a
# fresh tmpdir copy to keep the source tree clean.
cp "$GO_SRC" "$TMPDIR/greet.go"
if ! (cd "$TMPDIR" && CGO_ENABLED=1 go build -buildmode=c-shared -o "$LIB_PATH" greet.go) 2>"$TMPDIR/go.err"; then
    echo "  [FAIL] go build -buildmode=c-shared failed:"
    head -10 "$TMPDIR/go.err"
    exit 1
fi

# The `import contrib.host.tinygo` triggers `ae build`'s auto-link
# of libaether_host_tinygo.a — AND, as of this PR, its transitive
# `-lffi` when the bridge .a was compiled with AETHER_HAS_LIBFFI
# (the call_dynamic escape hatch). No aether.toml workaround
# required here any more (previously needed link_flags = "-lffi").
ACTUAL="$TMPDIR/actual.txt"
if ! AETHER_HOME="$ROOT" TINYGO_LIB="$LIB_PATH" \
        "$AE" run "$SCRIPT_DIR/uses_tinygo.ae" >"$ACTUAL" 2>"$TMPDIR/err.log"; then
    echo "  [FAIL] ae run exited non-zero"
    cat "$TMPDIR/err.log" | head -20
    cat "$ACTUAL" | head -10
    exit 1
fi

# Check each line individually so we can be precise about which
# wrapper failed if the test breaks.
for expected in \
    "Answer = 42" \
    "Add(2, 40) = 42" \
    "Negate(7) = -7" \
    "hello, world"
do
    if ! grep -Fxq "$expected" "$ACTUAL"; then
        echo "  [FAIL] expected line not found: $expected"
        echo "--- actual output ---"
        cat "$ACTUAL"
        exit 1
    fi
done

echo "  [PASS] contrib.host.tinygo: in-process c-shared invocation across 5 wrapper shapes"
