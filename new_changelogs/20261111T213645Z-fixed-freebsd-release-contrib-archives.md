- **The FreeBSD release ships `lib/contrib/` like the Linux and macOS
  ones.** Its archive had no `libaether_sqlite.a` or `libsqlite3.a`, because
  those are built natively by each leg and the FreeBSD leg is cross-built,
  so a program linked with `-laether_sqlite -lsqlite3` (aeb's link, an
  `aether.toml` `link_flags` list: sae on FreeBSD) failed with "unable to
  find library". `scripts/stage-release.sh --contrib-target <triple>` now
  builds the two archives with zig for the target (`-fPIC`, as the others),
  checks their objects are for it, and the FreeBSD leg passes it; PR CI's
  FreeBSD cross job stages them the same way. The cross-mode architecture
  check in `contrib_build.sh` looked for "FreeBSD" in `file`'s description
  of a relocatable object, which names no OS, so it refused every FreeBSD
  archive; it checks ELF and the machine now. And a `FREEBSD=1` cross build
  from a macOS host now gives a toolchain that works on FreeBSD: archives
  are written with `zig ar` (macOS's `ar` indexed no ELF symbol, leaving
  `libaether.a` empty) and programs are not linked with the host's CoreAudio
  frameworks.
