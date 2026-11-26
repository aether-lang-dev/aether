- **`os.which` and `os.getcwd` are checked against the sandbox.** `os.which`
  checked only `env PATH` and then looked at every candidate on it
  unchecked, so a sandboxed caller with no file-system grant could learn
  where any program was installed; `os.getcwd` checked nothing. Each
  candidate `os.which` looks at is now an `fs_read` of its path (a refused
  one reads as absent and the search goes on), and `os.getcwd` needs
  `fs_read` on the working directory (refused, it gives ""), as
  `fs.exists` and `fs.realpath` do. A sandbox that runs a program found
  with `os.which` now grants `fs_read` on its directory as well.
