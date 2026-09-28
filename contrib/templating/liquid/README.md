# `contrib.templating.liquid` — Shopify Liquid for Aether

A pure-Aether port of the [Liquid template language](https://shopify.github.io/liquid/).
Sandbox-friendly: no reflection, no eval, the filter and tag set is
explicit and finite. Intended for rendering operator-supplied templates
(email bodies, dashboards, generated reports) where the surrounding
program needs the rendering to be deterministic and free of arbitrary
code execution.

```aether
import contrib.templating.liquid

t, _ = liquid.parse_string("Hello {{ user.name }}! {% for t in user.tags %}#{{ t }} {% endfor %}")

ctx = liquid.context_new()
liquid.context_put_json_text(ctx, "user", "{\"name\": \"alice\", \"tags\": [\"a\", \"b\"]}")

out, _ = liquid.render(t, ctx)
// out == "Hello alice! #a #b "
liquid.context_free(ctx)
```

This module lives in `contrib/`, **not** in `libaether.a`. To use it
you import `contrib.templating.liquid` from your Aether program; the
build system links it in automatically.

## When to use this

- **Operator-supplied templates.** Marketing emails, admin dashboards,
  generated PDFs / HTML reports where the template author is a trusted
  human but not a code author, and the template should not be able to
  read arbitrary files, fork processes, or call out to the network.
- **Static-site generation.** Render Liquid against a JSON context,
  write the output. The renderer is single-pass and the output is
  deterministic: it reads no clock of its own (see `date` below).
- **Server-side rendering of Shopify-shaped data.** Existing
  `.liquid` files port over directly for the supported subset (see
  below).

If you control the template author and want them to write Aether
directly (escape-correct via the host language's type system rather
than the renderer's escape rules), see the
[native DSL sketch in TODO.md](../../../TODO.md) instead.

## What's supported

Every feature below has specs beside the module, in `test_values.ae`,
`test_syntax.ae`, `test_tags.ae`, `test_filters.ae` and
`test_inheritance.ae`: 407 of them.

### Values

A value is nil, a boolean, an integer, a float, a string, an array or an
object. The host binds them:

```aether
liquid.context_put_string(ctx, "name", "alice")
liquid.context_put_int(ctx,   "n",    42)
liquid.context_put_float(ctx, "pi",   3.14)
liquid.context_put_bool(ctx,  "ok",   1)        // 1 = true, 0 = false
liquid.context_put_nil(ctx,   "x")
liquid.context_put_json_text(ctx, "product", "{\"title\": \"Hat\", \"sizes\": [\"S\", \"M\"]}")
liquid.context_put_json(ctx, "order", tree)     // a std.json tree
```

`context_put_json` and `context_put_json_text` copy the JSON into the
context: the caller frees its own tree whenever it likes, and
`context_free` releases the copy with everything else the context holds.
A JSON number written as an integer (`42`) is an integer, and one written
with a fraction or an exponent (`42.0`, `1e2`) is a float. Nesting deeper
than 256 levels is refused, as `std.json`'s parser refuses it. A typed
binding shadows a `context_put_string` of the same name, whichever was
bound last.

Arrays and objects live in a store the context owns, and a template makes
new ones as it renders (`split`, `sort`, `for` over an object). Those that
no binding reaches any more are reclaimed when an outermost render
returns, so a context reused for many renders does not grow with them.
`context_value_nodes(ctx)` reports how many arrays and objects the context
holds, for measuring.

How values read:

- **Output.** `{{ x }}` prints an array's elements one after another (a
  nested array flattened into it, nil as nothing), an object as Liquid
  inspects a hash (`{"title"=>"Hat", "sizes"=>["S", "M"]}`), nil as
  nothing, a float as Ruby prints it (`3.0`, `1.0e+20`).
- **Truthiness.** nil and false are false. Every other value is true: the
  empty string, zero, and an empty array included.
- **`==` / `!=`.** Arrays and objects are equal when their elements (their
  keys and values) are. Two numbers compare as numbers (`1 == 1.0`). nil
  equals only nil. Any other two scalars compare by their text, as the
  string-typed context always has (`3 == "3"`).
- **`<` `>` `<=` `>=`.** Each side is read as a number, as Liquid reads
  one (`"10" > "9"`). An array or an object on either side is an error.
- **`contains`.** A string holds a substring, an array holds an element
  equal to the operand, and an object has the operand as a key.
- **`empty` and `blank`.** `x == empty` is true for an empty string, array
  or object. `x == blank` is true for those, for nil and false, and for a
  string of nothing but whitespace.

### Variable paths

`{{ user.name }}`, `{{ items[0] }}`, `{{ items[-1] }}`,
`{{ user["first name"] }}`, `{{ items[i] }}`, `{{ a.b[c.d].e }}`. On an object a
segment is a key, and on an array an integer index, negative from the end.
`size`, `first` and `last` work on arrays, on strings (a string's first and
last character), and on an object that has no key of that name (`size`
counts its keys; `first` is its first `[key, value]` pair). A missing key,
index or segment is nil and renders as nothing. A path works anywhere a
value can appear: output, conditions, `assign`, `for` sources, `case` and
`when`, filter arguments, `{% render %}` arguments, and `limit` / `offset`.

A name bound whole with its dots (`context_put_string(ctx, "site.title",
...)`) is still found under that name when the path's head is not bound.

Literals: `"string"` / `'string'`, `42`, `3.7`, `-5`, `true`, `false`, `nil` /
`null`, `empty`, `blank`.

### Tags

- `{% if cond %}` / `{% elsif cond %}` / `{% else %}` / `{% endif %}` —
  with `==` `!=` `<` `>` `<=` `>=`, `and` `or`, and `contains`.
- `{% unless cond %}` / `{% endunless %}` — inverted `if`.
- `{% case x %}{% when v, w %}…{% else %}…{% endcase %}` — pattern match.
- `{% for x in SOURCE %}…{% else %}…{% endfor %}` — SOURCE is a range
  `(lo..hi)` (inclusive; the bounds may be variables), an array, an object
  (iterated as `[key, value]` pairs, in key order), or a string, iterated
  once as itself. nil and an empty collection iterate zero times, and the
  `else` branch renders instead. `limit: N`, `offset: N` (N may be a
  variable) and `reversed` slice the source; `forloop.index` / `index0` /
  `rindex` / `rindex0` / `first` / `last` / `length` / `name`, and
  `forloop.parentloop` in a nested loop, are bound inside.
- `{% break %}` / `{% continue %}` — for-body control flow.
- `{% assign name = expr %}` — bind a name in the context (full filter
  chain on the right; an array stays an array).
- `{% capture name %}…{% endcapture %}` — render the block to a
  string and assign it.
- `{% increment x %}` / `{% decrement x %}` — Shopify counters,
  namespace independent from `assign`.
- `{% cycle 'a', 'b', 'c' %}` — anonymous and named-group lockstep.
- `{% tablerow %}` — HTML-table iterator over the same sources as `for`,
  with `cols` / `limit` / `offset` / `reversed`.
- `{% comment %}` / `{% endcomment %}` — body suppressed.
- `{% raw %}` / `{% endraw %}` — body emitted verbatim (no
  interpolation).
- `{%# inline comment %}` — Shopify's single-tag comment form.
- `{% liquid %}` — line-per-tag block form.
- `{{- … -}}` / `{%- … -%}` whitespace control on adjacent text
  tokens, symmetric on comment/raw open and close edges.

### Includes & layouts

- `{% include 'partial' %}` and `{% render 'partial', key: value %}` —
  read a partial from a configurable root, parse it, render it inline.
  `include` shares the caller's context; `render` gets a fresh one holding
  just its arguments (an array or object argument included).
  `context_set_include_root(ctx, "path/to/partials")` is required
  before render or an error is raised. Path traversal escapes are
  rejected via `std.fs.is_within_base`. Depth-limited at 100 to
  prevent infinite-include recursion.
- `{% layout 'parent' %}` + `{% block name %}…{% endblock %}` —
  Jekyll-style template inheritance, to any depth: a layout may itself have
  a layout. The most derived template that defines a block supplies it, and
  a cycle is an error.
- `{% extends 'parent' %}` — Django/Jinja alias for `{% layout %}`.
- `{{ block.super }}` — inside a block override, emits the same block as
  the next template up the chain defines it (its own `block.super`
  included).

### Filters

String, math, html, encoding, array and date. The complete list:

```
upcase  downcase  capitalize  strip  lstrip  rstrip  reverse  size
append  prepend  default  truncate  truncatewords  replace
replace_first  remove  remove_first  newline_to_br  strip_html
strip_newlines  escape  escape_once  url_encode  url_decode  slice
plus  minus  times  divided_by  modulo  at_least  at_most
abs  ceil  floor  round
md5  sha1  sha256  base64_encode  base64_decode
escape_xml  json_escape
split  join  first  last  sort  sort_natural  uniq  map  where
compact  concat
date
```

The numeric filters read their input as Liquid does: text that is exactly
`-?digits.digits` is a decimal, anything else is read as Ruby's `to_i`
reads it (`"12px"` is 12, `"abc"` is 0). `ceil`, `floor` and `round` give an
integer (`{{ 3.7 | floor }}` is `3`, `{{ -2.5 | round }}` is `-3`: halves
round away from zero). `round: N` rounds a decimal to N places and prints it
as Liquid does, without padding zeros but with at least one fraction digit
(`{{ 3.14159 | round: 2 }}` is `3.14`, `{{ 9.999 | round: 2 }}` is `10.0`),
and it rounds in decimal, so `{{ 1.005 | round: 2 }}` is `1.01`. `abs` keeps
the input's shape (`-5` is `5`, `-5.50` is `5.5`).

The arithmetic filters (`plus`, `minus`, `times`, `divided_by`, `modulo`,
`at_least`, `at_most`) read both sides the same way and compute exactly, as
Liquid's Ruby numbers do. Integers stay integers and do not overflow
(`99999999999 | times: 99999999999` is `9999999999800000000001`), and
division and modulo floor (`-7 | divided_by: 2` is `-4`, `-7 | modulo: 3`
is `2`). A decimal on either side makes the result a decimal, computed
exactly (`0.1 | plus: 0.2` is `0.3`, `0.3 | divided_by: 0.1` is `3.0`) and
printed as Ruby prints the Float nearest to it (`10 | divided_by: 3.0` is
`3.3333333333333335`; `1.0e+17` and `1.0e-05` past the range Ruby writes
in full). Dividing by zero is a render error.

The array filters read their input as Liquid's `InputIterator` does: an
array is its elements with nested arrays flattened into them, nil is no
elements, and any other value is the one element.

- `split: pattern` — Ruby's `String#split`: a single space splits on runs
  of whitespace, the empty pattern into characters, and empty strings at
  the end are dropped (`"a,b,," | split: ","` is `["a", "b"]`).
- `join: sep` — the elements' text, `" "` between them by default.
- `first` / `last` — an array's first and last element, a string's first
  and last character, an object's first `[key, value]` pair.
- `size` — an array's length, an object's key count, a string's length in
  bytes.
- `reverse` — an array reversed; a string's bytes reversed.
- `sort` / `sort_natural` (optionally `: "property"` to sort objects by) —
  numbers as numbers and strings byte by byte, nil last; sort_natural by
  text, ignoring case. Numbers mixed with strings, and booleans, arrays or
  objects, cannot be sorted, and `sort` says so as Liquid does.
- `uniq` (optionally `: "property"`) — the first of each set of equal
  elements; `1` and `"1"` stay two.
- `map: "property"` — each object's value for the property.
- `where: "property", value` — the objects whose property equals value;
  `where: "property"` keeps those whose property is true.
- `compact` (optionally `: "property"`) — drops nil elements.
- `concat: array` — the elements of both; an argument that is not an
  array is an error.

`default: value` gives `value` for nil, false (unless `allow_false: true`),
an empty array or object, and the empty string. For a string it also fires
on `"false"`, `"nil"` and `"null"` when `allow_false` is `"true"` — how
the string-typed context has always spelt a missing value.

`date: format` reads its input as a time — an ISO-8601 date or date-time
(`2024-01-02`, `2024-01-02T10:20:30Z`, `2024-01-02 10:20:30 +0530`), a
Unix timestamp (an integer, or a string of digits), or `now` / `today` —
and writes it through Ruby's `strftime` pattern: `%Y %m %d %H %M %S`, the
names `%B %b %A %a`, `%j %e %I %l %p`, the offset `%z` / `%:z`, `%Z`, ISO
weeks `%V %G`, `%s`, the combinations `%F %T %D %c`, and Ruby's flags
(`%-d`, `%^B`). A time written with an offset prints at that offset;
anything else prints at UTC. Input it cannot read as a time, and an empty
format, come back unchanged, as in Liquid. The renderer reads no clock of
its own: `now` is the time the host gave with
`context_set_now(ctx, unix_seconds)`, and a template that asks for `now`
before the host has set one gets a render error that says so. That keeps a
render deterministic, and keeps the module off `std.os`: the date code is
`std.time.calendar`, which needs no `--with=os` under `--emit=lib`.

Unknown filter names pass the input through unchanged (Shopify
behaviour, not an error). `divided_by:"0"` and `modulo:"0"` raise
render-time errors rather than crashing.

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
context_put_json(ctx: ptr, key: string, tree: ptr) -> string       // error
context_put_json_text(ctx: ptr, key: string, json: string) -> string  // error
context_set_now(ctx: ptr, unix_seconds: long)        // what `date` reads as now
context_set_include_root(ctx: ptr, root: string)
context_value_nodes(ctx: ptr) -> int                 // arrays + objects held
context_free(ctx: ptr)

render(t: ptr, ctx: ptr)                       -> (string, string)
render_to_strbuilder(t: ptr, ctx: ptr, sb: ptr) -> string  // (error)
template_free(t: ptr)                          // release a parsed template

partial_cache_new()                            -> ptr
context_set_partial_cache(ctx: ptr, cache: ptr)
partial_cache_free(cache: ptr)
partial_loads()                                -> int     // reads + parses so far
```

### Partial cache

Without a cache, every `{% include %}`, `{% render %}` and layout parent is
read from disk and parsed at every use. A cache parses each once, keyed by
its resolved path. The include root is still checked at every use, and
`{% render %}`'s fresh context shares the cache:

```aether
cache = liquid.partial_cache_new()
// for each page:
ctx = liquid.context_new()
liquid.context_set_include_root(ctx, "partials")
liquid.context_set_partial_cache(ctx, cache)
out, err = liquid.render(t, ctx)
liquid.context_free(ctx)
// once no context uses it:
liquid.partial_cache_free(cache)
```

A cache holds what it read. A partial edited on disk after being cached is
not re-read; use a new cache for that. `partial_loads()` counts reads and
parses, so the saving can be measured. A page including one partial 200
times, rendered 20 times, loads it 4,000 times without a cache and once
with one (162 ms against 39 ms on a Windows laptop).

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
value_to_string(v: string)   -> string        // a scalar's text
value_get_int(v: string)     -> int
value_get_float(v: string)   -> float
```

An array or an object is a node of its context, so it is bound with
`context_put_json` rather than built from these.

## Sandbox story

The Liquid renderer itself does **not** call `fs.read`. Only
`{% include %}` / `{% render %}` and `parse_file` do. When the
include tags are used, `context_set_include_root` must be called with
an explicit root directory; partials resolved against that root are
checked with `std.fs.is_within_base` so an attacker-supplied filename
like `'../../etc/passwd'` is rejected.

The context's own state — the include root and depth, the counters and
cycles, the partial cache, the value store — sits under keys that start with a
control character, which the lexer refuses inside a tag, so a template can
neither read nor write it.

Under `--emit=lib`, `import contrib.templating.liquid` requires
`--with=fs` — the module transitively imports `std.fs`, and Aether's
import gate refuses the transitive dependency without explicit
opt-in. So a host that links your `.so` and embeds Liquid renders
is making an explicit acknowledgement that its filesystem surface
is in scope. Without `--with=fs`, the build fails with the standard
capability-empty error. Nothing else is needed: the module reads no
clock, so it does not import `std.os`. Verified by
`tests/integration/liquid_sandbox_gate/`.

## Performance notes

- Parsing is single-pass over a token stream; render walks the tokens
  with no AST allocation per render.
- Strings are `std.string` (refcounted heap strings); the renderer
  uses `std.strbuilder` for output accumulation so output assembly
  is O(N).
- A value is a packed string: a kind letter and a colon in front of its
  payload. A bare string is packed once, when it is bound, so reading it
  costs a lookup. An array or object is a number into the context's store,
  checked against the store at every use.
- Unreclaimed arrays and objects are swept when the nodes made since the
  last sweep outnumber those that survived it, so the sweeping costs time
  in proportion to what the renders made.
- Includes read and parse their partial at every use unless the context
  has a partial cache (above), which parses each once.

## Testing

```
ONLY="templating/liquid/" make contrib-check
```

`make contrib-check` alone runs every contrib module's specs, as CI does.

## License

Same as Aether.
