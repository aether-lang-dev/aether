#!/bin/sh
# Regression: `ae cflags` prints pkg-config-style include and link
# flags so external tooling can `$(ae cflags)` instead of carrying
# its own copy of the install layout. Issue #329 follow-on item 1.
#
# Also exercises the dynamic -I enumeration that backs cflags — the
# previous hardcoded list silently dropped new modules; the walker
# can't miss them. Confirmed by checking that recently-added stdlib
# subdirs (std/cryptography, std/zlib, std/dl, std/config, std/actors,
# std/http*) all appear in the output.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] ae_cflags: $AE not built"
    exit 0
fi

flags="$("$AE" cflags 2>/dev/null)"
if [ -z "$flags" ]; then
    echo "  [FAIL] ae_cflags: produced empty output"
    exit 1
fi

# Spot-check: every previously-missing stdlib subdir must show up.
# Normalise backslashes to forward slashes first — Windows mingw and
# MSYS2 produce paths with '\' separators while the rest of the world
# uses '/'. Both are valid -I arguments to gcc; the dedup-via-walker
# guarantee we want to test (#329's stale-list fix) is path-separator
# agnostic.
flags_normalized=$(printf %s "$flags" | tr '\\' '/')
for needle in std/cryptography std/zlib std/dl std/config std/actors std/http; do
    if ! printf %s "$flags_normalized" | grep -q -- "-I.*$needle"; then
        echo "  [FAIL] ae_cflags: missing -I for $needle (the dynamic walker should pick it up)"
        echo "  ---- output ----"
        echo "$flags"
        exit 1
    fi
done

# Must include -laether (or skip if no precompiled lib was built).
if ! printf %s "$flags" | grep -q -- "-laether"; then
    if [ -f "$ROOT/build/libaether.a" ]; then
        echo "  [FAIL] ae_cflags: libaether.a exists but -laether is missing from output"
        echo "  ---- output ----"
        echo "$flags"
        exit 1
    fi
fi

# --cflags subset: must be -I-only, no -L / -l flags.
cflags_only="$("$AE" cflags --cflags 2>/dev/null)"
if printf %s "$cflags_only" | grep -qE -- "(^|[[:space:]])-(L|l)"; then
    echo "  [FAIL] ae_cflags --cflags: leaked link flags into the compile-only subset"
    echo "  ---- output ----"
    echo "$cflags_only"
    exit 1
fi

# --libs subset: no -I flags.
libs_only="$("$AE" cflags --libs 2>/dev/null)"
if printf %s "$libs_only" | grep -q -- "-I"; then
    echo "  [FAIL] ae_cflags --libs: leaked include flags into the link-only subset"
    echo "  ---- output ----"
    echo "$libs_only"
    exit 1
fi

# Transitive deps: if libaether.a was compiled with PCRE2 / OpenSSL /
# zlib / nghttp2 (detectable via the unresolved-symbol set in its
# archive), `ae cflags --libs` MUST emit the corresponding -l flags
# — otherwise downstream `gcc app.c $(ae cflags)` fails to link with
# `undefined reference to pcre2_*` and similar. This pattern shipped
# v0.193.0 broken; fix here in tools/ae.c cmd_cflags. The check uses
# nm on the local libaether.a so it only fails when there's a real
# symbol-to-flag mismatch (it self-skips on a no-pcre2 build).
LIBAETHER="$ROOT/build/libaether.a"
if [ -f "$LIBAETHER" ] && command -v nm >/dev/null 2>&1; then
    # For each (symbol-prefix, expected-flag-pattern) pair: if the
    # archive references the symbol family AND does not define it
    # itself, the link flags must mention the corresponding library.
    # The self-definition subtraction matters for the vendored pcre2
    # engine (#1389): aether_regex.o's `U pcre2_*` refs are satisfied
    # by aether_pcre2_vendored.o in the same archive, so a vendored
    # build correctly emits no -lpcre2-8 — the old any-U-ref form
    # would demand the flag exactly on the boxes that lack the lib.
    nm_out="$(mktemp)"
    defs_out="$(mktemp)"
    nm "$LIBAETHER" 2>/dev/null > "$nm_out"
    awk 'NF>=3 && $2~/^[TDBRSW]$/{print $3}' "$nm_out" | sort -u > "$defs_out"
    check_dep() {
        sym_pat="$1"; flag_pat="$2"; name="$3"
        if awk '$1=="U"{print $2}' "$nm_out" | grep "^$sym_pat" | sort -u \
                | grep -qvxF -f "$defs_out"; then
            if ! printf %s "$libs_only" | grep -qE -- "$flag_pat"; then
                echo "  [FAIL] ae_cflags --libs: libaether.a references $name ($sym_pat) but no $flag_pat in cflags"
                echo "  ---- libs ----"
                echo "$libs_only"
                exit 1
            fi
        fi
    }
    check_dep 'pcre2_'    '-lpcre2-8'           'PCRE2'
    check_dep 'SSL_'      '-l(ssl|crypto)'      'OpenSSL'
    check_dep 'deflate'   '-lz'                 'zlib'
    check_dep 'nghttp2_'  '-lnghttp2'           'nghttp2'
    rm -f "$nm_out" "$defs_out"
fi

# Functional check: `gcc trivial.c $(ae cflags) -o out` runs end-to-end.
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
cat > "$tmpdir/trivial.c" <<'C'
#include <stdio.h>
int main(void) { printf("ae_cflags ok\n"); return 0; }
C

if ! gcc "$tmpdir/trivial.c" $flags -o "$tmpdir/trivial" >"$tmpdir/gcc.log" 2>&1; then
    echo "  [FAIL] ae_cflags: gcc \$(ae cflags) failed end-to-end"
    cat "$tmpdir/gcc.log"
    exit 1
fi

actual="$("$tmpdir/trivial")"
if [ "$actual" != "ae_cflags ok" ]; then
    echo "  [FAIL] ae_cflags: end-to-end binary printed '$actual' (expected 'ae_cflags ok')"
    exit 1
fi

echo "  [PASS] ae_cflags: full / --cflags / --libs subsets correct, transitive deps present, gcc \$(ae cflags) builds"
exit 0
