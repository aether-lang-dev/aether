#!/bin/sh
# #3435: what a program printed before a fatal error reaches its stdout.
#
# The runtime ends the process on an error it cannot return (a panic outside
# any try, a failed runtime check, a fatal std.log record, an
# out-of-memory it cannot report) with its message on stderr and abort().
# stdout is fully buffered when it is a file or a pipe, and glibc's abort()
# has flushed no stdio stream since 2.27, so the program's last lines were
# lost (emit_lib_generated_alloc's "caught:" line, on every Linux leg).
# macOS's abort() flushes, so this failed on Linux only.
#
# Each program prints two lines and then fails; its stdout goes to a file,
# which must hold both lines.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=fatal_flushes_stdout
[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
fails=0

# check <label> <main body>
check() {
    printf 'import std.log\n\nmain() {\n    println("first line")\n    println("second line")\n%s\n}\n' "$2" > "$T/p.ae"
    if ! "$AE" build "$T/p.ae" -o "$T/p" >"$T/build.log" 2>&1; then
        echo "    FAIL: $1: build failed"; sed 's/^/      /' "$T/build.log" | head -8
        fails=$((fails + 1)); return
    fi
    AETHER_STACK_TRACE=0 "$T/p" > "$T/out.txt" 2> "$T/err.txt"
    rc=$?
    got=$(tr -d '\r' < "$T/out.txt" | tr '\n' '|')
    if [ "$rc" -ne 0 ] && [ "$got" = "first line|second line|" ] && [ -s "$T/err.txt" ]; then
        echo "    ok: $1: both lines reached the file, then the error (exit $rc)"
    else
        echo "    FAIL: $1: exit $rc, stdout [$got], stderr [$(head -2 "$T/err.txt" | tr '\n' '|')]"
        fails=$((fails + 1))
    fi
}

check "a panic outside any try" '    panic("the end")'
check "a fatal log record" '    log.write(log.LOG_LEVEL_FATAL, "the end")'

# Another thread holding stdout (held.c): POSIX, where the flush tries the
# lock; Windows' C runtime has no try, and flushes.
held_note=""
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) held_note=" (another thread holding stdout: POSIX only)" ;;
    *)
        if ! "${CC:-cc}" -O1 -I"$ROOT/runtime/utils" -I"$ROOT/runtime" "$SCRIPT_DIR/held.c" \
                "$ROOT/runtime/utils/aether_bounds_check.c" -o "$T/held" -lpthread > "$T/held_cc.log" 2>&1; then
            echo "    FAIL: held.c did not build"; sed 's/^/      /' "$T/held_cc.log" | head -8
            fails=$((fails + 1))
        else
            "$T/held" 50 > "$T/held_out.txt" 2> "$T/held_err.txt"
            rc=$?
            got=$(tr -d '\r' < "$T/held_out.txt" | tr '\n' '|')
            if [ "$rc" -ne 0 ] && [ "$got" = "first line|second line|" ]; then
                echo "    ok: another thread holding stdout for 50 ms: the flush waits, both lines reached the file"
            else
                echo "    FAIL: another thread holding stdout for 50 ms: exit $rc, stdout [$got]"
                fails=$((fails + 1))
            fi
            # Bounded by a watchdog: the old fflush waited for ever. Not under
            # TSan (#3542): its interceptor of abort() flushes every stdio
            # stream first (fflush(NULL), compiler-rt tsan_interceptors_posix),
            # which waits for the lock this case holds for ever, so the abort
            # it checks is TSan's, not the program's. Every other leg runs it.
            if [ "${AETHER_SANITIZER:-}" = tsan ]; then
                held_note=" (another thread holding stdout for ever: not under TSan, whose abort flushes every stream)"
            else
                "$T/held" -1 > "$T/held_out2.txt" 2> "$T/held_err2.txt" &
                pid=$!
                i=0
                while kill -0 "$pid" 2>/dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
                if kill -0 "$pid" 2>/dev/null; then
                    # kill-ok: this case runs on POSIX only (skipped on Windows); $pid is held, still running
                    kill -9 "$pid" 2>/dev/null
                    echo "    FAIL: another thread holding stdout for ever: the abort waited past 10 s"
                    fails=$((fails + 1))
                else
                    wait "$pid"
                    rc=$?
                    if [ "$rc" -ne 0 ] && [ -s "$T/held_err2.txt" ]; then
                        echo "    ok: another thread holding stdout for ever: the flush gave up, the abort came (exit $rc)"
                    else
                        echo "    FAIL: another thread holding stdout for ever: exit $rc, stderr [$(head -2 "$T/held_err2.txt" | tr '\n' '|')]"
                        fails=$((fails + 1))
                    fi
                fi
            fi
        fi ;;
esac

[ "$fails" = 0 ] || { echo "  [FAIL] $NAME: $fails case(s)"; exit 1; }
echo "  [PASS] $NAME: a panic and a fatal log record leave the program's lines on its stdout, and another thread holding stdout neither loses them nor holds the abort$held_note"
