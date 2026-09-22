#!/bin/sh
# #2002: a PR records its change in a file of its own, so a release cannot
# fold it into an already-released section.
#
# Every open PR used to edit the same shared lines -- `## [current]`. A
# release RENAMES those lines, so merging the new main into any open branch
# folded that branch's entry into the released section, with no conflict:
# the headings merge cleanly and only the bullets end up under the wrong
# one. Four PRs in a row across 0.661-0.664, three more since, and twice in
# one day to the batch that wrote this.
#
# What this pins is the collector's behaviour, on a scratch repository
# rather than on the real CHANGELOG: the window is respected, a release
# takes everything, sections land in Keep a Changelog's order and merge
# into an existing one, a bad name is refused, and -- the point of the whole
# exercise -- two fragments never touch the same line.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
COLLECT="$ROOT/tests/scripts/collect_changelogs.py"

PY=""
for cand in python3 python "py -3"; do
    if $cand -c "import sys" >/dev/null 2>&1; then PY="$cand"; break; fi
done
[ -n "$PY" ] || { echo "  [SKIP] changelog_fragments: no python3"; exit 0; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0

# A scratch repository with the shape the real one has.
#
# Everything about the cooling-off window depends on git being able to date
# a commit here, so the setup is checked rather than assumed: a silently
# failed `git init` or `git commit` would make the fragments read as
# "uncommitted", which is also a hold, and the test would be asserting
# nothing while appearing to pass.
# A scratch repository must not inherit one. `git -C` does not override
# these, so with any of them set every git call below -- and every one the
# collector makes -- would answer about the outer checkout instead.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY GIT_COMMON_DIR

mkdir -p "$tmp/new_changelogs"
cd "$tmp" || exit 1
if ! git init -q . 2>"$tmp/git.err"; then
    echo "  [SKIP] changelog_fragments: git init failed here"
    sed 's/^/         /' "$tmp/git.err" | head -3
    exit 0
fi
# Local identity, and no inherited hook/signing configuration: a global
# commit.gpgsign or a template hook would fail the commit below on a
# machine that has one.
git config user.email t@example.com
git config user.name Test
git config commit.gpgsign false
git config core.hooksPath /dev/null

cat > CHANGELOG.md <<'MD'
# Changelog

## [current]

### Fixed

- **An entry that was already here.** Kept, and new Fixed bullets join it.

## [0.708.0]

### Added

- **Something released.** This must not be touched.
MD

# The real directory carries a README, which is both its documentation and
# what keeps it alive: `git rm` of the last fragment would otherwise take
# the directory with it. The collector must ignore it.
printf '# Changelog fragments\n' > new_changelogs/README.md

frag() { mkdir -p new_changelogs; printf -- '- **%s**\n' "$2" > "new_changelogs/$1"; }
frag 20260101T000000Z-added-1-first.md      "An added thing."
frag 20260101T000001Z-fixed-2-second.md     "A fixed thing."
frag 20260101T000002Z-performance-3-third.md "A faster thing."

# 1. Uncommitted fragments belong to someone's working tree, not to us.
out="$($PY "$COLLECT" --root "$tmp" 2>&1)"
if ! printf '%s\n' "$out" | grep -q 'nothing settled yet'; then
    echo "  [FAIL] changelog_fragments: uncommitted fragments were folded"
    printf '%s\n' "$out" | sed 's/^/        /'
    fail=1
fi

git add -A >/dev/null 2>&1
if ! git commit -qm "fragments" >"$tmp/commit.log" 2>&1; then
    echo "  [FAIL] changelog_fragments: the scratch commit did not take, so the window cannot be tested"
    sed 's/^/        /' "$tmp/commit.log" | head -5
    exit 1
fi
# ... and git can date it. This is the exact call the collector makes.
if [ -z "$(git log -1 --format=%cI -- new_changelogs/20260101T000000Z-added-1-first.md)" ]; then
    echo "  [FAIL] changelog_fragments: git reports no commit date for a file it just committed"
    git log --oneline | head -3 | sed 's/^/        /'
    git status --short | head -5 | sed 's/^/        /'
    exit 1
fi

# 2. Committed but inside the cooling-off window: still not folded, so a
#    change reverted within the window never becomes a permanent line.
out="$($PY "$COLLECT" --root "$tmp" 2>&1)"
if ! printf '%s\n' "$out" | grep -q 'window is'; then
    echo "  [FAIL] changelog_fragments: a fresh fragment was not held for the cooling-off window"
    printf '%s\n' "$out" | sed 's/^/        /'
    fail=1
fi
if [ "$(ls new_changelogs/*.md 2>/dev/null | grep -v README | wc -l)" -ne 3 ]; then
    echo "  [FAIL] changelog_fragments: fragments were removed while still waiting"
    fail=1
fi

# 3. A release takes everything pending, whatever its age: a change merged
#    minutes before a release must not be missing from it.
out="$($PY "$COLLECT" --root "$tmp" --all 2>&1)"
if [ "$(ls new_changelogs/*.md 2>/dev/null | grep -v README | wc -l)" -ne 0 ]; then
    echo "  [FAIL] changelog_fragments: --all left fragments behind"
    printf '%s\n' "$out" | sed 's/^/        /'
    fail=1
fi
if [ ! -f new_changelogs/README.md ]; then
    echo "  [FAIL] changelog_fragments: the README was folded or removed with the fragments"
    fail=1
fi
if grep -q 'Changelog fragments' CHANGELOG.md; then
    echo "  [FAIL] changelog_fragments: the README's contents were folded into the changelog"
    fail=1
fi

# 4. They landed under [current], in Keep a Changelog's section order, with
#    the pre-existing Fixed bullet still there and the released section
#    untouched -- which is the entire point.
current="$(awk '/^## \[current\]$/{f=1;next} /^## \[/{f=0} f' CHANGELOG.md)"
for want in 'An added thing.' 'A fixed thing.' 'A faster thing.' 'An entry that was already here.'; do
    if ! printf '%s\n' "$current" | grep -qF "$want"; then
        echo "  [FAIL] changelog_fragments: '$want' is not under [current]"
        fail=1
    fi
done
order="$(printf '%s\n' "$current" | grep '^### ' | tr -d '\r' | paste -sd, -)"
if [ "$order" != "### Added,### Fixed,### Performance" ]; then
    echo "  [FAIL] changelog_fragments: sections are out of order: $order"
    fail=1
fi
released="$(awk '/^## \[0\.708\.0\]$/{f=1;next} /^## \[/{f=0} f' CHANGELOG.md)"
if ! printf '%s\n' "$released" | grep -qF 'Something released.'; then
    echo "  [FAIL] changelog_fragments: the released section was disturbed"
    fail=1
fi
if printf '%s\n' "$released" | grep -qF 'An added thing.'; then
    echo "  [FAIL] changelog_fragments: a new entry was folded into the RELEASED section"
    fail=1
fi

# 5. A name the collector cannot read is refused by name, not folded blindly.
mkdir -p new_changelogs; printf -- '- **Bad.**\n' > new_changelogs/not-a-fragment.md
if $PY "$COLLECT" --root "$tmp" --check >/dev/null 2>&1; then
    echo "  [FAIL] changelog_fragments: a malformed fragment name was accepted"
    fail=1
fi
err="$($PY "$COLLECT" --root "$tmp" --check 2>&1 || true)"
if ! printf '%s\n' "$err" | grep -q 'not-a-fragment.md'; then
    echo "  [FAIL] changelog_fragments: the malformed fragment was not named in the error"
    fail=1
fi
rm -f new_changelogs/not-a-fragment.md

# 6. An unknown section is refused too, and says what the choices are.
mkdir -p new_changelogs; printf -- '- **Bad.**\n' > new_changelogs/20260101T000003Z-improved-4-x.md
err="$($PY "$COLLECT" --root "$tmp" --check 2>&1 || true)"
if ! printf '%s\n' "$err" | grep -q "section 'improved' is not one of"; then
    echo "  [FAIL] changelog_fragments: an unknown section was not reported with the valid ones"
    printf '%s\n' "$err" | sed 's/^/        /'
    fail=1
fi
rm -f new_changelogs/20260101T000003Z-improved-4-x.md

# 6b. A fragment created and never written is refused too. `make
#     add-changelog` seeds a placeholder line; the name is valid and the
#     body is non-empty, so nothing else in the pipeline would notice, and
#     it would be published verbatim into a release section.
mkdir -p new_changelogs
printf -- '- **Say what changed, and why it mattered.**
' > new_changelogs/20260101T000006Z-fixed-7-unwritten.md
err="$($PY "$COLLECT" --root "$tmp" --check 2>&1 || true)"
if ! printf '%s
' "$err" | grep -q 'placeholder text'; then
    echo "  [FAIL] changelog_fragments: an unwritten fragment was accepted"
    printf '%s
' "$err" | sed 's/^/        /'
    fail=1
fi
rm -f new_changelogs/20260101T000006Z-fixed-7-unwritten.md

# 7. The claim that makes this worth doing: two PRs adding entries at the
#    same time touch no common line. Fragments are separate files, so the
#    only way to show it is that adding one changes nothing the other did.
git add -A >/dev/null 2>&1; git commit -qm folded >/dev/null 2>&1
frag 20260101T000004Z-fixed-5-pr-one.md "PR one's entry."
git add -A >/dev/null 2>&1; git commit -qm one >/dev/null 2>&1
frag 20260101T000005Z-fixed-6-pr-two.md "PR two's entry."
touched="$(git diff --name-only HEAD)"
if [ "$touched" != "" ]; then
    echo "  [FAIL] changelog_fragments: adding a fragment modified a tracked file: $touched"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] changelog_fragments: the window holds, a release takes everything, sections order and merge, bad names are refused, and two entries share no line"
fi
exit $fail
