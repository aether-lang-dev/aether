# std.observe

Observable struct models: run closures after every field store on a value of
a `struct T @observable`.

SwiftUI's `@Observable` lets a view re-render when a field of the model it
shows changes. Without it, an aether-ui app kept `refresh_list` /
`refresh_crumbs` methods and dirty flags in step with each store by hand,
about a third of its app layer and a steady source of bugs. With it, the
toolkit binds once (`ui.bind(model, |m| { ... })`) and the language reports
the stores.

The attribute is the language half: `struct Model @observable { ... }` makes
the compiler follow every field store on a `Model` with a runtime call
carrying the object's address (see `docs/language-reference.md`,
"`@observable` notify on every field store"). This module is the runtime
half: `observe` registers a closure on an object, `unobserve` and
`unobserve_all` remove it. The object is its address: a `heap.new` box as it
is, a local value as `&m`. Observers run synchronously on the storing thread,
in registration order, with the observed address as their one argument; a
store an observer makes on the object it is being told about lands but starts
no nested pass. Observers live in a side table, so the struct's layout is
unchanged and a program that observes nothing pays one counter read per store.

```aether,run
import std.observe

struct Model @observable {
    count: int
    status: string
}

main() {
    m = heap.new(Model)
    m.status = "idle"                 // nothing registered yet: silent

    token = observe.observe(m, |obj: ptr| {
        println("render: count=${m.count} status=${m.status}")
    })
    m.count = 1
    m.status = "busy"
    m.count += 1

    println("observers: ${observe.observer_count(m)}")
    observe.unobserve(m, token)
    m.count = 99                      // silent again
    println("observers: ${observe.observer_count(m)}")
    heap.free(m)
}
```
```output
render: count=1 status=idle
render: count=1 status=busy
render: count=2 status=busy
observers: 1
observers: 0
```

## What is notified

- `m.count = 1` on a local `Model` value notifies `&m`; `p.count = 1` on a
  `*Model` notifies `p`. The compound forms (`m.count += 1`) are stores too.
- `a.b.c = v` with `b: Inner` notifies `&a.b` then `&a`, each when its struct
  is observable: both values' bytes changed. `a.p.c = v` with `p: *Inner`
  notifies only the pointee.
- One notification per store, per object. There is no field name in the
  call; per-field observation is deferred until a caller needs it.
- A first field shares its parent's address, so `&a.first` and `&a` name one
  object. Put a nested observable value after another field when the two
  must be told apart.

## Lifetime

An observer holds its closure environment until `unobserve` / `unobserve_all`
releases it. Removing an observer from inside its own callback is safe: the
release waits for the pass to end. Remove observers before freeing an object;
otherwise a later value at the same address inherits them.

Stores made on another thread notify on that thread. Marshal to the loop
thread in the observer when the work belongs there (`std.worker`'s poster is
the tool for that).

## API

| Call | Returns | Notes |
|---|---|---|
| `observe(obj: ptr, on_change: fn)` | `long` token, `0` for a null object | `on_change` is invoked as `on_change(obj)` |
| `unobserve(obj: ptr, token: long)` | `bool` | `false` when the token is not registered on `obj` |
| `unobserve_all(obj: ptr)` | `int` removed | call before freeing `obj` |
| `observer_count(obj: ptr)` | `int` | |
| `is_notifying(obj: ptr)` | `bool` | true while a pass on `obj` runs |

The raw externs (`aether_observe`, `aether_unobserve`, `aether_unobserve_all`,
`aether_observer_count`, `aether_observe_is_notifying`) are the same calls
with C return conventions, for a host that registers observers from C:
`runtime/aether_observe.h` declares them, with the `{fn, env}` closure layout
the callback is stored in.
