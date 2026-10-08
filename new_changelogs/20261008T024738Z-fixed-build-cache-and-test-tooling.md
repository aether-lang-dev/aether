- **The second identical `ae build` hits the cache, and one program leaves
  one entry (#2500).** The first build of a source has no depfile yet, so its
  cache key falls back to a walk of the source tree; aetherc writes the
  depfile during that build, and every later build keys on it. `ae build`
  published the binary under the tree-walk key, which no later build
  computes, so the second build compiled again and published a second copy,
  and only the third hit. It now recomputes the key once the depfile is
  written and publishes under that, as `ae run` already did. `ae run`
  recomputes with the same salt it looked up with, so a program with a
  binary import no longer publishes under a key its next run never asks for.
- **`ae build` and `ae run` rebuild when the C compiler changes (#2477).**
  The cache key covered the source, aetherc, ae, libaether and the flags, but
  not the C compiler, so the same source built with another `gcc` first on
  PATH, another `$CC` / `$AE_CC`, or a compiler upgraded in place was handed
  the binary the previous compiler made, reported as a cache hit. The key now
  includes the compiler setting and, for each program it names, the resolved
  path and a hash of the file.
- **On Windows, an AVX build no longer faults on a 256-bit spill (#2476).**
  The Win64 stack is only 16-byte aligned and GCC does not realign it for
  32-byte values (GCC bug 54412), but it can still spill them with the
  aligned `vmovaps` / `vmovdqa`, so an `f32x8` program built with `-mavx2`
  segfaulted under MinGW GCC 15. When the cflags carry a `-m` option and the
  compiler reports AVX enabled under them, `ae build` and `ae run` on Windows
  pass `-Wa,-muse-unaligned-vector-move`, and the assembler (binutils 2.38 or
  later) encodes those moves as `vmovups` / `vmovdqu`, which cost the same on
  aligned data and do not fault on the rest. An older assembler gets a
  warning. Builds without AVX, and every other platform, are unchanged.
- **Shell tests signal the servers they started by job, never by a
  remembered pid (#2479).** The Windows test jobs intermittently died partway
  through the shell tests with exit code 2304, an MSYS2 shell killed by
  SIGKILL, and no test named; each time the test in flight was
  `http_reverse_proxy_pool` tearing down. That test and its `_extra` half
  `disown`ed their servers and later sent `kill -9` to the remembered pid
  numbers. A server that had already exited (a lost port bind, which the test
  retries) gives its number back, and MSYS2 reuses pid numbers out of order,
  so the SIGKILL could reach an unrelated process. Both tests now keep their
  servers as jobs of the test shell and signal them by job
  (`tests/lib/server_jobs.sh`): bash resolves a job when it signals, and
  fails once the job is gone, so only a live child of the test is ever
  killed. Liveness checks match running jobs by pid, not by the state word,
  which bash prints in the locale's language.
