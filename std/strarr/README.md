# std.strarr

A growable array of string pointers whose backing **is** a `string[]`.

`string[]` in Aether is a compile-time literal (`["a", "b"]`) — a bare C array
with no carried length. But the strings you actually want to sort usually arrive
at runtime and in another shape: `string.split` hands back an opaque array
handle, `fs.glob` a `dir_list`. There was no way to build a `string[]` of
runtime-determined length to pass to `std.sort`.

`strarr` fills that gap. It is the string companion to `std.intarr` /
`std.longarr` / `std.floatarr`, except it **grows** (`push`) — the whole point
is a count you don't know up front. Its `array()` view is a real `string[]`, so
`std.sort.strings_by` (and `strings` / `string_search`) sort it in place.

**Ownership:** `push` *borrows* — exactly like a `string[]` literal, which
points at strings the caller owns — so keep the pushed strings alive for as long
as the array (or any sort result taken from it) is read. When you push a string
you just built (a per-iteration `concat` or basename), reach for **`push_copy`**
instead: the array takes its own reference and frees it in `free`, so a reused
loop local can't dangle. `free` releases only the spine plus any `push_copy`
references; `push`-borrowed elements stay the caller's.

```aether,run
import std.strarr
import std.sort
import std.string

_by_version(a: string, b: string) -> int {
    return string.version_compare(a, b)
}

main() {
    // Build a string[] of runtime length, then sort newest-last by version.
    sa = strarr.new()
    _a = strarr.push(sa, "app-1.9.jar")
    _b = strarr.push(sa, "app-1.12.0.jar")
    _c = strarr.push(sa, "app-1.11.jar")

    sort.strings_by(strarr.array(sa), strarr.size(sa), _by_version)

    newest = strarr.get(sa, strarr.size(sa) - 1)
    println("newest = ${newest}")
    strarr.free(sa)
    return 0
}
```

```output
newest = app-1.12.0.jar
```

Passing a bare `ptr` (say a `string.split` result) where a `string[]` is
expected is a compile error, not a silent crash — cast with `as string[]` only
when you know the pointer names a contiguous string buffer.
