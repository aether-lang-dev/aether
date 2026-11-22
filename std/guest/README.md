# std.guest

A guest in a separate, sandboxed child process, and a conversation with it
both ways over [`std.ipc`](../ipc/README.md).

The guest is a *worker*: one program with an engine for some language
compiled in (a Rexx interpreter, a JavaScript engine, anything that runs a
script), and nothing of its own to reach the world through. The host
starts it, sends it a script and the functions it may call, and answers
every call. The script's effects all cross the socket, where the host
decides each one.

Two modules, one for each end:

| Module | Who uses it | What it does |
|---|---|---|
| `std.guest` | the host | starts the worker held to a grant list, sends it the script, answers its calls, bounds every exchange with a deadline, cancels and kills, keeps the audit log |
| `std.guest.child` | the worker | everything about the protocol, so a worker is its engine plus the glue between the two: how a script runs, and how a mounted function looks in its language |

Two walls hold the worker. The **process sandbox**: `guest.launch` spawns it
with `sandbox.spawn_socket`, which fails closed: where it cannot contain the
child (macOS, Windows; Linux without Landlock) it starts nothing.
`guest.launch_uncontained` is the explicit alternative, asked for by name.
And the **worker's own**: `child.run` enters `sandbox.enforce` with no grants
before the script's first step, so even a bug in the engine is held to
nothing, and a file reaches it only as a descriptor the host hands over.

## The host

The example **compiles but is not run** in CI: it needs a worker program.

```aether
import std.guest
import std.sandbox

main() {
    perms = sandbox.new("worker") {}             // an Aether worker needs nothing
    g, err = guest.launch(perms, "build/my-worker", null, "demo", "build/audit.log")
    if err != "" {
        println("not started: ${err}")           // macOS: refused, nothing ran
        return
    }
    guest.mount(&g, "GREET", 1, 1)               // a host function, 1 argument
    guest.mount(&g, "REQUEST", 1, 1)
    guest.start(&g, "...the script...")
    while true {
        e = guest.next(&g)                       // waits at most g.deadline_ms
        if e.kind == "SAY" {
            println(guest.printable(e.text))     // a guest chooses these bytes
        } else if e.kind == "CALL" && e.name == "GREET" {
            guest.answer(&g, guest.json_string("hello"))
        } else if e.kind == "CALL" {
            guest.deny(&g, "the file", "not in this guest's policy")
        } else if e.kind == "LOG" {
            println("${e.name} ${guest.printable(e.text)}")
        } else {
            println("${e.kind} ${guest.printable(e.text)}")   // DONE, ERROR, KILLED
            break
        }
    }
}
```

The third argument of `launch` is the worker's arguments, a `std.list` of
strings, or `null` for none: an Aether worker reads everything it needs
from the socket, while a foreign interpreter takes its script (`python3 -I
guest.py`, with `grant_runtime("python3")` and read access to the script).

`guest.next` returns an `Event`: `kind` and, by kind,

| `kind` | Fields | The host |
|---|---|---|
| `SAY` | `text` | shows it (through `printable`) |
| `LOG` | `code` the level 0..5, `name` `TRACE`..`FATAL`, `text`, `tags` (a JSON object) | nothing to answer |
| `CALL` | `name`, `text` the arguments as a JSON array | answers with `answer`, `fail`, `grant_file` or `deny` before calling `next` again |
| `DONE` | `code`, the script's exit code | the worker has been reaped |
| `ERROR` | `text`, the worker's message | the worker has been reaped |
| `KILLED` | `text`, why the host hung up | the worker has been killed |
| `READY` | `text`, the exported names as a JSON array (also `g.exported`) | the script has ended and the guest serves: `invoke` it, or `end` it |
| `RETURN` | `text`, the invoked function's JSON value | the guest serves again (`result(e)` gives `(value, "")`) |
| `RAISE` | `text`, the invoked function's error | the guest still serves (`result(e)` gives `("", error)`) |
| `IDLE` | | `next` was called on a serving guest: nothing was read, and no deadline ran |

`grant_file(&g, name, path)` answers a call with the file at `path` as an
open descriptor; the worker reads it and the script gets
`{"granted":true,"text":...}`. `deny(&g, name, why)` answers
`{"granted":false,"why":...}`. Both go to the audit log, with every spawn,
timeout, cancel and kill, and the `AUDIT` lines the worker sends (escaped:
`printable` turns control bytes, DEL and backslashes into `\xNN`, so a guest
cannot forge a log line or drive a terminal).

**Deadlines.** Each exchange, from what the host last sent or received to
the worker's next frame, is bounded by `g.deadline_ms` (5 s by default).
When it passes the host sends `CANCEL`; a worker that is looping or waiting
on a reply stops its script and answers `ERROR cancelled by host: deadline
of N ms passed`. One that does not answer within `g.grace_ms` (1 s) is hung
up on and killed (`KILLED no answer to CANCEL within N ms`). A frame over
`g.max_frame` (64 KiB), a malformed one or one that stalls part way is
refused the same way: `KILLED frame refused: ...`. `guest.cancel(&g, why)`
cancels at any time. `guest.close(&g)` hangs up and kills early, and is safe
after the end.

`guest.run_script(&g, source, handler)` writes that loop once: it starts the
script and hands every `SAY`, `LOG` and `CALL` to `handler`, a function of
`(*Guest, Event)` that answers the calls (one it leaves unanswered is
failed for it), and returns the event that ended the script: `READY`,
`DONE`, `ERROR` or `KILLED`.

## The host calls the guest

A script can leave functions behind for the host to call later: an event
handler, a callback for a transaction it started, a comparator. It ends
with `READY` and their names instead of `DONE`, and the guest *serves*: it
sits idle until the host invokes one of them by name or ends it. Host and
guest strictly alternate. The host invokes only an idle guest, one
invocation at a time; inside an invocation the guest may call the host's
mounted functions as a script does, and the host answers them as usual.
A script that exports nothing ends with `DONE`, exactly as before. The
design, the options that lost and the questions still open are in
[docs/design/guest-host-calls-guest.md](../../docs/design/guest-host-calls-guest.md).

The verb is a general call: an `INVOKE` carries whatever arguments the
function takes, of any JSON type and size up to the frame limit, and a
`RETURN` any value. The example **compiles but is not run** in CI: it needs
a worker program (`tests/integration/std_guest_invoke` runs exchanges like
it against the test worker). The script exports `on_tick` and `compare`;
the host calls them directly with their arguments.

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
    if e.kind != "READY" {                      // DONE: it exported nothing
        println("${e.kind} ${guest.printable(e.text)}")
        return
    }
    v, verr = guest.call_export(&g, "on_tick", "[1]", on_call)
    if verr != "" { println("on_tick raised: ${guest.printable(verr)}") } else { println("ticks: ${v}") }
    order, _ = guest.call_export(&g, "compare", "[\"pear\",\"apple\"]", on_call)
    println("compare: ${order}")
    guest.end(&g)                               // END; the guest answers DONE 0
    e = guest.next(&g)
}
```

Applications call across the divide in more than one way, and they mix in
one guest; the design note sets them side by side, each with an example:

- **A direct call** with its arguments, as above: the simplest, right for
  small values and pure functions (a spreadsheet's user-defined function,
  a comparator, an event handler given its event).
- **Notify, then fetch**, a suggested pattern borrowed from message-bus
  practice: the host invokes with a small id, and the export phones home
  for the details through a mounted host function (`txn_details`), which
  the host may refuse. Good when payloads are large, when the host wants to
  keep authority over what is read, or when a notification may be dropped
  or coalesced; it costs a second round trip.
- **The pull pattern**, with no protocol at all: the guest's script loops
  on a mounted `next_event` whose answer says what to run. It suits one
  application with its own event shape, or a worker that has not learnt to
  export.

The same with the loop written out: `guest.invoke(&g, name, args_json)`
sends `INVOKE`; then `guest.next(&g)` returns the invocation's `SAY`,
`LOG` and `CALL` events (answer each) and ends with `RETURN` or `RAISE`,
which `guest.result(e)` turns into Aether's `(value, err)`.

| Host | |
|---|---|
| `invoke(&g, name, args)` | `""`, or why nothing was sent: the guest is not serving (its script, an invocation or a host call it waits on is still running, or it is ending or over), it did not export `name`, `args` is not a JSON array |
| `call_export(&g, name, args, handler)` | `invoke`, then the events to `handler` until `RETURN` (`(value, "")`) or `RAISE`, a refused invoke, or the guest's end (`("", why)`) |
| `end(&g)` | `END`; `next` then returns `DONE 0` and the worker is reaped |
| `result(e)` | `(value, err)` from the event that ended an invocation |
| `is_exported(&g, name)`, `g.exported` | what `READY` announced, a JSON array written `["a","b"]` |
| `is_serving(&g)`, `g.phase` | `SCRIPT`, `SERVING`, `INVOKING`, `ENDING` or `CANCELLED` |

What holds a serving guest:

- `READY` is checked: a JSON array of 1 to 256 (`MAX_EXPORTS`) distinct
  names of letters, digits, `_` and `.`, at most 128 bytes each
  (`MAX_EXPORT_NAME`). Any other list hangs up: `KILLED protocol: READY
  ...`.
- Inside an invocation every exchange has the script's deadline,
  `deadline_ms`, then `CANCEL`, then `grace_ms`, then the kill; `max_frame`
  bounds `RETURN` and `RAISE` as every frame. A cancel ends the guest, not
  only the invocation.
- An idle guest has no deadline. `next` on it returns `IDLE` without
  waiting, and a frame it sends while idle (found by `next`, `invoke` or
  `end`) is unsolicited: the host hangs up. A `RETURN` or `RAISE` with no
  `INVOKE`, a second `READY`, and anything but `DONE` after `END` hang up
  too. A guest never sends `INVOKE`.
- `invoke` refuses an `INVOKE` frame longer than the 64 KiB a worker
  reads, saying why, and sends nothing. A worker refuses a `RETURN` longer
  than the host reads (`child.returned`), so it can `RAISE` and serve on;
  one that sends it anyway is still killed.
- `invoke` and `end` are refused, and send nothing, unless the guest
  serves: so a host function's handler cannot invoke the guest that is
  calling it.
- An invoked function reaches only what the host mounted: being callable
  gives the guest nothing new.

## The worker

The example **compiles but is not run**: it needs a host that opened the
channel. Its engine is the smallest there is: it says the script back.

```aether
import std.guest.child

main() {
    h, err = child.open("echo-worker")
    if err != "" {
        println(err)                             // not started by a host
        exit(2)
    }
    if child.read_script(h) != "" { exit(1) }    // the host has been told why
    // Engine setup from what the host sent: h.source, the mounts
    // (mount_count, mount_at), h.log_level.
    child.run(h) callback {
        // Locked down: no grants from here on.
        if child.poll(h) != "" {                 // the host cancelled
            child.error(h, "cancelled by host: ${h.cancel}")
            return
        }
        child.say(h, h.source)
        if child.is_mounted(h, "GREET") {
            r = child.call_host(h, "GREET", "[\"echo\"]")
            if r.kind == "CANCEL" {
                child.error(h, "cancelled by host: ${r.text}")
                return
            }
            child.say(h, r.text)                 // VALUE: JSON; FAIL: the message
        }
        child.done(h, 0)
    }
    child.free(h)                                // the Host and what it holds
    exit(0)
}
```

The engine supplies one callback, the block given to `child.run`, which
runs `h.source`. Everything else is a call the engine makes:

| The engine | `std.guest.child` |
|---|---|
| prints | `say(h, text)` |
| logs | `log(h, level, tags, text)`: sent only at or above the host's threshold `h.log_level` (off by default) |
| calls a mounted function | `call_host(h, name, args_json)` returns a `Reply`: `VALUE` (`text` is the JSON value), `FAIL` (`text` the message) or `CANCEL` (`text` the reason) |
| notes something for the host's audit log | `audit(h, line)` |
| ends | `done(h, code)` or `error(h, why)` |
| looks for a cancel | `poll(h)`, without waiting: `""`, or the reason, also in `h.cancel` |
| sets up | `h.source`, `mount_count(h)`, `mount_at(h, i)` (name and arity), `is_mounted(h, name)`, `h.probe`, `h.log_level` |
| ends a script that left functions behind | `ready(h, names_json)` instead of `done`, then serves |
| serves | `serve(h)` waits, with no deadline, and returns an `Invocation`: `INVOKE` (`name`, `args` a JSON array, also `h.invoked`), `END` (answer `done(h, 0)`) or `CANCEL` (`args` the reason: answer `error`) |
| answers an invocation | `returned(h, json)` or `raised(h, why)`; the guest serves on after either. `returned` gives back `""`, or, for a value whose frame is longer than the host reads (`h.max_frame`: 64 KiB, or the host's `MAXFRAME`), why it sent nothing, so the worker can `raised` that instead of being killed; `raised` cuts a message to fit |
| is done with the Host | `free(h)`: the Host, its mounts and its strings; the channel stays open |

Two kinds of engine meet the cancel two ways. One that runs a script in
steps calls `poll` between them. One that runs it to the end in a single
call has an interrupt hook (MicroQuickJS's `JS_SetInterruptHandler`,
quickjs-ng's, Lua's `lua_sethook`) and calls `poll` from there. A script
waiting in a mounted function gets the cancel as its reply. Either way the
engine stops the script and answers `error(h, "cancelled by host: " +
h.cancel)`.

`say`, `log`, `call_host`, `done` and `error` first send what the worker's
own sandbox refused since the last report, as `AUDIT <name> sandbox refused
<what> <resource>`, so a refusal reaches the host's audit log before what
the script did next. With `PROBE`, `run` also tries `fs.read("/etc/passwd")`
and `tcp.connect("1.1.1.1", 443)` from inside the lockdown and reports both;
an engine can add probes of its own in its block when `h.probe` is 1.

A worker serves after its script like this; inside an invocation it calls
the host and polls exactly as inside a script:

```aether,fragment
child.ready(h, "[\"on_tick\",\"compare\"]")
while true {
    inv = child.serve(h)
    if inv.kind == "END" {
        child.done(h, 0)
        break
    }
    if inv.kind == "CANCEL" {
        child.error(h, "cancelled by host: ${inv.args}")
        break
    }
    // INVOKE: run inv.name with inv.args in the engine, which may call
    // the host (child.call_host) and poll on the way. Here, an echo.
    child.returned(h, inv.args)
}
```

`serve` answers an `INVOKE` of a name `ready` did not announce with
`RAISE` itself. A function that raises leaves the guest serving; a cancel,
heard by `poll` or in place of a reply inside an invocation, ends it with
`error`, as in a script.

The host is the worker's only way out, so when the channel breaks (the host
has gone, or sent a frame the worker cannot read) nobody is left to report
to, and the worker process ends with exit status 1.

## The protocol

`std.ipc` frames: a 4-byte big-endian length, then that many bytes, at most
64 KiB a frame (`MAX_FRAME`). Each frame is a verb, a space, and its text;
arguments and values are JSON. A host or worker in another language
speaks it with a socket and nothing else.

| Host to worker | When | |
|---|---|---|
| `MOUNT <name> <min> <max>` | before `SCRIPT` | a host function taking `min..max` arguments; `0 <= min <= max`. Whether the name is one the engine can mount is the engine's to say (`ERROR`) |
| `PROBE` | before `SCRIPT` | run the self-check once locked down |
| `LOGLEVEL <n>` | any time | log threshold: `-1` off (the default), `0` trace .. `5` fatal. While the script runs it holds from the next step; while a `CALL` waits, from the answer |
| `MAXFRAME <n>` | before `SCRIPT`, only when the host's `max_frame` is below 64 KiB | the longest frame the host reads, 64..65536: the worker holds its `RETURN` and `RAISE` to it. At the default it is not sent, so the wire is as before; a worker that does not know the verb refuses it as any unknown verb before the script |
| `SCRIPT <source>` | last before the script | the rest of the frame is the script, newlines and all: lock down and run it |
| `REPLY <json>` | answers a `CALL` | the function's value |
| `FAIL <message>` | answers a `CALL` | the function failed (`host function failed` when empty) |
| `FILE` | answers a `CALL` | then a descriptor on the next byte (`ipc.send_fd`); the value is `{"granted":true,"text":...}`, at most 64 KiB (`MAX_FILE`) |
| `CANCEL <reason>` | any time after `SCRIPT` | stop the script, the invocation, or the serving guest; answered with `ERROR cancelled by host: <reason>`, and the worker exits |
| `INVOKE <name> <json array>` | while the guest serves: after `READY`, and after the `RETURN` or `RAISE` of the last invocation | call the exported function `name`; answered with `RETURN` or `RAISE`, with `SAY`, `LOG`, `CALL` and `AUDIT` on the way |
| `END` | while the guest serves | the host is finished; answered with `DONE 0`, and the worker exits |

| Worker to host | | |
|---|---|---|
| `SAY <text>` | | the script printed it |
| `LOG <level> <n> <tags> <message>` | no answer | a log message at or above the threshold; `<tags>` is `n` bytes of JSON object |
| `CALL <name> <json array>` | the host answers | a call to a mounted function |
| `AUDIT <text>` | no answer | for the host's audit log: the lockdown, the probes, what the worker's sandbox refused |
| `DONE <code>` | then it exits | the script ended |
| `ERROR <message>` | then it exits | the script failed or was cancelled, or the host broke the protocol before the script |
| `READY <json array>` | instead of `DONE`, then it serves | the script ended and left these functions: 1 to 256 distinct names of letters, digits, `_` and `.`, at most 128 bytes each |
| `RETURN <json>` | answers `INVOKE` | the function's value |
| `RAISE <message>` | answers `INVOKE` | the function failed (`guest function failed` when empty); the guest still serves |

Before `SCRIPT`, a verb other than the four above, a `MOUNT` that is not
`name min max` or a `LOGLEVEL` out of range is refused with `ERROR
protocol: ...`. While the script runs, any frame other than `CANCEL` and
`LOGLEVEL` (or an answer to a pending `CALL`) breaks the protocol and
cancels the script with `protocol: unexpected '<verb>' ...` as the reason.
While the guest serves, any frame other than `INVOKE`, `END`, `CANCEL` and
`LOGLEVEL` does the same. On the host's side, a frame from an idle guest,
a `RETURN` or `RAISE` with no `INVOKE`, a second `READY`, a `READY` list
out of bounds, anything after `END` but `AUDIT`, `DONE` and `ERROR`, and
an `INVOKE` from the worker are protocol errors: the host hangs up and
kills it.

The host refuses descriptors sent up by a worker (`std.ipc`'s direction
rule): a file only ever travels down.

Text lines with JSON arguments keep the protocol readable in a log and
trivial to speak from any language. A binary encoding (CBOR, through
`std.cbor`) is a possible later option for arguments that are large or
not text; nothing needs it yet.
