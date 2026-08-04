# Aether Through Chlipala's Lens

A framing writeup reading Aether against Adam Chlipala's *"The Expensive
Fictions of Low-Level Programming Languages"* (Jun 2026,
[stng.substack.com](https://stng.substack.com/p/the-expensive-fictions-of-low-level)).
It is a *framing*
document, not a spec, the goal is to place Aether honestly on the two axes the
essay uses to judge languages, and to name the two research directions that
would let Aether actually engage the half of the critique it currently ducks.

It separates the genuine alignment from the wishful, and it is deliberately
unflattering where the language deserves it.

## What Chlipala is actually arguing

The naive reading, "C bad, high-level good", is *not* his claim, and Aether
is vulnerable to being praised for the wrong reasons. His argument runs on two
axes for judging a notation used in automated program search:

1. **Tractability of search**, can we describe systems succinctly and
   manipulate them algebraically?
2. **Control over the final program's hardware behaviour**, can we spell out
   every last detail of harnessing the hardware?

His indictment of C and Rust is that they score *poorly on both*: too low-level
to manipulate cleanly, yet too high-level to expose the real performance model.
And the specific "fictions" he names are precise:

- **The sequential-instruction fiction**, code reads as one-instruction-at-a-
  time while the hardware is a spatial fabric where *everything is always
  running*.
- **The flat-shared-memory fiction**, one uniform address space, when real
  access cost is a function of *locality and topology* and requires global
  reasoning to predict.

The through-line: these fictions make **performance opaque**, and opaque
performance makes **program search intractable**, because a correct-by-
construction search cannot use "will this run fast on the real fabric?" as a
fitness function. His refinement diagram runs from abstract-notation-at-top to
hardware-detail-at-bottom, and *most of the essay is a complaint about the
bottom.*

## The uncomfortable starting point

**Aether compiles to C.** By construction it sits *downstream* of exactly the
model the essay indicts. It cannot claim to have escaped the two fictions,
because its backend *is* the "1970s hardware model." Any honest placement has
to start there rather than around it.

So on his two axes:

### Axis 2 (hardware control): Aether is *further from* the fabric than C

This is the counterintuitive part. Aether is not a lower-level language than C,
it is C *plus* abstractions that move away from the fabric:

- RAII-via-codegen,
- ref-counted / arena-owned strings,
- closures lowered to plain C functions,
- `heap.new` boxing that adopts and frees string fields.

Every one of those is a layer between the source and the emitted instructions.
Aether exposes **no locality model at the language level**, no topology, no
notion in the *source* that memory access cost is nonuniform. On the essay's
own terms, a search choosing among Aether variants has *less* insight into cost
than one choosing among C variants, because refcount traffic, arena lifetimes,
and codegen'd cleanup all sit in the gap. Chlipala's "even simple improvements
require nonlocal changes" problem is **amplified** by a compile-to-C layer, not
relieved.

The one real qualifier: the *runtime* is topology-aware even though the
*language* is not, see the actor section below.

### Axis 1 (search tractability): a genuine story, but a *different* one

This is where Aether has a defensible answer, but it lives near the **top and
middle** of his refinement diagram, which is the half he spends the least time
on. It is important to claim the right win rather than the flattering one.

- **The trailing-block / `builder` DSL and `config IS code`** are a succinct-
  notation play, "notations that let us describe complex systems succinctly and
  manipulate algebraically." The call site reads like a config file while the
  body is full Aether underneath (see `docs/config-is-code.md`,
  `docs/closures-and-builder-dsl.md`). That is precisely his axis-1 virtue,
  expressed at the requirements/config altitude, *not* at the codegen altitude
  he cares most about.

- **The capability / effect system is the most Chlipala-aligned thing in the
  language**, and it should be foregrounded. His actual research program (he is
  the Bedrock / Fiat-Crypto / verified-compilation person) is *correct-by-
  construction via machine-checked specification*. Aether's compile-time effect
  tags (`@pure`, `@no_fs`, `@no_net`, `@no_os`), the `--emit=lib --with=`
  capability gate, `@scoped` escape analysis, and `distinct` types are a
  **lightweight, gradual** version of exactly that discipline: invariants turned
  into things the compiler *rejects programs for violating*. A search generating
  Aether can use "does it typecheck under `@no_fs`?" as a real, cheap fitness
  signal, verification-as-fitness-function, at the safety-property level.

## Where the critique bites Aether specifically

Three places worth naming rather than papering over:

### 1. Aether's guarantees are *safety*, not *functionality*

The capability system proves *a program cannot touch the filesystem*; it does
not prove *the program meets its spec*. Chlipala's premise is search where "a
tool might give up but will never return an incorrect program", full
functional correctness. Aether's `where`-contracts are **runtime** panics
(`precondition_violation: b != 0 in divide`), not static proofs. A violation is
caught at execution, which is exactly what a correct-by-construction search
*cannot* rely on. To play in his world, those contracts would need to be
*discharged statically* (SMT / refinement-type style). They currently are not.
This is the single largest gap between Aether's marketing-adjacent story ("the
compiler checks your invariants") and Chlipala's bar ("the compiler proves your
program correct or refuses it").

### 2. The performance model is fully implicit, and the C layer widens the gap

Covered above under axis 2. The point bears repeating because it is the crux:
inserting a compile-to-C stage between the search's candidate and the emitted
instructions makes the *cost of a candidate less predictable*, not more. This is
the opposite of what a self-improving search wants from its target language.

### 3. The actor model is the one place Aether *could* answer the sequential
fiction, and half-does

Chlipala explicitly praises models that "reveal hardware topology" (MPI) even
while noting they are painful. Aether's `message` / `receive` / `spawn` is a
*logical* concurrency model (no shared mutable state, message-passing) that
answers the *correctness* worry the sequential fiction creates (data races) but
says nothing at the **language** level about *placement*: which node runs which
actor, how far a message travels.

The nuance the first draft of this document got wrong: the **runtime already
carries the topology Aether's source hides.** The multicore scheduler is
NUMA-aware (`runtime/aether_numa.*`, `docs/numa-support.md`), it detects the
node topology and places work to keep memory accesses local. So the fiction is
half-broken *underneath* the language even though the language surface still
presents flat, placeless actors.

That is the design opening. If actor handles carried **affinity/locality as a
first-class, source-visible property**, and the scheduler surfaced its placement
decisions back to the program (or to a search process), Aether would have a
principled, *language-level* answer to fiction #1, a genuine move down-and-
across his diagram rather than a restatement of the C model. The runtime
plumbing is already there; what's missing is exposing it in the type/effect
system the way capabilities are exposed.

## The two research directions this points at

1. **Static discharge of `where`-contracts.** Move preconditions from runtime
   panics toward compile-time proof obligations (refinement types / an SMT
   backend), so a contract becomes something a correct-by-construction search
   can *rely on* rather than *discover at runtime*. This is the path from
   "safety-checked" to Chlipala's "correct or refused."

2. **Topology-visible actors.** Lift the NUMA-aware scheduler's placement model
   into the language: affinity as a first-class property of actor handles,
   placement decisions legible to the program and to an optimizing search. This
   is the path from "logical concurrency over a hidden fabric" to "concurrency
   over a fabric the notation can reason about."

## The one-line verdict

Aether is a strong answer to the *half* of Chlipala's critique he spends the
*least* time on: succinct, algebraically-manipulable, machine-checkable
notation, via the DSL layer and the capability/effect system. It is **not an
answer** to the half he spends the *most* time on, the sequential-execution and
flat-memory performance fictions, because it compiles to C and adopts C's cost
model verbatim (its NUMA-aware runtime notwithstanding, because that awareness
is not yet visible to the source).

Marketing Aether as "a better low-level language for the AI-search era" invites
exactly the rebuttal the essay pre-loads. Marketing it as **"a high-tractability
specification-and-capability layer that happens to emit native code"** is both
more defensible and more true, and it names the two concrete moves (static
`where`-contracts; topology-visible actors) that would let Aether actually
engage the performance half of the argument.

## See also

- `docs/config-is-code.md`, `docs/closures-and-builder-dsl.md` the succinct-
  notation / axis-1 story.
- `docs/emit-lib.md`, `docs/hide-and-seal.md`, `docs/distinct-types.md` the
  capability / effect surface.
- `docs/numa-support.md`, `docs/actor-concurrency.md` the runtime topology
  awareness the language does not yet surface.
