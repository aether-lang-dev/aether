# REPLY: contrib.quickjs: an operation budget, and the exception as a handle

**Re:** aether-guest-languages `asks/contrib-quickjs-op-budget-and-exception-handle.md`

**RESOLVED** on the branch `batch/fix-runtime` (the release that carries it).

## 1. The budget

```aether
set_poll_budget(q: ptr, polls: long)   // 0: spent, negative: none
clear_poll_budget(q: ptr)
poll_budget(q: ptr) -> long            // left: 0 spent, -1 none
const INTERRUPTED_BUDGET = 4
```

**The unit is the engine's interrupt poll**, and how exact it is follows from
quickjs-ng 0.18.0's source: `js_poll_interrupts` decrements a per-context
counter and calls the handler when it reaches zero, resetting it to
`JS_INTERRUPT_COUNTER_INIT` (10,000). The counter is decremented at every
JavaScript function call (`JS_CallInternal`'s entry), every jump the
interpreter takes or tests (`goto`, `if_true`, `if_false`, so each loop
iteration and each `if`), every element step of the built-ins that iterate
(Array.prototype's find, map, sort and the rest) and every step of a proxy
chain; the regexp engine has its own counter of 10,000 backtracking steps and
calls the same handler. Each call of the handler spends one poll; the poll
that leaves none stops JavaScript. So a budget of n stops JavaScript after
between (n - 1) * 10000 and n * 10000 counted steps from when it is set
(where in its 10,000 the counter stands is not visible), and in a fresh
runtime, which polls at its first step, after exactly (n - 1) * 10000:
`for (;;) i++` (one jump per iteration) stops at `i == 100000` under a budget
of 11 and at 200000 under 21, on every run. Straight-line code with no call
and no jump is not counted (it is finite); a native call that does not
iterate in the engine is one step.

It is held on the runtime (`budget` in `AeQjs`, no process-wide state), spent
across entries like the deadline, never reset by an entry, and reported below
a cancel and a deadline and above a stop: cancel, deadline, budget, stop. For
a bridge's `vm_set_instruction_limit(vm, budget, every)`: `ceil(budget /
10000)` polls, with `every` documented as 10,000 counted steps.

## 2. The exception as a handle

```aether
last_exception(q: ptr) -> int
```

The value the last failure threw (an eval, eval_async, eval_module, call,
run_jobs, parse_json, or a conversion that ran JavaScript), as a new handle.
**Ownership:** the caller's, to `release` (or to return from a host function,
which hands it to the engine; `dispose` frees it otherwise). A later failure
does not change a handle already taken, only what the next call returns. 0
when the failure had no value behind it: the host's own refusal (`TypeError:
not a function`, a filename holding a NUL) or a heap too full to make the
error. It runs no JavaScript. In C it is `put_(q, JS_DupValue(ctx, q->exc))`,
as you proposed.

`last_error` keeps its text for showing a person, built from that value as
before (String(e), a newline, e.stack), now by its length (see
REPLY-contrib-quickjs-length-aware-strings.md). It is documented as not
splittable, since a message may hold a newline; the handle is how a message
with one is kept whole: `get(q, e, "message")` is `"a\n    at b"` for your
repro, and `get(q, e, "stack")` is the stack alone.

## Tests (the new API did not exist: the test file failed to compile on the old module, `E0303: 'set_poll_budget' is not exported`)

`contrib/quickjs/test_quickjs.ae`, "a poll budget" (4 cases: stops a
runaway and stays spent across entries; exact and repeatable steps; a runaway
regexp; spent across entries, a deadline reported over it) and "the
exception as a handle" (4 cases: message, name and stack apart for your
repro; a thrown 42 and a message with a NUL; 0 for a host refusal; a handle
unchanged by a later failure). All pass; the runtime holds no handles at the
end.

One correction found on the way: the README and module said a single long
regexp is not interrupted midway. It is: the regexp engine polls the same
handler, and a backtracking `/(a+)+b/` on 40 `a`s is stopped by a deadline
and by a budget (a test now says so).

## For the bridge (quickjs/aether_host_quickjs.c), after the release

- `vm_set_instruction_limit` can be real: `qjs_set_poll_budget(q, (budget +
  9999) / 10000)`, reason code 4 beside `QJS_INTERRUPT_DEADLINE` (line 96);
  `set_error` (line 449) maps 4 to "InternalError: interrupted: instruction
  limit exceeded" or the words the other bridges use.
- `caught_text` (line 426) and the catcher (line 218) become
  `int e = qjs_last_exception(q)` and `value_str(q, e, &n)` (String(e)), or
  `qjs_get(q, e, "message")`; `message_of`'s cut at `"\n    at "` (line 408)
  goes. Release `e`. The thrown object's own `toString` no longer runs twice.
