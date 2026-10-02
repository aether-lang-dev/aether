- **A trailing block on a call used inside an expression runs.**
  `return build() { ... }` and `f(build() { ... })` used to compile to the
  bare call, and the block was dropped without a diagnostic. Before, only a
  declaration, an assignment or an expression statement ran the block, so
  a `std.schema` record returned from a function had no fields. The block
  now runs wherever the call sits, once, before its value is used. For a
  builder it configures the call; for any other call it takes the call's
  value as its context.
