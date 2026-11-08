#!/bin/sh
# Fails when a libaether.a built with the vendored libfyaml (YAML=vendored,
# the default, and every cross build) carries no YAML engine, or exports one
# of the vendored library's symbols under its own name.
#
# 1. The engine is there. A cross build cannot use the host's libfyaml, so
#    std.yaml there has to be the vendored copy (std/yaml/libfyaml/, compiled
#    by aether_libfyaml_vendored.c under AETHER_VENDOR_YAML). The cross
#    builds set YAML := 0 before it existed and shipped std.yaml as its stub
#    on FreeBSD, Windows and Android, and nothing noticed: those artifacts
#    are built, not tested. The engine is in when the archive exports
#    aether_fy_document_build_from_string (std.yaml's parse entry point,
#    renamed by std/yaml/aether_libfyaml_names.h).
#
# 2. Nothing leaks. Every external symbol of aether_libfyaml_vendored.o must
#    carry the aether_ prefix the names header gives it, so a program that
#    also links a system libfyaml, libxxhash or BLAKE3 gets that library's
#    fy_*/XXH*/blake3_* and not ours. A symbol the header misses (one a new
#    upstream version or a platform adds) fails here; rerun
#    scripts/vendor-libfyaml.sh to regenerate the header.
#
# Only for archives built with the vendored engine: YAML=1 links a system
# libfyaml and YAML=0 builds the stubs, and neither has these symbols.

set -e

ARCHIVE="$1"
if [ -z "$ARCHIVE" ] || [ ! -f "$ARCHIVE" ]; then
    echo "  [FAIL] yaml engine check: no archive '$ARCHIVE'"
    exit 1
fi
NM="${NM:-nm}"
syms="$("$NM" "$ARCHIVE" 2>/dev/null)" || true

# Mach-O prefixes an underscore and ELF/COFF do not; accept either.
if ! printf '%s\n' "$syms" | grep -Eq ' T _?aether_fy_document_build_from_string$'; then
    echo "  [FAIL] $ARCHIVE has no yaml engine: std.yaml there is the stub"
    echo "         (a cross build needs YAML := vendored, as FREEBSD=1, WINDOWS=1"
    echo "          and ANDROID=1 set it in the Makefile)"
    exit 1
fi

# The defined external symbols of the vendored member. nm heads each member
# "archive(member):" (macOS) or "member:" (GNU, llvm, BSD); a symbol line is
# "[value] TYPE name". Compiler-made names (.refptr.* on Windows) are not ours.
leaks="$(printf '%s\n' "$syms" | awk '
    /:$/ { inmember = ($0 ~ /aether_libfyaml_vendored\.o\)?:$/); next }
    inmember && NF >= 2 {
        type = $(NF - 1); name = $NF
        if (type !~ /^[TDBSRCV]$/) next
        sub(/^_/, "", name)
        if (name ~ /^aether_/ || name ~ /^\./) next
        print name
    }')"
if [ -n "$leaks" ]; then
    n=$(printf '%s\n' "$leaks" | wc -l | tr -d ' ')
    echo "  [FAIL] $ARCHIVE exports $n vendored libfyaml symbol(s) under their own name:"
    printf '%s\n' "$leaks" | head -10 | sed 's/^/         /'
    echo "         (std/yaml/aether_libfyaml_names.h is stale: rerun scripts/vendor-libfyaml.sh)"
    exit 1
fi
echo "  [PASS] $ARCHIVE has the vendored yaml engine, every symbol prefixed"
