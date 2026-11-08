# Bootstrapping Aether from source (HEAD)

For **consumers** who want to build and use Aether straight from this
repository, because they're tracking `HEAD`, or want a feature that
hasn't reached a tagged release / package manager yet, or are a
downstream project (avn, aether-ui, aeb, servirtium-dotnet, …) that
links against the toolchain.

This is **not** the contributor flow. You do **not** need to run the
test suite to use Aether, building the binaries and (optionally) the
contrib libraries is enough. The test targets (`make test`,
`make test-ae`, `make ci`) are entirely separate and only matter if
you're changing Aether itself. See [CONTRIBUTING.md](../CONTRIBUTING.md)
for that.

If you just want the latest *released* build, prefer `./install.sh` (see
the README) or `ae version install <vX.Y.Z>` this document is for the
from-source / HEAD case specifically.

---

## TL;DR

```sh
git clone https://github.com/aether-lang-dev/aether.git
cd aether

./install.sh --contrib-too   # build from this tree, install to ~/.aether, no sudo
```

That builds the compiler, the `ae` CLI and the stdlib from the checkout,
installs them to `~/.aether`, and (with `--contrib-too`) builds and installs
the native contrib archives (sqlite, avcodec, …) for each module whose dev
library is present; the rest are skipped, not fatal. Leave the flag off if you
don't use contrib. No tests run.

Prefer this to `sudo make install`. An install under `/usr/local` is
root-owned and shared by every checkout and session on the machine, and both
`ae` and the compiler fall back to it when they find nothing earlier, so a
stale one quietly supplies the wrong version and needs `sudo` to remove.

Then, in a consumer project, `import std.X` / `import contrib.X` resolve,
and you link Aether-as-a-library with **`$(ae cflags)`** (never hand-rolled
`-I`/`-L`/`-laether`). Details below.

---

## Prerequisites

| Need | For |
|---|---|
| **C compiler** (`gcc` or `clang`) | everything |
| **GNU make** | the build is GNU-make-based; `nmake` won't work |
| **git** | version stamping reads `git tag` (a non-git tree falls back to the `VERSION` file) |
| **pthreads + libm** | linked by default on POSIX (already present on Linux/macOS) |

Optional **system libraries** that enable specific stdlib features
(each is auto-detected via `pkg-config`; without it, the related stdlib
calls return a clean "built without ..." diagnostic rather than failing
the build):

| Lib | Enables |
|---|---|
| `libssl-dev` / `libcrypto-dev` (OpenSSL) | TLS in `std.http.client` (HTTPS) and the `std.http` server |
| `zlib1g-dev` (zlib) | `std.zlib.deflate`/`inflate`; HTTP gzip |
| `libnghttp2-dev` | HTTP/2 server (`std.http` h2/h2c + HPACK) |
| `libpcre2-dev` | `std.regex` (Perl-compatible regex via libpcre2-8) |

Optional, only if you want the corresponding **contrib** module built
natively (each is independently probed and **skipped if its dev library
is absent**, a skip is normal, not an error):

| Contrib module | Needs | Probe |
|---|---|---|
| `sqlite` | nothing: fetches the pinned amalgamation (#1372); `libsqlite3-dev` only when offline | `scripts/fetch-sqlite-amalgamation.sh`, else `pkg-config sqlite3` / `<sqlite3.h>` |
| `avcodec` | FFmpeg's `libavcodec`/`libavformat`/`libavutil`/`libswscale`/`libswresample` dev | `pkg-config libavcodec …` |
| `host_stunt`, `host_aether` | nothing (libc and the in-tree runtime) | always built |

The language bridges (Lua, Python, Tcl, Perl, Ruby, JS, Go, Java and more)
are not built here: they live in
[aether-guest-languages](https://github.com/aether-lang-dev/aether-guest-languages), which a project adds with
`ae add`.

On Windows, build under MSYS2 (`pacman -S mingw-w64-x86_64-gcc make git`);
see the README "Building on Windows" section.

### Building on NetBSD / OpenBSD

There are no release archives for these two, so `ae install` / `ae upgrade`
do not apply (`ae version` reports the platform as unsupported): build from
a clone. Checked on NetBSD 11.1 and OpenBSD 8.0, amd64.

| | NetBSD | OpenBSD |
|---|---|---|
| System compiler (`cc`) | gcc 12 | clang |
| GNU make | `gmake` (pkgsrc) | `gmake` (ports) |
| In base already | OpenSSL, zlib | LibreSSL, zlib, pkg-config |

```sh
# NetBSD (as root)
pkgin install git gmake pkgconf pcre2 nghttp2
# OpenBSD (as root)
pkg_add git gmake pcre2 nghttp2
```

`pcre2` (`std.regex`) and `nghttp2` (HTTP/2) are optional, as on any
platform. To run the test suite as well, add `bash` and `python3`.

```sh
git clone https://github.com/aether-lang-dev/aether.git
cd aether
gmake CC=cc ae stdlib     # or ./install.sh, which picks gmake and a compiler itself
./build/ae run examples/01-language/01-hello.ae
gmake CC=cc test test-ae  # optional: the C unit tests and the .ae suite
```

`make` there is BSD make, which cannot read this Makefile; always `gmake`.
`CC=cc` names the system compiler (the Makefile's default is `gcc`, which
OpenBSD does not have).

Known gaps:

- **No libc-level sandbox layer.** `spawn_sandboxed` and the
  `libaether_sandbox.so` LD_PRELOAD shim are Linux and FreeBSD only; the
  compile-time (`--emit=lib --with=`) and scope (`hide` / `seal except`)
  layers work. The preload's source compiles on both, but nothing builds or
  loads it there. On OpenBSD it would also see less: libc calls its own
  internal symbols, so an interposed `open` does not catch `fopen`, and
  libc has no `wordexp` or `syscall` to wrap.
- **`mem.heap_in_use()` is -1**: NetBSD's jemalloc does not export
  `mallctl`, and OpenBSD's malloc keeps no statistics.
- **OpenBSD has no IPv4-mapped IPv6 sockets**: a listener on `::` takes IPv6
  only. Listen on `0.0.0.0` as well for IPv4.
- **No sendfile(2)** on either: `http.serve_file` reads the file into the
  response body, as on Windows.
- **No stack trace on a panic on OpenBSD**, which has no `backtrace()`
  (NetBSD's libexecinfo is opened when a trace is needed, as FreeBSD's is).
  The panic's reason is still printed.

---

## Step by step

### 1. Build the binaries (no tests)

```sh
make ae
```

`make ae` builds both `build/aetherc` (the compiler) and `build/ae` (the
CLI front-end, `ae` depends on `compiler`). It does **not** depend on
any test target, so nothing runs the suite. To sanity-check the build
without installing:

```sh
./build/ae version
./build/ae run examples/01-language/01-hello.ae
```

If you want to stay entirely in the source tree (no system install),
you can stop here and use `./build/ae` directly, its `cflags` will
point the linker at `build/libaether.a` in this tree (run `make stdlib`
once so that archive exists).

### 2. Install (still no tests)

```sh
./install.sh                 # installs to ~/.aether; add --contrib-too for contrib
```

`install.sh` builds from the tree around it and installs to `~/.aether`. It
also records the build under `~/.aether/versions/`, so `ae use <version>`
switches between it and any other installed version, and it adds
`~/.aether/bin` to your `PATH` in your shell rc. Flags and an install prefix
combine in any order (`./install.sh --contrib-too /opt/aether`).

`make install` lays down the same files without the version record or the rc
change. Its `PREFIX` also defaults to `~/.aether`:

```sh
make install                           # ~/.aether, no sudo
make install PREFIX="$HOME/.local"     # any prefix you own
sudo make install PREFIX=/usr/local    # a system install, deliberately
```

As root, `make install` and `make install-contrib` refuse to run without an
explicit `PREFIX`, because under `sudo` `$HOME` is root's and the default
would land in `/root/.aether`.

What lands where (full reference: [install-layout.md](install-layout.md)):

```
$(PREFIX)/bin/ae, $(PREFIX)/bin/aetherc
$(PREFIX)/lib/aether/libaether.a            # the runtime/stdlib archive
$(PREFIX)/share/aether/…                     # shipped source, MANIFEST, contrib descriptors
```

Make sure `$(PREFIX)/bin` is on your `PATH`.

Without `--contrib-too`, `install.sh` installs the contrib *descriptors* (so
`import contrib.X` resolves) but not the native `.a` archives.

### 3. Contrib modules (optional, deps-permitting)

```sh
./install.sh --contrib-too   # as part of the install, or separately:
make contrib                 # probe + build only what your system can
make install-contrib         # install the built archives + descriptors to ~/.aether
```

`--contrib-too` installs the archives before the version is recorded, so they
are part of `~/.aether/versions/<v>` and `ae use` carries them; a separate
`make install-contrib` is not recorded there.

`make contrib` runs `tests/scripts/contrib_build.sh`, which probes each
module's dependency and builds only the ones it finds. Expected output
looks like:

```
  sqlite             OK   build/contrib/libaether_sqlite.a
  avcodec            SKIP (dev library not found)
  host_aether        OK   build/contrib/libaether_host_aether.a
  host_stunt         OK   build/contrib/libaether_host_stunt.a
```

A `SKIP` is not a failure, it means the dev library wasn't found and
that module isn't built. `install-contrib` places each built
archive at `$(PREFIX)/lib/aether/libaether_<module>.a` and its
`module.ae` + headers under `$(PREFIX)/share/aether/contrib/<module>/`.

---

## Consuming Aether from your project

Once installed (or built in-tree), a downstream project uses it two ways:

**As `.ae` source**, `import std.X` and `import contrib.X` resolve
against the installed `share/aether/` tree automatically. Build/run with
`ae build` / `ae run`. Per-project config (entry point, link flags) goes
in `aether.toml`.

**As a C-linkable library**, always get the flags from the toolchain,
never hand-write them:

```sh
cc myhost.c $(ae cflags) -o myhost
```

`ae cflags` emits the correct include paths plus
`-L<prefix>/lib/aether -laether -pthread -lm` for whichever install is in
effect (in-tree, user, or system), a bare `-laether` won't find the
prefixed archive path on its own.

**Linking a native contrib module**, add its libs in `aether.toml`:

```toml
[build]
link_flags = "-laether_sqlite -lsqlite3"
```

---

## Tracking HEAD over time

To move to a newer `HEAD`:

```sh
git pull
make clean                   # clean rebuild, incremental sometimes misses aetherc reshapes
./install.sh --contrib-too   # drop the flag if you don't use contrib
```

`ae --version` describes the binary in front of you: the version it was
compiled from, its own path, and the `aetherc` it will actually run with that
one's version. It says so because the two can differ, and `aetherc` is what
performs codegen; a mismatch is printed as a warning rather than left to be
discovered later. A pinned release (`~/.aether/active_version`, written by
`ae version use`) is reported as a pin and never as the binary's version.

The version stamp itself comes from the `VERSION` file in the tree. After a
`git pull`, a `make clean` ensures it isn't stale.

---

## For LLMs / automation

Deterministic, non-interactive recipe. Assumes a POSIX shell at the repo
root with a C toolchain, GNU make, and git present.

```sh
# 1. Build compiler + CLI. No tests run by this target.
make ae

# 2. Install to a writable prefix (avoids sudo in CI/sandboxes).
make install PREFIX="$PWD/.aether-prefix"

# 3. Optional contrib, never fatal; missing dev libs are SKIPped.
make contrib
make install-contrib PREFIX="$PWD/.aether-prefix"

# 4. Verify (these are the success signals to assert on):
"$PWD/.aether-prefix/bin/ae" version          # prints "ae X.Y.Z (Aether Language)"
printf 'main() { println("ok") }\n' > /tmp/smoke.ae
"$PWD/.aether-prefix/bin/ae" run /tmp/smoke.ae # prints "ok"
```

Rules of thumb:

- **Do not run the test suite** (`make test`, `make test-ae`, `make ci`)
  to *use* Aether, those validate changes *to* Aether and
  are slow. Building + installing is sufficient and touches no test code.
- **Treat contrib `SKIP` lines as success**, not failure, they mean a
  dev library is absent. Only a non-zero exit from `make contrib` /
  `make install-contrib` is a real failure.
- **Always link downstream C with `$(ae cflags)`**, do not synthesize
  `-I` / `-L` / `-laether` by hand; the include path and the
  `lib/aether/` archive location vary by install mode and `ae cflags`
  resolves them.
- Re-running `make ae` / `make install` is safe. After a `git pull`, run
  `make clean && make ae` before reinstalling so the version stamp and
  any compiler reshape are picked up.
- Use a writable `PREFIX=…` to avoid `sudo`, and make sure
  `$(PREFIX)/bin` is on `PATH` (or invoke `ae` by absolute path as
  above). The default `PREFIX` is `~/.aether`; as root, `make install`
  refuses to run without an explicit one.
- A `--emit=lib` link error mentioning `recompile with -fPIC` means a
  stale pre-0.181 `libaether.a`; rebuild from current `HEAD`.
- If `ae --version` and the `VERSION` file disagree, the binary is stale:
  `make clean && make ae`. If it warns that its `aetherc` is a different
  version, two installs are mixed; `install.sh` defaults to `~/.aether/bin`
  and `get.sh` to `~/.local/bin`, and whichever is earlier on `PATH` wins.
  Both installers now name the shadowing binary when they see one.

---

## See also

- [install-layout.md](install-layout.md), exactly what `make install` /
  `install.sh` lay down, and the MANIFEST contract aeb relies on.
- [getting-started.md](getting-started.md), installing, a first program,
  a project and the tools; the language itself is taught by the path in
  [examples/README.md](../examples/README.md) and [tutorial.md](tutorial.md).
- [emit-lib.md](emit-lib.md), building Aether as a `.so`/`.dylib` for
  FFI hosts (`--emit=lib`).
- [CONTRIBUTING.md](../CONTRIBUTING.md), the contributor flow, including
  the full CI suite you'd run if you were changing Aether itself.
