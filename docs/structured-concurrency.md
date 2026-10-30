# Structured Concurrency for Aether, Design Direction

Status: **Phase 1 (supervision) shipped** (#2974); **Phase 2
(capability-scoped concurrency) is a proposal**. This doc captured the
design choices before the code, and now records where Phase 1 landed.

## What we're trying to fix

Aether had actors. It did not have:

- **A way for a parent to know when its children die.** If an actor
  handler panicked or exited, no notification reached the actor that
  spawned it. Long-running supervisor patterns (Erlang-style
  restart-on-crash, graceful shutdown of a subtree) could not be written.
  Phase 1 fixed this.
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
problem. They're a library-level pattern on top of a primitive the runtime
now exposes: actor failure notification. Parents register interest in a
child; if the child dies, the parent gets a `Down { ref, reason }`
message. Everything else, restart strategies, escalation, graceful
shutdown, is ordinary actor code on top of that primitive, plus restart
in place so a restarted child keeps its ref.

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

## Phase 1, supervision: shipped

Phase 1 is in (#2974). What shipped, and where it went past or away from
the proposal:

### What the runtime exposes

1. An actor ends once, by the first of: a panic in its handler (caught by
   the step's barrier, on both schedulers where the target has setjmp),
   `actors.kill(ref)`, `actors.stop()` from its own handler, the death of
   an actor it is linked to, or a C host's release. Its death is finalized
   on the thread that owns its mailbox while it steps nothing: what was
   queued for it is dropped and counted, and every actor watching it gets
   one `Down { ref, reason }`: `"normal"`, `"killed"`, or the panic's
   message. A `Down` never comes while the dead actor's handler still
   runs, and comes after the last messages it sent its watcher.
2. **Monitor**, one-way: `actors.monitor(ref)` returns an id;
   `actors.demonitor(id)` drops it, and no `Down` of it is handled after,
   one already on its way included. A monitor of an actor that has ended
   already is answered at once.
3. **Link**, two-way: `actors.link(ref)`. When either ends, the other ends
   with the same reason, unless it is `"normal"`; an actor that called
   `actors.trap_exits(true)` gets a `Down` instead and lives on.
4. **Restart in place**: a supervisor restarts a dead child under the ref
   its siblings and clients hold (`actors.rebirth_begin`, the child's
   factory, `actors.rebirth_end`); nothing sent before the restart reaches
   the new incarnation.
5. Delivery is at most once and counted, through the normal mailbox (see
   docs/actor-concurrency.md, "The delivery contract"), with an opt-in
   mailbox bound (`@mailbox(bound: N)`, `try_send`).

### Language / stdlib surface

The proposal had bare builtins (`monitor`, `link`, `demonitor`,
`unlink`). They are `std.actors` functions instead, since bare `kill`,
`stop`, `unlink` and `monitor` collide with std.os, std.audio, std.fs and
std.http under glob imports:

```aether,fragment
import std.actors

id = actors.monitor(ref)        // one-way, from a handler
actors.demonitor(id)
actors.link(ref)                // two-way, from a handler
actors.unlink(ref)
actors.trap_exits(true)
actors.kill(ref)                // from anywhere
actors.stop()                   // from the actor's own handler
```

`Down` is built into the compiler (`message Down { ref: actor_ref, reason:
string }`, the reserved id 254), since the runtime writes its payload; a
program's own `message Down` is refused. On wasi a panic still ends the
program (no setjmp), so `monitor`, `link` and `trap_exits` are refused at
compile time there.

### Supervisor as a library actor

`std.supervise` is an ordinary actor on these pieces, with the builder
shape the proposal sketched:

```aether,fragment
import std.supervise

main() {
    sup = supervise.start(strategy: "one_for_one", max_restarts: 3, within_secs: 5) {
        supervise.child("worker_a", || -> spawn(WorkerA()))
        supervise.child("worker_b", || -> spawn(WorkerB()))
        supervise.child_restart("db_writer", || -> spawn(DbWriter()), "transient")
    }
}
```

Strategies mirror Erlang: `one_for_one`, `one_for_all`, `rest_for_one`;
restart kinds `permanent`, `transient`, `temporary`; past `max_restarts`
within `within_secs` the supervisor kills its children and ends with a
reason that says why, so a supervisor above it takes the failure. See
std/supervise/README.md and examples/02-actors/13-supervisor.ae.

## Phase 2, capability-scoped concurrency

Phase 1 is in; this is a compile-time-only change. No new runtime
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
handler = sandbox.new("payment-handler") {
    grant_tcp("payments.example.com", 0)
}
sandbox.enforce(handler) callback {
    seal except req, res, payment_client, logger

    // This block can only see the four whitelisted names, and its
    // network access is limited to payments.example.com.
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
`sandbox.enforce` block's runtime checks) and capability containment (the
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

1. **Runtime, `Down` message delivery.** Done (#2974): the death is
   finalized on the dead actor's own thread, its watchers sent `Down`.
2. **`monitor`, `link`, `demonitor`, `unlink`.** Done (#2974), as
   `std.actors` functions, with `trap_exits`, `kill`, `stop`, and restart
   in place.
3. **`std.supervise` module.** Done (#2974): the three strategies, restart
   kinds, a restart budget, the builder DSL.
4. **Tests.** Done (#2974): restart on crash, escalation past the budget,
   monitor and link, kills mid-handler, restarts racing senders on three
   cores (tests/runtime/test_actor_death.c, test_actor_rebirth.c,
   tests/regression/test_actor_down_notices.ae, test_actor_links.ae,
   test_supervise.ae).
5. **Compiler, capability-scoped spawn/send/ask.** Phase 2, not started.
   Extend existing `hide`/`seal except` denial to concurrency sites.
6. **Docs and worked example.** Done for Phase 1:
   `docs/actor-concurrency.md` ("The delivery contract", "The end of an
   actor", "Supervisors") and examples/02-actors/13-supervisor.ae. The
   sealed subtree waits for Phase 2.

Phase 1 shipped without Phase 2. Phase 2 can ship independently, they
touch different compiler subsystems.

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
