- **A store into a closure's string cell takes the value (#2514).** `s = p`
  inside a closure, where `p` is the caller's string the closure captured,
  stored the pointer as it stood: the env held its own reference and the
  cell adopted the same one without taking it, so both released it at
  scope exit and the caller's string was freed under the caller. The cell
  now takes what it holds the way every owning slot does: a borrowed value
  is copied or retained, a fresh one adopted. A match arm or a tuple
  destructure that binds such a variable stores the same way, and a
  function that returns such a variable hands the caller a copy, since
  the cell is released at the function's exit.
