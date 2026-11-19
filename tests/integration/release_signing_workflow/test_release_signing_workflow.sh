#!/bin/sh
# The release workflow publishes SHA256SUMS, its keyless signature and build
# provenance, and only the job that signs holds the OIDC token (#3752).
#
# Pinned, by reading .github/workflows/release.yml:
#   1. the sign job asks for id-token: write and attestations: write, and no
#      other job asks for id-token: write;
#   2. sign runs cosign sign-blob over SHA256SUMS into SHA256SUMS.sigstore.json
#      and actions/attest-build-provenance over SHA256SUMS;
#   3. publish needs sign, is gated on it, checks the assets against
#      SHA256SUMS, checks the release against the signing floor before it
#      creates the release, and uploads SHA256SUMS and
#      SHA256SUMS.sigstore.json;
#   4. the previous release the ABI matrix runs is verified before it is
#      unpacked, on Unix and on Windows.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
WF="$ROOT/.github/workflows/release.yml"
NAME=release_signing_workflow

[ -f "$WF" ] || { echo "  [SKIP] $NAME: no release.yml"; exit 0; }
fail() { echo "  [FAIL] $NAME: $1"; exit 1; }

# job NAME: the lines of one top-level job (two-space indented key).
job() {
    awk -v j="  $1:" '
        $0 == j { on = 1; print; next }
        on && /^  [A-Za-z0-9_-]+:[[:space:]]*$/ { exit }
        on { print }' "$WF"
}

SIGN="$(job sign)"
PUBLISH="$(job publish)"
[ -n "$SIGN" ] || fail "release.yml has no sign job"
[ -n "$PUBLISH" ] || fail "release.yml has no publish job"

printf '%s\n' "$SIGN" | grep -q "^      id-token: write" || fail "sign does not ask for id-token: write"
printf '%s\n' "$SIGN" | grep -q "^      attestations: write" || fail "sign does not ask for attestations: write"
n=$(grep -c "id-token: write" "$WF")
[ "$n" = 1 ] || fail "id-token: write appears $n times in release.yml; only the sign job may hold it"

printf '%s\n' "$SIGN" | grep -q "cosign sign-blob --yes --bundle SHA256SUMS.sigstore.json SHA256SUMS" \
    || fail "sign does not sign SHA256SUMS keyless into SHA256SUMS.sigstore.json"
printf '%s\n' "$SIGN" | grep -q "uses: actions/attest-build-provenance@" || fail "sign does not attest provenance"
printf '%s\n' "$SIGN" | grep -q "subject-checksums: dist/SHA256SUMS" || fail "the attestation does not cover SHA256SUMS' files"

printf '%s\n' "$PUBLISH" | grep -q "needs: \[.*sign.*\]" || fail "publish does not need sign"
printf '%s\n' "$PUBLISH" | grep -q "needs.sign.result == 'success'" || fail "publish is not gated on sign"
printf '%s\n' "$PUBLISH" | grep -q "sha256sum -c --strict SHA256SUMS" || fail "publish does not check the assets against SHA256SUMS"
printf '%s\n' "$PUBLISH" | grep -q "^          SHA256SUMS$" || fail "publish does not upload SHA256SUMS"
printf '%s\n' "$PUBLISH" | grep -q "^          SHA256SUMS.sigstore.json$" || fail "publish does not upload the signature"
printf '%s\n' "$PUBLISH" | grep -q "id-token" && fail "publish holds an OIDC token"
# The signing floor is checked before the release is created.
floor_at=$(printf '%s\n' "$PUBLISH" | grep -n "name: Check the release against the signing floor" | cut -d: -f1)
create_at=$(printf '%s\n' "$PUBLISH" | grep -n "name: Create GitHub Release" | cut -d: -f1)
[ -n "$floor_at" ] || fail "publish does not check the release against the signing floor"
[ -n "$create_at" ] && [ "$floor_at" -lt "$create_at" ] || fail "publish checks the signing floor after creating the release"

n=$(grep -c 'git show "\$WF_SHA:scripts/verify-release-asset.sh" > "\$vra"' "$WF")
[ "$n" = 2 ] || fail "the previous release is verified $n times by the workflow's own script, not on Unix and Windows both"
[ -f "$ROOT/scripts/verify-release-asset.sh" ] || fail "scripts/verify-release-asset.sh is missing"

# 5. A job that checks out the release's tag runs steps against a tree that
# can predate this workflow file (the note at the build job's checkout;
# 0.757.0 failed every leg on a step calling a script its tag did not have).
# Such a step names a file of the tree only if every tag it can build has it:
# one in TREE_OK, all of them in v0.925.0. Anything newer is read from the
# workflow's own commit (git show "$WF_SHA:path"), never skipped when absent.
TREE_OK="
.github/scripts/build_release_body.py
scripts/check_archive_exports.sh
scripts/check_regex_engine.sh
scripts/check_yaml_engine.sh
scripts/fetch-sqlite-amalgamation.sh
scripts/get-zig.sh
scripts/stage-release.sh
tests/integration/abi_matrix/test_abi_matrix.sh
scripts/fetch-freebsd-base.sh
"
# (scripts/fetch-freebsd-base.sh is aether-crossbuild's, run in its checkout.)
TAG_JOBS="$(awk '
    /^  [A-Za-z0-9_-]+:[[:space:]]*$/ { j = $1; sub(/:$/, "", j) }
    /ref: \$\{\{ needs\.tag\.outputs\.tag \|\| github\.ref \}\}/ { print j }' "$WF" | sort -u)"
[ -n "$TAG_JOBS" ] || fail "no job checks out the release's tag; this check reads nothing"
for j in $TAG_JOBS; do
    named="$(job "$j" | grep -v '^[[:space:]]*#' | grep -v '^[[:space:]]*echo ' | grep -v 'git show "\$WF_SHA:' |
        grep -oE '(\./|\$GITHUB_WORKSPACE/)?(scripts|tests|\.github/scripts)/[A-Za-z0-9_./-]*[A-Za-z0-9_]' |
        sed 's#^\./##; s#^\$GITHUB_WORKSPACE/##' | sort -u)"
    for p in $named; do
        printf '%s\n' "$TREE_OK" | grep -qxF "$p" ||
            fail "the $j job, which runs on the release's tag, names $p from the tree; an older tag may lack it: read it with git show \"\$WF_SHA:$p\", or add it to TREE_OK once every tag has it"
    done
done

echo "  [PASS] $NAME: SHA256SUMS signed keyless and attested; only sign holds the OIDC token"
