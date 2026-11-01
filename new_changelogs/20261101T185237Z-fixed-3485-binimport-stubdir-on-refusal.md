- **A refused binary import no longer leaves its stub directory behind**
  (#3485). `ae build` and `ae run` write an import's stub under the temp dir
  (`ae-binimport-*`), and a refusal (`ae_lib_check_compat`, or a value that
  needs one runtime crossing two) called `exit(1)` with the stub still open.
  `exit` runs the atexit removal before it closes open files, and Windows
  cannot delete an open file, so every refused build left a directory in
  `%TEMP%`. The refusal now returns to the caller, which closes the stub,
  removes the directory and exits; the messages are unchanged.
