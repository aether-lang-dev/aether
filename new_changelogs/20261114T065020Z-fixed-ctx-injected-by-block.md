- **Argument checks line up for a call that ends in a block.** Whether a
  builder's `_ctx` is injected was decided by counting every closure out of
  the call, so `each(c, name) callback { ... }` for `each(_ctx, name, cb: fn)`
  looked injected and its arguments were checked one slot off; the ptr-as-string
  check (E0202) was skipped for every call ending in a block for that reason.
  The checker now uses codegen's rule (one argument fewer than the parameters,
  a trailing DSL block not counted), so E0202 covers `label(raw) { ... }` and
  `each(null, raw) callback { ... }`, and a float given to a `ptr` after an
  injected `_ctx` (`setp(1, 0.5)` in a block) is refused instead of reaching C,
  where the offset had been applied twice.
