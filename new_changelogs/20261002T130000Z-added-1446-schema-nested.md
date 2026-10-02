- **std.schema validates nested records and arrays.**
  - **New types.** `schema.OBJECT` holds a record of its own, declared in
    the field's block. `schema.array(T)` holds a list of `T`, and arrays
    nest. A field's min, max and len count an array's items, and the rules
    for each item go in `items()`.
  - **parse_json.** `parse_json(schema, root)` validates a std.json tree
    against such a schema. It returns the passing fields as a typed
    std.json object, with ints as integers and bools as booleans, and each
    error names its path: `customer.email`, `lines[2].qty`, `tags[0]`.
  - **JSON Schema.** `to_json_schema` emits nested `object` schemas and
    `array` with `items`, `minItems` and `maxItems`.
  - **Misplaced rules are refused.** A rule an object or array cannot honour
    is refused when the schema is built, with a message saying where it
    belongs.
  - **tinyweb.** tinyweb's `schema_api` now validates request bodies through
    `parse_json`, nested objects and arrays included. A 422's JSON pointer
    names the failing field's path (#1446).
