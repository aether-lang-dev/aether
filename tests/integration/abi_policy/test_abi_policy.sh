#!/bin/sh
# #2957: what a shipped library promises, checked at each boundary the
# policy (docs/abi-policy.md) names, before any incompatible call:
# - the importer: a catalog of another major, one that requires a duty this
#   importer does not know (refused, naming it, both versions and the fix),
#   one of a newer minor with nothing new to do (read), and shared-runtime
#   libraries of another runtime ABI or of none recorded (refused), none of
#   them leaving its stub directory in the temp dir (#3485);
# - the first call: a library compiled for another runtime ABI than the
#   runtime it links returns zero values with AETHER_INCOMPATIBLE, none of
#   its code run, and its preflight says why;
# - compile time: generated C compiled against another runtime's headers;
# - the rendezvous: a sandbox rendezvous of another layout loaded first.
#
# Fixtures, not tests: app.ae imports the hand-made catalog (fake_catalog.c)
# and abi_lib.ae is a library for the C host; the driver builds them.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
CC="${CC:-cc}"
export AETHER_HOME="$ROOT"
export AETHER_STACK_TRACE=0

[ -x "$AE" ] || { echo "  [SKIP] abi_policy: $AE not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT
fail=0
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) ext=dll; exe=.exe; dl="" ;;
    Darwin) ext=dylib; exe=""; dl="" ;;
    Linux) ext=so; exe=""; dl="-ldl" ;;
    *) ext=so; exe=""; dl="" ;;
esac

# $1 = what failed, $2 = a log to show
bad() {
    echo "  [FAIL] abi_policy: $1"
    [ -n "$2" ] && sed 's/^/        /' "$2" | head -8
    fail=1
}

# $1 = case dir, the rest = -D flags for the hand-made catalog
fake() {
    d="$TMP/$1"; shift
    mkdir -p "$d"
    if ! "$CC" -shared -fPIC -I"$ROOT/runtime" "$@" "$SCRIPT_DIR/fake_catalog.c" -o "$d/libfake.$ext" \
            > "$d/cc.log" 2>&1; then
        bad "the hand-made catalog did not build" "$d/cc.log"
        return 1
    fi
    cp "$SCRIPT_DIR/app.ae" "$d/app.ae"
}

# $1 = case dir: 0 when the importer built the program. #3485: in a temp
# directory of its own, so what the import leaves there can be seen: the
# stub directory it wrote is removed, whether it was refused or not.
import_it() {
    tmp="$TMP/$1/tmp"
    mkdir -p "$tmp"
    tmp_env="$tmp"
    command -v cygpath >/dev/null 2>&1 && tmp_env="$(cygpath -m "$tmp")"
    (cd "$TMP/$1" && TMPDIR="$tmp_env" TEMP="$tmp_env" TMP="$tmp_env" \
        "$AE" build app.ae -o app > build.log 2>&1)
    rc=$?
    left="$(ls -d "$tmp"/ae-binimport-* 2>/dev/null)"
    [ -z "$left" ] || bad "$1: the import left its stub directory ($left)" "$TMP/$1/build.log"
    return $rc
}

# $1 = case dir, $2 = a fixed string its build log must hold, $3 = label
refused() {
    if import_it "$1"; then
        bad "$3: imported"
    elif ! grep -qF "$2" "$TMP/$1/build.log"; then
        bad "$3: the refusal does not say \"$2\"" "$TMP/$1/build.log"
    fi
}

if fake major -DSCHEMA='"2.0"' -DREQUIRES='"outcome_check"'; then
    refused major "reads schema 1 only" "a schema 2 catalog"
fi

if fake duty -DSCHEMA='"1.11"' -DREQUIRES='"outcome_check,warp_drive"'; then
    refused duty "requires \`warp_drive\` of its importer" "an unknown duty"
    grep -qF "built by Aether 9.9.9" "$TMP/duty/build.log" || bad "the duty refusal names the library's version" "$TMP/duty/build.log"
    grep -qF "or rebuild the library" "$TMP/duty/build.log" || bad "the duty refusal names the fix" "$TMP/duty/build.log"
fi

if fake newer -DSCHEMA='"1.12"' -DREQUIRES='"outcome_check"'; then
    if import_it newer; then
        got="$(cd "$TMP/newer" && PATH="$TMP/newer:$PATH" LD_LIBRARY_PATH="$TMP/newer" \
               DYLD_LIBRARY_PATH="$TMP/newer" ./app$exe 2>&1 | tr -d '\r')"
        [ "$got" = "add: 5" ] || bad "a newer minor with known duties runs: got \"$got\""
    else
        bad "a newer minor with only known duties was refused" "$TMP/newer/build.log"
    fi
fi

if fake otherabi -DSHARED -DSCHEMA='"1.11"' -DREQUIRES='"outcome_check,shared_runtime,runtime_abi"' \
        -DRUNTIME_ABI='"0.0.1/abi9"'; then
    refused otherabi "was made for the Aether runtime 0.0.1/abi9" "a shared runtime of another ABI"
fi

if fake legacy -DSHARED -DSCHEMA='"1.5"'; then
    refused legacy "host ABI level 0" "a legacy shared-runtime library"
fi

# The first call: the same library compiled as its runtime was, and for
# another runtime ABI.
got_same=""
got_other=""
if ! "$AETHERC" --emit=lib --with=extern "$SCRIPT_DIR/abi_lib.ae" "$TMP/abi_lib.c" > "$TMP/abi_lib.log" 2>&1; then
    bad "aetherc --emit=lib failed" "$TMP/abi_lib.log"
else
    for v in same other; do
        flag=""
        [ $v = other ] && flag="-DAETHER_DEBUG_MAILBOX"
        # shellcheck disable=SC2046,SC2086
        if ! "$CC" -O1 $flag -I"$ROOT/include" -I"$ROOT/runtime" $("$AE" cflags --cflags) \
                "$TMP/abi_lib.c" "$SCRIPT_DIR/host_abi.c" -o "$TMP/host_$v" \
                $("$AE" cflags --libs) > "$TMP/host_$v.log" 2>&1; then
            bad "the $v-runtime host did not build" "$TMP/host_$v.log"
            continue
        fi
        out="$("$TMP/host_$v" 2>"$TMP/host_$v.err" | tr -d '\r')"
        if [ $v = same ]; then got_same="$out"; else got_other="$out"; fi
    done
    want_same='check 0
bump: 42 outcome 0
greet: a string outcome 0
ran 2'
    want_other='check -1, names both: yes
bump: 0 outcome 6, names both: yes
greet: (null) outcome 6
ran 0'
    if [ "$got_same" != "$want_same" ]; then
        bad "the same runtime ABI:"
        printf '%s\n' "$got_same" | sed 's/^/        /'
    fi
    if [ "$got_other" != "$want_other" ]; then
        bad "another runtime ABI:"
        printf '%s\n' "$got_other" | sed 's/^/        /'
    fi
    [ "$(grep -c "was made for Aether runtime" "$TMP/host_other.err" 2>/dev/null)" = 1 ] ||
        bad "the refusal is reported once" "$TMP/host_other.err"
fi

# Compile time: the generated C against headers of another runtime ABI.
mkdir -p "$TMP/inc"
sed 's/^#define AETHER_RUNTIME_ABI_REV .*/#define AETHER_RUNTIME_ABI_REV 99/' "$ROOT/runtime/aether_abi.h" > "$TMP/inc/aether_abi.h"
cp "$ROOT/runtime/aether_version.h" "$TMP/inc/"
# shellcheck disable=SC2046
if "$CC" -c -I"$TMP/inc" -I"$ROOT/include" -I"$ROOT/runtime" $("$AE" cflags --cflags) \
        "$TMP/abi_lib.c" -o "$TMP/abi_lib.o" > "$TMP/compile_other.log" 2>&1; then
    bad "generated C compiled against another runtime's headers"
elif ! grep -qF "generated by aetherc" "$TMP/compile_other.log"; then
    bad "the compile-time refusal says why" "$TMP/compile_other.log"
fi

# The rendezvous: a sandbox rendezvous of another layout loaded first, the
# one before this runtime's (version 1, no size) and a later one, then a
# library with a runtime copy (sbx_copy.c), from a host with no runtime of
# its own (sbx_driver.c), so the foreign one is first on every platform
# (#3437).
# shellcheck disable=SC2046
if "$CC" -shared -fPIC -O1 -DAETHER_HAS_SANDBOX -I"$ROOT/include" -I"$ROOT/runtime" $("$AE" cflags --cflags) \
        "$SCRIPT_DIR/sbx_copy.c" -o "$TMP/sbx_copy.$ext" $("$AE" cflags --libs) $dl > "$TMP/sbx_cc.log" 2>&1 &&
   "$CC" -O1 "$SCRIPT_DIR/sbx_driver.c" -o "$TMP/sbx_driver" $dl >> "$TMP/sbx_cc.log" 2>&1; then
    for rvv in 1 9; do
        if ! "$CC" -shared -fPIC -I"$ROOT/runtime" -DRV_VERSION=$rvv "$SCRIPT_DIR/foreign_sbx.c" -o "$TMP/foreign_sbx$rvv.$ext" \
                > "$TMP/sbx_cc$rvv.log" 2>&1; then
            bad "the version-$rvv rendezvous did not build" "$TMP/sbx_cc$rvv.log"
            continue
        fi
        got="$("$TMP/sbx_driver" "$TMP/foreign_sbx$rvv.$ext" "$TMP/sbx_copy.$ext" 2>"$TMP/sbx$rvv.err" | tr -d '\r')"
        want='before: 1
in a deny-all level: 0
after: 1
foreign levels called: 0'
        if [ "$got" != "$want" ]; then
            bad "a version-$rvv sandbox rendezvous: the copy enforces its own levels, and never calls it"
            printf '%s\n' "$got" | sed 's/^/        /'
        fi
        [ "$(grep -c "cannot share sandbox levels" "$TMP/sbx$rvv.err")" = 1 ] ||
            bad "a version-$rvv sandbox rendezvous is reported once" "$TMP/sbx$rvv.err"
    done
else
    bad "the sandbox copy or its driver did not build" "$TMP/sbx_cc.log"
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] abi_policy: the importer refuses another major, an unknown duty and a shared runtime of another or no ABI, and reads a newer minor; a library compiled for another runtime returns AETHER_INCOMPATIBLE and runs nothing; generated C against other headers does not compile; a sandbox rendezvous of another layout is refused once"
fi
exit $fail
