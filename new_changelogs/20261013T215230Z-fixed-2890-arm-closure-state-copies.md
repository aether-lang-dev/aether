- **A receive arm's closure is refused a state field it cannot copy.**
  Since #2890 a closure made in a receive arm copies the state fields it
  reads. The copy of an `fn[N]` or of an array of structs that own strings
  took none of the elements' references, so the next store into the state
  freed what the closure read: wrong values, or a crash. Reading such a
  field (or an optional, a tuple or a slice) from an arm's closure is now
  a compile-time error naming the field, its type and the way out: read
  what the closure needs into arm locals first. Scalars, enums, bitsets,
  `ptr` and actor references, strings, closures, structs and fixed-size
  arrays of scalars, handles, plain structs or strings are still copied
  (#2890).
