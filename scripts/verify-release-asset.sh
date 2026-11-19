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
#   - an older release has neither, and the digest is all there is.
#
# Exits 0 when every check that can be made passed, saying which were made;
# 1 on a mismatch or a check that fails. release.yml runs it on the previous
# release it downloads for the ABI matrix, inside the job that builds the
# next one.

set -eu

[ $# -eq 3 ] || { echo "usage: $0 REPO TAG FILE" >&2; exit 2; }
repo="$1"; tag="$2"; file="$3"
name="$(basename "$file")"
say() { printf 'verify-release-asset: %s\n' "$*"; }
die() { printf 'verify-release-asset: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

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
if [ "$has_sums" = 0 ]; then
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

if have cosign; then
    rr="$(printf '%s' "$repo" | sed 's/[.]/\\./g')"
    rt="$(printf '%s' "$tag" | sed 's/[.]/\\./g')"
    cosign verify-blob --bundle "$work/SHA256SUMS.sigstore.json" \
        --certificate-identity-regexp "^https://github\\.com/$rr/\\.github/workflows/release\\.yml@refs/(heads/main|tags/$rt)\$" \
        --certificate-oidc-issuer https://token.actions.githubusercontent.com \
        "$work/SHA256SUMS" || die "the signature of SHA256SUMS of $tag does not verify"
    say "the signature of SHA256SUMS verified with cosign"
else
    say "the signature of SHA256SUMS was not checked: cosign is not installed"
fi

gh attestation verify "$file" --repo "$repo" \
    --signer-workflow "$repo/.github/workflows/release.yml" >/dev/null ||
    die "$name has no build provenance from $repo's release workflow that gh accepts"
say "$name's build provenance verified with gh attestation verify"
