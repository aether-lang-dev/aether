- **A macOS `--emit=lib` cross build from Linux or FreeBSD is now a loadable
  dylib.** zig spells the macOS targets `aarch64-macos-none`, with no
  "-apple-" in them, so the cross link took them for ELF: no install name
  (zig recorded the `-o` build path as the dylib's id, and every program
  linked against it failed with "Library not loaded"), GNU strip flags under
  `--size`, and zig's default minimum of macOS 15.0. Every Apple spelling now
  links as Mach-O: `-dynamiclib -install_name @rpath/<leaf>`, as the native
  macOS build sets it, and Apple ld's `-x -dead_strip`. Every macOS cross build
  is stamped with an explicit minimum, macOS 13.0 by default, overridden with
  `AETHER_MACOS_MIN`. A Mac host had hidden the id half by rewriting it with
  install_name_tool afterwards.
