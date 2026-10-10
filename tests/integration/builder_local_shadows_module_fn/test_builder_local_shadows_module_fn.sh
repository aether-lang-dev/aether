#!/bin/sh
# A builder body's parameters and locals shadow a same-named function of its
# module, exactly as a plain function's do
# (aeb asks/builder-local-resolves-to-module-function.md; aether's
# asks/REPLY-builder-local-resolves-to-module-function.md).
#
# The module namespacing pass (rename_intra_module_refs) skips the
# `<ns>_<name>` rewrite for a name a local binding shadows, but only a plain
# function (and a closure) entered a scope there. A builder entered none, so
# in a module that also defined `main_class()` the parameter of
# `builder java_main(main_class: string)` was renamed to the function: its
# ADDRESS interpolated (E0200), compared with 1, or passed as an int. One
# module per shape, because the interpolation case rejects the whole module:
#
#   bs_param  a parameter passed on and interpolated   (aeb java.java_main)
#   bs_cmp    a local assigned, then compared with 1    (aeb bldr.install_launcher)
#   bs_int    a local passed as an int argument         (aeb fetch.git)
#
# Each shape is checked by what it prints and by the emitted C naming the
# local, not `<ns>_<name>`. Before the fix: bs_param failed to compile
# (E0200), bs_cmp printed "PATH untouched" (the address is never 1), and
# bs_int failed to compile under clang (-Wint-conversion) or printed a
# truncated address under gcc.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
[ -n "${EXE_EXT:-}" ] && { AE="$AE$EXE_EXT"; AETHERC="$AETHERC$EXE_EXT"; }
NAME=builder_local_shadows_module_fn

[ -x "$AE" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }

cd "$ROOT" || exit 1
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT

fails=0
fail() {
    echo "  [FAIL] $NAME: $1"
    [ -n "${2:-}" ] && [ -f "$2" ] && sed 's/^/        /' "$2" | head -12
    fails=$((fails + 1))
}

# check <shape> <expected output line> <C the use site must contain>
#       <C it must not contain>
check() {
    shape="$1"; want="$2"; c_has="$3"; c_not="$4"
    if AETHER_LIB_DIR="$SCRIPT_DIR/lib" "$AE" run "$SCRIPT_DIR/probe_$shape.ae" \
            >"$TMP/$shape.log" 2>&1; then
        grep -qF "$want" "$TMP/$shape.log" \
            || fail "$shape: expected '$want'; the module function won" "$TMP/$shape.log"
    else
        fail "$shape: the probe did not build or run" "$TMP/$shape.log"
    fi
    if AETHER_LIB_DIR="$SCRIPT_DIR/lib" "$AETHERC" "$SCRIPT_DIR/probe_$shape.ae" "$TMP/$shape.c" \
            >"$TMP/$shape.gen" 2>&1; then
        grep -qF "$c_has" "$TMP/$shape.c" \
            || fail "$shape: the emitted C does not use the local ('$c_has')" "$TMP/$shape.c"
        if grep -qF "$c_not" "$TMP/$shape.c"; then
            fail "$shape: the emitted C uses the module function ('$c_not')"
        fi
    else
        fail "$shape: aetherc failed" "$TMP/$shape.gen"
    fi
}

check param "run java com.example.Main" "bs_param_show(main_class);" "bs_param_show(bs_param_main_class)"
check param "show [com.example.Main]"   "bs_param_show(main_class);" "bs_param_show(bs_param_main_class)"
check cmp   "tool: BINDIR on PATH"      "if (with_path == 1)"        "bs_cmp_with_path == 1"
check int   "depth=7"                   "bs_int_show_depth(depth);"  "bs_int_show_depth(bs_int_depth)"

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] $NAME: builder parameters and locals shadow same-named module functions"
exit 0
