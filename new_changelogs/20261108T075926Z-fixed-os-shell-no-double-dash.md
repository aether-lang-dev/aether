- **`os.system` and `os.exec` work on FreeBSD again.** Both ran `sh -c -- CMD`.
  dash and bash read the `--` as the end of sh's options, but FreeBSD's
  `/bin/sh` takes it as the command string, so every `os.system` there
  returned 127 (`--: not found`) and every `os.exec` came back empty; a stock
  aeb stopped at its first helper build. They run `sh -c CMD` now, and a
  command that starts with `-` or `+` gets a leading space instead, so it is
  still the command and not sh options on every shell. Test:
  `tests/regression/test_os_shell_command_string.ae`.
