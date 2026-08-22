# std.capsicum

FreeBSD Capsicum: capability-mode sandboxing.

`enter()` puts the process into capability mode permanently — no path can be
opened afterwards, only descriptors already held can be used, and there is no
way back out. That one-way door is the security property: code after the call
cannot widen its own reach even if it is compromised.

Available on **FreeBSD only**. Every entry point reports that rather than
failing obscurely, so a portable program probes and degrades.

```aether,run
import std.capsicum

main() {
    // 0 on Linux, macOS and Windows; 1 on FreeBSD.
    println("available: ${capsicum.available()}")

    if capsicum.available() == 0 {
        println("not sandboxing on this platform")
        return
    }

    // Beyond this point no new files can be opened.
    rc = capsicum.enter()
    println("entered: ${rc}")
}
```
```output
available: 0
not sandboxing on this platform
```

`rights_limit` narrows what a specific descriptor may do — read but not write,
say — which is how a process keeps an fd it needs while giving up the
operations it does not.

`in_mode()` reports whether capability mode is already active, so library code
can avoid attempting an operation that will certainly fail.

For portable containment, `std.audit` and the in-process permission layer work
everywhere; Capsicum is the kernel-enforced tier where the platform offers one.
See `docs/aether_compared_to_capsicum.md`.

## Exports

`available`, `enter`, `in_mode`, `rights_limit`, and the `CAP_OK`,
`CAP_ERR`, `CAP_UNSUPPORTED` status constants.
