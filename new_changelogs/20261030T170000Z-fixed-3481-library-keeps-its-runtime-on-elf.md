- **A library with a runtime of its own keeps it on Linux and FreeBSD
  (#3481).** An `ae build` program is linked `-rdynamic`, so it exports
  its runtime, and on ELF a library's calls into the runtime it carries
  bound to the program's instead: its code ran on a runtime it was not
  made for. Across versions its runtime ABI check refused, and the program
  ended on the panic ("the library was made for Aether runtime 0.860.0/abi1
  and runs on 0.863.0/abi1"), which kept the cross-version ABI matrix red
  on Linux since 0.861.0. `ae` now links such a library `-Bsymbolic`, as
  macOS and Windows keep the copies apart by themselves, so on every
  platform a static-runtime library runs on its own runtime: `ae` refuses
  an import that needs one runtime (a struct holding strings, a builder
  using its config) and warns that a library's actors are not drained at
  exit, naming `--shared-runtime`, as it did on macOS and Windows. A
  library of another runtime ABI linked before this is refused at import
  on ELF, naming the rebuild, instead of ending the program.
