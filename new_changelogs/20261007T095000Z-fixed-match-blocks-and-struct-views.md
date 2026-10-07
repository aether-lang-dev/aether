- **A `match` arm whose body is a `{ ... }` block yields its last value
  (#2496).** In `m = match x { 1 -> { t = ...; t } _ -> "other" }` the block
  ran but nothing assigned `t` to `m`: the binding kept its previous value
  (unset on a first binding) and the string the block built leaked. A block
  arm now yields its last expression, or a nested `match`, to the result of
  a binding, a reassignment or a `return match`, before the block's defers
  run. A `match` statement inside the block no longer assigns to the outer
  result either: it used to, which failed to compile when the types differed.
- **Containers and structs keep their own copy of a value something else
  owns (#2497).** `list.add(l, r.name)` and `map.put(m, k, r.name)` stored
  the field's pointer, which reassigning `r.name` or leaving `r`'s scope
  freed; they now take their own reference, as does an `if` whose arm is a
  field or a local. A struct held by value inside another (`o.inner`) is
  released with its holder: replacing `o` or leaving its scope used to leak
  `o.inner`'s strings. `b = a` for a struct with string fields freed each
  string twice, and `x = o.inner`, `return o.inner` and `Wrap { o: o }`
  borrowed a struct that was then freed; the struct is now moved out of a
  local on its last use and copied otherwise. A tuple position or `T!`
  value returned from an `if` with freshly built arms is handed over
  instead of copied and leaked.
