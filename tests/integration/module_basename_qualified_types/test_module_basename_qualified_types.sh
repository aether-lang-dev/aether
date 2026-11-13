#!/bin/sh
# Two modules with the same last segment (`a.timing`, `b.timing`), each
# exporting a struct of the same name (`Rec`) with a different layout. A
# qualified TYPE, `timing.Call`, `*timing.Model`, `timing.Rec[]`, means the
# module the file it is written in imports (nib-code ask
# module-basename-collision; qualified CALLS were fixed in #2268).
#
# The merge kept the two apart as namespaces `a_timing` and `b_timing` and
# resolved each file's `timing.` to its own import, but the typechecker's
# check of qualified types matched the qualifier against the namespace
# only, so every `timing.T` in every file that names one was refused:
# "unknown module 'timing' in the type 'timing.Call'", even in a file that
# imports just one of the two.
#
# Fixture: lib/a/timing, lib/b/timing, lib/c (imports a.timing: a pointer
# cast, a parameter, a struct field, a slice local, a literal); main.ae
# imports c and b.timing.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run --lib lib main.ae 2>&1 )
rc=$?
got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tr '\n' ' ' | sed 's/ *$//')
if [ "$rc" -ne 0 ] || [ "$got" != "1 2 b205 a10 3" ]; then
    echo "  [FAIL] module_basename_qualified_types: expected '1 2 b205 a10 3' (rc 0), got rc=$rc:"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi
echo "  [PASS] module_basename_qualified_types: each file's qualified types name its own import"
exit 0
