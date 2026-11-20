- **`get.sh` gives `cosign` and `gh` the identity regex unchanged on Git Bash
  and MSYS2.** Those shells rewrite an argument that looks like a path before
  a native Windows program receives it, which turned the backslashes of
  `release\.yml` in the certificate-identity regex into slashes, as #3791 found
  in `scripts/verify-release-asset.sh`. `get.sh` built the same regex and ran
  the same programs the same way. `gh_release_download` now runs `cosign` and
  `gh` with the rewriting off and gives them their files as Windows paths. `get.sh`
  installs no prebuilt on Windows yet, so this is not reached there today. A new
  test, `test_getsh_native_args.sh`, runs its download check against native
  stand-ins for the two tools, and the Windows CI job runs it and then the same
  check on the latest release.
