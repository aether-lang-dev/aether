# A selective import inside a module doesn't bind an extern's short name (the glob form does since #2637)

**From:** aeo (2026-10-10) · **Where it bit:** aeo's runner stopped compiling
on 0.799.0/0.801.0 (fine on 0.791.0). Not a regression in itself: 0.798's
"every function of every module is checked" (#2613) exposed it. aeo has
worked around it (aeo's lib/transport_http now imports the `http_`-prefixed
externs). This ask is for the inconsistency underneath.

## Symptom

`import std.http (server_create)` then a bare `server_create(port)`:

| where the import is written | 0.778.0 | 0.791.0 | 0.801.0 |
|---|---|---|---|
| the program file (`main` lives there) | OK | OK | OK |
| a module (`lib/mm/module.ae`), selective `(server_create)` | E0301 | E0301 | E0301 (0.803.0 too) |
| a module, glob `(*)` | E0301 | E0301 | OK (#2637) |
| a module, `import std.http` + `http.server_create(...)` | OK | OK | OK |
| a module, selective `(http_server_create)` | OK | OK | OK |

`server_create` is the short name of `extern http_server_create` (std.http
doesn't export a `server_create` wrapper). The program file and, since #2637,
a module's glob import both resolve it. A module's selective import of the same
name does not.

## Repro

```
lib/mm/module.ae:
    import std.http (server_create)
    exports ( mk )
    mk() -> ptr { return server_create(0) }

c.ae:
    import mm
    main() {
        p = mm.mk()
        println("c")
    }
```

`ae build c.ae -o c --lib lib` gives
`error[E0301]: Undefined function 'server_create'` at `lib/mm/module.ae`, on
every version above. Move the same import + call into `c.ae` and it builds.

## Why it hid until now

Before 0.798, a module function nothing called was never checked. aeo's
`transport_http` server half (`serve_create` and the handlers calling
`response_set_status`/`response_set_body`) is called by nothing, so the broken
names went unchecked in every aeo program. Once #2613 checked every module
function, the runner (`import transport_http (post_path, probe_health)`)
failed. `bin/aeo-agent.ae` imports the same module and kept building. Its own
program file has `import std.http (server_create, …)`, and those bare names
reached the merged module functions too (repro below). So there are two
things here:

1. **The gap:** a selective import in a module should bind an extern-backed
   short name the way the glob form does since #2637, and the way the
   language reference says ("rewritten to their canonical prefixed form when
   the module is merged into a consumer, the same way selective and qualified
   imports are, extern-backed ones … included").
2. **The leak (maybe already intended away by #2614):** the entry file's
   selective import made `server_create` resolve inside a merged module that
   didn't import it. #2614 says "a module can only call the modules it
   imports itself". That covers qualified `ns.name`. It's worth checking that
   it also covers bare names that come from the *entry file's* selective
   import. Otherwise a module builds or doesn't depending on which program
   imports it. That is how aeo's agent and runner disagreed.

The leak, confirmed on 0.778.0 and 0.801.0. The module has **no import of
std.http at all**, and the program builds and runs:

```
lib/mm/module.ae:
    exports ( mk )
    mk() -> ptr { return server_create(0) }

e.ae:
    import std.http (server_create)
    import mm
    main() {
        p = mm.mk()
        println("e")
    }
```

## Ask

Make a module's selective import bind extern-backed short names (apply #2637's
fix to the selective path), and stop an entry file's selective import from
supplying bare names to merged modules.
