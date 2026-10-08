# aether-jsonpath

[JSONPath](https://www.rfc-editor.org/rfc/rfc9535.html) for
[Aether](https://github.com/aether-lang-dev/aether) — a query language for JSON,
the way XPath is one for XML.

**Fully compliant with RFC 9535: 703/703 cases of the official
[JSONPath Compliance Test Suite](https://github.com/jsonpath-standard/jsonpath-compliance-test-suite).**

Ported from Boris Zhguchev's two prior implementations of the same RFC —
[jsonpath-rust](https://github.com/besok/jsonpath-rust) and
[zig-jsonpath](https://github.com/besok/zig-jsonpath). The AST follows the Rust
model, the recursive-descent parser follows the Zig one, and the cursor-list
evaluation model comes from `zig-jsonpath/src/query.zig`.

## Example

Given the RFC's bookstore document:

```json
{
  "store": {
    "book": [
      { "category": "reference", "author": "Nigel Rees",
        "title": "Sayings of the Century", "price": 8.95 },
      { "category": "fiction", "author": "Evelyn Waugh",
        "title": "Sword of Honour", "price": 12.99 },
      { "category": "fiction", "author": "Herman Melville",
        "title": "Moby Dick", "isbn": "0-553-21311-3", "price": 8.99 },
      { "category": "fiction", "author": "J. R. R. Tolkien",
        "title": "The Lord of the Rings", "isbn": "0-395-19395-8", "price": 22.99 }
    ],
    "bicycle": { "color": "red", "price": 19.95 }
  },
  "expensive": 10
}
```

| JSONPath | Result |
|---|---|
| `$.store.book[*].author` | the authors of all books |
| `$..author` | all authors |
| `$.store.*` | all things, both books and bicycles |
| `$.store..price` | the price of everything |
| `$..book[2]` | the third book |
| `$..book[-2]` | the second-to-last book |
| `$..book[0,1]` | the first two books |
| `$..book[:2]` | books from index 0 (inclusive) to 2 (exclusive) |
| `$..book[?@.isbn]` | all books with an ISBN |
| `$..book[?@.price<10]` | all books cheaper than 10 |
| `$..book[?@.price < $.expensive]` | books cheaper than the document's own threshold |
| `$..book[?match(@.title, '.*Rings')]` | books whose title matches a regex |
| `$..*` | every node in the document |

## Usage

```aether,run
import std.json
import std.jsonpath

main() {
    source = "{\"store\":{\"book\":[{\"title\":\"Sayings of the Century\",\"price\":8.95},{\"title\":\"Sword of Honour\",\"price\":12.99}]}}"
    doc, jerr = json.parse(source)
    if jerr != "" { println("bad json: ${jerr}"); return }

    // Values AND their normalized paths.
    results, err = jsonpath.query(doc, "$.store.book[?@.price < 10].title")
    if err != "" { println("bad path: ${err}"); return }

    n = jsonpath.result_size(results)
    i = 0
    while i < n {
        value = jsonpath.result_value(results, i)   // borrowed json pointer
        path  = jsonpath.result_path(results, i)    // e.g. $['store']['book'][0]['title']
        println("${path} = ${json.json_stringify_raw(value)}")
        i = i + 1
    }

    jsonpath.free_result(results)
    json.json_free(doc)
}
```
```output
$['store']['book'][0]['title'] = "Sayings of the Century"
```

### API

Three entry points, mirroring jsonpath-rust's `query` / `query_only_path` /
`query_with_path` trio:

| Function | Returns |
|---|---|
| `query(root, path) -> ptr!` | `(results, err)` — values *and* normalized paths |
| `query_values(root, path) -> ptr!` | `(list, err)` — borrowed JSON pointers |
| `query_paths(root, path) -> ptr!` | `(list, err)` — normalized path strings |
| `query_ast(root, ast) -> ptr` | a result set for a previously parsed AST |

Each returns the Go-style `(value, err)` pair Aether's stdlib uses throughout:
a non-empty `err` means the *path* failed to parse, and carries a
caret-annotated diagnostic.

Result-set accessors: `result_size`, `result_value(i)`, `result_path(i)`,
`free_result`.

For callers that need to reuse a query, the module also exposes:

| Function | Returns |
|---|---|
| `parse(path) -> ptr!` | `(ast, err)`; a reusable AST or a diagnostic |
| `free_query(ast)` | releases the AST returned by `parse` |

The parser has no process-global cursor or error slot, so separate parses may
run concurrently. `query_ast(root, ast)` borrows the AST for the duration of
the call and borrows the JSON values in its returned result set.

**Parse once, run many.** When the same path runs against many documents,
parse it once and skip the parser on every subsequent call:

```aether,run
import std.json
import std.jsonpath

main() {
    doc, jerr = json.parse("[{\"id\":1},{\"id\":2}]")
    if jerr != "" { println(jerr); return }

    ast, aerr = jsonpath.parse("$[*].id")
    if ast == null { println(aerr); return }

    results = jsonpath.query_ast(doc, ast)     // repeat per document
    println("matches: ${jsonpath.result_size(results)}")
    jsonpath.free_result(results)

    jsonpath.free_query(ast)
    json.json_free(doc)
}
```
```output
matches: 2
```

### Memory

Result sets **borrow** their JSON values from the document, which must outlive
them, and **own** their path strings. So: `free_result(results)` before
`json.json_free(doc)`, and copy a path with `string.copy` if it must outlive
the result set.

The same rule applies to the convenience functions: `query_values` returns a
list of borrowed JSON pointers, while `query_paths` returns a list whose path
strings are owned by that list. Free the list with `list.free`; never call
`json.json_free` on an individual value returned by JSONPath. To serialise a
selected value, pass the borrowed pointer directly to
`json.json_stringify_raw(value)` while the source document is alive.

## Supported syntax

Everything in RFC 9535:

- **Segments** — child (`.name`, `[...]`) and descendant (`..`)
- **Selectors** — name (`.name`, `['name']`, `["name"]`), wildcard (`*`),
  index (`[0]`, `[-1]`), slice (`[1:5:2]`, `[::-1]`), and filter (`[?...]`)
- **Unions** — `[0, 2]`, `['a', 'b']`, mixed kinds
- **Filters** — comparisons (`==` `!=` `<` `<=` `>` `>=`), logical operators
  (`&&`, `||`, `!`), grouping, existence tests, `@` (current) and `$` (root)
- **Function extensions** — `length()`, `count()`, `value()`, `match()`,
  `search()`, with the §2.4.3 well-typedness rules enforced at parse time
- **Normalized paths** (§2.7) for every match

Notable conformance details the suite checks and this implementation gets
right: `length()` counts Unicode *codepoints*, not bytes; `match()`/`search()`
use I-Regexp semantics (RFC 9485), where `.` means `[^\n\r]`; integers are
constrained to the I-JSON safe range, ±(2^53−1), and a slice keeps that whole
range when it is applied (`[::4294967297]` steps past the end, not by 1);
`-0` and leading zeros are rejected;
and `<=`/`>=` hold for unordered types via their equality half
(`null <= null` is true, `null < null` is false).

## Testing

The suites live beside the module and run standalone; the CTS is the
conformance gate. All four run as part of `make test-ae`:

```sh
ae run std/jsonpath/test_conformance.ae  # 703/703 — the official RFC 9535 suite
ae run std/jsonpath/test_parser.ae       # 34 specs — grammar, incl. negative cases
ae run std/jsonpath/test_query.ae        # 51 specs — evaluation and ownership
ae run std/jsonpath/test_concurrency.ae  # parser reentrancy across actor workers
```

The parser and query spec files use [`std.spec`](https://github.com/aether-lang-dev/aether/blob/main/std/spec/module.ae),
Aether's mocha-style BDD framework — `describe` / `it` via trailing blocks and
closures:

```aether,fragment
spec.describe(fw, "query") {
    spec.describe("slice selector") {
        spec.it("reverses on a negative step") callback {
            spec.assert_str_eq(all_json_of("[0,1,2,3,4]", "$[::-1]"),
                               "4|3|2|1|0", "reversed")
        }
    }
}
```

`tools/cts_runner.ae` is itself written in Aether against this library's public
API, so the conformance gate exercises the same surface a user would. The suite
is vendored under `compliance-test-suite/`; pass `-v` to print every failure
rather than the first fifteen.

The two ported spec files retain the upstream-friendly `*_test.ae` names,
which Aether discovers. The new concurrency suite uses the stdlib's common
`test_*.ae` form. When this directory is installed as `std/jsonpath`, the
implementation files remain under its `src/` directory and the facade in
`module.ae` provides the public namespace.

The parser reentrancy test uses two actor workers, each performing successful
and failing parses. The ownership smoke test is intended to be run under
Valgrind; the parser and query suites currently report zero invalid accesses
and zero definitely- or indirectly-lost blocks.

## Known limitations

- **User-defined function extensions** are not supported. RFC 9535's function
  registry is closed for conformance purposes, so an unregistered name is a
  parse error. jsonpath-rust allows custom extensions; that hook could be added
  behind an explicit registration API.

## License

MIT. Portions copyright (c) 2021-2026 Boris Zhguchev; portions copyright (c)
2026 Aether language developers. See [LICENSE](LICENSE).
The vendored compliance test suite carries its own license.
