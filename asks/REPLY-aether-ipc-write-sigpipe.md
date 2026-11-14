# REPLY: std.ipc.write_some / write raise SIGPIPE on a full pipe whose reader died

**Re:** nib-code `asks/aether-ipc-write-sigpipe.md` (branch `aether-port`)

**RESOLVED** in PR #3694 (branch `batch/nov13-naeb-asks`; the first
release after 0.913.0 carries it).

Cause: std.ipc already wrote to pipes with SIGPIPE blocked on the writing
thread (`ipc_write_nosigpipe`), and that holds on Linux and the BSDs, which
raise a write's SIGPIPE on the thread that wrote. macOS raises it on the
*process* (xnu's `dofilewrite`), and a process-directed signal goes to any
thread that has not blocked it. With one thread the blocked writer kept it
pending and took it back with `sigwait`, which is why "reader already gone,
room in the pipe" behaved; with a worker writing and the main thread waiting
in `sleep`, the main thread took it and the process died with 141. On Linux the
pipe being full is the point: a blocking write that the reader's exit
interrupts returns the bytes it had written (65536 here) and still raises
SIGPIPE on the thread, and std.ipc took the pending signal back only after
an EPIPE return, so restoring the mask delivered it (strace on ae-x64).

Fix, per descriptor and never process-wide: on macOS every write std.ipc
makes (`write`, `write_some`, `write_frame`, the frame writer, `send_fd`)
first sets the descriptor's own `F_SETNOSIGPIPE` (for a socket that is
`SO_NOSIGPIPE`). No handler, disposition or process mask changes; other
descriptors raise SIGPIPE as their owners expect. It stays set on the
descriptor rather than being restored after each call, which would race
another thread writing the same descriptor. Everywhere, any SIGPIPE a std.ipc write
raised (not only on an EPIPE return) is taken back with `sigwait` before the
thread's mask is restored; sockets keep `MSG_NOSIGNAL`.

`tests/integration/os_run_stdio` gains the ask's scenario, for `ipc.write`
and for `ipc.write_some` under `ipc.wait_writable`: a worker fills a child's
stdin while the child has stopped reading, the child is killed, and the
write must report the gone reader. On origin/main the test process dies of
SIGPIPE on macOS and on Linux. nib can drop `nibsys_ignore_sigpipe()` (ae/nib/sysgap) and
its call in `nib.tools.process` on the release carrying this.
