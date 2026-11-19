#!/bin/sh
# `ae build --target=aarch64-ios` and the simulator variants.
#
# iOS is the one cross target NOT served by zig: Apple's SDKs are Xcode-
# licensed and not redistributable, so these triples shell to `xcrun clang`
# with the SDK xcrun reports. That also makes iOS the one cross target where
# --emit=lib is supported, because an iOS app is built by Xcode and what it
# wants from Aether is a loadable library, not a standalone binary (iOS will
# not run a loose executable at all).
#
# Asserts:
#   - aarch64-ios / aarch64-ios-simulator / x86_64-ios-simulator are accepted
#   - the Mach-O PLATFORM distinguishes device (IOS) from simulator
#     (IOSSIMULATOR) — same arch, non-interchangeable binaries
#   - AETHER_IOS_MIN sets the deployment target (LC_BUILD_VERSION minos)
#   - --emit=lib produces a dylib whose install_name is @rpath-relative (an
#     absolute build path there fails to load from inside an .app bundle)
#   - --emit=csrc needs no Xcode at all (it never invokes the toolchain)
#   - the Mac Catalyst triples (-macabi) are accepted, build against the MACOS
#     SDK, and carry platform MACCATALYST
#   - --emit=staticlib produces an ar archive holding the program AND the
#     runtime objects (iOS forbids third-party dylibs, so an App Store build
#     must link Aether statically) and is refused on a native build
#
# Skips off macOS, or when Xcode's iPhoneOS SDK is absent (the Command Line
# Tools alone do not carry one).
#
# NB on cost: an executable/dylib build recompiles the whole runtime and
# stdlib for the target (~90 TUs, no per-target archive cache yet), so this
# does exactly TWO of those. Everything that can be asserted on a single-TU
# --emit=obj is, because that path is ~100x cheaper and carries the same
# LC_BUILD_VERSION the linked artifacts do.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
SRC="$SCRIPT_DIR/mylib.ae"      # exports, no main() — lib/obj/csrc
EXE_SRC="$SCRIPT_DIR/hello.ae"  # has main() — executable build

TMP="${TMPDIR:-/tmp}/ae_cross_ios_$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP" || true' EXIT

fail() { echo "  FAIL: $1"; exit 1; }

# --- csrc first: it must work with no Xcode, so it runs before the skip ---
"$AE" build --target=aarch64-ios --emit=csrc "$SRC" -o "$TMP/csrc" >/dev/null 2>&1 \
    || fail "--target=aarch64-ios --emit=csrc was rejected"
[ -f "$TMP/csrc.c" ] && [ -f "$TMP/csrc.h" ] \
    || fail "--emit=csrc under an iOS target emitted no .c/.h"

# Catalyst accepts --emit=csrc with no Xcode too, for the same reason.
for CAT in aarch64-ios-macabi arm64-ios-macabi x86_64-ios-macabi; do
    "$AE" build --target="$CAT" --emit=csrc "$SRC" -o "$TMP/cat_csrc" >/dev/null 2>&1 \
        || fail "--target=$CAT --emit=csrc was rejected"
    [ -f "$TMP/cat_csrc.c" ] || fail "--target=$CAT --emit=csrc emitted no .c"
    rm -f "$TMP/cat_csrc.c" "$TMP/cat_csrc.h"
done

# --emit=staticlib is cross-only: on a native build it must be REFUSED rather
# than silently producing a shared library under a .a name. Host-independent,
# so it runs everywhere.
if "$AE" build --emit=staticlib "$SRC" -o "$TMP/native.a" >"$TMP/log" 2>&1; then
    fail "--emit=staticlib was accepted on a native build (should require a cross target)"
fi
grep -q 'requires a cross target' "$TMP/log" \
    || fail "--emit=staticlib native rejection did not explain itself: $(cat "$TMP/log")"

# The --emit list in the error path must name staticlib, or it is undiscoverable.
"$AE" build --emit=nonsense "$SRC" >"$TMP/log" 2>&1 || true
grep -q 'staticlib' "$TMP/log" \
    || fail "--emit error message does not list staticlib: $(cat "$TMP/log")"

if [ "$(uname -s)" != "Darwin" ]; then
    echo "  [skip] iOS cross checks: not a macOS host"
    echo "  PASS: --emit=csrc (iOS + Catalyst), staticlib guards (rest skipped)"
    exit 0
fi

# #3052: every step below drives xcrun, and one hung once on CI (the
# staticlib build, killed by the sweep's per-test timeout with no word on
# which child stalled). Each runs bounded: past STEP_LIMIT seconds, well
# under the sweep's 420 s, the processes still running under it are
# stopped (ps, walking its tree until no new one appears), printed and
# killed, and the test fails naming them.
#
# A step's limit is also what is left of the sweep's own budget for this
# test (AE_SH_TEST_DEADLINE, from tests/scripts/run_ae_sh_dir.sh), less
# DEADLINE_MARGIN for the report: eight steps of up to 180 s each fit in
# 420 s only when most are quick, and on a slow macOS x86_64 runner they
# were not (2026-11-12). The sweep then killed the test mid-step, before
# any step reached its own limit, and the hang went unexplained again.
STEP_LIMIT="${AE_IOS_STEP_TIMEOUT:-180}"
DEADLINE_MARGIN=30

# step_limit: the seconds the next step may take.
step_limit() {
    lim="$STEP_LIMIT"
    if [ -n "${AE_SH_TEST_DEADLINE:-}" ]; then
        left=$((AE_SH_TEST_DEADLINE - $(date +%s) - DEADLINE_MARGIN))
        [ "$left" -lt "$lim" ] && lim="$left"
        [ "$lim" -lt 1 ] && lim=1
    fi
    echo "$lim"
}

# proc_tree <pid>: <pid> and every process descended from it.
proc_tree() {
    all=$(ps -A -o pid= -o ppid=)
    list="$1"
    frontier="$1"
    while [ -n "$frontier" ]; do
        next=""
        for p in $frontier; do
            next="$next $(printf '%s\n' "$all" | awk -v p="$p" '$2 == p { print $1 }')"
        done
        next=$(echo $next)
        [ -n "$next" ] && list="$list $next"
        frontier="$next"
    done
    echo $list
}

# running <pid>: the process is there and has not exited. A shell that has
# not reaped its background child yet leaves it a zombie, which `kill -0`
# would still find.
running() {
    st=$(ps -o stat= -p "$1" 2>/dev/null) || return 1
    st=$(echo $st)
    case "$st" in
        ""|Z*) return 1 ;;
        *) return 0 ;;
    esac
}

# bounded <label> <command...>: runs the command with its output in
# $TMP/log and returns its status, or, past step_limit seconds of wall
# time, freezes what is still running under it, reports it, kills it and
# fails. Each step's time is printed, so a slow run says where it went.
bounded() {
    label="$1"
    shift
    limit=$(step_limit)
    started=$(date +%s)
    "$@" >"$TMP/log" 2>&1 &
    bpid=$!
    waited=0
    while running "$bpid"; do
        waited=$(($(date +%s) - started))
        if [ "$waited" -ge "$limit" ]; then
            # Frozen before it is read, so nothing under the step starts a
            # process the kill below would miss: the step is stopped, then
            # each process a walk finds under it, until a walk finds none
            # new (a stopped process starts nothing).
            # kill-ok: macOS-only (a bounded step runs xcrun), never on Windows; $bpid is this step, still running
            kill -STOP "$bpid" 2>/dev/null || true
            tree="$bpid"
            while :; do
                fresh=""
                for p in $(proc_tree "$bpid"); do
                    case " $tree " in
                        *" $p "*) ;;
                        *) fresh="$fresh $p" ;;
                    esac
                done
                [ -n "$fresh" ] || break
                # kill-ok: macOS-only, as above; each pid was just read from ps under the stopped step
                for p in $fresh; do kill -STOP "$p" 2>/dev/null || true; done
                tree="$tree$fresh"
            done
            echo "  TIMEOUT: $label still running after ${waited}s (limit ${limit}s); its processes:"
            echo "      PID  PPID     ELAPSED COMMAND"
            stuck=""
            for p in $tree; do
                line=$(ps -o pid=,ppid=,etime=,command= -p "$p" 2>/dev/null) || continue
                echo "    $line"
                [ "$p" = "$bpid" ] || stuck="$stuck $(ps -o comm= -p "$p" 2>/dev/null)"
            done
            # A stopped process takes TERM when it is continued.
            # kill-ok: macOS-only (a bounded step runs xcrun), never on Windows; the tree is stopped, read from ps
            for p in $tree; do kill -TERM "$p" 2>/dev/null || true; done
            # kill-ok: macOS-only, as above; the same stopped tree, continued to take its TERM
            for p in $tree; do kill -CONT "$p" 2>/dev/null || true; done
            sleep 2
            # kill-ok: macOS-only, as above; what survived TERM
            for p in $tree; do kill -KILL "$p" 2>/dev/null || true; done
            wait "$bpid" 2>/dev/null || true
            echo "  output so far:"
            sed 's/^/    /' "$TMP/log"
            fail "$label hung: killed after ${waited}s (limit ${limit}s); still running under it:${stuck:- nothing but the step itself}"
        fi
        sleep 1
    done
    rc=0
    wait "$bpid" || rc=$?
    echo "  step: $label took $(($(date +%s) - started))s"
    return $rc
}

if ! bounded "xcrun --sdk iphoneos --show-sdk-path" xcrun --sdk iphoneos --show-sdk-path; then
    echo "  [skip] iOS cross checks: no iPhoneOS SDK (Xcode not installed)"
    echo "  PASS: --emit=csrc under an iOS target (rest skipped)"
    exit 0
fi

# Prime xcrun's lookups once, one at a time, before the builds below run
# their compiles in parallel (#3052): each SDK the triples use (device,
# simulator, and the macOS one Catalyst builds against) and the clang and
# ar ae calls through `xcrun --sdk <sdk>`.
for SDK in iphoneos iphonesimulator macosx; do
    bounded "priming xcrun for the $SDK SDK" sh -c \
        "xcrun --sdk $SDK --show-sdk-path && xcrun --sdk $SDK --find clang && xcrun --sdk $SDK --find ar" \
        || { cat "$TMP/log"; fail "xcrun could not resolve the $SDK SDK's toolchain"; }
done

macho_platform() { vtool -show-build "$1" 2>/dev/null | awk '/platform/ {print $2; exit}'; }
macho_minos()    { vtool -show-build "$1" 2>/dev/null | awk '/minos/ {print $2; exit}'; }

# --- 1. target matrix, on the cheap --emit=obj path ---
build_obj() { # <target> <out> [env-assignment]
    if [ -n "$3" ]; then
        bounded "--target=$1 --emit=obj ($3)" \
            env "$3" "$AE" build --target="$1" --emit=obj "$SRC" -o "$2" \
            || { cat "$TMP/log"; fail "--target=$1 --emit=obj ($3) failed"; }
    else
        bounded "--target=$1 --emit=obj" "$AE" build --target="$1" --emit=obj "$SRC" -o "$2" \
            || { cat "$TMP/log"; fail "--target=$1 --emit=obj failed"; }
    fi
}

build_obj aarch64-ios            "$TMP/dev.o"
build_obj aarch64-ios-simulator  "$TMP/sim.o"
build_obj x86_64-ios-simulator   "$TMP/sim64.o"
build_obj aarch64-ios            "$TMP/min.o" AETHER_IOS_MIN=17.0

file "$TMP/dev.o"   | grep -q 'Mach-O 64-bit object arm64' \
    || fail "device object is not arm64 Mach-O: $(file "$TMP/dev.o")"
file "$TMP/sim64.o" | grep -q 'Mach-O 64-bit object x86_64' \
    || fail "x86_64 simulator object is not x86_64 Mach-O: $(file "$TMP/sim64.o")"

# Device and simulator are the same ARCH but different PLATFORM; that is the
# whole reason they are separate targets rather than one with a flag.
[ "$(macho_platform "$TMP/dev.o")" = "IOS" ] \
    || fail "device object platform is '$(macho_platform "$TMP/dev.o")', expected IOS"
[ "$(macho_platform "$TMP/sim.o")" = "IOSSIMULATOR" ] \
    || fail "simulator object platform is '$(macho_platform "$TMP/sim.o")', expected IOSSIMULATOR"

[ "$(macho_minos "$TMP/dev.o")" = "15.0" ] \
    || fail "default minos is '$(macho_minos "$TMP/dev.o")', expected 15.0"
[ "$(macho_minos "$TMP/min.o")" = "17.0" ] \
    || fail "AETHER_IOS_MIN=17.0 gave minos '$(macho_minos "$TMP/min.o")'"

# --- 2. a real executable link (full runtime compile) ---
bounded "--target=aarch64-ios executable build" \
    "$AE" build --target=aarch64-ios "$EXE_SRC" -o "$TMP/dev" \
    || { cat "$TMP/log"; fail "--target=aarch64-ios executable build failed"; }
file "$TMP/dev" | grep -q 'Mach-O 64-bit executable arm64' \
    || fail "device build is not a Mach-O arm64 executable: $(file "$TMP/dev")"
[ "$(macho_platform "$TMP/dev")" = "IOS" ] \
    || fail "device exe platform is '$(macho_platform "$TMP/dev")', expected IOS"

# --- 3. --emit=lib is SUPPORTED on iOS (unlike the zig cross targets) ---
bounded "--target=aarch64-ios --emit=lib" \
    "$AE" build --target=aarch64-ios --emit=lib "$SRC" -o "$TMP/libmylib.dylib" \
    || { cat "$TMP/log"; fail "--target=aarch64-ios --emit=lib was rejected"; }
file "$TMP/libmylib.dylib" | grep -q 'Mach-O 64-bit dynamically linked shared library arm64' \
    || fail "--emit=lib did not produce an arm64 dylib: $(file "$TMP/libmylib.dylib")"
# An absolute install_name here is the classic "works on my Mac, fails in the
# .app" bug: the loader would look for the dylib at its BUILD path.
otool -D "$TMP/libmylib.dylib" | tail -1 | grep -q '^@rpath/' \
    || fail "dylib install_name is not @rpath-relative: $(otool -D "$TMP/libmylib.dylib" | tail -1)"
nm -gU "$TMP/libmylib.dylib" | grep -q '_aether_add' \
    || fail "dylib does not export _aether_add"

# --- 4. Mac Catalyst (-macabi): a third platform, on the macOS SDK ---
# aether-ui builds and pixel-tests its UIKit backend on Catalyst, so this is
# the triple its CI actually links. It carries "-ios" like the device arm but
# resolves to `xcrun --sdk macosx`; getting that pairing wrong fails the link
# rather than producing a wrong-platform binary, so assert the platform stamp.
build_obj aarch64-ios-macabi "$TMP/cat.o"
file "$TMP/cat.o" | grep -q 'Mach-O 64-bit object arm64' \
    || fail "Catalyst object is not arm64 Mach-O: $(file "$TMP/cat.o")"
[ "$(macho_platform "$TMP/cat.o")" = "MACCATALYST" ] \
    || fail "Catalyst object platform is '$(macho_platform "$TMP/cat.o")', expected MACCATALYST"
# Catalyst's floor is its own, and differs by arch: the macabi ABI does not
# exist before iOS 13.1 (x86_64), and arm64 Catalyst did not exist until Apple
# Silicon, where clang RAISES anything lower to 14.0. Asking for 13.1 on arm64
# would produce a binary stamped 14.0 — a triple that does not describe its own
# output. Assert what each arch actually stamps.
[ "$(macho_minos "$TMP/cat.o")" = "14.0" ] \
    || fail "Catalyst arm64 default minos is '$(macho_minos "$TMP/cat.o")', expected 14.0"

build_obj x86_64-ios-macabi "$TMP/cat64.o"
[ "$(macho_platform "$TMP/cat64.o")" = "MACCATALYST" ] \
    || fail "x86_64 Catalyst platform is '$(macho_platform "$TMP/cat64.o")', expected MACCATALYST"
[ "$(macho_minos "$TMP/cat64.o")" = "13.1" ] \
    || fail "Catalyst x86_64 default minos is '$(macho_minos "$TMP/cat64.o")', expected 13.1"

# AETHER_IOS_MIN still overrides on Catalyst, as on the other Apple triples.
build_obj aarch64-ios-macabi "$TMP/catmin.o" AETHER_IOS_MIN=16.0
[ "$(macho_minos "$TMP/catmin.o")" = "16.0" ] \
    || fail "AETHER_IOS_MIN=16.0 on Catalyst gave minos '$(macho_minos "$TMP/catmin.o")'"

# --- 5. --emit=staticlib: the shape an App Store build actually links ---
# iOS forbids third-party dynamic libraries, so the dylib in section 3 cannot
# ship inside an .app; the app must link Aether statically. One archive holds
# the program's own objects AND the runtime/stdlib, so Xcode needs exactly one
# file in "Link Binary With Libraries".
bounded "--target=aarch64-ios --emit=staticlib" \
    "$AE" build --target=aarch64-ios --emit=staticlib "$SRC" -o "$TMP/libmylib.a" \
    || { cat "$TMP/log"; fail "--target=aarch64-ios --emit=staticlib was rejected" ; }
file "$TMP/libmylib.a" | grep -qi 'archive' \
    || fail "--emit=staticlib did not produce an archive: $(file "$TMP/libmylib.a")"
# The program object alone would be a useless .a — the runtime has to be in it.
ar t "$TMP/libmylib.a" | grep -q '__aether_program.o' \
    || fail "static archive does not contain the program object"
[ "$(ar t "$TMP/libmylib.a" | wc -l)" -gt 10 ] \
    || fail "static archive holds only $(ar t "$TMP/libmylib.a" | wc -l) members; runtime objects missing"
nm -g "$TMP/libmylib.a" 2>/dev/null | grep -q '_aether_add' \
    || fail "static archive does not export _aether_add"

echo "  PASS: iOS device/simulator objects, arm64 exe, @rpath dylib, csrc,"
echo "        Catalyst MACCATALYST/minos 13.1, static archive; AETHER_IOS_MIN honoured"
