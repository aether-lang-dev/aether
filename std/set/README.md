# std.set

A hash set of strings: membership without duplicates.

Adding a value that is already present is a no-op rather than an error, which
is what makes a set the right shape for de-duplication — feed it everything and
ask afterwards.

```aether,run
import std.set

main() {
    seen = set.new()

    set.add(seen, "aether")
    set.add(seen, "rust")
    set.add(seen, "aether")   // already there: no-op, not an error

    println("size=${set.size(seen)}")
    println("has aether: ${set.contains(seen, "aether")}")
    println("has go:     ${set.contains(seen, "go")}")

    set.remove(seen, "rust")
    println("after remove: ${set.size(seen)}")

    set.free(seen)
}
```
```output
size=2
has aether: true
has go:     false
after remove: 1
```

`contains` returns a `bool`, so it reads directly in a condition — no
comparison against 1 needed.

## Reading the members

`items` returns a snapshot of the members; read it with `items_size` and
`items_get`. Same contract as `map.keys`: the strings are **borrowed** and
valid until `items_free`, the order is **unspecified** (sort with
`std.sort.strings` for a deterministic listing), and an out-of-range index
returns `""` rather than trapping.

## `add` vs `try_add`

`add` returns `false` both when the item was **already present** and when the
insert **failed**, so a caller cannot tell "this was a duplicate" from "this
was never stored". For a dedupe filter that is fine — a duplicate is the
expected case and there is nothing to do about it. For anything that must know
its data landed, the two are not the same outcome.

`try_add` reports all three, using information the layer underneath already
has:

```aether,run
import std.set

main() {
    s = set.new()

    added, err = set.try_add(s, "alpha")
    println("first:     added=${added} err='${err}'")

    added, err = set.try_add(s, "alpha")
    println("duplicate: added=${added} err='${err}'")

    added, err = set.try_add(null, "alpha")
    println("failure:   added=${added} err='${err}'")

    set.free(s)
}
```
```output
first:     added=true err=''
duplicate: added=false err=''
failure:   added=false err='set: insert failed'
```

A duplicate is `(false, "")` — not an error. Only a genuine failure (a null
set, or an allocation that did not succeed) carries a message.

## Exports

`new`, `add`, `try_add`, `contains`, `remove`, `size`, `clear`, `free`, `items`,
`items_size`, `items_get`,
plus the `aether_set_*` raw forms.
