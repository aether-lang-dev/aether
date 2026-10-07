- **Postfix `i++` / `i--` yields the value from before the step (#2457).**
  The parser built the same node for `i++` as for `++i`, so wherever the
  value was used it was the new one: `j = i++` gave 6 for `i = 5`,
  `xs[k++]` skipped the first element, and `while n-- > 0` ran one
  iteration short, with no diagnostic. The node is now marked postfix and
  reaches C as postfix. `x++` / `x--` on a variable a closure captures now
  also promotes it to a shared cell as `x += 1` does, so the closure's
  increment is no longer lost when it returns. A `++` / `--` at the start of
  a line begins a new statement instead of applying to the previous line's
  operand.
- **A `defer` in a `match` arm runs when that arm ends, only if it was taken
  (#2459).** A match arm's block had no defer scope of its own, so its
  defers ran at the end of the enclosing function or loop body whichever arm
  had been taken, and in a loop on every iteration. An arm block is now a
  scope like an `if` / `switch` arm, which also stops a name declared in one
  arm leaking into the next (where it compiled to an assignment to an
  undeclared C variable).
- **A closure can reassign its own parameter (#2462).** `|n: int| { n = n + 1 }`
  stopped the C compiler with "'n' redeclared as different kind of symbol",
  and a string parameter was re-declared as `NULL`: the closure body did not
  count its parameters as declared names. It now does, as a function does.
- **A fixed-size array can be captured by a closure or held as actor state
  (#2464).** `int[3] arr` captured by a closure, or `state int[4] hist` in an
  actor, was emitted as the field `int[3] arr;`, which is not C. Both now
  use the declarator `int arr[3]`, the capture copies the array into the
  closure's environment, and an actor's spawn zeroes the field and sets the
  elements of an array-literal initializer.
- **`state uint8 x`, `uint16`, `uint32`, `int64`, `f32` and `ptr` state
  fields are declared, and `ptr p = null` declares a local (#2465).** Type
  names that lex as identifiers were not recognised after `state`: `state
  uint8 b8 = 250` declared a field named `uint8` and left `b8 = 250` as a
  stray statement ("'b8' undeclared"), and `state ptr p` was dropped. A
  state declaration now takes the same `TYPE NAME` shape as a typed local.
  `ptr p = null` as a statement was rejected with "Undefined variable
  'ptr'".
- **An actor state field's C type no longer depends on its name (#2466).** A
  field whose name ended in `_ref` was always `void*`, so `state self_ref = 0`
  used as a number did not compile. A field is now a pointer when the program
  uses it as one, inside the actor or as `r.field` anywhere else: it is the
  target of a `!` / `?` send, it is assigned a `ptr` or an actor reference,
  or it is passed as a message field of either type. The `state next = 0`
  ... `a.next = b` ... `next ! Msg {}` pattern the suffix stood for keeps
  working, under any name. `my_ref` is no exception: spawn sets it to the
  actor's own address only when it is used as a reference, and one used as
  a number is a number.
- **A string literal in a function-clause pattern compares by content
  (#2467).** `greet("bob") -> ...` was emitted as `if (_arg0 == bob)`: an
  undeclared name, and even when quoted a pointer comparison. The pattern is
  now a quoted, escaped C string compared with `string_equals`, as a `match`
  string arm is.
- **A closure's write to a captured variable through `++` / `--`, a field
  or an element reaches the variable (#2458).** Only a bare `n = ...` /
  `n op= ...` promoted a capture to a shared cell, so `h = || { n++ }`
  called twice left `n` at 0, and `p.x += 10` in a closure changed the
  closure's own copy of `p`. `n++` now promotes `n`, and a field or element
  write (`p.x = v`, `p.inner.y *= 3`, `b.vals[i] = v`) promotes a struct
  held by value; a `heap.new` box is written through its pointer, which is
  shared already, and stays as it is. A promoted struct with `string`
  fields stores into them through the cell, and the cell's last holder
  frees the strings the struct owns, as its scope exit does for a local.
  Writing to a captured bare fixed-size array, an element or the whole
  array, is reported at the source line instead of being compiled into a
  lost write; hold the values in a list, or wrap the array in a struct and
  capture the struct (#2474 tracks shared cells for sized arrays).
- **Replacing a struct whose new value reuses one of its strings keeps the
  string.** `q = Rec { name: q.name, count: 1 }` freed the old value's
  owned strings before storing the new one, so `q.name` pointed at freed
  memory (it printed `(null)`). The old value's strings are now freed only
  when the new value does not hold them; one it does hold moves to it.
