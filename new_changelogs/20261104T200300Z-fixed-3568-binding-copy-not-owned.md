- **A receive arm's or a pattern's slice binding frees nothing and grows a
  copy (#3568).** A receive arm's field binding is a copy of the message's
  field, and the binding of a `match` arm whose body is no block a copy of
  what was matched; neither holds a reference of its own. `free(items)` in
  an arm gave back the message's reference, which the message then gave
  back again, `items.push(v)` reallocated the message's buffer under it, and
  `some(v) -> free(v)` freed the buffer under the value matched. Such a
  `free` gives back nothing now, and a binding the body binds again or pushes
  into takes a reference of its own for the body, so the growth copies and
  nothing leaks.
