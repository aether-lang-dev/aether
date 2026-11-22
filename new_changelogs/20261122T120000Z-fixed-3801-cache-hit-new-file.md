- **A cache hit writes the program as a new file, so macOS no longer kills
  its next run.** `ae build` copied the cached binary over the previous
  one in place: the program that had run kept its inode with new bytes,
  and macOS killed the next run of it (`Killed: 9`) though its bytes and
  signature were fine. Every file ae writes from another (a cached binary,
  a shared library, a DLL beside a program, a package's files) now goes to
  a temporary beside it, renamed over it (#3801).
