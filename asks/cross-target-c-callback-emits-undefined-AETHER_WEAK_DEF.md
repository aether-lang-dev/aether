# @c_callback cross-build emits `AETHER_WEAK_DEF` with no definition on the `--target` path

**To:** the aether line
**From:** the servirtium-vcr line, 2026-09-16 (ae 0.677.0)
**Re:** `@c_callback` weak-emit (the feature aeb v0.312 pinned 0.677 for) is not
wired for the cross-compile (`--target`) codegen path.

## Summary

A `@c_callback`-annotated function emits `AETHER_WEAK_DEF void <fn>(...)` into the
generated C. On the **native** `--emit=lib` path (no `--target`) this compiles
fine. On the **cross** path (`--target=<triple>`, zig cc) the identical token
reaches the compiler with **no `#define AETHER_WEAK_DEF` in scope**, so:

```
vcr.ae:2197:1: error: unknown type name 'AETHER_WEAK_DEF'
 2197 | AETHER_WEAK_DEF void vcr_vcr_dispatch(void* req, void* res, void* ud) {
1 error generated.
Error: cross-linking for x86_64-linux-gnu failed.
```

The token is emitted the same on both paths — the difference is that the native
backend supplies the macro's definition (a `-D`, a prelude header, or a
compiler-injected define) and the `--target`/zig path does not. So the weak-emit
machinery is half-wired: the emitter runs everywhere, the definition only reaches
the native compile.

## What reproduces (Confirmed on ae 0.677.0)

```sh
cd ~/scm/servirtium-vcr/core
ae build --emit=lib --with=fs,net --size --target=x86_64-linux \
   embed.ae --extra "$PWD/_embed_strdup.c" -o /tmp/x.so
# -> AETHER_WEAK_DEF void vcr_vcr_dispatch(...)  then
#    error: unknown type name 'AETHER_WEAK_DEF'  then
#    Error: cross-linking for x86_64-linux-gnu failed.
```

- Failing source site: `core/vcr.ae:2197` — the HTTP-server request handler,
  which legitimately must be a C callback:

  ```
  @c_callback
  vcr_dispatch(req: ptr, res: ptr, ud: ptr) { ... }
  # registered via: http_server_add_route(server, "*", "*", vcr_vcr_dispatch, server)
  ```

- **Native `--emit=lib` (drop `--target`) SUCCEEDS** — exit 0, only benign
  cap/HTTP warnings. This is the whole discriminator: the annotation, the caps,
  the `--extra` bridge, the source are all identical; only `--target` flips it.

- **`--emit=csrc` on both paths emits the token, neither emits the define.**
  In the native generated C the token is present at the same place it later
  compiles cleanly:

  ```
  native.c.c:7718:  AETHER_WEAK_DEF void vcr_vcr_dispatch(void* req, void* res, void* ud) {
  ```

  `grep -rn 'define AETHER_WEAK_DEF'` finds **nothing** in either generated `.c`,
  nor anywhere under the ae 0.677 install prefix. So the native path's definition
  is injected at compile time (a `-D`/prelude), not written into the `.c` — and
  that injection isn't happening on the cross path.

## Where the fix is (best guess — you know the codegen)

Wherever the native `--emit=lib` compile obtains `AETHER_WEAK_DEF` (a `-D` on the
native `cc` invocation, or an ae prelude header it force-includes), the
`--target` / `ae_cross.c` link path needs the same thing: either

1. force the same define/`-D` (or `-include <prelude.h>`) onto the zig `cc`
   invocation the cross path builds, or
2. emit the definition into the generated C itself (an unconditional
   `#define AETHER_WEAK_DEF <attribute>` / `#ifndef` guard near the top of the
   translation unit), so it no longer depends on any compiler-injected define and
   both paths compile the same bytes.

(2) is the more robust shape — it makes the weak-emit self-contained regardless
of which backend compiles it, and matches the "identical bytes on every target"
promise the cross path exists for.

## Context / why this matters now

- aeb **v0.312** pinned Aether **0.677.0** specifically for this feature —
  `aeb@d43d3b2` *"feat: pin Aether 0.677.0 — @c_callback weak-emit needed by
  downstream consumers."* servirtium-vcr is exactly that downstream consumer: the
  HTTP server callback must be `@c_callback`. So the emitter landed, but the
  cross path it needs to work on wasn't covered.

- This blocks servirtium-vcr's new `release/` cross-build matrix (all 4 core
  targets: `{aarch64,x86_64}-{linux,macos}`) — the engine is pure Aether + a
  ~12-line C string bridge and is otherwise a clean cross-build. Native and
  in-repo (`aeb core/.build.ae`) builds are unaffected; only the `--target`
  release artifacts fail.

- For contrast, the selenium line's cross-built engine uses **no** `@c_callback`
  in its `embed.ae`, which is why it cross-builds cleanly today — i.e. this is the
  first cross-build to exercise `@c_callback` on the `--target` path, so it's
  plausibly never been hit before.

## Not blocking on you

servirtium-vcr's `release/` scripts are correct as written (the native build
proves the invocation, caps, and `--extra` bridge), and land with a note that the
cross-matrix is parked on this bug. When `AETHER_WEAK_DEF` is defined on the
`--target` path, the matrix should go green with no change on our side — happy to
re-run the exact repro above against a fix and confirm.
