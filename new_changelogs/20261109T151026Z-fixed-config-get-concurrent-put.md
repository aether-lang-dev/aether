- **`config.get` is safe under a concurrent `config.put` of the same key.**
  `get` handed back a borrowed pointer into the store, which the module
  wrapper copied only after the store's read lock was released, so a `put`
  on another thread could free the value in between: the reader copied
  freed, often reused, memory (heap-use-after-free under ASan; torn values
  without it). The store now keeps each value as a counted string, published
  before it is stored, and `get` / `get_or` return a reference of the
  caller's own, taken under the read lock (`-> string @heap`), released at
  scope exit like any owned string. The Aether API is unchanged; C callers
  of `aether_config_store_get` / `_get_or` now own the result and release it
  with `string_release`.
