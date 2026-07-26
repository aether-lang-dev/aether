#!/bin/sh
# #1259: a module declares its own native link deps with @link("...");
# codegen unions them across the resolved import closure into the
# `// aether-link:` header comment. The compiler's static table no longer
# carries rows for downstream modules (contrib.sqlite moved to this).

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

mkdir -p "$TMPDIR/proj/lib/veneer"
cat > "$TMPDIR/proj/lib/veneer/module.ae" <<'AEOF'
@link("-lveneer -lm")
exports (twice)
twice(x: int) -> int { return x * 2 }
AEOF

cat > "$TMPDIR/proj/main.ae" <<'AEOF'
import veneer
main() { println("${veneer.twice(21)}") }
AEOF

cd "$TMPDIR/proj"
"$ROOT/build/aetherc" main.ae out.c >/dev/null 2>&1

head -1 out.c | grep -q "aether-link:.*-lveneer" || {
    echo "  [FAIL] module @link flags missing from aether-link header"
    head -2 out.c; exit 1
}
head -1 out.c | grep -q -- "-lm" || {
    echo "  [FAIL] second @link token missing"; exit 1
}

# Dedup: the same flag from two modules appears once.
mkdir -p lib/other
cat > lib/other/module.ae <<'AEOF'
@link("-lm")
exports (thrice)
thrice(x: int) -> int { return x * 3 }
AEOF
cat > main2.ae <<'AEOF'
import veneer
import other
main() { println("${veneer.twice(3) + other.thrice(2)}") }
AEOF
"$ROOT/build/aetherc" main2.ae out2.c >/dev/null 2>&1
n=$(head -1 out2.c | grep -o -- "-lm" | wc -l | tr -d ' ')
[ "$n" = "1" ] || {
    echo "  [FAIL] duplicate -lm not deduped (count=$n)"
    head -1 out2.c; exit 1
}

# A program with no native deps must not emit the comment at all.
printf 'main() { println("plain") }\n' > plain.ae
"$ROOT/build/aetherc" plain.ae plain.c >/dev/null 2>&1
head -1 plain.c | grep -q "aether-link" && {
    echo "  [FAIL] aether-link emitted for a program with no deps"; exit 1
}

echo "  [PASS] link_directive: @link unions across the closure, dedups, absent when unused"
