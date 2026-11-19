- **A file with no `#!` line needs the shell's exec grant, in-process too
  (#3755).** `os.run` and its siblings run such a file through `/bin/sh`, as
  `execvp` does, after the exec grant matched the file. The shell then ran
  whatever the file held with no grant of its own, where the LD_PRELOAD
  library's `execvp` already asked for one. Both now apply the rule #3735
  set for command strings: without `grant_shell()` the start fails with
  "permission denied". A script with a `#!` line is unchanged, since the
  kernel starts its interpreter, and code trusted to run shell scripts keeps
  running them by adding `grant_shell()`.
