- **One runtime for a program and its libraries, and binary import on
  Windows.** The toolchain now also builds the runtime as a shared library:
  `aether.dll` with its import library, `libaether.dylib` or `libaether.so`.
  It is installed in `lib/aether/shared/`, beside the static archive.
  `ae build --emit=lib --shared-runtime` links a library against it, and the
  library's catalog records that (schema 1.5). A program that imports such a
  library links the shared runtime too, without being asked. The program and
  its libraries then share one scheduler, one config and one set of panic
  frames, so a panic inside a library reaches the program's `catch` on every
  platform; before, that worked only on Linux and FreeBSD. Binary import
  (`import foo` of a prebuilt `--emit=lib` library) and `ae lib-info` now
  work on Windows. `ae` maps the DLL without running its init code to read
  the catalog, and `ae build` copies the DLLs a program needs next to it,
  since PE has no rpath. A Windows DLL now exports its `aether_lib_meta`
  catalog, which it never did before (PE cannot export a weak definition).
  `@c_callback` hooks such as the pure-TLS client and server reach a shared
  runtime by registering by name at load. The build cache now keys on the
  imported libraries, so a program is rebuilt after one of them changes
  (#2297).
