#!/bin/sh
# Two modules defining one struct name differently is a diagnosed clash
# (#2129). Struct names are one namespace across modules and the merge
# keeps the first definition it sees, so a program importing both used to
# see the LOSER's own code fail — "Struct 'Quat' has no field 's'",
# pointing into that module — or the C compiler. The clash is reported at
# the second definition, naming both field lists and both files; two
# identical definitions still share one type, silently.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] struct_clash_across_modules: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0

mkdir -p "$tmp/lib/geo" "$tmp/lib/phys" "$tmp/lib/same"
cat > "$tmp/lib/geo/module.ae" <<'AE'
exports(make_q, qx)
struct Quat { x: float, y: float, z: float, w: float }
make_q() -> Quat { return Quat { x: 1.0, y: 2.0, z: 3.0, w: 4.0 } }
qx(q: Quat) -> float { return q.x }
AE
cat > "$tmp/lib/phys/module.ae" <<'AE'
exports(make_p, ps, vx)
struct Vec3 { x: float, y: float, z: float }
struct Quat { v: Vec3, s: float }
make_p() -> Quat { return Quat { v: Vec3 { x: 1.0, y: 2.0, z: 3.0 }, s: 9.0 } }
ps(q: Quat) -> float { return q.s }
vx(v: Vec3) -> float { return v.x }
AE
cat > "$tmp/lib/same/module.ae" <<'AE'
exports(make_v)
struct Vec3 { x: float, y: float, z: float }
make_v() -> Vec3 { return Vec3 { x: 7.0, y: 8.0, z: 9.0 } }
AE

cat > "$tmp/clash.ae" <<'AE'
import geo
import phys
main() {
    a = geo.make_q()
    b = phys.make_p()
    println("${geo.qx(a)} ${phys.ps(b)}")
}
AE
rm -f "$tmp/out.c"
if (cd "$tmp" && AETHER_HOME="$ROOT" "$AETHERC" clash.ae out.c >/dev/null 2>&1) || [ -s "$tmp/out.c" ]; then
    echo "  [FAIL] struct_clash_across_modules: the clash did not stop compilation (aetherc exited 0 or emitted C)"
    fail=1
fi
out="$(cd "$tmp" && AETHER_HOME="$ROOT" "$AETHERC" clash.ae out.c 2>&1)"
if ! printf '%s' "$out" | grep -q "struct 'Quat' is defined differently in two modules: {v: Vec3, s: float} in lib/phys/module.ae and {x: float, y: float, z: float, w: float} in lib/geo/module.ae"; then
    echo "  [FAIL] struct_clash_across_modules: differing definitions were not reported with both field lists and files"
    printf '%s\n' "$out" | grep -E "^error|-->" | head -4 | sed 's/^/        /'
    fail=1
fi

# Importing two modules whose clashing struct is used by neither is still
# refused: the pruner drops the bodies, so nothing later would notice.
cat > "$tmp/unused.ae" <<'AE'
import geo
import phys
main() { println("x") }
AE
if (cd "$tmp" && AETHER_HOME="$ROOT" "$AETHERC" unused.ae out.c >/dev/null 2>&1); then
    echo "  [FAIL] struct_clash_across_modules: a clash between unused imports compiled"
    fail=1
fi

# The program's own struct against an imported one.
cat > "$tmp/own.ae" <<'AE'
import geo
struct Quat { a: int }
main() {
    q = Quat { a: 1 }
    println("${q.a}")
}
AE
out="$(cd "$tmp" && AETHER_HOME="$ROOT" "$AETHERC" own.ae out.c 2>&1)"
if ! printf '%s' "$out" | grep -q "struct 'Quat' is defined differently in two modules: {v: Vec3, s: float} in lib/geo/module.ae and {a: int} in own.ae\|struct 'Quat' is defined differently in two modules: {x: float, y: float, z: float, w: float} in lib/geo/module.ae and {a: int} in own.ae"; then
    echo "  [FAIL] struct_clash_across_modules: a program struct clashing with an imported one was not reported"
    printf '%s\n' "$out" | grep -E "^error|-->" | head -4 | sed 's/^/        /'
    fail=1
fi

# Identical definitions in two modules are one type, no diagnostic.
cat > "$tmp/same.ae" <<'AE'
import phys
import same
main() {
    v = same.make_v()
    println("${phys.vx(v)} ${phys.ps(phys.make_p())}")
}
AE
out="$(cd "$tmp" && AETHER_HOME="$ROOT" "$AE" run same.ae 2>&1)"
got="$(printf '%s\n' "$out" | tail -1)"
if [ "$got" != "7 9" ] || printf '%s' "$out" | grep -q "defined differently"; then
    echo "  [FAIL] struct_clash_across_modules: identical definitions no longer merge silently (got '$got')"
    printf '%s\n' "$out" | head -4 | sed 's/^/        /'
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] struct_clash_across_modules: differing definitions of one struct name are reported; identical ones share the type"
fi
exit $fail
