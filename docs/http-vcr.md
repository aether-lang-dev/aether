# HTTP Record/Replay (VCR), moved out of Aether

The Servirtium record/replay engine that used to ship in the Aether
stdlib as **`std.http.server.vcr`** has moved to its own repository:

> **https://github.com/servirtium/servirtium-vcr**

That monorepo is now its authoritative home, the VCR core (`core/vcr.ae`,
`core/embed.ae`, `core/aether_vcr.c`) plus its language bindings (.NET, Go,
Java, Rust, JavaScript, Python, Ruby, PHP, Elixir, Dart, Haskell, Pharo)
and the Servirtium interop suites live there together.

## Why it moved

VCR served its purpose inside Aether: it shaped and hardened the HTTP
server (port-0 binding, background/quiet serving, chunked decode, the
`--emit=lib` PIC runtime and `aether_vcr_embed_*` C ABI). Now that the
server is solid, the record/replay engine belongs with the cross-language
bindings it exists to serve, not in the language's standard library.

Nothing in the Aether language or the rest of the stdlib depended on it,
so the removal has no effect on the stdlib surface.

## If you used `std.http.server.vcr`

Depend on the `servirtium-vcr` monorepo and `import core.vcr` (the module
was renamed from `module.ae` to `vcr.ae`). The API is the same one
documented here previously, and has since gained `untaped()` (don't-record
/ serve-404 paths), see the monorepo's `core/TODO.md`.

## What stayed in Aether

The pure `std.http.client` behaviour the VCR work motivated remains in the
stdlib and keeps its own coverage here, e.g. transparent de-chunking of
`Transfer-Encoding: chunked` responses
(`tests/integration/http_client_dechunk/`).

## History

CHANGELOG entries for the VCR work done while it lived here (the 0.181
embed ABI and global `-fPIC` runtime archive, 0.183 chunked de-chunk,
0.187 embed-surface + strict-ignore) are retained as project history in
[CHANGELOG-archive.md](../CHANGELOG-archive.md).
