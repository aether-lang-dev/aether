# Aether Standard Library Reference

Reference for Aether's standard library. The index lists every module that
ships, with its purpose and export count taken from the source rather than
maintained by hand, so it cannot drift out of step with the tree. The sections
after it cover the most-used modules in depth; for the others the index links
to the module source, whose header comment is the authoritative description.

## Module index (69 modules)

| Module | Purpose | Exports | Detail |
|---|---|---:|---|
| `std.actors` | Process-global name → actor_ref registry. | 12 | [module source](../std/actors/module.ae) |
| `std.alloc` | std.alloc (#1045): swappable allocator convention. | 6 | [module source](../std/alloc/module.ae) |
| `std.arena` | Arena (bulk) allocator. | 14 | [module source](../std/arena/module.ae) |
| `std.audio` | Audio playback (v1: playback tier). | 23 | [module source](../std/audio/module.ae) |
| `std.audit` | Query the sandbox audit trail. | 15 | [module source](../std/audit/module.ae) |
| `std.bignum` | Arbitrary-precision integers. | 25 | [module source](../std/bignum/module.ae) |
| `std.bits` | Unsigned-bit helpers. | 34 | [module source](../std/bits/module.ae) |
| `std.bytes` | Mutable byte buffer with random-access write and overlap-safe forward copy_within. | 50 | [module source](../std/bytes/module.ae) |
| `std.capsicum` | FreeBSD Capsicum capability-mode bindings. | 26 | [module source](../std/capsicum/module.ae) |
| `std.cas` | Content-addressed artifact store. | 21 | [module source](../std/cas/module.ae) |
| `std.casper` | FreeBSD Casper service delegation. | 22 | [module source](../std/casper/module.ae) |
| `std.cbor` | CBOR (RFC 8949) encode and decode. | 45 | [module source](../std/cbor/module.ae) |
| `std.clapae` | Command-line argument parser, modelled on clap. | 32 | [module source](../std/clapae/module.ae) |
| `std.collections` | Collections Module Import with: import std.collections. | 43 | [module source](../std/collections/module.ae) |
| `std.config` | Process-global immutable string→string KV store. | 12 | [module source](../std/config/module.ae) |
| `std.cryptography` | Cryptographic hash primitives + Base64 codec Import with: import std.cryptography. | 46 | [full section](#cryptography-stdcryptography) |
| `std.deque` | A fixed-capacity double-ended queue / ring buffer of `long` values (a `long` holds any int, and a pointer via. | 14 | [module source](../std/deque/module.ae) |
| `std.dir` | Directory Operations (alias for std.fs). | 11 | [module source](../std/dir/module.ae) |
| `std.dl` | Cross-platform dynamic library loader. | 8 | [module source](../std/dl/module.ae) |
| `std.encoding` | General-purpose binary/text encodings. | 11 | [module source](../std/encoding/module.ae) |
| `std.file` | File Operations (alias for std.fs). | 14 | [module source](../std/file/module.ae) |
| `std.floatarr` | Fixed-size packed-double buffer (float twin of std.intarr). | 13 | [module source](../std/floatarr/module.ae) |
| `std.fs` | File System Module Import with: import std.fs. | 147 | [module source](../std/fs/module.ae) |
| `std.hash` | Fast NON-cryptographic hashes for checksums and hashmap seeding. | 4 | [module source](../std/hash/module.ae) |
| `std.host` | Primitives for Aether scripts embedded in a host application. | 17 | [module source](../std/host/module.ae) |
| `std.http` | HTTP Client & Server (alias for std.net). | 142 | [module source](../std/http/module.ae) |
| `std.http1` | Pure-Aether HTTP/1.1 response reader (RFC 9112). | 20 | [module source](../std/http1/module.ae) |
| `std.intarr` | Fixed-size packed-int buffer (alias for std.collections). | 13 | [module source](../std/intarr/module.ae) |
| `std.io` | Input/Output Module Import with: import std.io. | 43 | [full section](#io-stdio) |
| `std.ipc` | Child-to-parent back-channel IPC. | 4 | [module source](../std/ipc/module.ae) |
| `std.json` | JSON Module Import with: import std.json. | 54 | [full section](#json-stdjson) |
| `std.ksuid` | KSUID (https://github.com/segmentio/ksuid). | 1 | [module source](../std/ksuid/module.ae) |
| `std.language` | BCP 47 Language Tags and Matching (RFC 5646, RFC 4647). | 11 | [module source](../std/language/module.ae) |
| `std.list` | ArrayList Operations (alias for std.collections). | 12 | [module source](../std/list/module.ae) |
| `std.log` | Logging Module Import with: import std.log Log level constants. | 9 | [full section](#logging-stdlog) |
| `std.longarr` | Fixed-size packed-long buffer (the 64-bit twin of std.intarr). | 13 | [module source](../std/longarr/module.ae) |
| `std.lzf` | One-shot LZF compression/decompression. | 12 | [module source](../std/lzf/module.ae) |
| `std.map` | HashMap Operations (alias for std.collections). | 14 | [module source](../std/map/module.ae) |
| `std.math` | Math Module Import with: import std.math. | 31 | [full section](#math-stdmath) |
| `std.mem` | Byte-level access to caller-allocated raw pointers. | 94 | [module source](../std/mem/module.ae) |
| `std.message` | ICU MessageFormat + message catalog (Phase 3 of #863). | 8 | [module source](../std/message/module.ae) |
| `std.msgpack` | MessagePack serialisation and deserialisation. | 35 | [module source](../std/msgpack/module.ae) |
| `std.nanoid` | NanoID (https://github.com/ai/nanoid). | 2 | [module source](../std/nanoid/module.ae) |
| `std.net` | Networking Module Import with: import std.net. | 69 | [module source](../std/net/module.ae) |
| `std.number` | Locale-aware number, percent, and currency formatting (Phase 4 of #863). | 10 | [module source](../std/number/module.ae) |
| `std.os` | Shell & Process Execution Import with: import std.os. | 75 | [full section](#os-stdos) |
| `std.path` | Path Utilities. | 20 | [module source](../std/path/module.ae) |
| `std.plural` | CLDR Plural Rules evaluation (Phase 2 of #863). | 2 | [module source](../std/plural/module.ae) |
| `std.pqueue` | Priority queue over (priority, item) pairs. | 18 | [module source](../std/pqueue/module.ae) |
| `std.regex` | Perl-compatible regular expressions (PCRE2-backed). | 12 | [module source](../std/regex/module.ae) |
| `std.schema` | Declarative, typed data validation & coercion. | 46 | [module source](../std/schema/module.ae) |
| `std.set` | Unordered set of unique strings. | 18 | [module source](../std/set/module.ae) |
| `std.signal` | POSIX signal-number constants. | 11 | [module source](../std/signal/module.ae) |
| `std.snapshot` | Copy-on-write snapshot cell for read-mostly shared data. | 5 | [module source](../std/snapshot/module.ae) |
| `std.sort` | In-place ascending sort and binary search over the packed numeric array types. | 6 | [module source](../std/sort/module.ae) |
| `std.strbuilder` | Amortised-O(1) string append. | 33 | [module source](../std/strbuilder/module.ae) |
| `std.string` | String Module Import with: import std.string. | 92 | [full section](#strings-stdstring) |
| `std.tar` | Streaming POSIX ustar TAR Archive Module Import with: import std.tar. | 24 | [full section](#posix-ustar-archives-stdtar) |
| `std.tcp` | TCP Socket Operations (alias for std.net). | 24 | [module source](../std/tcp/module.ae) |
| `std.time` | Civil date/time over Unix epoch seconds (UTC). | 19 | [module source](../std/time/module.ae) |
| `std.tracking` | std.tracking (#1049): leak-detecting allocator wrapper. | 5 | [module source](../std/tracking/module.ae) |
| `std.tsid` | TSID (Twitter Snowflake family / Discord-shaped). | 1 | [module source](../std/tsid/module.ae) |
| `std.ulid` | ULID (https://github.com/ulid/spec). | 1 | [module source](../std/ulid/module.ae) |
| `std.url` | RFC 3986 percent-encoding + query-string parsing (#629). | 7 | [module source](../std/url/module.ae) |
| `std.uuid` | UUID v4 and v7 (RFC 9562). | 2 | [module source](../std/uuid/module.ae) |
| `std.worker` | Run blocking work off the loop thread, deliver the result back on it. | 19 | [module source](../std/worker/module.ae) |
| `std.xml` | XML Module Import with: import std.xml. | 45 | [full section](#xml-stdxml) |
| `std.yaml` | YAML Module Import with: import std.yaml. | 16 | [module source](../std/yaml/module.ae) |
| `std.zlib` | One-shot zlib and gzip deflate/inflate. | 16 | [full section](#compression-stdzlib) |

> **Note:** The standard library follows the canonical module pattern in [stdlib-module-pattern.md](stdlib-module-pattern.md), fallible operations expose a `_raw` extern plus a Go-style `(value, err)` Aether wrapper; pure/infallible operations stay raw without a suffix. See the [error handling example](../examples/basics/error-handling.ae) for how the pattern is used from user code, and [std/fs/module.ae](../std/fs/module.ae) for the reference implementation.

## Platform support

| Target | Filesystem | Networking | Threading | Notes |
|---|---|---|---|---|
| Linux / macOS / BSD | full POSIX | full | full | Reference target. |
| Windows (MSYS2 / mingw-w64) | partial | full | full | Process exec uses POSIX fallbacks; `os.run` is POSIX-only until the `CreateProcessW` backend lands. Some fs operations (`symlink`, `readlink`) are stubs returning clean errors via the Go-style wrappers. |
| WASI (wasi-sdk) | per preopened paths | none | single-threaded | wasi-libc provides POSIX-compatible `fopen`/`fread`/`stat`/etc., so the normal fs code path compiles. Paths must be under a WASI preopen. |
| Emscripten (browser WASM) | off by default | off | cooperative | Builds pass `-DAETHER_NO_FILESYSTEM -DAETHER_NO_NETWORKING`. File ops return `(null, "cannot open file")` via the Go-style wrappers, no silent failures. To enable, compile with `-sFORCE_FILESYSTEM=1` and drop the define; untested in CI. |
| Bare embedded | off | off | cooperative | Same as Emscripten, stubs route all failures through the Go-style error tuples. |

When a target lacks a capability, the stub implementations in each stdlib module return `NULL` / `0` so the Go-style wrapper produces a descriptive error string rather than crashing. A call like `file.read("/etc/hosts")` on a no-fs target returns `("", "cannot open file")`, which the caller handles the same way as any other I/O error.

## Using the Standard Library

Import modules with the `import` statement and call functions with namespace syntax:

```aether
import std.string
import std.file

main() {
    // Namespace-style calls
    s = string.new("hello");
    len = string.length(s);

    if (file.exists("data.txt") == 1) {
        print("File exists!\n");
    }

    string.release(s);
}
```

---

## Collections

### List (`std.list`)

Dynamic array (ArrayList) implementation.

```aether
import std.list

main() {
    mylist = list.new()
    defer list.free(mylist)

    // list.add returns an error string. Empty = success; non-empty
    // indicates a resize/OOM failure that was previously silent.
    list.add(mylist, 10)
    list.add(mylist, 20)

    item = list.get(mylist, 0)
    size = list.size(mylist)

    list.remove(mylist, 0)
    list.clear(mylist)
}
```

**Functions:**
- `list.new()` - Create new list
- `list.add(list, item)` → `string` - Append item, return error string
- `list.get(list, index)` - Get item at index
- `list.set(list, index, item)` - Set item at index
- `list.remove(list, index)` - Remove item at index
- `list.size(list)` - Get number of elements
- `list.clear(list)` - Remove all elements
- `list.free(list)` - Free list memory

Raw extern: `list_add_raw` (returns 1/0).

**String-value ownership.** `list.add(l, <heap string expr>)` is auto-routed
by the compiler to an *adopting* add: the value is escaping into the list, the
caller's own release is suppressed, and `list.free` releases it. That transfer
is invisible and costs nothing.

Calling the owned-add extern **by hand** is different, and the two entry points
say which they are:

- `list_add_string_owned(l, s)` gives the list its **own** reference. A
  refcounted string is retained; a plain pointer (a literal, a borrowed
  `char*`) is copied, because a non-refcounted pointer cannot be shared
  safely. The caller keeps and independently frees theirs, so the same value
  may live in several containers, in any free order.
- `list_add_string_adopted(l, s)` takes over the caller's single reference
  without retaining. This is what codegen emits. Call it by hand only when
  ownership genuinely transfers: adopting a pointer you did not own
  double-frees at `list.free` time.

`map_put_string_owned` / `map_put_string_adopted` are the same pair for maps.

### String list (`string_list_*`)

Refcount-aware list for `AetherString` values. Use this instead of plain `list_*` when the list is meant to hold strings, the plain list stores items as raw `void*` and doesn't bump the refcount, so a string pushed and held while its original variable goes out of scope silently dangles.

```aether
import std.collections
import std.string

build() -> ptr {
    L = string_list_new()
    s = string.copy("ephemeral")   // wrapped AetherString*
    string_list_add(L, s)          // takes a strong reference
    return L
    // `s` goes out of scope; the list's retain keeps the bytes alive.
}

main() {
    L = build()
    println(string_list_get(L, 0))  // "ephemeral"
    string_list_free(L)             // releases every entry
}
```

Plain string literals pass through unchanged, `string_retain` is a no-op on values that don't bear the AetherString magic header.

**Functions:**
- `string_list_new()` → `ptr` - Allocate an empty list
- `string_list_add(list, s)` → `int` - Append; takes a strong reference; returns 1 on success, 0 on OOM
- `string_list_get(list, index)` → `string` - Borrowed read; null on OOB
- `string_list_set(list, index, s)` - Replace at index; releases old + retains new
- `string_list_size(list)` → `int` - -1 on null
- `string_list_remove(list, index)` - Remove + release the entry
- `string_list_clear(list)` - Drop every entry, keep the backing alloc
- `string_list_free(list)` - Release every entry then free
- `string_list_sort_lex(list)` - Stable ascending lexicographic (byte-wise) sort, in place
- `string_list_sort(list, cmp)` - Stable in-place sort by a comparator closure `|a: string, b: string| { ... }` returning negative / 0 / positive (like `strcmp`)

Both sorts reorder the backing slots only, no element is copied or freed, so they sidestep the get/set aliasing trap of a hand-rolled swap (`string_list_get` returns the slot's internal pointer; a naive adjacent swap would free a slot another borrowed pointer still aliases).

```aether
names = string_list_new()
string_list_add(names, "Charlie")
string_list_add(names, "alice")
string_list_add(names, "Bob")

string_list_sort_lex(names)                 // "Bob", "Charlie", "alice" (ASCII order)

string_list_sort(names, |a: string, b: string| {
    return string.length(a) - string.length(b)   // shortest first, stable
})
```

For a similar `string_map`, file an issue, same pattern would apply.

### Map (`std.map`)

Hash map implementation.

```aether
import std.map
import std.string

main() {
    mymap = map.new();
    defer map.free(mymap);

    val = string.new("Aether");
    defer string.release(val);

    // map.put returns an error string.
    map.put(mymap, "name", val);
    result = map.get(mymap, "name");
    exists = map.has(mymap, "name");

    map.remove(mymap, "name");
    size = map.size(mymap);

    map.clear(mymap);
}
```

**Functions:**
- `map.new()` - Create new map
- `map.put(map, key, value)` → `string` - Insert or update, return error string
- `map.get(map, key)` - Get value by key (null if missing)
- `map.has(map, key)` - Check if key exists
- `map.remove(map, key)` - Remove key-value pair
- `map.size(map)` - Get number of entries
- `map.clear(map)` - Remove all entries
- `map.free(map)` - Free map memory

Raw extern: `map_put_raw` (returns 1/0).

### Set (`std.set`)

Unordered collection of unique strings, backed by the same hash table as
`std.map`, so lookups are O(1) on average. Items are copied on insert, so
the caller's string lifetime does not matter.

```aether
import std.set

main() {
    visited = set.new()
    defer set.free(visited)

    set.add(visited, "/index")          // true, newly added
    set.add(visited, "/index")          // false, already present
    set.add(visited, "/about")

    if set.contains(visited, "/about") {
        println("pages: ${set.size(visited)}")
    }

    set.remove(visited, "/about")
}
```

**Functions:**
- `set.new()` → `ptr` - Create a new set (null on allocation failure)
- `set.add(set, item)` → `bool` - True if added, false if already present or the insert failed
- `set.contains(set, item)` → `bool` - Membership test
- `set.remove(set, item)` - Drop an item; absent items are ignored
- `set.size(set)` → `int` - Number of unique items
- `set.clear(set)` - Drop every item, keeping the set usable
- `set.free(set)` - Release the set (items were copied in, nothing else to free)
- `set.items(set)` → `(ptr, string)` - Snapshot of the items in unspecified order; release with `set.items_free`
- `set.items_free(items)` - Release a snapshot from `set.items`

Calls on a null set are safe: `size` reports 0, `contains` reports false.

Raw externs are the `aether_set_*` entry points, which return C-style ints.

### Priority queue (`std.pqueue`)

Binary heap over `(priority, item)` pairs. The lowest priority value comes
out first; negate the priority for highest-first. Push and pop are
O(log n); peek and size are O(1).

The queue stores item pointers **without taking ownership**: it never frees
them, so heap items you push remain yours to release.

```aether
import std.pqueue

main() {
    jobs = pqueue.new()
    defer pqueue.free(jobs)

    pqueue.push(jobs, 30, "send newsletter")
    pqueue.push(jobs, 5,  "page on-call")
    pqueue.push(jobs, 20, "rebuild index")

    while pqueue.size(jobs) > 0 {
        priority = pqueue.peek_priority(jobs)
        job = pqueue.pop(jobs)
        println("[${priority}] ${job}")     // 5, 20, 30
    }
}
```

**Functions:**
- `pqueue.new()` → `ptr` - Create a new queue (null on allocation failure)
- `pqueue.push(pq, priority, item)` → `bool` - Enqueue; false on a null queue or allocation failure
- `pqueue.pop(pq)` → `ptr` - Remove and return the lowest-priority item, null when empty
- `pqueue.peek(pq)` → `ptr` - Next item without removing it, null when empty
- `pqueue.peek_priority(pq)` → `long` - Priority of the item `peek` would return (0 when empty)
- `pqueue.size(pq)` → `int` - Number of queued entries
- `pqueue.is_empty(pq)` → `bool` - True when nothing is queued
- `pqueue.clear(pq)` - Drop every entry, keeping the queue usable (items are not freed)
- `pqueue.free(pq)` - Release the queue (items were never owned by it)

Calls on a null queue are safe: `size` reports 0, `pop` and `peek` return null.

Raw externs are the `aether_pqueue_*` entry points, which return C-style ints.

### Fixed-size int array (`std.intarr`)

Packed int buffer with O(1) random access. For DP tables, flat
int-keyed lookup, and other hot paths where `std.list`'s `void*`-boxed
items cost an allocation per entry. Size is fixed at allocation,
callers that need growth use `std.list`.

```aether
import std.intarr

main() {
    // Blame LCS DP table: M rows * N cols, flat buffer.
    rows = 100
    cols = 50
    dp, err = intarr.new(rows * cols)
    if err != "" { return }

    // Hot loop, _unchecked skips the bounds check (valid index required).
    r = 0
    while r < rows {
        c = 0
        while c < cols {
            intarr_set_unchecked(dp, r * cols + c, r + c)
            c = c + 1
        }
        r = r + 1
    }

    intarr_free(dp)
}
```

**Functions:**
- `intarr.new(size)` → `(ptr, string)` - Allocate zero-initialised array
- `intarr.new_filled(size, init)` → `(ptr, string)` - Allocate with every slot set to `init`
- `intarr.get(arr, i)` → `(int, string)` - Bounds-checked read
- `intarr.set(arr, i, value)` → `string` - Bounds-checked write
- `intarr_size(arr)` → `int` - Returns -1 for null
- `intarr_fill(arr, value)` - Reset every slot to `value`
- `intarr_free(arr)` - Release

**Hot-path (caller-validated) variants, no bounds check:**
- `intarr_get_raw(arr, i)` / `intarr_set_raw(arr, i, v)` - Safe on OOB (returns 0 / no-op), no error report
- `intarr_get_unchecked(arr, i)` / `intarr_set_unchecked(arr, i, v)` - Undefined behaviour on OOB, for inner loops

### Fixed-size float array (`std.floatarr`)

Packed double buffer, the float twin of `std.intarr`. Aether's `float`
lowers to C `double`, so the element type is `double` end-to-end. Same
fixed-size discipline, same bounds-check policy. Motivating use cases:
SVG path-command argument storage, rasterizer edge tables, bbox
accumulators, blur kernel coefficients, any `number[]`-shaped data
where boxing each scalar into a `*Pt` struct would chase a pointer per
element.

```aether
import std.floatarr

main() {
    // Gaussian kernel coefficients, length 2r+1 for radius r.
    radius = 3
    n = 2 * radius + 1
    kernel, err = floatarr.new(n)
    if err != "" { return }

    // ... fill from sigma ...
    floatarr_free(kernel)
}
```

**Functions:**
- `floatarr.new(size)` → `(ptr, string)` - Allocate zero-initialised array
- `floatarr.new_filled(size, init)` → `(ptr, string)` - Allocate with every slot set to `init`
- `floatarr.get(arr, i)` → `(float, string)` - Bounds-checked read
- `floatarr.set(arr, i, value)` → `string` - Bounds-checked write
- `floatarr_size(arr)` → `int` - Returns -1 for null
- `floatarr_fill(arr, value)` - Reset every slot to `value`
- `floatarr_free(arr)` - Release

**Hot-path (caller-validated) variants, no bounds check:**
- `floatarr_get_raw(arr, i)` / `floatarr_set_raw(arr, i, v)` - Safe on OOB (returns 0.0 / no-op), no error report
- `floatarr_get_unchecked(arr, i)` / `floatarr_set_unchecked(arr, i, v)` - Undefined behaviour on OOB, for inner loops

### Mutable byte buffer (`std.bytes`)

Mutable random-access byte buffer with overlap-safe forward `copy_within`. Aether's `string` is immutable; reach for `std.bytes` when you need to write bytes at arbitrary indices and read bytes the loop just wrote (binary codec output buffers, varint emit, frame layout, RLE-style overlap copy).

```aether
import std.bytes

main() {
    // Build "[abc]" by writing one byte at a time.
    b = bytes.new(8)
    bytes.set(b, 0, 91)                    // '['
    bytes.copy_from_string(b, 1, "abc", 3)
    bytes.set(b, 4, 93)                    // ']'

    // Hand off to a refcounted AetherString, buffer is consumed.
    s = bytes.finish(b, 5)                 // "[abc]"
}
```

The headline use case is the RLE-overlap pattern that immutable concat can't express:

```aether
// Pre-fill with [A, B], then expand to [A, B, A, B, A, B] in one call.
b = bytes.new(8)
bytes.set(b, 0, 65)
bytes.set(b, 1, 66)
bytes.copy_within(b, 2, 0, 4)             // dst=2, src=0, length=4
out = bytes.finish(b, 6)                  // "ABABAB"
```

`copy_within` is **forward byte-by-byte** (deliberately not `memmove`-style). When `dst > src`, each iteration `i` reads `data[src + i]` which earlier iterations of the same call may have just written. That's how runs of repeated bytes get encoded.

**Functions:**
- `bytes.new(initial_capacity)` → `ptr` - Allocate empty buffer with reserved capacity
- `bytes.length(b)` → `int` - Logical byte count (-1 if null)
- `bytes.set(b, index, byte)` → `int` - Write byte at index; gaps zero-fill; returns 1 on success
- `bytes.get(b, index)` → `int` - Read byte at index as unsigned 0..255; -1 on OOB / NULL / negative
- `bytes.set_le16(b, index, value)` → `int` - Little-endian 16-bit write at index..index+1; grows; 1 on success
- `bytes.get_le16(b, index)` → `int` - Little-endian 16-bit read; -1 if range past current length
- `bytes.set_le32(b, index, value)` → `int` - Little-endian 32-bit write at index..index+3; grows; 1 on success
- `bytes.get_le32(b, index)` → `int` - Little-endian 32-bit read; -1 if range past current length
- `bytes.set_le64(b, index, value: long)` → `int` - Little-endian 64-bit write at index..index+7; grows; 1 on success
- `bytes.get_le64(b, index)` → `long` - Little-endian 64-bit read; -1 if range past current length; round-trips losslessly with set_le64
- `bytes.set_be16(b, index, value)` → `int` - Big-endian 16-bit write (MSB first); grows; 1 on success
- `bytes.get_be16(b, index)` → `int` - Big-endian 16-bit read; -1 if range past current length
- `bytes.set_be32(b, index, value)` → `int` - Big-endian 32-bit write (MSB first); grows; 1 on success
- `bytes.get_be32(b, index)` → `int` - Big-endian 32-bit read; -1 if range past current length
- `bytes.set_be64(b, index, value: long)` → `int` - Big-endian 64-bit write (MSB first); grows; 1 on success
- `bytes.get_be64(b, index)` → `long` - Big-endian 64-bit read; -1 if range past current length; round-trips losslessly with set_be64
- `bytes.copy_from_string(b, dst, src, src_len)` → `int` - Copy from a string into the buffer at offset
- `bytes.copy_from_bytes(dst, dst_off, src, src_off, length)` → `int` - Copy between distinct buffers (memmove semantics); grows `dst` if needed. Use for two-pass algorithms like separable Gaussian blur.
- `bytes.copy_within(b, dst, src, length)` → `int` - Self-copy, forward byte-by-byte (RLE-safe)
- `bytes.finish(b, length)` → `string` - Hand off to refcounted AetherString; buffer is consumed
- `bytes.free(b)` - Discard without finishing (idempotent on null)
- `bytes.data(b)` → `ptr` - Borrowed pointer to the buffer's own memory, for handing the region to a foreign runtime (a WebAssembly `HEAPU8.set`, a C API) without copying it byte by byte. Valid until the next call that can grow the buffer, which may move the allocation and invalidate it.
- `bytes.capacity(b)` → `int` - Bytes reserved, which is at least `length` and may be more.
- `bytes.set_length(b, n)` → `int` - Publish how many bytes a direct write into `data()`'s region made live. Clamped to the capacity, and returns the length actually set.

**Streaming binary reads without a per-block copy.** A fixed-size block reader (walking 512-byte sectors of a device, say) can read straight into a reused `std.bytes` buffer instead of allocating a fresh string per block: `fs.pread_into(file, buf, len, offset)` reads up to `len` bytes at `offset` directly into `buf` (clamped to its capacity), sets `buf`'s length to the count read, and returns `(n, err)` with the same EOF (`n == 0`) / short-read (`0 < n < len`) / I/O-error (`err != ""`) distinction as `fs.pread`. The packed integers are then read in place with `bytes.get_le64` etc., or walked with a cursor. `std.bytes.cursor` offers both byte orders, `read_be_u16/32/64` and `read_le_u16/32/64` (each returns `-1` and leaves the cursor unchanged at end-of-buffer), so little-endian on-disk formats stream as cleanly as big-endian wire formats.

The build-then-walk pattern needed by binary-codec encoders (svndiff and similar) is what `bytes.get` / `bytes.{set,get}_le32` are for, accumulate packed-int ops into the same buffer via `set_le32` at known offsets, then walk and read each back at finish time:

```aether
b = bytes.new(0)
bytes.set_le32(b, 0,  action)    // op0
bytes.set_le32(b, 4,  length)
bytes.set_le32(b, 8,  offset)
// … later …
op0_action = bytes.get_le32(b, 0)
op0_length = bytes.get_le32(b, 4)
```

---

## Strings (`std.string`)

Reference-counted strings with comprehensive operations.

### String Types: Plain Strings vs Managed Strings

> **Most users don't need managed strings.** All `std.string` functions work on both plain strings
> and managed strings transparently. `string.length("hello")` just works, no conversion needed.
> Only create managed strings via `string.new()` when you need reference counting.

Aether has two string representations:

| | `string` (plain) | Managed (`ptr` via `string.new()`) |
|---|---|---|
| **C type** | `const char*` | `AetherString*` |
| **Allocation** | Static (literals) or manual | Heap (reference-counted) |
| **Memory** | None needed | `string.release()` or `defer string.free()` |
| **Knows length?** | Computed via `strlen` | Stored in struct (`O(1)`) |
| **std.string functions** | All work | All work |

**`string`**, plain C string. String literals like `"hello"` are this type. All `std.string` functions accept these directly.

**Managed strings**, heap-allocated objects returned by `string.new()`. Typed as `ptr` in Aether code. Use when you need reference counting or the result of transformation functions like `string.trim()`, `string.to_upper()`.

**Converting between them:**

```aether
import std.string

main() {
    // Raw literal → managed: use string.new()
    raw = "  hello  "
    managed = string.new(raw)
    trimmed = string.trim(managed)

    // Managed → raw: use string.to_cstr()
    print(string.to_cstr(trimmed))

    defer string.free(managed)
    defer string.free(trimmed)
}
```

**Best practices:**
- Use `string` for message fields, keeps payloads simple
- Use managed strings when you need to manipulate text (trim, split, concat)
- Always `defer string.free()` immediately after creating a managed string
- Use `string.to_cstr()` when passing managed strings to `print` or message fields

### Usage Examples

```aether
import std.string

main() {
    // Create strings
    s = string.new("Hello");
    s2 = string.new(" World");

    // Operations
    len = string.length(s);
    combined = string.concat(s, s2);

    // String methods
    upper = string.to_upper(s);
    lower = string.to_lower(s);
    trimmed = string.trim(s);

    // Searching
    contains = string.contains(s, "ell");
    index = string.index_of(s, "l");
    starts = string.starts_with(s, "He");
    ends = string.ends_with(s, "lo");

    // Substrings
    sub = string.substring(s, 0, 3);  // "Hel"

    // Splitting, pick the shape that matches your access pattern.
    csv = string.new("a,b,c");

    // (a) AetherStringArray, O(1) random access via integer index.
    parts = string.split(csv, ",");
    count = string.array_size(parts);   // 3
    first = string.array_get(parts, 0);  // "a"
    string.array_free(parts);

    // (b) *StringSeq cons-cell, O(1) head/tail/cons/length, refcount-
    //     aware, pattern-matches with [h | t]. Reach for this when the
    //     result will be walked recursively or sent across an actor
    //     boundary as a message field. See docs/sequences.md.
    parts_seq = string.split_to_seq(csv, ",");
    n = string.seq_length(parts_seq);    // 3 (O(1) cached)
    h = string.seq_head(parts_seq);       // "a"
    string.seq_free(parts_seq);

    string.release(csv);

    // Conversion
    numstr = string.from_int(42);  // "42"
    f = string.from_float(3.14);   // "3.14"
    cstr = string.to_cstr(s);     // raw C string pointer

    // Memory management
    string.release(s);
    string.release(s2);
}
```

**Creation:**
- `string.new(cstr)` - Create from C string
- `string.from_literal(cstr)` - Create from string literal (alias for `new`)
- `string.from_cstr(cstr)` - Create from a C string (alias for `new`). Also accepts an `AetherString*` (e.g. a value read back from `list.add_string_owned` via `list.get`), copying its payload rather than its header bytes.
- `string.empty()` - Create empty string

**Operations:**
- `string.length(str)` - Get length
- `string.concat(a, b)` - Concatenate two strings (returns new string)
- `string.char_at(str, index)` - Get character at index
- `string.equals(a, b)` - Check equality (returns 1/0)
- `string.compare(a, b)` - Lexicographic compare (returns -1, 0, 1)

**Searching:**
- `string.starts_with(str, prefix)` - Check prefix (returns 1/0)
- `string.ends_with(str, suffix)` - Check suffix (returns 1/0)
- `string.contains(str, sub)` - Check if substring exists (returns 1/0)
- `string.index_of(str, sub)` - Find position of substring (returns -1 if not found)

**Transformation:**
- `string.replace(str, old, new)` - Replace the first occurrence of `old` with `new`. Returns a new managed string; `str` is untouched. Empty `old` returns a copy of `str` unchanged (no infinite empty-match expansion). Binary-safe.
- `string.replace_all(str, old, new)` - Replace every non-overlapping occurrence of `old` with `new`, scanned left to right. `new` may be empty (deletion) or longer than `old`. Same lifetimes and empty-`old` guard as `replace`. Single exact-size allocation regardless of match count.
- `string.substring(str, start, end)` - Extract substring
- `string.substring_n(str, str_len_bytes, start, end)` - Length-aware sibling. Caller threads the source length through; `str_len(s)` is not consulted internally. Reach for this when `str` arrived as a `string`-typed parameter at a function boundary AND the content may contain embedded NULs, see [c-interop.md § Passing string values into C externs (auto-unwrap)](c-interop.md#passing-string-values-into-c-externs-auto-unwrap). Without it, the auto-unwrap strips the AetherString header at the call site, `str_len` falls through to `strlen`, and binary content gets truncated at the first NUL.
- `string.length_n(str, known_length)` - Identity helper that documents intent. In code that receives a `string` parameter plus an explicit length, the explicit length IS the truth, don't consult the AetherString header. `n = string.length_n(s, n)` reads as "yes I know my length" instead of looking like a forgotten `string.length(s)` that would have truncated at NUL.
- `string.to_upper(str)` - Convert to uppercase (returns new string)
- `string.to_lower(str)` - Convert to lowercase (returns new string)
- `string.trim(str)` - Remove leading/trailing whitespace

**Splitting:**
- `string.split(str, delimiter)` - Split string by delimiter (returns array)
- `string.array_size(arr)` - Get number of parts in split result
- `string.array_get(arr, index)` - Get string at index from split result
- `string.array_free(arr)` - Free split result array
- `string.split_to_seq(str, delimiter)` - Split into a `*StringSeq` cons-cell list (Erlang/Elixir-shaped). Same split semantics as `string.split`, but returns the result as an O(1) head/tail/cons/length linked list with refcount-aware structural sharing. Use this when the result will be pattern-matched, walked recursively, or sent across an actor boundary as a message field. See [docs/sequences.md](sequences.md) for the full surface.
- `string.strip_prefix(s, prefix)` → `(rest, stripped)` - If `s` starts with `prefix`, returns the remainder and 1. Otherwise returns `s` and 0. Cleaner than manual `starts_with` + `substring` length arithmetic.

**Glob-pattern matching (string side, NOT filesystem):**
- `string.glob_match(pattern, s)` → `int` - Does `pattern` match `s`? POSIX fnmatch(3) syntax: `*` zero-or-more, `?` single-char, `[abc]` / `[a-z]` char classes, `[!abc]` negation, `\*` / `\?` literal escapes. Returns 1 on match, 0 on no-match, -1 on glob-syntax error. Distinct from `fs.glob` which enumerates matching files on disk, this is pure string matching (svn:ignore patterns, message routing, branch-spec matching).
- `string.glob_match_pathname(pattern, s)` → `int` - Same as `glob_match` but `*` and `?` do NOT cross a `/` separator. Use when matching path patterns: `src/*.c` matches `src/foo.c` but not `src/sub/foo.c`.

**Sequences (`*StringSeq` Erlang/Elixir-shaped cons-cell list):**

- `string.seq_empty()` → `*StringSeq` empty list (NULL pointer)
- `string.seq_cons(head, tail)` → `*StringSeq` prepend; retains both head and tail
- `string.seq_head(s)` → `string` `""` on empty
- `string.seq_tail(s)` → `*StringSeq` empty seq on empty
- `string.seq_is_empty(s)` → `int` 1 if empty
- `string.seq_length(s)` → `int` O(1) cached
- `string.seq_retain(s)` → `*StringSeq` bump refcount; pair with `seq_free`
- `string.seq_free(s)` iterative spine walk; stops at shared cells
- `string.seq_from_array(arr, count)` → `*StringSeq` build from an `AetherStringArray*` (the shape `string.split` returns)
- `string.seq_to_array(s)` → `ptr` materialise as `AetherStringArray*` for legacy callers; free with `string.array_free`
- `string.seq_reverse(s)` → `*StringSeq` O(n), fresh independent spine
- `string.seq_concat(a, b)` → `*StringSeq` O(|a|), `a` copied, `b` shared via refcount bump
- `string.seq_take(s, n)` → `*StringSeq` first `n` elements (clamped to length, negative yields empty); fresh independent spine
- `string.seq_drop(s, n)` → `*StringSeq` n-th tail retained (clamped to length, negative yields `s` retained); pointer walk only, no allocations
- `string.join(s, sep)` → `string` concatenate every element with `sep` between adjacent pairs; the complement of `split_to_seq`. **Linear cost**: two passes (sum lengths, fill one exact-size buffer), one allocation regardless of element count. Empty seq yields `""`, a single element yields itself with no separator, and both elements and separator are binary-safe. Returns a fresh owned string.

Pattern-match `[]` and `[h|t]` arms work directly against `*StringSeq` matched expressions:

```aether
match s {
    []      -> { /* end of list */ }
    [h | t] -> { println(h); walk(t) }   // h: string, t: *StringSeq
}
```

Array literal `[a, b, c]` builds a cons chain when the target type is `*StringSeq` (in message-field initializers); see [docs/sequences.md](sequences.md) for the disambiguation rule and worked examples.
- `string.copy(s)` - Return an independently-owned copy of `s`. Equivalent to `string.concat(s, "")` but with a discoverable name; callers use it to snapshot a borrowed TLS buffer before the next C call overwrites it.
- `string.format(fmt, args)` - Format a string by substituting `{}` placeholders with entries from an `std.list` of strings. `{{` and `}}` are literal braces. Use this for runtime-built strings of N parts where literal `${...}` interpolation isn't an option (e.g. when the format string itself comes from a config file or message-template lookup). Non-string values must be converted via `string.from_int(...)` etc. before being added to the list.

For a `split_once`-style operation (find the first `sep` in `s`, return the halves), use `string.index_of(s, sep)` + two `string.substring` calls, two lines of code that avoid a tuple-unification foot-gun the typechecker currently has around three-string tuples.

> **Note: `string + string` is not defined.** Use `"${a}${b}"` interpolation for literals or `string.concat(a, b)` for runtime-built strings; `string.format(fmt, args)` handles the N-part case. The typechecker rejects `+` between two string operands at compile time (E0200) with a hint naming `"${a}${b}"` interpolation and `string.concat(a, b)` it does NOT silently emit broken pointer arithmetic.

**Conversion:**
- `string.to_cstr(str)` - Get raw C string pointer
- `string.from_int(value)` - Create string from integer
- `string.from_float(value)` - Create string from float
- `string.from_double(value)` - Create lossless, round-trip-safe decimal string from float (binary64)

**Parsing (Go-style):**
- `string.to_int(s)` → `(int, string)` - Parse base-10 integer
- `string.to_long(s)` → `(long, string)` - Parse 64-bit integer
- `string.to_int_radix(s, radix)` → `(long, string)` - Parse base-N integer; `radix` in `[2, 36]`. No `"0x"`/`"0b"` prefix recognition. Returns `long` so 32-bit-wide hex (ARGB colors, file offsets) survives. Errors on invalid radix, invalid digit, empty input, overflow, or trailing garbage.
- `string.from_int_radix(value, radix)` → `string` - Inverse of `to_int_radix`. Render `value` in base `radix` (`[2, 36]`); empty string on invalid radix; `'-'` prefix for negatives. Pair with `pad_start` for fixed-width hex bytes.
- `string.pad_start(s, total_width, pad_char)` → `string` - Prepend `pad_char` (single-byte char code, e.g. `48` for `'0'`, `32` for `' '`) until `s` reaches `total_width`. Returns a fresh copy if `s` is already long enough (no truncation).
- `string.pad_end(s, total_width, pad_char)` → `string` - Append-side variant of `pad_start`. Useful for columnar text output.
- `string.to_float(s)` → `(float, string)` - Parse float
- `string.to_double(s)` → `(float, string)` - Parse double

Each returns `(value, "")` on success or `(0, "invalid ...")` on parse failure. Handles leading whitespace, sign, trailing whitespace; rejects trailing non-whitespace.

Raw out-parameter externs are preserved as `string_to_int_raw`, `string_to_long_raw`, `string_to_float_raw`, `string_to_double_raw` for callers who need to distinguish zero from parse failure without a tuple destructure.

**Memory:**
- `string.retain(str)` - Increment reference count
- `string.release(str)` - Decrement reference count (frees when zero)
- `string.free(str)` - Alias for `release`

### Building strings incrementally (avoiding quadratic cost)

Self-append accumulation re-copies everything you have built so far on
every iteration, so a loop that appends `n` pieces costs O(n²) in total
bytes copied:

```aether
// SLOW: quadratic. Each iteration allocates a fresh buffer and copies
// the entire accumulation into it.
d = ""
while i < n {
    d = "${d}${piece}"           // same shape as string.concat(d, piece)
    i = i + 1
}
```

The trap is invisible at small `n` and catastrophic at large `n`: a
real SVG path builder went from ~30 ms per call on a light font to
seconds per call (wedging the host app's event loop) once a denser
face pushed the outline-point count into the thousands.

Two linear-cost escapes, pick by shape.

**Appending piece by piece**: use `std.strbuilder`, which grows one
buffer with amortized doubling:

```aether
import std.strbuilder

b = strbuilder.new(0)            // 0 = default capacity hint
i = 0
while i < n {
    strbuilder.append(b, piece)  // amortized O(len(piece))
    i = i + 1
}
d = strbuilder.finish(b)         // finalize; frees the builder
```

`strbuilder` also carries typed appends (`append_int`, `append_long`,
`append_hex`, `append_byte`, `append_codepoint`) so number formatting
doesn't route through an intermediate string.

**Joining a sequence you already have**: use `string.join`, which
sizes one exact buffer in a first pass and fills it in a second:

```aether
d = string.join(parts, ", ")     // parts: *StringSeq
```

Neither escape changes the semantics of the naive form; they only
change its cost. Reach for the builder when pieces arrive one at a
time, `join` when the elements already exist as a `*StringSeq`.

### String ownership and the heap-string tracker

Strings reassigned to a variable are reclaimed automatically by a compiler-emitted wrapper, you do **not** write `defer string.free(s)` for in-Aether assignments. For every string variable in a function, the compiler emits a companion `_heap_<name>` tracker at function-entry scope that flips between 0 (current value is a literal) and 1 (current value is heap-allocated) as you reassign. On every reassignment, the wrapper `if (_heap_<name>) free(<old>)` decides whether to release the previous buffer.

Both the **stdlib** functions in the table above (`string.concat`, `string.substring`, `string.to_upper`, `string.to_lower`, `string.trim`) and **string interpolation** (`"foo ${x}"`) are recognised as heap-allocated.

A **user-defined `-> string` function** is recognised as heap-allocated iff every return statement in its body yields a heap-string-expression (recursive structural check with cycle detection):

```aether
my_concat(a: string, b: string) -> string {
    return string.concat(a, b)        // RHS is heap → my_concat is heap-returning
}

s = ""
i = 0
while i < 1000000 {
    s = my_concat(s, "x")              // O(1) memory, old s is freed automatically
    i = i + 1
}
```

A function returning a string literal, or a function whose returns mix heap and literal sources, is NOT recognised, and the wrapper won't try to free its result. This is the heap-string tracker's structural escape analysis.

The function-entry hoist closes the cross-block visibility gap that previously kept the simpler `node_type == TYPE_STRING` recognition unsafe: the tracker is now visible at every nesting depth, so a variable first-assigned in an if-then and reassigned in an else-if (or in a deeply nested loop) sees the same `_heap_<name>` cell and follows the same free/no-free rules.

For the full memory-management background, see [Memory Management](memory-management.md#string-memory-model-heap-string-tracker).

---

## File System

### Files (`std.file`)

Go-style tuple returns. Check the error string first, then use the value.

```aether
import std.file

main() {
    // Check existence
    if file.exists("data.txt") == 1 {
        size, err = file.size("data.txt")
        if err == "" { println("Size: ${size} bytes") }
    }

    // Read entire file (opens, reads, closes in one call)
    content, rerr = file.read("data.txt")
    if rerr != "" {
        println("read failed: ${rerr}")
        return
    }
    println(content)

    // Write
    werr = file.write("output.txt", "Hello")
    if werr != "" {
        println("write failed: ${werr}")
        return
    }

    // Delete
    derr = file.delete("temp.txt")
    _ = derr  // ignore if missing
}
```

**Functions:**
- `file.read(path)` → `(string, string)` - Read entire file (opens, reads, closes)
- `file.write(path, content)` → `string` - Overwrite file, return error string
- `file.open(path, mode)` → `(ptr, string)` - Low-level open (caller must `file.close`)
- `file.close(handle)` - Close file
- `file.size(path)` → `(int, string)` - Get size in bytes
- `file.delete(path)` → `string` - Delete file
- `file.exists(path)` - 1 if a **regular file** is at `path`, 0 otherwise. Returns 0 for directories, even if they exist, see `fs.exists` for the path-agnostic check.

Raw externs: `file_open_raw`, `file_read_all_raw`, `file_write_raw`, `file_delete_raw`, `file_size_raw`.

### Directories (`std.dir`)

```aether
import std.dir

main() {
    // Check and create
    if dir.exists("output") == 0 {
        err = dir.create("output")
        if err != "" { println("mkdir failed: ${err}") }
    }

    // List contents, with each entry's kind straight from readdir's d_type
    // (no per-entry stat needed).
    list, lerr = dir.list(".")
    if lerr == "" {
        n = dir.list_count(list)
        i = 0
        while i < n {
            name = dir.list_get(list, i)
            kind = dir.list_kind(list, i)      // 1 file / 2 dir / 3 symlink / 4 other / 0 unknown
            if kind == 2 { println("[dir]  ${name}") }
            i = i + 1
        }
        dir.list_free(list)
    }

    // Delete
    dir.delete("temp_dir")
}
```

**Functions:**
- `dir.create(path)` → `string` - Create directory, return error string
- `dir.delete(path)` → `string` - Delete empty directory, return error string
- `dir.list(path)` → `(ptr, string)` - List contents (caller must `dir.list_free`)
- `dir.list_count(list)` → `int` - Number of entries
- `dir.list_get(list, index)` → `string` - Entry name at `index`
- `dir.list_kind(list, index)` → `int` - Entry's file kind from readdir's `d_type`, avoiding a `stat(2)` per entry: 1 = file, 2 = directory, 3 = symlink (target not followed), 4 = other, 5 = socket, 6 = FIFO, 7 = device; 0 = unknown (the filesystem didn't report a type, stat that entry to resolve it). Same encoding as `file_stat`'s kind. Kinds 5-7 are POSIX-only: Windows reports those nodes as 4. The named constants `fs.STAT_KIND_FILE`, `STAT_KIND_DIR`, `STAT_KIND_SYMLINK`, `STAT_KIND_OTHER`, `STAT_KIND_SOCKET`, `STAT_KIND_FIFO` and `STAT_KIND_DEVICE` are exported from `std.fs`, prefer them over the bare numbers.
- `dir.exists(path)` - 1 if a **directory** is at `path`, 0 otherwise. Returns 0 for regular files, even if they exist, see `fs.exists` for the path-agnostic check.
- `dir.list_free(list)` - Free directory listing

Raw externs: `dir_create_raw`, `dir_delete_raw`, `dir_list_raw`, `dir_list_count`, `dir_list_get`, `dir_list_kind`, `dir_list_free`.

### Recursive walk and change notification (`fs.walk`, `fs.watch_*`)

The building blocks beyond one-level listing: visit a whole tree, and learn when a directory changes underneath you.

```aether
import std.fs

// Walk: the callback sees every entry with its kind and depth.
n, err = fs.walk(root, |path: string, kind: int, depth: int| {
    // kind: 1 file / 2 dir / 3 symlink / 4 other / 5 socket / 6 fifo / 7 device
    // (same encoding as file_stat; see fs.STAT_KIND_*)
    if kind == 2 && string.ends_with(path, "/node_modules") == 1 {
        return 1                 // skip this subtree
    }
    println("${depth} ${path}")
    return 0                     // 0 continue · 1 skip subtree · 2 stop walk
})

// Watch: coarse change ping, re-list to see what changed.
w, werr = fs.watch_open(dir)
changed = fs.watch_wait(w, 1000)   // 1 changed / 0 timeout / -1 error
fs.watch_close(w)
```

**Functions:**
- `fs.walk(path, cb)` → `(int, string)` - Visit `path` (depth 0) and every entry beneath it. Entry kinds come from readdir's `d_type`, one sweep per directory, no per-entry `stat(2)`. Symlinks are reported (kind 3) but never followed, so cycles are impossible. `path` inside the callback is borrowed, copy it to keep it. Traversal order within a directory is the platform's readdir order (unspecified). Returns (entries visited, `""`), or (0, error) when `path` can't be read.
- `fs.watch_open(path)` → `(ptr, string)` - Watch one directory (or file), non-recursive, over the platform primitive: kqueue `EVFILT_VNODE` (macOS/BSD), inotify (Linux), `FindFirstChangeNotification` (Windows). The handle is single-threaded.
- `fs.watch_wait(watch, timeout_ms)` → `int` - Block up to `timeout_ms` (negative = forever): 1 = something changed (create/delete/modify/rename inside the watched directory), 0 = timeout, -1 = error. Changes made **between** `watch_open` and `watch_wait` are queued, not lost, and a burst of changes reports once (pending events are drained).
- `fs.watch_close(watch)` - Release the handle. Safe on null.

The watch event is deliberately coarse, a "something changed here" ping without the file name (that is the only semantics all three platform primitives share; kqueue in particular reports no names). The idiomatic pattern is: wake on the ping, re-list with `dir.list` + `dir.list_kind`, diff against what you rendered. An actor can own the watch and poll with a short timeout in a self-send loop for live refresh.

### Paths (`std.path`)

Path functions return heap-allocated plain strings (`char*`). Use `defer free(result)` if you want explicit cleanup.

```aether
import std.path

main() {
    joined = path.join("dir", "file.txt")
    println(joined)                            // "dir/file.txt"

    dirname = path.dirname("/a/b/file.txt")    // "/a/b"
    basename = path.basename("/a/b/file.txt")  // "file.txt"
    ext = path.extension("file.txt")           // ".txt" (includes dot)
    is_abs = path.is_absolute("/usr/bin")      // 1

    println("${dirname}/${basename}")
}
```

**Functions:**
- `path.join(a, b)` - Join path components
- `path.dirname(path)` - Get directory name
- `path.basename(path)` - Get file name
- `path.extension(path)` - Get file extension including dot
- `path.is_absolute(path)` - Check if absolute path (returns 1/0)
- `path.clean(path)` → `string` - Lexical normalize: collapses `//`, resolves `.` and `..`, drops a trailing separator. Purely textual, it never touches the filesystem, so unlike `fs.realpath` it works on paths that do not exist yet, which is the case when you are computing an output path before creating it. On Windows it understands both separators and a `C:` / UNC volume prefix, and emits the platform separator.
- `path.join_clean(a, b)` → `string` - `join` followed by `clean` in one call. Use this rather than `join` whenever `b` is caller-supplied (an object key, an archive entry name) so a `..` is resolved before the path reaches the filesystem: `path.join_clean("bucket", "a/../b")` is `"bucket/b"`. Pair with `is_within_base`.
- `path.is_within_base(base, target)` → `int` - 1 if `target` lies within `base` after both are cleaned, else 0. The lexical pre-`open` check for a blob store, static-file server or archive extractor: reject the request before you open it. Comparison follows platform rules, so on Windows it accepts either separator and is case-insensitive. Symlinks are not followed, a link under `base` pointing outside is an open-time concern.
- `path.rel(base, target)` → `string` - The relative path from `base` to `target`, such that joining it onto `base` and cleaning yields `target`. Returns empty when there is no such path (one absolute and one relative, or different Windows volumes).
- `path.separator()` → `string` - The platform path separator, `"/"` on POSIX and `"\\"` on Windows. Use it instead of hardcoding a separator.

### Full-fat filesystem (`std.fs`)

`std.file` / `std.dir` / `std.path` cover most calls; `std.fs` re-exports
them and adds the accessors that need a bit more plumbing, durable
writes, atomic rename, one-shot stat, binary-safe read.

```aether
import std.fs

main() {
    // Durable write: staging + fsync + rename.
    err = fs.write_atomic("config.json", body, string.length(body))

    // Rename composes with write_atomic for stage-then-publish.
    err = fs.rename("config.json.new", "config.json")

    // One stat, four fields.
    kind, size, mtime, err = fs.file_stat("config.json")
    //   kind: 1=file, 2=dir, 3=symlink, 4=other

    // Binary-safe read with explicit length.
    data, n, err = fs.read_binary("payload.bin")
}
```

**Functions (beyond those re-exported from `std.file`/`std.dir`/`std.path`):**
- `fs.exists(path)` → `int` - **Path-agnostic** existence check: 1 if anything is at `path` (regular file, directory, symlink, fifo, ...), 0 otherwise. Distinct from `file.exists` (regular-file-only) and `dir.exists` (directory-only), those filter by type, this one doesn't. Uses `lstat(2)` so a dangling symlink counts as existing, matches POSIX `test -e`. Reach for this in tooling that probes whether a path is bound without caring what's there (build-system runtime-path discovery, "did the user pass a real path?" CLI validation).
- `fs.write_atomic(path, data, length)` → `string` - Stage to `<path>.tmp.<pid>.<n>`, fsync, rename over destination. Binary-safe via explicit length. The tmp file is created with `O_CREAT|O_EXCL|O_NOFOLLOW` so an attacker who pre-plants a symlink at the predictable tmp path can't trick the write into following it; permissions track the process umask exactly as the previous `fopen("wb")` would have.
- `fs.write_binary(path, data, length)` → `string` - Non-atomic `fopen("wb")` + `fwrite` + `fclose`. Binary-safe via explicit length. Cheaper than `write_atomic` when a partial file on crash is acceptable (scratch writes, caches).
- `fs.rename(from, to)` → `string` - POSIX `rename(2)` wrapper. Atomic when source and target are on the same filesystem.
- `fs.create_dir_with_mode(path, mode)` → `string` - Like `fs.create_dir` but takes an explicit POSIX mode (0777-masked). Use this for private dirs (e.g. `0o700` for keys), sets the bits at creation time, closing the `mkdir` → `chmod` race window. Windows ignores the mode at the directory layer; the parameter is accepted for portability.
- `fs.mtime(path)` → `(int, string)` - File's mtime as Unix epoch seconds, in the standard `(value, err)` shape. Distinguishes "stat failed" from "file's mtime is 0 (1970 epoch)", the older `file_mtime` extern collapsed both into a single 0 sentinel and is kept only for back-compat.
- `fs.file_stat(path)` → `(kind, size, mtime, err)` - One `lstat(2)`; symlinks report kind 3, target is not followed. Kind is 1 = file, 2 = directory, 3 = symlink, 4 = other, 5 = socket, 6 = FIFO, 7 = device (char and block devices share 7). Sockets, FIFOs and devices are distinguished on POSIX only; Windows reports them as 4. Use the `fs.STAT_KIND_*` constants rather than the literals.
- `fs.fs_is_socket(path)` → `int` - 1 if `path` is a UNIX-domain socket, 0 otherwise (including when nothing is there). Unlike `fs.fs_is_symlink` this **follows** symlinks, since a caller asking "is this a socket" wants the target. POSIX only; always 0 on Windows. Pair it with `os.user_id()` to locate a per-user runtime socket such as `/run/user/${os.user_id()}/podman/podman.sock`.
- `fs.read_binary(path)` → `(content, length, err)` - Length-aware read preserving embedded NULs.

### Structured-error pilot

The four wrappers below return a three-element tuple `(value, kind: int, message: string)` instead of the usual `(value, err)` shape. `kind` is one of the `KIND_*` constants exported from `std.fs`; switch on it to discriminate failure modes programmatically without parsing English. `kind == fs.KIND_OK` (the integer `0`) means success; the message stays empty.

```aether
import std.fs

bytes, kind, msg = fs.copy("a.bin", "b.bin")
if kind == fs.KIND_OK {
    println("copied ${bytes} bytes")
} else if kind == fs.KIND_NOT_FOUND {
    println("source missing")
} else if kind == fs.KIND_PERMISSION_DENIED {
    println("permission denied (${msg})")
} else {
    println("copy failed: ${msg}")
}
```

`fs.copy`, `fs.move`, `fs.realpath`, and `fs.chmod` all ship with this structured-error shape. Existing wrappers keep their `(value, err)` shape unchanged, the structured-error shape sits next to it, not in place of it.

**Constants** (exported from `std.fs`):
| Constant | Value | Errno |
|---|---|---|
| `KIND_OK` | 0 | (none, success) |
| `KIND_NOT_FOUND` | 1 | `ENOENT` |
| `KIND_PERMISSION_DENIED` | 2 | `EACCES` / `EPERM` |
| `KIND_EXISTS` | 3 | `EEXIST` |
| `KIND_CROSS_DEVICE` | 4 | `EXDEV` |
| `KIND_IO` | 5 | `EIO` and unspecified I/O errors |
| `KIND_INVALID` | 6 | `EINVAL` (illegal argument; e.g. `src == dst`) |
| `KIND_LOOP` | 7 | `ELOOP` (symlink cycle) |
| `KIND_NAME_TOO_LONG` | 8 | `ENAMETOOLONG` |
| `KIND_NO_SPACE` | 9 | `ENOSPC` |
| `KIND_IS_DIR` | 10 | `EISDIR` |
| `KIND_NOT_DIR` | 11 | `ENOTDIR` |
| `KIND_UNAVAILABLE` | 99 | platform feature compiled out |

**Functions:**
- `fs.copy(src, dst)` → `(int, int, string)` Copy file contents; preserves source mode bits. Symlinks in `src` are followed (matches POSIX `cp` without `-P`); `dst` is overwritten if it exists; `dst` cannot be an existing directory (returns `KIND_IS_DIR`). On partial failure, the bytes count reflects how far the copy got.

  Performance: zero-copy via the platform's best primitive, Linux `copy_file_range(2)` (reflinks on btrfs/XFS) → `sendfile(2)`; macOS `fcopyfile(COPYFILE_DATA)` (APFS clone on same-volume); Windows `CopyFileExW` (kernel block copy). An 8 MiB read/write loop is the portable fallback for filesystems that reject the kernel primitives. The byte count saturates at `INT_MAX` for files larger than 2³¹ bytes, the data is still copied correctly; only the reported count is truncated.

- `fs.move(src, dst)` → `(int, int, string)` Move file from `src` to `dst`. Atomic when source and destination are on the same filesystem (POSIX `rename(2)`). On `EXDEV` (cross-device) the call transparently falls back to `fs.copy` + `unlink` correct, but no longer atomic. Cross-device directory moves surface as `KIND_IS_DIR` (the underlying copy refuses to recurse). Windows uses `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED` so the cross-fs case is handled internally.

- `fs.realpath(path)` → `(string, int, string)` OS-canonicalise the path: every symlink is followed; `.` and `..` components are folded out. POSIX `realpath(3)`; on Windows `CreateFileW(FILE_FLAG_BACKUP_SEMANTICS)` + `GetFinalPathNameByHandleW(FILE_NAME_NORMALIZED)`, with the `\\?\` prefix stripped before returning. Common kinds: `KIND_NOT_FOUND` for a missing component, `KIND_LOOP` for symlink cycles, `KIND_NAME_TOO_LONG` if the resolved form exceeds the OS limit.

- `fs.chmod(path, mode)` → `(int, int, string)` Change permission bits. POSIX `chmod(2)` follows symlinks (matches what shell `chmod` does); `mode` is masked with `07777` internally so set-uid/sgid/sticky high-bits are honoured. On Windows only the user-write bit (`0o200`) is meaningful, read-only is toggled via `SetFileAttributesW(FILE_ATTRIBUTE_READONLY)`; every other bit is silently ignored, matching Python's `os.chmod` documented behaviour.

---

## In-process script gateway (`std.http.script_gateway`)

CGI-style ergonomics, one `.ae` script file per route, without paying the per-request fork/exec cost. The script is pre-compiled with `aetherc --emit=lib --with=net script.ae -o script.so` to a shared library; the host server `dlopen()`s it once at mount time and dispatches matched requests via a direct indirect call. Empirically ~50× faster than the equivalent subprocess-spawn dispatch.

### Script shape

The script must export an `aether_script_handle` symbol with the canonical `HttpHandler` signature, marked `@c_callback` so the emitted C uses the unmangled name:

```aether
// greeting.ae
import std.http

@c_callback aether_script_handle(req: ptr, res: ptr, ud: ptr) {
    http.response_set_status(res, 200)
    http.response_set_header(res, "Content-Type", "text/plain")
    http.response_set_body(res, "hello from greeting.ae\n")
}
```

Build it as a shared library, `--with=net` is required because `std.http` is the `net` capability and `--emit=lib` is capability-empty by default:

```sh
aetherc --emit=lib --with=net greeting.ae -o /var/aether/scripts/greeting.so
```

### Mount in the host

```aether
import std.http
import std.http.script_gateway

main() {
    s = http.server_create(8080)
    ok, kind, msg = script_gateway.mount(
        s, "/greet", "/var/aether/scripts/greeting.so")
    if kind != script_gateway.KIND_OK {
        println("mount failed: ${msg}"); exit(1)
    }
    http.server_start(s)
}
```

Now `GET /greet/anything` runs `aether_script_handle` from `greeting.so` directly on the connection thread.

### API

- `script_gateway.mount(server, path_prefix, so_path)` → `(int, int, string)` Mount the shared library at `so_path` as the request handler for every URL whose path starts with `path_prefix`. Returns `(1, KIND_OK, "")` on successful mount; `(0, KIND_*, msg)` on failure. The dlopen handle is intentionally long-lived (process-lifetime); hot-reload is a separate feature.

**Constants** (subset of std.fs's KIND_*, values match so callers can mix the two surfaces):

| Constant | Value | Meaning |
|---|---|---|
| `KIND_OK` | 0 | mount succeeded |
| `KIND_NOT_FOUND` | 1 | `so_path` is missing or unreadable |
| `KIND_INVALID` | 6 | null arg, or `.so` missing the `aether_script_handle` entrypoint |
| `KIND_IO` | 5 | `dlopen` failure (incompatible ABI, etc.) |
| `KIND_UNAVAILABLE` | 99 | platform stub (Windows DLL hosting is a follow-up) |

### Sandbox

`mount()` calls `aether_sandbox_check("fs_read", so_path)` before dlopen, so a sandboxed host cannot bring in arbitrary native code via untrusted `.so` paths. The script itself runs subject to whatever caps the host has armed via `libaether.h` (`aether_set_memory_cap`, `aether_set_call_deadline`).

### Performance characteristic

Per-request hot path is one `strncmp(path, prefix, prefix_len)` plus one indirect call. On Linux/glibc the indirect call goes through the dlopened SO's PLT once and is then jit-bound; subsequent calls are direct. On macOS (lazy bind disabled by RTLD_NOW) the binding happens at mount time so per-request cost is one direct indirect call.

---

## JSON (`std.json`)

JSON parsing, creation, and serialization. RFC 8259 conformant (318/318
JSONTestSuite cases, all mandatory `y_*` and `n_*` pass).

**Implementation.** Arena-allocated parser: every parsed value,
string, and container backing array comes from a single per-document
bump-pointer arena that `json.free` releases in one step.
Character-class lookup tables drive the hot-path dispatch (whitespace,
digit, structural, string-safe). UTF-8 validation uses Hoehrmann's
public-domain DFA. Numbers follow a three-path design: pure integers
go through an int64 accumulator; fractional/exponential values within
double's exact range (≤15 significant digits, |exponent| ≤ 22) take a
`POW10` fast-double path with one multiply and one cast; anything past
those bounds falls back to `strtod` for correct IEEE-754 rounding.
Strings are decoded in two phases, a pre-scan locates the closing
quote so the decode buffer is sized exactly once, and the inner
fast-loop over safe printable-ASCII bytes dispatches to SSE2 on
`__SSE2__`, NEON on `__ARM_NEON && __aarch64__`, or a scalar LUT
fallback compiled in for WASM / embedded / anywhere else. The SIMD
kernels are gated at compile time, not run-time detected, so there's
no branch cost on the happy path and the scalar fallback is always
linked. Compiles clean under `-Wall -Wextra -Werror -pedantic` on
every target in the CI matrix. Design rationale in
[json-parser-design.md](json-parser-design.md); measured throughput
in [benchmarks/json/baseline.md](../benchmarks/json/baseline.md).

**Security.** ASan+UBSan clean on the full bench corpus including a
10 MB synthesized document. JSONTestSuite conformance: every
`y_*` case accepted, every `n_*` case rejected, `i_*` outcomes
recorded. Fixed nesting depth limit of 256 prevents stack-overflow
DoS. First-error-wins diagnostics include `<reason> at <line>:<col>`.

```aether
import std.json

main() {
    // Parse JSON string, Go-style tuple return
    data, err = json.parse("{\"name\": \"Aether\", \"version\": 1}")
    if err != "" {
        println("parse failed: ${err}")
        return
    }
    name, _ = json.object_get(data, "name")
    text, _ = json.get_string(name)
    println(text)  // "Aether"

    // Create values
    obj = json.create_object()
    json.object_set(obj, "key", json.create_string("value"))
    json.object_set(obj, "count", json.create_number(42.0))

    // Arrays
    arr = json.create_array()
    json.array_add(arr, json.create_number(1.0))
    json.array_add(arr, json.create_number(2.0))
    size = json.array_size(arr)

    // Serialize to string, Go-style (output, err) tuple
    output, _ = json.stringify(obj)
    println("JSON: ${output}")

    // Type checking
    type = json.type(json.create_number(3.0))  // 2 = JSON_NUMBER

    // Cleanup
    json.free(data)
    json.free(obj)
    json.free(arr)
}
```

**JSON Type Constants:**
- `0` = NULL, `1` = BOOL, `2` = NUMBER, `3` = STRING, `4` = ARRAY, `5` = OBJECT

**Parsing / Serialization:**
- `json.parse(json_str)` → `(ptr, string)` - Parse JSON, returns `(value, err)` tuple
- `json.parse_strict(json_str)` → `(ptr, int, string)` - Structured-error variant of `parse`: returns `(value, KIND_OK, "")` on success or `(null, KIND_*, "<reason> at <line>:<col>")` on failure. `kind` discriminates between syntax errors (`KIND_PARSE_ERROR`), out-of-memory (`KIND_OUT_OF_MEMORY`), and invalid input (`KIND_INVALID_INPUT`) without parsing the human message. KIND values match `std.fs`'s pilot so callers can mix the two surfaces in a single switch.
- `json.last_error_kind()` / `last_error_line()` / `last_error_col()` → `int` - Programmatic accessors for the most recent parse failure on this thread. Read AFTER a `parse` / `parse_strict` returned a failure; undefined after a successful parse. Line/column are 1-based and match the values embedded in the error message.
- `json.stringify(value)` → `(string, string)` - Serialize to JSON string; `(output, err)` tuple (`("", "stringify failed")` on failure)
- `json.free(value)` - Free a JSON value tree

Raw extern: `json_parse_raw`.

**Structured-error kinds** (exported from `std.json`):

| Constant | Value | Meaning |
|---|---|---|
| `KIND_OK` | 0 | parse succeeded |
| `KIND_PARSE_ERROR` | 1 | malformed JSON (the message carries `... at line:col`) |
| `KIND_OUT_OF_MEMORY` | 2 | arena allocation failed during parse |
| `KIND_INVALID_INPUT` | 3 | NULL / empty input handed to `parse_strict` |

```aether
import std.json

main() {
    v, kind, msg = json.parse_strict(input)
    if kind == json.KIND_OK {
        // use v ...
        json.free(v)
    } else if kind == json.KIND_PARSE_ERROR {
        line = json.last_error_line()
        col  = json.last_error_col()
        println("syntax error at ${line}:${col}: ${msg}")
    } else if kind == json.KIND_OUT_OF_MEMORY {
        println("OOM during parse")
    } else {
        println("invalid input: ${msg}")
    }
}
```

The `parse_strict` shape is the std.fs structured-error pilot extended to a second module, same tuple, same KIND_* convention.

**Type Checking:**
- `json.type(value)` - Get type constant (0-5)
- `json.is_null(value)` - Check if null (returns 1/0)

**Value Getters:**
- `json.get_number(value)` - Get float value (lossy past 2^53)
- `json.get_int(value)` - Get 32-bit integer (clamps to +/-2147483647 on overflow)
- `json.get_long(value)` - Get the full int64 value exactly (IDs, byte-counts)
- `json.get_bool(value)` - Get boolean (1/0)
- `json.get_string(value)` → `(string, string)` - Get string value; `(text, err)` tuple, errors with `"not a string"` if `value` is not a `JSON_STRING`

**Object Operations:**
- `json.object_get(obj, key)` → `(ptr, string)` - Get value by key; `(child, err)` tuple. Absent key returns `(null, "")`, distinct from the error case `(null, "not an object")`
- `json.object_set(obj, key, value)` - Set key-value pair
- `json.object_has(obj, key)` - Check if key exists (returns 1/0)
- `json.object_size(obj)` - Number of entries (`0` for empty, `-1` if not an object)
- `json.object_entry(obj, i)` - `(key, value, err)` for the i-th entry; keys
  are yielded in insertion order (same as parsed input; same as the order
  `object_set` was called). Mutating `obj` during iteration is not supported.

**Array Operations:**
- `json.array_get(arr, index)` → `(ptr, string)` - Get value at index; `(value, err)` tuple. Out-of-range returns `(null, "")`, distinct from `(null, "not an array")`
- `json.array_add(arr, value)` - Append value
- `json.array_size(arr)` - Get array length

**Value Creation:**
- `json.create_null()`, `json.create_bool(value)`, `json.create_number(value)`
- `json.create_string(value)`, `json.create_array()`, `json.create_object()`

**Terse builder / encoder**, thin aliases over the above for
assembling a value tree and serializing it without hand-concatenating
strings (escaping is handled by the encoder):
- `json.obj()` / `json.arr()` new empty object / array
- `json.str(s)` / `json.num(f)` / `json.boolean(0|1)` / `json.null_value()` scalars
- `json.set(obj, key, value)` / `json.push(arr, value)` `""` on success, error
  string on wrong-kind target (the orphaned value is reclaimed); the parent
  takes ownership of `value`
- `json.encode(value)` → `(string, string)` `(json, "")` on success

```aether
import std.json

main() {
    root = json.obj()
    json.set(root, "name", json.str("aether"))
    json.set(root, "ok", json.boolean(1))
    tags = json.arr()
    json.push(tags, json.str("lang"))
    json.set(root, "tags", tags)

    out, err = json.encode(root)   // {"name":"aether","ok":true,"tags":["lang"]}
    println(out)
    json.free(root)                // frees the whole tree
}
```

### What `std.json` doesn't do

Coming from Go's `json.Unmarshal`, Java's Jackson, Python's `json.load` + dataclasses, or C#'s `JsonSerializer`, expect to do more by hand:

- **No struct ↔ JSON mapping.** Aether has no runtime reflection, no `instanceof`, no `T.GetType()`, no `reflect.TypeOf` so a library function that takes a struct type and a JSON tree and populates the struct fields can't exist as a stdlib API. Callers walk the tree by hand: `json.object_get(v, "name")` then `json.get_string(...)`, repeated per field. For tree-shaped or dynamically-shaped JSON the Aether code looks similar to other languages; for struct-shaped JSON it's more verbose. A future codegen step (a `--derive-json` flag on struct definitions, or a build-step macro) could close this gap without runtime reflection, but isn't shipped today.
- **No annotations / struct tags.** `@JsonProperty("user_name")`, Go struct tags `json:"user_name,omitempty"`, etc. don't apply, there's nothing for them to attach to without struct-mapping in the first place.
- **No streaming parse.** The whole document is buffered into the arena before the tree is walkable. For multi-gigabyte JSON, use a different tool. Documents into the tens of MB are fine.
- **No JSON5 / comments / trailing commas.** Strict RFC 8259 only.
- **No pretty-print on stringify.** Compact output only. Wrap with a separate prettier if you need one.
- **Declarative validation is `std.schema`** — typed records, composable
  validators, `parse -> (values, errors)`, and `to_json_schema()` to *emit* a
  draft-07 JSON Schema. It does not *consume* external JSON Schema documents;
  validate against a `std.schema` record, or build that on top.
- **No arbitrary-precision numbers.** Numbers are `int` or `double`; the parser falls through to `strtod` for correctly-rounded IEEE-754 on edge cases but there's no `BigDecimal` / `decimal.Decimal` equivalent for financial precision.

### Other structured-data formats

Beyond JSON, the stdlib has **no built-in support** for:

- **YAML**, no parser. Configuration files for Aether projects use TOML (read by the build tool internally, not a user-facing stdlib module) or hand-rolled formats.
- **XML**, `std.xml` provides a pull/SAX reader and an escaping builder (see the XML section above). It deliberately omits XSD, XPath, namespaces, and DTD validation; for those a host-language tool is still the answer.
- **TOML**, there's a parser at `tools/apkg/toml_parser.c` used internally by the `ae` CLI to read `aether.toml` project files. It's not exposed as `std.toml`. If a project needs TOML, copying that parser or shelling out to a host-language tool are the options today.
- **INI**, no parser. Trivial to implement on top of `string.split` if needed.
- **Java-style `.properties`**, no parser. Same shape as INI without sections; same advice.
- **CSV**, no parser. `string.split(line, ",")` covers the no-quoting / no-embedded-commas case; anything more needs a real CSV parser, which isn't shipped.
- **Protocol Buffers / MessagePack / CBOR / Avro / Thrift**, no codecs. Same reflection-gap reasoning as struct ↔ JSON: without struct introspection there's no automatic encode/decode.

This isn't a hidden roadmap, these are absent because no downstream user has driven the need yet. If you're starting a project that needs YAML config, expect to write a parser, ship a contrib module, or shell out. Structured-data thinking in the stdlib is currently JSON-shaped and HTTP-adjacent; broader format coverage is open territory.

---

## XML (`std.xml`)

A deliberately small XML surface: a **pull/SAX reader** and an
**escaping builder**. Enough for S3 / SOAP-ish / config XML. **Not** in
scope: XSD, XPath, namespaces, DTD validation, or custom entity
definitions, the five predefined entities (`&amp; &lt; &gt; &quot;
&apos;`) and numeric character references (`&#NN;` / `&#xHH;`) are decoded;
CDATA is passed through raw; the prolog, comments, and processing
instructions are skipped.

```aether
import std.xml

main() {
    // ---- Reader: pull one event at a time ----
    p = xml.parser("<Result><Key>a &lt;b&gt;.txt</Key><Empty/></Result>")
    done = 0
    while done == 0 {
        ev = xml.next(p)
        if ev == xml.EVENT_EOF { done = 1 }
        else if ev == xml.EVENT_ERROR { println(xml.error(p))  done = 1 }
        else if ev == xml.EVENT_START { println("start ${xml.name(p)}") }
        else if ev == xml.EVENT_TEXT  { println("text  ${xml.text(p)}") }  // entity-decoded
        else if ev == xml.EVENT_END   { println("end   ${xml.name(p)}") }
    }
    xml.free(p)

    // ---- Builder: escaping handled for you ----
    b = xml.writer()
    xml.declaration(b)
    xml.start(b, "Error")
    xml.attribute(b, "code", "NoSuchKey")
    xml.element(b, "Message", "the key \"x\" & <y> are gone")
    xml.end(b, "Error")
    out = xml.finish(b)        // <?xml ...?><Error code="NoSuchKey"><Message>...escaped...</Message></Error>
    xml.free_builder(b)
    println(out)
}
```

**Event kinds** (returned by `xml.next`): `EVENT_START`, `EVENT_END`,
`EVENT_TEXT`, `EVENT_EOF`, `EVENT_ERROR`.

**Reader:**
- `xml.parser(data)` → `ptr` new pull reader (free with `xml.free`)
- `xml.next(p)` → `int` advance; returns an `EVENT_*`
- `xml.name(p)` → `string` element name for the current START/END
- `xml.text(p)` → `string` entity-decoded character data for the current TEXT
- `xml.attr(p, key)` → `string` attribute value on the current START (`""` if absent)
- `xml.attr_count(p)` / `xml.attr_name(p, i)` / `xml.attr_value(p, i)` iterate attributes
- `xml.error(p)` → `string` message after an `EVENT_ERROR`
- `xml.free(p)` release the reader

A self-closing `<tag/>` yields a START immediately followed by an END.
The name/text/attr values are owned copies, safe to hold across `next`.

**Builder:**
- `xml.writer()` → `ptr` new builder (named `writer` because `builder` is a keyword; free with `xml.free_builder`)
- `xml.declaration(b)` emit `<?xml version="1.0" encoding="UTF-8"?>`
- `xml.start(b, name)` / `xml.attribute(b, name, value)` / `xml.end(b, name)`
- `xml.text_node(b, content)` append escaped character data
- `xml.element(b, name, content)` `<name>escaped</name>` in one call
- `xml.finish(b)` → `string` the document (escaping already applied)
- `xml.escape(s)` → `string` escape the five predefined entities (rarely needed directly)

---

## Cryptography (`std.cryptography`)

Hash digests + Base64 codec. Pure functions, bytes in, hex digest
or Base64 string out, binary-safe via an explicit byte length
(embedded NULs are fine; pass 0 to hash or encode an empty buffer).

Built on OpenSSL's EVP API, which is already linked for `std.net`'s
TLS support. When the Aether toolchain was built without OpenSSL,
the wrappers return `("", "openssl unavailable")` rather than
crashing, callers should always check the error slot.

```aether
import std.cryptography
import std.encoding
import std.fs

main() {
    // Text payload, length is explicit.
    digest, err = cryptography.sha256_hex("abc", 3)
    // digest == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"

    // Algorithm chosen at runtime (e.g. read from a config file).
    algo = "sha256"
    if cryptography.hash_supported(algo) == 1 {
        d, _ = cryptography.hash_hex(algo, "abc", 3)
    }

    // Base64, round-trip a binary payload through JSON.
    b64      = encoding.base64_encode("\x01\x02\x03", 3)       // "AQID"
    raw, _   = encoding.base64_decode(b64)                     // 3 bytes
}
```

**Hash functions:**
- `cryptography.sha1_hex(data, length)` → `(string, string)` - 40-char lowercase hex digest. Included for interop with legacy formats (Git, Subversion, HMAC-SHA1). Prefer SHA-256 for new work.
- `cryptography.sha256_hex(data, length)` → `(string, string)` - 64-char lowercase hex digest.
- `cryptography.hash_hex(algo, data, length)` → `(string, string)` - Algorithm-by-name dispatcher. `algo` is `"sha1"`, `"sha256"`, or any other name OpenSSL's `EVP_get_digestbyname()` recognizes (`"sha384"`, `"sha512"`, `"sha3-256"`, ...). Returns `("", "unknown algorithm")` for unrecognized names. Useful when the algorithm is config-driven rather than compile-time.
- `cryptography.hash_supported(algo)` → `int` - `1` if this build can compute `algo`, `0` otherwise. Always succeeds; never errors. Use at config time to validate user-supplied algorithm names before they hit `hash_hex`.
- `cryptography.md4_hex(data, length)` / `md5_hex(data, length)` → `(string, string)` - 32-char lowercase hex digest. Legacy interop only (Content-MD5, ETag, zsync, pre-SHA1 fixtures), NOT collision-resistant, do not use for security. `("", error)` on failure.

**Raw-bytes digests** (same `(bytes, length, error)` tuple as `base64_decode`; `bytes` is an owned AetherString preserving embedded NULs, use when the wire format wants a fixed-width binary digest rather than hex):
- `cryptography.sha1_bytes(data, length)` / `sha256_bytes(data, length)` → `(string, int, string)`.
- `cryptography.md4_bytes(data, length)` / `md5_bytes(data, length)` → `(string, int, string)`.
- `cryptography.hash_bytes(algo, data, length)` → `(string, int, string)` - Algorithm-by-name binary digest. `("", 0, "unknown algorithm")` for unrecognized names.

**HMAC-SHA256** (RFC 2104 / FIPS 198-1; RFC 4231 test vectors):
- `cryptography.hmac_sha256_hex(key, key_len, msg, msg_len)` → `(string, string)` - Hex digest. Natural shape for opaque-token signing and bearer-token derivation.
- `cryptography.hmac_sha256_bytes(key, key_len, msg, msg_len)` → `(string, int, string)` - Raw 32-byte digest. Use for chained key derivation (SigV4 / HKDF-shaped flows) where each round's output keys the next.

**Cryptographically-secure random** (OS CSPRNG, `getrandom(2)` / `/dev/urandom` on Linux, `arc4random_buf(3)` on macOS/BSD):
- `cryptography.random_bytes(n)` → `(string, int, string)` - `n` random bytes as `(bytes, length, error)`; `bytes` preserves embedded NULs.
- `cryptography.random_hex(n)` → `(string, string)` - `n` random bytes as a `2*n`-char lowercase-hex string. For opaque bearer-token / API-key minting.
- `cryptography.random_base64(n)` → `(string, string)` - `n` random bytes as RFC 4648 §4 unpadded Base64 (~33% denser than hex).

**Streaming (incremental) digest** - Hash data that arrives in pieces without ever holding it whole (streaming an upload to disk in fixed windows, S3-style multipart ETags). The two `final` variants free the context; call `digest_free` only when bailing out before finalizing.
- `cryptography.digest_new(algo)` → `(ptr, string)` - Open a context for `algo` (`"md5"`, `"sha256"`, `"sha1"`, `"md4"`, or any name OpenSSL recognizes). `(null, "unknown algorithm")` / `(null, "openssl unavailable")` on failure.
- `cryptography.digest_update(ctx, data, length)` → `(int, string)` - Feed `length` bytes; `(1, "")` on success, `(0, error)` on failure. Binary-safe. Does NOT free the context.
- `cryptography.digest_final_hex(ctx)` → `(string, string)` - Finalize to lowercase hex. FREES `ctx`.
- `cryptography.digest_final_bytes(ctx)` → `(string, int, string)` - Finalize to raw digest bytes. FREES `ctx`.
- `cryptography.digest_free(ctx)` - Abandon a context without finalizing (NULL-safe). Only for the bail-out-before-final case.

**Base64 (RFC 4648 §4 standard alphabet):**
- `encoding.base64_encode(data, length)` → `string` - Encode `length` bytes, **unpadded** output.
- `encoding.base64_encode_padded(data, length)` → `string` - Encode `length` bytes, **with `=` padding** to a multiple of 4. Reach for this when the wire format on the other end requires padding; most non-strict decoders accept either.
- `encoding.base64_decode(b64)` → `string!` - Decode, destructured as `(bytes, err)`. `err` is non-empty on malformed input. Accepts both padded and unpadded input; `bytes` is an AetherString preserving embedded NULs.

Base64 lives in `std.encoding`, not `std.cryptography`: encoding is not a
security primitive, and the split keeps that honest. `std.cryptography` keeps
`random_base64`, which is crypto-random bytes rendered as base64.

**What `std.cryptography` doesn't do:**

Coming from Java's `java.security`, Python's `cryptography`, or Go's `crypto/*`, expect to reach for an external library if you need:

- **Public-key crypto (RSA, ECDSA, Ed25519, X25519), symmetric ciphers (AES, ChaCha20-Poly1305), and key derivation (KDFs).** These live under [`std.cryptography`](../std/cryptography/) (`rsa`, `aes`, `chacha20poly1305`, `ed25519`, `x25519`, `p256`, `secp256k1`, `pem`, `asn1`, ...), pure-Aether ports with no OpenSSL dependency. Each family is a separate sub-module you import explicitly.
- **URL-safe Base64 (RFC 4648 §5).** Standard alphabet only; URL-safe (`-` / `_` instead of `+` / `/`) is a separate variant the wrappers don't expose.
- **Constant-time comparison.** Equality checks via `string.equals` are not constant-time; callers comparing hashes for security-sensitive cases need their own constant-time helper.

Raw externs: `cryptography_sha1_hex_raw`, `cryptography_sha256_hex_raw` return allocated `char*` or NULL on failure. The Go-style wrappers translate the NULL into `("", "openssl unavailable")`.

Public-key crypto, symmetric ciphers, and key derivation live under `std.cryptography` as explicitly-imported sub-modules (e.g. `std.cryptography.rsa`, `std.cryptography.x25519`, `std.cryptography.aes`), pure-Aether ports, no OpenSSL. The top-level `std.cryptography` module stays focused on the hash/HMAC/Base64/CSPRNG primitives with a single obvious shape.

---

## POSIX ustar archives (`std.tar`)

`std.tar` reads, writes, and safely extracts uncompressed POSIX ustar archives.
It supports regular files, directories, symbolic links, permission modes,
modification times, ustar prefix/name splitting, header checksums, payload
padding, and the two-record end marker. Reader payloads and writer source files
are transferred in bounded chunks; no entry-sized allocation is made.

```aether
import std.tar

main() {
    writer, err = tar.writer_create("release.tar")
    if err != "" { return }
    err = tar.writer_add_file(writer, "bin/app", "build/app")
    if err != "" {
        tar.writer_abort(writer)
        return
    }
    tar.writer_finish(writer)

    opts = tar.default_extract_options()
    err = tar.extract("release.tar", "unpacked", opts)
}
```

The low-level reader API is `reader_open`, `reader_next`, the `entry_*`
metadata accessors, `entry_read`, `entry_skip`, and `reader_close`.
`entry_read(reader, max_bytes)` returns a binary-safe `(buffer, length, err)`;
the caller releases every non-null buffer with `string.release`. Entry metadata
is borrowed from the reader and remains valid only until the next
`reader_next` call or `reader_close`. Calling `reader_next` skips any unread
payload and its padding.

The writer API is `writer_create`, `writer_add_file`, `writer_add_directory`,
`writer_add_symlink`, `writer_finish`, and `writer_abort`. Source files are
streamed. `writer_finish` writes both zero records and consumes the writer on
success; call `writer_abort` after an earlier add failure to close the handle
and remove the incomplete output.

`default_extract_options()` disables symlinks and overwriting, does not restore
mode or mtime, and applies conservative entry, per-entry byte, and total-byte
limits. `extract` rejects absolute, drive-qualified, UNC, and root-escaping
paths; refuses parents that are symlinks; validates enabled symlink targets;
and delays directory metadata until children have been created. Reading needs
filesystem-read capability; writing and extraction need filesystem-write
capability under the normal `--emit=lib` sandbox checks.

This is deliberately POSIX ustar support, not general TAR support. The iterator
reports hard links, devices, FIFOs, GNU/PAX records, sparse entries, base-256
numbers, and unknown type flags as `KIND_OTHER`; extraction rejects them rather
than treating them as files. Invalid UTF-8 names are rejected. Gzip, zstd, xz,
and bzip2 are separate formats/layers and are not inferred from suffixes.
Symlink creation and metadata preservation remain platform-dependent; no
ownership, setuid/setgid, device-node, or FIFO restoration is attempted.

---

## Compression (`std.zlib`)

One-shot zlib deflate/inflate for in-memory byte buffers. Output is
a length-aware AetherString plus explicit byte count (matching
`fs.read_binary`'s shape), so binary payloads with embedded NULs
round-trip intact.

Under the hood `deflate` uses `compress2` with `compressBound`
sizing; `inflate` uses streaming `inflate()` with a geometric-grow
output buffer so callers don't need to know the decompressed size in
advance.

Auto-detects zlib via pkg-config (same pattern as OpenSSL). When
absent, the wrappers return `("", 0, "zlib unavailable")` rather
than crashing.

```aether
import std.zlib
import std.fs
import std.string

main() {
    // Compress a text payload at the default level (-1).
    msg = "Hello, zlib. Repetition repetition repetition."
    n_in = string.length(msg)
    compressed, nc, cerr = zlib.deflate(msg, n_in, -1)

    // Round-trip back to the original bytes. `inflate` doesn't need
    // to be told the decompressed size, it grows as needed.
    out, nu, uerr = zlib.inflate(compressed, nc)

    // Binary payloads work the same way: fs.read_binary gives a
    // length-aware AetherString; the extern unwraps it before
    // feeding the bytes to zlib.
    data, nd, _ = fs.read_binary("payload.bin")
    blob, nb, _ = zlib.deflate(data, nd, 9)  // level 9 = best
}
```

**Functions:**
- `zlib.deflate(data, length, level)` → `(string, int, string)` - Compress the first `length` bytes of `data` at `level` (0..9, or -1 for default). Out-of-range levels are clamped to default. Returns `(bytes, byte_count, "")` on success, `("", 0, error)` on failure.
- `zlib.inflate(data, length)` → `(string, int, string)` - Decompress a zlib stream (RFC 1950). Returns `(bytes, byte_count, "")` on success, `("", 0, error)` on corruption, truncation, or empty input.

Gzip-framed helpers for HTTP `Content-Encoding: gzip` are also available: `zlib.gzip_deflate(data, length, level)` and `zlib.gzip_inflate(data, length)`. Streaming APIs remain out of scope for v1, additive future work under the same module. See [stdlib-vs-contrib.md](stdlib-vs-contrib.md) for the "one obvious shape" criterion.

---

## Networking

### HTTP (`std.http`)

> **Note:** Use `import std.http` for the `http.*` prefix shown below. You can also `import std.net` which includes both HTTP and TCP functions, but the namespace prefix becomes `net` e.g. the raw client extern is reached as `net.http_get_raw(url)`, and the Go-style wrapper as `net.get(url)`.

```aether
import std.http

main() {
    // HTTP Client, Go-style
    body, err = http.get("http://example.com")
    if err != "" {
        println("failed: ${err}")
        return
    }
    println("got: ${body}")

    // HTTP Server
    server = http.server_create(8080)
    berr = http.server_bind(server, "127.0.0.1", 8080)
    if berr != "" {
        println("bind failed: ${berr}")
        return
    }
    serr = http.server_start(server)
    if serr != "" { println("start failed: ${serr}") }
    http.server_free(server)
}
```

**Client (Go-style):**
- `http.get(url)` → `(string, string)` - HTTP GET, returns `(body, err)`
- `http.get_with_timeout(url, timeout)` → `(string, string)` - HTTP GET with a `Duration` per-call timeout. `0ns` keeps `get`'s "block forever" default; positive values are rounded up to whole seconds internally today. For any third-party URL, without a timeout, a hung site stalls the calling actor's whole message handler.
- `http.post(url, body, content_type)` → `(string, string)` - HTTP POST
- `http.put(url, body, content_type)` → `(string, string)` - HTTP PUT
- `http.delete(url)` → `(string, string)` - HTTP DELETE

All wrappers auto-free the underlying response and return an error string for transport failures or non-2xx status codes. Raw externs: `http_get_raw`, `http_get_with_timeout_raw`, `http_post_raw`, `http_put_raw`, `http_delete_raw`.

**Response accessors (used with raw externs):**
- `http.response_status(response)` - Read HTTP status code (0 on transport failure)
- `http.response_body(response)` - Read body as string
- `http.response_headers(response)` - Read headers as string
- `http.response_error(response)` - Read transport error, empty string on success
- `http.response_ok(response)` - 1 if request succeeded (no transport error, 2xx status), else 0
- `http.response_free(response)` - Free response

**Server Lifecycle:**
- `http.server_create(port)` - Create server (never fails)
- `http.server_set_host(server, host)` - Set bind address before `server_start`. Default is `"0.0.0.0"`. Pass `"127.0.0.1"` to bind loopback only, useful in tests because macOS / Windows firewalls don't prompt on loopback binds.
- `http.server_bind(server, host, port)` → `string` - Bind to address, return error string
- `http.server_start(server)` → `string` - Start serving (blocking), return error string
- `http.server_stop(server)` - Stop server
- `http.server_free(server)` - Free server

Raw externs: `http_server_bind_raw`, `http_server_start_raw`, `http_server_set_host`.

**Static file serving:**
- `http.serve_file(res, filepath)` - Serve a single file. Zero-copy: under HTTP/1.1 cleartext on Linux/macOS, takes the `sendfile(2)` fast path (zero heap allocation for the body); falls back to a buffered read for TLS / HTTP/2 / Range requests / Windows. `Content-Type` resolved via `http.mime_type(filepath)`.
- `http.serve_static(req, res, base_dir)` - Wildcard-route static-file dispatcher. Path traversal (`..`, `%2e`, etc.) is rejected with 403; missing files return 404.

**Server Routing:**
- `http.server_get(server, path, handler, user_data)` - Register GET route
- `http.server_post(server, path, handler, user_data)` - Register POST route
- `http.server_put(server, path, handler, user_data)` - Register PUT route
- `http.server_delete(server, path, handler, user_data)` - Register DELETE route
- `http.server_use_middleware(server, middleware, user_data)` - Add middleware

**Server Configuration:**
- `http.server_set_tls(server, cert_path, key_path)` → `string` - Enable HTTPS with PEM cert + key.
- `http.server_set_keepalive(server, enable, max_requests, idle_timeout)` → `string` - HTTP/1.1 keep-alive with a `Duration` idle timeout (`max_requests=0` is unlimited per connection).
- `http.server_set_h2(server, max_concurrent_streams)` → `string` - Enable HTTP/2 (h2 + h2c + ALPN). `max_concurrent_streams=0` uses libnghttp2's default (100). Returns error string when the build is missing libnghttp2.
- `http.server_set_h2_concurrent_dispatch(server, worker_count)` → `string` - Routes h2 stream handlers onto the shared `std.worker` thread pool, sized to `worker_count`. `worker_count > 0` lets streams across all h2 connections execute their handlers in parallel; `worker_count == 0` (default) keeps dispatch sequential on each connection thread. POSIX-only; on Windows the call is a silent no-op. See `docs/http-server.md` for the architecture rationale (pool threads vs actors, one shared pool vs per-connection).
- `http.server_shutdown_graceful(server, timeout)` → `string` - Stop accepting new connections, drain in-flight requests for up to a `Duration`, exit. h2 sessions emit a `GOAWAY` frame so peers know not to start new streams while existing ones complete.
- `http.server_set_health_probes(server, live_path, ready_path, ready_check, ud)` → `string` - Built-in `/healthz` (always 200) + `/readyz` (200 only when the readiness check returns 1).
- `http.server_set_access_log(server, format, output_path)` → `string` - Built-in access logger. `format` is `"combined"` or `"json"`; `output_path` is a file path, `"-"` for stderr, or `""` to disable.
- `http.server_set_metrics(server, endpoint)` → `string` - Prometheus-compatible counters/histograms at the configured endpoint (default `"/metrics"`).

**Request Accessors:**
- `http.request_method(req)` → `string` - HTTP method (`GET`, `POST`, `PUT`, …); empty if `req` is null.
- `http.request_path(req)` → `string` - URL path (no query string); empty if `req` is null.
- `http.request_body(req)` → `string` - Request body as a C-string. **Truncates at the first embedded NUL** when read via `string.length(...)`; pair with `http.request_body_length` for binary-safe access. On a large (streaming) request the first call materializes the body, it drains the remaining wire bytes into one buffer, preserving the v1 whole-body contract at the O(Content-Length) cost the caller asked for. Don't mix it with `request_body_read` on the same request (the consumed prefix is gone; the mixed call returns `""`).
- `http.request_body_length(req)` → `int` - Byte count of the request body. Returns 0 if `req` is null or has no body. Reach for this whenever the body may contain NUL bytes (svn PUT, image uploads, gzipped JSON), the length-aware companion to `http.request_body`. On a streaming request this is the declared `Content-Length` until the body is materialized, then the actual received count.
- `http.request_body_read(req, offset, max)` → `(bytes, n, err)` - Chunked body read. Bodies ≤ 16 KiB are pre-buffered (random-access offsets); larger bodies are **streamed**, the handler is dispatched at headers-complete and each read pulls the next window straight off the socket (sequential offsets only), so peak server RAM per upload is one window, not the object. Backpressure is TCP flow control itself: the server doesn't `recv` until the handler asks.
- `http.request_body_complete(req)` → `int` - 1 once every declared body byte has arrived (streaming: pulled off the wire; buffered: always 1). The natural chunked-loop terminator. `Transfer-Encoding: chunked` request bodies remain unsupported (no `Content-Length` → length 0, no body), a deliberate v1 semantics decision.
- `http.request_query(req)` → `string` - Raw query string; empty if absent.
- `http.get_header(req, name)` - Get request header
- `http.get_query_param(req, name)` - Get query parameter
- `http.get_path_param(req, name)` - Get URL path parameter
- `http.request_free(req)` - Free request

**Response Building:**
- `http.response_create()` - Create response
- `http.response_set_status(res, code)` - Set HTTP status code
- `http.response_set_header(res, name, value)` - Set response header
- `http.response_set_body(res, body)` - Set response body. Uses `strdup` + `strlen` internally, **truncates at the first embedded NUL**. Fine for text bodies; use `response_set_body_n` for anything that may contain binary.
- `http.response_set_body_n(res, body, length)` - Length-aware sibling of `response_set_body`. Treats `body` as `length` bytes verbatim, no NUL searching. Reach for this when the body is binary content (gzip / image / packed binary) or may contain NUL bytes mid-payload. `length == 0` clears the body; negative length is a no-op.
- `http.response_json(res, json)` - Set JSON response
- `http.server_response_free(res)` - Free response

### HTTP Middleware (`std.http.middleware`)

Composable pre-handler middleware + response transformers. Each
middleware is a C function pointer registered on the server's
function-pointer chain, no Aether-side dispatch overhead in the
hot path. Aether-side factory wrappers allocate the per-middleware
config struct and register it.

```aether
import std.http
import std.http.middleware

main() {
    server = http.server_create(8080)

    // Order matters: real_ip first so downstream sees the client IP
    middleware.use_real_ip(server, "")                // default X-Forwarded-For

    middleware.use_cors(server, "*", "GET, POST", "Content-Type", 0, 600)

    middleware.use_rate_limit(server, 100, 60000)     // 100 req / 60s per IP

    middleware.use_bearer_auth(server, "api", verify_token, null)

    middleware.use_gzip(server, 256, 6)               // response transformer

    http.server_get(server, "/", handle_root, 0)
    http.server_start(server)
}
```

**Pre-handler middleware (run before route dispatch; can short-circuit):**
- `middleware.use_cors(server, allow_origin, allow_methods, allow_headers, allow_credentials, max_age_seconds)` → `string` - CORS headers + preflight OPTIONS short-circuit.
- `middleware.use_basic_auth(server, realm, verify_cb, ud)` → `string` - HTTP Basic auth (RFC 7617). Verifier receives decoded `(username, password)`.
- `middleware.use_bearer_auth(server, realm, verify_cb, ud)` → `string` - Bearer token auth (RFC 6750). Verifier receives the raw token; on failure emits `WWW-Authenticate: Bearer realm="…"` with `error="invalid_token"` for malformed credentials.
- `middleware.use_session_auth(server, cookie_name, redirect_url, verify_cb, ud)` → `string` - Session-cookie auth. Reads a named cookie, hands the value to the verifier; on failure either 401s (when `redirect_url` is empty) or 302s to the configured login URL.
- `middleware.use_rate_limit(server, max_requests, window_ms)` → `string` - Token-bucket per-client-IP rate limit. Client IP comes from `X-Forwarded-For` → `X-Real-IP` → `"anonymous"` resolution chain.
- `middleware.use_vhost(server, hosts_csv)` → `string` - Host-header gate. Comma-separated allowed hosts; unknown hosts get 404.
- `middleware.use_real_ip(server, header_name)` → `string` - Proxy-aware client IP detection. Reads the configured header (default `X-Forwarded-For`), takes the leftmost non-empty IP, adds `X-Real-IP` to the request. Idempotent. **Trust model: only safe behind a proxy that strips client-supplied X-Forwarded-For.**
- `middleware.use_static_files(server, url_prefix, root)` → `string` - Mount a directory under a URL prefix; `..` traversal blocked.
- `middleware.use_rewrite(server, opts)` → `string` - Prefix-rewrite rules; build via `middleware.rewrite_add_rule(opts, from, to)`.

**Response transformers (run after the route handler emits the response):**
- `middleware.use_gzip(server, min_size, level)` → `string` - Gzip the response body when the client sends `Accept-Encoding: gzip` and the body is at least `min_size` bytes; level 1 (fastest) – 9 (best), 0 = default 6.
- `middleware.use_error_pages(server, opts)` → `string` - Replace error-status response bodies with operator-supplied content; build via `middleware.error_pages_register(opts, status_code, body, content_type)`.

### HTTP Reverse Proxy (`std.http.proxy`)

nginx-class outbound HTTP forwarding. Forwards inbound requests
to a pool of upstream HTTP servers with five load-balancing
algorithms, active health checks, in-memory LRU response cache,
per-upstream circuit breaker, idempotent retry with `proxy_next_upstream`
semantics, per-upstream token-bucket rate limit, active drain,
W3C Trace-Context propagation, Prometheus 0.0.4 metrics, and
Hop-by-Hop header handling per RFC 7230.

```aether
import std.http
import std.http.proxy

// Convenience: single upstream, RR, default opts.
proxy.mount_simple(server, "/", "http://localhost:9000", 30)

// Production: pool + LB + health + cache + breaker + retry + rate-limit + metrics.
pool = proxy.upstream_pool_new("weighted_rr", 30, 0, 100)
proxy.upstream_add(pool, "http://10.0.0.1:8080", 3)
proxy.upstream_add(pool, "http://10.0.0.2:8080", 1)
proxy.health_checks_enable(pool, "/health", 200, 5000, 1000, 2, 3)
proxy.breaker_configure(pool, 5, 30000, 1)
proxy.rate_limit_set(pool, 200, 50)
cache = proxy.cache_new(1000, 65536, 60, "method_url_vary")
opts  = proxy.opts_new()
proxy.opts_bind_cache(opts, cache)
proxy.opts_set_retry_policy(opts, 3, 100)
proxy.opts_set_trace_inject(opts, 1)
proxy.mount(server, "/api", pool, opts)
```

**Pool + LB:**
- `proxy.upstream_pool_new(lb_algo, request_timeout_sec, dial_timeout_ms, max_inflight_per_up)` → `ptr` - LB algos: `"round_robin"`, `"least_conn"`, `"ip_hash"`, `"weighted_rr"`, `"cookie_hash"`.
- `proxy.upstream_pool_free(pool)` - Decrements refcount; joins health-check thread on zero.
- `proxy.upstream_add(pool, base_url, weight)` → `string`
- `proxy.upstream_remove(pool, base_url)` → `string`
- `proxy.upstream_drain(pool, base_url)` → `string` - skip in LB; in-flight finish.
- `proxy.upstream_undrain(pool, base_url)` → `string` - re-admit to LB.
- `proxy.pool_set_cookie_name(pool, name)` → `string` - cookie key for `cookie_hash` algo.
- `proxy.rate_limit_set(pool, max_rps, burst)` → `string` - per-upstream token-bucket rate limit. `max_rps = 0` disables.

**Health checks (one pthread per pool):**
- `proxy.health_checks_enable(pool, probe_path, expect_status, interval_ms, timeout_ms, healthy_threshold, unhealthy_threshold)` → `string`

**Circuit breaker (per-upstream state, per-pool config):**
- `proxy.breaker_configure(pool, failure_threshold, open_duration_ms, half_open_max)` → `string` - `failure_threshold = 0` disables.

**Cache (in-memory LRU + TTL):**
- `proxy.cache_new(max_entries, max_body_bytes, default_ttl_sec, key_strategy)` → `ptr` - Key strategy: `"url"`, `"method_url"`, `"method_url_vary"`. RFC 7234 cacheability gates with Vary-aware lookup.
- `proxy.cache_free(cache)`

**Per-mount options:**
- `proxy.opts_new()` → `ptr`
- `proxy.opts_set_strip_prefix(opts, prefix)` → `string` - chops a path prefix before forwarding (e.g. `/api/users` → `/users` upstream).
- `proxy.opts_set_preserve_host(opts, on)` → `string` - 0 (default) rewrites Host: to upstream; 1 forwards client Host: verbatim.
- `proxy.opts_set_xforwarded(opts, xff, xfp, xfh)` → `string` - toggles X-Forwarded-{For, Proto, Host} injection (defaults all on).
- `proxy.opts_bind_cache(opts, cache)` → `string`
- `proxy.opts_set_body_cap(opts, max_body_bytes)` → `string` - default 8 MiB.
- `proxy.opts_set_retry_policy(opts, max_retries, backoff_base_ms)` → `string` - retry idempotent methods on 5xx + transport with exponential backoff + full jitter; re-picks per attempt.
- `proxy.opts_set_trace_inject(opts, on)` → `string` - 0 (default) passthrough W3C traceparent; 1 generates a fresh trace when missing.
- `proxy.opts_free(opts)`

**Install:**
- `proxy.mount(server, path_prefix, pool, opts)` → `string` - mount the proxy under `path_prefix`. `"/"` forwards everything; `"/api"` forwards just the `/api` subtree.
- `proxy.mount_simple(server, path_prefix, upstream_url, request_timeout_sec)` → `string` - one-upstream convenience.

**Observability:**
- `proxy.pool_metrics_text(pool)` → `string` - Prometheus 0.0.4 exposition (per-upstream + per-pool counters / gauges).

See [`docs/http-reverse-proxy.md`](http-reverse-proxy.md) for the full reference (LB algorithms, retry semantics, error responses, performance budget, limitations).

### HTTP Client Builder (`std.http.client`)

The `http.get` / `http.post` / `http.put` / `http.delete` one-liners above are good for "no auth, JSON in, 200 means good" calls. Reach for `std.http.client` when you need custom request headers, response-header capture, status discrimination, per-request timeouts, or methods other than the four common verbs (PROPFIND, PATCH, custom RPC verbs all work).

Non-2xx is **not** an error from `send_request`'s perspective, the caller branches on `response_status`. Transport-level failures (DNS, connect, TLS handshake, timeout) populate the `err` slot.

```aether
import std.http
import std.http.client

main() {
    req = client.request("GET", "https://api.example.com/users/42")
    client.set_header(req, "Authorization", "Bearer abc123")
    client.set_header(req, "Accept",        "application/json")
    client.set_timeout(req, 30s)

    resp, err = client.send_request(req)
    client.request_free(req)
    if err != "" {
        println("transport: ${err}")
        return
    }

    status = client.response_status(resp)        // 200, 404, ...
    body   = client.response_body(resp)          // binary-safe AetherString
    etag   = client.response_header(resp, "ETag") // case-insensitive lookup
    client.response_free(resp)
}
```

**Builder + send:**
- `client.request(method, url)` → `ptr` - Build a request handle (method as arbitrary string)
- `client.set_header(req, name, value)` → `string` - Append `Name: value` to outgoing headers
- `client.set_body(req, body, length, content_type)` → `string` - Set request body (length explicit so binary payloads with embedded NULs survive)
- `client.set_timeout(req, timeout)` → `string` - `Duration` per-request timeout (`0ns` = block forever)
- `client.set_follow_redirects(req, max_hops)` → `string` - Follow up to `max_hops` redirects (`0` = don't follow, the default)
- `client.send_request(req)` → `(ptr, string)` - Fire the request; returns `(resp, "")` on transport success or `(null, err)` on failure
- `client.set_stream(req, on)` → `string` - Enable streaming of the response body for this request; `send_stream` is the convenience form
- `client.send_stream(req)` → `(ptr, string)` - Like `send_request`, but the response body is streamed rather than buffered (see below)
- `client.request_free(req)` - Free the request handle

**TLS + forward proxy (per request):** the client is hardened by default, TLS
peer verification is **on** and env proxies are **not** followed unless you opt
in. These knobs relax that per request; precedence for proxy is
`ignore > explicit > env > direct`.
- `client.set_insecure(req, on)` → `string` - `1` skips TLS peer + hostname verification for this request only (`curl -k` / `wget --no-check-certificate`). Relaxed per-connection, never on the shared process-wide `SSL_CTX`, so other requests keep verifying. Default `0`. Use only against hosts trusted out-of-band (self-signed dev/staging/appliance certs), it removes MITM protection for that request.
- `client.set_cafile(req, path)` → `string` - pin a custom CA for this request: verify the peer against the PEM bundle at `path` instead of the system trust store, while **keeping peer and hostname verification on**. This is the "verify, but against THIS cert" knob, strictly stronger than `set_insecure`, for machine-to-machine calls to a host with a private/self-signed CA couriered out-of-band (courier the CA once over SSH, then pin it instead of blind-trusting). Per-connection via a per-`SSL` `X509_STORE`; never touches the shared `SSL_CTX`. `""` clears the pin (revert to the system store). A certificate the pinned CA doesn't cover fails the handshake, fails closed, never open.
- `client.use_env_proxy(req, on)` → `string` - `1` follows `$HTTP_PROXY`/`$HTTPS_PROXY`/`$NO_PROXY` (Go-compatible). **Off by default**, the deliberate inverse of the default-follow that caused the httpoxy vulnerability class (CVE-2016-5385). It is a code-visible opt-in, never ambient, and carries two guards: the CGI-injectable uppercase `HTTP_PROXY` is refused when `$REQUEST_METHOD`/`$GATEWAY_INTERFACE` is set (lowercase `http_proxy` stays honoured), and a proxy resolving to a loopback/link-local IP literal (127/8, 169.254/16 IMDS, `::1`, `fc00::/7`, `fe80::/10`) is rejected (SSRF).
- `client.use_http_proxy(req, "http://host:port")` → `string` - Pin an explicit forward proxy; env is ignored entirely (empty url reverts to direct). A team-controlled proxy (recorder / toxiproxy) is thus immune to whatever the shell or CI has set. HTTP goes through the proxy with an absolute-form request line; HTTPS establishes a `CONNECT` tunnel with TLS end-to-end to the origin.
- `client.ignore_http_proxy(req)` → `string` - Force a direct connection regardless of env or any proxy a higher layer set (the determinism escape hatch, e.g. a VCR that must record the origin, not a proxy's view).

**Response accessors:**
- `client.response_status(resp)` → `int` - HTTP status code
- `client.response_body(resp)` → `string` - Response body, binary-safe (buffered responses)
- `client.response_is_stream(resp)` → `int` - 1 if the body is streamed (from `send_stream`), 0 if buffered
- `client.response_read(resp, max)` → `string` - Pull the next decoded body window (up to `max` bytes) from a streaming response; empty result = end-of-body (streaming)
- `client.response_header(resp, name)` → `string` - Case-insensitive single-header lookup, `""` if absent
- `client.response_headers(resp)` → `string` - Raw header block
- `client.response_error(resp)` → `string` - Transport error string
- `client.response_free(resp)` - Free the response (closes the connection for a streaming response)

**Sugar wrappers** (pure Aether on top of the builder, no new C externs):
- `client.get_with_headers(url, header_pairs)` → `(string, int, string)` - GET with auth/whatever headers; returns `(body, status, err)`
- `client.post_with_status(url, body, content_type)` → `(string, int, string)` - POST and inspect status
- `client.post_json(url, value)` → `(ptr, string)` - Marshal a JSON value (`std.json`), set `Content-Type` + `Accept` to `application/json`, send
- `client.response_body_json(resp)` → `(ptr, string)` - Wrap `response_body` + `json.parse`; returns `(value, "")` on success or `(null, parse_error)` on malformed JSON

**Streaming large response bodies:** `send_request` materialises the whole body into one `AetherString`, fine for JSON APIs, but for a multi-megabyte download that is O(Content-Length) memory. `send_stream` instead reads only the header block, keeps the connection open, and hands back a response you drain window-by-window with `response_read`, so peak memory is one window regardless of body size. `Content-Length` and `Transfer-Encoding: chunked` bodies are both decoded transparently (you always see payload bytes, never chunk framing). Redirects are still followed if enabled; only the final hop streams. Always `response_free` the response when done (it closes the connection), even if you stop reading early.

```aether
import std.http
import std.http.client

main() {
    req = client.request("GET", "https://example.com/big.iso")
    client.set_timeout(req, 60s)
    resp, err = client.send_stream(req)     // reads headers only; body stays on the wire
    client.request_free(req)
    if err != "" { println("transport: ${err}"); return }

    // Drive the body in windows; peak memory is one chunk, not the whole file.
    done = 0
    while done == 0 {
        chunk = client.response_read(resp, 65536)   // up to 64 KiB of decoded body
        if string.length(chunk) == 0 {
            done = 1                                 // end-of-body (or error, see below)
        } else {
            // ... write chunk to disk, hash it, forward it, ...
        }
    }
    // An empty chunk means EOF *or* a mid-stream failure; disambiguate here.
    serr = client.response_error(resp)
    client.response_free(resp)                       // closes the connection
    if serr != "" { println("stream error: ${serr}") }
}
```

Design choices: `method` is an arbitrary string, not a `{GET,POST,PUT,DELETE}` enum, so WebDAV / DeltaV / PATCH / project-specific verbs ride through without a stdlib release (the native client sends the method verbatim on the request line). A non-2xx status is not an error: `send_request` returns the response cleanly and the caller drives status interpretation, so 404/403/401 are distinguishable rather than collapsed to `"http error"`. The builder is named `send_request` rather than `send` because `send` is reserved for actor messaging. `tests/integration/test_http_client_v2.ae` is the runnable example for the buffered API; `tests/integration/http_client_stream/` and `http_client_stream_chunked/` cover streaming.

### HTTP record/replay (VCR), moved out of the stdlib

The Servirtium record/replay engine that used to ship as
`std.http.server.vcr` has been lifted into its own repository,
[`servirtium-vcr`](https://github.com/servirtium/servirtium-vcr),
now its authoritative home, alongside its language bindings. It is no
longer part of the Aether stdlib (it had served its purpose: shaping
Aether's HTTP server). See [`docs/http-vcr.md`](http-vcr.md) for the
pointer and history.

### TCP (`std.tcp`)

> **Note:** `send` and `receive` are reserved actor keywords in Aether, so
> the TCP byte-transfer wrappers are named `write`/`read`. The raw externs
> retain their `send_raw`/`receive_raw` names.

```aether
import std.tcp

main() {
    // Client, Go-style
    sock, cerr = tcp.connect("localhost", 8080)
    if cerr != "" { println("connect failed: ${cerr}"); return }

    _, werr = tcp.write(sock, "Hello")
    if werr != "" { println("write failed: ${werr}") }

    data, rerr = tcp.read(sock, 1024)
    if rerr == "" { println("got: ${data}") }
    tcp.close(sock)

    // Server
    server, lerr = tcp.listen(8080)
    if lerr != "" { return }
    client, aerr = tcp.accept(server)
    if aerr == "" {
        tcp.write(client, "Welcome")
        tcp.close(client)
    }
    tcp.server_close(server)
}
```

**Functions (Go-style):**
- `tcp.connect(host, port)` → `(ptr, string)` - Connect, return `(socket, err)`
- `tcp.write(sock, data)` → `(int, string)` - Write text-shaped data, return `(bytes_sent, err)`. Uses the legacy strlen-shaped raw send; use `tcp.write_n` for binary payloads.
- `tcp.write_n(sock, data, length)` → `(int, string)` - Length-aware write, return `(bytes_sent, err)`. Sends exactly the caller-supplied byte prefix, preserving embedded NUL bytes.
- `tcp.read(sock, max)` → `(string, string)` - Read text-shaped data, return `(data, err)`. Use `tcp.read_n` for binary payloads.
- `tcp.read_n(sock, max)` → `(string, int, string)` - Binary-safe read, return `(bytes, length, err)`. The returned length is authoritative for payloads with embedded NUL bytes.
- `tcp.listen(port)` → `(ptr, string)` - Create server socket
- `tcp.accept(server)` → `(ptr, string)` - Accept connection
- `tcp.close(sock)` - Close socket (infallible)
- `tcp.server_close(server)` - Close server socket

Raw externs: `tcp_connect_raw`, `tcp_send_raw`, `tcp_send_n_raw`, `tcp_receive_raw`, `tcp_receive_n_raw`, `tcp_listen_raw`, `tcp_accept_raw`.

### Reactor-pattern async I/O (`await_io`)

Aether's runtime already owns a per-core I/O reactor (epoll on Linux,
kqueue on macOS/BSD, poll() elsewhere). `net.await_io` exposes that
reactor to Aether code so an actor can suspend on a file descriptor
*without blocking any scheduler thread*. When the fd becomes ready,
the scheduler delivers an `IoReady { fd, events }` message to the
actor's mailbox and resumes it on any available core.

```aether
import std.net

message IoReady { fd: int, events: int }
message Begin { fd: int }

actor Echo {
    state my_fd = 0
    receive {
        Begin(fd) -> {
            my_fd = fd
            err = net.await_io(fd)
            if err != "" {
                println("await_io failed: ${err}")
                exit(1)
            }
        }
        IoReady(fd, events) -> {
            // Resumed here, no OS thread was blocked while we waited.
            data, rerr = tcp.read(/*...*/)
            // ... process, then re-arm ...
            net.await_io(fd)
        }
    }
}
```

**The `IoReady` message name is reserved.** The runtime scheduler
delivers I/O-readiness notifications under a fixed message ID; the
Aether message registry assigns that same ID to any user message
named `IoReady` so your handler sees the event as a normal receive
arm.

| Function | Returns | Description |
|---|---|---|
| `net.await_io(fd)` | `string` | Register `fd` with the current core's I/O poller and suspend the calling actor. Returns `""` on success, error string on failure. One-shot: the fd is automatically unregistered after the next `IoReady` delivery. |
| `net.ae_io_cancel(fd)` |, | Abandon a pending `await_io` without waiting for the message. Rarely needed due to one-shot policy. |

**Constraints:**

- `await_io` must be called from inside an actor's `receive` handler
  (not from `main()`). The bridge reads the current actor from a TLS
  set at the top of every generated `_step()` function.
- Single-actor programs run in main-thread mode which bypasses the
  scheduler loop, and therefore the I/O reactor. Spawn at least two
  actors to force multi-threaded scheduler mode if you want `await_io`
  to function.
- The fd must outlive the `await_io` registration. If you close the
  fd before the `IoReady` fires, behavior depends on the backend
  (epoll reports EPOLLHUP; kqueue silently drops the one-shot).

**Performance:** a C-level benchmark demonstrated the raw
reactor pattern delivering substantially higher HTTP throughput than
the blocking keep-alive worker it replaced. `await_io` is the
Aether-language surface over the same runtime machinery, rerun the
HTTP benchmark on your own target host before relying on historical
numbers.

---

## Logging (`std.log`)

Structured logging with levels.

```aether
import std.log

main() {
    err = log.init("app.log", 0)  // 0 = LOG_DEBUG
    if err != "" {
        println("log file unavailable, falling back to stderr: ${err}")
    }

    log.write(0, "Debug message")
    log.write(1, "Info message")
    log.write(2, "Warning message")
    log.write(3, "Error message")

    log.print_stats()
    log.shutdown()
}
```

**Log Levels:**
- `0` = DEBUG
- `1` = INFO
- `2` = WARN
- `3` = ERROR
- `4` = FATAL

**Functions:**
- `log.init(filename, level)` → `string` - Initialize logging, return error string if the log file could not be opened (logging still works via stderr as a fallback)
- `log.shutdown()` - Shutdown logging
- `log.write(level, message)` - Write a log message at the given level
- `log.set_level(level)` - Set minimum level
- `log.set_colors(enabled)` - Enable/disable colored output (1/0)
- `log.set_timestamps(enabled)` - Enable/disable timestamps (1/0)
- `log.print_stats()` - Print logging statistics

Raw extern: `log_init_raw` (returns 1/0).

---

## OS (`std.os`)

Shell execution, command output capture, and environment variables.

```aether
import std.os

main() {
    // Run a shell command, get exit code
    code = os.system("echo hello")
    println("Exit: ${code}")

    // Capture command output, Go-style tuple return
    output, err = os.exec("date")
    if err != "" {
        println("exec failed: ${err}")
        return
    }
    println("Date: ${output}")

    // Get environment variable
    home = os.getenv("HOME")
    if home != 0 {
        println("HOME = ${home}")
    }
}
```

**Functions:**
- `os.system(cmd)` - Run shell command, returns exit code (0 = success, POSIX convention)
- `os.exec(cmd)` → `(string, string)` - Run command and capture stdout, return `(output, err)`
- `os.getenv(name)` - Get environment variable (returns string, or null if not set, infallible)
- `os.setenv(name, value)` → `string` - Set environment variable, returns "" on success or an error string. Same C-side function as `io.setenv` use `os.setenv` when you've already imported `std.os` for `os.getenv`.
- `os.unsetenv(name)` → `string` - Unset environment variable, returns "" on success or an error string. Same C-side function as `io.unsetenv`.
- `os.getpid()` → `int` - Process identifier of the current process. POSIX `getpid(2)`; Windows `_getpid()`. Useful for tmpfile names (`/tmp/myprog.${os.getpid()}.tmp`), per-process locks, log prefixes, and stable tagging across forked children. Returns 0 on platforms compiled without filesystem support.
- `os.user_id()` → `int` - Effective user id of the calling process (POSIX `geteuid(2)`). Windows has no numeric uid model and returns -1, so treat any negative result as "unavailable" rather than as a uid. Mainly for building per-user runtime paths like `/run/user/${os.user_id()}/`.
- `os.now_utc_iso8601()` → `string` - Current UTC time as ISO-8601 (`YYYY-MM-DDThh:mm:ssZ`). Returns `""` (never null) on clock/format failure. Thread-safe.
- `os.wall_seconds()` → `long` - Whole seconds since the Unix epoch (POSIX `gettimeofday`; Windows `GetSystemTimeAsFileTime`). NTP-jumpable, pair with `wall_micros` for sub-second precision, or use the monotonic accessors below for elapsed-time measurements.
- `os.wall_micros()` → `int` - Sub-second microsecond fraction (0..999999) from the same `struct timeval` as `wall_seconds`.
- `os.now_monotonic_ms()` → `long` - Monotonic clock, milliseconds since boot / process-start / arbitrary epoch. Value-domain is opaque; only *deltas* are meaningful. POSIX `clock_gettime(CLOCK_MONOTONIC)`; Windows `QueryPerformanceCounter`. Use for animation tick loops, frame-time budgets, microbenchmarks, anything that must survive a wall-clock jump.
- `os.now_monotonic_ns()` → `long` - Same source as `now_monotonic_ms`, nanosecond precision. Useful for sub-millisecond timing.
- `aether_args_count()` → `int` - Number of command-line arguments
- `aether_args_get(index)` → `string` - Get the i-th argument; null if out of range
- `aether_argv0()` → `string` - Path the OS launched the current process with (argv[0]); null before `aether_args_init` runs
- `os.argv0()` → `string` - Convenience wrapper around `aether_argv0()` that returns `""` instead of null and hands back a fresh copy
- `os.args_seal()` - **One-shot runtime seal** of the argv accessors. After this returns, `aether_args_count()` reports `0`, `aether_args_get(i)` returns null, `os.argv0()` returns `""`, and `aether_argv_raw()` returns null, as if argv had never been initialised. Idempotent (calling twice is a no-op); there is no unseal. Intended use: once `main()` has parsed its CLI flags into config state, call `os.args_seal()` to prevent any later code (imported libraries, plugin callbacks, untrusted Aether modules) from reading the original argv. Complements the compile-time `hide` / `seal except` scope directives, those deny *lexical* access, this denies *runtime* access. Caveat: this is a co-operative Aether-side gate, not a kernel boundary; the OS still has the original argv in process memory (Linux `/proc/self/cmdline`, macOS sysctl) and code that goes around the Aether accessors can still read it. Pair with the LD_PRELOAD libc sandbox if the threat model demands true inaccessibility.
- `os.args_sealed()` → `int` - Returns `1` if `args_seal()` has been called in this process, `0` otherwise. Cheap; useful for cooperative callers that want to check before they call.
- `os_execv(prog, argv_list)` → `int` - Replace the current process image with `prog`, passing an explicit `list<ptr>` argv. Uses POSIX `execvp(3)` so `prog` is looked up on `PATH` when it does not contain a slash. Flushes stdio before the exec so pre-exec output is not lost. On success this call **never returns**; on failure returns `-1` and the current process continues. Not available on Windows, use `os_run` + `exit(rc)` instead.

Raw extern: `os_exec_raw`.

**Process replacement example:**

```aether
import std.os
import std.list

main() {
    argv = list.new()
    _e1 = list.add(argv, "echo")
    _e2 = list.add(argv, "hello from")
    _e3 = list.add(argv, os.argv0())
    rc = os_execv("/bin/echo", argv)
    // Only reached if exec failed.
    println("exec failed: ${rc}")
    exit(rc)
}
```

---

## Math (`std.math`)

Mathematical functions. Note: `abs`, `min`, `max`, and `clamp` have separate int/float variants.

```aether
import std.math

main() {
    // Basic operations (type-specific variants)
    a = math.abs_int(-5)           // 5
    af = math.abs_float(-3.14)     // 3.14
    lo = math.min_int(3, 7)        // 3
    hi = math.max_int(3, 7)        // 7
    c = math.clamp_int(15, 0, 10)  // 10

    // Trigonometry
    s = math.sin(0.5)
    c = math.cos(0.5)
    t = math.tan(0.5)

    // Inverse trig
    asn = math.asin(0.5)
    ac = math.acos(0.5)
    at = math.atan2(1.0, 1.0)

    // Power, roots, logarithms
    sq = math.sqrt(16.0)    // 4.0
    p = math.pow(2.0, 3.0)  // 8.0
    l = math.log(2.718)     // ~1.0
    e = math.exp(1.0)       // ~2.718

    // Rounding
    fl = math.floor(3.7)    // 3.0
    ce = math.ceil(3.2)     // 4.0
    ro = math.round(3.5)    // 4.0

    // Random
    math.random_seed(12345)
    r = math.random_int(1, 100)
    f = math.random_float()
}
```

**Basic (int/float variants):**
- `math.abs_int(x)` / `math.abs_float(x)` - Absolute value
- `math.min_int(a, b)` / `math.min_float(a, b)` - Minimum
- `math.max_int(a, b)` / `math.max_float(a, b)` - Maximum
- `math.clamp_int(x, min, max)` / `math.clamp_float(x, min, max)` - Clamp to range

**Trigonometry:**
- `math.sin(x)`, `math.cos(x)`, `math.tan(x)` - Trig functions
- `math.asin(x)`, `math.acos(x)`, `math.atan(x)` - Inverse trig
- `math.atan2(y, x)` - Two-argument arctangent

**Power / Logarithms:**
- `math.sqrt(x)` - Square root
- `math.pow(base, exp)` - Power
- `math.log(x)` - Natural logarithm
- `math.log10(x)` - Base-10 logarithm
- `math.exp(x)` - Exponential (e^x)

**Rounding:**
- `math.floor(x)`, `math.ceil(x)`, `math.round(x)`

**Random:**
- `math.random_seed(seed)` - Seed RNG
- `math.random_int(min, max)` - Random integer in range
- `math.random_float()` - Random float 0.0-1.0

---

## I/O (`std.io`)

Console output, file operations, and environment variable access.

```aether
import std.io

main() {
    io.print("Hello ")
    io.print_line("World")
    io.print_int(42)
    io.print_line("")

    // getenv is infallible, returns the value or null if unset
    home = io.getenv("HOME")
    if home != 0 {
        io.print_line(home)
    }

    // read_file is Go-style
    content, err = io.read_file("myfile.txt")
    if err != "" {
        println("read failed: ${err}")
    } else {
        io.print_line(content)
    }
}
```

**Console Output (infallible):**
- `io.print(str)` - Print string
- `io.print_line(str)` - Print string with newline
- `io.print_int(value)` - Print integer
- `io.print_float(value)` - Print float

**Unbuffered fd writes (crash-trace use case):**
- `io.stderr_write(data)` → `int` - Write `data` to fd 2 directly (length computed internally with `string.length`), bypassing stdio buffering. Returns the byte count actually written, or -1 on error. Loops on partial writes; retries `EINTR` on POSIX. Reach for this when output must reach the terminal / pipe before the process aborts, `println` and `io.print` are line-buffered on tty and block-buffered when piped, so the last few lines reliably get lost during a crash.
- `io.stdout_write(data)` → `int` - Same shape as `stderr_write` but writes to fd 1. Useful for shell-pipe-friendly tools that need each record flushed before the next stage reads.

**File-descriptor lifecycle and bulk fd I/O:**
- `io.fd_open_read(path)` → `(int, string)` - Open `path` for reading (POSIX `O_RDONLY` / Win `_O_RDONLY | _O_BINARY`). Returns `(fd, "")` on success, `(-1, error)` on failure.
- `io.fd_open_write(path)` → `(int, string)` - Open `path` for writing, implicit `O_CREAT | O_TRUNC` (mode 0644 on POSIX, `_O_BINARY` on Windows). Returns `(fd, "")` / `(-1, error)`. Pair with `fd_close`. For O_APPEND or non-truncating opens, file an issue.
- `io.fd_close(fd)` → `string` - `""` on success, error string on failure. Single attempt, does not retry on EINTR (Linux requires not retrying; the descriptor is already gone).
- `io.fd_write_n(fd, data, length)` → `int` - Write exactly `length` bytes to `fd`. Loops on partial writes; retries EINTR on POSIX. Returns 0 on success, -1 on error. Note: when `data` is an Aether `string` parameter that crossed an extern boundary, the auto-unwrap may have stripped the AetherString header and the C side sees a plain `const char*` strlen-truncation applies for embedded NULs. For binary writes from Aether-side, marshal through `std.bytes` first.
- `io.fd_read_n(fd, n)` → `(ptr, int, string)` - Read up to `n` bytes from `fd`. Returns `(bytes, count, err)`: `bytes` is a refcounted AetherString carrying the explicit byte count (binary-safe, embedded NULs survive), `count` is the number actually read (1..n on success, 0 on clean EOF or error), `err` is `""` on success or clean EOF, otherwise an error message.
- `io.fd_read_line(fd)` → `(ptr, string)` - Read one `\n`-delimited line from `fd`. Trailing `\n` is stripped (a preceding `\r` is also stripped, so CRLF input yields content with neither). Returns `(line, "")` on a normal line, `("", "")` on clean EOF before any byte, `(partial, "")` on EOF mid-line (server-side dump streams sometimes omit a trailing newline), `("", error)` on read error.

**File Operations (Go-style):**
- `io.read_file(path)` → `(string, string)` - Read entire file
- `io.write_file(path, content)` → `string` - Write (overwrites), return error string
- `io.append_file(path, content)` → `string` - Append to file
- `io.delete_file(path)` → `string` - Delete file
- `io.file_info(path)` → `(ptr, string)` - Get file metadata
- `io.file_info_free(info)` - Free file info
- `io.file_exists(path)` - 1 if exists, 0 otherwise (infallible)

**Environment:**
- `io.getenv(name)` - Get environment variable (returns string or null, infallible)
- `io.setenv(name, value)` → `string` - Set env var, return error string
- `io.unsetenv(name)` → `string` - Unset env var, return error string

Raw externs: `io_read_file_raw`, `io_write_file_raw`, `io_append_file_raw`, `io_delete_file_raw`, `io_file_info_raw`, `io_setenv_raw`, `io_unsetenv_raw`.

---

## Concurrency

### Built-in Functions

- `spawn(ActorName())` - Create actor instance
- `wait_for_idle()` - Block until all actors finish
- `sleep(milliseconds)` - Pause execution
- `release(s)` - Decrement an AetherString's refcount and free if it reaches zero. Sugar for `string.release(s)` argument must be `string`-typed (other heap types call their typed release, e.g. `string.string_seq_free`). Pair with `defer` to undo allocations made by stdlib functions returning ownership: `body, err = http.get(url); defer release(body)`.

### Blocking work off the loop (`std.worker`)

Runs blocking work on a bounded thread pool and delivers the result back on
the thread that owns the event loop. The full contract (poster vs drain,
cooperative builds, pool sizing) is documented at the top of
`std/worker/module.ae`; the batch surface is:

- `worker.run(work, done)` - Run `work` on a pool thread; `done` receives its
  `ptr` result on the loop thread. Concurrency is bounded by the pool, so job
  N+1 queues behind the N in flight.
- `worker.wait()` → `int` - Block until every submitted job has completed
  **and** had its completion delivered, running those completions on the
  calling thread. Returns how many ran, or -1 if a main-thread poster is
  installed (there the host loop delivers, so waiting on that thread would
  block the very thread that has to deliver). This is the headless batch
  join: no poll loop, and unlike `pool_shutdown` it leaves the pool running,
  so a reusable helper can wait once per batch and be called again.
- `worker.map(items, f)` → `ptr` - Bounded parallel map: apply `f` to every
  element of `items` on the pool, collecting results into a new list that is
  index-aligned with the input (the Go `errgroup.SetLimit` shape). `f` runs on
  a worker thread; the result slots are written on the calling thread during
  the wait, so the returned list needs no locking. The caller owns the
  returned list and keeps ownership of `items`.
- `worker.drain(max)` / `worker.pending()` - Manual pump and outstanding
  count, for hosts driving their own loop.

```aether
lengths = worker.map(paths, |p: ptr| { return measure(p) })
```

  **When it pays.** Dispatching and joining a batch costs a flat ~100 us
  (measured on 8 cores, #1297), independent of how much work the items do, so
  `worker.map` wins only when each item is worth more than roughly 20 us. Below
  that it is slower than a plain loop, by a lot at trivial work sizes. There is
  deliberately no automatic fallback: the threshold depends on the work per
  item, which the runtime cannot see. Numbers and method in
  [`docs/cross-references/bend.md`](cross-references/bend.md).

---

## Memory

### Arena allocator (`std.arena`)

A bulk allocator for short-lived raw buffers. The arena hands out memory via `arena.alloc()` but cannot free individual allocations, call `arena.reset()` to drop everything in one shot, or `arena.destroy()` to return the underlying memory to the OS.

The headline use case is a polling loop or parsing pass that allocates many scratch buffers per iteration:

```aether
import std.arena

main() {
    a = arena.create(0)              // 0 = default 1 MiB block
    defer arena.destroy(a)

    iter = 0
    while iter < 1000 {
        arena.reset(a)               // O(1) bulk free of last iter
        scratch = arena.alloc(a, 4096)
        // ... use scratch for parsing, formatting, IO buffer, etc.
        iter = iter + 1
    }
}
```

**Functions:**
- `arena.create(size)` → `ptr` - Allocate an arena with the given block size in bytes (`0` = default 1 MiB). Overflow blocks chain on demand. NULL on OOM.
- `arena.alloc(arena, bytes)` → `ptr` - Allocate `bytes` (8-byte aligned). Memory is uninitialized. NULL on OOM or null arena.
- `arena.alloc_aligned(arena, bytes, alignment)` → `ptr` - Same as `alloc` with explicit alignment (must be a power of 2).
- `arena.reset(arena)` - Free every allocation in one shot. Pointers become invalid.
- `arena.destroy(arena)` - Free the arena and its memory.
- `arena.used(arena)` → `int` - Bytes currently allocated (sum across overflow blocks).
- `arena.size(arena)` → `int` - Total capacity (sum across overflow blocks).

Arenas don't track AetherString refcounts, strings allocated through the regular stdlib still need `release()` (or `defer release(...)`); the arena is for bulk raw allocations that the user controls themselves. Avoid handing arena-allocated pointers to functions that retain them past the next `arena.reset()` or `arena.destroy()`.

### Content-addressed store (`std.cas`)

A small content-addressed store keyed by the hex sha256 of file contents. Useful for sharing built artifacts (`.so` files from `--emit=lib`, signed configs, anything content-addressable) between machines and runs. Puts go through write-tmp + atomic rename so partial writes never appear under the final name; gets re-hash before delivering so a corrupted store entry can't quietly hand back wrong bytes.

```aether
import std.cas

main() {
    digest, err = cas.put("./build/myplugin.so")
    if err != "" { return }
    println("published: ${digest}")           // hex sha256

    if cas.has(digest) {
        gerr = cas.get(digest, "./fetched.so")
        // fetched.so's bytes hash to `digest`, verified on the way out.
    }
}
```

**Functions:**
- `cas.put(file_path)` → `(string, string)` - Insert a file into the store. Returns `(digest, "")` on success or `("", err)` on failure (file missing, OOM, mkdir failure, write failure). Idempotent: re-putting the same content returns the same digest without breaking the store.
- `cas.get(digest, dest_path)` → `string` - Copy an entry out of the store, verifying its sha256 hashes back to `digest`. Returns `""` on success or an error string on missing digest, read failure, dest write failure, or digest mismatch (corrupted entry).
- `cas.has(digest)` → `int` - Existence check. NULL/empty-safe.
- `cas.path(digest)` → `string` - Composes `<root>/<digest>` without touching the filesystem.
- `cas.root()` → `string` - The CAS root: `$AETHER_CAS` if set, else `$HOME/.aether/cas`, else `/tmp/aether-cas`.

The store layout is intentionally flat (one file per digest). Grow to two-level fan-out (`<digest[0:2]>/<digest[2:]>`) only if entry counts ever push filesystem dirent limits in practice.

---

## Process state

Aether deliberately rejects mutable assignment to module-level identifiers, the design philosophy is "if state is mutable, it lives inside an actor or a runtime registry." Two stdlib modules give you the **set-during-init, read-everywhere** shape that BEAM achieves with `persistent_term` and `register/whereis`, without spawning a long-lived actor or paying message round-trip on the read path.

These are the right tool when:
- A handler is entered from a C-callback and can't take an Aether-typed parameter (the `void* user_data` slot doesn't carry typed values).
- The state is genuinely process-wide (CLI flags, current-user identity, the long-lived registry of named actors).
- You'd otherwise reach for a `static` C global.

**Don't** use these for per-request / per-tenant state, that should live in the actor or function that owns the work, plumbed through call parameters or actor messages.

### `std.config` string→string KV

```aether
import std.config

main() {
    config.put("user", "alice")
    config.put("token", "xyz")
}

handle_request(req: ptr, res: ptr, ud: ptr) {
    user  = config.get("user")            // "alice"
    level = config.get_or("level", "info") // fallback default
}
```

- `config.put(key, value)` - Insert / overwrite. Both `key` and `value` are duplicated internally; caller's string lifetimes don't matter.
- `config.get(key)` → `string` - Returns `""` if the key isn't set (matches Aether's Go-style "" = absent convention). Reads are concurrent (no lock contention with each other).
- `config.get_or(key, default_value)` → `string` - Returns the registered value if set, otherwise `default_value`.
- `config.has(key)` → `int` - 1 if `key` has been put, 0 otherwise.
- `config.size()` → `int` - Number of keys currently registered.
- `config.clear()` - Wipe all keys. Tests use this for isolation; production code rarely needs it.

Storage: a single process-global hashmap protected by a reader/writer lock. Implementation models BEAM's `persistent_term`. Returned `get` strings are borrowed, they remain valid until the next `put` / `clear` that touches the same key, which is fine for the "set once at startup" pattern and lets reads avoid an allocation. Copy via `string.copy(value)` if you need a value that survives a later `put`.

### `std.actors` name → actor_ref registry

```aether
import std.actors

actor Auditor {
    receive {
        Analyze(payload) -> { /* ... */ }
    }
}

main() {
    a = spawn(Auditor())
    actors.register("auditor", a)
}

handle_request(req: ptr, res: ptr, ud: ptr) {
    a = actors.whereis("auditor")
    a ! Analyze { payload: ud }
}
```

- `actors.register(name, ref)` - Bind `name` → `ref`. Overwrites any prior binding (no error). The name is duplicated; the actor_ref is stored as-is.
- `actors.whereis(name)` → `actor_ref` - Look up by name. Returns the registered ref, or null if unregistered.
- `actors.unregister(name)` → `int` - 1 if a binding was removed, 0 if `name` wasn't registered.
- `actors.is_registered(name)` → `int` - 1 if bound, 0 otherwise.
- `actors.registry_size()` → `int` - Number of currently-registered names.
- `actors.registry_clear()` - Wipe all bindings.

Models BEAM's `erlang:register` / `whereis`. The registry doesn't track actor liveness, if the actor exits, `whereis` keeps returning the stale ref until something explicitly calls `unregister`. Match BEAM's behaviour for non-link'd registrations.

**Why the module name is plural**: `actor` (singular) is a reserved keyword in Aether and can't appear as a namespace prefix. `actors.register(...)` parses; `actor.register(...)` does not. The plural also reads correctly, "the actors registry."

---

## See Also

- [Getting Started](getting-started.md)
- [Tutorial](tutorial.md)
- [Module System Design](module-system-design.md)
- [Standard Library API](stdlib-api.md)
