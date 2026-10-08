#!/bin/sh
# Regression (#2534, #2535): a command ae runs reaches the program whole,
# however many arguments it has and whatever they hold.
#
# 1. `ae build` with a `[build] cflags` of 400 defines plus one that comes
#    from `${AETHER_LONG_CMD_TAIL}`, which a C shim requires, on one line
#    with a trailing comment. The compile command then carries more than
#    511 arguments, where the spawners' fixed table stopped and dropped the
#    rest; the line is far past the 512 bytes the aether.toml reader split
#    it at, and the expanded value past the 512 it was then cut to; and the
#    comment is not part of the value.
# 2. `ae run x.ae -- <args>`: 606 arguments, among them one with a space,
#    one with quotes, one ending in a backslash, an empty one, one with an
#    `&` and one with a tab, reach the program exactly. They go to the
#    spawn as a vector; the command string they went through before
#    dropped what did not fit its buffer and could not carry a quote.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] ae_long_command: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
AETHER_CACHE_DIR="$TMP/cache"
export AETHER_CACHE_DIR
fail=0

# --- 1. a compile command past 511 arguments and a long cflags value ---
mkdir -p "$TMP/proj"
cd "$TMP/proj" || exit 1
cat > probe.ae <<'AEOF'
extern long_cmd_marker() -> int

main() {
    println("marker=${long_cmd_marker()}")
}
AEOF
cat > shim.c <<'COF'
#if !defined(AE_LC_0) || !defined(AE_LC_399)
#error "a [build] cflags define did not reach the compiler"
#endif
#ifndef AE_LONG_CMD_ENV
#error "the define from ${AETHER_LONG_CMD_TAIL} did not reach the compiler"
#endif
int long_cmd_marker(void) { return AE_LONG_CMD_ENV; }
COF
{
    printf '[build]\ncflags = "'
    i=0
    while [ $i -lt 400 ]; do printf -- '-DAE_LC_%d=1 ' "$i"; i=$((i + 1)); done
    printf '${AETHER_LONG_CMD_TAIL}"  # 401 defines, one from the environment\n\n'
    printf '[[bin]]\nname = "probe"\npath = "probe.ae"\nextra_sources = ["shim.c"]\n'
} > aether.toml

if ! AETHER_LONG_CMD_TAIL="-DAE_LONG_CMD_ENV=42" "$AE" build probe.ae -o "$TMP/probe" \
        >"$TMP/build.log" 2>&1; then
    echo "  [FAIL] ae_long_command: the build with 401 cflags defines failed"
    sed 's/^/        /' "$TMP/build.log" | head -10
    fail=1
else
    got=$("$TMP/probe" 2>&1 | tr -d '\r')
    if [ "$got" != "marker=42" ]; then
        echo "  [FAIL] ae_long_command: the probe printed '$got', expected 'marker=42'"
        fail=1
    else
        echo "  [PASS] ae_long_command: 401 cflags defines reach the compiler"
    fi
fi

# --- 2. ae run forwards every argument exactly ---
# Outside the project above, so its aether.toml does not apply.
cd "$TMP" || exit 1
cat > "$TMP/args.ae" <<'AEOF'
import std.os

main() {
    n = os.args_count()
    println("count=${n - 1}")
    i = 1
    while i < n {
        println("[${os.args_get(i)}]")
        i = i + 1
    }
}
AEOF
tab="$(printf '\t')"
set -- "a b" 'say "hi"' 'dir with space\' '' 'x&y' "tab${tab}in"
i=0
while [ $i -lt 600 ]; do set -- "$@" "n$i"; i=$((i + 1)); done
{
    printf '%s\n' "count=606"
    for a in "$@"; do printf '[%s]\n' "$a"; done
} > "$TMP/expected.txt"
"$AE" run "$TMP/args.ae" -- "$@" 2>"$TMP/run.err" | tr -d '\r' > "$TMP/got.txt"
if [ "$(cat "$TMP/expected.txt")" = "$(cat "$TMP/got.txt")" ]; then
    echo "  [PASS] ae_long_command: ae run forwards 606 arguments exactly"
else
    echo "  [FAIL] ae_long_command: ae run did not forward the arguments exactly; it printed:"
    head -8 "$TMP/got.txt" | sed 's/^/        /'
    sed 's/^/        /' "$TMP/run.err" | head -5
    fail=1
fi

exit $fail
