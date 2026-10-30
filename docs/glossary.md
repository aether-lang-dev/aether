# Glossary

The words the [tutorial](tutorial.md), [the examples](../examples/README.md)
and the [language reference](language-reference.md) use, each in a few
lines, with where to read more.

**actor.** State that only messages reach. It handles its messages one at a
time, so its state needs no lock. Declared with `actor Name { state ...
receive { ... } }`, started with `spawn`.
[Actors](language-reference.md#actors)

**actor_ref.** What `spawn` returns: a reference to an actor, a value that
goes into variables, messages and state. `actor_ref[Member]` is a reference
to an actor of one kind.

**ask (`?`).** `ref ? Msg { ... }` sends a message and waits for the
handler's `reply`. It ends at once with an error when the actor is dead,
its mailbox is full, or its handler returns without replying, and after 5
seconds when the handler has not returned.
[Ask pattern](language-reference.md#ask-pattern-request-reply)

**audit trail.** The record of every check the sandbox made, allowed or
refused, which `std.audit` reads back.

**borrowed.** How a function holds its parameters: for the call only. It
may read them and pass them on, not free them; the caller still owns them.
[Memory management](memory-management.md)

**bounded mailbox.** An actor declared `@mailbox(bound: N)` holds at most N
messages it has not handled yet; past that a send is refused at once.
[A bounded mailbox](language-reference.md#a-bounded-mailbox)

**builder, trailing block.** A call followed by a block on the same line
passes the block as its last argument, and the calls inside it configure
what the call builds: `sandbox.new("app") { grant_fs_read("/tmp/*") }`.
[Closures and builder DSLs](closures-and-builder-dsl.md)

**closure.** A function value, `|x| -> x * 2`, that sees the variables
around it where it was made.

**contract.** `requires` (what a function needs on entry) and `ensures`
(what it promises about `result`). A broken one is a panic naming the
clause. [Contracts](language-reference.md#function-contracts-requires--ensures)

**cooperative scheduler.** The scheduler for targets without threads
(WebAssembly, ESP32): every actor runs on one thread, stepped in turn.

**core.** One of the scheduler's threads: one per CPU the process may use,
up to 16. Each runs the actors assigned to it.

**counted value.** A string, a closure, a string sequence, or a struct,
tuple, optional, sum value or fixed-size array made from them. Every place
that holds one owns one reference, and the compiler releases it; the
program writes no free.
[Memory management](memory-management.md)

**credit.** In `std.stream`, the right to send one more chunk. The consumer
gives one back for each chunk it handles, so at most a window of chunks is
in flight. [std.stream](../std/stream/README.md)

**defer.** `defer stmt` runs `stmt` when the enclosing block ends, however
it ends. [Defer](language-reference.md#defer-statement)

**Down.** The message a monitor gets when the actor it watches ends:
`Down { ref, reason }`. The reason is "normal" for a stop, "killed" for a
kill, or the panic's message.
[Death notices](actor-concurrency.md#death-notices)

**enum.** A set of named integer constants, written `Enum.Member`.
[Enums](language-reference.md#enums)

**execution account.** A memory budget and a deadline a host gives one
script, across all its calls. [Execution accounts](emit-lib.md#execution-accounts-2954)

**execution frame.** What every exported function runs in: however a call
ends (a panic, a deadline, a cancel, memory refused), the host gets
control back with an outcome. [Per-call resource caps](emit-lib.md#per-call-resource-caps)

**export.** A name a module lets other files use, listed in its
`exports(...)` line. Also: a function a library built with `--emit=lib`
offers to C, as `aether_<name>`. [Modules](language-reference.md#modules-and-imports)

**extern.** A declaration of a C function the program calls:
`extern puts(s: string) -> int`. [C interoperability](c-interop.md)

**grant.** One thing a sandbox allows: a file pattern to read or write, an
environment variable, a program, a host. A sandbox with no grants allows
nothing. [The containment sandbox](containment-sandbox.md)

**handle.** A `ptr` a module gave the program: a list, a map, a JSON
document, a socket, a `heap.new` box, memory from C. A `ptr` owns nothing,
so the program frees it, through its module, usually with `defer` next to
the call that made it.

**handler.** One arm of an actor's `receive` block: what it does with one
kind of message.

**interpolation.** `"${expr}"`: a value written into a string.

**link.** A tie between two actors: when one ends, the other ends too,
with the same reason, unless it traps exits. An end whose reason is
"normal" (a stop) ends nobody. [Links](actor-concurrency.md#links)

**mailbox.** Where an actor's messages wait until it handles them.

**match.** An expression that runs the first of its arms that fits the
value: literals, ranges, strings, list and struct patterns, variants.
[Match expressions](language-reference.md#match-expressions)

**message.** A record an actor can be sent, declared with
`message Name { field: type }`. It owns the strings and arrays in it.

**module.** A file of functions, types and constants other files import:
`std.string` from the standard library, or `lib/<name>/module.ae` of a
project. [Modules](language-reference.md#modules-and-imports)

**monitor.** `actors.monitor(ref)` asks for one `Down` when `ref` ends.

**optional (`T?`).** A `T` or `none`. `??` gives a fallback.
[Optionals](language-reference.md#optionals)

**outcome.** How a call into an exported function ended, from
`aether_last_outcome()`: ok, panicked, deadline, cancelled, memory cap,
out of memory, or incompatible (a library made for another runtime).

**panic.** What a bug raises: it unwinds to the nearest `try`, and in an
actor's handler it ends that actor. Outside both, it ends the program.

**receive.** The block of an actor that lists its handlers.

**reply.** What a handler answers an ask with.

**result (`T!`).** A value and an error string, `""` when it worked. `or`
gives a fallback, and `!` passes the error up.
[Result types](language-reference.md#the-t-result-type-and-or---sugar)

**runtime.** The C library every program links: the scheduler, the actors,
the panic barrier, the sandbox.

**safe point.** A place where a library's code checks whether its call
should end (a deadline, a cancel): every loop head of code built as a
library (`--emit=lib`, `--emit=obj`), and every few thousand values a JSON
parse reads.

**sandbox.** A list of grants that `sandbox.enforce` holds a block of code
to. [The containment sandbox](containment-sandbox.md)

**scheduler.** The part of the runtime that runs actors on the cores and
delivers their messages.

**self.** Inside a handler, the actor's own `actor_ref`.

**send (`!`).** `ref ! Msg { ... }`: a message on its way, without waiting.

**slice (`T[]`).** A view of elements stored elsewhere: a pointer and a
length. `xs[lo..hi]` makes one. The buffer `make([]T, n)` hands out has one
owner, the local that holds it, which releases it when its scope ends: the
program writes no free. [Slices](language-reference.md#slices-t)

**spawn.** `spawn(Name())` starts an actor and returns its `actor_ref`.

**state.** The fields an actor keeps between messages, declared with
`state name = value`. [State variables](language-reference.md#state-variables)

**std, contrib.** `std/` is the standard library every install has;
`contrib/` holds modules outside it, such as bridges to a database or to
another language, which a build may leave out.
[std or contrib](stdlib-vs-contrib.md)

**step.** One run of an actor's handler for one message.

**stream.** A flow of chunks from one actor to another, held to a window
of credits by `std.stream`.

**struct.** A record of named fields, held by value: assigning it copies
it. [Structs](language-reference.md#structs)

**sum type.** A value that is exactly one of several structs, its
variants: `type Shape = Circle | Rect`. A `match` over it must cover each.
[Sum types](language-reference.md#sum--variant-types)

**supervisor.** An actor that starts children, links them, and restarts
one that fails under the same ref. [Supervisors](actor-concurrency.md#supervisors)

**trap exits.** An actor that calls `actors.trap_exits(true)` gets a
`Down` when an actor linked to it ends, instead of ending too.

**try, catch.** `try { ... } catch msg { ... }` runs the block and, when a
panic unwinds out of it, the catch with the panic's message.

**wait_for_idle().** Waits until every actor has nothing left to do. Called
from main, never from inside an actor.
