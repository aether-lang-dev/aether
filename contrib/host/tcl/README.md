# contrib.host.tcl: Embedded Tcl

`import contrib.host.tcl` lets an Aether program embed Tcl in-process:
`tcl.run_sandboxed(perms, "<source>")` evaluates Tcl with
permission-checked access controls.

## How it loads libtcl

The bridge `dlopen`s libtcl at the **deploy host's** runtime: there
is no `-ltcl` on the link line. The produced binary works against
whatever Tcl minor version the deploy host has (8.5 / 8.6 / 9.0
supported).

Discovery order at first call to `tcl.run` (strict two-step):

1. `${AETHER_TCL_SONAME}` env var (orchestrator-supplied exact,
   e.g. `libtcl8.6.so`).
2. `libtcl.so` (Debian-style unversioned symlink, present with
   tcl-dev).

If both fail, `tcl.run*` returns -1 with a clear error naming the
env var. **There is no hardcoded version fallback list**: the
bridge stays distro-agnostic; the orchestrator owns the probe.

Orchestrator probe via tclsh:

```sh
AETHER_TCL_SONAME=$(echo 'puts libtcl$tcl_version.so' | tclsh)
```

## What `ae build` does for you automatically

When your program has `import contrib.host.tcl`, `ae build`:
1. Links `libaether_host_tcl.a` (the in-tree bridge) automatically.
2. Does NOT add any libtcl link flags: the bridge dlopens libtcl
   at runtime.

`ae build` errors with a clear actionable message if the bridge .a
hasn't been built: build the image with
`aether-build --with=tcl --rebuild-image` or
`make contrib MODULES=tcl && make install-contrib`.

## `aether.toml`: usually empty for tcl

```toml
# nothing required for contrib.host.tcl
[[bin]]
name = "myapp"
path = "myapp.ae"
```

If the deploy host's Tcl isn't discoverable via the default order,
set `AETHER_TCL_SONAME`:

```sh
AETHER_TCL_SONAME=libtcl8.6.so.0 ae build myapp.ae
```

## Usage

```aether
import contrib.host.tcl

main() {
    tcl.run("puts \"hello from tcl\"")
}
```

## Bidirectional embedding (#904)

`tcl.run` is enough to *run* a script. A host that uses Tcl as an extension
language, calling into it and being called back, needs a **persistent VM**
that registers host commands, injects globals, and returns **typed** results:

```aether
import contrib.host.tcl

@c_callback
app_add(vm: ptr) -> int {
    tcl.push_int(vm, tcl.arg_int(vm, 1) + tcl.arg_int(vm, 2))
    return 1
}

main() {
    vm = tcl.vm_new()
    tcl.vm_register(vm, "app.add", app_add)
    tcl.vm_set_global_int(vm, "BASE", 100)
    tcl.vm_set_instruction_limit(vm, 1000000, 100)
    tcl.vm_set_time_limit(vm, 2000, 100)

    if tcl.vm_eval(vm, "expr {[app.add 2 3] + $BASE}") == 0 {
        println("${tcl.vm_result_int(vm)}")   // 105
    }
    tcl.vm_pop_result(vm)
    tcl.vm_free(vm)
}
```

| Call | Effect |
|------|--------|
| `tcl.vm_new() -> ptr` | a new interpreter, separate from the one `tcl.run` shares |
| `tcl.vm_register(vm, name, fn_ptr) -> int` | register an Aether `(vm: ptr) -> int` `@c_callback` as the Tcl command `name`, taken verbatim: `"app.add"` is called `app.add 2 3`, and `"app::add"` lands in the namespace `::app` |
| `tcl.vm_set_global_str/_int/_strlist(vm, name, v)` | set a global variable (a string list becomes a Tcl list) |
| `tcl.vm_eval(vm, code) -> int` | run at global level; 0, or -1 with the error as the result |
| `tcl.vm_result_type(vm) -> int` | the result's tag (`TCL_T*`, below) |
| `tcl.vm_result_int/_double/_str/_bool(vm)` | read the result; valid until `vm_pop_result` or the next eval |
| `tcl.vm_pop_result(vm)` | release the result |
| `tcl.vm_set_instruction_limit(vm, budget, every) -> string` | a budget of Tcl commands per eval, checked every `every` commands; `budget` <= 0 removes it |
| `tcl.vm_set_time_limit(vm, ms, every) -> string` | a wall-clock budget per eval; `ms` <= 0 removes it |
| `tcl.arg_count/_type/_int/_double/_str/_bool(vm, i)` | **inside a callback**: the arguments, 1-based; the command's name is not one |
| `tcl.push_int/_double/_str/_bool/_nil(vm, v)` | **inside a callback**: the command's return value; the last push wins, and the callback returns 1 to return it or 0 to return nothing |
| `tcl.raise_error(vm, msg) -> int` | `return tcl.raise_error(vm, msg)`: the command fails with `msg` after the callback has returned, so its defers run; a script can `catch` it |

Tags: `TCL_TNIL`, `TCL_TBOOL`, `TCL_TINT`, `TCL_TSTR`, `TCL_TTABLE`
(a list or dict), `TCL_TERR`, `TCL_TOTHER` (an integer too wide for 64
bits), and `TCL_TFLOAT`, which Lua and Duktape do not have.

What is particular to Tcl:

- **Every value is a string.** A value's tag comes from what Tcl has
  already made of it: `expr {2+3}` is `TCL_TINT`, but the literal 5 in
  `set x 5` is still `TCL_TSTR`. The readers convert from the string either
  way, so `vm_result_int` reads both as 5. Tcl has no nil and no boolean:
  `push_nil` returns the empty string, which reads `TCL_TNIL`, and
  `push_bool` returns 1 or 0.
- **Numbers.** An integer too wide for 64 bits (`expr {2**70}`) and a double
  out of range saturate when read as int; NaN and non-numbers read 0.
- **Threads.** An interpreter belongs to the thread that created it. Every
  `vm_*` call from another thread is refused without touching Tcl:
  `vm_eval` returns -1, with an error result saying why. Keep a VM inside
  one actor, or on one thread.
- **Two limits.** The instruction limit counts Tcl commands, so a loop that
  runs none, such as `while 1 {}` (compiled to jumps), is not bounded by it:
  the time limit is. A script cannot `catch` either limit and carry on.
  Both apply to the outermost eval; an eval a callback makes runs inside
  the same budget rather than starting a new one.
- **Nested evals.** A callback may `vm_eval` on its own VM. It runs at
  global level, not in the frame of the proc that called the callback,
  and the outer call's arguments survive it.
- **Not a sandbox.** A VM's script has Tcl's full reach (`open`, `exec`,
  `socket`). For untrusted code use `run_sandboxed`, or a separate process.

## Implementation notes

- All Tcl C-API access goes through a `dlsym` function-pointer
  table: see `aether_host_tcl.c`. The bridge .a has NO unresolved
  Tcl symbols (`nm -u` confirms).
- Tcl's C API is conventionally non-macro: the headers expose
  real exported `Tcl_*` functions, no `pTHX_`-style context
  passing, no struct-tag bit-twiddling. **No macro re-expressions
  needed** (unlike python's `Py_INCREF` / perl's `SvTRUE` /
  duktape's `duk_peval_string`). The cleanest of the dlopen
  rewrites.
- `TCL_OK = 0` / `TCL_ERROR = 1` constants are stable across Tcl
  8.x and 9.x.
- The bidirectional tier keeps to the same rule. Reference counts go
  through `Tcl_DbIncrRefCount` / `Tcl_DbDecrRefCount`, the exported
  functions behind the `Tcl_IncrRefCount` macros, which write a field 9.0
  widened. The one field read directly is `Tcl_Obj.typePtr`, for a value's
  tag: it sits three pointers in on 8.6 and 9.0 alike, and a
  `_Static_assert` holds the bridge to that.

## Testing

Automated coverage today is the contrib-build smoke test at
[`tests/scripts/contrib_build.sh`](../../../tests/scripts/contrib_build.sh):
the `tcl|host_tcl …` catalogue entry builds `aether_host_tcl.c` (gated on its
`probe_tcl` Tcl-headers probe) so `make contrib` produces and links the
`libaether_host_tcl.a` bridge archive. It confirms the bridge compiles and has
no unresolved Tcl symbols, but exercises nothing at runtime.

The bidirectional tier has two tests:
[`test_host_tcl.ae`](test_host_tcl.ae), the co-located `std.spec` run by
`make contrib-host-check`, and
[`tests/integration/host_tcl_bidirectional/`](../../../tests/integration/host_tcl_bidirectional/),
a C harness for the rest: both limits, nested evals and their return codes,
the thread refusal, and memory staying flat across 60,000 rounds.

Tcl is still **not** in the cross-host shared-map test
([`tests/sandbox/test_shared_map_all.sh`](../../../tests/sandbox/test_shared_map_all.sh)).
