# Containment and Sandboxing in Aether

## Background

The Principles of Containment state: the container can see the contained,
but the contained cannot casually reach the container. Each boundary
is an implicit sandbox, and these should be nestable with each level
further restricted.

This pattern appears in:
- Dependency Injection containers (PicoContainer, Spring)
- Virtual Machines (VMware, EC2)
- Docker containers (namespaces, cgroups)
- Java's SecurityManager + ClassLoader hierarchies
- UI component trees (Swing, the DOM)

Aether's builder DSL with closures provides the language-level mechanism
for this: **trailing blocks for declarative structure, closures for
isolation boundaries, and a permissions context that flows inward but
cannot be reached outward.**

## What to contain

A sandbox restricts access to system capabilities. These fall into
categories:

| Category | Examples | Wildcard meaning |
|----------|----------|-----------------|
| **Network outbound** | TCP connect to host:port | Any host, any port |
| **Network inbound** | Listen on port | Any port |
| **Filesystem read** | Read files/dirs by path pattern | Any path |
| **Filesystem write** | Write/create/delete by path | Any path |
| **Process execution** | Spawn subprocesses | Any command |
| **Environment** | Read env vars | Any var |
| **Memory** | Allocation limits | Unlimited |
| **Time** | Wall clock, sleep | Unrestricted |
| **FFI** | Call extern C functions | Any symbol |

The outermost scope starts with no permissions (deny-all) or all
permissions (`"*"`), depending on the trust model. Each nested scope
can only grant a subset of what its parent has.

## Design

### Permission model

Permissions are capabilities granted to a scope. A child scope cannot
exceed its parent's permissions, it can only narrow them.

```
outermost: grant("*")              → everything allowed
  child:   grant_tcp("*.corp", 80) → only TCP to *.corp:80
    inner:  grant_tcp("db.corp", 5432) → only TCP to db.corp:5432
```

### Aether implementation

The sandbox uses:
- **Builder DSL** (`_ctx: ptr`) for declarative nesting
- **Closures** (`fn` params) for isolation, contained code runs in a
  hoisted function, cannot reach parent locals
- **Ref cells / lists** for the permissions registry
- **Wrapper functions** that check permissions before executing

### The key insight

Trailing blocks `{ }` are inlined, they CAN reach the enclosing scope.
These are used for **configuration** (granting permissions).

Closures `|ctx| { }` are hoisted, they CANNOT reach the enclosing scope
unless explicitly passed. These are used for **contained code**.

This maps directly to the container/contained boundary:
- Container configures grants (trailing block, full access)
- Contained runs with only what was granted (closure, restricted)

## Working example

```aether
import std.list

// ---- Sandbox library ----

// Permission entry: category + pattern
// Categories: "tcp", "fs_read", "fs_write", "exec", "env", "*"

add_permission(_ctx: ptr, category: string, pattern: string) {
    list.add(_ctx, category)
    list.add(_ctx, pattern)
}

// Convenience grant functions, configure what the contained can do
grant_all(_ctx: ptr) {
    add_permission("*", "*")
}

grant_tcp(_ctx: ptr, host: string, port: int) {
    add_permission("tcp", host)
}

grant_fs_read(_ctx: ptr, path: string) {
    add_permission("fs_read", path)
}

grant_fs_write(_ctx: ptr, path: string) {
    add_permission("fs_write", path)
}

grant_exec(_ctx: ptr, cmd: string) {
    add_permission("exec", cmd)
}

grant_env(_ctx: ptr, var_name: string) {
    add_permission("env", var_name)
}

// Create a sandbox scope, returns the permission list
sandbox(name: string) {
    perms = list.new()
    println("sandbox: ${name}")
    return perms
}

// Check if a permission is granted
check_permission(perms: ptr, category: string, resource: string) {
    n = list.size(perms)
    for (i = 0; i < n; i = i + 2) {
        // list_get_raw returns the bare ptr; list.get would return a
        // (value, err) tuple that str_eq can't take.
        cat = list_get_raw(perms, i)
        pat = list_get_raw(perms, i + 1)
        // Wildcard "*" matches everything
        if str_eq(cat, "*") == 1 && str_eq(pat, "*") == 1 { return 1 }
        // Category match + pattern match (exact or wildcard)
        if str_eq(cat, category) == 1 {
            if str_eq(pat, "*") == 1 { return 1 }
            if str_eq(pat, resource) == 1 { return 1 }
        }
    }
    return 0
}

// ---- Sandboxed operations ----
// These wrappers check permissions before executing

sandboxed_tcp_connect(perms: ptr, host: string, port: int) {
    if check_permission(perms, "tcp", host) == 1 {
        println("  [ALLOW] tcp connect ${host}:${port}")
        // In real code: tcp.connect(host, port)
        return 1
    }
    println("  [DENY]  tcp connect ${host}:${port}")
    return 0
}

sandboxed_read_file(perms: ptr, path: string) {
    if check_permission(perms, "fs_read", path) == 1 {
        println("  [ALLOW] read ${path}")
        return 1
    }
    println("  [DENY]  read ${path}")
    return 0
}

sandboxed_exec(perms: ptr, cmd: string) {
    if check_permission(perms, "exec", cmd) == 1 {
        println("  [ALLOW] exec ${cmd}")
        return 1
    }
    println("  [DENY]  exec ${cmd}")
    return 0
}

sandboxed_env(perms: ptr, var_name: string) {
    if check_permission(perms, "env", var_name) == 1 {
        println("  [ALLOW] env ${var_name}")
        return 1
    }
    println("  [DENY]  env ${var_name}")
    return 0
}

// ---- Run contained code ----
// The contained closure receives ONLY the permissions context.
// It cannot reach the parent's locals, file handles, or other state.

run_contained(perms: ptr, code: fn) {
    call(code, perms)
}

// ---- Usage ----

main() {
    // Outermost: the application sandbox
    app = sandbox("app") {
        grant_all()  // app can do anything
    }

    // A worker that can only talk to the database and read config
    worker_perms = sandbox("db-worker") {
        grant_tcp("db.internal", 5432)
        grant_fs_read("/etc/app/config.yaml")
        grant_env("DATABASE_URL")
    }

    // An untrusted plugin that can only make HTTP calls to one host
    plugin_perms = sandbox("plugin") {
        grant_tcp("api.example.com", 443)
    }

    // Run contained code, each closure is isolated
    println("")
    println("=== db-worker ===")
    worker_code = |perms: ptr| {
        sandboxed_tcp_connect(perms, "db.internal", 5432)
        sandboxed_tcp_connect(perms, "evil.com", 80)
        sandboxed_read_file(perms, "/etc/app/config.yaml")
        sandboxed_read_file(perms, "/etc/shadow")
        sandboxed_env(perms, "DATABASE_URL")
        sandboxed_env(perms, "AWS_SECRET_KEY")
    }
    run_contained(worker_perms, worker_code)

    println("")
    println("=== plugin ===")
    plugin_code = |perms: ptr| {
        sandboxed_tcp_connect(perms, "api.example.com", 443)
        sandboxed_tcp_connect(perms, "db.internal", 5432)
        sandboxed_exec(perms, "rm -rf /")
        sandboxed_read_file(perms, "/etc/passwd")
    }
    run_contained(plugin_perms, plugin_code)

    println("")
    println("=== app (full access) ===")
    app_code = |perms: ptr| {
        sandboxed_tcp_connect(perms, "anywhere.com", 80)
        sandboxed_exec(perms, "make build")
        sandboxed_read_file(perms, "/any/path")
    }
    run_contained(app, app_code)

    list.free(app)
    list.free(worker_perms)
    list.free(plugin_perms)
}
```

### Expected output

```
sandbox: app
sandbox: db-worker
sandbox: plugin

=== db-worker ===
  [ALLOW] tcp connect db.internal:5432
  [DENY]  tcp connect evil.com:80
  [ALLOW] read /etc/app/config.yaml
  [DENY]  read /etc/shadow
  [ALLOW] env DATABASE_URL
  [DENY]  env AWS_SECRET_KEY

=== plugin ===
  [ALLOW] tcp connect api.example.com:443
  [DENY]  tcp connect db.internal:5432
  [DENY]  exec rm -rf /
  [DENY]  read /etc/passwd

=== app (full access) ===
  [ALLOW] tcp connect anywhere.com:80
  [ALLOW] exec make build
  [ALLOW] read /any/path
```

## How containment works

### Container sees contained

The `sandbox("db-worker") { grant_tcp(...) }` block runs in the parent
scope. The parent configures exactly what the worker can do. The parent
has full visibility into the permissions it grants.

### Contained cannot reach container

The `worker_code = |perms: ptr| { ... }` closure is a hoisted C function.
It receives only `perms` as its argument. It cannot access:
- The parent's `app` permissions
- The parent's `plugin_perms`
- Any variables in main's scope
- Any file handles, sockets, or state from the parent

The closure is compiled to:
```c
static void _closure_fn_N(_closure_env_N* _env, void* perms) {
    // Can only use 'perms', nothing else from parent scope
    sandboxed_tcp_connect(perms, "db.internal", 5432);
}
```

### Nesting rules

Sandboxes can nest, and each level can only narrow permissions:

```aether
outer = sandbox("outer") {
    grant_tcp("*", 0)        // any TCP
    grant_fs_read("*")       // any file read
}

inner = sandbox("inner") {
    // Can only grant what outer has
    grant_tcp("db.corp", 5432)   // narrowed to one host
    // fs_read not granted, inner cannot read files
}
```

The `check_permission` function only looks at the scope's own grants.
If the parent didn't grant something, the child can't either, because
the child's grant functions only add to the child's own permission list.

### The `*` wildcard

`grant_all()` adds `("*", "*")` a wildcard category and pattern.
`check_permission` checks wildcards first, so `grant_all()` permits
everything. Individual grants like `grant_tcp("host", port)` add
specific entries. The check function matches exact or wildcard.

## Mapping to your Java SecurityPolicyDemo

| Java concept | Aether equivalent |
|-------------|-------------------|
| `new SecureSystem() {{ ... }}` | `sandbox("name") { ... }` |
| `classLoader(() -> { ... })` | `sandbox("child") { ... }` (nested) |
| `classPathElement("x.jar")` | Not applicable (Aether is single-binary) |
| `grant(new SocketPermission(...))` | `grant_tcp("host", port)` |
| `component("Bear")` | `run_contained(perms, bear_code)` |
| SecurityManager check | `check_permission(perms, cat, resource)` |
| ClassLoader isolation | Closure isolation (hoisted C function) |

## Mapping to Docker/VM containment

| Docker/VM concept | Aether equivalent |
|-------------------|-------------------|
| Container image | `sandbox("name") { grants... }` |
| Volume mount (read-only) | `grant_fs_read("/path")` |
| Volume mount (read-write) | `grant_fs_write("/path")` |
| Port mapping | `grant_tcp("host", port)` |
| `--cap-drop ALL` | No `grant_all()` deny by default |
| `--cap-add NET_RAW` | `grant_tcp("*", 0)` |
| Entrypoint/CMD | `run_contained(perms, code)` |
| Namespace isolation | Closure cannot reach parent scope |

## What's enforced

The sandbox intercepts at the **stdlib level**. These calls are checked
transparently, the contained code uses normal stdlib wrappers and
cannot tell it's sandboxed. The check happens inside the underlying
`_raw` C function, so both the Go-style wrapper (`tcp.connect`) and
direct calls to the raw extern (`tcp_connect_raw`) are enforced.

| Wrapper | Raw C symbol | Category | Enforced |
|---------|--------------|----------|----------|
| `tcp.connect(host, port)` | `tcp_connect_raw` | `"tcp"` | Yes, wrapper returns `(null, "connect failed")` if denied |
| `tcp.listen(port)` | `tcp_listen_raw` | `"tcp_listen"` | Yes, wrapper returns `(null, "listen failed")` if denied |
| `file.open(path, "r")` | `file_open_raw` | `"fs_read"` | Yes, wrapper returns `(null, "cannot open file")` if denied |
| `file.open(path, "w")` | `file_open_raw` | `"fs_write"` | Yes, wrapper returns `(null, "cannot open file")` if denied |
| `file.exists(path)` | `file_exists` | `"fs_read"` | Yes, returns 0 if denied |
| `file.delete(path)` | `file_delete_raw` | `"fs_write"` | Yes, wrapper returns `"cannot delete file"` if denied |
| `file.size(path)` | `file_size_raw` | `"fs_read"` | Yes, wrapper returns `(0, "cannot stat file")` if denied |
| `os.system(cmd)` | `os_system` | `"exec"` | Yes, returns -1 if denied |
| `os.exec(cmd)` | `os_exec_raw` | `"exec"` | Yes, wrapper returns `("", "command failed")` if denied |
| `os.getenv(name)` | `os_getenv` | `"env"` | Yes, returns null if denied |

Nested sandboxes are intersected, an inner sandbox cannot escalate
beyond what the outer sandbox grants.

## Per-function effect tags

The sandbox and `--with=fs,net,os` gate the *whole program*. Effect tags add a
**finer, per-function** capability axis, checked statically at compile time
(zero runtime cost):

```aether
@pure
parse_config(text: string) -> Config { ... }   // must touch no fs/net/os

@no_fs
classify(req: Request) -> int { ... }            // must touch no filesystem
```

- `@pure` forbids all of fs/net/os; `@no_fs` / `@no_net` / `@no_os` forbid one
  capability each (stackable: `@no_fs @no_net f() {}`).
- A whole-program pass walks the call graph from each tagged function. If it
  reaches a capability-gated stdlib call, `file.*`/`dir.*`/`path.*` (fs),
  `tcp.*`/`http.*` (net), `os.*` (os), directly or through another function,
  the compile fails with the offending call path named.
- Composes with `--with=`: that switch says the *program* may use fs; an
  effect tag says *this function* must not. The two axes are independent.
- A raw `extern` call is unclassifiable (the compiler can't know a foreign
  symbol's effects), so it is not flagged, the same boundary the `--with=`
  import gate has.

## Pattern matching

| Pattern | Matches | Example |
|---------|---------|---------|
| `"*"` | Anything | `grant_tcp("*")` |
| `"/etc/*"` | Prefix match | `/etc/hostname`, `/etc/app/config.yaml` |
| `"*.example.com"` | Suffix match | `api.example.com`, `db.example.com` |
| `"echo *"` | Prefix match | `echo hello`, `echo goodbye` |
| `"exact.host"` | Exact only | Only `exact.host` |

### Path resolution before fs match

For `fs_read` / `fs_write` grants the LD_PRELOAD layer resolves the
resource path with `realpath(3)` *before* running the pattern match.
This closes the path-identity bypass: a grant for `/tmp/*` can no
longer be subverted by `/tmp/../etc/shadow`, and a symlink under a
granted prefix can no longer escape it.

When the path doesn't exist yet (writing a new file), the resolver
falls back to resolving the parent directory and reattaching the
basename, the kernel will refuse the create syscall otherwise, and
the parent must be real for any meaningful operation. If even the
parent can't be resolved (truly unusable path), the original string
is matched as a baseline, same behaviour as before this fix, so the
gate never silently tightens or loosens at the edges.

Non-fs categories (`tcp`, `env`, `exec`) carry no path semantics;
their resources are matched verbatim.

## Scope boundaries

The sandbox mediates stdlib I/O only. These are the places its enforcement line sits, and where it deliberately stops.

### Extern calls bypass the sandbox

Aether's `extern` keyword lets code call raw C functions directly. If contained code declares:

```aether
extern fopen(path: string, mode: string) -> ptr
```

It can call `fopen` directly, bypassing `file_open` and its sandbox
check entirely. The sandbox enforces stdlib calls, not raw C.

**Today:** this is not enforced. Extern calls inside sandboxed closures
compile and run without restriction.

**Impact:** the sandbox is effective when you control compilation and
don't put `extern` declarations in contained code. It's the same trust
model as Docker, you trust the container runtime (Aether's stdlib),
but the contained code must use the provided APIs.

### Proposed fix: compiler-enforced extern check

The compiler should reject `extern` function calls inside closures
passed to `run_sandboxed`. Implementation:

1. Track whether codegen is inside a sandboxed closure (similar to
   `in_trailing_block`)
2. When generating an `AST_FUNCTION_CALL` that resolves to an
   `AST_EXTERN_FUNCTION`, check if we're in a sandboxed context
3. Emit a compile error: `"extern calls not permitted in sandboxed code"`

This is a typechecker/codegen change, not a runtime change. It would
make the sandbox inescapable at compile time, analogous to Java's
ClassLoader preventing contained code from seeing restricted classes.

The key insight: extern restriction is a **compilation** concern. The
sandbox grants (tcp, fs, exec, env) are a **runtime** concern. Both
are needed for complete containment:

```
Compile time: reject extern in sandboxed closures
Runtime:      check grants before stdlib operations
```

This matches Java's two-layer model: ClassLoader isolation (compile/link
time) + SecurityManager checks (runtime). Java removed the runtime layer
(SecurityManager) but kept the compile/link layer (module system). Aether
should have both.

### Other boundaries

- **No `deny` grants.** The model is deny-by-default, grant what's
  needed. There is no way to grant broadly then carve exceptions.
  This is intentional for the initial release.

- **No per-connection port filtering.** `grant_tcp("host")` allows
  any port on that host. Port-level grants would require extending
  the pattern format.

- **Application-level only.** See "Cross-process containment" below
  for extending enforcement to child processes.

## Denial logging

Denied operations are logged by default. Three modes controlled via
the `AETHER_SANDBOX_LOG` environment variable:

| Mode | Env var | Behaviour |
|------|---------|-----------|
| **File** (default) | `AETHER_SANDBOX_LOG=file` or unset | Writes to `./aether-sandbox.log` |
| **Stderr** | `AETHER_SANDBOX_LOG=stderr` | `AETHER_DENIED: category resource` |
| **Silent** | `AETHER_SANDBOX_LOG=none` | No output |

The stderr format uses the `AETHER_DENIED:` prefix for grep:

```bash
./my-app 2>&1 | grep AETHER_DENIED
```

The file mode is the default because it's the right experience for
a novice: run the program, it fails, open `aether-sandbox.log`, see
exactly what to grant. Self-service.

## Audit trail

Denial logging above covers the **cross-process** (LD_PRELOAD) layer.
The **in-process** layer, the stdlib grant checks an in-program
`sandbox { }` block drives, and the checks embedded/hosted plugins go
through, has its own audit trail, recording every permission decision,
*allowed as well as denied*.

### Live sink

Set `AETHER_SANDBOX_AUDIT` same shape as `AETHER_SANDBOX_LOG`:

| Mode | Env var | Behaviour |
|------|---------|-----------|
| **Off** (default) | `AETHER_SANDBOX_AUDIT=none` or unset | No sink. The ring buffer (below) is still populated. |
| **Stderr** | `AETHER_SANDBOX_AUDIT=stderr` | `AETHER_ALLOWED:` / `AETHER_DENIED:` lines as checks happen. |
| **File** | `AETHER_SANDBOX_AUDIT=file` | Same lines appended to `./aether-sandbox.log`. |

Off by default because, unlike denial-only logging, auditing records
*allowed* checks too and is verbose. Denials keep the `AETHER_DENIED:`
prefix the rest of the tooling greps for; allowed checks get a parallel
`AETHER_ALLOWED:`.

### Queryable log, `std.audit`

The last 256 checks are also held in an in-memory ring buffer that the
program can read back through the `std.audit` module:

```aether
import std.audit

// ... run sandboxed work ...

n = audit.count()
for (i = 0; i < n; i = i + 1) {
    cat, res, allowed = audit.entry(i)   // allowed: 1 = passed, 0 = denied
    // ...
}
println("${audit.denied_count()} denials of ${n} checks")
audit.clear()                            // scope the next query
```

This is the programmatic face of the audit trail: counting denials,
asserting an expected access pattern in a test, building a "why was
this blocked" report. Worked example: `examples/audit-demo.ae`.

## Platform support

The runtime-process layer (`libaether_sandbox.so` + `spawn_sandboxed`)
builds and runs on **Linux and FreeBSD**. Both ship a dynamic linker
that honours `LD_PRELOAD` and `dlsym(RTLD_NEXT, ...)`, which is all the
interception model requires.

| Platform | Runtime sandbox | Notes |
|----------|----------------|-------|
| **Linux** | LD_PRELOAD (`libaether_sandbox.so`) | Preload locates itself via `/proc/self/exe`. Intercepts the glibc large-file (`open64`/`fopen64`/`mmap64`) and `clone3` entry points in addition to the common surface. |
| **FreeBSD** | LD_PRELOAD + **Capsicum self-sandbox** + **Casper** | LD_PRELOAD works as on Linux (preload locates itself via `sysctl(KERN_PROC_PATHNAME)` no `/proc`; no `*64`/`clone3`). An Aether process launched with `AETHER_CAPSICUM=1` enters FreeBSD capability mode at startup, kernel-enforced, unbypassable containment; `spawn_sandboxed` sets that for Aether children automatically. `std.casper` lets a capability-mode process still resolve DNS / read passwd / read sysctl by delegating to the Casper daemon. See [`aether_compared_to_capsicum.md`](aether_compared_to_capsicum.md). |
| **macOS** | Not supported | `DYLD_INSERT_LIBRARIES` exists but the hardened runtime ignores it for system binaries. `spawn_sandboxed` is a stub that fails loudly. |
| **Windows** | Not supported | No `LD_PRELOAD` equivalent. `spawn_sandboxed` is a stub. |

The in-process layer (module boundary, scope boundary, and the stdlib
grant checks) is pure C and works on every platform regardless.

## Security review checklist

Before signing off an Aether sandboxed deployment, verify:

### 1. LD_PRELOAD is active

The sandbox preload library must be in the LD_PRELOAD chain.
Without it, there is no interception, all libc calls pass through
unfiltered.

```bash
# Verify for spawned processes:
grep LD_PRELOAD /proc/<pid>/environ
```

For `spawn_sandboxed()` this is automatic. For manual deployments,
the launch script must set `LD_PRELOAD=libaether_sandbox.so`.

### 2. No wildcard exec grants

`grant_exec("*")` allows executing any binary, including statically
linked ones that bypass LD_PRELOAD. Exec grants should be specific:

```aether
// Bad:
grant_exec("*")

// Good:
grant_exec("python3")
grant_exec("echo *")
```

### 3. No statically linked binaries in granted paths

A statically linked binary doesn't use libc, it talks to the kernel
directly, bypassing all LD_PRELOAD interception. Verify no static
binaries exist in any path accessible to the sandbox:

```bash
# Check for statically linked binaries in /usr/bin
find /usr/bin -type f -exec file {} \; | grep "statically linked"
# Should return nothing on stock Debian/Ubuntu
```

Go binaries are the primary risk, they're often statically linked.
Don't grant `fs_read` to directories containing Go binaries unless
the sandbox needs them.

### 4. Native loading is restricted

`grant_native("*")` allows `dlopen` of any shared library, including
`libc.so.6` directly. This enables ctypes/Fiddle/DynaLoader escape.
Don't grant it unless necessary. Without it:

- Python `ctypes.CDLL("libc.so.6")` is blocked
- Perl `DynaLoader::dl_load_file("libc.so.6")` is blocked
- Ruby `Fiddle.dlopen("libc.so.6")` loads but calls are still intercepted
- Lua has no FFI, not a vector

### 5. Grant list follows least privilege

Review grants like you'd review Docker capabilities:

```aether
// Each grant should be justified
worker = sandbox("worker") {
    grant_env("DATABASE_URL")       // needs DB connection string
    grant_fs_read("/app/config/*")  // needs config files
    grant_tcp("db.internal")        // talks to database
    // Nothing else, deny by default
}
```

No `grant_all()` in production. No broad `grant_fs_read("*")`.
No `grant_env("*")`.

### 6. Log file is monitored

`aether-sandbox.log` records all denied operations. In production:
- Ship it to your log aggregator
- Alert on unexpected denials (may indicate misconfiguration or attack)
- Periodically review for grants that can be tightened

### 7. Sandbox code is auditable

The sandbox policy is Aether source code, readable, diffable,
reviewable in a PR. Unlike Docker's layered Dockerfile + compose +
k8s manifests, the entire policy is in one place:

```bash
# The security review is: read this file
cat sandbox-config.ae
```

### What's blocked

| Attack vector | Blocked by |
|--------------|------------|
| File read/write outside grants | `open`/`fopen`/`openat` interception |
| Network to non-granted hosts | `connect` interception |
| Env var access outside grants | `getenv` interception |
| Process execution outside grants | `execve` interception |
| `dlopen("libc.so.6")` (ctypes escape) | `dlopen` interception |
| Raw `syscall()` | `syscall` interception |
| Shellcode via `mmap(PROT_EXEC)` | `mmap`/`mmap64` interception |
| Shellcode via `mprotect(PROT_EXEC)` | `mprotect` interception |
| `fork()` / `vfork()` / `clone3()` | Blocked by default at the **kernel** level (seccomp-bpf in `spawn_sandboxed`, see below). Grant with `fork:*`. |

### What's NOT blocked (requires kernel enforcement)

| Attack vector | Why |
|--------------|-----|
| Statically linked binaries | Don't use libc, bypass LD_PRELOAD entirely. Process creation by such binaries is still caught by the seccomp-bpf clone fence (see below). |
| `ptrace` self | Can modify own process memory to skip checks |
| Kernel exploits | We're userspace, can't defend against kernel bugs |

For these vectors, combine with OS-level sandboxing (seccomp-bpf,
Linux namespaces, OpenBSD pledge/unveil) for defence in depth.

### Kernel-level fence for process creation, the clone3 gap, closed

The LD_PRELOAD layer cannot intercept syscalls that don't go through
exported libc symbols. Two notable bypasses, both common:

- **glibc `__vfork`** on x86_64 is an inline `syscall` instruction in
  libc's text. LD_PRELOAD never sees it, there's no symbol to interpose.
- Any program calling `syscall(SYS_clone3, …)` (or issuing the raw
  syscall instruction directly).

Modern gcc on Linux uses both paths to spawn `cc1`/`as`/`ld`. So a
purely libc-symbol-level fence on `fork:*` was a paper tiger against
real toolchains.

`spawn_sandboxed` therefore installs a **seccomp-bpf filter** on the
child side (post-fork, pre-exec) that traps `clone`, `clone3`, `fork`,
and `vfork` with `EPERM` regardless of how they're invoked, when
`fork:*` is not in the grant list. This is true kernel-level
enforcement, immune to call-site obfuscation. Requires Linux ≥ 3.5
(`PR_SET_NO_NEW_PRIVS` + `PR_SET_SECCOMP`). x86_64-only filter; on
other architectures the filter falls through to ALLOW, so the
existing libc-level fence is the only fence there. The kernel fence
is automatic and not configurable through the grant grammar: if
`fork:*` is granted, the filter isn't installed at all; otherwise it
fires unconditionally. If the kernel doesn't support seccomp, the
child aborts with `exit(126)` rather than silently running uncontained
a `spawn_sandboxed` that asked for containment we can't deliver
must not exec.

The complementary gap, `connect()` and
`execve()` issued by statically-linked binaries or raw asm, remains.
Those callers can still skip the LD_PRELOAD layer for network reach
and exec, since the seccomp filter here targets only the
process-creation family. Adding seccomp arms for `connect`/`execve`
is a natural extension but introduces a much wider filter that needs
its own discussion (an allowlist by host/path becomes a per-call BPF
program); not done here.

### Interception surface, what LD_PRELOAD sees and what it doesn't

**Lesson from Google App Engine (2013):** An intern broke out of
App Engine's Java sandbox by exploiting a gap between what the
bytecode rewriter (ASM) thought it serialized and what the JVM
actually parsed. The architectural lesson: if the enforcement layer
and the execution layer see different things, an attacker can exploit
the difference.

For Aether, the equivalent gap is: **we intercept specific libc
functions, but the kernel offers many alternative paths to the same
operations.** A determined attacker who knows Linux internals can
use paths we don't intercept. The tables below enumerate the surface
so you can reason about it explicitly rather than assume coverage.
See `docs/next-steps.md` → *Interception surface expansion* for the
in-flight work to widen the LD_PRELOAD surface.

#### Filesystem, not intercepted

| Function / syscall | What it does | Risk |
|-------------------|-------------|------|
| `openat2()` | Newer open variant (Linux 5.6+) | Bypasses our `open`/`openat` interception |
| `open_by_handle_at()` | Open file by kernel handle | Bypasses path-based checks entirely |
| `name_to_handle_at()` | Get kernel handle for a path | Used with `open_by_handle_at` |
| `sendfile()` | Kernel-level copy between fds | Reads files without `read()` |
| `copy_file_range()` | Kernel-level file-to-file copy | No `open` interception needed if fd already obtained |
| `io_uring` | Async I/O submission ring | Submits read/write/open ops that bypass libc entirely |
| `readlink()` / `readlinkat()` | Read symlink target | Information leak about filesystem layout |
| `stat()` / `fstat()` / `lstat()` | File metadata | Can probe existence of files we intend to hide |
| `access()` / `faccessat()` | Check file permissions | Same as stat, probes file existence |
| `getdents64()` | Read directory entries | Lists files in granted directories |

#### Network, not intercepted

| Function / syscall | What it does | Risk |
|-------------------|-------------|------|
| `sendto()` / `sendmsg()` | Send data on already-open socket | If socket was opened before sandbox |
| `recvfrom()` / `recvmsg()` | Receive data | Same, works on pre-existing sockets |
| `accept4()` | Accept incoming connection (Linux variant) | Only `accept()` is intercepted; `accept4()` is not |
| `socketpair()` | Create paired sockets | Local IPC bypass |
| UDP (`SOCK_DGRAM`) | Connectionless networking | `connect` interception only checks TCP |

#### Process / execution, not intercepted

| Function / syscall | What it does | Risk |
|-------------------|-------------|------|
| `execveat()` | Execute by fd (Linux 3.19+) | Alternative to `execve` |
| `clone()` (arch-specific) | Create new process/thread | Variadic, arch-dependent, hard to intercept portably |
| `prctl()` | Process control | Can disable dumpable, change name, etc. |
| `ptrace()` | Debug/modify another process | Can modify own memory to skip checks |
| `process_vm_readv/writev()` | Read/write another process's memory | Cross-process data exfiltration |

#### Memory, not intercepted (beyond mmap/mprotect)

| Function / syscall | What it does | Risk |
|-------------------|-------------|------|
| `mremap()` | Resize/move memory mapping | Could move non-exec mapping to exec region |
| `shmat()` | Attach shared memory segment | IPC without file I/O |
| `memfd_create()` | Create anonymous file in memory | Combined with write + exec = smuggled binary |

#### Why this list matters

Every item above is a path that our LD_PRELOAD interception doesn't
see. For **cooperative containment** (plugins, workers, hosted
languages) this doesn't matter, Python's `os.getenv()` goes through
libc `getenv()`, not raw `syscall(SYS_getenv)`. Normal code uses
the functions we intercept.

For **adversarial containment** (untrusted code trying to escape),
this list is the attack surface. Each item is a potential bypass.
The mitigation is kernel enforcement:

```
Aether sandbox    → intercepts the libc surface normal code uses
+ seccomp-bpf     → closes the raw-syscall bypasses above
= defence in depth
```

The App Engine lesson applies: **enumerate what you don't intercept,
don't pretend the list of what you do intercept is complete.**

### What the contained code sees

To the contained code, a denied resource looks like it was never
there: `open("/etc/shadow")` fails as if the file did not exist, and
`getenv("AWS_SECRET_KEY")` returns null as if the variable were unset.
The check is invisible; the contained code can't distinguish a denial
from an absent resource.

## Sandboxing bash scripts

Bash is a common choice for build steps, deployment scripts, and
system orchestration. It can be sandboxed via `spawn_sandboxed`,
but with an important caveat.

### The model: Aether orchestrates, bash works

Aether is the guard. Bash is the tool. Each bash invocation gets
specific grants for that step:

```aether
compile_sandbox = sandbox("compile") {
    grant_fs_read("src/*")
    grant_fs_write("build/*")
    grant_exec("/usr/bin/gcc")
    grant_exec("/usr/bin/bash")
}

deploy_sandbox = sandbox("deploy") {
    grant_fs_read("build/bin/*")
    grant_tcp("deploy.internal")
    grant_exec("/usr/bin/bash")
    grant_exec("/usr/bin/scp")
    grant_env("DEPLOY_TOKEN")
}

spawn_sandboxed(compile_sandbox, "bash", "-c 'gcc -o build/app src/*.c'")
spawn_sandboxed(deploy_sandbox, "bash", "-c 'scp build/bin/app deploy.internal:/opt/'")
```

Each bash step inherits LD_PRELOAD. Every external command that bash
spawns (`gcc`, `scp`, `curl`, `cat`) is sandboxed, checked against
the step's grants.

### What's sandboxed and what's not

| Bash operation | Sandboxed? | Why |
|---------------|-----------|-----|
| `gcc src/*.c` | Yes | External command, inherits LD_PRELOAD |
| `cat /etc/shadow` | Yes | External command, `open()` intercepted |
| `curl http://evil.com` | Yes | External command, `connect()` intercepted |
| `scp file host:path` | Yes | External command, `connect()` intercepted |
| `echo $SECRET` | No | Shell builtin, no libc call |
| `read -r line < /etc/shadow` | No | Shell builtin redirection |
| `exec 3<>/dev/tcp/host/80` | No | Bash built-in network, direct kernel |

### Why builtins don't matter

The builtin gap sounds alarming but isn't a practical concern:

- **`echo`** doesn't access protected resources. It writes to stdout.
  If stdout is a terminal, the output is visible to the user who
  launched the sandbox, not an escalation.
- **`read < file`** is a concern in theory, but bash scripts that
  read files almost always use `cat` or other external commands
  which ARE sandboxed.
- **`/dev/tcp`** is the real risk, bash can open TCP connections
  without an external command. However, `/dev/tcp` is a compile-time
  option in bash and is disabled in many distributions (Debian,
  Ubuntu disable it by default).

### The Docker analogy

This is the same model as Docker. A Docker container doesn't restrict
what the shell does internally, it restricts what resources are
mounted and what network is available. Bash can `echo` all it wants
inside a container. It can't `curl` to a host that isn't in the
container's network.

Aether's sandbox is the same: bash runs freely inside the
permission boundary. The boundary is what matters.

### Not recommended: forking bash

Forking bash (~140K lines of C) to add sandbox checks to builtins
is technically possible but creates a maintenance burden. Every
bash security patch requires a rebase. Users won't install a custom
bash. The LD_PRELOAD model with external command interception is
the practical approach.

### Not recommended: rbash

Bash's restricted mode (`rbash`) disables `cd`, PATH changes, and
redirections. It solves a different problem, restricting the user
FROM bash features, not restricting bash's access to resources.
It's not a substitute for sandbox grants.

## Cross-process containment (future)

The sandbox currently enforces within the Aether process. The natural
next step: spawn a child process (Python, Ruby, Node, any language)
that is unknowingly sandboxed by the same grants.

### The approach: LD_PRELOAD interception

Aether spawns the child process with `LD_PRELOAD=libaether_sandbox.so`.
This shared library intercepts libc calls and checks against the Aether
grant list, the child process has no idea.

```
Aether process                     Python process
─────────────                      ──────────────
worker = sandbox("worker") {       import socket
    grant_tcp("api.example.com")   s.connect(("api.example.com", 443))  → OK
    grant_fs_read("/app/data/*")   open("/app/data/input.csv")          → OK
    grant_env("DATABASE_URL")      os.getenv("DATABASE_URL")            → OK
}                                  s.connect(("evil.com", 80))          → denied
                                   open("/etc/shadow")                  → denied
spawn_sandboxed(worker,            os.getenv("AWS_SECRET_KEY")          → denied
    "python3", "plugin.py")
```

### How it works

1. Aether writes the grant list to shared memory (or a temp file)
2. Aether spawns the child with `LD_PRELOAD=libaether_sandbox.so`
3. The preload library intercepts libc calls:

   | libc call | Checks | On deny |
   |-----------|--------|---------|
   | `connect()` | `grant_tcp` against resolved hostname | Returns `EACCES` |
   | `open()` | `grant_fs_read` or `grant_fs_write` by path + mode | Returns `EACCES` |
   | `execve()` | `grant_exec` against command path | Returns `EPERM` |
   | `getenv()` | `grant_env` against variable name | Returns `NULL` |

4. Python, Ruby, Node, anything using libc, hits the interception.
   No language-specific hooks needed.

### Why LD_PRELOAD, not kernel features

| Approach | Keeps Aether's grant model? | Cross-language? | Nesting? |
|----------|---------------------------|-----------------|----------|
| **LD_PRELOAD** (recommended) | Yes, same patterns, same checker | Yes, any libc language | Yes, grants in shared memory |
| seccomp-bpf | No, only sees syscall numbers, not paths | Yes | No nesting model |
| Landlock | Partial, filesystem only, no env/exec globs | Yes | No |
| Linux namespaces | No, coarse-grained isolation | Yes | No |
| gVisor | No, full kernel reimpl, massive dependency | Yes | No |

LD_PRELOAD is the only approach that preserves:
- **Same grant DSL**, `grant_tcp("*.example.com")` works identically
- **Same glob patterns**, prefix, suffix, wildcard, exact
- **Same invisibility**, the child can't tell it's sandboxed
- **Same nesting**, parent and child share a grant stack via shared memory
- **Zero kernel dependencies**, works on stock Linux, macOS, FreeBSD

### Precedent

This technique is proven in production:
- **proxychains** / **tsocks**, intercept `connect()` to route through SOCKS
- **faketime**, intercept `time()` / `gettimeofday()` to lie about the clock
- **libeatmydata**, intercept `fsync()` to skip disk flushes for test speed
- **Electric Fence**, intercept `malloc()` for memory debugging

### What it looks like in Aether

```aether
worker = sandbox("python-worker") {
    grant_tcp("*.internal")
    grant_tcp("api.example.com")
    grant_fs_read("/app/data/*")
    grant_fs_write("/tmp/output/*")
    grant_env("DATABASE_URL")
    grant_env("APP_MODE")
    grant_exec("python3")
}

// Python runs with normal socket/open/getenv calls.
// libaether_sandbox.so intercepts at libc level.
// Python has no idea it's contained.
spawn_sandboxed(worker, "python3", "plugin.py")
```

### Implementation sketch

The preload library (`libaether_sandbox.so`) would be ~200 lines of C:

```c
// libaether_sandbox.so, LD_PRELOAD interception

#include <dlfcn.h>    // dlsym for real libc functions
#include <sys/mman.h> // shared memory for grant list

// Load grants from shared memory (written by Aether parent)
static grant_list* grants = NULL;
static void __attribute__((constructor)) init() {
    int fd = shm_open("/aether_sandbox_PID", O_RDONLY, 0);
    grants = mmap(...);
}

// Intercept connect()
int connect(int fd, const struct sockaddr* addr, socklen_t len) {
    char* host = resolve_addr(addr);
    if (!check_grant(grants, "tcp", host)) {
        errno = EACCES;
        return -1;
    }
    return real_connect(fd, addr, len);  // dlsym(RTLD_NEXT, "connect")
}

// Intercept open()
int open(const char* path, int flags, ...) {
    const char* cat = (flags & O_WRONLY || flags & O_RDWR) ? "fs_write" : "fs_read";
    if (!check_grant(grants, cat, path)) {
        errno = EACCES;
        return -1;
    }
    return real_open(path, flags, ...);
}

// Intercept getenv()
char* getenv(const char* name) {
    if (!check_grant(grants, "env", name)) return NULL;
    return real_getenv(name);
}
```

The grant checking logic is identical to the in-process checker, same
glob patterns, same prefix/suffix matching. One codebase, two enforcement
points: in-process (stdlib checks) and cross-process (LD_PRELOAD).

## Comparison with other systems

### Java SecurityManager (deprecated in Java 17, removed in Java 24)

| Aspect | Java SecurityManager | Aether Sandbox |
|--------|---------------------|----------------|
| Enforcement | JVM runtime, every Socket(), FileInputStream() checked | Stdlib runtime, every tcp_connect(), file_open() checked |
| Granularity | Per-classloader (code origin) | Per-scope (builder block nesting) |
| Policy format | External `.policy` file | Inline DSL in Aether code |
| Nesting | ClassLoader hierarchy | Context stack, inner can't escalate |
| Visibility | SecurityException thrown (contained knows) | Returns null/0 (contained can't tell) |
| Bypass | Reflection, `setSecurityManager(null)` | Extern C calls (proposed fix above) |
| Deny grants | Yes (grant then deny) | No, deny-by-default, grant only |
| Deprecated/removed | Deprecated 17, removed 24, too complex, everyone disabled it | N/A |

### gVisor (Google, Go)

| Aspect | gVisor | Aether Sandbox |
|--------|--------|----------------|
| Intercepts | Linux syscalls (200+) | Stdlib functions (tcp, fs, os, env) |
| Enforcement | Kernel boundary, inescapable | Stdlib boundary, extern bypasses |
| Performance | Significant (every syscall through Go) | Near zero (pointer check + list scan) |
| Nesting | Containers don't nest | Sandboxes nest, inner restricted |
| Implementation | 100k+ lines of Go | ~80 lines of C + codegen |
| Scope | Full OS virtualization | Application-level containment |

### Docker / OCI

| Docker concept | Aether equivalent |
|---------------|-------------------|
| `FROM scratch` (empty image) | `sandbox("name") { }` (deny all) |
| `--cap-drop ALL --cap-add NET_RAW` | Only grant what's needed |
| Volume mount read-only | `grant_fs_read("/path/*")` |
| Volume mount read-write | `grant_fs_write("/path/*")` |
| `--network=none` | No `grant_tcp` |
| Entrypoint | `run_sandboxed(perms) \|ctx\| { ... }` |
| Nested containers | Nested sandboxes, inner can't escalate |

### OpenBSD pledge / unveil

The closest philosophical match. A process declares upfront what
capabilities it will use; the kernel kills it if it tries anything else.

```c
// OpenBSD C
pledge("stdio rpath inet", NULL);  // only stdio, read files, and network
unveil("/etc", "r");               // only /etc readable
unveil("/tmp", "rwc");             // /tmp read-write-create
unveil(NULL, NULL);                // lock it down, no more unveil calls
```

```aether
// Aether equivalent
worker = sandbox("worker") {
    grant_fs_read("/etc/*")
    grant_fs_write("/tmp/*")
    grant_tcp("*")
}
```

| Aspect | pledge/unveil | Aether Sandbox |
|--------|--------------|----------------|
| Enforcement | Kernel, process killed on violation | Stdlib, returns null/0 on violation |
| Granularity | Category-level (pledge) + path-level (unveil) | Both in one grant system |
| Irreversible | Yes, can only narrow after pledge | Yes, inner sandbox can't escalate |
| Visibility | Process gets SIGABRT (knows it was caught) | Returns null (can't tell why) |
| Nesting | Not nested, one pledge per process | Nested sandboxes with intersection |

**Inspiration:** pledge's simplicity, a flat list of capability strings.
No XML, no policy files, no 30 permission classes. Aether follows this:
`grant_tcp`, `grant_fs_read`, `grant_exec`. That's it.

**Inspiration:** unveil's path model, lock down the filesystem view
before running untrusted code. Aether's `grant_fs_read("/etc/*")` is
unveil with glob syntax.

### Deno permissions

Deno (the Node.js successor by Ryan Dahl) has the most similar runtime
model. Permissions are granted at launch, enforced at runtime, and the
contained code uses normal APIs.

```bash
# Deno CLI
deno run --allow-net=api.example.com --allow-read=/tmp --allow-env=HOME app.ts
```

```aether
// Aether equivalent
app = sandbox("app") {
    grant_tcp("api.example.com")
    grant_fs_read("/tmp/*")
    grant_env("HOME")
}
```

| Aspect | Deno | Aether Sandbox |
|--------|------|----------------|
| Enforcement | V8 runtime, every fetch(), readFile() checked | Stdlib, every tcp_connect(), file_open() checked |
| Policy format | CLI flags | Builder DSL (code) |
| Granularity | Per-domain, per-path, per-env | Same, with glob patterns |
| Nesting | No nested permissions | Nested sandboxes with intersection |
| Prompt mode | `--prompt` asks user at runtime | No, grants declared upfront |
| Visibility | Throws PermissionDenied error | Returns null (invisible) |
| Language | TypeScript/JavaScript (interpreted) | Aether (compiled to C) |

**Inspiration:** Deno proved that per-resource grants work in practice
for real applications. The `--allow-net=host` model maps directly to
`grant_tcp("host")`. Deno's mistake was CLI flags, policy should be
code, not command-line arguments. Aether's builder DSL fixes this.

**Inspiration:** Deno's deny-by-default. Before Deno, Node.js had no
permissions at all. Deno showed that deny-by-default is practical and
developers adapt quickly. Aether follows the same principle.

### WebAssembly WASI (Capability-based)

WASI takes the most extreme position: no ambient authority at all.
A WASM module receives only the file handles and capabilities that
the host explicitly passes to it. There are no global functions like
`fopen` everything comes through explicit parameters.

```javascript
// Host (JavaScript) passes only what the module can use
const wasi = new WASI({
    preopens: { '/data': '/host/path/to/data' },  // only this dir
    env: { 'APP_MODE': 'production' },             // only this var
});
```

| Aspect | WASI | Aether Sandbox |
|--------|------|----------------|
| Model | Capability-based, no ambient authority | Grant-based, ambient authority filtered |
| Enforcement | WASM runtime, hardware-level isolation | Stdlib, software checks |
| Bypass | Impossible, no syscalls available | Extern C calls (proposed fix) |
| Nesting | Host composes capabilities | Nested sandboxes |
| Ergonomics | Verbose, every capability threaded through | Clean, normal API calls, transparent checks |

**Inspiration:** WASI's "no ambient authority" ideal. Aether can't
fully achieve this (compiled C has ambient access to everything), but
the extern restriction proposal moves toward it, contained code
would have no way to access capabilities not granted by the sandbox.

### Cloudflare Workers / V8 Isolates

Workers run JavaScript in V8 isolates with a stripped-down API. No
filesystem. No raw sockets. Only `fetch()` for network, and only to
allowed origins. Each worker is a function that receives a request
and returns a response.

| Aspect | Workers | Aether Sandbox |
|--------|---------|----------------|
| Model | Stripped API, missing functions, not checked functions | Full API, checked transparently |
| Filesystem | None | Granted per-path |
| Network | `fetch()` only | `tcp_connect()` checked per-host |
| Isolation | V8 isolate (separate heap) | Closure (separate scope) |
| Startup | Immediate (V8 snapshots) | Immediate (compiled native) |

**Inspiration:** the idea that isolation doesn't require heavyweight
VMs or containers. A V8 isolate is just a memory boundary. An Aether
closure is just a scope boundary. Both achieve containment without
OS-level virtualization.

## Why doPrivileged is unnecessary

Java's `AccessController.doPrivileged()` let contained code temporarily
escalate permissions to perform a privileged operation:

```java
// Java: restricted code escalates to read a file
AccessController.doPrivileged(() -> {
    return new FileInputStream("/etc/app/config.yaml");
});
```

This was a containment violation by design, the contained code reaches
upward for capabilities it shouldn't have.

The IoC / Dependency Injection pattern (originating with Stefano Mazzocchi's
Inversion of Control in Apache Avalon, later refined in PicoContainer,
Spring, and others) eliminates the need entirely. The **container** does
the privileged work and **injects the result** into the contained, as
a constructor argument, a closure parameter, or a service interface.

The injected dependency isn't limited to read-only data. It can be a
service with full business logic, including mutation:

```aether
// Container: has full access, creates a database service
db = connect_database("/etc/app/db-config.yaml")

// The db service has methods: query, insert, update, delete
// It encapsulates the privileged connection, the worker never
// sees the filesystem or raw TCP

worker = sandbox("worker") {
    // No grant_fs_read, no grant_tcp, worker can't touch either
    // But it CAN use the db service that was injected
}

run_sandboxed(worker) |ctx: ptr| {
    // Worker calls db.query(), db.insert(), business logic with
    // mutation, not just read-only data. The privileged connection
    // is behind the service interface. Worker never escalates.
    result = call(db_query, db, "SELECT * FROM users")
    call(db_insert, db, "INSERT INTO logs VALUES (...)")
}
```

The contained code has no filesystem access, no TCP access, but full
database read/write capability, because the container injected a
service that encapsulates the privilege. The privilege boundary is
the service interface, not a `doPrivileged` escalation.

This is the IoC principle applied to sandboxing: **inject capabilities,
don't let the contained reach for them.**

## Design influences, summary

The Aether sandbox draws from:

| Source | What we took |
|--------|-------------|
| Apache Avalon / IoC | Inversion of Control, the container wires, the contained receives |
| OpenBSD pledge | Flat list of capability grants, irreversible narrowing |
| OpenBSD unveil | Path-level filesystem grants with glob patterns |
| Deno | Per-resource grants (host, path, env), deny-by-default |
| Java SecurityManager | Stack-based permission checking, nested scopes |
| Java doPrivileged | What NOT to do, IoC eliminates the need to escalate |
| gVisor | API-level interception (stdlib, not kernel) |
| WASI | No ambient authority ideal (extern restriction proposal) |
| Docker | Container/contained metaphor, nested restriction |
| Cloudflare Workers | Lightweight isolation without OS virtualization |
| DI containers | The contained cannot reach the container; inject capabilities instead |

## Language host modules

Aether can embed foreign language runtimes and run their code inside
sandboxes. Each hosted language is a module under `contrib/host/`.

### Available modules

```
contrib/host/python/, import contrib.host.python   (CPython 3.x)
contrib/host/lua/, import contrib.host.lua      (Lua 5.3)
contrib/host/js/, import contrib.host.js       (Duktape ES5)
contrib/host/perl/, import contrib.host.perl     (Perl 5.x)
contrib/host/ruby/, import contrib.host.ruby     (CRuby 3.x)
contrib/host/tcl/, import contrib.host.tcl      (Tcl 8.5+)
```

### Two containment models

| Model | How it works | Used by |
|-------|-------------|---------|
| **LD_PRELOAD interception** | Intercept libc calls (connect, open, getenv, execve). The hosted language has ambient access to libc; we filter it. | Python, Lua, Perl, Ruby, Tcl |
| **Native bindings only** | The hosted engine has NO ambient access. We expose only the functions we choose (env, readFile, etc.), each with a sandbox check built in. | JS (Duktape) |

The native bindings model (Duktape) is the purest containment, there
is nothing to intercept because there is nothing ambient. The hosted
code can only call functions we explicitly provide.

### Host module matrix

| | Python | Lua | JS (Duktape) | Perl | Ruby | Tcl |
|---|--------|-----|-------------|------|------|-----|
| **Runtime** | CPython 3.x | Lua 5.3 | Duktape 2.x | Perl 5.x | CRuby 3.x | Tcl 8.5+ |
| **Dev package** | python3-dev | liblua5.3-dev | duktape-dev | (ships with perl) | ruby-dev | tcl-dev |
| **Compile flag** | AETHER_HAS_PYTHON | AETHER_HAS_LUA | AETHER_HAS_DUKTAPE | AETHER_HAS_PERL | AETHER_HAS_RUBY | AETHER_HAS_TCL |
| **Link** | -lpython3.11 | -llua5.3 | -lduktape | -lperl | -lruby-3.1 | -framework Tcl / -ltcl |
| **Containment model** | LD_PRELOAD | LD_PRELOAD | Native bindings | LD_PRELOAD | LD_PRELOAD | LD_PRELOAD |
| **Needs LD_PRELOAD .so** | Yes | Yes | No | Yes | Yes | Yes |
| **env var access** | libc getenv (intercepted) | libc getenv (intercepted) | `env()` binding (checked) | %ENV scrubbed at entry | ENV scrubbed at entry | ::env lazy via getenv (intercepted) |
| **File access** | libc open/fopen (intercepted) | libc fopen (intercepted) | `readFile()` binding (checked) | libc open (intercepted) | libc open (intercepted) | libc open (intercepted) |
| **Network** | libc connect (intercepted) | libc connect (intercepted) | Not exposed | libc connect (intercepted) | libc connect (intercepted) | libc connect (intercepted) |
| **Process exec** | libc execve (intercepted) | libc execve (intercepted) | Not exposed | libc execve (intercepted) | libc execve (intercepted) | libc execve (intercepted) |
| **Env cache issue** | Yes, os.environ cached at startup; use ctypes.CDLL(None).getenv | No, os.getenv goes through libc | No, no cache | Yes, %ENV cached; scrubbed by host module | Yes, ENV cached; scrubbed by host module | Yes, ::env cached; not auto-scrubbed |
| **Sandbox grants honoured** | Yes | Yes | Yes | Yes | Yes | Yes |
| **Glob patterns work** | Yes (prefix, suffix, exact, wildcard) | Yes | Yes | Yes | Yes | Yes |
| **Nested sandbox** | Yes | Yes | Yes | Yes | Yes | Yes |
| **Guest knows it's sandboxed** | No | No | No | No | No | No |

### Shared-interpreter behavior

Perl, Ruby, and Tcl keep a single long-lived interpreter across calls.
`run_sandboxed` scrubs the environment on entry and leaves the scrubbed
state in place on exit, a subsequent unsandboxed `run()` in the same
process sees the scrubbed environment. Two stable usage patterns:
(a) one mode per process, restart the host between modes; or
(b) have the host snapshot the environment from the guest language
before entering the sandbox and reassign it on exit. Python is unaffected
because its cached `os.environ` is a separate copy from libc's environ.
Lua and JS don't cache the environment at all.

Ruby behavior to be aware of: `Fiddle.dlopen("libc.so.6")` inside a
sandbox succeeds and returns a handle, but any libc function invoked
through that handle still goes through the Aether preload layer and
respects grants. The `dlopen` succeeding isn't a sandbox escape, it's
the expected result, since interception is on the call, not the load.

### Usage pattern

All host modules follow the same pattern:

```aether
import std.list
import contrib.host.python   // or lua, js, perl, ruby, tcl

// Define sandbox grants
worker = sandbox("worker") {
    grant_env("HOME")
    grant_fs_read("/etc/hostname")
}

// Run hosted code, it uses normal APIs, has no idea it's contained
python.run_sandboxed(worker, <<SCRIPT
import os
print(os.getenv("HOME"))        # allowed
print(os.getenv("AWS_SECRET"))   # returns None, sandboxed
SCRIPT
)
```

### Enforcement modes

A hosted language can be sandboxed in three ways:

| Mode | Description | Grant transport |
|------|------------|-----------------|
| **In-process embedded** | Host module links the runtime into the Aether binary. LD_PRELOAD intercepts libc. | Context stack (in-memory) |
| **Cross-process spawn** | `spawn_sandboxed(perms, "python3", "script.py")`. Aether forks, sets LD_PRELOAD, execs. | Shared memory (shm_open) |
| **Native bindings** | Engine has no ambient access. Every capability is an explicit binding. | Direct function calls |

The same grant list works across all three modes. The containment
principle is the same: the contained code uses normal APIs and cannot
tell it's sandboxed.

## Data exchange: shared map

Aether and hosted languages exchange data through a token-guarded
string:string map. All values are strings. The map has function call
semantics, inputs flow in, outputs flow out. It is not a
bidirectional messaging channel.

### Contract

```
        Aether          │         Hosted Code
                        │
  map_put("input", x)   │
  map_put("config", y)  │
        ── freeze ──────┤
                        │  map_get("input")  → x  ✓
                        │  map_get("config") → y  ✓
                        │  map_put("input", z)    ✗ (frozen)
                        │  map_put("result", r)   ✓ (new key)
        ── return ──────┤
  map_get("result") → r │
  map_revoke_token()    │
  map_free()            │
```

### How it works

1. **Aether creates the map** and puts input key-value pairs
2. **Inputs are frozen** before hosted code runs, hosted code
   can read them but cannot overwrite them
3. **Hosted code writes outputs** as new keys via `aether_map_put`
4. **Hosted code returns**, Aether reads output keys
5. **Token is revoked**, the map is inaccessible from the hosted side
6. **Aether frees the map** when done

### Token guard

The map is accessed from hosted code via a one-time token. The token
is generated randomly, validated on every get/put, and revoked after
the hosted code returns. A stale or guessed token returns nothing.

### Native bindings

Each host module provides two bindings to the hosted language:

| Language | Get | Put |
|----------|-----|-----|
| Lua | `aether_map_get(key)` | `aether_map_put(key, value)` |
| Python | `aether_map_get(key)` | `aether_map_put(key, value)` |
| JS | `aether_map_get(key)` | `aether_map_put(key, value)` |
| Perl | `aether_map_get(key)` | `aether_map_put(key, value)` |
| Ruby | `aether_map_get(key)` | `aether_map_put(key, value)` |

### Example

```aether
import contrib.host.python

worker = sandbox("worker") {
    grant_env("HOME")
    grant_fs_read("/etc/*")
}

// Create map with inputs
map = shared_map_new(&token)
shared_map_put(map, "user", "alice")
shared_map_put(map, "threshold", "42")

// Run Python, it reads inputs, writes outputs
python.run_sandboxed_with_map(worker, <<PY
    user = aether_map_get("user")
    threshold = aether_map_get("threshold")
    aether_map_put("result", "processed " + user)
    aether_map_put("count", "1000")
PY
, token)

// Read outputs
println(shared_map_get(map, "result"))  // "processed alice"
println(shared_map_get(map, "count"))   // "1000"
shared_map_free(map)
```

### Type convention

All values are strings. Numbers, booleans, and other types are
encoded as strings by the sender and parsed by the receiver:

```aether
shared_map_put(map, "threshold", "42")      // int
shared_map_put(map, "rate", "3.14")         // float
shared_map_put(map, "enabled", "true")      // bool
shared_map_put(map, "tags", "a,b,c")        // list (caller's convention)
```

```lua
local threshold = tonumber(aether_map_get("threshold"))  -- 42
local rate = tonumber(aether_map_get("rate"))             -- 3.14
local enabled = aether_map_get("enabled") == "true"       -- true
```

This is the same convention as HTTP headers, environment variables,
and command line arguments. Every cross-boundary interface in
computing passes numbers as strings. The sandbox boundary is no
different.

### Nested maps

The shared map is flat by design. Use dot-delimited keys when you
need hierarchy:

```
db.host = localhost
db.port = 5432
db.credentials.user = admin
```

The hosted code splits on dots if it wants to reconstruct a tree.
The map stays a single-level key-value store, simple, auditable,
and with no deserialization attack surface.

### Future: string:bytes mode

A flag on `shared_map_new()` will switch values from null-terminated
strings to length-prefixed byte arrays. Same API, same token guard,
same freeze/revoke lifecycle. For binary data too large to base64.

## How Aether compares to other capability / sandbox systems

Aether's capability model runs at three levels and composes with
bidirectional host-language interop. Useful to anchor against
systems readers already know.

### Three enforcement layers

1. **Module boundary**, under `--emit=lib` the compiler rejects
   imports of `std.fs`, `std.net`, `std.os` at build time; the host
   opts each one in with `--with=fs[,net,os]`. See [`emit-lib.md`](emit-lib.md).
2. **Scope boundary**, `hide <names>` and `seal except <allowlist>`
   let any lexical block (closure, trailing-block DSL, actor
   handler) decline to see selected enclosing names; reading,
   assigning, or re-declaring a hidden name is a compile error, and
   the denial travels with the block. See [`hide-and-seal.md`](hide-and-seal.md).
3. **Runtime process boundary**, `libaether_sandbox.so` (LD_PRELOAD)
   intercepts libc (`open*`, `connect` / `bind` / `accept`, `execve`
   / `fork`, `mmap` / `mprotect`, `dlopen`, `getenv`) against a
   builder-DSL grant list, inherited across `execve`. Covers
   normal-libc code. Adversaries using `openat2` / `io_uring` /
   `sendfile` / `execveat` / raw `syscall()` / `ptrace` bypass it
   (enumerated in detail above under *Interception surface*).

### Bidirectional host interop

Aether plays either side of the embedding relationship, with the
same permissions registry and LD_PRELOAD checker either way.

- **Aether as guest**, a host-language app loads an Aether `.so`
  built via `--emit=lib`, which carries a typed namespace manifest
  (`aether_describe()`) used by `ae build --namespace` to generate
  per-language SDKs. Shipped: Python (ctypes), Java (Panama, JDK
  22+), Ruby (Fiddle). Go stubbed. Host-side is normal methods,
  no JNI, SWIG, `MemorySegment`, or `ctypes.CDLL` boilerplate.
  Callback model is Hohpe's *claim check*: script emits
  `notify(event, id)`, host calls back through the typed downcall
  API for detail. See [`embedded-namespaces-and-host-bindings.md`](embedded-namespaces-and-host-bindings.md)
  (typed-SDK story) and [`aether-embedded-in-host-applications.md`](aether-embedded-in-host-applications.md)
  (rationale + YAML/HCL/Pkl/Jsonnet/Starlark comparison).
- **Aether as host**, an Aether `main()` executable embeds
  `contrib.host.<lang>.run_sandboxed(perms, code)` for Lua, Python
  (CPython), Perl, Ruby, Tcl, JavaScript in-process. Java, Go, and
  aether-hosts-aether are separate-process (Aether is compiled, so
  aether-hosts-aether uses fork+exec with LD_PRELOAD on the child).
  `hide` / `seal except` are Aether compile-time constructs, they
  do NOT travel into hosted non-Aether interpreters, which have
  their own scoping; containment for those is grants + LD_PRELOAD
  only. `hide` / `seal except` still shape the Aether-side
  grant-assembly block and the hosting closure. See
  `contrib/host/<lang>/README.md` and `contrib/host/TODO.md`.

### What it's most like

- **Pony object capabilities**, closest analogue in a systems
  language. Aether's grants are coarser (stdlib category at the
  module level, name at the scope level, libc entry point at the
  runtime level); Pony attaches capability modes to individual
  references.
- **Java's removed SecurityManager**, same structural idea
  (`java.policy` grants, `AccessController.doPrivileged`), same
  layer (interpreter / VM-level check on sensitive operations).
  Deprecated in JDK 17, removed in JDK 24 because the maintenance
  cost outgrew the benefit in an ecosystem where most applications
  trust their dependencies. Aether's equivalent survives because
  the scope is narrower (the grant list is populated by a builder
  DSL in the same codebase, not by a system-wide policy file
  parsed from XML) and the enforcement point is libc, not the VM.
- **A fraction of gVisor**, both intercept at a boundary
  (gVisor's Sentry emulates syscalls; Aether's LD_PRELOAD wraps
  libc). gVisor is kernel-level (process-scoped, every syscall)
  and handles adversarial workloads. Aether is userspace-level
  (libc-scoped, easily bypassed by raw `syscall()`) and handles
  cooperative containment. gVisor gives you a hardened sandbox
  for untrusted containers; Aether gives you a developer-ergonomic
  permission surface for trusted-but-sandboxed plugins.

### What it is NOT

- **NOT Ruby / Smalltalk / Groovy's builder-style closures**,
  those languages interpret the trailing block at runtime with
  full access to the interpreter's reflection surface. Aether
  compiles the closure to C; the sandbox grant list is the
  compiled function's only handle to privileged operations.
  `hide` / `seal except` are checked by the compiler, not by a
  runtime SecurityManager.
- **NOT a runtime wrapper** (WASI, gVisor, Firejail), `--emit=lib`
  changes what the compiler will emit at all, not what the runtime
  will permit later.
- **NOT a library flag** (Deno's `--allow-net`, Node's
  experimental permission model), those are process-wide
  allowlists checked at API call sites. Aether's gate is at
  compile time and at scope entry, plus an optional runtime check.
- **NOT an annotation convention** (Rust crates, Go build tags),
  those require ecosystem buy-in and don't prevent a dependency
  from pulling in what it needs. Aether rejects the import.

### What's novel is the combination

Most embeddable languages (Lua, Wren, Starlark, Hermes) give you
the embedding but leave capability management to the host. Most
capability-secure languages (Pony, E) don't ship polyglot SDK
generators or in-process interpreter bridges. Aether bundles both
directions (guest + host) behind one permissions model.

### Cross-cutting gaps (contributor surface)

Active work listed in [`../contrib/host/TODO.md`](../contrib/host/TODO.md)
and [`next-steps.md`](next-steps.md) → *Host Language Bridges*:

- Capturing stdout/stderr from hosted scripts, pipe rewire vs.
  shared-map key vs. pass-through, design undecided.
- Native shared-map bindings for Perl and Ruby, currently
  tied-hash via `eval`, which swallows writes. Python, Lua, Tcl,
  JS already have proper native C bindings.
- `bytes` mode on the shared map, so callers don't have to
  base64 binary payloads across the boundary.

### Worked examples and tests

- `examples/embedded-java/trading/` direction 1 (Aether as
  guest, Java host).
- `examples/sandbox-spawn.ae`, `examples/sandbox-demo.ae`,
  direction 2 (Aether as host).
- `tests/integration/namespace_{python,ruby,java}/`,
  `tests/integration/embedded_java_trading_e2e/` per-SDK
  regression tests for direction 1.
