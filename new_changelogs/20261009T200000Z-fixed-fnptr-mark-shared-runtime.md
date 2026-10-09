- **A program that calls a string function through a typed fn pointer links
  against the shared runtime on Windows again.** Since 0.799.0 the generated
  helpers that decide whether such a call's result is owned (#2586) read and
  wrote the runtime's thread-local ownership mark directly, and Windows
  cannot import a thread-local from a DLL: the link failed with an undefined
  reference to `g_aether_fnptr_owned`, or ld crashed on larger programs.
  ae3d, which links its programs and the scripts they load to one runtime
  DLL, could not link most of its suites. The mark is now private to the
  runtime and reached through `aether_fnptr_mark` and `aether_fnptr_claim`,
  as the runtime's other thread-locals are (#2687).
