# contrib.host.python — Embedded CPython

`import contrib.host.python` lets an Aether program embed CPython
in-process: `python.run_sandboxed(perms, "<source>")` evaluates Python
inside the calling process, with permission-checked access controls.

## How it loads CPython

The bridge `dlopen`s `libpython` at the **deploy host's** runtime
— there is no `-lpython` on the link line. This means:

- The build environment doesn't need `python3-dev` (or any libpython
  at all). Only the headers, which are already vendored.
- The produced binary works against whatever Python minor version the
  deploy host happens to have — no ABI lock-in.
- A binary built today on a 3.11 machine and run on a 3.14 machine
  works as long as the host has a usable libpython.

Discovery order at first call to `python.run` (strict two-step):

1. `${AETHER_PYTHON_SONAME}` env var (orchestrator-supplied exact
   soname, e.g. `libpython3.14.so.1.0`). The orchestrator probes
   the host's python via sysconfig and passes this.
2. `libpython3.so` (unversioned symlink — present in the runtime
   package on Fedora-likes / Bazzite; typically -dev-only on
   Debian-likes).

If both fail, `python.run*` returns -1 with a clear error naming
the env var. **There is no hardcoded version fallback list** — the
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
compile (which `ae build` handles transparently — see below).

## What `ae build` does for you automatically

When your program has `import contrib.host.python`, `ae build`:

1. Links `libaether_host_python.a` (the in-tree bridge) onto the
   resulting binary automatically. You do NOT need to add
   `-laether_host_python` to `link_flags` — the import is the
   trigger.
2. Does NOT add any libpython link flags — none are needed, since
   the bridge dlopens libpython at runtime.

`ae build` errors with a clear actionable message if the bridge .a
hasn't been built: run `make contrib` (dev tree) or
`make install-contrib` (installed).

## `aether.toml` — usually empty for python

You typically don't need to set `cflags` or `link_flags` for
`contrib.host.python` at all:

```toml
# aether.toml — nothing required for contrib.host.python
[[bin]]
name = "myapp"
path = "myapp.ae"
```

If the deploy host's Python isn't discoverable via the default
order (1)–(3), set `AETHER_PYTHON_SONAME` in the build environment:

```sh
AETHER_PYTHON_SONAME=libpython3.14.so.1.0 ae build myapp.ae
```

`aether.toml` values support `${VAR}` substitution for the
`AETHER_*` env-var allowlist; `AETHER_PYTHON_SONAME` is honoured by
the bridge directly at runtime, so you don't need to forward it
through `aether.toml` — it just needs to be set in the environment
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

## Implementation notes

- All CPython C API access goes through a `dlsym` function-pointer
  table — see `aether_host_python.c`. The bridge .a has NO
  unresolved `Py_*` symbols at link time (`nm -u` confirms).
- `RTLD_GLOBAL` is used at `dlopen` so that Python C extensions
  loaded later (ctypes, native modules) resolve their `Py_*`
  references against the same libpython instance. Without it they
  segfault on shared singletons like `_Py_NoneStruct`.
- The `Py_RETURN_NONE` / `Py_INCREF` macros are replaced with
  explicit `g_py.Py_IncRef(g_py.py_none); return g_py.py_none;`
  to avoid the macros' direct struct-field access.

## Testing

Three tests exercise this host, each covering a different facet:

- **Namespace / FFI** —
  [`tests/integration/namespace_python/`](../../../tests/integration/namespace_python/),
  driven by
  [`test_namespace_python.sh`](../../../tests/integration/namespace_python/test_namespace_python.sh).
  This goes the *other* direction from embedding: `ae build --namespace`
  compiles [`calc.ae`](../../../tests/integration/namespace_python/calc.ae)
  into a `.so` plus a generated `calc_generated_sdk.py` wrapper, then
  [`check.py`](../../../tests/integration/namespace_python/check.py)
  imports the SDK and round-trips the full surface — `describe()`
  metadata (inputs `limit`/`name`, events `Computed`/`Overflow`), the
  input setters, the event handlers, and the script functions
  (`double_it`, `label`, `is_positive`), including the overflow path. It
  also asserts the Aether-side `[ae]` `println` output reaches the host's
  stdout. SKIPs on Windows or when `python3` is absent.

- **Shared-map interop** —
  [`tests/sandbox/test_shared_map_all.sh`](../../../tests/sandbox/test_shared_map_all.sh),
  the cross-host round-trip for `run_sandboxed_with_map`. The Python
  case hands the guest a frozen input map (`name=Charlie`, `factor=5`),
  the script reads those keys plus an absent `secret` (which comes back
  as `None`), writes `product=50` (`factor * 10`), and tries to overwrite
  the frozen `name` to `TAMPERED`; Aether reads the map back and asserts
  the write landed and the frozen input is untampered. SKIPs when the
  Python dev package isn't installed.

- **Build smoke** —
  [`tests/scripts/contrib_build.sh`](../../../tests/scripts/contrib_build.sh),
  run by `make contrib`. It probes for Python via `python3-config` and,
  when present, compiles + archives the bridge as
  `build/contrib/libaether_host_python.a` (the `python` catalogue entry).
  In the default build-all mode a missing dep is a silent SKIP; under
  `make contrib MODULES=python` it's a hard failure.

## Testing — co-located embedding spec

[`test_host_python.ae`](test_host_python.ae) sits next to this bridge and drives it
through `import contrib.host.python` — the path a real user takes, so it covers
module.ae's declarations and the contrib-link plumbing as well as the C shim.

It asserts that a script evaluates; that the interpreter **genuinely runs**
(Python computes a value and a second eval asserts on it Python-side, so a
bridge that loaded `libpython` but never evaluated would not pass); that runtime
errors *and* syntax errors propagate as failures rather than being swallowed;
and that `init` is idempotent while the VM is live.

Run by `make contrib-host-check`'s `[3/3]` phase, which resolves this
runtime's soname first. Without Python installed the specs report **skipped**
(#1610's `it_when`) rather than failing — an absent runtime is a provisioning
gap, not a code defect.

Assertions are on **return codes and Python-side state, never on the bridge's
stdout**: the embedded interpreter buffers its own output and it is not
reliably flushed when an Aether program exits.
