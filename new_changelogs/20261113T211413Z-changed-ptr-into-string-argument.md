- **A `ptr` passed to an Aether function's `string` parameter is E0202.**
  The callee read whatever the pointer pointed at as text: nib passed a parsed
  JSON node to `long_of(text: string)`, which compiled and read garbage. A
  `ptr` bound to a `string` local was already refused; the argument now is
  too, naming the parameter and the typed accessor (`map.get_string`,
  `list.get_string`, `string.from_ptr`). `null`, a `string.*` producer and an
  `extern`'s `string` parameter still take one. The in-tree callers it found
  (std.spec's reports, std.clapae, std.schema, liquid, tinyweb, collate,
  vkgen and some tests) read their strings with the typed accessors now;
  `schema.parse` reports an input value that is not a string, and
  `clapae.parse` copies the process's argv into strings (nib-code ask
  ptr-passed-as-string).
