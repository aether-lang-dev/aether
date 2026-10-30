#!/bin/sh
# #2955 (D2): a library's export called from inside another library's
# enforce block, through a host service on the same thread, is held to that
# block's grants, and an enforce of its own only narrows them.
#
# The grant levels lived in each library's generated code, with one
# process-wide checker pointer each enforce installed and cleared. Inside
# the outer block's deny-all, the inner library's grant_all enforce replaced
# the outer checker with its own (PATH read), and its end cleared the
# pointer, so the rest of the outer block ran unchecked (PATH read again).
# The levels are now each thread's, in the runtime.
#
# 1. one runtime: both libraries' C linked into the host;
# 2. each library built the default way, with its own copy of the runtime:
#    the copies stay apart (on every platform since #3481), so the inner one
#    cannot read the outer block's grants; it refuses, and its message names
#    --shared-runtime;
#    2c. the outer library linked into a program with a runtime of its own
#    that exports nothing and looks first (#3445);
#    2d. a program with the panic and no sandbox, -rdynamic on Linux, and a
#    library whose block catches a panic from a nested block (#3438);
# 3. both built with --shared-runtime (skipped where build/shared has none).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
CC="${CC:-cc}"
export AETHER_HOME="$ROOT"

[ -x "$AE" ] || { echo "  [SKIP] sandbox_nested_libraries: $AE not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT
fail=0
want="before 1 inner 0 granted 0 outer 0 after 1"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) ext=dll; dl="" ;;
    Darwin) ext=dylib; dl="" ;;
    Linux) ext=so; dl="-ldl" ;;
    *) ext=so; dl="" ;;
esac

# 1. one runtime
if ! "$AETHERC" --emit=lib --with=os,extern "$SCRIPT_DIR/outer.ae" "$TMP/outer.c" > "$TMP/outer.log" 2>&1 ||
   ! "$AETHERC" --emit=lib --with=os,extern "$SCRIPT_DIR/inner.ae" "$TMP/inner.c" > "$TMP/inner.log" 2>&1; then
    echo "  [FAIL] sandbox_nested_libraries: aetherc --emit=lib failed"
    tail -5 "$TMP/outer.log" "$TMP/inner.log" | sed 's/^/        /'
    exit 1
fi
# shellcheck disable=SC2046
if ! "$CC" -O1 -DHOST_STATIC $("$AE" cflags --cflags) "$TMP/outer.c" "$TMP/inner.c" \
        "$SCRIPT_DIR/hooks.c" "$SCRIPT_DIR/host.c" -o "$TMP/host_static" \
        $("$AE" cflags --libs) > "$TMP/cc.log" 2>&1; then
    echo "  [FAIL] sandbox_nested_libraries: the one-runtime host did not build"
    head -10 "$TMP/cc.log" | sed 's/^/        /'
    exit 1
fi
got="$("$TMP/host_static" 2>"$TMP/static.err" | tr -d '\r')"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] sandbox_nested_libraries: one runtime: got '$got', want '$want'"
    head -5 "$TMP/static.err" | sed 's/^/        /'
    fail=1
fi

# The host that loads the two libraries at run time.
if ! "$CC" -O1 "$SCRIPT_DIR/host.c" -o "$TMP/host_dyn" $dl > "$TMP/cc_dyn.log" 2>&1; then
    echo "  [FAIL] sandbox_nested_libraries: the loading host did not build"
    head -10 "$TMP/cc_dyn.log" | sed 's/^/        /'
    exit 1
fi

# $1 = label, $2 = extra build flags
build_pair() {
    mkdir -p "$TMP/$1"
    # shellcheck disable=SC2086
    (cd "$TMP/$1" &&
     "$AE" build --emit=lib $2 --with=os,extern "$SCRIPT_DIR/outer.ae" --extra "$SCRIPT_DIR/hooks.c" \
         -o "$TMP/$1/libouter.$ext" > "$TMP/$1/outer.log" 2>&1 &&
     "$AE" build --emit=lib $2 --with=os,extern "$SCRIPT_DIR/inner.ae" --extra "$SCRIPT_DIR/hooks.c" \
         -o "$TMP/$1/libinner.$ext" > "$TMP/$1/inner.log" 2>&1)
}

# 2. a runtime per library
if ! build_pair static ""; then
    echo "  [FAIL] sandbox_nested_libraries: ae build --emit=lib failed"
    tail -5 "$TMP/static/outer.log" "$TMP/static/inner.log" 2>/dev/null | sed 's/^/        /'
    exit 1
fi
got="$("$TMP/host_dyn" "$TMP/static/libouter.$ext" "$TMP/static/libinner.$ext" 2>"$TMP/apart.err" | tr -d '\r')"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] sandbox_nested_libraries: a runtime per library: got '$got', want '$want'"
    head -5 "$TMP/apart.err" | sed 's/^/        /'
    fail=1
fi
if ! grep -q -- "--shared-runtime" "$TMP/apart.err"; then
    echo "  [FAIL] sandbox_nested_libraries: a runtime per library: the refusal does not name --shared-runtime"
    head -5 "$TMP/apart.err" | sed 's/^/        /'
    fail=1
fi
# 2b. the inner library loaded first and local, reading at once, the outer
# one global after it (#3442): the outer copy's lookup from the program's
# handle sees the global scope, where it is itself, and still agrees on the
# inner copy, the first in load order. On Windows, where a module has no
# global scope, this is case 2 again.
got="$("$TMP/host_dyn" "$TMP/static/libouter.$ext" "$TMP/static/libinner.$ext" mixed 2>"$TMP/mixed.err" | tr -d '\r')"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] sandbox_nested_libraries: the inner library local and first: got '$got', want '$want'"
    head -5 "$TMP/mixed.err" | sed 's/^/        /'
    fail=1
fi

# 2c. the outer library linked into a program with a runtime of its own, an
# executable that exports nothing (Windows, and Linux, where `ae cflags
# --libs` adds no -rdynamic), which runs the outer block before it loads the
# inner library (#3445): the program's copy looked the copies up alone, and
# the inner library's copy, loaded after, finds it by its record. It found
# none and kept levels of its own, so the inner reads inside the deny-all
# block were allowed (inner 1 granted 1).
# shellcheck disable=SC2046,SC2086
if ! "$CC" -O1 -DHOST_OUTER_LINKED $("$AE" cflags --cflags) "$TMP/outer.c" "$SCRIPT_DIR/hooks.c" \
        "$SCRIPT_DIR/host.c" -o "$TMP/host_outer" $("$AE" cflags --libs) $dl > "$TMP/cc_outer.log" 2>&1; then
    echo "  [FAIL] sandbox_nested_libraries: the host with the outer library linked in did not build"
    head -10 "$TMP/cc_outer.log" | sed 's/^/        /'
    exit 1
fi
got="$("$TMP/host_outer" "$TMP/static/libinner.$ext" 2>"$TMP/outer_first.err" | tr -d '\r')"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] sandbox_nested_libraries: the program's copy first, alone: got '$got', want '$want'"
    head -5 "$TMP/outer_first.err" | sed 's/^/        /'
    fail=1
fi

# 2d. a program with the panic of a runtime of its own and no sandbox,
# linked -rdynamic on Linux as `ae build` links one, loading a library with a
# runtime of its own whose enforce block catches a panic raised in a nested
# block (#3438). Before #3481 the library ran the program's panic and try
# frames there (ELF interposition), so its sandbox had to set the program's
# pointers; now it runs its own on every platform, and the catch must still
# leave neither the nested level (PATH 0) nor, at the block's end, the outer
# one (probe 0 after it) in force.
rdyn=""
case "$(uname -s 2>/dev/null)" in Linux|FreeBSD) rdyn="-rdynamic" ;; esac
# shellcheck disable=SC2046,SC2086
if ! "$CC" -O1 "$SCRIPT_DIR/host_caught.c" -o "$TMP/host_caught" $("$AE" cflags --libs) $rdyn $dl \
        > "$TMP/cc_caught.log" 2>&1 ||
   ! (cd "$TMP/static" && "$AE" build --emit=lib --with=os,extern "$SCRIPT_DIR/caught.ae" \
        --extra "$SCRIPT_DIR/hooks.c" -o "$TMP/static/libcaught.$ext" > "$TMP/static/caught.log" 2>&1); then
    echo "  [FAIL] sandbox_nested_libraries: the panic-only host or the catching library did not build"
    head -10 "$TMP/cc_caught.log" "$TMP/static/caught.log" 2>/dev/null | sed 's/^/        /'
    exit 1
fi
got="$("$TMP/host_caught" "$TMP/static/libcaught.$ext" 2>"$TMP/caught.err" | tr -d '\r')"
want_caught="after the catch PATH 1 probe 0; after the block probe 1"
if [ "$got" != "$want_caught" ]; then
    echo "  [FAIL] sandbox_nested_libraries: a panic caught inside the library's block, the program's panic: got '$got', want '$want_caught'"
    head -5 "$TMP/caught.err" | sed 's/^/        /'
    fail=1
fi
# 2e. the same, with a second library with a sandbox of its own (inner.ae's)
# that enforces after the first's block ran, so its copy looks the copies up
# last (#3438): both run the program's panic, and one slot of hooks the last
# copy set served only it, so the first's caught panic left its level.
got="$("$TMP/host_caught" "$TMP/static/libcaught.$ext" "$TMP/static/libinner.$ext" 2>"$TMP/caught2.err" | tr -d '\r')"
want_two="$want_caught
again, after another copy looked: $want_caught"
if [ "$got" != "$want_two" ]; then
    echo "  [FAIL] sandbox_nested_libraries: two libraries with a sandbox each, the program's panic:"
    printf '%s\n' "$got" | sed 's/^/        got:  /'
    head -5 "$TMP/caught2.err" | sed 's/^/        /'
    fail=1
fi

# 3. one shared runtime
if ls "$ROOT/build/shared/" 2>/dev/null | grep -q "aether"; then
    if ! build_pair shared "--shared-runtime"; then
        echo "  [FAIL] sandbox_nested_libraries: ae build --emit=lib --shared-runtime failed"
        tail -5 "$TMP/shared/outer.log" "$TMP/shared/inner.log" 2>/dev/null | sed 's/^/        /'
        exit 1
    fi
    got="$(PATH="$ROOT/build/shared:$PATH" \
           LD_LIBRARY_PATH="$ROOT/build/shared${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
           DYLD_LIBRARY_PATH="$ROOT/build/shared${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
           "$TMP/host_dyn" "$TMP/shared/libouter.$ext" "$TMP/shared/libinner.$ext" 2>"$TMP/shared.err" | tr -d '\r')"
    if [ "$got" != "$want" ]; then
        echo "  [FAIL] sandbox_nested_libraries: --shared-runtime: got '$got', want '$want'"
        head -5 "$TMP/shared.err" | sed 's/^/        /'
        fail=1
    fi
    if grep -q "another copy" "$TMP/shared.err"; then
        echo "  [FAIL] sandbox_nested_libraries: --shared-runtime: one runtime reported another copy"
        fail=1
    fi
    shared_note=", and on one shared runtime"
else
    shared_note=" (no shared runtime in build/shared: that leg skipped)"
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] sandbox_nested_libraries: an inner library's export inside an outer enforce is held to it, its own grant_all narrows nothing, and the outer block stays checked: one runtime, a runtime per library (refused, naming --shared-runtime), a program whose copy looked first and exports nothing, a panic caught inside the block of a library loaded by a program linked -rdynamic$shared_note"
fi
exit $fail
