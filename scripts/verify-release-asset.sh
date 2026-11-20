#!/bin/sh
# Verify a downloaded release asset before it is used (#3752):
#
#   sh scripts/verify-release-asset.sh REPO TAG FILE
#
# FILE is the asset as downloaded; its base name is the asset's name. With
# the gh CLI (authenticated: GH_TOKEN in a workflow), it checks:
#
#   - the sha256 digest GitHub lists for the asset;
#   - a release made since #3752: the asset's line in the release's
#     SHA256SUMS, the keyless cosign signature of SHA256SUMS
#     (SHA256SUMS.sigstore.json) as made by REPO's release workflow when
#     cosign is installed, and the asset's build provenance with
#     `gh attestation verify`;
#   - a release older than signed_since (below) has neither, and the
#     digest is all there is. One at or above it without SHA256SUMS, or
#     without its bundle when cosign is installed, is refused.
#
# Exits 0 when every check that can be made passed, saying which were made;
# 1 on a mismatch or a check that fails. release.yml runs it on the previous
# release it downloads for the ABI matrix, inside the job that builds the
# next one.

set -eu

# The signing floor: the first release signed. A release at or above it must
# have SHA256SUMS, and its bundle when cosign is installed, so someone who
# can edit a release cannot delete them and have its asset taken on GitHub's
# digest alone. It must be the first release published by a release.yml that
# has the sign job: release.yml's publish job reads it from here and fails a
# release that disagrees with it. Equal to SIGNED_SINCE in get.sh and
# AE_SIGNED_SINCE in tools/ae_version.c (tests/scripts/check_signing_floor.sh,
# in make check-tests).
signed_since=0.927.0

[ $# -eq 3 ] || { echo "usage: $0 REPO TAG FILE" >&2; exit 2; }
repo="$1"; tag="$2"; file="$3"
name="$(basename "$file")"
say() { printf 'verify-release-asset: %s\n' "$*"; }
die() { printf 'verify-release-asset: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

# release_signed TAG : 0 when TAG (vX.Y.Z, anything after Z ignored) is at or
# above signed_since, or is not a version at all; 1 when it is older.
release_signed() {
    awk -v t="$1" -v f="$signed_since" '
        function num(s, i,   p) { split(s, p, "."); return p[i] + 0 }
        BEGIN {
            sub(/^v/, "", t)
            if (t !~ /^[0-9]+\.[0-9]+\.[0-9]+/) exit 0
            for (i = 1; i <= 3; i++) {
                a = num(t, i); b = num(f, i)
                if (a != b) exit (a > b) ? 0 : 1
            }
            exit 0
        }'
}

have gh || die "the gh CLI is needed"
[ -f "$file" ] || die "$file does not exist"

sha256_of() {
    if have sha256sum; then sha256sum "$1" | awk '{print $1}'
    elif have shasum; then shasum -a 256 "$1" | awk '{print $1}'
    else openssl dgst -sha256 "$1" | awk '{print $NF}'
    fi
}
got="$(sha256_of "$file")"
[ "${#got}" = 64 ] || die "no sha256 tool"

listed="$(gh release view "$tag" --repo "$repo" --json assets \
    -q ".assets[] | select(.name == \"$name\") | .digest")" || die "cannot read the $tag release of $repo"
[ -n "$listed" ] || die "the $tag release of $repo lists no $name"
case "$listed" in
    sha256:*) [ "${listed#sha256:}" = "$got" ] ||
        die "$name does not match the sha256 digest GitHub lists (expected ${listed#sha256:}, got $got)" ;;
    *) die "the $tag release of $repo lists no sha256 digest for $name" ;;
esac
say "$name matches the sha256 digest GitHub lists for it"

has_sums="$(gh release view "$tag" --repo "$repo" --json assets \
    -q '[.assets[] | select(.name == "SHA256SUMS")] | length')"
[ -n "$has_sums" ] || die "cannot read the assets of the $tag release of $repo"
if [ "$has_sums" = 0 ]; then
    release_signed "$tag" &&
        die "$tag is at or above $signed_since, the first signed release, and its SHA256SUMS is missing"
    say "$tag predates signed checksums (SHA256SUMS): the digest is all it has"
    exit 0
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
gh release download "$tag" --repo "$repo" --dir "$work" \
    --pattern SHA256SUMS --pattern SHA256SUMS.sigstore.json ||
    die "cannot download SHA256SUMS of $tag"
want="$(awk -v f="$name" 'length($0) == 66 + length(f) && substr($0, 67) == f { print substr($0, 1, 64) }' \
    "$work/SHA256SUMS")"
[ "$(printf '%s\n' "$want" | grep -c .)" = 1 ] || die "SHA256SUMS of $tag does not list $name exactly once"
[ "$want" = "$got" ] || die "$name does not match its sha256 in SHA256SUMS (expected $want, got $got)"
say "$name matches its line in SHA256SUMS"

# The identity the signature and the provenance must both carry: REPO's
# release workflow, run from main (a release the tag job made) or from TAG
# (a tag pushed by hand), and no other ref. The repository and the tag are
# literal text in the pattern. The same as get.sh's and ae's.
regex_literal() {
    printf '%s' "$1" | sed 's/[]\\.*^$+?(){}|[]/\\&/g'
}
idre="^https://github\\.com/$(regex_literal "$repo")/\\.github/workflows/release\\.yml@refs/(heads/main|tags/$(regex_literal "$tag"))\$"

if have cosign; then
    [ -s "$work/SHA256SUMS.sigstore.json" ] ||
        die "the $tag release has no SHA256SUMS.sigstore.json for cosign to check"
    cosign verify-blob --bundle "$work/SHA256SUMS.sigstore.json" \
        --certificate-identity-regexp "$idre" \
        --certificate-oidc-issuer https://token.actions.githubusercontent.com \
        "$work/SHA256SUMS" || die "the signature of SHA256SUMS of $tag does not verify"
    say "the signature of SHA256SUMS verified with cosign"
else
    say "the signature of SHA256SUMS was not checked: cosign is not installed"
fi

# Anchored: --signer-workflow matches a prefix, and so takes the release
# workflow run from any branch or tag. gh's own report is kept and shown on
# a failure: its one-line error is the same for a certificate that names
# another identity as for one it cannot check at all.
if ! gh attestation verify "$file" --repo "$repo" --cert-identity-regex "$idre" \
    --cert-oidc-issuer https://token.actions.githubusercontent.com >"$work/gh-verify.log" 2>&1; then
    {
        printf 'verify-release-asset: gh attestation verify said:\n'
        sed 's/^/  /' "$work/gh-verify.log"
        printf 'verify-release-asset: %s, asked for an identity matching %s\n' "$(gh --version | head -n 1)" "$idre"
    } >&2
    die "$name has no build provenance from $repo's release workflow that gh accepts"
fi
say "$name's build provenance verified with gh attestation verify"
