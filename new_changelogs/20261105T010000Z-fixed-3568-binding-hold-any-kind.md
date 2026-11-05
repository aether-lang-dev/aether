- **A receive arm's or a `match` arm's slice binding bound again from a
  slice that owns keeps the message's buffer (#3568).** `Give(items) -> {
  n = items.len; xs = make([]int, 2); items = xs }` gave back the message's
  reference, which the message then gave back again (a heap corruption),
  and leaked `xs`'s buffer: the binding took no reference of its own once
  the analysis saw an owner moved into it. It takes one now whatever it
  becomes.
