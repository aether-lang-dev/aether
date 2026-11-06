- **A dlopen the sandbox preload refuses leaves a `dlerror()` message.** The
  refusal of libc or the loader without a `native` grant returned NULL with
  `errno = EACCES` and nothing for `dlerror()`, which dlopen's contract
  promises: Tcl 8.6's `load` formats that message with `%s` and segfaulted,
  and Ruby's Fiddle reported a NULL handle with no reason. The refusal now
  sets a per-thread message naming the library and the sandbox
  (`libc.so.6: refused by the sandbox: ...`), returned once and then cleared,
  and a later loader error is never shadowed by it.
