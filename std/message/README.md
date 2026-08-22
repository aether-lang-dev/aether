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

`catalog_new` / `catalog_add` / `catalog_format` hold a set of messages by key,
which is the shape an application wants: look a message up by identifier,
format it for the user's locale.

## Exports

`format`, `parse`, `format_pattern`, `pattern_free`, `catalog_new`,
`catalog_add`, `catalog_format`, `catalog_free`.
