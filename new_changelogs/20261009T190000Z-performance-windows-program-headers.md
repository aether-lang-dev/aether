- **A program's cold build on Windows takes about a third less time: its C
  no longer includes `windows.h`.** mingw-w64's `winnt.h` includes `<x86intrin.h>`,
  and with it every AVX-512 and AVX10 header GCC ships, so every program's
  translation unit carried about 80,000 lines of headers (91,735 for a
  1,027-line program), most of the time a build spent compiling it. The few
  Windows services the generated C used (console setup, the monotonic clock,
  the preemption yield, starting an actor's thread) are now runtime
  functions, and a program gets the thread types the actor runtime's
  structs carry without the Win32 calls behind them. Measured on Windows 11
  with GCC 16.2: the same programs preprocess to about 9,500 lines, the
  front end goes from 380 ms to 73 to 88 ms, and a cold `ae build` from 830
  to 950 ms to 500 to 610 ms. Actor programs on x86 Linux and macOS lose
  about 35,000 lines as well: the runtime's profiling header included
  `<x86intrin.h>` for a counter read that only `AETHER_PROFILE` builds use.
  The Windows clock also stops going through a `double`, which dropped
  nanoseconds once the counter was large (#2673).
- **The CI's Windows checks take less time and fewer runners (#2673).** The
  MINGW64 suite ran twice on every PR, in ci.yml and in windows.yml; it now
  runs once, in ci.yml, beside UCRT64 in windows.yml. The contrib series,
  which was the last ten minutes of ci.yml's Windows job after its
  half-hour `make ci`, runs as a job of its own beside it. And the sweep
  ends with its timings: build and run time summed over the tests and the
  slowest of each kind, so the next per-test regression shows in the log of
  the run that caused it.
