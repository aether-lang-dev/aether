#!/bin/sh
# The signing floor (#3752), the first signed release, is written three
# times, once for each language that reads it, and the copies must agree:
#
#   get.sh                        SIGNED_SINCE="X.Y.Z"
#   tools/ae_version.c            #define AE_SIGNED_SINCE "X.Y.Z"
#   scripts/verify-release-asset.sh   signed_since=X.Y.Z
#
# A release at or above it without SHA256SUMS is refused by each installer,
# so a copy that differs either refuses a release the others install, or
# installs a stripped release the others refuse. release.yml's publish job
# reads the third copy and fails a release that disagrees with it.
#
# Usage: check_signing_floor.sh [root]   (default: .)

root="${1:-.}"
fail() { echo "  [FAIL] signing floor: $1"; exit 1; }

one() {   # FILE SED-EXPRESSION: the single value it finds, or fails
    [ -f "$root/$1" ] || fail "$1 is missing"
    v="$(sed -n "$2" "$root/$1")"
    [ -n "$v" ] || fail "$1 sets no signing floor"
    [ "$(printf '%s\n' "$v" | wc -l | tr -d ' ')" = 1 ] || fail "$1 sets the signing floor more than once"
    printf '%s\n' "$v" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$' || fail "$1 sets the signing floor to '$v', not X.Y.Z"
    printf '%s' "$v"
}

sh_v="$(one get.sh 's/^SIGNED_SINCE="\(.*\)"$/\1/p')" || { echo "$sh_v"; exit 1; }
c_v="$(one tools/ae_version.c 's/^#define AE_SIGNED_SINCE "\(.*\)"$/\1/p')" || { echo "$c_v"; exit 1; }
s_v="$(one scripts/verify-release-asset.sh 's/^signed_since=\(.*\)$/\1/p')" || { echo "$s_v"; exit 1; }

[ "$sh_v" = "$c_v" ] && [ "$c_v" = "$s_v" ] ||
    fail "the copies differ: get.sh $sh_v, tools/ae_version.c $c_v, scripts/verify-release-asset.sh $s_v"

wf="$root/.github/workflows/release.yml"
if [ -f "$wf" ]; then
    grep -q 'git show "\$WF_SHA:scripts/verify-release-asset.sh"' "$wf" ||
        fail "release.yml does not read signed_since from scripts/verify-release-asset.sh at its own commit"
fi

echo "  [PASS] signing floor: $sh_v in get.sh, tools/ae_version.c and scripts/verify-release-asset.sh"
