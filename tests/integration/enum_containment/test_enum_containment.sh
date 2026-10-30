#!/bin/sh
# Qualified enum members retain their written prefix through enum lowering so
# lexical hide/seal checks see the same name the source used.
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] enum_containment: $AE not built"; exit 0; }

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

cat >"$TMPDIR/hide.ae" <<'AE'
enum Color { Red, Blue }
main() {
    {
        hide Color
        c = Color.Red
    }
}
AE

cat >"$TMPDIR/seal.ae" <<'AE'
enum Color { Red, Blue }
main() {
    keep = 1
    {
        seal except keep
        c = Color.Red
    }
}
AE

cat >"$TMPDIR/allowed.ae" <<'AE'
enum Color { Red, Blue }
main() {
    {
        seal except Color
        c = Color.Red
    }
}
AE

failed=0
reject() {
    name="$1"
    if "$AE" build "$TMPDIR/$name.ae" -o "$TMPDIR/$name" >"$TMPDIR/$name.log" 2>&1; then
        echo "  [FAIL] enum_containment: $name accepted a blocked enum prefix"
        failed=1
        return
    fi
    if ! grep -q "'Color' is hidden in this scope" "$TMPDIR/$name.log"; then
        echo "  [FAIL] enum_containment: $name failed without the containment diagnostic"
        sed 's/^/    /' "$TMPDIR/$name.log" | head -10
        failed=1
    fi
}

reject hide
reject seal

if ! "$AE" build "$TMPDIR/allowed.ae" -o "$TMPDIR/allowed" >"$TMPDIR/allowed.log" 2>&1; then
    echo "  [FAIL] enum_containment: whitelisted enum prefix was rejected"
    sed 's/^/    /' "$TMPDIR/allowed.log" | head -10
    failed=1
fi

[ "$failed" -eq 0 ] || exit 1

echo "  [PASS] enum_containment: hidden and sealed prefixes reject; whitelist accepts"
