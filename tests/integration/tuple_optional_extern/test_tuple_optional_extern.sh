#!/bin/sh
# A C function returning a tuple with an optional element (#2652).
#
# `extern c_opt_pair(a: int) -> (string?, int)` failed in gcc with "unknown
# type name 'ae_opt_string'": the tuple's typedef was emitted before the
# optional's. The tuple typedef now declares its elements' typedefs first,
# so the C side returns `{ { has, val }, n }` by value and the program reads
# both slots. The same tuple from an Aether function is
# tests/regression/test_tuple_optional_element.ae.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] tuple_optional_extern: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT

# The C layout of `(string?, int)`: the optional is `{ int has; const char*
# val; }`, embedded by value.
cat > "$tmp/shim.c" <<'C'
typedef struct { int has; const char* val; } opt_str;
typedef struct { opt_str _0; int _1; } opt_pair;

opt_pair c_opt_pair(int a) {
    opt_pair r;
    r._0.has = a > 0;
    r._0.val = a > 0 ? "pos" : 0;
    r._1 = a;
    return r;
}
C

cat > "$tmp/main.ae" <<'AE'
extern c_opt_pair(a: int) -> (string?, int)

main() {
    s, k = c_opt_pair(3)
    if s != none {
        println("opt ${s!} ${k}")
    }
    t, j = c_opt_pair(-2)
    if t == none {
        println("none ${j}")
    }
}
AE

if ! (cd "$tmp" && AETHER_HOME="$ROOT" "$AE" build main.ae -o main --extra shim.c) \
        > "$tmp/build.log" 2>&1; then
    echo "  [FAIL] tuple_optional_extern: the program did not build"
    grep -E "error" "$tmp/build.log" | head -5 | sed 's/^/        /'
    exit 1
fi
got="$("$tmp/main" 2>&1 | tr -d '\r' | tr '\n' ' ')"
if [ "$got" != "opt pos 3 none -2 " ]; then
    echo "  [FAIL] tuple_optional_extern: read '$got' from the C tuple"
    exit 1
fi
echo "  [PASS] tuple_optional_extern: a C function's (string?, int) tuple builds and reads back"
exit 0
