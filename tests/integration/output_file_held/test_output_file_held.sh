#!/bin/sh
# #3574: aetherc writes its output while another process holds the file
# without sharing it for writing, as a virus scanner, an indexer or an
# editor on Windows holds a file it has just seen written. The open failed
# with a sharing violation ("Error opening output file: Permission
# denied"), and a sweep writing one output path a program after another
# lost about one compile in a thousand to it. A hold is released within
# moments, so aetherc waits for it, up to about two seconds; a file it
# cannot write for good (here, a directory where the file should be) still
# fails at once, naming the path.
#
# Windows only: POSIX files are not held this way.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
NAME=output_file_held

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) ;;
    *) echo "  [SKIP] $NAME: Windows only (a file held without write sharing)"; exit 0 ;;
esac
if [ ! -x "$AETHERC" ] && [ ! -x "$AETHERC.exe" ]; then
    echo "  [SKIP] $NAME: aetherc not built"
    exit 0
fi
if ! command -v gcc >/dev/null 2>&1; then
    echo "  [SKIP] $NAME: no gcc for the holder"
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fails=0

if ! gcc -O2 "$SCRIPT_DIR/holder.c" -o "$WORK/holder.exe" 2>"$WORK/gcc.log"; then
    echo "  [FAIL] $NAME: the holder did not build:"
    sed 's/^/        /' "$WORK/gcc.log" | head -5
    exit 1
fi
printf 'main() {\n    println("held")\n}\n' > "$WORK/main.ae"

# The output exists and is held for 700 ms from before aetherc opens it.
printf '/* an earlier compile */\n' > "$WORK/out.c"
"$WORK/holder.exe" "$WORK/out.c" "$WORK/held" 700 &
holder=$!
i=0
while [ ! -f "$WORK/held" ] && [ $i -lt 200 ]; do sleep 0.05; i=$((i + 1)); done
if [ ! -f "$WORK/held" ]; then
    echo "  [FAIL] $NAME: the holder never held the file"
    exit 1
fi
(cd "$WORK" && AETHER_HOME="$ROOT" "$AETHERC" main.ae out.c > "$WORK/aetherc.log" 2>&1)
rc=$?
wait "$holder"
if [ $rc -ne 0 ] || ! grep -q 'held' "$WORK/out.c"; then
    echo "  [FAIL] $NAME: aetherc did not wait for the held output (rc $rc):"
    sed 's/^/        /' "$WORK/aetherc.log" | head -5
    fails=$((fails + 1))
fi

# A path it can never write: refused at once, naming it.
mkdir -p "$WORK/dir.c"
start=$(date +%s)
(cd "$WORK" && AETHER_HOME="$ROOT" "$AETHERC" main.ae dir.c > "$WORK/dir.log" 2>&1)
rc=$?
took=$(( $(date +%s) - start ))
if [ $rc -eq 0 ] || ! grep -q "Error opening output file 'dir.c'" "$WORK/dir.log"; then
    echo "  [FAIL] $NAME: a directory as the output was not refused by name (rc $rc):"
    sed 's/^/        /' "$WORK/dir.log" | head -5
    fails=$((fails + 1))
fi
if [ "$took" -gt 3 ]; then
    echo "  [FAIL] $NAME: refusing an output it can never write took ${took}s"
    fails=$((fails + 1))
fi

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] $NAME: a held output is waited for and written; one that cannot be is refused at once, by name"
exit 0
