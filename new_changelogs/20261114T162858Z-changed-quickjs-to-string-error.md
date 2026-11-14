- **`contrib.quickjs`: `to_string` returns `(string, error)`.** It returned
  `""` both for an empty string and for a conversion that threw (a Symbol,
  a throwing or interrupted `toString`), so a host could not tell them
  apart. `quickjs.to_string(q, h) -> (string, string)` gives `("", error)`
  on a failure, and now keeps NUL bytes in the string. **Migration:**
  `s = quickjs.to_string(q, h)` becomes `s, err = quickjs.to_string(q, h)`
  (or `s, _err = ...` where a failure may read as empty). `arg_string` is
  unchanged (nib-code ask aether-quickjs-async-eval-and-interrupt).
