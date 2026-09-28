- **contrib.templating.liquid has the array filters (#2180).** `split`,
  `join`, `first`, `last`, `size`, `sort`, `sort_natural`, `uniq`, `map`,
  `where`, `compact`, `concat` and `reverse` work on arrays, read their input
  as Liquid's InputIterator does, and follow Ruby's semantics: `split` drops
  empty strings at the end, and `sort` puts nil last and refuses to order
  numbers against strings. An array flows through a chain unstringified
  (`s | split: "," | sort | join: "-"`). `default` also fires on false and
  on an empty array or object. Filters used to pass these names through
  silently as unknown.
