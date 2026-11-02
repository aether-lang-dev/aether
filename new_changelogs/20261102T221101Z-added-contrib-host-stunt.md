- **`contrib/host/stunt`: a test double for a hosted language.** A C engine
  with no language that a bridge loads as it loads an interpreter
  (`import contrib.host.stunt`, `libstunt` dlopen'd), or that a host runs as
  a child under `sandbox.spawn_socket` speaking std.ipc frames. Its "script"
  is `<check> <succeed|refused>`, and its named catalogue (`stunt list`)
  covers what a bridge must handle: the grant checks, the preload (libc,
  dlopen of libc, `syscall()`), Landlock and seccomp (the syscall
  instruction), nested levels, typed host callbacks, the shared map, the
  cached environment, the instruction limit and cancel, an oversize frame.
  `tests/integration/contrib_host_stunt` runs it under nine policies with no
  interpreter installed; reverting the preload's dlopen-by-path fix, the
  `::ffff:` normalisation, or nested-level checking each fails a check. The
  real bridges stay; `make stunt` builds it, and `make contrib-host-check`
  runs its spec.
