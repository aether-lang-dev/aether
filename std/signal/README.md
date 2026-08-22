# std.signal

POSIX signal-number constants.

Pairs with `std.os`'s process-supervision primitives, so a caller writes
`os.kill(pid, signal.SIGTERM())` instead of hard-coding 15 — or, worse, 9 when
they meant 15.

They are zero-argument **functions** rather than constants because Aether has
no top-level `const`.

```aether,run
import std.signal

main() {
    println("SIGINT:  ${signal.SIGINT()}")
    println("SIGTERM: ${signal.SIGTERM()}")
    println("SIGKILL: ${signal.SIGKILL()}")
}
```
```output
SIGINT:  2
SIGTERM: 15
SIGKILL: 9
```

The set is deliberately limited to the signals whose numbers are **identical
on every POSIX target**. The real-time and job-control signals are not here —
`SIGUSR1` is 10 on Linux and 30 on macOS, and shipping one number for both
would be wrong somewhere. Read those from the platform header on the C side.

The usual shutdown ladder is `SIGTERM`, wait, then `SIGKILL`: the first can be
caught and cleaned up after, the second cannot be caught at all.

## Exports

`SIGHUP`, `SIGINT`, `SIGQUIT`, `SIGILL`, `SIGABRT`, `SIGFPE`, `SIGKILL`,
`SIGSEGV`, `SIGPIPE`, `SIGALRM`, `SIGTERM`.
