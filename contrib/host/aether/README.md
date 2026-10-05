# contrib.host.aether — Aether-hosts-Aether Sandbox

Run Aether scripts as sandboxed subprocesses from an Aether host
application.  The child process is compiled to a native binary and
launched with `LD_PRELOAD=libaether_sandbox.so` so its libc calls
(`connect`, `open`, `execve`, `getenv`) are intercepted against a
grant list.

Unlike the Lua/Python/Ruby host modules which embed an interpreter
in-process, this module launches a **separate process** — because
Aether is compiled, not interpreted.

## How it differs from other host modules

| Module | Mechanism | Containment |
|---|---|---|
| `contrib.host.lua` | Embed Lua interpreter in-process | Sandbox checker on libc calls |
| `contrib.host.python` | Embed CPython in-process | Sandbox checker on libc calls |
| **`contrib.host.aether`** | **Compile to binary, fork+exec** | **LD_PRELOAD on child process** |

The LD_PRELOAD approach is stronger: even if the child binary contains
unexpected `extern` calls or inline C, the interception still catches
the libc calls underneath.

## Prerequisites

The `ae` compiler must be on `$PATH` (or set `AETHER_AE_PATH`), and
`libaether_sandbox.so` must be built (or set `AETHER_SANDBOX_LIB`).

## Usage

```aether
import contrib.host.aether
import std.sandbox

// Set up sandbox grants
worker = sandbox.new("worker") {
    grant_fs_read("/etc/app/config.yaml")
    grant_env("PRICING_VERSION")
    // No network, no exec, no other filesystem access
}

// Compile + run a script in one step
rc = aether.run_script_sandboxed(worker, "rules/pricing.ae")

// Or compile once, run many times
aether.compile("rules/pricing.ae", "build/pricing")
rc = aether.run_sandboxed(worker, "build/pricing")

// Capture stdout
output = aether.capture_script_sandboxed(worker, "rules/report.ae")
println(output)
```

## Data exchange via shared map

```aether
import contrib.host.aether
import std.sandbox

// Create shared map with inputs
map, token = shared_map_new()
shared_map_put(map, "sku", "WIDGET-1")
shared_map_put(map, "quantity", "5")

// Run script — it reads inputs via aether_map_get("sku")
// and writes outputs via aether_map_put("result", "...")
worker = sandbox.new("worker") { grant_env("AETHER_MAP_SHM") }
aether.run_script_sandboxed_with_map(worker, "rules/pricing.ae", token)

// Read outputs back
result = shared_map_get(map, "result")
println("Price: ${result}")
shared_map_free(map)
```

## Environment variables

| Variable | Purpose |
|---|---|
| `AETHER_AE_PATH` | Path to `ae` compiler (default: `ae` on PATH) |
| `AETHER_SANDBOX_SHM` | Set on child — POSIX shm name for grants |
| `AETHER_MAP_SHM` | Set on child — shared memory name for map I/O |

The grant list is serialized to POSIX shared memory by
`aether_spawn_sandboxed()` (in `runtime/aether_spawn_sandboxed.c`)
and read by `libaether_sandbox.so` at child startup.  No env-var
encoding of grants — the same mechanism used by all host modules.

## Security model

The **facade** is the security boundary: the child process can only
do what the grant list allows.  `LD_PRELOAD` enforces this at the
libc level — `connect()`, `open()`, `execve()`, `getenv()` are all
intercepted.

`hide`/`seal except` within the hosted script is author-side hygiene
(organizing the script's own code), not a security mechanism.  The
grants are what provide containment.

See [containment-sandbox.md](../../../docs/containment-sandbox.md) for
the full security model, including limitations (statically linked
binaries bypass LD_PRELOAD).

## Testing

The spec is [`test_host_aether.ae`](test_host_aether.ae), beside the bridge,
written against `std.spec`. Unlike the VM-host specs, which embed one
interpreter and call into it, this one exercises Aether hosting Aether: it
writes a trivial child `.ae` under a PID-scoped temp directory, has the bridge
compile it to a native binary through the `ae` driver, and runs it as a
subprocess. It checks four outcomes on the **unsandboxed** rung: the child
compiles and runs, a non-zero child exit code comes back, a child that fails
to compile is reported, and a missing script path is reported. The
`run_sandboxed` and `*_with_map` paths need `libaether_sandbox.so` and
LD_PRELOAD, so they stay out of the portable spec.

The contrib check's discovery phase builds `libaether_host_aether.a` on
demand. The spec points `AETHER_AE_PATH` at `build/ae` when it is unset, so a
bare `ae run contrib/host/aether/test_host_aether.ae` works from the
repository root. Its cases **skip** (never fail) when there is no `ae` driver
to shell out to. On platforms other than Linux and macOS the bridge compiles
to an empty stub, and the discovery phase skips it.

## TODO

- [x] Shared map serialization to shm in `run_sandboxed_with_map`
- [ ] Pre-compiled binary caching (avoid recompiling unchanged scripts)
- [ ] Wall-clock timeout (kill child after N ms)
- [ ] Allocation budget (rlimit on child process)
- [ ] Windows support (no LD_PRELOAD — needs alternative containment)
