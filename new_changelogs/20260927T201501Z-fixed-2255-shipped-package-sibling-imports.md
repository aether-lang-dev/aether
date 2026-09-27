- **Two packages that each import their own `parser` no longer share one.**
  std.jsonpath and contrib.jq both `import parser` from inside the package,
  and the module registry was keyed by that bare name, so whichever loaded
  first answered for both. Importing std.jsonpath then contrib.jq failed to
  compile ("'free' is not exported from module 'parser'"); the other order
  compiled and `jsonpath.query` silently failed. A package's own files are
  now registered by their place in it (`contrib.jq.parser`), and a file
  already loaded under another name binds to that module rather than
  loading twice. A module now keeps its short namespace when its externs
  need it (`extern math_floor` in std.math) rather than for being shipped,
  so two pure-Aether modules that share a last segment both take their full
  path. `make check-docs` fails if two shipped modules ever need the same
  one. contrib.jq's internal `Parser` struct is now `JqParser`, since struct
  names are one namespace and std.jsonpath has a `Parser` too (#2255).
