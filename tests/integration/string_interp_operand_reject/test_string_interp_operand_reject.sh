#!/bin/sh
# String interpolation checks the KIND of each operand.
#
# `"${expr}"` used to accept any type: the typechecker walked the operands and
# discarded the result, so codegen emitted a `%s`-shaped read of whatever bits
# arrived. The worst case was a bare zero-arg function name, which rendered the
# function's ADDRESS as a string -- "UH\x89\xe5..." is an x86 prologue. That
# surfaced in the wild only when such a string was handed to /bin/sh, i.e. it
# is memory disclosure rather than a formatting wart, and it was silent: the
# same mistake in `x = cmd` at least produced a C warning.
#
# The rejections are one half of this test. The other half matters just as
# much: interpolation is everywhere, so a check that over-rejects breaks the
# language. The ACCEPT cases below are sentries against exactly that.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] string_interp_operand_reject: ae not built"; exit 0; }

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP" || :; return 0; }
trap cleanup EXIT

pass=0
fail=0

# --- REJECT: a bare zero-arg function name (the reported bug) -------------
cat > "$TMP/fn.ae" << 'EOF'
cmd() -> string { return "V" }
main() {
    println("value is ${cmd}")
}
EOF
out=$("$AE" build "$TMP/fn.ae" -o "$TMP/fn.bin" 2>&1)
if echo "$out" | grep -q "it is a function"; then
    echo "  [PASS] bare function name rejected"
    pass=$((pass + 1))
else
    echo "  [FAIL] expected a function-interpolation diagnostic; got:"
    echo "$out" | head -6 | sed 's/^/    /'
    fail=$((fail + 1))
fi

# The message must name the fix. A diagnostic that says "cannot interpolate"
# and stops leaves the reader to guess; `${cmd()}` is the whole answer.
if echo "$out" | grep -q '\${cmd()}'; then
    echo "  [PASS] diagnostic names the fix"
    pass=$((pass + 1))
else
    echo "  [FAIL] diagnostic did not suggest \${cmd()}"
    fail=$((fail + 1))
fi

# ...and point at the interpolation, not the declaration. A bare identifier
# inside ${...} carries the position of the DECLARATION it resolves to, so
# this reported line 1 (where cmd is defined) for a misuse on line 3 until the
# parser was fixed to give interpolation nodes a real position.
if echo "$out" | grep -q "fn.ae:3:"; then
    echo "  [PASS] diagnostic points at the interpolation site"
    pass=$((pass + 1))
else
    echo "  [FAIL] expected the error at fn.ae:3; got:"
    echo "$out" | grep -oE 'fn\.ae:[0-9]+:[0-9]+' | head -2 | sed 's/^/    /'
    fail=$((fail + 1))
fi

# --- REJECT: a struct (silently printed its first field) ------------------
cat > "$TMP/st.ae" << 'EOF'
struct P { x: int, y: int }
main() {
    p = P { x: 1, y: 2 }
    println("point ${p}")
}
EOF
out=$("$AE" build "$TMP/st.ae" -o "$TMP/st.bin" 2>&1)
if echo "$out" | grep -q "cannot interpolate a struct"; then
    echo "  [PASS] struct rejected"
    pass=$((pass + 1))
else
    echo "  [FAIL] expected a struct-interpolation diagnostic; got:"
    echo "$out" | head -6 | sed 's/^/    /'
    fail=$((fail + 1))
fi

# --- ACCEPT: every renderable kind ----------------------------------------
# The sentry. Interpolation is used constantly, so over-rejection is a worse
# failure than the bug being fixed -- and the first version of this check did
# over-reject, breaking std/pqueue and std/schema.
cat > "$TMP/ok.ae" << 'EOF'
cmd() -> string { return "V" }
main() {
    n = 42
    s = "str"
    f = 1.5
    b = 1 == 1
    println("int ${n} str ${s} float ${f} bool ${b} call ${cmd()} expr ${n + 1}")
}
EOF
if out=$("$AE" build "$TMP/ok.ae" -o "$TMP/ok.bin" 2>&1) && "$TMP/ok.bin" >"$TMP/ok.out" 2>&1; then
    if grep -q "int 42 str str float 1.5 bool true call V expr 43" "$TMP/ok.out"; then
        echo "  [PASS] all renderable kinds still interpolate"
        pass=$((pass + 1))
    else
        echo "  [FAIL] renderable kinds produced wrong output:"
        sed 's/^/    /' "$TMP/ok.out"
        fail=$((fail + 1))
    fi
else
    echo "  [FAIL] a valid interpolation was rejected:"
    echo "$out" | head -6 | sed 's/^/    /'
    fail=$((fail + 1))
fi

# --- ACCEPT: a `ptr` --------------------------------------------------------
# Deliberately allowed. Interpolating a ptr is an established idiom here --
# map/pqueue/schema return `ptr` for values that really are char*, and 21
# sites across std/, contrib/, examples/ and tests/ rely on it. Rejecting it
# broke std/pqueue and std/schema when this check first went in, and there is
# no sanctioned conversion to migrate those callers to. Narrowing it is its
# own change; this sentry records the decision.
cat > "$TMP/ptr.ae" << 'EOF'
import std.map
main() {
    m = map.new()
    map.put(m, "k", "hello")
    v, _ = map.get(m, "k")
    println("value ${v}")
}
EOF
if "$AE" build "$TMP/ptr.ae" -o "$TMP/ptr.bin" >"$TMP/ptr.log" 2>&1; then
    echo "  [PASS] ptr interpolation still accepted"
    pass=$((pass + 1))
else
    echo "  [FAIL] ptr interpolation was rejected; that breaks 21 in-tree sites:"
    grep -E "^error" "$TMP/ptr.log" | head -3 | sed 's/^/    /'
    fail=$((fail + 1))
fi

# --- ACCEPT: a bare function name as a callback argument --------------------
# Passing a function by name is legitimate and must stay so; only rendering
# one into a string is not.
cat > "$TMP/cb.ae" << 'EOF'
handler() -> int { return 7 }
run(f: fn() -> int) -> int { return f() }
main() {
    println("via callback: ${run(handler)}")
}
EOF
if "$AE" build "$TMP/cb.ae" -o "$TMP/cb.bin" >"$TMP/cb.log" 2>&1; then
    echo "  [PASS] bare function name still valid as a callback argument"
    pass=$((pass + 1))
else
    echo "  [FAIL] callback argument was rejected:"
    grep -E "^error" "$TMP/cb.log" | head -3 | sed 's/^/    /'
    fail=$((fail + 1))
fi

# --- ACCEPT: a single-bound tuple -------------------------------------------
# `x = f()` where f returns (int, string) binds the FIRST element -- verified,
# it yields 42 rather than a tuple -- but the slot keeps the tuple type.
# Rejecting that flagged a working example in std/tcp/README.md, so the type
# being wrong is not evidence the value is un-renderable.
cat > "$TMP/tup.ae" << 'EOF'
pair() -> (int, string) { return 42, "err" }
main() {
    single = pair()
    println("first element ${single}")
}
EOF
if "$AE" build "$TMP/tup.ae" -o "$TMP/tup.bin" >"$TMP/tup.log" 2>&1; then
    echo "  [PASS] single-bound tuple still accepted"
    pass=$((pass + 1))
else
    echo "  [FAIL] single-bound tuple rejected; that breaks std/tcp's README:"
    grep -E "^error" "$TMP/tup.log" | head -3 | sed 's/^/    /'
    fail=$((fail + 1))
fi

echo ""
echo "string_interp_operand_reject: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
