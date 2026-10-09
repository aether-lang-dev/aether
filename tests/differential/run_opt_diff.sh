#!/bin/sh
# Differential testing across optimisation levels (#2488).
#
# `ae build` compiles the generated C at -O2, while `ae run` and
# `ae build --quick` compile it at -O0 -g, and the .ae corpus only ever ran at
# -O2. A bug that shows only at -O0 (a stack canary tripping on an overflow
# -O2 happens to hide, a read of uninitialised memory, a dangling borrow)
# passed every sweep and reached `ae run` users. Bugs of that shape: #1957,
# #2128, #2484.
#
# This builds every program `make test-ae` builds (tests/scripts/
# ae_sweep_list.sh) at both levels, runs both, and compares stdout and the
# exit code. A difference is a hard failure that prints both sides.
#
#   run_opt_diff.sh [extra-prune-file]
#
# NPROC sets the parallelism (default 4). OPT_DIFF_ONLY, an extended regex,
# narrows the list, for re-running the programs a sweep reported.
#
# Carveouts (opt_diff_carveouts.txt) are the programs whose stdout varies from
# run to run at a single level: clocks, pids, thread interleaving. They are
# reported with their reason on every run, and their exit codes are still
# compared, so a crash at one level is caught in them too.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CARVEOUTS="$SCRIPT_DIR/opt_diff_carveouts.txt"
cd "$ROOT" || exit 1

if [ ! -x "$ROOT/build/ae" ] && [ ! -x "$ROOT/build/ae.exe" ]; then
    echo "  [FAIL] opt-diff: build/ae is not built"
    exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/res" "$TMP/work"

# A carveout naming a program that no longer exists is an error, so the file
# cannot rot into a list of ghosts that excuse nothing.
stale=0
while read -r path _rest; do
    case "$path" in ''|'#'*) continue ;; esac
    if [ ! -f "$path" ]; then
        echo "  [FAIL] opt-diff: carveout '$path' names a program that does not exist"
        stale=1
    fi
done < "$CARVEOUTS"
[ "$stale" -eq 0 ] || exit 1

# Self-check: prove the comparison the per-program script uses can fail, or a
# broken one would make every program agree vacuously.
if ! sh "$SCRIPT_DIR/opt_diff_one.sh" --self-check "$TMP"; then
    echo "  [FAIL] opt-diff: the output comparison does not tell outputs apart"
    exit 1
fi

sh tests/scripts/ae_sweep_list.sh "${1:-}" > "$TMP/list.txt"
if [ -n "${OPT_DIFF_ONLY:-}" ]; then
    grep -E "$OPT_DIFF_ONLY" "$TMP/list.txt" > "$TMP/list.only" || true
    mv "$TMP/list.only" "$TMP/list.txt"
fi
total=$(wc -l < "$TMP/list.txt" | tr -d ' ')

echo "==================================="
echo "  .ae corpus at -O0 against -O2 ($total programs)"
echo "  Parallel: ${NPROC:-4} jobs"
echo "==================================="

xargs -P "${NPROC:-4}" -I{} sh "$SCRIPT_DIR/opt_diff_one.sh" "{}" "$TMP" "$ROOT" < "$TMP/list.txt"

count() { ls "$TMP/res" | grep -c "^$1_" ; }
same=$(count SAME)
diffs=$(count DIFF)
carved=$(count CARVED)
failed=$(count FAIL)

echo ""
if [ "$carved" -gt 0 ]; then
    echo "=== CARVED OUT (stdout not compared, exit codes agree) ==="
    for m in "$TMP"/res/CARVED_*; do
        n=${m##*/CARVED_}
        echo "  $n: $(cat "$TMP/res/$n.why")"
    done
    echo ""
fi
if [ "$diffs" -gt 0 ] || [ "$failed" -gt 0 ]; then
    echo "=== DIFFERENCES ==="
    for m in "$TMP"/res/DIFF_* "$TMP"/res/FAIL_*; do
        [ -f "$m" ] || continue
        n=${m##*/DIFF_}
        n=${n##*/FAIL_}
        echo "--- $n ---"
        sed 's/^/    /' "$TMP/res/$n.why"
        echo ""
    done
fi

echo "opt-diff: $same agree, $diffs differ, $failed failed to build, $carved carved out, $total total"
if [ $((same + diffs + carved + failed)) -ne "$total" ]; then
    echo "  [FAIL] opt-diff: $total programs listed but $((same + diffs + carved + failed)) recorded a result"
    exit 1
fi
if [ "$diffs" -gt 0 ] || [ "$failed" -gt 0 ]; then
    exit 1
fi
exit 0
