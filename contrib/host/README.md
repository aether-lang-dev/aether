# contrib/host

Language bridges. Each subdirectory lets an Aether program run code in
another language and call back and forth with it, under the same grant list
the rest of aether's sandbox uses (`std.sandbox`, `docs/containment-sandbox.md`).

Most bridges are in-process: a C shim loads the language's runtime with
`dlopen` and a module (`import contrib.host.<lang>`) wraps it. A bridge
pushes its `perms` as a level of the calling thread, so what the guest asks
of libc is held to the grants, by aether's grant checks inside the process
and by the `LD_PRELOAD` library for what the interpreter does on its own.
Java, Go, TinyGo and the Aether host run as a separate process instead.

`stunt/` is the test double: a scriptless engine that bridges are tested
against so that aether's CI does not need an interpreter package for every
language. See its README.

Each bridge builds only when its development files are present
(`make contrib`, `make contrib-host-check`); without them it is a stub that
reports itself unavailable.

## Moved out

Bridges are moving to
[aether-guest-languages](https://github.com/aether-lang-dev/aether-guest-languages),
a separate repository where each language is held to stunt's catalogue by
name. A bridge that has moved is deleted here, with no transition period.

| Bridge | Moved | Now |
|---|---|---|
| `lua` | 2026-11-04 | `lua/` in aether-guest-languages, copied from this tree at `d3732a0ed` |

To use it:

```sh
ae add github.com/aether-lang-dev/aether-guest-languages
```

```aether,fragment
import lua
```

The other languages will follow there one at a time. `TODO.md` in this
directory tracks which have moved and what is left.
