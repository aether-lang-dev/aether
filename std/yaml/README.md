# std.yaml

YAML parsing: documents, mappings, sequences and scalars.

`parse` returns `(document, err)`. Navigate from `root` by node type:
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

That prints:

```
err=''
entries: 2
name: Ada
```

`std.yaml` is backed by **libfyaml, vendored**: `std/yaml/libfyaml/` is a
pinned, unmodified subset of upstream libfyaml (see its `VENDOR.md`),
compiled into `libaether.a` by `aether_libfyaml_vendored.c`, as `std.regex`
does with PCRE2. No system libfyaml is needed to build or run a program, and
nothing links `-lfyaml`. Every release has it: Linux, macOS, Windows,
FreeBSD and Android, and `ae build --target ...` cross builds, except wasi
(no mmap or threads for it) and iOS (libfyaml calls `mach_vm_map()`, which
the iOS SDK marks unsupported), where `std.yaml` is the stub. Its symbols are all renamed
`aether_<name>`, so a program can also link a system libfyaml of another
version without the two meeting.

The Makefile's `YAML` picks the library: `vendored` (the default), `1` (a
system libfyaml found by pkg-config, required; a program importing
`std.yaml` then links `-lfyaml`), `auto` (the system one if found, else
vendored), or `0` (stubs: every entry point returns `"std.yaml unavailable:
this build has no libfyaml ..."`). The block above is **compiled but not
run** in CI, since a `YAML=0` build prints that error instead.

Unlike `std.zlib`, this module exposes no `available()` probe, so a program
that must degrade gracefully has to check the error string from `parse`.

`free` on the document releases the whole tree; nodes inside it are borrowed
and must not be freed separately.

Mapping order is preserved, so `mapping_get_key(root, 0)` is the first key as
written. YAML itself does not guarantee that mappings are ordered, but a
config file's author usually assumes it, and iterating in document order is
what makes a round-trip readable.

Scalars come back as text. YAML's implicit typing (`36` as a number, `yes` as
a boolean) is not applied: convert at the point of use with
`string.get_int` and friends, where you know what the field means.

## Exports

`parse`, `free`, `root`, `type`, `get_scalar`, `sequence_size`,
`sequence_get`, `mapping_size`, `mapping_get_key`, `mapping_get_value`.
