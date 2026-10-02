# std.schema

Declarative, typed data **validation and coercion** for Aether.

A `record { … }` describes a set of typed fields with composable validation
rules. `parse(schema, input)` turns an untyped key→value map into a typed,
validated value-map **or** a list of structured errors. Validation and parsing
are a single act (the *"parse, don't validate"* philosophy): a successful parse
yields coerced values; a failure yields `{field, code, message}` entries with
Zod-style issue codes.

`std.schema` is **HTTP-agnostic** and depends only on `std.map`, `std.list`, and
`std.string`. Use it to validate an HTTP request body, a config file, a set of
CLI args, a JSON document read from disk, or any other untyped input.

## Quick start

```aether
import std.schema
import std.map(map_new, map_put_raw, map_free)

main() {
    person = schema.record() {
        schema.field("age", schema.INT) {
            schema.min(18)
            schema.max(120)
        }
        schema.field("name", schema.STR) {
            schema.present()
            schema.len(1, 80)
        }
        schema.field("role", schema.STR) {
            schema.one_of("admin,user,guest")
            schema.optional()
        }
    }

    input = map_new()
    map_put_raw(input, "age", "25")
    map_put_raw(input, "name", "Ada")

    values, errors = schema.parse(person, input)
    if schema.ok(errors) == 1 {
        // `values` is a map of coerced field name -> value
        print("valid!\n")
    } else {
        i = 0
        while i < schema.error_count(errors) {
            print("${schema.error_field(errors, i)}: ${schema.error_message(errors, i)} (${schema.error_code(errors, i)})\n")
            i = i + 1
        }
    }

    schema.values_free(values)
    schema.errors_free(errors)
    map_free(input)
    schema.schema_free(person)
}
```

## Field types

| Constant       | Accepts (lax coercion)                       |
|----------------|----------------------------------------------|
| `schema.STR`   | any string                                   |
| `schema.INT`   | a string parseable as an integer             |
| `schema.FLOAT` | a string parseable as a number               |
| `schema.BOOL`  | `true`/`false`/`1`/`0` (case-insensitive)    |

Coercion is **lax by default** (Pydantic-style): `"25"` is accepted for an
`INT` field. A value that cannot be coerced to the declared type produces an
`invalid_type` error and the field's other rules are skipped.

## Nested records and arrays

Two more types hold structure, and `parse_json` validates a `std.json` tree
against them:

| Type                    | Holds                                              |
|-------------------------|----------------------------------------------------|
| `schema.OBJECT`         | the fields declared in its block, a nested record  |
| `schema.array(T)`       | a list of `T`; `T` may be `OBJECT` or another array |

```aether
import std.schema
import std.json

main() {
    order = schema.record() {
        schema.field("customer", schema.OBJECT) {
            schema.field("email", schema.STR) { schema.email() }
        }
        // An array of records: the item's fields go straight in the block,
        // and the field's own min/max/len count the items.
        schema.field("lines", schema.array(schema.OBJECT)) {
            schema.min(1)
            schema.field("sku", schema.STR) { schema.present() }
            schema.field("qty", schema.INT) { schema.positive() }
        }
        // An array of scalars: the rules for each item go in items().
        schema.field("tags", schema.array(schema.STR)) {
            schema.optional()
            schema.max(5)
            schema.items() { schema.len(1, 20) }
        }
    }

    root, err = json.parse("{\"customer\": {\"email\": \"a@b.co\"}, \"lines\": [{\"sku\": \"X\", \"qty\": 0}]}")
    if err != "" { return }
    values, errors = schema.parse_json(order, root)
    i = 0
    while i < schema.error_count(errors) {
        // lines[0].qty: lines[0].qty must be positive
        println("${schema.error_field(errors, i)}: ${schema.error_message(errors, i)}")
        i = i + 1
    }
    json.json_free(values)
    schema.errors_free(errors)
    json.json_free(root)
    schema.schema_free(order)
}
```

- **Errors name their path** from the root: `customer.email`, `lines[0].qty`,
  `tags[2]`.
- **Values are a `std.json` object** of the fields that passed, typed: an `INT`
  is a JSON integer, a `BOOL` a boolean, an `OBJECT` an object, an array an
  array. Keys the schema does not declare are left out. Free it with
  `json.json_free`.
- **A JSON `null` is an absent field**, so an `optional()` field may be null.
- **Lax mode** also takes a number or boolean for a `STR` field as its text.
  Strict mode refuses that, and refuses a string for an `INT`, `FLOAT` or
  `BOOL` field.
- The flat `parse` cannot carry nesting: an `OBJECT` or array field it is given
  is an `invalid_type` error that points at `parse_json`.
- A rule an object or array cannot honour is refused when the schema is
  built, with a message that says where it belongs.

## Validators

Rules are composable builder calls inside a `field(…) { … }` block:

| Builder                     | Applies to        | Meaning                                             |
|-----------------------------|-------------------|-----------------------------------------------------|
| `min(n)`                    | INT/FLOAT / STR   | value `>= n` — or string length `>= n`              |
| `max(n)`                    | INT/FLOAT / STR   | value `<= n` — or string length `<= n`              |
| `len(lo, hi)`               | STR               | string length within `[lo, hi]`                     |
| `present()`                 | any               | value must be non-empty                             |
| `one_of("a,b,c")`           | any               | value must be one of the comma-separated set        |
| `email()`                   | STR               | value must look like an email address               |
| `positive()`                | INT/FLOAT         | value `> 0`                                         |
| `nonneg()`                  | INT/FLOAT         | value `>= 0`                                        |
| `pattern("kind:needle")`    | STR               | `contains` / `prefix` / `suffix` match (no regex)   |
| `one_of("a,b,c")`           | any               | value must be one of the comma-separated set        |
| `email()`                   | STR               | value must look like an email address               |
| `optional()`                | any               | absence is not an error (rules skipped when absent) |
| `default_to("v")`           | any               | when absent, fill `"v"` instead of erroring (implies optional) |
| `refine(\|v: string\| { … })` | any             | custom predicate: return `1` to pass, `0` to fail   |

**Transforms** rewrite the canonical value in declaration order (before the
checks that follow them) and never fail — the "parse, don't validate" payoff of
getting a normalized value back:

| Transform     | Effect                          |
|---------------|---------------------------------|
| `trim()`      | strip surrounding whitespace    |
| `lowercase()` | lowercase the value             |
| `uppercase()` | uppercase the value             |

```aether,fragment
schema.field("email", schema.STR) {
    schema.trim()        // "  Ada@X.COM " ...
    schema.lowercase()   // ... -> "ada@x.com" in the returned values
    schema.email()
}
schema.field("plan", schema.STR) {
    schema.default_to("free")
    schema.one_of("free,pro,enterprise")
}
```

`refine` takes a closure — use it for any check the built-ins don't cover
(the module intentionally carries no regex dependency; `pattern()` handles the
common contains/prefix/suffix cases without one):

```aether,fragment
schema.field("even", schema.INT) {
    schema.refine(|v: string| {
        n, _ = to_int(v)
        if n % 2 == 0 { return 1 }
        return 0
    })
}
```

> Note: pass `refine` a **closure literal** (`|v: string| { … }`), not a bare
> named function — bare-fn-into-an-imported-`fn`-param currently trips a
> compiler codegen gap.

Rules for a field are evaluated in order and **short-circuit on the first
failure** (one error per field per parse). Missing required fields and
type-coercion failures are reported before any other rule runs.

## Issue codes

Errors carry a stable, machine-readable `code` (mirroring Zod's vocabulary):

| Code             | Raised by                                    |
|------------------|----------------------------------------------|
| `missing`        | a required field absent from the input       |
| `invalid_type`   | value not coercible to the declared type     |
| `too_small`      | `min` / `len` lower bound                     |
| `too_big`        | `max` / `len` upper bound                     |
| `invalid_value`  | `present` empty / `one_of` not in set        |
| `invalid_format` | `email` (and other format checks)            |
| `custom`         | a `refine` predicate returned `0`            |

## JSON Schema projection

A schema is a *description*, not just a gate. `to_json_schema(s)` emits a
standard **draft-07 JSON Schema** string, so the same declaration can drive
OpenAPI docs, other-language validators, or form generators (the idea behind
Zod's `z.toJSONSchema()`).

```aether,fragment
s = schema.record() {
    schema.field("age", schema.INT)  { schema.min(18); schema.max(120) }
    schema.field("role", schema.STR) { schema.one_of("admin,user"); schema.optional() }
    schema.field("plan", schema.STR) { schema.default_to("free") }
}
js = schema.to_json_schema(s)   // owned string — free with string_free
```

produces:

```json
{"$schema":"http://json-schema.org/draft-07/schema#","type":"object",
 "properties":{
   "age":{"type":"integer","minimum":18,"maximum":120},
   "role":{"type":"string","enum":["admin","user"]},
   "plan":{"type":"string","default":"free"}},
 "required":["age"]}
```

Constraint mapping: `min`/`max` → `minimum`/`maximum` (or `minLength`/`maxLength`
for `STR`), `len` → `minLength`/`maxLength`, `one_of` → `enum`, `email` →
`"format":"email"`, `positive` → `exclusiveMinimum:0`, `nonneg` → `minimum:0`,
`default_to` → `default` (and the field drops out of `required`). It *emits*
JSON Schema; it does not consume external JSON Schema documents.

## API reference

### Types
- `schema.STR`, `schema.INT`, `schema.FLOAT`, `schema.BOOL`
- `schema.OBJECT`, `schema.array(T) -> int`

### Builders
- `record() -> ptr` — start a schema; the trailing block declares its fields
- `field(name, type) -> ptr` — declare a field; the trailing block adds its rules
  (and, for an `OBJECT` or `array(OBJECT)`, the nested record's fields)
- `items() -> ptr` — in an array field's block, the rules each item is checked against
- checks: `min(n)`, `max(n)`, `len(lo, hi)`, `present()`, `positive()`,
  `nonneg()`, `one_of(set)`, `email()`, `pattern("kind:needle")`, `refine(fn)`
- transforms: `trim()`, `lowercase()`, `uppercase()`
- field modifiers: `optional()`, `default_to("v")`

### Parse
- `parse(schema, input) -> (values, errors)` — `input` is a `*Map` of
  `string → string`; returns a coerced value-map and an error list (both always
  non-null, both owned by the caller)
- `parse_json(schema, root) -> (values, errors)` — `root` is a `std.json`
  object; returns a typed `std.json` object (free with `json.json_free`) and an
  error list whose fields are paths

### Projection
- `to_json_schema(schema) -> string` — emit a draft-07 JSON Schema (owned
  string; free with `string_free`). An `OBJECT` field is a nested `object`
  schema; an array is `array` with its `items` schema and `minItems`/`maxItems`.

### Result helpers
- `ok(errors) -> int` — `1` if there are no errors
- `error_count(errors) -> int`
- `error_field(errors, i) -> string`
- `error_code(errors, i) -> string`
- `error_message(errors, i) -> string`

### Cleanup
- `schema_free(schema)` — free a schema built with `record()`
- `values_free(values)` — free a value-map returned by `parse`
- `errors_free(errors)` — free an error list returned by `parse`

Every `parse` result must be released with `values_free` **and** `errors_free`.
`std.schema` is manual-memory and leak-clean under valgrind; the regression
suite (`std/schema/test_schema.ae`) runs zero-leak.

## Design credits

`std.schema` borrows its shape from three excellent MIT-licensed libraries:

- **[Zod](https://github.com/colinhacks/zod)** (© 2025 Colin McDonnell) — the
  `parse → (value | errors)` contract and the issue-code vocabulary.
- **[Pydantic](https://github.com/pydantic/pydantic)** (© Pydantic Services
  Inc.) — lax-vs-strict coercion.
- **[io-ts](https://github.com/gcanti/io-ts)** (© 2017 Giulio Canti) — decode
  as `input → success | failure`, with errors accumulated rather than thrown.
- **[Ash Framework](https://github.com/ash-project/ash)** (© 2019 ash
  contributors) — the declarative resource/attribute/validation block shape
  (`record { field { rules } }`). We keep validation HTTP-agnostic in `std`
  rather than adopting Ash's framework-owns-the-resource model; the web
  projection lives separately in `tinyweb.schema_api`.
