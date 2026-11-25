- **A trailing block on a discarded call runs.** `_ = f() { ... }` emitted
  the call alone and dropped its block without a word, while the same call
  bound to a name ran the block with the value as its context. The discard
  now runs the block the same way, then throws the value away, freeing a
  fresh string.
