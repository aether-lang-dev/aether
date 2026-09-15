# os.spawn_proc: the child inherits the parent's BLOCKED signal mask

> **STATUS: FIXED (2026-09-15)** by `8335661f` (PR #2038) —
> `aether_child_reset_signals()` now runs in the forked child before exec in
> all seven POSIX spawn paths: empty mask via `sigprocmask(SIG_SETMASK)`, and
> any inherited `SIG_IGN` reset to `SIG_DFL`.
>
> Verified from selaenium on the exact path the engine uses. A C harness that
> blocks SIGTERM/INT/QUIT in the parent and then drives `os_spawn_raw`
> (`os.spawn_proc`, what `selenium_core/driver.ae` launches chromedriver with)
> reports the child's `SigBlk: 0000000000000000`, where it was `0x4206` before.
> Upstream's own `tests/integration/spawn_signal_mask_reset` also passes.

Found porting Selenium (selaenium): a driver process spawned from inside a
managed runtime could not be killed, and the reap blocked forever.

## Observed

`selenium_core` spawns `chromedriver` with `os.spawn_proc` and later tears it
down with `os.kill(token, 15)` + `os.wait(token)`. From the Julia binding, the
teardown hung indefinitely — `wait4` never returned, the driver stayed alive,
and the whole test process wedged (Julia 1.12.7, Linux 7.2.3 / CachyOS).

The same engine, same driver binary, from every other binding: fine.

The difference is the parent's signal mask. `/proc/<pid>/status` of the spawned
chromedriver:

    spawned from the Julia binding:   SigBlk: 0000000000004206
    spawned from a shell:             SigBlk: 0000000000000000

`0x4206` has bit 14 set — **SIGTERM is blocked** in the child (also SIGINT,
SIGQUIT, SIGUSR1). Julia blocks those in its threads; `fork()` copies the
calling thread's mask, and `execve()` preserves it. So `kill(pid, SIGTERM)` is
delivered but never acted on, the child never exits, and an unbounded
`os.wait()` on it blocks for good.

Any embedder that masks signals hits this — Julia today, and the JVM, .NET and
Ruby runtimes all mask signals to varying degrees. It is not specific to
Selenium: any Aether program used as a library inside such a runtime will spawn
children that silently ignore the signals its own `os.kill` sends.

## Cause

`os_spawn_raw` (std/os/aether_os.c) does not reset the child's signal state
between `fork()` and `execve()`. A spawn API owes the child a clean slate:
POSIX says the mask is inherited across both calls, so the child of a
signal-masking parent starts life deaf to the signals the parent will later
send it.

## Suggested fix

In the forked child, before `execve()`:

- restore the full signal mask — `sigemptyset(&set); sigprocmask(SIG_SETMASK, &set, NULL);`
- reset dispositions the parent may have set to `SIG_IGN` back to `SIG_DFL`
  (ignored dispositions are also inherited across `exec`, unlike handlers).

This is what `posix_spawn` does with `POSIX_SPAWN_SETSIGMASK` /
`POSIX_SPAWN_SETSIGDEF`, and what Python's `subprocess` (`restore_signals=True`)
and Go's `os/exec` do by default. Callers who genuinely want the inherited mask
are the rare case and can be given an opt-in later.

## Workaround in place

`selenium_core/driver.ae` now escalates: SIGTERM, a bounded
`os.wait_any_timeout(…, 5)`, then SIGKILL (which cannot be blocked or ignored)
and a final reap. That is correct supervisor behaviour regardless — a wedged
driver may decline SIGTERM for its own reasons — so it stays either way, but it
should not be the thing that makes signals work at all.
