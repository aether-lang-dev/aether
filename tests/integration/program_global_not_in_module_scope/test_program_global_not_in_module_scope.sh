#!/bin/sh
# A program's top-level `var` is not in an imported module's scope
# (aether-ui asks/aether-program-global-leaks-into-module-scope.md; aether's
# asks/REPLY-aether-program-global-leaks-into-module-scope.md).
#
# A module function's bare `n = ...` binds its own local. The checker and
# codegen resolved it to the importing program's `var n` instead:
#
#   probe_fs_typecheck  `var n = 0` + `import std.fs`: std.fs's pwrite
#                       (`n = fs_pwrite_raw(...)`, 64-bit) failed to
#                       type-check against the program's 32-bit `n` (E0200,
#                       since 0.801 checks every function of every module)
#   probe_fs_corrupt    `var ok = 42`; fs.delete's `ok = file_delete_raw(..)`
#                       wrote the program's global: it printed ok=0
#   probe_module        the same for a module's local (count) and closure-
#                       captured local (total), beside a module global and a
#                       program global of one name (hits), which stay apart
#
# Each probe must print exactly the program's values unchanged.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
NAME=program_global_not_in_module_scope
EXE="${EXE_EXT:-}"
if [ -z "$EXE" ] && [ ! -x "$ROOT/build/ae" ] && [ -x "$ROOT/build/ae.exe" ]; then
    EXE=".exe"
fi
AE="$ROOT/build/ae$EXE"
[ -x "$AE" ] || { echo "  [SKIP] $NAME: build/ae not built"; exit 0; }

cd "$ROOT" || exit 1
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT

fails=0
# check <probe> <expected output, newline-separated>
check() {
    if ! AETHER_LIB_DIR="$SCRIPT_DIR/lib" "$AE" run "$SCRIPT_DIR/$1.ae" > "$TMP/$1.log" 2>&1; then
        echo "  [FAIL] $NAME: $1 did not build or run"
        grep -v '^ *|' "$TMP/$1.log" | head -6 | sed 's/^/        /'
        fails=$((fails + 1))
        return
    fi
    tr -d '\r' < "$TMP/$1.log" | grep -v -e '^Type checking' -e '^warning' -e '^ *-->' -e '^ *[0-9]* |' -e '^ *|' -e '^$' > "$TMP/$1.out"
    printf '%s\n' "$2" > "$TMP/$1.want"
    if ! diff "$TMP/$1.want" "$TMP/$1.out" > "$TMP/$1.diff"; then
        echo "  [FAIL] $NAME: $1 printed the wrong values (a module wrote the program's global?)"
        sed 's/^/        /' "$TMP/$1.diff"
        fails=$((fails + 1))
    fi
}

check probe_fs_typecheck "n=1"
check probe_fs_corrupt "ok=42"
check probe_module "bump=8 closure=3 module_hits=2
count=5 total=9 hits=100"

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] $NAME: a program's globals stay out of its modules' scope"
exit 0
