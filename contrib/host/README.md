# contrib/host

Host bridges: an Aether program runs another engine's code and calls back
and forth with it, under the same grant list the rest of aether's sandbox
uses (`std.sandbox`, `docs/containment-sandbox.md`). A bridge pushes its
`perms` as a level of the calling thread, so what the guest asks of libc is
held to the grants, by aether's grant checks inside the process and by the
`LD_PRELOAD` library for what the engine does on its own.

Two bridges are left here:

- `stunt/` is the test double: a scriptless engine, loaded the way a bridge
  loads an interpreter (`dlopen`) or run as a contained child, that the
  language bridges are held to, so that aether's CI needs no interpreter
  package for any language. See its README.
- `aether/` is Aether hosting Aether: a compiled Aether program run as a
  sandboxed child process. See its README.

Each builds with `make contrib` (`MODULES=stunt`, `MODULES=aether`) and is
checked by `make contrib-host-check`; an `import contrib.host.<name>` links
its archive with no `link_flags`.

## Moved out

The language bridges moved to
[aether-guest-languages](https://github.com/aether-lang-dev/aether-guest-languages),
a separate repository where each language is held to stunt's catalogue by
name. Each was deleted here when it moved, with no transition period, and an
`import contrib.host.<lang>` becomes `import <lang>` after the `ae add`
below.

| Bridge | Moved | Now |
|---|---|---|
| `lua` | 2026-11-04 | `lua/` there, copied from this tree at `d3732a0ed` |
| `python` | 2026-11-05 | `python/` there, copied from this tree at `9673641e6` |
| `duktape`, `js` (its alias) | 2026-11-05 | `duktape/`, `js/` there, copied from this tree at `2609ec841` |
| `quickjs` | 2026-11-05 | `quickjs/` there, copied at `2609ec841`; the engine, `contrib/quickjs`, stays here |
| `perl` | 2026-11-05 | `perl/` there, copied at `2609ec841` |
| `ruby` | 2026-11-05 | `ruby/` there, copied at `2609ec841` |
| `tcl` | 2026-11-05 | `tcl/` there, copied at `2609ec841` |
| `go`, `tinygo` | 2026-11-05 | `go/`, `tinygo/` there, copied at `2609ec841` |
| `java` | 2026-11-05 | `java/` there, copied at `2609ec841` |
| `factor` | 2026-11-05 | `factor/` there, copied at `2609ec841` |
| `racket`, `rhombus` | 2026-11-05 | `racket/`, `rhombus/` there, copied at `2609ec841` |

To use one:

```sh
ae add github.com/aether-lang-dev/aether-guest-languages
```

```aether,fragment
import tcl
import python
```

Each language there has its ways to run it, from in-process with no
containment to a contained child process (`std.guest`), and a containment
table held to stunt's catalogue by name; see its README (`python/README.md`'s
"Four ways to run Python from Aether" and "Containment table", for one).
`PROVENANCE.md` beside each says what it was copied from. `TODO.md` in this
directory keeps what is left here.
