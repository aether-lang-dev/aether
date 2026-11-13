- **`ipc.write` / `ipc.write_some` from a worker thread no longer end the
  process with SIGPIPE.** A write that filled a child's stdin pipe and was
  waiting when the child was killed took SIGPIPE and the process died (141),
  though the docs promise an error. On Linux the interrupted write returned
  its partial count and still raised the signal, which std.ipc took back only
  on an EPIPE return; on macOS the signal is the process's, not the writing
  thread's, so blocking it on that thread did not hold it off. std.ipc now
  takes back any SIGPIPE its write raised, and on macOS sets the descriptor's
  `F_SETNOSIGPIPE` first; no process-wide signal state is touched (nib-code
  ask ipc-write-sigpipe).
