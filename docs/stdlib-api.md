# Aether Standard Library Guide

## Overview

Aether provides a standard library for strings, I/O, math, file system, networking, and actor concurrency. The library is automatically linked when you compile Aether programs.

> **Go-style result types.** Every stdlib function that can fail returns a `(value, err)` tuple. Check `err` first, then use `value`. The raw C-style externs are preserved under a `_raw` suffix for advanced callers who need direct access to the underlying pointer or int:
>
> ```aether
> body, err = http.get("http://example.com")
> if err != "" {
>     println("failed: ${err}")
>     return
> }
> println(body)
> ```
>
> See the [error handling example](../examples/basics/error-handling.ae) for the user-function pattern, and `examples/stdlib/http-client.ae` for the stdlib pattern.

## Namespace Calling Convention

Functions are called using **namespace-style syntax**: `namespace.function()`

| Import | Namespace | Example Call |
|--------|-----------|--------------|
| `import std.string` | `string` | `string.new("hello")`, `string.release(s)` |
| `import std.file` | `file` | `file.exists("path")`, `file.open("path", "r")` |
| `import std.dir` | `dir` | `dir.exists("path")`, `dir.create("path")` |
| `import std.path` | `path` | `path.join("a", "b")`, `path.dirname("/a/b")` |
| `import std.tar` | `tar` | `tar.reader_open("src.tar")`, `tar.extract("src.tar", "out", opts)` |
| `import std.json` | `json` | `json.parse(str)`, `json.create_object()` |
| `import std.cryptography` | `cryptography` | `cryptography.sha256_hex(data, n)`, `cryptography.hash_hex(algo, data, n)` |
| `import std.http` | `http` | `http.get(url)`, `http.server_create(port)` |
| `import std.tcp` | `tcp` | `tcp.connect(host, port)`, `tcp.write(sock, data)` |
| `import std.list` | `list` | `list.new()`, `list.add(l, item)` |
| `import std.map` | `map` | `map.new()`, `map.put(m, key, val)` |
| `import std.set` | `set` | `set.new()`, `set.add(s, item)` |
| `import std.pqueue` | `pqueue` | `pqueue.new()`, `pqueue.push(q, pri, item)` |
| `import std.math` | `math` | `math.sqrt(x)`, `math.sin(x)` |
| `import std.log` | `log` | `log.init(file, level)`, `log.write(level, msg)` |
| `import std.io` | `io` | `io.print(str)`, `io.read_file(path)`, `io.getenv(name)` |
| `import std.os` | `os` | `os.system(cmd)`, `os.exec(cmd)`, `os.getenv(name)`, `os.argv0()`, `os_execv(prog, argv)` |

---

## Using the Standard Library

Import modules with the `import` statement:

```aether
import std.string       // String functions
import std.file         // File operations
import std.dir          // Directory operations
import std.json         // JSON parsing
import std.http         // HTTP client & server
import std.tcp          // TCP sockets
import std.list         // ArrayList
import std.map          // HashMap
import std.set          // Unique-string set
import std.pqueue       // Priority queue
import std.math         // Math functions
import std.log          // Logging
import std.io           // Console I/O, environment variables
import std.os           // Shell execution, environment variables
```

Call functions using namespace syntax:

```aether
import std.string
import std.file

main() {
    // String operations
    s = string.new("hello")
    len = string.length(s)
    string.release(s)

    // File operations
    if (file.exists("data.txt") == 1) {
        size = file.size("data.txt")
    }
}
```

Or use `extern` for direct C bindings:

```aether
extern my_c_function(x: int) -> ptr
```

---

## String Library

### Types

```c
typedef struct AetherString {
    unsigned int magic;    // Always 0xAE57C0DE, enables runtime type detection
    int ref_count;
    size_t length;
    size_t capacity;
    char* data;
} AetherString;
```

> **Note:** All `std.string` functions accept both plain `char*` strings and managed `AetherString*` transparently. The `magic` field is used internally to distinguish between the two at runtime.

### Available Functions

#### Creating Strings

- `string_new(const char* cstr)` - Create from C string
- `string_from_literal(const char* cstr)` - Alias for new
- `string_empty()` - Create empty string
- `string_new_with_length(const char* data, size_t len)` - Create with explicit length

#### String Operations

- `string_concat(AetherString* a, AetherString* b)` - Concatenate strings
- `string_length(AetherString* str)` - Get length
- `string_char_at(AetherString* str, int index)` - Get character
- `string_equals(AetherString* a, AetherString* b)` - Check equality
- `string_compare(AetherString* a, AetherString* b)` - Compare (-1, 0, 1)

#### String Methods

- `string_starts_with()` - Check prefix
- `string_ends_with()` - Check suffix
- `string_contains()` - Search for substring
- `string_index_of()` - Find position
- `string_substring()` - Extract substring
- `string_to_upper()` - Convert to uppercase
- `string_to_lower()` - Convert to lowercase
- `string_trim()` - Remove whitespace
- `string.join(seq, sep)` - Concatenate a `*StringSeq`'s elements with `sep` between adjacent pairs. Linear cost (two passes, one exact-size allocation), the complement of `string.split_to_seq`. For building a string piece by piece instead, use `std.strbuilder`; both avoid the O(n²) self-append trap described in the [Standard Library Reference](stdlib-reference.md#building-strings-incrementally-avoiding-quadratic-cost).

#### Conversion

- `string.to_cstr(str)` - Get C string pointer
- `string.from_int(value)` - Convert int to string
- `string.from_float(value)` - Convert float to string
- `string.from_double(value)` - Convert float (binary64) to lossless decimal string (round-trip safe)

#### Parsing (Go-style)

All parsers return `(value, err)` tuples. Empty `err` means success.

```aether
n, err = string.to_int("42")
if err != "" { println("bad: ${err}"); return }
println(n)
```

- `string.to_int(s)` → `(int, string)` - Parse base-10 integer
- `string.to_long(s)` → `(long, string)` - Parse 64-bit integer
- `string.to_float(s)` → `(float, string)` - Parse float
- `string.to_double(s)` → `(float, string)` - Parse double

Raw out-parameter externs are preserved as `string_to_int_raw`, `string_to_long_raw`, `string_to_float_raw`, `string_to_double_raw` for code that needs to distinguish zero from parse failure without a tuple.

#### Memory Management

- `string.new(cstr)` - Allocate a new string (use `string.free` when done)
- `string.free(str)` - Free the string

Use `defer string.free(s)` right after `string.new()` to ensure cleanup at scope exit.

The underlying C implementation also exposes `string.retain()` / `string.release()` for advanced use cases (e.g., sharing ownership across C callbacks), but Aether programs should use `string.free()` directly.

---

## File System Library

Complete filesystem library with file and directory operations.

### Usage

```aether
import std.file
import std.dir

main() {
    // Read a file in one call (opens, reads, closes)
    content, err = file.read("data.txt")
    if err != "" {
        println("cannot read: ${err}")
        return
    }
    println(content)

    // Write a file
    werr = file.write("output.txt", "hello")
    if werr != "" {
        println("cannot write: ${werr}")
        return
    }

    // Get size
    size, serr = file.size("data.txt")
    if serr == "" {
        println("size: ${size} bytes")
    }

    // Create a directory
    derr = dir.create("output")
    if derr != "" {
        println("cannot mkdir: ${derr}")
    }
}
```

### File Operations (Go-style)

- `file.read(path)` → `(string, string)` - Read entire file (opens, reads, closes). Handles `/proc`/`/sys` seq-files and unseekable fds via a read-to-EOF fallback (a size-0 `ftell` no longer silently returns `""`).
- `file.write(path, content)` → `string` - Write content, return error string
- `file.open(path, mode)` → `(ptr, string)` - Low-level open (caller must `file.close`)
- `file.close(handle)` - Close a file handle
- `file.size(path)` → `(int, string)` - Get file size in bytes
- `fs.statvfs(path)` → `(total, free, avail, err)` - Filesystem byte counts (POSIX `statvfs`) for the fs containing `path`. `avail` is space usable by a non-root process (`f_bavail`), use it for "how much can I actually write" (`end = avail / file_size`). Portable Linux/macOS/BSD; Windows returns the error branch.
- `fs.mounts()` → `(count, err)` - Load the mount table: Linux `/proc/self/mountinfo` (octal escapes decoded), macOS/BSD `getmntinfo(3)`, Windows drive letters. Read entries with `fs.mount_source(i)` / `fs.mount_point(i)` / `fs.mount_fstype(i)` / `fs.mount_options(i)` (strings borrowed from a thread-local table, valid until the next `mounts()` or `fs.fs_release_mounts()`). Out-of-range indexes return `""`.
- `fs.block_info(dev)` → `(size_bytes, removable, transport, err)` - Block-device facts, Linux sysfs backend. `dev` accepts `/dev/sda`, `sda`, or a partition (`nvme0n1p2`; the removable flag resolves through the parent disk). `removable` is 1/0 or -1 when unknown; `transport` is `usb`/`nvme`/`sata`/`virtio`/`mmc` or `""`. Non-Linux platforms return the error branch, never a fabricated answer.
- `file.delete(path)` → `string` - Delete a file, return error string
- `file.exists(path)` → `int` - 1 if exists, 0 otherwise (infallible predicate)

Raw externs: `file_open_raw`, `file_read_all_raw`, `file_write_raw`, `file_delete_raw`, `file_size_raw`.

### Directory Operations (Go-style)

- `dir.create(path)` → `string` - Create directory, return error string
- `dir.delete(path)` → `string` - Delete empty directory, return error string
- `dir.list(path)` → `(ptr, string)` - List contents (caller must `dir.list_free`)
- `dir.exists(path)` → `int` - 1 if exists, 0 otherwise
- `dir.list_count(list)` / `dir.list_get(list, i)` / `dir.list_free(list)` - DirList accessors

Raw externs: `dir_create_raw`, `dir_delete_raw`, `dir_list_raw`.

### Path Utilities

- `path.join(path1, path2)` - Join two path components
- `path.dirname(path)` - Get directory name
- `path.basename(path)` - Get file name
- `path.extension(path)` - Get file extension
- `path.is_absolute(path)` - Check if path is absolute

---

## I/O Library

### Console Output

The primary I/O functions in Aether are `print()` and `println()`:

```aether
print("Hello, World!\n")
println("Hello, World!")       // same, with automatic newline
println("Value: ${x}")         // string interpolation
println("Float: ${pi}")
```

### Console Output (infallible)

- `io.print(str)` - Print string
- `io.print_line(str)` - Print string with newline
- `io.print_int(value)` - Print integer
- `io.print_float(value)` - Print float

### File I/O (Go-style)

All operations that can fail return an error string ("" on success).

```aether
content, err = io.read_file("data.txt")
if err != "" { println("failed: ${err}"); return }

werr = io.write_file("output.txt", "hello")
```

- `io.read_file(path)` → `(string, string)` - Read entire file
- `io.write_file(path, content)` → `string` - Write (overwrites)
- `io.append_file(path, content)` → `string` - Append to file
- `io.delete_file(path)` → `string` - Delete file
- `io.file_info(path)` → `(ptr, string)` - Get file metadata (caller must `io.file_info_free`)
- `io.file_exists(path)` → `int` - 1 if exists, 0 otherwise

### Environment variables

- `io.getenv(name)` → `string` - Returns the value, or null if unset (infallible)
- `io.setenv(name, value)` → `string` - Set env var, return error string
- `io.unsetenv(name)` → `string` - Unset env var, return error string

Raw externs: `io_read_file_raw`, `io_write_file_raw`, `io_append_file_raw`, `io_delete_file_raw`, `io_file_info_raw`, `io_setenv_raw`, `io_unsetenv_raw`.

---

## OS / Process Library

### Shell execution

- `os.system(cmd)` → `int` Run a shell command, return exit code
- `os.exec(cmd)` → `(string, string)` Run a command and capture stdout; returns `(output, err)` tuple
- `os.getenv(name)` → `string` Read environment variable; returns null if unset

The shell-execution path passes `cmd` to `/bin/sh -c` (`cmd.exe /c` on Windows), which means quoting, glob expansion, and `$VAR` interpolation all happen before the child sees the string. **Prefer `os.run_capture` (below)** for any input that touches user data, it skips the shell entirely and is binary-safe.

### Process spawn (argv-based, no shell)

The argv-based path is the recommended way to launch a child binary. Argv is passed as a list of strings, no shell sits in the middle, paths with spaces / quotes / `$`-signs are safe, and there's no command-injection surface for user-provided values.

- `os.run(prog, argv, env)` → `int` Spawn `prog`, wait for it to finish, return the exit code. `argv` is a `list<ptr>` of C strings (element 0 is conventionally the program name; the OS sees this as `argv[0]` for the child). `env` is the same shape, or `null` to inherit. `prog` is looked up on `PATH` if it does not contain a slash.

- `os.run_capture(prog, argv, env)` → `(stdout: string, exit_code: int, stderr: string)` Same as `os.run`, but captures the child's stdout and stderr. The child's three outputs come back as a single tuple. The `exit_code` slot lets callers distinguish "ran cleanly" (`exit_code == 0`) from "ran but exited non-zero", important for tools like `diff3 -m` (returns 1 on conflicts), `grep` (returns 1 on no-match), or `gcc` (returns non-zero on compile errors), where non-zero is meaningful information rather than a hard failure.

Raw externs (rarely needed directly; the wrappers above are idiomatic):

- `os_run(prog, argv, env)` → `int` Same as `os.run`.
- `os_run_capture_raw(prog, argv, env)` → `string` Captures stdout only; exit code is discarded. Use `os.run_capture` instead unless the exit code is genuinely irrelevant.
- `os_run_capture_status_raw(prog, argv, env)` → `(string, int, string)` The tuple-returning extern that `os.run_capture` wraps.

Worked example:

```aether
import std.os
import std.list

main() {
    argv = list.new()
    _ = list.add(argv, "git")
    _ = list.add(argv, "rev-parse")
    _ = list.add(argv, "HEAD")

    stdout, exit_code, stderr = os.run_capture("git", argv, null)
    if exit_code != 0 {
        println("git failed (${exit_code}): ${stderr}")
        return
    }
    println("HEAD is ${stdout}")
}
```

**Capabilities and Windows portability:** `os.run` / `os.run_capture` require the `--with=os` capability when building with `--emit=lib` (capability-empty by default). Windows MSYS2 / mingw-w64 currently uses POSIX-fallback shims; a native `CreateProcessW` backend is tracked separately. The function signatures and return shapes are stable across the current Windows fallback and the future native backend.

**Synchronous, no streaming.** The current API blocks until the child exits and returns the entire captured stdout / stderr. There is no streaming-stdin or incremental-stdout-read path today; if you need to feed input to a child, write your input to a temp file and have the child read from there, or pipe through a shell wrapper.

### Argv discovery

- `aether_args_count()` → `int` Number of command-line arguments
- `aether_args_get(index)` → `string` Get the i-th argument; returns null if out of range
- `aether_argv0()` → `string` Path the OS launched the current process with (argv[0]); returns null before `aether_args_init` has run
- `os.argv0()` → `string` Convenience wrapper around `aether_argv0()` that returns `""` instead of null
- `os.args_count()` → `int` Ergonomic alias for `aether_args_count()`
- `os.args_get(index)` → `string` Ergonomic wrapper: owned copy of argv[index], `""` when out of range / sealed (never null)

All four raw `aether_args_*` names also resolve qualified (`os.aether_args_count()`, the form the language reference shows).

Typical use: a tool that needs to find its own binary (to locate sibling helpers next to itself, re-exec with different flags, or print a self-path in a diagnostic) can call `os.argv0()` and skip the argv-index bookkeeping.

### Process replacement

- `os_execv(prog, argv_list)` → `int` Replace the current process image with `prog`, passing an explicit argv list. `argv_list` is a `list<ptr>` of C strings (element 0 is argv[0] for the new program). On success this call **never returns**; on failure it returns `-1` and the current process keeps running. `prog` is looked up on `PATH` if it does not contain a slash. Not available on Windows, returns `-1`.

Paired with `os.run` / `os.run_capture` (see [Process spawn](#process-spawn-argv-based-no-shell) above), this gives Aether programs a full argv-based process-launch surface with no shell in the middle, so paths with spaces, quotes, or `$`-signs are safe. Stdio is flushed before the exec, so pre-exec diagnostics are not lost.

Example:

```aether
import std.os
import std.list

main() {
    argv = list.new()
    _e1 = list.add(argv, "echo")
    _e2 = list.add(argv, "from")
    _e3 = list.add(argv, os.argv0())
    rc = os_execv("/bin/echo", argv)
    // Only reached if exec failed.
    println("exec failed: ${rc}")
    exit(rc)
}
```

---

## Math Library

### Basic Operations

- `math.abs_int(x)` - Absolute value (int)
- `math.abs_float(x)` - Absolute value (float)
- `math.min_int(a, b)` - Minimum (int)
- `math.max_int(a, b)` - Maximum (int)
- `math.min_float(a, b)` - Minimum (float)
- `math.max_float(a, b)` - Maximum (float)
- `math.clamp_int(x, min, max)` - Clamp value to range
- `math.clamp_float(x, min, max)` - Clamp value to range

### Advanced Math

- `math.sqrt(x)` - Square root
- `math.pow(base, exp)` - Power
- `math.sin(x)` - Sine
- `math.cos(x)` - Cosine
- `math.tan(x)` - Tangent
- `math.asin(x)` - Arc sine
- `math.acos(x)` - Arc cosine
- `math.atan(x)` - Arc tangent
- `math.atan2(y, x)` - Two-argument arc tangent
- `math.floor(x)` - Floor
- `math.ceil(x)` - Ceiling
- `math.round(x)` - Round to nearest
- `math.log(x)` - Natural logarithm
- `math.log10(x)` - Base-10 logarithm
- `math.exp(x)` - Exponential

### Random Numbers

- `math.random_seed(seed)` - Set random seed
- `math.random_int(min, max)` - Random int in range [min, max]
- `math.random_float()` - Random float in [0.0, 1.0)

---

## JSON Library

### Parsing and Serialization

```aether
import std.json

main() {
    // Parse, Go-style (value, err) tuple.
    v, err = json.parse("{\"name\":\"Aether\",\"count\":42}")
    if err != "" {
        println("parse failed: ${err}")
        return
    }

    // Typed readers are infallible (sentinel 0/0.0 on wrong type).
    name_val, _ = json.object_get(v, "name")
    name, _ = json.get_string(name_val)
    count_val, _ = json.object_get(v, "count")
    count = json.get_int(count_val)
    println("${name} / ${count}")

    // Build values, each create_* allocates a standalone value. Passing
    // it to object_set / array_add transfers ownership to the container.
    obj = json.create_object()
    _ = json.object_set(obj, "x", json.create_number(1.5))
    _ = json.object_set(obj, "flag", json.create_bool(1))

    arr = json.create_array()
    _ = json.array_add(arr, json.create_number(10.0))
    _ = json.array_add(arr, json.create_number(20.0))

    out, _ = json.stringify(obj)
    println(out)

    json.free(v)
    json.free(obj)
    json.free(arr)
}
```

### JSON Functions

Fallible calls return Go-style tuples. The typed readers (`get_bool` /
`get_number` / `get_int`) return sentinels (0, 0.0) on wrong-type input
so they stay infallible.

- `json.parse(str)` → `(ptr, string)` parse into a tree. Error is a
  position-qualified message like `"expected ':' at 3:17"`.
- `json.stringify(value)` → `(string, string)` `(output, err)`.
- `json.free(value)` release the value. Safe on both parsed roots
  (frees the arena) and standalone-created values.
- `json.get_string(value)` → `(string, string)` `(text, err)`; errors
  if `value` is not a `JSON_STRING`.
- `json.object_get(obj, key)` → `(ptr, string)` `(child, err)`.
  Absent key returns `(null, "")`, which is distinct from the error
  case `(null, "not an object")`.
- `json.object_set(obj, key, value)` → `string` error string or `""`.
- `json.array_get(arr, index)` → `(ptr, string)` same shape;
  out-of-range returns `(null, "")`.
- `json.array_add(arr, value)` → `string` error string or `""`.

Infallible externs (no tuple):

- `json.type(value)` → `int` returns one of the `JSON_*` constants.
- `json.is_null(value)` → `int`.
- `json.get_bool(value)` → `int` 0 on wrong type.
- `json.get_number(value)` → `float` 0.0 on wrong type (lossy past 2^53).
- `json.get_int(value)` → `int`, clamps to `+/-2147483647` on overflow.
- `json.get_long(value)` → `long`, the full int64 value exactly. Use this for
  large integer IDs / byte-counts; `get_int` clamps and `get_number` is lossy.
- `json.object_has(obj, key)` → `int`.
- `json.array_size(arr)` → `int`.
- `json.create_null()`, `json.create_bool(v)`, `json.create_number(v)`,
  `json.create_string(s)`, `json.create_array()`, `json.create_object()`
  → `ptr` allocate standalone values.
- `json.last_error()` → `string` the last parser error on the current
  thread; redundant with `json.parse`'s tuple but useful when calling
  the raw extern directly.

Raw externs (bypass the Go-style wrappers): `json_parse_raw`,
`json_parse_raw_n` (length-taking, for non-null-terminated input),
`json_stringify_raw`, `json_get_string_raw`, `json_object_get_raw`,
`json_object_set_raw`, `json_array_get_raw`, `json_array_add_raw`.
All documented in [stdlib-module-pattern.md](stdlib-module-pattern.md).

### JSON Type Constants

- `JSON_NULL` = 0
- `JSON_BOOL` = 1
- `JSON_NUMBER` = 2
- `JSON_STRING` = 3
- `JSON_ARRAY` = 4
- `JSON_OBJECT` = 5

### What `std.json` doesn't do

Coming from Go's `json.Unmarshal`, Java's Jackson, Python's `json.load` + dataclasses, or C#'s `JsonSerializer`, expect to do more by hand:

- No struct ↔ JSON mapping. Aether has no runtime reflection (no `instanceof`, no `T.GetType()`, no `reflect.TypeOf`), so a library function that takes a struct type and a JSON tree and populates the fields can't exist as a stdlib API. Callers walk the tree by hand: `json.object_get(v, "name")` then `json.get_string(...)`, repeated per field. For tree-shaped or dynamically-shaped JSON the code looks similar to other languages; for struct-shaped JSON it's more verbose. A future codegen step (a `--derive-json` flag on struct definitions, or a build-step macro) could close this gap without runtime reflection, but isn't shipped today.
- No annotations or struct tags. `@JsonProperty("user_name")`, Go struct tags `json:"user_name,omitempty"`, and the like have nothing to attach to without struct-mapping in the first place.
- No streaming parse. The whole document is buffered into the arena before the tree is walkable. Documents into the tens of MB are fine; for multi-gigabyte JSON, use a different tool.
- Strict RFC 8259 only: no JSON5, comments, or trailing commas.
- Compact output only on stringify; wrap with a separate prettier if you need one.
- Declarative validation lives in `std.schema` (typed records, composable
  validators, `parse -> (values, errors)`, and `to_json_schema()` to emit a
  draft-07 JSON Schema). It does not itself *consume* external JSON Schema
  documents — validate against a `std.schema` record, or build that on top.
- No arbitrary-precision numbers. Numbers are `int` or `double`; the parser falls through to `strtod` for correctly-rounded IEEE-754 on edge cases (16+ significant digits, huge exponents), but there's no `BigDecimal` / `decimal.Decimal` equivalent for financial precision.
- Hard-coded depth limit of 256, as DoS protection against deeply nested JSON bombs. Not configurable; rare to hit in practice.

### Other structured-data formats

Beyond JSON, XML is the other format with a stdlib module: `std.xml` provides a pull/SAX reader (`xml.parser`, `xml.next`, `xml.name`, `xml.text`, `xml.attr`, driven by the `EVENT_START`/`END`/`TEXT`/`EOF`/`ERROR` constants) and an escaping element builder (`xml.writer`, `xml.start`, `xml.element`, `xml.finish`). Enough for S3 / SOAP-ish / config XML; not in scope are XSD, XPath, namespaces, and DTD validation. There is no DOM tree, so you drive events yourself. The rest have no stdlib parser or codec:

- YAML, INI, and Java-style `.properties` have no parser. INI and `.properties` are trivial to build on `string.split`; for YAML, the runtime is single-language, so Aether projects configure via TOML or hand-rolled formats.
- TOML has a parser at `tools/apkg/toml_parser.c`, used internally by the `ae` CLI to read `aether.toml` project files. It isn't exposed as `std.toml`. A project needing TOML can copy that parser or shell out to a host-language tool.
- CSV has no parser. `string.split(line, ",")` covers the no-quoting / no-embedded-commas case; anything more needs a real CSV parser, which isn't shipped.
- Protocol Buffers, MessagePack, CBOR, Avro, and Thrift have no codecs. Same reflection-gap reasoning as struct ↔ JSON: without struct introspection there's no automatic encode/decode, so a hand-written codec on top of `tcp.write` / `tcp.read` / `aether_string_data` is what you'd build.

These are absent because no downstream user has driven the need yet. If you're starting a project that needs YAML config, expect to write a parser, ship a contrib module, or shell out. The structured-data thinking in the stdlib is currently JSON-shaped and HTTP-adjacent; broader format coverage is open territory.

---

## Cryptography Library

Hash digests, built on OpenSSL's EVP API. When OpenSSL isn't linked, every
wrapper returns `("", "openssl unavailable")` rather than crashing. Base64 is
next door in `std.encoding`.

```aether
import std.cryptography
import std.encoding

main() {
    digest, _ = cryptography.sha256_hex("abc", 3)
    b64     = encoding.base64_encode("\x01\x02\x03", 3)
    raw, _  = encoding.base64_decode(b64)
}
```

### Hash Functions

- `cryptography.sha1_hex(data, length)` → `(string, string)` - 40-char lowercase hex digest. Legacy interop (Git, Subversion); prefer SHA-256 for new work.
- `cryptography.sha256_hex(data, length)` → `(string, string)` - 64-char lowercase hex digest.
- `cryptography.hash_hex(algo, data, length)` → `(string, string)` - Algorithm-by-name dispatcher. `algo` is `"sha1"`, `"sha256"`, or any name `EVP_get_digestbyname()` recognizes (`"sha384"`, `"sha512"`, `"sha3-256"`, ...). Returns `("", "unknown algorithm")` for unrecognized names.
- `cryptography.hash_supported(algo)` → `int` - `1` if this build can compute `algo`, `0` otherwise. Always succeeds. Use to validate user-supplied algorithm names before calling `hash_hex`.
- `cryptography.md4_hex(data, length)` / `md5_hex(data, length)` → `(string, string)` - 32-char hex digest. Legacy interop only (Content-MD5, ETag, zsync), NOT collision-resistant.
- `cryptography.sha1_bytes(data, length)` / `sha256_bytes(data, length)` / `md4_bytes(data, length)` / `md5_bytes(data, length)` → `(string, int, string)` - Raw-bytes digest, `(bytes, length, err)`; `bytes` preserves embedded NULs.
- `cryptography.hash_bytes(algo, data, length)` → `(string, int, string)` - Algorithm-by-name binary digest.

`length` is explicit so binary payloads with embedded NULs survive. `data` may be either a plain string literal or an AetherString from `fs.read_binary` the runtime unwraps automatically.

### HMAC, Random, and Streaming Digests

- `cryptography.hmac_sha256_hex(key, key_len, msg, msg_len)` → `(string, string)` - HMAC-SHA256 hex digest (RFC 2104 / FIPS 198-1).
- `cryptography.hmac_sha256_bytes(key, key_len, msg, msg_len)` → `(string, int, string)` - Raw 32-byte HMAC for chained key derivation (SigV4 / HKDF-shaped flows).
- `cryptography.random_bytes(n)` → `(string, int, string)` - `n` CSPRNG bytes from the OS (`getrandom` / `arc4random_buf`).
- `cryptography.random_hex(n)` → `(string, string)` - `n` random bytes as `2*n` hex chars.
- `cryptography.random_base64(n)` → `(string, string)` - `n` random bytes as unpadded Base64.
- `cryptography.digest_new(algo)` → `(ptr, string)` - Open a streaming digest context (`"md5"`, `"sha256"`, ...).
- `cryptography.digest_update(ctx, data, length)` → `(int, string)` - Feed bytes incrementally; does not free `ctx`.
- `cryptography.digest_final_hex(ctx)` → `(string, string)` / `digest_final_bytes(ctx)` → `(string, int, string)` - Finalize; both FREE `ctx`.
- `cryptography.digest_free(ctx)` - Abandon a context without finalizing (NULL-safe).

### Base64 (RFC 4648 §4 standard alphabet)

Base64 lives in `std.encoding`, not here; `std.cryptography` keeps only
`random_base64`, which is crypto-random bytes rendered as base64.

- `encoding.base64_encode(data, length)` → `string` - Encode `length` bytes, **unpadded** output.
- `encoding.base64_encode_padded(data, length)` → `string` - Encode `length` bytes, **with `=` padding** to a multiple of 4. For wire formats (auth headers, JSON-encoded blobs) that require it.
- `encoding.base64_decode(b64)` → `string!` - Decode, destructured as `(bytes, err)`. Accepts padded and unpadded input; `bytes` is an AetherString preserving embedded NULs.

### What's not in `std.cryptography`

The public-key and cipher families (RSA, ECDSA, Ed25519, X25519, AES, ChaCha20-Poly1305, …) live under `std.cryptography` as explicitly-imported sub-modules (`std.cryptography.rsa`, `std.cryptography.x25519`, …), pure-Aether, no OpenSSL. URL-safe Base64 (RFC 4648 §5) and constant-time comparison remain out of the top-level module. See [stdlib-reference.md](stdlib-reference.md) §"What `std.cryptography` doesn't do" for the rationale.

---

## Networking Library

### HTTP Client (Go-style)

```aether
import std.http

main() {
    body, err = http.get("http://example.com")
    if err != "" {
        println("failed: ${err}")
        return
    }
    println(body)
}
```

See `examples/stdlib/http-client.ae` for a runnable version.

### HTTP Client Functions

- `http.get(url)` → `(string, string)` - HTTP GET, auto-frees response
- `http.post(url, body, content_type)` → `(string, string)` - HTTP POST
- `http.put(url, body, content_type)` → `(string, string)` - HTTP PUT
- `http.delete(url)` → `(string, string)` - HTTP DELETE

All wrappers return `("", err)` for transport failures and for any non-2xx HTTP status. If you need status codes or headers, use the raw extern + accessor pattern:

```aether
response = http.get_raw(url)
status = http.response_status(response)
body = http.response_body(response)
http.response_free(response)
```

Raw externs: `http_get_raw`, `http_post_raw`, `http_put_raw`, `http_delete_raw`.

Response accessors (used with the raw API):

- `http.response_status(response)` - Read HTTP status code (0 on transport failure)
- `http.response_body(response)` - Read response body as string
- `http.response_headers(response)` - Read response headers as string
- `http.response_error(response)` - Read transport error string
- `http.response_ok(response)` - 1 if transport succeeded AND status is 2xx, else 0
- `http.response_free(response)` - Free response

### HTTP Server

```aether
import std.http

main() {
    server = http.server_create(8080)

    berr = http.server_bind(server, "127.0.0.1", 8080)
    if berr != "" {
        println("bind failed: ${berr}")
        return
    }

    serr = http.server_start(server)  // Blocks
    if serr != "" {
        println("start failed: ${serr}")
    }
    http.server_free(server)
}
```

### Server Functions

- `http.server_create(port)` - Create server (never fails)
- `http.server_bind(server, host, port)` → `string` - Bind to address, return error string
- `http.server_start(server)` → `string` - Start serving (blocking), return error string
- `http.server_stop(server)` - Stop server
- `http.server_free(server)` - Free server
- `http.server_get(server, path, handler, data)` - Register GET handler
- `http.server_post(server, path, handler, data)` - Register POST handler
- `http.response_json(res, json)` - Send JSON response
- `http.response_set_status(res, code)` - Set status code
- `http.response_set_header(res, name, value)` - Set header

Raw externs: `http_server_bind_raw`, `http_server_start_raw`.

### Server Configuration

- `http.server_set_tls(server, cert_path, key_path)` → `string` - HTTPS with PEM cert + key
- `http.server_set_keepalive(server, enable, max_requests, idle_timeout)` → `string` - HTTP/1.1 keep-alive with a `Duration` idle timeout
- `http.server_set_h2(server, max_concurrent_streams)` → `string` - HTTP/2 (h2 + h2c + ALPN). `0` uses libnghttp2's default (100). Returns error string when libnghttp2 isn't linked.
- `http.server_set_h2_concurrent_dispatch(server, worker_count)` → `string` - Routes h2 stream handlers onto the shared `std.worker` thread pool, sized to `worker_count`. `worker_count > 0` lets streams across all h2 connections execute their handlers in parallel; `0` (default) keeps dispatch sequential. POSIX-only; on Windows the call is a silent no-op.
- `http.server_shutdown_graceful(server, timeout)` → `string` - Stop accepting, drain in-flight for up to a `Duration`, exit. h2 sessions emit GOAWAY so peers don't start new streams.
- `http.server_set_health_probes(server, live_path, ready_path, ready_check, ud)` → `string` - Built-in `/healthz` (always 200) + `/readyz` (200 only when readiness check returns 1)
- `http.server_set_access_log(server, format, output_path)` → `string` - `"combined"` or `"json"` to a file path / `"-"` (stderr) / `""` (disable)
- `http.server_set_metrics(server, endpoint)` → `string` - Prometheus-compatible counters/histograms (default `/metrics`)

### HTTP Middleware (`std.http.middleware`)

Pre-handler middleware (run before route dispatch; can short-circuit with a populated response):

- `middleware.use_cors(server, allow_origin, allow_methods, allow_headers, allow_credentials, max_age_seconds)` → `string`
- `middleware.use_basic_auth(server, realm, verify_cb, ud)` → `string` - HTTP Basic (RFC 7617). Verifier receives decoded `(username, password)`.
- `middleware.use_bearer_auth(server, realm, verify_cb, ud)` → `string` - Bearer token (RFC 6750). Verifier receives raw token; bad credentials get `WWW-Authenticate: Bearer ... error="invalid_token"`.
- `middleware.use_session_auth(server, cookie_name, redirect_url, verify_cb, ud)` → `string` - Reads a named cookie; on failure 401 (when `redirect_url` is empty) or 302.
- `middleware.use_rate_limit(server, max_requests, window_ms)` → `string` - Token bucket per client IP.
- `middleware.use_vhost(server, hosts_csv)` → `string` - Host-header gate; unknown hosts get 404.
- `middleware.use_real_ip(server, header_name)` → `string` - Reads `X-Forwarded-For` (or configured header), takes leftmost IP, adds `X-Real-IP`. **Only safe behind a trusted proxy that strips client-supplied XFF.**
- `middleware.use_static_files(server, url_prefix, root)` → `string` - Mount a directory; `..` traversal blocked.
- `middleware.use_rewrite(server, opts)` → `string` - Prefix rewrite rules; build via `middleware.rewrite_add_rule(opts, from, to)`.

Response transformers (run after the route handler emits the response):

- `middleware.use_gzip(server, min_size, level)` → `string` - Gzip body when client sends `Accept-Encoding: gzip` and body is at least `min_size` bytes.
- `middleware.use_error_pages(server, opts)` → `string` - Custom error-status bodies; build via `middleware.error_pages_register(opts, status, body, content_type)`.

### HTTP Client Builder (`std.http.client`)

Builder-shaped requests with full response access. The `http.get` / `http.post` / `http.put` / `http.delete` one-liners above are good for "no auth, JSON in, 200 means good" calls; reach for `std.http.client` when you need custom request headers, response-header capture, status discrimination, per-request timeouts, or methods other than the four common verbs (PROPFIND, PATCH, custom RPC verbs all work). Method is an arbitrary string. Non-2xx is not an error, the caller checks `response_status`.

```aether
import std.http.client

main() {
    req = client.request("GET", "https://api.example.com/users/42")
    client.set_header(req, "Authorization", "Bearer abc123")
    client.set_timeout(req, 30s)
    resp, err = client.send_request(req)
    client.request_free(req)
    if err != "" { return }
    status = client.response_status(resp)
    body   = client.response_body(resp)
    client.response_free(resp)
}
```

**Builder + send:**
- `client.request(method, url)` → `ptr` - Build a request handle
- `client.set_header(req, name, value)` → `string` - Append a request header
- `client.set_body(req, body, length, content_type)` → `string` - Set request body (length explicit for binary safety)
- `client.set_timeout(req, timeout)` → `string` - `Duration` per-request timeout (`0ns` = block forever)
- `client.set_follow_redirects(req, max_hops)` → `string` - Follow up to `max_hops` redirects (`0` = don't follow, the default)
- `client.send_request(req)` → `(ptr, string)` - Fire it; `(resp, "")` on success, `(null, err)` on transport failure
- `client.request_free(req)` - Free the request handle

**TLS + proxy (per request):**
- `client.set_insecure(req, on)` → `string` - `1` skips TLS peer + hostname verification for this request only (`curl -k` / `--no-check-certificate`); relaxed per-connection, never on the shared `SSL_CTX`. Default `0` (verify). Use only against hosts you trust out-of-band.
- `client.set_cafile(req, path)` → `string` - pin a custom CA for this request: verify the peer against the PEM bundle at `path` instead of the system trust store, **keeping peer + hostname verification on**. Strictly stronger than `set_insecure`, for M2M calls to a host with a private/self-signed CA you've couriered out-of-band (e.g. a Proxmox VE API's `pve-root-ca.pem`). Per-connection (never touches the shared `SSL_CTX`); `""` clears the pin. A cert the pinned CA doesn't cover fails the handshake (fails closed, never open).
- `client.use_env_proxy(req, on)` → `string` - `1` follows `$HTTP_PROXY`/`$HTTPS_PROXY`/`$NO_PROXY`. **Off by default**, the client does not honour env proxies unless you opt in (the hardened inverse of the httpoxy/CVE-2016-5385 default-follow). Guarded: the CGI-injectable uppercase `HTTP_PROXY` is refused under `$REQUEST_METHOD`/`$GATEWAY_INTERFACE`, and a proxy at a loopback/link-local IP is rejected (SSRF).
- `client.use_http_proxy(req, "http://host:port")` → `string` - Pin an explicit forward proxy; env is ignored entirely (empty url = direct). A team-controlled proxy is immune to whatever the shell/CI set.
- `client.ignore_http_proxy(req)` → `string` - Force a direct connection regardless of env / any set proxy (determinism escape hatch, e.g. VCR record mode). Precedence: ignore > explicit > env > direct.

**Response accessors:**
- `client.response_status(resp)` → `int`
- `client.response_body(resp)` → `string` (binary-safe AetherString)
- `client.response_header(resp, name)` → `string` (case-insensitive)
- `client.response_headers(resp)` → `string` (raw header block)
- `client.response_error(resp)` → `string`
- `client.response_free(resp)` - Free the response

**Sugar wrappers** (pure Aether on top of the builder):
- `client.get_with_headers(url, header_pairs)` → `(string, int, string)`
- `client.post_with_status(url, body, content_type)` → `(string, int, string)`
- `client.post_json(url, value)` → `(ptr, string)` - Marshal value via `std.json`, set Content-Type + Accept
- `client.response_body_json(resp)` → `(ptr, string)` - `response_body` + `json.parse` round-trip

See `tests/integration/test_http_client_v2.ae` for ten worked examples (header round-trip, status discrimination, binary body, timeout, transport failure, JSON sugar, malformed-JSON parse failure); the [Standard Library Reference](stdlib-reference.md) covers the design choices (arbitrary-string method, non-2xx-is-not-an-error, `send_request` naming). Streaming response bodies for large downloads are not supported yet.

### HTTP Reverse Proxy (`std.http.proxy`)

nginx-class outbound HTTP forwarding for the Aether server. Five
load-balancing algorithms, active health checks, in-memory LRU
response cache (Vary-aware), per-upstream circuit breaker,
idempotent retry with `proxy_next_upstream` semantics, per-upstream
token-bucket rate limit, active drain, W3C Trace-Context
propagation, and Prometheus 0.0.4 metrics.

```aether
import std.http
import std.http.proxy

main() {
    server = http.server_create(8080)
    pool = proxy.upstream_pool_new("weighted_rr", 30, 0, 0)
    proxy.upstream_add(pool, "http://10.0.0.1:8080", 3)
    proxy.upstream_add(pool, "http://10.0.0.2:8080", 1)
    proxy.health_checks_enable(pool, "/health", 200, 5000, 1000, 2, 3)
    proxy.breaker_configure(pool, 5, 30000, 1)
    cache = proxy.cache_new(1000, 65536, 60, "method_url_vary")
    opts = proxy.opts_new()
    proxy.opts_bind_cache(opts, cache)
    proxy.opts_set_retry_policy(opts, 3, 100)
    proxy.mount(server, "/api", pool, opts)
    http.server_start(server)
}
```

**Pool + LB**: `proxy.upstream_pool_new(lb_algo, request_timeout_sec, dial_timeout_ms, max_inflight_per_up) -> ptr` (algos: `round_robin`, `least_conn`, `ip_hash`, `weighted_rr`, `cookie_hash`), `upstream_pool_free`, `upstream_add(pool, base_url, weight)`, `upstream_remove`, `upstream_drain` / `upstream_undrain`, `pool_set_cookie_name`, `rate_limit_set(pool, max_rps, burst)`.

**Health**: `proxy.health_checks_enable(pool, probe_path, expect_status, interval_ms, timeout_ms, healthy_threshold, unhealthy_threshold)`.

**Breaker**: `proxy.breaker_configure(pool, failure_threshold, open_duration_ms, half_open_max)`.

**Cache**: `proxy.cache_new(max_entries, max_body_bytes, default_ttl_sec, key_strategy) -> ptr` (strategies: `url`, `method_url`, `method_url_vary`).

**Per-mount opts**: `opts_new`, `opts_set_strip_prefix`, `opts_set_preserve_host`, `opts_set_xforwarded(opts, xff, xfp, xfh)`, `opts_bind_cache`, `opts_set_body_cap`, `opts_set_retry_policy(opts, max_retries, backoff_base_ms)`, `opts_set_trace_inject(opts, on)`.

**Install**: `proxy.mount(server, path_prefix, pool, opts)` or single-upstream `proxy.mount_simple(server, path_prefix, upstream_url, request_timeout_sec)`.

**Metrics**: `proxy.pool_metrics_text(pool) -> string` returns Prometheus 0.0.4 exposition (per-upstream counters by status class, transport_errors, timeouts, retries, latency_ms_sum/count + gauges for inflight/healthy/breaker_state/draining; pool-level cache_hits/misses/revalidations/503_no_upstream).

Full reference: [`docs/http-reverse-proxy.md`](http-reverse-proxy.md).

### HTTP Record/Replay (VCR), moved out of the stdlib

The Servirtium record/replay engine that used to ship as
`std.http.server.vcr` has been lifted into its own repository,
[`servirtium-vcr`](https://github.com/servirtium/servirtium-vcr),
which is now its authoritative home (alongside its language bindings).
It is no longer part of the Aether stdlib. See [`docs/http-vcr.md`](http-vcr.md).

### TCP Sockets (Go-style)

> Note: `send` and `receive` are reserved actor keywords in Aether, so
> the TCP wrappers use `write`/`read` instead. The raw externs retain
> the `send_raw`/`receive_raw` naming.

- `tcp.connect(host, port)` → `(ptr, string)` - Connect, return `(socket, err)`
- `tcp.write(sock, data)` → `(int, string)` - Text-shaped write, return `(bytes, err)`
- `tcp.write_n(sock, data, length)` → `(int, string)` - Length-aware write for binary payloads
- `tcp.read(sock, max_bytes)` → `(string, string)` - Text-shaped read, return `(data, err)`
- `tcp.read_n(sock, max_bytes)` → `(string, int, string)` - Binary-safe read, return `(bytes, length, err)`
- `tcp.poll(sock, timeout_ms)` → `(bool, string)` - Wait for readability. `(true, "")` = a following `read_n` won't block, `(false, "")` = timeout, `(false, err)` = closed/failed. `timeout_ms`: `-1` blocks, `0` polls, `>0` waits that many ms.
- `tcp.poll2(a, b, timeout_ms)` → `(bool, bool, string)` - Wait on two sockets at once (the full-duplex relay primitive); each flag is true when that side is readable, both may be true.
- `tcp.listen(port)` → `(ptr, string)` - Create listening socket
- `tcp.accept(server)` → `(ptr, string)` - Accept connection
- `tcp.close(sock)` - Close socket (infallible)
- `tcp.server_close(server)` - Close server socket

> **Timeout vs. close.** A quiet-but-alive peer (recv-timeout /
> would-block) is *not* a close: `tcp.read_n` returns the distinct
> `"timeout"` error and leaves the socket connected so you can retry or
> `tcp.poll` it. Only an orderly FIN or a hard error returns `"connection
> closed or receive failed"`. A full-duplex relay must branch on the
> `"timeout"` sentinel rather than collapsing all non-empty errors into
> "closed."

Raw externs: `tcp_connect_raw`, `tcp_send_raw`, `tcp_send_n_raw`, `tcp_receive_raw`, `tcp_receive_n_raw`, `tcp_listen_raw`, `tcp_accept_raw`, `tcp_poll_raw`, `tcp_poll2_raw`.

### Reactor-Pattern Async I/O (`await_io`)

Aether's scheduler has a per-core I/O reactor (epoll on Linux, kqueue
on macOS/BSD, poll() elsewhere) that can suspend an actor on a file
descriptor without blocking any OS thread. When the fd becomes ready,
the scheduler delivers an `IoReady` message to the actor's mailbox
and resumes it on any available core.

```aether
import std.net

message IoReady { fd: int, events: int }
message Connection { fd: int }

actor Worker {
    receive {
        Connection(fd) -> {
            req = ae_http_recv(fd)
            http_response_json(res, "{\"hello\":\"world\"}")
            net.await_io(fd)   // suspends, zero CPU until data arrives
        }
        IoReady(fd, events) -> {
            // Resumed here when fd is readable again
            req = ae_http_recv(fd)
            http_response_json(res, "{\"hello\":\"world\"}")
            net.await_io(fd)
        }
    }
}
```

**The `IoReady` message name is reserved.** The Aether message
registry assigns it the ID that the runtime scheduler uses for I/O
readiness notifications, so any actor that defines `message IoReady {
fd: int, events: int }` will receive scheduler-delivered events in
that arm.

Functions:

- `net.await_io(fd)` → `string` Register `fd` with the current
  core's I/O poller and mark the calling actor as waiting. Returns
  `""` on success, error string otherwise (invalid fd, no active
  actor context, or scheduler refused the registration). One-shot:
  the fd is automatically unregistered after the `IoReady` delivery.
- `net.ae_io_cancel(fd)` Abandon a prior `await_io` without waiting
  for the message. Rare; the one-shot policy makes this unnecessary
  in most flows.

Performance note: a C-level benchmark demonstrated the raw reactor pattern
delivering substantially higher HTTP throughput than a blocking
keep-alive worker. `await_io` is the Aether-language surface over
that same machinery, rerun the HTTP benchmark on your target host
to get a current figure for your environment.

---

## Collections Library

### ArrayList

```aether
import std.list

main() {
    mylist = list.new()
    defer list.free(mylist)

    list.add(mylist, some_ptr)
    item = list.get(mylist, 0)
    size = list.size(mylist)
}
```

### ArrayList Functions

- `list.new()` - Create new list (never fails)
- `list.add(list, item)` → `string` - Append item, return error string (non-empty on resize/OOM)
- `list.get(list, index)` - Get item (returns null for out-of-bounds)
- `list.set(list, index, item)` - Set item at index
- `list.remove(list, index)` - Remove item at index
- `list.size(list)` - Get size
- `list.clear(list)` - Clear all items
- `list.free(list)` - Free list

Raw extern: `list_add_raw` (returns 1/0).

### HashMap

```aether
import std.map

main() {
    mymap = map.new()
    defer map.free(mymap)

    map.put(mymap, "name", some_ptr)
    result = map.get(mymap, "name")
    exists = map.has(mymap, "name")
}
```

### HashMap Functions

- `map.new()` - Create new map (never fails)
- `map.put(map, key, value)` → `string` - Put key-value pair, return error string (non-empty on resize/OOM)
- `map.get(map, key)` - Get value by key (returns null if missing)
- `map.remove(map, key)` - Remove key
- `map.has(map, key)` - Check if key exists
- `map.size(map)` - Get number of entries
- `map.clear(map)` - Clear all entries
- `map.free(map)` - Free map

Raw extern: `map_put_raw` (returns 1/0).

### Set

Unique strings, backed by the `std.map` hash table. Items are copied on
insert. O(1) average lookup.

```aether
import std.set

visited = set.new()
set.add(visited, "/index")      // true
set.add(visited, "/index")      // false, already present
```

- `set.new()` → `ptr` - Create a new set (null on allocation failure)
- `set.add(set, item)` → `bool` - True if added, false if already present or the insert failed
- `set.contains(set, item)` → `bool` - Membership test
- `set.remove(set, item)` - Drop an item; absent items are ignored
- `set.size(set)` → `int` - Number of unique items
- `set.clear(set)` - Drop every item, keeping the set usable
- `set.free(set)` - Release the set
- `set.items(set)` → `(ptr, string)` - Snapshot of the items, unspecified order; release with `set.items_free`
- `set.items_free(items)` - Release a snapshot

Null-set calls are safe (`size` 0, `contains` false). Raw externs are the
`aether_set_*` entry points.

### PriorityQueue

Binary heap over `(priority, item)` pairs; lowest priority pops first.
Push/pop are O(log n), peek/size O(1). The queue does **not** own items,
it never frees them.

```aether
import std.pqueue

jobs = pqueue.new()
pqueue.push(jobs, 5, "page on-call")
next = pqueue.pop(jobs)
```

- `pqueue.new()` → `ptr` - Create a new queue (null on allocation failure)
- `pqueue.push(pq, priority, item)` → `bool` - Enqueue; false on a null queue or allocation failure
- `pqueue.pop(pq)` → `ptr` - Remove and return the lowest-priority item, null when empty
- `pqueue.peek(pq)` → `ptr` - Next item without removing it, null when empty
- `pqueue.peek_priority(pq)` → `long` - Priority of the next item (0 when empty)
- `pqueue.size(pq)` → `int` - Number of queued entries
- `pqueue.is_empty(pq)` → `bool` - True when nothing is queued
- `pqueue.clear(pq)` - Drop every entry, keeping the queue usable
- `pqueue.free(pq)` - Release the queue

Null-queue calls are safe (`size` 0, `pop`/`peek` null). Raw externs are the
`aether_pqueue_*` entry points.

---

## Logging Library

```aether
import std.log

main() {
    err = log.init("app.log", 0)  // 0 = DEBUG level
    if err != "" {
        println("cannot open log file: ${err}")
        // falls back to stderr automatically
    }

    log.write(0, "Debug message")
    log.write(1, "Info message")
    log.write(2, "Warning message")
    log.write(3, "Error message")

    log.print_stats()
    log.shutdown()
}
```

### Logging Functions

- `log.init(filename, level)` → `string` - Initialize logging, return error string if the log file could not be opened (logging still falls back to stderr)
- `log.shutdown()` - Shutdown logging
- `log.write(level, message)` - Write a log message at the given level
- `log.set_level(level)` - Set minimum level
- `log.set_colors(enabled)` - Enable/disable colored output (1/0)
- `log.set_timestamps(enabled)` - Enable/disable timestamps (1/0)
- `log.get_stats()` - Get logging statistics
- `log.print_stats()` - Print logging statistics

Raw extern: `log_init_raw` (returns 1/0).

### Log Levels

- `0` = DEBUG
- `1` = INFO
- `2` = WARN
- `3` = ERROR
- `4` = FATAL

---

## Concurrency Functions

### Actor Management

- `spawn(ActorName())` - Create a new actor instance

### Synchronization

- `wait_for_idle()` - Block until all actors finish processing
- `sleep(milliseconds)` - Pause execution

### Example

```aether
message Task { id: int }

actor Worker {
    state completed = 0

    receive {
        Task(id) -> {
            completed = completed + 1
        }
    }
}

main() {
    w = spawn(Worker())

    w ! Task { id: 1 }
    w ! Task { id: 2 }

    wait_for_idle()

    print("Completed: ")
    print(w.completed)
    print("\n")
}
```

---

## Memory Management

Aether uses **manual memory management** with `defer` as the primary tool.

### defer

Use `defer` immediately after allocation to ensure cleanup at scope exit:

```aether
import std.list
import std.string

main() {
    mylist = list.new()
    defer list.free(mylist)

    s = string.new("hello")
    defer string.free(s)

    // ... use mylist and s ...
    // Automatically freed when scope exits
}
```

### Guidelines

- **`defer type.free(x)`**, primary cleanup pattern for all allocations
- **Stack allocations**, freed automatically (no `defer` needed)
- **Actors**, managed by the runtime
- **Managed strings**, reference-counted internally; use `string.free()` (alias for `string.release()`)
- **`string.retain(str)`**, advanced: increment reference count when sharing ownership across C callbacks

---

## Best Practices

1. **Use `import` for stdlib** - Cleaner than `extern`
2. **Use `print()` for output** - Simple and reliable
3. **Free resources** - Use `defer type.free(x)` after allocation, or explicit `.free()` calls
4. **Enable bounds checking in debug** - Catches array errors
5. **Use actors for concurrency** - Safer than manual threading

---

## Example: Complete Program

```aether
import std.file

message Increment { amount: int }

actor Counter {
    state count = 0

    receive {
        Increment(amount) -> {
            count = count + amount
        }
    }
}

main() {
    print("Aether Runtime Example\n")

    if (file.exists("README.md") == 1) {
        print("README.md found!\n")
    }

    counter = spawn(Counter())
    counter ! Increment { amount: 1 }
    counter ! Increment { amount: 1 }

    wait_for_idle()

    print("Final count: ")
    print(counter.count)
    print("\n")
}
```
