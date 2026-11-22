#!/bin/sh
# std.tcp's per-connection deadline and cancel handle (deadline.ae says
# what each case is): connect, read and write held to one absolute
# deadline however slowly the peer trickles, a cancel from another thread
# waking each of them at once, ordinary traffic unaffected, and expiry and
# cancel racing close with decoy descriptors opened in the gap.
#
# Then the race check is shown able to fail: BROKEN_CANCELLER=1 adds a
# canceller that closes the connection's descriptor by number, the design
# this replaces, and a decoy must be found disturbed (POSIX only: on
# Windows close() does not close a socket).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=tcp_deadline_cancel

[ -x "$AE" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || :' EXIT

"$AE" build "$SCRIPT_DIR/deadline.ae" -o "$TMP/deadline" >"$TMP/build.log" 2>&1 \
    || { sed -n '1,20p' "$TMP/build.log"; echo "  [FAIL] $NAME: did not build"; exit 1; }
"$TMP/deadline" >"$TMP/out" 2>&1
rc=$?
sed 's/^/    /' "$TMP/out"
if [ "$rc" != 0 ] || [ "$(tail -1 "$TMP/out")" != PASS ]; then
    echo "  [FAIL] $NAME"
    exit 1
fi
# Under ThreadSanitizer the broken canceller is a real race, there on
# purpose, and TSan reports it (its close against the owner's connect): the
# demonstration is left to the plain runs, which every other lane makes.
broken_demo=1
[ "${AETHER_SANITIZER:-}" = tsan ] && broken_demo=0
case "$broken_demo:$(uname -s)" in
    0:*|*:MINGW*|*:MSYS*|*:CYGWIN*) ;;
    *)
        BROKEN_CANCELLER=1 RACE_ROUNDS=400 "$TMP/deadline" >"$TMP/broken" 2>&1
        if grep -q '^FAIL expiry and cancel racing close.*a decoy was disturbed' "$TMP/broken"; then
            echo "    ok: a canceller that closes by number is caught ($(grep -c . "$TMP/broken") lines)"
        else
            echo "    FAIL: the race check did not catch a canceller that closes by number"
            sed 's/^/      /' "$TMP/broken" | tail -3
            echo "  [FAIL] $NAME"
            exit 1
        fi
        ;;
esac
echo "  [PASS] $NAME"
