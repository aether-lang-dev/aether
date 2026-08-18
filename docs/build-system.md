# Build System

## Overview

Aether uses a multi-tier build system with different optimization profiles for development, testing, and release builds.

## Project Configuration (`aether.toml`)

Every Aether project has an `aether.toml` at its root. `ae run` and `ae build` read it automatically.

### Minimal project

```toml
[package]
name = "myapp"
version = "0.1.0"

[[bin]]
name = "myapp"
path = "src/main.ae"
```

### Full configuration reference

```toml
[package]
name = "myapp"
version = "1.0.0"
description = "What this program does"

[build]
# Extra C compiler flags appended to the gcc invocation on every build path,
# including `ae run`. They come after the optimisation level, so a cflag like
# `-O3` overrides the `-O0` that `ae run` and `ae build --quick` use by default.
cflags = "-O3 -march=native"

# Platform-specific linker flags (e.g. for third-party C libraries).
# macOS/Linux: link_flags = "-lraylib"
# Windows:     link_flags = "-Ldeps/raylib/lib -lraylib -lopengl32 -lgdi32 -lwinmm"
link_flags = ""

[[bin]]
name = "myapp"
path = "src/main.ae"

# Extra C source files compiled alongside the Aether output.
# Useful for C FFI helpers, renderer backends, or any C code your program needs.
# Merged additively with any --extra flags passed on the command line.
extra_sources = ["src/ffi_helpers.c", "src/renderer.c"]
```

### Link flags you do not have to write

`link_flags` is for libraries *your* program needs. A library that a *module*
needs is declared by that module, with `@link` at the top of its `module.ae`:

```aether,fragment
@link("-laether_sqlite -lsqlite3")
```

Codegen unions those declarations across the resolved import closure into a
`// aether-link:` comment on the generated C, and `ae build` puts them on the
link command (#1549). So `import contrib.sqlite` links libsqlite3 without any
`aether.toml` entry, and it works transitively — a module three imports deep
contributes its own dependencies.

Two consequences worth knowing:

- **Your `link_flags` still win.** Module flags are placed before them on the
  command line, so anything you specify takes precedence.
- **A `-D`-dropped import drops its libraries too.** Because the flags come
  from the AST, an `import` inside a losing `when defined(...)` region never
  contributes them — the library is absent from the binary, not merely
  unused. Restating it in `link_flags` defeats this, since that is static and
  applies to every build. Prefer `@link` for a module's own dependencies.

`-L` search paths remain yours to supply; they are site-specific. The one
exception is `<lib_dir>/contrib`, added automatically so the veneer archives
`make contrib` builds resolve without configuration.

The `// aether-link:` comment also lists libraries for std modules backed by
native code (`std.http` names `-lssl -lcrypto -lnghttp2`). Those are there for
downstream C builds; `ae build` skips them, because it already passes the same
libraries from its own pkg-config detection — and passes *nothing* when the
library was not found, so `std.http` still links on a box without libnghttp2
and the h2 surface falls back to its "unavailable" stub. A module's `@link` is
for dependencies the toolchain does not probe for.

### `extra_sources` vs `--extra`

Both add C files to the build, they are additive when both are present.

| | `extra_sources` in `aether.toml` | `--extra file.c` CLI flag |
|---|---|---|
| **Scope** | Always included for this binary | Per-invocation |
| **Good for** | C helpers your program always needs | Renderer backends, platform variants |
| **Works with** | `ae build` and `ae run` | `ae build` and `ae run` |

### `--quick` for fast iteration

By default, `ae build` runs the C compiler with `-O2` to match release-quality codegen. For tight edit/build/test loops where binary speed isn't critical, pass `--quick` to drop to `-O0 -g`:

```sh
ae build src/main.ae           # release-shape, -O2 (default)
ae build src/main.ae --quick   # iteration-shape, -O0 -g
```

`--quick` typically halves the gcc step on small programs, at the cost of unoptimised codegen. `ae run` already uses `-O0` regardless, since cache hits dominate over a single optimised compile.

### Resolving the build target

`ae build` accepts either a path to a `.ae` file or a `[[bin]]` name from `aether.toml`. The two are equivalent:

```sh
ae build src/main.ae   # explicit path
ae build myapp         # [[bin]] name = "myapp"
```

When the positional argument doesn't exist as a file, `ae` checks `aether.toml`'s `[[bin]]` entries for a matching `name = "..."` and uses that bin's `path` field. Cargo and similar build systems work the same way.

If you run `ae build` from a subdirectory and there's no `aether.toml` in the current directory, `ae` walks up the directory tree looking for one. When it finds an ancestor `aether.toml`, it switches to that directory before resolving paths, so `cd src && ae build main.ae` works as if you had run `ae build src/main.ae` from the project root, and `extra_sources` declared in the toml are still applied. Walk-up only happens when there's no toml in the current directory; a project with a local `aether.toml` always wins.

---

## Build Cache

Both `ae run` and `ae build` cache compiled binaries in `~/.aether/cache/`. The cache key is an FNV64 hash of:

- The source file's content
- The `aetherc` binary's mtime (recompile invalidates everything)
- `libaether.a`'s mtime (stdlib rebuild invalidates everything)
- Every `--extra` C file's *content* (editing an FFI shim invalidates the cache, not just touching it)
- Every imported lib module's *content*: the `.ae`/`.c`/`.h` files under each lib-search directory, walked recursively. This covers both an explicit `--lib`/`$AETHER_LIB_DIR` directory and the **default `lib/`** the compiler searches when neither is set (the `src/main.ae` + `lib/<name>/module.ae` package layout). Because entries are content-hashed rather than keyed on mtime+size, a same-second, same-size edit (a one-character constant flip in an editor-save loop) still invalidates the cache, and a bare `touch` does not. This lib-dir walk is POSIX-only; on Windows the key covers the entry file and `--extra` content but not lib-module edits, so a lib-module change there still needs `ae cache clear` until the walk is wired for Windows.
- The optimisation level (`-O0` for `ae run` and `ae build --quick`, `-O2` for default `ae build`)

`ae run` and `ae build` use separate cache slots so toggling between them doesn't churn one entry back and forth.

**Cost breakdown of a build:**
- Cache hit (`ae build`): copy the cached binary to the requested output path. Sub-millisecond on local disk.
- Cache hit (`ae run`): fork + exec the cached binary directly.
- Cache miss: full `aetherc` front-end + gcc compile + link. Dominant cost is gcc; `aetherc` is a small fraction.
- First macOS run: an extra one-time pause while the OS performs its Gatekeeper check on the newly compiled binary. Subsequent runs of the same cached binary are hit-path.

```bash
ae cache          # Show cache location and entry count
ae cache clear    # Delete all cached builds
```

Wasm builds, `--emit=lib`, and `--namespace` SDK generation skip the cache (different artefact shapes; each will get its own cache layout when measurement justifies it).

---

## Build Tiers

All builds go through the Makefile, which handles the full source list across subdirectories (`compiler/parser/`, `compiler/analysis/`, `compiler/codegen/`, `runtime/scheduler/`, `runtime/memory/`, etc.).

### Development Build

```bash
make compiler CFLAGS="-O0 -g -Icompiler -Iruntime -Iruntime/actors -Iruntime/scheduler -Wall -Wextra -Wno-unused-parameter"
```

**Purpose**: Fast compilation with debug symbols for active development.

### Testing Build

```bash
make compiler    # Uses -O2 by default
```

**Purpose**: Moderate optimization for CI and testing.

### Release Build

```bash
make compiler CFLAGS="-O3 -march=native -flto -Icompiler -Iruntime -Iruntime/actors -Iruntime/scheduler -Wall -Wextra -Wno-unused-parameter"
```

**Purpose**: Full optimization for production use and benchmarking.

**Flags:**
- `-O3`: Aggressive inlining, loop unrolling, auto-vectorization
- `-march=native`: CPU-specific instruction selection
- `-flto`: Link-time optimization for cross-translation-unit inlining

### Profile-Guided Optimization

```bash
# Stage 1: Instrument
gcc -O3 -march=native -fprofile-generate -o aetherc_pgo ...

# Stage 2: Profile (run representative workload)
./aetherc_pgo <typical_usage>

# Stage 3: Optimize with profile data
gcc -O3 -march=native -fprofile-use -o aetherc ...
```

PGO uses runtime profiling data to improve branch prediction, function inlining decisions, and code layout. It is used by major projects including Chrome, Firefox, CPython, and LLVM for their release builds.

## Incremental Compilation

**Dependency Tracking:**
```makefile
CFLAGS += -MMD -MP
-include $(DEPS)

build/%.o: %.c
    $(CC) $(CFLAGS) -c $< -o $@
```

The `-MMD` flag generates `.d` dependency files listing all headers included by each source file. Make uses these to rebuild only modified files and their dependents.

## Parallel Compilation

```bash
make -j8    # 8 parallel jobs
```

Limited by dependency ordering: some files must build before others.

## Cross-Compilation (PLATFORM variable)

The `PLATFORM` Makefile variable selects the scheduler backend and sets platform-specific flags:

```bash
# Native (default), multi-core scheduler, pthreads
make stdlib PLATFORM=native

# WebAssembly, cooperative scheduler, no pthreads/fs/net
make stdlib PLATFORM=wasm    # CC=emcc, -DAETHER_NO_THREADING/FILESYSTEM/NETWORKING

# Or use the ae CLI directly:
ae build --target wasm hello.ae    # Produces hello.js + hello.wasm
node hello.js                       # Run with Node.js

# Embedded, cooperative scheduler, no pthreads/fs/net/getenv
make stdlib PLATFORM=embedded    # -DAETHER_NO_THREADING/FILESYSTEM/NETWORKING/GETENV

# Override individual features on native
make stdlib EXTRA_CFLAGS="-DAETHER_NO_THREADING"    # Auto-selects cooperative scheduler
make stdlib EXTRA_CFLAGS="-DAETHER_NO_FILESYSTEM -DAETHER_NO_NETWORKING"
```

The Makefile auto-detects `AETHER_NO_THREADING` in `EXTRA_CFLAGS` and switches to the cooperative scheduler automatically. It also omits `-pthread` from linker flags.

### Cross-building the toolchain itself (`FREEBSD=1` / `WINDOWS=1`)

`PLATFORM` above selects a scheduler/feature profile. Two separate knobs
cross-build the **`ae`/`aetherc` toolchain itself** for another OS, both via
`zig cc`:

```bash
# FreeBSD x86_64 (needs a base sysroot: zig bundles no FreeBSD libc)
make compiler ae stdlib FREEBSD=1 ZIG=/path/to/zig AETHER_SYSROOT=/path/to/base

# Windows x86_64 — no sysroot needed: zig bundles the mingw-w64 headers + CRT
make compiler ae stdlib WINDOWS=1 ZIG=/path/to/zig
file build/ae.exe    # PE32+ executable (console) x86-64, for MS Windows
```

Provision `zig` with the [aether-crossbuild](https://github.com/aether-lang-dev/aether-crossbuild)
repo's `scripts/get-zig.sh` (the same source CI uses).

Both cross builds are **capability-lean**: OpenSSL, zlib, nghttp2 and YAML are
forced off, because the host's `pkg-config` would resolve the *host's* Linux
libraries and poison the target binary. Those std features ship as their
"unavailable" stubs, exactly as in any build without the libraries. Vendored
PCRE2 needs no host library, so `std.regex` survives.

One `build/` tree holds one target's objects and archives — they are not
interchangeable (ELF vs PE). The tree carries a `build/.build-target` stamp and
a mismatching build stops immediately with an actionable error rather than
failing deep in the link; `make clean` between targets is the fix (and `clean`
itself is never blocked by the guard).

**Testing a cross-built toolchain**: see `AE_TEST_RUNNER` in the next
section.

### Testing with a runner (`AE_TEST_RUNNER`)

`ae run` and `ae test` normally exec the binary they just built. Setting
`AE_TEST_RUNNER` makes them prefix it with a wrapper instead — the same idea as
cargo's `CARGO_TARGET_<triple>_RUNNER`:

```bash
# Run the cross-built Windows toolchain's output under Wine
AE_TEST_RUNNER=wine make test-ae WINDOWS=1 ZIG=/path/to/zig

# Any wrapper works — qemu-user, a tracer, a timing harness
AE_TEST_RUNNER="qemu-aarch64 -L /sysroot" ae test
```

Unset or empty means "exec directly", so it is inert by default. The value may
carry its own arguments. Because the hook sits at the *edge*, nothing in
`std.spec` or in your test sources needs to know it is running under an
emulator — the same `describe`/`it` blocks run either way, and the
`AE_SPEC_FORMAT`/`AE_SPEC_REPORT` structured-report contract is inherited
straight through the wrapper.

Layer an extra exclusion list onto a sweep with
`make test-ae AE_SWEEP_EXTRA_PRUNE=<file>` (applies to both the `.ae` and
`.sh` sweeps).

**Wine and the Windows cross lane.** CI's `windows-cross` job
(`.github/workflows/windows.yml`) currently cross-builds only — it does not
run the suite under Wine. That was attempted and deferred for a structural
reason worth knowing before trying again: `ae` is a *compile-and-run* driver,
so `ae run`/`ae test` inside a Wine prefix want a **Windows** C toolchain
there to compile the C the compiler emits (the driver tries to fetch
MinGW-w64). Driving the *native* `ae` with `AE_CC="zig cc -target
x86_64-windows-gnu"` avoids that, but then needs a Windows `libaether.a`
kept alongside the native one — and one `build/` tree holds exactly one
target's archives. `tests/ae_sweep_prune_wine.txt` records which areas such
a lane must never claim to cover (fs/path semantics, sockets/h2, actor
timing, the LD_PRELOAD sandbox).

### Docker-Based Cross-Compilation

For cross-compilation without installing toolchains locally:

```bash
make docker-ci-wasm        # Emscripten SDK → compile + run with Node.js
make docker-ci-embedded    # arm-none-eabi-gcc → syntax-check
make ci-portability        # All: native coop + WASM + embedded
```

### RISC-V 64-bit (`ci-riscv64`)

Cross-compile + run-under-qemu portability check:

```bash
# Install host toolchain (Ubuntu 22.04+):
sudo apt-get install -y gcc-riscv64-linux-gnu \
    libc6-dev-riscv64-cross qemu-user-static

# Cross-compile compiler/ae/stdlib for riscv64; verify the binaries
# are riscv64 ELF; smoke-run them under qemu-user-static.
make ci-riscv64
```

Useful for catching pointer-width, struct-padding, and atomic-
instruction-availability bugs that an x86_64-only matrix can't
surface. Optional libs (OpenSSL, zlib, nghttp2, GTK4) are disabled
in the riscv64 build because the host runner's pkg-config returns
x86_64 lib paths, the std.* feature-detection wrappers fall into
their "unavailable" stubs cleanly.

Docker images: `docker/Dockerfile.wasm` (Emscripten), `docker/Dockerfile.embedded` (ARM Cortex-M4).

### `ae build` with an alternate or cross C compiler (`$CC` / `$AE_CC`)

`ae build`, `ae run`, and `ae build --emit=lib` select their C-backend
compiler the way the Makefile does: they honor `$AE_CC` first, then `$CC`,
falling back to `gcc` (the WinLibs-bundled gcc on Windows) when neither is
set. This affects only the C backend that turns Aether's generated C into the
final binary; it never changes `aetherc`, the Aether-to-C front end.

The same-OS, cross-arch cell is then a one-liner. On an x86_64 Linux host with
a cross-gcc installed:

```bash
CC=aarch64-linux-gnu-gcc ae build --emit=lib core/embed.ae -o libfoo.so
file libfoo.so          # => ELF 64-bit LSB shared object, ARM aarch64
qemu-aarch64 ./probe    # load/run the emitted lib under an emulator
```

That produces an arm64 `.so` on a cheap x86_64 runner with no arm64 hardware
and no new codegen. A compiler that cannot be found fails fast with
`C compiler '<name>' (from $CC) not found` rather than a later link error.
Cross-**OS** targets (for example Linux to macOS) need a matching C toolchain
plus that OS's headers, which a stock cross-gcc does not carry: for those, use
the `zig cc` backend below.

### `ae build --target=<triple>` (cross-OS via a `zig cc` backend)

`ae build --target=<triple>` cross-compiles a foreign-OS/arch binary using
[zig](https://ziglang.org) as a self-contained cross toolchain. zig bundles
each target's libc, system headers, and linker, so the Aether runtime and
standard library compile straight from source for the target: no cross-gcc, no
target sysroot, no per-host file juggling. The platform backend (`epoll` vs
`kqueue`, `spawn_sandboxed_linux` vs the BSD/stub path) is selected by the
`__linux__` / `__APPLE__` macros zig predefines for the target, so one source
set serves every target.

```bash
ae build --target=x86_64-linux  hello.ae -o hello      # ELF x86-64
ae build --target=aarch64-macos hello.ae -o hello      # Mach-O arm64
```

Supported triples: `aarch64-macos`, `x86_64-macos`, `aarch64-linux`,
`x86_64-linux` (the `arm64-`/`amd64-` spellings are accepted too). `zig` must
be on `PATH` (`brew install zig`, or <https://ziglang.org/download/>); the
build fails fast with an install hint otherwise.

**How it links.** The full runtime and standard library are compiled from
source for the target and archived, then the program links against that
archive, so the linker pulls only the objects it references, exactly as a
native `-laether` link against the complete `libaether.a` does. The first build
recompiles the runtime from source (a few seconds); caching the per-target
archive is a planned optimization.

**Scope.** Cross binaries are built without OpenSSL / zlib / nghttp2
(zig ships none of those), so standard-library features that need them
(HTTPS/TLS, hashing, base64, compression, HTTP/2) report errors at
runtime, exactly like a native build on a host that lacks those libraries;
plain sockets and pure helpers keep working. `ae build` prints a note when a
program uses such a module (`std.http`, `std.net`, `std.cryptography`,
`std.zlib`, `std.encoding`), then builds it anyway. Cross-building
those libraries is the documented follow-up; staging them via
`CROSSBUILD_SYSROOT` (provisioned by aether-crossbuild) links them for real
today. **`std.regex` is the exception: it always works on cross builds, with
no sysroot** — its engine is vendored in-tree (`std/regex/pcre2/`, pinned
upstream PCRE2 compiled by `std/regex/aether_pcre2_vendored.c`; #1389), so a
regex-using program cross-builds self-contained for every target. A
CROSSBUILD_SYSROOT that stages a real libpcre2-8 takes precedence over the
vendored copy. Executables only
(`--emit=lib`/`--emit=both` are rejected for now), and the host must be POSIX
(Linux/macOS). The generated binary targets another platform, so it is not
runnable on the build host; copy it to a matching machine (or an emulator).

The same vendored engine backs two native cases: a build box with no system
libpcre2-8 (`make` compiles the vendored copy instead of stubbing
`std.regex` out — `PCRE2=0` restores the stub, `PCRE2=vendored` forces the
vendored engine even when a system library exists), and `ae`'s
no-`libaether.a` source fallback, which always compiles the vendored engine
so an installed toolchain never ships a silently-stubbed regex.

## Build Recommendations

| Use Case | Flags | Notes |
|----------|-------|-------|
| Development | `-O0 -g` | Fast iteration, debug symbols |
| Testing/CI | `-O2` | Balanced optimization |
| Release | `-O3 -march=native -flto` | Full optimization |
| Profiling | PGO pipeline | Based on representative workload |
| Hardened | `HARDEN=1` | See "Hardening" section below |
| WASM | `PLATFORM=wasm` | Cooperative scheduler, Emscripten |
| Embedded | `PLATFORM=embedded` | Cooperative scheduler, no OS |
| Cross-OS/arch | `ae build --target=<triple>` | `zig cc` backend, POSIX host, executables |

## Hardening (`HARDEN=1`)

Opt-in hardening flags add stack canaries, fortified libc-call wrappers, and format-string-injection diagnostics. Enabled with the `HARDEN=1` environment variable; disabled by default in release builds because the runtime overhead is non-zero (~3-5% on micro-benchmarks) and macOS Clang has historically been finicky with `_FORTIFY_SOURCE` on a few setups.

```bash
# Local hardened build, recommended before submitting a PR that
# touches C in compiler/, runtime/, or std/.
HARDEN=1 make compiler ae stdlib

# Hardened CI: run the full suite end-to-end under hardening.
HARDEN=1 make ci
```

Flags enabled:

| Flag | Purpose |
|------|---------|
| `-fstack-protector-all` | Stack canaries on every function (not just gcc-strong heuristic candidates), catches the smashing class of bugs that escape the default heuristic. |
| `-D_FORTIFY_SOURCE=2` | Runtime checks on `read`/`write`/`memcpy`/`strncpy`/`printf`-family calls. Linux `gcc` also emits compile-time warnings when it can prove a buffer overflow, those should be fixed at the source, never blanket-suppressed. Requires at least `-O1`; the default `-O2` satisfies that. |
| `-Wformat -Wformat-security` | Diagnose `printf`-family format strings sourced from non-literals (the `%s`-format-injection class). Default in modern Linux distros; we standardise on it explicitly. |

The flags are added to `CFLAGS` only when `HARDEN=1` is set; the default build path is byte-identical to the unhardened release. The Linux/Hardened (gcc) CI matrix entry pins this so a regression that introduces an unchecked `memcpy`-over-fixed-buffer trips a red check before merge.

## LLM diagnostics (`AETHER_ENABLE_LLM=1`)

`ae help <script> --llm <weights.gguf>` is an opt-in offline local-LLM escalation
path for the [config-IS-code diagnostic flow](cic-help.md). Default
builds omit the LLM module entirely so the binary stays small and the link line
stays dependency-light; embedders enable it explicitly at build time.

```bash
# Default build, `ae help --llm <path>` returns a clear "rebuild with
# AETHER_ENABLE_LLM=1" message; no llama.cpp dependency.
make ae

# LLM-enabled build, requires llama.cpp built and linkable. The shim
# in `tools/llm_shim.c` targets the stable `llama.h` C API.
make ae AETHER_ENABLE_LLM=1 \
    LLM_LDFLAGS="-L/path/to/llama.cpp/build -lllama -lggml -lstdc++ -lm" \
    LLM_CFLAGS="-I/path/to/llama.cpp"
```

Hard privacy invariants (enforced by code structure + a Linux CI `strace -e network` guard):

- **No network calls.** The shim opens exactly the weights file the user names.
  No download, no fetch, no telemetry, no "anonymised usage stats." Same offline
  as online.
- **No bundled weights.** We ship no `.gguf`. You bring your own (3-7B range
  works on a laptop CPU; larger models also fine but slower).
- **No model marketplace integration.** No `ae help --download-model`. Path
  argument only; a missing path is a clean error, not a "would you like to fetch
  it?" prompt.
- **Stripped builds (default) omit the entire module.** A binary built without
  `AETHER_ENABLE_LLM=1` cannot run inference even if someone passes `--llm
  <path>`, they get the documented "rebuild with the flag" message instead.

The shim's surface is a single C function (`int ae_llm_run(const char* weights_path,
const char* prompt, FILE* out)`); upstream API rotations in llama.cpp need
touching only that one TU. See [`docs/cic-help.md`](cic-help.md) for the full
design.

## References

- GCC Optimization Options: https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html
- LLVM PGO Guide: https://llvm.org/docs/HowToBuildWithPGO.html
- llama.cpp (C API used by the LLM shim): https://github.com/ggerganov/llama.cpp
