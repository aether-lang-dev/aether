#!/bin/sh
# #1584: a std module's co-located spec (std/<mod>/test_*.ae) is a test
# program, not part of the module. The resolver only ever looks for
# <path>/module.ae, so `import std.deque.test_deque` must not resolve to
# std/deque/test_deque.ae: the stdlib namespace stays exactly the set of
# module.ae files, and a spec beside a module can never be imported by a
# user program, in the tree or from an install.
#
# Acceptance: the import is rejected at compile time (exit non-zero, no
# binary), the diagnostic names the module it looked in, and the plain
# `import std.deque` beside it still works — so the failure is the
# resolver refusing the spec, not a broken std.deque.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] std_spec_not_importable: $AE not built"
    exit 0
fi

if [ ! -f "$ROOT/std/deque/test_deque.ae" ]; then
    echo "  [FAIL] std_spec_not_importable: fixture std/deque/test_deque.ae is gone; pick another co-located spec"
    exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

cat > "$tmp/imports_spec.ae" <<'AE'
import std.deque.test_deque

main() {
    println("must not compile")
}
AE

out=$("$AE" build "$tmp/imports_spec.ae" -o "$tmp/imports_spec" 2>&1)
rc=$?
if [ "$rc" -eq 0 ] || [ -x "$tmp/imports_spec" ]; then
    echo "  [FAIL] std_spec_not_importable: import std.deque.test_deque compiled (rc=$rc)"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi
if ! printf '%s\n' "$out" | grep -q "test_deque"; then
    echo "  [FAIL] std_spec_not_importable: the diagnostic does not name test_deque"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

cat > "$tmp/imports_module.ae" <<'AE'
import std.deque

main() {
    d = deque.new(2)
    d = deque.push_back(d, 7)
    println("len=${deque.len(d)}")
}
AE

out=$("$AE" run "$tmp/imports_module.ae" 2>&1)
rc=$?
if [ "$rc" -ne 0 ] || ! printf '%s\n' "$out" | grep -q '^len=1$'; then
    echo "  [FAIL] std_spec_not_importable: plain import std.deque broke (rc=$rc)"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

echo "  [PASS] std_spec_not_importable: a co-located spec is not an importable module; std.deque itself still is"
exit 0
