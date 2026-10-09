#!/bin/sh
# #2487: a dev-mode ./build/ae compiles std from its own build tree, whatever
# AETHER_HOME says. Run from outside the repository with AETHER_HOME naming an
# older install, it compiled that install's std with the new compiler: ae
# exported its own root without replacing the variable (setenv's overwrite
# flag was 0), and aetherc's module resolver tries AETHER_HOME before the
# compiler's own tree.
#
# The "older install" here is a std.string without length(); a program that
# calls string.length compiles only against the build tree's std. On Windows
# ae always replaced the variable (_putenv), so there the check holds either
# way.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] dev_mode_std_from_build_tree: ae not built"; exit 0; }

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP" || :; return 0; }
trap cleanup EXIT
fail() { echo "  [FAIL] dev_mode_std_from_build_tree: $1"; exit 1; }

OLD="$TMP/old"
mkdir -p "$OLD/share/aether/std/string" "$TMP/work"
cat > "$OLD/share/aether/std/string/module.ae" <<'EOF'
// An older install's std.string, without length().
upper_only(s: string) -> string {
    return s
}
EOF
cat > "$TMP/work/probe.ae" <<'EOF'
import std.string

main() {
    println("length ${string.length("abc")}")
}
EOF

if command -v cygpath >/dev/null 2>&1; then
    OLD_HOME="$(cygpath -m "$OLD")"
else
    OLD_HOME="$OLD"
fi

cd "$TMP/work"
OUT="$(AETHER_HOME="$OLD_HOME" "$AE" run probe.ae 2>&1)" || fail "the build used the older install's std: $(echo "$OUT" | grep -m1 -i error)"
case "$OUT" in
    *"length 3"*) ;;
    *) fail "unexpected output: $OUT" ;;
esac

echo "  [PASS] dev_mode_std_from_build_tree: std comes from the build tree, not AETHER_HOME"
