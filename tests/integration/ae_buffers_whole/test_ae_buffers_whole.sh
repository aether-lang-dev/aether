#!/bin/sh
# Regression (#2862): what tools/ae.c builds from a path or a name is built
# whole, or refused with an error; it is never cut short.
#
# gcc 15 at -O2 -Wall -Wformat-truncation=1 reported fixed buffers in
# tools/ae.c that a long value could overflow, each of which then went on
# with the cut value: the shared runtime's link argument, the toolchain
# root (from AETHER_HOME and the working directory), a dependency module's
# path, a package module's name, a namespace's function list entries,
# script paths and import lines, a project directory's main.ae, the .wasm
# path, an example's path, the abnormal-exit note on Windows and AETHERC
# for `ae help`. They are heap paths now, checked writes that report an
# overflow, or buffers sized for the longest value.
#
# 1. tools/ae.c compiles with no -Wformat-truncation or
#    -Wstringop-truncation warning, where $CC is gcc 15 or later (the
#    versions that report them).
# 2. `ae build --emit=lib --package <300 bytes>` is refused: the name was
#    cut to 255 bytes and the package looked up under the cut name.
# 3. An AETHER_HOME longer than a toolchain root holds is refused: cut to
#    1023 bytes, it named another directory, and the build went on with
#    whatever toolchain the next strategy found. On Linux, where such a
#    path exists for the tool to find.
# 4. A working directory longer than a toolchain root holds still works as
#    one (a checkout found there by its build/aetherc): it is the root as
#    ".", where 4bf49161 refused it. On Linux, with the tree's directories
#    linked into an 1100-byte directory.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
EXE=""
[ -x "$ROOT/build/ae" ] || EXE=".exe"
AE="$ROOT/build/ae$EXE"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] ae_buffers_whole: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0

# --- 1. no truncation warning from gcc 15 or later ---
CC_BIN="${CC:-gcc}"
if "$CC_BIN" --version 2>/dev/null | head -1 | grep -qi "gcc" &&
   ! "$CC_BIN" --version 2>/dev/null | grep -qi "clang"; then
    major="$("$CC_BIN" -dumpversion 2>/dev/null | cut -d. -f1)"
    case "$major" in ''|*[!0-9]*) major=0 ;; esac
    if [ "$major" -ge 15 ]; then
        if ! ( cd "$ROOT" && "$CC_BIN" -O2 -Wall -Wformat-truncation=1 -Wno-unused-function \
                  -Werror=format-truncation -Werror=stringop-truncation \
                  -c tools/ae.c -o "$TMP/ae.o" \
                  -Icompiler -Iruntime -Iruntime/actors -Iruntime/utils -Iruntime/memory \
                  -Iruntime/scheduler -Iruntime/config -Istd ) > "$TMP/cc.log" 2>&1; then
            echo "  [FAIL] ae_buffers_whole: tools/ae.c has a buffer gcc $major says may be cut"
            grep -E "warning|error" "$TMP/cc.log" | head -10 | sed 's/^/        /'
            fail=1
        fi
    fi
fi

# --- 2. a package name longer than the build keeps ---
long_pkg="$(printf 'p%.0s' $(seq 1 300))"
out="$(cd "$TMP" && "$AE" build --emit=lib --package "$long_pkg" 2>&1)"
if ! printf '%s' "$out" | grep -q "the package name is longer than 255 bytes"; then
    echo "  [FAIL] ae_buffers_whole: a 300-byte --package name was not refused"
    printf '%s\n' "$out" | head -5 | sed 's/^/        /'
    fail=1
fi

# --- 3. an AETHER_HOME longer than a toolchain root ---
if [ "$(uname -s 2>/dev/null)" = "Linux" ]; then
    home="$TMP/home"
    seg="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    while [ ${#home} -lt 1100 ]; do home="$home/$seg"; done
    if mkdir -p "$home/bin" "$home/share/aether" 2>/dev/null && [ -d "$home/share/aether" ]; then
        : > "$home/bin/aetherc"
        chmod +x "$home/bin/aetherc"
        # An ae with no aetherc beside it, so AETHER_HOME is what it reads.
        mkdir -p "$TMP/solo"
        cp "$AE" "$TMP/solo/ae"
        printf 'main() {}\n' > "$TMP/x.ae"
        out="$(cd "$TMP" && AETHER_HOME="$home" "$TMP/solo/ae" build x.ae -o "$TMP/x" 2>&1)"
        if ! printf '%s' "$out" | grep -q "the toolchain root .* bytes long; it can be at most 1023"; then
            echo "  [FAIL] ae_buffers_whole: an AETHER_HOME of ${#home} bytes was not refused"
            printf '%s\n' "$out" | head -5 | cut -c1-200 | sed 's/^/        /'
            fail=1
        fi
    fi
fi

# --- 4. a working directory longer than a toolchain root ---
if [ "$(uname -s 2>/dev/null)" = "Linux" ]; then
    cwd_deep="$TMP/cwd"
    seg="cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
    while [ ${#cwd_deep} -lt 1100 ]; do cwd_deep="$cwd_deep/$seg"; done
    if mkdir -p "$cwd_deep" 2>/dev/null; then
        for d in build runtime std compiler contrib include; do
            [ -e "$ROOT/$d" ] && ln -s "$ROOT/$d" "$cwd_deep/$d"
        done
        mkdir -p "$TMP/solo_cwd"
        cp "$AE" "$TMP/solo_cwd/ae"
        printf 'main() {\n    println("x")\n}\n' > "$cwd_deep/x.ae"
        out="$(cd "$cwd_deep" && env -u AETHER_HOME "$TMP/solo_cwd/ae" check x.ae 2>&1)"
        rc=$?
        if [ $rc -ne 0 ] || ! printf '%s' "$out" | grep -q "OK: no errors"; then
            echo "  [FAIL] ae_buffers_whole: ae does not work in a ${#cwd_deep}-byte working directory"
            printf '%s\n' "$out" | head -3 | cut -c1-200 | sed 's/^/        /'
            fail=1
        fi
    fi
fi

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "  [PASS] ae_buffers_whole: paths and names are built whole or refused, and gcc finds no buffer they could overflow"
exit 0
