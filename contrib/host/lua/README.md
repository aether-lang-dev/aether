# contrib.host.lua — Embedded Lua

`import contrib.host.lua` lets an Aether program embed Lua in-process:
`lua.run_sandboxed(perms, "<source>")` evaluates Lua with
permission-checked access controls.

## How it loads liblua

The bridge `dlopen`s liblua at the **deploy host's** runtime — there
is no `-llua` on the link line. The produced binary works against
whatever Lua minor version the deploy host has (5.3 or 5.4 supported).

Discovery order at first call to `lua.run` (strict two-step):

1. `${AETHER_LUA_SONAME}` env var (orchestrator-supplied exact,
   e.g. `liblua5.4.so.0`).
2. `liblua.so` (unversioned symlink, when present).

If both fail, `lua.run*` returns -1 with a clear error naming the
env var. **There is no hardcoded version fallback list** — the
bridge stays distro-agnostic; the orchestrator owns the probe.

Lua doesn't have a standard sysconfig-style probe. The orchestrator
can read `lua -e 'print(_VERSION)'` (gives `Lua 5.4`) and derive
the soname (`liblua5.4.so.0` on Debian, `liblua-5.4.so` on Fedora).

## What `ae build` does for you automatically

When your program has `import contrib.host.lua`, `ae build`:
1. Links `libaether_host_lua.a` (the in-tree bridge) onto the
   resulting binary automatically.
2. Does NOT add any liblua link flags — the bridge dlopens liblua
   at runtime.

`ae build` errors with a clear actionable message if the bridge .a
hasn't been built: build the image with
`aether-build --with=lua --rebuild-image` (containerised) or
`make contrib MODULES=lua && make install-contrib` (installed
toolchain).

## `aether.toml` — usually empty for lua

```toml
# nothing required for contrib.host.lua
[[bin]]
name = "myapp"
path = "myapp.ae"
```

If the deploy host's Lua isn't discoverable via the default order,
set `AETHER_LUA_SONAME` in the build environment:

```sh
AETHER_LUA_SONAME=liblua5.4.so.0 ae build myapp.ae
```

## Usage

```aether
import contrib.host.lua

main() {
    lua.run("print('hello from lua')")
}
```

## Bidirectional embedding — a Redis-class host (#904)

The fire-and-forget `run_sandboxed` is enough to *run* a script, but a host
that uses Lua as a first-class, callback-driven extension surface (Redis-style
`EVAL`/`FCALL`) needs a **persistent, host-owned VM** that registers host
callbacks the script can call, injects globals, and returns a **typed** result.
That tier:

```aether
import contrib.host.lua

// A host callback: reads typed args (1-based), pushes a typed result,
// returns the number of values pushed (the Lua C-function rule). It must be
// `@c_callback` so its symbol is addressable as a `ptr` value (see
// docs/c-interop.md "Exporting an Aether Function as a C Callback").
@c_callback
redis_call(vm: ptr) -> int {
    cmd = lua.arg_str(vm, 1)
    // ...dispatch the command in the host, marshal the reply...
    lua.push_str(vm, "OK")
    return 1
}

main() {
    vm = lua.vm_new()                                   // persistent VM handle
    lua.vm_register(vm, "redis.call", redis_call)       // pass the @c_callback fn
    lua.vm_set_global_strlist(vm, "KEYS", keys)         // inject KEYS/ARGV
    lua.vm_set_global_strlist(vm, "ARGV", argv)
    lua.vm_set_instruction_limit(vm, 100000000, 1000)   // script-timeout guard

    status = lua.vm_eval(vm, source)                    // 0 ok, -1 error
    t = lua.vm_result_type(vm)                          // LUA_TINT/TSTR/TERR/…
    if t == LUA_TSTR { reply = lua.vm_result_str(vm) }
    else { if t == LUA_TINT { n = lua.vm_result_int(vm) } }
    lua.vm_pop_result(vm)
    lua.vm_free(vm)
}
```

| Call | Effect |
|------|--------|
| `lua.vm_new() -> ptr` | create a persistent host-owned VM (libs opened) |
| `lua.vm_register(vm, name, fn_ptr) -> int` | register an Aether `(vm: ptr) -> int` callback as a Lua global (dotted names like `"redis.call"` create/reuse the prefix table) |
| `lua.vm_set_global_str/_int/_strlist(vm, name, v)` | inject a global before a run (string / int / string-array) |
| `lua.vm_eval(vm, code) -> int` | run; leaves the single return value on the VM stack |
| `lua.vm_result_type(vm) -> int` | typed tag of the result (`LUA_T*`) |
| `lua.vm_result_int/_str/_bool(vm)` | read the typed result |
| `lua.vm_pop_result(vm)` | clear the stack after reading |
| `lua.vm_set_instruction_limit(vm, budget, every)` | count-hook guard; abort after `budget` VM instructions |
| `lua.arg_count/_type/_str/_int/_bool(vm, i)` | **inside a callback**: read args (1-based) |
| `lua.push_int/_str/_bool/_nil(vm, v)` | **inside a callback**: push a result |
| `lua.push_double(vm, v)` | push a RESP3 Double (integral → Lua number, else precise string) |
| `lua.push_tagged_table(vm, "ok"\|"err", msg)` | push a Redis-style `{ok=…}`/`{err=…}` table |
| `lua.raise_error(vm, msg) -> int` | raise a Lua error from a callback (`return lua.raise_error(vm, m)`) |

Typed-value tags exported as consts: `LUA_TNIL`, `LUA_TBOOL`, `LUA_TINT`,
`LUA_TSTR`, `LUA_TTABLE`, `LUA_TERR`, `LUA_TOTHER`.

### Returning arrays / maps / nested tables from a callback (#910)

A faithful `redis.call` returns RESP multibulks — arrays (`KEYS`, `LRANGE`,
`SMEMBERS`), maps (`HGETALL`), and nested arrays-of-arrays. The table builder
constructs them inside a callback. **The table being built is the top of the
stack:** `table_begin` pushes a fresh one, the `seti_*`/`set_*` ops write into
it, and nesting is just another `table_begin` closed with `table_end_seti` /
`table_end_setfield`.

| Call | Effect |
|------|--------|
| `lua.table_begin(vm, narr, nrec)` | push a fresh table (`narr`/`nrec` are size hints) |
| `lua.table_seti_str/_int/_bool(vm, i, v)` | array set: `t[i] = v` (1-based) |
| `lua.table_set_str/_int/_bool(vm, key, v)` | map set: `t[key] = v` |
| `lua.table_end_seti(vm, i)` | close the current child table into its parent at `[i]` |
| `lua.table_end_setfield(vm, key)` | close the current child table into its parent at `[key]` |
| `lua.table_finish(vm)` | finish the outermost table (it stays on the stack as the return) |

```aether
// redis.call('KEYS', pattern) -> {"k1","k2","k3"}
@c_callback
redis_keys(vm: ptr) -> int {
    lua.table_begin(vm, 3, 0)
    lua.table_seti_str(vm, 1, "k1")
    lua.table_seti_str(vm, 2, "k2")
    lua.table_seti_str(vm, 3, "k3")
    lua.table_finish(vm)
    return 1                      // the table is the single return value
}

// array-of-arrays: { {"a","b"}, {"c"} }
@c_callback
nested(vm: ptr) -> int {
    lua.table_begin(vm, 2, 0)     // outer
    lua.table_begin(vm, 2, 0)     // inner #1 (now on top)
    lua.table_seti_str(vm, 1, "a")
    lua.table_seti_str(vm, 2, "b")
    lua.table_end_seti(vm, 1)     // outer[1] = inner#1
    lua.table_begin(vm, 1, 0)     // inner #2
    lua.table_seti_str(vm, 1, "c")
    lua.table_end_seti(vm, 2)     // outer[2] = inner#2
    lua.table_finish(vm)
    return 1
}
```

This is the next tier above the factor bridge's two-way persistent-VM + shared-
map model: **host-callbacks-with-typed-marshalling**, in both directions.

## Implementation notes

- All Lua C-API access goes through a `dlsym` function-pointer
  table — see `aether_host_lua.c`. The bridge .a has NO unresolved
  Lua symbols (`nm -u` confirms).
- Lua's headers `#define` many macros that wrap real functions
  (e.g. `luaL_dostring(L,s)` = `luaL_loadstring(L,s) || lua_pcall(…)`,
  `lua_pop(L,n)` = `lua_settop(L, -(n)-1)`). The bridge re-implements
  these as inline static functions over the dlopen table — only the
  underlying real functions need dlsym entries.
- Symbol versioning (`lua_close@@LUA_5.4`): dlsym strips version
  tags, so `dlsym(h, "lua_close")` works against 5.3 or 5.4
  indifferently.

## Testing

The bidirectional API (#904) has a dedicated end-to-end test:

- [`../../../tests/integration/host_lua_bidirectional/`](../../../tests/integration/host_lua_bidirectional/)
  — a C harness over the tier-2 surface: registers two host callbacks
  (`host.double`, `host.ping`), injects an int global + a `KEYS` string-array,
  runs a script that calls the callbacks and reads the globals, and asserts the
  **typed** results (int `142`, string `hello k2`) plus that the
  instruction-limit guard aborts an infinite loop. SKIPs when no matching
  liblua headers + runtime soname are found.
- [`../../../tests/integration/host_lua_table_builder/`](../../../tests/integration/host_lua_table_builder/)
  — the #910 table builder: callbacks returning a multi-element array
  (`{"k1","k2","k3"}`), a string-keyed map (`{field=…, count=…}`), and a nested
  array-of-arrays (`{{"a","b"},{"c"}}`), with a Lua script asserting each shape.
  Same SKIP gate.

The fire-and-forget surface is covered by two cross-cutting tests:

- [`../../../tests/sandbox/test_shared_map_all.sh`](../../../tests/sandbox/test_shared_map_all.sh)
  — the cross-host shared-map round-trip. The Lua case seeds a shared
  map with `name=Alice` / `age=30`, runs Lua under
  `lua_run_sandboxed_with_map`, and asserts the script reads both keys
  (and gets `nil` for an absent `secret`), writes back
  `greeting=hello Alice`, and that a tamper attempt
  (`aether_map_put('name', 'TAMPERED')`) is rejected once the input
  token is revoked — `name` stays `Alice`. SKIPs when no liblua /
  `lua5.x` dev package is detected.
- [`../../../tests/scripts/contrib_build.sh`](../../../tests/scripts/contrib_build.sh)
  — the `make contrib` smoke test. Confirms the `host_lua` bridge
  archive (`build/contrib/libaether_host_lua.a`) builds from
  `aether_host_lua.c`. SKIPs the module when `pkg-config` finds no
  `lua5.4` / `lua5.3` / `lua` dev kit (hard-fails only in explicit
  `MODULES=lua` mode).

## Testing — co-located embedding spec

[`test_host_lua.ae`](test_host_lua.ae) sits next to this bridge and drives it
through `import contrib.host.lua` — the path a real user takes, so it covers
module.ae's declarations and the contrib-link plumbing as well as the C shim.

It asserts that a script evaluates; that the interpreter **genuinely runs**
(Lua computes a value and a second eval asserts on it Lua-side, so a
bridge that loaded `liblua` but never evaluated would not pass); that runtime
errors *and* syntax errors propagate as failures rather than being swallowed;
and that `init` is idempotent while the VM is live.

Run by `make contrib-host-check`'s `[3/3]` phase, which resolves this
runtime's soname first. Without Lua installed the specs report **skipped**
(#1610's `it_when`) rather than failing — an absent runtime is a provisioning
gap, not a code defect.

Assertions are on **return codes and Lua-side state, never on the bridge's
stdout**: the embedded interpreter buffers its own output and it is not
reliably flushed when an Aether program exits.
