# REPLY: the FreeBSD release `ae` does not link std.casper on GhostBSD

**Re:** aeo `asks/freebsd-release-casper-link.md`

**RESOLVED** on the branch `batch/getsh-msys-freebsd-casper` (a release that
carries it): a program that imports `std.casper` links on stock FreeBSD and on
GhostBSD with a release `ae` and with no environment variable. aeo can drop
`AETHER_LDFLAGS_EXTRA` for the Casper libraries.

**The report's theory was half right.** The release `ae` did carry no Casper
link libraries. `tools/ae.c` took them from `AETHER_CASPER_LIBS`, which the
Makefile filled by globbing `/lib` of the machine the build ran on, and only
when that machine was FreeBSD. The release's FreeBSD `ae` is cross-built on a
Linux runner, so the glob found nothing and the macro was empty; a source
build on FreeBSD had the paths and linked. Reproduced on ghostbsd by building
`ae` there with `CASPER_LDFLAGS=` (which is what the cross-build leaves): the
link fails on `cap_getpwnam`, `cap_sysctlbyname` and the rest, as in the
report. The glob also meant a source-built `ae` carried the paths of the
machine it was built on.

**The second half is true too:** GhostBSD ships only the versioned files for
these libraries (`/lib/libcasper.so.1`, `libcap_pwd.so.1`, `libcap_sysctl.so.2`,
`libcap_net.so.1`, `libcap_dns.so.2`, `libcap_grp.so.1`) and no `.so` links
under `/lib` or `/usr/lib`, so `-lcasper` does not resolve there. Stock
FreeBSD has the `/usr/lib/libcasper.so` links.

**The fix.** `ae` on FreeBSD now finds the libraries when it links a program
(`freebsd_casper_libs` in `tools/ae.c`): `-l<name>` where the unversioned
`lib<name>.so` exists, else the versioned `lib<name>.so.N` file itself, for
`casper`, `cap_pwd`, `cap_sysctl` and `cap_net`, or `cap_dns` where there is
no `cap_net`. `AETHER_CASPER_LIBS` is gone. The libraries are the ones
`std/casper/aether_casper.c` calls; `libcap_grp` was in the cross-build list
but nothing calls it, so the native list leaves it out.

**Verified** on ghostbsd (FreeBSD 15.1): with `ae` built from the branch and
no `AETHER_LDFLAGS_EXTRA`, `tests/freebsd/run.sh` passes (4 of 4, including
`casper_delegation`), and aeo's `bin/aeo` builds from a fresh clone. The
stock layout, with `.so` links, was checked on the same host with the library
lookup alone against a scratch directory laid out that way; it yields
`-lcasper -lcap_pwd -lcap_sysctl -lcap_net`. Not checked: a release tarball's
`ae` itself on GhostBSD (the branch's `ae` is a source build there; the change
does not depend on how `ae` was built), and a stock FreeBSD install.
