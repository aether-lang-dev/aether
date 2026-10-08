- **A `receive` pattern binding named like a state field is a compile
  error (#2454).** Inside an actor, state is reached by its bare name, so in
  `state v = 100 ... M(v) -> { v += 1 }` codegen resolved `v` to the state:
  the message's value was never read and the state changed instead, with no
  diagnostic. The binding is refused at its source line, with the rename to
  use: `M(v: new_v)`.
- **A closure parameter shadows a same-named local of the enclosing function
  in type inference (#2453).** The early inference pass did not enter a
  closure's parameters into its table, so inside `|v: int| { w = v * 2 }` in a
  function with an `f32x4 v`, `w` was inferred as a lane and the program
  failed with "type mismatch in variable initialization". The closure's own
  locals no longer leak into the enclosing function's table either.
- **Postfix `i++` / `i--` yields the value from before the step (#2457).**
  The parser built the same node for `i++` as for `++i`, so wherever the
  value was used it was the new one: `j = i++` gave 6 for `i = 5`,
  `xs[k++]` skipped the first element, and `while n-- > 0` ran one
  iteration short, with no diagnostic. The node is now marked postfix and
  reaches C as postfix. A `++` / `--` at the start of a line begins a new
  statement instead of applying to the previous line's operand.
- **A closure's write to a captured variable through `++` / `--`, a field,
  an element or a whole array reaches the variable (#2458, #2474).** Only a
  bare `n = ...` / `n op= ...` promoted a capture to a shared cell, so
  `h = || { n++ }` called twice left `n` at 0, `p.x += 10` in a closure
  changed the closure's own copy of `p`, and a write to a captured `int[3]`
  (`arr[i] = v`, `arr[i]++`, `arr = [...]`) was refused at compile time.
  `n++` now promotes `n`; a field or element write (`p.x = v`,
  `p.inner.y *= 3`, `b.vals[i] = v`) promotes a struct held by value, while
  a `heap.new` box is written through its pointer, which is shared already.
  A fixed-size array's cell is a pointer to the whole array, so the closure
  and the enclosing function see one array, through nested closures, loops
  and calls that take it as a slice; a string array's cell owns its
  elements (a store frees the one it replaces) and the last release frees
  them all. A promoted struct with `string` fields stores into them through
  the cell, and the cell's last holder frees the strings the struct owns. A
  closure in an actor handler that writes a state array element is refused
  like any other write to state.
- **A `defer` in a `match` arm runs when that arm ends, only if it was taken
  (#2459).** A match arm's block had no defer scope of its own, so its
  defers ran at the end of the enclosing function or loop body whichever arm
  had been taken, and in a loop on every iteration. An arm block is now a
  scope like an `if` / `switch` arm, which also stops a name declared in one
  arm leaking into the next (where it compiled to an assignment to an
  undeclared C variable).
- **A closure can reassign its own parameter, and a closure nested in
  another can write the outer closure's parameter (#2462, #2463).**
  `|n: int| { n = n + 1 }` stopped the C compiler with "'n' redeclared as
  different kind of symbol", and a string parameter was re-declared as
  `NULL`: the closure body did not count its parameters as declared names.
  `outer = |p: int| { inner = || { p = p + 1 } ... }` failed with "makes
  pointer from integer": the parameter stayed a plain value while the nested
  closure expected a shared cell. The outer closure now keeps such a
  parameter in a cell, as a function does, so the write is seen after the
  nested call. A string parameter's cell, for a closure or a function, takes
  its own reference to the caller's string; it used to free the caller's
  string on the first write through it or at scope exit. Closure bodies also
  no longer inherit the string-escape sets of the function emitted before
  them, which made a string closure free an undeclared variable when another
  closure returned a local of that name.
- **Fixed-size arrays are values: captured, held as state, passed and
  assigned (#2464, #2516).** `int[3] arr` captured by a closure, or
  `state int[4] hist` in an actor, was emitted as the field `int[3] arr;`,
  which is not C, and `f(xs: int[3])` or `a = b` between two `int[3]` locals
  failed in the C compiler too. Both fields now use the declarator
  `int arr[3]`; a capture the closure only reads is copied into its
  environment, an actor's spawn zeroes the field and sets the elements of an
  array-literal initializer, a parameter is the callee's own copy of the
  caller's elements (a write, a closure's too, stays in the callee), and
  binding an array to one that exists copies its elements. The lengths must
  agree: a longer or shorter array passed to such a parameter is a type
  error, and writing an element of a `const` array (`TABLE[0] = v`,
  `TABLE[i]++`) is an Aether error instead of a C one.
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
  or it is passed as an argument or a message field of either type. The
  `state next = 0` ... `a.next = b` ... `next ! Msg {}` pattern the suffix
  stood for keeps working, under any name. `my_ref` is no exception: spawn
  sets it to the actor's own address only when it is used as a reference,
  and one used as a number is a number.
- **A state field summed in a loop is the field (#2505).** In a receive
  arm, a loop that adds to a state field (`while i < n { kept = kept + 1
  ... }`) was rewritten into a closed form on a bare `kept` and a local of
  that name was hoisted, so the build failed; the rewrite now writes the
  field, a promoted capture's cell or a closure's capture as the loop would,
  with errors on the loop's own lines, and a state field is never hoisted as
  a local.
- **A string literal in a function-clause pattern compares by content
  (#2467).** `greet("bob") -> ...` was emitted as `if (_arg0 == bob)`: an
  undeclared name, and even when quoted a pointer comparison. The pattern is
  now a quoted, escaped C string compared with `string_equals`, as a `match`
  string arm is.
- **A closure in a receive arm can capture the names the arm's message
  pattern binds (#2492).** `Ping(n) -> { g = || { println("n=${n}") } }`
  failed in the C compiler with "'n' undeclared": capture discovery only
  looked at the declarations in the arm's body, and the pattern's bindings
  (`Ping(n)`, `Named(who: name)`) are declared by the handler from the
  message. They now count as the arm's own names, for int, string and `ptr`
  fields, in closures nested at any depth. A capture in an arm (a binding or
  a local) also takes its type from the arm; it was looked up through every
  function, so a string local captured from an arm was declared int. A
  closure that writes a binding gets it as a shared cell seeded from the
  message, and a string cell holds its own reference, so the message's
  string is still released once.
- **Calling a closure yields what its body returns (#2460, #2484, #2501).**
  A closure literal's type is the erased `fn`, so `r = call(f, x)` (and
  `r = f(x)`) on `f = |t: string| -> t` was typed int: `println(r)` printed
  the string's address as a number, a float result printed 0, and the
  compiler warned the result was "assumed int". The call now takes its type
  from the literal the variable holds, also through an alias, a capture and
  a closure that returns a closure, by the same rule that picks the
  closure's C return type; a string result is owned and freed by the
  binding. A variable re-bound to a closure with a different result type
  goes back to the erased `fn`. A closure that returns another closure's
  call result (`g = || { return call(f, x) }`, with `f` an erased `fn`)
  returned the int an erased call defaults to, so `let r: ptr = call(g)`
  cut the pointer to 32 bits under `ae run`; a typed binding or a declared
  return of such a call now types the closure's return too, through any
  number of pass-through closures. A closure that no typed use reaches (one
  handed to an extern, or returned through `-> fn`) is still int, and the
  compiler warns at its `return` and names the fix. `f = |n: int| { return
  "v", n }` was typed by its first value, so `s, k = call(f, 4)` was refused
  and a call left whole did not compile; a multi-value return now gives the
  closure a tuple result in the type checker and in its C function, and
  each string slot is handed over owned, so the destructured binding frees
  it. `worker.map` binds its callback's result as `ptr` now, and
  `docs/closures-and-lifetimes.md` shows `let p: ptr = call(y, 5)` (the
  `ptr p = call(...)` it showed is refused).
- **A call on an `fn` parameter goes through the parameter's value (#2513).**
  `f()` on the `fn` parameter of one function used to be bound by name to
  the closure variable `f` of an unrelated function and ran that closure's
  code with the parameter's env, an access violation. The closure a
  variable holds is now recorded per scope, so a parameter, or a name bound
  in another function, dispatches through `f.fn(f.env)`.
- **`call()` on a value that is not a closure is a type error (#2468).**
  `call(x, ...)` invokes a closure, and a callee of any other known type was
  let through to the C compiler, which stopped at `'_tuple_ptr_string' has
  no member named 'fn'` against generated code. The usual way in was a
  `(value, err)` return such as `list.get` bound to one name. The checker now
  reports `call() needs a closure, but 'cl' has type (ptr, string)` at the
  argument, with how to destructure the tuple, or to unbox a closure stored
  as a `ptr`. The closures-and-builder-DSL guide reads list elements with
  `list.get_raw` where it needs the value alone.
- **A const defined by an operator expression has its type everywhere it is
  read, and may name a const declared after it (#2475, #2495).** A const was
  typed at registration only when its initializer was a bare literal, so
  `const MOVED = 1 << 30` stayed untyped until the checker reached the
  declaration; an imported const is merged at the end of the program and a
  same-file const can come after its users, so `mark = n.flag_index &
  flags.MOVED` warned "unresolved type in codegen, defaulting to int", and
  with a 64-bit const the int silently truncated the value. A const is now
  typed from its initializer's operators (`<<`, `>>`, `&`, `|`, `^`, `~` and
  the arithmetic ones) and from the other consts it names, in its own module
  or another, before any function is checked. Codegen also wrote each const
  in source order, and a C initializer can only name a definition above it,
  so `const HIGH = LOW << 4` ahead of `const LOW = 3` stopped the C compiler;
  consts, module `var`s and the consts merged in from modules are emitted in
  dependency order, and a const that depends on itself, directly or through
  others, is reported at its declaration: `const 'A' depends on itself: A
  -> B -> A`.
- **A struct argument of the wrong struct type, and an assignment to a
  call's result, are type errors (#2491, #2481).** Passing a `Narrow` where
  the parameter takes a `Wide` went through to gcc as "incompatible type for
  argument", one error per compile; each such argument is now reported at
  the argument (`Argument 1 'a' of 'length2': expected Wide, got Narrow`),
  all of them in one pass, while `*T` and `ptr` parameters and a variant
  struct passed for its sum type are unchanged. A call's value is a
  temporary: `copy(q).x = 0.0` and `copy(q) = v` stopped the C compiler at
  "lvalue required", `grid().cells[0] = 9` compiled and lost the write, and
  `next_int(1) += 4` stopped the parser. Each is now reported as `cannot
  assign to the result of 'copy(...)': a call's result is a temporary`; a
  field or element reached through a pointer the call returned
  (`holder(&q).x = 5.0` with a `*P` result, an element of a returned slice)
  is real storage and still assigns.
- **A `match` arm whose body is a `{ ... }` block yields its last value
  (#2496).** In `m = match x { 1 -> { t = ...; t } _ -> "other" }` the block
  ran but nothing assigned `t` to `m`: the binding kept its previous value
  (unset on a first binding) and the string the block built leaked. A block
  arm now yields its last expression, or a nested `match`, to the result of
  a binding, a reassignment or a `return match`, before the block's defers
  run. A `match` statement inside the block no longer assigns to the outer
  result either: it used to, which failed to compile when the types differed.
- **Operands are evaluated left to right, and a call the compiler cannot see
  into is ordered before what it could affect (#2478, #2516, #2524).** A
  call's arguments, an interpolation's segments, the two sides of `+`, the
  fields of a struct literal or a message, an array literal and a
  multi-value `return` reached C in a form that leaves their order
  unspecified, so `"${j++} ${j++} ${j}"` printed `1 0 2` with GCC on Windows
  and `add(next(), counter)` read the global before the call that changes
  it; `pair(strbuilder.append(b, "xy"), strbuilder.length(b))` read the
  length first, since an extern's effects are unknown. An operand a later one
  depends on (one writes a variable the other uses, or a call can change what
  the other reads) is now evaluated first, in source order. A C extern, a C
  function pointer, a closure, a message send, and a function of the program
  that makes such a call are opaque: an opaque call is evaluated into a
  temporary ahead of every later operand that calls anything, reads a module
  global or a variable shared with a closure, or reads memory through a
  pointer, a field or an index. Operands that read only plain locals and
  literals stay inline, and lists with no dependent pair compile as before.
  The indexes of an assignment's target are evaluated before its value
  (`arr[i++] = i` stores at the old `i`), an array literal stored into an
  existing array is evaluated whole before the store (`a = [a[1], a[0]]`
  swaps), a closure literal reads its captures where it stands and is
  ordered against the operands beside it, a call with named arguments
  evaluates them in the order written, and `pair(pqueue.pop(q),
  pqueue.pop(q))` pops in source order. A write inside the target of a
  compound assignment (`a[i++] += 1`) is refused rather than run twice.
- **A function named like a C math or character function no longer
  replaces it (#2526).** A top-level `floor(x: int) -> int` was emitted as
  a global `floor`, the link resolved libm's `floor` to it, and
  `math.floor(2.5)` called the user's function and returned 2.5; a
  `toupper` failed to link on Windows. The compiler already gave socket,
  I/O, process, memory and string names a C symbol of their own; it now
  does the same for every `<math.h>` function (with its `f` and `l`
  forms), `<ctype.h>`, `<setjmp.h>`, `<locale.h>` and the rest of the C11
  library. The Aether name is unchanged.
