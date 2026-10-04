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

The end-to-end test lives at
[`tests/integration/host_aether/`](../../../tests/integration/host_aether/).
Unlike the VM-host tests (which embed one interpreter and call into it),
this one exercises Aether-hosting-Aether: a host program compiles a
*second* Aether program to a native binary and runs it as a subprocess.
So there are two `.ae` files —
[`uses_aether.ae`](../../../tests/integration/host_aether/uses_aether.ae)
is the host/driver, and
[`child.ae`](../../../tests/integration/host_aether/child.ae) is the
trivial child it compiles and spawns (it just prints a sentinel line).
The driver calls `aether_host_run_script(child)` on the **unsandboxed**
rung (the `run_sandboxed` / `*_with_map` paths need
`libaether_sandbox.so` + LD_PRELOAD, so they stay out of the portable CI
path), and asserts the child exited `0`.

The runner is
[`test_host_aether.sh`](../../../tests/integration/host_aether/test_host_aether.sh).
It **SKIPs** (never fails) in two cases: when `build/ae` hasn't been
built, and when the platform isn't Linux/macOS (the bridge `.c` compiles
to an empty stub elsewhere, so there's nothing to link).  Where it does
run, it compiles the bridge into `build/contrib/`, points
`AETHER_AE_PATH` at the freshly built `ae`, sets `AETHER_HOST_CHILD` to
`child.ae`, runs the driver, and greps stdout for the
`PASS: aether host compile + run round-trip` line.

## TODO

- [x] Shared map serialization to shm in `run_sandboxed_with_map`
- [ ] Pre-compiled binary caching (avoid recompiling unchanged scripts)
- [ ] Wall-clock timeout (kill child after N ms)
- [ ] Allocation budget (rlimit on child process)
- [ ] Windows support (no LD_PRELOAD — needs alternative containment)
