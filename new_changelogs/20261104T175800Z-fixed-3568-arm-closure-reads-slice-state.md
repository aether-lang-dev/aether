- **A closure in a receive arm may read a state field of a slice type
  (#3568).** The closure copies the field when it is made, and the next
  store into the state releases what the field held, so a slice, which no
  count covered, was refused (E0600). A slice buffer is counted now: the
  copy takes a reference of its own, given back with the closure's
  environment, and reads what it was made with however often the state is
  stored over. It views the same buffer, as any copy of a slice does, so an
  element stored before the field is bound again is seen through it.
