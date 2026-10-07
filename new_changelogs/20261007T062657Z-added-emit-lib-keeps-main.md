- **`--emit=lib` keeps a program's `main()` callable.** A library built from
  a `.ae` that defines `main()` used to drop its body, so an app that *is* a
  library (an Android app, loaded by its activity) had nothing to run. It now
  exports `int aether_main(int argc, char** argv)`, which runs the
  executable's prologue (args, sandbox, scheduler) and `main()`'s body and
  returns its result with the actors still running, and
  `void aether_main_exit(void)`, the executable's epilogue that drains and
  joins the scheduler. An executable's `main` is emitted from the same code,
  unchanged. A second `aether_main` before `aether_main_exit` is rejected;
  `aether_main_exit` is idempotent; the program can run again after it. A
  top-level `main_exit` beside `main()`, which would export as
  `aether_main_exit`, is a compile error under `--emit=lib`. A scheduler
  started again after `scheduler_shutdown()` now frees the previous run's
  per-core tables instead of leaking them. See docs/emit-lib.md
  (aether-ui `asks/aether-emit-lib-keeps-main.md`).
