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

## Byte sizes

`bytes(n, style, locale)` renders a byte count the way a file manager does.
The decimal-vs-binary choice is **explicit** in the style — `GB` and `GiB` are
not interchangeable, and picking one silently is how two views of the same file
come to disagree:

```aether,fragment
import std.number

number.bytes(1070000000, number.BYTES_SI, "en-US")   // "1.07 GB"  (÷1000)
number.bytes(1073741824, number.BYTES_IEC, "en-US")  // "1.00 GiB" (÷1024)
number.bytes(512, number.BYTES_SI, "en-US")          // "512 B"    (plain count)
number.bytes_si(1500)                                // "1.50 KB"  (en-US, SI)
number.bytes_iec(1536)                               // "1.50 KiB" (en-US, IEC)
number.bytes(1070000000, number.BYTES_SI, "de-DE")   // "1,07 GB"  (locale decimal)
```

The mantissa is shown to 3 significant figures (2 fraction digits under 10, 1
under 100, 0 at/above 100) — the convention Finder / Nautilus /
`ByteCountFormatter` use, so `1_070_000_000` is `1.07 GB`, not `1.070 GB` or
`1 GB` — and its decimal separator follows `locale`. A count below one unit is
a plain integer (`512 B`, no grouping). `bytes_si` / `bytes_iec` are en-US
convenience forms.

## Exports

`FormatOptions`, `default_options`, `format_decimal`, `format_percent`,
`format_currency`, `format_decimal_default`, `format_percent_default`,
`format_decimal_string`, `format_percent_string`, `format_currency_string`;
`BYTES_SI`, `BYTES_IEC`, `bytes`, `bytes_si`, `bytes_iec`.
