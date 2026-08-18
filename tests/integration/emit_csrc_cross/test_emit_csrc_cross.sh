#!/bin/sh
# Regression: aether#1648. `ae build --target=<triple> --emit=csrc` is allowed.
#
# The cross guard rejected every library-shaped emit mode, justified by a
# comment about "library-shaped C that the executable link rejects". That
# reasoning is false for --emit=csrc, which emits C and STOPS — there is no
# link. csrc under --target is the cross-linkable-lib path that needs no
# cross-link support: the consumer compiles the .so/.a/.wasm themselves.
#
# Asserts:
#   - --target=<triple> --emit=csrc emits .c/.h/.catalog.json and exits 0
#   - it needs NO zig (csrc never links, so the cross linker is irrelevant)
#   - the emitted C is target-NEUTRAL: byte-identical across triples and to
#     the native emit, since platform selection stays in #if and is resolved
#     by the consumer's compiler
#   - --emit=lib / obj / both under --target are still rejected up front,
#     with the diagnostic naming csrc as the supported mode

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
SRC="$SCRIPT_DIR/mylib.ae"

TMP="${TMPDIR:-/tmp}/ae_csrc_cross_$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

fail() { echo "  FAIL: $1"; exit 1; }

# --- 1. csrc under a cross triple succeeds and writes all three artifacts ---
if ! "$AE" build --target=aarch64-linux --emit=csrc "$SRC" -o "$TMP/out" >"$TMP/log" 2>&1; then
    cat "$TMP/log"
    fail "--target=aarch64-linux --emit=csrc was rejected"
fi
for f in out.c out.h out.catalog.json; do
    [ -s "$TMP/$f" ] || fail "$f missing or empty"
done

# No native library may be produced — csrc stops before any compile.
for bad in out.so out.dylib out.dll out; do
    [ -e "$TMP/$bad" ] && fail "csrc produced $bad; it must not compile or link"
done

# The catalog header must carry the aether_<name> prototypes.
grep -q 'aether_greet' "$TMP/out.h" || fail "out.h lacks the aether_greet prototype"

# --- 2. the emitted C is target-neutral ---
# Platform selection stays in #if __linux__ / __APPLE__ / __wasi__ and is
# resolved by the CONSUMER's compiler, so --target must not change the bytes.
"$AE" build --target=x86_64-macos   --emit=csrc "$SRC" -o "$TMP/mac"  >/dev/null 2>&1 \
    || fail "--target=x86_64-macos --emit=csrc was rejected"
"$AE" build --target=x86_64-windows --emit=csrc "$SRC" -o "$TMP/win"  >/dev/null 2>&1 \
    || fail "--target=x86_64-windows --emit=csrc was rejected"
"$AE" build                          --emit=csrc "$SRC" -o "$TMP/nat" >/dev/null 2>&1 \
    || fail "native --emit=csrc was rejected"

if ! cmp -s "$TMP/out.c" "$TMP/mac.c" || ! cmp -s "$TMP/out.c" "$TMP/win.c" \
   || ! cmp -s "$TMP/out.c" "$TMP/nat.c"; then
    fail "emitted C differs by target; csrc is meant to be target-neutral source"
fi

# --- 3. --emit=obj under --target produces a TARGET-format object ---
# obj is the other non-linking mode (#1648): it stops at `zig cc -c`, so the
# "executable link rejects it" rationale does not apply to it either. Unlike
# csrc it emits target-FORMAT machine code, so it does need zig — skip when
# absent rather than fail, matching how the cross exe tests behave.
if command -v zig >/dev/null 2>&1; then
    for spec in "aarch64-linux:ELF:aarch64" "x86_64-windows:COFF:" "x86_64-macos:Mach-O:"; do
        t="${spec%%:*}"; rest="${spec#*:}"; want="${rest%%:*}"; arch="${rest#*:}"
        if ! "$AE" build --target="$t" --emit=obj "$SRC" -o "$TMP/o_$t.o"                 >"$TMP/objlog" 2>&1; then
            cat "$TMP/objlog"
            fail "--target=$t --emit=obj was rejected"
        fi
        [ -s "$TMP/o_$t.o" ] || fail "no object emitted for $t"
        desc="$(file "$TMP/o_$t.o")"
        case "$desc" in
            *"$want"*) ;;
            *) fail "$t object is not $want format: $desc" ;;
        esac
        if [ -n "$arch" ]; then
            case "$desc" in
                *"$arch"*) ;;
                *) fail "$t object is not $arch: $desc" ;;
            esac
        fi
    done

    # The cross object must differ from the host one — a silently-native
    # object would satisfy every check above except this.
    "$AE" build --emit=obj "$SRC" -o "$TMP/o_native.o" >/dev/null 2>&1         || fail "native --emit=obj was rejected"
    if cmp -s "$TMP/o_aarch64-linux.o" "$TMP/o_native.o"; then
        fail "the aarch64 object is byte-identical to the host object"
    fi
else
    echo "  [skip] --emit=obj cross checks: zig not on PATH"
fi

# --- 4. the linking emit modes are still rejected, up front ---
# Up front matters: --emit=both re-dispatches as exe, and without an explicit
# check it reaches the cross LINKER and dies with "undefined symbol: main" on
# a source that has no main by design.
for mode in lib both; do
    if "$AE" build --target=aarch64-linux --emit="$mode" "$SRC" -o "$TMP/x" \
            >"$TMP/err" 2>&1; then
        fail "--emit=$mode under --target should be rejected but succeeded"
    fi
    grep -q 'supports executables, --emit=csrc and --emit=obj' "$TMP/err" \
        || fail "--emit=$mode gave the wrong diagnostic: $(head -1 "$TMP/err")"
done

echo "  PASS: --emit=csrc + --emit=obj work under --target; lib/both still rejected"
