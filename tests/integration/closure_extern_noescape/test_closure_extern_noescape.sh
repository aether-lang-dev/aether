#!/bin/sh
# Regression (#2523): an extern parameter declared `@noescape` tells the
# compiler the C function uses the closure only during the call, so the
# closure's environment is released once the call returns; an unannotated
# extern that stores the closure keeps it alive for the later call from C.
#
# probe.ae passes capturing closures (literals and a local, by value and
# through a `ptr` slot) to the annotated shims many times and, where the
# heap count is exact, requires zero heap growth over two later rounds; then
# registers one with the storing shim and has C call it. It must print
# "captured" and "OK" and exit 0.
#
# A third case locks the parser: `@noescape` on a `string` parameter is
# refused, since a string parameter is borrowed unless `@retain` says
# otherwise, so the annotation would say nothing there.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] closure_extern_noescape: toolchain not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cd "$SCRIPT_DIR" || exit 1

build_log="$tmpdir/build.log"
if ! "$AE" build probe.ae -o "$tmpdir/probe" >"$build_log" 2>&1; then
    echo "  [FAIL] closure_extern_noescape: ae build failed"
    sed 's/^/    /' "$build_log" | head -30
    exit 1
fi

run_log="$tmpdir/run.log"
"$tmpdir/probe" >"$run_log" 2>&1
rc=$?

if [ $rc -ne 0 ]; then
    echo "  [FAIL] closure_extern_noescape: probe exited $rc (expect 0)"
    sed 's/^/    /' "$run_log" | head -30
    exit 1
fi

if ! grep -q "^captured$" "$run_log" || ! grep -q "^OK$" "$run_log"; then
    echo "  [FAIL] closure_extern_noescape: probe did not print 'captured' and 'OK'"
    sed 's/^/    /' "$run_log" | head -10
    exit 1
fi

# @noescape on a string parameter is a parse error.
cat > "$tmpdir/bad.ae" <<'AE'
extern takes(s: @noescape string)

main() {
    takes("x")
    return 0
}
AE
if "$AETHERC" "$tmpdir/bad.ae" "$tmpdir/bad.c" >"$tmpdir/bad.log" 2>&1; then
    echo "  [FAIL] closure_extern_noescape: @noescape on a string parameter compiled"
    exit 1
fi
if ! grep -q "@noescape on an extern parameter is only valid on" "$tmpdir/bad.log"; then
    echo "  [FAIL] closure_extern_noescape: @noescape on a string parameter failed for another reason"
    sed 's/^/    /' "$tmpdir/bad.log" | head -10
    exit 1
fi

echo "  [PASS] closure_extern_noescape: annotated extern releases the closure; storing extern keeps it"
exit 0
