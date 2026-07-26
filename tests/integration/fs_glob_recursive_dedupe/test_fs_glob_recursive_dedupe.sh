#!/bin/sh
# Regression for issue #1279: fs.glob("<root>/**/<pat>") must not emit a
# root-level match twice. `**` matches zero-or-more directories, so a file
# directly under the glob root is a legitimate match — but it must appear
# exactly ONCE, not once from a base-dir scan plus once from the recursive
# walk. The fix removes the redundant base-dir scan and relies solely on
# the recursive walk (which already matches top-level files).
#
# Fixture:
#   <tmp>/.presubmit.ae      (root level)
#   <tmp>/sub/.build.ae      (one level down)
# Pattern "<tmp>/**/.*.ae" must return each file exactly once (2 total).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] fs_glob_recursive_dedupe: $AE not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

mkdir -p "$tmpdir/sub"
: > "$tmpdir/.presubmit.ae"
: > "$tmpdir/sub/.build.ae"

# The program globs the fixed relative pattern "./**/.*.ae", so run it with
# the fixture dir as the working directory. `ae run` does not forward extra
# argv to the program, hence the cwd approach rather than passing a path.
out="$(cd "$tmpdir" && "$AE" run "$SCRIPT_DIR/prog.ae" 2>&1)"
status=$?
if [ $status -ne 0 ]; then
    echo "  [FAIL] fs_glob_recursive_dedupe: program exited $status"
    echo "$out" | sed 's/^/    /'
    exit 1
fi

# Count total match lines and per-file occurrences.
total="$(printf '%s\n' "$out" | grep -c '\.ae$')"
root_hits="$(printf '%s\n' "$out" | grep -c '/\.presubmit\.ae$')"
sub_hits="$(printf '%s\n' "$out" | grep -c '/sub/\.build\.ae$')"

fail=0
if [ "$total" -ne 2 ]; then
    echo "  [FAIL] fs_glob_recursive_dedupe: expected 2 matches, got $total"
    fail=1
fi
if [ "$root_hits" -ne 1 ]; then
    echo "  [FAIL] fs_glob_recursive_dedupe: root-level .presubmit.ae appeared $root_hits time(s), expected 1 (issue #1279 double-emit)"
    fail=1
fi
if [ "$sub_hits" -ne 1 ]; then
    echo "  [FAIL] fs_glob_recursive_dedupe: sub/.build.ae appeared $sub_hits time(s), expected 1"
    fail=1
fi

if [ $fail -ne 0 ]; then
    echo "  --- glob output was: ---"
    printf '%s\n' "$out" | sed 's/^/    /'
    exit 1
fi

echo "  [PASS] fs_glob_recursive_dedupe: each match emitted exactly once"
exit 0
