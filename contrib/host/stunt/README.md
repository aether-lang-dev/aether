# contrib.host.stunt: a test double for a hosted language

`contrib/host/stunt` is not a language. It is a small C engine that a host
loads exactly as it loads a real interpreter, and that does, on request, each
thing a hosted language can do that a host bridge has to handle: open a file,
read the environment, connect, exec, load libc, make a raw syscall, call back
into the host, use the shared map, run forever, never answer, send too much.
The name is the film one: a stunt double takes every hit, so the real
languages don't have to.

It exists because the real bridges (`lua`, `python`, `ruby`, `perl`, `tcl`,
`js`/`duktape`, `quickjs`, ...) bring interpreter packages and their version
churn into aether's CI: Lua 5.5, Ruby 3.0 and Python 3.14 each broke a lane
in one month. Most of what those tests are there to catch is not about Lua
or Python at all; it is about the bridge and the sandbox under it. stunt
catches the same things with nothing to install. It sits beside the real
bridges; nothing is replaced.

## The interface

```sh
stunt list                          # the catalogue: name, modes, who judges it, what it does
stunt <check> <succeed|refused>     # run one check
stunt <check> host                  # a host-checked one (spin, silent, oversize, half-frame)
```

stunt runs the one check, compares the outcome with the expectation and
prints one line:

```
dlopen-libc-by-path: refused (/lib/x86_64-linux-gnu/libc.so.6) - expected refused: PASS
```

It exits 0 when they match, 1 when they do not (or the outcome could not be
judged), 2 on a usage error (an unknown check, an expectation the check
cannot take), and 3 when a host-checked check was stopped.

The expectation lives in the test, which knows the grants it gave, so one
catalogue runs under any number of policies. stunt judges an outcome itself
wherever it can. A **refusal** is what a sandbox gives: `EACCES` or `EPERM`
from libc, the preload library or the kernel, or a host function's error
whose reason starts `refused:` (what `read_file` and `getenv` say when the
grants say no). Anything else that goes wrong (a missing file, a failed
read, an unset variable, an unreachable network, no such host function) is
an **error**, never a refusal, so it cannot pass for a sandbox at work. A
variable that is not there is a refusal only with evidence of one: it is in
the environment and `getenv` will not give it (the preload), the bridge's
scrub took it out of the engine's copy, or the host that started the child
named it in `STUNT_WITHHELD`. A connect is non-blocking: refused at once, or
let through. It goes to TEST-NET-1, a range reserved for documentation, so a
connect the sandbox lets through reaches nobody and sends nothing to a real
host; being non-blocking, it waits at most 100 ms there and cannot hang.

## The catalogue

| Check | Modes | What it does |
|---|---|---|
| `open-etc-passwd` | in-process, child | open `/etc/passwd` for reading through libc |
| `open-granted-file` | in-process, child | open `./stunt-fixture.txt` through libc |
| `write-granted-file` | in-process, child | create and write `./stunt-written.txt` through libc |
| `getenv-home` | in-process, child | `getenv("HOME")` through libc |
| `env-cached` | in-process, child | `HOME` from the engine's own copy of the environment, taken at start (Python's `os.environ`, Perl's `%ENV`) |
| `connect-192.0.2.1` | in-process, child | TCP connect to `192.0.2.1:443` through libc: TEST-NET-1 (RFC 5737), where nothing answers |
| `connect-ipv4-mapped` | in-process, child | TCP connect to `[::ffff:192.0.2.1]:443` over `AF_INET6`, which a grant for `192.0.2.1` must cover |
| `exec-bin-sh` | in-process, child | fork, then `execve("/bin/sh", "-c", "exit 0")` in the child |
| `fork` | child | fork a child that exits at once |
| `dlopen-libc-by-name` | child | `dlopen` libc by its soname: a handle on the real `open`/`connect`/`execve` |
| `dlopen-libc-by-path` | child | `dlopen` libc by the path the loader loaded it from |
| `dlopen-libm` | in-process, child | `dlopen` libm: a library that is not libc loads |
| `syscall-fn-openat` | child | `openat` of `/etc/passwd` through libc's `syscall()` (ctypes, Perl's `syscall`) |
| `syscall-openat` | child | `openat` of `/etc/passwd` by the syscall instruction (Landlock's row) |
| `syscall-socket` | child | `socket(AF_INET)` by the syscall instruction (seccomp's row) |
| `binding-read-etc-passwd` | in-process, child | ask the host's `read_file` for `/etc/passwd` |
| `binding-read-granted-file` | in-process, child | ask the host's `read_file` for `./stunt-fixture.txt` |
| `binding-getenv-home` | in-process, child | ask the host's `getenv` for `HOME` |
| `callback-greet` | in-process, child | call host function `greet("stunt")`: a string in, `"hello, stunt"` out |
| `callback-add` | in-process, child | call host function `add(2, 40)`: ints in, `42` out |
| `callback-unregistered` | in-process, child | call `nope`, which no host registers: an error, not a crash |
| `map-get` | in-process, child | read `input` from the shared map (the host put `"stunt-in"`) |
| `map-set` | in-process, child | write `output` to the shared map and read it back |
| `map-overwrite-input` | in-process, child | overwrite `input`, which the host froze |
| `spin` | in-process, child | **host-checked**: loop until stopped, by the instruction limit or a cancel |
| `silent` | child | **host-checked**: never answer, never exit; the host must kill it after a grace period |
| `oversize` | child | **host-checked**: send the host one 1 MiB message; the host must refuse the frame |
| `half-frame` | child | **host-checked**: send a frame's header and part of its payload, then stall; the host must time out inside the frame (`ipc.TIMED_OUT_INSIDE_FRAME`) and hang up |

The `open-*`, `write-*`, `getenv-*`, `connect-*`, `exec-*`, `dlopen-*` and
`syscall-*` rows go the way an interpreter's own runtime goes (Lua's
`io.open`, Python's `os.getenv`, ctypes): straight to libc, or past it. In
process, only `libaether_sandbox.so` holds them, so they need the host to
run under it (Linux, FreeBSD). The `binding-*` rows go through host
functions that check the grants themselves (the native-bindings model of the
Duktape and QuickJS bridges), which works everywhere. The child-only rows are
the ones only a separate process can be held to: the preload refuses a
handle on libc, `fork` and `syscall()` only in a child it was started in, and
only Landlock and seccomp see the syscall instruction.

## Two modes, one catalogue

**In-process.** `stunt.c` built as a shared library, `libstunt`, which
`module.ae` and `aether_host_stunt.c` load as `contrib/host/lua` loads liblua:
`dlopen` at the first call (`AETHER_STUNT_LIB`, else `libstunt.so`,
`libstunt.dylib` or `stunt.dll` on the loader's path), every function found
with `dlsym`. The "script" passed to a run is `"<check> <expectation>"`. The
bridge has the surface every contrib host shares, and the #904 tier:

```aether,fragment
import contrib.host.stunt

@c_callback
greet(vm: ptr) -> int {
    stunt.push_str(vm, "hello, ${stunt.arg_str(vm, 1)}")
    return 1
}

main() {
    worker = sandbox.new("worker") { grant_fs_read("/srv/data/*") }
    rc = stunt.run_sandboxed(worker, "open-etc-passwd refused")   // prints the line, 0 on a match

    vm = stunt.vm_new()
    stunt.vm_register(vm, "greet", greet)
    m = stunt.map_new()
    stunt.map_put(m, "input", "stunt-in")
    stunt.vm_set_map(vm, stunt.map_token(m))         // inputs frozen
    stunt.vm_set_instruction_limit(vm, 5000000)     // spin stops here
    rc = stunt.vm_eval_sandboxed(vm, worker, "callback-greet succeed")
    println(stunt.vm_result(vm))
}
```

A sandboxed run does what every in-process bridge has to: it pushes the
grants as a level of the calling thread with `aether_sandbox_push` and pops
it after, so a bridge called inside an `enforce` block is held to both; it
scrubs the engine's cached environment to what the grants allow, as the Perl
and Ruby bridges scrub `%ENV` and `ENV`; and it gives the engine `read_file`
and `getenv` (checked) and, with a map, `aether_map_get` and `aether_map_put`.

**Child process.** The same `stunt.c` with `stunt_main.c`, the `stunt`
program, started by `sandbox.spawn_socket` with `args(1)` and `args(2)` the
check and the expectation. When `AETHER_IPC_FD` is set it speaks std.ipc's
frames on that descriptor (a 4-byte big-endian length, then the payload):

```
child -> host   CALL <name>[\t<arg>]...        an argument is s<text> or i<int>
host -> child   OK s<text> | OK i<int> | OK n | ERR <reason>
host -> child   CANCEL                          at any time; stops spin
child -> host   RESULT <code> <line>            last; then it exits <code>
```

The host reads and writes only whole frames, with `ipc.read_frame_timeout`
and `ipc.write_frame`; the child writes each message as one frame, except
`half-frame`, which stops inside one on purpose.

None of the host-checked rows can outlive a broken host. `silent` and
`half-frame` stop when the host hangs up (the socket polls closed or reads
EOF), and after 30 s regardless; a child the host started ends by `SIGALRM`
after 60 s whatever it is doing; and `spin` has a default instruction limit
wherever no host is there to cancel it: the bridge's tier 1
(`stunt.run("spin host")`) and the stunt program run by hand.

Every host function is the host's to answer, so `callback-*`, `map-*` and
`binding-*` mean the same in both modes; an `ERR` is a refusal only when its
reason starts `refused:`. The host passes the child only the environment the
grants allow, so a copy taken at start (`env-cached`) is no wider than
`getenv`, and names what it kept back in `STUNT_WITHHELD=NAME,...`, so the
child can tell a withheld variable from one that was never set.

## The tests

`tests/integration/contrib_host_stunt/test_stunt_*.sh`, one per policy, each a
list of `stunt` invocations that stops at the first mismatch:

```sh
policy "fs_read=$WORK/stunt-fixture.txt,env=HOME"
stunt open-granted-file succeed
stunt open-etc-passwd refused
stunt syscall-openat refused
stunt env-cached succeed
```

`stunt_lib.sh` builds `libstunt`, the `stunt` program, the bridge and the
host (`host.ae`), and runs each check in every mode that applies, chosen by
check name: in-process everywhere (the libc rows under the preload, on Linux
and FreeBSD); the contained child on Linux with Landlock; the uncontained
child (`sandbox.spawn_socket_uncontained`) on macOS and FreeBSD, where the
contained spawn refuses, for the rows that are about the host. Windows has
the in-process rows that are not POSIX calls or POSIX paths (callbacks, the
map, the environment, spin): there is no socket channel there.

The policies: no grants, one file granted, a directory granted for writing,
the network to one address and to another, a native grant, `/bin/sh` and
fork, fork without exec (so `exec-bin-sh refused` is the execve's refusal,
not the fork's), nested (`"<outer>|<inner>"`: the bridge called inside an `enforce`
block, with the depth checked after every run) and the host-checked rows.

They catch what the real bridges' tests catch. Each of these was reverted on
an aarch64 Linux lane and a stunt check failed, then restored:

| Reverted | Failed |
|---|---|
| the preload's dlopen of libc by path (eb90d1d6) | `dlopen-libc-by-path: succeeded (loaded /lib/aarch64-linux-gnu/libc.so.6) - expected refused: FAIL` |
| `::ffff:` normalisation in `aether_sandbox_match.h` | `connect-ipv4-mapped: refused (connect: Permission denied) - expected succeed: FAIL` |
| every level checked, not only the innermost (#2955's class) | `open-etc-passwd: succeeded (read "root:x:0:0:...") - expected refused: FAIL` |
| `aether_sandbox_pop` taking a nested level off | `host: open-etc-passwd: the bridge left 1 level(s) in force` |

`contrib/host/stunt/test_host_stunt.ae` is the bridge's own spec, run by
`make contrib-host-check` (after `make stunt`).

## Adding a check

1. Write `static int c_<name>(st_vm* vm, char* d, size_t cap)` in `stunt.c`:
   do the one thing, put what happened in `d`, and return `OUT_SUCCEEDED`,
   `OUT_REFUSED` (only for a sandbox's refusal: `by_errno` decides from
   `errno`) or `OUT_ERROR`.
2. Add it to `CATALOGUE` with its modes, whether the host judges it, and a
   one-line description; and to the table above.
3. Name it in the `test_stunt_*.sh` whose policy decides it, with the
   expectation that policy gives. If a mode or platform cannot run it, say
   so in `applies` in `stunt_lib.sh`, by name.
4. Revert the fix it is there for and watch it fail.

## A real language in a hosts repo

The plan is a separate hosts repository with the real bridges, and more
languages, each held to this same named catalogue. A language there mirrors
stunt one check at a time: for each name, a short script in that language
that does the same thing its own way (`io.open("/etc/passwd")` in Lua,
`ctypes.CDLL(None).syscall(...)` in Python, `Fiddle.dlopen` in Ruby) and
prints the same line, run by the same `test_stunt_*.sh` lists with
`stunt` pointed at that language's runner. A check a language cannot express
is left out by name, as stunt's own child-only rows are. Moving the real
bridges out of aether is a later step, with a transition.
