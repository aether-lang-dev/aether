#!/bin/sh
# Regression: a @c_callback definition carried into MORE THAN ONE translation
# unit must not collide at link. A @c_callback symbol is external by design (a C
# caller binds it by name, so it can't be static) — but when the module holding
# it is pulled into two TUs (two programs that both import it, e.g. two TUs that
# both transitively import std.http.client and so both carry the tls13_client
# pure_tls_client_* bridge), each TU emits the same external definition and they
# collide: "multiple definition of shared_cb_symbol".
# asks/pure-tls-client-defined-non-static-in-every-tu.md.
#
# The fix emits the @c_callback DEFINITION weak (AETHER_WEAK_DEF), so duplicate
# copies dedupe at link while the symbol stays externally addressable. This test
# compiles two programs that both import a module with a @c_callback leaf, links
# both objects plus a C driver that binds the symbol, and requires the link to
# succeed and the callback to run.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

# Weak linkage is a GCC/clang attribute; skip where unsupported (the fix is a
# no-op there and the multi-TU case was already unbuildable on that toolchain).
case "$(uname -s)" in
    Linux|Darwin|*BSD) ;;
    *) echo "  [SKIP] c_callback multi-TU: weak-symbol link check is POSIX/GNU-ld"; exit 0 ;;
esac

CFLAGS="$("$ROOT/build/ae" cflags 2>/dev/null)"
if [ -z "$CFLAGS" ]; then echo "  [SKIP] c_callback multi-TU: ae cflags unavailable"; exit 0; fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
# aetherc resolves imports relative to the source's dir; work from a copy so the
# import of `shared` finds shared.ae.
cp "$SCRIPT_DIR"/shared.ae "$SCRIPT_DIR"/one.ae "$SCRIPT_DIR"/two.ae "$tmpdir/"

fail=0
gen() {  # gen <name> — emit C, neutralise its main() (we link a single C main)
    if ! (cd "$tmpdir" && "$AETHERC" "$1.ae" "$1.c") >"$tmpdir/$1.emit" 2>&1; then
        echo "  [FAIL] aetherc could not emit $1.ae"; sed 's/^/          /' "$tmpdir/$1.emit" | head -6; fail=1; return 1
    fi
    # Rename each program's main so both objects link under one C main. Use a
    # sed-to-temp-then-mv rewrite, NOT `sed -i` — GNU sed and BSD/macOS sed
    # disagree on whether -i takes a mandatory backup-suffix argument.
    sed 's/int main(int argc/int main_'"$1"'_(int argc/' "$tmpdir/$1.c" > "$tmpdir/$1.c.tmp" \
        && mv "$tmpdir/$1.c.tmp" "$tmpdir/$1.c"
}
gen one || exit 1
gen two || exit 1

# Both TUs must actually carry the callback definition (else the test proves nothing).
if [ "$(grep -c 'shared_cb_symbol' "$tmpdir/one.c")" -eq 0 ] || \
   [ "$(grep -c 'shared_cb_symbol' "$tmpdir/two.c")" -eq 0 ]; then
    echo "  [FAIL] the @c_callback symbol was not emitted into both TUs"; exit 1
fi

if ! gcc -c "$tmpdir/one.c" -o "$tmpdir/one.o" $CFLAGS -w 2>"$tmpdir/one.cc" \
   || ! gcc -c "$tmpdir/two.c" -o "$tmpdir/two.o" $CFLAGS -w 2>"$tmpdir/two.cc"; then
    echo "  [FAIL] a TU failed to compile"; sed 's/^/          /' "$tmpdir"/*.cc | head -8; exit 1
fi

if ! gcc "$SCRIPT_DIR/driver.c" "$tmpdir/one.o" "$tmpdir/two.o" $CFLAGS -w \
        -o "$tmpdir/bin" 2>"$tmpdir/link.err"; then
    echo "  [FAIL] two TUs carrying the same @c_callback did not link (the bug)"
    grep -i "multiple definition" "$tmpdir/link.err" | sed 's/^/          /' | head -4
    exit 1
fi

if out="$("$tmpdir/bin" 2>&1)" && [ "$out" = "cb(7)=21" ]; then
    echo "  [PASS] two TUs with the same @c_callback link and the symbol runs"
    echo "PASS: c_callback_multi_tu"
    exit 0
fi
echo "  [FAIL] linked, but the callback did not run correctly: $out"
exit 1
