- **A module-level string `var` stored and read from two actors at once no
  longer frees a string twice or reads a freed one (#3522).** The counts of
  a shared string were atomic, but the var itself was not guarded: two
  stores at once both released the value they read and one new value
  leaked, and a reader could use a string a store on another thread had just
  freed. Each string `var` now has a small lock of its own: a store swaps
  the new value in under it and releases the old one after, and a read takes
  a reference of its own under it, which its statement gives back.
  A read pays for the lock and the count: a few nanoseconds for one reader,
  and readers on several cores at once contend (four actors reading one var,
  8M reads in all, took about 345 ms where the unlocked read took 25 ms).
  Give such a value to each actor, or keep it in a local, rather than
  reading the `var` in a hot loop.
