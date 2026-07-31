# The Compiler Trust Boundary

## Background

Aether's sandboxing story (see [containment-sandbox.md](containment-sandbox.md),
[emit-lib.md](emit-lib.md), [hide-and-seal.md](hide-and-seal.md)) is about
*runtime* containment: a compiled Aether program, or a hosted interpreter, is
restricted in what it may touch. This document is about the layer beneath that:
**what the build toolchain itself, `aetherc`, `ae`, `aeb` is permitted to
touch, and why some conveniences are permanently off the table.**

A sandbox that only constrains the program-under-execution but trusts the
compiler unconditionally has a hole the size of the build step. The compiler
reads files, resolves imports, and (in `aeb`'s case) may orchestrate package
discovery. Every one of those is an opportunity for code or data of unknown
provenance to influence the build.

## The cautionary tale

In April 2026, lcamtuf published a satirical post demonstrating "remote,
on-demand includes" for C and C++:

```c
#include <https://lcamtuf.coredump.cx/leftpad.h>
```

The trick is an `LD_PRELOAD` shim that intercepts the C preprocessor's
`open()` and, when the path looks like a URL, fetches it over the network.
The post is a joke, the "feature" *is* the vulnerability. But it is a
precise threat model, and it states the failure mode cleanly:

1. **Transparent remote resolution.** `#include <https://...>` looks
   syntactically identical to a local include. Nothing at the call site
   signals that a network fetch happens.
2. **The compiler is an unsandboxed process.** `LD_PRELOAD` works because
   the preprocessor has ambient authority to call `open()`, `connect()`,
   and anything else libc exposes. Nobody constrains it.
3. **Provenance is invisible.** The resulting binary depends on whatever
   the remote server returned *at build time*, uncached, unpinned,
   unaudited, and different on the next build.

Aether already has the machinery to be the foil to all three. This document
records the posture so it does not erode.

## The posture

### 1. The compiler is itself a sandbox target

Aether's `libaether_sandbox.so` LD_PRELOAD gate (see
[containment-sandbox.md](containment-sandbox.md)) constrains a *sandboxed
Aether program's* libc calls against a grant list. The same idea applies
defensively to the toolchain.

`aetherc` and `ae` need a small, enumerable set of filesystem reads and
nothing else:

| Capability | Why the toolchain needs it | Wildcard? |
|------------|----------------------------|-----------|
| **Filesystem read** | `*.ae` sources under the invoked project root; the stdlib/contrib tree under `<prefix>/lib/aether` and `<prefix>/share/aether` | **No**, two fixed roots |
| **Filesystem write** | The build output directory and a temp dir for generated `.c` | **No**, output dir only |
| **Process execution** | The C compiler (`cc`/`gcc`/`clang`) and the linker | **No**, toolchain binaries only |
| **Network** | Never | **Never** |
| **Environment** | A short fixed list (`PATH`, `AETHER_*`, install-layout vars) | **No** |

The recommendation: a documented, opt-in `--sandboxed-build` mode (not yet
implemented) that runs `aetherc` under a grant list matching exactly the
table above. Anything
outside it, a network handle, a read outside the two roots, an `exec` of a
non-toolchain binary, is a hard build failure, not a warning. This turns
"could a malicious `import` or a hostile build script reach the network
during compilation" from a trust question into a compile-time-enforced one.

This is the inverse of the lcamtuf shim: instead of an `LD_PRELOAD` that
*adds* ambient authority to the compiler, Aether's `LD_PRELOAD` *removes* it.

### 2. Imports never resolve to anything fetched at build time

Aether's glob and selective imports (`import std.math (*)`,
`import std.math (sqrt, pow)` see [module-system-design.md](module-system-design.md)
and the language reference's "Glob Import" section) resolve to exactly three
kinds of target:

- **stdlib**, `<prefix>/lib/aether` / `<prefix>/share/aether`, shipped with the toolchain
- **contrib**, same tree, vendored
- **local modules**, `.ae` files under the project root

There is no fourth kind, and there must never be one that is fetched
transparently. An `import` is a pointer into the local filesystem, period.
The lcamtuf post's core horror, `#include <https://...>` looking local, is
structurally impossible in Aether because the import grammar has no URL
production and the resolver has no network code path.

If a future need for remote packages arises, it belongs to `aeb`, not to
`import` (see below). It must never be reachable by writing an `import`
statement.

### 3. If remote packages ever exist, fetch is a separate, audited, pinned step

`aeb` (the external multi-package build system, github.com/aether-lang-dev/aeb)
reads `share/aether/MANIFEST` to discover link-suitable runtime/stdlib `.c`
files; [install-layout.md](install-layout.md) documents the MANIFEST contract it
consumes. Today it operates entirely on the local install tree.

Should `aeb` ever grow remote package fetch, the lcamtuf post is the
cautionary tale for *how not to do it*. The non-negotiable constraints:

- **Fetch is a distinct command**, not a side effect of build. `aeb fetch`
  is separate from `aeb build`. A build never reaches the network.
- **Pinned by content hash.** A lockfile records the exact hash of every
  fetched artifact. A build verifies hashes against the lockfile and fails
  on mismatch. The build's inputs are reproducible and auditable.
- **Fetched code is contrib-vendored, then reviewed.** A fetched package
  lands as files on disk that a human (or CI) can diff before it is ever
  compiled, not streamed straight into the compiler.
- **Fetch runs without compiler authority.** The thing that touches the
  network is not the thing that generates code.

The principle: **transparent or build-time-implicit remote resolution is
permanently off the table.** Provenance must always be visible, pinned, and
separable from the build.

## How this relates to other Aether features

- **Capability sandboxing** ([containment-sandbox.md](containment-sandbox.md))
  constrains the *program*. This document constrains the *toolchain*. They
  are the same idea, applied one layer down: deny by default, and prefer a
  fixed grant list to a wildcard.
- **Effect tags** (proposed, see the per-function capability-axis issue)
  would let `aetherc` reason about whether a given `.ae` *needs* `fs`/`net`
  at runtime. That is orthogonal to whether the *compiler* touched the
  network, but the two together give a full picture: provenance of inputs
  *and* authority of outputs.
- **`--emit=lib` capability-emptiness** ([emit-lib.md](emit-lib.md)) is the
  same deny-by-default reflex: a library built for embedding starts with no
  capabilities and opts in via `--with=`. The toolchain sandbox is the same
  reflex pointed at the build itself.

## Invariants to not break

- `import` resolves only to local filesystem targets (stdlib, contrib,
  local modules). No URL production in the grammar. No network code path in
  the resolver. Ever.
- A `build` never touches the network. If remote fetch exists, it is a
  separate command (`aeb fetch`), gated by a lockfile of content hashes.
- The `--sandboxed-build` grant list has no `net` entry and no filesystem
  wildcard. Adding a network grant or a `--fetch-during-build` backdoor is
  the lcamtuf vulnerability, re-introduced.
- Fetched third-party code is vendored to disk and reviewable *before* it
  reaches the compiler, never streamed into `aetherc` directly.

## References

- lcamtuf, "A breakthrough in C/C++ dependency management",
  *lcamtuf's thing*, 2026-04-26, the satirical post that frames the threat
  model (the "feature" is the vulnerability)
- [containment-sandbox.md](containment-sandbox.md), the runtime sandbox this
  mirrors at the toolchain layer
- [emit-lib.md](emit-lib.md), `--emit=lib` capability-emptiness, the same
  deny-by-default reflex
- [install-layout.md](install-layout.md), the MANIFEST contract that `aeb`
  consumes and that any remote-fetch design must respect
- [module-system-design.md](module-system-design.md), import resolution; the
  three (and only three) kinds of import target
