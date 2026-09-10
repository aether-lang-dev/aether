#!/bin/sh
# CRITICAL: this is the single implementation of the CHANGELOG release-fold
# gate. The CI job "CHANGELOG entry present" calls it and so does
# `make check-changelog`. Do not reimplement either half separately: a local
# check that drifts from the gate is worse than no local check, because it
# reports green on the exact corruption the gate exists to stop.
set -u

FILE=${1:-CHANGELOG.md}
BASE=${CHANGELOG_FOLD_BASE:-origin/main}

if [ ! -f "$FILE" ]; then
    echo "check_changelog_fold: no such file: $FILE" >&2
    exit 2
fi

if [ -n "${GITHUB_ACTIONS:-}" ]; then
    err() { echo "::error::$*"; }
else
    err() { echo "ERROR: $*" >&2; }
fi

rc=0

if ! grep -q '^## \[current\]$' "$FILE"; then
    err "$FILE has no '## [current]' section."
    echo ""
    echo "The release job renames the first '## [current]' heading into"
    echo "the new version, so an entry outside one is not released."
    echo "If main cut a release while this branch was open, the merge"
    echo "folded your entry under that version's heading: move it back"
    echo "under a fresh '## [current]' and leave the tagged section as"
    echo "it shipped (compare with \`git show vX.Y.Z:$FILE\`)."
    exit 1
fi

n_current=$(grep -cE '^## \[current\]$' "$FILE" || true)
if [ "$n_current" != "1" ]; then
    err "$FILE has $n_current '## [current]' headings; expected exactly 1."
    echo ""
    echo "The release job renames the FIRST '## [current]' into the new"
    echo "version. A second one further down then gets renamed by the"
    echo "NEXT release, which buries entries inside a section that has"
    echo "already shipped."
    echo ""
    echo "Since the release now leaves a '[current]' behind, a PR should"
    echo "add its entry UNDER the existing heading rather than adding a"
    echo "new one. Merge the sections into one."
    exit 1
fi

# Compared against the base branch rather than the git tags. Editing a
# released section AFTER its tag is legitimate and routine here, so
# "differs from the tag" would red a third of all PRs and be switched off.
# The real invariant is narrower and is exactly the fold: THIS branch must
# not change a section that the base already released.
#
# Whitespace is normalised so a merge that only reflowed spacing does not
# read as a fold. cksum rather than cmp: MSYS2 ships no diffutils, and
# `cmp -s` exits non-zero both when files differ AND when cmp is missing,
# reporting a false failure instead of a missing tool.
git fetch --quiet origin main 2>/dev/null || true
if ! git rev-parse --verify --quiet "$BASE" >/dev/null; then
    echo "check_changelog_fold: $BASE is not available, skipping the fold check."
    exit $rc
fi

norm() { sed -e 's/[[:space:]]*$//' | cat -s; }
# CRITICAL: awk, not sed. BSD sed (macOS) rejects `q` inside a brace group,
# so the sed form of this extraction returns nothing on macOS and both sides
# compare equal -- a silent false green on exactly the corruption this
# checks for. awk behaves identically on GNU, BSD and MSYS2.
section() {
    awk -v v="$1" '
        BEGIN { gsub(/\./, "\\.", v); pat = "^## \\[" v "\\]" }
        $0 ~ pat { inside = 1; next }
        inside && /^## \[/ { exit }
        inside { print }
    '
}

folded=""
for v in $(grep -oE '^## \[[0-9]+\.[0-9]+\.[0-9]+\]' "$FILE" | tr -d '#[] ' | head -8); do
    a=$(git show "$BASE:$FILE" 2>/dev/null | section "$v" | norm)
    [ -z "$a" ] && continue
    b=$(section "$v" < "$FILE" | norm)
    if [ "$(printf '%s' "$a" | cksum)" != "$(printf '%s' "$b" | cksum)" ]; then
        folded="$folded $v"
    fi
done

if [ -n "$folded" ]; then
    err "This branch rewrites already-released CHANGELOG section(s):$folded"
    echo ""
    echo "A release was almost certainly cut while this branch was open."
    echo "The release job renames '## [current]' into the version"
    echo "heading, so merging main folds this branch's entry INTO that"
    echo "released section, usually with no git conflict, which is why"
    echo "nothing flagged it."
    echo ""
    echo "Move the entry back under a fresh '## [current]' and restore"
    echo "the released section as $BASE has it:"
    for v in $folded; do
        echo "  git show $BASE:$FILE   # section [$v]"
    done
    exit 1
fi

echo "Released sections match $BASE, and there is exactly one '## [current]'."
exit $rc
