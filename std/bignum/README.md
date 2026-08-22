# std.bignum

Arbitrary-precision integers.

Values are immutable: every operation returns a new bignum rather than
mutating its operands, which is what makes them safe to share and awkward to
use in a tight loop. The module exists mainly to serve `std.cryptography` —
RSA, DSA and the elliptic-curve arithmetic all need integers far past 64 bits.

```aether,run
import std.bignum

main() {
    a = bignum.from_int(255)
    b = bignum.from_int(2)

    sum = bignum.add(a, b)
    println("bits: ${bignum.bit_length(sum)}")
    println("hex:  ${bignum.to_hex(sum)}")

    // sign is -1, 0 or 1; compare is the usual three-way result.
    println("sign:    ${bignum.sign(sum)}")
    println("compare: ${bignum.compare(a, b)}")
    println("zero?    ${bignum.is_zero(bignum.from_int(0))}")
}
```
```output
bits: 9
hex:  101
sign:    1
compare: 1
zero?    1
```

257 needs nine bits and is `0x101` — the example is small enough to check by
eye, which is the point of picking it.

`from_bytes` and `to_bytes` are the interop pair: they read and write
big-endian two's-complement, the encoding a key or a signature arrives in.
The `_unsigned` variants skip the sign bit for values known to be positive,
which is what most cryptographic material is.

## Exports

`from_int`, `from_bytes`, `from_bytes_unsigned`, `to_bytes`,
`to_bytes_unsigned`, `to_hex`, `compare`, `is_zero`, `sign`, `bit_length`,
`add`, `subtract`, `multiply`, `divide`, `mod`, `mod_pow`, `mod_inverse`,
`gcd`, `shift_left`, `shift_right`, and the bitwise operations.
