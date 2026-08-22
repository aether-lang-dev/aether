# Proposed `.aea` Module Artifacts

## Problem

`make install` currently gives downstream users two different kinds of
stdlib material:

- `$(PREFIX)/lib/aether/libaether.a`: precompiled C runtime and C-backed
  stdlib support.
- `$(PREFIX)/share/aether/std/.../module.ae`: Aether-authored stdlib
  modules installed as source.

That means top-level C-backed modules such as `std.cryptography` are already
available through the installed archive, while Aether-authored modules such as
`std.cryptography.md2`, `std.cryptography.sha3`, `std.cryptography.blake2`,
and the KDF/hash submodules are compiled again by each downstream application
that imports them.

This is functional, but it is not the experience users expect from a
first-class installed standard library. Java users do not think about whether
a package came from source or from a `.jar`; the import experience is the same.
Aether should aim for that same level of artifact transparency.

## Goal

Compiled Aether stdlib modules should behave as if their sources were
co-located with the importing program.

For example:

```aether
import std.cryptography.md2
```

should mean the same thing whether the resolver uses:

- a local source tree,
- an installed `share/aether/std/cryptography/md2/module.ae`, or
- an installed compiled module artifact.

The artifact is an implementation detail and cache boundary, not a new import
surface.

## Non-goal

Do not make this a flattened C-grade experience.

An artifact such as:

```text
$(PREFIX)/lib/aether/std.cryptography.md2.aea
```

must not degrade the user-facing model into "generated C plus a linker
archive". Users should not need to know generated C symbol names, object file
layout, or link ordering to import a normal Aether module.

## Desired Developer Experience

The import path remains the API:

```aether
import std.cryptography.md2
import std.cryptography.sha3
import std.cryptography.pbkdf2
```

Diagnostics remain Aether diagnostics:

- module names are reported as `std.cryptography.md2`, not generated C files;
- exported functions are reported with their Aether signatures;
- source locations map back to the original `.ae` module where possible;
- `ae help`, doc generation, and import resolution see the same exported
  surface they would see from source.

Build tools should not have a separate "compiled stdlib import" path. The
normal module resolver should choose the best compatible backing artifact.

## Proposed Artifact Shape

Use a first-class compiled Aether module archive, tentatively `.aea`.

One possible installed layout:

```text
$(PREFIX)/lib/aether/modules/std/cryptography/md2.aea
$(PREFIX)/lib/aether/modules/std/cryptography/sha3.aea
$(PREFIX)/lib/aether/modules/std/cryptography/pbkdf2.aea
```

The exact path is open, but it should preserve module hierarchy rather than
flattening everything into opaque filenames.

Each `.aea` should contain structured metadata and compiler-owned payloads,
for example:

```text
module.json
typed-ir.aeir
objects/<target-triple>.o
docs.json
source.map
```

Where:

- `module.json` records module name, compiler version, Aether ABI version,
  target compatibility, build flags, imports, exports, and source hash.
- `typed-ir.aeir` is a typed Aether IR payload, not generated C as the public
  contract.
- `objects/<target-triple>.o` is an optional native object cache for targets
  where the artifact producer built one.
- `docs.json` records exported docs and signatures for `ae help` and docgen.
- `source.map` maps diagnostics and generated code back to original Aether
  source locations.

The object cache is allowed to be disposable. The typed IR and metadata are
the important first-class module boundary.

## Resolver Behavior

Suggested precedence:

1. local project source;
2. explicit `--lib` source or artifact paths;
3. compatible installed `.aea` artifact;
4. installed source fallback under `$(PREFIX)/share/aether/std/.../module.ae`.

This preserves today's source-based behavior while allowing an installed
toolchain to skip recompiling stdlib Aether modules when a compatible artifact
exists.

Compatibility checks should include at least:

- compiler version or IR format version;
- Aether ABI version;
- target triple or target family;
- relevant compile flags and feature gates;
- dependency artifact hashes;
- stdlib/toolchain version.

If the artifact is missing or incompatible, the resolver should fall back to
source when source is installed.

## Why This Matters

The current model pushes a compilation chore onto downstream applications.
Every clean build of an application that imports Aether-authored stdlib modules
must recompile those modules into the application.

That has costs:

- slower downstream clean builds;
- repeated work across applications;
- less obvious installed artifact boundaries;
- a split mental model where C-backed stdlib feels prebuilt but Aether-backed
  stdlib feels source-distributed.

A first-class `.aea` format fixes that without changing imports.

## Open Design Questions

- What typed IR is stable enough to serialize across patch releases?
- Is `.aea` a per-module artifact, a package-level artifact, or both?
- Should native object caches be mandatory or opportunistic?
- How much whole-program analysis currently assumes source ASTs are available?
- How are generic instantiations, closures, builders, and effect/capability
  checks represented in the artifact?
- How does `--emit=lib` plus `--with=fs,net,os` capability gating interact
  with precompiled artifacts?
- How does artifact invalidation work for local overlays, patched stdlib
  trees, and cross-compilation?

## Success Criteria

This feature is successful when:

- `import std.cryptography.md2` works identically from source or `.aea`;
- downstream applications do not need to recompile installed stdlib Aether
  modules on every clean build;
- diagnostics and docs remain Aether-native;
- installed source remains a fallback and debugging aid;
- build tools can treat `.aea` as a normal module-resolver result, not a
  special linker-only input.

