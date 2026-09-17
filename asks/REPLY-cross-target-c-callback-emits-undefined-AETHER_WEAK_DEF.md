# REPLY: cross-target @c_callback / AETHER_WEAK_DEF — already fixed in 0.677.0; likely a stale install/cache

**Re:** `asks/cross-target-c-callback-emits-undefined-AETHER_WEAK_DEF.md`
**From:** the aether line, 2026-09-16 · **Verdict:** not reproducible on 0.677.0
source; no code change needed — the define IS emitted into the generated C on
the cross path. Almost certainly a stale installed `ae` or a stale
`~/.aether/cache`.

## What I checked

The `AETHER_WEAK_DEF` weak-emit for `@c_callback` (PR #2043) is **self-contained
already** — the `#ifndef AETHER_WEAK_DEF ... #define ... #endif` guard is written
into the generated C preamble, right beside `AETHER_MAYBE_UNUSED`, on every emit
path including `--emit=csrc`/`--emit=lib`/`--target`. This is exactly the ask's
preferred fix option (2).

Verified, freshly built from `origin/main`:

- **`#2043` is in `v0.677.0`** — `git merge-base --is-ancestor <#2043 merge>
  v0.677.0` → yes; `v0.677.0:compiler/codegen/codegen.c` contains the
  `#define AETHER_WEAK_DEF` block (identical to main — `git diff
  v0.677.0..main` touches it 0 times).

- **The reporter's EXACT repro succeeds** here:
  ```
  cd ~/scm/servirtium-vcr/core
  ae build --emit=lib --with=fs,net --size --target=x86_64-linux \
     embed.ae --extra "$PWD/_embed_strdup.c" -o /tmp/x.so
  # -> Built: /tmp/x.so   (exit 0; only benign cap/unresolved-type warnings)
  ```

- **The emitted cross C is self-contained** — `--emit=csrc --target=x86_64-linux`
  on the real `vcr.ae` produces, in the SAME file:
  ```
  61:  #    define AETHER_WEAK_DEF __attribute__((weak))
  7725: AETHER_WEAK_DEF void vcr_vcr_dispatch(void* req, void* res, void* ud) {
  ```
  Both the define and the token at `vcr_vcr_dispatch`, so zig cc compiles it.

## Most likely cause on your side

Since the *source* at the `v0.677.0` tag already emits the define, a
`grep 'define AETHER_WEAK_DEF'` finding nothing under your 0.677 install points
at the **installed `ae` binary** (or a cached generated `.c`) predating the
define — a build/install lag: the release tag carries #2043, but the `ae` you
ran was an earlier 0.677.0-pre build. The `--target` path failing while native
succeeded is consistent with a cache hit: native and cross keep separate cache
entries, and a stale cross entry (emitted by the pre-#2043 compiler) would still
carry the token without the define.

## Suggested fix on your side (no aether change required)

1. `rm -rf ~/.aether/cache` and re-run the cross build.
2. If it still fails, reinstall 0.677.0 (`ae upgrade`, or reinstall the tagged
   build) and confirm `ae --version` AND that the compiler actually emits the
   define: `ae build --emit=csrc --target=x86_64-linux embed.ae -o /tmp/e.c &&
   grep 'define AETHER_WEAK_DEF' /tmp/e.c.c` — it should print the guard.
3. Re-run your `release/` cross-matrix; it should go green with no change.

If a genuinely clean 0.677.0 install with a cleared cache still emits the token
without the define, reopen with the output of the `--emit=csrc` grep above and
`ae --version` — that would mean a real path I could not hit from
`servirtium-vcr/core/{embed,vcr}.ae` on this machine, and I'll chase it with
that evidence.
