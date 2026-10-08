#!/bin/sh
# Security rejection tests — programs that must NOT compile,
# and edge cases that must compile but not crash.
#
# Informed by Zig/Hare/Nim/Odin vulnerability analyses.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

# A directory of this run's own: fixed /tmp names let two sweeps on one
# machine overwrite each other's program and binary, and a case then ran
# the other sweep's build.
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

pass=0
fail=0

run_reject() {
    case_name="$1"
    src_file="$2"
    if AETHER_HOME="" "$ROOT/build/ae" build "$src_file" \
        -o $T/ae_sec_out 2>/dev/null; then
        echo "  [FAIL] $case_name should have been rejected"
        fail=$((fail + 1))
    else
        echo "  [PASS] $case_name correctly rejected"
        pass=$((pass + 1))
    fi
}

run_accept() {
    case_name="$1"
    src_file="$2"
    if AETHER_HOME="" "$ROOT/build/ae" build "$src_file" \
        -o $T/ae_sec_out 2>/dev/null; then
        echo "  [PASS] $case_name correctly accepted"
        pass=$((pass + 1))
    else
        echo "  [FAIL] $case_name should have been accepted"
        fail=$((fail + 1))
    fi
}

run_accept_and_exec() {
    case_name="$1"
    src_file="$2"
    expected="$3"
    if ! AETHER_HOME="" "$ROOT/build/ae" build "$src_file" \
        -o $T/ae_sec_out 2>/dev/null; then
        echo "  [FAIL] $case_name compile failed"
        fail=$((fail + 1))
        return
    fi
    out=$($T/ae_sec_out 2>/dev/null)
    if echo "$out" | grep -qF "$expected"; then
        echo "  [PASS] $case_name"
        pass=$((pass + 1))
    else
        echo "  [FAIL] $case_name expected '$expected'"
        echo "         got: $out"
        fail=$((fail + 1))
    fi
}

# ---------------------------------------------------------------
# Rejection: undefined variables
# ---------------------------------------------------------------
cat > $T/ae_sec_undef.ae << 'EOF'
main() { println(nosuchvar) }
EOF
run_reject "undefined variable" $T/ae_sec_undef.ae

# ---------------------------------------------------------------
# Rejection: use hidden var
# ---------------------------------------------------------------
cat > $T/ae_sec_hide_use.ae << 'EOF'
extern println(s: string)
main() {
    secret = "abc"
    {
        hide secret
        println(secret)
    }
}
EOF
run_reject "use hidden variable" $T/ae_sec_hide_use.ae

# ---------------------------------------------------------------
# Rejection: write to hidden var
# ---------------------------------------------------------------
cat > $T/ae_sec_hide_write.ae << 'EOF'
main() {
    secret = "abc"
    {
        hide secret
        secret = "xyz"
    }
}
EOF
run_reject "write hidden variable" $T/ae_sec_hide_write.ae

# ---------------------------------------------------------------
# Rejection: sealed-out variable
# ---------------------------------------------------------------
cat > $T/ae_sec_seal.ae << 'EOF'
extern println(s: string)
main() {
    allowed = 1
    blocked = 2
    {
        seal except allowed, println
        println(blocked)
    }
}
EOF
run_reject "sealed-out variable" $T/ae_sec_seal.ae

# ---------------------------------------------------------------
# Rejection: redeclare hidden name in same scope
# ---------------------------------------------------------------
cat > $T/ae_sec_hide_redecl.ae << 'EOF'
main() {
    x = 1
    {
        hide x
        x = 2
    }
}
EOF
run_reject "redeclare hidden name" $T/ae_sec_hide_redecl.ae

# ---------------------------------------------------------------
# Accept: long identifier (10000 chars)
# ---------------------------------------------------------------
{
    printf 'main() { '
    python3 -c "print('a' * 10000, end='')" 2>/dev/null \
        || printf '%10000s' | tr ' ' 'a'
    printf ' = 42\nprintln("ok")\n}\nextern println(s: string)\n'
} > $T/ae_sec_longid.ae
run_accept "10000-char identifier" $T/ae_sec_longid.ae

# ---------------------------------------------------------------
# Accept: long string literal (10000 chars)
# ---------------------------------------------------------------
{
    printf 'extern println(s: string)\nmain() { x = "'
    python3 -c "print('x' * 10000, end='')" 2>/dev/null \
        || printf '%10000s' | tr ' ' 'x'
    printf '"\nprintln("ok")\n}\n'
} > $T/ae_sec_longstr.ae
run_accept "10000-char string literal" $T/ae_sec_longstr.ae

# ---------------------------------------------------------------
# Accept+run: string with embedded quotes
# ---------------------------------------------------------------
cat > $T/ae_sec_quotes.ae << 'EOF'
extern println(s: string)
import std.string
extern exit(code: int)
main() {
    q = "hello \"world\""
    if string_length(q) != 13 {
        println("FAIL: quote length")
        exit(1)
    }
    println("ok quotes")
}
EOF
run_accept_and_exec "string embedded quotes" \
    $T/ae_sec_quotes.ae "ok quotes"

# ---------------------------------------------------------------
# Accept+run: string with backslashes
# ---------------------------------------------------------------
cat > $T/ae_sec_backslash.ae << 'EOF'
extern println(s: string)
import std.string
extern exit(code: int)
main() {
    bs = "path\\to\\file"
    if string_length(bs) != 12 {
        println("FAIL: backslash length")
        exit(1)
    }
    println("ok backslash")
}
EOF
run_accept_and_exec "string backslashes" \
    $T/ae_sec_backslash.ae "ok backslash"

# ---------------------------------------------------------------
# Accept+run: string with percent signs (not format specifiers)
# ---------------------------------------------------------------
cat > $T/ae_sec_percent.ae << 'EOF'
extern println(s: string)
import std.string
extern exit(code: int)
main() {
    pct = "100% done %s %d"
    if string_length(pct) != 15 {
        println("FAIL: percent length")
        exit(1)
    }
    println("ok percent")
}
EOF
run_accept_and_exec "string percent signs" \
    $T/ae_sec_percent.ae "ok percent"

# ---------------------------------------------------------------
# Accept+run: deep nesting (20 levels)
# ---------------------------------------------------------------
cat > $T/ae_sec_deep.ae << 'EOF'
extern println(s: string)
extern exit(code: int)
main() {
    x = 0
    { { { { { { { { { { { { { { { { { { { {
        x = 1
    } } } } } } } } } } } } } } } } } } } }
    if x != 1 { println("FAIL: deep"); exit(1) }
    println("ok deep")
}
EOF
run_accept_and_exec "20-level nesting" \
    $T/ae_sec_deep.ae "ok deep"

# ---------------------------------------------------------------
# Accept+run: many variables (50)
# ---------------------------------------------------------------
cat > $T/ae_sec_manyvars.ae << 'EOF'
extern println(s: string)
extern exit(code: int)
main() {
    v0 = 0; v1 = 1; v2 = 2; v3 = 3; v4 = 4
    v5 = 5; v6 = 6; v7 = 7; v8 = 8; v9 = 9
    v10 = 10; v11 = 11; v12 = 12; v13 = 13; v14 = 14
    v15 = 15; v16 = 16; v17 = 17; v18 = 18; v19 = 19
    v20 = 20; v21 = 21; v22 = 22; v23 = 23; v24 = 24
    v25 = 25; v26 = 26; v27 = 27; v28 = 28; v29 = 29
    v30 = 30; v31 = 31; v32 = 32; v33 = 33; v34 = 34
    v35 = 35; v36 = 36; v37 = 37; v38 = 38; v39 = 39
    v40 = 40; v41 = 41; v42 = 42; v43 = 43; v44 = 44
    v45 = 45; v46 = 46; v47 = 47; v48 = 48; v49 = 49
    if v0 + v49 != 49 {
        println("FAIL: vars"); exit(1)
    }
    println("ok manyvars")
}
EOF
run_accept_and_exec "50 variables" \
    $T/ae_sec_manyvars.ae "ok manyvars"

# ---------------------------------------------------------------
# Accept+run: string interpolation with special chars
# ---------------------------------------------------------------
cat > $T/ae_sec_interp.ae << 'EOF'
extern println(s: string)
import std.string
extern exit(code: int)
main() {
    name = "O'Reilly"
    greeting = "Hello, ${name}!"
    if string_length(greeting) == 0 {
        println("FAIL: interp"); exit(1)
    }
    a = "x"
    b = "${a}${a}${a}"
    if string_length(b) != 3 {
        println("FAIL: triple interp"); exit(1)
    }
    println("ok interp")
}
EOF
run_accept_and_exec "string interpolation specials" \
    $T/ae_sec_interp.ae "ok interp"

# ---------------------------------------------------------------
# Accept: hide + local in nested scope (not a rejection)
# ---------------------------------------------------------------
cat > $T/ae_sec_hide_local.ae << 'EOF'
extern println(s: string)
extern exit(code: int)
main() {
    x = 42
    {
        hide x
        y = 99
        if y != 99 { println("FAIL: local"); exit(1) }
    }
    println("ok hide local")
}
EOF
run_accept_and_exec "hide with local variable" \
    $T/ae_sec_hide_local.ae "ok hide local"

# ---------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------

echo ""
echo "Security rejection tests: $pass passed, $fail failed"
if [ "$fail" -gt 0 ]; then exit 1; fi
