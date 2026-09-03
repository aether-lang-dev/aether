#!/bin/sh
# `ae version` and a failed build say when a newer release exists.
#
# The reason this exists: `ae upgrade` always worked, and nothing ever said to
# run it. Every check in `ae version` compared the binary against its own
# siblings and never against what had been released, so an install 75 releases
# behind reported itself healthy while `ae run` answered from its stale stdlib
# and reported functions added since as undefined.
#
# Hermetic: HOME points at a temp dir and the cache is seeded, so no case here
# reaches the network. That is also the property under test in the last case,
# since `ae version` must answer without it.
#
# Asserts:
#   - behind the latest release: `ae version` names it and points at upgrade
#   - level with it: silent
#   - the comparison is numeric, not lexicographic (0.1000.0 is NEWER than
#     0.627.0, and a string compare says the opposite)
#   - a failed build carries the same hint, on stderr, and prints its
#     diagnostics exactly once
#   - `ae run` does the same, since it has its own copy of that path
#   - level with it, a failed build stays silent
#   - a seeded cache is honoured, so no fetch happens

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] version_staleness: ae not built"; exit 0; }

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP" || :; return 0; }
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

HOME="$TMP/home"; export HOME
mkdir -p "$HOME/.aether/cache"
CACHE="$HOME/.aether/cache/latest_release"

CUR="$("$AE" version 2>/dev/null | head -1 | awk '{print $2}')"
[ -n "$CUR" ] || fail "could not read the current version"

seed() { printf 'v%s\n' "$1" > "$CACHE"; }

# --- behind: named, with the upgrade command ------------------------------
seed 99.0.0
OUT="$("$AE" version 2>&1)"
case "$OUT" in
  *"newer release is available: 99.0.0"*) ;;
  *) fail "behind: no nudge naming the release. got: $OUT" ;;
esac
case "$OUT" in
  *"ae upgrade"*) ;;
  *) fail "behind: nudge does not name \`ae upgrade\`" ;;
esac

# --- level: silent --------------------------------------------------------
seed "$CUR"
"$AE" version 2>&1 | grep -q "newer release" && fail "level: nudged anyway"

# --- numeric, not lexicographic ------------------------------------------
# 0.1000.0 is newer than any 0.6xx.0, and strcmp puts '1' below '6'. A string
# compare passes every case above and fails only this one.
case "$CUR" in
  0.*) seed 0.1000.0
       "$AE" version 2>&1 | grep -q "newer release is available: 0.1000.0" \
         || fail "0.1000.0 not seen as newer than $CUR (lexicographic compare?)" ;;
esac

# --- a failed build carries the hint, once, on stderr ---------------------
printf 'main() { x = no_such_function_anywhere() }\n' > "$TMP/bad.ae"
seed 99.0.0
ERR="$("$AE" build "$TMP/bad.ae" -o "$TMP/bad" 2>&1 1>/dev/null || :)"
case "$ERR" in
  *"newer release is available"*) ;;
  *) fail "failed build: no hint on stderr" ;;
esac
N="$(printf '%s\n' "$ERR" | grep -c 'no_such_function_anywhere' || :)"
[ "$N" -le 2 ] || fail "failed build: diagnostics repeated ($N mentions, expected the error once)"
printf '%s\n' "$ERR" | grep -q '\[diag\]' && fail "failed build: internal [diag] output shipped to the user"

# --- `ae run` fails the same way, once ------------------------------------
# A separate call site with its own copy of the failure path, so it needs its
# own case; the two drifted apart before, one carrying [diag] output.
seed 99.0.0
ERR="$("$AE" run "$TMP/bad.ae" 2>&1 1>/dev/null || :)"
case "$ERR" in
  *"newer release is available"*) ;;
  *) fail "failed run: no hint on stderr" ;;
esac
N="$(printf '%s\n' "$ERR" | grep -c 'no_such_function_anywhere' || :)"
[ "$N" -le 2 ] || fail "failed run: diagnostics repeated ($N mentions)"
printf '%s\n' "$ERR" | grep -q '\[diag\]' && fail "failed run: internal [diag] output shipped"

# --- level: a failed build stays silent -----------------------------------
seed "$CUR"
ERR="$("$AE" build "$TMP/bad.ae" -o "$TMP/bad" 2>&1 1>/dev/null || :)"
case "$ERR" in
  *"newer release is available"*) fail "level: failed build nudged anyway" ;;
esac

# --- the seeded cache is honoured, so nothing was fetched -----------------
[ "$(cat "$CACHE")" = "v$CUR" ] || fail "cache overwritten: a fetch happened despite a fresh cache"

echo "  [PASS] version_staleness"
