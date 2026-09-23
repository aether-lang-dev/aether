# `contrib.templating.liquid` — Shopify Liquid for Aether

A pure-Aether port of the [Liquid template language](https://shopify.github.io/liquid/).
Sandbox-friendly: no reflection, no eval, the filter and tag set is
explicit and finite. Intended for rendering operator-supplied templates
(email bodies, dashboards, generated reports) where the surrounding
program needs the rendering to be deterministic and free of arbitrary
code execution.

```aether
import contrib.templating.liquid

t, _ = liquid.parse_string("Hello {{ name }}!")

ctx = liquid.context_new()
liquid.context_put_string(ctx, "name", "alice")

out, _ = liquid.render(t, ctx)
// out == "Hello alice!"
```

This module lives in `contrib/`, **not** in `libaether.a`. To use it
you import `contrib.templating.liquid` from your Aether program; the
build system links it in automatically.

## When to use this

- **Operator-supplied templates.** Marketing emails, admin dashboards,
  generated PDFs / HTML reports where the template author is a trusted
  human but not a code author, and the template should not be able to
  read arbitrary files, fork processes, or call out to the network.
- **Static-site generation.** Render Liquid against a JSON-like
  context, write the output. The renderer is single-pass and the
  output is deterministic.
- **Server-side rendering of Shopify-shaped data.** Existing
  `.liquid` files port over directly for the supported subset (see
  below).

If you control the template author and want them to write Aether
directly (escape-correct via the host language's type system rather
than the renderer's escape rules), see the
[native DSL sketch in TODO.md](../../../TODO.md) instead.

## What's supported

All of the following have integration tests in
`tests/integration/liquid_*/`. The Liquid suite is 312 tests across
22 directories.

### Output and lookup

- `{{ name }}` — single-segment variable lookup against a string-keyed
  context.
- `{{ "literal string" }}` — string literals (single or double quotes).
- `{{ 42 }}` — integer literals.
- `{{ 3.7 }}`, `{{ -5 }}` — decimal and negative number literals.
- `{{ x | filter | filter2: 2 | filter3: 'a', 'b' }}` — filter chains
  with positional args: quoted strings or bare numbers.

### Tags

- `{% if cond %}` / `{% elsif cond %}` / `{% else %}` / `{% endif %}` —
  with `==` `!=` `<` `>` `<=` `>=`, `and` `or`, and `contains`
  (substring-in-string / item-in-string-list).
- `{% unless cond %}` / `{% endunless %}` — inverted `if`.
- `{% case x %}{% when v %}…{% else %}…{% endcase %}` — pattern match.
- `{% for i in (LO..HI) %}…{% endfor %}` — range iteration. `forloop.index` /
  `index0` / `first` / `last` / `length` / `rindex` / `rindex0` are
  bound inside the loop. `limit` / `offset` / `reversed` modifiers.
- `{% break %}` / `{% continue %}` — for-body control flow.
- `{% assign name = expr %}` — bind a name in the context (full filter
  chain on RHS).
- `{% capture name %}…{% endcapture %}` — render the block to a
  string and assign it.
- `{% increment x %}` / `{% decrement x %}` — Shopify counters,
  namespace independent from `assign`.
- `{% cycle 'a', 'b', 'c' %}` — anonymous and named-group lockstep.
- `{% tablerow %}` — HTML-table iterator with `cols` / `limit` /
  `offset` / `reversed`.
- `{% comment %}` / `{% endcomment %}` — body suppressed.
- `{% raw %}` / `{% endraw %}` — body emitted verbatim (no
  interpolation).
- `{%# inline comment %}` — Shopify's single-tag comment form.
- `{% liquid %}` — line-per-tag block form.
- `{{- … -}}` / `{%- … -%}` whitespace control on adjacent text
  tokens, symmetric on comment/raw open and close edges.

### Includes & layouts

- `{% include 'partial' %}` and `{% render 'partial' %}` — read a
  partial from a configurable root, parse it, render it inline.
  `context_set_include_root(ctx, "path/to/partials")` is required
  before render or an error is raised. Path traversal escapes are
  rejected via `std.fs.is_within_base`. Depth-limited at 100 to
  prevent infinite-include recursion.
- `{% layout 'parent' %}` + `{% block name %}…{% endblock %}` —
  Jekyll-style template inheritance (single level).
- `{% extends 'parent' %}` — Django/Jinja alias for `{% layout %}`.
- `{{ block.super }}` — inside a child block override, emits the
  parent's default block content.

### Filters

String, math, html, encoding — over 30 filters. The complete list:

```
upcase  downcase  capitalize  strip  lstrip  rstrip  reverse  size
append  prepend  default  truncate  truncatewords  replace
replace_first  remove  remove_first  newline_to_br  strip_html
strip_newlines  escape  escape_once  url_encode  url_decode  slice
plus  minus  times  divided_by  modulo  at_least  at_most
abs  ceil  floor  round
md5  sha1  sha256  base64_encode  base64_decode
escape_xml  json_escape
```

The numeric filters read their input as Liquid does: text that is exactly
`-?digits.digits` is a decimal, anything else is read as Ruby's `to_i`
reads it (`"12px"` is 12, `"abc"` is 0). `ceil`, `floor` and `round` give an
integer (`{{ 3.7 | floor }}` is `3`, `{{ -2.5 | round }}` is `-3`: halves
round away from zero). `round: N` rounds a decimal to N places and prints it
as Liquid does, without padding zeros but with at least one fraction digit
(`{{ 3.14159 | round: 2 }}` is `3.14`, `{{ 9.999 | round: 2 }}` is `10.0`),
and it rounds in decimal, so `{{ 1.005 | round: 2 }}` is `1.01`. `abs` keeps
the input's shape (`-5` is `5`, `-5.50` is `5.5`). The integer arithmetic
filters (`plus` .. `at_most`) stay in integers: `10 | divided_by: 3` is
`3`.

Unknown filter names pass the input through unchanged (Shopify
behaviour, not an error). `divided_by:"0"` and `modulo:"0"` raise
render-time errors rather than crashing. `default: VAL,
allow_false:"true"` widens the fallback set to include `"false"`,
`"nil"`, `"null"` (string-encoded falsy set).

### Typed bindings (Phase 1)

Alongside `context_put_string`, four scalar typed setters:

```aether
liquid.context_put_int(ctx,   "n",  42)
liquid.context_put_float(ctx, "pi", 3.14)
liquid.context_put_bool(ctx,  "ok", 1)        // 1 = true, 0 = false
liquid.context_put_nil(ctx,   "x")
```

Typed bindings are stored under an internal `v:<key>` namespace and
stringified for `{{ x }}` interpolation via the canonical
`value_to_string`. A typed binding shadows a same-name legacy
`context_put_string` regardless of bind order.

Arrays and objects in the context are not available yet (#2177), and
so neither is path access into them (#2178).

### Lexer / parse errors

Unterminated `{{` / `{%` / `{% comment %}` / `{% raw %}` errors carry
an `at line N` suffix (1-based, counts `\n`).

## Public surface

```aether
parse_string(src: string) -> (ptr, string)        // (template, error)
parse_file(path: string)  -> (ptr, string)        // fs.read + parse_string

context_new()                                        -> ptr
context_put_string(ctx: ptr, key: string, v: string)
context_put_int(ctx: ptr, key: string, v: int)
context_put_float(ctx: ptr, key: string, v: float)
context_put_bool(ctx: ptr, key: string, v: int)       // 1=true, 0=false
context_put_nil(ctx: ptr, key: string)
context_set_include_root(ctx: ptr, root: string)
context_free(ctx: ptr)

render(t: ptr, ctx: ptr)                       -> (string, string)
render_to_strbuilder(t: ptr, ctx: ptr, sb: ptr) -> string  // (error)
```

Value constructors / inspectors (mostly for downstream code that
builds packed values directly):

```aether
value_nil()             -> string
value_bool(b: int)      -> string
value_int(i: int)       -> string
value_float(f: float)   -> string
value_str(s: string)    -> string

value_kind(v: string)        -> int           // LV_NIL .. LV_OBJ
value_payload(v: string)     -> string        // strip "X:" prefix
value_to_string(v: string)   -> string        // canonical stringify
value_get_int(v: string)     -> int
value_get_float(v: string)   -> float
```

## Sandbox story

The Liquid renderer itself does **not** call `fs.read`. Only
`{% include %}` / `{% render %}` and `parse_file` do. When the
include tags are used, `context_set_include_root` must be called with
an explicit root directory; partials resolved against that root are
checked with `std.fs.is_within_base` so an attacker-supplied filename
like `'../../etc/passwd'` is rejected.

Under `--emit=lib`, `import contrib.templating.liquid` requires
`--with=fs` — the module transitively imports `std.fs`, and Aether's
import gate refuses the transitive dependency without explicit
opt-in. So a host that links your `.so` and embeds Liquid renders
is making an explicit acknowledgement that its filesystem surface
is in scope. Without `--with=fs`, the build fails with the standard
capability-empty error. Verified by
`tests/integration/liquid_sandbox_gate/`.

## What's not supported yet

Each has its own issue.

- Arrays and objects in the render context: the value model the next
  three need (#2177).
- Path access: `{{ user.name }}`, `{{ items[0] }}`, `.size` / `.first` /
  `.last` (#2178).
- `{% for x in items %}` over a bound array; ranges `(1..5)` work
  today (#2179).
- Array filters: `split`, `join`, `first`, `last`, `sort`, `uniq`, `map`,
  `where`, `compact`, `concat` (#2180). Until then, as unknown filters,
  they pass their input through unchanged.
- `date`: passes its input through unchanged (#2181).
- A filter argument that names a variable, `{{ price | times: qty }}`:
  rejected at render (#2182).
- Layout inheritance deeper than one level (#2183).
- Caching parsed partials: each `{% include %}` reads and parses its file
  again (#2184).

## Performance notes

- Parsing is single-pass over a token stream; render walks the tokens
  with no AST allocation per render.
- Strings are `std.string` (refcounted heap strings); the renderer
  uses `std.strbuilder` for output accumulation so output assembly
  is O(N).
- Includes are uncached: partials are read and parsed again at every
  use. For a static-site generator that uses 100 partials, this is the
  long-pole cost (#2184).

## Testing

```
bash tests/integration/liquid_*/test_*.sh
```

Or as part of the full Aether CI:

```
make ci
```

## License

Same as Aether.
