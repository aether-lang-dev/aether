#!/bin/sh
# Fails when libaether.a is missing a symbol the shipped std sources declare.
#
# The 0.467.0 release shipped std/worker/aether_worker.c defining
# aether_worker_wait alongside an archive built from an older tree that did not
# export it (#1395). Nothing failed until a user called the one function added
# last, and then it failed at THEIR link step, not ours.
#
# For every `extern NAME(...)` in a std module.ae, if a std C source defines
# NAME, the archive must export it. Externs with no std definition are libc or
# optional-dependency symbols and are skipped, which is what keeps this free of
# false positives.

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCHIVE="${1:-$ROOT/build/libaether.a}"

if [ ! -f "$ARCHIVE" ]; then
    echo "  [SKIP] archive export check: $ARCHIVE not built"
    exit 0
fi

exported="$(mktemp)"
declared="$(mktemp)"
defined="$(mktemp)"
trap 'rm -f "$exported" "$declared" "$defined"' EXIT

# Exported text symbols, with the Mach-O leading underscore stripped.
nm -g "$ARCHIVE" 2>/dev/null | awk '$2=="T"||$2=="D"||$2=="S"{print $3}' \
    | sed 's/^_//' | sort -u > "$exported"

# Names declared `extern` by a std module.
grep -rhoE '^extern[[:space:]]+[A-Za-z_][A-Za-z0-9_]*' \
    "$ROOT"/std/*/module.ae "$ROOT"/std/*/*/module.ae 2>/dev/null \
    | awk '{print $2}' | sort -u > "$declared"

# Names a std C source actually defines at file scope.
grep -rhoE '^[A-Za-z_][A-Za-z0-9_ *]*[ *]([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*\(' \
    "$ROOT"/std/*/*.c "$ROOT"/std/*/*/*.c 2>/dev/null \
    | sed -E 's/.*[ *]([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*\($/\1/' | sort -u > "$defined"

missing=0
while read -r name; do
    [ -n "$name" ] || continue
    grep -qx "$name" "$defined"  || continue   # not ours: libc / optional dep
    grep -qx "$name" "$exported" && continue   # present, good
    echo "  [FAIL] $name is declared extern by a std module and defined in a std"
    echo "         source, but $ARCHIVE does not export it (stale archive)."
    missing=$((missing + 1))
done < "$declared"

if [ "$missing" -ne 0 ]; then
    echo "  $missing symbol(s) missing. The archive does not match the shipped sources."
    exit 1
fi

echo "  [PASS] archive exports every std-defined extern"
