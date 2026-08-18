# Named Arguments and select()

## Named Arguments

Function calls can use named arguments with colon syntax, consistent
with Aether's parameter definition style:

```aether,fragment
// Definition
greet(name: string, count: int) {
    for (i = 0; i < count; i++) { println(name) }
}

// Call with named args
greet(name: "alice", count: 3)

// Positional still works
greet("alice", 3)

// Mixed: positional first, named after
greet("alice", count: 3)
```

Named arguments are documentation at the call site, they make code
more readable but don't change semantics. The C compiler receives
positional arguments regardless.

### Rules

- Names use colon: `name: value` (not `name = value`)
- Positional and named can be mixed in the same call
- Names are not validated against parameter names (they're comments)
- The colon syntax is consistent with parameter definitions: `func(name: type)`

## List Literals

Array literals with square bracket syntax:

```aether,fragment
nums = [10, 20, 30]
println(nums[0])         // 10

names = ["alice", "bob", "charlie"]
println(names[1])        // bob

// Expressions in array literals
a = 5
computed = [a, a * 2, a * 3]     // [5, 10, 15]

// Empty array
empty = []

// Single element
one = [42]
```

Arrays are fixed-size C arrays. Element access uses `[]` subscript.

## select(), Platform Conditional

Compile-time platform selection using named arguments:

```aether,fragment
port = select(linux: 8080, windows: 80, macos: 8080)
timeout = select(windows: 5000, other: 3000)
flags = select(linux: "-lpthread", windows: "-lws2_32", macos: "-framework Security")
```

### How it works

`select()` emits a C `#ifdef` chain at compile time. Only the matching
platform's value is compiled into the binary:

```c
// select(linux: 8080, windows: 80, macos: 8080) compiles to:
#ifdef _WIN32
80
#elif defined(__APPLE__)
8080
#else
8080
#endif
```

### Platform names

| Name | C condition | Matches |
|------|-----------|---------|
| `linux:` | `#else` (default) | Linux, FreeBSD, other POSIX |
| `windows:` | `#ifdef _WIN32` | Windows, MinGW, MSYS2 |
| `macos:` | `#elif defined(__APPLE__)` | macOS, iOS |
| `other:` | fallback | Any platform not explicitly listed |

### Fallback with `other:`

`other:` provides a default value for platforms not explicitly named:

```aether,fragment
// Only need to specify the exception
separator = select(windows: "\\", other: "/")
```

### Missing platform is a compile error

If a platform is missing and there's no `other:` fallback, the Aether
compiler emits an error and the C compiler emits `#error`:

```aether,fragment
// ERROR: no value for windows or macos, no other: fallback
port = select(linux: 8080)
```

This prevents silent bugs where a platform gets `0` or empty string.
Either list all platforms or provide `other:`.

### Use cases

**Build system flags:**
```aether,fragment
linker_flags = select(
    linux: "-lpthread -lrt",
    windows: "-lws2_32",
    macos: "-framework Security"
)
```

**Default paths:**
```aether,fragment
config_dir = select(
    linux: "/etc/myapp",
    macos: "/Library/Application Support/myapp",
    windows: "C:\\ProgramData\\myapp"
)
```

**Feature availability:**
```aether,fragment
max_threads = select(linux: 16, windows: 8, macos: 12)
has_io_uring = select(linux: 1, other: 0)
```

### Printing string results

String values from `select()` store correctly as `const char*`, but type
inference doesn't yet propagate through the call site. Both a direct
`println(os)` and string interpolation currently print the pointer as an
integer, because the result is treated as an `int`:

```aether,fragment
os = select(linux: "Linux", windows: "Windows", macos: "macOS")
println(os)               // BROKEN: prints a garbage integer, not the string
println("os: ${os}")      // BROKEN: emits printf(..., %d) for the const char*
```

Until this is fixed, either inline the `select()` directly in the
`println()` call, or return it from a `-> string` function and print that
result:

```aether,fragment
// Works: inline, where the string type is known at the call site
println(select(linux: "Linux", windows: "Windows", macos: "macOS"))

// Works: a -> string function fixes the type before printing
os_name() -> string {
    return select(linux: "Linux", windows: "Windows", macos: "macOS")
}
main() {
    os = os_name()
    println(os)               // macOS
    println("os: ${os}")      // os: macOS
}
```

Tracked on the issue backlog as type-inference propagation through
`select()`.
