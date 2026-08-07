#!/bin/sh
# The toolchain must reference the CURRENT repository path, not an old name
# that only resolves through a GitHub rename redirect.
#
# `ae upgrade` / `ae install` download release binaries from AE_GITHUB_REPO and
# install them. curl and wget both follow redirects silently, so a stale
# owner/name keeps working right up until someone claims the freed path, at
# which point the self-updater fetches its binaries from whatever now lives
# there. It is not a broken link, it is an install path pointed at an address
# the project no longer controls.
#
# The changelogs are exempt: they record links as they were written, and an
# entry explaining this very fix has to be able to name the old path. They are
# prose, not addresses anything fetches. Everything a user or a tool actually
# follows (code, docs, editor manifests, UI links) stays covered.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$ROOT" || exit 1

# Every owner the project has moved away from. GitHub keeps each old path alive
# as a rename redirect, so a stale reference keeps working right up until someone
# claims the freed name — at which point anything that fetches from it lands on a
# repo the project no longer controls. Guard against all of them, not just the
# oldest:
#   - nicolasmd87    : the original owner (pre-org)
#   - aether-lang-org: renamed to aether-lang-dev by Nic
# The owner strings are spliced from parts at runtime so this source file does
# not itself contain a stale "owner/aether" literal and trip its own check.
ORG_SUFFIX='org'
STALE_OWNERS="nicolasmd87 aether-lang-${ORG_SUFFIX}"

hits=""
for owner in $STALE_OWNERS; do
    stale="$owner/aether"
    found=$(grep -rn "$stale" \
            --exclude-dir=.git \
            --exclude-dir=build \
            --exclude-dir=target \
            --exclude=CHANGELOG.md \
            --exclude=CHANGELOG-archive.md \
            . 2>/dev/null | grep -Ev '^\.?/?benchmarks/json/corpus/')
    [ -n "$found" ] && hits="${hits}${found}
"
done

if [ -n "$(printf '%s' "$hits" | tr -d '[:space:]')" ]; then
    echo "  [FAIL] canonical_repo_url: stale repository path still referenced"
    printf '%s' "$hits" | sed '/^$/d; s/^/        /' | head -12
    echo "        Use the current path (aether-lang-dev/aether); a rename redirect"
    echo "        is not a durable target for anything that downloads/installs."
    exit 1
fi

# And the constant the self-updater actually uses must be the canonical one.
if ! grep -q '#define AE_GITHUB_REPO "aether-lang-dev/aether"' tools/ae_version.c; then
    echo "  [FAIL] canonical_repo_url: AE_GITHUB_REPO is not the canonical path"
    grep -n 'define AE_GITHUB_REPO' tools/ae_version.c | sed 's/^/        /'
    exit 1
fi

echo "  [PASS] canonical_repo_url: release and docs URLs use the current repository path"
