#!/bin/sh
# Clone the hosted-language-headers repo at a pinned commit. The
# per-language WITH_<LANG> scripts then just copy from
# /opt/hosted-language-headers/<lang>/ into /opt/aether/include/<lang>/.
# This replaces the apt-install-dev / capture / apt-purge dance with
# a single network fetch + flat copies.
#
# Why pinned: reproducible builds. If hosted-language-headers gets
# new headers for a different language version (Python 3.12, say),
# image builds keep using the version we tested against until the
# pin is bumped explicitly.
#
# Why git rather than a tarball: cheap branching for non-x86_64 /
# non-glibc targets later. Today main = linux-x86_64-glibc; a
# linux-arm64-glibc branch would land later without a release-
# artefact dance.

set -eu

HEADERS_REPO="${HEADERS_REPO:-https://github.com/aether-lang-dev/hosted-language-headers.git}"
# Bumped to 80b44be — the commit that adds the per-target configured-header
# overlays (targets/<triple>/<lang>/). install-python-headers.sh now overlays
# targets/x86_64-linux-gnu/python/pyconfig.h from this tree, so the pin MUST
# include it (the prior 6872b5df predates the targets/ restructure).
HEADERS_REF="${HEADERS_REF:-80b44be5163a7c8279f23fd714dfe4d11993cb6f}"
HEADERS_DIR="${HEADERS_DIR:-/opt/hosted-language-headers}"

# Skip if already present (e.g. cached layer from a previous build).
if [ -d "$HEADERS_DIR/.git" ]; then
    cd "$HEADERS_DIR"
    if [ "$(git rev-parse HEAD)" = "$HEADERS_REF" ]; then
        echo "clone-host-headers: $HEADERS_DIR already at $HEADERS_REF, skipping"
        exit 0
    fi
fi

# Full clone, then check out the pinned commit. A `--depth 1` clone
# would be smaller, BUT then `git fetch --depth 1 origin <SHA>`
# requires the server to allow fetching arbitrary commit SHAs that
# aren't ref tips (uploadpack.allowAnySHA1InWant). GitHub usually
# supports it, but a real Bazzite report (2026-05-30) showed it
# failing with `fatal: reference is not a tree: <sha>`. A full clone
# of hosted-language-headers is ~14 MB and the .git dir is dropped
# below — small price for deterministic image builds.
mkdir -p "$(dirname "$HEADERS_DIR")"
rm -rf "$HEADERS_DIR"
git clone "$HEADERS_REPO" "$HEADERS_DIR"
cd "$HEADERS_DIR"
git checkout "$HEADERS_REF"

# Drop .git to save layer size — we won't be running git operations
# against the repo again from inside the image.
rm -rf "$HEADERS_DIR/.git"

echo "clone-host-headers: pinned at $HEADERS_REF in $HEADERS_DIR ($(du -sh "$HEADERS_DIR" | cut -f1))"
