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
- **No count limit on `@link` tokens, `@source` files, `@c_include`
  headers or wasm exports, and no stale depfile (#2537).** The compiler
  kept the first 64 `@link` tokens and 256 `@source` files of an import
  closure and dropped the rest unsaid; it kept 64 `@c_include` headers and
  directories and cut a directory past 399 bytes. `ae bindgen consts`
  stopped at 4096 macros and 256 KB of names (`windows.h` has 23,000 and
  500 KB), a piece of a long `-dM` line that began with `#define ` was
  taken for a macro, and two runs at once shared one temporary probe file
  and could read each other's macros. A wasm `--emit=lib` dropped the
  exports past 8 KB of names, or one whose catalog line was cut, and a
  cross build's `SQLITE_CFLAGS` past 1 KB was cut with its opening quote
  left in. A dependency path aetherc could not record (out of memory) was
  left out of the manifest, and a write error left a partial one; either
  way an edit to the missing file was served from the cache. aetherc now
  writes no manifest then, removes the previous one and says so, and ae
  keys on the source tree. The depfile slot is named for the whole
  absolute path, where a 1 KB buffer cut it and a longer working directory
  gave every project's `main.ae` the same slot.
- **A build cache key covers everything it is built from, or there is no
  key (#2538).** With no depfile to go on, the key walks the source trees,
  and the walk stopped without a word at 8 directory levels and 4096
  files, and skipped any path past 1 KB, so an edit to a module out of its
  reach was served from the cache. Reaching a limit now means the build is
  not cached (`--verbose` says why); it still asks aetherc for the depfile,
  and is cached under the key made from that, which needs no walk. The key
  text itself was cut at 2 KB, so with eight long `--lib` directories or a
  hundred `--extra` files the `-D` defines and the optimisation level at
  its end did not count, and a build with other defines was served the
  first one's binary. The compiler lookup no longer cuts a `PATH` entry
  past 1 KB or a long `$CC`, the working directory has no length limit,
  and on Windows a drive or backslash path names its depfile slot without
  the working directory in front. A function in the reserved `_` namespace
  or one colliding with an extern is renamed whole, where two names
  sharing their first 277 bytes became one C name.
- **A Windows `--emit=lib` DLL carries the `aether_config_*` accessors
  (#2540).** `ae` added `runtime/aether_config.c` to a library build only
  on POSIX, so a C host linking a Windows DLL to walk the map or list a
  script returned failed with undefined references. `emit_lib_composite`,
  the test that walks one, skipped on Windows; its host now loads the
  library with `LoadLibrary` there and runs.
- **`ae fmt` keeps arithmetic on `state`, `after` and `func` binary
  (#2542).** The parser takes those keywords as ordinary names (#880), but
  the formatter took the operator after one for a prefix operator, so
  `after - mid` came out as `after -mid`, and `state * 6364136223846793005`
  as `state *6364136223846793005`, which reads as a dereference. The ten
  test files written that way are reformatted.
- **An `AETHER_CACHE_DIR` too long for the cache is refused, and the
  lib-dir cache test runs on Windows (#2539).** The cache directory was
  copied into 512 bytes, so a longer one was cut to another directory,
  which the cache was then made in. ae now stops with an error naming the
  limit (511 bytes) and asking for a shorter path. `cache_lib_invalidation`
  skipped Windows as if the lib-dir walk were POSIX-only, which it has not
  been since #1235; it runs there now, with its cache isolated through
  `AETHER_CACHE_DIR`, since Windows finds the home directory through
  `USERPROFILE` rather than `HOME`.
- **`ae help`, the compiler's import resolution and ae's binary imports
  take paths of any length (#2543).** `ae help` kept each `--lib`
  directory in 1 KB, so the compile it runs, the library catalog and the
  `*.help.md` hints of a longer one were looked for in a directory nobody
  named. The compiler kept the entry file's directory in 2 KB, and an
  import beside a file with a longer path resolved from the wrong one. ae
  probed for a source or binary import under a `--lib` directory in
  1.2 KB, so a library there was missed and its import left unresolved,
  and it kept the libraries a program links, and their directories, in
  4 KB, dropping those past it. All are kept whole now, as are the
  directories `--package` walks.
- **`ae help` reads the stdlib of the toolchain it runs (#2544).** It
  looked for the stdlib only under the working directory and a few fixed
  prefixes, so with an installed toolchain (`<prefix>/share/aether`) or a
  build run from outside its checkout it had no export catalog, and a
  misspelt or unimported std function got no suggestion. ae now names the
  toolchain's stdlib to it, as it names the compiler. A `--lib` library's
  `*.help.md` hint no longer depends on the stdlib being found.
- **An `$AE_CC` / `$CC` that carries flags works on Windows and with
  `--emit=obj` (#2545).** The value is a command prefix, the program and
  then its flags, as the POSIX build line already used it, but every
  native Windows build, `ae bindgen` there and `ae build --emit=obj` on
  every platform quoted it whole, so `cc -Werror=incompatible-pointer-types`
  named a program nothing could start. Each now quotes the program alone
  and passes the flags after it, and `--emit=obj` on Windows checks the
  compiler the way the other builds do.
- **Ten `--emit=lib` tests and four `aether.toml` tests run on Windows
  (#2541).** The C hosts of `emit_lib_keeps_main`, `emit_lib_kind_safe`,
  `emit_lib_net`, `emit_lib_typed_ptr`, `emit_lib_primitives`,
  `emit_lib_lists` and `manifest` loaded the library with `dlopen` only,
  and the tests skipped Windows; they load it with `LoadLibrary` there now,
  without `-ldl` or an rpath, and `emit_lib_keeps_main` and
  `emit_lib_dual_build` read a DLL's export table with `objdump -p`.
  `emit_lib_with_capability`, `emit_lib_unsupported`, `emit_lib_banned`
  and `emit_lib_dual_build`, and the `toml_extra_sources` multiline,
  long-line and assembly-buffer tests, skipped Windows for no reason and
  pass there.
- **Cache salts, the -D list and the binary-import scan have no length
  limit (#2546).** The -D symbols with `[build] cflags` and `link_flags`
  went into a 4 KB salt and the linked binary libraries into 2900 bytes,
  so two builds differing only past the cut shared one cache entry. Past
  1 KB of -D symbols ae warned, dropped the next one and built a program
  without it. The binary-import scan stopped at 512 files and cut module
  names at 255 bytes, and `ae bindgen consts` cut its preprocessor command
  at 4 KB. Each now holds what it is given; a salt that cannot be built
  means the build is not cached.
