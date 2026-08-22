# std.path

Filesystem path manipulation: joining, splitting, and normalising.

Pure string work — nothing here touches the filesystem, so `basename` on a
path that does not exist is still a `basename`. Use `std.fs` to ask whether
something is there.

```aether,run
import std.path

main() {
    println("join:     ${path.join("a", "b.txt")}")
    println("basename: ${path.basename("/x/y/z.ae")}")
    println("dirname:  ${path.dirname("/x/y/z.ae")}")
    println("extension: ${path.extension("z.ae")}")
}
```
```output
join:     a/b.txt
basename: z.ae
dirname:  /x/y
extension: .ae
```

`extension` includes the dot, which is what makes `"${base}${ext}"`
round-trip without a conditional.

`clean` normalises `.` and `..` segments textually — it does not resolve
symlinks, because doing so would require touching the filesystem and would
make the function fail on paths that do not exist yet.

## Exports

`join`, `join_clean`, `basename`, `dirname`, `extension`, `clean`, `rel`,
`is_absolute`, `is_within_base`, `separator`.

`is_within_base` is the containment check: it answers whether a cleaned path
stays under a given root, which is what a static-file handler needs before
opening anything a request named.
