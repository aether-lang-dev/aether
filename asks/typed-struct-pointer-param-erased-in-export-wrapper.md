# A typed struct-pointer parameter is erased to `AetherValue*` in the generated export wrapper, so `--emit=lib` fails to compile

**From:** the selaenium line (2026-09-13) · **Where it bit:** `selaenium`'s
native Aether WebDriver client (`aether/webdriver.ae`) — `aeb aether/.tests.ae`
cannot compile at all, so the one binding that uses no FFI is red.

**Affects:** ae 0.666.0 (the version aeb v0.309 pins). Any GCC that treats
`-Wincompatible-pointer-types` as an error — i.e. GCC 14+ — turns this from a
warning into a hard build failure.

## Symptom

```console
$ aeb aether/.tests.ae
/home/paul/.aether/share/aether/std/mem/module.ae: In function ‘aether_ae_bidi_id’:
/home/paul/.aether/share/aether/std/mem/module.ae:833:23: error: passing argument 1
    of ‘ae_bidi_id’ from incompatible pointer type [-Wincompatible-pointer-types]
/home/paul/scm/selenium/aether/webdriver.ae:1144:27: note: expected ‘WdSession *’
    but argument is of type ‘AetherValue *’
 1144 | _bidi_id(s: *WdSession) -> int {
```

Note the file/line attribution is **wrong** and cost a while to see past:
`std/mem/module.ae` is 372 lines long and never mentions `bidi`. The `#line`
directives point somewhere unrelated to the offending code.

## Cause

For each module function the compiler emits an exported wrapper, built from a
reflection table that records the signature as `"(ptr) -> int"` — the typed
struct pointer has been **erased to `ptr`**. The wrapper is then generated with
`AetherValue*` and calls the real function, which still takes `WdSession*`:

```c
/* from the generated C */
{ "ae_bidi_id", "aether_ae_bidi_id", "(ptr) -> int", ".../webdriver.ae", 1144 },

int ae_bidi_id(WdSession* s) { ... }          /* the real function */

int32_t aether_ae_bidi_id(AetherValue* s) {   /* the wrapper */
    return ae_bidi_id(s);                     /* <-- incompatible */
}
```

The neighbouring wrapper compiles fine, because that function really does take
an untyped `ptr`:

```c
AetherValue* aether_ae_ensure_bidi(AetherValue* sp) { return ae_ensure_bidi(sp); }
```

So the trigger is precisely: **a module-level function whose parameter is a
typed struct pointer (`*T`)**.

## Minimal repro

17 lines, one import. Note it must be built as a **library** — `ae run` on the
same source is fine, because no export wrappers are emitted for a program,
which is what makes this easy to miss.

```aether
import std.heap

struct Thing {
    n: int
}

bump(t: *Thing) -> int {
    v = t.n
    t.n = v + 1
    return v
}

entry(p: ptr) -> int {
    t = p as *Thing
    return bump(t)
}
```

```console
$ ae run ptrlib.ae        # fine — no wrappers emitted for a program
$ ae build ptrlib.ae --emit=lib -o libptr.so
ptrlib.ae: In function ‘aether_bump’:
ptrlib.ae:26:17: error: passing argument 1 of ‘bump’ from incompatible pointer type
ptrlib.ae:8:17: note: expected ‘Thing *’ but argument is of type ‘AetherValue *’
Build failed.
```

## Suggested fix

Either:

1. **Emit the wrapper with the real parameter type** — keep `*T` in the
   reflection signature rather than erasing it to `ptr`, so the wrapper is
   `int32_t aether_bump(Thing* t)`; or
2. **Cast at the call inside the wrapper** — `return bump((Thing*)t);` — if the
   erased `ptr` in the reflection table is deliberate for the FFI surface.

(1) is the better shape: the erased signature is also what any reflection
consumer sees, and `(ptr) -> int` is simply not what the function takes.

Whichever way, worth fixing the `#line` attribution too — pointing at an
unrelated stdlib module for an error in user code is a long detour.

## Workaround in the caller (what selaenium did)

Take `ptr` and cast inside, which is what the other ~200 functions in that same
file already do:

```aether
_bidi_id(sp: ptr) -> int {
    s = sp as *WdSession
    ...
}
```
