<!-- Cross-reference: Zym vs. Aether comparison (full menu of features to consider/skip) -->
<!-- Lifted from issue body so the comparison lives next to the code, discoverable for future contributors. -->

# Aether vs Zym, comparison and lift candidates

> **Status:** Design-exploration survey (May 2026). A record of what was considered from Zym and what, if anything, was worth adopting into Aether, not a roadmap. Adopted items are noted inline; anything not yet shipped is a candidate, not a commitment.
>
> Source project: Zym, <https://github.com/zym-lang/zym>.

This survey draws on Zym's public repository (README + `src/` the CLI
shell and native bindings). The `zym_core/` submodule (compiler+VM) was
not examined, so anything below about the internals is inferred from the
public C ABI in `src/natives/natives.h` and behavior described in the
README.

The document is structured to let an Aether implementer skim "what is Zym
doing that Aether isn't?" and pull in the pieces worth lifting, with
enough detail to actually write the patch.

---

## TL;DR, one-paragraph picture

Zym is a **bytecode-compiled embeddable scripting language** (think
Lua/Wren niche). It is *not* a systems language and *not* a C-emit
compiler, it has its own VM (`ZymVM`), its own bytecode format with a
`ZYM\0` magic header, and its primary distribution story is "pack the
bytecode after the runtime binary's `.text` and detect it via a 12-byte
trailer at exe end." Three things in Zym are genuinely interesting and
**not** in Aether today, in priority order: (1) the
**single-binary-pack** story, (2) **delimited continuations + a
yield/resume status loop in every embed call site**, and (3) a
**nested-VM-as-script-value** primitive (`ZymVM()` from a script returns
an object with `.compileSource() / .load() / .call(name, args...)`,
giving you a sandbox boundary you can cross with marshalled values). The
rest (preemptive instruction-count time slicing, `@tco aggressive`
directive, `Cont.newPrompt` / `Cont.capture`, custom allocator hook,
PGO-ready CMake, "Tiny" build flavor) are smaller wins worth grabbing
opportunistically.

---

## Side-by-side feature matrix

Read this as: "if Aether already has it, skip; if not, see the section
below."

| Capability                           | Aether                                       | Zym                                         | Lift?                            |
| ------------------------------------ | -------------------------------------------- | ------------------------------------------- | -------------------------------- |
| Compilation target                   | C source → native binary                     | Bytecode → VM                               | n/a (different niche)            |
| Distribution: standalone exe         | `--emit=exe` produces native binary directly | Pack bytecode into runtime binary trailer   | **Maybe, see §1**               |
| Distribution: portable bytecode      | none                                         | `.zbc` files                                | No (Aether has native exes)      |
| Embedding C ABI                      | `--emit=lib` → `aether_<name>` exports       | `zym_newVM()` / `zym_call()` / `ZymValue`   | **Yes, see §2**                 |
| Capability gating                    | `--emit=lib` capability-empty + `--with=`    | None, full host access from any script     | n/a (Aether's story is stronger) |
| Cross-VM isolation                   | None (single emit, single binary)            | Nested `ZymVM()` from script                | **Yes, see §3**                 |
| Coroutines / continuations           | Actors + `send`/`receive`                    | Delimited continuations (`Cont.newPrompt`)  | **Maybe, see §4**               |
| Preemptive scheduling                | None (actors are cooperative)                | Instruction-count time slicing              | **Maybe, see §5**               |
| TCO                                  | Implicit (compiler decides)                  | Explicit `@tco aggressive` directive        | Probably no, see §6             |
| Status-driven yield/resume loop      | None                                         | `ZYM_STATUS_YIELD` + `zym_resume()`         | **Yes, see §2.3**               |
| Custom allocator hook in CLI         | None visible                                 | `ZymAllocator` struct, default malloc shim  | Low priority, see §7            |
| PGO build mode                       | None visible                                 | `PGO_MODE=GEN/USE` cmake flag               | Low priority, see §7            |
| "Tiny" optimized build               | None                                         | `-Oz`+gc-sections+stripped                  | Low priority, see §7            |
| Preprocess-only / combined-only mode | `--emit=src` (?)                             | `--preprocess`, `--combined` for inspection | **Maybe, see §8**               |
| Cross-platform packing (`-r`)        | n/a (native cross-compile)                   | `-r runtime` swaps the host stub binary     | n/a                              |

---

## §1, The single-binary-pack trick

**What Zym does.** When you run `zym main.zym -o my_app`, the CLI:

1. Compiles `main.zym` to bytecode (a `ZYM\0…` blob).
2. Reads its **own executable** off disk (`/proc/self/exe` on Linux,
   `GetModuleFileNameA` on Windows), that's the "stub."
3. Concatenates: `[runtime stub bytes][bytecode bytes][4-byte LE size][8-byte magic "ZYMBCODE"]`.
4. Writes it as the output exe.

At launch, `main.c` calls `has_embedded_bytecode()`
(`src/runtime_loader.c:120`) which seeks 12 bytes from the end of its
own file and checks the magic. If present, `runtime_main()` extracts
the bytecode, deserializes a `ZymChunk`, and runs it. Otherwise it
falls back to `full_main()` (the normal compiler CLI). One binary, two
modes, decided by self-inspection.

```c
// src/runtime_loader.c, the trailer format
// Bytecode package format:
// [bytecode][4B size little-endian][8B magic "ZYMBCODE"]
#define FOOTER_MAGIC "ZYMBCODE"
#define FOOTER_SIZE 12
```

**Why Aether might want this, or not.** Aether already has
`--emit=exe`. The pack trick is mostly relevant if Aether ever gets a
**REPL or eval-string mode** that compiles at runtime: then a
"compile-once, distribute-as-attached-blob" path lets users ship
without `aetherc` on the target. There's also a cross-platform angle
Zym uses (`-r runtime.exe` swaps in a Windows runtime stub when packing
on Linux), Aether sidesteps this because it generates C and lets the
host compiler cross-compile.

**Verdict:** **Don't lift wholesale.** It's a workaround for not having
a native-emit story, which Aether already has. The *one* piece worth
borrowing is the **self-inspecting trailer pattern** if you ever want
to embed a JSON/asset/config blob into the binary post-build without
re-linking, same trick, different payload.

If you do borrow it: keep the magic ASCII (so `strings` shows it, easy
forensics), keep the size field LE (matches every other Aether on-disk
format), and **use a different magic** than `ZYMBCODE` so cross-tool
detection doesn't false-positive (e.g. `AEBLOB\0\0` or
`AETHER_BLOB`).

---

## §2, The embedding C ABI shape

This is the most concrete thing to study, because Aether has an
embedding story (`--emit=lib` + `aether_<name>` exports) but it is
**generation-time**, the C side calls fixed exports. Zym's API is
**reflective**, the C side asks the VM "do you have a function called
`X` with arity 3?" and calls it by name. That difference matters for
plugin hosts where the script is supplied by the user.

### 2.1 Core types and lifecycle

```c
// from src/natives/natives.h and call sites in runtime_loader.c

typedef struct ZymVM ZymVM;
typedef struct ZymChunk ZymChunk;
typedef uintptr_t ZymValue;       // tagged value, opaque to embedders
typedef enum { ZYM_STATUS_OK, ZYM_STATUS_YIELD, /* errors */ } ZymStatus;

// Allocator (CLI passes one, library users can pass NULL = default)
typedef struct {
    void* (*alloc)  (void* ctx, size_t size);
    void* (*calloc) (void* ctx, size_t count, size_t size);
    void* (*realloc)(void* ctx, void* p, size_t old_sz, size_t new_sz);
    void  (*free)   (void* ctx, void* p, size_t size);
    void* ctx;
} ZymAllocator;

ZymVM*    zym_newVM(ZymAllocator*);            // pass NULL for default
void      zym_freeVM(ZymVM*);
ZymChunk* zym_newChunk(ZymVM*);
void      zym_freeChunk(ZymVM*, ZymChunk*);

// Compile pipeline, three phases with intermediate artifacts
ZymStatus zym_preprocess(ZymVM*, const char* src, ZymLineMap*, const char** out);
ZymStatus zym_compile   (ZymVM*, const char* combined_src, ZymChunk*,
                         ZymLineMap*, const char* entry_path,
                         ZymCompilerConfig {.include_line_info});
ZymStatus zym_serializeChunk  (ZymVM*, ZymCompilerConfig, ZymChunk*,
                               char** out_bytes, size_t* out_size);
ZymStatus zym_deserializeChunk(ZymVM*, ZymChunk*, const char* bytes, size_t);

// Run
ZymStatus zym_runChunk(ZymVM*, ZymChunk*);
ZymStatus zym_resume  (ZymVM*);                   // on ZYM_STATUS_YIELD
bool      zym_hasFunction(ZymVM*, const char* name, int arity);
ZymStatus zym_call    (ZymVM*, const char* name, int argc, ...);
ZymValue  zym_getCallResult(ZymVM*);
```

**What's notable for Aether:**

- **Three-stage pipeline (preprocess / compile / serialize) is each
  separately exposed.** `aetherc` today is a one-shot CLI; if Aether
  ever wants to be embeddable as a library (host calls into libaetherc
  to compile a string), this is the right shape: each stage allocates
  intermediate artifacts the caller frees explicitly. Note Zym always
  passes the same `ZymVM*` across stages, the VM is the *arena*, not
  just the runtime.
- **`include_line_info` is a config flag**, not a separate "strip"
  pass. Aether could use this for `--emit=lib` to optionally drop debug
  symbols from the C output without a post-process step.
- **`ZymCompilerConfig` is a struct, not flags.** Cheap thing to
  borrow: a `struct AetherCompileConfig { bool include_line_info;
  bool with_fs; bool with_net; bool with_os; ... }` makes the
  `--emit=lib` + `--with=` matrix expressible as a single value. Right
  now those flags are spread across `compiler/aetherc.c` argv parsing.

### 2.2 Calling script functions from C, the variadic fan-out

```c
ZymValue argv_list = zym_newList(vm);
for (int i = 0; i < argc; i++)
    zym_listAppend(vm, argv_list, zym_newString(vm, argv[i]));

if (zym_hasFunction(vm, "main", 1)) {
    ZymStatus r = zym_call(vm, "main", 1, argv_list);
    while (r == ZYM_STATUS_YIELD) r = zym_resume(vm);
    if (r != ZYM_STATUS_OK) /* error path */;
}
```

Two things to lift:

1. **`hasFunction(name, arity)` before calling**, so missing entry
   points are a graceful no-op instead of a runtime error. Aether's
   `aether_<name>` ABI gives you compile-time presence (the symbol is
   either linked or not), but for a hypothetical
   `aetherc --emit=lib --reflective` mode where script defines its own
   exports, this gating is the ergonomic shape.

2. **`(name, argc, ...)` variadic call.** Painful to use safely from C
   (no type checking), but it lets the host stay generic. Aether's
   current `aether_<name>` exports are typed by signature, which is
   *better* for safety but worse for the "the script's exports aren't
   known until it is read" case. If both are ever wanted, the Zym
   pattern is the escape hatch.

### 2.3 The yield-resume status loop, most underrated piece

Look at *every* call site in `runtime_loader.c` and `full_executor.c`:

```c
ZymStatus result = zym_runChunk(vm, chunk);
while (result == ZYM_STATUS_YIELD) {
    result = zym_resume(vm);
}
```

That `while` is everywhere. The VM can return mid-execution with
`ZYM_STATUS_YIELD` (used for delimited continuations, sleep, async I/O
hooks) and the *embedder* drives the resume. Which means a
single-threaded host can:

- Run multiple Zym scripts cooperatively by interleaving `zym_resume()`
  calls across VM instances.
- Inject a check between yields ("is the time budget exceeded?", "did the
  user cancel?").
- Implement a debugger that yields on breakpoints and pumps UI events
  during the pause.

**Aether equivalent and gap.** Aether has actors (Erlang-ish
`send`/`receive`), which solve concurrency *within* an Aether program,
but the **C-host has no equivalent re-entry point**. Once a C caller
invokes `aether_<name>(args)`, control doesn't return until the
function does. There's no "Aether yielded, run the host's UI tick, then
call back into the function."

**Recommendation:** for the embed-as-lib case, add a `aether_yield()`
runtime intrinsic and a corresponding C-side resume function:

```c
// Proposed Aether C-side embedding API
typedef enum { AETHER_OK, AETHER_YIELD, AETHER_ERR } aether_status;

aether_status aether_call(struct aether_vm*, const char* name, ...);
aether_status aether_resume(struct aether_vm*);
```

This is roughly the work Lua does with `lua_yield` / `lua_resume`. The
actor model handles the script-internal case; this handles the
script-to-host case. Worth a roadmap entry,
probably P3, gated on someone needing it. The CHANGELOG has no record
of this being requested yet.

### 2.4 Native function declaration via signature strings

Look at `src/natives/natives.c`:

```c
zym_defineNative(vm, "fileOpen(path, mode)", nativeFile_open);
zym_defineNative(vm, "ProcessSpawn(command, args, options)", nativeProcess_spawn);
zym_defineNativeVariadic(vm, "print(...)", nativePrint);
```

Native functions are registered with a **string signature** that
encodes both the name and the parameter names. This gives you free
documentation, free arity inference, and overload dispatch by arity:

```c
// Same name, different arities, the VM dispatches by argc
zym_defineNative(vm, "ProcessSpawn(command)",                  nativeProcess_spawn_1);
zym_defineNative(vm, "ProcessSpawn(command, args)",            nativeProcess_spawn_2);
zym_defineNative(vm, "ProcessSpawn(command, args, options)",   nativeProcess_spawn);
```

There's also explicit dispatcher construction visible in
`ZymVM.c:784–794`:

```c
ZymValue call_dispatcher = zym_createDispatcher(vm);
zym_addOverload(vm, call_dispatcher, call_0);
zym_addOverload(vm, call_dispatcher, call_1);
// ... up to call_8
```

**Lift candidate:** Aether currently registers `extern` functions with
typed C signatures (`extern fs_read_binary_raw(path: string) -> ptr`).
That's strictly better for type safety. But the **dispatcher pattern**
(one logical name, many arity-specialized implementations selected at
call time) is something Aether currently fakes by giving each variant a
distinct name (`fs.read_binary` vs `fs.read_binary_to`). Worth thinking
about whether `extern` blocks should support a `dispatch` keyword that
groups them, but this is purely sugar and low priority unless a
downstream user (svn-aether, for instance) asks.

---

## §3, Nested VMs as a script-callable value

This is the most original idea in Zym and the one most worth lifting.

```javascript
// In Zym source, from src/natives/ZymVM.c:737–814 readout
var sandbox = ZymVM();             // creates a fresh, nested ZymVM
var bc = sandbox.compileSource("func add(a,b) { return a+b; }");
sandbox.load(bc);                  // or sandbox.loadSource("...") in one shot
if (sandbox.hasFunction("add", 2)) {
    sandbox.call("add", 3, 4);
    var result = sandbox.getCallResult();   // → 7
}
sandbox.end();                     // tear down nested VM
```

The script gets an object with **`.compileFile / .compileSource /
.load / .loadFile / .loadSource / .hasFunction / .call(...) /
.getCallResult / .end`**. The C implementation (`ZymVM.c`) creates a
real second `ZymVM` via `zym_newVM(NULL)`, registers all natives in it
(`setupNatives(vmdata->vm)`), and exposes it through a closure-context
pattern.

**Cross-VM marshalling** is where this gets interesting
(`src/natives/marshal.c`):

- Primitives (null, bool, number) cross by value, same `ZymValue`.
- Strings are **copied** into the target VM's allocator
  (`zym_newString(target_vm, str)`).
- Lists and maps are **deep-copied recursively**.
- Buffers are deep-copied via the BufferData struct.
- **Functions, structs, enums are explicitly NOT marshallable**, the
  call sites all check and return a runtime error:
  ```c
  zym_runtimeError(parent_vm,
      "Cannot pass unsupported type to nested VM "
      "(functions, structs, enums not supported)");
  ```

This gives you a **value-only boundary**: parent and child cannot share
references, only copies. The child VM's heap is independent. Closures
can't cross. That's a real isolation guarantee.

There's also a concession to ergonomics: Zym registers `call_0` through
`call_8` (each a separate C function, lots of repetition, see
`ZymVM.c:212–584`) and unifies them via a runtime dispatcher. If
Aether picks this up, varargs in the C runtime would let you collapse
those into one. Or just generate the boilerplate.

### Why Aether should care

Aether's current sandbox story is:

1. **Compile-time:** `--emit=lib` is capability-empty by default,
   `--with=fs[,net,os]` opts in at the *build* level.
2. **Lexical:** `hide` / `seal except` deny names per-block at compile
   time.
3. **Runtime:** `libaether_sandbox.so` LD_PRELOAD checks libc calls
   against a builder-DSL grant list.

What's *missing* is a **runtime-spawnable sandbox**, "an Aether program
that wants to load *a user's untrusted Aether code* into a walled-off
sub-VM and call functions on it." You can do something
similar today with the in-process language hosts
(`contrib.host.python.run_sandboxed(perms, code)` etc.) but Aether
hosting Aether currently requires a separate process. The README and
`LLM.md` both note this limitation:

> Java/Go/aether-hosts-aether are separate-process.

Lifting Zym's pattern would give you **in-process aether-in-aether** with
the same value-only isolation. Concretely:

```aether
// Hypothetical Aether API
import std.sandbox

let child = sandbox.new_aether_vm(grants={fs: ["/tmp/work"]})
child.compile_and_load("func add(a: int, b: int) -> int { ... }")
let r = child.call_int_int_int("add", 3, 4)   // typed, unlike Zym
child.end()
```

Differences from Zym you'd want for Aether:

- **Typed call surface, not variadic.** Aether already requires arity
  *and* type info to mangle exports. So `child.call_int_int_int(name,
  3, 4)` or a generated typed binding, not Zym's `call(name, ...)`.
- **Reuse the existing grant-list DSL.** The child VM should accept the
  same builder-DSL grants `libaether_sandbox.so` already understands;
  don't invent a parallel permissions model.
- **Cross-VM marshalling rules.** Same as Zym: primitives by value,
  strings/lists/maps/`*StringSeq` deep-copied, **closures and actor
  refs do not cross**. Actor refs especially, that would punch a hole
  through isolation.
- **Lifecycle parity with grant-list.** Closing the child VM should
  release any grants scoped to it; opening one inside a `seal except`
  block should inherit the seal.

This is genuinely novel work and probably the single highest-leverage
thing to lift from Zym. Estimate it as a P2 item gated on a real use
case, the svn-aether port doesn't need it (they're the host), but a
plugin-host scenario (e.g., an Aether-written editor loading
user-supplied Aether macros) would.

A docs/sandbox-aether-in-aether.md alongside
`docs/containment-sandbox.md` would be the natural home for the spec.

---

## §4, Delimited continuations

Zym ships `Cont.newPrompt` / `Cont.capture` as first-class primitives
(README example). The use case advertised is "build fibers, coroutines,
generators, custom schedulers from scratch":

```javascript
var tag = Cont.newPrompt("fiber");
func yield() { return Cont.capture(tag); }

func worker(name) {
    print(name + ": step 1");
    yield();                  // suspends, returns control to scheduler
    print(name + ": step 2");
    yield();
}
```

**Aether equivalent and gap.** Aether has actors with `send`/`receive`,
which is a *higher-level* concurrency primitive than continuations.
Anything you can build with delimited continuations in Zym
(generators, async/await, custom schedulers), you can build with
actors in Aether, usually more clearly.

**The case for lifting:** delimited continuations let you implement
the C-host yield/resume hook (§2.3) cleanly *inside* the language,
`aether_yield()` from Aether code becomes "capture continuation up to
the embedder's prompt." Without continuations, every yield point has
to be a compiler-known intrinsic (sleep, await-message, etc.).

**The case against:** delimited continuations are notoriously hard to
implement on top of a C-emit compiler. Zym can do it because it has a
VM with explicit stack frames the runtime owns. Aether emits C, where
the *C compiler* owns the stack. You'd need stack-copying tricks
(à la libdill) or CPS-transform the entire codebase. Both have real
costs and break C interop in subtle ways.

**Verdict:** **don't lift directly.** If the C-host yield case in §2.3
ever becomes real, do it the Lua way, explicit yield/resume points
that the codegen knows about, not general first-class continuations.
File this under "interesting but wrong tool for Aether's design."

---

## §5, Preemptive instruction-count time slicing

README claim:

> The VM supports instruction-count-based time-slicing. Run untrusted
> code or build fair multi-tasking systems without cooperative yields.

Implementation isn't visible (it's in the unloaded `zym_core/`
submodule), but the shape is clear: every N bytecode instructions the
dispatcher checks a budget and yields if exceeded. This is **not**
cooperative, the script can't refuse.

**Aether equivalent and gap.** None. Actors yield cooperatively at
`receive` points. A tight loop like `while (true) {}` in Aether will
peg a CPU.

**Why this matters:** combine §3 (in-process sandbox) with §5
(preemption) and you have **runaway-script-proof embedding**. Without
preemption, your "sandbox" can DoS the host with `for(;;);`. The
LD_PRELOAD layer doesn't help, it gates syscalls, not CPU.

**The cost for Aether:** Aether emits C. There is no bytecode
dispatcher to hook. To do this for native-emit, you'd need:

1. The codegen to emit periodic budget-checks (every N basic blocks,
   or at every back-edge). This is doable but adds runtime overhead
   measurable in benchmarks, even a counter increment + branch on
   every loop iteration costs.
2. A signal-based alternative: `SIGALRM` from a timer thread, signal
   handler sets a flag, codegen checks it at safe points. Lower
   steady-state cost but harder to make correct (signal-safety,
   actor-context interactions).

**Verdict:** **only lift if §3 lifts.** A sandboxed nested VM is the
*reason* you'd want preemption. Without that use case, the cost is
unjustified. If you do lift §3, file preemption as a follow-up, start
with the back-edge counter approach (simpler, predictable), measure
overhead, decide whether to escalate to SIGALRM.

A `--with=time-slicing=N` build flag (compiles in the budget checks)
keeps the cost opt-in for embedders who want it.

---

## §6, `@tco aggressive` directive

```javascript
@tco aggressive
func sum(n, acc) {
    if (n == 0) return acc;
    return sum(n - 1, acc + n);
}
print(sum(1000000, 0));   // no stack overflow
```

Script-directed TCO opt-in. The compiler will normally TCO when it
*can* prove the call is in tail position; `@tco aggressive` forces it
even when the compiler is conservative.

**Aether equivalent and gap.** Aether emits C. C compilers (clang/gcc)
do TCO opportunistically with `-O2`; Aether could in principle emit
musttail-style hints. But Aether's current generated C is
hand-readable on purpose, and `__attribute__((musttail))` (clang) /
`[[clang::musttail]]` ties you to one compiler.

**Verdict:** **probably no.** If a downstream user files an issue
about a deep-recursion overflow, revisit. The svn-aether port hasn't
hit it. Easier escape hatches: rewrite to iteration; if the
recursion shape is known, codegen a trampoline.

If you *did* want to lift it, the syntax is fine,
`@tco(mode="aggressive")` as an Aether attribute on a function is
clean. But save the cost for when there's demand.

---

## §7, Build-system polish (low priority)

Three small things in `zym/CMakeLists.txt` worth a glance:

### 7.1 PGO mode

```cmake
set(PGO_MODE "OFF")
if(PGO_MODE STREQUAL "GEN")
    # adds -fprofile-generate
elseif(PGO_MODE STREQUAL "USE")
    # adds -fprofile-use -fprofile-correction
endif()
```

Two-stage build: instrument, run a representative workload, rebuild
using the profile. Worth ~5–15% on hot-loop-heavy code.

For Aether: a `make pgo-gen && ./build/aetherc tests/regression/*.ae &&
make pgo-use` cycle would be straightforward to add. Use the
JSONTestSuite run as the representative workload, it's already in CI
and exercises the parser + emitter heavily.

Low priority but cheap.

### 7.2 "Tiny" build flavor

```cmake
set(CMAKE_C_FLAGS_TINY "-Oz -DNDEBUG -ffunction-sections -fdata-sections \
    -fno-exceptions -fvisibility=hidden -fno-asynchronous-unwind-tables \
    -fno-unwind-tables")
set(CMAKE_EXE_LINKER_FLAGS_TINY "-Wl,--gc-sections -Wl,-s -Wl,-Map,zym_cli.map")
```

Optimize for size, gc unused sections, strip. Useful if `aetherc` is
being shipped in containers or embedded contexts.

For Aether: less compelling, `aetherc` is already small (it's
hand-written C) and the *output* binary size is dominated by user code,
not the compiler. Skip unless someone asks.

### 7.3 Custom allocator hook

Already covered in §2.1, `ZymAllocator` is passed in at VM creation.
The Aether C runtime uses `malloc` directly in lots of places
(`std/string/aether_string.c`, etc.). Threading an allocator through
would be a big mechanical change with unclear payoff. **Skip.**

---

## §8, `--preprocess` and `--combined` inspection modes

```text
zym <file.zym> --preprocess           # show preprocessed source
zym <file.zym> --combined             # show source after module concatenation
zym <file.zym> --preprocess <out.zym> # write to file
```

Lets users see exactly what the compiler sees after preprocessing /
module loading. This is *very* useful for debugging weird parser errors
and for filing bug reports, instead of "the parser rejects this code,"
a reporter can attach the post-preprocess output that actually reaches
the compiler.

Aether already has a preprocessor (or its equivalent, directives,
includes, macro expansion all happen somewhere in `compiler/`). It is
not clear from the LLM.md what the user-facing flag is, if any.

**Recommendation:** if there isn't already an `aetherc --emit=preproc`
or similar, **add one**. Cheap to implement (the data is on the heap
already, just dump it to stdout or a path), invaluable for triage.
Pattern:

```text
aetherc foo.ae --emit=preproc           # → stdout
aetherc foo.ae --emit=preproc -o foo.preproc.ae
aetherc foo.ae --emit=combined          # post-import-resolution
```

Worth noting on the backlog as a P3 ergonomics item.

---

## §9, Things Aether does that Zym does *not* (so don't regress)

Quick inventory so this comparison isn't one-sided.

- **Native compilation.** Aether → C → native binary. Zym requires its
  VM at runtime. Aether's deployment story is strictly simpler for
  systems work.
- **Capability-empty `--emit=lib`.** Zym's library mode (well, embed
  mode) gives the script *full* host access by default, every `fileX`,
  `ProcessSpawn`, `processEnv`, `processExit` is registered
  unconditionally in `setupNatives`. There is no "default-deny." This
  is a real safety advantage for Aether in the embed-untrusted-scripts
  case.
- **Lexical `hide` / `seal except`.** No equivalent in Zym. Once a
  symbol is in scope, it's in scope.
- **LD_PRELOAD `libaether_sandbox.so`.** Catches violations *below* the
  language layer. Zym has nothing comparable; if you exec via
  `ProcessSpawn`, it just calls `posix_spawn` no syscall gate.
- **Actor model with typed messages.** Higher-level concurrency
  primitive than Zym's continuations.
- **Strict typing.** Zym is dynamically typed (`var x = ...`). Aether
  is statically typed.
- **Go-style `(value, err)` returns + empty-string-as-success
  convention.** Aether has a single canonical error idiom across
  `std`. Zym mostly uses C-style sentinel returns and exceptions.
- **`*StringSeq`** as a refcount-aware, structurally-shared, O(1)
  head/tail string sequence. No equivalent in Zym; lists are mutable
  arrays.

Don't trade any of these for Zym's features. The lifts in §1–§8 are
**additive**, every one of them can coexist with Aether's existing
guarantees.

---

## §10, Ranked lift list

If you only have time for one: **§3 (nested-VM-as-script-value)**.
That's the genuine novelty.

Full ranked priority for the Aether roadmap:

| Pri | Item                                            | Effort | Gating signal                                             |
| --- | ----------------------------------------------- | ------ | --------------------------------------------------------- |
| P2  | §3 nested aether-in-aether VMs (in-process)     | Large  | Real plugin-host use case, or downstream wish file        |
| P3  | §2.3 `aether_yield()` / `aether_resume()` C ABI | Medium | Embedder asks for it; pairs with P2                       |
| P3  | §8 `--emit=preproc` / `--emit=combined`         | Small  | Ergonomics, do it next time you touch `aetherc.c`        |
| P3  | §2.1 `AetherCompileConfig` struct unification   | Small  | Refactor, bundle when next adding a `--with=` capability |
| P4  | §5 instruction-budget preemption                | Large  | Only after §3 lands                                       |
| P4  | §7.1 PGO build mode                             | Small  | Only if benchmarks say it's worth it                      |
| ✗   | §1 self-extracting binary trailer               |, | Aether emits native, not needed                          |
| ✗   | §4 delimited continuations                      |, | Wrong tool for C-emit                                     |
| ✗   | §6 `@tco aggressive`                            |, | No demand                                                 |
| ✗   | §7.2 "Tiny" build                               |, | No demand                                                 |
| ✗   | §7.3 custom allocator hook                      |, | Too invasive                                              |

---

## Appendix A, files in zym to read if you're implementing §3

In the [zym-lang/zym](https://github.com/zym-lang/zym) repository:

- `src/natives/ZymVM.c` (entire file, 814 lines), the native binding
  that exposes `ZymVM()` to scripts. Pattern-match the
  `nativeZymVM_create` builder, the `VMData` struct, the `CREATE_METHOD`
  macro at line 760, and the dispatcher construction at 784–794.
- `src/natives/marshal.c` (149 lines), the cross-VM value-copy logic.
  `marshal_reconstruct_value` at line 125 is the dispatch point;
  primitives pass through, containers recurse, unsupported types
  return null and the call sites check for it.
- `src/runtime_loader.c` (the `while (result == ZYM_STATUS_YIELD)` loop
  pattern, lines 172–175 and 189–192), the embedder-side yield/resume
  shape that §2.3 recommends.
- `src/natives/natives.c` (58 lines), the native registration table.
  Useful as a reference for how Zym names script-facing functions; the
  signature-string convention (`"fileOpen(path, mode)"`) is something
  Aether's `extern` blocks could optionally adopt for documentation
  purposes.

The actual VM internals (instruction dispatch, GC, continuation
machinery) are in the `zym_core/` submodule which wasn't checked out
when this comparison was written. If you need to lift §5 (preemption)
specifically, fetch that submodule first, the budget-check site will
be in the dispatch loop.

## Appendix B, what was *not* examined

- `zym_core/` the compiler+VM. Submodule not checked out. Anything
  about Zym's internal architecture (GC strategy, dispatch model, exact
  bytecode format beyond the `ZYM\0` header) is inferred from external
  signals only.
- Zym's actual language semantics beyond what the README shows. The
  README claims first-class structs, enums, closures, pattern matching
  via `[h|t]`-style destructuring (not directly shown, that's actually
  Aether's syntax, just confirming neither file claims this for Zym),
  and modules. Any deeper claim ("Zym's type coercion rules,"
  "operator precedence," etc.) would need the unloaded submodule.
- Zym's documentation site (`zym-lang.org`), not fetched. README
  links to `/docs-language.html` and `/docs-embedding.html` for the
  authoritative guide.

If this comparison is going to drive a real implementation, validate
the lift candidate against the live submodule before committing, the
README and the `src/natives/` C ABI are consistent and recent, but the
shape of the bytecode dispatcher and the continuation machinery
matters for any preemption or yield work.

---

## TL;DR for triage

The full doc above is a survey of [Zym](https://github.com/zym-lang/zym), a bytecode-compiled embeddable scripting language (Lua/Wren niche, not a systems language). Like the Flux, Fir, and Flint comparisons, it's written to let Aether decide deliberately what to absorb.

**Key shape difference from the prior surveys**: Zym is a different niche (embed-a-script-VM-in-host, not compile-to-native). Most of its features either don't apply (single-binary trailer, bytecode portability) or are already covered better by Aether (capability discipline, actor concurrency). The doc is more selective in what it recommends.

**The doc's own ranked lift list (§10):**

| Pri | Item | Effort | Gating signal |
|---|---|---|---|
| **P2** | §3 nested aether-in-aether VMs (in-process sandbox) | Large | Real plugin-host use case, or downstream wish file |
| P3 | §2.3 `aether_yield()` / `aether_resume()` C ABI | Medium | Embedder asks for it; pairs with P2 |
| P3 | §8 `--emit=preproc` / `--emit=combined` | Small | Ergonomics, do it next time you touch aetherc.c |
| P3 | §2.1 `AetherCompileConfig` struct unification | Small | Refactor, bundle when next adding a --with= capability |
| P4 | §5 instruction-budget preemption | Large | Only after §3 lands |
| P4 | §7.1 PGO build mode | Small | Only if benchmarks say it's worth it |
| ✗ | §1 self-extracting binary trailer |, | Aether emits native, not needed |
| ✗ | §4 delimited continuations |, | Wrong tool for C-emit |
| ✗ | §6 `@tco aggressive` |, | No demand |

## The headline lift: §3 nested in-process VMs

If only one item from this survey lands, it's §3, Zym's pattern of `var sandbox = ZymVM()` from script code, returning an object with `.compileSource() / .load() / .call(name, args)` and a value-only marshalling boundary (primitives copy, strings/lists deep-copy, **closures and refs do not cross**).

This would give Aether **in-process aether-in-aether sandboxing**, currently the README and LLM.md both note that Aether-hosts-Aether requires a separate process. Real use case: an Aether-written editor loading user-supplied Aether macros, a plugin host loading untrusted user scripts, etc. The svn-aether port doesn't need it (they're the host, not the host-of-untrusted-code), so this is gated on a downstream demand surfacing.

**Why this is not filed as a focused issue**: P2/large effort + explicit "only worth doing if a real use case shows up" gate. Filing prematurely would create implementation pressure without justification. Flag for separate issue when (if) someone files a wish for it.

## Note on overlap with prior surveys

The four comparison docs (Flux, Fir, Flint, Zym) each pull on different parts of the design space:

- **Flux**: bit-precise types (`data{N}`), `from`-cast, binary protocol parsing
- **Fir**: row-typed errors, `#[derive(...)]`, anonymous records, type-system ergonomics
- **Flint**: optionals, variants, FIP C-bindings, runtime-shape ergonomics
- **Zym**: nested-VM sandboxing, yield/resume C ABI, embedder-shape extensions

Zym is the most distant from Aether's identity (different compilation model). Its useful contributions are mostly **architectural patterns** (the value-only marshalling boundary, the yield/resume status loop) rather than language features.

## What this issue is NOT asking

- Not a roadmap. "Use this as a menu, not a roadmap."
- Not asking for any single feature to ship as-listed.
- Not litigating the explicit-skip items (§1, §4, §6, §7.2, §7.3). All correctly identified as wrong-fit for Aether's design.

## Cross-refs

- Zym source: [zym-lang/zym](https://github.com/zym-lang/zym).
- Sister surveys: Flux, Fir, Flint
- §3 nested-VM proposal: not filed as focused issue, gated on real use case
- §8 `--emit=preproc`: small standalone ergonomics; could be its own focused issue if you want to greenlight it independently of the rest of the survey
