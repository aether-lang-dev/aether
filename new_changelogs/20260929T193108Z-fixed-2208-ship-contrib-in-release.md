- **A gh-release binary can now `import contrib.<X>` (#2208).** The release
  tarball shipped `share/aether/runtime` and `std` but not `contrib`, so a
  project that got Aether from a release, rather than a source install, could
  not build against any contrib module. `release.yml` now copies and trims
  `contrib/` into the tarball the way `install.sh` already does, so
  `import contrib.jq` resolves and `ae build` compiles the module in.
  Separately, the trim script (and the install-layout tests) scanned only
  each `module.ae` for `@source`, so it deleted `contrib/jq/aether_jq.c`,
  which `value.ae` names — an installed or released jq that could not build.
  Both now scan every `.ae`.
