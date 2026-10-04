# std.sandbox

Grant lists, and blocks that run under them.

`sandbox.new(name) { … }` builds a grant list: each `grant_*` call in the
trailing block allows one kind of access. `sandbox.enforce(perms) { … }` runs a
block with those grants in force. Inside it, std's file, environment, process
and network calls are checked against the grants, and anything not granted is
refused the way a missing file or an unset variable would be. The contained
code does not have to know it is sandboxed.

```aether,run
import std.sandbox
import std.os

main() {
    path_only = sandbox.new("path-only") {
        grant_env("PATH")
    }
    nothing = sandbox.new("nothing") {}

    sandbox.enforce(path_only) callback {
        println("granted:   PATH visible = ${os.getenv("PATH") != null}")
    }
    sandbox.enforce(nothing) callback {
        println("no grants: PATH visible = ${os.getenv("PATH") != null}")
    }
    println("after:     PATH visible = ${os.getenv("PATH") != null}")

    sandbox.free(path_only)
    sandbox.free(nothing)
}
```
```output
granted:   PATH visible = true
no grants: PATH visible = false
after:     PATH visible = true
```

## Grants

| Grant | Allows |
|---|---|
| `grant_fs_read(path)` | opening files for reading |
| `grant_fs_write(path)` | opening, creating and writing files |
| `grant_fs(path)` | both of those, plus directory operations such as `os.chdir` |
| `grant_exec(cmd)` | running commands whose command line matches |
| `grant_env(name)` | reading environment variables |
| `grant_tcp(host)` | outbound TCP to matching hosts (ports are not checked) |
| `grant_tcp_listen()` | listening for and accepting TCP connections |
| `grant_udp(host)` | UDP with matching hosts |
| `grant_native(path)` | loading native libraries (`dlopen`) |
| `grant_all()` | everything |

A pattern is `"*"` (anything), a prefix ending in `*` (`"/etc/*"`, `"echo *"`),
a suffix starting with `*` (`"*.example.com"`), or an exact string. A grant
list with no grants denies everything.

Nested `enforce` blocks intersect: an operation is allowed only when every
enclosing grant list allows it, so an inner block can narrow its parent's
access but never widen it.

## Trusted names

Inside `enforce` every check applies to whatever code makes it, including a
function defined before the block. To let particular code run with the
authority of the code that wrote the `enforce`, name it after the grant list:

```aether,run
import std.sandbox
import std.os

// Defined outside the block, and given the caller's authority by name.
path_is_set() -> int {
    if os.getenv("PATH") == null { return 0 }
    return 1
}

main() {
    nothing = sandbox.new("nothing") {}
    sandbox.enforce(nothing, path_is_set) callback {
        println("in the block:  PATH visible = ${os.getenv("PATH") != null}")
        println("trusted call:  PATH visible = ${path_is_set() == 1}")
    }
    sandbox.free(nothing)
}
```
```output
in the block:  PATH visible = false
trusted call:  PATH visible = true
```

A name is a function or an imported module (`sandbox.enforce(perms, db)`
covers every `db.x(...)` call). The exemption is deliberately narrow:

- it covers calls *written in the block* (nested blocks included); a function
  called from elsewhere stays sandboxed, even one that calls the trusted name;
- the trusted call's arguments are still evaluated inside the sandbox, so
  `audit(fs.read(secret))` still has `fs.read` refused;
- a trusted name cannot be used as a value in the block;
- only Aether functions can be trusted, not C externs;
- authority is what the `enforce` was entered with. Contained code that writes
  its own `enforce` and trusts a name gains nothing, because it is entered
  already sandboxed. This is not Java's `doPrivileged`: the exemption is
  granted by the container, at the `enforce`, never claimed by contained code.

A panic that unwinds through a trusted call, or out of the block, leaves the
sandbox where the catching `try` found it.

## Beyond this process

The same grant list goes to:

- `spawn_sandboxed(perms, prog, args...)`, which runs a child process with
  `libaether_sandbox.so` preloaded, so the child's libc calls are checked too;
- the `contrib/host/<lang>` modules, which run embedded Lua, Python, Ruby and
  others under it: `python.run_sandboxed(worker, code)`.

## Limits

The checks sit in std and, for spawned children, in libc via LD_PRELOAD. Code
that bypasses both (raw `syscall()`, statically linked binaries, `io_uring`)
is not contained by them. On FreeBSD, `std.capsicum` adds kernel-enforced
containment. `enforce` limits what the block can *do*, not which names it can
*see*; add `seal except` to the block for that (`docs/hide-and-seal.md`).

The full design, with the patterns, the interception surface and comparisons
with other sandboxes, is in `docs/containment-sandbox.md`.

## Exports

`new`, `enforce` (with optional trusted names), `free`, and the grants
`grant_all`, `grant_fs_read`, `grant_fs_write`, `grant_fs`, `grant_exec`,
`grant_env`, `grant_tcp`, `grant_tcp_listen`, `grant_udp`, `grant_native`.
