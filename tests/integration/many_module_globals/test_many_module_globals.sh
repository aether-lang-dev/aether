#!/bin/sh
# #2144: the set of module-level `var` globals was capped at 256.
#
# The unused-variable pass and the `__pure(fn)` analysis both need the names
# of every `var` global in the merged program (a write to one is a store to
# the file-scope static, not a local declaration). Both collected them into
# a 256-entry array and stopped. A program importing enough modules — ae3d's
# editor with ui + ae3d + aephysics — went past the cap, so a setter of any
# later global (`set_native_lanes(e: bool) { g_native_lanes = e }`) was
# reported as W1001 "unused variable", and `__pure` of that setter folded to
# true. The same modules in a smaller program were fine, which is what made
# it look like the checker's read tracking was being reset.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] many_module_globals: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

# 300 globals: g_300 is past the old cap; g_1 is inside it, as a control.
mkdir -p "$tmp/many"
{
    i=1
    while [ "$i" -le 300 ]; do
        echo "var g_$i = $i"
        i=$((i + 1))
    done
    echo "exports(set_first, set_last, total)"
    echo "set_first(v: int) { g_1 = v }"
    echo "set_last(v: int) { g_300 = v }"
    echo "total() -> int { return g_1 + g_300 }"
} > "$tmp/many/module.ae"
# `__pure` takes a bare name, so the module functions are queried through
# wrappers; purity is transitive.
cat > "$tmp/main.ae" <<'AE'
import many
write_first(v: int) { many.set_first(v) }
write_last(v: int) { many.set_last(v) }
read_total() -> int { return many.total() }
main() {
    write_first(1)
    write_last(5)
    println("${read_total()} ${__pure(write_first)} ${__pure(write_last)} ${__pure(read_total)}")
}
AE

out="$(cd "$tmp" && "$AE" run main.ae 2>&1)"
fail=0
if printf '%s\n' "$out" | grep -q "W1001"; then
    echo "  [FAIL] many_module_globals: a setter of a global past the 256th is reported unused"
    printf '%s\n' "$out" | grep -A1 "W1001" | head -4 | sed 's/^/        /'
    fail=1
fi
got="$(printf '%s\n' "$out" | tail -1)"
if [ "$got" != "6 false false true" ]; then
    echo "  [FAIL] many_module_globals: output '$got' (want '6 false false true': the writer of g_300 is not pure)"
    fail=1
fi

# The walks now follow a qualified call into the imported module; the same
# blindness hid a capability call behind `mod.fn()` from an effect tag.
mkdir -p "$tmp/shell"
cat > "$tmp/shell/module.ae" <<'AE'
import std.os
exports(run_it)
run_it() -> int { return os.system("echo hi") }
AE
cat > "$tmp/tagged.ae" <<'AE'
import shell
@no_os
quiet() -> int { return shell.run_it() }
main() { println("${quiet()}") }
AE
out="$(cd "$tmp" && "$AE" run tagged.ae 2>&1)"
if ! printf '%s
' "$out" | grep -q "declared .*@no_os.*reaches a .*os.* operation through .*os.system"; then
    echo "  [FAIL] many_module_globals: @no_os did not see os.system behind a module call"
    printf '%s
' "$out" | head -4 | sed 's/^/        /'
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] many_module_globals: 300 module globals — no spurious W1001; __pure and effect tags see through module calls"
fi
exit $fail
