# contrib.host.duktape: Embedded JavaScript (Duktape ES5.1)

`import contrib.host.duktape` lets an Aether program embed the
Duktape ES5.1 engine in-process: `duktape.run_sandboxed(perms,
"<source>")` evaluates JavaScript with permission-checked access
controls.

**Naming**: the canonical capability and module name is `duktape`
(named after the engine, not the language) because "JS" is the one
slot where the host could in principle be served by multiple
runtimes (Duktape / QuickJS / V8-node); `--with=quickjs` etc. can
coexist later under their own engine names. **`contrib.host.js`
remains as a back-compat alias**: existing code with
`import contrib.host.js` keeps working; new code should prefer
`import contrib.host.duktape`.

## How it loads libduktape

The bridge `dlopen`s libduktape at the **deploy host's** runtime:
there is no `-lduktape` on the link line. The produced binary works
against whatever Duktape minor version the deploy host has.

Discovery order at first call to `duktape.run` (strict two-step):

1. `${AETHER_DUKTAPE_SONAME}` env var (orchestrator-supplied exact,
   e.g. `libduktape.so.207`).
2. `libduktape.so` (Debian-style unversioned symlink).

If both fail, `duktape.run*` returns -1 with a clear error naming
the env var. **There is no hardcoded version fallback list**: the
bridge stays distro-agnostic; the orchestrator owns the probe.

Duktape is embedded-only (no `duktape` CLI to probe with). The
orchestrator can find the soname via `ldconfig` or distro
package-manager queries:

```sh
AETHER_DUKTAPE_SONAME=$(ldconfig -p | awk '/libduktape\.so/{print $1; exit}')
```

## What `ae build` does for you automatically

When your program has `import contrib.host.duktape`, `ae build`:
1. Links `libaether_host_duktape.a` (the in-tree bridge)
   automatically.
2. Does NOT add any libduktape link flags: the bridge dlopens
   libduktape at runtime.

`ae build` errors with a clear actionable message if the bridge .a
hasn't been built: build the image with
`aether-build --with=duktape --rebuild-image` (or the alias
`--with=js`) or `make contrib MODULES=duktape && make install-contrib`.

## `aether.toml`: usually empty for duktape

```toml
# nothing required for contrib.host.duktape
[[bin]]
name = "myapp"
path = "myapp.ae"
```

If the deploy host's duktape isn't discoverable via the default
order, set `AETHER_DUKTAPE_SONAME`:

```sh
AETHER_DUKTAPE_SONAME=libduktape.so.207 ae build myapp.ae
```

## Usage

```aether
import contrib.host.duktape

main() {
    duktape.run("print('hello from duktape: ' + Duktape.version);")
}
```

## Bidirectional embedding (#904)

The fire-and-forget `run_sandboxed` is enough to *run* a script, but a host
that uses JavaScript as a first-class, callback-driven extension surface
needs a **persistent, host-owned VM** that registers host
callbacks the script can call, injects globals, and returns a **typed** result.
That tier:

```aether
import contrib.host.duktape

@c_callback
app_add(vm: ptr) -> int {
    a = duktape.arg_int(vm, 1)
    b = duktape.arg_int(vm, 2)
    duktape.push_int(vm, a + b)
    return 1
}

main() {
    vm = duktape.vm_new()
    duktape.vm_register(vm, "app.math.add", app_add)
    duktape.vm_set_global_str(vm, "NAME", "Aether")
    duktape.vm_set_global_int(vm, "BASE", 100)

    status = duktape.vm_eval(vm, "app.math.add(2, 3) + BASE")
    t = duktape.vm_result_type(vm)
    if t == DUK_TINT { reply = duktape.vm_result_int(vm) }
    duktape.vm_pop_result(vm)
    duktape.vm_free(vm)
}
```

| Call | Effect |
|------|--------|
| `duktape.vm_new() -> ptr` | create a persistent host-owned VM handle |
| `duktape.vm_register(vm, name, fn_ptr) -> int` | register an Aether `(vm: ptr) -> int` `@c_callback` as a JS function (dotted names like `"app.math.add"` create/reuse parent objects) |
| `duktape.vm_set_global_str/_int/_strlist(vm, name, v)` | inject a global before a run (string / int / string-array) |
| `duktape.vm_eval(vm, code) -> int` | run; leaves the single return value on the VM stack |
| `duktape.vm_result_type(vm) -> int` | typed tag of the result (`DUK_T*`) |
| `duktape.vm_result_int/_str/_bool(vm)` | read the typed result |
| `duktape.vm_pop_result(vm)` | clear the stack after reading |
| `duktape.vm_set_instruction_limit(vm, budget, every) -> string` | returns `"execution limit unavailable: libduktape built without DUK_USE_EXEC_TIMEOUT_CHECK"` because system libduktape packages disable this build option |
| `duktape.arg_count/_type/_str/_int/_bool(vm, i)` | **inside a callback**: read args (1-based) |
| `duktape.push_int/_str/_bool/_nil/_double(vm, v)` | **inside a callback**: push a result value |
| `duktape.raise_error(vm, msg) -> int` | raise a JS `Error` from a callback (`return duktape.raise_error(vm, msg)`) |

Typed-value tags exported as consts: `DUK_TNIL`, `DUK_TBOOL`, `DUK_TINT`,
`DUK_TSTR`, `DUK_TTABLE`, `DUK_TERR`, `DUK_TOTHER`.

## Containment model

Duktape implements ES5.1. No ambient capabilities: all functions
(`env`, `readFile`, `fileExists`, `writeFile`, `exec`) are native
bindings with sandbox checks. No LD_PRELOAD needed. Purest
containment model of the host bridges.

## Implementation notes

- All Duktape C-API access goes through a `dlsym` function-pointer
  table: see `aether_host_duktape.c`. The bridge .a has NO
  unresolved Duktape symbols (`nm -u` confirms).
- Three convenience macros are re-expressed as inline helpers
  over the dlsym'd underlying functions:
  - `duk_create_heap_default()` → `duk_create_heap(NULL, NULL, NULL, NULL, NULL)`
  - `duk_peval_string(c, s)` → `duk_eval_raw(c, s, 0, FLAGS)`
  - `duk_safe_to_string(c, i)` → `duk_safe_to_lstring(c, i, NULL)`
- The `DUK_COMPILE_*` flag constants used by `duk_peval_string` are
  baked directly (public ABI of Duktape's compile-flags interface;
  same shape as Ruby's `Qnil`-as-literal in that bridge).

## Testing

Coverage comes from three test suites:

- [`tests/integration/host_duktape_bidirectional/`](../../../tests/integration/host_duktape_bidirectional/):
  dedicated C harness exercising the bidirectional API (persistent VM, dotted callback registration,
  typed args/results, try/catch error handling, non-ASCII round trip `"héllo €"`, and limit check).
- [`tests/sandbox/test_shared_map_all.sh`](../../../tests/sandbox/test_shared_map_all.sh):
  the `run_sandboxed_with_map` shared-map round-trip run across
  ALL host bridges.
- [`tests/scripts/contrib_build.sh`](../../../tests/scripts/contrib_build.sh):
  the `make contrib` smoke test; its `duktape` catalogue entry
  verifies the bridge archive `libaether_host_duktape.a` compiles and
  archives cleanly.

## Testing: co-located embedding spec

[`test_host_duktape.ae`](test_host_duktape.ae) sits next to this bridge and drives it
through `import contrib.host.duktape`: the path a real user takes, so it covers
module.ae's declarations and the contrib-link plumbing as well as the C shim.

It asserts that a script evaluates; that state persists across calls; that callbacks
and typed results work; that non-ASCII strings round trip; that errors propagate;
and that `init` is idempotent while the VM is live.

Run by `make contrib-host-check`'s `[3/3]` phase.
