#!/bin/sh
# Build examples/04-interop/07-limits: script.ae as an object file, host.c
# linked against it and the Aether runtime, then run the program.
# Run from the repository root. AE and CC name the tools (default: ae, cc).
set -e
AE="${AE:-ae}"
CC="${CC:-cc}"
here="$(dirname "$0")"
out="build/examples/limits"
mkdir -p "$out"
"$AE" build --emit=obj "$here/script.ae" -o "$out/script.o" >/dev/null
# shellcheck disable=SC2046
"$CC" $("$AE" cflags --cflags) "$here/host.c" "$out/script.o" $("$AE" cflags --libs) -o "$out/host"
"$out/host"
