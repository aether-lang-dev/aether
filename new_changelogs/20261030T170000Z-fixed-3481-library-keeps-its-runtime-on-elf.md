- **A library with a runtime of its own keeps it on Linux and FreeBSD
  (#3481).** An `ae build` program is linked `-rdynamic`, so it exports
  its runtime, and on ELF a library's calls into the runtime it carries
  bound to the program's instead: its code ran on a runtime it was not
  made for. Across versions its runtime ABI check refused, and the program
  ended on the panic ("the library was made for Aether runtime 0.860.0/abi1
  and runs on 0.863.0/abi1"), which kept the cross-version ABI matrix red
  on Linux since 0.861.0. `ae` now links such a library `-Bsymbolic`,
  natively and in cross builds, as macOS and Windows keep the copies apart
  by themselves, so on every platform a static-runtime library runs on its
  own runtime: `ae` refuses an import that needs one runtime (a struct
  holding strings, a builder using its config) and warns that a library's
  actors are not drained at exit, naming `--shared-runtime`, as it did on
  macOS and Windows. Such a library names the `own_runtime` duty in its
  catalog, so an importer of 0.864.0 or before, which took ELF's copies as
  merged, refuses it instead of letting the two runtimes free each other's
  values. A library of another runtime ABI linked before this is refused
  at import on ELF, naming the rebuild, instead of ending the program. A
  failure inside a library still reaches a `try` around the call: the
  import stub raises it again (#2956). This replaces 0.864.0's note that
  the matrix expects ELF's merged runtimes.
