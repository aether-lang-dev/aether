#!/bin/sh
# #2957: the cross-version matrix of docs/abi-policy.md. mlib.ae is built by
# this Aether and by a release before it (AETHER_ABI_OLD_HOME, an extracted
# release archive: CI and the release workflow download the latest
# published release), and each pair the policy supports is run:
#
#   C hosts (host.c, plugin style, everything by name):
#     this header, this library                    the level-1 baseline
#     this header, the old library                 at the level it records
#     the old header, this library                 a host at an older level
#   two.c: the old and the new library in one process, each with its own
#     runtime: calls interleaved, strings given back to their own library,
#     a panic in one, an account of the new one suspended while the old one
#     runs and resumed, both unloaded
#   Aether consumers (app.ae, a binary import):
#     this importer, the old library; the old importer, this library. On
#     ELF this ae links a library -Bsymbolic, so it keeps its own runtime in
#     a program that exports one (#3481); an old library linked without it,
#     of another version, is refused at import, naming the rebuild.
#   The shared runtime:
#     this host, this library on this shared runtime       the baseline
#     this host, the old library on the old shared runtime
#     this host, this library on the old shared runtime    accepted if the
#                                                          ABI IDs match;
#                                                          otherwise refused: at load
#                                                          (a level-0 runtime
#                                                          lacks what it
#                                                          calls) or by the
#                                                          library's preflight
#     this importer, the old shared-runtime library        accepted if the
#                                                          ABI IDs match,
#                                                          otherwise refused at import
#
# Call output depends on the host ABI levels (the old library's, read from
# its catalog, and the old header's). Shared-runtime acceptance depends on
# the full runtime ABI IDs, so the matrix also holds against a release
# with the same ABI as this tree.
#
# Fixtures, not tests: mlib.ae is a library with no main and app.ae imports
# it; the driver builds both.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
CC="${CC:-cc}"
export AETHER_STACK_TRACE=0

OLD_HOME="${AETHER_ABI_OLD_HOME:-}"
if [ -z "$OLD_HOME" ]; then
    echo "  [SKIP] abi_matrix: AETHER_ABI_OLD_HOME names no earlier release (CI downloads the latest one; docs/abi-policy.md)"
    exit 0
fi
[ -x "$AE" ] || { echo "  [SKIP] abi_matrix: $AE not built"; exit 0; }

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) ext=dll; exe=.exe; dl="" ;;
    Darwin) ext=dylib; exe=""; dl="" ;;
    Linux) ext=so; exe=""; dl="-ldl" ;;
    *) ext=so; exe=""; dl="" ;;
esac
OLD_AE="$OLD_HOME/bin/ae$exe"
[ -x "$OLD_AE" ] || { echo "  [FAIL] abi_matrix: no ae in $OLD_HOME/bin"; exit 1; }
old_version="$(tr -d '\r\n' < "$OLD_HOME/VERSION" 2>/dev/null)"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT
fail=0

# $1 = what failed, $2 = a log to show
bad() {
    echo "  [FAIL] abi_matrix: $1"
    [ -n "$2" ] && [ -f "$2" ] && tail -8 "$2" | sed 's/^/        /'
    fail=1
}

# $1 = label, $2 = what it printed, $3 = what it should
expect() {
    if [ "$2" != "$3" ]; then
        echo "  [FAIL] abi_matrix: $1:"
        echo "      got:"; printf '%s\n' "$2" | sed 's/^/        /'
        echo "      want:"; printf '%s\n' "$3" | sed 's/^/        /'
        fail=1
    fi
}

# $1 = dir, $2 = toolchain home, $3 = its ae, $4 = extra flags
build_lib() {
    mkdir -p "$1"
    # shellcheck disable=SC2086
    (cd "$1" && AETHER_HOME="$2" "$3" build --emit=lib $4 "$SCRIPT_DIR/mlib.ae" -o "$1/libmlib.$ext" \
        > "$1/build.log" 2>&1) || { bad "mlib.ae did not build in $1" "$1/build.log"; return 1; }
}

build_lib "$TMP/new" "$ROOT" "$AE" "" || exit 1
build_lib "$TMP/old" "$OLD_HOME" "$OLD_AE" "" || exit 1

# shellcheck disable=SC2086
"$CC" -O1 -I"$ROOT/include" -I"$ROOT/runtime" "$SCRIPT_DIR/host.c" -o "$TMP/host_new" $dl \
    > "$TMP/cc_host_new.log" 2>&1 || { bad "host.c did not build with this header" "$TMP/cc_host_new.log"; exit 1; }
# shellcheck disable=SC2086
"$CC" -O1 -I"$OLD_HOME/include/aether" -I"$OLD_HOME/include/aether/runtime" "$SCRIPT_DIR/host.c" \
    -o "$TMP/host_old" $dl > "$TMP/cc_host_old.log" 2>&1 ||
    { bad "host.c did not build with the old header" "$TMP/cc_host_old.log"; exit 1; }
# shellcheck disable=SC2086
"$CC" -O1 -I"$ROOT/include" -I"$ROOT/runtime" "$SCRIPT_DIR/two.c" -o "$TMP/two" $dl \
    > "$TMP/cc_two.log" 2>&1 || { bad "two.c did not build" "$TMP/cc_two.log"; exit 1; }

# The schema minor is the library's; the rest is the level's.
norm() { tr -d '\r' | sed 's/schema 1\.[0-9]*/schema 1.x/'; }

common_lines='typed: add 5, scale 7.50, wide 3000000000, even 1 0
strings: "hello 42", held yes, 20000 more released, the runtime holds 0 bytes more'
level1='catalog: schema 1.x, host ABI level 1
preflight: ok
'"$common_lines"'
errors: checked(3) 6 outcome 0; checked(-1) 0 outcome 1 "negative"; then 10
deadline: 50 ms ends a loop of 2e8: 0 outcome 2, then 45
unload: 0'
level0='catalog: schema 1.x, host ABI level 0
'"$common_lines"'
errors: level 0 has no outcomes; checked(3) 6
deadline: level 0, not asked; spin(10) 45
unload: 0'
old_host='catalog: schema 1.x, read by a level-0 host
'"$common_lines"'
errors: this header has no outcomes; checked(3) 6
deadline: 50 ms cuts a loop of 2e8: yes, then 45
unload: 0'

expect "this header, this library" "$("$TMP/host_new" "$TMP/new/libmlib.$ext" 2>"$TMP/nn.err" | norm)" "$level1"

got="$("$TMP/host_new" "$TMP/old/libmlib.$ext" 2>"$TMP/no.err" | norm)"
old_level="$(printf '%s\n' "$got" | sed -n 's/^catalog: schema 1\.x, host ABI level \([0-9]*\)$/\1/p')"
case "$old_level" in
    0) expect "this header, the $old_version library (level 0)" "$got" "$level0" ;;
    1) expect "this header, the $old_version library (level 1)" "$got" "$level1" ;;
    *) bad "the $old_version library records no level this matrix knows: $(printf '%s\n' "$got" | head -1)" ;;
esac

if grep -q '^#define AETHER_HOST_ABI ' "$OLD_HOME/include/aether/libaether.h"; then
    want_old_host="$level1"
else
    want_old_host="$old_host"
fi
expect "the $old_version header, this library" "$("$TMP/host_old" "$TMP/new/libmlib.$ext" 2>"$TMP/on.err" | norm)" "$want_old_host"

cp "$TMP/old/libmlib.$ext" "$TMP/libmlib_old.$ext"
cp "$TMP/new/libmlib.$ext" "$TMP/libmlib_new.$ext"
expect "two libraries in one process" "$("$TMP/two" "$TMP/libmlib_old.$ext" "$TMP/libmlib_new.$ext" 2>"$TMP/two.err" | tr -d '\r')" \
'typed: old 3, new 7, old 11
strings: hello 1, hello 2
errors: new checked(-1) 0 outcome 1; then old 6, new 8
account: holds bytes, the same while the old library ran (65536): yes; resumed, past its allowance 0 outcome 4, within it 64; at zero yes, freed
unload: new 0, old still works 15, old 0'

# Aether consumers. $1 = label, $2 = library dir, $3 = toolchain home, $4 = its ae
app_want='typed: add 5, wide 7000000, even true
strings: hello 9, 2000 more read: 18890 bytes'
run_app() {
    cp "$SCRIPT_DIR/app.ae" "$2/app.ae"
    if ! (cd "$2" && AETHER_HOME="$3" "$4" build app.ae -o app > app_build.log 2>&1); then
        bad "$1: app.ae did not build" "$2/app_build.log"
        return
    fi
    got="$(cd "$2" && PATH="$2:$PATH" LD_LIBRARY_PATH="$2${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        DYLD_LIBRARY_PATH="$2${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" ./app$exe 2>"$2/app.err" | tr -d '\r')"
    expect "$1" "$got" "$app_want"
    # What the program said on stderr: an abort prints nothing on stdout.
    if [ "$got" != "$app_want" ] && [ -s "$2/app.err" ]; then
        echo "      stderr:"; tail -12 "$2/app.err" | sed 's/^/        /'
    fi
}
# A library with a runtime of its own linked before #3481 on ELF binds that
# runtime to the program's (an `ae build` program exports its runtime), so it
# would run on this one, of another version: this importer refuses it at
# build, naming the rebuild, instead of the program ending on its first call.
refuse_app() {
    cp "$SCRIPT_DIR/app.ae" "$2/app.ae"
    if (cd "$2" && AETHER_HOME="$3" "$4" build app.ae -o app > app_build.log 2>&1); then
        bad "$1: app.ae built; the import was not refused" "$2/app_build.log"
    elif ! grep -q "carries the Aether runtime" "$2/app_build.log" ||
         ! grep -q -- "--shared-runtime" "$2/app_build.log"; then
        bad "$1: the refusal does not say why and name the rebuild" "$2/app_build.log"
    fi
}
this_version="$(tr -d '\r\n' < "$ROOT/VERSION" 2>/dev/null)"
# This ae links a library with a runtime of its own -Bsymbolic on ELF, so it
# keeps that runtime in any program (#3481).
if [ "$ext" = so ] && command -v readelf >/dev/null 2>&1 &&
   ! readelf -d "$TMP/new/libmlib.$ext" 2>/dev/null | grep -q SYMBOLIC; then
    bad "this ae linked libmlib.$ext without -Bsymbolic: it would run on an importing program's runtime"
fi
old_binds_own=1
if [ "$ext" = so ] && [ "$old_version" != "$this_version" ]; then
    if ! command -v readelf >/dev/null 2>&1; then
        old_binds_own=unknown
    elif ! readelf -d "$TMP/old/libmlib.$ext" 2>/dev/null | grep -q SYMBOLIC; then
        old_binds_own=0
    fi
fi
case "$old_binds_own" in
    0) refuse_app "this importer, the $old_version library (linked before #3481)" "$TMP/old" "$ROOT" "$AE" ;;
    unknown) echo "  [NOTE] abi_matrix: no readelf, so this importer with the $old_version library is not run" ;;
    *) run_app "this importer, the $old_version library" "$TMP/old" "$ROOT" "$AE" ;;
esac
run_app "the $old_version importer, this library" "$TMP/new" "$OLD_HOME" "$OLD_AE"

# The shared runtime. $1 = case dir, $2 = library dir, $3 = runtime dir,
# $4 = optional host mode:
# the runtime's files and the host in one directory, as an application
# ships them, the runtime loaded first.
rt_file() { ls "$1/"aether.dll "$1/"libaether.dylib "$1/"libaether.so 2>/dev/null | head -1; }
run_shared() {
    mkdir -p "$1"
    cp "$3/"* "$1/" 2>/dev/null
    cp "$2/libmlib.$ext" "$TMP/host_new$exe" "$1/"
    (cd "$1" && PATH="$1:$PATH" LD_LIBRARY_PATH="$1${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        DYLD_LIBRARY_PATH="$1${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
        "./host_new$exe" "$1/libmlib.$ext" "$(rt_file "$1")" "${4:-}" 2>"$1/err" | norm)
}
if [ -n "$(rt_file "$ROOT/build/shared")" ] && [ -n "$(rt_file "$OLD_HOME/lib/shared")" ]; then
    build_lib "$TMP/newsh" "$ROOT" "$AE" "--shared-runtime" &&
    build_lib "$TMP/oldsh" "$OLD_HOME" "$OLD_AE" "--shared-runtime" && {
        expect "this library on this shared runtime" "$(run_shared "$TMP/sh_nn" "$TMP/newsh" "$ROOT/build/shared")" "$level1"
        got="$(run_shared "$TMP/sh_oo" "$TMP/oldsh" "$OLD_HOME/lib/shared")"
        if [ "$old_level" = 0 ]; then want="$level0"; else want="$level1"; fi
        expect "the $old_version library on its shared runtime" "$got" "$want"
        # The latest published release can have this tree's ABI (especially
        # just after a release). Read the IDs the libraries actually record:
        # equal versions alone would miss a revision or layout-flag change.
        new_abi="$(run_shared "$TMP/sh_nn" "$TMP/newsh" "$ROOT/build/shared" --runtime-abi)"
        old_abi="$(run_shared "$TMP/sh_oo" "$TMP/oldsh" "$OLD_HOME/lib/shared" --runtime-abi)"
        [ -n "$new_abi" ] || { bad "this shared-runtime library records no runtime ABI"; exit 1; }
        if [ "$old_level" != 0 ] && [ -z "$old_abi" ]; then
            bad "the $old_version shared-runtime library records no runtime ABI"
            exit 1
        fi
        got="$(run_shared "$TMP/sh_no" "$TMP/newsh" "$OLD_HOME/lib/shared")"
        if [ "$new_abi" = "$old_abi" ]; then
            expect "this library on the $old_version shared runtime (same ABI)" "$got" "$level1"
            run_app "this importer, the $old_version shared-runtime library (same ABI)" "$TMP/oldsh" "$ROOT" "$AE"
        else
            case "$got" in
                "cannot load the library") [ "$old_level" = 0 ] ||
                    bad "this library on the $old_version shared runtime (level $old_level): refused at load, not by its preflight" ;;
                "catalog: schema 1.x, host ABI level 1
preflight: refused, the library was made for Aether runtime "*) ;;
                *) bad "this library on the $old_version shared runtime is not refused:"
                   printf '%s\n' "$got" | sed 's/^/        /' ;;
            esac
            if (cd "$TMP/oldsh" && cp "$SCRIPT_DIR/app.ae" . && AETHER_HOME="$ROOT" "$AE" build app.ae -o app > app_build.log 2>&1); then
                bad "this importer took the $old_version shared-runtime library"
            elif ! grep -q "host ABI level 0\|was made for the Aether runtime" "$TMP/oldsh/app_build.log"; then
                bad "this importer's refusal of the $old_version shared-runtime library says why" "$TMP/oldsh/app_build.log"
            fi
        fi
        shared_note=", and on the shared runtimes"
    }
else
    shared_note=" (a shared runtime missing: those legs skipped)"
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] abi_matrix: against $old_version (host ABI level $old_level): C hosts of either header and level, two libraries with a runtime each in one process (an account suspended and resumed beside the old one), and Aether consumers both ways run typed calls, strings given back with no byte kept, errors and deadlines at their levels$shared_note; shared-runtime pairs checked by runtime ABI ID"
fi
exit $fail
