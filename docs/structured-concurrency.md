# Structured Concurrency for Aether, Design Direction

Status: **proposal**. Nothing shipped yet. This doc captures the design
choices we've committed to before writing code, so the implementation PRs
have a target to aim at.

## What we're trying to fix

Aether has actors. It does not have:

- **A way for a parent to know when its children die.** If an actor
  handler panics or exits, no notification reaches the actor that spawned
  it. Long-running supervisor patterns (Erlang-style restart-on-crash,
  graceful shutdown of a subtree) can't be written.
- **Any containment on which actors a block can reach.** A function body
  can send a message to any actor reference it can name, and `hide` /
  `seal except` don't extend that restriction to concurrency sites (spawn,
  `!`, `ask`). A sealed block can still talk to any actor whose handle
  happens to be in scope.

Both are fixable without fundamentally changing the actor model.

## The design space, and why we picked what we picked

Six families of structured concurrency exist. The summary table:

| Model | Fit for Aether | Verdict |
|---|---|---|
| Nurseries / TaskGroups (Trio, Swift, Kotlin `coroutineScope`) | Needs a new "task" primitive distinct from actors | Possible, high cost |
| Supervision trees (Erlang/OTP, Akka) | Natural extension of existing actors | **Strong candidate** |
| Async/await (Rust, Kotlin, Swift) | Breaks run-to-completion, requires coloured functions | Ruled out |
| CSP / channels (Go, Clojure core.async) | Duplicates actor mailboxes | Ruled out |
| Fork-join (Cilk, Rayon, Java ForkJoinPool) | Complements actors for data parallelism | Nice-to-have, not core |
| Capability-scoped (E, Monte, Pony refcaps) | Builds on hide/seal, unique to Aether | **Strong candidate** |

### The two rulings worth unpacking

**Async/await is ruled out** because Aether's handlers are run-to-completion.
Introducing suspension points inside a handler would break the invariant
that makes the shared-nothing actor model safe: a handler either finishes
or panics, it doesn't yield. Async/await would also force **coloured
functions** (async vs sync), which splits the ecosystem, every stdlib
function becomes two functions, every actor becomes two kinds. Aether's
cooperative scheduler gets its simplicity from the run-to-completion
rule, and async/await would throw that away.

**CSP channels are ruled out** because Aether already has actor
mailboxes. A channel is a point-to-point typed queue with a send and a
receive end; an actor's mailbox is exactly that with an identity
attached. Adding channels on top would give users two ways to do the
same thing and force stdlib authors to pick one, usually the wrong one
for half their callers.

### The two we're building

**Supervision trees** solve the "who's responsible when a child dies"
problem. They're a library-level pattern on top of primitives the runtime
*doesn't yet expose but easily could*: actor failure notification.
Parents register interest in a child; if the child dies, the parent gets
a `Down { ref, reason }` message. Everything else, restart strategies,
escalation, graceful shutdown, is ordinary actor code on top of that
one primitive.

**Capability-scoped concurrency** solves the "who can reach whom"
problem. Aether already has `hide` and `seal except` for compile-time
capability denial on *variable* references. Extending that enforcement
to *actor* references means a sealed block cannot `spawn` a type it
can't name, cannot `!` a reference it can't see, and cannot `ask` an
actor outside its whitelist. **No new mechanism is needed**, just the
existing scope-denial check applied at two more compile-time sites
(spawn, send).

The two compose: a supervised subtree that is also sealed gives you both
bounded lifetime and bounded blast radius.

## Phase 1, supervision primitives

The runtime change needed is small: actor failure notification.

### What the runtime must expose

1. When an actor's handler returns abnormally (an explicit `exit(reason)`,
   or a panic caught by the scheduler), the scheduler delivers a
   `Down { ref, reason }` message to every actor that registered
   interest in the failed actor.
2. Registration is one of two kinds:
   - **Monitor**, one-way. I get a `Down` if it dies; it doesn't hear
     about me dying.
   - **Link**, two-way (Erlang semantics). If either of us dies, the
     other gets a `Down`.
3. Delivery is best-effort at-most-once, routed through the normal
   mailbox. No new transport.

### Language / stdlib surface

```aether,fragment
// Builtins
monitor(actor_ref) -> monitor_id   // one-way
link(actor_ref)                     // two-way
demonitor(monitor_id)               // cancel monitor
unlink(actor_ref)                   // cancel link

// System message
message Down {
    ref: actor_ref
    reason: string    // "normal", "killed", or a user-supplied atom
}
```

### Supervisor as a library actor

No language-level supervisor primitive. A `std.supervise` module provides
an ordinary actor type:

```aether,fragment
import std.supervise

main() {
    sup = spawn(supervisor(strategy: "one_for_one")) {
        child("worker_a", || spawn(WorkerA()))
        child("worker_b", || spawn(WorkerB()))
        child("db_writer", || spawn(DbWriter()))
    }
}
```

The supervisor actor internally links each child, handles `Down`
messages according to its strategy, and decides whether to restart,
escalate, or shut the subtree down. Strategies mirror Erlang: `one_for_one`
(restart just the dead one), `one_for_all` (restart all siblings on any
death), `rest_for_one` (restart the dead one and everything started
after it).

Note the builder-DSL shape (`spawn(...) { child(...) child(...) }`) uses
the trailing-block + closure pattern that shipped with the closures and
embedded-namespaces features. The supervisor actor is the `_ctx` holder;
`child()` registers into it.

## Phase 2, capability-scoped concurrency

Once Phase 1 is in, this is a compile-time-only change. No new runtime
mechanism.

### What the compiler must enforce

In a block that contains `hide X` or `seal except Y, Z`, every
concurrency site is checked against the same visibility rules already
applied to ordinary reads:

- `spawn(X())` rejected if `X` is hidden or not on the seal whitelist.
- `ref ! Msg {}` rejected if `ref` is hidden or not whitelisted.
- `ref ? Request {}` (the `?` ask operator), same check on `ref`.

The error message reuses the existing `E0304` (hidden in this scope)
diagnostic.

### Worked example

```aether,fragment
sandbox("payment-handler") {
    grant_tcp("payments.example.com")
    seal except req, res, payment_client, logger

    // This block can only see the five whitelisted names.
    // It cannot spawn any actor type not named here.
    // It cannot send to any actor reference not named here.
    // It cannot ask any outside service.

    result = payment_client ? Charge { amount: req.amount }
    if result != null {
        logger ! Audit { who: req.user, what: "charged", amount: req.amount }
        res.status = 200
    } else {
        res.status = 502
    }
}
```

The subtree rooted at this block has both lifetime containment (the
`sandbox` scope's containment runtime) and capability containment (the
`seal except` list). A malicious or buggy handler cannot spawn a
crypto-miner actor because it cannot name one.

## What's not in scope

- **Fork-join / `parallel_for`**, useful for data parallelism,
  does not solve the lifecycle or reachability problem. Can be added
  later as a narrow stdlib feature layered on the existing
  work-stealing scheduler. Not part of this design.
- **Nurseries**, would require a second concurrency primitive
  (lightweight tasks distinct from actors). High cost for small
  marginal benefit given the actor model already exists. Revisit only
  if users ask for it after Phases 1 and 2 ship.
- **Channels**, actors already cover this. No plan.

## Order of work

1. **Runtime, `Down` message delivery.** Scheduler catches handler
   exits, iterates the monitor/link table, enqueues `Down` messages.
2. **Builtins, `monitor`, `link`, `demonitor`, `unlink`.** Thin
   wrappers over the runtime table.
3. **`std.supervise` module.** Supervisor actor type with the three
   strategies. Builder DSL for child registration.
4. **Tests.** Restart-on-crash, escalation, graceful shutdown, monitor
   vs link asymmetry, double-death ordering.
5. **Compiler, capability-scoped spawn/send/ask.** Extend existing
   `hide`/`seal except` denial to concurrency sites.
6. **Docs and worked example.** Update `docs/actor-concurrency.md` with
   the supervision section. One worked example in `examples/actors/`
   showing a supervised, sealed subtree.

Phase 1 can ship without Phase 2. Phase 2 can ship independently once
Phase 1 is in, they touch different compiler subsystems.

## References

- [`docs/actor-concurrency.md`](actor-concurrency.md), current actor
  runtime.
- [`docs/hide-and-seal.md`](hide-and-seal.md), existing
  capability-denial primitive that Phase 2 extends.
- [`docs/containment-sandbox.md`](containment-sandbox.md), filesystem /
  network capability grants, natural pairing with Phase 2.
- [Erlang OTP design principles](https://www.erlang.org/doc/design_principles/des_princ.html)
  the supervision-tree pattern we're borrowing.
- [Nathaniel Smith, *Notes on structured concurrency*](https://vorpus.org/blog/notes-on-structured-concurrency-or-go-statement-considered-harmful/)
  the Trio post that named the pattern. Aether takes the "structure"
  part but not the nursery/task primitive.
