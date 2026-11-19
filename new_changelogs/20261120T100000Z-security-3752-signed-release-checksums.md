- **Releases are signed, and the installers verify them (#3752).** Every
  release now publishes `SHA256SUMS` over all its assets, a keyless cosign
  signature of it (`SHA256SUMS.sigstore.json`, made with the release
  workflow's GitHub OIDC identity and logged in Rekor; no long-lived key or
  secret) and a build provenance attestation per asset. `get.sh`,
  `ae version install`, `ae install` and `ae upgrade` check an archive
  against `SHA256SUMS` and GitHub's digest before unpacking it, verify the
  signature when `cosign` is installed and the provenance when `gh` is
  installed and logged in, refuse on any failure, and say which checks they
  could not make. `SHA256SUMS` is fetched from the release URL, so a spent
  API rate limit no longer blocks an install of a signed release. Releases
  from before this change are checked by GitHub's digest, as before. The
  release workflow verifies the previous release before its ABI matrix runs
  it. See `docs/release-signing.md`.
