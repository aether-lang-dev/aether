#!/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT HUP INT TERM
# Match the runtime build: its CPUID wrapper is unused on ARM.
"${CC:-cc}" -O2 -Wall -Wextra -Werror -Wno-unused-function \
    "$ROOT/tests/integration/cpu_core_count/core_count.c" -o "$work/core_count"
"$work/core_count"
