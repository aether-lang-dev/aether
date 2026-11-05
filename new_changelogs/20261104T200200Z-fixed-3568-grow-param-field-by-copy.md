- **`append` to a struct parameter's slice field grows a copy (#3568).**
  The field is the caller's (parameters borrow), so it holds no reference of
  the function's own, and `append` saw the caller's one reference, took the
  buffer for unshared and reallocated it in place: `return append(b.items,
  v)` left the caller's `b.items` pointing at freed storage, and so did
  `b.items.push(v)`. A value its place does not own now takes a reference
  first, so the growth copies to storage of its own and the caller keeps its
  buffer; a push into a parameter's field makes the parameter a value of its
  own, released when the function returns.
