# std.yaml

YAML parsing: documents, mappings, sequences and scalars.

`parse` returns `(document, err)`. Navigate from `root` by node type —
`mapping_size` / `mapping_get_key` / `mapping_get_value` for a mapping,
`sequence_size` / `sequence_get` for a sequence, `get_scalar` for a leaf.

Accessors that return text return `(value, err)`, so a scalar read on a node
that turns out to be a mapping is reported rather than yielding a pointer
printed as a number.

```aether
import std.yaml

main() {
    doc, err = yaml.parse("name: Ada\nage: 36\n")
    println("err='${err}'")

    root = yaml.root(doc)
    println("entries: ${yaml.mapping_size(root)}")

    key = yaml.mapping_get_key(root, 0)
    value = yaml.mapping_get_value(root, 0)

    kname, kerr = yaml.get_scalar(key)
    vname, verr = yaml.get_scalar(value)
    println("${kname}: ${vname}")

    yaml.free(doc)
}
```

With libfyaml present that prints:

```
err=''
entries: 2
name: Ada
```

The block is **compiled but not run** in CI. `std.yaml` is backed by
**libfyaml, an optional build dependency**: without it every entry point
returns `"std.yaml unavailable: this build has no libfyaml (install libfyaml
+ rebuild, or set YAML=1)"` and the example prints that instead. An asserted
`output` block would therefore pass or fail depending on how the machine
running CI was provisioned, which is not something a documentation example
should depend on.

Unlike `std.zlib`, this module exposes no `available()` probe, so a program
that must degrade gracefully has to check the error string from `parse`.

`free` on the document releases the whole tree; nodes inside it are borrowed
and must not be freed separately.

Mapping order is preserved, so `mapping_get_key(root, 0)` is the first key as
written. YAML itself does not guarantee that mappings are ordered, but a
config file's author usually assumes it, and iterating in document order is
what makes a round-trip readable.

Scalars come back as text. YAML's implicit typing (`36` as a number, `yes` as
a boolean) is not applied — convert at the point of use with
`string.get_int` and friends, where you know what the field means.

## Exports

`parse`, `free`, `root`, `type`, `get_scalar`, `sequence_size`,
`sequence_get`, `mapping_size`, `mapping_get_key`, `mapping_get_value`.
