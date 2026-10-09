- **A function written as several clauses hands over its strings like any
  other.** Ownership was worked out from its first clause alone and its
  clause bodies skipped the setup a single function's body gets. A caller of
  `label(n) when n > 0 -> string { ... }` / `label(n) -> string { return "lit" }`
  freed the literal (a crash), or leaked the fresh string when the literal
  clause came first; a clause's string locals were never freed; a later
  clause that stored a `string` argument had it freed under it by the
  caller; and a clause set that returns nothing was defined `int` against a
  `void` prototype, which gcc rejected. Every clause is now read: one owned
  result makes every clause hand over an owned string, each clause body
  frees its locals, and a parameter is kept when any clause keeps it (#2627).
