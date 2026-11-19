- **The sandbox preload's `syscall()` forwards what a sandbox needs and
  stays default-deny under a policy.** With `libaether_sandbox.so`
  preloaded it answered every raw `syscall()` `ENOSYS` when nothing was
  enforced and `EPERM` under a grant file, futexes included: GLib's futex
  waits returned at once, so an idle GTK4 program's `pool-spawner`,
  `pool-0` and pango threads spun (sae-driver: 35 s of CPU in 12 s idle, 0.2 s
  now, with no grant file, under a grant file, and with an in-process level
  on the GTK thread), and the Aether scheduler's `sched_getaffinity` and
  `membarrier` failed. Now, with nothing in force every call is forwarded;
  under a policy (a grant file, or an in-process level on the calling
  thread) only an allowlist of calls that reach nothing outside the process
  is (futex, rseq, membarrier, scheduling and signals of this process,
  ids, clocks, `getrandom`, I/O on a descriptor already held, epoll and
  friends; FreeBSD libthr's `getpid` and `sigreturn`), process creation gets
  `fork()`'s rule and executable memory `mmap()`'s and `mprotect()`'s, and
  everything else is refused and logged (`AETHER_DENIED: native
  syscall:unlinkat`) unless a native grant names it (`syscall:unlinkat`;
  `native:*` and `grant_all` cover all). Every argument is passed on where
  the caller put it, and call numbers are the architecture's own (the old
  named list was x86_64 numbers on every Linux).
  `tests/integration/sandbox_preload_syscall`; `sandbox_clone_fence` runs a
  raw `clone3` with the fork grant.
