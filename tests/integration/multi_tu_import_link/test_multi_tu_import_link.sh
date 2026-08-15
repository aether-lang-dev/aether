#!/bin/sh
# Regression for the aeb-line ask "imported-module fns not static break
# macOS link": a multi-TU program — each module compiled to its own
# object, all linked together — must link with NO
# -Wl,--allow-multiple-definition (GNU-only; Apple ld64 rejects it) and
# no -multiply_defined (removed in Xcode 15). The root fix (#1568):
# imported-module functions are cloned per-TU as `static`, so N TUs
# importing one module produce zero duplicate external symbols; a
# module's OWN functions stay external (the unit of inter-TU linking).
#
# This is exactly aeb's orchestrator shape (one .c/.o per build node,
# one final link), which could not link on macOS at all before the fix.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$ROOT" || exit 1

AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
[ -n "${EXE_EXT:-}" ] && { AE="$AE$EXE_EXT"; AETHERC="$AETHERC$EXE_EXT"; }
[ -x "$AETHERC" ] || { echo "  [SKIP] multi_tu_import_link: aetherc missing (run make)"; exit 0; }

CC="${CC:-cc}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

fail() { echo "  [FAIL] multi_tu_import_link: $1"; shift; for f in "$@"; do [ -f "$f" ] && sed 's/^/        /' "$f" | tail -8; done; exit 1; }

# One TU per module + the driver, aeb-style.
for m in helper/module a/module b/module main; do
    out="$TMP/$(echo "$m" | tr '/' '_').c"
    ( cd "$SCRIPT_DIR" && "$AETHERC" "$m.ae" "$out" ) >"$TMP/cc.log" 2>&1 \
        || fail "aetherc failed on $m.ae" "$TMP/cc.log"
done

CFLAGS_ALL=$("$AE" cflags --cflags 2>/dev/null)
LIBS_ALL=$("$AE" cflags --libs 2>/dev/null)
for c in "$TMP"/*.c; do
    $CC -c "$c" -o "${c%.c}.o" $CFLAGS_ALL 2>"$TMP/gcc.log" \
        || fail "TU compile failed: $c" "$TMP/gcc.log"
done

# The load-bearing step: link every object with NO duplicate-symbol
# escape hatch of any kind.
if ! $CC "$TMP"/*.o -o "$TMP/repro" $LIBS_ALL 2>"$TMP/link.log"; then
    if grep -qiE "duplicate symbol|multiple definition" "$TMP/link.log"; then
        fail "duplicate symbols across TUs (imported clones not static?)" "$TMP/link.log"
    fi
    fail "link failed" "$TMP/link.log"
fi

out=$("$TMP/repro" 2>&1)
[ "$out" = "42 43" ] || fail "wrong output: '$out'"

# Belt: the imported clone really is local, the module's own fn external.
if command -v nm >/dev/null 2>&1; then
    a_o=$(ls "$TMP"/a_module.o)
    nm "$a_o" | grep -qE " T _?a_val" || fail "a_val lost external linkage (inter-TU calls would break)"
    if nm "$a_o" | grep -qE " T _?helper_shared"; then
        fail "imported clone helper_shared is EXTERNAL in a's TU (dup-symbol regression)"
    fi
fi

echo "  [PASS] multi_tu_import_link: 4 TUs, no dedup flags, runs (42 43)"
