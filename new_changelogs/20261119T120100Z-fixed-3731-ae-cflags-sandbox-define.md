- **`ae cflags --cflags` prints `-DAETHER_HAS_SANDBOX`, the define
  `libaether.a` is always built with.** Without it `runtime/aether_sandbox.c`
  compiles to nothing, and `aether_sandbox.h`'s check is a macro that allows
  everything, with no link error to say so (docs/install-layout.md). A build
  that compiles the runtime from the MANIFEST beside the archive's std got a
  runtime with no sandbox. The weekly cross-language benchmarks are such a
  build, and their Aether entry stopped linking on
  `aether_sandbox_check_slow`, which the archive's `std.os` calls (#3731).
