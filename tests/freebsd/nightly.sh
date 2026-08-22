#!/bin/sh
# tests/freebsd/nightly.sh — self-run nightly FreeBSD sandbox validation.
#
# There is no FreeBSD runner in the GitHub CI matrix, so the Capsicum /
# Casper / audit enforcement paths can't be gated automatically. This
# script is the stand-in: run it on a cron on the GhostBSD build box
# (paul@192.168.0.57), it builds the target branch from a clean tree and
# runs the FreeBSD enforcement tests + the portable contract test. On
# failure it opens (or updates) a GitHub issue so the result feeds back.
#
# Cron example (GhostBSD), 03:17 nightly:
#   17 3 * * *  cd $HOME/aether && BRANCH=feat/freebsd-sandbox-parity-v2 \
#               sh tests/freebsd/nightly.sh >> $HOME/aether-nightly.log 2>&1
#
# Requirements on the host: git, gmake, a C toolchain, and `gh` (GitHub
# CLI) authenticated for issue filing. If `gh` is absent the script still
# runs the tests and just prints the report (no issue filed).
#
# Env:
#   BRANCH  branch to test          (default: feat/freebsd-sandbox-parity-v2)
#   REPO    owner/name for issues   (default: aether-lang-dev/aether)
#   NO_ISSUE=1  run tests, never file an issue (dry run)
set -u

BRANCH="${BRANCH:-feat/freebsd-sandbox-parity-v2}"
REPO="${REPO:-aether-lang-dev/aether}"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 3

MARKER="[nightly-freebsd-sandbox]"   # dedup tag in the issue title
TS=$(date '+%Y-%m-%d %H:%M %Z')
LOG=$(mktemp /tmp/fbsd-nightly.XXXXXX)
trap 'rm -f "$LOG"' EXIT

note() { echo "== $* =="; }

fail_report() {
    # $1 = stage label
    stage="$1"
    echo "FreeBSD sandbox nightly FAILED at: $stage ($TS)"
    if [ "${NO_ISSUE:-0}" = "1" ]; then
        echo "(NO_ISSUE=1 — not filing a GitHub issue)"
        return
    fi
    if ! command -v gh >/dev/null 2>&1; then
        echo "(gh CLI not found — not filing a GitHub issue; see log above)"
        return
    fi
    title="$MARKER $BRANCH failed: $stage"
    body=$(printf 'Nightly FreeBSD sandbox validation failed.\n\n- host: %s\n- branch: `%s`\n- commit: `%s`\n- stage: **%s**\n- when: %s\n\n```\n%s\n```\n\n_Filed automatically by tests/freebsd/nightly.sh (no FreeBSD CI runner yet)._' \
        "$(uname -srm)" "$BRANCH" "$(git rev-parse --short HEAD 2>/dev/null)" "$stage" "$TS" "$(tail -120 "$LOG")")
    # Dedup: if an open issue with the same title exists, comment instead.
    existing=$(gh issue list --repo "$REPO" --state open --search "$title in:title" --json number --jq '.[0].number' 2>/dev/null)
    if [ -n "$existing" ] && [ "$existing" != "null" ]; then
        gh issue comment "$existing" --repo "$REPO" --body "$body" >/dev/null 2>&1 \
            && echo "Updated existing issue #$existing" || echo "(failed to comment on #$existing)"
    else
        gh issue create --repo "$REPO" --title "$title" \
            --label "freebsd,sandbox,nightly" --body "$body" 2>/dev/null \
            || gh issue create --repo "$REPO" --title "$title" --body "$body" 2>/dev/null \
            || echo "(failed to create issue)"
    fi
}

{
    note "FreeBSD sandbox nightly  branch=$BRANCH  $TS"
    uname -srm

    note "sync $BRANCH"
    git fetch origin "$BRANCH" || { fail_report "git fetch"; exit 1; }
    git checkout "$BRANCH" || { fail_report "git checkout"; exit 1; }
    git reset --hard "origin/$BRANCH" || { fail_report "git reset"; exit 1; }

    note "build"
    gmake clean >/dev/null 2>&1
    # Work around the FreeBSD obj-dir order-only-prereq quirk (tracked
    # separately): pre-create object dirs so a clean build doesn't trip on
    # a missing dependency-file directory.
    find compiler runtime std lsp tests -type d 2>/dev/null \
        | sed 's#^#build/obj/#' | xargs mkdir -p 2>/dev/null
    gmake -j1 compiler ae stdlib || { fail_report "build"; exit 1; }

    note "portable contract (co-located spec)"
    # Moved from tests/regression/ to sit beside the module (#1584). On
    # FreeBSD it asserts the enforcement path is live; elsewhere it
    # asserts clean degradation, so it is worth running on both.
    ./build/ae run std/capsicum/test_capsicum_portability.ae \
        || { fail_report "portability test"; exit 1; }

    note "FreeBSD enforcement tests"
    sh tests/freebsd/run.sh || { fail_report "freebsd enforcement tests"; exit 1; }

    # The seccomp clone-fence test (#668) is Linux-only (it compiles a probe
    # against <linux/sched.h>); on FreeBSD the kernel fence is Capsicum, not
    # seccomp, so skip it here. Run it only when this harness runs on Linux.
    if [ "$(uname -s)" = "Linux" ]; then
        note "seccomp clone fence (issue #668)"
        sh tests/integration/sandbox_clone_fence/test_sandbox_clone_fence.sh \
            || { fail_report "sandbox_clone_fence"; exit 1; }
    fi

    note "ALL GREEN  $TS"
} 2>&1 | tee "$LOG"
exit 0
