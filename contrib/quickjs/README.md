# contrib.quickjs — QuickJS, embedded

Modern JavaScript (ES2023: classes with private fields, async/await and
promises, `Map`/`Set`, `?.`, `??`, spread, modules) run inside an Aether
program, by [quickjs-ng](https://github.com/quickjs-ng/quickjs), the
maintained fork of Fabrice Bellard's QuickJS. The engine compiles into the
program from source, natively or with `ae build --target=<triple>`.

```aether
import contrib.quickjs

main() {
    q = quickjs.runtime(8192, 0)            // 8 MB heap cap, default stack
    quickjs.set_time_limit(q, 100)          // each entry: at most 100 ms

    add = quickjs.function(q, "add", 2, |q: ptr, this_h: int, args: int| {
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

## Limits

- `quickjs.set_time_limit(q, ms)`: each entry into JavaScript (an eval, a
  call, a run of the job queue) is stopped after `ms` with
  `InternalError: interrupted`; the runtime stays usable.
- `quickjs.runtime(memory_kb, ...)`: allocation past the cap fails with
  `InternalError: out of memory`.
- `stack_kb` caps the JavaScript stack (`RangeError: Maximum call stack
  size exceeded`).

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

Callbacks work too: hold a JavaScript function's handle and
`quickjs.call` it when the answer comes. JavaScript runs on one thread
either way; actors are where the concurrency is.

## What a script can reach

Only what the host gives it. quickjs-ng's `quickjs-libc` (the `std` and
`os` modules: files, processes, sockets) is in the amalgamation behind
`QJS_BUILD_LIBC`, which this module never defines, and there is no
`require` or Node API. File, network and process access exist only as
host functions you write. A script that loops forever or grows without
bound is stopped by the limits above.

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
