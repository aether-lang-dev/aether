# std.actors

A process-global registry mapping names to actor references.

Models BEAM's `erlang:register` / `erlang:whereis`. The problem it solves is
reach: an actor spawned at startup needs to be addressable from a callback
that has nowhere to carry an `actor_ref` — a `@c_callback` handler, a
signal-driven path, a plugin entry point. Registering it under a name means
any of those can find it.

```aether,run
import std.actors

message Bump {}

actor Counter {
    state count = 0
    receive { Bump() -> { count = count + 1 } }
}

main() {
    actors.registry_clear()

    counter = spawn(Counter())
    actors.register("counter", counter)

    println("registered: ${actors.is_registered("counter")}")
    println("size:       ${actors.registry_size()}")

    // whereis returns the ref, usable for sending.
    found = actors.whereis("counter")
    found ! Bump {}

    println("unregister: ${actors.unregister("counter")}")
    println("again:      ${actors.unregister("counter")}")
}
```
```output
registered: 1
size:       1
unregister: 1
again:      0
```

Registering a name that is already bound **overwrites** without error —
last writer wins, and the map does not grow. `unregister` returns 1 if it
removed a binding and 0 if the name was not bound, which is the difference
between "I cleaned up" and "someone else already did".

`whereis` on an unbound name returns null, so check before sending.

## Exports

`register`, `whereis`, `unregister`, `is_registered`, `registry_size`,
`registry_clear`.
