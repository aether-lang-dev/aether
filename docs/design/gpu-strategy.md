# GPU strategy: what to build, what to borrow, where the border is

A design note, not a commitment or a shipped API. Reviewed on 2026-09-10 against
Aether's compiler and closure documentation, and the local Modular checkout at
`b61be59` (2026-09-09). External references describe the versions reviewed;
recheck dependency and distribution details when implementing.

Aether's builder-style DSL is a good fit for **configuring GPU launches**. Its
existing host C backend can call a device runtime through an FFI layer. Neither
property supplies a GPU runtime contract or makes ordinary Aether closures
executable on a device.

The recommended first step is a library that loads and launches externally
compiled kernels on one backend, with explicit argument layouts, buffer
ownership, completion and errors. A restricted Aether kernel language can follow
once that contract works. Emitting device source for a vendor compiler is a
credible route; integrating Mojo through a compiled C-ABI wrapper is another
option to evaluate, not a technical impossibility.

## 1. What the existing DSL gives us

[`closures-and-builder-dsl.md`](../closures-and-builder-dsl.md) describes two
independent choices: the caller's trailing-block form and the function's regular
or `builder` flavour. For a launch descriptor, the useful combination is an
**immediate trailing block attached to a builder function**:

1. A zero-argument factory creates the configuration object.
2. The compiler pushes it onto the host builder context stack.
3. Ordinary `_ctx: ptr` setters execute inline and fill the configuration.
4. The compiler pops the context and calls the builder function to execute it.

That mechanism already exists and emits readable sequential C. A launch library
can use it without new syntax. The factory, setters and launch implementation
still need an agreed protocol, including configuration cleanup on early exits.
The builder context itself is an untyped host pointer; it does not enforce GPU
argument types or resource lifetimes.

Launch configuration can describe grid/block dimensions and dynamic shared-memory
size. Thread indexing, shared-memory accesses, atomics, barriers and warp/subgroup
collectives belong to the **kernel body**. Nested builder syntax does not provide
their execution semantics.

## 2. The host/device contract comes before kernel syntax

### Argument representation and validation

Host addresses are not automatically usable by a device. Even on systems with
unified addressing or shared physical memory, accessibility, ownership and
synchronization remain explicit concerns. An initial API should accept device
buffer handles obtained from its allocator, with explicit upload/download
operations; mapped or managed memory can be a later, separately specified mode.

Mojo 1.0 removed `DevicePassable` conformance from its platform-sized `Int` and
`UInt` because passing them to kernels could miscompile when host and device
widths differed. That is useful precedent for an explicit ABI, not a rule to
copy by spelling alone: Aether's numeric types have their own representations.
See the [Mojo 1.0 release notes](https://mojolang.org/releases/v1.0.0/).

For Aether, fixed-width setters establish how bytes are packed, but their names
do not prevent implicit numeric conversion. During this review, a function
`arg_scalar_i32(v: int32_t)` accepted a `long` containing `4294967297` and received
`1`. This follows the current numeric compatibility rules in
[`typechecker.c`](../../compiler/analysis/typechecker.c).

The launch contract therefore needs:

- A kernel signature/schema covering argument count, order, scalar width and
  signedness, floating-point representation, and backend-required alignment.
  Packing must be checked against that schema. For external kernels, supply
  metadata produced with the artifact or a trusted explicit declaration; do not
  assume every backend can recover a complete source signature from a binary.
- Checked scalar conversions before narrowing. Nominal wrappers using Aether's
  `distinct` types can require callers to cross an explicit type boundary, but
  an `as` cast alone is not a range check. Checked constructors must define their
  accepted input types and reject values they cannot represent.
- Distinguishable kernel, context and device-buffer handles. Validate buffer
  bounds and context/device association. A raw host `ptr` must not accidentally
  satisfy an ordinary device-buffer parameter.
- Explicit buffer element types and lengths, including whether a length is in
  bytes or elements. Initially reject aggregates containing pointers, strings,
  closures or other host runtime objects. Define aggregate layout before adding
  by-value structs. Aether's `float` emits C `double`, so an f32 boundary needs
  its own representation and conversion policy too.

These checks establish a launch ABI. They do not prove that an arbitrary external
kernel respects buffer bounds or that independently supplied metadata is true.

### Completion, ownership and errors

For the first version, prefer **synchronous public operations**: upload completes
before returning; launch waits for its submitted work; download returns only when
host data is available. The implementation may use a stream internally, but it
must complete or safely drain outstanding work before releasing its resources,
including on failure paths.

The launch configuration owns copies of scalar argument values. It must keep
buffers, the kernel module and the context valid through completion. Define who
frees the configuration and what happens if its block exits before launch. A
setter failure should be recorded in the descriptor and prevent submission;
launch returns the error through Aether's ordinary error-return convention.

Distinguish validation and submission errors from errors discovered while waiting
for device execution. Document whether a failed context remains usable. Validate
launch dimensions and shared-memory requests against the selected device's limits.

An asynchronous extension would need explicit streams/events or completion
handles, ordering rules, retention until completion, and a policy for freeing or
mutating in-flight buffers. Scope exit alone is insufficient. Mojo's
`max.gpu.host.DeviceContext` is a useful implementation reference: its buffer
destructors schedule release on the stream, and its copy documentation requires
synchronization before reading results. See the
[DeviceContext source](https://github.com/modular/modular/blob/b61be59/max/mojo/max/gpu/host/device_context.mojo).

Mojo's experimental interior origins detect references invalidated by collection
mutation. Aether's closure escape analysis and `@scoped` checks do not supply that
analysis, and neither lexical non-escape nor fixed-width arguments establish
asynchronous completion. Do not promise equivalent compile-time safety without
designing and implementing the required checks.

## 3. What can be reused from Mojo

There are three different reuse decisions. Keeping them separate makes both the
cost and the remaining dependencies clearer.

| Route | What it provides | Assessment |
|---|---|---|
| Study or adapt GPU library source | Buffer/stream protocols, argument encoding, device primitives and algorithms | Useful reference material; adaptation must replace dependencies on Mojo's types, intrinsics and runtime |
| Compile a Mojo wrapper with a C ABI | A host-callable entry point that can hide Mojo-specific implementation details | Technically possible; GPU operation, runtime initialization, packaging and redistribution need a concrete integration probe |
| Integrate compiler internals | A route into Mojo's MLIR/LLVM compilation machinery | A substantial additional backend and dependency commitment; defer unless measured benefits justify it |

The GPU-facing library moved largely into `max.gpu`, but package placement is not
the same as source licensing. The reviewed `max.gpu` source, including
`host/device_context.mojo` and `host/compile.mojo`, has Apache-2.0-with-LLVM-exceptions
headers. Modular explicitly states that the moved accelerator source remains
available under Apache. The repository README separately identifies terms for
MAX usage and distribution. Assess the exact source and binary dependencies of a
proposed integration, rather than inferring their status from the package name.
See [Modular's clarification](https://forum.modular.com/t/open-source-device-codegen/3429/2)
and the [repository licensing summary](https://github.com/modular/modular/blob/b61be59/README.md#license).

Mojo supports `@export` functions with a C calling convention and
`mojo build --emit shared-lib`. Aether could call such a wrapper from emitted C;
it would not have to lower its own host code to MLIR. The wrapper must initialize
the Mojo runtime where required, and the GPU-specific build and runtime path
still needs verification. This is a supported interop mechanism, not evidence
that every GPU dependency is independently reusable. See
[Mojo's shared-library documentation](https://mojolang.org/docs/tools/compilation/#call-a-mojo-shared-library-from-c-or-c).

Direct integration of compiler internals is a different proposition: it adds
MLIR/LLVM integration and maintenance work. It need not replace Aether's host C
backend, but it is much larger than an FFI library. Defer it on scope and cost
grounds. Aether already uses externs; their existence is not an architectural
objection to either runtime route.

The package split also does not establish that kernels require a separate
language construct. That is an Aether design choice. Borrow concrete lessons
from Mojo, and evaluate API stability, supported hardware and deployment cost
for the versions actually used.

## 4. The proposed launch library

Start with one backend selected for available hardware and a concrete workload.
CUDA's driver API is a plausible first candidate for NVIDIA hardware; HIP is a
separate integration. Metal's main host interfaces are Objective-C and C++ and
need a C-facing bridge for a conventional Aether FFI surface. It should not be
costed as another drop-in vendor C API. See
[Apple's Metal-cpp documentation](https://developer.apple.com/metal/cpp/).

The following is an **illustrative proposed API**, not runnable shipped code.
Assume the kernel and its signature have been loaded together, `x`, `y` and `out`
are device buffers, and `n_i32` came from a successful checked conversion. The
kernel computes `out[i] = x[i] + y[i]` and guards `i < n`.

```aether,fragment
// Proposed: builder launch_sync(kernel: Kernel) with launch_config_new
err = gpu.launch_sync(vector_add) {
    grid(block_count, 1, 1)
    block(64, 1, 1)
    arg_buffer(x)
    arg_buffer(y)
    arg_buffer(out)
    arg_i32(n_i32)
    shared_mem_bytes(0)
}
if err != "" { println(err); return }
// Device execution has completed. Download out explicitly to read it on the CPU.
```

This is ordinary host-side configuration followed by validation, argument
packing, submission and completion. It needs no GPU closure syntax. The API
specification must also cover allocation, transfer, module loading, signature
creation and destruction; the launch example is only one part of that lifecycle.

Keep the optional GPU dependency out of ordinary CPU-only and WASM builds.
Specify how the module fits Aether's capability-empty `--emit=lib` mode before
shipping it: driver access must not become an implicit capability bypass, and
the host libc sandbox should not be assumed to contain arbitrary device code.
The module's capability policy remains an implementation design decision.

## 5. A later Aether kernel language

A separate `@gpu` or `kernel` declaration is a candidate, not a settled spelling.
Its body needs a restricted type and execution model, validation of the entire
reachable device call graph, and device-specific lowering. The existing host
closure representation, `ref()` cells and boxed closures are not device values.
Initially require explicit kernel parameters and reject implicit captures.

Specify at least these rules before implementing the syntax:

- Supported scalar types and arithmetic semantics, including overflow, shifts,
  floating-point contraction and narrowing.
- Device pointer/address-space types, indexing, local and shared storage, and
  which helper functions are callable. Reject transitive calls to host-only
  externs, allocation, string/collection runtimes and actor operations.
- Thread/block indexing, supported atomics, barrier participation and memory
  ordering. Define any subgroup operations against actual target capabilities;
  a target branch does not make subgroup widths or collective semantics uniform.
- Diagnostics for unsupported operations and source mapping through the device
  compiler. State which bounds, race and synchronization errors remain the
  programmer's responsibility.

Device allocation is not universally absent: CUDA supports device-side `malloc`
and `free`. The relevant limitation is that Aether's current host allocation and
closure machinery cannot simply execute there. A first kernel subset can exclude
allocation as an explicit scope choice. See
[CUDA's device-allocation documentation](https://docs.nvidia.com/cuda/archive/13.0.0/cuda-c-programming-guide/index.html#dynamic-global-memory-allocation-and-operations).

Emitting CUDA C++ for `nvcc` is a practical candidate for an NVIDIA backend. HIP
and Metal require their own lowering and toolchain work. This preserves readable
host C and inspectable device source, but extends the build beyond plain C. It
does not require implementing PTX or another device ISA directly.

The existing `--emit=csrc` workflow is a useful precedent for handing source to
another toolchain. GPU builds additionally need separate host and device target
selection, architecture/features, artifact loading and packaging, toolchain
versioning and cache keys. A single host `--target` does not supply those rules.

## 6. Recommended sequence and acceptance criteria

1. **Prove one runtime integration with external kernels.** Choose hardware,
   load a separately compiled vector-add kernel and its signature, allocate and
   upload inputs, launch synchronously, download and compare with a CPU result.
   Include non-multiple block sizes, an empty workload, invalid arguments and
   cleanup on errors. Decide whether the initial dependency is a vendor runtime
   binding or a Mojo wrapper using an actual build/deployment probe.
2. **Put the builder facade over the proven operations.** Specify handle types,
   checked conversions, descriptor ownership and error propagation. Exercise the
   implicit-narrowing case above, wrong signatures and buffer/context mismatches.
   Keep host validation testable without a GPU; gate execution tests on hardware.
3. **Specify and implement a small Aether kernel subset.** Lower to one vendor
   source language, with diagnostics for unsupported host features. Check results
   against CPU and external-kernel references before adding more targets or
   language features.
4. **Add asynchronous execution and additional backends when workloads need
   them.** Define completion and retention contracts first. Measure transfers,
   submission and synchronization as well as kernel time, and include a workload
   that keeps data on the device across several launches.

The launch DSL is the part Aether already has the language machinery to express.
The work to design and implement the GPU ABI, resource lifecycle and kernel
semantics remains substantial even when borrowing another system's lessons.

## See also

- [`../closures-and-builder-dsl.md`](../closures-and-builder-dsl.md) — the
  immediate trailing-block and builder mechanisms used by the proposed facade.
- [`closure-lineage-and-runtime-tradeoffs.md`](closure-lineage-and-runtime-tradeoffs.md)
  — the host closure representation and its lifetime tradeoffs.
