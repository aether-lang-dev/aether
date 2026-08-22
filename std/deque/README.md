# std.deque

A fixed-capacity double-ended queue of `long` values.

**`std.deque` is value-semantic, and this is the thing to know before using
it.** `push_back`, `push_front` and `pop_front` return a *new* `Deque` rather
than mutating in place. Ignoring the return value silently discards the
operation:

```aether,fragment
d = deque.new(4)
deque.push_back(d, 1)        // WRONG: the result is thrown away
d = deque.push_back(d, 1)    // right
```

At capacity, `push_back` drops the front element to make room — a bounded ring
buffer, not an error. That makes it a natural fit for a rolling window of the
last N samples.

```aether,run
import std.deque

main() {
    d = deque.new(4)
    d = deque.push_back(d, 1)
    d = deque.push_back(d, 2)
    d = deque.push_front(d, 0)

    println("len=${deque.len(d)}")

    // Peeks return (value, err); err is non-empty only when empty.
    front, ferr = deque.peek_front(d)
    println("front=${front} err='${ferr}'")

    // pop_front returns (value, new_deque, err) — rebind the deque.
    value, rest, perr = deque.pop_front(d)
    println("popped=${value} remaining=${deque.len(rest)}")

    deque.free(rest)
}
```
```output
len=3
front=0 err=''
popped=0 remaining=2
```

## Exports

`new`, `free`, `len`, `cap`, `is_empty`, `is_full`, `push_back`, `push_front`,
`pop_front`, `pop_back`, `peek_front`, `peek_back`, `clear`.
