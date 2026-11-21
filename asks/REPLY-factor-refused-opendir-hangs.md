# REPLY: a Factor in-process row hangs when the preload refuses a directory listing

Answers `asks/factor-refused-opendir-hangs-since-0.927.md` in
aether-guest-languages (2026-11-21).

## Verdict

**Factor's own, not the preload's.** The same hang, with the same faulting
instruction and the same destination address, happens with no preload and no
Aether sandbox at all when the kernel refuses the vocabulary roots
(`chmod 000`). It comes from Factor's vocabulary monitor, which the embedded
VM starts and which reports a failure to start through an output stream that
is not usable in the embedded VM. Starting the embedded VM with
`-no-monitors` makes every row pass under the refusal, with no grant on
Factor's tree.

## What the preload does on a refused listing

The question was whether a refusal under the preload is distinguishable from
the kernel's `EACCES`. A probe that lists one directory every common way
(`opendir` and `readdir`, `open` and `openat` with `O_DIRECTORY` then
`fdopendir`, `scandir`, `nftw`, `glob` with and without `GLOB_ERR` and with
`GLOB_NOCHECK`, `open` and `fopen` of an entry) printing each call's result
and `errno` was run against a `chmod 000` directory and under the preload
with grants that do not reach it, on Linux (glibc 2.44) and FreeBSD 15.1, and
the outputs compared byte for byte:

- `opendir`, `open` and `openat` with `O_DIRECTORY`, `scandir` and the opens
  of an entry were identical already: `NULL` or -1, `errno` `EACCES`, set
  last (after the log line), no stream or descriptor handed back. This is
  what Factor's `directory-entries` calls (`opendir`, then `readdir64_r`).
- `nftw` and `glob` were not (a refused root was -1 where the kernel's
  refusal calls the function with `FTW_DNR`; `glob` returned `GLOB_ABORTED`
  without calling the error function, whatever the flags). Those are fixed
  on this branch and the probe is kept as a regression test
  (`tests/integration/sandbox_preload_refusal_errno`). Factor does not use
  either on this path, and the hang is unchanged by the fix.

`log_deny` now also saves and restores `errno` around the log write, though
every refusing path already set `EACCES` after logging.

## The experiment

On cachyos, with the fork at `c467127bc2`, its stock `factor.image` copied
into a directory of its own (Factor's resource path is the image's
directory, so the vocabulary roots are `<dir>/core`, `basis`, `extra`,
`work`), and `factor-host` from aether-guest-languages
(`factor/containment/host.ae`) running `exec-bin-sh` under
`exec=/bin/sh,fork`:

| roots | preload | result |
|---|---|---|
| readable | no | passes |
| readable | yes, roots not granted | hangs (the reported case); the log has `AETHER_DENIED: fs_read <dir>/core`, `basis`, `extra`, `work` |
| `chmod 000` | no | **hangs** |
| `chmod 000` | no, and `GHL_DEBUG=1` (a plain `factor.eval` before any sandbox level) | **hangs** |
| `chmod 000` | yes, `fs_read <dir>/*` granted | hangs |

gdb on the no-preload, no-sandbox case stops on SIGSEGV at the same
instruction as the report, with the same destination:

    => vmovdqu %ymm0,(%rdi)     (libc memcpy, 32 to 64 bytes)
    rdi 0x55e75963ec70   rsi -> "fatal error for monitor root 'resource:core': "   rdx 0x2e

The destination is the same as in the report's own gdb run, and lies in no
mapping of this process (its heap was 0x5555555c3000 to 0x555555cdf000): by
its look a heap address of another process, most likely the one that saved
the image, kept in an object the embedded VM did not set up again.

## The cause, in Factor

`basis/vocabs/refresh/monitor/monitor.factor` has a `STARTUP-HOOK:` (line
69) that, unless `-no-monitors` is on the command line, starts a thread per
vocabulary root running a recursive monitor (`t <monitor>`, which on Linux
lists each directory under the root to watch it). When that fails, line 57
writes `fatal error for monitor root '...': ` and the error to the global
output stream (`with-global`). In the embedded VM that stream's buffer is not
one this process set up, so the copy into it faults, and Factor's SIGSEGV
handler returns to the same instruction for ever. The four directory
listings the preload logs, in root order, are the four monitors starting.

`vm/embed_api.cpp` starts the embedded VM with `argv = { "factor" }`
(`embed_vm_init`, `init_from_args(1, argv)` and
`pass_args_to_factor(1, argv)`). Built with `{ "factor", "-no-monitors" }`
instead (a private build of libfactor, nothing pushed), the reported case
passes, the `open-etc-passwd` row is still refused, the `chmod 000` case
passes, and the preload logs no listing of the roots at all.

Before 0.927.0 the preload did not check `opendir`, so the monitors started
(they need a listing, not a read of any file), which is why 0.926.0 passed.

## Suggested fixes, none of them Aether's

- In the fork: pass `-no-monitors` in `embed_vm_init`. An embedded VM has no
  use for reloading vocabularies when their files change, and it starts a
  thread per root that the host never asked for.
- Separately, the monitor's error report going through a stream that is not
  set up is the deeper bug in the embedded VM: any error written to the
  global streams before the host's eval sets them up dies the same way
  (`factor/README.md` in aether-guest-languages already notes one).
- In aether-guest-languages, once the fork has the first: the grant of
  `$FACTOR_HOME/*` in `factor/containment/host.ae` (`with_tree`) can go,
  unless a row needs to load a vocabulary from source.

Nothing in Aether was changed for this, beyond the probe and the `nftw` and
`glob` fixes it found.
