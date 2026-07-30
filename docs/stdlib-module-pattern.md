# Aether Stdlib Module Pattern

This is the canonical shape for an Aether standard-library module. Use it
when adding a new `std/<name>/` module or retrofitting an existing one.

The worked example throughout this document is
[`std/fs/module.ae`](../std/fs/module.ae), read it alongside this doc.

## The shape

Every `std/<name>/module.ae` has two layers:

1. **Raw externs**, thin declarations of the underlying C functions.
   Fallible externs use a `_raw` suffix and return the old C conventions
   (`ptr` for nullable, `int` for 1/0 success flags).
2. **Go-style wrappers**, Aether functions that call the raw externs
   and translate C-style returns into Go-style `(value, err)` tuples
   (or just `err` for operations with no return value).

```
// std.X - ... header
//
// API shape:
//   - Raw externs end in `_raw` and return ptr/int in the old C-style
//     convention. They are the escape hatch for advanced callers.
//   - Aether-native wrappers (below) use Go-style `(value, err)` tuple
//     returns and are the idiomatic way to call X operations.

// ---- raw externs ----
extern X_op_raw(args) -> ptr
extern X_pure_op(args) -> int     // infallible, no _raw, no wrapper

// ---- Go-style wrappers ----
// Short docstring stating: what it does, and exactly what shape it
// returns on success vs. failure.
op(args) -> {
    r = X_op_raw(args)
    if r == null {
        return null, "human-readable error"
    }
    return r, ""
}
```

## Rules

### When to add a `_raw` extern + wrapper

Any operation that can **fail at runtime**, open a file that doesn't
exist, allocate when memory is tight, parse malformed input, look up an
absent key, etc. These get:

- A `_raw`-suffixed C function (either rename the existing one or add
  it alongside).
- A companion Aether wrapper that turns the C return into either
  `(value, "")` on success or `(zero-value, "reason")` on failure.

### When NOT to wrap

Keep raw externs with **no** `_raw` suffix and **no** wrapper for
operations that are:

- Pure total functions that can't fail for any well-typed input
  (`path_join`, `path_basename`, `math_sqrt`, `math_sin`,
  `string_length`, `list_size`), and infallible boolean queries
  (`file_exists`, `path_is_absolute`, `fs_is_symlink`, `json_is_null`,
  `map_has`).
- **Sentinel-returning infallibles** where the sentinel is part of the
  contract, not an error. Example: `file_mtime` returns `0` for missing
  files or stat failure, callers already handle zero as "no mtime", so
  wrapping would just add noise.
- **Intentionally void fire-and-forget operations.** Example:
  [`log_write`](../std/log/module.ae) in `std.log` the module
  deliberately degrades to stderr if the log file is unopenable, so
  callers don't need an error from each write.
- **Structural/DSL builders.** Example: the manifest DSL in
  [`std.host`](../std/host/module.ae), `describe`, `input`, `event`,
  `bindings`, `java`, … are void because they structurally append to a
  global registry. They're not silent failures, they're a different
  pattern.

### Return shapes

| Operation | Success return | Failure return |
|---|---|---|
| Produces a value | `(value, "")` | `(zero-value, "reason")` |
| Produces nothing (setter, writer) | `""` | `"reason"` |
| Lookup that can be absent | `(value, "")` / `(null, "")` for absent | `(null, "reason")` for error |

For the lookup row: distinguish **absent** from **error**. An absent key
is not an error, the caller asked if X exists. Only return a non-empty
error string when something actually went wrong (null receiver, wrong
type, allocation failure).

### Structured errors (pilot)

A small set of newer wrappers extends the (value, err) shape with a
**third element**: an integer "kind" that lets callers
programmatically discriminate between failure modes without parsing
the human-readable message. The tuple is `(value, kind: int,
message: string)`. `kind == 0` means success (constant `KIND_OK`);
non-zero means an error of the named class.

```
// (bytes_copied, kind, message)
//   on success:  (n, KIND_OK, "")
//   on failure:  (bytes_so_far, KIND_NOT_FOUND, "open src: ...")
copy(src: string, dst: string) -> (int, int, string) {
    return fs_copy_raw(src, dst)
}
```

Why kinds are returned per-call (not via process-global accessors like
`vcr.last_kind()`): std.fs is called concurrently from actors; a
shared mutable slot would race. The triple costs one extra register
on every ABI we target (x86-64 SysV, AArch64 AAPCS, MS x64).

The kinds themselves are exported `const int` values from the module
that uses them (e.g. `fs.KIND_NOT_FOUND`, `fs.KIND_PERMISSION_DENIED`).
Modules that use kinds **must** also export a matching
`AETHER_<MODULE>_KIND_*` C macro set in their header so the C side
and the Aether surface stay in lock-step.

This shape is **opt-in and additive**. Existing wrappers keep their
(value, err) or string-error returns, converting them is a
non-breaking but wider change tracked separately. The pilot scope is
`std.fs.{copy, move, realpath, chmod}`. Future modules may adopt the
shape after the pilot has shaken out.

| Operation | Success return | Failure return |
|---|---|---|
| Structured-error producer | `(value, KIND_OK, "")` | `(zero-value-or-partial, KIND_*, "reason")` |

### Docstring

Every wrapper gets a short docstring stating what it does and **the
exact return shape**. Match this voice:

```
// Open a file. Returns (handle, "") on success, (null, error) on failure.
open(path: string, mode: string) -> {
    ...
}
```

### Ownership of returned strings

If the underlying C function returns a borrowed `const char*` (pointer
into another object's memory), the wrapper **must** copy it before
returning so the caller's string outlives the source object. The
idiomatic copy in Aether is `string_concat(borrowed, "")` see
[`std.fs`](../std/fs/module.ae)'s `readlink` wrapper for the pattern.

## Worked example: `std.fs`

[`std/fs/module.ae`](../std/fs/module.ae) is the reference
implementation. Skim it start-to-finish; it demonstrates:

- Raw `_raw` externs grouped at the top with structural comments.
- Pure-function externs (the `path_*` block) that correctly have no
  `_raw` suffix and no wrapper.
- Every fallible operation wrapped in a Go-style function below.
- Consistent `"cannot <verb> <noun>"` error phrasing.
- An explicit note where a raw extern is left unwrapped intentionally
  (`file_mtime`'s sentinel-0 contract).

## Modules that follow this pattern

`fs`, `io`, `http`, `net`, `os`, `string`, `tcp`, `dir`, `file` full
wrapper coverage. `json`, `collections`, `list`, `map` completed in
the stdlib-consistency pass alongside this doc. `log` and `host`,
intentionally follow domain-specific variants (fire-and-forget logging;
DSL builders) as described above.

## Adding a new stdlib module

1. Create `std/<name>/` with `module.ae` and `aether_<name>.c`/`.h`.
2. Write the C functions first; split into `<op>_raw` (fallible) and
   `<op>` (infallible) per the rules above.
3. Mirror each function as an `extern` in `module.ae`, copying the
   `_raw` suffix for fallible ones.
4. Add the "API shape" preamble comment.
5. Add Go-style wrappers for every fallible extern.
6. Add tests exercising both the success and error paths of each
   wrapper.
7. Add an example under `examples/stdlib/<name>-demo.ae` showing
   idiomatic use of the Go-style API.
