- **contrib.quickjs carries strings whole, both ways.** Strings went in as C
  strings, so a NUL cut them: `new_string(q, "a\0b")` made `"a"`, a script
  ran up to its first NUL and said nothing, `parse_json("1\0garbage")` was
  `1`, a property name was a shorter, different name, and an error was
  recorded as C text, so `throw new Error('x\0y')` read back as `Error: x`.
  Now `new_string`, `eval`, `eval_async`, `eval_module`, `parse_json`,
  `throw_error`'s message, `function`'s name and the names of `get`, `set`,
  `get_global` and `set_global` take every byte (`@aether string`), and
  `to_json`, `last_error` and every error text keep every byte. A filename
  holding a NUL is refused (`TypeError: eval: the filename holds a NUL`),
  never cut. What the module hands out is valid UTF-8: a lone surrogate,
  which the engine writes as WTF-8, is U+FFFD, and `to_json` is the
  lossless route for one (README, "Strings, NUL and UTF-8").
