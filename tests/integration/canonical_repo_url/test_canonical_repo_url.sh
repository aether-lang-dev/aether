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

# Assembled from parts so this file does not itself contain the literal it
# searches for, which would make the check fail on its own source.
STALE_OWNER='nicolasmd87'
STALE="$STALE_OWNER/aether"

hits=$(grep -rn "$STALE" \
        --exclude-dir=.git \
        --exclude-dir=build \
        --exclude-dir=target \
        --exclude=CHANGELOG.md \
        --exclude=CHANGELOG-archive.md \
        . 2>/dev/null | grep -v '^\./benchmarks/json/corpus/')

if [ -n "$hits" ]; then
    echo "  [FAIL] canonical_repo_url: stale repository path still referenced"
    echo "$hits" | sed 's/^/        /' | head -12
    echo "        Use the current path; a rename redirect is not a durable target"
    echo "        for anything that downloads and installs binaries."
    exit 1
fi

# And the constant the self-updater actually uses must be the canonical one.
if ! grep -q '#define AE_GITHUB_REPO "aether-lang-dev/aether"' tools/ae_version.c; then
    echo "  [FAIL] canonical_repo_url: AE_GITHUB_REPO is not the canonical path"
    grep -n 'define AE_GITHUB_REPO' tools/ae_version.c | sed 's/^/        /'
    exit 1
fi

echo "  [PASS] canonical_repo_url: release and docs URLs use the current repository path"
