#!/bin/sh
# Regression: issue #1172 — selective imports of a module must union across a
# compilation unit, and a symbol reached only transitively (by a non-entry
# module, or only by address) must still be emitted.
#
# Repro: `main` does `import aa (one)`; a second module `bb` does
# `import aa (two)` and calls `two()`. Before the fix, the emitted symbol set
# for `aa` was taken from the entry's import list only, so `aa_two` was never
# emitted and `bb.use_two` failed with `Undefined function 'aa_two'`. The
# emitted set must be the union of every selective import of a module across
# the unit, plus whatever is reached transitively.
#
# Also covers the sibling case the issue flags: a function reached only by
# address (passed as a `fn` value), defined in a non-entry module, must be
# emitted too.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] issue1172: $AE not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
cd "$tmpdir" || exit 1

# ---- Case 1: selective-import union across the unit ----
mkdir -p lib/aa lib/bb
cat > lib/aa/module.ae <<'EOF'
exports ( one, two )
one() -> int { return 1 }
two() -> int { return 2 }
EOF
cat > lib/bb/module.ae <<'EOF'
import aa (two)
exports ( use_two )
use_two() -> int { return two() }
EOF
cat > main.ae <<'EOF'
import aa (one)
import bb (use_two)
main() {
    a = one()
    b = use_two()
    println("one=${a} use_two=${b}")
}
EOF

if ! "$AE" build main.ae -o out1 --lib lib >"$tmpdir/b1.log" 2>&1; then
    echo "  [FAIL] issue1172: union build failed (aa_two not emitted for bb.use_two)"
    cat "$tmpdir/b1.log"
    exit 1
fi
out1="$( { [ -x ./out1 ] && ./out1; } || { [ -x ./out1.exe ] && ./out1.exe; } 2>&1)"
if [ "$out1" != "one=1 use_two=2" ]; then
    echo "  [FAIL] issue1172: union expected 'one=1 use_two=2', got '$out1'"
    exit 1
fi

# ---- Case 2: function reached only by address from a non-entry module ----
mkdir -p lib/hh lib/ww
cat > lib/hh/module.ae <<'EOF'
exports ( greet )
greet() -> int { return 99 }
EOF
cat > lib/ww/module.ae <<'EOF'
import hh (greet)
exports ( wrap )
call_it(cb: fn) -> int { return cb() }
wrap() -> int { return call_it(greet) }
EOF
cat > addr.ae <<'EOF'
import ww (wrap)
main() { println("wrap=${wrap()}") }
EOF

if ! "$AE" build addr.ae -o out2 --lib lib >"$tmpdir/b2.log" 2>&1; then
    echo "  [FAIL] issue1172: by-address build failed (greet not emitted)"
    cat "$tmpdir/b2.log"
    exit 1
fi
out2="$( { [ -x ./out2 ] && ./out2; } || { [ -x ./out2.exe ] && ./out2.exe; } 2>&1)"
if [ "$out2" != "wrap=99" ]; then
    echo "  [FAIL] issue1172: by-address expected 'wrap=99', got '$out2'"
    exit 1
fi

echo "  [PASS] issue1172: selective imports union across the unit (incl. by-address)"
exit 0
