# std.json

JSON parsing, building and serialisation.

`parse` returns `(value, err)`; the value is a tree you navigate with typed
accessors. Errors carry a position — `last_error_line` and `last_error_col`,
and the message itself names it — so a malformed config points at the line
rather than just failing.

```aether,run
import std.json

main() {
    doc, err = json.parse("{\"name\":\"Ada\",\"age\":36,\"tags\":[\"x\",\"y\"]}")
    println("err='${err}'")

    name, nerr = json.object_get(doc, "name")
    text, terr = json.get_string(name)
    println("name: ${text}")

    age, aerr = json.object_get(doc, "age")
    println("age: ${json.json_get_int(age)}")

    tags, gerr = json.object_get(doc, "tags")
    println("tags: ${json.array_size(tags)}")

    json.json_free(doc)

    // A parse error names where it gave up.
    _, bad = json.parse("{oops")
    println("bad: ${bad}")
}
```
```output
err=''
name: Ada
age: 36
tags: 2
bad: expected string key in object at 1:2
```

`json_free` on the root frees the whole tree; children must not be freed
separately.

Accessors that can fail return `(value, err)` — `get_string` above — while
`json_get_int` and friends return the value directly and rely on the caller
having checked `json_type` first. Check the type when the input is
untrusted.

For **building**, the `obj`/`arr`/`str`/`num` constructors compose a tree that
`stringify` serialises. For validating an untyped map against a schema before
you trust it, see `std/schema`.

## Exports

`parse`, `parse_strict`, `stringify`, `json_free`, `json_type`, `json_is_null`,
`json_get_bool`, `json_get_number`, `json_get_int`, `json_get_long`,
`get_string`, `object_get`, `object_set`, `object_size`, `object_entry`,
`json_object_has`, `json_object_key_at`, `json_object_value_at`, `array_get`,
`array_add`, `json_array_size`, `obj`, `arr`, `str`, `num`, `from_int`,
`boolean`, `null_value`, `set`, `push`, `encode`, `last_error`,
`last_error_kind`, `last_error_line`, `last_error_col`, and the `KIND_*`
constants.
