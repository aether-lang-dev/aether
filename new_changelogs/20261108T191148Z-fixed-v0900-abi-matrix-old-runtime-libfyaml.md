- **The release's ABI matrix can load the previous release's shared runtime
  again, and an import that cannot load a library says why.** 0.894.0 to
  0.899.0 ship a shared runtime linked to a system libfyaml; since std.yaml's
  libfyaml is vendored the release workflow installs none, so 0.900.0's
  matrix failed on "cannot load the runtime" (no ABI break: with libfyaml
  present the matrix passes against 0.899.0). The step now installs it, as
  ci.yml's already did, and the matrix prints the loader's reason. `ae
  build` importing a binary library the loader refuses (a dependency
  missing) now prints the loader's error before the "unresolved import",
  which alone said no such module existed with the library right there.
