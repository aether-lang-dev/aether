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

# ---- Phase 2: the SAME shape with --emit=lib TUs (#1590) ------------------
# aeb regen TUs are emitted with --emit=lib, and every lib TU carries the
# aether_lib_meta() reflection catalog (issue #403). Before #1590 that
# symbol had external linkage, so this exact link died with N-1
# "multiple definition of `aether_lib_meta'" errors — a hole the plain-
# emission phase above cannot see. The catalog entry point is now weak.
TMP2="$TMP/libtu"
mkdir -p "$TMP2"
for m in helper/module a/module b/module; do
    out="$TMP2/$(echo "$m" | tr '/' '_').c"
    ( cd "$SCRIPT_DIR" && "$AETHERC" --emit=lib "$m.ae" "$out" ) >"$TMP2/cc.log" 2>&1 \
        || fail "aetherc --emit=lib failed on $m.ae" "$TMP2/cc.log"
done
# The driver stays plain emission (it has main()).
( cd "$SCRIPT_DIR" && "$AETHERC" main.ae "$TMP2/main.c" ) >"$TMP2/cc.log" 2>&1 \
    || fail "aetherc failed on main.ae (lib phase)" "$TMP2/cc.log"

for c in "$TMP2"/*.c; do
    $CC -c "$c" -o "${c%.c}.o" $CFLAGS_ALL 2>"$TMP2/gcc.log" \
        || fail "lib-TU compile failed: $c" "$TMP2/gcc.log"
done

# Load-bearing: three catalogs, one link, still NO escape hatch.
if ! $CC "$TMP2"/*.o -o "$TMP2/repro_lib" $LIBS_ALL 2>"$TMP2/link.log"; then
    if grep -qi "aether_lib_meta" "$TMP2/link.log"; then
        fail "duplicate aether_lib_meta across --emit=lib TUs (#1590 regression)" "$TMP2/link.log"
    fi
    if grep -qiE "duplicate symbol|multiple definition" "$TMP2/link.log"; then
        fail "duplicate symbols across --emit=lib TUs" "$TMP2/link.log"
    fi
    fail "lib-TU link failed" "$TMP2/link.log"
fi

out=$("$TMP2/repro_lib" 2>&1)
[ "$out" = "42 43" ] || fail "wrong output from lib-TU build: '$out'"

# Belt: the catalog entry point is emitted weak ("W"/"w" in nm), not "T".
if command -v nm >/dev/null 2>&1; then
    a_lib_o=$(ls "$TMP2"/a_module.o)
    if nm "$a_lib_o" | grep -qE " T _?aether_lib_meta"; then
        fail "aether_lib_meta has strong external linkage in a lib TU (#1590)"
    fi
    nm "$a_lib_o" | grep -qE " [Ww] _?aether_lib_meta" \
        || fail "aether_lib_meta missing/not weak in a lib TU"
fi

echo "  [PASS] multi_tu_import_link: 4 TUs, no dedup flags, runs (42 43); --emit=lib TUs link too (#1590)"
