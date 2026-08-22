# std.regex

Regular expressions, backed by PCRE2 (vendored, so no system dependency).

`compile` returns `ptr!` — a pattern is user input as often as not, and a
malformed one is a value to handle rather than a crash. Compile once and reuse
the handle: compilation is the expensive half.

`matches` returns `1`/`0` rather than a `bool`, so compare explicitly.

```aether,run
import std.regex

main() {
    re = regex.compile("[0-9]+") or {
        println("pattern did not compile")
        return
    }

    println("abc123 matches: ${regex.matches(re, "abc123")}")
    println("abc    matches: ${regex.matches(re, "abc")}")

    // replace_all returns (result, err).
    out, err = regex.replace_all(re, "a1b22c333", "#")
    println("${out} err='${err}'")

    regex.free(re)
}
```
```output
abc123 matches: 1
abc    matches: 0
a#b#c# err=''
```

`captures` and `find_all` return handles with their own accessors
(`captures_get`, `find_all_start`, and so on) and their own `*_free`; each is
`ptr!` for the same reason `compile` is.

## Exports

`compile`, `free`, `matches`, `captures`, `captures_count`, `captures_get`,
`captures_start`, `captures_end`, `captures_free`, `find_all`,
`find_all_count`, `find_all_start`, `find_all_end`, `find_all_free`,
`replace_all`.
