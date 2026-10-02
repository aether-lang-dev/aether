- **Rebuilding a contrib archive invalidates the build cache.** The cache key
  hashed `libaether.a` but not the archives `make contrib` puts beside it, so
  after rebuilding `libaether_<module>.a` (or switching contrib.sqlite between
  the system library and the vendored one) `ae build` reported a cache hit
  and handed back a binary linked against the old archive.
- **A static module archive can need libm.** `ae build` dropped `-lm`,
  `-lpthread` and `-ldl` from a module's `@link` because the runtime links
  them already, but earlier on the line than the module's archives, which a
  single-pass linker cannot use for a later static archive. They now follow
  the module archives when a module names them (#1372).
