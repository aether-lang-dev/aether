- **A string returned from inside a trailing block is now handed to the
  caller, not leaked.** `f() { s = string.concat(...); if c { return n, s } }`
  leaked `s` on every call: the classifiers that decide whether a function
  returns an owned string skipped the block as if it were a separate
  closure, although its `return`s leave the enclosing function. The function
  was classified as not returning a heap string, so the return did not hand
  `s` over and the caller never freed it. The classifiers now look inside a
  call's trailing DSL block (single and multi-value returns, builder and
  plain calls alike). Also fixed in any function body: a string a tuple
  return hands over on one path is now freed on a path that returns
  something else (`return 0, ""`), as single-value returns already did.
