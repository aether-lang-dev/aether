# Aether examples

A path through Aether, from the first program to actors, servers, C, the
sandbox, the tools and whole programs. Read them in order: each file
teaches one idea, starts by saying what it teaches and how to run it,
and ends with the exact output it prints. CI runs every example that can
run and fails when one stops printing what it says (`make
check-examples`).

Run them from the repository root:

```sh
ae run examples/01-language/01-hello.ae
```

With a toolchain built from this tree, that is `./build/ae run ...`.
[The tutorial](../docs/tutorial.md) walks this path in prose, a chapter
per part below (the first two with the mistakes newcomers make and
exercises); the [glossary](../docs/glossary.md) defines its words. The [language
reference](../docs/language-reference.md) is the full story behind each
step; every row below links the part it builds on. [Getting
started](../docs/getting-started.md) installs the toolchain.

## 1. The language

| Example | What it teaches | Reference |
|---|---|---|
| [01-hello.ae](01-language/01-hello.ae) | `main`, `println`, interpolation | [Functions](../docs/language-reference.md#functions) |
| [02-values-and-types.ae](01-language/02-values-and-types.ae) | numbers, bools, strings, written types, `const`, module `var`, `as` | [Types](../docs/language-reference.md#types) |
| [03-strings.ae](01-language/03-strings.ae) | escapes, comparison by content, std.string, parsing numbers | [String literals](../docs/language-reference.md#string-literals) |
| [04-control-flow.ae](01-language/04-control-flow.ae) | `if` as a statement and a value, `while`, `for`, `break`, labels | [Control flow](../docs/language-reference.md#control-flow) |
| [05-functions.ae](01-language/05-functions.ae) | typed parameters, defaults, named arguments, several results, clauses with guards | [Functions](../docs/language-reference.md#functions) |
| [06-structs.ae](01-language/06-structs.ae) | struct values, `&p` pointers, struct patterns, `@derive(eq)`, `using` | [Structs](../docs/language-reference.md#structs) |
| [07-enums-and-sums.ae](01-language/07-enums-and-sums.ae) | `enum`, sum types, exhaustive `match` | [Sum types](../docs/language-reference.md#sum--variant-types) |
| [08-match.ae](01-language/08-match.ae) | literals, ranges, strings, list and struct patterns | [Match expressions](../docs/language-reference.md#match-expressions) |
| [09-optionals.ae](01-language/09-optionals.ae) | `T?`, `none`, `??`, `?.`, narrowing | [Optionals](../docs/language-reference.md#optionals) |
| [10-errors.ae](01-language/10-errors.ae) | `(value, err)`, `T!`, `or`, `!`, `panic` and `try` | [Result types](../docs/language-reference.md#the-t-result-type-and-or---sugar) |
| [11-arrays-and-slices.ae](01-language/11-arrays-and-slices.ae) | fixed arrays, slices, `.len`, checked indexes, `make` | [Slices](../docs/language-reference.md#slices-t) |
| [12-collections.ae](01-language/12-collections.ae) | std.list, std.map, std.set, sorting | [Handles](../docs/memory-management.md#handles-and-defer) |
| [13-closures.ae](01-language/13-closures.ae) | closures, captured variables, `fn` parameters, trailing blocks | [Closures](../docs/closures-and-builder-dsl.md) |
| [14-modules/main.ae](01-language/14-modules/main.ae) | a module of your own, `exports`, `import ... as` | [Modules](../docs/language-reference.md#modules-and-imports) |
| [15-contracts.ae](01-language/15-contracts.ae) | `requires`, `ensures`, `where` | [Contracts](../docs/language-reference.md#function-contracts-requires--ensures) |
| [16-defer.ae](01-language/16-defer.ae) | `defer`, `defer try`, `defer catch` | [Defer](../docs/language-reference.md#defer-statement) |
| [17-ownership.ae](01-language/17-ownership.ae) | what the compiler frees, what the program frees | [Memory management](../docs/memory-management.md) |

## 2. Actors

| Example | What it teaches | Reference |
|---|---|---|
| [01-first-actor.ae](02-actors/01-first-actor.ae) | `message`, `actor`, `receive`, `spawn`, `!`, `wait_for_idle` | [Actors](../docs/language-reference.md#actors) |
| [02-state.ae](02-actors/02-state.ae) | state an actor keeps between messages | [State variables](../docs/language-reference.md#state-variables) |
| [03-ask-reply.ae](02-actors/03-ask-reply.ae) | `?` and `reply`, a handle in state | [Ask pattern](../docs/language-reference.md#ask-pattern-request-reply) |
| [04-actor-refs.ae](02-actors/04-actor-refs.ae) | `actor_ref` in messages, `self`, message order | [Sending messages](../docs/language-reference.md#sending-messages-fire-and-forget) |
| [05-messages.ae](02-actors/05-messages.ae) | strings, arrays, sequences and handles in messages | [Messages](../docs/memory-management.md#containers-module-variables-actor-state-messages) |
| [06-pipeline.ae](02-actors/06-pipeline.ae) | stages connected by references, items in order | [Actor runtime](../docs/actor-concurrency.md) |
| [07-worker-pool.ae](02-actors/07-worker-pool.ae) | fan-out to workers, fan-in to a collector | [Actor runtime](../docs/actor-concurrency.md) |
| [08-state-machine.ae](02-actors/08-state-machine.ae) | an enum as state, a transition table | [Enums](../docs/language-reference.md#enums) |
| [09-timeouts.ae](02-actors/09-timeouts.ae) | `receive ... after`, a watchdog | [Actor timeouts](../docs/actor-concurrency.md#actor-timeouts) |
| [10-crash-isolation.ae](02-actors/10-crash-isolation.ae) | a panic stays in its actor, and the others go on | [Panics](../docs/actor-concurrency.md#panic-and-stack-traces) |
| [11-ending.ae](02-actors/11-ending.ae) | `actors.stop`, `actors.kill`, a panic; the reason a monitor's `Down` carries | [The end of an actor](../docs/actor-concurrency.md#the-end-of-an-actor) |
| [12-links.ae](02-actors/12-links.ae) | `actors.link`, `actors.trap_exits`: a failure climbs the links | [Links](../docs/actor-concurrency.md#links) |
| [13-supervisor.ae](02-actors/13-supervisor.ae) | a crashed actor restarted under the same ref, and a supervisor that gives up | [Supervisors](../docs/actor-concurrency.md#supervisors) |
| [14-bounded-mailbox.ae](02-actors/14-bounded-mailbox.ae) | `@mailbox(bound: N)`, `try_send`, what a full mailbox does to `!` | [A bounded mailbox](../docs/language-reference.md#a-bounded-mailbox) |
| [15-streams.ae](02-actors/15-streams.ae) | std.stream: a producer held to a window of credits | [std.stream](../std/stream/README.md) |
| [16-sharded-map.ae](02-actors/16-sharded-map.ae) | keys split over owner actors instead of a lock | [Sharded actor map](../docs/design/sharded-actor-map.md) |

## 3. The standard library

| Example | What it teaches | Reference |
|---|---|---|
| [01-files.ae](03-std/01-files.ae) | std.fs: a temp directory, write, read, walk, remove | [stdlib reference](../docs/stdlib-reference.md) |
| [02-json.ae](03-std/02-json.ae) | parse, read by kind, build, encode, errors with a position | [stdlib reference](../docs/stdlib-reference.md) |
| [03-processes.ae](03-std/03-processes.ae) | run a program with arguments, input, environment; read its output and status | [stdlib reference](../docs/stdlib-reference.md) |
| [04-http.ae](03-std/04-http.ae) | an HTTP server and client in one program | [HTTP server](../docs/http-server.md) |
| [05-http-server.ae](03-std/05-http-server.ae) | a long-running server: middleware, health probes, metrics | [HTTP server](../docs/http-server.md) |
| [06-http2-tls.ae](03-std/06-http2-tls.ae) | HTTP/2 and TLS on the same server API | [HTTP server](../docs/http-server.md) |
| [07-reverse-proxy.ae](03-std/07-reverse-proxy.ae) | forwarding to an upstream with std.http.proxy | [Reverse proxy](../docs/http-reverse-proxy.md) |
| [08-proxy-pool.ae](03-std/08-proxy-pool.ae) | a weighted pool configured as code | [Reverse proxy](../docs/http-reverse-proxy.md) |
| [09-logging.ae](03-std/09-logging.ae) | std.log levels, a log file, the threshold | [stdlib reference](../docs/stdlib-reference.md) |
| [10-tcp.ae](03-std/10-tcp.ae) | std.tcp: a server and a client, reading a stream up to where a message ends | [std.tcp](../std/tcp/README.md) |
| [11-cryptography.ae](03-std/11-cryptography.ae) | SHA-256, an HMAC, an Ed25519 signature and its check | [Cryptography](../docs/stdlib-reference.md#cryptography-stdcryptography) |

## 4. C and other languages

Aether compiles to C, so C is one declaration away in both directions.
The host examples need that language installed; CI's contrib/host job
builds and runs them.

| Example | What it teaches | Reference |
|---|---|---|
| [01-libc.ae](04-interop/01-libc.ae) | `extern`, C's types, `@extern("c_name")` | [The extern keyword](../docs/c-interop.md#the-extern-keyword) |
| [02-own-c/main.ae](04-interop/02-own-c/main.ae) | `@source`: a C file the program ships; strings, arrays and structs into C | [@source](../docs/c-interop.md#a-module-that-ships-its-c-source) |
| [03-callbacks.ae](04-interop/03-callbacks.ae) | `@c_callback`: C's `qsort` calling an Aether comparison | [C callbacks](../docs/c-interop.md#exporting-an-aether-function-as-a-c-callback-c_callback) |
| [04-embedding/greeter.ae](04-interop/04-embedding/greeter.ae) | `--emit=obj`: an Aether library in a C program, strings across | [--emit=obj](../docs/emit-lib.md#--emitobj-one-object-file-for-an-existing-c-build) |
| [05-hosts/python.ae](04-interop/05-hosts/python.ae) | Python run inside the program, under the sandbox | [Host modules](../docs/containment-sandbox.md#language-host-modules) |
| [05-hosts/lua.ae](04-interop/05-hosts/lua.ae) | Lua run inside the program, under the sandbox | [Host modules](../docs/containment-sandbox.md#language-host-modules) |
| [05-hosts/tcl.ae](04-interop/05-hosts/tcl.ae) | Tcl run inside the program, under the sandbox | [Host modules](../docs/containment-sandbox.md#language-host-modules) |
| [05-hosts/go.ae](04-interop/05-hosts/go.ae) | a Go program as a sandboxed child process | [Host modules](../docs/containment-sandbox.md#language-host-modules) |
| [05-hosts/tinygo.ae](04-interop/05-hosts/tinygo.ae) | a Go c-shared library loaded and called in-process | [contrib/host/tinygo](../contrib/host/tinygo/README.md) |
| [06-java-host/aether/manifest.ae](04-interop/06-java-host/aether/manifest.ae) | a namespace with a generated Java SDK, called from Java | [Embedded namespaces](../docs/embedded-namespaces-and-host-bindings.md) |
| [07-limits/script.ae](04-interop/07-limits/script.ae) | a host's limits on the code it runs: a deadline, a cancel, a memory budget | [Per-call resource caps](../docs/emit-lib.md#per-call-resource-caps) |

## 5. Containing code

Code you run does not have to be code you trust. The first three work
on every platform; the others need the operating system they name.

| Example | What it teaches | Reference |
|---|---|---|
| [01-sandbox.ae](05-containment/01-sandbox.ae) | grant lists, `sandbox.enforce`, what a refusal looks like | [Containment sandbox](../docs/containment-sandbox.md) |
| [02-trusted-calls.ae](05-containment/02-trusted-calls.ae) | a function the sandboxed block may call with your authority | [Containment sandbox](../docs/containment-sandbox.md) |
| [03-audit.ae](05-containment/03-audit.ae) | std.audit: every check, allowed or refused, read back | [Audit trail](../docs/containment-sandbox.md#audit-trail) |
| [04-spawn-sandboxed.ae](05-containment/04-spawn-sandboxed.ae) | a child process held to grants at its libc calls (Linux) | [Platform support](../docs/containment-sandbox.md#platform-support) |
| [05-capsicum.ae](05-containment/05-capsicum.ae) | FreeBSD Capsicum: open first, `enter`, then only what is held | [Capsicum](../docs/aether_compared_to_capsicum.md) |
| [06-casper.ae](05-containment/06-casper.ae) | Casper: DNS and sysctl from capability mode (FreeBSD) | [Capsicum](../docs/aether_compared_to_capsicum.md) |

## 6. Tools

What comes with `ae` besides `run` and `build`.

| Example | What it teaches | Reference |
|---|---|---|
| [01-testing/discount_test.ae](06-tooling/01-testing/discount_test.ae) | std.spec: `describe`, `it`, assertions; `ae test` | [Testing](../docs/testing.md) |
| [02-mutation/mutate.ae](06-tooling/02-mutation/mutate.ae) | std.mutation: are the tests checking the code? | [Mutation testing](../docs/mutation-testing.md) |
| [03-project/src/main.ae](06-tooling/03-project/src/main.ae) | a project: aether.toml, src/, lib/, `ae run <dir>` | [Build system](../docs/build-system.md) |
| [04-lib-path/main.ae](06-tooling/04-lib-path/main.ae) | `--lib`: the module search path and its order | [Module search path](../docs/module-system-design.md) |
| [05-ae-help/broken.ae](06-tooling/05-ae-help/broken.ae) | `ae help`: what a configuration script got wrong, and the fix | [ae help](../docs/cic-help.md) |
| [05-ae-help/fixed.ae](06-tooling/05-ae-help/fixed.ae) | the same script with the fixes applied | [Builder DSL](../docs/closures-and-builder-dsl.md) |

## 7. Programs

Whole programs, each putting several of the ideas above to work.

| Example | What it teaches | Reference |
|---|---|---|
| [01-word-count/main.ae](07-applications/01-word-count/main.ae) | a file read, split, sorted and counted | [stdlib reference](../docs/stdlib-reference.md) |
| [02-chat-room.ae](07-applications/02-chat-room.ae) | actors that keep each other in state and broadcast | [Actor runtime](../docs/actor-concurrency.md) |
| [03-build-supervisor.ae](07-applications/03-build-supervisor.ae) | `os.run_supervised`: a deadline, a process group, nothing left behind | [stdlib reference](../docs/stdlib-reference.md) |
| [04-calculator.ae](07-applications/04-calculator.ae) | a terminal UI: a button grid as a DSL, closures over ref cells | [Closures and lifetimes](../docs/closures-and-lifetimes.md) |

## 8. Rosetta Code

Classic programming tasks as [Rosetta Code](https://rosettacode.org)
states them, each a whole program whose output CI checks like the rest
(#1191). The lines are counted the same way for each language: a
non-blank line that is not only a comment, for Aether's file here (its
header and Output block left out) and for the first Go and Python
solution in [RosettaCodeData](https://github.com/acmeism/RosettaCodeData)
(1d475861, 2026-07-16). The other languages' first files are not always
whole programs (Python's GCD imports `fractions.gcd`, and its string
reversal is `input()[::-1]`), so the counts compare length, not like for like.

| Example | Task | Lines: Aether, Go, Python |
|---|---|---|
| [01-fizzbuzz.ae](08-rosetta/01-fizzbuzz.ae) | [FizzBuzz](https://rosettacode.org/wiki/FizzBuzz) | 13, 16, 9 |
| [02-ackermann.ae](08-rosetta/02-ackermann.ae) | [Ackermann function](https://rosettacode.org/wiki/Ackermann_function) | 12, 9, 3 |
| [03-fibonacci.ae](08-rosetta/03-fibonacci.ae) | [Fibonacci sequence](https://rosettacode.org/wiki/Fibonacci_sequence) | 22, 6, 10 |
| [04-factorial.ae](08-rosetta/04-factorial.ae) | [Factorial](https://rosettacode.org/wiki/Factorial) | 12, 19, 2 |
| [05-gcd.ae](08-rosetta/05-gcd.ae) | [Greatest common divisor](https://rosettacode.org/wiki/Greatest_common_divisor) | 9, 35, 1 |
| [06-sieve.ae](08-rosetta/06-sieve.ae) | [Sieve of Eratosthenes](https://rosettacode.org/wiki/Sieve_of_Eratosthenes) | 21, 33, 7 |
| [07-hanoi.ae](08-rosetta/07-hanoi.ae) | [Towers of Hanoi](https://rosettacode.org/wiki/Towers_of_Hanoi) | 9, 25, 6 |
| [08-n-queens.ae](08-rosetta/08-n-queens.ae) | [N-queens problem](https://rosettacode.org/wiki/N-queens_problem) | 48, 51, 7 |
| [09-reverse-string.ae](08-rosetta/09-reverse-string.ae) | [Reverse a string](https://rosettacode.org/wiki/Reverse_a_string) | 13, 63, 1 |
| [10-palindrome.ae](08-rosetta/10-palindrome.ae) | [Palindrome detection](https://rosettacode.org/wiki/Palindrome_detection) | 26, 11, 2 |
| [11-roman-encode.ae](08-rosetta/11-roman-encode.ae) | [Roman numerals/Encode](https://rosettacode.org/wiki/Roman_numerals/Encode) | 17, 32, 2 |
| [13-binary-digits.ae](08-rosetta/13-binary-digits.ae) | [Binary digits](https://rosettacode.org/wiki/Binary_digits) | 7, 9, 17 |
| [14-quicksort.ae](08-rosetta/14-quicksort.ae) | [Sorting algorithms/Quicksort](https://rosettacode.org/wiki/Sorting_algorithms/Quicksort) | 36, 66, 19 |
| [15-binary-search.ae](08-rosetta/15-binary-search.ae) | [Binary search](https://rosettacode.org/wiki/Binary_search) | 20, 12, 9 |
| [16-hailstone.ae](08-rosetta/16-hailstone.ae) | [Hailstone sequence](https://rosettacode.org/wiki/Hailstone_sequence) | 23, 29, 14 |
| [19-levenshtein.ae](08-rosetta/19-levenshtein.ae) | [Levenshtein distance](https://rosettacode.org/wiki/Levenshtein_distance) | 26, 34, 17 |
| [20-happy-numbers.ae](08-rosetta/20-happy-numbers.ae) | [Happy numbers](https://rosettacode.org/wiki/Happy_numbers) | 16, 26, 10 |
| [21-perfect-numbers.ae](08-rosetta/21-perfect-numbers.ae) | [Perfect numbers](https://rosettacode.org/wiki/Perfect_numbers) | 18, 29, 6 |
| [22-look-and-say.ae](08-rosetta/22-look-and-say.ae) | [Look-and-say sequence](https://rosettacode.org/wiki/Look-and-say_sequence) | 22, 28, 17 |
| [23-josephus.ae](08-rosetta/23-josephus.ae) | [Josephus problem](https://rosettacode.org/wiki/Josephus_problem) | 26, 39, 13 |
| [24-pascals-triangle.ae](08-rosetta/24-pascals-triangle.ae) | [Pascal's triangle](https://rosettacode.org/wiki/Pascal's_triangle) | 20, 35, 9 |
| [25-prime-decomposition.ae](08-rosetta/25-prime-decomposition.ae) | [Prime decomposition](https://rosettacode.org/wiki/Prime_decomposition) | 34, 35, 52 |

Over these 22 tasks: Aether 450 lines, Go 642, Python 233. Three tasks of
the tranche wait for compiler fixes and are left out rather than written
around them: Roman numerals/Decode and Leap year (#3478, an `if` value
whose condition ends in a call) and Caesar cipher (#3479, `c = s[i]`).

## Writing an example

An example is one `.ae` file, or a directory when it needs more than one
(a module, a C file). Its header comment says what it teaches and holds:

- `// Run: <command>`, the command a reader types from the repository
  root: `ae run <file or project> [--lib dir] [-- args]`, another `ae`
  command, or `sh <script>`.
- `// Needs: <what>`, when it cannot run unattended: a server that waits
  for requests, a terminal, a platform, a library. CI builds it, or
  type-checks it when it imports a contrib bridge, which only that
  bridge's job builds.
- `// See: <doc>`, the part of the documentation it builds on.

A file that belongs to another example (a module it imports) says
`// Part of: <that example's file>` instead of `Run:`. An example that
runs unattended ends with its output, exactly:

```aether,fragment
// Output:
// Hello, Aether!
```

`make check-examples` builds every example run with `ae run` (a warning
fails it), runs the ones with an `Output:` block and compares, with the
tree's own `ae` first on PATH, and checks that this README links every
example and nothing else, and that the directory holds only source. Add
the new example's row here, in the place a reader should meet it. The
checker is [tests/examples/check.ae](../tests/examples/check.ae).
