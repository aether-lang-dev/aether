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

## Exports

`new`, `add`, `contains`, `remove`, `size`, `clear`, `free`, `items`,
plus the `aether_set_*` raw forms.
