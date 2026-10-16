- **A library calls its own functions, not a host's.** An `--emit=lib`
  library exported each function under its own name too, and on ELF a host
  or another library defining the same name took over the library's calls
  to it: the SWIG round-trip recursed until it overflowed its stack. A
  function exported through its `aether_<name>` alias is now hidden under
  its own name (#2937).
