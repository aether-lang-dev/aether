#!/bin/sh
# Keeps the test units on stdin that a select file picks, for a sweep that
# runs part of the corpus by what the tests exercise rather than by where
# they sit (#3542: `make test-tsan` runs the actor, scheduler and std.net
# tests). Run from the repository root.
#
#   sweep_select.sh [select-file] < units
#
# A unit is a .ae file or a shell test's directory, one per line, as the
# `make test-ae` sweep lists them. Without a select file every unit passes,
# and nothing is read. Each line of the file is one of
#
#   path:<ERE>   the unit's path matches the extended regular expression
#   text:<ERE>   the unit's source matches it: the .ae file itself, or for a
#                directory any .ae, .sh or .c file under it
#
# and a unit either kind picks is kept, in the order it came. `#` starts a
# comment line; blank lines are skipped. Any other line is a mistake that
# would pick nothing, so it stops the run (exit 2) and says which line.
set -u

sel="${1:-}"
[ -n "$sel" ] || exec cat

if [ ! -r "$sel" ]; then
    echo "sweep_select.sh: cannot read the select file $sel" >&2
    exit 2
fi
bad=$(sed -n '/^#/d; /^[[:space:]]*$/d; /^path:./d; /^text:./d; =' "$sel")
if [ -n "$bad" ]; then
    for n in $bad; do
        echo "sweep_select.sh: $sel:$n is neither path:<ERE> nor text:<ERE>: $(sed -n "${n}p" "$sel")" >&2
    done
    exit 2
fi

tmp=$(mktemp -d) || exit 2
trap 'rm -rf "$tmp"' EXIT
sed -n 's/^path://p' "$sel" > "$tmp/path"
sed -n 's/^text://p' "$sel" > "$tmp/text"

while IFS= read -r unit; do
    [ -n "$unit" ] || continue
    if [ -s "$tmp/path" ] && printf '%s\n' "$unit" | grep -q -E -f "$tmp/path"; then
        printf '%s\n' "$unit"
        continue
    fi
    [ -s "$tmp/text" ] || continue
    if [ -d "$unit" ]; then
        found=$(find "$unit" -type f \( -name '*.ae' -o -name '*.sh' -o -name '*.c' \) \
                    -exec grep -l -E -f "$tmp/text" {} + 2>/dev/null | head -1)
        [ -n "$found" ] && printf '%s\n' "$unit"
    elif grep -q -E -f "$tmp/text" "$unit" 2>/dev/null; then
        printf '%s\n' "$unit"
    fi
done
exit 0
