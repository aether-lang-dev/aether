- **A string taken from a struct field, an `if` or a `match` is owned by
  what it is stored into (#2461).** `t = r.name` stored the field's pointer
  without owning it, so reassigning `r.name`, replacing `r` or leaving `r`'s
  scope freed what `t` still pointed at; the same held for a struct literal
  field, a field store, a returned value, a module global, actor state and a
  closure's string cell. An `if` or `match` whose arm was a local string did
  the same once the local was reassigned or its function returned, and a
  `match` binding never updated its local's ownership at all, so the value it
  replaced leaked and a later free could hit a literal. Each of these now
  takes the value the way `b = a` already did: a field read is copied, a
  local is moved on its last use and copied otherwise, a freshly built string
  is adopted and a literal is borrowed, so every buffer is freed once. A
  function that returns a field read returns a copy its caller owns.
- **A string stored into a struct field through a pointer from a call, a
  pointer field or a cast frees the field's previous string (#2369).** Only
  a local bound to `heap.new` in the same function released the old value,
  because the release reads the box's ownership tracker, and a box made with
  `malloc(n) as *T` has garbage there (#1873). A box returned by a
  constructor, held in another struct's pointer field, or passed as a `ptr`
  and cast back leaked every string it replaced. The compiler now follows
  the pointer back to where it was made: a function every return of which is
  a `heap.new` box, a struct field every store into which is one, a local
  every binding of which is one, or a cast of one. A pointer whose origin it
  cannot see (a parameter, a list element, a C function's result) still only
  sets the tracker, as before. Code that freed the old value by hand before
  storing through one of the pointers now covered frees it twice; drop that.
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
  instead of copied and leaked. Replacing a struct with a value that reuses
  one of its strings (`q = Rec { name: q.name, count: 1 }`) freed that
  string first, so `q.name` printed `(null)`; the old value's strings are
  freed only when the new value does not hold them.
- **A closure environment is reference counted and released by its last
  holder (#2480, #2494, #2498, #2506, #2507, #2519).** `g = || { println(n)
  }` allocated `g`'s environment and never freed it, nor the shared cells
  and strings it holds; a closure a function returned was not freed by the
  caller's local; an environment that captured another closure copied it
  without taking a reference; and a receive arm was emitted without a
  scope, so a `defer` in an arm never ran and nothing an arm built was
  released. Now every environment carries a reference count: capturing a
  closure takes a reference, and the environment's destructor gives back
  its cells, strings and captured closures. A local bound only to closure
  literals, or to the result of a function whose every `return` hands back
  a closure nothing else holds (a call through a closure local included),
  releases its reference at scope exit and on `return`, `break` and
  `continue`, through the closure's destructor; one rebound in a loop
  releases the one it replaces. The release is kept back when a copy that
  holds no reference can outlive the scope: the value is returned (that
  hands the reference to the caller), aliased, used as an operand, passed
  to a parameter that keeps it or to a callee whose body cannot be seen, or
  captured by a closure that does any of these. A local that hands its value
  on in a whole statement, an `if` or loop condition, a statement with a
  trailing block or a `defer` stops owning it at that point and still
  releases the closures it is bound to afterwards; a hand-off inside a
  nested closure's body (`g = || { keep(f) }`) retains the environment for
  the new holder right before it, so the local and the capturing closure
  still release theirs. A closure a function hands over is released after
  the call that consumes it, anywhere in an expression
  (`x = take(make_counter(r))`, `run(make_counter())`, a callee with no
  declared result type), at once when thrown away, and once when used as
  the callee (`call(make_counter())` used to run the call twice and keep
  both environments). Each handler and timeout arm is a scope, so its
  defers, cell releases, environment frees and struct destroys run when the
  handler ends, on every exit.
- **Every holder of a closure value keeps a reference of its own (#2518,
  #2525, #2528).** `list.add(l, f)` handed the list the caller's single
  reference, so adding one closure twice, to two lists, or keeping it in a
  local past `list.free` released the environment twice: an access
  violation. A closure kept in a struct field, a message field, a global or
  an actor's state was never released: `h = Holder { cb: build("a") };
  h.cb = build("b")` kept both environments, with the cells and strings they
  captured, for the rest of the program. Each list slot, map entry, struct
  field, message field, global and state field now takes a reference when a
  closure is stored (a fresh closure's is adopted, a view of one held
  elsewhere is retained), releases it when the element goes (`free`,
  `remove`, `clear`, an overwrite, the holder's destruction), and copies
  retain (`b = a`, a struct passed or returned by value, `<Name>_dup`). A
  list also releases the strings it owns on `remove` and `clear`, as the
  string list does. A `string[N]` or `fn[N]` field of a struct or an actor
  owns its elements, released on destroy and on an element store, copied or
  retained by a copy; a local array of structs that own strings or closures
  destroys its elements at scope exit, replaces one on `arr[i] = v` and
  copies each on `other = arr`. A local bound to a field read (`x = h.cb`),
  a closure a function returns from a field, and a closure an ask brings
  back hold references of their own; a message's closure field is released
  with the message once the handler is done. A closure literal, or a
  parameter, stored into any of these holders is not kept by the store: the
  caller releases its own reference after the call. A module-level `var
  name: fn = null` starts as the zero closure instead of failing to compile.
- **An actor's state is destroyed with the actor, and a reply is released
  by its taker or its replier (#2528).** An actor now has a `destroy_state`
  hook (`ActorBase`, set by the generated spawn) that the scheduler runs
  once when it ends a scheduler-owned actor, on a release or at teardown:
  each `string` state field is freed per its tracker, which is a field of
  the actor rather than a handler local (so a value stored by an earlier
  message is freed when a later one overwrites it; before, neither it nor
  the last value was ever freed), each closure field's environment is
  released and each owning struct or array field destroyed. A reply
  message's string and closure fields are the asker's: the field the ask
  reads out is taken, the rest released with the reply, and a closure reply
  (a field or an expression) now compiles and is owned by the asker's
  binding. A reply nobody takes (the asker timed out, or the message was
  sent rather than asked) is released by the replier
  (`scheduler_reply_owned`), on every host including the MSVC ask path,
  whose helper now delivers a reply field of any size whole (a closure
  reply used to be cut to a pointer's width there); on that path a
  capturing closure built inside a function also compiles now (its
  constructor was used before it was declared).
  `scheduler_reclaim_released()` ends the released actors a host wants
  settled and returns how many are still held back by a reader (0 when all
  are ended).
- **A closure borrows its arguments, so an owned string passed to a closure
  call is freed after the call (#2493, #2499).** `call(f, mk("x"))`, and
  `f(mk("x"))` on a closure local or an `fn` parameter, passed the argument
  straight to the closure, so every such call leaked the string, while a
  named call `g(mk("x"))` hoists it into a temporary and frees it after the
  call. A closure call whose literal is known is decided by that body, as a
  function's is: an argument the closure only reads is freed, one it keeps
  is left to its new owner, and one returned as the string result is freed
  unless the result is that same pointer. A call with no body to read (an
  `fn` parameter, a variable bound to several closures) follows the closure
  calling convention: the caller frees its owned argument after the call.
  That holds because a closure that keeps a `string` parameter (in a list, a
  map, a struct field, a captured variable, a local that keeps it) takes its
  own reference, or copies a plain buffer, when it is entered, and a
  function used as a closure value gets the same from its adapter; the
  parameter is a tracked string, so an alias into a local moves the
  reference, a return hands it to the caller and whatever it still holds at
  exit is freed. A `ptr` parameter cannot be copied, so one closure anywhere
  that keeps one (a store, a capture, a return) turns the convention off,
  and so does any way a closure the compiler did not see can be called: a
  library build, an extern that returns a closure or takes or returns a
  struct with a closure field, a C-laid-out struct with a closure field, a
  `@c_callback` with a closure parameter, a `ptr` turned into a closure
  (`unbox_closure`, or a `ptr` passed to an `fn` parameter) or a raw pointer
  viewed as such a struct. With the convention off, closure-call arguments
  are left alone: a leak, never a free under a closure that kept the
  pointer. `docs/memory-management.md` describes the convention. A struct
  literal returned with a parameter in a field counts as keeping it; the
  caller freed that argument and the returned field pointed at freed
  memory. Assigning a `string` parameter to a local is a keep only when
  that local keeps the value, so neither a closure nor a function's closure
  adapter takes a reference that nothing gives back.
- **A closure that captures a struct reads its own copy of the strings
  (#2504).** A closure capturing a struct that owns strings copied the
  struct's bytes only, and the declaring scope's destroy freed the strings
  while a returned or stored closure still read them; it now captures a
  copy with strings of its own (`<Name>_dup`), destroyed with the closure.
- **A store into a closure's string cell takes the value (#2514).** `s = p`
  inside a closure, where `p` is the caller's string the closure captured,
  stored the pointer as it stood: the env held its own reference and the
  cell adopted the same one without taking it, so both released it at
  scope exit and the caller's string was freed under the caller. The cell
  now takes what it holds the way every owning slot does: a borrowed value
  is copied or retained, a fresh one adopted. A match arm or a tuple
  destructure that binds such a variable stores the same way, and a
  function that returns such a variable hands the caller a copy, since
  the cell is released at the function's exit.
- **A closure passed to an extern is released when the extern says it keeps
  nothing (#2523).** A closure handed to an extern parameter declared
  `@noescape` is released as it would be after an Aether callee that keeps
  nothing: a literal's environment right after the call, a local's at scope
  end. An unannotated extern keeps the environment alive, since the callee
  may have stored it. The std functions that only call their callback
  during the call carry the attribute and no longer free the box or the
  environment on the C side, which leaked a capturing closure's cells and
  strings on every call and, for a closure local passed to `seq_each`
  twice, used freed memory.
- **A message string field built from a call or an interpolation is freed
  once copied.** `w ! Keep { s: string.concat(p, "pt") }`, and the same in
  an ask or a reply, copied the temporary for the receiver and never freed
  it.
