# The host calls the guest: `READY`, `INVOKE`, `RETURN`, `RAISE`, `END`

The design of record for the host-to-guest direction of `std.guest`. The
protocol itself, frame by frame, is in
[`std/guest/README.md`](../../std/guest/README.md); this note says why it is
shaped as it is, what was considered and lost, what holds a serving guest,
and which questions are still open.

## The question

`std.guest` runs a guest language in a contained child process and talks to
it over `std.ipc`. The host speaks first (`MOUNT`, `PROBE`, `SCRIPT`), and
until this verb the guest then started every exchange: it called a host
function by name and waited for the answer, one call at a time, and the host
could only answer or `CANCEL`. A contained guest could leave nothing behind
for the host to call later: no event handler, no callback for a
transaction it started, no timer firing into it, no comparator the host
sorts with. In process the family has all of these (sae's pages register
handlers its kernel calls at any time; the Lua, Python, Duktape and Tcl
bridges let a host `eval` again and again against one interpreter), which
kept the contained child from being the default for anything but a
one-shot script.

## The decision

Paul, 2026-11-21: add the smallest verb that closes it, and document the
zero-change alternative beside it.

- When its script ends, a guest that left functions behind sends `READY`
  with the list of their names. A guest that exports nothing sends `DONE`
  exactly as before, so nothing changes on the wire for it.
- The host then sends `INVOKE name [args]` and gets back `RETURN value` or
  `RAISE why`, surfaced as Aether's usual `(value, err)`.
- The host may call only while the guest is idle. Host and guest strictly
  alternate, so there is no deadlock to design around.
- `END` finishes a serving guest cleanly (`DONE 0`).

Out of scope, deliberately: function handles, nested call-backs, request ids
or asynchrony, reuse of a worker across scripts, streaming, a batch verb.

## Ways of calling across the divide

The verb is a general call: an `INVOKE` carries whatever arguments the
function takes, of any JSON type and of any size up to the frame limit,
and a `RETURN` any value. How an application uses it is its own choice;
three ways are set out here side by side, each with when it fits. They mix
freely in one guest.

### 1. A direct call, with its arguments

The host calls the export with everything it needs and takes its value.
The simplest way, and the right one for small values and pure functions:
a spreadsheet's user-defined function called per cell, a comparator, an
event handler given the event.

The guest registers `on_tick` and a comparator; the host calls `on_tick`
three times and sorts a list of its own with `compare`, all while the guest
is idle between calls (the host's sort never runs inside a guest `CALL`,
so this needs no nesting). The host, in Aether (`guest.run_script` and
`guest.call_export` write the `next` loop once; the handler answers the
guest's own calls to the host):

```aether
import std.guest
import std.sandbox

main() {
    perms = sandbox.new("worker") {}
    g, err = guest.launch(perms, "build/my-worker", null, "ticks", "build/audit.log")
    if err != "" {
        println("not started: ${err}")
        return
    }
    guest.mount(&g, "log", 1, 1)
    on_call = | h: *guest.Guest, ev: guest.Event | {
        if ev.kind == "CALL" && ev.name == "log" {
            println("guest: ${guest.printable(ev.text)}")
            guest.answer(h, "null")
        }
    }
    e = guest.run_script(&g, "...the script...", on_call)
    if e.kind != "READY" { return }            // DONE: it exported nothing
    n = 1
    while n <= 3 {
        v, verr = guest.call_export(&g, "on_tick", "[${n}]", on_call)
        if verr != "" { println("on_tick raised: ${guest.printable(verr)}") } else { println("ticks: ${v}") }
        n = n + 1
    }
    order, cerr = guest.call_export(&g, "compare", "[\"pear\",\"apple\"]", on_call)
    if cerr == "" { println("compare: ${order}") }   // 1: pear sorts after apple
    guest.end(&g)
    e = guest.next(&g)                         // DONE 0
}
```

The guest, as each worker's export mark reads (the mark and the serving
loop are each worker's own, in aether-guest-languages; this is the shape,
not a promise of the spelling):

```python
ticks = []

@export
def on_tick(n):
    ticks.append(n)
    log("tick %d, %d so far" % (n, len(ticks)))
    return len(ticks)

@export
def compare(a, b):
    return (a > b) - (a < b)
```

```lua
-- Lua: export(name, fn) from the worker's prelude
local ticks = 0
export("on_tick", function(n)
  ticks = ticks + 1
  log(string.format("tick %d, %d so far", n, ticks))
  return ticks
end)
export("compare", function(a, b) return a < b and -1 or (a > b and 1 or 0) end)
```

```rexx
/* Rexx on RexxAE: the main program EXPORTs labels and ends */
ticks = 0
LET ok = EXPORT('on_tick')
LET ok = EXPORT('compare')
EXIT 0
ON_TICK:
  PARSE ARG n
  ticks = ticks + 1
  LET ok = log('tick' n',' ticks 'so far')
  RETURN ticks
COMPARE:
  PARSE ARG a, b
  IF a < b THEN RETURN -1
  IF a > b THEN RETURN 1
  RETURN 0
```

In JavaScript (Duktape, QuickJS) `export` is a reserved word, so a prelude
there names it `export_fn`. Tcl exports `proc`s by name, Perl and Ruby a
sub or method by name. The Go worker's script is `call NAME [JSON]` lines:
it has nothing to export.

### 2. Notify, then fetch

A suggested pattern, borrowed from message-bus experience: after years of
pushing full payloads, much of that world settled on sending the smallest
notification that tells the recipient to phone home for more. Here the
notification is an ordinary invocation whose argument is an id, and the
guest phones home through a host function the host mounted:

1. The guest's script asks the host to start something through a mounted
   function that answers at once with an id (`begin_lookup("alice")`
   returns `"123456"`). The script ends, and the guest serves.
2. Later, on the host's schedule, the host invokes an export with the id:
   `on_txn("123456")`.
3. Inside the export the guest fetches the details through another
   mounted function (`txn_details("123456")`), does its work, and returns.

It fits when the details are large (the notification stays a few bytes,
and the data crosses only if the guest asks for it); when the host wants to
keep authority over what is read (the fetch is a host function, so it can
refuse an id, return less, or grant a file as a descriptor); when a
notification may be dropped or coalesced (several "something changed"
notifications cost one fetch of the current state); and when the audit log
should show the notification and the read apart (`INVOKE on_txn` is a line
std.guest writes, the fetch one the host's function writes with
`guest.audit`). It needs nothing from the protocol: the id is application
data, an ordinary argument, and the fetch happens inside the invocation, so
strict alternation holds. It costs a second round trip, which a direct call
does not.

The host's side of the notification and the fetch:

```aether,fragment
v, verr = guest.call_export(&g, "on_txn", "[\"123456\"]") callback | h: *guest.Guest, ev: guest.Event | {
    if ev.kind == "CALL" && ev.name == "txn_details" && ev.text == "[\"123456\"]" {
        guest.audit(h, "FETCH txn 123456")
        guest.answer(h, "{\"who\":\"alice\",\"amount\":12}")
    } else if ev.kind == "CALL" {
        guest.fail(h, "unknown txn")          // the guest sees a failed call
    }
}
```

The guest's side, in Python:

```python
started = set()
started.add(begin_lookup("alice"))         # a mounted host function

@export
def on_txn(txn):
    if txn not in started:
        raise KeyError("unknown txn %s" % txn)  # RAISE; the guest serves on
    details = txn_details(txn)              # the fetch: the host may refuse it
    return details["amount"]
```

`tests/integration/std_guest_invoke` runs this exchange against aether's
own test worker (two notifications in a row with the guest's state kept,
a transaction the guest never started, an id the host's fetch refuses),
beside the direct calls of the value grid and the safety rows.

### 3. The pull pattern, with no protocol change

Before the verb existed a guest could already be driven by the host: its
script loops on a mounted host function, `next_event`, that blocks until
the host has something for it. Each answer says what to run; the outcome
rides on the next call. The host keeps the guest's `next_event` `CALL`
unanswered ("parked") until it has work.

The guest (Python):

```python
handlers = {"on_tick": on_tick, "compare": compare}
event = next_event()                        # parks until the host has work
while event is not None:
    try:
        outcome = ["R", handlers[event["name"]](*event["args"])]
    except Exception as e:
        outcome = ["E", str(e)]
    event = next_event(*outcome)
```

The host (Aether), one call: answer the parked `next_event` with what to
run, then run until the guest parks again; its arguments are the outcome:

```aether,fragment
// The guest is parked in next_event (a CALL the host has not answered).
guest.answer(&g, "{\"name\":\"on_tick\",\"args\":[1]}")
while true {
    e = guest.next(&g)
    if e.kind == "CALL" && e.name == "next_event" { break }   // parked again: e.text is ["R", value] or ["E", why]
    if e.kind == "CALL" { answer_host_function(&g, e) }       // its other host calls, as usual
    if e.kind != "SAY" && e.kind != "LOG" && e.kind != "CALL" { break }
}
```

While the guest is parked the host must not call `next`: the guest is
waiting on the host, so the deadline would pass and cancel a healthy guest
(the aether-child-sandboxing showcase's `rexx/stuck.rexx` is that case on
purpose). With the verb, `next` on an idle guest returns `IDLE` instead.

It fits when one application owns both ends and wants an event shape of
its own (a stream of typed events rather than calls), when the worker in
hand has not learnt to export yet, or in Rexx, where a call through an
exported label still costs about three times a pull round trip (the
measurements below).

| | Pull (`next_event`) | The verb |
|---|---|---|
| Protocol change | none | five frames, the `Guest` phase |
| The guest's author writes | the loop and the dispatch (seven lines of Python; an `IF` chain in Rexx) | the functions and an export mark |
| The host's author writes | the parking, and must not call `next` while parked | `call_export`, or `invoke` and `next` to `RETURN`/`RAISE` |
| Errors | a convention per application (`"R"`/`"E"` above) | `RAISE`, `guest.result` |
| The host knows the guest's surface | no | `READY` |
| Audit log | every notification looks like a `CALL next_event` | `READY`, `INVOKE`, `RAISE` and `END` are lines of their own |
| Deadlines, cancel, frames | identical: the same frames underneath | identical |
| Cost per call | the same in Python; cheaper in Rexx (below) | |

**Choosing.** A direct call when the values are small and the function is
pure or self-contained. Notify then fetch when the payload is large, the
host should decide what is read, or notifications may be dropped or
coalesced. Pull when no worker change is wanted, or the application wants
its own event vocabulary. The verb, either way it is used, is the one to
reach for when the guest's surface should be something the host, or the
family, knows and shares: named entry points, errors that are errors, an
audit log that reads as what happened.

## The protocol

| Frame | Direction | When | Means |
|---|---|---|---|
| `READY <json array>` | worker to host | instead of `DONE`, when the script ends | the guest serves these functions: 1 to 256 distinct names of letters, digits, `_` and `.`, at most 128 bytes each |
| `INVOKE <name> <json array>` | host to worker | only while the guest serves | call the export `name`; `SAY`, `LOG`, `CALL` and `AUDIT` may come on the way, as in a script |
| `RETURN <json>` | worker to host | answers `INVOKE` | the function's value; the guest serves again |
| `RAISE <message>` | worker to host | answers `INVOKE` | the function failed; the guest serves again |
| `END` | host to worker | only while the guest serves | the host is finished; the worker answers `DONE 0` and exits |

One frame is new beside the five: `MAXFRAME <n>`, host to worker before
`SCRIPT`, sent only when the host's `max_frame` is below 64 KiB, so the
worker holds what it sends to what the host reads (at the default nothing
extra goes, and the wire is the prototype's). Otherwise unchanged from the
prototype on branch `explore/guest-host-calls-guest` (cc2f4b6af): every
frame keeps its shape, so a worker written against the
prototype speaks this protocol. Two things are stricter on the host, and
neither changes a frame a correct worker sends: a frame from an idle guest
hangs up (the prototype read it as the next invocation's answer), and
after `END` only `AUDIT`, `DONE` and `ERROR` are accepted.

The guest's conversation, as the host tracks it (`Guest.phase`):

| Phase | From | To | The host may |
|---|---|---|---|
| `SCRIPT` | `launch` | `READY` (`SERVING`), `DONE`/`ERROR`/`KILLED` (over) | answer `CALL`s, `cancel` |
| `SERVING` | `READY`, `RETURN`, `RAISE` | `invoke` (`INVOKING`), `end` (`ENDING`), `cancel` (`CANCELLED`) | `invoke`, `end`, `cancel`, `close`; `next` returns `IDLE` |
| `INVOKING` | `invoke` | `RETURN`/`RAISE` (`SERVING`), a cancel (`CANCELLED`), the end | answer `CALL`s, `cancel` |
| `ENDING` | `end` | `DONE` | wait for it |
| `CANCELLED` | `cancel`, a passed deadline | `ERROR`/`DONE` (over) | wait for it; a `READY`, `RETURN` or `RAISE` already on its way is dropped and audited |

## The API

Host, `std.guest`, on the existing `Guest` handle (nothing global):

| | |
|---|---|
| `invoke(&g, name, args)` | send `INVOKE`; `""` or why nothing was sent |
| `next(&g)` | the invocation's events, ending with `RETURN` or `RAISE`; `IDLE` on a serving guest |
| `result(e)` | `(value, "")` for `RETURN`, `("", why)` for `RAISE` or the end of the guest |
| `end(&g)` | send `END`; `next` then gives `DONE 0` |
| `run_script(&g, source, handler)` | start the script and run it to `READY`, `DONE`, `ERROR` or `KILLED`, handing `SAY`, `LOG` and `CALL` to `handler(*Guest, Event)` |
| `call_export(&g, name, args, handler)` | `invoke`, then the same loop to the invocation's end, as `(value, err)` |
| `is_exported(&g, name)`, `g.exported` | what `READY` announced, written `["a","b"]` |
| `is_serving(&g)`, `g.phase` | where the conversation is |

`run_script` and `call_export` are the same loop (`until_settled`): the
handler answers the guest's calls exactly as a hand-written loop would, and
a `CALL` it leaves unanswered is failed for it (`the host has no answer for
'NAME'`), so a forgotten branch cannot leave the guest waiting until the
deadline cancels it. They exist so a host never hand-writes the loop the
prototype kept in its test (`call_guest`); the handler is a closure, so it
carries whatever state the host's functions need.

Worker, `std.guest.child`, on the existing `Host`:

| | |
|---|---|
| `ready(h, names_json)` | instead of `done`: announce the exports and serve |
| `serve(h)` | wait, with no deadline, for an `Invocation`: `INVOKE` (`name`, `args`, also `h.invoked`), `END` or `CANCEL` |
| `returned(h, json)`, `raised(h, why)` | answer the invocation; the guest serves on |
| `is_exported(h, name)` | what `ready` announced |

`serve` answers an `INVOKE` of a name `ready` did not announce with `RAISE`
itself (std.guest never sends one; a host in another language might).
`h.invoked` lets engine glue fetch the arguments from a callback instead of
splicing them into source: the prototype measured a 60 KiB argument
spliced as a Python string literal at three times the cost of a host call,
and the same through a callback at the same cost.

## Containment

The child gains no capability by being callable: an `INVOKE` reaches only a
name the guest itself exported, and everything the invoked function does
still goes through the host's mounted functions under the same lockdown.
The new surface is on the host, and each rule has a test:

| Risk | Rule | Test |
|---|---|---|
| An export that loops | `deadline_ms` per exchange inside the invocation, then `CANCEL`, which ends the guest | `guest-export-slow` (integration); `std/guest/test_guest.ae` "cancels an invocation that outlives the deadline" |
| An export that blocks where no cancel is heard | `grace_ms`, then the kill | `guest-export-silent` |
| An oversized `RETURN` or `RAISE` | `max_frame`, as for every frame: refused, killed | `guest-export-oversize`; "refuses a RETURN over max_frame" |
| An `INVOKE` larger than a worker reads | `invoke` refuses a frame over `MAX_FRAME` and sends nothing; the guest serves on | "refuses an INVOKE larger than the frame limit" (both levels) |
| A value too large for a frame, from a polite worker | `child.returned` refuses a `RETURN` over the host's limit (64 KiB, or a lower `max_frame` the host sent as `MAXFRAME` before the script), and the worker raises why and serves on; `raised` cuts a message to fit | "a worker that knows its value is too large ... raises instead"; `test_child` "holds RETURN and RAISE to the host's frame limit" |
| An unbounded or bogus export list | `READY` checked: 1 to 256 distinct names, letters, digits, `_`, `.`, at most 128 bytes, JSON spacing allowed; else `KILLED protocol: READY ...` | `guest-export-flood`, `guest-export-malformed`; "checks the READY list" (twelve refusals, the bounds accepted) |
| The host re-entering the guest from a host function's handler | `invoke` and `end` are refused, and send nothing, unless the guest serves | `guest-reenters-while-called`; "refuses invoke and end unless the guest serves" |
| A guest answering what was not asked | `RETURN`/`RAISE` with no `INVOKE`, a second `READY`, anything but `DONE` after `END`: hang up | "hangs up on what nobody asked for" |
| A frame from an idle guest | found by `next`, `invoke` or `end` before anything is sent: hang up | "a frame from an idle guest is unsolicited"; the same over a socket pair |
| A guest sending `INVOKE` | an unknown verb from a worker: hang up | "a guest cannot send INVOKE" |
| `next` on an idle guest running the deadline down | `next` returns `IDLE` without reading; an idle guest has no deadline | "an idle guest has no deadline" |
| An exception in an export | `RAISE`; the guest still serves | `guest-export-raises` |
| A call after the end | refused: `it is ending`, then `the guest has finished` | `guest-after-end` |
| A cancel inside an invocation | ends the guest; a `RETURN` already on its way is dropped and audited | "ends the guest on a cancel inside an invocation" |

The integration rows run contained on Linux (`sandbox.spawn_socket`:
Landlock, seccomp, the preload) and uncontained by name elsewhere.

## Types

`INVOKE`'s arguments are a JSON array and `RETURN`'s value is JSON: the
same values the guest-to-host direction carries. The integration test sends
each of these as an invocation's argument and checks it back both from
`RETURN` and through a `CALL` the export makes to the host: the empty
string, plain, non-ASCII, non-BMP raw and as a surrogate escape, an
embedded NUL (`"a\u0000b"`), quotes, backslashes and control characters,
0, -1, the 64-bit bounds, 1.5 and -2.5e-300, `true`, `false`, `null`, `[]`,
`{}`, nested arrays and maps, and a 60 KB string. `std.guest` treats the
JSON as text and frames it with a length, so it carries all of them byte
for byte; whether a worker's engine keeps each value (a 64-bit int in
JavaScript, a NUL in Tcl) is that worker's row in aether-guest-languages'
boundary table, as for the other direction. `guest.json_string` and
`child.json_string` write an embedded NUL as `\u0000`, and a raw NUL in a
frame arrives whole. A descriptor does not travel up, so an export cannot
return a file.

## Options considered

**Shape of the verb.**

| Option | Gives | Lost because |
|---|---|---|
| **Exports by name, declared at `READY`** (chosen) | the host knows the guest's surface, as the guest knows the host's mounts; no lifetimes | |
| Function handles as values (`{"$fn": 7}`, `INVOKE` by handle, `RELEASE`) | first-class callbacks, anonymous closures | every worker needs a handle table, the host a release discipline and a cap, and a handle used after release is a new error class. It can be added later without changing names: a handle is an export with a generated name, plus `RELEASE`. Wanted only when a contained sae page needs anonymous handlers. |
| Both | the in-process experience | twice the surface |

**Concurrency.**

| Option | Lost because |
|---|---|
| **(a) Script, then serve; strict alternation** (chosen) | |
| (b) Nested call-backs: the host invokes while answering a guest `CALL` | every worker must run its engine re-entrantly from a native callback (Rexx cannot: a RexxAE context waiting on the host cannot run another routine), an inner cancel must unwind the outer call, and the outer deadline runs during the inner one. The one case it serves, a guest asking the host to sort with the guest's comparator, can be written with no protocol change: the host answers the guest's `CALL sort` with a request to compare, the guest's prelude runs the comparator and calls back, until the host answers with the sorted list. |
| (c) Full asynchrony, request ids both ways (sae's app-backend shape) | an event loop in every worker; real deadlocks; per-request cancellation. Right for a child that is a program of its own (sae's backends already have it), not for an engine. |

**Prior art** agreed on what was kept: declare up front what each side
exports (LSP's `initialize`, WASI's imports and exports); keep re-entrancy
out without an event loop on both ends (LSP, the Debug Adapter Protocol's
reverse requests need ids); make cancellation a message; let the stronger
side kill (Erlang ports). JSON-RPC's named methods and `(result | error)`
are `INVOKE` and `RETURN`/`RAISE` without the ids that only overlapping
calls need.

**Lifetime.** One worker, one script, then serve: a second `SCRIPT` would
need a reset story per engine, and a new worker is cheaper to reason about.
Exports are fixed at `READY`. The host ends a serving guest with `END`;
`close` still hangs up and kills at any time.

## Compatibility

No negotiation and no back-compat. On the wire, a worker that exports
nothing sends what it always sent, and a host that never invokes sees
nothing new: aether's own `tests/integration/std_guest` (17 specs) runs
unchanged against the extended test worker. In the API, `guest.Guest` has
three more fields (`phase`, `exported`, `pending`), so every struct literal
of it changes (two in aether's tests). `next` may return `IDLE`, but only
for a guest that sent `READY`. Each worker that gains exports reserves a
name in its language for the mark (`export` in Python, `EXPORT` in RexxAE's
worker), which a host can then no longer mount there.

## Still open

Paul is taking these to Nic and Adil. Each is decided one way in the code,
in one named place, so flipping it is a small change.

1. **A cancel inside an invocation ends the worker** (alternative: end only
   the invocation, `RAISE cancelled`, and keep the worker). The function's
   state is half done, so the worker ends, as a cancelled script does; a
   spreadsheet might prefer to keep it. Decided in `std/guest/module.ae`,
   `cancelling`: every cancel, the host's own and a passed deadline's,
   enters `CANCELLED`, where only `ERROR`/`DONE` end the exchange. To flip
   it: keep an `INVOKING` guest `INVOKING` there, accept `RAISE` as the
   answer to the cancel in `deadline_passed`, and have workers answer a
   cancel inside an invocation with `raised` (the `Invocation` comment in
   `std/guest/child/module.ae` says what workers do today).
2. **No total time budget per invocation** beyond the per-exchange
   deadline (alternative: a budget). An export that keeps calling the host
   runs as long as the host keeps answering, exactly as a script that does
   so. Decided in `exchange_deadline_ms`, which every read in `next` and
   `deadline_passed` asks; a budget would return what remains of it, when
   smaller, while the phase is `INVOKING`.
3. **`INVOKE` carries no sequence number** (alternative: a number that
   `RETURN` and `RAISE` echo). What can be checked without one is: a frame
   from an idle guest is unsolicited and hangs up, whether `next`,
   `invoke` or `end` finds it. What cannot: a `RETURN` the guest sends
   after the host's `INVOKE` has gone but before the guest read it is
   taken as that invocation's answer. The guest can only lie about its own
   function, which it could anyway. Decided in `unsolicited_while_idle`;
   an id would be written by `invoke` and checked in `next`'s `RETURN` and
   `RAISE`. The integration test "a frame from an idle guest is
   unsolicited" shows both halves: the early `RETURN` taken, the late one
   caught.

Also open, and not for this verb: handles (until a contained sae page needs
anonymous handlers); a `rexxae_call(ctx, label, args)` entry point in
RexxAE, which would make Rexx exports as cheap as the pull pattern and drop
the worker's generated serving loop; a batch `INVOKE` for spreadsheet
recalculation, until a measurement needs it.

## The stunt catalogue

`contrib/host/stunt` is the test double every bridge is held to by row
name (`stunt_catalogue_name`), but its child mode speaks its own small
protocol (`CALL`/`OK`/`ERR`/`RESULT`), not `std.guest`'s, so it cannot
carry these rows without learning `std.guest`'s frames: a change of medium
size to the double every bridge depends on, not made with the verb. The
rows are held in aether by name instead, as the spec names of
`tests/integration/std_guest_invoke` (`guest-export-called`, the value
grid called both ways, then `guest-export-slow`, `-silent`, `-oversize`,
`-raises`, `guest-reenters-while-called`, `guest-export-flood`,
`-malformed`, `guest-after-end`), each with the exact `KILLED`/`ERROR`
text a host sees. For aether-guest-languages to hold its workers to them
by name it needs from aether: a released tag with this verb (its
`ci/versions.env` pins by tag); these row names and outcome texts as the
stable reference (today the integration spec names, and the texts in
`std/guest/module.ae`); and its own `tests/boundary.py` run in the new
direction with the same value grid. If the catalogue should grow these
rows itself, the step is stunt's child mode speaking `std.guest.child`,
after which the rows are ordinary host-checked entries.

## Measurements (the prototype, 2026-11-21)

10 000 calls, one string argument echoed back; microseconds per call. `g2h`
is the guest calling the host (including the worker's start, spread over
the calls), `h2g` the host invoking the guest, `pull` the same through
`next_event`.

| Worker | Host | 16 B: g2h | h2g | pull | 60 KiB: g2h | h2g | pull |
|---|---|---|---|---|---|---|---|
| Python | ae-x64, contained | 116 | 112 | 119 | 456 | 456 | 545 |
| Rexx | ae-x64, contained | 72 | 311 | 117 | 1045 | 3753 | 1761 |
| Python | Mac, uncontained | 17 | 16 | 14 | 154 | 154 | 154 |
| Rexx | Mac, uncontained | 18 | 65 | 23 | 308 | 1247 | 469 |

In Python a host-to-guest call costs what a guest-to-host call costs: the
protocol adds nothing measurable, and 1000 spreadsheet cells calling a
user-defined function contained would cost about 0.1 s. In Rexx `INVOKE`
costs about three times the pull pattern, and the difference is RexxAE's,
not the protocol's: an exported label is a `CALL` with its arguments
copied in and its `RESULT` copied out (each copy of 60 KiB is slow in
RexxAE), plus the worker's dispatch loop. The contained lane is about seven
times the Mac's in every column alike (a VM, and the preload and seccomp on
every syscall).

## Found on the way

For the compiler, seen in the prototype on released 0.934.0: inside a
trailing `callback` block passed to a module's function, an unqualified
call to a local function resolves to that module's export of the same name
instead, against `docs/closures-and-builder-dsl.md` ("explicit local
definitions still shadow the receiver fallback"):

```aether,fragment
import std.guest.child
main() {
    h = child.attach(-1, "x")
    child.run(h) callback {
        say(1, 2, 3)        // error: Function 'child.say' expects 2 argument(s), got 3
    }
}
fn say(a: int, b: int, c: int) { println("${a + b + c}") }
```

The same call outside the block builds. The prototype's Python worker
names its serving function `serve_exports` because of it, as the test
worker here does.
