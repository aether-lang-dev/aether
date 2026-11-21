- **A function returning its own untyped local or parameter no longer
  returns a constant's type.** `same(f) { return f }` beside `const f = 7`
  was inferred to return an int, so `same("word")` was refused ("'same'
  returns int ..., but this value is string"), and `get(xs) { f = pick(xs);
  return f }` beside `const f = "text"` returned a string. Return types are
  inferred after a function's walk, when its own names are gone, and a
  returned name the walk had left untyped was looked up among the
  program's. They are now looked up beneath the function's own names.
