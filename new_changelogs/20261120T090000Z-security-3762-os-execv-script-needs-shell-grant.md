- **`os.execv` runs a file with no `#!` line through the shell only with
  `grant_shell()` (#3762).** It called libc's `execvp`, which runs a file the
  kernel refuses with ENOEXEC through `/bin/sh` by itself, after the exec
  grant matched the file alone: the shell then ran whatever the file held.
  `os.execv` now finds the program as `os.run` does and starts it with
  `execv`, and makes the shell fallback itself only when the sandbox lets
  the shell run, the rule #3755 set for `os.run`. Refused, `os.execv`
  returns -1 with errno EACCES. A `#!` script is unchanged, and code trusted
  to run shell scripts keeps running them by adding `grant_shell()`.
