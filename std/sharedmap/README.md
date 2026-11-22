# std.sharedmap

The shared map from Aether: a token-guarded map of strings that a host fills
with a hosted language's inputs and reads its outputs back from. A guest
bridge reaches it through the token (`aether_map_get` / `aether_map_put` in
the guest), and a sandboxed child through a POSIX shared memory block named by
`AETHER_MAP_SHM`. The C side, which the bridges call, is
`runtime/aether_shared_map.h`.

```aether
import std.sharedmap

main() {
    m = sharedmap.new()
    _p = sharedmap.put(m, "user", "Alice")
    sharedmap.freeze(m)                 // inputs: read-only to a guest
    // hand sharedmap.token(m) to a *_with_map runner, run the guest ...
    v, found = sharedmap.get(m, "reply")
    if found == 1 { println(v) }
    sharedmap.free(m)
}
```

| call | what |
|---|---|
| `new() -> ptr` | a new map, the caller's |
| `token(m) -> long` | the token a hosted language reaches it through; dead once the map is freed |
| `put(m, key, value) -> int` | 0, or -1 refused (a new key in a full map, no memory) |
| `get(m, key) -> (string, int)` | `(value, 1)`, or `("", 0)` when absent |
| `has(m, key) -> int` | 1 or 0 |
| `count(m)`, `key_at(m, i)`, `value_at(m, i)` | the entries, in the order keys were first put |
| `freeze(m)` | every key so far becomes an input a guest cannot overwrite |
| `guest_put(token, key, value) -> int` | a put with a guest's rules: -1 for a frozen input or a dead token |
| `free(m)` | frees the map; its token dies |

**By length.** Keys and values are strings by their length, so a NUL inside
one is a byte: stored, compared, read back and carried through the shared
memory block whole. `"k\0x"` and `"k"` are different keys.

**Limits.** At most `sharedmap.MAX_ENTRIES` (256) keys: a put of a new key
past it is refused with -1, never dropped. The shared memory block a host
writes for a child is at most 64 KiB (`to_shm` refuses a larger map with
`E2BIG`, and the `*_with_map` runner refuses to start), and a block read back
at most 64 MiB.

**The shared memory block** is layout 2, length-prefixed, with a magic and a
version: a reader built for the old NUL-separated layout 1 sees an empty map
instead of misreading lengths as text, and a block in layout 1 is refused
whole when read back. `runtime/aether_shared_map.h` has the table. POSIX
shared memory is used on Linux and macOS; on Windows, Android and the BSDs
the map works in process and `to_shm` says `ENOSYS`.

**Ownership.** A map is the caller's, freed with `free`. Strings passed in
stay the caller's (the map copies them); strings returned are new and the
caller's.
