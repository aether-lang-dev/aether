# contrib.host.quickjs: Embedded JavaScript (QuickJS ES2023)

`import contrib.host.quickjs` exposes QuickJS (quickjs-ng 0.17.0) as a member
of the `contrib.host` family: `quickjs.run_sandboxed(perms, "<source>")`
evaluates modern JavaScript (ES2023) with permission-checked access controls.

## Engine & Containment Model

QuickJS is compiled into the program from a pinned amalgamation
(`contrib/quickjs/amalgamation.lock`). `quickjs-libc` (the engine's own file,
network and OS modules) is **not compiled in**, so scripts have **no ambient
file, network, or OS access of their own**. They reach only what Aether exposes
to them via host functions.

When code is executed under `quickjs.run_sandboxed(perms, code)`, Aether pushes
the grant list for the duration of the run so any Aether host function called from
the script is checked against `perms`.

## Usage

```aether
import contrib.host.quickjs

main() {
    quickjs.run("var x = (2 + 3) * 7; if (x !== 35) throw new Error('wrong');")
}
```

## Shared Map Surface

`quickjs.run_sandboxed_with_map(perms, code, map_token)` exposes
`aether_map_get(key)` and `aether_map_put(key, value)` as JS globals
bound to `map_token`, freezing inputs before execution.

## Full Engine API

For the persistent runtime API (runtimes, handles, closures, values, promises, bytes),
import [`contrib.quickjs`](../../quickjs/README.md) directly.

## Testing

Co-located spec: [`test_host_quickjs.ae`](test_host_quickjs.ae)
