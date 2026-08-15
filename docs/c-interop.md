# C Interoperability

Aether interoperates with C code, so you can call existing libraries like SQLite, libcurl, and OpenSSL.

## Calling C Functions

Aether compiles to C, and the generated code already includes `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<math.h>`, etc. To call any C function, whether from the standard library, your own `.c` files, or a third-party library, you declare it with `extern`:

```aether
extern abs(x: int) -> int
extern atoi(s: string) -> int
extern puts(s: string) -> int
extern rand() -> int

main() {
    puts("Hello from C's puts()!")

    n = abs(0 - 42)
    print("abs(-42) = ")
    println(n)

    val = atoi("123")
    print("atoi = ")
    println(val)
}
```

The `extern` signature must match the real C signature. See the type mapping table below.

> **Note:** Aether has built-in functions (`print`, `println`, `sleep`, `clock_ns`, `spawn`, etc.) that don't need `extern`. See [Built-in Functions](language-reference.md#built-in-functions) in the language reference.

## The `extern` Keyword

Use `extern` to declare C functions you want to call from Aether code:
- Standard C library functions (`abs`, `atoi`, `puts`, `rand`, etc.)
- Your own C functions in separate `.c` files
- Third-party C libraries (SQLite, libcurl, etc.)
- System APIs

### Syntax

```aether
extern function_name(param1: type1, param2: type2) -> return_type
extern void_function(param: type)  // No return type = void
```

### Type Mapping

| Aether Type | C Type |
|-------------|--------|
| `int` | `int` |
| `float` | `double` |
| `string` | `const char*` |
| `bool` | `int` |
| `byte` | `unsigned char` |
| `ptr` | `void*` |

> **`byte` mapping note.** `byte` lowers to `unsigned char`, not `uint8_t`. The two are typedef-compatible on every platform Aether targets, but C's strict-aliasing rules give `unsigned char *` an exemption (it can legally alias any type's bytes for read/write); `uint8_t *` does not. Since `byte` is exactly the type used in C extern signatures that scrape bytes from other types' storage (packed binary protocol parsers, NaN-boxing tag readers, on-disk file headers), `unsigned char` is the right choice.

> **Binding a C `bool` return: use `byte`, not `bool`.** Aether's `bool` maps to C `int` (above), so declaring a C function that returns C's one-byte `_Bool` as `-> bool` reads a full `int` and picks up three bytes of stack garbage past the result, a success can read back as something like `-255`. Bind a `bool`-returning C function with `-> byte` instead: it reads exactly the one byte the ABI wrote, and `0` vs non-zero tests the same way.

### Example: Custom C Functions

**my_math.c:**
```c
#include <math.h>

int my_add(int a, int b) {
    return a + b;
}

double my_power(double base, double exp) {
    return pow(base, exp);
}

void my_greet(const char* name) {
    printf("Hello, %s!\n", name);
}
```

**main.ae:**
```aether
// Declare our C functions
extern my_add(a: int, b: int) -> int
extern my_power(base: float, exp: float) -> float
extern my_greet(name: string)

main() {
    result = my_add(10, 20)
    print("10 + 20 = ")
    print(result)
    print("\n")

    power = my_power(2.0, 10.0)
    print("2^10 = ")
    print(power)
    print("\n")

    my_greet("Aether")
}
```

**Build and run:**
```bash
# Compile C code
gcc -c my_math.c -o my_math.o -lm

# Compile Aether code to C
aetherc main.ae main.c

# Link everything together. `ae cflags` emits the right -I / -L / -l
# flags for the install in effect, never hand-craft them: the install
# layout (and the linker search path it requires) is unstable across
# releases.
gcc main.c my_math.o $(ae cflags) -o myapp

./myapp
```

## Tuple Return Types, `extern foo(...) -> (T1, T2, ...)`

C functions that return more than one logical value typically pack the results into a struct returned by value. Aether mirrors this on the FFI side: declare the extern with a parenthesised tuple return type, and the codegen synthesises the matching C struct typedef so the call site can destructure the result like any Aether-side tuple-returning function:

```aether
extern parse_int_safe(s: string) -> (int, string)

main() {
    val, err = parse_int_safe("42")
    if err != "" {
        // …handle error…
    }
    println(val)
}
```

The C side provides a function returning a struct with the matching layout. The struct's fields are named `_0`, `_1`, … in declaration order, and the typedef name is `_tuple_<T1>_<T2>` (e.g. `_tuple_int_string`):

```c
#include <stdlib.h>
#include <string.h>

typedef struct { int _0; const char* _1; } _tuple_int_string;

_tuple_int_string parse_int_safe(const char* s) {
    _tuple_int_string t;
    if (!s || !*s) { t._0 = 0; t._1 = "empty"; return t; }
    char* end;
    long n = strtol(s, &end, 10);
    if (end == s) { t._0 = 0; t._1 = "not a number"; return t; }
    t._0 = (int)n;
    t._1 = "";
    return t;
}
```

Three or more elements work the same way:

```aether
extern fs_read_binary_tuple(path: string) -> (ptr, int, string)

read_binary(path: string) -> {
    return fs_read_binary_tuple(path)   // Aether destructures into (bytes, length, err)
}
```

The struct-by-value ABI matches what Aether-side multi-return functions emit, so an Aether-defined `-> { return a, b }` (inferred) or `-> (T1, T2) { ... }` (explicit) and a C-side `-> (T1, T2)` extern are interchangeable from the caller's perspective.

### Type-name mangling

The codegen builds the typedef name from the element types' lowercase Aether names:

| Aether tuple                  | C typedef name                |
|-------------------------------|-------------------------------|
| `(int, int)`                  | `_tuple_int_int`              |
| `(int, string)`               | `_tuple_int_string`           |
| `(ptr, int, string)`          | `_tuple_ptr_int_string`       |
| `(int, float, string)`        | `_tuple_int_float_string`     |

Element types use `string` for `const char*` and `ptr` for `void*`. The typedef is emitted once per unique tuple shape, multiple externs returning the same tuple share the typedef.

### When to reach for it

Tuple returns are the natural shape for any C function that produces a value plus an error message, a value plus a length, or any other small product type. Before this form was available, FFI authors worked around the single-scalar return by either packing values into a delimited string or splitting the operation into 4+ split-accessor externs (`<op>_raw` + `<op>_get` + `<op>_get_length` + `<op>_release` with TLS-backed storage). The tuple form replaces both patterns with a single declaration.

### Binding struct-returning C functions

The mechanism above is, in practice, **by-value struct return** for any C
struct whose layout matches the synthesized typedef, not just for
"multiple logical values". raylib's `Image LoadImage(const char*)`
layout `{void*, int, int, int, int}`, binds with zero glue:

```aether
@extern("LoadImage") load_image(path: string) -> (ptr, int, int, int, int)

main() {
    data, w, h, mips, fmt = load_image("frame.png")
    // -> Image: 400x420 mips=1 fmt=7
}
```

Any struct of scalar/pointer fields works this way; the field ORDER in the
tuple is the layout contract.

### Tuple parameters, by-value struct arguments

The parameter-position mirror: an extern parameter typed as a tuple lowers
to the same synthesized struct typedef, passed **by value**. This is most
of any real C API, raylib again:

```c
void ImageDrawTriangle(Image *dst, Vector2 v1, Vector2 v2, Vector2 v3, Color color);
```

```aether
@extern("ImageDrawTriangle")
img_triangle(dst: ptr, v1: (f32, f32), v2: (f32, f32), v3: (f32, f32),
             c: (byte, byte, byte, byte))

img_triangle(img, (10.0, 10.0), (60.0, 10.0), (35.0, 50.0), (255, 0, 0, 255))
```

Call sites pass **parenthesized tuple literals**, `(x, y)`; a comma inside
parens is what makes it a tuple rather than grouping. The codegen packs
each literal into the matching `_tuple_*` compound literal with per-element
casts, so Aether `float` (double) expressions narrow to `float` fields and
ints to `unsigned char` without warnings, and no hand-written flat-scalar
C shim (or its extra call frame) is needed.

A call site may also pass a **tuple-typed value** whose type matches the
parameter, not only a literal: a variable holding a tuple, or the
result of a tuple-returning extern passed straight through. The value already
*is* the matching `_tuple_*` struct, so it crosses by value with no
destructure, which is what makes pass-through FFI chains compose:

```aether
img = gen_image(64, 48, (30, 20, 16, 255))   // img : (ptr, int, int, int, int)
export_image(img, "out.png")                  // hand the same struct back
save(load(path))                              // and chains compose directly
```

A value whose tuple shape does not match the parameter (wrong element count or
element kinds), or a non-tuple value, is rejected at type-check with the same
`tuple-typed` diagnostic.

`f32` is the 32-bit C `float` type, added for exactly these signatures:
raylib's `Vector2` is `float` ×2 and Aether's own `float` is C `double`,
so without it the layout was inexpressible. It works in both parameter and
return position (`extern get_pos() -> (f32, f32)`); Aether-side arithmetic
on the destructured values still happens in double.

**Conservative slice** (current contract): tuple parameter elements may be
`int`, `long`, `float`, `f32`, `byte`, `bool`, or `ptr`, no strings, no
nesting. The typechecker enforces element count and kinds, and rejects a tuple
literal aimed at a non-tuple parameter. Exercised end-to-end by
`tests/integration/extern_tuple_param/` (literals) and
`tests/integration/extern_tuple_var_passthrough/` (values and call chains).

## Renaming a C Symbol, `@extern("c_name")`

Sometimes the C symbol you want to bind has a name that clashes with a wrapper you'd like to expose, or that doesn't fit the module's naming style. Use the `@extern` annotation to bind an Aether-side name to a chosen C symbol:

```aether
@extern("EVP_MD_CTX_new") md_ctx_new() -> ptr
@extern("EVP_MD_CTX_free") md_ctx_free(ctx: ptr)
@extern("strerror") describe_errno(errno: int) -> string
```

The Aether-side name (`md_ctx_new`, `describe_errno`) is what callers write. The compiler emits the forward declaration and every call site using the C symbol from the annotation, no wrapper function is generated. This is exactly equivalent to writing:

```aether
extern EVP_MD_CTX_new() -> ptr
md_ctx_new() -> ptr {
    return EVP_MD_CTX_new()
}
```

…minus the wrapper. Both forms link to the same C symbol; `@extern` just removes the ceremony when all you wanted was a rename.

The annotation accepts a single string literal (the C symbol name). Parameter types and return type are required, exactly as for plain `extern`.

An `@extern` declaration is part of its module's public surface: unlike a bare `extern` C binding (which stays private to its defining file), an `@extern` name declared in a module is callable as `module.name(...)` from any file that imports the module. This is the mechanism a stdlib module uses to expose a C symbol under a clean qualified name without an Aether wrapper.

`@extern` also accepts a trailing `...` for variadic C symbols, exactly like a plain `extern`:

```aether
@extern("aether_strbuilder_append_format")
append_format(b: ptr, fmt: string, ...) -> int
```

Call sites pass any number of trailing arguments literally, and `@extern` is the way to give a variadic C function a clean module-qualified name with no wrapper at all. An Aether function can also *define* a variadic tail and forward it, see [Forwarding a variadic tail to C](#forwarding-a-variadic-tail-to-c-va_list).

## Exporting an Aether Function as a C Callback, `@c_callback`

The inverse of `@extern`. When a C library wants you to hand it a function pointer, HTTP route handlers, signal handlers, `qsort` comparators, libcurl callbacks, sqlite hooks, annotate the Aether function with `@c_callback`. The compiler emits a stable, externally-visible C symbol that the linker can resolve from any translation unit:

```aether
extern http_server_add_route(server: ptr, method: string, path: string, handler: ptr, user_data: ptr)

@c_callback
my_handler(req: ptr, res: ptr, ud: ptr) {
    // …handler body in Aether…
}

main() {
    http_server_add_route(server, "GET", "/hello", my_handler, null)
}
```

`my_handler` here is a normal Aether function, direct calls work the same as any other function. The annotation only changes two things:

1. **The C symbol stays addressable across the linkage boundary.** Without `@c_callback`, an imported function would be emitted as `static` (so each translation unit gets a private copy and macOS's `ld64` doesn't trip on duplicate symbols). With `@c_callback`, the function is non-`static` and the symbol resolves at link time wherever it's referenced.
2. **The function name used as a value** (when you pass it as a `ptr` argument like the `http_server_add_route` example above) emits the C symbol the annotation binds to, not the namespace-mangled Aether name.

### Optional explicit symbol

By default, the C symbol matches the Aether-side name (or the namespace-prefixed form `<module>_<name>` when defined inside an imported module, e.g. `vcr_dispatch`). Pass an explicit string when you need a specific C name:

```aether
@c_callback("aether_signal_handler")
on_sigint(sig: int) {
    // …
}
```

…the linker resolves `aether_signal_handler`; calls in Aether code still use `on_sigint`. Useful when integrating with a C library whose API documents a specific symbol name.

### When to use it

`@c_callback` is the right shape any time a C function takes a function pointer parameter:

- HTTP route handlers (`http_server_add_route`).
- POSIX signal handlers (`signal(SIGINT, …)`).
- `qsort` / `bsearch` comparators.
- libcurl write/read/header callbacks.
- libuv `uv_*_cb` callbacks.
- sqlite update/commit/rollback hooks.

For plain Aether-to-Aether function-pointer use within a single program, no annotation is needed, top-level functions are already addressable as values. The annotation is specifically for the cross-language path.

### Callback tables: Aether functions in C function-pointer fields

`@c_callback` gives an Aether function a C symbol; the other half of the pattern is storing it in a C struct that C then calls through. Callback tables are everywhere in C: Redis's `dictType`, `rio` method tables and connection vtables, `qsort` comparators, module hooks.

Declare the fields with their signatures rather than as bare `ptr`:

```aether
extern struct dictType @c_import {
    hashFunction: fn(ptr) -> int
    keyCompare: fn(ptr, ptr) -> int
}

@c_callback
my_hash(key: ptr) -> int { return 7 }

t = malloc(64) as *dictType
t.hashFunction = my_hash        // checked against fn(ptr) -> int
```

- **The slot is typed.** A function whose arity or parameter types do not match the field is a compile error, so the table cannot be wired up wrong. A `ptr`-typed field would accept anything and fail as a corrupt call later.
- **The field holds the function's real address**, cast to whatever type the header declared for that member, so C can call through it directly. Only a top-level function qualifies: a closure carries an environment pointer, and a C function pointer has nowhere to put it.
- This is specific to **C-owned** structs (`extern struct`, with or without `@c_import`). A callback field on an Aether-owned struct holds a closure and keeps its captures, which is the right behaviour there and unchanged.

### Companion: `@extern("c_symbol")`

`@extern` and `@c_callback` close the FFI loop in both directions. `@extern` binds an Aether-namespace name to a C symbol the linker provides; `@c_callback` emits an Aether function under a C symbol the linker can hand to any consumer. Both use the same `@`-prefixed annotation grammar.

## Letting the Header Own the Prototype, `extern ... @c_import`

For every `extern` Aether normally emits its own forward declaration into the generated C. That is what you want when the C side has no header handy. When it does have one, the two declarations have to agree exactly, and Aether's cannot always match: it writes `int` where the header says `uint8_t` or `size_t`, and `void*` where the header names a type. Both spellings are ABI-compatible, which is why the failure is confusing, you get a `conflicting types` error from a call that would have worked, or under LTO a type-mismatch warning indistinguishable from a real porting mistake.

`@c_import` after the signature hands prototype ownership to the header. Aether emits no declaration at all:

```aether
extern type blob

extern api_scale(v: int) -> int          @c_import   // header: uint8_t api_scale(uint8_t)
extern api_span(a: int, b: int) -> int   @c_import   // header: size_t api_span(size_t, size_t)
extern api_blob() -> *blob               @c_import
```

```toml
# aether.toml, so the header is in scope in the generated TU
[build]
cflags = "-include api.h -I."
```

Nothing is given up by doing this. The Aether-side types are still declared and still drive typechecking, they simply never reach the generated C, so there is only one declaration in the translation unit and it is the header's. The C compiler continues to check every **call site** against that declaration, so a genuinely wrong Aether signature is still caught, just as a call-site diagnostic rather than a prototype clash.

### `static inline` header helpers

A `static inline` helper has no linkable symbol, so a non-static prototype for it refers to nothing and the C compiler rejects the pair outright:

```c
/* api.h */
static inline uint8_t api_inline_twice(uint8_t v) { return (uint8_t)(v * 2); }
```

```aether
extern api_inline_twice(v: int) -> int @c_import
```

With `@c_import` the force-included definition is the only one the translation unit sees, the call inlines directly, and no hand-written C bridge function is needed. Without it, Aether's `int api_inline_twice(int);` collides with the header and the build fails. This is the shape to reach for when porting a C codebase whose headers are full of small inline helpers.

`@c_import` stacks with the other extern attributes, `-> string @heap @c_import` and variadic `extern log_line(fmt: string, ...) @c_import` are both legal. It is the same "the header is the sole source of truth" contract as `extern struct ... @c_import` and `extern const NAME: type @c_import`.

## Forwarding a Variadic Tail to C, `va_list`

Every printf-style C function comes in a pair: the variadic one and the `v*` one that takes an already-collected argument list (`printf`/`vprintf`, `snprintf`/`vsnprintf`, and in Redis `serverLog`, `sdscatprintf`, `sentinelEvent`). An Aether function can define a variadic tail and forward it to the `v*` half:

```aether
extern vsnprintf(buf: ptr, n: int, fmt: ptr, ap: va_list) -> int

format_into(buf: ptr, n: int, fmt: ptr, ...) -> int {
    let ap = va_start()
    let written = vsnprintf(buf, n, fmt, ap)
    va_end(ap)
    return written
}
```

Three intrinsics drive it, mirroring `<stdarg.h>`:

| Form | Meaning |
|---|---|
| `va_start()` | Opens the calling function's variadic tail and yields an opaque cookie. Valid only inside a function declared with a trailing `...`. |
| `va_arg(ap, T)` | Reads the next argument as `T`. The declared type is trusted, exactly as C trusts it, there is no runtime tag to check it against. |
| `va_end(ap)` | Closes the tail. |

**Spell the receiving parameter `va_list`, not `ptr`.** `va_start()` yields a cookie that *points at* the function's `va_list`, so it can be held in an ordinary `ptr` local; the callee needs the `va_list` itself. Declaring the parameter `va_list` is what tells the call site to pass it correctly. Declaring it `ptr` compiles without a warning and then formats garbage, because the callee reads the cookie as though it were the argument list.

`va_list` typechecks as an opaque `ptr`; the difference is only the C spelling that codegen emits, the same mechanism as `size_t` and `cstring` under [Typed and qualified C pointers](#typed-and-qualified-c-pointers).

For **checking** format strings rather than forwarding them, nothing extra is needed: a call to a printf-family extern with a literal format keeps that literal in the generated C, so the C compiler's `-Wformat` sees it and the warning is attributed back to the `.ae` line.

## Linking External Libraries

Use `link_flags` in your `aether.toml` to link external C libraries:

```toml
[project]
name = "my-project"
version = "1.0.0"

[build]
link_flags = ["-lsqlite3", "-lcurl", "-lm"]
```

### Example: Using SQLite

**database.ae:**
```aether
// SQLite C API
extern sqlite3_open(path: string, db: ptr) -> int
extern sqlite3_close(db: ptr) -> int
// `callback` is a reserved word in Aether, so the parameter is named `cb`.
// Parameter names are local to the declaration; the C symbol is unaffected.
extern sqlite3_exec(db: ptr, sql: string, cb: ptr, arg: ptr, errmsg: ptr) -> int

main() {
    db = 0  // Will hold database pointer

    // Open database
    result = sqlite3_open("test.db", db)
    if (result != 0) {
        print("Failed to open database\n")
        return
    }

    // Execute SQL
    sql = "CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT)"
    sqlite3_exec(db, sql, 0, 0, 0)

    // Close database
    sqlite3_close(db)
    print("Database operations complete\n")
}
```

**aether.toml:**
```toml
[project]
name = "sqlite-demo"
version = "1.0.0"

[build]
link_flags = ["-lsqlite3"]
```

## Best Practices

1. **Prefer Aether's standard library** for common operations when available (e.g. `import std.list` rather than hand-rolling a linked list in C)
2. **Use `extern` for any C function** you want to call, including standard library functions like `abs`, `atoi`, `puts`, etc.
3. **Do not redeclare stdlib symbols via `extern`** as a workaround. If you find yourself writing `extern list_add_raw(list: ptr, item: ptr) -> int` (or similar mirrors of `std.list` / `std.map` / `std.string` raw functions) in your own code, stop and use `import std.list` (etc.) instead. Older Aether had link-time issues with shared modules importing stdlib which made the manual-extern shape look attractive as a workaround; those bugs are closed. The import-then-namespace shape (`list.add(out, x)`) is the only supported path today, it gives compile-time type checking against the stdlib's declared signatures, automatic API tracking when stdlib evolves, and consistency with every other Aether codebase. Manual externs of stdlib symbols are fragile (signature drift bites at runtime), bypass type-safety, and grow into per-feature maintenance debt.
4. **Document your C dependencies** in your project's README
5. **Handle errors** - C functions often return error codes
6. **Memory management** - Be careful with C memory; use Aether's memory management where possible

## Built-in primitives and the `aether_` C-symbol convention

Aether's built-in primitives that have a corresponding libc function (e.g. `sleep(ms)`) lower to `aether_`-prefixed runtime symbols rather than the bare libc name. For `sleep(ms)`, the generated C calls `aether_sleep_ms(ms)` a thin wrapper provided by `libaether.a` that handles `Sleep()` on Win32 and `usleep()` on POSIX.

This matters when your Aether program (or any module it imports) declares its own `extern sleep(...)` for some other purpose. Without the prefix, the codegen-emitted forward declaration of the user's `extern sleep` would conflict with libc's `unsigned int sleep(unsigned int)` and break compilation:

```
error: conflicting types for 'sleep'; have 'void(int)'
note:  previous declaration … 'unsigned int(unsigned int)'
```

To avoid this collision class entirely:

- **Built-ins are routed through `aether_`-prefixed runtime helpers** in the generated C, they never emit a bare libc symbol.
- **User `extern` declarations of well-known libc names** (e.g. `sleep`, `exit`, `printf`, `puts`, `malloc`, `read`, `write`, `time`, …) are recognized and the compiler **suppresses the C forward declaration**. The libc header (already included by the prelude) supplies the canonical prototype, and call-site code generation still uses the registered Aether parameter types for type-aware casting.

When you write a C shim or third-party binding, you can follow the same convention: prefix exported symbols with `aether_<module>_` (e.g. `aether_vcr_record`, `aether_http_get`) so they don't collide with vendor libraries the user might link via `[build] extra_sources = […]`.

## Working with Pointers

The `ptr` type maps to `void*` in C, useful for opaque handles and callbacks:

```aether
extern create_handle() -> ptr
extern use_handle(h: ptr) -> int
extern destroy_handle(h: ptr)

main() {
    handle = create_handle()
    if (handle != 0) {
        use_handle(handle)
        destroy_handle(handle)
    }
}
```

### Passing Integers to `ptr` Parameters

When an extern function expects a `ptr` parameter and you pass an `int`, the compiler automatically emits the correct `(void*)(intptr_t)` cast, no explicit casting required:

```aether
import std.list

main() {
    items = list.new()
    defer list.free(items)

    i = 0
    while i < 5 {
        list.add(items, i)   // int passed to void*, cast emitted automatically
        i = i + 1
    }
    print(list.size(items))
    print("\n")
}
```

The generated C is `list_add(items, (void*)(intptr_t)(i))`, which is the well-defined idiom for storing integer values in `void*` containers.

## Consuming `-> string` Returns From C

An extern declared `-> string` does NOT give the C side a plain `const char*`. It returns an `AetherString*` a 32-byte, magic-tagged header whose `data` field points at the actual bytes:

```c
struct AetherString {
    unsigned int magic;      // 0xAE57C0DE
    int          ref_count;
    size_t       length;
    size_t       capacity;
    char        *data;       // <- the payload the shim actually wants
};
```

If a C shim types the same extern as `extern const char* foo(...)` and then runs `memcpy(dst, result, n)` or `strlen(result)` on it, the shim reads into the struct header instead of the payload: the first bytes are the magic, followed by the ref count, lengths, etc. The symptom is garbage data + wildly wrong lengths; there is no build warning and no runtime diagnostic.

The fix is to unwrap the result through the public helpers in `aether_string.h`:

```c
#include "aether_string.h"

extern const void* read_config_file(const char* path);
// (Note: redeclaring the extern's return as `const void*` makes the
// unwrap requirement obvious. `const char*` compiles but is a trap.)

void load(const char* path, char* out, size_t cap) {
    const void* result = read_config_file(path);
    const char* data   = aether_string_data(result);     // real byte pointer
    size_t      len    = aether_string_length(result);   // exact length
    if (len > cap) len = cap;
    memcpy(out, data, len);                               // binary-safe
}
```

Both helpers accept either an `AetherString*` (the common case) or a raw `char*` (legacy TLS-arena externs whose names end in `_raw`), so shims can be agnostic about which flavour they got. Both are safe on NULL.

> **Historical note:** older shims open-coded this unwrap by pattern-matching the magic number themselves (`#define AETHER_STRING_MAGIC 0xAE57C0DE` + cast). `aether_string_data` / `aether_string_length` have replaced that pattern, downstream projects can delete the open-coded unwrap once they pick up the new headers.

### Passing `string` values INTO C externs (auto-unwrap)

The symmetric direction, Aether code passing a `string` value to a C extern declared `const char*` is handled by the codegen automatically. When the call site has the form:

```aether
import std.encoding
import std.string

extern probe_consume(content: string, len: int) -> int

main() {
    raw, _ = encoding.base64_decode("QUI=")      // an AetherString
    probe_consume(raw, string.length(raw))       // C side gets the payload, not the header
}
```

…the codegen emits `probe_consume(aether_string_data(raw), raw_len)` rather than passing `raw` straight through. `aether_string_data` dispatches on the AetherString magic header: returns `s->data` for wrapped strings, the bare pointer for plain `char*` values (string literals). Idempotent and safe on either shape.

This means C shim authors don't have to remember to unwrap on the receiving side; the C function can `memcpy(...)` or `strlen(...)` on the `const char*` parameter and get the payload bytes regardless of what the Aether caller passed.

**Stdlib exception:** C functions whose names start with `string_` or `aether_string_` are treated as "string-aware" and are *not* unwrapped at the call site. They use `str_data` / `str_len` internally and need the AetherString header to recover the stored length on binary content. If a downstream user-defined function happens to match those name prefixes, rename it (or expose it via a different namespace).

**Sophisticated-consumer escape hatch.** A C function that *wants* the AetherString header pointer (e.g. so it can call `aether_string_data` / `aether_string_length` itself to recover the stored length on binary content) should declare its parameter as `ptr` rather than `string`:

```aether
// Naive consumer, receives payload bytes; codegen auto-unwraps:
extern memcpy_into_buf(content: string, len: int) -> int

// Sophisticated consumer, receives raw pointer; consumer dispatches:
extern parse_with_explicit_length(s: ptr) -> int
```

The C function then takes `const void*` (or `const AetherString*`) and goes through the helpers manually. This is the escape hatch for any FFI shim that needs binary-safe length reads, declaring `s: string` would auto-unwrap and `aether_string_length` would fall back to `strlen`, truncating at the first NUL.

### Aether-emitted receivers, `name: @aether string`

The auto-unwrap above is correct for **naive C externs**, functions that receive a `const char*` and call `memcpy`/`strlen` on it. But `extern foo(s: string)` is also the spelling used to call into **another Aether-emitted module** (e.g. a function exported via `--emit=lib` from a separate `.ae` file). Aether-emitted receivers don't behave like naive C: their `string.length(s)` / `string.char_at(s, i)` already dispatch on the AetherString magic via `str_len`. If the call site auto-unwraps before the value crosses the boundary, the receiver's dispatch sees only payload bytes, the magic check fails, and `str_len` falls through to `strlen` truncating binary content at the first NUL.

The fix is per-param, opt-in: annotate the param with `@aether` to mark it as receiving an AetherString pointer (header preserved) rather than an unwrapped `const char*`:

```aether
// helper.ae (compiled with --emit=lib):
export consume_binary(s: string) {
    println("len=${string.length(s)}")   // sees stored length, not strlen
}

// caller.ae:
extern consume_binary(s: @aether string)   // ← header preserved
extern other_c_function(s: string)         // ← still unwrapped (naive C contract)
```

The `@aether` annotation is per-param, so a single extern can mix Aether-emitted-string params with naive-C-pointer params:

```aether
extern do_thing(aether_msg: @aether string,
                c_path:     string) -> int
// Codegen emits: do_thing(msg, aether_string_data(path))
//                          ^^^                      ^^^^^
//          header preserved                 unwrapped to const char*
```

**When to reach for it:**

- Calling a function exported via `export foo(s: string) { ... }` from another `.ae` file linked into the same binary.
- The caller's value is binary content (contains NULs, returned from `bytes.finish` / `encoding.base64_decode` / `fs.read_binary` / etc.) and the receiver needs to see the full length, not the strlen-truncated prefix.

**When NOT to reach for it:**

- The receiver is hand-written C that takes `const char*`. Use bare `s: string` the auto-unwrap is doing the right thing.
- The receiver works on text content with no NULs. `strlen()` and the stored length agree, so either annotation works; bare is simpler.

The `@aether` annotation is a sibling to the `: ptr` escape hatch above. Both opt out of the auto-unwrap; they differ in what the receiving side sees:

| Param declaration | Call site emits | Receiver sees |
|---|---|---|
| `s: string` | `foo(aether_string_data(s))` | unwrapped `const char*` (strlen-bounded) |
| `s: @aether string` | `foo(s)` | AetherString pointer with header, dispatches via `str_len` |
| `s: ptr` | `foo(s)` | `void*` caller dispatches manually with `aether_string_data` / `aether_string_length` |

The regression range was v0.97.0 → v0.98.0 (the blanket auto-unwrap landed in v0.98.0); the `@aether` annotation restores the v0.97.0 behaviour for Aether-to-Aether crossings without re-breaking the v0.98.0 fix for naive C externs.

**Length-clamp hazard for binary content.** Once the auto-unwrap has fired, a C shim that receives a `string`-typed parameter has only payload bytes, no header, no stored length. A common defensive pattern is fatal here:

```c
// HAZARD once auto-unwrap is in play. `s` is auto-unwrapped payload; aether_string_length
// falls through to strlen() on binary content and truncates at NUL.
int shim(const char* s, int caller_len) {
    int n = aether_string_length(s);   // falls through to strlen!
    int safe = (caller_len < n) ? caller_len : n;  // wrong on binary
    return process(s, safe);
}
```

Before the auto-unwrap, the AetherString header leaked through into the shim, and the dispatch inside `aether_string_length` correctly returned the stored length. After it, the header is stripped at the call site, the dispatch falls through to `strlen`, and binary content gets truncated at the first embedded NUL, silently producing corrupted output on disk / in messages / etc.

**Rule:** when a C shim takes a `string` parameter from Aether AND an explicit length, **trust the length**. Don't re-derive via `aether_string_length(s)` that path is now unreachable for header-bearing values (auto-unwrap stripped the header), and the strlen fallback corrupts binary content. The Aether-side caller knows the length; pass it across the boundary.

```c
// CORRECT, caller-supplied length is the source of truth.
int shim(const char* s, int len) {
    return process(s, len);
}
```

If a shim genuinely needs to dispatch on the header (e.g. for a polymorphic API where the caller might pass either a literal or a wrapped string and the length isn't known to the caller), declare its parameter as `ptr` to opt out of auto-unwrap, and dispatch via `aether_string_data` / `aether_string_length` manually.

**The same hazard fires inside Aether code.** Any Aether library that exports a `string`-typed parameter and tries to derive length / slice / iterate is at risk for the symmetric reason: the auto-unwrap fires at the `.ae→.ae` extern boundary, replacing the length-aware AetherString with a raw payload pointer. By the time the helper sees its argument, calls to `string.length(s)`, `string.substring(s, …)`, or `string.char_at(s, i)` go through `str_len` → `strlen` and truncate binary content at the first embedded NUL.

```aether
// HAZARD, looks safe; truncates binary content at the auto-unwrap.
export slice_from(s: string, start: int, end: int) -> string {
    n = string.length(s)        // strlen on auto-unwrapped payload!
    if end > n { end = n }      // clamps to first-NUL-prefix length
    return string.substring(s, start, end)
}
```

**The proper fix is the `@aether` annotation on the caller's `extern` declaration** (see "Aether-emitted receivers" above). Mark the boundary on the caller side and the auto-unwrap is suppressed for that arg slot, the receiver sees the AetherString pointer intact, `string.length` reads the stored length, and binary content with embedded NULs round-trips correctly:

```aether
// caller.ae
extern slice_from(s: @aether string, start: int, end: int) -> string
//                ^^^^^^^ marks the receiver as Aether-emitted
```

The `slice_from` definition above can stay as-written, `string.length(s)` now sees the stored length because the header survived the boundary.

**Alternative, explicit-length parameter.** If the call site is a hand-written C shim that can't use `@aether`, or you want to be defensive about the input type, the explicit-length companions in `std.string` work:

```aether
// CORRECT, caller threads the length through; no internal strlen.
export slice_from(s: string, s_len: int, start: int, end: int) -> string {
    n = string.length_n(s, s_len)   // identity; documents intent
    if end > n { end = n }
    return string.substring_n(s, s_len, start, end)
}
```

`string.substring_n(s, s_len, start, end)` and `string.length_n(s, s_known)` exist specifically to make this pattern available without falling back to a C shim. If you find yourself accumulating `_n`-suffixed externs (`some_op_n`, `some_op_n_n`) at the FFI boundary, that's a sign the function should accept the length as a regular parameter on the Aether side too, or the caller's extern declaration should use `@aether` to skip the unwrap entirely.

### Struct overlay on raw pointers, `*StructName` and `expr as *StructName`

Systems-programming code often needs to overlay a struct header on a raw `ptr`: a linked-list node whose `next` field lives at offset 8 of a malloc'd block, a tagged-pointer JSValue, an arena-allocator chunk header, etc. Writing a parallel API of width-typed intrinsics (`ptr_set_int`, `ptr_set_ptr`, `ptr_set_long`, …) for every field width is the wrong shape, it doesn't generalise to mixed-field structs and locks the language into an opaque-pointer style that's harder to read than C itself.

Aether's answer is a first-class **pointer-to-struct type** spelled `*StructName`, plus an `as` cast operator that produces a value of that type from a raw `ptr`. The pointer-ness is part of the spelled type so callers can declare it on parameters, returns, struct fields, and locals:

```aether
extern malloc(size: int) -> ptr
extern free(p: ptr)

struct ListHead {
    next: ptr
    prev: ptr
    flags: int
}

// `*ListHead` is a first-class type. Functions can declare it on
// parameters, returns, locals, and struct fields.
init_head(h: *ListHead) {
    h.next  = 0
    h.prev  = 0
    h.flags = 1
}

main() {
    raw  = malloc(64)
    head = raw as *ListHead    // `head` has type *ListHead
    init_head(head)
    free(raw)
}
```

`*ListHead` lowers to `ListHead*` in the generated C; the cast `raw as *ListHead` lowers to `((ListHead*)(raw))`. Member access on a `*StructName`-typed value emits `view->field` (not `view.field`), matching the C convention.

**Semantics**

- The operand of `as` must be a `ptr` value (or `int` / `int64`, since Aether already coerces ptr-shaped integers to and from `ptr`). A struct value, string, or other type produces a typecheck error.
- The named struct must be defined and visible at the cast site. Unknown struct names produce a typecheck error.
- The cast itself is a view, **it does not allocate, refcount, or auto-free**. The operand pointer's lifetime is the caller's problem (the same contract as raw `extern` interaction). If the underlying memory is freed while a struct view still references it, you have a use-after-free; Aether does not track this.
- Two views of the same memory alias each other (writes through one are visible through the other). This is the whole point.
- A `ptr`-typed field of a struct view can itself be re-cast: `head.next as *ListHead` reaches the next list element.
- `*StructName` is accepted in any type position: variable annotations, function parameters, function return types, struct fields, extern declarations.

**When to reach for it**

- Porting C data structures (linked lists, intrusive trees, ring buffers) where the storage was allocated by C and Aether wants to manipulate fields.
- Implementing tagged-pointer / NaN-boxing / pointer-bit-flag schemes for embedded interpreters.
- Reading on-disk file headers or wire-protocol frames where you've `read()` raw bytes into a buffer and want to reach individual fields.

**When NOT to reach for it**

- For Aether-owned data that participates in the language's normal value semantics, declare a struct and use struct-literal construction (`Point { x: 1, y: 2 }`), that path runs constructors / refcounting / lifetime tracking. The `*T` cast is specifically the unsafe-by-design escape hatch for crossing into raw memory; reaching for it on Aether-owned data sidesteps the safety machinery for no benefit.

**Token-sharing with `import x as y`**

The `as` keyword is the same token already used for module-import aliasing (`import std.cryptography as crypto`). The two parses don't collide because import-aliasing is recognised only inside `import` statements; expression-level `as *T` is recognised only as a postfix operator on expressions. Both forms continue to work in the same source file.

### Address of a field, `&lvalue`

The prefix `&` operator takes the address of an lvalue and lowers directly to C's `&`. This is what a C extern with a `&struct->field` out-parameter needs (in-place mutation, a sub-field written by the callee, a `memcpy`/resize destination):

```aether
extern struct JSObject {
    proto: long
    u: union { array: struct { tab: long  len: uint32_t } }
}
extern js_shrink_value_array(ctx: ptr, slot: ptr, new_len: int)

shrink(ctx: ptr, p: ptr, new_len: int) {
    js_shrink_value_array(ctx, &(p as *JSObject).u.array.tab, new_len)
}
```

- `&(p as *T).field` lowers to `&((T*)p)->field` the address of a field on an `extern struct` / `*StructName` overlay.
- `&local.field` lowers to `&local.field` the address of a field on an Aether-owned value struct.
- `&local` and `&arr[i]` work the same way (address of a local, address of an array element).
- The result is typed as a pointer to the field's type (`*T`), assignable to a bare `ptr` parameter. Like the overlay casts, it is a raw view: the pointer is valid only while the underlying storage is.

Without `&`, a `&struct->field` out-param forces raw `mem.long_to_ptr(base + OFFSET)` offset math, re-introducing the hand-maintained offset constant the typed overlay was meant to eliminate.

### Typed array view, `expr as T[]`

The third axis of the `as` family (after `*StructName` and `fn(...) -> R`). Views a raw `ptr` as a typed C element-pointer so subscript access scales by `sizeof(T)` automatically. Use this whenever the underlying storage is a `malloc`'d typed buffer and you'd otherwise be writing `mem.set_int(buf, 4*i, v)` boilerplate.

```aether
extern malloc(sz: long) -> ptr
extern free(p: ptr)

main() {
    raw = malloc(4 * 10)
    arr = raw as int[]         // ((int*)(raw))
    arr[3] = 42                 // ((int*)(raw))[3] = 42  → byte offset 12
    v = arr[3]                  // ((int*)(raw))[3]
    free(raw)
}
```

**What you get**

- C lowering: `arr = raw as int[]` emits `int* arr = ((int*)(raw));`. Subsequent `arr[i]` emits `arr[i]` which C scales by `sizeof(int) == 4`.
- Covered element types: `int`, `long`, `byte`, `ptr`, `float` (and struct types, though for those you usually want `*Struct` member-access instead).
- The `[]` syntax (no size) reads as "I don't know the bound, trust me to index inside it." A fixed-size `as int[10]` parses too but adds no extra checking; useful only as documentation.
- Like `as *StructName`, this is a **view**. It does NOT allocate, free, or bounds-check. The buffer's lifetime stays with whoever produced the original `ptr`.
- Aliasing: two views of the same memory alias each other. `arr = raw as int[]` and `arr_b = raw as byte[]` see the same bytes, that's the point.

**When to reach for it**

- Porting C code that uses `int *table = malloc(sizeof(int) * n); table[i] = ...` shapes. The C readability is preserved without the `mem.*` arithmetic.
- Hot loops where the offset arithmetic was hand-rolled and likely to have a one-off bug.

**When NOT to reach for it**

- For Aether-owned data; prefer fixed-size stack arrays (`int[5] arr`) or the higher-level collections in `std.collections`. The `as T[]` cast is the systems-programming escape hatch, not the everyday array.
- When you want bounds-checked storage. There is none here.

### `@c_struct` typed overlays, width-correct field access over a raw `ptr`

When you're porting C that overlays a struct on memory the **C side owns**
(`s->length--`, `max->ms = id->ms`), the raw way is hand-rolled offset math
through `std.mem`:

```aether
const ST_LENGTH = 8
mem.set_long(s, ST_LENGTH, mem.get_long(s, ST_LENGTH) - 1)   // s->length--
```

The footgun is the **accessor width**: picking `get_long` (8 bytes) for a
`uint32` field silently pulls the next field/padding. A `@c_struct` overlay
fixes that *structurally*, declare the layout once with explicit offsets, and
field access lowers to a width-correct `mem` accessor the **compiler** chooses:

```aether
@c_struct streamID {
    ms:  uint64 @0
    seq: uint64 @8
}

@c_struct stream {
    rax:         ptr      @0
    length:      uint64   @8
    slen:        uint32   @16     // 32-bit field, read/written at 4 bytes
    last_id:     streamID @24     // nested overlay
    max_deleted: streamID @40
}

s = robj_get_ptr(kv) as *stream   // reuse the `as *Name` cast, same syntax
s.length = s.length - 1           // mem_set_long at offset 8 (uint64 width)
s.slen   = 42                     // mem_set_uint32 at offset 16 (NOT get_long!)
s.max_deleted.ms = s.last_id.ms   // nested: offsets add (40+0, 24+0)
```

- **The width is derived from the field type**, `uint8`/`byte`→1, `uint16`→2,
  `uint32`/`int`→4, `int64`/`uint64`→8, `float64`→8, `ptr`→pointer-width. The
  `--audit-mem` class of bug is gone for overlay-declared structs: you
  never hand-pick the accessor.
- **The offsets stay explicit**, Aether can't see the C headers, so you probe
  them (as before). But they live in one declaration, not a scattered `const`
  block, and a `ptr` field can be re-cast (`s.rax as *otherStruct`) to chase
  links.
- **It's a pure-Aether lens, not a C struct.** No `extern struct`, no
  `#include`, no C type is emitted, the overlay lowers to `aether_mem_*` calls
  over a `void*`. The C side keeps owning the allocation; the overlay never
  allocates, frees, or bounds-checks. **No `import std.mem` needed**, the
  overlay *is* the access surface (the accessor prototypes are emitted for you).
- Access reuses the existing `expr as *Name` cast and `s.field` member syntax,
  nothing new to learn at the call site. Nested overlays add offsets along the
  chain (`s.last_id.seq` → 24+8).

Choose `@c_struct` when the C side owns the memory and you want by-name,
width-safe access; choose `extern struct` (below) only when a C header genuinely
owns the struct type and you're linking against it.

### Checking overlay offsets against the header, `@c_verify`

The offsets above are written by hand, and nothing so far has checked them. That
is the overlay's one remaining sharp edge: add a field to the C struct upstream
and every offset after it shifts, while the overlay keeps reading the old ones.
The types still line up, the program still runs, and it quietly reads the wrong
field. The same goes for the width, declaring `uint32` for a field the header
widened to `uint64` reads half of it.

When the owning header is in scope, `@c_verify` hands both checks to the C
compiler:

```aether
@c_struct shape @c_verify {
    rax: ptr @0
    length: uint64 @8
    slen: uint32 @16
}
```

```toml
[build]
cflags = "-include shape.h -I."
```

Codegen emits a `_Static_assert` per field, comparing the declared offset against
`offsetof` and the declared width against `sizeof` of the real member. A
mismatch is a build error naming the field:

```
@c_struct shape: field 'length' is declared at offset 16, the C header puts it elsewhere
```

- **Opt-in, because it needs the C type.** `offsetof` requires the struct to be
  complete at that point, so `@c_verify` only works when the header is
  force-included. Without it the overlay behaves exactly as before, the author
  asserts the offsets.
- **Free at runtime.** The assertions are checked and discarded by the C
  compiler; the emitted accessors are unchanged.
- The C field names have to match the Aether ones, since that is what
  `offsetof` is given.

### `extern struct` unions, `union { ... }` and nested `struct { ... }`

C structs that span an FFI boundary often contain unions. mquickjs's `JSPropDef` has six variants overlayed on the same 32-byte tail; every tagged-union C shape (Lua `TValue`, JSON value, ...) hits the same problem.

```aether
extern struct JSPropDef {
    def_type: int          //  0
    name: ptr              //  8
    u: union {             // 16
        f64: float        //   16, variant: JS_DEF_PROP_DOUBLE
        str: ptr          //   16, variant: JS_DEF_PROP_STRING
        class1: ptr       //   16, variant: JS_DEF_CLASS
        func: struct {
            length: byte  //   16, start of variant: JS_DEF_CFUNC
            magic: ptr    //   24
            cproto_name: ptr //  32
            func_name: ptr   //  40
        }
        getset: struct {
            magic: ptr     //   16, start of variant: JS_DEF_CGETSET
            cproto_name: ptr //  24
            get_func_name: ptr //  32
            set_func_name: ptr //  40
        }
    }
}
```

Access via dotted notation: `d.u.f64`, `d.u.func.length`, `d.u.getset.get_func_name`. Pointer-overlay access reads the same: `(raw as *JSPropDef).u.f64`.

**What you get**

- C lowering emits a real `union { ... }` (and nested `struct { ... }` where present), the same C body any hand-rolled FFI declaration would write.
- The compiler trusts the user's offsets; it does not synthesize the C layout from anything other than the literal field declarations.
- Heap-string ownership tracking (the `_heap_<name>` companion ints emitted for `string`-typed fields) does NOT descend into unions, use `ptr`-typed fields instead. Mixed string-and-union ownership is poorly defined in C; declining to support it keeps the boundary clean.

**Where it's not allowed**

- Pure Aether structs (without the `extern` annotation) reject `union { ... }` at parse time, Aether's normal value semantics aren't compatible with overlapping fields.
- `union` is now a reserved keyword and can't be used as an identifier name anywhere.

### Packed structs, `extern struct ... @packed`

A `@packed` attribute on an `extern struct` emits the C body with `__attribute__((packed))`, so the layout has **no inter-field padding** and no trailing alignment:

```aether
extern struct sdshdr16 @packed { len: uint16  alloc: uint16  flags: byte }
// sizeof == 5 (not 6); offsetof(flags) == 4
```

This is the shape a packed C header struct needs, e.g. the Redis `sdshdr8/16/32/64` headers, where the length/alloc/flags sit at fixed packed offsets immediately before the string data. `sizeof(StructName)` and `offsetof(StructName, field)` lower to C and therefore report the packed numbers, and a `*StructName` overlay on a raw buffer reads/writes the fields at their packed offsets.

- `@packed` governs only a struct body **Aether emits**. It is mutually exclusive with `@c_import` (whose layout, including packing, comes from the included C header); combining them is a parse error.
- Bit-width fields (`name: type : NN`) and a trailing flexible array still work under `@packed`, exactly as for a plain `extern struct`.

#### Recipe: header-before-the-pointer strings (the `sds` pattern)

Redis-style strings hand around a pointer to the *data* while the packed
header lives at negative offsets, `SDS_HDR(T, s)` is just `s -
sizeof(struct sdshdrT)`, and `s[-1]` is the flags byte that selects the
variant. Both halves compose from `@packed` + `std.mem`:

```aether
import std.mem

extern struct sdshdr8 @packed { len: uint8  alloc: uint8  flags: byte }

// One allocation: [header][data...]; hand around only `s`.
base = malloc(sizeof(sdshdr8) + cap)
*sdshdr8 h = base as *sdshdr8
h.len = 5
h.alloc = cap
h.flags = 1
s = mem.long_to_ptr(mem.ptr_to_long(base) + sizeof(sdshdr8))

// Given ONLY `s`: the s[-1] flags read selects the header variant...
flags = mem.get_uint8(s, 0 - 1)
// ...and the negative-offset overlay recovers the whole header.
*sdshdr8 hdr = mem.long_to_ptr(mem.ptr_to_long(s) - sizeof(sdshdr8)) as *sdshdr8
n = hdr.len
```

`mem.get_uint8(p, offset)` and friends accept negative offsets (they index
a `uint8_t*` with a signed int), and the recovered overlay aliases the same
memory, a write through `hdr.len` is visible through every other view.
End-to-end proof: `tests/regression/test_issue747_sds_floor.ae`.

### Aether-side string-builder return types

Every Aether-side primitive that builds a string returns an `AetherString*` a refcounted, length-aware header that is binary-safe (embedded NULs preserved via the stored length):

| Returns `AetherString*` (length-aware, binary-safe) |
|-----------------------------------------------------|
| `string_new`, `string_new_with_length`, `string_empty` |
| `string_from_int`, `string_from_long`, `string_from_float` |
| `string_format` |
| `string_concat`, `string_concat_wrapped` |
| `string_substring`, `string_to_upper`, `string_to_lower`, `string_trim` |

These all mint their result through `string_adopt_caps_buffer`, which sets the magic header and stores the byte length. Aether's `string` type is `void*`-shaped at the C level, and the runtime helpers (`str_data`, `str_len`) dispatch on the AetherString magic header, so `string.length(value)` on any of these results reads the stored length, not `strlen()`, and does not truncate at an embedded NUL.

`string_concat_wrapped` is retained as an explicit alias for `string_concat`: both return an `AetherString*` with the same contract. The truncation hazard only appears when a value has been reduced to a bare `char*` for example a string literal, or a `string` argument whose AetherString header was stripped by the auto-unwrap at an extern call boundary (see "Aether-emitted receivers" above). On such a bare pointer, `str_len` falls through to `strlen()` and truncates at the first embedded NUL.

> **Historical note:** `string_concat` originally returned a plain `char*`, so downstream `string.length(string.concat(a, b))` patterns truncated binary content at the first NUL. `string_concat` now returns an `AetherString*` (via `string_adopt_caps_buffer`), and `string_concat_wrapped` is kept as an alias for the same contract. Existing call sites don't need to change.

### Heap-string ownership across the FFI boundary

When an Aether function takes the result of a C extern `-> string` call and assigns it to a variable, the same heap-string-tracker machinery decides whether the buffer is freed on later reassignment:

- **Hardcoded heap-returning stdlib calls** (`string.concat`, `string.substring`, `string.{to_upper, to_lower, trim}`) and **string interpolation** are always treated as heap.
- **A user-defined Aether `-> string` function** is heap-returning iff the codegen's recursive structural-escape-analysis pass can prove every return statement yields a heap-string-expression (chain into another heap-returning function counts).
- **A C extern `-> string`** is NOT analysed structurally, the codegen has no body to walk. It's treated as **non-heap** by default (literal-shape contract). If your C extern returns malloc'd memory that the caller is expected to free, capture its result via `ptr` and free it explicitly:

```aether
// Heap-returning C extern, declare result as ptr, free explicitly.
extern my_strdup_raw(s: string) -> ptr

main() {
    p = my_strdup_raw("hello")
    defer free(p)   // explicit cleanup
    // ... use p ...
}
```

Why not auto-treat C externs as heap: the C side has no calling-convention discipline that distinguishes heap from literal returns. `getenv(3)`, `dlerror(3)`, and dozens of POSIX functions return *borrowed* `char*` that must NOT be freed. A blanket "C extern returning string = heap" rule would surface as silent free-of-borrowed-memory aborts. The conservative stance is correct.

If your C function genuinely matches Aether's heap-returning contract (always allocates, never returns a literal or borrow), wrap it in a small `.ae` shim that returns a heap-string-expr (e.g. `string.concat("", c_result)`) and the structural-analysis pass will recognise the shim as heap-returning automatically.

## Embedding Aether in C Applications

If you want to embed Aether actors in your existing C application (the reverse direction), see the [C Embedding Guide](c-embedding.md). This covers:

- Compiling Aether to C and linking with your application
- Using the Aether runtime API directly from C
- The `--emit-header` compiler flag for generating C headers

## C symbol namespace (libc collision avoidance)

Aether is free with its identifier space, you can define an Aether
function called `bind`, `listen`, `accept`, or `connect` without
upsetting the parser. At the C-codegen layer, however, those names
collide with libc's own `bind(2)`, `listen(2)`, etc., and the linker
silently picks the wrong symbol, your `bind` either never runs, or
runs when libc's was expected.

Mitigation: the codegen keeps a curated list of
libc / POSIX symbol names (see `compiler/codegen/codegen.c`'s
`is_c_reserved_word` for the full set, it covers C keywords, the
entire BSD/POSIX network sockets API, POSIX I/O, process control,
memory + dynamic linking, string + stdio, and time + env). Any
Aether function whose name appears in the list gets transparently
emitted with an `ae_` prefix in the C output:

| Aether-side spelling | Emitted C symbol |
|----------------------|------------------|
| `bind`               | `ae_bind`        |
| `listen`             | `ae_listen`      |
| `read`               | `ae_read`        |
| `write`              | `ae_write`       |
| `connect`            | `ae_connect`     |
| `socket`             | `ae_socket`      |
| `fork`               | `ae_fork`        |
| `malloc`             | `ae_malloc`      |

The renaming is **invisible to Aether source**, you call your
function `bind(...)` and the compiler does the right thing. The only
place you see the prefix is in the emitted C (useful if you're
debugging with `nm`, `objdump`, or a stack trace).

A renamed function is equally usable **as a value** — passing it across an
ABI boundary as a `ptr` handler reaches the renamed definition, not a
same-named libc symbol:

```aether
_h_health(req: ptr, res: ptr, ud: ptr) -> int { return 0 }
server_get(raw, "/health", _h_health, 0)   // references ae_h_health
```

(Before #1598 only *calls* were rewritten, so a value reference kept the
original spelling and either failed to compile or — for an
extern-collision rename — silently bound to the libc symbol.)

One sharp edge worth knowing: a **local variable may shadow a top-level
function's name**, and such a local is emitted verbatim rather than
renamed. That works on its own, but a single function that *both* shadows
the name and passes the function as a value cannot work in either
spelling — in the emitted C the value reference would precede the local's
declaration. Pick one meaning per scope; the trailing-underscore
file-local convention (`h_health_`) is the idiomatic way to keep them
apart.

### Collisions with the Aether standard library

The curated list above covers libc, but `libaether.a` exports a couple of
thousand bare C symbols of its own (`string_*`, `list_*`, `map_*`, …), and it
grows with every release. A top-level function in your entry file called
`string_replace_all` used to emit a bare C symbol of that name and collide with
`libaether.a` the moment `std.string` gained a function that lowered to the same
symbol, breaking a build that had worked for months with no change to your code
(#1366).

Two things prevent that now, and neither is a name list that has to be kept up
to date:

- **Top-level functions in an executable build get internal linkage.** The
  generated C for an executable is a single translation unit, so nothing
  outside it can legitimately reference those functions by name, and `static`
  means the linker never compares them against `libaether.a` at all.
- **A function whose name matches an extern the same file declares is renamed**
  to the `ae_` spelling above. Importing `std.string` puts a
  `string_replace_all` prototype in your translation unit, so your own function
  of that name is emitted as `ae_string_replace_all`. Calls follow the rename;
  `string.replace_all(...)` still reaches the standard library.

Both are invisible to Aether source. `--emit=lib`, `--emit=both` and
`--emit=csrc` are excluded from the linkage change, because there the bare
symbols of functions the flattened ABI cannot express (builders, `fn`-typed
parameters) are a documented part of what consumers link against.

### When to use `@c_callback` instead

If your Aether function specifically needs a **stable, unprefixed C
symbol**, e.g. it's going to be looked up via `dlsym()` from a host
program, or passed as a function pointer to a C library, annotate
it with `@c_callback("desired_c_name")`. That bypasses the prefix
logic and emits exactly the symbol you ask for. It is also the opt-out
from the internal-linkage rule above: a `@c_callback` function keeps
external linkage in every emit mode, so a C file you pass with
`--extra` (or `extra_sources` in `aether.toml`) can call it by name.

### Aether-keyword collisions

A handful of libc names also happen to be Aether keywords (`send` is
reserved for actor messaging). Those are caught one layer earlier by
the parser with `error[E0100]` the codegen mitigation never sees
them.

## See Also

- [Getting Started](getting-started.md)
- [C Embedding Guide](c-embedding.md) - Embed Aether actors in C applications
- [Standard Library Reference](stdlib-reference.md)
- [Language Reference](language-reference.md)
