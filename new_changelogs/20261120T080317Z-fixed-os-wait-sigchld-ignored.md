- **std.os reports a child's exit status when the process ignores
  SIGCHLD.** An ignored SIGCHLD (inherited across exec: the Erlang VM starts
  its children so, and a library loaded into such a host has it) makes the
  kernel reap each child as it exits, and `waitpid` fails with ECHILD.
  `os.run_capture` read the child's output and returned
  `(stdout, -1, "waitpid failed")`; `os.run`, `os_system`,
  `run_pipe_drain_and_wait`, and `wait` / `wait_any` / `wait_pid` after
  `spawn_proc`, `run_pipe` or `run_stdio` failed the same way (seen on
  Linux; macOS does not auto-reap). While any child std.os started is unreaped, SIGCHLD is now
  at its default, and the process's own disposition is put back when the
  last one is reaped; a process that does not ignore SIGCHLD is never
  touched. `docs/stdlib-reference.md` (std.os) says what a host may notice.
