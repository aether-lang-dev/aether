# REPLY: contrib.quickjs: async global eval, a host-controlled interrupt, promise state

**Re:** nib-code `asks/aether-quickjs-async-eval-and-interrupt.md` (branch `aether-port`)

**RESOLVED**, pending release: branch `contrib-quickjs-async-interrupt`
(the first release after 0.915.0 that carries it). The stand-in
`ae/nib/qjsgap/` can be deleted on that release.

## What landed

1. **Async global eval.** `quickjs.eval_async(q, code, filename) -> (int, string)`:
   `JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_ASYNC`, flag bit 4 in `qjs_eval`. The
   handle is the promise, which settles to `{ value: <completion> }` or is
   rejected with what the script threw (a throw before the first `await`
   too). `(0, error)` for a script that does not parse, or that is
   interrupted before its first `await`.
2. **A host-controlled interrupt, per runtime**, with no process-wide state:
   - `quickjs.set_deadline_ns(q, ns)`, `quickjs.clear_deadline(q)`,
     `quickjs.deadline_ns(q)`: an absolute deadline on `quickjs.now_ns()`'s
     monotonic clock (CLOCK_MONOTONIC, QueryPerformanceCounter on Windows:
     the clock `os.now_monotonic_ns()` reads), which no entry resets.
   - `quickjs.set_cancel_cell(q, cell)`, `quickjs.set_stop_cell(q, cell)`:
     `std.sync` cells the host owns, read with an acquire load, writable
     from any thread. `null` unsets. Free them after `dispose` or after
     unsetting.
   - `quickjs.interrupted(q) -> int`: `INTERRUPTED_DEADLINE` (1),
     `INTERRUPTED_CANCEL` (2), `INTERRUPTED_STOP` (3), the first since the
     runtime was made or `quickjs.clear_interrupted(q)`. The stand-in's
     numbering and precedence (cancel, then deadline, then stop) are kept.
   - Every entry honours them, `to_string` included, and also `to_int`,
     `to_float`, `to_bool`, `length`, `keys` (which call a `valueOf`, a
     getter or a Proxy trap) and `call` while it reads its arguments.
   - `set_time_limit` is removed (no backward compatibility): the deadline
     subsumes it. `set_time_limit(q, ms)` before an entry is
     `set_deadline_ns(q, now_ns() + ms * 1000000)`.
   - In an async eval an interrupt is not a rejection: before the first
     `await` the eval returns it, after it `run_jobs` does, and the promise
     stays pending.
3. **Promise state.** `quickjs.promise_state(q, h) -> int`
   (`PROMISE_NOT_A_PROMISE` -1, `PROMISE_PENDING` 0, `PROMISE_FULFILLED` 1,
   `PROMISE_REJECTED` 2) and `quickjs.promise_result(q, h) -> (int, string)`,
   a handle to the value or the rejection reason, `(0, error)` for a pending
   promise or a non-promise. Neither runs JavaScript.

Also noticed, both done:

- `quickjs.to_string(q, h) -> (string, string)`: `("", error)` when the
  conversion threw or was interrupted, so `""` is an empty string. NUL bytes
  are kept. `arg_string` still returns a plain string.
- quickjs-ng is 0.18.0. One change reaches scripts: an `InternalError`
  (out of memory, interrupted, a host function that failed without
  throwing) can no longer be caught by JavaScript. `quickjs.rethrow` with no
  exception value kept throws the error its text names (a host
  `TypeError: not a function` is a catchable TypeError).

## The stand-in, function by function

| `ae/nib/qjsgap` | contrib.quickjs |
|---|---|
| `watch(q, cancel)` | `quickjs.set_cancel_cell(q, cancel)`. There is no watch object: every runtime has the handler. |
| `unwatch(w)` | `quickjs.set_cancel_cell(q, null)` and `quickjs.set_stop_cell(q, null)` if the cells are freed before `dispose`; nothing else to free. |
| `set_stop(w, cell)` | `quickjs.set_stop_cell(q, cell)` |
| `begin(w, remaining_ns)` | `quickjs.set_deadline_ns(q, quickjs.now_ns() + remaining_ns)` (`remaining_ns <= 0` is a deadline already passed, as in the stand-in). The stack top is updated on every entry already. |
| `end(w)` | `quickjs.clear_deadline(q)` |
| `reason(w)` | `quickjs.interrupted(q)`, same numbers |
| `now_ns()` | `quickjs.now_ns()` (or `os.now_monotonic_ns()`, the same clock) |
| `eval_async(w, code, file, "__nib_run", "__nib_exc")` | `p, err = quickjs.eval_async(q, code, file)`: the promise is `p`, no global. On failure `err` is the exception's text with its stack, the shape `describe_error_` builds (`msg\nstack`; trim the trailing newline). |
| `promise_state(w, h)` | `quickjs.promise_state(q, h)` |
| `promise_result_to_global(w, h, name)` | `r, err = quickjs.promise_result(q, h)`, a handle to release |

Callers of `quickjs.to_string` change to `s, err = quickjs.to_string(q, h)`
(nib's `to_str_` in `ae/nib/sandbox/module.ae`; servirtium-vcr's
`core/bru.ae` too).

Tests in `contrib/quickjs/test_quickjs.ae`: the ask's repro gives 42; a
busy loop stopped by a cancel cell set from an actor's thread, measured
within half a second of the store; ten 100 ms entries against a 350 ms
deadline stopped from the 3rd to 5th entry on (0 of 10 with the old
per-entry limit); a looping `toString`, `valueOf` and getter stopped; a
stop cell reported as STOP and a cancel over it; the first reason kept;
rejected and pending promise state and result; `to_string`'s empty against
failed.
