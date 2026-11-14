# contrib.quickjs: QuickJS, embedded

Modern JavaScript (ES2023: classes with private fields, async/await and
promises, `Map`/`Set`, `?.`, `??`, spread, modules) run inside an Aether
program, by [quickjs-ng](https://github.com/quickjs-ng/quickjs), the
maintained fork of Fabrice Bellard's QuickJS. The engine compiles into the
program from source, natively or with `ae build --target=<triple>`.

```aether
import contrib.quickjs

main() {
    q = quickjs.runtime(8192, 0)            // 8 MB heap cap, default stack
    quickjs.set_deadline_ns(q, quickjs.now_ns() + 100000000)   // stop JS 100 ms from now

    add = quickjs.function(q, "add", 2, | q: ptr, this_h: int, args: int | {
        return quickjs.new_int(q, quickjs.arg_int(q, args, 0) + quickjs.arg_int(q, args, 1))
    })
    quickjs.set_global(q, "add", add)
    quickjs.release(q, add)

    v, err = quickjs.eval(q, "add(2, 3) * 10", "demo.js")
    if err != "" { println(err); return }
    println(quickjs.to_int(q, v))           // 50
    quickjs.release(q, v)
    quickjs.dispose(q)
}
```

## The model

- **A runtime** (`quickjs.runtime(memory_kb, stack_kb)`) is one QuickJS
  runtime with one context, used from one thread at a time.
  `quickjs.dispose` frees it and everything it still holds.
- **Values are handles** (ints). Every function that returns one gives the
  caller a reference to `quickjs.release`, or to return from a host
  function, which hands it to the engine. `quickjs.handles(q)` is 0 when
  everything taken has been released, which makes leaks a one-line test.
  A `JSValue` never crosses into Aether: on 64-bit targets it is a 16-byte
  struct, returned in registers on some ABIs and through memory on others.
- **Host functions** are Aether closures,
  `|q: ptr, this_h: int, args: int|`, returning a handle, 0 for undefined,
  or `quickjs.throw_error(q, kind, message)`. `args` is an array of the
  arguments; `arg_int`, `arg_float`, `arg_string`, `arg_bool` read it
  without leaving handles behind. One C dispatcher serves them all, so
  there is no C to write per function.
- **Errors** come back as `(value, error)`: the error is the exception's
  text with its stack, e.g. `TypeError: cannot read property 'x' of null`.

## Bytes

A `Uint8Array` (or `Uint8ClampedArray`) is how a script hands the host
pixels or a file, and how the host hands one back, without a string in
between (`arg_string` stringifies a typed array as `"1,2,3"`) or a handle
per element.

- `quickjs.bytes_of(q, h) -> (ptr, int)`: the array's bytes, its own window
  of its buffer (`a.subarray(4)` starts at `a[4]`), as a pointer into the
  engine's memory and a count; `(null, -1)` when `h` is not one, or its
  buffer is detached. No copy: read or copy the bytes before JavaScript runs
  again (a script can resize or detach the buffer), and never free them.
- `quickjs.arg_bytes(q, args, i) -> (ptr, int)`: the same for a host
  function's argument `i`, valid while the host function runs.
- `quickjs.new_uint8array(q, data, n) -> int`: a new `Uint8Array` holding a
  copy of the `n` bytes at `data` (a raw pointer, `bytes.data(b)` for a
  `std.bytes` buffer), so the host's buffer can be freed at once.

```aether
sum = quickjs.function(q, "sum", 1, | q: ptr, this_h: int, args: int | {
    p, n = quickjs.arg_bytes(q, args, 0)
    if n < 0 { return quickjs.throw_error(q, "TypeError", "sum: a Uint8Array") }
    t = 0
    i = 0
    while i < n {
        t = t + mem.get_byte(p, i)
        i = i + 1
    }
    return quickjs.new_int(q, t)
})
```

## Limits

- The interrupt, below: a deadline and cancel/stop cells that stop running
  JavaScript with `InternalError: interrupted`; the runtime stays usable.
- `quickjs.runtime(memory_kb, ...)`: allocation past the cap fails with
  `InternalError: out of memory`.
- `stack_kb` caps the JavaScript stack (`RangeError: Maximum call stack
  size exceeded`).

## Interrupting JavaScript

When JavaScript stops is the host's decision, per runtime, and nothing
resets it behind the host's back:

- `quickjs.set_deadline_ns(q, ns)`: JavaScript running at or after `ns`, an
  absolute time on `quickjs.now_ns()`'s monotonic clock (the same clock as
  `os.now_monotonic_ns()`, `QueryPerformanceCounter` on Windows), is stopped,
  in this entry and every later one, until the host moves it or calls
  `quickjs.clear_deadline(q)`. `quickjs.deadline_ns(q)` reads it (0: none).
- `quickjs.set_cancel_cell(q, cell)`: a `std.sync` cell
  (`sync.atomic_new(0)`) whose nonzero value stops JavaScript. Any thread may
  set it: an Esc key, a SIGINT watch, a watchdog.
- `quickjs.set_stop_cell(q, cell)`: a second cell, reported apart from a
  cancel, for a stop that is not the user's (a replay that diverged from
  its recording, whose outcome is what the script threw, not "canceled").
- `quickjs.interrupted(q) -> int`: why JavaScript was first stopped since
  the runtime was made or `quickjs.clear_interrupted(q)`:
  `quickjs.INTERRUPTED_NONE` (0), `INTERRUPTED_DEADLINE` (1),
  `INTERRUPTED_CANCEL` (2), `INTERRUPTED_STOP` (3). When several hold at
  once, a cancel is reported over a deadline and both over a stop. The
  reason stays while the host reads errors and values afterwards.

Every entry honours them: an eval, a call, `run_jobs`, and the conversions
that can run JavaScript (`to_string` running a `toString`, `to_int` a
`valueOf`, `get` a getter, `length`, `keys`). The interrupt is uncatchable
by the script. In an `eval_async` script it is not a rejection: before the
first `await` the eval returns the error, after it `run_jobs` does, and the
promise stays pending. QuickJS polls every few thousand operations, so a
busy loop stops within microseconds of a cell being set; one long native
call (a huge regexp, say) is not interrupted midway.

A budget of JavaScript time across many entries, which pauses while the
host waits on other work, is the host setting the deadline when JS starts
and taking the time spent off what is left when it stops:

```aether
quickjs.set_deadline_ns(q, quickjs.now_ns() + remaining)
started = quickjs.now_ns()
_n, err = quickjs.run_jobs(q)
quickjs.clear_deadline(q)
remaining = remaining - (quickjs.now_ns() - started)
if quickjs.interrupted(q) == quickjs.INTERRUPTED_CANCEL { println("canceled") }
```

Threads and ownership: the cells are read with an acquire load and may be
written from any thread. Everything else, the deadline included, belongs to
the thread running the runtime. The cells stay the host's: free them
(`sync.atomic_free`) after `quickjs.dispose`, or after unsetting them with
`null`. There is no process-wide state: every runtime has its own deadline,
cells and reason.

## Promises and actors

A script that `await`s runs until it waits, and carries on when the promise
it waits on settles and the job queue runs. That fits work done elsewhere,
on an actor or a `std.worker`:

```aether
// In a host function: hand the script a promise, keep its settlers.
pending = quickjs.new_promise(q)            // { promise, resolve, reject }
p, _e = quickjs.get(q, pending, "promise")
return p

// Later, when the answer arrives (on the runtime's thread):
resolve, _e = quickjs.get(q, pending, "resolve")
_r, _e2 = quickjs.call(q, resolve, 0, args_array)
_n, _e3 = quickjs.run_jobs(q)               // the script's await continues
```

### A script that awaits, and its value

`quickjs.eval` runs a script and gives its completion value, but a
top-level `await` is a SyntaxError there; `quickjs.eval_module` allows the
`await` but a module has no completion value. `quickjs.eval_async(q, code,
filename) -> (int, string)` does both (`JS_EVAL_FLAG_ASYNC`): it returns a
promise that settles, as `run_jobs` runs what the awaits queued, to
`{ value: <the completion value> }`, or is rejected with what the script
threw (before its first `await` too). Only a script that does not parse is
an error rather than a promise.

```aether
p, err = quickjs.eval_async(q, "const x = await Promise.resolve(2); x * 21", "run.js")
_n, _e = quickjs.run_jobs(q)
if quickjs.promise_state(q, p) == quickjs.PROMISE_FULFILLED {
    r, _e2 = quickjs.promise_result(q, p)       // { value: 42 }
    v, _e3 = quickjs.get(q, r, "value")
    println(quickjs.to_int(q, v))               // 42
    quickjs.release(q, v)
    quickjs.release(q, r)
}
quickjs.release(q, p)
```

A host that holds a promise (one a script returned, or `eval_async`'s) can
ask how it stands without running any JavaScript:

- `quickjs.promise_state(q, h) -> int`: `quickjs.PROMISE_PENDING` (0),
  `PROMISE_FULFILLED` (1), `PROMISE_REJECTED` (2), or
  `PROMISE_NOT_A_PROMISE` (-1) for anything else, a thenable included.
- `quickjs.promise_result(q, h) -> (int, string)`: what a settled promise
  holds, its value or the reason it was rejected with, as a handle to
  release; `(0, error)` for a pending promise or something that is not one.

Callbacks work too: hold a JavaScript function's handle and
`quickjs.call` it when the answer comes. JavaScript runs on one thread
either way; actors are where the concurrency is.

## What a script can reach

Only what the host gives it. quickjs-ng's `quickjs-libc` (the `std` and
`os` modules: files, processes, sockets) is in the amalgamation behind
`QJS_BUILD_LIBC`, which this module never defines, and there is no
`require` or Node API. File, network and process access exist only as
host functions you write. A script that loops forever or grows without
bound is stopped by the interrupt and the limits above.

## Build

Nothing to configure: `module.ae`'s `@source("aether_quickjs.c")`
compiles the engine into the program (about 7 s the first time per
target), natively or for `ae build --target=<triple>`.

The engine is quickjs-ng's own single-file amalgamation (a release asset),
pinned in `amalgamation.lock` (version **0.17.0**) and fetched by
`scripts/fetch-quickjs-amalgamation.sh` into `contrib/quickjs/amalgamation/`
(gitignored), the way `contrib.sqlite` pins SQLite's. Release archives ship
it, so an installed toolchain needs no network. In a source checkout, run
the script once. To move to a new quickjs-ng release, change the lock's
three values, run the script and `contrib/quickjs/test_quickjs.ae`, and
update the version here.

## Tested on

macOS arm64, Linux x86_64 and arm64 (glibc), FreeBSD 15 amd64, and Windows
x86_64 (cross-built with zig, run under Wine); leak-clean under valgrind.

## License

quickjs-ng is MIT licensed (Copyright Fabrice Bellard, Charlie Gordon, and
the quickjs-ng contributors); the amalgamation carries its notice.
