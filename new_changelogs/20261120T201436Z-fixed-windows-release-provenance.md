- **Releases are published again: the Windows leg's provenance check works
  (#3791).** 0.928.0, 0.929.0 and 0.930.0 were not published. Each failed in
  `Build -- windows-x86_64` at the check of the previous release's build
  provenance, while the Linux and macOS legs accepted the same attestation.
  Git Bash turns the backslashes of an argument it hands a native program
  into slashes, so `gh.exe` was asked for an identity of `release/.yml`,
  which no certificate has. `scripts/verify-release-asset.sh` now runs
  `gh` and `cosign` with that rewriting off and gives them Windows paths. On
  a failure it prints gh's own report, gh's version and the regex. A new CI
  job, `ci-windows-release-verify`, runs the release leg's step on every PR,
  so a Windows-only failure there shows up before a release.
