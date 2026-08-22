# std.cas

A content-addressed store: files filed under the SHA-256 of their bytes.

`put` hashes a file and stores it under the digest; `get` copies it back out.
Two consequences follow from addressing by content: storing the same bytes
twice is idempotent and costs nothing the second time, and a digest is a
verifiable name — if `get` returns it, it hashes to what you asked for.

The root is `AETHER_CAS`, falling back to `$HOME/.aether/cas`.

The example **compiles but is not run** in CI: it writes to a store on disk,
and a documentation example should not need one to be checked.

```aether
import std.cas
import std.fs

main() {
    werr = fs.write("/tmp/payload.txt", "hello content-addressed world")
    if werr != "" {
        println("write failed: ${werr}")
        return
    }

    digest, perr = cas.put("/tmp/payload.txt")
    if perr != "" {
        println("put failed: ${perr}")
        return
    }
    println("stored as ${digest}")

    // The same bytes always yield the same digest.
    println("present: ${cas.has(digest)}")

    gerr = cas.get(digest, "/tmp/copy.txt")
    if gerr != "" {
        println("get failed: ${gerr}")
    }

    fs.delete("/tmp/payload.txt")
    fs.delete("/tmp/copy.txt")
}
```

`path(digest)` composes the on-disk location without touching the filesystem,
which is what a caller wants when handing the path to something else.

`has("")` is 0 rather than an error, so an empty digest from an uninitialised
variable reads as "not present" instead of exploding.

## Exports

`root`, `path`, `has`, `put`, `get`, plus the `cas_*_raw` escape hatches.
