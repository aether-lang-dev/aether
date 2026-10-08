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
  path, a hash of the file and the first line of its `--version`, since the
  file found on PATH can be a trampoline whose bytes never change when the
  compiler behind it does (macOS's xcrun `/usr/bin/gcc`, a ccache
  masquerade). The key text is also appended with a bound: it was built with
  unchecked appends into a 2 KiB stack buffer, which about 110 `--extra`
  files overran; past the end the key is truncated, which only means a
  rebuild.
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
- **`ae help <script.ae>` diagnoses with the compiler `ae build` uses.**
  Its own compiler search tried `$AETHER_HOME/bin` before the `aetherc`
  beside `ae`, so in a source tree with an installed toolchain configured
  it reported the installed compiler's errors (an older one rejected the
  current std outright). It now resolves the toolchain as every other
  command does; `AETHERC` still overrides it.
- **`ae build` on Windows works with a batch-file compiler from a deep
  directory (#2533).** A C compiler that resolves to a `.cmd` or `.bat`
  (a gcc wrapper or shim first on PATH, or `$CC` naming one) runs through
  cmd.exe, whose command line is capped at 8191 characters, so a build
  with a long command line failed with "The command line is too long".
  ae now resolves the program the way Windows will (an explicit `.cmd` or
  `.bat`, or a bare name found through PATH with the .com, .exe, .bat,
  .cmd order) and, when the arguments would not fit, hands them to such a
  compiler through a response file (`gcc @file`, quoted the way gcc and
  clang read it), removed after the run. An executable compiler keeps the
  direct spawn.
- **`ae` passes every argument of a command whole (#2534).** The
  spawners split a command into at most 511 arguments and dropped the
  rest without an error; on Windows a 32 KB re-quoting buffer let a long
  argument with spaces split, and a quoted argument ending in a backslash
  fused with the next. They now share one splitter with no count or length
  limit, and each Windows argument is quoted the way the child's C runtime
  reads it back. `ae run x.ae -- args` forwards the program's arguments as
  a vector, so spaces, quotes and any number of them arrive exactly (the
  command string it used to build dropped what did not fit and could not
  carry a quote). A program `ae run` cannot start is reported as that, no
  longer as a crash. `[build] cflags`, `link_flags` and `defines` expand
  `${AETHER_*}` into a string of any length, where they were cut at 512
  or 1024 bytes, and so are the compile flags built from them.
- **`aether.toml` lines of any length, and comments after values
  (#2535).** The reader split a line longer than 511 bytes, cutting the
  value and reading the rest as a line of its own, and it kept a trailing
  `# comment` in the value, so `cflags = "-O2"  # tuned` handed the C
  compiler `#` and `tuned` as files. A `#` inside quotes stays part of the
  value.
- **`ae` reads the generated C's header lines, `extra_sources`, the
  depfile and `ae bindgen`'s preprocessor output whole (#2536).** These
  readers took a long line in fixed pieces and read the rest as a line of
  its own. A module `@link` line past 511 bytes made `ae build` say the
  program has no main(); past 1 KB, the flags after the cut never reached
  the link; past 2 KB, the `@source` files and `@c_include` directories
  listed after it were dropped, and a cross build missed a sysroot library
  named past the cut. A one-line `[[bin]] extra_sources` past 8 KB lost the
  entry the cut fell in and passed `", "` to the compiler as a file. A
  depfile `read` line past 2 KB hashed a path that does not exist, so an
  edit to that file was served from the cache; a depfile line that cannot
  be read whole now makes ae walk the source tree as it does with no
  depfile. `ae bindgen consts` cut an expansion past 1 KB, so `1+1+...`
  imported as a smaller number. The `@link`, `@source` and `@c_include`
  lists now have no length limit, and `extra_sources` that do not fit the
  8 KiB source list are an error, as `--extra` past it already was, where
  the entries past it were dropped with a warning.
