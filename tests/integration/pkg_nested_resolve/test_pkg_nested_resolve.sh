#!/bin/sh
# Regression: installed packages must resolve from the host-nested layout.
#
# `ae add <host>/<owner>/<repo>` and `apkg install` clone into
# ~/.aether/packages/<host>/<owner>/<repo>/, but the module resolver
# only ever probed the flat ~/.aether/packages/<name>/ path, so every
# package installed through the documented workflow was unimportable
# ("module not found"). The nested scan in module_resolve_local_path
# closes that gap.
#
# HOME is redirected to a temp dir so this never reads or writes the
# developer's real ~/.aether/packages. The test program uses only the
# builtin println, so it needs no stdlib resolution from that HOME.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

FAKE_HOME="$TMPDIR/home"

# Exactly the layout `ae add github.com/testuser/testpkg` produces.
PKG="$FAKE_HOME/.aether/packages/github.com/testuser/testpkg"
mkdir -p "$PKG/src/utils"

cat > "$PKG/src/module.ae" <<'EOF'
exports (greet)
greet() -> string { return "nested-ok" }
EOF

cat > "$PKG/src/utils/module.ae" <<'EOF'
exports (shout)
shout() -> string { return "nested-sub-ok" }
EOF

# 1. Import the package root.
cat > "$TMPDIR/root.ae" <<'EOF'
import testpkg
main() { println(testpkg.greet()) }
EOF

OUT=$(HOME="$FAKE_HOME" "$AE" run "$TMPDIR/root.ae" 2>"$TMPDIR/err1.log") || {
    echo "  [FAIL] nested package root did not resolve"
    head -10 "$TMPDIR/err1.log"
    exit 1
}
if [ "$OUT" != "nested-ok" ]; then
    echo "  [FAIL] expected 'nested-ok', got '$OUT'"
    exit 1
fi

# 2. Import a sub-module inside the package.
cat > "$TMPDIR/sub.ae" <<'EOF'
import testpkg.utils
main() { println(utils.shout()) }
EOF

OUT=$(HOME="$FAKE_HOME" "$AE" run "$TMPDIR/sub.ae" 2>"$TMPDIR/err2.log") || {
    echo "  [FAIL] nested package sub-module did not resolve"
    head -10 "$TMPDIR/err2.log"
    exit 1
}
if [ "$OUT" != "nested-sub-ok" ]; then
    echo "  [FAIL] expected 'nested-sub-ok', got '$OUT'"
    exit 1
fi

# 3. The pre-existing flat layout must keep working.
FLAT="$FAKE_HOME/.aether/packages/flatpkg/src"
mkdir -p "$FLAT"
cat > "$FLAT/module.ae" <<'EOF'
exports (hi)
hi() -> string { return "flat-ok" }
EOF

cat > "$TMPDIR/flat.ae" <<'EOF'
import flatpkg
main() { println(flatpkg.hi()) }
EOF

OUT=$(HOME="$FAKE_HOME" "$AE" run "$TMPDIR/flat.ae" 2>"$TMPDIR/err3.log") || {
    echo "  [FAIL] flat package layout regressed"
    head -10 "$TMPDIR/err3.log"
    exit 1
}
if [ "$OUT" != "flat-ok" ]; then
    echo "  [FAIL] expected 'flat-ok', got '$OUT'"
    exit 1
fi

# 4. Two owners shipping the same package name must be reported, not
#    silently guessed. readdir order is unspecified, so picking the
#    first hit would make the resolved dependency vary by machine.
#    NOTE: each case uses distinct source text on purpose, the build
#    cache is content-addressed and would otherwise serve case 1's
#    binary here.
DUP="$FAKE_HOME/.aether/packages/github.com/otheruser/testpkg/src"
mkdir -p "$DUP"
cat > "$DUP/module.ae" <<'EOF'
exports (greet)
greet() -> string { return "duplicate-owner" }
EOF

cat > "$TMPDIR/ambig.ae" <<'EOF'
import testpkg
main() {
    msg = testpkg.greet()
    println(msg)
}
EOF

if HOME="$FAKE_HOME" "$AE" build "$TMPDIR/ambig.ae" -o "$TMPDIR/ambig.bin" \
        >"$TMPDIR/ambig.log" 2>&1; then
    echo "  [FAIL] ambiguous package resolved silently instead of erroring"
    exit 1
fi
if ! grep -q "ambiguous package import" "$TMPDIR/ambig.log"; then
    echo "  [FAIL] ambiguous install did not produce the expected diagnostic"
    head -10 "$TMPDIR/ambig.log"
    exit 1
fi
# The diagnostic must name both owners so the user can act on it.
if ! grep -q "testuser" "$TMPDIR/ambig.log" || ! grep -q "otheruser" "$TMPDIR/ambig.log"; then
    echo "  [FAIL] ambiguity diagnostic did not name both candidate owners"
    grep "ambiguous" "$TMPDIR/ambig.log" | head -2
    exit 1
fi

echo "  [PASS] pkg_nested_resolve: nested root + sub-module + flat layout resolve; duplicates rejected"
