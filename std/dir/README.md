# std.dir

Directory creation, deletion and listing.

Listing returns a handle rather than an array, with `list_count` /
`list_get` / `list_kind` accessors and a `dir_list_free` to release it. The
kind comes straight from `readdir`'s `d_type`, so a listing does not pay for a
`stat` per entry — worth having when walking a large tree.

The example **compiles but is not run** in CI: it touches the filesystem, and
a documentation example should not need a scratch directory to be checked.

```aether
import std.dir

main() {
    err = dir.create("/tmp/aether-example")
    if err != "" {
        println("create failed: ${err}")
        return
    }

    entries, lerr = dir.list("/tmp/aether-example")
    if lerr != "" {
        println("list failed: ${lerr}")
        return
    }

    n = dir.list_count(entries)
    println("${n} entries")
    for (i = 0; i < n; i = i + 1) {
        // kind: 1 file, 2 dir, 3 symlink, 4 other, 0 unknown
        println("  ${dir.list_get(entries, i)} kind=${dir.list_kind(entries, i)}")
    }

    dir.dir_list_free(entries)
    dir.delete("/tmp/aether-example")
}
```

**`list_kind` can return 0.** Not every filesystem reports a type through
`readdir` — network mounts often do not — and 0 means "unknown, stat it if you
need to know". Code that assumes 1-or-2 will misclassify entries on those
filesystems.

`delete` removes an empty directory only. To remove a populated one, list it,
delete the entries, then delete the directory.

`std.fs` carries the recursive helpers (`mkdir_p`, `walk`, `glob`) for when
one call should cover a tree.

## Exports

`create`, `delete`, `list`, `list_count`, `list_get`, `list_kind`,
`dir_exists`, `dir_list_free`.
