# std.url

Percent-encoding and query-string parsing, matching Go's `net/url` semantics.

Three encoders, because the correct escaping depends on where the text is
going. Using the wrong one is a real bug and a quiet one — a `+` that should
have been `%2B` round-trips fine through a lenient server and corrupts a
signature.

| function | space becomes | `+` becomes | `/` becomes | use for |
|---|---|---|---|---|
| `encode` | `+` | `%2B` | `%2F` | query components |
| `encode_path` | `%20` | `+` (kept) | `%2F` | path segments |
| `encode_strict` | `%20` | `%2B` | `%2F` | RFC 3986 unreserved only |

```aether,run
import std.url

main() {
    // Query component: space -> '+', and a literal '+' is escaped so it
    // survives the round trip.
    println(url.encode("hello world & more"))

    // Path segment: space -> %20, because '+' means '+' in a path.
    println(url.encode_path("a/b c"))

    // decode returns (value, err) — err is "" on success.
    decoded, err = url.decode("a%20b")
    println("${decoded} err='${err}'")
}
```
```output
hello+world+%26+more
a%2Fb%20c
a b err=''
```

## Query strings

`parse_query` returns `(list, err)`; `query_get` reads a single value from it,
and `query_get_all` returns every value bound to a repeated key.

```aether,run
import std.url

main() {
    q, err = url.parse_query("name=Ada&lang=aether")
    println("err='${err}'")
    println(url.query_get(q, "name"))
    println(url.query_get(q, "lang"))

    // An absent key reads as the empty string rather than an error.
    println("missing='${url.query_get(q, "nope")}'")
}
```
```output
err=''
Ada
aether
missing=''
```

## Exports

`encode`, `encode_path`, `encode_strict`, `decode`, `parse_query`,
`query_get`, `query_get_all`.
