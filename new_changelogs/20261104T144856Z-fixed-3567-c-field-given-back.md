- **A string stored into a header-defined struct's string field can be given
  back (#3567).** A store into a field of an `extern struct ... @c_import`
  (`p.text = s`, or the field in a struct literal) gave C's field a
  reference of its own and then kept only the characters, dropping the
  string's header, so the reference could never be given back: every store
  leaked a string, and a C field updated in a loop grew the heap without
  bound. The reference is now recorded by the characters the field holds,
  with a count per string (the same string in two fields is two references),
  and `string.release_kept(p.text)` gives it back once C is done with it;
  nothing releases it behind C's back. A field read back is the string it
  was given, retained, so `old = p.text`, a store of the next value and
  `string.release_kept(old)` give the old one back. A pinned literal's
  characters and a plain C string are not recorded. `string.keep` records
  the reference it hands over the same way, and `string.release_kept` gives
  back only a recorded reference: a pointer nothing handed over (a literal,
  a copy, C's own characters, one already given back) is left alone, where
  a counted string not from `keep` was released, and a field's copy given
  back was freed twice.
- **Migrating:** `string.release_kept(p)` of a counted string that did not
  come from `string.keep` or a C field store no longer releases it; a
  program that used it as a plain release calls `string.release` instead.
