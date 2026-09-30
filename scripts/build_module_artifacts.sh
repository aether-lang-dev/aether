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
list="$(cd "$share_root" && find std -name '*.ae' ! -name 'test_*' ! -name 'example*.ae' | LC_ALL=C sort)"
for rel in $list; do
    case "$rel" in
        */module.ae) stem="${rel%/module.ae}" ;;
        *)           stem="${rel%.ae}" ;;
    esac
    mkdir -p "$out_root/$(dirname "$stem")"
    (cd "$share_root" && "$aetherc" --emit=aea "$rel" "$out_root/$stem.aea")
    count=$((count + 1))
done
echo "Built $count module artifacts in $out_root"
