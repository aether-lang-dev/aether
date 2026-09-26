# std.decimal

Arbitrary-precision decimal fixed-point numbers: exact base-10 arithmetic with
explicit rounding, for money and anything else where the digits on the page
are the value.

A `float` is binary, so `0.1 + 0.2` is `0.30000000000000004` and a column of
prices drifts by a cent after enough lines. A decimal is a coefficient and a
base-10 exponent, held exactly, so `0.1 + 0.2` is `0.3` and stays so. The
port follows Go's [shopspring/decimal](https://github.com/shopspring/decimal):
the same operations, the same rounding rules, the same default division
precision.

```aether,run
import std.decimal

main() {
    a, _e1 = decimal.from_string("0.1")
    b, _e2 = decimal.from_string("0.2")
    sum = decimal.add(a, b)
    println("0.1 + 0.2 = ${decimal.to_string(sum)}")

    price, _e3 = decimal.from_string("19.99")
    qty = decimal.from_int(3)
    total = decimal.multiply(price, qty)
    println("3 x 19.99 = ${decimal.to_string(total)}")

    // Division cannot always be exact: it rounds half away from zero to 16
    // places by default, or to the places you ask for.
    third, _e4 = decimal.divide_round(total, qty, 2)
    println("each:      ${decimal.to_string_fixed(third, 2)}")

    decimal.free(a)
    decimal.free(b)
    decimal.free(sum)
    decimal.free(price)
    decimal.free(qty)
    decimal.free(total)
    decimal.free(third)
}
```
```output
0.1 + 0.2 = 0.3
3 x 19.99 = 59.97
each:      19.99
```

Values are immutable: every operation returns a new decimal and never touches
its operands, and each result is the caller's to `free`. Operations that can
fail, division by zero and a malformed string, return `(value, err)` with a
null value on failure, so there is nothing to free when `err` is set.

This is a different type from `std.bignum`, not a wrapper over it in spirit:
bignum is the integer coefficient underneath and has no fractional part, no
exponent and no rounding modes. Its `to_decimal` / `from_decimal` print and
parse an integer in base 10; this module does base-10 arithmetic.

## Rounding

Money math is mostly a rounding policy, so each mode has a name. `places` is
the number of fractional digits to keep; a negative count rounds to the left
of the point.

```aether,run
import std.decimal

show(label: string, d: ptr) {
    println("${label} ${decimal.to_string(d)}")
    decimal.free(d)
}

main() {
    x, _e = decimal.from_string("2.5")
    y, _f = decimal.from_string("-1.25")

    show("round(2.5, 0)        ", decimal.round(x, 0)) // half away from zero
    show("round_bank(2.5, 0)   ", decimal.round_bank(x, 0)) // half to even
    show("round(-1.25, 1)      ", decimal.round(y, 1))
    show("round_ceil(-1.25, 1) ", decimal.round_ceil(y, 1)) // toward +inf
    show("round_floor(-1.25, 1)", decimal.round_floor(y, 1)) // toward -inf
    show("round_up(-1.25, 1)   ", decimal.round_up(y, 1)) // away from zero
    show("round_down(-1.25, 1) ", decimal.round_down(y, 1)) // toward zero
    show("truncate(-1.25, 1)   ", decimal.truncate(y, 1))

    decimal.free(x)
    decimal.free(y)
}
```
```output
round(2.5, 0)         3
round_bank(2.5, 0)    2
round(-1.25, 1)       -1.3
round_ceil(-1.25, 1)  -1.2
round_floor(-1.25, 1) -1.3
round_up(-1.25, 1)    -1.3
round_down(-1.25, 1)  -1.2
truncate(-1.25, 1)    -1.2
```

`to_string` prints the shortest plain form, dropping trailing fractional zeros
and never using exponent notation. `to_string_fixed(d, places)` rounds half
away from zero and pads, so an invoice line always shows two digits;
`to_string_fixed_bank` does the same with half-to-even.

## Scale

A decimal remembers how it was written: `from_string("1.50")` has exponent
`-2` and coefficient `150`, while `from_string("1.5")` has exponent `-1`.
Comparison is by value, so the two are `equals`; only `exponent` and
`coefficient` can tell them apart. Addition works at the finer of the two
scales and multiplication adds the exponents, so exactness is never lost
until you round. `rescale(d, exp)` changes the exponent explicitly: lowering it
is exact, raising it truncates toward zero.

`quo_rem(d, d2, precision)` is the primitive under division: the quotient
truncated to `precision` places and the exact remainder, with
`d == q * d2 + r`. `divide_round` rounds that quotient half away from zero,
`divide` does so at `division_precision()` (16) places, and `mod` is the
remainder at precision zero.

## Exports

Construction: `new`, `from_int`, `from_string`, `zero`, `clone`, `free`.
Inspection: `coefficient`, `exponent`, `sign`, `is_zero`, `is_negative`,
`is_positive`. Arithmetic: `add`, `subtract`, `multiply`, `negate`, `abs`,
`shift`, `pow`, `divide`, `divide_round`, `quo_rem`, `mod`,
`division_precision`. Comparison: `compare`, `equals`, `min`, `max`.
Rounding: `rescale`, `round`, `round_bank`, `round_ceil`, `round_floor`,
`round_up`, `round_down`, `truncate`, `ceil`, `floor`. Rendering and
conversion: `to_string`, `to_string_fixed`, `to_string_fixed_bank`,
`to_double`, `int_part`.
