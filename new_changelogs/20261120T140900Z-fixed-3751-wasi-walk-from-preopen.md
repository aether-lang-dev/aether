- **The checked file calls work on WASI (#3751).** The walk that opens a
  checked path one directory at a time started from `openat(AT_FDCWD, "/")`,
  which wasi-libc resolves only when a preopened directory is "/". It now
  starts from the preopen that holds the path (`__wasilibc_find_abspath`),
  as does the checked `mkdir -p`.
