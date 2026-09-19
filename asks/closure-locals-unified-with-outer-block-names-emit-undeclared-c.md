# A closure's own locals are unified with same-named locals in an earlier closed block, and emitted as captures outside their declaring block

> **STATUS: live on ae 0.696.0.** Worked around downstream in aeb
> (`6af17aa`, renames the colliding locals), so the symptom is currently hidden
> — but the compiler bug is unfixed and will bite the next person who reuses a
> name. Verified still-broken on 0.696 by reverting only the rename.
>
> Companion ask on the aeb side:
> `aeb/asks/closure-var-collides-with-function-body-name.md`. That one describes
> the trigger as "the transform/inline path"; the mechanism below supersedes it.

## Symptom

gcc rejects the generated C for `aeb`'s dotnet SDK:

```
lib/dotnet/module.ae: In function 'dotnet_build_project':
lib/dotnet/module.ae:764:126: error: 'idx' undeclared (first use in this function)
lib/dotnet/module.ae:764:290: error: 'entry' undeclared (first use in this function)
aeb-link: FATAL — failed to link the fan-out orchestrator
```

**Blast radius is the whole graph, not one node.** aeb's fan-out orchestrator is
a single binary for every node, so nothing in any graph containing the offending
module can run — a full presubmit dies before executing a single test.

Note the reported location is misleading: line 764 is
`if string.length(line_r) > 0 {`, which has no column 290. The `#line` mapping
back to `.ae` is off, so chasing that line wastes time. Chase the generated C.

## Bisect

One fixed aeb (dotnet SDK byte-identical across v0.311–v0.319), varying only the
compiler:

| ae | result |
|----|--------|
| 0.668.0 | clean |
| 0.675.0 | **`idx`/`entry` undeclared** |
| 0.677.0 | **undeclared** |
| 0.681.0 | **undeclared** |
| 0.696.0 | **undeclared** (re-confirmed 2026-09-19 by reverting only the rename) |

So it landed in **0.675** and is still present in **0.696**.

## Mechanism, from the generated C

In `dotnet_build_project`, the names `idx`/`entry` are assigned in **two** places:
once in an earlier nested block (a pkgrefs loop, `.ae` ~724) and once as the
*own locals* of a later closure passed to `string.seq_each` (`.ae` ~766).

The emitted C for that one function:

```c
/* ~12739 — inside the EARLIER block */
int* idx = (int*)_aether_cell_new(sizeof(int));
const char** entry = (const char**)_aether_cell_new(sizeof(const char*));
...
/* ~12775 — that block ENDS; the cells are released */
_aether_cell_release_str(entry);
_aether_cell_release(idx);
...
/* ~12827 — closure-construction site, OUTSIDE that block */
_e->idx   = (int*)_aether_cell_retain(idx);      /* 'idx' undeclared here */
_e->entry = (const char**)_aether_cell_retain(entry);
```

So codegen:

1. unifies the closure's own locals with the enclosing function's same-named
   locals purely by name,
2. concludes the pair is captured-and-mutated and promotes the **earlier** ones
   to heap cells,
3. emits the capture at a construction site that sits **outside the C block
   where those cells were declared and released**.

The closure's locals are not captures at all — they are declared in the closure
body. A name assigned inside a closure should not be unified with a same-named
local of the enclosing function, least of all one in an already-closed block.

## What did NOT reproduce it (so you don't redo this)

Five reduced cases all compile clean on 0.677 and 0.696, so the shape alone is
not sufficient:

1. a closure local declared inside a nested `if`;
2. the same plus tuple-destructuring assignment (`a, b = f(...)`) in the closure;
3. the same with string interpolation over captured vars;
4. the closure inside a `while` loop;
5. the same name in an earlier closed block **and** in the closure — the shape
   described above, in a small function.

The trigger needs something the small cases lack — plausibly the size of
`dotnet_build_project` (~200 lines, several closures) tipping an inlining or
scope-flattening decision. `aeb/lib/dotnet/module.ae:698-893` at the pre-`6af17aa`
revision is the reliable reproducer.

## Confirming the diagnosis

Renaming *only* the closure's own locals (`idx`→`vr_idx`, `entry`→`vr_entry`),
changing nothing else, makes it compile and pass. That is what shipped in aeb
`6af17aa`. It confirms name-unification as the mechanism, and is the reason the
symptom is currently invisible.

## Why it is worth fixing rather than leaving worked around

Two unrelated locals sharing a name in one long function is ordinary code, not a
smell anyone would flag in review. The failure is a C compile error naming a
variable that does not exist at the reported line, in generated code the author
never sees — with a whole-graph blast radius. The next occurrence will cost
someone the same day it cost here.

---

Reported from `servirtium-vcr` on CachyOS (session sv-co), with the bisect and
generated-C evidence; the aeb-side workaround and the presubmit blast-radius
detail came from the selenium side (session se-co).
