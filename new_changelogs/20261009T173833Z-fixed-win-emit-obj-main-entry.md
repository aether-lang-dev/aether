- **An `--emit=obj` object links into a program on Windows with no C `main()`
  of its own.** The object's weak `main()` (#2511) is a COFF weak external,
  and GNU ld up to 2.46 still searched the archives for `main`: it pulled
  `libmingw32.a`'s `crtexewin.o`, whose `main()` calls `WinMain`, and
  `cc app.o $(ae cflags --libs)` failed with `undefined reference to
  'WinMain'` (GitHub's runners, on binutils 2.47 and a newer mingw-w64 crt,
  happened to link it, so CI stayed green). On Windows
  the object now carries no `main()`; the entry is the one member of the new
  `libaether_main.a` beside `libaether.a`, which `ae cflags --libs` names
  first (`-laether_main -laether`). An archive member is linked only when the
  program still needs `main`, so a host's own `main()` wins as before with
  the same command line. A host entering through `WinMain` names
  `-lmingw32` ahead of it (docs/emit-lib.md). The shared runtime and every
  DLL `ae` links still define no `main`.
