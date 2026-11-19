- **A command string needs an exec grant for the shell, in-process too
  (#3735).** `os.system` and `os.exec` matched the exec grant against the
  whole command string, then ran it through `/bin/sh -c`. A grant such as
  `grant_exec("echo *")` matches any string that starts with `echo `, so
  `"echo hi; rm -rf ~"` passed the check and the shell ran all of it. Both
  now also need an exec grant for the shell, the rule the LD_PRELOAD library
  already applied to `system()` and `popen()`: `/bin/sh`, or on Windows the
  program `COMSPEC` names (`cmd.exe`). The new `grant_shell()` gives it; code
  that held only `grant_exec("echo *")` is now refused, and its correct forms
  are `grant_exec("echo")` with `os.run_capture` and an argv (no shell), or
  `grant_shell()` for code trusted to run any command.
  docs/containment-sandbox.md no longer presents `grant_exec("echo *")` as
  the safe pattern for an untrusted plugin.
