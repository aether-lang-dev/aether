# Release checksums, signatures and provenance

Every release since #3752 publishes, beside its archives:

- **`SHA256SUMS`**: the sha256 of every asset, one line each, in the format
  `sha256sum` writes (`<64 hex digits>  <name>`).
- **`SHA256SUMS.sigstore.json`**: a keyless [cosign](https://docs.sigstore.dev/)
  signature of `SHA256SUMS`. The release workflow signs with its GitHub OIDC
  token: Fulcio issues a short-lived certificate naming the workflow
  (`https://github.com/aether-lang-dev/aether/.github/workflows/release.yml@refs/heads/main`,
  or `@refs/tags/vX.Y.Z` for a tag pushed by hand), and the signature is
  recorded in the Rekor transparency log. No signing key exists, so none can
  leak; the bundle holds the certificate and the log entry.
- **A build provenance attestation** for every asset
  ([actions/attest-build-provenance](https://github.com/actions/attest-build-provenance)),
  stored by GitHub and checked with `gh attestation verify`. It records the
  workflow, commit and run that built the file (SLSA provenance).

The `sign` job in `.github/workflows/release.yml` writes and signs
`SHA256SUMS` from the artifacts the build jobs made, and attests each file it
lists. It alone holds `id-token: write` and `attestations: write`, and cannot
write the repository; the `publish` job, which can, holds no OIDC token. Before
uploading, `publish` checks every asset against `SHA256SUMS` and that the file
lists every asset, so what is published is what was signed. A release is
published with all three or not at all.

## What the installers check

`get.sh`, `ae version install`, `ae install` and `ae upgrade` verify a release
archive before anything in it is unpacked:

1. **`SHA256SUMS`**: the archive must match its line, and the sha256 digest
   GitHub's API lists for the asset when the API can be read. `SHA256SUMS` is
   fetched from the release URL, or through the API when that fails, so the
   check still works when the API's rate limit is spent.
2. **The signature**, when `cosign` is installed (2.4 or later): it must verify
   as made by this repository's release workflow, under GitHub's token issuer
   (`https://token.actions.githubusercontent.com`).
3. **The provenance**, when `gh` is installed and logged in (`gh auth status`):
   `gh attestation verify` must accept the archive as built by
   `aether-lang-dev/aether/.github/workflows/release.yml`.

A check that fails refuses the archive, and nothing is installed (`get.sh` does
not fall back to a source build after a refusal). A tool that is missing is
said, not passed over in silence:

```
Verified aether-0.930.0-linux-x86_64.tar.gz against SHA256SUMS and the sha256 digest GitHub lists.
  The signature of SHA256SUMS was not checked: cosign is not installed.
  Its build provenance was not checked: gh is not installed.
```

Releases from before #3752 have no `SHA256SUMS`. Their archives are checked
against the sha256 digest GitHub's API lists (#3748), as before, and the
installer says that the release predates signed checksums. Every older version
still installs.

The release workflow verifies the previous release the same way before its ABI
matrix runs it (`scripts/verify-release-asset.sh`), since that release runs
inside the job that builds the next one.

## Verifying by hand

```sh
tag=v0.930.0
gh release download "$tag" --repo aether-lang-dev/aether \
    --pattern 'aether-*-linux-x86_64.tar.gz' --pattern 'SHA256SUMS*'

# The checksums
sha256sum -c --ignore-missing SHA256SUMS

# Their signature
cosign verify-blob --bundle SHA256SUMS.sigstore.json \
    --certificate-identity-regexp '^https://github\.com/aether-lang-dev/aether/\.github/workflows/release\.yml@refs/(heads/main|tags/v[0-9.]+)$' \
    --certificate-oidc-issuer https://token.actions.githubusercontent.com \
    SHA256SUMS

# The archive's build provenance
gh attestation verify aether-*-linux-x86_64.tar.gz --repo aether-lang-dev/aether \
    --signer-workflow aether-lang-dev/aether/.github/workflows/release.yml
```

Or, with `gh` alone: `sh scripts/verify-release-asset.sh aether-lang-dev/aether
"$tag" aether-*-linux-x86_64.tar.gz`.
