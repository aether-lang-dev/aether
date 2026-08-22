# std.audit

Reads back the sandbox permission trail.

Aether's in-process containment records every grant check — **allowed as well
as denied** — into a 256-entry ring buffer. This module is the programmatic
face of that: counting denials, asserting an expected access pattern, or
building a "why was this blocked" report.

The buffer is populated **unconditionally**. The live sink
(`AETHER_SANDBOX_AUDIT=stderr|file`) is opt-in and verbose, but the query API
below works with it off.

```aether,run
import std.audit

main() {
    // Outside a sandbox nothing is recorded: the permission check
    // short-circuits before the audit hook.
    println("count: ${audit.count()}")
    println("denied: ${audit.denied_count()}")

    // An out-of-range entry is ("", "", -1) rather than a crash, so a
    // loop that runs past the end gets an unmistakable value.
    cat, res, allowed = audit.entry(0)
    println("entry(0): cat='${cat}' res='${res}' allowed=${allowed}")
}
```
```output
count: 0
denied: 0
entry(0): cat='' res='' allowed=-1
```

Entries appear only while a sandbox context is active. `allowed` is 1 for a
passed check and 0 for a denial; `clear()` scopes the next query to just the
work that follows it.

The strings from `entry` are Aether-owned copies, safe to keep after further
audit activity — the ring buffer's own slots are overwritten as it wraps.

`examples/audit-demo.ae` is a worked example that grants one environment
variable and shows the allowed and denied reads side by side.

## Exports

`count`, `entry`, `clear`, `denied_count`, plus the `aether_audit_*` raw
forms.
