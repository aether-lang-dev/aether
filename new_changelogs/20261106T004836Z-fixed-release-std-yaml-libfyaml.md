- **The Linux and macOS releases ship a working `std.yaml`.** Their runners had
  no libfyaml, `YAML=auto` built the stubs without a word, and the 0.889.0
  archives answered every yaml call with "std.yaml unavailable: this build has
  no libfyaml". The release jobs now install it (apt `libfyaml-dev`, brew
  `libfyaml`), build with `YAML=1`, which now stops `make` when pkg-config
  cannot find libfyaml instead of acting as `auto`, and run a yaml program
  before packaging. `ae` and `aetherc` no longer link libfyaml themselves, so a
  toolchain built with it starts without it; a program importing `std.yaml`
  links it, and needs it installed where it is built. Windows, FreeBSD and
  Android releases keep the stubs (no static MSYS2 libfyaml; none in the cross
  sysroots).
