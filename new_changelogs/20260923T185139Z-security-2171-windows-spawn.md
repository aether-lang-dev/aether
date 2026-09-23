- **On Windows, `std.os` no longer runs a program from the current
  directory in place of the one on `PATH`, and a batch file can no longer
  be made to run a second command through its arguments (#2171).** Every
  argv-based launch (`run_capture`, `run_full`, `spawn_proc`,
  `run_supervised`, `os_run`, and `run_pipe`'s spawn) passed
  `lpApplicationName = NULL` and let `CreateProcessW` find the program
  itself. That search tries the application's directory and the **current
  directory** before `PATH`, so `os.run_capture("git", ...)` run from a
  directory someone else can write to ran their `.\git.exe`. And a `.bat` or
  `.cmd` it found ran under `cmd.exe`, which reads the command line by its
  own rules. An argument such as `x" & echo INJECTED & "`, quoted correctly
  for the C runtime, ran the second command: the BatBadBut class,
  CVE-2024-24576 in Rust's `std::process`.

  `std.os` now resolves the program the way POSIX `execvp` does. A name with
  a path separator is used as written. A bare name is looked up on `PATH`
  only, with `PATHEXT`'s extensions when it has none. The absolute result is
  passed to `CreateProcessW`, which has nothing left to search. A batch file
  runs through the system `cmd.exe` (`/d /e:ON /v:OFF /s /c`) with each
  argument quoted for cmd, so `a & b` arrives as one argument. An argument
  holding a `"`, a `%` or a line break is refused with
  `argument cannot be passed to a batch file safely`, since cmd acts on
  those even inside quotes. A program found nowhere now reports
  `program not found` instead of `spawn failed`.

  `tests/integration/win_spawn_resolution` plants an `aetool.exe` in the
  working directory next to the real one on `PATH`, and sends a
  quote-injection and a `%PATH%` argument to a batch file. Against the old
  launcher it ran the planted tool, executed `echo INJECTED`, and expanded
  `%PATH%`. It clears `NoDefaultCurrentDirectoryInExePath`, which MSYS2
  shells set and which would otherwise hide the first of those.
