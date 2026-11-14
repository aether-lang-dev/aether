# REPLY: std.os watches signals; std.io turns terminal echo off

**Re:** nib-code `asks/aether-signal-flag-and-tty-echo.md`

**RESOLVED-pending-release** in branch `batch/nov14-naeb-asks-2` (the first
release after 0.914.0 carries it). `sysgap`'s `watch_signals`,
`signal_received` and `stdin_echo` have std replacements.

## 1. SIGINT and SIGTERM as something to poll

```aether
os.signal_watch(signals: int[]) -> (ptr, string)   // (watch, "") or (null, why)
os.signal_received(watch: ptr) -> int              // a watched signal's number, or 0
os.signal_unwatch(watch: ptr)                      // ends the watch, frees it
```

```aether
w, err = os.signal_watch([signal.SIGINT(), signal.SIGTERM()])
defer os.signal_unwatch(w)
...
sig = os.signal_received(w)
if sig == signal.SIGINT() { /* [interrupted by user] */ }
if sig == signal.SIGTERM() { /* [interrupted: process terminated] */ }
```

A watched signal no longer ends the process; each delivery is counted, and
`signal_received` gives the number of one delivered since that watch last
reported it, one per call (SIGINT and SIGTERM both arriving come out on two
calls). `signal_unwatch` is the `signal_reset` you asked for: when the last
watch on a signal ends, the disposition from before the first one (the
default, for nib) is back. SIGKILL and SIGSTOP, and numbers outside 1..64,
refuse the whole watch and install nothing. Interrupted system calls restart
(`SA_RESTART`): the watch is polled, so nothing needs an EINTR to notice it.

Windows has no kill(2) signals for a console program, so the console control
events stand in through `SetConsoleCtrlHandler`: Ctrl-C and Ctrl-Break are
SIGINT; console close, logoff and shutdown are SIGTERM. Other numbers are
refused there. For the SIGTERM events Windows ends the process about five
seconds after the event whatever the program does (the handler holds it
that long), so poll at least that often.

### Why there is process-wide state at all, and how little

Paul's rule is no module-level mutable state as an API, and no keyed global
in place of a single one. A signal is the one place some is unavoidable:
the kernel calls a handler with the signal number and nothing else, no user
pointer, so whatever the handler records has to be reachable from the
number alone. So:

- **The handler's storage** is one delivery counter per signal number. The
  handler only adds one to it (an atomic add, async-signal-safe); nothing
  else writes it. It is not an API: no call reads or resets it.
- **The caller's state is the watch**, an instance you hold and pass: the
  signals it asked for and the count it last saw of each. `signal_received`
  compares and advances only that watch's snapshot. There is no shared
  "last signal" slot to consume, so two parts of a program (nib's Session
  and a background-bash supervisor, say) can each watch SIGINT without one
  swallowing the other's Ctrl-C.
- **Install bookkeeping**: how many watches hold each signal and the
  disposition the first replaced, so the last `signal_unwatch` can restore
  it. Touched only by watch and unwatch, under a lock, never by the handler.

The stand-in's `static volatile sig_atomic_t nibsys_signal` was a single
consumable slot; this is the shape that keeps the per-caller state with the
caller.

## 2. Terminal echo off for a secret

```aether
io.set_echo(fd: int, on: bool) -> (bool, string)   // (echo was on, err)
```

```aether
was, err = io.set_echo(0, false)
defer io.set_echo(0, was)
key, rerr = io.fd_read_line(0)
```

It returns what it found rather than whether it changed anything, so the
restore puts back the terminal's real state (echo already off stays off). A
descriptor that is no terminal is left alone, `(false, "")`, with no error,
so the same code reads a key piped in. `io.is_terminal(0)` says which case
you are in (to print the newline the unechoed Enter did not). POSIX termios
`ECHO`, switched off with `TCSAFLUSH` so typed-ahead input is not read as
the key's start; Windows the console's `ENABLE_ECHO_INPUT`.

A general `tty_mode(fd, raw|cooked|noecho)` for ae/term is not in this
batch; `set_echo` is the part `nib login` needs.

Tests: `tests/regression/test_os_signal_watch.ae` (refusals, two signals
one per call, a second watch seeing the same SIGINT, the first watch still
holding after the second ends), `tests/integration/os_signal_watch` (your
repro: a sleeping program sent SIGINT and SIGTERM from outside prints and
exits 0; after `signal_unwatch` SIGTERM ends it again) and
`tests/integration/io_set_echo` (on a pseudo-terminal echo goes off and back
on, each call reporting the state before it; on a pipe nothing changes).
