#!/bin/sh
# Integration test: `ae run <file.ae> -- <args>` forwards everything after
# the `--` separator to the running program's argv (like `cargo run --`).
# A config-is-code entry point (e.g. an Aether build supervisor) needs
# this so `ae run supervisor.ae -- make -j8` sees make/-j8 in its args.
#
# Asserts:
#   1. Args after `--` reach the program (they were dropped before).
#   2. A single arg containing spaces stays ONE token (double-quoted
#      through run_cmd's tokenizer), not split.
#   3. No `--` → no forwarded args (argc == 1).
#   4. A CACHE HIT forwards them too. `ae run` runs the cached exe on the
#      second invocation, and that path used to run it bare: the args
#      reached the program the first time and silently vanished every time
#      after, which is the shape a supervisor entry point actually runs in.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
MAIN="$SCRIPT_DIR/main.ae"

fail() { echo "  FAIL: $1"; exit 1; }

# A private cache, so this test controls cold vs warm without touching the
# shared one the rest of the parallel sweep is using.
TMPDIR_T=$(mktemp -d)
trap 'rm -rf "$TMPDIR_T"' EXIT
export AETHER_CACHE_DIR="$TMPDIR_T/cache"

# --- Case 1 + 2: forward three args, the middle one with a space -------
out=$("$AE" run "$MAIN" -- alpha "beta gamma" delta 2>&1)

echo "$out" | grep -q "argc=4" || fail "expected argc=4, got: $(echo "$out" | grep argc)"
echo "$out" | grep -qx "ARG:alpha"      || fail "missing ARG:alpha"
echo "$out" | grep -qx "ARG:beta gamma" || fail "spaces-arg was split or lost (expected 'ARG:beta gamma')"
echo "$out" | grep -qx "ARG:delta"      || fail "missing ARG:delta"

# --- Case 3: no `--` → nothing forwarded -------------------------------
out2=$("$AE" run "$MAIN" 2>&1)
echo "$out2" | grep -q "argc=1" || fail "expected argc=1 with no '--', got: $(echo "$out2" | grep argc)"

# --- Case 4: same command again, now a cache hit ----------------------
out3=$("$AE" run "$MAIN" -- alpha "beta gamma" delta 2>&1)

echo "$out3" | grep -q "argc=4" || fail "cache hit dropped the forwarded args, got: $(echo "$out3" | grep argc)"
echo "$out3" | grep -qx "ARG:beta gamma" || fail "cache hit split or lost the spaces-arg"

echo "  PASS: ae run forwards post-'--' args cold and cached (spaces preserved); none without '--'"
