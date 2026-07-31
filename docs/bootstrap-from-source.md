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

make ae                      # build the compiler (aetherc) + the `ae` CLI, no tests
sudo make install            # install ae, aetherc, libaether.a, stdlib to /usr/local

# Optional: native contrib modules (sqlite, host_python, …), built only
# where the dev libraries are present; the rest are skipped, not fatal.
make contrib
sudo make install-contrib
```

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
| `sqlite` | `libsqlite3-dev` | `pkg-config sqlite3` / `<sqlite3.h>` |
| `host_lua` | `liblua5.4-dev` (or 5.3) | `pkg-config lua5.4\|lua5.3\|lua` |
| `host_python` | `python3-dev` | `python3-config --includes` |
| `host_ruby` | `ruby-dev` | `pkg-config ruby-3.x\|ruby` |
| `host_perl` | `perl` + `ExtUtils::Embed` | `perl -MExtUtils::Embed -e ccopts` |
| `host_tcl` | `tcl-dev` | `pkg-config tcl` |
| `host_js` | `duktape` dev | `pkg-config duktape` |

On Windows, build under MSYS2 (`pacman -S mingw-w64-x86_64-gcc make git`);
see the README "Building on Windows" section.

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
./build/ae run examples/basics/hello.ae
```

If you want to stay entirely in the source tree (no system install),
you can stop here and use `./build/ae` directly, its `cflags` will
point the linker at `build/libaether.a` in this tree (run `make stdlib`
once so that archive exists).

### 2. Install (still no tests)

```sh
sudo make install            # PREFIX defaults to /usr/local
```

`make install` builds the optimized compiler, the `ae` CLI, and the
stdlib archive, then lays them down, **it has no test dependency**.
Override the location with `PREFIX`:

```sh
make install PREFIX="$HOME/.local"     # no sudo needed for a user prefix
```

What lands where (full reference: [install-layout.md](install-layout.md)):

```
$(PREFIX)/bin/ae, $(PREFIX)/bin/aetherc
$(PREFIX)/lib/aether/libaether.a            # the runtime/stdlib archive
$(PREFIX)/share/aether/…                     # shipped source, MANIFEST, contrib descriptors
```

Make sure `$(PREFIX)/bin` is on your `PATH`.

> **Alternative, user-local install via `./install.sh`.** This
> source-builds and installs to `~/.aether` and patches your shell rc.
> Note it installs `ae`/`aetherc` and the contrib *descriptors* (so
> `import contrib.X` resolves) but does **not** build the contrib native
> `.a` archives, for those, run the contrib targets below against the
> same prefix.

### 3. Contrib modules (optional, deps-permitting)

```sh
make contrib                 # probe + build only what your system can
sudo make install-contrib    # install the built archives + descriptors
```

`make contrib` runs `tests/scripts/contrib_build.sh`, which probes each
module's dependency and builds only the ones it finds. Expected output
looks like:

```
  sqlite             OK   build/contrib/libaether_sqlite.a
  host_python        OK   build/contrib/libaether_host_python.a
  host_lua           OK   build/contrib/libaether_host_lua.a
  host_perl          OK   build/contrib/libaether_host_perl.a
  host_ruby          OK   build/contrib/libaether_host_ruby.a
  host_js            SKIP (dev library not found)
  host_tcl           SKIP (dev library not found)
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
make clean && make ae        # clean rebuild, incremental sometimes misses aetherc reshapes
sudo make install
make contrib && sudo make install-contrib   # if you use contrib
```

The version (`ae version`) is derived from the highest `v*.*.*` git tag,
falling back to the `VERSION` file. After a `git pull` that introduced a
new tag, a `make clean` ensures the stamp isn't stale.

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
  above). The default `PREFIX` is `/usr/local`.
- A `--emit=lib` link error mentioning `recompile with -fPIC` means a
  stale pre-0.181 `libaether.a`; rebuild from current `HEAD`. A version
  mismatch between `ae version` and the CHANGELOG/`VERSION` usually means
  stale git tags: `git fetch --tags` then `make clean && make ae`.

---

## See also

- [install-layout.md](install-layout.md), exactly what `make install` /
  `install.sh` lay down, and the MANIFEST contract aeb relies on.
- [getting-started.md](getting-started.md), first program, project
  config, error handling, the language tour.
- [emit-lib.md](emit-lib.md), building Aether as a `.so`/`.dylib` for
  FFI hosts (`--emit=lib`).
- [CONTRIBUTING.md](../CONTRIBUTING.md), the contributor flow, including
  the full CI suite you'd run if you were changing Aether itself.
