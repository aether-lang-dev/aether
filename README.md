# Aether Programming Language

[![CI](https://github.com/aether-lang-org/aether/actions/workflows/ci.yml/badge.svg)](https://github.com/aether-lang-org/aether/actions/workflows/ci.yml)
[![Windows](https://github.com/aether-lang-org/aether/actions/workflows/windows.yml/badge.svg)](https://github.com/aether-lang-org/aether/actions/workflows/windows.yml)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS%20%7C%20WASM%20%7C%20Embedded-lightgrey)]()

A compiled, actor-based programming language with type inference, built for concurrent systems. Aether compiles to readable C, so it runs at native speed and links against existing C libraries directly.

## Overview

Aether brings actor-based concurrency to systems programming. The compiler emits C, not bytecode, not a VM, which keeps the runtime dependency-free, portable anywhere a C toolchain reaches, and callable from (and into) existing C code.

**Where it sits on the OO ↔ FP spectrum:** structs are plain data, behaviour is free functions, closures + trailing blocks are first-class, Aether leans **closer to functional than OO**, sitting near Go and Rust in the hybrid middle of the paradigm spectrum. There are no classes, no inheritance, no method dispatch; the one piece of OO machinery present is the actor (stateful, encapsulated behind a message boundary, no polymorphism). See [Language Reference § Paradigm placement](docs/language-reference.md#paradigm-placement).

**Core Features:**
- **Actor-based concurrency**, automatic multi-core scheduling, lock-free message passing, and adaptive batching/migration. See [Actor Concurrency](docs/actor-concurrency.md).
- **Type inference** with optional annotations, plus Go-style multi-value returns, `a, err = func()` with `_` discard.
- **Compiles to readable C**, native performance and direct C-library interop, no FFI glue. See [Architecture](docs/architecture.md).
- **Three-layer capability sandbox**, `--emit=lib` gates the stdlib at compile time, `hide`/`seal except` gate lexical scopes, and an `LD_PRELOAD` shim gates libc. See [Containment Sandbox](docs/containment-sandbox.md).
- **Embeds in (and hosts) other languages**, `--emit=lib` emits a `.so` + typed SDK and a `.rodata` symbol catalog read by `ae lib-info`; the same grant list runs Lua / Python / Perl / Ruby / Tcl / JS in-process. See [Embedding & emit=lib](docs/emit-lib.md).
- **Per-guest resource caps**, embedders set a process memory ceiling and per-thread wall-clock deadline; the codegen tripwires every loop head under `--emit=lib` (zero overhead on `--emit=exe`).
- **Production HTTP server** in the stdlib, TLS, HTTP/1.1 keep-alive, HTTP/2 (h2 + h2c), WebSocket, Server-Sent Events, zero-copy `sendfile(2)`, and a stack of composable middleware. See [HTTP Server](docs/http-server.md).
- **nginx-class reverse proxy** in the stdlib, five load-balancing algorithms, active health checks, an LRU response cache, per-upstream circuit breaker, idempotent retries, and rate limiting. See [Reverse Proxy](docs/http-reverse-proxy.md).
- **Automatic string-ownership tracking**, heap strings are freed on reassignment and at scope exit with no manual `defer`; `@retain` marks parameters that store the pointer. See [String memory model](docs/memory-management.md#string-memory-model-heap-string-tracker).
- **Runtime contracts**, `requires` / `ensures` pre- and postconditions on any function; `--no-contracts` compiles them out at zero per-call cost. See [Function contracts](docs/language-reference.md#function-contracts-requires--ensures-issue-348).
- **`@derive(eq)` on structs**, synthesize a field-by-field equality helper automatically.
- **PATH-style module search**, `--lib ./lib:./vendor` layers project, vendored, and shared module roots left-to-right; `ae lib-path` shows the resolved chain. See [Module System](docs/module-system-design.md).
- **Package management**, `ae add host/user/repo[@version]` from GitHub, GitLab, Bitbucket, or any git host.
- **Offline config-script diagnostics**, `ae help <script>` suggests fixes for closure-DSL config errors with no network access and no code execution. See [Config-IS-Code Diagnostics](docs/cic-help.md).

## Runtime Features

The Aether runtime implements a native actor system with optimized message passing:

### Concurrency Model
- **Multi-core partitioned scheduler** with locality-aware actor placement
- **Locality-aware spawning**, actors placed on the caller's core for efficient parent-child messaging
- **Message-driven migration**, communicating actors automatically converge onto the same core
- **Work-stealing fallback** for idle core balancing
- **Lock-free SPSC queues** for same-core messaging
- **Cross-core messaging** with lock-free mailboxes

### Memory Management
- **Manual by default**, use `defer` for cleanup. All allocations cleaned up explicitly.
- **Automatic string-ownership tracking**, heap-allocated strings auto-freed on reassignment, tuple destructure, and function exit; opt-in `@retain` extern annotation for parameters that store the pointer beyond the call. See [String memory model](docs/memory-management.md#string-memory-model-heap-string-tracker).
- **Arena allocators** for actor lifetimes
- **Memory pools** with thread-local allocation
- **Actor pooling** reducing allocation overhead
- **Zero-copy message delivery** in single-actor main-thread mode (caller stack passed directly)

### Message Optimization
- **Sender-side batching** for reduced overhead
- **Message coalescing** for higher throughput
- **Adaptive batching** dynamically adjusts batch sizes
- **Direct send** for same-core actors bypasses queues

### Capabilities & Sandboxing

Aether is compiled, but comes with a capability system normally associated with interpreted / VM-hosted languages. Three enforcement layers:

- **Compile-time module gate**, `--emit=lib` rejects `std.fs` / `std.net` / `std.os` imports by default; the host opts each in with `--with=fs,net,os`.
- **Compile-time scope gate**, `hide <names>` and `seal except <allowlist>` on any lexical block (closure, trailing-block DSL, actor handler) block ambient names from leaking into contained code.
- **Runtime process gate**, `libaether_sandbox.so` (LD_PRELOAD) intercepts libc (`open*`, `connect`/`bind`, `execve`, `mmap`, `dlopen`, `getenv`) against a builder-DSL grant list; inherited across `execve` to child processes.

The same grant list + LD_PRELOAD also contains embedded interpreters, an Aether `main()` can host Lua, Python, Perl, Ruby, Tcl, and JavaScript in-process (`contrib.host.<lang>.run_sandboxed(perms, code)`) with the same permission model that scopes Aether's own libc calls. In the reverse direction, `--emit=lib` + `ae build --namespace` produce a `.so` plus a typed SDK (Python ctypes, Java Panama, Ruby Fiddle) so host-language apps can embed Aether without writing FFI by hand.

Mashup of Pony object capabilities, Java's removed SecurityManager, and a fraction of gVisor, see [Containment Sandbox](docs/containment-sandbox.md) for the full comparison, threat model, and known bypass surface.

### Platform Portability
- **Compile-time platform detection** via `AETHER_HAS_*` flags (threads, atomics, filesystem, networking, NUMA, SIMD, affinity)
- **Cooperative scheduler** for single-threaded platforms (WebAssembly, embedded, bare-metal)
- **Graceful degradation**, stdlib stubs return errors when features are unavailable
- **`ae build --target wasm`** compiles to WebAssembly via Emscripten
- **`PLATFORM=wasm|embedded`** Makefile targets for cross-compilation
- **Docker CI images** for Emscripten (WASM) and ARM (embedded) verification

### Advanced Features
- **Actor timeouts**, `receive { ... } after N -> { ... }` fires handler if no message arrives within N ms
- **Cooperative preemption** (opt-in), `AETHER_PREEMPT=1` breaks long handlers, `--preempt` yields at loop back-edges
- **Reactor-pattern async I/O**, `net.await_io(fd)` suspends an actor on a file descriptor without blocking any scheduler thread; the runtime's per-core I/O poller (epoll/kqueue/poll) delivers an `IoReady { fd, events }` message when the fd becomes readable
- **SIMD batch processing** with AVX2 support
- **NUMA-aware allocation** for multi-socket systems
- **CPU feature detection** for runtime optimization selection
- **Performance profiling** with per-core cycle counting
- **Message tracing** for debugging

### Benchmarks

Cross-language benchmark suite based on the [Savina Actor Benchmark Suite](https://dl.acm.org/doi/10.1145/2687357.2687368), 11 languages × 5 patterns (ping-pong, counting, thread ring, fork-join, skynet). Both the benchmark runner and the visualization server are written in Aether, dogfooding the stdlib.

```bash
make benchmark    # Builds runner, runs all 55 benchmarks, opens UI at http://localhost:8080
```

See [Performance Benchmarks](docs/performance-benchmarks.md) for methodology and [benchmarks/cross-language/](benchmarks/cross-language/) for source.

## Quick Start

### Install

**Linux / macOS / FreeBSD, prebuilt binary (no toolchain, no build):**

Every [release](https://github.com/aether-lang-org/aether/releases/latest) ships a ready-to-run tarball (`ae` + `aetherc` + stdlib) named `aether-<version>-<platform>.tar.gz`, where `<platform>` is one of `linux-x86_64`, `macos-arm64`, `macos-x86_64`, `freebsd-x86_64`. Grab the URL for your platform from the [latest release](https://github.com/aether-lang-org/aether/releases/latest), then extract and add its `bin/` to your `PATH`:

```bash
# Example (substitute the current version + your platform from the Releases page):
VER=0.458.0; PLATFORM=linux-x86_64
curl -fsSL "https://github.com/aether-lang-org/aether/releases/download/v${VER}/aether-${VER}-${PLATFORM}.tar.gz" | tar xz -C ~/.local
export PATH="$HOME/.local/bin:$PATH"     # add to ~/.bashrc / ~/.zshrc to persist
ae version
```

**Linux / macOS, build from source (remote one-liner, no clone):**

```bash
curl -sSL https://raw.githubusercontent.com/aether-lang-org/aether/main/get.sh | sh
```

Fetches a pinned source tarball, builds the toolchain (Aether compiles to C, so the only prerequisites are a C compiler + GNU make, no tests run), and installs to `~/.local` (sudo-free). Pin a version with `AETHER_REF=<tag>` (see the [releases page](https://github.com/aether-lang-org/aether/releases) for tags), or change the prefix with `PREFIX=/usr/local` (system-wide; needs sudo). Add `~/.local/bin` to your `PATH` if it isn't already.

**Linux / macOS, full clone install** (editor extension, `ae version` management, shell-PATH setup, `~/.aether` layout):

```bash
git clone https://github.com/aether-lang-org/aether.git
cd aether
./install.sh
```

Installs to `~/.aether` and adds `ae` to your PATH. Restart your terminal or run `source ~/.bashrc`, `~/.zshrc`, or `~/.bash_profile`.

**Windows, download and run:**

1. Download `aether-*-windows-x86_64.zip` from [Releases](https://github.com/aether-lang-org/aether/releases)
2. Extract to any folder (e.g. `C:\aether`)
3. Add `C:\aether\bin` to your PATH
4. **Restart your terminal** (so PATH takes effect)
5. Run `ae init hello && cd hello && ae run`

GCC is downloaded automatically the first time you run a program (~80 MB, one-time), no MSYS2 or manual toolchain setup required.

**All platforms, install, upgrade, and switch versions:**

```bash
ae version list              # see all available releases (newest first)
ae upgrade                   # install the latest release and switch to it
ae install                   # install the latest release (or `ae install <tag>` for a specific one)
ae use <tag>                 # switch to an already-installed version
```

Run `ae version list` (or check the [latest release](https://github.com/aether-lang-org/aether/releases/latest)) to find the current tag; there's no need to hard-code one.

(The longer `ae version install <v>` / `ae version use <v>` forms still
work and are equivalent to `ae install` / `ae use`.)

### Your First Program

```bash
# Create a new project
ae init hello
cd hello
ae run
```

Or run a single file directly:

```bash
ae run examples/basics/hello.ae
```

### Editor Setup (Optional)

Install syntax highlighting for a better coding experience:

**VS Code / Cursor:**
```bash
cd editor/vscode
./install.sh
```

This provides:
- Syntax highlighting with TextMate grammar
- Custom "Aether Erlang" dark theme
- `.ae` file icons

### Development Build (without installing)

If you prefer to build without installing:

```bash
make ae
./build/ae version
./build/ae run examples/basics/hello.ae
```

### The `ae` Command

`ae` is the single entry point for everything, like `go` or `cargo`:

```bash
ae init <name>           # Create a new project
ae run [file.ae]         # Compile and run (file or project)
ae build [file.ae]       # Compile to executable
ae check [file.ae]       # Type-check without compiling (skips codegen + link)
ae fmt [--check] [path]  # Format source (stdin, or .ae files/dirs in place)
ae test [file|dir]       # Discover and run tests
ae examples [dir]        # Build all example programs
ae add <host/user/repo>  # Add a dependency (any git host)
ae repl                  # Start interactive REPL
ae cache                 # Show build cache info
ae cache clear           # Clear the build cache
ae version               # Show current version
ae upgrade               # Install the latest release and switch to it
ae install <v>           # Install a specific release (latest if omitted)
ae use <v>               # Switch to an installed version
ae version list          # List all available releases
ae help                  # Show all commands
```

In a project directory (with `aether.toml`), `ae run` and `ae build` compile `src/main.ae` as the program entry point. You can also pass `.` as the directory: `ae run .` or `ae build .`.

**Using Make (alternative):**

```bash
make compiler                    # Build compiler only
make ae                          # Build ae CLI tool
make test                        # Run runtime C test suite
make test-ae                     # Run .ae source tests
make test-all                    # Run all tests
make examples                    # Build all examples
make -j8                         # Parallel build
make help                        # Show all targets
```

### Building on Windows

The Aether build is GNU-make based. Use one of the two paths below, `nmake` from a Visual Studio Developer Prompt **will not work** (the Makefile uses GNU-only syntax that NMAKE can't parse).

**Just running Aether? Skip this section** and use the [release binary](https://github.com/aether-lang-org/aether/releases), no MSYS2 setup required.

**Building from source, recommended (MSYS2 / MinGW-w64):**

1. Install [MSYS2](https://www.msys2.org/) and open the **MSYS2 MinGW 64-bit** shell (not the bare MSYS shell).
2. Install the toolchain:
   ```bash
   pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make \
             mingw-w64-x86_64-openssl mingw-w64-x86_64-zlib \
             mingw-w64-x86_64-ca-certificates pkg-config make bc
   ```
3. Clone and build:
   ```bash
   git clone https://github.com/aether-lang-org/aether.git
   cd aether
   make ci   # full suite: compiler, ae, stdlib, REPL, C tests, .ae tests, examples
   ```

For HTTPS to verify certs, the `mingw-w64-x86_64-ca-certificates` package above provides the bundle at `/mingw64/etc/ssl/certs/ca-bundle.crt`. The runtime auto-detects it; if your install is in a non-standard location, export `SSL_CERT_FILE` to the bundle's Windows path.

**Native MSVC (cl.exe / nmake):** not currently supported as a full build path, tracker [#99](https://github.com/aether-lang-org/aether/issues/99). The MSVC matrix job in CI verifies our public headers parse under `cl.exe` so a future native MSVC port stays feasible, but `make` (the build system itself) requires GNU make. The MSYS2 MinGW build above is the supported source-build path for Windows today.

## Project Structure

```
aether/
├── compiler/           # Aether compiler (lexer, parser, codegen)
│   ├── parser/        # Lexer, parser, tokens
│   ├── analysis/      # Type checker, type inference
│   ├── codegen/       # C code generation, optimizer
│   └── aetherc.c      # Compiler entry point
├── runtime/           # Runtime system
│   ├── actors/        # Actor implementation and lock-free mailboxes
│   ├── config/        # Platform detection, optimization tiers, runtime config
│   ├── memory/        # Arena allocators, memory pools, batch allocation
│   ├── scheduler/     # Multi-core scheduler + cooperative single-threaded backend
│   └── utils/         # CPU detection, SIMD, tracing, thread portability
├── std/                # Standard library
│   ├── string/         # String operations
│   ├── file/           # File operations (open, read, write, delete)
│   ├── dir/            # Directory operations (create, delete, list)
│   ├── path/           # Path utilities (join, basename, dirname)
│   ├── fs/             # Combined file/dir/path module
│   ├── collections/    # List, HashMap, Vector, Set, PQueue
│   ├── list/           # Dynamic array (ArrayList)
│   ├── map/            # Hash map
│   ├── intarr/         # Fixed-size packed int buffer
│   ├── json/           # JSON parser and builder
│   ├── http/           # HTTP client + server (TLS, keep-alive, h2, WS, SSE, metrics)
│   │   ├── client/         # Builder client (request builder, full response, JSON sugar)
│   │   ├── middleware/     # CORS, basic/bearer/session auth, rate-limit, real-IP, vhost, gzip, static, rewrite, error pages
│   │   ├── proxy/          # Reverse proxy: upstream pool, LB (RR/LC/iphash/WRR), health, cache, circuit breaker
│   │   └── server/h2/      # HTTP/2 framing via libnghttp2 (h2 + h2c + ALPN + GOAWAY + concurrent dispatch)
│   ├── tcp/            # TCP client and server
│   ├── net/            # Combined TCP/HTTP networking module
│   ├── cryptography/   # Hash family (SHA-2/3, BLAKE2, RIPEMD, Whirlpool, Tiger, Skein, SM3), HMAC, HKDF/PBKDF2/scrypt/Argon2, DRBG
│   ├── zlib/           # One-shot deflate/inflate
│   ├── math/           # Math functions and random numbers
│   ├── io/             # Console I/O, environment variables
│   ├── os/             # Shell execution, command capture, env vars, ISO-8601 time
│   └── log/            # Structured logging
├── contrib/            # Optional / opinionated modules outside std/
│   ├── cryptography/   # Extra crypto beyond std.cryptography
│   ├── parsers/        # xml_expat (SAX XML via libexpat)
│   ├── sqlite/         # SQLite bindings (open, prepare, bind, step, column, ...)
│   ├── templating/     # liquid (Shopify-Liquid port) + native emitter DSL
│   ├── tinyweb/        # Server-side request/response DSL
│   └── host/<lang>/    # Embed js, lua, perl, python, ruby, tcl, duktape,
│                       #   tinygo, go, java, factor, racket, rhombus in-process
├── tools/              # Developer tools
│   ├── ae.c            # Unified CLI tool (ae command)
│   └── apkg/           # Project tooling, TOML parser
├── tests/              # Test suite (runtime, syntax, integration, regression)
├── examples/           # Example programs (.ae files)
│   ├── basics/         # Hello world, variables, arrays, etc.
│   ├── actors/         # Actor patterns (ping-pong, pipeline, etc.)
│   └── applications/   # Complete applications
├── docs/               # Documentation
└── docker/             # Docker (CI, dev, WASM, embedded)
```

## Language Example

```aether
// Counter actor with message handling
message Increment {}
message Decrement {}
message Reset {}

actor Counter {
    state count = 0

    receive {
        Increment() -> {
            count = count + 1
        }
        Decrement() -> {
            count = count - 1
        }
        Reset() -> {
            count = 0
        }
    }
}

main() {
    // Spawn counter actor
    counter = spawn(Counter())

    // Send messages
    counter ! Increment {}
    counter ! Increment {}
    counter ! Decrement {}
    counter ! Reset {}
    counter ! Increment {}

    // Wait for all messages to be processed
    wait_for_idle()

    println("Final count: ${counter.count}")
}
```

## Closures and Builder DSL

Aether closures take three shapes after a function call. They look similar but have different semantics, picking the right one is the language's main lever for separating DSL structure from runtime behaviour.

| Mode | Syntax | Semantics |
|------|--------|-----------|
| **Immediate** | `func() { block }` | Runs inline at the call site, used for DSL structure |
| **Closure** | `func() \|x\| { block }` | Real closure with explicit params, hoisted to a C function |
| **Callback** | `func() callback { block }` | Real closure that captures enclosing scope, no params needed |

```aether
// Immediate, declarative structure, runs during construction
panel("Settings") {
    button("OK")
    button("Cancel")
}

// Closure, explicit params, deferred invocation
apply_twice(x: int, f: fn) { return call(f, call(f, x)) }
doubler = |x: int| -> x * 2
println(apply_twice(3, doubler))    // 12

// Callback, captures from scope, runs when invoked
counter = ref(0)
btn("increment") callback { ref_set(counter, ref_get(counter) + 1) }
btn("decrement") callback { ref_set(counter, ref_get(counter) - 1) }
```

The compiler distinguishes them at parse time, which is what makes the sandboxing story (above) work: `hide`/`seal except` checks happen against the hoisted form of `closure` and `callback` blocks, so a `seal except req, res` on a callback body genuinely prevents the body from reaching outer scope. Immediate blocks inherit the caller's lexical scope by design, they're structure, not callbacks.

Inspired by Smalltalk blocks, Ruby's blocks/procs, Groovy closures, and Kotlin/SwiftUI's trailing-block DSLs. See [Closures and Builder DSL](docs/closures-and-builder-dsl.md) for the builder-context mechanism, ref cells, and full DSL pattern; see [Closure lineage and runtime tradeoffs](docs/design/closure-lineage-and-runtime-tradeoffs.md) for why Aether keeps closure-shaped values without adopting a Lisp/Smalltalk runtime.

## Config IS Code

**Don't ship a YAML loader.** If your Aether library has a "start the thing" surface, HTTP server, daemon, agent, scheduler, test rig, expose it as a closure-DSL block and let the operator's "config" be a `.ae` file they run with `ae run`. The pattern collapses YAML → templating → second-language-DSL (HCL, Helm) → embedded-scripting all into one thing: real Aether, type-checked, sandboxable, with the full stdlib available when the operator needs it.

```aether
import avnserver

main() {
    avnserver.serve {
        host("127.0.0.1")
        port(9990)
        superuser_token(env("SUPER_TOKEN"))   // computed at config time
        repo("alpha", "/srv/alpha")
        repo("beta",  "/srv/beta")
    }
}
```

Same file is config, validation, conditional logic, and the entry point. No second parser, no second type system, no template language. Sandboxing (`--emit=lib --with=...`, `hide`, `seal except`) keeps it safe to accept untrusted configs as the embedded-DSL case demands. See [Config IS Code](docs/config-is-code.md) for the full progression (YAML → HCL → Pulumi → here) and library-author recipe.

## Runtime Configuration

When embedding the Aether runtime in a C application, configure optimizations at startup:

```c
#include "runtime/aether_runtime.h"

int main() {
    // Auto-detect CPU features and enable optimizations
    aether_runtime_init(4, AETHER_FLAG_AUTO_DETECT);

    // Or manually configure
    aether_runtime_init(4,
        AETHER_FLAG_LOCKFREE_MAILBOX |
        AETHER_FLAG_ENABLE_SIMD |
        AETHER_FLAG_ENABLE_MWAIT
    );

    // Your actor system runs here

    return 0;
}
```

Available flags:
- `AETHER_FLAG_AUTO_DETECT` - Detect CPU features and enable optimizations
- `AETHER_FLAG_LOCKFREE_MAILBOX` - Use lock-free SPSC mailboxes
- `AETHER_FLAG_ENABLE_SIMD` - AVX2 vectorization for batch operations
- `AETHER_FLAG_ENABLE_MWAIT` - MWAIT-based idle (x86 only)
- `AETHER_FLAG_VERBOSE` - Print runtime configuration

## Optimization Tiers

The runtime employs a tiered optimization strategy:

**TIER 0 - Platform Capabilities (compile-time):**
- `AETHER_HAS_THREADS` pthreads/Win32 threads (auto-detected; disabled on WASM/embedded)
- `AETHER_HAS_ATOMICS` C11 stdatomic (fallback: volatile for single-threaded)
- `AETHER_HAS_FILESYSTEM` / `AETHER_HAS_NETWORKING` stdlib feature gates
- `AETHER_HAS_SIMD` / `AETHER_HAS_NUMA` / `AETHER_HAS_AFFINITY` hardware feature gates
- Override any flag with `-DAETHER_NO_<FEATURE>` (e.g. `-DAETHER_NO_THREADING`)

**TIER 1 - Always Enabled:**
- Actor pooling (reduces allocation overhead)
- Direct send for same-core actors (bypasses queues)
- Adaptive batching (adjusts batch size dynamically)
- Message coalescing (combines small messages)
- Thread-local message pools

**TIER 2 - Auto-Detected:**
- SIMD batch processing (requires AVX2/NEON)
- MWAIT idle (requires x86 MONITOR/MWAIT)
- CPU core pinning (OS-dependent)

**TIER 3 - Opt-In:**
- Lock-free mailbox (better under contention)
- Message deduplication (prevents duplicate processing)

## Documentation

- [Getting Started Guide](docs/getting-started.md) - Installation and first steps
- [Bootstrapping from source (HEAD)](docs/bootstrap-from-source.md) - Build + install the toolchain and contrib straight from this repo (for consumers tracking HEAD or pre-package); humans and LLMs
- [Language Tutorial](docs/tutorial.md) - Learn Aether syntax and concepts
- [Language Reference](docs/language-reference.md) - Complete language specification
- [Standard Library Reference](docs/stdlib-reference.md) - Full stdlib surface
- [HTTP Server](docs/http-server.md) - TLS, HTTP/2, middleware, health probes, metrics, graceful shutdown
- [Reverse Proxy](docs/http-reverse-proxy.md) - `std.http.proxy` upstream pool, load balancing, health, cache, circuit breaker
- [HTTP Record/Replay (VCR)](docs/http-vcr.md) - moved to the [`servirtium-vcr`](https://github.com/aether-lang-org/servirtium-vcr) monorepo; no longer in the Aether stdlib
- [Install Layout](docs/install-layout.md) - What ships in `~/.aether`, MANIFEST format, downstream-link contract
- [C constant import](docs/bindgen-consts.md) - `ae bindgen consts`, C macro constants as Aether consts
- [Module System](docs/module-system-design.md) - `import`/`exports`, PATH-style `--lib` search chain, selective imports, package layout
- [Config-IS-Code Diagnostics (`ae help`)](docs/cic-help.md) - Offline heuristic diagnostics for closure-DSL config scripts (Levenshtein, YAML→call form, missing-import suggestions, `--fix`, `--json`, optional `--llm`)
- [C Interoperability](docs/c-interop.md) - Using C libraries and the `extern` keyword
- [Architecture Overview](docs/architecture.md) - Runtime and compiler design
- **Design & rationale** (why Aether is built the way it is): [closure model](docs/design/closure-lineage-and-runtime-tradeoffs.md), [parse, don't validate](docs/design/parse-dont-validate-review.md), [Aether through Chlipala's lens](docs/design/chlipala-lens.md), [DSL as a rules engine](docs/design/aether-dsl-as-a-rules-engine.md), and concurrency patterns ([sharded actor map](docs/design/sharded-actor-map.md), [snapshot cell](docs/design/snapshot-cell.md), [cache benchmark](docs/design/concurrent-cache-benchmark.md))
- [Language comparisons](docs/cross-references/) (design history): surveys of Fir, Flint, Zym, and GoogleCloudPlatform/Aether
- [Memory Management](docs/memory-management.md) - defer-first manual model, arena allocators
- [Structured Concurrency](docs/structured-concurrency.md) - Proposal: supervision trees + capability-scoped spawn/send (not yet shipped)
- [Runtime Optimizations](docs/runtime-optimizations.md) - Performance techniques
- [Cross-Language Benchmarks](benchmarks/cross-language/README.md) - Comparative performance analysis
- [Docker Setup](docker/README.md) - Container development environment

## Development

### Running Tests

```bash
# Full CI suite (9 steps, -Werror), runs on your current platform
make ci

# Unit tests only (runtime C test suite)
make test

# Integration + regression .ae tests
make test-ae

# Everything (unit + .ae)
make test-all

# Build all example programs
make examples

# Full CI + Valgrind + ASan in Docker (Linux)
make docker-ci
```

### Cross-Platform Testing

**CI runs automatically on:** Linux (GCC + Clang), macOS (ARM64 + x86_64), Windows (MinGW/MSYS2)

```bash
# Cooperative scheduler (no Docker needed)
make ci-coop

# Windows cross-compile syntax check (requires mingw-w64 or Docker)
make ci-windows              # needs: brew install mingw-w64
make docker-ci-windows       # or use Docker

# WebAssembly (requires Docker with Emscripten)
make docker-ci-wasm

# ARM embedded syntax check (requires Docker with arm-none-eabi-gcc)
make docker-ci-embedded

# All portability checks (coop + WASM + embedded)
make ci-portability
```

**`make ci` tests your current OS only.** No OS can locally test another OS natively, macOS cannot be virtualized on Linux/Windows, Windows build+run requires MSYS2. GitHub Actions CI automatically tests all 5 platform targets (Linux GCC, Linux Clang, macOS ARM64, macOS x86_64, Windows MinGW) on every PR. Docker targets (`docker-ci-windows`, `docker-ci-wasm`, `docker-ci-embedded`) provide cross-compilation syntax checking from any host.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the full pre-PR checklist.

### Running Benchmarks

```bash
# Run cross-language benchmark suite with interactive UI
make benchmark
# Open http://localhost:8080 to view results

```

The benchmark runner is written in Aether (`run_benchmarks.ae`), dogfooding the stdlib. It compiles and runs all 11 languages, parses output, and writes JSON results.

## Sibling Projects

The Aether ecosystem includes downstream consumers that live in their
own repos and release independently:

- **[aether-ui](https://github.com/aether-lang-org/aether-ui)**,
  Cross-platform widget toolkit (GTK4 on Linux, AppKit on macOS, Win32
  on Windows) with an AetherUIDriver HTTP test server for headless
  integration testing. Previously shipped as `contrib/aether_ui/` in
  this repo; spun out so it can iterate on its own cadence.
- **[aeb](https://github.com/aether-lang-org/aeb)**, Build system for
  multi-package Aether projects. Reads `share/aether/MANIFEST` (the
  authoritative list of link-suitable runtime/stdlib `.c` files) and
  dispatches per-package builds with cache reuse and incremental
  relinking.

If you're adding to Aether and the change isn't a runtime / compiler /
stdlib concern, the right home may be one of the siblings above. Both
repos consume Aether the same way external users do, `import` against
the installed `share/aether/` tree plus `$(ae cflags)` for the link
line, so they're useful references for downstream integration shapes.

## Status

Aether is under active development. The compiler, runtime, and standard library are functional and tested.

## Contributing

Contributions are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

**Areas of interest:**
- Runtime optimizations
- Standard library expansion
- Documentation and examples

## Supporting Aether

Aether is free and open source, built and maintained in personal time. CI runners, cross-platform testing infrastructure, and future project hosting cost real money.

If Aether is useful to you, consider [sponsoring the project on GitHub](https://github.com/sponsors/nicolasmd87). Every contribution goes directly into development and infrastructure.

[![Sponsor](https://img.shields.io/badge/Sponsor-Aether-blue?logo=github-sponsors)](https://github.com/sponsors/nicolasmd87)

## Acknowledgments

Aether's main lineage is:

- **Erlang/OTP**, actor model, message passing, receive-pattern syntax, and
  "let it be isolated behind a mailbox" concurrency.
- **Go**, pragmatic tooling, simple syntax, explicit `(value, err)` returns,
  and stdlib surfaces that favor direct operational code over framework magic.
- **Rust**, systems-programming discipline, explicit capability boundaries,
  zero-cost lowering goals, and careful ownership/lifetime thinking without
  adopting a borrow checker.
- **Pony**, actor-oriented type-safety ideas and the object-capability framing
  behind sandboxed imports, lexical `hide` / `seal`, and explicit grants.
- **Smalltalk / Ruby / Groovy**, block and closure ergonomics: trailing-block
  builders, `do |x| ... end`-style readability, and DSL-shaped APIs where the
  closure is the configuration.

Smaller facets are deliberately borrowed or adapted from elsewhere:

- **Elixir / Erlang lists**, for `*StringSeq`: O(1) head/tail/cons/length,
  structural sharing, and pattern matching with `[h | t]`.
- **Odin**, for struct field injection via `using embed: Sub` while omitting
  Odin's broader `using` statement form.
- **Nim / Pony**, for nominal wrapper and move-only-style ideas in features
  such as distinct types and isolated actor-message payloads.
- **V**, for compact systems-language ergonomics and some C-adjacent syntax
  choices in declarations, modules, and direct standard-library APIs.
- **Ruby**, for heredoc dedent rules in the `<<MARKER ... MARKER` syntax.
- **Go's standard library**, for concrete API semantics such as
  `filepath.Clean` / `filepath.Rel`-style path helpers and proxy environment
  handling compatible with `HTTP_PROXY`, `HTTPS_PROXY`, and `NO_PROXY`.
- **Java, gVisor, WASI, Deno, and FreeBSD Capsicum**, as comparison points and
  partial prior art for sandboxing, syscall boundaries, and capability-style
  containment.
- **HCL, Helm, Pulumi, CDK, and YAML ecosystems**, as the configuration systems
  Aether's trailing-block "config is code" style is intentionally designed to
  replace for programmable server-shaped libraries.
- **Rust macros, jOOQ, and RSpec**, as reference points for what Aether's
  trailing-block DSLs can express directly without a macro or code-generation
  layer.

## License

MIT License. See [LICENSE](LICENSE) for details.
