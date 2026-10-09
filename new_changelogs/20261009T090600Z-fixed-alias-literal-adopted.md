- **A list no longer adopts, and frees, a string literal bound through an
  alias.** `g = x; list.add(xs, g)` with `x` only ever holding literals
  stored the literal as the list's own, and `list.free` freed it (a crash):
  the local behind the alias counted as owned because it had a tracker,
  which every string local has. It now counts as owned only when its body
  binds it from a fresh string, or it is a parameter the function keeps a
  reference of its own to, however long the chain of aliases (#2642).
