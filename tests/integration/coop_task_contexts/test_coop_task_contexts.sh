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
# A second build of the same list, with networking and a filesystem and with
# the sandbox added, runs the host's NET_MODES: std.net's pinned clock, the
# sandbox's levels and the HTTP client's connection pool, each used by two
# tasks at once. Every object of both builds is checked for thread-local
# storage (readelf on ELF, nm -m on Mach-O): a cooperative build has none of
# Aether's own, since ESP-IDF puts each thread-local on every FreeRTOS
# task's stack. Before this, std.net, std.fs, std.os and std.ipc kept
# thread-locals there, and the clock and the sandbox were one plain global
# for every task.
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

NET_LIBS=""
[ "$(uname -s)" = Linux ] && NET_LIBS="-ldl"
TMP="$(mktemp -d)"
trap '[ -n "${CTX_KEEP:-}" ] && echo "  kept $TMP" || rm -rf "$TMP" || true' EXIT
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
# The same list with networking and a filesystem, plus the sandbox (which the
# list leaves out: no freestanding build links it). Linked from an archive, as
# a program links libaether: the HTTP server's objects reach the event loop
# and the proxy, which a client alone does not pull in.
NDEFS="-DAETHER_NO_THREADING -DAETHER_NO_SIMD -DAETHER_NO_AFFINITY -DAETHER_NO_NUMA -DMAX_CORES=1"
NDEFS="$NDEFS -DQUEUE_SIZE=64 -DMAX_ACTORS_PER_CORE=256 -DAETHER_HAS_SANDBOX"
mkdir -p "$TMP/nobj"
nobjs=""
for f in $(grep -v '^#' "$ROOT/runtime/coop_runtime_sources.txt") runtime/aether_sandbox.c \
         runtime/sandbox/aether_audit.c; do
    o="$TMP/nobj/$(echo "$f" | tr / _).o"
    # shellcheck disable=SC2086
    if ! "$CC" -O1 -g -w $NDEFS $EXTRA $INC -c "$ROOT/$f" -o "$o" 2> "$TMP/cc.log"; then
        echo "  [FAIL] coop_task_contexts: $f did not compile with networking"
        head -10 "$TMP/cc.log" | sed 's/^/        /'
        exit 1
    fi
    nobjs="$nobjs $o"
done

# No thread-local storage in any object of either build.
# shellcheck disable=SC2086
sh "$SCRIPT_DIR/check_no_tls.sh" $objs $nobjs > "$TMP/tls.out"
case $? in
    0) tls_checked=1 ;;
    2) tls_checked=0 ;;
    *) echo "  [FAIL] coop_task_contexts: objects of a cooperative build keep thread-locals:"
       sed 's/^/        /' "$TMP/tls.out"
       exit 1 ;;
esac

# shellcheck disable=SC2086
"$CC" -O1 -g -w $DEFS $EXTRA $INC -c "$TMP/ctxlib.c" -o "$TMP/obj/ctxlib.o" 2> "$TMP/cc.log" &&
    "$CC" -O1 -g $EXTRA -I"$ROOT/include" -c "$SCRIPT_DIR/host.c" -o "$TMP/obj/host.o" 2>> "$TMP/cc.log" &&
    "$CC" $EXTRA -o "$TMP/host" "$TMP/obj/host.o" "$TMP/obj/ctxlib.o" $objs -lpthread -lm 2>> "$TMP/cc.log" &&
    "$CC" -O1 -g -w $NDEFS $EXTRA $INC -c "$TMP/ctxlib.c" -o "$TMP/nobj/ctxlib.o" 2>> "$TMP/cc.log" &&
    "$CC" -O1 -g $EXTRA -DNET_MODES -I"$ROOT/include" -c "$SCRIPT_DIR/host.c" -o "$TMP/nobj/host.o" 2>> "$TMP/cc.log" &&
    ar rc "$TMP/libnet.a" $nobjs 2>> "$TMP/cc.log" &&
    "$CC" $EXTRA -o "$TMP/host_net" "$TMP/nobj/host.o" "$TMP/nobj/ctxlib.o" "$TMP/libnet.a" $NET_LIBS -lpthread -lm 2>> "$TMP/cc.log"
if [ $? -ne 0 ]; then
    echo "  [FAIL] coop_task_contexts: the host did not build"
    head -10 "$TMP/cc.log" | sed 's/^/        /'
    exit 1
fi

# Each run is bounded by the sweep's own limit helper (#3154): timeout(1),
# gtimeout, or its perl stand-in on macOS, which GitHub's macOS runners need
# (they have no timeout(1): every mode failed there with exit 127).
. "$ROOT/tests/lib/sweep_timeout.sh"
sweep_timeout_bound TIMEOUT "$ROOT" 300
fail=0
HOST="$TMP/host"
expect() {   # mode, exit status wanted, stdout wanted, stderr text wanted ("" for none)
    # In a subshell of its own, so an expected abort is not reported here.
    # shellcheck disable=SC2086
    { ( $TIMEOUT "$HOST" "$1" > "$TMP/out" 2> "$TMP/err" ); rc=$?; } 2> /dev/null
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
HOST="$TMP/host_net"
expect clock 0 "clock: 0 wrong" ""
expect sandbox 0 "sandbox: 0 wrong" ""
expect pool 0 "pool: 600 requests from two tasks, 0 wrong, some idle" ""
expect tcpstop 0 "tcpstop: 0 wrong" ""

[ $fail -eq 0 ] || exit 1
echo "  [PASS] coop_task_contexts: two tasks on one cooperative runtime, each with its own context and account: quota, deadline, cancel and panic on one never reach the other's calls, outcomes, charges or caller identity; 2000 attach/detach rounds and 200 tasks ended attached reuse one cancel cell and leak nothing; an unattached task, or actors off the owning task, stop the program; std.net's pinned clock, the sandbox's levels and the HTTP connection pool are per task or locked; a native task's tcp.cancel and a std.tcp deadline stop an attached task's blocked read; $([ $tls_checked = 1 ] && echo "no object keeps a thread-local" || echo "thread-locals not checked: no readelf")${EXTRA:+ ($EXTRA)}"
