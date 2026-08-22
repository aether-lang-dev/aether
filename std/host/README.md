# std.host

Primitives for Aether scripts **embedded in a host application**.

The inversion is the point: normally Aether is the program. Here it is a
script compiled with `aetherc --emit=lib` and loaded by a host — a Java, Python,
Ruby or Go application — which calls into it and receives events back.

`notify()` is the upward path, and it deliberately carries almost nothing: an
event name and an `int64` id. That is Hohpe's **claim check** pattern. If the
host wants detail it calls back down through the script's typed exports,
rather than the script pushing a payload up and both sides having to agree on
a serialisation.

The example **compiles but is not run** in CI: a script needs a host process
to load it, and there is none in the test harness.

```aether
import std.host

// A typed downcall the host can invoke by name.
fn compute(x: int) -> int {
    return x * 2
}

main() {
    // Announce something happened. The host registered a handler for
    // this event name via aether_event_register() on the C side.
    host.notify("ready", 0)
}
```

`describe` publishes what the script offers so a host can discover it rather
than being told out of band. `input` and `event` are the downward paths;
`bindings` reports what the host has wired up.

`caller_identity`, `caller_attribute` and `caller_deadline_ms` expose who is
calling and how long they will wait — the context a shared script needs to
make a policy decision, and the reason it is here rather than passed as an
argument to every function.

The `java`, `python`, `ruby` and `go` helpers are the language-specific entry
points; `contrib/host/<lang>/` carries the bridge for each, and
`docs/aether-embedded-in-host-applications.md` is the full account.

## Exports

`notify`, `describe`, `input`, `event`, `bindings`, `java`, `python`, `ruby`,
`go`, `manifest_get`, `manifest_clear`, `caller_identity`,
`caller_attribute`, `caller_deadline_ms`.
