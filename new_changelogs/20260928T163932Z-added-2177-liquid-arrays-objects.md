- **contrib.templating.liquid binds arrays and objects (#2177).**
  `context_put_json(ctx, key, tree)` copies a `std.json` tree into the
  context, and `context_put_json_text` parses JSON text first. A number
  written as an integer stays an integer, and the caller's tree is its
  own to free. Arrays and objects live in a store the context owns, and a
  packed value names one by number, checked at every use. The reserved raw
  pointer form it replaces could be forged by a bound string. Arrays and
  objects a render makes and no binding keeps are reclaimed as an
  outermost render returns, so a reused context does not grow. `{{ array }}`
  prints its elements, `{{ object }}` prints as Liquid inspects a hash, and
  `==` compares both element by element. `contains` finds an element or a
  key, and `x == empty` / `x == blank` test x as Liquid does.
