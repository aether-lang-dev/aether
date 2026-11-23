#!/bin/sh
# #3840: a string argument held for a call (read from a place the callee can
# reach, R3) and a closure argument are journaled for a panic with a token,
# as #3787 does for a slice: the track says whether it wrote the entry, and
# the forget after the call runs only then (aether_unwind_forget_tok).
# Outside every `try` the pair was two out-of-line calls that did nothing.
#
#   1. held.ae's C forgets each held argument through its `_j` token, and
#      no plain aether_unwind_forget of an argument temporary is left.
#   2. held.ae runs: 1000 calls outside a `try`, then 200 inside one whose
#      third call panics. The entries written inside are forgotten after
#      each call, the panic's drain releases the hold, and the held field
#      reads the same at the end.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
[ -f "$AE.exe" ] && AE="$AE.exe"
[ -f "$AETHERC.exe" ] && AETHERC="$AETHERC.exe"
NAME=held_arg_journal_token

if [ ! -f "$AE" ] || [ ! -f "$AETHERC" ]; then
    echo "  [SKIP] $NAME: ae or aetherc not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
AETHER_CACHE_DIR="$TMP/cache"
export AETHER_CACHE_DIR
export AETHER_HOME="$ROOT"
unset AE_CC
fails=0
fail() { echo "  [FAIL] $NAME: $1"; fails=$((fails + 1)); }

cp "$SCRIPT_DIR/held.ae" "$TMP/"
cd "$TMP" || exit 1

if ! "$AETHERC" held.ae held.c > held.log 2>&1; then
    sed 's/^/        /' held.log | head -5
    fail "held.ae did not compile"
else
    toks=$(grep -o 'aether_unwind_forget_tok(_ad_[0-9]*_j, (const void\*)&_ad_[0-9]*)' held.c | wc -l | tr -d ' ')
    [ "$toks" -ge 4 ] ||
        fail "held.ae's C forgets $toks argument temporaries by their token, not the 4 or more it holds"
    plain=$(grep -o 'aether_unwind_forget((const void\*)&_ad_[0-9]*)' held.c | head -1)
    [ -z "$plain" ] || fail "held.ae's C still forgets an argument temporary unconditionally: $plain"
    tracked=$(grep -o '_ad_[0-9]*_j = aether_unwind_track_str_place' held.c | wc -l | tr -d ' ')
    [ "$tracked" -ge 2 ] || fail "held.ae's C keeps no token from a held string's track ($tracked)"
fi

for opt in "" --quick; do
    if ! "$AE" build $opt held.ae -o held > build.log 2>&1; then
        grep -i -m3 "error" build.log | cut -c1-200 | sed 's/^/        /'
        fail "held.ae did not build ($opt)"
    else
        out="$(./held 2>&1 | tr -d '\r')"
        want="$((1000 * (9 + 10) + 200 * (9 + 10))) 200 held-name"
        [ "$out" = "$want" ] || fail "held.ae ($opt) printed '$out', not '$want'"
    fi
done

if [ "$fails" -ne 0 ]; then
    exit 1
fi
echo "  [PASS] $NAME: held arguments forget by their journal token; a caught panic still releases the hold"
exit 0
