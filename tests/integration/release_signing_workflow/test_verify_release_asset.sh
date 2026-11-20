#!/bin/sh
# scripts/verify-release-asset.sh, which release.yml runs on the previous
# release before the ABI matrix unpacks it (#3752), keeps to the signing
# floor (signed_since in the script) as get.sh and ae do.
#
# Fake `gh` and `cosign` programs stand in for the real ones: gh answers the
# release listing from files this test puts in a directory, and logs how it
# was called. Pinned:
#   1. a signed release whose SHA256SUMS lists the asset verifies, cosign
#      and gh are asked, both with the same identity anchored at both ends
#      (the release workflow, run from main or from the tag), and it says so;
#   2. a release at or above the floor stripped of SHA256SUMS is refused;
#   3. one stripped of the bundle alone is refused when cosign is installed;
#   4. a release below the floor, with no SHA256SUMS, passes on its digest.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/portable.sh"
NAME=verify_release_asset
VRA="$ROOT/scripts/verify-release-asset.sh"

[ -f "$VRA" ] || { echo "  [SKIP] $NAME: no scripts/verify-release-asset.sh"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() {
    echo "  [FAIL] $NAME: $1"
    [ -n "$2" ] && [ -f "$2" ] && sed 's/^/        /' "$2"
    exit 1
}

FLOOR="$(sed -n 's/^signed_since=\(.*\)$/\1/p' "$VRA")"
[ -n "$FLOOR" ] || fail "scripts/verify-release-asset.sh sets no signed_since"
ABOVE="v$FLOOR"
BELOW="v0.0.1"

mkdir -p "$TMP/rel" "$TMP/bin" "$TMP/nocosign"
ASSET="$TMP/aether-0.0.0-linux-x86_64.tar.gz"
head_c 3000 /dev/urandom > "$ASSET"
SUM="$(sha256sum "$ASSET" 2>/dev/null | awk '{print $1}')"
[ -n "$SUM" ] || SUM="$(shasum -a 256 "$ASSET" | awk '{print $1}')"
[ "${#SUM}" = 64 ] || { echo "  [SKIP] $NAME: no sha256 tool"; exit 0; }

# gh: `release view` answers the digest and the SHA256SUMS count from
# $TMP/rel, `release download` copies the files the patterns name, and
# `attestation verify` passes. cosign passes when its bundle exists.
cat > "$TMP/bin/gh" <<EOF
#!/bin/sh
echo "gh \$*" >> "$TMP/tools.log"
case "\$1 \$2" in
    "release view")
        case "\$*" in
            *digest*) echo "sha256:$SUM" ;;
            *SHA256SUMS*) if [ -f "$TMP/rel/SHA256SUMS" ]; then echo 1; else echo 0; fi ;;
        esac ;;
    "release download")
        shift 2
        while [ \$# -gt 0 ]; do
            case "\$1" in
                --dir) d="\$2"; shift ;;
                --pattern) [ -f "$TMP/rel/\$2" ] && cp "$TMP/rel/\$2" "\$d/" ;;
            esac
            shift
        done ;;
esac
exit 0
EOF
cat > "$TMP/bin/cosign" <<EOF
#!/bin/sh
echo "cosign \$*" >> "$TMP/tools.log"
for a in "\$@"; do
    case "\$prev" in --bundle) [ -s "\$a" ] || exit 1 ;; esac
    prev="\$a"
done
exit 0
EOF
chmod +x "$TMP/bin/gh" "$TMP/bin/cosign"
cp "$TMP/bin/gh" "$TMP/nocosign/gh"

signed() {
    printf '%s  %s\n' "$SUM" "$(basename "$ASSET")" > "$TMP/rel/SHA256SUMS"
    echo '{}' > "$TMP/rel/SHA256SUMS.sigstore.json"
}
verify() {   # $1 = case, $2 = tag, $3 = the directory of tools
    : > "$TMP/tools.log"
    PATH="$3:$PATH" sh "$VRA" aether-lang-dev/aether "$2" "$ASSET" > "$TMP/$1.log" 2>&1
}

signed
verify signed "$ABOVE" "$TMP/bin" || fail "a signed release was refused" "$TMP/signed.log"
grep -q "matches its line in SHA256SUMS" "$TMP/signed.log" || fail "SHA256SUMS was not checked" "$TMP/signed.log"
grep -q "^cosign verify-blob " "$TMP/tools.log" || fail "cosign was not asked" "$TMP/tools.log"
grep -q "^gh attestation verify " "$TMP/tools.log" || fail "gh attestation verify was not asked" "$TMP/tools.log"
# Both with the identity anchored at both ends: the release workflow run
# from main or from this tag, and no other ref (--signer-workflow, a prefix
# match, took any).
# Written without a backslash: Git Bash and MSYS2 turn "\." into "/." in an
# argument on its way to a native gh.exe or cosign.exe, and the release job's
# Windows leg then matched nothing (0.928.0 to 0.930.0 went unpublished).
IDENTITY="^https://github[.]com/aether-lang-dev/aether/[.]github/workflows/release[.]yml@refs/(heads/main|tags/$(printf '%s' "$ABOVE" | sed 's/[.]/[.]/g'))\$"
case "$IDENTITY" in *\\*) fail "the expected identity has a backslash, which MSYS rewrites" ;; esac
for flag in "cosign verify-blob .*--certificate-identity-regexp" "gh attestation verify .*--cert-identity-regex"; do
    arg="$(grep "^$flag " "$TMP/tools.log" | head -1 | sed "s/^$flag \([^ ]*\).*/\1/")"
    [ "$arg" = "$IDENTITY" ] || fail "${flag%% *} was given the identity '$arg', not '$IDENTITY'" "$TMP/tools.log"
done
grep -q -- "--signer-workflow" "$TMP/tools.log" && fail "gh was asked by --signer-workflow, a prefix match" "$TMP/tools.log"

rm -f "$TMP/rel/SHA256SUMS" "$TMP/rel/SHA256SUMS.sigstore.json"
verify stripped "$ABOVE" "$TMP/bin" && fail "a release at or above the floor without SHA256SUMS passed" "$TMP/stripped.log"
grep -q "the first signed release, and its SHA256SUMS is missing" "$TMP/stripped.log" \
    || fail "the refusal does not say SHA256SUMS is missing" "$TMP/stripped.log"

signed
rm -f "$TMP/rel/SHA256SUMS.sigstore.json"
verify nobundle "$ABOVE" "$TMP/bin" && fail "a release without its bundle passed with cosign installed" "$TMP/nobundle.log"
grep -q "no SHA256SUMS.sigstore.json for cosign" "$TMP/nobundle.log" \
    || fail "the refusal does not say the bundle is missing" "$TMP/nobundle.log"

rm -f "$TMP/rel/SHA256SUMS"
verify presigning "$BELOW" "$TMP/nocosign" || fail "a release below the floor was refused" "$TMP/presigning.log"
grep -q "predates signed checksums" "$TMP/presigning.log" \
    || fail "it did not say the release predates signed checksums" "$TMP/presigning.log"

echo "  [PASS] $NAME: the signing floor ($FLOOR) holds in scripts/verify-release-asset.sh"
