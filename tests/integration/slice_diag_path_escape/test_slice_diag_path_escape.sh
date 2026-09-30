#!/bin/sh
# Issue #2305: a slice bounds check names its source file in a C string
# literal, and the path was written into the generated C unescaped. A Windows
# path's backslashes became escape sequences: gcc warned "unknown escape
# sequence" once per checked access (about 1,200 times building ae3d), and a
# `\a` silently became BEL, so the panic printed a mangled path.
#
# The program indexes a slice past its end from a source file whose path has
# backslashes: a native Windows path (D:\...) on Windows, and on POSIX a
# directory named with literal backslashes, one of them `\a`. The build must
# not warn about escapes, and the panic must name the path exactly.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] slice_diag_path_escape: $AE not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        mkdir -p "$TMP/src"
        cp "$SCRIPT_DIR/oob.ae" "$TMP/src/oob.ae"
        src="$(cygpath -w "$TMP/src/oob.ae")"
        ;;
    *)
        dir="$TMP/"'back\slash\at'
        mkdir -p "$dir"
        cp "$SCRIPT_DIR/oob.ae" "$dir/oob.ae"
        src="$dir/oob.ae"
        ;;
esac

if ! AETHER_HOME="$ROOT" "$AE" build "$src" -o "$TMP/oob" > "$TMP/build.log" 2>&1; then
    echo "  [FAIL] slice_diag_path_escape: build failed"
    head -10 "$TMP/build.log" | sed 's/^/          /'
    exit 1
fi
if grep -q "unknown escape sequence" "$TMP/build.log"; then
    echo "  [FAIL] slice_diag_path_escape: the generated C wrote the path unescaped"
    grep -m 3 "unknown escape sequence" "$TMP/build.log" | sed 's/^/          /'
    exit 1
fi

"$TMP/oob" > "$TMP/run.log" 2>&1
want="$src:5: slice index 5 out of range for length 3"
if ! grep -qF "$want" "$TMP/run.log"; then
    echo "  [FAIL] slice_diag_path_escape: the panic did not name the path exactly"
    echo "          want: $want"
    head -5 "$TMP/run.log" | sed 's/^/          got:  /'
    exit 1
fi

echo "  [PASS] slice_diag_path_escape: a backslash path is escaped and reported exactly"
exit 0
