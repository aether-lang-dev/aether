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

# Byte-identity of two files, without cmp(1).
#
# The Windows MSYS2 CI shell ships no diffutils, so `cmp` is absent there —
# the same constraint tests/integration/fmt_gate documents and works around.
# Using `cmp` here made this test report "emitted C differs by target" on all
# three Windows legs when the real cause was `cmp: command not found`: a
# missing tool read as a content mismatch. cksum is POSIX and always present.
same_bytes() {
    [ "$(cksum < "$1")" = "$(cksum < "$2")" ]
}

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

if ! same_bytes "$TMP/out.c" "$TMP/mac.c" || ! same_bytes "$TMP/out.c" "$TMP/win.c" \
   || ! same_bytes "$TMP/out.c" "$TMP/nat.c"; then
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
    if same_bytes "$TMP/o_aarch64-linux.o" "$TMP/o_native.o"; then
        fail "the aarch64 object is byte-identical to the host object"
    fi
else
    echo "  [skip] --emit=obj cross checks: zig not on PATH"
fi

# --- 3b. wasm32-wasi routes through zig and needs no hand-passed defines ---
# WASI's setjmp.h #errors without -D__wasm_exception_handling__=1, and WASI has
# no POSIX signal API without -D_WASI_EMULATED_SIGNAL. CI passed both by hand
# ("Verify WASI panic runtime"); `ae build` now supplies them itself, so the
# whole point of this case is that NOTHING is passed on the command line here.
if command -v zig >/dev/null 2>&1; then
    "$AE" build --target=wasm32-wasi --emit=csrc "$SRC" -o "$TMP/wasi"         >"$TMP/wasilog" 2>&1 || { cat "$TMP/wasilog"; fail "wasm32-wasi --emit=csrc was rejected"; }
    [ -s "$TMP/wasi.c" ] || fail "no C emitted for wasm32-wasi"
    same_bytes "$TMP/wasi.c" "$TMP/out.c" \
        || fail "wasm32-wasi csrc differs; csrc is meant to be target-neutral"

    "$AE" build --target=wasm32-wasi --emit=obj "$SRC" -o "$TMP/wasi.o"         >"$TMP/wasiobj" 2>&1 || { cat "$TMP/wasiobj"; fail "wasm32-wasi --emit=obj was rejected"; }
    case "$(file "$TMP/wasi.o")" in
        *WebAssembly*) ;;
        *) fail "wasm32-wasi object is not a wasm module: $(file "$TMP/wasi.o")" ;;
    esac

    # A full executable link now WORKS for wasm32-wasi (#1655). It was rejected
    # while the threaded runtime was the blocker; wasi now selects the
    # cooperative scheduler and the exe links.
    printf 'main() {\n    println("hi")\n}\n' > "$TMP/app.ae"
    if ! "$AE" build --target=wasm32-wasi "$TMP/app.ae" -o "$TMP/app.wasm" \
            >"$TMP/wasiexe" 2>&1; then
        cat "$TMP/wasiexe"
        fail "wasm32-wasi executable link failed"
    fi
    case "$(file "$TMP/app.wasm")" in
        *WebAssembly*) ;;
        *) fail "wasi exe is not a wasm module: $(file "$TMP/app.wasm")" ;;
    esac

    # An ACTOR program must link too. This is the case that caught the real
    # bug: codegen emitted a computed-goto dispatch table (label addresses),
    # which the wasm backend rejects with "relocations for function or section
    # offsets are only supported in metadata sections". The guard excluded
    # __EMSCRIPTEN__ but not __wasi__, so only wasi hit it — and only when
    # LLVM folded the table rather than keeping it, which is why a
    # single-receive-arm actor failed while a two-arm one compiled.
    printf 'message Ping { n: int }\n\nactor Counter {\n    state total = 0;\n    receive { Ping(n) -> { total = total + n; } }\n}\n\nmain() { c = spawn(Counter()); c ! Ping { n: 5 }; wait_for_idle(); }\n' > "$TMP/act.ae"
    if ! "$AE" build --target=wasm32-wasi "$TMP/act.ae" -o "$TMP/act.wasm" \
            >"$TMP/wasiact" 2>&1; then
        cat "$TMP/wasiact"
        fail "wasm32-wasi actor executable failed to link"
    fi
else
    echo "  [skip] wasm32-wasi checks: zig not on PATH"
fi

# --- 4. the linking emit modes are still rejected, up front ---
# Up front matters: --emit=both re-dispatches as exe, and without an explicit
# check it reaches the cross LINKER and dies with "undefined symbol: main" on
# a source that has no main by design.
# aarch64-linux specifically: --emit=lib IS supported on Apple targets
# (-dynamiclib) and, since the wasm-lib work, on wasm32-wasi (--no-entry plus
# an export list). It remains unsupported on the plain zig triples, which is
# what this loop pins.
for mode in lib both; do
    if "$AE" build --target=aarch64-linux --emit="$mode" "$SRC" -o "$TMP/x" \
            >"$TMP/err" 2>&1; then
        fail "--emit=$mode under --target should be rejected but succeeded"
    fi
    grep -q 'supports executables, --emit=csrc and --emit=obj' "$TMP/err" \
        || fail "--emit=$mode gave the wrong diagnostic: $(head -1 "$TMP/err")"
done

echo "  PASS: csrc/obj under --target (incl. wasm32-wasi); lib/both still rejected on non-wasm/non-Apple"
