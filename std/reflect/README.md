# std.reflect

Read the field table `@derive(schema)` emits for a struct: its name and size,
and each field's name, type, kind, offset, size and attributes.

A game engine's components are structs, and several systems walk their fields
by name: the editor's inspector draws a row per field, the save and scene
files write and read each one, the network replicates the ones that changed,
and an agent asks for `Player.Health.value`. Unity gets this from C#
reflection, Godot from `ClassDB`, Unreal from `UPROPERTY` codegen. Without
it, every component registered each field by hand beside its struct
(`component.float_field(k, "rate", offsetof(Bob, rate), 2.0)`), a second copy
of the struct kept in step by hand.

`@derive(schema)` is the language half: the compiler emits the table as
static constant data, from C's own `offsetof` and `sizeof`, and
`<StructName>_schema()` returns it (see `docs/language-reference.md`,
"`@derive(schema)`"). This module is the reading half. Attributes written
after a field's type (`rate: float @range(0.0, 10.0) @default(2.0)`) are
carried into the table, so defaults, ranges and tooltips live on the field.

```aether,run
import std.reflect
import std.mem

@derive(schema)
struct Bob {
    rate: float @range(0.0, 10.0) @default(2.0)
    height: float @default(6.0)
    hops: int @range(0, 3)
    label: string @tooltip("shown in the inspector")
}

main() {
    s = Bob_schema()
    println("${reflect.name(s)}: ${reflect.field_count(s)} fields")
    i = 0
    while i < reflect.field_count(s) {
        kind = reflect.kind_name(reflect.field_kind(s, i))
        line = "  ${reflect.field_name(s, i)}: ${reflect.field_type(s, i)} (${kind})"
        r = reflect.attr_index(s, i, "range")
        if r >= 0 {
            line = "${line} in [${reflect.attr_text(s, i, r, 0)}, ${reflect.attr_text(s, i, r, 1)}]"
        }
        println(line)
        i = i + 1
    }

    // A generic loader: apply every float field's @default through the table.
    b = heap.new(Bob)
    i = 0
    while i < reflect.field_count(s) {
        d = reflect.attr_index(s, i, "default")
        if d >= 0 && reflect.field_kind(s, i) == reflect.KIND_FLOAT {
            mem.set_float64(b, reflect.field_offset(s, i), reflect.attr_float(s, i, d, 0))
        }
        i = i + 1
    }
    println("rate=${b.rate} height=${b.height}")
    heap.free(b)
}
```
```output
Bob: 4 fields
  rate: float (float) in [0.0, 10.0]
  height: float (float)
  hops: int (int) in [0, 3]
  label: string (string)
rate=2 height=6
```

## Fields

Fields are numbered in declaration order from 0: the same index a
`std.observe` field observer receives when that field is stored (#2299), so
a replicator can keep a changed-field bitmask and look each bit up here.

- `field_offset` is the byte offset std.mem's accessors take;
  `field_size` is the field's size (an array's whole size). The width of an
  integer or float kind is its size.
- `field_type` is the type as written: `float`, `*Node`, `int[4]`, `Vec[]`.
- A `KIND_ARRAY` field has `field_len` elements of `field_elem_kind`; a
  `KIND_SLICE` (`T[]`) has an element kind and no fixed length.
- A field that holds an Aether struct by value (`KIND_STRUCT`), points at one
  (`KIND_PTR` to `*T`), or is an array or slice of one names that struct's
  table: `field_schema(s, i)`. The nested struct need not derive schema
  itself. A struct that points at itself names its own table.
- A `KIND_STRING` field's `field_heap_offset` is the offset of the `int` that
  records whether the struct owns the string (1: a heap string it frees on
  reassignment and when destroyed). A generic writer that stores a heap
  string frees the old one when that flag was 1, then sets it to 1; one that
  stores a string it does not hand over sets it to 0.

## Attributes

`@name` or `@name(arg, ...)`, any number after a field's type. The compiler
does not interpret them. An argument is a literal: a number (optionally
negative; decimal, `0x`, `0o` or `0b`), a string, `true` or `false`. Every
argument has three readings: `attr_int` (an int's value, a float's
truncation, a bool's 0 or 1), `attr_float` (an int converted, a float's
value), and `attr_text` (a string's value, a number or bool as written).
`attr_arg_kind` says which it was.

A field names each attribute once, and attributes on a struct without
`@derive(schema)` are a compile error: nothing would carry them.

## Safety

Every accessor is safe on a null table and on any index out of range: a name
reads as `""`, a count or number as `0`, an index lookup as `-1`, a nested
table as `null`. The tables are constant and live as long as the program;
nothing is allocated or freed.

## API

| Call | Returns | Notes |
|---|---|---|
| `name(s)` | `string` | the struct's name |
| `size(s)` | `int` | `sizeof` the struct |
| `field_count(s)` | `int` | |
| `field_index(s, field_name)` | `int` | `-1` when absent |
| `field_name(s, i)` | `string` | |
| `field_type(s, i)` | `string` | the type as written |
| `field_kind(s, i)` | `int` | a `KIND_*` constant |
| `field_elem_kind(s, i)` | `int` | an array's or slice's element kind |
| `field_len(s, i)` | `int` | a fixed array's length, else `0` |
| `field_offset(s, i)` | `int` | byte offset; `-1` out of range |
| `field_size(s, i)` | `int` | bytes |
| `field_heap_offset(s, i)` | `int` | a string's ownership flag; `-1` otherwise |
| `field_schema(s, i)` | `ptr` | the nested struct's table, or `null` |
| `attr_count(s, i)` | `int` | |
| `attr_index(s, i, attr_name)` | `int` | `-1` when absent |
| `has_attr(s, i, attr_name)` | `bool` | |
| `attr_name(s, i, a)` | `string` | |
| `attr_arg_count(s, i, a)` | `int` | `@hidden` has 0 |
| `attr_arg_kind(s, i, a, j)` | `int` | an `ARG_*` constant; `0` when absent |
| `attr_int(s, i, a, j)` | `long` | |
| `attr_float(s, i, a, j)` | `float` | |
| `attr_text(s, i, a, j)` | `string` | |
| `kind_name(kind)` | `string` | `"float"`, `"struct"`, ... |

Kinds: `KIND_OTHER` (0: optional, tuple, sum, bitset, SIMD), `KIND_BOOL`,
`KIND_BYTE`, `KIND_INT`, `KIND_LONG`, `KIND_UINT8`, `KIND_UINT16`,
`KIND_UINT32`, `KIND_ULONG`, `KIND_FLOAT` (a C double), `KIND_F32`,
`KIND_LONGDOUBLE`, `KIND_DURATION`, `KIND_STRING`, `KIND_PTR`, `KIND_STRUCT`,
`KIND_ARRAY`, `KIND_SLICE`, `KIND_ENUM`, `KIND_FN`. Argument kinds:
`ARG_INT`, `ARG_FLOAT`, `ARG_STRING`, `ARG_BOOL`.

The raw externs (`aether_schema_name`, `aether_schema_field_count`, ...) are
the same calls with C conventions (a missing name is `NULL`), for a host that
reads tables from C: `runtime/aether_schema.h` declares them with the table
layout.
