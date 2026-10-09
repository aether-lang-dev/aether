#!/bin/sh
# Build the compiled module artifacts (.aea, issue #1746) for every
# Aether-authored std module.
#
#   build_module_artifacts.sh <aetherc> <share-root> <out-root>
#
# <share-root> is the directory that installs as $PREFIX/share/aether (the
# repository root in a build, or share/aether itself in an install); the
# artifacts land under <out-root> (normally $PREFIX/lib/aether/modules)
# mirroring the module hierarchy:
#
#   std/cryptography/md2/module.ae  ->  <out-root>/std/cryptography/md2.aea
#   std/jsonpath/parser.ae          ->  <out-root>/std/jsonpath/parser.aea
#
# Specs (test_*.ae) and examples (example*.ae) are not importable modules and
# get no artifact. One `make install`, one test and the release packaging all
# run this script, so the set of artifacts cannot drift between them. Fails on
# the first module that cannot be encoded: a module that ships must be
# artifact-clean.
set -eu

if [ $# -ne 3 ]; then
    echo "usage: $0 <aetherc> <share-root> <out-root>" >&2
    exit 2
fi

case "$1" in
    /*|[A-Za-z]:*) aetherc="$1" ;;
    *) aetherc="$(pwd)/$1" ;;
esac
share_root="$2"
mkdir -p "$3"
out_root="$(cd "$3" && pwd)"

count=0
cd "$share_root"
list="$(find std -name '*.ae' ! -name 'test_*' ! -name 'example*.ae' | LC_ALL=C sort)"

stem_of() {
    case "$1" in
        */module.ae) stem="${1%/module.ae}" ;;
        *)           stem="${1%.ae}" ;;
    esac
}

# Every directory an artifact lands in, made by one mkdir (the paths are
# relative to <out-root>, so its own spaces cannot split them).
dirs=""
for rel in $list; do
    stem_of "$rel"
    dirs="$dirs ${stem%/*}"
done
# shellcheck disable=SC2086
(cd "$out_root" && mkdir -p $dirs)

# Many modules to an aetherc (#2596). The parse that makes an artifact takes
# milliseconds, and a process start a fifth of a second on Windows: the 157
# std artifacts took 37 s one aetherc each, and take 1.3 s this way, byte
# for byte the same. aetherc writes the pairs in order and stops at the
# first module it cannot encode, as the loop did. A batch is flushed past
# 16 KB of arguments, half of what a Windows command line holds.
set --
size=0
for rel in $list; do
    stem_of "$rel"
    set -- "$@" "$rel" "$out_root/$stem.aea"
    size=$((size + ${#rel} + ${#out_root} + ${#stem} + 8))
    count=$((count + 1))
    if [ "$size" -gt 16384 ]; then
        "$aetherc" --emit=aea "$@"
        set --
        size=0
    fi
done
if [ $# -gt 0 ]; then
    "$aetherc" --emit=aea "$@"
fi
# An aetherc from before the batch form writes its first pair, ignores the
# rest and exits 0: that is a failure here, not a tree of missing artifacts.
for rel in $list; do
    stem_of "$rel"
    if [ ! -f "$out_root/$stem.aea" ]; then
        echo "$0: $aetherc wrote no artifact for $rel" >&2
        exit 1
    fi
done
echo "Built $count module artifacts in $out_root"
