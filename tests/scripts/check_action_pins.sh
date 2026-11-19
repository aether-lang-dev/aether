#!/bin/sh
# Every GitHub Action this repository runs is pinned to a full commit SHA
# (#3746). A tag such as `@v2` is a pointer its owner, or whoever takes over
# their account, can move: when tj-actions/changed-files' tags were moved in
# 2025, every workflow that named a tag ran the new code, secrets in reach.
# A SHA names one commit and cannot move. Dependabot (.github/dependabot.yml)
# proposes newer pins as PRs, so pinning does not freeze the versions.
#
# Usage: check_action_pins.sh [dir]   (default: .github)
# Fails, naming each line, on a `uses:` that is not a local action (./...),
# a docker:// image, or owner/repo[/path]@<40 hex digits>.

dir="${1:-.github}"
bad=$(grep -rnE '^[[:space:]-]*uses:[[:space:]]*' "$dir" 2>/dev/null |
    grep -vE 'uses:[[:space:]]*["'"'"']?\./' |
    grep -vE 'uses:[[:space:]]*["'"'"']?docker://' |
    grep -vE 'uses:[[:space:]]*["'"'"']?[A-Za-z0-9_.-]+/[A-Za-z0-9_./-]+@[0-9a-f]{40}([[:space:]"'"'"']|$)')
if [ -n "$bad" ]; then
    echo "  [FAIL] actions not pinned to a full commit SHA (pin as owner/repo@<sha> # vX.Y.Z):"
    printf '%s\n' "$bad" | sed 's/^/    /'
    exit 1
fi
n=$(grep -rcE '^[[:space:]-]*uses:' "$dir" 2>/dev/null | awk -F: '{s += $NF} END {print s + 0}')
echo "  [PASS] action pins: all $n uses in $dir are local or pinned to a commit SHA"
