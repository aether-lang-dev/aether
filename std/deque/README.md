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

**When every element matters, use `try_push_back` / `try_push_front` instead.**
They never overwrite: on a full buffer they return an error and leave the deque
untouched. The distinction matters most for BFS frontiers, tree traversals and
work queues, where an underestimated capacity does not fail loudly — it quietly
drops pending work and yields an answer that looks plausible and is wrong.

```aether,run
import std.deque

main() {
    // A work queue must not lose items. try_push_back reports a full
    // buffer instead of dropping the oldest entry.
    d = deque.new(2)
    d, e1 = deque.try_push_back(d, 1)
    d, e2 = deque.try_push_back(d, 2)
    d, e3 = deque.try_push_back(d, 3)

    println("third push err='${e3}'")
    println("len=${deque.len(d)}")

    // 1 and 2 both survived; the overwriting push_back would have
    // evicted 1 to make room for 3.
    a, d, _ = deque.pop_front(d)
    b, d, _ = deque.pop_front(d)
    println("kept ${a} and ${b}")

    deque.free(d)
}
```
```output
third push err='deque: full'
len=2
kept 1 and 2
```

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
