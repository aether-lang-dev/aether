# Release binary portability across glibc versions

## The problem

The prebuilt Linux `ae` / `aetherc` binaries published to GitHub Releases are
dynamically-linked glibc PIEs. glibc uses **forward-only symbol versioning**: a
binary linked on a machine with glibc *N* references versioned symbols like
`GLIBC_2.38`, and a target machine whose glibc is **older** than *N* cannot
satisfy them, the loader aborts at startup with:

```
./ae: /lib/x86_64-linux-gnu/libc.so.6: version `GLIBC_2.38' not found (required by ./ae)
```

The release workflow historically built the Linux artifact on `ubuntu-latest`.
That label tracks the newest Ubuntu image, as of this writing Ubuntu 24.04,
which ships **glibc 2.39**. The resulting binary therefore demanded glibc ≥2.38
and failed to load on still-current distributions, e.g. Debian 12 (glibc 2.36),
which is how this surfaced.

Empirically the drift is entirely a build-host artifact, not a source issue: the
*same* compiler source built on a glibc-2.36 box references only up to
`GLIBC_2.34`; built on glibc-2.39 it pulls `GLIBC_2.38`. The linker binds to
whatever the host provides.

## The crucial narrowing: only two binaries are affected

Aether compiles by **emitting C and invoking a C compiler**. The `ae` driver
resolves that compiler at runtime (`AE_CC` → `CC` → `gcc`; see `tools/ae.c`), and
cross-compilation goes through `zig cc` via `--target`. So **every machine that
runs Aether already has a working C toolchain and its own glibc**, Aether cannot
function without one.

The consequence is important for scoping the fix: **the glibc-version hazard is
confined to the `ae` and `aetherc` executables themselves.** Once `ae` runs, it
builds the user's program with the *target's* `cc` against the *target's* glibc,
so the programs Aether produces are automatically compatible with the machine
they were built on. We do **not** need to solve "portable output"; we only need
the two shipped binaries to *load* on an old-glibc host. This is what makes the
cheap fixes viable.

## Current fix (shipped)

Pin the Linux release build to the **oldest glibc we intend to support** instead
of `ubuntu-latest`. The build matrix in `.github/workflows/release.yml` uses
`ubuntu-22.04` (glibc 2.35). A binary linked against 2.35 loads on anything with
glibc ≥2.35: Debian 12 (2.36), Ubuntu 22.04+, current Fedora, recent RHEL/derived
distros, etc.

This is a one-line change (plus the matching `if: matrix.os ==` setup guard) with
no downside for the covered distros. Its limits are what motivate the longer-term
plan below.

### Immediate workaround for anyone still hitting it

The release tarball bundles the full `runtime/` and `std/` sources, and the target
already has `gcc`. So a too-new-glibc failure is always recoverable in seconds by
rebuilding the two binaries locally against the host glibc:

```sh
# from an extracted release tree, or a clone checked out at the release tag
make compiler ae
```

The rebuilt `ae`/`aetherc` reference only the local glibc's symbols and run fine.
This is the escape hatch regardless of what baseline the release is pinned to.

## Longer-term plan

### Rationale

`ubuntu-22.04` is a floor, not a solution:

1. **The floor keeps rising.** GitHub deprecates old runner images; when
   `ubuntu-22.04` ages out we are forced onto a newer glibc and the floor jumps
   again. This is a recurring maintenance tax, not a fixed decision.
2. **2.35 still excludes long-lived enterprise/embedded targets.** RHEL 8 /
   Amazon Linux 2 (glibc 2.28/2.26), older Debian, and musl distros (Alpine,
   common in containers) are all out. For a systems language that markets
   itself for constrained and cross-built targets, "runs on Alpine" and "runs on
   the LTS server distro in the datacenter" are reasonable expectations.

The durable answer is to make the Linux release binary **not depend on the host
glibc at all**. Two established techniques:

- **musl static (preferred).** Link `ae`/`aetherc` fully statically against musl
  libc: `zig cc -target x86_64-linux-musl` (Aether already carries the zig-cc
  cross backend used by `ae --target` and `test-cross`; Windows releases already
  link `-static`). A static-musl binary has **zero** dynamic libc dependency and
  runs on every x86-64 Linux, glibc distros, Alpine, containers, old and new
  alike. This is what Go ships and what portable CLI tools converge on.

- **manylinux-style old-glibc container.** Build inside
  `quay.io/pypa/manylinux_2_28` (glibc 2.28) or `manylinux2014` (glibc 2.17).
  Still a glibc binary, but pinned to a baseline old enough (2.17/2.28) to cover
  essentially everything in service. This is the Python-wheel strategy.

Recommended direction: **static-musl**, because it is the only option that also
covers musl distros and never needs re-pinning as runner images age. The
manylinux route is the fallback if a static-musl link surfaces blocking issues
(see the quandary).

### The quandary

Static-musl is not free, and the trade-offs are why this is a plan rather than a
same-day change:

1. **Optional native dependencies.** The release binary bakes in OpenSSL, zlib,
   PCRE2, and (via std) other C libraries so `std.net` TLS, `std.zlib`,
   `std.regex` etc. work out of the box. Under a static-musl link each of these
   must be available as a **musl-built static archive**, or the corresponding
   feature silently drops to its "unavailable" stub in the shipped binary, a
   regression for downstream users who currently get TLS/zlib/regex for free.
   zig cc can build these from source, but it is real per-dependency work and
   must be verified feature-by-feature, not assumed.

2. **Runtime behavior differences.** musl is not glibc. Differences that can
   bite: NSS / `getaddrinfo` (musl's resolver is simpler and has historically
   differed on `/etc/hosts`, search domains, and IPv6 ordering, relevant to
   `std.net`), locale handling, DNS, and dlopen (a fully static binary cannot
   `dlopen`, check whether `std.dl` or any plugin path needs it). The full
   `make test` / `make test-ae` suite must pass **when run as the static-musl
   binary**, not merely compile.

3. **The toolchain-at-runtime interaction.** Because `ae` shells out to the
   host `cc`, a static-musl `ae` on a glibc host will invoke the host's *glibc*
   gcc to build the user's program, which is correct and desirable (the user's
   program matches the user's system). But it means the shipped `ae` being musl
   does **not** make user output musl; the two are independent. This is fine, but
   it must be documented so nobody assumes a musl `ae` produces musl programs.

4. **Cost vs. benefit sizing.** The `ubuntu-22.04` pin already covers the large
   majority of real reports at ~zero cost and ~zero risk. Static-musl buys the
   long tail (Alpine, RHEL 8, Amazon Linux 2, future-proofing against runner
   deprecation) at the cost of a meaningful CI + dependency-packaging + testing
   effort. It should be scheduled deliberately, with the dependency matrix
   (item 1) and the musl test pass (item 2) as explicit acceptance criteria
   not bolted on reactively.

### Acceptance criteria for the static-musl release (when scheduled)

- `ae`/`aetherc` link fully static against musl (`ldd` reports "not a dynamic
  executable"); the binary loads and runs on a glibc distro, on Alpine, and in a
  `scratch`/`distroless` container.
- `AETHER_HAS_OPENSSL`, `AETHER_HAS_ZLIB`, `AETHER_HAS_PCRE2` (and any other
  baked-in feature flags) are **defined** in the shipped binary, verified by a
  smoke test that exercises TLS, zlib round-trip, and a regex match, not just by
  the flags being set at build time.
- `make test` + `make test-ae` pass **executed as the static-musl binary**,
  including the `std.net` / DNS / resolver tests.
- Docs state plainly that a musl `ae` still builds user programs with the host
  `cc` against the host libc (item 3).

Until then, the `ubuntu-22.04` pin plus the "rebuild from bundled source"
workaround is the supported story.
