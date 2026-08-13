#!/bin/sh
# Issue #1527: conditional compilation on build symbols.
#
# The reporter's need was not "choose a value" but "leave a subsystem out of
# the binary", so the assertions are:
#   - the losing branch's code does not run,
#   - and its symbols are not in the linked binary at all.
#
# A region is dropped from the AST rather than wrapped in a preprocessor #if,
# which is what makes the second assertion hold. It also means the excluded
# code needs nothing it references to exist.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
SRC="$SCRIPT_DIR/optional_subsystem.ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT
fails=0

build_run() {
    out="$1"; shift
    if ! AETHER_HOME="$ROOT" "$AE" build "$@" "$SRC" -o "$TMPDIR/$out" >"$TMPDIR/build_$out.log" 2>&1; then
        echo "  [FAIL] build with [$*] failed"
        head -5 "$TMPDIR/build_$out.log"
        fails=$((fails + 1))
        return 1
    fi
    "$TMPDIR/$out" > "$TMPDIR/$out.out" 2>&1
    return 0
}

want() {
    file="$1"; pattern="$2"; label="$3"
    if grep -q "$pattern" "$TMPDIR/$file"; then
        echo "  [PASS] $label"
    else
        echo "  [FAIL] $label (no '$pattern' in output)"
        cat "$TMPDIR/$file"
        fails=$((fails + 1))
    fi
}

reject() {
    file="$1"; pattern="$2"; label="$3"
    if grep -q "$pattern" "$TMPDIR/$file"; then
        echo "  [FAIL] $label ('$pattern' should not be there)"
        cat "$TMPDIR/$file"
        fails=$((fails + 1))
    else
        echo "  [PASS] $label"
    fi
}

# ---- symbol absent -------------------------------------------------------
if build_run off; then
    want   off.out "driver absent" "the else branch is what runs"
    reject off.out "driver on"     "the guarded branch does not run"
    want   off.out "quiet"         "a statement region falls to its else"
    want   off.out "mode prod"     "select falls through to other:"
    reject off.out "either"        "an all-false region emits nothing"
fi

# ---- symbol present ------------------------------------------------------
if build_run on -D TEST_SERVER; then
    want   on.out "driver on 9222" "the guarded declarations are built in"
    want   on.out "mode test"      "select takes the build symbol's value"
    want   on.out "test, quiet"    "&& with a negation"
    want   on.out "either"         "|| with one side set"
fi

# ---- both symbols --------------------------------------------------------
if build_run both -D TEST_SERVER -D VERBOSE; then
    want   both.out "verbose on"  "a nested platform select inside a region"
    reject both.out "test, quiet" "&& with a negation that is now false"
fi

# ---- the point: the symbols are not in the binary ------------------------
# `ae build` produced both, so compare them rather than trusting either.
if command -v nm >/dev/null 2>&1; then
    if nm "$TMPDIR/on" 2>/dev/null | grep -q driver_port_number; then
        if nm "$TMPDIR/off" 2>/dev/null | grep -q driver_port_number; then
            echo "  [FAIL] driver_port_number is still linked into the disabled build"
            fails=$((fails + 1))
        else
            echo "  [PASS] the excluded subsystem is absent from the binary"
        fi
    else
        # A stripped or LTO'd build has no symbol either way, so the
        # comparison would pass vacuously. Say so instead.
        echo "  [SKIP] nm sees no symbols even in the enabled build"
    fi
else
    echo "  [SKIP] nm not available"
fi

# ---- the cache must not serve one build's binary for the other -----------
# Same source, different symbols: a key that ignored -D would return the
# first artifact and the region would silently come back.
if build_run cache1 -D TEST_SERVER && build_run cache2; then
    want   cache1.out "driver on 9222" "cached build with the symbol"
    want   cache2.out "driver absent"  "a rebuild without it is not served the cached one"
fi

# ---- rejected spellings --------------------------------------------------
if "$AETHERC" -D "FEATURE=1" --emit-c "$SRC" >/dev/null 2>"$TMPDIR/err_val.txt"; then
    echo "  [FAIL] -D FEATURE=1 was accepted"
    fails=$((fails + 1))
else
    want err_val.txt "do not carry values" "a -D with a value is rejected"
fi

if "$AETHERC" -D "9BAD" --emit-c "$SRC" >/dev/null 2>"$TMPDIR/err_id.txt"; then
    echo "  [FAIL] -D 9BAD was accepted"
    fails=$((fails + 1))
else
    want err_id.txt "takes an identifier" "a -D that is not an identifier is rejected"
fi

if [ "$fails" -ne 0 ]; then
    echo "  [FAIL] build_defines: $fails failure(s)"
    exit 1
fi
echo "  [PASS] build_defines"
