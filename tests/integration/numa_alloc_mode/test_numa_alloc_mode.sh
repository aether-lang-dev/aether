#!/bin/sh
# A block is freed by the allocator that made it, whatever the NUMA state
# at free time (#3680): the module is built with its NUMA path on and two
# mocked nodes, so this runs on any host.
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)"
here="$ROOT/tests/integration/numa_alloc_mode"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT HUP INT TERM
"${CC:-cc}" -O2 -Wall -Wextra -Werror -Wno-unused-function \
    -DAETHER_HAS_NUMA=1 -I "$here/fake" \
    "$here/numa_alloc_mode.c" -o "$work/numa_alloc_mode" -lpthread
"$work/numa_alloc_mode"
