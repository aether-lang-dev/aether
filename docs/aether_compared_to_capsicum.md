# Aether Sandbox Model vs. BSD Capsicum: Architecture, Parallels, and FreeBSD Integration

## Executive Summary

Aether and BSD Capsicum are two fundamentally different approaches to capability-based sandboxing:

- **Capsicum** is an OS-level security framework (FreeBSD kernel + POSIX extensions) that restricts processes via file descriptor capabilities and capability mode.
- **Aether** is a language-level sandbox model (implemented in the compiler and runtime) that uses closures, permission contexts, and scope hygiene to restrict what code can access.

Despite the difference in scope (OS vs. language), Aether's closure-based isolation and permission hierarchy are **conceptually aligned with Capsicum's principles**. This document explores the parallels, gaps, and opportunities for Aether on FreeBSD, including the possibility of an Aether runtime that leverages Capsicum for OS-enforced containment of sandboxed actors.

> **Status (current):**
> - Aether's existing runtime sandbox, the `libaether_sandbox.so`
>   LD_PRELOAD layer and `spawn_sandboxed` builds and runs on FreeBSD
>   at parity with Linux.
> - **Phase 1 done:** the `std.capsicum` module (Part 3.2) gives
>   hand-written Aether code direct access to `cap_enter` /
>   `cap_rights_limit` / `cap_fcntls_limit`.
> - **Phase 2 done:** an Aether process can self-sandbox at startup via
>   `AETHER_CAPSICUM=1`, and `spawn_sandboxed` sets that for its
>   children, so a spawned Aether child gets kernel-enforced Capsicum
>   containment transparently. The originally-planned "parent
>   `cap_enter`s the child" model turned out to be impossible for
>   dynamic binaries; Part 3.2 records why and what shipped instead.
> - **Phase 3 done:** the in-process permission layer has an audit
>   trail, a live `AETHER_SANDBOX_AUDIT` sink and a queryable
>   `std.audit` ring buffer.
> - **Casper delegation done:** `std.casper` binds the DNS, passwd and
>   sysctl Casper services, so a capability-mode process can still
>   resolve hostnames and read sysctls via the casper daemon.
> - Still ahead: jail/RCTL integration (a separate, sysadmin-facing
>   axis) and wiring the grant list into pre-opened
>   `cap_rights_limit`'d fds.

---

## Part 1: Sandbox Models Compared

### 1.1 Capsicum: OS-Level Capability Restrictions

#### Design Principles

Capsicum extends the POSIX API with capability-based security primitives. The key insight: **treat file descriptors as capabilities** with fine-grained rights.

**Core concepts:**

| Concept | Meaning |
|---------|---------|
| **Capability** | A file descriptor with restricted rights (e.g., read-only, seek-only, ioctl-restricted) |
| **Capability mode** | A process state where global namespace access is denied; all I/O must derive from passed-in fds |
| **Capability rights** | Bitmask of allowed operations (CAP_READ, CAP_WRITE, CAP_SEEK, CAP_IOCTL, CAP_FCNTL, etc.) |
| **Process descriptor** | A handle to another process; can be used to query its state or revoke its capabilities |
| **Casper daemon** | A privileged service that sandboxed processes delegate to (e.g., for DNS, syslog) |

#### How Capsicum Works

1. **Start privileged**, A process begins with all capabilities (or a default set).
2. **Enter capability mode**, Call `cap_enter()`. This is **irreversible**; the process can never access global namespaces again.
3. **Restrict file descriptors**, Use `cap_rights_limit(fd, rights)` to reduce a fd's rights. This restriction is **hereditary**, child processes inherit the restricted fds.
4. **Delegate via fd-passing**, Pass restricted fds to other processes via IPC. They get only the rights you granted.
5. **Fail securely**, Attempts to access denied resources fail with ENOTCAPABLE (a new errno).

#### Example: tcpdump under Capsicum

```c
// Original tcpdump runs privileged, has access to everything
int main() {
    // ... capture packets, write to file ...
}

// Capsicum version: restrict early
int main() {
    // Open packet device and output file while privileged
    int pcap_fd = open_live(...);
    int out_fd = open("capture.pcap", O_WRONLY | O_CREAT);
    
    // Restrict both fds to their needed rights
    cap_rights_init(&rights, CAP_READ);
    cap_rights_limit(pcap_fd, &rights);
    
    cap_rights_init(&rights, CAP_WRITE);
    cap_rights_limit(out_fd, &rights);
    
    // Enter capability mode, can no longer open new files or access global state
    cap_enter();
    
    // Now run the packet capture loop with only pcap_fd and out_fd
    // ... capture packets, write to out_fd ...
    
    return 0;
}
```

**Key property:** Even if tcpdump is compromised, it cannot:
- Open new files (no access to `open()`)
- Bind to ports (no `socket()`)
- Execute commands (no `execve()`)
- Access network beyond the pcap fd

#### Capsicum in Practice

**Real-world Capsicum users:**
- tcpdump, dhclient, hastd, kdump, rwhod, ctld, iscsid (FreeBSD base system)
- Chromium sandboxing (with Google collaboration)
- Firefox (experimental)
- PostgreSQL (libpq sandboxing proposal)

**Why it's effective:**
1. OS-enforced, kernel checks rights on every system call
2. Fail-safe, denied operations return ENOTCAPABLE, not a silent failure
3. No performance overhead for allowed operations
4. Composable, can chain restricted fds arbitrarily deep
5. Casper daemon pattern, delegates privilege to a trusted service

---

### 1.2 Aether: Language-Level Scope-Based Isolation

#### Design Principles

Aether's sandbox model uses **three language features** to achieve isolation:

1. **Closures**, hoisted functions that cannot reach parent locals unless explicitly passed
2. **Permission contexts**, a data structure (list/map) representing granted capabilities
3. **Scope hygiene**, `hide` and `seal except` declarations that prevent name lookup in outer scopes

**Core concepts:**

| Concept | Meaning |
|---------|---------|
| **Closure** | A function value that captures a subset of variables; hoisted away from its declaration scope |
| **Permission context** | A list/map of (category, resource_pattern) tuples representing what a scope can do |
| **Permission category** | A string like "tcp", "fs_read", "fs_write", "exec", "env" |
| **Resource pattern** | A glob or exact match for a resource (e.g., "db.internal:5432", "/tmp/*") |
| **Hide** | A declaration that the enclosing scope cannot see names from outer scopes |
| **Seal except** | A declaration that the enclosing scope cannot see names except the ones listed |
| **Containment principle** | The container can see the contained, but the contained cannot reach the container |

#### How Aether's Sandbox Works

1. **Create a permissions context**, Allocate a list to hold permission tuples.
2. **Grant permissions**, Call `grant_tcp(ctx, "host", port)`, `grant_fs_read(ctx, path)`, etc.
3. **Wrap code in a closure**, The sandboxed code is a closure that receives the context as its only input.
4. **Run contained code**, Call `call(closure, ctx)`. The closure cannot reach parent locals.
5. **Check permissions**, Sandboxed code calls `sandboxed_tcp_connect(ctx, host, port)` which checks the context before operating.
6. **Fail gracefully**, Permission check fails, returns 0 (or throws, depending on implementation).

#### Example: Aether Sandboxed Worker

```aether
import std.list

// Permission check
check_permission(perms: ptr, category: string, resource: string) {
    n = list.size(perms)
    for (i = 0; i < n; i = i + 2) {
        cat, _ = list.get(perms, i)
        pat, _ = list.get(perms, i + 1)
        if str_eq(cat, "*") == 1 && str_eq(pat, "*") == 1 { return 1 }
        if str_eq(cat, category) == 1 {
            if str_eq(pat, "*") == 1 { return 1 }
            if str_eq(pat, resource) == 1 { return 1 }
        }
    }
    return 0
}

// Sandboxed operation wrapper
sandboxed_tcp_connect(perms: ptr, host: string, port: int) {
    if check_permission(perms, "tcp", host) == 1 {
        // Real code: tcp.connect(host, port)
        return 1
    }
    return 0  // Denied
}

// Create, grant permissions, and run the contained code
main() {
    db_worker = list.new()
    list.add(db_worker, "tcp")
    list.add(db_worker, "db.internal")

    // Contained code as a closure. Closures bind inside a function body,
    // and the captured param is typed so the list passes as a ptr.
    worker_code = |perms: ptr| {
        if sandboxed_tcp_connect(perms, "db.internal", 5432) == 1 {
            println("Connected to DB")
        } else {
            println("Connection denied")
        }
    }

    // Run the closure with the restricted context
    call(worker_code, db_worker)

    // worker_code cannot access globals, files, or anything else
    // It can only do what the permissions context allows
}
```

**Key properties:**

1. **Language-enforced**, The compiler ensures closures can't reach parent locals.
2. **Type-safe**, Closure signatures are checked at compile time.
3. **Transparent**, Permission checks are just function calls; can be optimized away for fully-trusted code.
4. **Composable**, Actors can nest, each with its own permission context.
5. **Portable**, Works on any OS; no OS-specific APIs needed.

---

### 1.3 Side-by-Side Comparison

| Aspect | Capsicum | Aether |
|--------|----------|--------|
| **Enforcement layer** | OS kernel (syscall-level) | Language runtime (function-call level) |
| **Isolation mechanism** | File descriptor restrictions + capability mode | Closure scope + permission lists |
| **Granularity** | Per-fd rights (CAP_READ, CAP_WRITE, etc.) | Per-category patterns (tcp, fs_read, etc.) |
| **Revocation** | Implicit (close the fd) | Implicit (actor dies, context goes away) |
| **Nested restrictions** | Process forking; each child inherits restricted fds | Lexical nesting; each closure inherits parent's context |
| **Overhead** | Zero for allowed ops; syscall overhead for denied ops | Function-call overhead for permission checks |
| **Failure mode** | ENOTCAPABLE errno | Return code / exception (language-defined) |
| **OS coupling** | Tightly coupled to FreeBSD/POSIX | OS-agnostic |
| **Privilege separation** | Casper daemon (separate process) | Actor delegation (same actor model) |
| **Audit trail** | Not built-in; requires additional instrumentation | Not built-in; requires explicit logging |

---

## Part 1.4 Closing the Gap: What LD_PRELOAD Needs to Match Capsicum on Linux

#### Current LD_PRELOAD Capabilities

Aether's LD_PRELOAD layer (`libaether_sandbox_preload.so`) currently intercepts:

| Category | Syscalls Intercepted | Status |
|----------|----------------------|--------|
| **File I/O (read)** | `open()`, `openat()`, `open64()`, `openat64()`, `fopen()`, `fopen64()` | ✓ Full |
| **File I/O (write)** | Same as above with write-flag detection | ✓ Full |
| **TCP outbound** | `connect()` (IPv4, IPv6 with normalization) | ✓ Full |
| **TCP inbound** | `bind()`, `listen()`, `accept()`, `accept4()` | ✓ Full |
| **Process control** | `fork()`, `vfork()`, `clone3()` | ✓ Full |
| **Program execution** | `execve()` | ✓ Full |
| **Environment** | `getenv()` | ✓ Full |

#### Current Limitations vs. Capsicum

| Gap | Capsicum | LD_PRELOAD | Impact |
|-----|----------|-----------|--------|
| **stat/access bypass** | Blocked by kernel | Not intercepted (glibc inlines as asm) | Can stat() files without permission check |
| **Metadata access** | CAP_FSTAT denied | Uncontrolled | Can learn file size, permissions, inode |
| **Memory protection** | CAP_MMAP denied | Not intercepted | Can mmap() arbitrary files |
| **Irreversible isolation** | `cap_enter()` is irreversible | Sandboxing can be disabled (LD_PRELOAD unloaded at runtime) | No guarantee against circumvention |
| **File descriptor leaks** | Only passed fds exist | All inherited fds remain open | Inherited sockets, files still accessible |
| **Signal handling** | CAP_SIGNAL restricted | Not intercepted | Can send signals outside sandbox |
| **Device access** | All /dev/* access denied | Only open() is checked | Can access /dev/zero, /dev/random |
| **Socket options** | CAP_SETSOCKOPT denied | Not intercepted | Can set socket options (SO_REUSEADDR, etc.) |
| **Resource limits** | CAP_FCNTL denied | Not intercepted | Unlimited file descriptors, etc. |
| **ptrace() access** | Not permitted | Not intercepted | Can ptrace() sibling processes |
| **Process accounting** | Hidden from sandboxed process | Still visible in /proc | Can enumerate other processes |

#### What LD_PRELOAD Would Need

To match Capsicum's **bulletproof guarantee**, LD_PRELOAD would need:

##### 1. **Kernel Collaboration (Most Critical)**

Capsicum's strength comes from the **kernel enforcing restrictions**. LD_PRELOAD cannot match this without kernel support:

```c
// Capsicum: irreversible, kernel-enforced
cap_enter();  // No code can ever undo this

// LD_PRELOAD: can be unloaded at runtime
dlclose(sandbox_handle);  // Sandbox disappears
```

**Needed for Linux:** A capability mode like Capsicum (patch to the kernel itself):
- `prctl(PR_ENTER_CAPMODE)` irreversible mode that denies all global namespace access
- Kernel-enforced checks on every syscall
- ENOTCAPABLE errno for denied operations

##### 2. **Mandatory Interception of All Syscalls**

Some syscalls bypass LD_PRELOAD because glibc inlines them as direct asm:

```c
// These bypass LD_PRELOAD entirely
stat(path, &sb);       // glibc inlines this
access(path, X_OK);    // kernel call, no libc wrapper
```

**Needed for Linux:** Either:
- **seccomp + BPF (Secure Computing)**, filter all syscalls at kernel level
- **ptrace (Tracer process)**, intercept ALL syscalls, including inlined ones
- **eBPF (extended Berkeley Packet Filter)**, hook syscalls before kernel processing

##### 3. **File Descriptor Tracking & Enforcement**

Capsicum tracks which fds a process can access. LD_PRELOAD cannot:

```c
// Capsicum: only fds passed are accessible
// Process cannot open new ones even if code tries

// LD_PRELOAD: inherited fds are accessible, untracked
// All open fds from parent are available
```

**Needed for Linux:**
- Kernel tracking of fd capabilities (fd → rights mapping)
- Deny operations on fds without proper capabilities
- Revoke fds when capabilities change

##### 4. **Metadata Access Control**

Capsicum prevents even **reading** metadata (size, permissions, etc.). LD_PRELOAD allows this:

```c
// Capsicum: stat() on restricted path fails with ENOTCAPABLE
stat("/etc/passwd", &sb);  // DENIED

// LD_PRELOAD: stat() succeeds (no hook)
stat("/etc/passwd", &sb);  // ALLOWED (information leak)
```

**Needed for Linux:**
- Hook all stat/lstat/fstat/statx variants
- Deny metadata access for restricted paths
- Prevent information leaks

##### 5. **Irreversible Sandboxing**

Capsicum's `cap_enter()` is **permanent**. LD_PRELOAD can be circumvented:

```c
// Capsicum: impossible to escape
cap_enter();
// No dlopen, no dlsym, no libc tricks, blocked forever

// LD_PRELOAD: can be unloaded
if (dlopen("/lib64/ld-linux.so", RTLD_NOW)) {
    // Manually reload libc without interception
    // Sandbox bypassed
}
```

**Needed for Linux:**
- Kernel prevents loading additional libraries after sandboxing
- Deny dlopen/dlsym syscalls in capability mode
- Lock down LD_PRELOAD at kernel level

##### 6. **Signal & IPC Restrictions**

Capsicum denies signal operations to processes outside the sandbox. LD_PRELOAD doesn't:

```c
// Capsicum: kill() of arbitrary processes fails
kill(victim_pid, SIGTERM);  // ENOTCAPABLE if victim outside sandbox

// LD_PRELOAD: no hook for kill()
kill(victim_pid, SIGTERM);  // ALLOWED
```

**Needed for Linux:**
- Hook kill/sigqueue/pidfd_send_signal
- Check if target is outside sandbox
- Deny inter-sandbox signaling

#### The Fundamental Limitation

**LD_PRELOAD is a userspace enforcement layer.** It cannot provide the same guarantees as Capsicum because:

1. **Userspace can be patched**, Compiled-in LD_PRELOAD hooks can be circumvented with asm tricks, dlopen of alternative libc, or ptrace self-modification.
2. **Inlined syscalls bypass interception**, glibc optimizes common syscalls to direct asm, avoiding the libc wrapper that LD_PRELOAD hooks.
3. **Inherited resources leak**, All file descriptors, signals, memory pages are inherited and accessible; LD_PRELOAD can't revoke them.
4. **Kernel doesn't enforce**, The kernel doesn't know about sandbox policies; it just executes syscalls.

#### Closest Approximation on Linux: seccomp + BPF

To get **close to** Capsicum's guarantees on Linux, use **seccomp with Berkley Packet Filter (BPF)**:

```c
// Instead of LD_PRELOAD, use seccomp to block syscalls
prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ALLOW);
seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EACCES), SCMP_SYS(open), ...);
seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(clone), ...);
seccomp_load(ctx);
```

**Benefits over LD_PRELOAD:**
- ✓ Kernel-enforced (cannot be bypassed from userspace)
- ✓ Intercepts all syscalls (including inlined ones)
- ✓ BPF programs run in the kernel (efficient)
- ✓ Supports parameter inspection (fd numbers, path arguments)

**Still not equivalent to Capsicum:**
- ✗ Per-fd capability rights not available (seccomp operates at syscall level, not fd level)
- ✗ No irreversible "capability mode" (can disable seccomp with CAP_SYS_ADMIN)
- ✗ File descriptor tracking is manual (need to track fds yourself)
- ✗ No Casper-style privilege delegation

#### Recommendation

**On Linux without Capsicum:**

1. **For maximum security:** Use `seccomp + BPF` (kernel-enforced) + language-level checks
2. **For ease of use:** Use LD_PRELOAD + language-level checks (accept inherent limitations)
3. **For production:** Use both LD_PRELOAD + seccomp + language checks (defense in depth)

**On FreeBSD today:** the LD_PRELOAD layer (`libaether_sandbox.so` +
`spawn_sandboxed`) builds and runs, FreeBSD's `rtld-elf` honours
`LD_PRELOAD` and `dlsym(RTLD_NEXT, ...)` exactly as glibc's loader does,
so FreeBSD has the same cooperative-containment story as Linux. It has
the same userspace limitations (raw `syscall()`, statically linked
binaries, etc.).

**On FreeBSD, the goal:** add Capsicum as a layer-4 backend, which is
**ironclad** and requires no workarounds. This is not yet implemented,
the roadmap below (`std.capsicum` bindings, then Capsicum-backed
`spawn_sandboxed`) is the path to it.

---

## Part 2: Architectural Parallels

### 2.1 Common Principles

Both systems are built on **three shared insights:**

#### 1. Capabilities over Permissions

Both Capsicum and Aether use **capability-based access control** instead of traditional role-based access control (RBAC):

- **RBAC** (Unix permissions): Check the caller's identity/role. "User alice is in group developers, so she can read /home/projects/*"
- **Capability** (Capsicum/Aether): Check what the caller has been given. "This process has a read-only fd to /home/projects/foo; it can read that one file, nothing else."

The capability model is **more restrictive and composable**. You can hand off a capability without expanding the recipient's overall privileges.

#### 2. Containment Boundaries

Both systems enforce a **container/contained boundary**: The container can see the contained, but the contained cannot reach the container.

- **Capsicum:** A privileged parent process starts, opens fds, restricts them, and enters capability mode. The restricted process can't open new fds or break back out.
- **Aether:** A parent function creates a permission context, passes it to a closure. The closure can't reach parent locals or dynamically add permissions.

This principle appears in:
- Virtual machines (hypervisor sees guest, guest can't see hypervisor)
- Docker (host sees container, container can't see host)
- Java ClassLoader hierarchies (parent loader visible to children, not vice versa)

#### 3. Fail-Safe Defaults

Both default to **deny** rather than **allow**:

- **Capsicum:** Capability mode denies all global namespace access. You must explicitly pass fds.
- **Aether:** When embedded in a host application (hosted modules), capabilities are **empty by default**. You must explicitly grant them.

This inverts the traditional Unix model (allow by default, explicitly restrict), making security the default rather than an afterthought.

---

### 2.2 Where They Differ

#### 1. Scope of Enforcement

- **Capsicum:** Enforces at the **OS boundary** (syscall interface). A compromised process can't call `open()`, `socket()`, or `execve()` regardless of what code it's running.
- **Aether:** Enforces at the **language boundary** (compiler + runtime). A compromised actor can still call C externs or skip permission checks if the code is malicious.

**Implication:** Aether is better for *controlled* environments (DSL scripts, plugin systems, hosted configs); Capsicum is better for *untrusted* code (third-party binaries, legacy applications).

#### 2. Granularity

- **Capsicum:** File descriptors are the unit of isolation. Restricted rights apply to one fd.
- **Aether:** Categories and patterns are the unit. "tcp", "fs_read:*/tmp/*", "exec:bash" are examples.

**Implication:** Capsicum is finer-grained for file I/O; Aether is simpler for abstract resources.

#### 3. Performance

- **Capsicum:** Zero overhead for allowed operations. Denied operations are caught by the kernel.
- **Aether:** Permission checks are function calls. Not free, but not expensive (cache-friendly).

**Implication:** Capsicum is suitable for performance-critical code (tcpdump, database servers); Aether is fine for configuration/wiring code.

---

## Part 3: Opportunities for Aether on FreeBSD

### 3.1 A Hybrid Approach: Aether + Capsicum

**Vision:** An Aether runtime that can optionally leverage Capsicum for OS-enforced containment of sandboxed actors.

#### Design

```
+------------------+
|   Aether Code    |
|  (untrusted      |
|   plugin)        |
+------------------+
        |
        v
+------------------+
| Permission Check |  (language-level, fast path)
|  (Aether runtime)|
+------------------+
        |
        v (if Aether running on FreeBSD in Capsicum mode)
+------------------+
| Capsicum Rights  |  (OS-level, ultimate enforcement)
|   Enforcement    |
+------------------+
        |
        v
  [ Kernel ]
```

**How it works:**

1. **Aether compiler** generates actors with permission contexts (as it does today).
2. **Aether runtime** (new feature) detects if it's running on FreeBSD.
3. **If Capsicum is available:**
   - When an actor is spawned with a permission context, the runtime:
     - Opens the minimal set of file descriptors needed for that context
     - Uses `cap_rights_limit()` to restrict each fd to only the rights needed
     - Calls `cap_enter()` to enter capability mode
     - Spawns the actor in a child process with only those fds
   - The actor's language-level permission checks become an optional defense-in-depth layer.
4. **If Capsicum is not available:**
   - Fall back to language-level checks (as today).

#### Benefits

1. **Defense in depth**, Even if an actor is malicious and bypasses language-level checks, Capsicum blocks the syscall.
2. **Incremental**, Doesn't require rewriting Aether code; works with existing programs.
3. **Transparent**, Aether code doesn't need to know about Capsicum.
4. **Portable**, On non-FreeBSD systems, the language-level checks still work.

#### Example Flow

```aether
// Untrusted plugin (could be malicious)
plugin = |ctx| {
    // This is sandboxed: permission checks will deny it
    sandboxed_tcp_connect(ctx, "evil.com", 443)  // Denied by language check
    
    // But even if malicious, on FreeBSD with Capsicum:
    // direct_syscall_socket()  // Would be caught by Capsicum; ENOTCAPABLE
}

main() {
    trusted_ctx = list.new()
    list.add(trusted_ctx, "tcp")
    list.add(trusted_ctx, "db.internal")
    
    // On FreeBSD with Capsicum:
    // - Aether runtime opens TCP socket to db.internal
    // - Restricts fd with CAP_READ | CAP_WRITE
    // - Enters capability mode in a child process
    // - Spawns the actor with only that fd
    
    spawn_with_context(plugin, trusted_ctx)
}
```

---

### 3.2 Concrete Implementation Roadmap

#### Phase 1: Capsicum Detection & API Bindings, **DONE**

The `std.capsicum` module ships these bindings (FreeBSD-enforced,
no-op stubs elsewhere):

```aether
import std.capsicum

capsicum.available()              // 1 if the kernel enforces Capsicum
capsicum.enter()                  // enter capability mode, irreversible
capsicum.in_mode()                // 1 if already sandboxed
capsicum.rights_limit(fd, mask)   // narrow an fd to R_* rights
capsicum.fcntls_limit(fd, mask)   // narrow allowed fcntl commands
```

Rights are an integer bitmask (`capsicum.R_READ | capsicum.R_SEEK | ...`)
the kernel's `cap_rights_t` is an opaque struct, so the C wrapper
translates the mask into `cap_rights_set()` calls internally. Worked
example: `examples/capsicum-demo.ae` (read a file, `enter()`, watch the
next `open` get refused by the kernel).

Not yet bound: process descriptors (`pdfork`/`pdwait`) and Casper
service delegation, additive, can land without disturbing the above.

**Benefit:** hand-written Aether code can leverage Capsicum today.

#### Phase 2: Runtime Integration, **DONE (self-sandbox model)**

> **A correction to the original plan.** Earlier drafts of this section
> proposed: parent opens the granted fds, `fork()`s, the child calls
> `cap_rights_limit()` + `cap_enter()`, then execs the sandboxed
> program. **That cannot work for the general case**, and the reason is
> worth recording so nobody re-attempts it:
>
> `cap_enter()` is irreversible and forbids reaching the global
> filesystem namespace. A `cap_enter()`'d process can no longer `open()`
> a path. Path-based `execve(2)` is therefore denied (`ECAPMODE`). The
> escape hatch is `fexecve(2)` exec from a pre-opened fd, but that
> only solves the *binary's own* path lookup. A **dynamically linked**
> binary still needs its runtime linker (`ld-elf.so.1`) and shared
> libraries (`libc`, ...) loaded at exec time, and those are `open()`
> calls on paths. In capability mode they fail. So
> `fexecve()`-after-`cap_enter()` works only for **statically linked**
> binaries, which `python3`, `bash`, and a default `ae`-built program
> are not.
>
> The model that *does* work is **self-sandboxing**: the child execs
> normally (rtld loads every library while still unconfined), and then
> the program itself calls `cap_enter()` at the top of its own
> `main()`, before any user code. This is what shipped.

What's implemented:

- **`capsicum_autosandbox.c`**, a runtime startup hook. The
  compiler-emitted `main()` calls `aether_capsicum_autosandbox()`
  immediately after `aether_args_init()` (so: after rtld has finished,
  before user code). If `AETHER_CAPSICUM=1` is set in the environment,
  the hook calls `cap_enter()`. If the variable is set but the kernel
  can't honour it, the process exits rather than run unconfined, a
  requested sandbox never silently downgrades.
- **`spawn_sandboxed` on FreeBSD** sets `AETHER_CAPSICUM=1` in every
  child's environment. Containment is then two-layered:
  1. **LD_PRELOAD**, intercepts libc for *any* child, Aether or not
     (python3, bash). Userspace, cooperative, bypassable.
  2. **Capsicum self-sandbox**, if the child is an Aether binary it
     reads `AETHER_CAPSICUM` and `cap_enter()`s itself. Kernel-enforced,
     unbypassable. A non-Aether child just ignores the variable and is
     left with layer 1.
- The preload library exempts the `AETHER_*` env namespace from its
  `getenv` interception, so a sandboxed Aether child can still read its
  own `AETHER_CAPSICUM` control variable.

**Benefit:** a spawned Aether child gets OS-enforced containment with no
change to its source. Note Capsicum is strictly the harder ceiling, in
capability mode *no* path-based open succeeds, grant list or not; the
LD_PRELOAD grants only matter for the operations Capsicum would
otherwise still allow (acting on already-open fds) and for non-Aether
children.

**Limitation:** a self-sandboxed program must open everything it needs
*before* `main()` runs, or accept that path access is gone. Wiring the
grant list into pre-opened, `cap_rights_limit()`'d fds handed to the
child is the remaining refinement. Its prerequisite landed with issue
The stdlib handle types now expose their descriptors,
`file.fd(handle)` / `fs.fd(handle)` for files, `tcp.fd(sock)` /
`tcp.server_fd(server)` for sockets — so a program can open through
the stdlib and narrow each descriptor with `capsicum.rights_limit()`
before `capsicum.enter()` (exercised by
`tests/freebsd/rights_limit_stdlib_fd.ae`).

#### Phase 3: Audit Logging, **DONE**

The in-process permission layer now records every grant check, allowed
and denied, with two faces (`runtime/sandbox/aether_audit.c`,
`std/audit/module.ae`):

- **Live sink**, `AETHER_SANDBOX_AUDIT=none|stderr|file` (default
  `none`), mirroring the existing `AETHER_SANDBOX_LOG` convention.
  Emits `AETHER_ALLOWED:` / `AETHER_DENIED:` lines.
- **Queryable log**, a 256-entry in-memory ring buffer read back via
  `std.audit`: `audit.count()`, `audit.entry(i)` → `(category,
  resource, allowed)`, `audit.denied_count()`, `audit.clear()`.

```aether
import std.audit
n = audit.count()
for (i = 0; i < n; i = i + 1) {
    cat, res, allowed = audit.entry(i)
    // ...
}
```

The hook lives in the compiler-emitted in-process checker, so it fires
for any `sandbox { }` block and for embedded/hosted plugins. Worked
example: `examples/audit-demo.ae`. Per-actor attribution (the
`actor_id` argument the original sketch imagined) is not wired, the
checker sees category + resource, not the calling actor; adding it
would need the actor identity threaded into the check, an additive
follow-up. **Benefit:** forensics and debugging without external
instrumentation.

#### Phase 4: Cross-Language Capsicum Callbacks (High effort, speculative)

Allow hosted modules (Java, Python, Go) to declare Capsicum requirements:

```aether
// Aether code compiled to library, embedded in Java
config = |ctx| {
    grant_tcp(ctx, "api.example.com", 443)
    grant_fs_read(ctx, "/etc/app/config")
    
    // Metadata for host to understand Capsicum needs
    // (requires new compiler emit mode)
}
```

The Java host could:
1. Parse the Aether library's metadata.
2. Understand that this Aether config needs tcp + fs_read.
3. Fork a child process, open those fds, apply Capsicum restrictions.
4. Load the Aether library in the restricted child.

**Benefit:** Aether configs run under OS-enforced sandboxing when embedded.

---

### 3.3 FreeBSD-Specific Optimizations

#### 1. Casper Daemon Integration, **DONE (DNS, pwd, sysctl)**

FreeBSD includes Casper, a daemon that performs operations a
capability-mode process can no longer do for itself, DNS resolution,
passwd lookups, sysctl, over a channel opened *before* `cap_enter()`.

The `std.casper` module binds it (`std/casper/`):

```aether
import std.casper
import std.capsicum

// Phase 1, open channels while still unconfined
c   = casper.init()
net = casper.service(c, "system.net")
casper.close(c)

// Phase 2, lock down
capsicum.enter()

// Phase 3, the channel still works, sandboxed
ip, err = casper.dns_resolve(net, "example.com")
```

Three services are bound: `system.net` (DNS, `casper.dns_resolve`),
`system.pwd` (`casper.pwd_uid` / `casper.pwd_home`) and `system.sysctl`
(`casper.sysctl`). The C wrapper flattens the service result structs
(`addrinfo`, `passwd`) to Aether strings/ints. Off FreeBSD every call
fails cleanly and `casper.available()` returns 0. Worked example:
`examples/casper-demo.ae` resolves a hostname *inside* capability
mode, which would otherwise be impossible. `syslog` and `grp` services
are unbound but additive. Note this build vendors the Casper prototypes
inline (the dev headers are absent on some installs, e.g. GhostBSD; the
base runtime `.so` files satisfy the link).

#### 2. Jail Integration

FreeBSD jails are lightweight OS-level containers. An Aether actor could be spawned in a jail with:
- Restricted filesystem (chroot)
- Network isolation (no access to host network stack)
- Process isolation (can't see other jails)

**Opportunity:** Aether runtime could optionally spawn actors in jails for maximum isolation.

#### 3. RCTL (Resource Control)

FreeBSD's RCTL allows resource limits (CPU, memory, file descriptors) per process.

**Opportunity:** Aether's permission context could map to RCTL rules:

```aether
grant_memory(ctx, 100)     // 100 MB limit
grant_cpu(ctx, 500)        // 500 ms per second
grant_fds(ctx, 10)         // Max 10 open fds
```

The runtime would apply RCTL rules when spawning the actor.

---

## Part 4: Real-World Use Cases for Aether on FreeBSD

### 4.1 Secure Configuration Management

**Scenario:** A system administration tool (like Ansible or Puppet) uses Aether for configuration logic.

```aether
// Untrusted configuration script
config = |ctx| {
    // Script can only access /etc/myapp/ and connect to config.internal
    grant_fs_read(ctx, "/etc/myapp/*")
    grant_tcp(ctx, "config.internal", 8080)
    
    // Run the configuration logic
    fetch_config("config.internal:8080")
    parse_and_apply("/etc/myapp/app.conf")
}

main() {
    spawn_sandboxed(config)
}
```

**FreeBSD benefit:** Aether runtime spawns the config actor in:
- Capability mode (no access to /etc/passwd, /etc/shadow, network sockets)
- A chroot jail (filesystem isolation)
- RCTL limits (max memory, CPU)

Even if the config script is malicious or compromised, it can't escape its sandbox.

### 4.2 Plugin System for Services

**Scenario:** A database server or web server allows plugins written in Aether.

```aether
// User-supplied plugin
plugin = |ctx| {
    // Plugin can query the database and read data files
    grant_tcp(ctx, "db.internal", 5432)
    grant_fs_read(ctx, "/var/lib/myapp/data/*")
    
    // Cannot write, execute, or access other resources
    // ...plugin code...
}

main() {
    spawn_sandboxed(plugin)
}
```

**FreeBSD benefit:** Plugins run under Capsicum restrictions. A malicious plugin can't:
- Write to the filesystem (open file is read-only)
- Execute commands (no /bin/sh fd)
- Access the database beyond the allowed connection (socket fd is restricted)
- Fork (in capability mode)

### 4.3 Secure Scripting in System Tools

**Scenario:** A DevOps tool (like Terraform or Nomad) uses Aether for provisioning scripts.

```aether
// Provisioning script
script = |ctx| {
    grant_tcp(ctx, "*.cloud.provider", 443)        // Only to cloud provider
    grant_fs_read(ctx, "/var/lib/provisioning/*")
    grant_fs_write(ctx, "/tmp/provision-*")         // Scratch space
    grant_exec(ctx, "/usr/sbin/pw")                 // User management only
    
    // Create users, deploy services, etc.
    // Cannot access network outside cloud provider
    // Cannot read system files
    // Cannot execute arbitrary commands
}

main() {
    spawn_sandboxed(script)
}
```

**FreeBSD benefit:** Multiple scripts can run concurrently, each in its own jail with its own RCTL limits, completely isolated.

---

## Part 5: Challenges and Limitations

### 5.1 Capsicum Limitations

1. **Process-based only**, Capsicum sandboxing is per-process. Aether actors are lightweight; spawning a process per actor is heavy.
   - **Solution:** Use thread-based actors with Capsicum process boundaries only at higher levels (e.g., one process per actor pool).

2. **Syscall overhead**, Every syscall in capability mode is checked by the kernel. High-syscall-rate code might see overhead.
   - **Solution:** Batch operations, use sendfile/splice for zero-copy I/O, avoid per-request syscalls.

3. **Not portable**, Capsicum is FreeBSD-specific. Code needs to fall back gracefully on Linux/macOS/Windows.
   - **Solution:** Aether's language-level checks always work; Capsicum is opt-in and transparent.

4. **Complexity**, Understanding fd rights, capability mode, and Casper requires FreeBSD knowledge.
   - **Solution:** Wrap Capsicum in Aether std library; users write portable Aether code, runtime handles OS differences.

### 5.2 Aether Limitations

1. **Untrusted code can still bypass checks**, If an actor is malicious and compiled with the Aether code, it can skip permission checks.
   - **Mitigation:** On FreeBSD, use Capsicum for ultimate enforcement. Don't rely on language-level checks alone for untrusted code.

2. **Audit trail**, the in-process permission layer records every
   check (allowed and denied) to an optional `AETHER_SANDBOX_AUDIT`
   sink and a queryable `std.audit` ring buffer (Phase 3, done).
   Per-actor attribution is not yet wired.

3. **No resource limits**, Aether can restrict *what*, not *how much* (bandwidth, memory, CPU).
   - **Mitigation:** On FreeBSD, use RCTL for resource limits.

---

## Part 6: Roadmap for Aether on FreeBSD

### Short Term (2-3 months)

1. **Add `std.capsicum` module** with basic bindings.
2. **Documentation** explaining how to use Capsicum from Aether.
3. **Examples** showing Capsicum-enabled plugins and sandboxed actors.

### Medium Term (3-6 months)

1. **Runtime integration**, `spawn_with_context()` optionally uses Capsicum.
2. **Automatic fd management**, Runtime maps permission contexts to fds and rights.
3. **CI on FreeBSD**, Test suite runs on FreeBSD; catch OS-specific bugs.

### Long Term (6+ months)

1. ~~**Audit logging**, Built-in forensics.~~ Done, `std.audit`.
2. ~~**Casper integration**, delegation to the Casper daemon.~~ Done,
   `std.casper` (DNS, pwd, sysctl).
3. **Jail + RCTL support**, Multi-level sandboxing. Still ahead.
4. **Cross-language Capsicum metadata**, Hosted modules declare Capsicum needs. Still ahead.

---

## Part 7: Conclusion

**Capsicum and Aether are orthogonal strengths:**

- **Capsicum** is OS-enforced, fine-grained, and ironclad, but process-heavy and OS-specific.
- **Aether** is language-level, portable, and lightweight, but relies on code-level enforcement.

**A hybrid approach** (Aether code + Capsicum enforcement on FreeBSD) combines the best of both:

1. **Portability**, Aether code runs on any OS.
2. **Simplicity**, Users write high-level permission contexts; the runtime handles the details.
3. **Security depth**, Language-level checks catch bugs; Capsicum catches malice.
4. **Performance**, Fast path on allowed operations; Capsicum only on denied syscalls.

**For Aether to become a credible sandboxing platform on FreeBSD**, the roadmap is:

1. ~~**Phase 1:** Capsicum bindings~~, done, `std.capsicum`.
2. ~~**Phase 2:** Runtime integration~~, done, Capsicum self-sandbox
   (`AETHER_CAPSICUM=1`); the transparent-for-actors form turned out
   impossible for dynamic binaries (see Part 3.2).
3. ~~**Phase 3:** Audit logging~~, done, `std.audit`.
4. **Phase 4:** Ecosystem integration, Casper delegation done
   (`std.casper`); jail/RCTL and cross-language Capsicum metadata
   still ahead.

The result is a language-level sandbox model that can fall back to OS-level enforcement (Capsicum) where the platform provides it.

---

## References

### Capsicum Documentation

- [Capsicum: practical capabilities for UNIX](https://www.cl.cam.ac.uk/research/security/capsicum/), Original research
- [FreeBSD Capsicum man pages](https://www.freebsd.org/cgi/man.cgi?capsicum), `cap_enter(2)`, `cap_rights_limit(2)`, etc.
- [Casper daemon documentation](https://www.freebsd.org/cgi/man.cgi?casper), Inter-process privilege delegation
- [Chromium Capsicum integration](https://www.chromium.org/), Production use case

### Aether Documentation

- [`docs/containment-sandbox.md`](./containment-sandbox.md), Aether's permission model and examples
- [`docs/aether-embedded-in-host-applications.md`](./aether-embedded-in-host-applications.md), Hosted modules and capability-empty defaults
- [`docs/closures-and-builder-dsl.md`](./closures-and-builder-dsl.md), Closure isolation and scope hygiene
- [`docs/emit-lib.md`](./emit-lib.md), Embedding Aether as a library in host applications

### Related Security Research

- [USENIX Security 2010: Capsicum paper](http://www.trustedbsd.org/2010usenix-security-capsicum-website.pdf)
- [BSDCan 2014: Capsicum and Casper](https://www.bsdcan.org/2014/schedule/track/Security/486.en.html)
- [Oblivious Sandboxing with Capsicum](https://www.engr.mun.ca/~anderson/publications/2017/towards-oblivious-sandboxing.pdf)
