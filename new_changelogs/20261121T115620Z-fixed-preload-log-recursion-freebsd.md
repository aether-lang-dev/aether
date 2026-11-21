- **A refusal under the LD_PRELOAD sandbox on FreeBSD no longer recurses
  until the stack runs out.** The library opened its log,
  `aether-sandbox.log` in the working directory, with libc's `fopen`, and
  FreeBSD's `fopen` opens through `_open`, which the library has wrapped
  since 0.927.0. Where the grants did not cover the working directory, the
  log line of a refused call asked to open the log, was refused, and logged
  that, without end: every refused open under the default log faulted after
  half a gigabyte of stack (eight `contrib_host_stunt` tests segfaulted), and
  in a child started by `spawn_sandboxed` it ran for minutes before the
  fault (`sandbox_preload_lookup` never finished). The log is now opened by
  `openat` to the system call, unchecked, as `fopen` reached it on glibc;
  writing a log line also leaves the refusal's `errno` as it was.
