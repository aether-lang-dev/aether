# REPLY: contrib.quickjs: take strings in by length, and keep an error's text whole

**Re:** aether-guest-languages `asks/contrib-quickjs-length-aware-strings.md`

**RESOLVED** on the branch `batch/fix-runtime` (the release that carries it).

## What the module does now

The whole module is length-aware in both directions, the way std's own
externs are: the string parameters cross as `@aether string` (std.regex,
std.json and std.map do the same), so the C side reads the string's bytes and
length rather than a C string. No parallel `_len` functions: the Aether
signatures are the ones you already call.

| function | before | now |
|---|---|---|
| `new_string(q, s) -> int` | `JS_NewString`, cut at a NUL | every byte (`JS_NewStringLen`) |
| `eval`, `eval_async`, `eval_module(q, code, filename) -> (int, string)` | `strlen(code)`: ran up to the NUL and said nothing | every byte: a NUL in a string literal is a character, a raw NUL elsewhere is a `SyntaxError` |
| the filename of those three | a C string | still the engine's C string: **refused** when it holds a NUL, `(0, "TypeError: eval: the filename holds a NUL")` |
| `parse_json(q, text) -> (int, string)` | `1\0garbage` parsed as `1` | every byte: that is a `SyntaxError` |
| `throw_error(q, kind, msg) -> int` | `"%s"` | the message by its length, for every kind; `kind` compared by its length |
| `function(q, name, arity, cb)` | `JS_NewString(name)` | the name by its length |
| `get`, `set`, `get_global`, `set_global` names | `JS_*PropertyStr`: a shorter, different key | `JS_NewAtomLen`: the whole name is the key |
| `last_error(q)`, the error half of every tuple | `JS_ToCString` then `strdup`: `Error: x` | the text and its byte count: `Error: x\0y` |
| `to_json(q, h) -> string` | `string.copy` of a C string | by its length |

In C, `fail_` reads `String(e)` and `e.stack` with `JS_ToCStringLen` and keeps
`err_len` beside `err`; `qjs_error` is gone, replaced by `qjs_error_data` and
`qjs_error_len` (a C caller that still declares `qjs_error` fails to link,
which is the point). `qjs_to_json` returns a byte count and hands the bytes
out through `qjs_scratch`, as `qjs_to_string` already did. JS_Eval and
JS_ParseJSON want `code[n]` to be a NUL: a string always has one there; a
view into a longer string may not, so that case is copied first.

## UTF-8, explicitly

What the module hands out (`to_string`, `arg_string`, `to_json`, error text)
is always valid UTF-8. The engine writes a lone surrogate as its WTF-8 bytes
(`ED A0 80`); the module turns each into U+FFFD (`EF BF BD`, the same three
bytes, in place), the choice the bridge already made. JSON is the lossless
route: `to_json` writes a lone surrogate as `"\ud800"`. A character outside
the BMP crosses as four UTF-8 bytes one way and one code point (a surrogate
pair) the other. Bytes going in that are not UTF-8 become U+FFFD, one per bad
byte (the engine's `JS_NewStringLen` rule). README.md, "Strings, NUL and
UTF-8", has the table.

## Ownership

Unchanged in kind, now written on every changed function: strings passed in
stay the caller's (the engine copies what it keeps); strings returned are new
and the caller's. `qjs_error_data` and `qjs_scratch` are the runtime's, valid
until its next failure or the next string it hands out.

## Tests (seen failing first)

`contrib/quickjs/test_quickjs.ae`, "strings go in by their length" (8 cases)
and "UTF-8 at the boundary" (3 cases). On the old module: 26 failed
assertions across 9 of the 11 cases (the non-BMP case and the bad-byte case
already held), including your three repros (`new_string` gave 1 byte, the
error read `Error: x`, the script ran to its NUL). On the new one: 50 cases
pass, and the runtime holds no handles at the end.

## For the bridge (quickjs/aether_host_quickjs.c), after the release

- The hand-written externs (lines 64-85) must match: `qjs_eval(void*, const
  void* code, const void* filename, int)`, `qjs_throw(void*, const void* kind,
  const void* msg)`, `qjs_string(void*, const void*)`, `qjs_parse_json(void*,
  const void*)`, `qjs_get`/`qjs_set(..., const void* name, ...)`,
  `qjs_function(void*, const void* name, ...)`, where each `const void*` is an
  AetherString (`string_new_with_length(d, n)`, released after the call).
  Never pass a raw `char*` there: the helpers would ask its bytes whether they
  are a string header (#3297), and the bytes are the script's.
  `qjs_error` becomes `qjs_error_data` + `qjs_error_len`; `qjs_to_json`
  returns a count, the bytes in `qjs_scratch`.
- The joiner (line 222) and `new_string`'s piecewise construction (351-395),
  the thrower (220, 762-800) and the refusals `WHY_CODE`, `WHY_NAME`,
  `WHY_JSON` (190-192, 503, 612-660, 663, 711, 945) can go: `qjs_string`,
  `qjs_throw`, `qjs_eval`, `qjs_set` and `qjs_parse_json` carry the NUL.
  A filename stays a refusal, the engine's.
- `value_str`'s lone-surrogate scrub (344) is now the module's.
- The catcher (218, `caught_text` at 426) and `message_of`'s stack cut (408) are
  replaced by `last_exception` (REPLY-contrib-quickjs-op-budget-and-exception-handle.md).
