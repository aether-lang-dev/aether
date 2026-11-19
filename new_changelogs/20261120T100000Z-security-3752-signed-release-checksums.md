- **Releases are signed, and the installers verify them (#3752).** Every
  release now publishes `SHA256SUMS` over all its assets, a keyless cosign
  signature of it (`SHA256SUMS.sigstore.json`, made with the release
  workflow's GitHub OIDC identity and logged in Rekor; no long-lived key or
  secret) and a build provenance attestation per asset. `get.sh`,
  `ae version install`, `ae install` and `ae upgrade` check an archive
  against `SHA256SUMS` and GitHub's digest before unpacking it, verify the
  signature when `cosign` is installed and the provenance when `gh` is
  installed and logged in, refuse on any failure, and say which checks they
  could not make. The signature and the provenance must both name the
  release workflow run from main or from the release's own tag, and no
  other ref. `SHA256SUMS` is fetched from the release URL, so a spent
  API rate limit no longer blocks an install of a signed release. Releases
  from before 0.926.0, the first signed one, are checked by GitHub's
  digest, as before; one at or above it without `SHA256SUMS`, or without
  its signature when `cosign` is installed, is refused, so whoever can edit
  a release cannot delete them and have its archive taken on the digest
  alone. The release workflow fails a release that disagrees with that
  floor, and verifies the previous release before its ABI matrix runs it.
  See `docs/release-signing.md`.
