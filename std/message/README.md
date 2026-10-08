# std.message

ICU-style message formatting: named placeholders filled from a map.

The point is not string interpolation — Aether has `${}` for that — but
**translatable** messages. A translator needs to reorder the parts of a
sentence, and a positional format string will not let them. Named placeholders
travel with the message, so `"Hello {name}"` and its German equivalent can put
`{name}` wherever the grammar requires.

`message` is a reserved word in Aether, so the import needs backticks:

```aether,run
import std.`message`(*)
import std.map(*)

main() {
    args = map_new()
    map_put_raw(args, "name", "Ada")
    map_put_raw(args, "count", "3")

    println(format("en", "Hello {name}, you have {count} messages.", args))

    map_free(args)
}
```
```output
Hello Ada, you have 3 messages.
```

The backtick form escapes the reserved word; the `(*)` makes the exports bare,
so calls read `format(...)` rather than a namespaced spelling that the reserved
word would block.

The locale argument drives plural and select forms — pair it with
`std.plural`, whose categories are what a message catalogue keys its variants
on.

A `plural` argument is read as a decimal numeral (`3`, `-1`, `1.5`,
`3000000000`), as ICU does with a number. An `=N` branch matches only an
exactly equal number (`1.50` takes `=1.5`; `1.5` never takes `=1`); otherwise
the branch is the locale's category for the digits as written, so in English
`1` is `one` while `1.5` and `1.0` are `other`. A value that is not a numeral
(`many`) matches no `=N` branch and has no category: it takes `other`, and `#`
still prints it as given. A missing argument counts as `0`.

`catalog_new` / `catalog_add` / `catalog_format` hold a set of messages by key,
which is the shape an application wants: look a message up by identifier,
format it for the user's locale.

## Exports

`format`, `parse`, `format_pattern`, `pattern_free`, `catalog_new`,
`catalog_add`, `catalog_format`, `catalog_free`.
