- **A store into a slice's element whose value grows the slice lands in
  the slice (#3568).** `rows[0] = grow()`, where `grow` appends to `rows`,
  took the element's address before the value was made, and stored into
  the buffer the growth had left (or freed). A value that calls anything is
  made first now, and the element's place taken after it; a panic in the
  place's bounds check still gives the value back.
