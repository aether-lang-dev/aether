- **The 37 modules the stdlib reference only indexed now have worked,
  verified sections, and there is one stdlib reference instead of two
  (#1523).** Each section says what the module is for and the thing about it
  that is easy to get wrong, gives an example, and lists the functions; this
  change adds the last 22 (`std.mem`, `alloc`, `tracking`, `snapshot`,
  `ipc`, `signal`, `dl`, `ulid`, `ksuid`, `tsid`, `nanoid`, `plural`,
  `message`, `language`, `clapae`, `http1`, `audio`, `audit`, `capsicum`,
  `casper`, `host`, `longarr`). None of the examples is written from memory:
  `make check-docs` compiles every one and runs most, comparing what they
  print with the output shown beside them. The ones that cannot run
  everywhere (`std.ipc` is POSIX-only, `std.capsicum` and `std.casper`
  FreeBSD-only) are compiled, and `std.host`, a library with no `main()`, is
  built as one.

  `docs/stdlib-api.md` described the same modules as the reference in
  different words, which is how errors survived in both. Its process-spawn
  section, the only one with no counterpart, was also wrong. Its example put
  the program name in `argv`, so it ran `git git rev-parse HEAD`. It
  documented an `os.run` that is not exported. And it said Windows used POSIX
  shims where it has used `CreateProcessW` for some time. The reference now
  has a *Running programs* section written from `std.os`'s own contracts,
  covering the whole surface: `run_capture`, `run_full`, `spawn_proc` with
  `wait` / `wait_any` / `wait_any_timeout`, `kill`, `wait_pid_timeout`,
  `run_supervised`, `chdir` / `getcwd` and `os_which`. It also says plainly
  what skipping the shell does not buy on Windows: `CreateProcessW` searches
  the current directory before `PATH`, and a batch file re-parses its
  arguments through `cmd.exe`. Its example re-runs itself as the child, so it
  runs on every OS. `stdlib-api.md` is now a one-page map from a task to the
  reference section that covers it.

  More corrections fell out of the checking:
  - The reference's *Other structured-data formats* said YAML, CSV,
    MessagePack and CBOR had no support, directly above the MessagePack
    section.
  - `std.mem`'s `ptr_to_long`, `long_to_ptr` and `call_fn2_void` were missing
    from its export list, so the index undercounted the module and its own
    guide called them unofficial.
  - `make check-docs`' index fixer rewrote every line of the reference with
    CRLF endings on Windows to change one count. It now keeps the file's own
    line endings.
