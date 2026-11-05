- **A string stored into a module-level `ptr` var is given a reference of
  its own (#3567).** `gp = s`, with `gp` a `ptr` global, stored the string
  as it stood: a string built in the statement (`gp = string.concat(a, b)`)
  was then owned by nothing and leaked, and a local's dangled once the
  local went. The store now gives the global the recorded reference
  `string.keep` hands over, so the string lives as long as the global holds
  it and `string.release_kept(gp)` gives it back. As with `keep`, a re-bind
  does not give the old one back: the program does, before the next store.
