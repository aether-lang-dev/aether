# contrib.jq

The [jq](https://jqlang.github.io/jq/) language in Aether: a port of
[gojq](https://github.com/itchyny/gojq) (MIT, see `NOTICE`). Programs run
against `std.json` values, so jq slots into anything else that already
speaks `std.json`.

```aether
import std.json
import contrib.jq

main() {
    // One call: program, JSON text, outputs one per line.
    out, err = jq.query(".[] | select(.age > 30) | .name",
                        "[{\"name\":\"ann\",\"age\":41},{\"name\":\"bob\",\"age\":7}]")
    if err != "" { println(err) }
    println(out)                                  // "ann"

    // Or compile once and run against many std.json values.
    prog, cerr = jq.compile("{total: (.items | map(.price) | add)}")
    if cerr != "" { println(cerr); return }
    doc, _ = json.parse("{\"items\":[{\"price\":2},{\"price\":3}]}")
    text, rerr = jq.run_json(prog, doc)          // "{\"total\":5}\n"
    if rerr != "" { println(rerr) }
    print(text)
    json.json_free(doc)
    jq.free(prog)
}
```

## API

| Call | Returns |
|---|---|
| `jq.compile(src)` | `(program, "")`, or `(null, "syntax error: … at line L, position P")` |
| `jq.free(program)` | |
| `jq.set_var(program, name, value)` | binds `$name` for every run (jq's `--arg`/`--argjson`); `value` is donated |
| `jq.run(program, input, out)` | `(status, code, err)`: each output, caller-owned, appended to the `std.list` `out` |
| `jq.run_with_inputs(program, input, inputs, out)` | as `run`, with the `std.list` `inputs` feeding `input`/`inputs` (they pop from its front) |
| `jq.run_json(program, input)` | `(text, diagnostic)`: outputs as compact JSON, one per line |
| `jq.run_json_opts(program, input, indent, sort_keys, raw)` | as `run_json` with jq's `--indent`, `-S` and `-r` |
| `jq.query(src, json_text)` | `(outputs, diagnostic)`: compile, parse, run, render; outputs joined by newlines |
| `jq.parse_stream(text)` | `(list, "")`: whitespace-separated JSON texts (or JSON Lines) as owned values |
| `jq.render(out, indent, sort_keys, raw)` / `jq.free_values(list)` | render or release a list of outputs |
| `jq.to_json(v)` / `jq.to_json_indent(v, indent, sort_keys)` | jq's serialisation of one value |
| `jq.error_message(err)` | a raised value as jq prints it: the string, or JSON plus `(not a string)` |

`status` is `jq.RESULT_OK`, `jq.RESULT_ERROR` (code 5, `err` is the raised
value) or `jq.RESULT_HALT` (`halt`/`halt_error`; `code` is the exit code,
`err` the message or null). A non-null `err` belongs to the caller. `input`
values are borrowed: the program never changes or frees them.

## Command line

`example_jq.ae` is a small `jq` over this module: it reads JSON texts from
stdin and supports `-n -s -c -r -j -S -e --indent n --arg --argjson`, with
jq's exit codes (2 usage, 3 compile error, 5 runtime error, `halt_error`'s
code, `-e`'s 1 and 4).

```sh
echo '{"a":[1,2,3]}' | ae run contrib/jq/example_jq.ae -- -c '.a | map(. * 2)'
[2,4,6]
```

## Coverage

The whole jq 1.7 language: paths and path expressions, every assignment
operator, `reduce`/`foreach`, `try`/`catch`, `label`/`break`, destructuring
with `?//` alternatives, `def` with filter and `$` parameters, recursion,
string interpolation and `@format` strings, `$__loc__`, `$ENV`. Output
matches gojq's: numbers print as the shortest text that reads back the same
double (`0.30000000000000004`, `1e+21`), object keys keep insertion order,
and errors carry jq's messages.

The builtin library, natives in `eval.ae` and the rest in jq in `prelude.ae`:
`length utf8bytelength keys has in map map_values path paths leaf_paths
getpath setpath delpaths del pick to_entries from_entries with_entries select
recurse env builtins input inputs debug stderr input_filename splits split
join ascii_downcase ascii_upcase ltrimstr rtrimstr trimstr trim ltrim rtrim
startswith endswith explode implode tostring tonumber tojson fromjson type
infinite nan isinfinite isnan isnormal isvalid sort sort_by group_by min max
min_by max_by unique unique_by reverse contains inside indices index rindex
flatten range floor sqrt pow log and the rest of jq's math, add any all
first last nth limit until while repeat isempty error halt halt_error
combinations walk transpose tostream fromstream truncate_stream bsearch
IN INDEX abs toarray ascii todate fromdate now mktime gmtime localtime strftime
strptime dateadd datesub date test match capture scan sub gsub splits
@text @json @html @uri @csv @tsv @sh @base64 @base64d @base32 @base32d`.

Regular expressions are PCRE2 (through `std.regex`) rather than Oniguruma;
the flags `g i x s n` work, and offsets and lengths are in codepoints as in
jq.

Not supported:

- Modules: `import`, `include`, `modulemeta`, `get_search_list`.
- Streaming input (`--stream`, `--seq`); the in-memory `tostream`,
  `fromstream` and `truncate_stream` work. `input_line_number` is always 0
  and `input_filename` is null.
- Arbitrary-precision numbers: every number is a double, as in gojq without
  big-integer support, so integers above 2^53 lose precision.
- Time zones: `localtime`, `strflocaltime` and `mktime` work in UTC.
  `strptime` understands the numeric directives and `%a %A %b %B %h %p %Z %z
  %s %T %R %F %D %%`.
- Unbounded recursion: the evaluator recurses on the C stack, about 2 KiB per
  jq call. When less than 256 KiB of the thread's stack is left, a call
  raises `function call depth exceeded` instead of overflowing it. The depth
  that allows depends on the stack: around 3500 calls on an 8 MiB Linux main
  thread, around 250 on a 1 MiB Windows one. Where the platform can't report
  its stack, calls stop at 300 deep.

## How it is built

| File | Role |
|---|---|
| `value.ae` | jq's view of a `std.json` value: total order, deep copy, serialiser, codepoints |
| `lexer.ae`, `parser.ae`, `ast.ae` | source to syntax tree, jq's precedence table |
| `eval.ae` | the evaluator and the native builtins |
| `prelude.ae` | builtins written in jq, installed under the program's own definitions |
| `module.ae` | this facade |
| `aether_jq.c` | number formatting, `environ`, and the libm functions `std.math` lacks |

The evaluator is continuation-passing: evaluating an expression calls a
*sink* once per output, so jq's generators (`,`, `.[]`, `range`, a function
yielding many values) need no coroutines, and every frame is released by the
call that made it. Each emitted value is an owned copy, which keeps lifetimes
local at the cost of copying on each pipe stage. `path(f)` and the assignment
operators run the same evaluator in *path mode*, where every value travels
with the path it was read from.

## Tests

The specs follow the test pyramid: many small unit specs at the bottom, a
few API-level ones at the top.

| Spec | Covers |
|---|---|
| `test_value.ae` | ordering, equality, copying, number formatting, codepoints |
| `test_lexer.ae` | tokens, string parts, lexical errors |
| `test_parser.ae` | tree shapes and precedence, rejected programs |
| `test_eval.ae` | the language: generators, operators, variables, patterns, reduce/foreach, functions, errors, labels |
| `test_paths.ae` | path expressions, path builtins, every assignment operator |
| `test_builtins.ae` | the builtin library, regex, formats, math, dates |
| `test_jq.ae` | the facade: compile, run, result codes, halt, variables, inputs, rendering |

```sh
ae run contrib/jq/test_eval.ae
.github/scripts/contrib_check.sh            # all contrib specs
VALGRIND=1 ONLY="jq/" .github/scripts/contrib_check.sh   # with the leak gate
```
