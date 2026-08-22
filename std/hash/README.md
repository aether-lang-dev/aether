# std.hash

Non-cryptographic hash functions: FNV-1a, MurmurHash3 and SipHash-2-4.

**None of these is a substitute for `std.cryptography`.** They are for hash
tables, checksums and sharding — fast, well-distributed, and trivially
reversible by anyone who cares to. Never hash a password or sign a message
with them.

Every function takes an explicit length alongside the data, so a payload with
embedded NUL bytes hashes in full rather than stopping at the first zero.

```aether,run
import std.hash
import std.string

main() {
    data = "abc"
    n = string.length(data)

    println("fnv32:    ${hash.fnv32(data, n)}")
    println("murmur3:  ${hash.murmur3_32(data, n, 0)}")

    // SipHash takes a 128-bit key as two longs. With a per-process
    // random key it resists the collision flooding that turns a hash
    // table into a linked list; with a fixed key it is deterministic.
    println("siphash:  ${hash.siphash24(data, n, 0, 0)}")
}
```
```output
fnv32:    440920331
murmur3:  3017643002
siphash:  4596069200710135518
```

Rough guidance: **FNV-1a** for short keys and simplicity, **MurmurHash3** for
better distribution over longer keys, **SipHash-2-4** when the input is
attacker-controlled and collision flooding is the threat.

## Exports

`fnv32`, `fnv64`, `murmur3_32`, `siphash24`.
