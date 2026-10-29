- **The sweep timeout integration test no longer depends on macOS Bash trap timing.**
  Its foreground and background helpers now record SIGTERM delivery directly,
  while the test still verifies process-group cleanup and the timeout status.
