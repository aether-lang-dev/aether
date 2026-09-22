#!/bin/sh
# #2037: a function referenced ONLY as a function-value survives the
# reachability prune, in every shape a library module can take.
#
# The report was an `E0300 Undefined variable 'bldr__gsd_cmp_base'` at a
# consumer's link: a comparator handed to `sort.strings_by` and never called
# directly, in a module the consumer reached through `--lib`. A function whose
# address is taken IS reachable, so the prune dropping it would be a real
# defect — `prune_collect_calls` seeds bare identifiers for exactly that
# reason.
#
# It does not reproduce on this tree. Rather than close the issue on an
# absence, this pins every shape that was suspected, so the day one of them
# regresses it is this test that says so and not a downstream project:
#
#   1. one compilation unit, the callee only ever passed as a value
#   2. a `--lib` module, the fn-value inside an exported function
#   3. two comparators selected by a branch (neither called directly)
#   4. a module reached THROUGH another module — the consumer never imports
#      the module that owns the comparator
#   5. `--emit=lib`: the symbol is in the emitted library, not pruned out
#
# Shapes 3 and 4 together are the reported form: `groovy` calls
# `bldr._glob_sorted_desc`, which picks between `_gsd_cmp_base` and
# `_gsd_cmp_full` and passes the winner to `sort.strings_by`.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] fn_value_library_survives: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0
export AETHER_HOME="$ROOT"
mkdir -p "$tmp/lib/bldr" "$tmp/lib/groovy" "$tmp/proj"

# The library module: two comparators, each referenced only as a value, and
# the function that picks between them. Exported with a leading underscore,
# as the reported one was.
cat > "$tmp/lib/bldr/module.ae" <<'AE'
import std.string
import std.strarr
import std.sort

exports(unrelated, _glob_sorted_desc)

_cmp_base(x: string, y: string) -> int { return string.version_compare(x, y) }
_cmp_full(x: string, y: string) -> int { return string.version_compare(y, x) }

unrelated() -> int { return 1 }

_glob_sorted_desc(newest_first: int) -> string {
    sa = strarr.new()
    _p = strarr.push_copy(sa, "a-1.0")
    _p = strarr.push_copy(sa, "a-10.0")
    _p = strarr.push_copy(sa, "a-2.0")
    if newest_first == 1 { sort.strings_by(strarr.array(sa), strarr.size(sa), _cmp_full) }
    else { sort.strings_by(strarr.array(sa), strarr.size(sa), _cmp_base) }
    out = string.copy(strarr.get(sa, 0))
    strarr.free(sa)
    return out
}
AE

# The module in between: the consumer imports THIS, never bldr.
cat > "$tmp/lib/groovy/module.ae" <<'AE'
import bldr
exports(newest, oldest)
newest() -> string { return bldr._glob_sorted_desc(1) }
oldest() -> string { return bldr._glob_sorted_desc(0) }
AE

# 1. One compilation unit: the callee is only ever passed as a value.
cat > "$tmp/proj/single.ae" <<'AE'
import std.string
import std.strarr
import std.sort
_by_version(x: string, y: string) -> int { return string.version_compare(x, y) }
main() {
    sa = strarr.new()
    _p = strarr.push_copy(sa, "a-10.0")
    _p = strarr.push_copy(sa, "a-2.0")
    sort.strings_by(strarr.array(sa), strarr.size(sa), _by_version)
    println(strarr.get(sa, 0))
    strarr.free(sa)
}
AE
got="$(cd "$tmp/proj" && "$AE" run single.ae 2>&1 | tail -1)"
if [ "$got" != "a-2.0" ]; then
    echo "  [FAIL] fn_value_library_survives: single unit (got '$got')"
    fail=1
fi

# 2 + 3. A --lib module, two comparators, neither called directly.
printf 'import bldr\nmain() { println(bldr._glob_sorted_desc(1)); println(bldr._glob_sorted_desc(0)) }\n' \
    > "$tmp/proj/direct.ae"
got="$(cd "$tmp/proj" && "$AE" build --lib "$tmp/lib" direct.ae -o "$tmp/direct" 2>&1 | tail -1; "$tmp/direct" 2>&1 | tr '\n' ' ')"
case "$got" in
    *"a-10.0 a-1.0 "*) ;;
    *) echo "  [FAIL] fn_value_library_survives: --lib module, two comparators (got '$got')"
       fail=1 ;;
esac

# 4. Reached through another module: the consumer never imports bldr.
printf 'import groovy\nmain() { println(groovy.newest()); println(groovy.oldest()) }\n' \
    > "$tmp/proj/indirect.ae"
got="$(cd "$tmp/proj" && "$AE" build --lib "$tmp/lib" indirect.ae -o "$tmp/indirect" 2>&1 | tail -1; "$tmp/indirect" 2>&1 | tr '\n' ' ')"
case "$got" in
    *"a-10.0 a-1.0 "*) ;;
    *) echo "  [FAIL] fn_value_library_survives: reached through another module (got '$got')"
       fail=1 ;;
esac

# 5. The prune is an AST decision, so the generated C is where it shows:
#    BOTH comparators must be defined there, not merely declared. This is
#    the assertion the report was really about — `bldr__gsd_cmp_base` was
#    undefined at the consumer's link, and `bldr__cmp_base` below is the
#    same mangling. Read from the C rather than from a built library's
#    symbol table, which is not portable: a MinGW DLL's exports do not
#    appear in `nm` output at all.
(cd "$tmp/proj" && "$AETHERC" --lib "$tmp/lib" indirect.ae emitted.c >/dev/null 2>&1)
for sym in bldr__cmp_base bldr__cmp_full; do
    if ! grep -q "int $sym(const char\* x, const char\* y) {" "$tmp/proj/emitted.c"; then
        echo "  [FAIL] fn_value_library_survives: $sym has no definition in the generated C"
        grep -c "$sym" "$tmp/proj/emitted.c" | sed 's/^/        mentions: /'
        fail=1
    fi
done

# ... and --emit=lib gets all the way through the C compiler with them.
lib="--emit=lib: built"
if ! (cd "$tmp/proj" && "$AE" build --lib "$tmp/lib" --emit=lib indirect.ae -o "$tmp/libind" \
        > "$tmp/lib.log" 2>&1); then
    echo "  [FAIL] fn_value_library_survives: --emit=lib failed"
    tail -3 "$tmp/lib.log" | sed 's/^/        /'
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] fn_value_library_survives: a fn-value-only callee survives the prune in all four module shapes, and in the generated C; $lib"
fi
exit $fail
