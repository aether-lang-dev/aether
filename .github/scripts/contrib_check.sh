#!/usr/bin/env bash
# contrib_check.sh — build + RUN every contrib test_*.ae (not just type-check).
#
# The nightly used to only `ae check` (type-check) contrib modules — which is
# exactly what failed to catch the tinyweb WebSocket server rot (it compiled
# fine; it was runtime-broken). This runs the actual tests, so runtime and (with
# VALGRIND=1) leak regressions surface.
#
# Usage:
#   .github/scripts/contrib_check.sh            # build + run each test
#   VALGRIND=1 .github/scripts/contrib_check.sh # ... under valgrind (leak gate)
#
# Assumes ./build/ae is already built (the Makefile target depends on it).
# Exits nonzero if any contrib test fails to build, fails at runtime, or (under
# valgrind) leaks. Each test binary is expected to self-terminate — the tinyweb
# server tests run an in-process client+server and exit(0)/exit(1) on their own.
set -u

AE="./build/ae"
EXE_EXT="${EXE_EXT:-}"
VALGRIND="${VALGRIND:-0}"
VG="valgrind --leak-check=full --error-exitcode=99 --errors-for-leak-kinds=definite"

rc=0
run_dir="build/contrib-check"
mkdir -p "$run_dir"

# Each entry: "<label>|<test.ae>|<extra C files>|<leakgate>".
#   leakgate = "leak" : under VALGRIND=1 this test is also a LEAK gate.
#   leakgate = "run"  : run for correctness only; excluded from the leak gate.
# Extra C files are passed via --extra. Keep this table in sync as contrib
# modules gain tests.
#
# Why some tests are run-only under valgrind: the tinyweb tests were written to
# `exit()` on completion (and the server tests keep an actor blocked in an
# accept loop), so the process is torn down without running scope-end heap
# cleanup — valgrind reports the still-live buffers as "leaks" even though the
# code is correct. Those tests are gated for RUNTIME correctness (the thing that
# actually caught the WS rot); making them leak-clean is separate follow-up
# work. i18n/collate was written leak-clean by design, so it IS leak-gated.
AVC="contrib/avcodec"
TW="contrib/tinyweb"
I18N="contrib/i18n"
TESTS=(
  # avcodec: needs FFmpeg's dev libraries to link and the ffmpeg BINARY to
  # generate its clip; the test SKIPs cleanly without the latter.
  "avcodec/decode|$AVC/test_avcodec.ae|$AVC/aether_avcodec.c|run"
  "tinyweb/spec|$TW/test_spec.ae||run"
  "tinyweb/inventory|$TW/test_inventory.ae|$TW/ws_handshake.c|run"
  "tinyweb/integration|$TW/test_integration.ae|$TW/ws_handshake.c|run"
  "tinyweb/schema_api|$TW/test_schema_api.ae|$TW/ws_handshake.c|run"
  "tinyweb/websocket|$TW/test_websocket.ae|$TW/ws_handshake.c|run"
  "i18n/collate|$I18N/collate/test_collate.ae|$I18N/aether_i18n.c $I18N/utf8proc/utf8proc.c $I18N/ducet/ducet_data.c|leak"
)

# Kill any stray cache/test binaries squatting ports before we start (aborted
# server tests orphan busy-looping binaries — the hidden cause of bind flakes).
reap_orphans() {
  pkill -9 -f 'contrib-check/' 2>/dev/null || true
}
reap_orphans

for entry in "${TESTS[@]}"; do
  IFS='|' read -r label src extras leakgate <<< "$entry"

  if [ ! -f "$src" ]; then
    printf '  SKIP  %-22s (%s not found)\n' "$label" "$src"
    continue
  fi

  safe="$(echo "$label" | tr '/' '_')"
  out="$run_dir/${safe}${EXE_EXT}"
  log="$run_dir/${safe}.log"
  # assemble --extra flags
  extra_flags=""
  for c in $extras; do extra_flags="$extra_flags --extra $c"; done

  if ! berr="$("$AE$EXE_EXT" build "$src" $extra_flags -o "$out" 2>&1)"; then
    printf '  FAIL  %-22s (build)\n' "$label"
    echo "$berr" | grep -iE "error" | head -5
    rc=1
    continue
  fi

  # Run (optionally under valgrind), detached from any tty, with a hard timeout
  # so a hung server test can never wedge the nightly. Valgrind applies only to
  # tests flagged as a leak gate (see the table's leakgate column).
  use_vg=0
  [ "$VALGRIND" = "1" ] && [ "$leakgate" = "leak" ] && use_vg=1
  if [ "$use_vg" = "1" ]; then
    runner="$VG $out"
  else
    runner="$out"
  fi
  if timeout 120 $runner > "$log" 2>&1 < /dev/null; then
    if [ "$use_vg" = "1" ]; then
      printf '  PASS  %-22s (run + valgrind)\n' "$label"
    elif [ "$VALGRIND" = "1" ]; then
      printf '  PASS  %-22s (run; leak-gate n/a)\n' "$label"
    else
      printf '  PASS  %-22s (run)\n' "$label"
    fi
  else
    code=$?
    if [ "$code" = "99" ]; then
      printf '  FAIL  %-22s (valgrind: leak/error)\n' "$label"
      grep -E "definitely lost|ERROR SUMMARY" "$log" | tail -3
    elif [ "$code" = "124" ]; then
      printf '  FAIL  %-22s (timeout — did not terminate)\n' "$label"
    else
      printf '  FAIL  %-22s (run, exit %s)\n' "$label" "$code"
      grep -iE "fail" "$log" | head -5
    fi
    rc=1
  fi
  reap_orphans
done

vg_note=""
[ "$VALGRIND" = "1" ] && vg_note=" (valgrind)"
if [ "$rc" = "0" ]; then
  echo "contrib-check: all contrib tests passed${vg_note}"
else
  echo "contrib-check: one or more contrib tests FAILED"
fi
exit $rc
