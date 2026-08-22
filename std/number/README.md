# std.number

Locale-aware number formatting: decimals, percentages and currency.

Unlike the rest of `std`, this module is **deliberately locale-sensitive**.
`std.string` and `std.json` are pinned to the C locale so a serialised float
is byte-identical everywhere; `std.number` is for the other job — rendering a
number for a person to read, where the separators should follow their
conventions.

```aether,run
import std.number

main() {
    // Same value, two conventions: the group and decimal separators swap.
    println(number.format_decimal_default("en-US", 1234567.891))
    println(number.format_decimal_default("de-DE", 1234567.891))

    println(number.format_percent_default("en-US", 0.4567))
    println(number.format_currency("en-US", "USD", 1234.5))
}
```
```output
1,234,567.891
1.234.567,891
45.67%
$1,234.50
```

Note the argument order on `format_currency`: locale, **currency**, then
value.

The `*_default` forms use sensible defaults for the locale. For control over
grouping, minimum and maximum fraction digits, build a `FormatOptions` with
`default_options()`, adjust it, and pass it to `format_decimal`,
`format_percent` or `format_currency`.

The `*_string` variants take and return strings throughout, for callers
holding a decimal that must not go through a float at all — arbitrary-precision
values, or money where a rounding step would be a bug.

## Exports

`FormatOptions`, `default_options`, `format_decimal`, `format_percent`,
`format_currency`, `format_decimal_default`, `format_percent_default`,
`format_decimal_string`, `format_percent_string`, `format_currency_string`.
