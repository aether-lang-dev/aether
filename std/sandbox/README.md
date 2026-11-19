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
| `grant_exec(cmd)` | running commands whose command line matches (`os.run`'s program, or `os.system`/`os.exec`'s command string) |
| `grant_shell()` | running the shell (`/bin/sh`; on Windows what `COMSPEC` names), which a command string for `os.system` or `os.exec` also needs |
| `grant_env(name)` | reading environment variables |
| `grant_tcp(host, port)` | outbound TCP to matching hosts on `port`, or any port when `port` is 0 |
| `grant_tcp_listen()` | listening for and accepting TCP connections |
| `grant_udp(host, port)` | UDP with matching hosts on `port` (0 = any) |
| `grant_native(path)` | loading native libraries (`dlopen`) |
| `grant_fork()` | starting new processes, in a child `spawn_sandboxed` starts (it may always start threads) |
| `grant_all()` | everything |

A pattern is `"*"` (anything), a prefix ending in `*` (`"/etc/*"`, `"echo *"`),
a suffix starting with `*` (`"*.example.com"`), or an exact string. A grant
list with no grants denies everything.

A command string runs through the shell, which runs all of it, not only the
part a grant matched: `grant_exec("echo *")` matches `"echo hi; rm -rf ~"`
too. So `os.system` and `os.exec` need `grant_shell()` as well as the
command's own grant, and that pair allows anything the shell can be told to
run. For untrusted code, grant the program (`grant_exec("echo")`) and call
`os.run` or `os.run_capture` with an argv, which runs no shell. A program
file with no `#!` line is the exception: it runs through the shell, and so
needs `grant_shell()` too (#3755).

`sandbox.new` returns a `sandbox.Grants`, an opaque type: it can be passed to
`enforce`, to `free`, and to a host bridge's `run_sandboxed` calls, but
it is not a `std.list`, so code holding one cannot add entries to it. The
grants go in through the `grant_*` calls in the trailing block.

Nested `enforce` blocks intersect: an operation is allowed only when every
enclosing grant list allows it, so an inner block can narrow its parent's
access but never widen it.

The grants are frozen when the block starts: `enforce` checks against a
private copy, and the block's parameter is `null`, not the list. Adding a
grant to the list from inside the block, by code it was passed to, changes
what the next `enforce` of that list allows, never the running block.

## enter and leave

`sandbox.enter(perms)` and `sandbox.leave()` are `enforce`'s two halves, for a
sandboxed stretch that is not one block: a host's entry helper that puts a
guest's grants in force, runs code it does not hold as a closure, and takes
them off later.

```aether,fragment
sandbox.enter(worker)
defer sandbox.leave()
run_guest_entry()          // checked against worker's grants
```

- `enter` **lends** the list: no copy, unlike `enforce`. A grant added to it
  after `enter` applies at once, and the list must stay alive until the
  matching `leave`. Use `enforce` when the code inside must not widen its own
  grants through the list.
- Levels nest by intersection, with `enforce` blocks and other `enter`s alike:
  an inner level only narrows. `null` narrows nothing; an empty list denies
  everything.
- A thread holds at most 64 levels (blocks, `enter`s, trusted calls, host
  bridge calls); the 65th panics rather than be left out.
- `leave` with no level in force is a no-op.
- `leave` takes off the innermost level, whoever pushed it: pair each `enter`
  with one `leave` on the same thread, on every path out (`defer` does it).
  An unpaired `leave` inside an `enforce` block takes that block's level off.
- A panic caught by a `try` outside the `enter` takes its level off as it
  unwinds.

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
- `sandbox.spawn_socket(perms, prog, argv, env)`, the same containment with
  a socket to the child (its fd 3, as `os.run_socket` gives it), returning
  `(fd, pid, err)`. It fails closed: where the child cannot be contained
  it returns an error and starts nothing. `sandbox.spawn_socket_uncontained`
  is the explicit way to run one with no sandbox. A foreign interpreter's
  start-up reads come from `grant_runtime("python3")`, which asks the
  interpreter for them;
- the host bridges, which run an embedded engine under it:
  `contrib.host.stunt` (the test double, `stunt.run_sandboxed(worker, code)`)
  here, and the Ruby, Tcl, Lua, Python and other bridges of
  [aether-guest-languages](https://github.com/aether-lang-dev/aether-guest-languages) (`tcl.run_sandboxed(worker, code)`).

## Paths and threads

A file-system path is matched where it leads: `..` and symlinks in it are
resolved first (a grant's directory too, when it is made), so a grant for
`/box/*` cannot be left through `/box/../x`, a symlink in `/box` that points
out, or a dangling one. A path that cannot be resolved is refused.

`enforce` contains the code that runs on its own thread inside the block.
Work it hands to another thread, an actor or a `std.worker`, runs with that
thread's authority, and other threads are not checked against the block's
grants while it runs. Contain a worker by entering `enforce` in the work it
runs.

## Limits

The checks sit in std and, for spawned children, in libc via LD_PRELOAD. Code
that bypasses both (raw `syscall()`, statically linked binaries, `io_uring`)
is not contained by them. On FreeBSD, `std.capsicum` adds kernel-enforced
containment. `enforce` limits what the block can *do*, not which names it can
*see*; add `seal except` to the block for that (`docs/hide-and-seal.md`).

The full design, with the patterns, the interception surface and comparisons
with other sandboxes, is in `docs/containment-sandbox.md`.

## Exports

`new`, `enforce` (with optional trusted names), `enter`, `leave`, `free`,
`spawn_socket`, `spawn_socket_uncontained`, and the grants
`grant_all`, `grant_fs_read`, `grant_fs_write`, `grant_fs`, `grant_exec`,
`grant_shell`, `grant_env`, `grant_tcp`, `grant_tcp_listen`, `grant_udp`,
`grant_native`, `grant_fork`, `grant_without_landlock`, `grant_runtime`.
