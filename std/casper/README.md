# std.casper

FreeBSD Casper: services that survive entering capability mode.

Capsicum's capability mode forbids opening anything by name — which also
forbids DNS resolution and password-database lookups, since both open files.
Casper is the answer: a helper process, forked **before** the sandbox closes,
that performs those lookups on the sandboxed process's behalf over an existing
descriptor.

FreeBSD only; `available()` reports 0 elsewhere.

```aether,run
import std.casper

main() {
    println("available: ${casper.available()}")

    if casper.available() == 0 {
        println("no casper on this platform")
        return
    }

    // init BEFORE entering capability mode — afterwards is too late.
    ok = casper.init()
    println("init: ${ok}")
}
```
```output
available: 0
no casper on this platform
```

**Order matters and is not recoverable.** `init` and each `service` must be
opened before `capsicum.enter()`. A service not opened beforehand cannot be
opened after, because opening it is exactly the operation capability mode
forbids.

`dns_resolve` and the `pwd_*` lookups are the services worth having: a network
daemon that resolves names and drops to a user account needs both, and both
would otherwise be impossible post-sandbox.

## Exports

`available`, `init`, `service`, `close`, `dns_resolve`, `pwd_uid`,
`pwd_home`, `sysctl_str`, `resolve`.
