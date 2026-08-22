# std.bits

Bit manipulation: rotates, logical shifts, population count and leading-zero
count, in 32- and 64-bit widths.

The reason this module exists is `lsr`. Aether's `>>` is an **arithmetic**
shift — it propagates the sign bit — which is right for arithmetic and wrong
for the bit-twiddling in a hash, a checksum or a cipher. `lsr32`/`lsr64` are
the logical shift that fills with zeros.

```aether,run
import std.bits

main() {
    // -1 is all ones. Shifted arithmetically it stays -1 forever;
    // shifted logically, the zeros march in from the top.
    println("lsr32(-1, 28) = ${bits.lsr32(-1, 28)}")

    // Rotates keep every bit, moving them around the word.
    println("rotl32(1, 1)  = ${bits.rotl32(1, 1)}")

    println("popcount32(255) = ${bits.popcount32(255)}")
    println("clz32(1)        = ${bits.clz32(1)}")
}
```
```output
lsr32(-1, 28) = 15
rotl32(1, 1)  = 2
popcount32(255) = 8
clz32(1)        = 31
```

`clz32(1)` is 31 because a 1 in the lowest bit of a 32-bit word leaves 31
zeros above it.

## Exports

`lsr32`, `lsr64`, `rotr32`, `rotl32`, `rotr64`, `rotl64`, `popcount32`,
`popcount64`, `clz32`, `clz64`.
