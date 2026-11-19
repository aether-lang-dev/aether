#!/bin/sh
# fs.write_atomic(path) under the sandbox preload, enforcement live.
#
# write_atomic creates <path>.tmp.<pid>.<n> and renames it over <path>.
# std checks <path>; the preload used to check the temporary's own name, so a
# write grant on <path> was refused there ("Permission denied", seen in sae's
# services/documents) though std had allowed it. Now std stages the
# temporary (aether_sandbox_stage) and the preload checks <path> for that one
# open, under an in-process level and a grant file alike, and for nothing
# else: stage_guard.c holds the preload to the temporary's exact shape.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LIB="$ROOT/build/libaether_sandbox.so"
AE="$ROOT/build/ae"

case "$(uname -s 2>/dev/null)" in
    Linux|FreeBSD) ;;
    *)
        echo "  [SKIP] sandbox_preload_write_atomic: preload is Linux and FreeBSD only"
        exit 0 ;;
esac
if [ ! -f "$LIB" ]; then
    echo "  [SKIP] sandbox_preload_write_atomic: preload library not built"
    exit 0
fi
if [ ! -x "$AE" ]; then
    echo "  [SKIP] sandbox_preload_write_atomic: $AE not built"
    exit 0
fi

TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT
if ! "$AE" build "$SCRIPT_DIR/atomic.ae" -o "$TMP/atomic" > "$TMP/build.log" 2>&1; then
    echo "  [FAIL] sandbox_preload_write_atomic: atomic.ae did not build"
    cat "$TMP/build.log"
    exit 1
fi
if ! ${CC:-cc} -std=c11 -Wall -Wextra -Werror -rdynamic \
        "$SCRIPT_DIR/stage_guard.c" -o "$TMP/stage_guard"; then
    echo "  [FAIL] sandbox_preload_write_atomic: stage_guard did not compile"
    exit 1
fi
fail=0

pass() { echo "  [PASS] $1"; }
flunk() {
    echo "  [FAIL] $1"
    sed 's/^/           /' "$TMP/out" "$TMP/err"
    fail=1
}

# expect LABEL LINE...: every LINE is in $TMP/out
expect() {
    label="$1"
    shift
    for want in "$@"; do
        if ! grep -qxF "$want" "$TMP/out"; then
            flunk "$label: no line '$want'"
            return
        fi
    done
    pass "$label"
}

# No temporaries left beside the file.
no_leftovers() {
    if ls "$1" | grep -q '\.tmp\.'; then
        echo "  [FAIL] a temporary was left in $1: $(ls "$1")"
        fail=1
    fi
}

# 1. Without the preload: std's own check (control).
mkdir "$TMP/d1"
"$TMP/atomic" enforce "$TMP/d1/doc.json" "$TMP/d1/other.json" > "$TMP/out" 2> "$TMP/err"
expect "no preload: write_atomic of the granted file" "granted ok"
grep -q '^other .*Permission denied' "$TMP/out" || flunk "no preload: the other file was not refused"

# 2. The preload with no grant file, an enforce block granting the file.
mkdir "$TMP/d2"
LD_PRELOAD="$LIB" AETHER_SANDBOX_LOG=stderr \
    "$TMP/atomic" enforce "$TMP/d2/doc.json" "$TMP/d2/other.json" > "$TMP/out" 2> "$TMP/err"
expect "preload + enforce: write_atomic of the granted file completes" "granted ok"
grep -q '^other .*Permission denied' "$TMP/out" ||
    flunk "preload + enforce: write_atomic of a file not granted was not refused"
[ "$(cat "$TMP/d2/doc.json" 2>/dev/null)" = granted ] ||
    flunk "preload + enforce: the file does not hold what was written"
[ ! -e "$TMP/d2/other.json" ] || flunk "preload + enforce: the file not granted was written"
no_leftovers "$TMP/d2"

# 3. The preload with a grant file naming the file, no in-process level.
mkdir "$TMP/d3"
printf 'fs_write:%s\n' "$TMP/d3/doc.json" > "$TMP/doc.grants"
LD_PRELOAD="$LIB" AETHER_SANDBOX_GRANTS="$TMP/doc.grants" AETHER_SANDBOX_LOG=stderr \
    "$TMP/atomic" plain "$TMP/d3/doc.json" > "$TMP/out" 2> "$TMP/err"
expect "grant file fs_write <path>: write_atomic(<path>) completes" "plain ok"
[ "$(cat "$TMP/d3/doc.json" 2>/dev/null)" = plain ] ||
    flunk "grant file: the file does not hold what was written"
no_leftovers "$TMP/d3"

# 4. A grant file naming another file: still refused, by the preload.
mkdir "$TMP/d4"
printf 'fs_write:%s\n' "$TMP/d4/else.json" > "$TMP/else.grants"
LD_PRELOAD="$LIB" AETHER_SANDBOX_GRANTS="$TMP/else.grants" AETHER_SANDBOX_LOG=stderr \
    "$TMP/atomic" plain "$TMP/d4/doc.json" > "$TMP/out" 2> "$TMP/err"
if grep -q '^plain .*Permission denied' "$TMP/out" && [ ! -e "$TMP/d4/doc.json" ]; then
    pass "grant file for another file: write_atomic refused"
else
    flunk "grant file for another file: write_atomic was not refused"
fi

# 5. The preload holds a stage to its shape, whatever is staged.
mkdir "$TMP/d5"
printf 'fs_write:%s\n' "$TMP/d5/doc.json" > "$TMP/d5.grants"
LD_PRELOAD="$LIB" AETHER_SANDBOX_GRANTS="$TMP/d5.grants" AETHER_SANDBOX_LOG=none \
    "$TMP/stage_guard" staged "$TMP/d5/doc.json" > "$TMP/out" 2> "$TMP/err"
expect "a hostile stage: only a new <path>.tmp.<pid>.<n> is let through" \
    "shape_excl ok" "shape_excl_openat ok" "shape_no_excl EACCES" "other_pid EACCES" \
    "bad_counter EACCES" "other_suffix EACCES" "elsewhere EACCES"
LD_PRELOAD="$LIB" AETHER_SANDBOX_GRANTS="$TMP/d5.grants" AETHER_SANDBOX_LOG=none \
    "$TMP/stage_guard" unstaged "$TMP/d5/doc.json" > "$TMP/out" 2> "$TMP/err"
expect "nothing staged: the temporary is checked as it is" \
    "shape_excl EACCES" "shape_excl_openat EACCES"

echo ""
echo "sandbox_preload_write_atomic: $([ "$fail" -eq 0 ] && echo PASS || echo FAIL)"
[ "$fail" -eq 0 ]
