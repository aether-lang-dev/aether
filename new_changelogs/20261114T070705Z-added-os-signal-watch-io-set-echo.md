- **`os.signal_watch` and `io.set_echo`.** A program can survive Ctrl-C and
  `kill -TERM` and wind down cleanly: `os.signal_watch([signal.SIGINT(),
  signal.SIGTERM()])` counts those signals instead of letting them end the
  process, `os.signal_received(w)` gives the number of one delivered since
  the watch last reported it, and `os.signal_unwatch(w)` puts the earlier
  disposition back once the last watch on a signal ends. Each watch keeps
  its own place, so two watches both see a delivery. Windows maps Ctrl-C /
  Ctrl-Break to SIGINT and console close, logoff and shutdown to SIGTERM.
  `io.set_echo(fd, on)` turns a terminal's echo off for reading a secret and
  returns whether it was on, so the caller restores it; a pipe or file is
  left alone (termios `ECHO`; the console's `ENABLE_ECHO_INPUT` on Windows)
  (nib-code ask aether-signal-flag-and-tty-echo).
