- **A `match` whose value is a slice gives its place what each arm gives
  (#3568).** The whole `match` was copied into the place it was bound,
  assigned, stored or returned to, so an arm that gave a call's result
  (`vs = match k { 0 -> mk(4) _ -> xs }`) held that buffer twice and leaked
  it each time the arm ran. Each arm's value now goes into the place as an
  `if` branch's does: a call's result hands its reference over, a local or
  another place is copied, and an arm's value that ends a block is the
  match's, not a temporary of its statement.
