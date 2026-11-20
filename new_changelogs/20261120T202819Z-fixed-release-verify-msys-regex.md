- **The Windows release job verifies the previous release again, so releases
  publish.** `scripts/verify-release-asset.sh` and `get.sh` wrote the
  identity pattern for cosign and `gh attestation verify` with `\.` for a
  dot. Git Bash and MSYS2 rewrite an argument that looks like a path on its
  way to a native program and turned each `\.` into `/.`, so on Windows the
  pattern matched no certificate, the job that builds the Windows archive
  refused the signed 0.927.0 it downloads for the ABI matrix, and 0.928.0,
  0.929.0 and 0.930.0 were tagged but never published. The pattern now
  writes a dot as `[.]` and has no backslash; a `get.sh` install from Git
  Bash with `gh` or cosign present was refused the same way and is fixed
  with it.
