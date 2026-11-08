#!/bin/sh
# Re-vendor libfyaml into std/yaml/libfyaml/.
#
# Downloads the pinned upstream release, verifies its checksum, and copies
# the byte-identical subset that std/yaml/aether_libfyaml_vendored.c
# compiles, then regenerates std/yaml/aether_libfyaml_names.h. Run from the
# repo root. To bump the version: update VERSION and SHA256 below, run the
# script, review the diff, rebuild with `make YAML=vendored` and run
# std/yaml's tests, and update THIRD_PARTY_LICENSES.md if a licence changed.
#
# What is verified: libfyaml publishes a release tarball as a GitHub release
# asset (libfyaml-<v>.tar.gz, made by upstream's `make dist`), so the
# checksum is that asset's SHA-256, taken when this version was pinned. It
# is not the git archive of the tag; the release asset is what upstream
# publishes as the release. Upstream does not sign it.
#
# The vendored tree is VERBATIM upstream — no local patches, ever. Two
# things differ from the tarball's layout, and neither changes a byte:
#   - the library sources upstream keeps in src/lib, src/util, src/xxhash,
#     src/thread, src/allocator and src/blake3 sit side by side in one src/
#     here. Their internal #include "..."s then resolve next to the file
#     that includes them, so the only include path the build adds is
#     include/ (for the public <libfyaml.h>), and a system xxhash.h or
#     blake3.h can never stand in for libfyaml's own. No basename repeats
#     across those directories (checked below).
#   - only the files the library itself compiles are kept: no tests, test
#     corpus, tools (fy-tool), examples, docs, build system, the generic and
#     reflection subsystems (HAVE_GENERIC / HAVE_REFLECTION, both off), or
#     BLAKE3's x86 assembly and SSE/AVX/NEON variants (portable C only).
# There is no config.h: libfyaml normally generates one with CMake or
# autotools and ships no ready-made one. The defines a configure step would
# make live in aether_libfyaml_vendored.c; every vendored file guards its
# `#include "config.h"` with HAVE_CONFIG_H, which is left undefined.
#
# std/yaml/aether_libfyaml_names.h (generated here, not vendored) renames
# every external symbol the vendored library defines to aether_<name>, so
# libaether.a never exports a libfyaml, xxHash or BLAKE3 symbol under its
# own name: a program that also links a system libfyaml (or libxxhash, or
# BLAKE3) gets that library's symbols, not ours. The list is what the unity
# build exports on Linux (glibc and musl), Windows, the three BSDs and macOS,
# cross-compiled with zig; a symbol only some of them define is renamed only
# there. The check script (scripts/check_yaml_engine.sh) fails a build where
# anything leaks.
set -eu

VERSION=1.0.0-beta2
SHA256=f891584e396ee17b520fc2f486f4848edd4315d2cc6d38aad4c5569aec113ec5
URL="https://github.com/pantoniou/libfyaml/releases/download/v${VERSION}/libfyaml-${VERSION}.tar.gz"
DEST=std/yaml/libfyaml
NAMES=std/yaml/aether_libfyaml_names.h

[ -f Makefile ] && [ -d std/yaml ] || { echo "run from the repo root" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

if command -v sha256sum >/dev/null 2>&1; then
    sha256() { sha256sum "$1" | cut -d' ' -f1; }
else
    sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
fi

echo "fetching libfyaml-${VERSION}..."
curl -fsSL -o "$TMP/libfyaml.tar.gz" "$URL"
[ "$(sha256 "$TMP/libfyaml.tar.gz")" = "$SHA256" ] \
    || { echo "SHA-256 mismatch — refusing to vendor" >&2; exit 1; }
tar -xzf "$TMP/libfyaml.tar.gz" -C "$TMP"
UP="$TMP/libfyaml-${VERSION}"

rm -rf "$DEST"
mkdir -p "$DEST/include/libfyaml" "$DEST/src"

# The public headers libfyaml.h pulls in (all but libfyaml-generic.h and
# libfyaml-reflection.h, which belong to the subsystems left out).
cp "$UP/include/libfyaml.h" "$DEST/include/"
for h in align allocator atomics blake3 composer core cpp docbuild dociter \
    endian path-exec thread util vlsize; do
    cp "$UP/include/libfyaml/libfyaml-$h.h" "$DEST/include/libfyaml/"
done

# The library's own sources: CMakeLists.txt's LIBSRCS without the generic
# and reflection subsystems, plus BLAKE3's portable backend (b3portable).
# Keep this list and the #includes in aether_libfyaml_vendored.c together.
LIB_C="fy-parse fy-cache fy-types fy-diag fy-dump fy-atom fy-token fy-input
    fy-docstate fy-doc fy-docbuilder fy-emit fy-event fy-accel fy-walk fy-path
    fy-composer fy-parse-diag fy-input-diag fy-doc-diag fy-docbuilder-diag
    fy-composer-diag"
UTIL_C="fy-ctype fy-utf8 fy-utils fy-blob"
ALLOC_C="fy-allocator fy-allocator-linear fy-allocator-malloc
    fy-allocator-mremap fy-allocator-dedup fy-allocator-auto
    fy-allocator-durable"
BLAKE3_C="blake3_host_state blake3_backend blake3_be_cpusimd fy-blake3
    blake3_portable blake3"
for f in $LIB_C;    do cp "$UP/src/lib/$f.c"       "$DEST/src/"; done
for f in $UTIL_C;   do cp "$UP/src/util/$f.c"      "$DEST/src/"; done
cp "$UP/src/xxhash/xxhash.c"  "$DEST/src/"
cp "$UP/src/thread/fy-thread.c" "$DEST/src/"
for f in $ALLOC_C;  do cp "$UP/src/allocator/$f.c" "$DEST/src/"; done
for f in $BLAKE3_C; do cp "$UP/src/blake3/$f.c"    "$DEST/src/"; done

# Their headers: every header in those directories except util/fy-vlsize.h,
# which nothing kept includes (`cc -MM` over the unity build on macOS,
# Linux glibc and musl, Windows, FreeBSD, NetBSD and OpenBSD).
for d in lib util xxhash thread allocator blake3; do
    for h in "$UP/src/$d"/*.h; do
        b=$(basename "$h")
        [ "$b" = fy-vlsize.h ] && continue
        [ -e "$DEST/src/$b" ] && { echo "basename clash: $b" >&2; exit 1; }
        cp "$h" "$DEST/src/"
    done
done

# Upstream licence, kept beside the code it covers (also reproduced in
# THIRD_PARTY_LICENSES.md, with xxHash's, whose text is in xxhash.c).
cp "$UP/LICENSE" "$DEST/LICENSE"

cat > "$DEST/VENDOR.md" <<EOF
# Vendored libfyaml ${VERSION}

Byte-identical subset of upstream libfyaml ${VERSION}
(<https://github.com/pantoniou/libfyaml>), MIT — full licence in \`LICENSE\`
here and in \`THIRD_PARTY_LICENSES.md\`. It bundles xxHash (BSD-2-Clause, the
licence is in \`src/xxhash.c\`) and BLAKE3's portable C implementation.
Vendored so \`std.yaml\` works with no system libfyaml and no program links
\`-lfyaml\`: native builds on any box, MSYS2's static Windows link, and
\`ae build --target ...\` cross builds with no sysroot.

- Upstream release asset: libfyaml-${VERSION}.tar.gz
- SHA-256: \`${SHA256}\`
- Regenerate with: \`scripts/vendor-libfyaml.sh\` (edit VERSION/SHA256 there to bump)

Layout: \`include/\` is upstream's \`include/\`. \`src/\` holds, side by side,
the files upstream keeps in \`src/lib\`, \`src/util\`, \`src/xxhash\`,
\`src/thread\`, \`src/allocator\` and \`src/blake3\` (no basename repeats), so
their quoted includes resolve next to each other with no include path.
Left out: tests, tools, examples, docs, the build system, the generic and
reflection subsystems, and BLAKE3's assembly and SIMD variants.

**No local patches.** There is no \`config.h\`: the configure-step defines
(portable BLAKE3 only, no SIMD or assembly, qsort_r and mremap where the
platform has them) live in \`std/yaml/aether_libfyaml_vendored.c\`, and
\`std/yaml/aether_libfyaml_names.h\` renames every exported symbol to
\`aether_<name>\`. Nothing in this directory is edited. If a change is ever
needed, it goes upstream — not here.

This directory is compiled ONLY as textual includes of
\`aether_libfyaml_vendored.c\` (a single translation unit, guarded by
\`AETHER_VENDOR_YAML\`). Its .c files must never be added to MANIFEST,
STD_SRC, or any build's source list.
EOF

# --- aether_libfyaml_names.h ------------------------------------------------
# Compile the unity build without the renames for every platform family and
# list what it exports. Needs zig (scripts/get-zig.sh) for the cross
# compiles, and an nm that reads ELF, COFF and Mach-O (llvm-nm; macOS's nm
# is llvm-nm).
command -v zig >/dev/null 2>&1 \
    || { echo "zig is needed to list the exports per platform (scripts/get-zig.sh)" >&2; exit 1; }
if [ -z "${NM:-}" ]; then
    if command -v llvm-nm >/dev/null 2>&1; then NM=llvm-nm; else NM=nm; fi
fi
[ -f "$NAMES" ] || : > "$NAMES"   # first vendoring: the wrapper includes it
# zig target and the macro that identifies its platform family.
TARGETS="x86_64-linux-gnu:__linux__ aarch64-linux-musl:__linux__
    x86_64-windows-gnu:_WIN32 x86_64-freebsd:__FreeBSD__
    x86_64-netbsd:__NetBSD__ x86_64-openbsd:__OpenBSD__
    aarch64-macos:__APPLE__ x86_64-macos:__APPLE__"
: > "$TMP/seen"      # lines "<symbol> <macro>"
for tm in $TARGETS; do
    t=${tm%%:*}; m=${tm#*:}
    zig cc -target "$t" -c -O1 -w -DAETHER_VENDOR_YAML -DAETHER_LIBFYAML_NO_RENAME \
        -I"$DEST/include" -idirafter std/yaml/libfyaml_compat \
        std/yaml/aether_libfyaml_vendored.c -o "$TMP/$t.o"
    # Mach-O prefixes C symbols with an underscore; ELF and COFF (x86_64) do not.
    macho=0
    case $t in *macos*) macho=1 ;; esac
    "$NM" -g "$TMP/$t.o" | awk -v m="$m" -v macho="$macho" '
        NF >= 3 && $2 ~ /^[TDBSRCV]$/ {
            s = $3; if (macho) sub(/^_/, "", s)
            if (s ~ /^[A-Za-z_][A-Za-z0-9_]*$/) print s, m
        }' >> "$TMP/seen"
done
[ -s "$TMP/seen" ] || { echo "no exported symbols found" >&2; exit 1; }
nfam=$(awk '{print $2}' "$TMP/seen" | sort -u | wc -l | tr -d ' ')

# A symbol every platform family defines is renamed unconditionally. One
# defined only on some (libfyaml's open_memstream fallback on macOS, its
# Windows helpers) is renamed only there: open_memstream elsewhere is libc's,
# and renaming the call would leave it unresolved.
{
    echo "/* Generated by scripts/vendor-libfyaml.sh for libfyaml ${VERSION}. Do not edit."
    echo " *"
    echo " * Renames every external symbol the vendored libfyaml (with its xxHash"
    echo " * and BLAKE3) defines to aether_<name>, so libaether.a exports none of"
    echo " * them under the name a system libfyaml, libxxhash or BLAKE3 uses."
    echo " * Included first by aether_libfyaml_vendored.c and, ahead of"
    echo " * <libfyaml.h>, by aether_yaml.c; both see the same names. A symbol"
    echo " * only some platforms define is renamed only on those. */"
    echo "#ifndef AETHER_LIBFYAML_NAMES_H"
    echo "#define AETHER_LIBFYAML_NAMES_H"
    echo "#ifndef AETHER_LIBFYAML_NO_RENAME"
    sort -u "$TMP/seen" | awk -v nfam="$nfam" '
        { fams[$1] = fams[$1] " " $2; n[$1]++ ; if (!($1 in order)) { order[$1] = ++k; name[k] = $1 } }
        END {
            for (i = 1; i <= k; i++) {
                s = name[i]
                if (n[s] == nfam) { print "#define " s " aether_" s; continue }
                split(substr(fams[s], 2), f, " "); cond = ""
                for (j in f) cond = cond (cond == "" ? "" : " || ") "defined(" f[j] ")"
                print "#if " cond; print "#define " s " aether_" s; print "#endif"
            }
        }'
    echo "#endif /* AETHER_LIBFYAML_NO_RENAME */"
    echo "#endif /* AETHER_LIBFYAML_NAMES_H */"
} > "$NAMES"

echo "vendored libfyaml-${VERSION} into $DEST: $(find "$DEST" -type f | wc -l | tr -d ' ') files, $(du -sh "$DEST" | cut -f1)"
echo "$NAMES: $(grep -c '^#define [A-Za-z0-9_]* aether_' "$NAMES") renamed symbols"
echo "done — review with: git status $DEST $NAMES"
