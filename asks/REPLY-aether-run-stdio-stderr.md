# REPLY: os.run_stdio with the child's stderr discarded (or to a file), and a cwd

**Re:** nib-code `asks/aether-run-stdio-stderr.md` (branch `aether-port`),
and the "no cwd" note in `asks/aether-fs-stat-flock-exclusive.md`

**RESOLVED** in PR #3694 (branch `batch/nov13-naeb-asks`; the first
release after 0.913.0 carries it).

`os.run_stdio` keeps its shape and gains three optional parameters, given by
name, in the style of the other run_* calls (int choices, a path, no option
map):

```aether
to, from, pid, err = os.run_stdio(prog, argv, env,
    stderr_to: os.STDERR_DISCARD)                       // /dev/null, NUL
to, from, pid, err = os.run_stdio(prog, argv, env,
    stderr_to: os.STDERR_FILE, stderr_path: "/tmp/child.err")
to, from, pid, err = os.run_stdio(prog, argv, env, cwd: workdir)
```

- `stderr_to`: `os.STDERR_INHERIT` (0, the default: this process's stderr),
  `os.STDERR_DISCARD`, or `os.STDERR_FILE` with `stderr_path` (created or
  truncated, relative to this process's directory). The file or the null
  device is opened by the parent, so a failure to open it is this call's
  error, with nothing spawned. Any other value, or STDERR_FILE with no path,
  is refused before anything is spawned.
- `cwd`: "" (the default) runs the child in this process's directory;
  otherwise in `cwd`. `prog` is still found as without it (PATH, or relative
  to this process's directory). A directory the child cannot enter fails
  the spawn with "os.run_stdio: cannot enter the working directory: ...",
  with nothing to reap, as a program that cannot start does.

POSIX: chdir and the stderr descriptor in the child between fork and exec
(the chdir failure goes up the exec-error pipe tagged apart from an exec
failure). Windows: the null device or file as the child's `hStdError`
(inherited alone with the two pipe ends), and `lpCurrentDirectory`.

`tests/integration/os_run_stdio` covers all three choices and both cwd
cases, from a "middle" process whose stderr the test captures: with
STDERR_DISCARD, the middle's reader finishes when the middle exits although
the child it left running still runs, which is the "killed parent" case the
ask describes. nib can replace the `/bin/sh -c 'exec "$@" 2>/dev/null'`
(and `cd "$1" && ...`) wrappers in `nib.tools.process` with
`stderr_to: os.STDERR_DISCARD` and `cwd:` on the release carrying this.
