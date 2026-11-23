- **`const NAMES = ["a", "b"]` now says to write `const NAMES[] = [...]`.**
  An array literal as a plain `const` initialiser was refused with E0200's
  message about function calls being re-evaluated at each use, which the
  literal is not, and the error never named the spelling that works: an
  array `const` has brackets on its name and is static storage, which a
  function may return. naeb's port built its tables with `make` and `push`
  until it found the form. The error and its help now name it, and point at
  `docs/compile-time-eval.md`. (The E0900 error for `return ["a", "b"]`
  already named it.)
