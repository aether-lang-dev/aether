- **`os.run_stdio` takes the child's stderr and working directory.** Optional
  named arguments: `stderr_to: os.STDERR_DISCARD` sends the child's stderr
  nowhere (`/dev/null`, `NUL`), `stderr_to: os.STDERR_FILE, stderr_path: p`
  to a file (created or truncated), and `cwd: dir` runs it there; the default
  is still this process's stderr and directory. A child that holds this
  process's stderr keeps whoever reads it waiting after this process has gone,
  which a child run unattended need not. A directory the child cannot enter
  fails the spawn. POSIX and Windows (`lpCurrentDirectory`) alike (nib-code
  ask run-stdio-stderr).
