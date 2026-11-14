#!/bin/sh
# io.set_echo (nib-code ask aether-signal-flag-and-tty-echo): `nib login`
# reads an API key without echoing it. On a terminal, echo goes off and
# back on and each call reports what it found; on a pipe or a file nothing
# changes and nothing is an error. The terminal half runs the program on a
# pseudo-terminal from python3's os.openpty, and is skipped without one.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || AE="$ROOT/build/ae.exe"
[ -x "$AE" ] || { echo "  [SKIP] io_set_echo: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export AETHER_HOME="$ROOT"
fails=0

cat > "$TMP/echo.ae" <<'AE'
import std.io

main() {
    tty = io.is_terminal(0)
    a, e1 = io.set_echo(0, false)
    b, e2 = io.set_echo(0, false)
    c, e3 = io.set_echo(0, true)
    d, e4 = io.set_echo(0, true)
    println("tty=${tty} ${a} ${b} ${c} ${d} '${e1}${e2}${e3}${e4}'")
}
AE
if ! "$AE" build "$TMP/echo.ae" -o "$TMP/echo" >"$TMP/build.log" 2>&1; then
    echo "  [FAIL] io_set_echo: build failed"; sed 's/^/        /' "$TMP/build.log"; exit 1
fi
prog="$TMP/echo"
[ -x "$prog" ] || prog="$TMP/echo.exe"

# Not a terminal: nothing to change, no error.
out=$(echo "" | "$prog" 2>&1 | tr -d '\r')
if [ "$out" != "tty=false false false false false ''" ]; then
    echo "  [FAIL] io_set_echo: on a pipe, got '$out'"
    fails=$((fails + 1))
fi

# A terminal: echo starts on, goes off, comes back.
py=""
for cand in python3 python; do
    if $cand -c "import os; os.openpty()" >/dev/null 2>&1; then py="$cand"; break; fi
done
if [ -n "$py" ]; then
    out=$($py - "$prog" <<'PY' 2>&1 | tr -d '\r'
import os, subprocess, sys
m, s = os.openpty()
p = subprocess.run([sys.argv[1]], stdin=s, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
sys.stdout.write(p.stdout.decode())
PY
)
    if [ "$out" != "tty=true true false false true ''" ]; then
        echo "  [FAIL] io_set_echo: on a pseudo-terminal, got '$out'"
        fails=$((fails + 1))
    fi
else
    echo "  [SKIP] io_set_echo: no python3 with os.openpty for the terminal half"
fi

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] io_set_echo: echo goes off and back on a terminal; a pipe is left alone"
