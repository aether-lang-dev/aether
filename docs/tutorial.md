# Learn Aether: the tutorial

This tutorial is the story told alongside [the path through the
examples](../examples/README.md). The path is the code: one file per idea,
each saying how to run it and ending with what it prints. This page says
what each step shows, why it comes where it does, and what newcomers get
wrong, and each chapter ends with exercises and their answers.

Read a chapter, then run its examples in order from the repository root
and change them:

```bash
ae run examples/01-language/01-hello.ae
```

Every program on this page is compiled by CI, and the ones followed by
their output are run and compared, so what you read is what Aether does
today. [Getting started](getting-started.md) installs the toolchain; the
[glossary](glossary.md) defines the words this page uses; the [language
reference](language-reference.md) has everything else.

1. [The language](#1-the-language)
2. [Actors](#2-actors)
3. [The standard library](#3-the-standard-library)
4. [C and other languages](#4-c-and-other-languages)
5. [Containing code](#5-containing-code)
6. [Tools](#6-tools)
7. [Programs](#7-programs)

---

## 1. The language

Aether compiles to C: `ae run` turns a file into a native program and runs
it. A program is a set of functions, and it starts at `main()`. There are
no classes and no methods: structs are plain data, behaviour is functions,
and the one stateful thing with an inside and an outside is the actor
(chapter 2).

### Values, strings and control flow

[01-hello](../examples/01-language/01-hello.ae),
[02-values-and-types](../examples/01-language/02-values-and-types.ae),
[03-strings](../examples/01-language/03-strings.ae),
[04-control-flow](../examples/01-language/04-control-flow.ae)

A line ends a statement, and `${...}` puts any value into a string. A local
takes its type from the first value it is given and keeps it. Conditions
take no parentheses, every body takes braces, and `if` is also a value:

```aether,run
main() {
    temps = [12, 19, 25, 31]
    for i in 0..temps.len {
        t = temps[i]
        feel = if t < 15 { "cold" } else if t < 28 { "mild" } else { "hot" }
        println("${t} C is ${feel}")
    }
}
```

```output
12 C is cold
19 C is mild
25 C is mild
31 C is hot
```

A string is immutable text that knows its length; `==` compares contents.
What takes strings apart and builds new ones is in `std.string`, called
through the module's name: `string.length(s)`, `string.to_upper(s)`.

### Functions

[05-functions](../examples/01-language/05-functions.ae)

Parameters can have defaults and calls can name their arguments. A
function can return several values, which the caller takes apart, and a
function can be written as clauses, each for one shape of argument:

```aether,run
min_max(xs: int[]) -> (int, int) {
    lo = xs[0]
    hi = xs[0]
    for i in 1..xs.len {
        if xs[i] < lo { lo = xs[i] }
        if xs[i] > hi { hi = xs[i] }
    }
    return lo, hi
}

describe(0) -> "zero"
describe(n) when n < 0 -> "negative"
describe(n) -> "positive"

greet(name: string, greeting: string = "Hello") -> string {
    return "${greeting}, ${name}"
}

main() {
    lo, hi = min_max([7, -2, 9, 4])
    println("${lo} .. ${hi}")
    println("${describe(0)} ${describe(-5)} ${describe(3)}")
    println(greet("Ada"))
    println(greet(greeting: "Welcome", name: "Grace"))
}
```

```output
-2 .. 9
zero negative positive
Hello, Ada
Welcome, Grace
```

### Data: structs, enums, sum types and match

[06-structs](../examples/01-language/06-structs.ae),
[07-enums-and-sums](../examples/01-language/07-enums-and-sums.ae),
[08-match](../examples/01-language/08-match.ae)

A struct is a record held by value: assigning or passing one copies it,
and a function that should change the caller's takes its address, `&p`.
An enum is a set of named constants. A sum type is a value that is exactly
one of several structs, and `match` over it must cover every one, so
adding a variant shows every place that has to learn it:

```aether,run
struct Deposit { amount: int }
struct Withdraw { amount: int }
struct Close {}

type Command = Deposit | Withdraw | Close

apply(balance: int, c: Command) -> int {
    return match c {
        Deposit -> balance + c.amount
        Withdraw -> balance - c.amount
        Close -> 0
    }
}

main() {
    let cmds: Command[3] = [Deposit { amount: 50 }, Withdraw { amount: 20 }, Deposit { amount: 5 }]
    balance = 0
    for i in 0..cmds.len {
        balance = apply(balance, cmds[i])
    }
    println("balance: ${balance}")
}
```

```output
balance: 35
```

Inside an arm, the value is that variant, so `c.amount` reads its field.
`match` also takes literals, ranges, strings, list patterns such as
`[head | tail]` and struct patterns; it is an expression, so it has a value.

### Absence and failure

[09-optionals](../examples/01-language/09-optionals.ae),
[10-errors](../examples/01-language/10-errors.ae)

Two different things, two different tools. A value that may be absent is
an optional, `T?`: `none` or a value, read with `??` for a fallback. An
operation that can fail returns its value and an error string, `""` when
it worked; `-> T!` names that pair, and `or` gives a fallback:

```aether,run
import std.string

first_even(xs: int[]) -> int? {
    for i in 0..xs.len {
        if xs[i] % 2 == 0 {
            return xs[i]
        }
    }
    return none
}

parse_percent(text: string) -> int! {
    n, err = string.to_int(text)
    if err != "" {
        return 0, "'${text}' is not a number"
    }
    if n < 0 || n > 100 {
        return 0, "${n} is not a percentage"
    }
    return n
}

main() {
    println(first_even([3, 8, 5]) ?? -1)
    println(first_even([3, 5]) ?? -1)

    v, err = parse_percent("120")
    println("${v} [${err}]")
    println(parse_percent("42") or 0)
    println(parse_percent("lots") or 0)
}
```

```output
8
-1
0 [120 is not a percentage]
42
0
```

A panic is neither: it is for a bug. It unwinds to the nearest `try`, and
outside one it ends the program. A bad index, a broken contract and a
`none` forced open with `!` are panics.

### Arrays, slices and collections

[11-arrays-and-slices](../examples/01-language/11-arrays-and-slices.ae),
[12-collections](../examples/01-language/12-collections.ae)

`[1, 2, 3]` is a fixed array, a value like a struct. A slice, `int[]`, is
a view of elements stored elsewhere, so a parameter typed `int[]` takes any
array without copying it. Every index is checked. For things that grow,
`std.list`, `std.map` and `std.set` give handles (below).

### Functions as values, and modules

[13-closures](../examples/01-language/13-closures.ae),
[14-modules](../examples/01-language/14-modules/main.ae)

`|x| -> x * 2` is a function value, and a closure sees the variables
around it. A call followed by a block passes the block as its last
argument: the trailing-block form that most of std's builders use.
A module is a directory under `lib/` with a `module.ae` whose
`exports(...)` line names what other files may use.

### Contracts, defer and ownership

[15-contracts](../examples/01-language/15-contracts.ae),
[16-defer](../examples/01-language/16-defer.ae),
[17-ownership](../examples/01-language/17-ownership.ae)

`requires` and `ensures` state what a function needs and what it
promises, and a broken one is a panic naming the clause. `defer` runs its
statement however the block ends.

Ownership is the last step of the chapter because it explains the others.
Aether has no garbage collector, and it does not ask you to free
everything either. Strings, closures, and the structs, arrays and
optionals made from them are counted: the compiler frees them, and you
write no free. A *handle*, a `ptr` a module gave you (a list, a map, a
JSON document, a buffer), is the program's to free, through its module,
and `defer` keeps the free next to the `new`:

```aether,run
import std.list

main() {
    names = list.new()
    defer list.free(names)
    list.add(names, "ada")
    list.add(names, "grace")
    println("${list.size(names)} names")
}
```

```output
2 names
```

### Common mistakes

- **Freeing a value the compiler counts.** A string needs no free, and
  freeing a parameter the function only borrows is refused (E0901). Free
  handles, and only handles.
- **Forgetting a handle.** A `list.new()` with no `list.free` leaks it.
  Write the `defer` on the next line.
- **Changing a local's type.** `count = 0` then `count = "many"` is a
  compile error, not a new variable.
- **A panic for an expected failure.** Bad input from a user is an error
  to return (`T!`), not a panic; panics are for bugs.
- **Reading an element of a list or map as a string without its getter.**
  Elements come back untyped; `list.get_string` and `map.get_string` say
  when one is not a string.

### Exercises

**1. FizzBuzz.** Print 1 to 15, but "Fizz" for multiples of 3, "Buzz" for
multiples of 5, and "FizzBuzz" for both.

<details>
<summary>An answer</summary>

```aether,run
fizzbuzz(n: int) -> string {
    if n % 15 == 0 { return "FizzBuzz" }
    if n % 3 == 0 { return "Fizz" }
    if n % 5 == 0 { return "Buzz" }
    return "${n}"
}

main() {
    line = ""
    for i in 1..16 {
        line = "${line}${fizzbuzz(i)} "
    }
    println(line)
}
```

```output
1 2 Fizz 4 Buzz Fizz 7 8 Fizz Buzz 11 Fizz 13 14 FizzBuzz 
```

</details>

**2. The largest.** Write `largest(xs: int[]) -> int?`, `none` for an empty
slice, and print the largest of `[4, 11, 7]` and of `[]`, with "nothing"
for the second.

<details>
<summary>An answer</summary>

```aether,run
largest(xs: int[]) -> int? {
    if xs.len == 0 {
        return none
    }
    best = xs[0]
    for i in 1..xs.len {
        if xs[i] > best { best = xs[i] }
    }
    return best
}

show(v: int?) -> string {
    return match v {
        none -> "nothing"
        some(n) -> "${n}"
    }
}

main() {
    println(show(largest([4, 11, 7])))
    println(show(largest([])))
}
```

```output
11
nothing
```

</details>

**3. A result that can fail.** Write `divide(a: int, b: int) -> int!` that
fails with "division by zero", and print `divide(9, 3)` and, with `or` and
a fallback of -1, `divide(1, 0)`.

<details>
<summary>An answer</summary>

```aether,run
divide(a: int, b: int) -> int! {
    if b == 0 {
        return 0, "division by zero"
    }
    return a / b
}

main() {
    q, err = divide(9, 3)
    println("${q} [${err}]")
    println(divide(1, 0) or -1)
}
```

```output
3 []
-1
```

</details>

---

## 2. Actors

Aether's concurrency is actors: a piece of state that only messages reach.
Nothing outside an actor touches its state; other code sends it a message,
and it changes its own state, one message at a time. So there are no locks
to take and no data races to find. The scheduler runs actors on every
core, so many actors use the whole machine.

### A first actor, its state, and asking it

[01-first-actor](../examples/02-actors/01-first-actor.ae),
[02-state](../examples/02-actors/02-state.ae),
[03-ask-reply](../examples/02-actors/03-ask-reply.ae)

`message` declares what can be sent, `actor` what an actor does with each
kind, `spawn` starts one, `!` sends without waiting, and `?` sends and
waits for the handler's `reply`:

```aether,run
message Add { n: int }
message Total {}

actor Counter {
    state total = 0
    receive {
        Add(n) -> { total = total + n }
        Total() -> { reply total }
    }
}

main() {
    c = spawn(Counter())
    c ! Add { n: 5 }
    c ! Add { n: 7 }
    int t = c ? Total {}
    println("total: ${t}")
}
```

```output
total: 12
```

The ask sees both adds: messages from one sender reach an actor in the
order they were sent. `wait_for_idle()` is the other way to wait: it
returns once every actor has nothing left to do, which is how a program
lets its actors finish before it ends.

### Actors that talk to each other

[04-actor-refs](../examples/02-actors/04-actor-refs.ae),
[05-messages](../examples/02-actors/05-messages.ae),
[06-pipeline](../examples/02-actors/06-pipeline.ae),
[07-worker-pool](../examples/02-actors/07-worker-pool.ae)

What `spawn` returns is an `actor_ref`, a value that goes into messages and
state, so an actor can be told whom to answer; `self` is an actor's own.
A message owns the strings and arrays in it, so the sender can reuse its
variables at once; a `ptr` in a message hands the handle over. With refs
come the shapes of concurrent programs: a pipeline of stages, and a pool
of workers whose results a collector gathers. Order holds between one
sender and one receiver, never between two senders.

### Shapes of an actor's life

[08-state-machine](../examples/02-actors/08-state-machine.ae),
[09-timeouts](../examples/02-actors/09-timeouts.ae)

An enum as an actor's state, with a transition table, makes a state
machine that applies one event at a time. `receive { ... } after N -> { ... }`
does something when no message has come for N milliseconds: a watchdog.

### When actors fail

[10-crash-isolation](../examples/02-actors/10-crash-isolation.ae),
[11-ending](../examples/02-actors/11-ending.ae),
[12-links](../examples/02-actors/12-links.ae),
[13-supervisor](../examples/02-actors/13-supervisor.ae)

A panic in a handler ends that actor and nothing else. An actor also ends
when it stops itself (`actors.stop()`) or something kills it
(`actors.kill(ref)`), and whoever monitors it gets a `Down` with the
reason. A link ties two actors so that one's failure ends the other,
unless that one traps exits and takes the `Down` instead. A supervisor is
built from those: it links its children, and when one fails it restarts
it in place, so every ref to the child reaches the new one.

### Holding a producer back

[14-bounded-mailbox](../examples/02-actors/14-bounded-mailbox.ae),
[15-streams](../examples/02-actors/15-streams.ae),
[16-sharded-map](../examples/02-actors/16-sharded-map.ae)

A send never blocks, so an actor that receives faster than it handles
grows its mailbox without bound. `@mailbox(bound: N)` caps it: past N, a
send is refused at once, and `try_send` says so. `std.stream` bounds a flow
between two actors with credits, so a fast producer never has more than a
window of chunks in flight. And when one actor's mailbox is the
bottleneck, its work can be split over several by key.

### Common mistakes

- **Reading an actor's state from outside.** Ask it (`?`), or have it send
  what it knows; its state is its own.
- **Expecting order between two senders.** Main and another actor sending
  to the same actor interleave in any order. Wait for one before the
  other, or route both through one sender.
- **Calling `wait_for_idle()` inside an actor.** It waits for every actor,
  this one too, so it can never return there; it panics instead. Call it
  from main.
- **Blocking in a handler.** A handler that sleeps or waits on a socket
  holds a core. Keep handlers short, and give slow work its own actor.
- **Using a handle after sending it.** A `ptr` sent in a message belongs to
  the receiver now; the sender must not touch it again.

### Exercises

**1. A bank account.** An actor with a balance takes `Deposit { n }` and
`Withdraw { n }`, refusing a withdrawal larger than the balance, and
answers `Balance {}`. Deposit 100, withdraw 30, then try to withdraw 500,
and print the balance.

<details>
<summary>An answer</summary>

```aether,run
message Deposit { n: int }
message Withdraw { n: int }
message Balance {}

actor Account {
    state balance = 0
    receive {
        Deposit(n) -> { balance = balance + n }
        Withdraw(n) -> {
            if n > balance {
                println("refused: ${n} is more than ${balance}")
            } else {
                balance = balance - n
            }
        }
        Balance() -> { reply balance }
    }
}

main() {
    a = spawn(Account())
    a ! Deposit { n: 100 }
    a ! Withdraw { n: 30 }
    a ! Withdraw { n: 500 }
    int b = a ? Balance {}
    println("balance: ${b}")
}
```

```output
refused: 500 is more than 70
balance: 70
```

</details>

**2. A two-stage pipeline.** A `Doubler` doubles each number it gets and
sends the result on to a `Printer`, which prints it. Send 1, 2 and 3
through.

<details>
<summary>An answer</summary>

```aether,run
message Num { n: int }
message Connect { next: actor_ref }

actor Printer {
    receive {
        Num(n) -> { println("got ${n}") }
    }
}

actor Doubler {
    state next: actor_ref = null
    receive {
        Connect(next: n) -> { next = n }
        Num(n) -> { next ! Num { n: n * 2 } }
    }
}

main() {
    printer = spawn(Printer())
    doubler = spawn(Doubler())
    doubler ! Connect { next: printer }
    for i in 1..4 {
        doubler ! Num { n: i }
    }
    wait_for_idle()
}
```

```output
got 2
got 4
got 6
```

</details>

---

## 3. The standard library

[01-files](../examples/03-std/01-files.ae) ·
[02-json](../examples/03-std/02-json.ae) ·
[03-processes](../examples/03-std/03-processes.ae) ·
[04-http](../examples/03-std/04-http.ae) ·
[05-http-server](../examples/03-std/05-http-server.ae) ·
[06-http2-tls](../examples/03-std/06-http2-tls.ae) ·
[07-reverse-proxy](../examples/03-std/07-reverse-proxy.ae) ·
[08-proxy-pool](../examples/03-std/08-proxy-pool.ae) ·
[09-logging](../examples/03-std/09-logging.ae) ·
[10-tcp](../examples/03-std/10-tcp.ae) ·
[11-cryptography](../examples/03-std/11-cryptography.ae)

The standard library follows the two rules of chapter 1 everywhere. A call
that can fail returns its value and an error string, so a program checks
`err` and goes on. What a module hands out as a `ptr` (a JSON document, a
server, a socket) is a handle the program frees through that module.

- **Files and processes.** `std.fs` reads, writes and walks the file
  system. `std.os` runs a program from an argument list, with no shell in
  between, and reads what it prints and its status.
- **Data.** `std.json` parses into a tree the program frees once, and
  builds documents to encode.
- **The network.** `std.http` serves by route and fetches: the same server
  API carries middleware, health probes and metrics, HTTP/2 and TLS, and
  `std.http.proxy` forwards to upstreams. Under it, `std.tcp` is the
  socket: a byte stream, so a protocol says where a message ends.
- **Logs and secrets.** `std.log` writes levelled lines. The
  `std.cryptography` modules are written in Aether: hashes, HMACs,
  signatures, and the pure-Aether TLS.

The [standard library reference](stdlib-reference.md) lists every module
and call.

---

## 4. C and other languages

[01-libc](../examples/04-interop/01-libc.ae) ·
[02-own-c](../examples/04-interop/02-own-c/main.ae) ·
[03-callbacks](../examples/04-interop/03-callbacks.ae) ·
[04-embedding](../examples/04-interop/04-embedding/greeter.ae) ·
[05-hosts](../examples/04-interop/05-hosts/python.ae) ·
[06-java-host](../examples/04-interop/06-java-host/aether/manifest.ae) ·
[07-limits](../examples/04-interop/07-limits/script.ae)

Aether compiles to C, so C is one declaration away in both directions.
`extern` declares a C function to call; `@source` ships a C file with a
module; `@c_callback` hands an Aether function to C as a function pointer.
The other way, `ae build --emit=obj` (or `--emit=lib`) makes an Aether
file a library a C program links, each function an `aether_<name>`
export.

A program that runs Aether code it does not control bounds it: every
export runs in an execution frame, so a deadline, a cancel from another
thread or a memory budget ends a call and gives the host an outcome,
never a crash. And Aether can host other languages (Python, Lua, Tcl, Go)
inside the program, under the sandbox of chapter 5. [C
interoperability](c-interop.md) and [emit=lib](emit-lib.md) are the
references.

---

## 5. Containing code

[01-sandbox](../examples/05-containment/01-sandbox.ae) ·
[02-trusted-calls](../examples/05-containment/02-trusted-calls.ae) ·
[03-audit](../examples/05-containment/03-audit.ae) ·
[04-spawn-sandboxed](../examples/05-containment/04-spawn-sandboxed.ae) ·
[05-capsicum](../examples/05-containment/05-capsicum.ae) ·
[06-casper](../examples/05-containment/06-casper.ae)

A sandbox is a list of grants: files it may read or write, environment
variables, programs, network hosts. `sandbox.enforce(grants) callback { ... }`
runs a block under them, and anything not granted is refused, the
refusal recorded where `std.audit` can read it back. A trusted call lets
the block use one service with your authority. A child process can be held
to grants at its own libc calls, and on FreeBSD, Capsicum has the kernel
enforce it. [The containment sandbox](containment-sandbox.md) is the
reference.

---

## 6. Tools

[01-testing](../examples/06-tooling/01-testing/discount_test.ae) ·
[02-mutation](../examples/06-tooling/02-mutation/mutate.ae) ·
[03-project](../examples/06-tooling/03-project/src/main.ae) ·
[04-lib-path](../examples/06-tooling/04-lib-path/main.ae) ·
[05-ae-help](../examples/06-tooling/05-ae-help/broken.ae)

`std.spec` writes tests (`describe`, `it`, assertions) and `ae test` finds
and runs them; `std.mutation` checks that the tests would notice a change
to the code. A project is a directory with an `aether.toml`, its program in
`src/` and its modules in `lib/`. `ae help` explains what the compiler
found in a script and suggests the fix. [Testing](testing.md) and
[the formatter](formatter.md) cover the rest of the everyday tools.

---

## 7. Programs

[01-word-count](../examples/07-applications/01-word-count/main.ae) ·
[02-chat-room](../examples/07-applications/02-chat-room.ae) ·
[03-build-supervisor](../examples/07-applications/03-build-supervisor.ae) ·
[04-calculator](../examples/07-applications/04-calculator.ae)

The last part puts the others together: a word counter (files, sorting,
errors), a chat room (actors keeping each other in state), a build
supervisor (a process with a deadline and nothing left behind) and a
terminal calculator (a builder DSL with closures). Read them for how the
pieces fit, then write your own.

## Where next

- [Language reference](language-reference.md): every construct, in full.
- [Memory management](memory-management.md): the ownership rules.
- [Actor concurrency](actor-concurrency.md): how the runtime delivers,
  orders and ends.
- [Glossary](glossary.md): the words, defined.
