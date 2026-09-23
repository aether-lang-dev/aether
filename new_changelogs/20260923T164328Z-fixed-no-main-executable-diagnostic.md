- **Building an executable from a file with no `main()` says so, instead of
  failing in the linker.** `ae build lib.ae` on a library-shaped source
  compiled cleanly and then died with `undefined reference to WinMain`
  (MinGW) or `undefined symbol: main` (ld.lld) — a message about the C
  runtime that names nothing the user wrote. It now stops before the link:

  ```
  Error: lib.ae has no main(), so there is no executable to build.
         To build it as a library, use --emit=lib (or --emit=obj for an
         object file). --emit=both builds an executable as well, so it
         needs a main() too.
  ```

  The cross path used to reject `--emit=both` outright partly to avoid this
  exact linker error — which hid it for one mode on one path while every
  native build still produced it. The check lives in the driver rather than
  the compiler, because the compiler is right to accept such a file:
  libraries, objects, emitted C, documentation blocks whose `main` lives in
  a host, and every test that runs `aetherc x.ae out.c` to inspect codegen
  all legitimately have none. codegen reports the fact the way it reports
  link, source and include requirements — a `// aether-entry: main` line in
  the generated file's header — and only an executable link reads it.
