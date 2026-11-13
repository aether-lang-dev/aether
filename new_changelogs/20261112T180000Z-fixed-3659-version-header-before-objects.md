- **`make test` and `make test-build` work in a fresh checkout (#3659).** The generated
  `runtime/aether_version.h` was a prerequisite of only the few objects that
  named it, while `runtime/aether_abi.h` includes it into every source that
  includes that header. In a checkout where `make compiler` had not run first,
  the test runner's build stopped at `tests/runtime/test_abi_layout.c` with
  "aether_version.h: No such file or directory". Every object rule, the
  sanitizer, memory, debug and coverage flavours included, now builds the
  header first, as it already did the other generated headers.
