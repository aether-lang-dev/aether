# std.config

A process-wide key→value store for configuration.

One flat namespace of strings, shared by the whole process. That is
deliberate: configuration is ambient by nature, and threading a config object
through every call site to reach the one function that needs a timeout is
noise. The cost is that it is global state — treat writes as startup-time and
reads as anywhere.

```aether,run
import std.config

main() {
    config.put("retries", "3")

    println("get:    ${config.get("retries")}")
    println("get_or: ${config.get_or("missing", "default")}")
    println("has:    ${config.has("retries")}")

    config.clear()
    println("after clear: ${config.size()}")
}
```
```output
get:    3
get_or: default
has:    1
after clear: 0
```

`get_or` is the form to reach for: `get` on an absent key returns the empty
string, which is indistinguishable from a key deliberately set to empty.

Values are strings, so numeric settings are parsed at the point of use —
`string.get_int(config.get_or("retries", "3"))`. That keeps the store simple
and puts the "what if it is not a number" decision where the value is
actually needed.

## Exports

`put`, `get`, `get_or`, `has`, `size`, `clear`.
