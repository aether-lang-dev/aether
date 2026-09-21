#!/bin/sh
# A hoisted local is zero-initialized.
#
# A local first bound inside a branch or loop body and used after it is
# declared at function scope by codegen (hoist_if_branch_vars,
# hoist_loop_vars, hoist_if_else_common_vars). It was declared bare —
# `int late;` — so on a path that skipped the body its value was
# indeterminate, and reading an indeterminate value is undefined in C: a
# compiler's interprocedural constant propagation may treat it as whatever
# the other call sites pass, with no sanitizer able to say so (#2128 is
# a report of that shape against gcc 16 -O2). The declaration now carries
# the zero initializer for its kind, so such a read is a defined 0.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] hoisted_local_zero_init: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0

cat > "$tmp/main.ae" <<'AE'
struct P { a: int, b: string }

report(flag: int) -> int {
    if flag > 0 { late = 7 }
    return late
}

main() {
    i = 0
    while i < 2 {
        n = i * 2
        s = "x"
        q = P { a: 1, b: "y" }
        f = 1.5
        i = i + 1
    }
    if i > 5 { name = "set" } else { name = "else" }
    println("${report(1)} ${report(0)} ${n} ${s} ${q.a} ${f} ${name}")
}
AE
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/main.ae" 2>&1 | tail -1)"
if [ "$got" != "7 0 2 x 1 1.5 else" ]; then
    echo "  [FAIL] hoisted_local_zero_init: a hoisted local read on the untaken path is not zero (got '$got')"
    fail=1
fi
AETHER_HOME="$ROOT" "$AETHERC" "$tmp/main.ae" "$tmp/out.c" >/dev/null 2>&1
for decl in 'int late = 0;' 'int n = 0;' 'const char\* s = NULL;' 'P q = {0};' 'double f = 0;'; do
    if ! grep -q "$decl" "$tmp/out.c"; then
        echo "  [FAIL] hoisted_local_zero_init: expected '$decl' among the hoisted declarations"
        fail=1
    fi
done
# No bare hoisted declaration of the program's locals is left (struct
# field declarations are excluded by name).
if grep -Eq '^\s*(int|double|const char\*|P) (late|n|s|q|f|name);$' "$tmp/out.c"; then
    echo "  [FAIL] hoisted_local_zero_init: an uninitialized hoisted declaration remains:"
    grep -En '^\s*(int|double|const char\*|P) (late|n|s|q|f|name);$' "$tmp/out.c" | head -3 | sed 's/^/        /'
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] hoisted_local_zero_init: hoisted locals carry a zero initializer and read as 0 on the untaken path"
fi
exit $fail
