#!/bin/sh
# The built toolchain must report the VERSION in the tree it was compiled from.
#
# Two bugs made it report something else, both measured on real releases:
#
#  1. The Makefile preferred the highest `git tag -l` over the VERSION file. Tag
#     visibility depends on CLONE DEPTH, and release.yml's build jobs check out
#     shallow, so the visible set could be stale and `tail -1` land on an OLDER
#     tag — overriding a correct VERSION. v0.516.0 shipped an `ae --version`
#     saying 0.417.0 while `git show v0.516.0:VERSION` said 0.516.0 all along.
#
#  2. The version is injected as -DAETHER_VERSION on the command line, not via
#     an #include, so -MMD could not see it: bumping VERSION left every object
#     looking up to date and the binary reporting the PREVIOUS version.
#
# This asserts the property both bugs broke: `ae --version` == the VERSION file.
# It deliberately does NOT re-run make (too slow for the .ae suite) — it checks
# the already-built binary, which is what CI and a developer actually ship.
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] version_stamp: $AE not built"
    exit 0
fi
if [ ! -f "$ROOT/VERSION" ]; then
    echo "  [SKIP] version_stamp: no VERSION file (tarball layout?)"
    exit 0
fi

want="$(tr -d '[:space:]' < "$ROOT/VERSION")"
got="$("$AE" --version 2>&1 | head -1 | sed -n 's/^ae \([0-9][0-9.]*\).*/\1/p')"

if [ -z "$got" ]; then
    echo "  [FAIL] version_stamp: could not parse a version from \`ae --version\`:"
    "$AE" --version 2>&1 | head -2 | sed 's/^/      /'
    exit 1
fi

if [ "$want" != "$got" ]; then
    echo "  [FAIL] version_stamp: ae reports $got, VERSION file says $want"
    echo "      The binary is mislabelled. Either the Makefile picked a git tag"
    echo "      over the tree (Makefile:66), or a stale object was not rebuilt"
    echo "      after a VERSION change (see the \$(VERSION_HEADER) deps)."
    exit 1
fi

echo "  [PASS] version_stamp: ae reports $got, matching the VERSION file"
exit 0
