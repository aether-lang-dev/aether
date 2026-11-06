# std.json

JSON parsing, building and serialisation.

`parse` returns `(value, err)`; the value is a tree you navigate with typed
accessors. Errors carry a position (`last_error_line` and `last_error_col`,
and the message itself names it), so a malformed config points at the line
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
    println("age: ${json.get_int(age)}")

    tags, gerr = json.object_get(doc, "tags")
    println("tags: ${json.array_size(tags)}")

    json.free(doc)

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

Accessors that can fail return `(value, err)` (`get_string` above), while
`json_get_int` and friends return the value directly and rely on the caller
having checked `json_type` first. Check the type when the input is
untrusted.

For **building**, the `obj`/`arr`/`str`/`num` constructors compose a tree,
`set` and `push` put values in it, and `encode` serialises it. For
validating an untyped map against a schema before you trust it, see
`std/schema`.

`encode` writes a number so that parsing it gives back the same value. An
integer built with `from_int`, or parsed as a bare integer, is written as that
integer over the full `long` range. Any other number is written with as few
digits as reading it back to the same double needs: 15 when those suffice,
else 16, else 17. So `0.1` is `0.1` and `0.061234567891` keeps every digit.
A NaN or an infinity has no JSON form and is written as `null`, as
JavaScript's `JSON.stringify` writes it.

Strings keep every byte. A string or key may hold U+0000 (written
`\u0000`): `get_string` and `object_entry` return all of it, `str` and
`set` take all of an Aether string, and `encode` writes the NUL
back as `\u0000`. `parse` reads the whole of its input, so bytes after a
NUL are part of the document, not ignored.

```aether,run
import std.json

main() {
    value = 0.061234567891
    node = json.num(value)
    text, _ = json.encode(node)
    back, _ = json.parse(text)
    same = json.get_number(back) == value
    println("${text} reads back as the same double: ${same}")
    json.free(node)
    json.free(back)

    tenth = json.num(0.1)
    short, _ = json.encode(tenth)
    println(short)
    json.free(tenth)
}
```
```output
0.061234567891 reads back as the same double: true
0.1
```

## Pretty-printing

`pretty(src, indent)` re-indents a JSON document for reading or recording,
the way `JSON.stringify(v, null, indent)` lays one out: one member or element
per line, `"key": value`, an empty `{}` or `[]` on one line, no trailing
newline. `indent` is 1 to 16; 0 makes the document compact instead. The
document's own text is re-indented, not re-serialised, so key order, number
spellings (`1.0`, `1e3`, integers past 2^53), escapes and duplicate keys come
out exactly as they went in. A malformed document returns `""` and the
parser's message, with its line and column.

```aether,run
import std.json

main() {
    out, err = json.pretty("{\"id\":7,\"price\":1.0,\"tags\":[\"a\",\"b\"],\"meta\":{}}", 2)
    println(out)
    _, bad = json.pretty("{\"id\":}", 2)
    println(bad)
}
```
```output
{
  "id": 7,
  "price": 1.0,
  "tags": [
    "a",
    "b"
  ],
  "meta": {}
}
unexpected character '}' at 1:7
```

## Exports

`parse`, `parse_strict`, `json_free`, `json_type`, `json_is_null`,
`json_is_integer`, `json_get_bool`, `json_get_number`, `json_get_int`, `json_get_long`,
`get_string`, `object_get`, `object_size`, `object_entry`,
`json_object_has`, `json_object_key_at`, `json_object_value_at`, `array_get`,
`json_array_size`, `obj`, `arr`, `str`, `num`, `from_int`,
`boolean`, `null_value`, `set`, `push`, `encode`, `pretty`, `last_error`,
`last_error_kind`, `last_error_line`, `last_error_col`, the `KIND_*`
constants, and the kinds `json_type` returns, `JSON_NULL` to `JSON_OBJECT`.
