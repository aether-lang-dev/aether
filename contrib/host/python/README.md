# contrib.host.python: Embedded CPython

`import contrib.host.python` lets an Aether program embed CPython
in-process: `python.run_sandboxed(perms, "<source>")` evaluates Python
inside the calling process, with permission-checked access controls.

## How it loads CPython

The bridge `dlopen`s `libpython` at the **deploy host's** runtime: there is no `-lpython` on the link line. This means:

- The build environment doesn't need `python3-dev` (or any libpython
  at all). Only the headers, which are already vendored.
- The produced binary works against whatever Python minor version the
  deploy host happens to have: no ABI lock-in.
- A binary built today on a 3.11 machine and run on a 3.14 machine
  works as long as the host has a usable libpython.

Discovery order at first call to `python.run` (strict two-step):

1. `${AETHER_PYTHON_SONAME}` env var (orchestrator-supplied exact
   soname, e.g. `libpython3.14.so.1.0`). The orchestrator probes
   the host's python via sysconfig and passes this.
2. `libpython3.so` (unversioned symlink: present in the runtime
   package on Fedora-likes / Bazzite; typically -dev-only on
   Debian-likes).

If both fail, `python.run*` returns -1 with a clear error naming
the env var. **There is no hardcoded version fallback list**: the
bridge stays distro-agnostic; the orchestrator owns the probe.

Hint command for orchestrators / users setting the env var manually:

```sh
AETHER_PYTHON_SONAME=$(python3 -c 'import sysconfig; print(sysconfig.get_config_var("INSTSONAME"))')
```

## On the deploy host

You need a Python 3 runtime installed. Examples:

| Host | What ships libpython |
|---|---|
| Bazzite / Fedora-derived | `python3` (includes `libpython3.so` symlink) |
| Debian / Ubuntu | `libpython3.X` (versioned only; set `AETHER_PYTHON_SONAME=libpython3.X.so.1.0`) |
| RHEL / Rocky | `python3-libs` |

The bridge does NOT need `python3-dev` on the deploy host. Headers
are only needed at build time, and even then only by the bridge .a's
compile (which `ae build` handles transparently: see below).

## What `ae build` does for you automatically

When your program has `import contrib.host.python`, `ae build`:

1. Links `libaether_host_python.a` (the in-tree bridge) onto the
   resulting binary automatically. You do NOT need to add
   `-laether_host_python` to `link_flags`: the import is the
   trigger.
2. Does NOT add any libpython link flags: none are needed, since
   the bridge dlopens libpython at runtime.

`ae build` errors with a clear actionable message if the bridge .a
hasn't been built: run `make contrib` (dev tree) or
`make install-contrib` (installed).

## `aether.toml`: usually empty for python

You typically don't need to set `cflags` or `link_flags` for
`contrib.host.python` at all:

```toml
# aether.toml: nothing required for contrib.host.python
[[bin]]
name = "myapp"
path = "myapp.ae"
```

If the deploy host's Python isn't discoverable via the default
order (1)-(3), set `AETHER_PYTHON_SONAME` in the build environment:

```sh
AETHER_PYTHON_SONAME=libpython3.14.so.1.0 ae build myapp.ae
```

`aether.toml` values support `${VAR}` substitution for the
`AETHER_*` env-var allowlist; `AETHER_PYTHON_SONAME` is honoured by
the bridge directly at runtime, so you don't need to forward it
through `aether.toml`: it just needs to be set in the environment
of the produced binary at run time.

## Usage

```aether
import contrib.host.python

main() {
    python.run("print('hello from python')")
}
```

Sandboxed:

```aether
import contrib.host.python
import std.list

main() {
    perms = list.new()
    list.add(perms, "fs_read")
    list.add(perms, "/etc/*")
    python.run_sandboxed(perms, "print(open('/etc/hostname').read())")
}
```

## Bidirectional embedding (#904)

`run_sandboxed` runs a script and reports success. A host that uses Python as
an extension language needs more: a **persistent VM** the host owns, host
functions the script can call, globals the host injects, and **typed** values
both ways. This is the same tier Lua, Duktape, QuickJS and Tcl have.

```aether
import contrib.host.python

@c_callback
app_add(vm: ptr) -> int {
    a = python.arg_int(vm, 1)
    b = python.arg_int(vm, 2)
    python.push_int(vm, a + b)
    return 1
}

main() {
    vm = python.vm_new()
    python.vm_register(vm, "app.math.add", app_add)
    python.vm_set_global_int(vm, "BASE", 100)

    python.vm_eval(vm, "app.math.add(2, 3) + BASE")
    if python.vm_result_type(vm) == python.PY_TINT {
        println("${python.vm_result_int(vm)}")   // 105
    }
    python.vm_pop_result(vm)
    python.vm_free(vm)
}
```

| Call | Effect |
|------|--------|
| `python.vm_new() -> ptr` | a persistent VM: its own globals dict, with `__builtins__` |
| `python.vm_register(vm, name, fn) -> int` | registers an Aether `@c_callback (vm: ptr) -> int` as a Python function; `"app.math.add"` makes (or reuses) the modules `app` and `app.math` |
| `python.vm_set_global_str/_int/_strlist(vm, name, v)` | injects a global (str, int, or a list of str) |
| `python.vm_eval(vm, code) -> int` | compiles `code` as an expression, or failing that as statements, and runs it once: an expression gives its value, statements give `None`. 0, or -1 on an exception |
| `python.vm_result_type(vm) -> int` | the result's tag (`PY_T*`) |
| `python.vm_result_int/_double/_str/_bool(vm)` | read the result (see below) |
| `python.vm_result_bytes(vm) -> ptr` | a `bytes` result as a new `std.bytes` handle (the caller frees it), or null |
| `python.vm_pop_result(vm)` | releases the result |
| `python.vm_set_instruction_limit(vm, budget, every) -> string` | bounds each outermost eval to `budget` trace events; 0 disarms; `""` when set |
| `python.arg_count/_type/_int/_double/_str/_bool/_bytes(vm, i)` | **inside a callback**: the arguments, 1-based, read like results |
| `python.push_int/_double/_str/_bool/_nil/_bytes(vm, v)` | **inside a callback**: the return value (the last push wins; return 1 to return it, 0 for `None`) |
| `python.raise_error(vm, msg) -> int` | **inside a callback**: `return python.raise_error(vm, msg)`; the script sees `RuntimeError(msg)` after the callback returns, so its defers run |

The tags are `PY_TNIL`, `PY_TBOOL`, `PY_TINT`, `PY_TSTR`, `PY_TTABLE` (a list
or dict), `PY_TERR`, `PY_TOTHER`, `PY_TBYTES` and `PY_TFLOAT`; the first seven
are Lua's numbering. How values read:

- **`bool` is not `int`.** Python's `bool` subclasses `int`, so `True` is
  checked first and tags `PY_TBOOL`. `_bool` reads 1 only for `True`; it does
  not take an int's truth.
- **Numbers read either way.** `_double` of an int is that number; `_int` of a
  float truncates, saturating at the int64 range.
- **Integers past 64 bits** (`2**64`) tag `PY_TOTHER`. `_int` reads 0, never a
  wrapped value; `_double` reads the nearest double (infinity past ~1e308).
- **Text and bytes stay apart.** A `str` reads with `_str` (UTF-8; it stops at
  an embedded NUL) and never as bytes; a `bytes` reads with `_bytes`, length
  and zeros intact, and never as text. `_str` is "" for any other type.
- **Errors.** A failed eval tags `PY_TERR`; `_str` is
  `"<ExceptionType>: <message>"`. The error indicator is always cleared.
- Tags are the value's real type: an object whose `__class__` claims to be
  `bytes` is still `PY_TOTHER`.

### One interpreter, many VMs

CPython has one interpreter per process, and a VM is a globals dict on it.
**Two VMs are not isolated**: they share `sys.modules`, everything imported,
`builtins`, and the GIL. A global set in one VM is not visible in another, but
anything reachable through a module is. A host function stored somewhere
shared can outlive its VM; calling it then raises `RuntimeError` instead of
reaching the freed VM.

Every call takes the GIL (`PyGILState_Ensure`), so any thread (an actor's
scheduler thread included) may use a VM, one thread at a time per VM.
Callbacks run with the GIL held and may call back into the bridge, including
`vm_eval` on their own VM: a nested eval keeps the outer call's arguments and
pushed value apart from its own.

`python.finalize()` ends the interpreter, and every VM with it. Call it from
the thread that called `init`. When the process already had an interpreter
(Aether loaded into Python), the bridge uses it and never finalizes it.

### The instruction limit

The limit is a `PyEval_SetTrace` hook installed on the calling thread for the
outermost `vm_eval`; a callback's nested eval neither resets the budget nor
removes the hook. It counts trace events (lines, calls, returns); `every` is
accepted for the other hosts' signature and ignored. Once the budget is spent
the hook raises `RuntimeError("instruction limit exceeded")` on **every** later
event, so a script that catches it (`except BaseException: pass`) is stopped
again at its handler's next line.

It bounds Python code only. It does not bound:

- a loop inside C, such as `sum(range(10**12))`, which makes no trace events;
- a script that calls `sys.settrace` and so replaces the hook;
- threads the script starts.

A process boundary (the planned sandboxed-child IPC) is the real answer to
those. Within one process, keep `sys` and `threading` away from scripts you
don't trust.

## Implementation notes

- All CPython C API access goes through a `dlsym` function-pointer
  table: see `aether_host_python.c`. The bridge .a has NO
  unresolved `Py_*` symbols at link time (`nm -u` confirms).
- `RTLD_GLOBAL` is used at `dlopen` so that Python C extensions
  loaded later (ctypes, native modules) resolve their `Py_*`
  references against the same libpython instance. Without it they
  segfault on shared singletons like `_Py_NoneStruct`.
- The `Py_RETURN_NONE` / `Py_INCREF` macros are replaced with
  explicit `g_py.Py_IncRef(g_py.py_none); return g_py.py_none;`
  to avoid the macros' direct struct-field access.

## Testing

Five tests exercise this host, each covering a different facet:

- **Namespace / FFI**:
  [`tests/integration/namespace_python/`](../../../tests/integration/namespace_python/),
  driven by
  [`test_namespace_python.sh`](../../../tests/integration/namespace_python/test_namespace_python.sh).
  This goes the *other* direction from embedding: `ae build --namespace`
  compiles [`calc.ae`](../../../tests/integration/namespace_python/calc.ae)
  into a `.so` plus a generated `calc_generated_sdk.py` wrapper, then
  [`check.py`](../../../tests/integration/namespace_python/check.py)
  imports the SDK and round-trips the full surface: `describe()`
  metadata (inputs `limit`/`name`, events `Computed`/`Overflow`), the
  input setters, the event handlers, and the script functions
  (`double_it`, `label`, `is_positive`), including the overflow path. It
  also asserts the Aether-side `[ae]` `println` output reaches the host's
  stdout. SKIPs on Windows or when `python3` is absent.

- **Shared-map interop**:
  [`tests/sandbox/test_shared_map_all.sh`](../../../tests/sandbox/test_shared_map_all.sh),
  the cross-host round-trip for `run_sandboxed_with_map`. The Python
  case hands the guest a frozen input map (`name=Charlie`, `factor=5`),
  the script reads those keys plus an absent `secret` (which comes back
  as `None`), writes `product=50` (`factor * 10`), and tries to overwrite
  the frozen `name` to `TAMPERED`; Aether reads the map back and asserts
  the write landed and the frozen input is untampered. SKIPs when the
  Python dev package isn't installed.

- **Nested shared-map context**:
  [`tests/integration/host_python_map_context/`](../../../tests/integration/host_python_map_context/)
  enters `run_sandboxed_with_map` recursively through the embedded CPython
  process. It verifies that the inner run sees only its map, the outer map is
  restored afterward, and no token remains active after the outer return.

- **Bidirectional embedding (#904)**:
  [`tests/integration/host_python_bidirectional/`](../../../tests/integration/host_python_bidirectional/),
  driven by
  [`test_host_python_bidirectional.sh`](../../../tests/integration/host_python_bidirectional/test_host_python_bidirectional.sh).
  A C harness linked without `-lpython` (so a call that skipped the dlsym table
  fails to link): dotted registration, every argument and result type, bytes
  with embedded zeros, int/float conversions and 64-bit overflow, exceptions,
  `raise_error`, nested evals, two VMs, a host function outliving its VM, the
  instruction limit (caught, nested, per eval), two threads, flat refcounts, gc
  objects and RSS across two loops, and finalize then init again.
  `AETHER_PYTHON_BRIDGE_SRC` points it at a copy of the bridge, which is how
  each case was shown to fail against a planted bug. The Aether side
  (`@c_callback`, `std.bytes` handles, defers across a raise) is in
  `test_host_python.ae`.

- **Build smoke**:
  [`tests/scripts/contrib_build.sh`](../../../tests/scripts/contrib_build.sh),
  run by `make contrib`. It probes for Python via `python3-config` and,
  when present, compiles + archives the bridge as
  `build/contrib/libaether_host_python.a` (the `python` catalogue entry).
  In the default build-all mode a missing dep is a silent SKIP; under
  `make contrib MODULES=python` it's a hard failure.

## Testing: co-located embedding spec

[`test_host_python.ae`](test_host_python.ae) sits next to this bridge and drives it
through `import contrib.host.python`: the path a real user takes, so it covers
module.ae's declarations and the contrib-link plumbing as well as the C shim.

It asserts that a script evaluates; that the interpreter **genuinely runs**
(Python computes a value and a second eval asserts on it Python-side, so a
bridge that loaded `libpython` but never evaluated would not pass); that runtime
errors *and* syntax errors propagate as failures rather than being swallowed;
and that `init` is idempotent while the VM is live. For the #904 tier it
registers `@c_callback` functions, reads typed results, round-trips bytes
through `std.bytes` handles, checks that a raising callback's defers run, and
evaluates from inside a callback and under the instruction limit.

Run by `make contrib-host-check`'s `[3/3]` phase, which resolves this
runtime's soname first. Without Python installed the specs report **skipped**
(#1610's `it_when`) rather than failing: an absent runtime is a provisioning
gap, not a code defect.

Assertions are on **return codes and Python-side state, never on the bridge's
stdout**: the embedded interpreter buffers its own output and it is not
reliably flushed when an Aether program exits.
