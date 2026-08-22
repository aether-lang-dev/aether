# std.language

BCP 47 language tags: parsing, and matching a request against what you have.

A tag like `en-US` or `zh-Hant-TW` carries a base language, an optional
script, and an optional region. `parse` validates and canonicalises it;
the accessors pull out the parts.

```aether,run
import std.language

main() {
    raw, err = language.parse("en-US")
    println("err='${err}'")

    // parse returns a plain string in the tuple; the accessors take
    // the distinct Tag type, so the cast at the boundary is required.
    tag = raw as language.Tag

    println("base:   ${language.base(tag)}")
    println("region: ${language.region(tag)}")
}
```
```output
err=''
base:   en
region: US
```

That `as language.Tag` is not ceremony you can skip — `base(raw)` is a type
error. The distinct type is what stops an arbitrary string being passed where
a validated tag is expected.

`Matcher` is the other half: given the locales an application actually ships
and the list a browser sent in `Accept-Language`, it picks the best available
match. That is more than string equality — a request for `en-GB` should be
served `en` rather than falling through to the default, and `zh-Hant` should
not be matched by `zh-Hans`.

Pair it with `std.plural` for grammatical number and `std.message` for the
messages themselves.

## Exports

`Tag`, `parse`, `language`, `script`, `region`, `base`, `Matcher`,
`matcher_create`, `matcher_free`, `match_tags`, `match_strings`.
