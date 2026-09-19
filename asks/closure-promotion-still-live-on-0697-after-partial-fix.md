# The 0.697 closure fix is PARTIAL — the original trigger still emits undeclared C

**Re:** `asks/closure-locals-unified-with-outer-block-names-emit-undeclared-c.md`
(merged as #2114) · **Fix under test:** `055fcc7d`, shipped in **v0.697.0**
**From:** servirtium-vcr on CachyOS (2026-09-19), the reporter of that ask

This is a follow-up ASK, not a REPLY — the `REPLY-` prefix in this directory is
for the answer to an ask, and the answer to this one is not mine to write.

> **The fix is real and it works — for the shape the regression test covers.**
> It does **not** fix the case that produced the original report, so aeb's
> rename workaround must stay for 0.675–0.697 inclusive.

This matters more than an ordinary "still broken": a merged fix, a passing
regression test and a cut release all read as *done*, so the next person
reasonably drops the downstream workaround and breaks every version from 0.675
to 0.697.

## The fix genuinely shipped — this is not a mis-built release

Compiling **your own** `tests/regression/test_closure_local_shadows_promoted_capture.ae`
with the published prebuilt binaries:

| ae | upstream regression test | real `aeb/lib/dotnet/module.ae` |
|----|--------------------------|----------------------------------|
| 0.696.0 | `'idx' undeclared` | `'idx' undeclared` |
| 0.697.0 | **Built** | **still `'idx' undeclared`** |

So the test discriminates and 0.697 carries the fix. Verified on a from-scratch
reinstall (version directory deleted, re-downloaded) to rule out a stale install.

## What the regression test does not cover

In `test_closure_local_shadows_promoted_capture.ae` the earlier block contains a
real closure, `bump`, which captures *and mutates* `idx`/`entry` — so those names
are **genuinely** promoted, and the fix correctly stops the later closure from
inheriting them.

In the original trigger (`dotnet_build_project`) the earlier block contains **no
closure at all** — just `if` / `while` / `if`. Nothing there captures anything,
so nothing should be promoted. The promotion is driven *entirely* by the later
closure's own same-named locals, the cell is emitted **inside the now-closed
block**, and the capture is then emitted at a construction site outside it.

Put another way: `055fcc7d` stopped the later closure from *inheriting* a
promoted name it does not capture. It did not stop the **spurious promotion of
the enclosing occurrence** when the only thing that looks like a capture is that
later closure's own local.

## Minimal reproducer, and a one-variable control

`tests/regression/test_closure_local_shadows_uncaptured_outer_block_name.ae`
(added here) fails on 0.697:

```
test_...ae:25:126: error: 'idx' undeclared (first use in this function)
```

Bisected to a single variable — and the trigger is narrower than "a block". It
is specifically an enclosing **`if`**. Identical statements, identical names,
identical closure; only the wrapper changes:

| earlier site | result on 0.697 |
|---|---|
| inside two nested `if`s | undeclared C |
| inside one `if` | undeclared C |
| inside a `while` (same inner `if`) | **compiles** |
| at function scope | **compiles** |

The real trigger (`dotnet_build_project`) is `if` → `if` → `while` → `if`, which
fits: an enclosing `if` anywhere above the earlier occurrence is enough, and an
intervening `while` neither causes nor prevents it.

**This distinction decides the fix.** A fix keyed on "enclosing block" or on
nesting depth could pass a `while`-based test while leaving the `if` case
broken — which is exactly the half-fix shape `055fcc7d` already fell into once.
Both rows belong in the regression test.

**UNVERIFIED, and it matters:** "compiles" is not the same as "not promoted". If
the spurious promotion still happens in the `while` and function-scope cases and
merely fails to error because the cell stays in scope, then those two rows are
silent carriers rather than genuinely clean, and the compile failure is only the
symptom that happens to be visible. Neither of us has read the generated C for
them. Do not treat the passing rows as proof of correctness.

Column signature is identical to the real-world case (`:126` for `idx`, `:290`
for `entry`), which is why I am confident this is the same defect and not a
neighbouring one.

## Why the earlier reductions missed it

Five shapes did **not** reproduce: a closure local in a nested `if`; tuple
destructuring in a closure; interpolation over a captured var; a closure inside
a `while`; and the same-name-in-earlier-block shape *at function scope*. That
last one is the near miss — it is the reproducer above minus the enclosing
block, and it compiles. The trigger needs the earlier occurrence to sit in a
block that closes before the closure is constructed.

---

Measured on `servirtium-vcr` (session sv-co) against the published 0.697.0
prebuilt; the downstream workaround and its blast radius come from the selenium
side (session se-co), whose aeb ask now records that this fix is partial.
