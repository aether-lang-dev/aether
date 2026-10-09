- **`t = x; return t` hands over the same string wherever the function sits.**
  Whether a function returns an owned string is decided once and remembered,
  but an alias of a local was judged by the tracker table of whichever
  function was being emitted when the question was first asked. A function
  first asked about while a caller defined before it was emitted was taken
  as returning a borrowed string, and every call leaked the alias. The alias
  is now judged against the function's own body (#2629).
