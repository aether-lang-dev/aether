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
no nested pass. Each notification names the field that changed, for an
observer that wants it (`observe_fields`), and a store the compiler cannot
see is reported with `notify`. Observers live in a side table, so the
struct's layout is unchanged and a program that observes nothing pays one
counter read per store.

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
- One notification per store, per object, naming the field by its index in
  declaration order (0 for the first). At each nesting level the index is
  that object's own field: `a.b.c = v` tells `&a.b` field `c` and `&a` field
  `b`.
- A first field shares its parent's address, so `&a.first` and `&a` name one
  object. Put a nested observable value after another field when the two
  must be told apart.

## Which field changed

A network layer that sends only a component's changed fields, or an undo log
that records the one field a store touched, registers with `observe_fields`;
its closure takes the field index as a second argument. Plain `observe`
observers on the same object keep their one-argument form.

```aether,run
import std.observe

struct Health @observable {
    current: int
    max: int
    regen: float
}

main() {
    h = heap.new(Health)
    dirty = 0
    observe.observe_fields(h, |obj: ptr, field: int| {
        if field == observe.ANY_FIELD {
            dirty = 7                 // unknown: resend all three fields
        } else {
            dirty = dirty | (1 << field)
        }
    })
    h.current = 50
    h.regen = 0.5
    println("dirty mask: ${dirty}")   // fields 0 and 2
    heap.free(h)
}
```
```output
dirty mask: 5
```

## Stores the compiler cannot see

A generic inspector, scene loader or deserializer writes a field by offset
through `std.mem` (`mem.set_int(obj, offset, v)`), knowing it only from a
schema. The compiler does not see that store as a field assignment, so the
writer reports it: `observe.notify_field(obj, field)` when it knows the
field's index, `observe.notify(obj)` when it does not (the observers get
`observe.ANY_FIELD`). Either runs the same pass a compiler-emitted store runs,
re-entrancy guard included.

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
| `observe_fields(obj: ptr, on_change: fn)` | `long` token, `0` for a null object | `on_change` is invoked as `on_change(obj, field)`, `field` an `int` |
| `notify(obj: ptr)` | | runs `obj`'s observers with `ANY_FIELD`; a null `obj` is a no-op |
| `notify_field(obj: ptr, field: int)` | | runs `obj`'s observers for field `field` |
| `unobserve(obj: ptr, token: long)` | `bool` | `false` when the token is not registered on `obj` |
| `unobserve_all(obj: ptr)` | `int` removed | call before freeing `obj` |
| `observer_count(obj: ptr)` | `int` | |
| `is_notifying(obj: ptr)` | `bool` | true while a pass on `obj` runs |

`ANY_FIELD` (`-1`) is the field a notification carries when it names none.

The raw externs (`aether_observe`, `aether_observe_fields`, `aether_unobserve`,
`aether_unobserve_all`, `aether_observer_count`, `aether_observe_is_notifying`,
`aether_observe_notify`, `aether_observe_notify_field`) are the same calls
with C return conventions, for a host that registers observers from C:
`runtime/aether_observe.h` declares them, with the `{fn, env}` closure layout
the callback is stored in.
