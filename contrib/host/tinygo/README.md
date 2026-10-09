# contrib.host.tinygo — In-Process Go via c-shared

Run Go code inside the Aether process — no subprocess, no IPC, no
marshalling. Just `dlopen` the `.so` / `.dll` that
`go build -buildmode=c-shared` produces and call its exported
functions directly.

> **Toolchain note.** The bridge is named `tinygo` (and the
> `--with=tinygo` capability layer ships TinyGo) for the
> small-footprint, embedded-friendly story, but
> `tinygo build -buildmode=c-shared` currently only supports
> `wasm` targets ("buildmode c-shared is only supported on wasm
> at the moment"). For **native** linux/darwin/windows c-shared
> libraries — what this bridge dlopens — use standard
> `go build -buildmode=c-shared`. The bridge dlopens by symbol;
> it doesn't care which compiler produced the `.so`. The
> `--with=tinygo` image ships both `go` and `tinygo` so either
> path is one command away.

This is the in-process counterpart of [`contrib/host/go`](../go/),
which spawns the standard `go` toolchain as a sandboxed
subprocess. Pick the host that matches your containment story:

| | `contrib/host/go` | `contrib/host/tinygo` |
|---|---|---|
| Toolchain | Standard `go` | `tinygo` |
| Process model | Subprocess (LD_PRELOAD sandbox) | In-process (`dlopen`) |
| Latency per call | Process spawn (~ms) | Direct function call (~ns) |
| Memory model | Separate heap | Shared heap (TinyGo + Aether) |
| Goroutines | Full Go runtime | TinyGo's reduced runtime |
| Sandbox enforcement | Per-call (libc grants) | Per-process (one-time grant) |

## Prerequisites

You need a Go toolchain that can build `-buildmode=c-shared`.
On native linux/darwin/windows that's standard `go` (≥1.21);
TinyGo is fine for the wasm target only.

```bash
# macOS / Linux / Windows — standard Go
# https://go.dev/dl/

# Verify
go version
```

## Build the Go side

Mark each function you want to call from Aether with a `//export`
comment. The function name on the Aether side matches the
exported C symbol exactly (case-sensitive, no name mangling).

```go
// greet.go
package main

import "C"  // required by -buildmode=c-shared

//export Answer
func Answer() int32 { return 42 }

//export Add
func Add(a, b int32) int32 { return a + b }

//export Greet
func Greet(name *C.char) *C.char {
    msg := "hello, " + C.GoString(name)
    return C.CString(msg)  // malloc'd by cgo: call it with call_str_str_owned
}

func main() {}  // c-shared still requires a main() — empty body is fine
```

Build (native — use standard `go`, not `tinygo`; see toolchain
note at the top):

```bash
CGO_ENABLED=1 go build -buildmode=c-shared -o libgreet.so    greet.go  # Linux
CGO_ENABLED=1 go build -buildmode=c-shared -o libgreet.dylib greet.go  # macOS
CGO_ENABLED=1 go build -buildmode=c-shared -o libgreet.dll   greet.go  # Windows
```

For the wasm target — and only then — substitute
`tinygo build -buildmode=c-shared -target=wasm …`.

## Call from Aether

```aether
import contrib.host.tinygo

main() {
    handle, err = tinygo.load("./libgreet.so")
    if handle == null {
        println("load failed: ${err}")
        return
    }

    answer = tinygo.call_int_void(handle, "Answer")
    println("Answer = ${answer}")          // -> Answer = 42

    total = tinygo.call_int_int_int(handle, "Add", 2, 40)
    println("Add(2, 40) = ${total}")       // -> Add(2, 40) = 42

    // Greet returns C.CString(...): the _owned call copies it and frees
    // the C pointer, and msg is freed like any other Aether string.
    msg = tinygo.call_str_str_owned(handle, "Greet", "world")
    println(msg)                           // -> hello, world

    tinygo.unload(handle)
}
```

## Calling-convention surface

The original wrappers, named for their shapes:

| Aether call | Matches TinyGo c-shared signature |
|---|---|
| `tinygo.call_int_void(h, "F")` | `int F(void)` |
| `tinygo.call_int_int(h, "F", a)` | `int F(int)` |
| `tinygo.call_int_int_int(h, "F", a, b)` | `int F(int, int)` |
| `tinygo.call_void_int(h, "F", a)` | `void F(int)` |
| `tinygo.call_str_str(h, "F", s)` | `const char* F(const char*)` |

Beside them, `call_<ret>_<args>` covers every combination in
[`module.ae`](module.ae)'s export list, up to three arguments, with
one letter per type: `v` void, `i` int32, `l` int64, `d` double,
`s` string, `p` ptr. `call_s_i(h, "F", 7)` calls
`const char* F(int)`. Adding a shape is one line in
[`aether_host_tinygo.c`](aether_host_tinygo.c) plus a matching
`extern` and wrapper in [`module.ae`](module.ae).

Every string-returning wrapper has an `_owned` twin for a function
that returns `C.CString(...)`; see [Memory ownership](#memory-ownership):

| Borrows the result | Takes it over and frees the C pointer |
|---|---|
| `call_str_str` | `call_str_str_owned` |
| `call_s_v` | `call_s_v_owned` |
| `call_s_s` | `call_s_s_owned` |
| `call_s_i` | `call_s_i_owned` |
| `call_s_s_s` | `call_s_s_s_owned` |
| `call_s_s_s_s` | `call_s_s_s_s_owned` |

For any other signature, `call_dynamic` dispatches through libffi
when the bridge is built with `AETHER_HAS_LIBFFI` defined. Without
libffi it returns 0 and `last_error()` says libffi is unavailable,
so the module itself needs nothing beyond `std.dl`.

## Memory ownership

A string-returning wrapper gives back the pointer the Go function
returned, and Aether only borrows it: nothing on this side frees it.
The bridge cannot tell from a signature who owns the result, so the
caller says so, per call site (#2569):

- **`C.CString(...)` result: call the `_owned` twin.** cgo allocates
  a `C.CString` with `malloc` on the C heap, outside the Go collector,
  so through a borrowing wrapper every call leaks one string.
  `call_str_str_owned` and the other `_owned` wrappers copy the result
  into a string Aether owns, `free()` the C pointer, and Aether frees
  the copy like any string it made. A nil result, or a symbol that
  does not resolve, gives `""`.
- **Static or long-lived result: call the plain wrapper.** A pointer
  the library keeps (a constant, a buffer it reuses) must never reach
  `free()`, so it is borrowed, and stays valid for as long as the
  library keeps it.

The `_owned` wrappers free with the C runtime the Aether program uses,
so the library has to allocate with the same one. On Linux and macOS
there is one. On Windows, MSVCRT and UCRT keep separate heaps, so
build the library with a `gcc` of the same runtime as the one `ae`
builds the program with: cgo uses the `gcc` on `PATH`, and `ae` uses
`$AE_CC`, `$CC`, the `gcc` on `PATH`, or its own WinLibs UCRT copy,
in that order.

`call_dynamic` with return kind `'s'` writes the library's pointer to
`result_out` as it is; for a `C.CString`, free it once you are done.

The plain wrappers keep their behaviour. Whether a `C.CString` result
should instead be taken over by default, with a borrowed form for the
static case, is still open in #2569, since it would change what the
existing call sites do.

## Limitations

- **Single Go runtime per process.** `dlopen` of two distinct
  c-shared Go libraries in the same process can collide on
  runtime state. Stick to one library per Aether process.
- **No goroutines that outlive the Aether call.** The Go
  scheduler runs inside the call; spawning a goroutine that
  blocks on I/O and returns to the Aether caller before the
  goroutine completes is undefined behaviour.
- **`tinygo build -buildmode=c-shared` is wasm-only today.**
  Native linux/darwin/windows c-shared `.so`s come from standard
  `go build` (see the toolchain note at the top of this file).
  The Aether-side bridge is purely a `dlopen` + symbol lookup —
  it loads whatever c-shared the toolchain produced.
- **Symbol export is `//export Name`.** Neither standard Go nor
  TinyGo exports package-level `Name` automatically — you must
  annotate each function you want callable from C / Aether.

## Testing

The end-to-end tests live at
[`tests/integration/host_tinygo/`](../../../tests/integration/host_tinygo/),
and both need the bridge archive (`make contrib`, or
`MODULES=tinygo bash tests/scripts/contrib_build.sh`); without it they
SKIP. The `contrib/host bridges (Linux)` CI job builds it and runs both.

- [`test_host_tinygo.sh`](../../../tests/integration/host_tinygo/test_host_tinygo.sh)
  builds `examples/greet.go` with
  `CGO_ENABLED=1 go build -buildmode=c-shared` and runs
  [`uses_tinygo.ae`](../../../tests/integration/host_tinygo/uses_tinygo.ae)
  against it: `Answer = 42`, `Add(2, 40) = 42`, `Negate(7) = -7`, and
  `hello, world` through `call_str_str_owned`, a real `C.CString`
  freed by the bridge. It SKIPs when `go` is not on PATH, matching
  `contrib/host/go`'s pattern.
- [`test_host_tinygo_owned.sh`](../../../tests/integration/host_tinygo/test_host_tinygo_owned.sh)
  needs no Go toolchain: it builds
  [`fake_cshared.c`](../../../tests/integration/host_tinygo/fake_cshared.c),
  a C library with the signatures cgo generates and `C.CString`'s
  `malloc`'d results, with the C compiler, and runs
  [`uses_owned.ae`](../../../tests/integration/host_tinygo/uses_owned.ae):
  every `_owned` wrapper, the nil and unresolved-symbol results, a
  static result through the borrowed wrappers (which must not free it),
  and rounds of 100 owned calls of a 64 KiB result, with the heap
  checked for growth from round to round (`mem.steady_growth`).

## See also

- [`contrib/host/go/`](../go/) — full Go via subprocess + LD_PRELOAD
  sandbox.
- [`std/dl/module.ae`](../../../std/dl/module.ae) — the
  cross-platform `dlopen` shim this host sits on top of.
- [TinyGo c-shared docs](https://tinygo.org/docs/guides/cgo/)
