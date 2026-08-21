#!/bin/sh
# `ae build --target=wasm32-wasi --emit=lib` produces a --no-entry wasm module
# exporting the module's declared ABI, and `--export=` narrows that set.
#
# Asked for by the aeb / html-sanitizer line. Before this, a downstream wanting
# a wasm library hand-rolled the whole link in a shell script — including a
# hand-picked copy of Aether's runtime source list, which existed only to avoid
# multicore_scheduler.c and drifted every time the runtime changed. Aether
# already owns that curation; this hands back a lib so nobody re-derives it.
#
# Asserts:
#   1. --emit=lib is accepted for wasm32-wasi and yields a real wasm module;
#   2. with NO flag, every function in the module's catalog is exported —
#      the module declares its own ABI, so the common case needs no flag;
#   3. `--export=greet` REPLACES that set: greet stays, add and
#      helper_internal go. Replace, not add, because a wasm surface is often a
#      deliberate subset ("all minus some" cannot be expressed by adding) —
#      html-sanitizer exports 16 of its 34 functions, omitting callback
#      registrars that take C function pointers and DOM-walk accessors;
#   4. malloc/free are always exported (a wasm consumer needs them to pass
#      strings across the ABI).
#
# Assertions read the wasm EXPORT SECTION, not the raw bytes. A symbol name can
# appear in the linking/name sections while the function is not exported at
# all; grepping would pass on a module that exports nothing.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
SRC="$SCRIPT_DIR/mylib.ae"
READ="$SCRIPT_DIR/read_exports.py"

command -v zig >/dev/null 2>&1 || { echo "  [SKIP] wasm_emit_lib_exports: zig not on PATH"; exit 0; }

# The wasi-libc that ships with zig decides how a library-shaped wasm link
# behaves, and it differs between releases: an older zig fails this with
# "wasm-ld: undefined symbol: main" from its own __main_void.o, which reads as
# a defect in the toolchain rather than a version mismatch. Skip unless the
# zig on PATH is the one the repo pins.
ZIG_PINNED="$(sed -n 's/^ZIG_VERSION=//p' "$ROOT/scripts/get-zig.sh" 2>/dev/null | head -1)"
ZIG_HAVE="$(zig version 2>/dev/null)"
if [ -n "$ZIG_PINNED" ] && [ "$ZIG_HAVE" != "$ZIG_PINNED" ]; then
    echo "  [SKIP] wasm_emit_lib_exports: zig $ZIG_HAVE on PATH, this needs the pinned $ZIG_PINNED"
    exit 0
fi
command -v python3 >/dev/null 2>&1 || { echo "  [SKIP] wasm_emit_lib_exports: python3 not on PATH"; exit 0; }

TMP="${TMPDIR:-/tmp}/ae_wasmlib_$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

fail() { echo "  FAIL: $1"; exit 1; }
has()  { grep -qx "$2" "$1"; }

# --- 1 + 2: default build exports the module's whole declared ABI -----------
if ! "$AE" build --target=wasm32-wasi --emit=lib "$SRC" -o "$TMP/all.wasm" \
        >"$TMP/log" 2>&1; then
    cat "$TMP/log"
    fail "--target=wasm32-wasi --emit=lib was rejected"
fi
case "$(file "$TMP/all.wasm")" in
    *WebAssembly*) ;;
    *) fail "output is not a wasm module: $(file "$TMP/all.wasm")" ;;
esac

python3 "$READ" "$TMP/all.wasm" > "$TMP/all.txt" || fail "could not read the export section"
for sym in aether_greet aether_add aether_helper_internal malloc free; do
    has "$TMP/all.txt" "$sym" || {
        echo "  exports were:"; sed 's/^/    /' "$TMP/all.txt"
        fail "default build did not export $sym"; }
done

# --- 3: --export= replaces the catalog set, it does not add to it ----------
if ! "$AE" build --target=wasm32-wasi --emit=lib "$SRC" -o "$TMP/one.wasm" \
        --export=greet >"$TMP/log2" 2>&1; then
    cat "$TMP/log2"
    fail "--export=greet was rejected"
fi
python3 "$READ" "$TMP/one.wasm" > "$TMP/one.txt" || fail "could not read the export section"

has "$TMP/one.txt" aether_greet || fail "--export=greet did not export greet"
for sym in aether_add aether_helper_internal; do
    if has "$TMP/one.txt" "$sym"; then
        echo "  exports were:"; sed 's/^/    /' "$TMP/one.txt"
        fail "--export=greet should REPLACE the catalog set, but $sym survived"
    fi
done

# --- 4: malloc/free survive the override ----------------------------------
for sym in malloc free; do
    has "$TMP/one.txt" "$sym" || fail "$sym must be exported even with an explicit list"
done

# The comma form is the same mechanism; check it selects two of three.
"$AE" build --target=wasm32-wasi --emit=lib "$SRC" -o "$TMP/two.wasm" \
    --exports=greet,add >"$TMP/log3" 2>&1 || { cat "$TMP/log3"; fail "--exports=a,b was rejected"; }
python3 "$READ" "$TMP/two.wasm" > "$TMP/two.txt"
has "$TMP/two.txt" aether_greet || fail "--exports= dropped greet"
has "$TMP/two.txt" aether_add   || fail "--exports= dropped add"
if has "$TMP/two.txt" aether_helper_internal; then
    fail "--exports=greet,add should not export helper_internal"
fi

# --- 5: the emcc backend derives the SAME set, spelled its own way ---------
# --target=wasm goes through Emscripten, which wants
# -sEXPORTED_FUNCTIONS=_sym (leading underscore) rather than -Wl,--export=sym.
# The two backends share one collector precisely so they cannot drift; this
# checks the emcc spelling without needing an emsdk, by putting a stub emcc on
# PATH that records the flags it was handed.
mkdir -p "$TMP/bin"
cat > "$TMP/bin/emcc" <<'STUB'
#!/bin/sh
case "$1" in --version) echo "emcc (stub) 0.0"; exit 0 ;; esac
for a in "$@"; do
  case "$a" in -sEXPORTED_FUNCTIONS=*|--no-entry) echo "FLAG: $a" ;; esac
done
exit 1
STUB
chmod +x "$TMP/bin/emcc"

PATH="$TMP/bin:$PATH" "$AE" build --target=wasm --emit=lib "$SRC" -o "$TMP/e.js" \
    >"$TMP/emcc_all.txt" 2>&1 || true
grep -q -- '--no-entry' "$TMP/emcc_all.txt" \
    || { sed 's/^/    /' "$TMP/emcc_all.txt" | head -5; fail "emcc lib build did not pass --no-entry"; }
grep -qE 'EXPORTED_FUNCTIONS=.*_aether_greet' "$TMP/emcc_all.txt" \
    || fail "emcc lib build did not export greet"
grep -qE 'EXPORTED_FUNCTIONS=.*_aether_add' "$TMP/emcc_all.txt" \
    || fail "emcc lib build did not export add"

PATH="$TMP/bin:$PATH" "$AE" build --target=wasm --emit=lib "$SRC" -o "$TMP/e2.js" \
    --export=greet >"$TMP/emcc_one.txt" 2>&1 || true
grep -qE 'EXPORTED_FUNCTIONS=.*_aether_greet' "$TMP/emcc_one.txt" \
    || fail "emcc --export=greet did not export greet"
grep -qE 'EXPORTED_FUNCTIONS=.*_aether_add' "$TMP/emcc_one.txt" \
    && fail "emcc --export=greet should REPLACE the set, but add survived"

# An EXE build must not pick up any of the library link flags.
PATH="$TMP/bin:$PATH" "$AE" build --target=wasm "$SRC" -o "$TMP/e3.js" \
    >"$TMP/emcc_exe.txt" 2>&1 || true
grep -q -- '--no-entry' "$TMP/emcc_exe.txt" \
    && fail "an emcc EXE build should not pass --no-entry"

echo "  PASS: wasm_emit_lib_exports: catalog-driven by default, --export= narrows it, both backends"
