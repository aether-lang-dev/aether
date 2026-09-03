# Note: import-closure cache key (spike)

**For:** Nic
**Branch:** `spike/1882-import-closure-cache-key` (commit `c6133520`)
**Status:** measured prototype, **not for merge** — the blocker is in
"What stops this shipping" below.
**Context:** follow-up to #1882, which is fixed and merged (`befa92b9`).

---

## The problem this is about

`compute_cache_key()` hashes whole **directory trees** that a module might
resolve from: the entry file's own directory, the working directory, and every
`--lib` dir. It does not look at imports at all.

That has produced five bugs, all the same shape — *a resolution root nobody
remembered to hash*:

| issue | the root that was missed |
|---|---|
| #623 | symlinked lib entries (needed `stat`, which follows, not `lstat`) |
| #623 follow-up | modules in lib **subdirectories** |
| #1025 | the default `lib/` dir when no `--lib` was passed |
| #1421 | the entry file's own directory (sibling modules) |
| #1882 | the working directory, when the entry is in a subdirectory |

Each fix added one more root to the list. `aether_module.c` has **six numbered
resolution strategies plus a source-dir fallback**, so the list is not obviously
complete, and correctness depends on someone noticing when a seventh appears.

The coarseness costs something too: a program importing one module rehashes
every `.ae` file in the tree, and **any** unrelated edit busts the cache.

## What the spike does

Walks the actual import graph. It is a **scanner, not a parser** — `import
<dotted.name>` is lexically trivial in Aether (top level, no wildcards, no
renaming, no `from`-style forms), so a line match extracts it with no AST and
no lexer.

Two deliberate design choices:

- **Conditional imports are over-approximated.** `when defined(X) { import a }
  else { import b }` hashes *both*. The key must be a superset of what gets
  compiled: hashing an uncompiled file costs a spurious rebuild, missing a
  compiled one serves a stale binary. The `-D` flags are already in the key's
  salt (`ae_define_salt`), so the two configurations never share a cache slot
  anyway — the extra hashing cannot cause a wrong answer.

- **An unresolved import produces no key at all**, and the caller falls back to
  the existing tree hash. Hashing the module *name* as a stand-in was in the
  first draft and is wrong: it turns "I could not find this file" into "this
  file never changes" — silent under-approximation, which is exactly the #1882
  failure. A failure must mean *rebuild*.

Enabled with `AETHER_CACHE_IMPORT_SCAN=1`; `AETHER_CACHE_SCAN_DEBUG=1` prints
what it resolved. It replaces only the **source** portion of the key —
compiler/driver mtimes, `--extra` file hashes, `--lib` identity, opt level and
the `-D` salt all still apply.

## Measurements

| scenario | current key | scanner |
|---|---|---|
| warm run from repo root (1399 `.ae` files) | 111ms | **13ms** |
| unrelated file edited, cache should hold | 177ms (busted) | **11ms** (held) |
| deep graph (`tls13_cert`) | hashes 1399+ files | **21 files, 3ms** |

The second row is the more interesting one. Today any edit anywhere in the tree
costs a rebuild; with the closure, editing an unrelated module keeps the hit.

## Correctness A/B

- All **7** `cache_*` integration suites pass with the flag on, including
  `cache_symlinked_lib_edit` (#623) and `cache_libdir_invalidation`.
- **60** regression tests, identical outcomes both ways (59 pass / 1 pre-existing
  non-main file).
- Transitive edits two hops deep detected in both modes.
- Unresolvable import falls back cleanly; the compiler still reports the real
  error.

Permutations checked individually: `--lib` ordering, shadowing, deletion,
import cycles (`a→b→a` terminates), diamonds (hashed once, deterministic),
source import order, `--extra` C files, `@link` directives, install-resolved
stdlib, and editing the installed stdlib.

Two of those found real bugs *in the spike*, both now fixed: I was passing
`std.math` to `module_resolve_stdlib_path`, which prepends `std` itself
(producing `std/std/math/`), and Try 3 resolves relative to the **running
binary**, so a prototype outside the install prefix cannot find the stdlib. The
no-fallback rule is what made the first one visible — it surfaced as a loud
`NO KEY` instead of a silently wrong key.

## What stops this shipping

**It duplicates the resolver's search order.** `ae` does not link
`libaether_compiler.a`, so `scan_resolve()` in `ae_cache.c` is a hand-copied
mirror of `module_resolve_local_path()`'s Try 1–6 plus the package roots.

That duplication is *precisely* the thing that caused the five bugs above. As
written, the spike trades a coarse-but-safe design for a precise-but-fragile
one, and the next resolution strategy anyone adds silently desynchronises it.
I would not merge it in this form.

## Three ways to fix that, worst to best

1. **Link `libaether_compiler.a` into `ae`.** Smallest diff, but pulls the whole
   compiler into the driver for two functions.

2. **Extract the resolver into a small shared TU** both `ae` and `aetherc`
   link. Honest, but it is a refactor of a load-bearing file, and the resolver
   reads `global_module_registry` state that would have to come with it.

3. **Have `aetherc` report the files it opened.** My preference by some
   distance. The compiler already knows every file it read — it opened them.
   Emitting that list (a `--emit-deps` style flag, or a sidecar written next to
   the cached artifact) gives *exact* invalidation with **no duplicated
   knowledge at all**, and it cannot drift, because it is a record of what
   actually happened rather than a prediction of what would.

   The cost is that the list only exists *after* a compile, so the first build
   of anything still needs the conservative key. That is fine — it is already
   the cold-cache path. Subsequent runs read the recorded list. This is roughly
   how `gcc -MD` / ninja depfiles work, and for the same reason.

If you like (3), the natural shape is: cold build uses today's tree hash and
writes a depfile; warm runs hash the depfile's contents and skip the tree walk
entirely; a missing or unreadable depfile falls back to the tree hash. Same
safety posture as the spike — failure means rebuild.

## Also worth knowing

- **Hash choice.** The spike keeps FNV-1a 64, matching the existing key. This
  is a cache key, not a security boundary — nobody is choosing file contents to
  force a collision, and at 64 bits collision odds across a few thousand
  entries are ~10⁻¹², well below the rate at which mtimes lie. SHA-256 would
  cost ~10× for no correctness gain and a new dependency in the driver. I would
  revisit only if cache entries were ever shared between machines or users,
  where adversarial inputs become plausible.

- The spike branch is merged up to current `main` and builds clean; the flag is
  off by default, so `main`'s behaviour is untouched with it present.
