- **`std.guest` and `std.guest.child`: a guest in a sandboxed child process,
  and the protocol to it, in the standard library.** A host starts a worker
  (one binary with an engine for some language compiled in) under
  `sandbox.spawn_socket`, failing closed, or uncontained by name
  (`guest.launch_uncontained`); mounts the functions its script may call;
  sends the script; and answers each `CALL` with a JSON value, a failure, or
  a file handed over as an open descriptor. Every exchange has a deadline,
  then a `CANCEL`, then a kill after a grace; every spawn, grant, deny,
  timeout and kill goes to one audit log, with what the worker reports.
  `std.guest.child` is the worker's half: it reads the mounts and the
  script, enters `sandbox.enforce` with no grants before the first step,
  and gives the engine `say`, `log`, `call_host`, `poll` (for a cancel,
  between steps or from an interrupt hook), `done` and `error`, so a
  worker is its engine and the glue between the two. The protocol (text
  verbs with JSON arguments over `std.ipc` frames) is documented once, in
  `std/guest/README.md`. It was RexxAE's `worker/rexxworker`, which already
  drove two engines (RexxAE and a MicroQuickJS worker) with nothing
  Rexx-specific in it.
