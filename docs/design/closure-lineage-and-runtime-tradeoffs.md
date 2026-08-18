# Closure lineage and runtime tradeoffs

Lisp and Smalltalk are the Adam and Eve of modern high-level language
design: Lisp formalised code-as-data and lexical closure; Smalltalk
formalised the live object world where blocks participate uniformly in
message passing. Aether closures sit in an awkward but deliberate place
downstream of both: they keep the surface shape of Lisp/Smalltalk closures,
but they do not assume a runtime that owns every closure environment for the
program's lifetime. They compile to plain C data and functions, so the
compiler has to decide when each captured environment lives, escapes, and
dies.

That choice explains both the attraction and the recurring sharp edges.

## The ancestors

Lisp established the core closure idea: a function value paired with a
lexical environment. A closure can be returned, stored, called later, and
kept alive by the runtime as long as anything can reach it.

Smalltalk made blocks participate in the object system. A block is a
first-class receiver; control flow itself is often expressed by sending
messages to blocks (`ifTrue:`, `whileTrue:`, `do:`). The closure is not a
special interop shape bolted onto the side of the language. It is just
another object in a heap-managed image.

The shared assumption is a uniform runtime value model:

- closure environments are heap objects;
- bindings are cells, so later mutation is visible through the closure;
- lifetime is automatic;
- functions, lambdas, and blocks are interchangeable at the call site.

## Aether's position

Aether wants the useful part of that lineage for systems code: deferred
callbacks, trailing-block DSLs, builder-style configuration, and functions
that can carry lexical context.

But Aether also wants to compile to C, cross the C ABI cleanly, avoid a GC,
and keep the runtime small. Its closure value is therefore concrete:

```c
typedef struct {
    void (*fn)(void);
    void *env;
} _AeClosure;
```

A capturing closure gets a generated environment struct and a generated C
function. Zero-capture closures can collapse toward a bare function pointer.
Captured scalar locals are captured by value; shared mutable state is made
explicit with `ref()` cells or pointers.

The one-line placement is:

> Aether is C with closures whose environments are structs whose lifetime is
> mostly inferred by the compiler.

That puts it closer to Rust and Zig than to Lisp or Smalltalk:

| Language family | Closure representation | Lifetime owner | Cost paid |
|-----------------|------------------------|----------------|-----------|
| Lisp / Scheme | function + heap environment | GC/runtime | heap pressure, runtime dependency |
| Smalltalk | block object in image | object runtime | image/VM dependency |
| Rust | anonymous structs implementing `Fn` traits | borrow checker / ownership | type-system complexity |
| Zig | usually explicit function pointer + context pointer | programmer | manual plumbing |
| Aether | `_AeClosure { fn, env }` + generated env struct | compiler heuristics + explicit ownership rules | lifetime edge cases and FFI seams |

## Prior no-runtime closure attempts

Aether is not the first no-GC or no-VM language to lower closures into
plain data plus code. That small print matters:

- **C++ lambdas** are compiler-generated function objects. Captures become
  fields on an unnamed closure type, and a non-capturing lambda can decay to
  a plain function pointer. C++ gives the programmer value/reference capture
  control, but a reference-capturing lambda that outlives the referenced
  stack frame is undefined behavior. The language has closure objects, but
  it does not make Smalltalk/Ruby-style trailing-block DSLs the center of
  the surface.
- **D delegates** are close mechanically: a function pointer paired with a
  context pointer. Modern D can move captured stack state to the heap when a
  delegate escapes. The shape is very relevant to Aether, though D's broader
  language/runtime assumptions are different.
- **Rust closures** are anonymous structs implementing `Fn`, `FnMut`, or
  `FnOnce`. Rust solves the lifetime problem by making capture mode,
  mutation, moving, and borrowing part of the type system. The use-after-free
  bugs Aether has to find in codegen are often borrow-checker errors in Rust.
- **Apple Blocks** brought closure literals to C, Objective-C, and C++ as a
  compiler/runtime extension. They use a block object representation and
  explicit copy/lifetime rules for blocks that escape their stack creation
  scope.

So Aether's novelty claim should stay narrow: it is not "the first native
language with closure structs." It is a systems language aiming those
mechanics at the Lisp/Smalltalk/Ruby lineage of closure-shaped DSLs, instead
of treating closures mainly as local callbacks, iterator glue, or manually
threaded context pointers.

## Where Aether falls short

### Closures are not fully uniform values

In Lisp and Smalltalk, returning a closure, storing it in a collection, or
passing it to another function is the same value operation. In Aether those
paths matter because each implies a different environment lifetime:

- an inline callback may be proven transient and freed after the callee
  returns;
- a returned closure must suppress the creator frame's env cleanup;
- a closure stored in a list must be boxed, and the container must own that
  box;
- a closure passed through an extern boundary needs a contract that keeps
  the environment alive for the callee's use.

Those distinctions are the price of not having a closure-aware GC own all
environments.

### Capture is by value unless state is explicit

Lisp and Smalltalk bindings behave like cells from the closure's point of
view. Aether captures ordinary scalar locals by value. If several callbacks
need shared mutation, the program says so:

```aether,fragment
count = ref(0)
button("inc") callback {
    ref_set(count, ref_get(count) + 1)
}
```

That makes the generated C representation direct and predictable, but it is
less magical than classic closures.

### Functions and closures are not perfectly interchangeable

A top-level function pointer and a capturing closure can share a surface
signature, but they do not have the same runtime shape. A raw function
pointer is just code. A capturing closure is code plus environment. The
compiler bridges the common cases, but the distinction leaks through `fn`,
`ptr`, `box_closure`, `unbox_closure`, and `call()`.

This is a real gap from Lisp and Smalltalk, where lambda/block values and
named functions live inside one runtime object model.

### Control flow is still syntax

Smalltalk can model control flow as block messages. Lisp can reshape control
flow through special forms and macros. Aether's `if`, `while`, `for`, and
`match` remain syntax. The trailing-block builder DSL recovers much of the
readable nested shape, but it is a fixed compiler-supported pattern, not an
open-ended "every block can become a new control form" system.

## Where Aether gains

The payoff is the systems-language side of the bargain:

- generated closures are plain C data and functions;
- zero-capture callbacks can avoid environment allocation;
- captured environments can cross C boundaries with explicit wrappers;
- there is no GC pause or VM image requirement;
- builder DSLs can be sandboxed by compile-time scope gates such as
  `hide` / `seal except`, plus the process-level sandbox where needed.

This is why closure bugs in Aether tend to be use-after-free, double-free,
or leak bugs rather than GC pressure or VM interop bugs. The language moved
the bill from a runtime collector into compiler analysis and explicit
ownership contracts.

## The design bet

Aether's bet is that trailing blocks, `callback` blocks, `ref()` cells,
boxed closures, and `_ctx` injection provide enough of the Lisp/Smalltalk
expressiveness to build real DSL-shaped libraries without taking on a
Lisp/Smalltalk runtime.

That bet is already visible in:

- `docs/closures-and-builder-dsl.md` the syntax and implementation shape;
- `docs/dsl-without-macros.md` why trailing-block DSLs replace many macro
  use cases;
- `docs/config-is-code.md` why Aether configuration should be executable,
  sandboxable Aether rather than YAML/HCL plus a second templating layer;
- `contrib/templating/liquid` and `contrib/parsers/xml_expat` concrete
  examples where callbacks and DSL surfaces carry structure and behavior
  without a VM.

The result is not "Lisp, but native" or "Smalltalk blocks, but faster." It
is a different point in the design space: closure-shaped values in a
no-GC, C-targeting language, with the compiler carrying as much lifetime
responsibility as it can and the programmer making the remaining ownership
edges explicit.
