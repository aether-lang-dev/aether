- **`ae toolchain zig` installs the pinned Zig and verifies it.** `ae build
  --target=<triple>` cross-compiles with Zig 0.16.0, and the only pinned,
  verified way to get it was `scripts/get-zig.sh`, which exists only in a
  source checkout and fetched only the x86_64 Linux archive. The new command
  installs it on x86_64 and aarch64 Linux, macOS, Windows and FreeBSD into
  `~/.aether/toolchains/zig-0.16.0/` (or under `$AETHER_HOME`), checks the
  archive against a sha256 compiled into `ae` before unpacking it, refuses a
  mismatch with both hashes printed, renames the finished tree into place,
  and prints the path of `zig` as its last line. `--check` verifies an
  install without downloading. The version and the hashes live in
  `tools/ae_zig_pin.h`, which `scripts/get-zig.sh` now reads too, and the
  script hands its work to `ae toolchain zig` when an `ae` is available
  (#3623).
