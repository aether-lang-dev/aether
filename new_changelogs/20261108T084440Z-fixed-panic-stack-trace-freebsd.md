- **A panic prints its stack trace on FreeBSD.** The trace came from
  `backtrace()`, which the runtime used only on glibc and macOS; on FreeBSD it
  lives in libexecinfo, which programs do not link (and GhostBSD ships only
  its `libexecinfo.so.1`, with no `-lexecinfo` symlink). The runtime now loads
  libexecinfo with `dlopen` when a panic first needs a trace, as glibc loads
  libgcc_s for its own `backtrace()`, and reads FreeBSD's
  `0xADDR <symbol+OFF> at binary` lines. No link line changes. Test:
  `tests/integration/panic_stack_trace`.
