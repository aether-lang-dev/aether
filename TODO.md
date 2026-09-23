# Aether — TODO

Long-running list of things to build / decide later. Short-lived
tasks live in PRs and the CHANGELOG; this file is for ideas with a
hint of design but no committed scope.

## `contrib.templating.dsl` — native, no-reflection templating

**Why:** Aether already has the closure-DSL pattern
(`docs/closures-and-builder-dsl.md`). A native templating engine
fits naturally on top: the template *is* a `.ae` closure, the output
format is chosen by which emitter the builder runs against.

The `contrib.templating.liquid` port is shipping first because the
ecosystem-interop case is more immediate (operators bring their own
`.liquid` files). The native DSL is the inverse pitch: library
authors expose a template-shaped surface where users write Aether,
not template syntax.

### Sketch

```aether
import contrib.templating.dsl

t = template(html) {
    h1   { text("Hello ${user}") }
    each items |it| {
        li { text(it.name) }
    }
}
println(render(t))
```

Swap the first arg to switch output:
- `template(html)` — escape-aware HTML
- `template(xml)`  — XML (CDATA-aware, namespace-prefix-aware)
- `template(json)` — JSON (commas, array/object dispatch)
- `template(sql)`  — SQL with placeholder binding (never raw
                     concatenation — `text(user_input)` produces
                     `?` + a side-channel parameter list)

The escape decision lives in the **emitter**, not the template. That
is the whole reason this shape is valuable in a sandboxed language —
the template author cannot accidentally produce raw user content in
an HTML attribute.

### Dispatch model — three options to pick between

When the time comes to build this, decide first:

1. **One namespace per emitter** — `import contrib.templating.dsl.html`
   brings `h1/text/each` bound to HTML escape rules. SQL importers
   get `select/where/text` bound to bind-parameter rules. Same
   surface symbol (`text`) means different things based on which
   module imported it. No runtime dispatch, sandbox-clean. Mixing
   HTML + SQL in one file requires aliasing.
2. **Single namespace, emitter chosen at `template()` call** —
   runtime dispatch off the active emitter on the context stack.
   More flexible, slightly more magical, risk of invisible
   escape-mode changes if emitters nest.
3. **Emitter-as-typed-receiver** — `html.h1 { html.text(x) }`.
   Most explicit, most verbose. Dead-simple sandbox story.

Leaning toward option 1 — it's the most Aether-shaped (matches how
`std.fs` / `std.http` / etc. work).

### Dependencies

- The existing closures-and-builder-dsl machinery, no additions.
- `std.strbuilder` for output accumulation.
- No reflection, no runtime type dispatch — same constraints as the
  Liquid port.

### Out of scope (for v1 of this when it lands)

- Partials / layouts (the `extends`/`block` pattern from Liquid).
- Cross-emitter composition (an HTML fragment embedded in a SQL
  string, say) — solve when there's a real use case.
- Streaming output. v1 builds a string; if a server author needs
  chunked output, that's a v2 ask.

### When to pick this up

After the Liquid port has been in `contrib/` long enough to validate
the value-tree + filter-registry shapes, and after at least one
downstream user has asked for the "templates that ARE Aether
code" pitch explicitly.

### Walking skeleton landed (`contrib/templating/native/`)

The plain-function-call escape-correct emitter pair landed:
`html_text`, `html_raw`, `html_tag_open`, `html_tag_close`, `html_tag`,
all writing into a `std.strbuilder`. Tests in
`native_templating_skeleton/` (6 cases). This is the foundation the
eventual builder DSL will layer on top of — the escape rule is in
the helper functions, so any sugar above can compose without
relitigating the contract. Still pending from the design above: XML
/ SQL / JSON emitter triples, attribute helper (`html_attr`), the
trailing-block builder shape itself, and the `import as nt`
shorthand pattern.

---

## `contrib.templating.liquid`

Shipped. `contrib/templating/liquid/README.md` lists what it supports and,
under "What's not supported yet", the issue for each thing it does not do
(#2177 through #2184). The build log that used to live here described the
first slice, and the module has since outgrown every line of it.

## Other parked work

### Contrib runtime coverage is split across two mechanisms

`contrib/sqlite` is **not** in `.github/scripts/contrib_check.sh` — it is
covered by `tests/integration/sqlite_roundtrip/` instead. So contrib
runtime coverage currently lives in two places with different shapes, and
nothing says which a new module should use.

Noticed while fixing the `avcodec` entry (2026-08-09), which had been added
to `contrib_check.sh` but could never pass: the runner builds with
`ae build --extra shim.c`, which compiles a C shim but **cannot pass `-l`
flags**, so any module backed by a system library compiled and then died at
link with `undefined reference`. The fix taught `contrib_check.sh` a fifth
column naming pkg-config modules; when set it stages an `aether.toml`
workspace carrying `link_flags` — which is precisely the shape
`sqlite_roundtrip` had been using all along, hand-rolled in its own `.sh`.

So the two mechanisms now overlap: `contrib_check.sh` can do what
`sqlite_roundtrip` does, generically and in a table.

**The work:** move `contrib/sqlite` into the `contrib_check.sh` table

```
"sqlite/roundtrip|contrib/sqlite/test_sqlite.ae|contrib/sqlite/aether_sqlite.c|run|sqlite3"
```

and retire `tests/integration/sqlite_roundtrip/`. Needs a
`contrib/sqlite/test_sqlite.ae` first — the existing `probe.ae` is written
for the workspace harness and would need adapting.

**Why it is worth doing:** one table means a new native-backed contrib
module gets runtime coverage by adding one line, rather than by copying a
50-line shell driver and rediscovering the `aether.toml` trick. It also
puts sqlite into the nightly's `make contrib-check-valgrind` sweep, which
it is currently outside of.

**Why it is parked:** the avcodec fix already covers the mechanism, sqlite
*is* tested today (just elsewhere), and consolidating means rewriting a
working test — pure cleanup with no new coverage. Do it when someone next
adds a native-backed contrib module and has to choose between the two
patterns.

### `std.bignum` — deferred optimizations

The arbitrary-precision integer surface (`std/bignum/module.ae`) is functionally
complete and oracle-verified; Montgomery `mod_pow` (odd moduli) and Karatsuba
multiplication have shipped. Still deferred, none blocking correctness:

- Barrett reduction for the even-modulus `mod_pow` path.
- Optimized long division (schoolbook division is the current throughput floor).
- Toom-Cook multiplication above the Karatsuba threshold for very large operands.
- Random-witness Miller-Rabin (the primality test currently uses fixed witnesses).
