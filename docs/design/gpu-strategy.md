# GPU strategy: what to build, what to borrow, where the border is

A design note, not a commitment. It answers two questions that keep coming up
once you notice that Aether's builder-style DSL lowers a declarative-looking
tree to plain sequential C: *could that same pseudo-declarative feel reach GPU
constructs?* And *if we go there, do we need our own equivalents of everything,
or can we bootstrap off Mojo?*

The short version: the CPU side is already done (that is what
[`closures-and-builder-dsl.md`](../closures-and-builder-dsl.md) describes); the
GPU **launch site** is a natural extension of the builder flavour we already
have; the GPU **kernel body** is the one genuinely new piece of work; and
"bootstrapping off Mojo" is the wrong frame — you borrow Mojo's *rules* (which
are language-agnostic and hard-won), reuse the *vendor C runtimes beneath* Mojo
(the stable layer), and write the thin Aether-shaped pieces yourself. You do
not link, transpile, or lower through Mojo itself, because its reusable core is
MLIR, and MLIR is the one thing a readable-C compiler cannot absorb.

## 1. The CPU side is the existence proof, not the question

Aether's "DSL with scope" is a source-to-source transform. `frame { panel {
button } }` lowers to ordinary C with `_aether_ctx_push/pop` around sequential
calls — no runtime interpreter, no reflection, just readable emitted C (see the
generated-code sections of the closures doc). That property is the whole reason
the GPU question is even askable: because the DSL is *lowering*, in principle it
can lower to any backend text, not only host C.

So the GPU question is not "can the DSL express it" — the DSL can express almost
any nested structure. It is "what does the emitted target look like, and where
does the host/device border fall." Those are the parts a widget-tree builder
never had to model.

## 2. What a GPU program has that a CPU builder DSL does not

Three things, and each one is a place where "seamless" quietly stops being true.

**(a) Two address spaces, and the boundary is not free.** A host pointer and a
device pointer are different things, and — the detail worth tattooing on the
wall — they may not even be the same *width*. Mojo shipped platform-sized `int`
into GPU kernels, used it in production, and then in 1.0 **removed** that ability
because host-int vs device-int width mismatches were a miscompilation source;
developers must now use fixed-width types such as `int32` at the boundary. That
is a language *retracting* seamlessness on purpose, after production taught it
where convenience became dangerous. Aether's `ref()` cells are literally
`malloc(sizeof(intptr_t))` and its `_ctx: ptr` injection assumes one flat host
heap — both are host-C-shaped to the bone and mean nothing on a device.

**(b) The block body must lower to a different language, not just different C.**
A kernel body compiles to PTX / SPIR-V / AMDGPU, not to the host's
`_closure_fn_N` hoisted statics with heap environment structs. There is no
`malloc` on the device, function pointers do not work the CPU way, and captured
variables must become explicit kernel arguments in constant/global memory. "The
same trailing block runs on the GPU" is therefore not a lowering tweak; it is a
second code generator.

**(c) The declarative part that matters on GPU is the launch geometry, not the
tree.** Where a CPU builder DSL declares *structure* (widgets, config), the GPU
equivalent declares *iteration space and memory placement*: grid/block
dimensions, shared vs. global memory, warp-level collectives. That is a good fit
for a builder-with-config DSL — but it is a *different* DSL than the widget-tree
one, and it is closer to our `builder … with <factory>` shape (fill a config
object first, then "execute" = launch) than to the immediate-block shape.

## 3. The three layers, and why "bootstrap off Mojo" splits across them

Mojo open-sourced its compiler and toolchain (Apache-2.0 with LLVM exceptions)
one week after 1.0. That sounds like a reuse opportunity. It mostly is not, and
the reason is structural, not legal. "Reuse Mojo" means one of three different
things depending on which layer you point at.

### Layer 1 — the compiler / codegen (the part that emits PTX): **No.**

This is the valuable, hard part Mojo opened. But it is an **MLIR/LLVM dialect
stack** — Mojo source → MLIR → LLVM → PTX / SPIR-V / AMDGPU. To reuse it, Aether
would have to lower to MLIR, which means abandoning "emit readable C" for the
GPU path and taking LLVM as a hard build dependency. That is not bootstrapping
*off* Mojo; it is *becoming a different compiler* for that path, and it
contradicts the spine of the project (readable C, no externs, runtime stays C).
Rejected on identity grounds, before cost even enters.

### Layer 2 — the stdlib GPU library (`_gpu`, warp primitives, `DeviceContext`): **No.**

Legally readable, practically unusable to us:

- It is `.mojo` source written against Mojo's type system, and its intrinsics
  *are* MLIR ops (`_gpu/intrinsics.mojo` uses `external_call` plus
  `is_nvidia_gpu` / `is_amd_gpu` compile-time branches that resolve inside Layer
  1). It is not C, it does not link into C, and there is nothing to transpile
  into Aether-emitted C.
- It is `stdlib/std/_gpu` — the underscore means private — and per Mojo's own
  1.0 notes the accelerator-facing APIs **moved out of the Mojo stdlib into the
  MAX package**, which is a commercial product, not the open compiler. The
  launch/kernel surface you would actually want to reuse is behind Modular's
  (now Qualcomm's) product line.

### Layer 3 — the design: the boundary rules and idioms: **Yes. Borrow freely.**

This is the real bootstrap, and it is worth a lot. Mojo spent a production 1.0
cycle *discovering the rules*, and rules are language-agnostic knowledge, not
code:

- **Fixed-width-at-the-boundary** — `int32`, never platform `int`, into kernels.
  Adopt verbatim, and bake it into the DSL's *type surface* (below) so the seam
  cannot be crossed silently.
- **Interior origins** — reject a reference that points into storage a later
  mutation may reallocate, *at compile time*. That is the *shape* of the safety
  check to want on the device side; Aether's existing closure escape-walk is the
  host-lifetime cousin of the same idea.
- **Compile-time target branching** — `is_nvidia_gpu` / `is_amd_gpu`. Aether
  already owns the equivalent machinery: `--target` plus emit-mode selection, and
  the `--emit=csrc`/`--emit=lib` cross work from #1648, where the emitted C is
  target-neutral and the consumer compiles it. GPU target selection maps onto
  machinery that already exists.
- **Host/device as an explicit border** — Mojo's MAX-vs-stdlib split is the
  same conclusion this note reaches independently: orchestration in-language,
  kernel body as a separate construct.

## 4. What we would actually build ourselves

Almost all of the *mechanism*, almost none of the *design thinking*.

| Piece | Bootstrap off Mojo? | Why |
|---|---|---|
| Launch-DSL ergonomics (builder blocks) | **Already ours** | It is the existing trailing-block / `builder … with` machinery |
| Boundary rules (`int32`, no host-ptr to device) | **Yes — copy the rules** | Pure design knowledge, hard-won by Mojo in production |
| Host→device runtime calls | **Reuse the vendor runtime, not Mojo** | FFI to the CUDA driver / HIP / Metal C ABIs directly — the same runtimes `DeviceContext` wraps — emitting readable C that calls them |
| Kernel-body codegen (→ PTX / SPIR-V) | **Build our own, or defer** | The one genuinely large piece; Mojo's is MLIR-locked and unusable to a C emitter |

### The launch DSL is the piece that is genuinely in reach

It is our builder flavour verbatim: block-first, fill a launch config, then the
function performs the launch. It lowers to readable host C that calls a device
runtime — an FFI-shaped launch descriptor, squarely in scope.

```aether,fragment
// builder gpu_launch(kernel: ptr) with launch_config_new { ... }
gpu_launch(saxpy, grid = (256, 1, 1), block = (64, 1, 1)) {
    arg_buffer(x, n)          // block fills a launch config
    arg_buffer(y, n)
    arg_scalar_i32(n)         // fixed-width AT THE BOUNDARY (Mojo's retracted-then-mandated rule)
    shared_mem(4096)
}
```

The setters are ordinary `_ctx: ptr` DSL functions. The type surface is where we
encode Mojo's boundary rule so it is unbreakable rather than advisory: there is
an `arg_scalar_i32` and an `arg_scalar_i64`, and there is deliberately **no**
`arg_scalar_int` — a platform-width scalar simply cannot be named at the launch
boundary. Lowered, it is a sequence of `device_set_arg_*` calls followed by a
`device_launch` — plain C against whichever driver we target.

### The kernel body is a separate construct, not a reused trailing block

Be explicit that closures, `ref()` cells and boxed closures are **host-only**.
The kernel body wants a `@gpu` / `kernel` construct with its own backend and its
own rules (captures become explicit arguments; the interior-origins-style check
applies; no host heap). The pragmatic lowering that stays true to Aether is to
**emit for the vendor's own toolchain** — lower a `@gpu` body to CUDA-C and hand
it to `nvcc`, exactly the compile-on-install model `--emit=csrc` already
established (#1648): we emit the source, the consumer's device toolchain
compiles it. That calls the *same C runtimes Mojo calls*, without routing
through Mojo, and without taking on MLIR.

## 5. Why not just depend on Mojo directly

Beyond the layer analysis, the ecosystem facts argue against a load-bearing
dependency: Mojo is pre-2.0 (its own roadmap flags a possible source-breaking
2.0), the accelerator APIs have already migrated *out* of the open stdlib into a
commercial package, and the whole thing is now Qualcomm-owned. Mojo 1.0's own
framing is that betting a serious project on a still-moving, recently-acquired
stack is the risk to avoid. Borrowing its *rules* carries none of that risk;
depending on its *code* carries all of it.

## Recommendation

1. **Launch DSL: do it** as a builder-flavour extension, with the fixed-width
   boundary enforced in the *type surface*, lowering to readable host C that
   calls a vendor device runtime by FFI.
2. **Kernel body: a separate `@gpu` construct** with its own backend; emit
   vendor C (CUDA-C via `nvcc`, the `--emit=csrc` compile-on-install model)
   rather than reusing host closures or routing through MLIR.
3. **Do not promise "seamless."** Mojo tried the seamless version (platform int
   into kernels), shipped it, and retracted it under production pressure. Adopt
   the boundary they landed on, not the one they abandoned.

## See also

- [`../closures-and-builder-dsl.md`](../closures-and-builder-dsl.md) — the CPU
  pseudo-declarative mechanism this note extends, and the builder flavour the
  launch DSL reuses.
- [`closure-lineage-and-runtime-tradeoffs.md`](closure-lineage-and-runtime-tradeoffs.md)
  — why Aether closures compile to plain C data and functions rather than
  assuming a runtime that owns every environment; the same "no seamless heap on
  the other side" reasoning is what breaks host closures on a device.
