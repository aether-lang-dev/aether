#!/bin/sh
# Task execution contexts (docs/design/task-execution-contexts.md): two
# tasks entering one cooperative (AETHER_NO_THREADING) runtime at once, each
# under its own context and execution account. POSIX threads stand in for
# FreeRTOS tasks; the runtime is the cooperative one an ESP32 links (every
# file runtime/coop_runtime_sources.txt lists), built here with the host's C
# compiler. host.c says what each mode drives and checks.
#
# Before task contexts every task shared one block of execution state: the
# same host built without a port (-DNO_CONTEXT_API) crashes or returns
# crossed outcomes and caller identities in every run.
#
# CTX_CFLAGS adds flags to every compile and the link (the sanitizer runs:
# CTX_CFLAGS="-fsanitize=address,undefined" or "-fsanitize=thread").

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
CC="${CC:-cc}"
EXTRA="${CTX_CFLAGS:-}"
export AETHER_HOME="$ROOT"
export AETHER_STACK_TRACE=0

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] coop_task_contexts: the stand-in tasks are POSIX threads"; exit 0 ;;
esac
[ -x "$AETHERC" ] || { echo "  [SKIP] coop_task_contexts: $AETHERC not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT
mkdir -p "$TMP/obj"

if ! "$AETHERC" --emit=lib "$SCRIPT_DIR/ctxlib.ae" "$TMP/ctxlib.c" > "$TMP/gen.log" 2>&1; then
    echo "  [FAIL] coop_task_contexts: aetherc --emit=lib failed"
    tail -5 "$TMP/gen.log" | sed 's/^/        /'
    exit 1
fi

INC="-I$ROOT/include -I$ROOT/runtime -I$ROOT/runtime/actors -I$ROOT/runtime/scheduler -I$ROOT/runtime/utils"
INC="$INC -I$ROOT/runtime/memory -I$ROOT/runtime/config -I$ROOT/std -I$ROOT/std/string -I$ROOT/std/io"
INC="$INC -I$ROOT/std/math -I$ROOT/std/net -I$ROOT/std/collections -I$ROOT/std/json"
# The ESP32 lane's defines, as far as a hosted build takes them.
DEFS="-DAETHER_NO_THREADING -DAETHER_NO_NETWORKING -DAETHER_NO_FILESYSTEM -DAETHER_NO_SIMD"
DEFS="$DEFS -DAETHER_NO_AFFINITY -DAETHER_NO_NUMA -DMAX_CORES=1 -DQUEUE_SIZE=64 -DMAX_ACTORS_PER_CORE=256"
objs=""
for f in $(grep -v '^#' "$ROOT/runtime/coop_runtime_sources.txt"); do
    o="$TMP/obj/$(echo "$f" | tr / _).o"
    # shellcheck disable=SC2086
    if ! "$CC" -O1 -g -w $DEFS $EXTRA $INC -c "$ROOT/$f" -o "$o" 2> "$TMP/cc.log"; then
        echo "  [FAIL] coop_task_contexts: $f did not compile"
        head -10 "$TMP/cc.log" | sed 's/^/        /'
        exit 1
    fi
    objs="$objs $o"
done
# shellcheck disable=SC2086
"$CC" -O1 -g -w $DEFS $EXTRA $INC -c "$TMP/ctxlib.c" -o "$TMP/obj/ctxlib.o" 2> "$TMP/cc.log" &&
    "$CC" -O1 -g $EXTRA -I"$ROOT/include" -c "$SCRIPT_DIR/host.c" -o "$TMP/obj/host.o" 2>> "$TMP/cc.log" &&
    "$CC" $EXTRA -o "$TMP/host" "$TMP/obj/host.o" "$TMP/obj/ctxlib.o" $objs -lpthread -lm 2>> "$TMP/cc.log"
if [ $? -ne 0 ]; then
    echo "  [FAIL] coop_task_contexts: the host did not build"
    head -10 "$TMP/cc.log" | sed 's/^/        /'
    exit 1
fi

fail=0
expect() {   # mode, exit status wanted, stdout wanted, stderr text wanted ("" for none)
    # In a shell of its own, so an expected abort is not reported here.
    sh -c 'exec timeout 300 "$0" "$1" > "$2" 2> "$3"' "$TMP/host" "$1" "$TMP/out" "$TMP/err" 2> /dev/null
    rc=$?
    got="$(tr -d '\r' < "$TMP/out")"
    if [ "$got" != "$3" ] || { [ "$2" = 0 ] && [ $rc -ne 0 ]; } || { [ "$2" != 0 ] && [ $rc -eq 0 ]; } ||
       { [ -n "$4" ] && ! grep -qF "$4" "$TMP/err"; }; then
        echo "  [FAIL] coop_task_contexts $1 (exit $rc):"
        printf '%s\n' "$got" | sed 's/^/        got: /'
        printf '%s\n' "$3" | sed 's/^/        want: /'
        head -10 "$TMP/err" | sed 's/^/        /'
        fail=1
    fi
}

two='interrupted task: 60 rounds, 0 wrong outcomes
worker task: some rounds, 0 wrong results'
expect all 0 "$two" ""
expect deadline 0 "$two" ""
expect caller 0 "$two" ""
expect churn 0 "churn: 2000 attach/detach rounds, 200 threads ended attached, 0 wrong" ""
expect unattached 1 "" "a task with no execution context entered the runtime"
expect actors 1 "owner's ask: 42" "on a task that does not own the actors"

[ $fail -eq 0 ] || exit 1
echo "  [PASS] coop_task_contexts: two tasks on one cooperative runtime, each with its own context and account: quota, deadline, cancel and panic on one never reach the other's calls, outcomes, charges or caller identity; 2000 attach/detach rounds and 200 tasks ended attached reuse one cancel cell and leak nothing; an unattached task, or actors off the owning task, stop the program${EXTRA:+ ($EXTRA)}"
