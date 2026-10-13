#!/bin/sh
# One program of the .ae sweep for test_msvc_compat.sh's whole-sweep check
# (#2885): its C on the non-GCC path (AETHER_GCC_COMPAT forced to 0), under
# gcc -std=c11 -pedantic, holds no GNU statement expression and no
# __auto_type outside the preprocessor lines that spell the GCC path.
#
#   strict_c11_sweep_one.sh <program> <tmpdir> <root> <aetherc> <flags>
#
# The C is the one the sweep kept (`make test-ae` with AE_SWEEP_KEEP_C=1 leaves
# build/test_<name>.c, named as run_ae_test.sh names the binary), so nothing
# is generated twice. A program the sweep took from the build cache, or did
# not build, has none, and its C is generated here. Results are files in
# <tmpdir>: bad_<name> (what was found), gen_<name> (generated here),
# nogen_<name> (no C could be generated: nothing to check).

f="$1"
tmp="$2"
root="$3"
aetherc="$4"
flags="$5"

name=$(echo "$f" | sed "s|tests/||;s|/|_|g;s|\.ae$||")
c="$root/build/test_$name.c"
if [ ! -f "$c" ]; then
    dir=$(dirname "$f")
    base=$(basename "$f")
    c="$tmp/$name.c"
    if [ -d "$dir/lib" ]; then
        (cd "$dir" && "$aetherc" "$base" "$c") >/dev/null 2>&1
    else
        "$aetherc" "$f" "$c" >/dev/null 2>&1
    fi
    if [ $? -ne 0 ] || [ ! -f "$c" ]; then
        : > "$tmp/nogen_$name"
        exit 0
    fi
    : > "$tmp/gen_$name"
fi

ng="$tmp/${name}_nogcc.c"
{ echo "#define AETHER_GCC_COMPAT 0"; sed '/^#line /d' "$c"; } > "$ng"
# shellcheck disable=SC2086
gcc -fsyntax-only -std=c11 -pedantic -Wno-unused-label $flags "$ng" > "$tmp/$name.log" 2>&1

found=""
grep -q "braced-groups" "$tmp/$name.log" && found="a GNU statement expression"
if grep -v "^#" "$ng" | grep -q "__auto_type"; then
    found="${found:+$found, }__auto_type"
fi
# A fatal error stops gcc before the end of the file: the rest is unchecked.
grep -q "fatal error" "$tmp/$name.log" && found="${found:+$found, }a fatal error (not checked to the end)"
if [ -n "$found" ]; then
    printf '%s %s\n' "$f" "$found" > "$tmp/bad_$name"
else
    rm -f "$ng" "$tmp/$name.log" "$tmp/$name.c"
fi
