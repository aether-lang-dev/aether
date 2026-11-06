#!/bin/sh
# tests/scripts/sweep_shard.sh (#2757, #2772): the shards CI splits the test
# sweep into cover the list exactly once between them, a list of shards
# (2,3/3) keeps each of them, weights split by cost (#3601), and a
# malformed AE_SWEEP_SHARD stops the sweep (status 2) instead of running
# some other share of it.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SHARD="$ROOT/tests/scripts/sweep_shard.sh"

# The sweep that runs this test may itself be a shard (CI exports
# AE_SWEEP_SHARD), and each case below sets the spec it means.
unset AE_SWEEP_SHARD

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() { echo "  [FAIL] sweep_shard: $1"; exit 1; }

i=1
while [ "$i" -le 20 ]; do echo "test_$i"; i=$((i + 1)); done > "$TMP/all.txt"

pick() { AE_SWEEP_SHARD="$1" sh "$SHARD" < "$TMP/all.txt"; }

[ "$(sh "$SHARD" < "$TMP/all.txt" | wc -l | tr -d ' ')" = 20 ] \
    || fail "without AE_SWEEP_SHARD every line should pass"

# Each split CI uses covers the 20 lines once, in order within a shard.
for split in "1/2 2/2" "1/3 2,3/3" "1/3 2/3 3/3"; do
    : > "$TMP/union.txt"
    for s in $split; do pick "$s" >> "$TMP/union.txt" || fail "$s exited $?"; done
    [ "$(sort "$TMP/union.txt")" = "$(sort "$TMP/all.txt")" ] \
        || fail "the shards $split do not cover the list once: $(tr '\n' ' ' < "$TMP/union.txt")"
done

[ "$(pick 1/3 | tr '\n' ' ')" = "test_1 test_4 test_7 test_10 test_13 test_16 test_19 " ] \
    || fail "1/3 should keep every third line from the first: $(pick 1/3 | tr '\n' ' ')"
[ "$(pick 2,3/3 | wc -l | tr -d ' ')" = 13 ] \
    || fail "2,3/3 should keep 13 of 20 lines, kept $(pick 2,3/3 | wc -l)"
[ "$(pick 2,1/3 | wc -l | tr -d ' ')" = 14 ] \
    || fail "2,1/3 should keep 14 of 20 lines, kept $(pick 2,1/3 | wc -l)"

# By cost (#3601): with weights for every unit, each goes, heaviest first,
# to the shard whose total is lowest so far (ties to the first), so the
# totals come out even; within a shard the heaviest come first.
printf '%s\n' a b c d e f > "$TMP/units.txt"
printf '%s\n' "50 a" "40 b" "30 c" "20 d" "10 e" "10 f" > "$TMP/weights.txt"
wpick() { AE_SWEEP_WEIGHTS="$TMP/weights.txt" AE_SWEEP_SHARD="$1" sh "$SHARD" < "$TMP/units.txt"; }
[ "$(wpick 1/2 | tr '\n' ' ')" = "a d e " ] || fail "1/2 by cost should be 'a d e', got '$(wpick 1/2 | tr '\n' ' ')'"
[ "$(wpick 2/2 | tr '\n' ' ')" = "b c f " ] || fail "2/2 by cost should be 'b c f', got '$(wpick 2/2 | tr '\n' ' ')'"
# A unit the weights do not name costs their median (the lower one, 20
# here), not nothing.
printf '%s\n' a b c d e f g > "$TMP/units.txt"
[ "$(wpick 1/2 | tr '\n' ' ')" = "a d g " ] || fail "with an unnamed unit, 1/2 should be 'a d g', got '$(wpick 1/2 | tr '\n' ' ')'"
[ "$(wpick 2/2 | tr '\n' ' ')" = "b c e f " ] || fail "with an unnamed unit, 2/2 should be 'b c e f', got '$(wpick 2/2 | tr '\n' ' ')'"
# AE_SWEEP_WEIGHTS set and empty: by position, whatever weights exist.
[ "$(AE_SWEEP_WEIGHTS= AE_SWEEP_SHARD=1/2 sh "$SHARD" < "$TMP/units.txt" | tr '\n' ' ')" = "a c e g " ] \
    || fail "with AE_SWEEP_WEIGHTS empty, 1/2 should split by position"

for bad in 0/2 3/2 2 /2 1/ a/2 1/0 ,1/3 1,/3 1,,2/3 1,4/3 1/3/3 "1 /3"; do
    out=$(pick "$bad" 2>&1)
    rc=$?
    [ "$rc" = 2 ] || fail "AE_SWEEP_SHARD='$bad' should stop with status 2, got $rc: $out"
done

echo "  [PASS] sweep_shard: the CI splits cover the sweep once, by position and by cost, lists of shards work, malformed specs stop the sweep"
