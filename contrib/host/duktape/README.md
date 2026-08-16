# contrib.host.duktape — Embedded JavaScript (Duktape ES5.1)

`import contrib.host.duktape` lets an Aether program embed the
Duktape ES5.1 engine in-process: `duktape.run_sandboxed(perms,
"<source>")` evaluates JavaScript with permission-checked access
controls.

**Naming**: the canonical capability and module name is `duktape`
(named after the engine, not the language) because "JS" is the one
slot where the host could in principle be served by multiple
runtimes (Duktape / QuickJS / V8-node); `--with=quickjs` etc. can
coexist later under their own engine names. **`contrib.host.js`
remains as a back-compat alias** — existing code with
`import contrib.host.js` keeps working; new code should prefer
`import contrib.host.duktape`.

## How it loads libduktape

The bridge `dlopen`s libduktape at the **deploy host's** runtime —
there is no `-lduktape` on the link line. The produced binary works
against whatever Duktape minor version the deploy host has.

Discovery order at first call to `duktape.run` (strict two-step):

1. `${AETHER_DUKTAPE_SONAME}` env var (orchestrator-supplied exact,
   e.g. `libduktape.so.207`).
2. `libduktape.so` (Debian-style unversioned symlink).

If both fail, `duktape.run*` returns -1 with a clear error naming
the env var. **There is no hardcoded version fallback list** — the
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
2. Does NOT add any libduktape link flags — the bridge dlopens
   libduktape at runtime.

`ae build` errors with a clear actionable message if the bridge .a
hasn't been built: build the image with
`aether-build --with=duktape --rebuild-image` (or the alias
`--with=js`) or `make contrib MODULES=duktape && make install-contrib`.

## `aether.toml` — usually empty for duktape

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

## Containment model

Duktape implements ES5.1. No ambient capabilities — all functions
(`env`, `readFile`, `fileExists`, `writeFile`, `exec`) are native
bindings with sandbox checks. No LD_PRELOAD needed. Purest
containment model of the host bridges.

## Implementation notes

- All Duktape C-API access goes through a `dlsym` function-pointer
  table — see `aether_host_duktape.c`. The bridge .a has NO
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

There's no dedicated `host_duktape/` end-to-end test (no fib-style
set-piece). Coverage comes from two cross-cutting tests instead:

- [`tests/sandbox/test_shared_map_all.sh`](../../../tests/sandbox/test_shared_map_all.sh)
  — the `run_sandboxed_with_map` shared-map round-trip run across
  ALL host bridges. Its JS (Duktape) case compiles
  `aether_host_duktape.c`, seeds a map (`name`=Bob, `count`=7),
  runs JS that reads those keys, writes back `doubled`, and tries to
  tamper with `name`; it asserts the read, the write (`result=14`),
  and that the frozen `name` stays `Bob`. SKIPs when Duktape's dev
  headers aren't installed.
- [`tests/scripts/contrib_build.sh`](../../../tests/scripts/contrib_build.sh)
  — the `make contrib` smoke test; its `duktape` catalogue entry
  verifies the bridge archive `libaether_host_duktape.a` compiles and
  archives cleanly.

## Testing — co-located embedding spec

[`test_host_duktape.ae`](test_host_duktape.ae) sits next to this bridge and drives it
through `import contrib.host.duktape` — the path a real user takes, so it covers
module.ae's declarations and the contrib-link plumbing as well as the C shim.

It asserts that a script evaluates; that the interpreter **genuinely runs**
(Duktape computes a value and a second eval asserts on it Duktape-side, so a
bridge that loaded `libduktape` but never evaluated would not pass); that runtime
errors *and* syntax errors propagate as failures rather than being swallowed;
and that `init` is idempotent while the VM is live.

Run by `make contrib-host-check`'s `[3/3]` phase, which resolves this
runtime's soname first. Without Duktape installed the specs report **skipped**
(#1610's `it_when`) rather than failing — an absent runtime is a provisioning
gap, not a code defect.

Assertions are on **return codes and Duktape-side state, never on the bridge's
stdout**: the embedded interpreter buffers its own output and it is not
reliably flushed when an Aether program exits.
