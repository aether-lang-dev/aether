# std.dl

Dynamic library loading: `dlopen`, `dlsym`, `dlclose`.

For plugins and optional dependencies — code that may or may not be present at
runtime, resolved by name rather than linked. If the library is a hard
requirement, link it instead; this is for the cases where absence is a
supported outcome.

The example **compiles but is not run** in CI: it needs a shared library
present at a known path, which varies by platform.

```aether
import std.dl

main() {
    handle, err = dl.open("libm.so.6")
    if err != "" {
        // Absence is a normal outcome here, not a fault.
        println("not available: ${err}")
        return
    }

    sym, serr = dl.symbol(handle, "cos")
    if serr != "" {
        println("symbol not found: ${serr}")
        dl.close(handle)
        return
    }

    println("resolved")
    dl.close(handle)
}
```

`last_error()` returns the platform's own diagnostic, which is more specific
than the error string and worth logging when a load fails unexpectedly.

A resolved symbol is a raw `ptr`. Calling it requires a matching signature at
the call site, and Aether cannot check that for you — a mismatch is undefined
behaviour rather than a type error, so this is one of the few places where the
compiler is not covering you.

Library naming is platform-specific (`libm.so.6`, `libm.dylib`,
`msvcrt.dll`), so a portable caller probes several names rather than assuming
one.

## Exports

`open`, `symbol`, `close`, `last_error`.
