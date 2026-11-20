- **A release `ae` links `std.casper` programs on FreeBSD and GhostBSD.** The
  Casper libraries came from a list the Makefile built by globbing `/lib` of
  the machine that built `ae`, and the release's FreeBSD `ae` is built on a
  Linux runner, so its list was empty and a program importing `std.casper`
  failed to link on `cap_init`, `cap_getaddrinfo` and the rest unless
  `AETHER_LDFLAGS_EXTRA` named the library files. `ae` now finds `libcasper`
  and the `cap_pwd`, `cap_sysctl` and `cap_net` (else `cap_dns`) libraries
  itself when it links: `-l<name>` where the unversioned `.so` exists, else the
  versioned file, which is all GhostBSD has. `AETHER_CASPER_LIBS` is removed.
