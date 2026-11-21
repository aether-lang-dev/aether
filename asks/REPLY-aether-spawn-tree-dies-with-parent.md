# REPLY: a spawned process tree that ends with its parent

**Re:** selaenium `asks/aether-spawn-tree-dies-with-parent.md`

**RESOLVED** on the branch `batch/runtime-quickjs-staticlib-spawntree` (a
release that carries it): `std.os` has `spawn_tree_to`. Once selaenium is on
such a release it can replace `selenium_core/drivermgr/proctree/` with it.

**The shape.** An explicit spawn, not a change to any default, and a handle
the caller owns rather than a table keyed by token:

```aether
tree, err = os.spawn_tree_to(prog, argv, env, "driver.log")   // grace_ms: 2000
pid = os.tree_pid(tree)            // the program's own pid (and process group)
os.tree_kill(tree)                 // end the whole tree now; does not wait
code, _e = os.wait(os.tree_token(tree))
os.tree_free(tree)                 // ends the tree first if it still runs
```

The token works with `wait`, `wait_any`, `wait_any_timeout` and
`wait_pid_timeout` like any other. The tree ends when the caller's process
ends however it ends, when the handle asks, and when the program exits on its
own (what it left running is ended before the token reaps), so after the wait
nothing it started is running. SIGTERM to the group and every descendant
first, `grace_ms` for them to go, then SIGKILL until none is left. The exit
code is the program's (143 when the tree was ended and it died of the
SIGTERM).

**How, by platform** (the same as your `proctree.c` in outline, with three
differences):

- POSIX: the caller forks a supervisor (no exec; only async-signal-safe calls
  after the fork) and the supervisor forks the program. The supervisor is the
  caller's child and is the token, exiting with the program's status, so the
  ordinary waits reap it; no intermediate process.
- The supervisor watches a pipe, not the caller's pid: the caller holds the
  only write end, in the handle, so end of file means the handle was released
  or the caller's process died, SIGKILL included. It is the process's
  descriptor, so a runtime that spawns from a pooled thread does not set it
  off (your reason for rejecting `PR_SET_PDEATHSIG` applies to that too), and
  there is no pid to reuse. It is close-on-exec: an exec of the caller ends
  the tree, and a fork of the caller that does not exec keeps it alive while
  that fork lives. The supervisor closes every descriptor but its own, so it
  never holds another tree's write end.
- Linux: `PR_SET_CHILD_SUBREAPER`, and the descendants read from `/proc` (the
  parent of each process, so it does not need `CONFIG_PROC_CHILDREN`). The
  program gets `PR_SET_PDEATHSIG` on the supervisor, which is single-threaded:
  a supervisor killed by SIGKILL takes the program with it, not what the
  program started.
- FreeBSD: `PROC_REAP_ACQUIRE`, `PROC_REAP_KILL` for SIGTERM and SIGKILL, and
  `PROC_PDEATHSIG_CTL` for the program.
- macOS has no subreaper. The supervisor lists the descendants
  (`proc_listpids`) every 50 ms while it watches and signals every one it
  listed while its start time still matches, so a descendant that left the
  group is ended even after its parent exited. One that leaves the group and
  is orphaned within one poll (a fast double-forked daemon) belongs to launchd
  and is missed; the test shows that window by having its leaf exit at once.
- Windows: the Job Object `spawn_proc_to` already makes, with
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, the caller holding its only handles;
  the job is ended when the token reaps and on `tree_kill`. No grace period
  (no SIGTERM). Compile-checked; not run on Windows for this change.
- Other POSIX systems: the program's process group only.

`os.kill(token, SIGTERM)` (or SIGINT, SIGHUP) ends the tree as `tree_kill`
does; SIGKILL to the token cannot be caught, so use `tree_kill`.

**Tests.** `tests/integration/os_spawn_tree`: a leaf whose child calls `setsid`;
the parent exits without stopping it, the parent is killed with SIGKILL,
`tree_kill`, and the leaf exiting on its own, each asserting by recorded pid
that the leaf and the escaper are gone, plus a control showing
`spawn_proc_to`'s group kill misses the escaper. Passes on Linux (gcc 16),
FreeBSD 15.1 and macOS arm64; with the supervisor's descendant signalling
removed, the four escaper assertions fail.

**The signal-mask ask** (`asks/aether-spawn-child-inherits-blocked-signal-mask.md`)
was fixed earlier (8335661f, `aether_child_reset_signals`), and both the
supervisor and the program start with it.
