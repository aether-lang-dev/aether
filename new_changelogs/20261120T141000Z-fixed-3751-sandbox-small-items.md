- **Smaller sandbox fixes (#3751).** Under the preload `lsetxattr` and
  `lremovexattr` act on the name opened `O_PATH|O_NOFOLLOW` (through
  `/proc/self/fd/N`) instead of by its resolved name; the chmod fallback for
  glibc before 2.32 opens with `O_NONBLOCK|O_NOCTTY`, so a FIFO there no
  longer blocks it; and the shell `os.execv` falls back to, which `os.run`
  and `grant_shell` name too, is `_PATH_BSHELL` where the system defines it.
