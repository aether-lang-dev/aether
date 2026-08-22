# std.plural

CLDR plural categories: which grammatical form a number takes in a language.

English has two forms — "1 file", "5 files" — so a naive `if n == 1` works and
teaches the wrong habit. Polish has four, Arabic six, and the rule is not
"is it one" but a function of the number's value, its fractional digits and
its last two digits. This module answers that question per locale so callers
select a message rather than build one.

```aether,run
import std.plural

main() {
    // English: one, then other.
    println("en 1: ${plural.plural_category("en", 1)}")
    println("en 5: ${plural.plural_category("en", 5)}")

    // Polish distinguishes a "few" category that English has no word for.
    println("pl 2: ${plural.plural_category("pl", 2)}")
}
```
```output
en 1: one
en 5: other
pl 2: few
```

The categories are CLDR's: `zero`, `one`, `two`, `few`, `many`, `other`. A
locale uses only the subset its grammar needs, and `other` is always present —
so a message catalogue that provides `other` alone degrades to something
readable rather than to nothing.

`plural_category_decimal` is the form for fractional values, where the digits
after the point change the answer in some locales.

Pair it with `std.message` to select the phrasing.

## Exports

`plural_category`, `plural_category_decimal`.
