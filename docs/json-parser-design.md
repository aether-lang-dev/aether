# `std.json` Design Notes

This document explains *why* `aether_json.c` is shaped the way it is.
It's meant to be read alongside the source, not as a standalone spec,
but as a map to the design decisions so future maintainers know what
can be changed freely and what's load-bearing.

## Goals

1. **Correctness first.** Pass RFC 8259 conformance. Never UB, never OOB,
   never silent data loss. JSONTestSuite (318 tests) gates CI.
2. **Portable to every CI target.** Compiles clean on GCC, Clang, Apple
   Clang, MinGW (32+64-bit), Emscripten under `-Wall -Wextra -Werror
   -pedantic`. SIMD kernels gated behind compile-time feature detection
   (`__SSE2__`, `__ARM_NEON`) with a scalar fallback that's always
   compiled, no target is SIMD-only.
3. **Fast enough that JSON isn't a bottleneck.** Every byte in the hot
   path is touched at most once. Allocations happen from a per-document
   arena, not per-node. Hot classification decisions are a single LUT
   read. Measurement harness lives in
   [`benchmarks/json/baseline.md`](../benchmarks/json/baseline.md).
4. **Low maintenance.** One `.c` file, one design doc, no vendored deps.
   Anyone who reads this file should be able to maintain the parser
   without additional context.

These are in priority order. Correctness outranks speed. Portability
outranks a fast-path optimisation that only works on one arch.

## Memory model: one arena per parsed document

Every parsed document lives in a bump-pointer arena allocated at
`json_parse_raw_n` time. Every `JsonValue`, every string payload, every
container backing array for that document comes from the same arena.
`json_free(root)` calls `arena_destroy` which walks the chunk list and
returns everything to `malloc`'s free list in a single pass.

This eliminates the **per-node malloc** pattern that dominated the
original parser's profile: every JsonValue, every string buffer, and
every container (via `ArrayList`/`HashMap` wrappers) was its own
allocation. On `large.json` (10 MB, 676k values), that's a million
allocator round-trips; on our arena path it's a handful of chunk
allocations.

### Chunks and growth

The arena is a singly-linked list of `ArenaChunk`s. First chunk is 16 KB
(big enough that tiny documents never grow), subsequent chunks double
in size up to a 2 MB cap. An allocation bigger than the current chunk's
remaining space triggers a new chunk sized at least as large as the
requested bytes, so a single 5 MB string doesn't force the allocator
to keep doubling until it exceeds the request.

Alignment is fixed at 8 bytes. All allocations round up.

### Why not a tape?

Several fast parsers store parsed values in a **tape**: a flat array of
fixed-size slots indexed by position, with parent/child relationships
encoded via sibling offsets. That layout is maximally cache-friendly
but it couples value lifetimes to the tape and makes the creation path
(`json_create_string`, `json_object_set`, …) awkward, a standalone
value has nowhere to live without a tape to sit in.

We preserve the API shape, values are reachable through a tree of
pointers, and recover cache locality from the arena's chunk-local
allocation. Values allocated close in time end up close in memory, and
`JsonValue` itself is sized to fit two per 64-byte cache line
(see "JsonValue sizing" below). A full tape representation is possible
later with copy-on-mutate preservation of the creation API; the design
is sketched in "When to change what" near the end of this doc.

### Heap path for `json_create_*`

Values created standalone (`json_create_null`, `json_create_string`,
`json_create_array`, etc.) are heap-allocated individually via `malloc`.
Their children are similarly heap-allocated. `json_free` on such a
value walks the tree and frees each node.

When a standalone value is handed to `json_array_add_raw` or
`json_object_set_raw` on an arena-backed container, the mutator
**deep-copies the subtree into the parent's arena** and frees the
original. After insertion the entire tree is arena-owned; a single
`json_free` at the root tears everything down.

This double-path (arena for parse, heap for create, deep-copy on merge)
is the only real complexity cost of preserving the mutation API. The
parse path is a tight loop with no branching on allocation mode; the
mutation path is rare and pays for its complexity only when invoked.

## Character classification via a 256-entry LUT

`JSON_CC_RW[256]` holds per-byte flag bits:
`CC_WHITESPACE`, `CC_DIGIT`, `CC_STRUCTURAL`, `CC_STR_OK`, `CC_HEX`,
`CC_NUM_START`.

Hot-path decisions become a single indexed load + bit-test:

```c
while (s->p < s->end && (CC(*s->p) & CC_STR_OK)) p_advance(s);
```

The table is initialized once on first parse via `cc_init()` (the
static initializer handles the digit/hex/structural entries; runtime
code fills `CC_STR_OK` for the rest of printable ASCII). `cc_init` is
idempotent and guarded by a simple non-atomic flag, the race on first
parse is benign because both racing threads would compute the same
table contents.

### Why runtime init?

C89 static initializers can't include loops, and writing out every
byte's flags explicitly made the source very noisy. A one-time
initializer trades one branch on the first parse call for a much more
readable declaration. The check is predicted cold after the first parse.

## Fast string scanning

`parse_string_raw` is two-phase:

1. **Pre-scan**, walk forward from the opening `"`, skipping
   `\\<byte>` pairs verbatim, until the closing `"` is found.
   This gives us the exact span length. We allocate a decode buffer
   of `span + 1` bytes in the arena, tight upper bound because
   JSON escapes cannot *grow* their output (a 6-byte `\uXXXX` decodes
   to at most 3 UTF-8 bytes; a 12-byte surrogate pair to 4).
2. **Decode**, walk again, this time with the LUT fast loop:

   ```c
   while (s->p < s->end && (CC(*s->p) & CC_STR_OK)) {
       p_advance(s);  // tight loop, no branches except loop condition
   }
   memcpy(dst + di, run_start, run_len);
   ```

   When the fast loop exits we look at exactly one byte and dispatch:
   close-quote, escape, control char (reject), or non-ASCII (UTF-8 DFA).

### The critical bug the pre-scan prevents

The first rewrite didn't pre-scan. It allocated `max_len = s->end - s->p`
bytes per string as an upper bound on decoded length. That's fine for
one string in a small document, but for `large.json` with 676k strings
each allocating the remaining input size, the arena exploded to gigabytes
before the process was killed.

The pre-scan turns per-string allocation into `O(string bytes)` instead
of `O(remaining document bytes)`. Without it, correctness degrades into
an OOM long before throughput becomes interesting.

## UTF-8 validation: Hoehrmann's DFA

Non-ASCII bytes in strings go through a ~30-line state machine from
[Bjoern Hoehrmann's 2010 paper](http://bjoern.hoehrmann.de/utf-8/decoder/dfa/).
Public domain, easy to verify against the reference, rejects every
malformed case RFC 3629 forbids: overlongs, surrogate halves,
codepoints > U+10FFFF, continuation bytes in lead position.

The DFA state `UTF8_ACCEPT = 0` starts a new codepoint,
`UTF8_REJECT = 12` latches an error, any other value means mid-sequence.
Per-byte cost: one table load + two arithmetic ops, much cheaper than
branching on each byte's bit pattern.

## SIMD string fast-loop

The string parser's inner loop skips over "safe" printable ASCII bytes
(>= 0x20, < 0x80, not `"`, not `\`). That loop is in the hot path of
every non-trivial JSON document and is embarrassingly parallel, each
byte is classified in isolation.

`scan_str_safe()` picks one implementation at **compile time**, never
at runtime:

- `__SSE2__` → SSE2 kernel (16-byte vectors). SSE2 is the AMD64
  baseline, so this lights up on every x86_64 CI row without any extra
  flags. Uses `_mm_cmpeq_epi8` for quote/backslash + a signed-compare
  trick (`_mm_cmplt_epi8(v, 0x20)`) that flags both control chars
  (0x00-0x1F) and non-ASCII (0x80-0xFF) in a single instruction, then
  `_mm_movemask_epi8` + `__builtin_ctz` for "offset of first unsafe
  byte."
- `__ARM_NEON && __aarch64__` → NEON kernel. Same classification as
  SSE2, different intrinsics. NEON lacks `movemask`, so we use the
  canonical `vshrn_n_u16` narrow-to-nibbles trick to produce a u64
  where each input byte contributes 4 bits, `__builtin_ctzll >> 2`
  gives the first-set-byte offset.
- fallback → a scalar loop over the CC_STR_OK lookup table. Byte-at-
  a-time but branch-predictor-friendly. Active on WASM, embedded, or
  any target that lacks both intrinsic headers.

All three produce byte-identical results, SIMD is purely additive
acceleration, never a semantic shift. The SIMD path always leaves a
scalar tail loop to clean up 0-15 bytes at the end. No runtime feature
detection = no per-call branch cost on the happy path.

Line/col accounting stays correct because safe bytes are by definition
>= 0x20, so no newline can ever appear inside a SIMD chunk: the string
loop bumps col by the run length without checking for '\n'.

Why not SIMD for UTF-8 validation or structural scan? Those either need
tape-based output (structural) or have significantly more state to
track (UTF-8 DFA state carries across bytes). The string fast-loop is
the one place SIMD drops in with no architectural follow-through. If a
profile ever puts parse throughput ahead of everything else, the next
structural step is tape representation, not more SIMD.

## Number parsing: validate + dispatch

RFC 8259 numbers have a strict grammar:
`[ minus ] int [ frac ] [ exp ]` where `int` rejects leading zeros.
We walk the grammar explicitly **and accumulate all three fields in a
single pass**, sign, integer mantissa (int64), fractional mantissa
(int64) with digit count, signed exponent.

Three paths, chosen by which accumulators are in-range:

1. **Pure-integer path.** No `.`, no `e/E`, int accumulator hasn't
   overflowed. Return `(double)int_acc` (signed). No `strtod` call.
2. **Fast-double path.** Total significant digits ≤ 15 (double's
   fully-exact range) and |effective exponent| ≤ 22 (where
   `POW10_POS[n]` is exactly representable in a double). Fuse int and
   frac into one integer mantissa, multiply/divide by
   `POW10_POS[|exp|]` once. One multiply, one cast, one negate.
3. **Fallback.** Any of the above conditions fails, typically 16+
   significant digits, huge exponents, denormals, or overflow of the
   int64 mantissa. Hand the validated span off to `strtod` for
   correctly-rounded IEEE-754. Guarantees we never silently return a
   wrong double.

Paths 1 and 2 cover essentially every config file, API response, and
log record we've benchmarked. Path 3 is reserved for scientific data
sets, cryptographic constants, and similar edge-case corpora.

The `POW10_POS` table tops out at `1e22` because doubles can exactly
represent powers of ten only through that index; past 22 the `strtod`
fallback is the only correct answer.

## Containers: flat arrays + one indirection for objects

`JsonValue::arr` is `{ JsonValue** items; uint32_t count, capacity }`,
a pointer to an arena-owned array plus inline count/capacity.

`JsonValue::obj` holds the same count/capacity pair inline but reaches
its three parallel arrays through a single `JsonObjBlock*`:

```c
typedef struct JsonObjBlock {
    const char** keys;
    uint32_t*    key_lens;
    JsonValue**  values;
} JsonObjBlock;
```

Reading an object field is `obj->data.obj.blk->values[i]` one extra
pointer load compared to inlining the three arrays in `JsonValue`. The
payoff is that the `obj` union variant is 16 bytes instead of 32, which
collapses `sizeof(JsonValue)` from 48 to 32 and lets two values share a
64-byte cache line on every machine we care about (see the next
section). Since every non-trivial object access is going to miss cache
*somewhere*, either on the value struct or on the block, putting the
indirection at the block level is essentially free, and halving the
value struct's footprint pays back across the whole parsed tree.

Growth uses simple doubling with an initial capacity of
`JSON_CONTAINER_INITIAL_CAP` (= 8). Realistic JSON objects cluster in
the 5–15 key range, so starting at 8 skips one doubling cycle in the
common case; tiny objects waste a few unused pointers which round-off
into the arena anyway. Old buffers become arena garbage until the arena
is freed, waste is bounded by `O(final size)` per container.

**Iteration order.** The parallel-array layout means objects iterate
in insertion order by construction, the parser appends to the tail as
it reads each `key: value` pair, and the builder's `json_object_set_raw`
does the same on first insert. The public iteration API
(`json_object_size_raw` / `json_object_key_at` / `json_object_value_at`,
and the Aether-side `object_entry(obj, i)` wrapper) commits to this
order: parsed JSON iterates in the order keys appeared in the source,
built JSON iterates in the order `object_set` was called. Callers that
need sorted iteration copy the keys and sort.

## JsonValue sizing

```c
struct JsonValue {
    uint8_t type;           // 1
    uint8_t flags;          // 1  (JV_FLAG_ROOT | JV_FLAG_HEAP_STRUCT | JV_FLAG_INTEGER)
    uint8_t _pad[6];        // 6, needed to align the union at offset 8
    union {
        int       boolean;                          // 4
        double    number;                           // 8
        long long integer;                          // 8, used when JV_FLAG_INTEGER set
        struct { const char* data; uint32_t length; } str;  // 16
        struct { JsonValue** items; uint32_t c, cap; } arr; // 16
        struct { JsonObjBlock* blk; uint32_t c, cap; } obj; // 16
    } data;                 // 16 bytes, widest variant is str / arr / obj
    Arena* arena;           // 8, set on root only (interior: NULL)
};                          // 32 bytes total
```

Two values per 64-byte cache line. That matters because every parsed
tree is a graph of JsonValues pointing at JsonValues; fewer cache lines
touched per traversal is a direct throughput win, with no change to
any code that reads the fields.

The `_pad[6]` after `type`/`flags` is forced by the union's 8-byte
alignment (it contains a `double`). The `arena` pointer lives on the
root only, interior values keep it `NULL` and use it as a
"is-this-a-root" marker when the mutation path needs to branch.

### Object lookup: linear scan

`json_object_get_raw` does a byte-by-byte `memcmp` walk across all
keys. The typical object in API responses has <20 keys; linear scan
beats hashing at those sizes because branch prediction is perfect.
The `key_lens[]` array lets us reject mismatched lengths in a single
integer compare before touching `memcmp`.

For objects with hundreds of keys this becomes O(n) and pathological.
If that pattern ever shows up in profiles we'd switch to a sorted
keys representation and binary search, or a tiny open-addressed hash
for objects above some threshold. Not v1.

## Error reporting

Positioned errors funnel through `err_set(line, col, fmt, ...)` which
formats into a `_Thread_local` buffer. Input-level failures with no
cursor position (null input, arena-create OOM in `json_parse_raw_n`)
go through a separate `err_set_no_pos(reason)`. Both share the
**first error wins** rule: once `g_json_err_set` is true, subsequent
calls no-op. This preserves the innermost diagnostic, which is almost
always the most specific.

Position accuracy is maintained by `p_advance(s)` every cursor
motion goes through it, incrementing `line` on `\n` and `col`
otherwise. The fast string loop calls `p_advance` in every iteration
(not just on the escape branch), which costs a little throughput but
keeps error messages like "expected `:` at 3:17" correct.

### Structured-error pilot

The base parser surfaces errors via `err_set` (human string) plus the
`g_json_err_buf` thread-local. The structured-error pilot adds a
parallel surface that callers can switch on programmatically without
parsing English:

- `g_json_err_kind` one of `AETHER_JSON_KIND_*` (`OK`, `PARSE_ERROR`,
  `OUT_OF_MEMORY`, `INVALID_INPUT`). `err_set` only ever assigns
  `OUT_OF_MEMORY` or `PARSE_ERROR`: OOM detection is a single-site
  `strstr(msg, "out of memory")` check, so the dozens of existing
  `err_set("out of memory ...")` sites need no change, and everything
  else falls through to `PARSE_ERROR`. `INVALID_INPUT` comes only from
  `err_set_no_pos`, which handles the null/empty-input path.
- `g_json_err_line` / `_col` the same 1-based position the human
  message reports, but as integers for callers that want to point at
  source.
- `json_last_error_kind()` / `_line()` / `_col()` public C accessors
  for the three thread-locals. Aether-side wrappers in
  `std/json/module.ae` re-export them.
- `parse_strict(json_str) -> (ptr, int, string)` Aether wrapper that
  returns `(value, kind, message)` per call. The kind is read from
  the thread-local immediately after parse so concurrent callers from
  different threads each see their own; the tuple shape is the
  canonical structured-error shape from the std.fs pilot.

KIND values (`AETHER_JSON_KIND_*` macros + `std.json.KIND_*` int
exports) are intentionally a subset that overlaps `std.fs.KIND_*`
where the meanings align (`OK`, `OUT_OF_MEMORY`, `INVALID_INPUT`),
so callers mixing fs and json calls can switch on either surface in
the same `if` chain.

The existing `parse(json_str) -> (ptr, string)` surface is unchanged
and remains the recommended path for callers that just need
"succeeded / failed with this string." `parse_strict` is opt-in.

## Depth limit

Hard cap at `JSON_MAX_DEPTH = 256`. Enforced on every container entry
via `parse_value_depth(depth + 1)`. Prevents both stack overflow on
pathological input and denial-of-service via deeply nested JSON
bombs (which DOS many naive parsers). No way to disable at runtime,
it's a bounded recursion guarantee, not a configurable limit.

## Thread safety

The parser itself is **reentrant and thread-safe**, there is no
mutable global state during parse. The five `_Thread_local` globals
(`g_json_err_buf`, `g_json_err_set`, and the structured-error trio
`g_json_err_kind` / `_line` / `_col`) and the `JSON_CC_RW` init flag
isolate per-thread parses from each other. Two threads parsing
concurrently hold independent arenas and see independent error slots.

`cc_init`'s first-run race is benign (both threads write the same bytes).

## Structural headroom

Three deliberate structural choices bound how far the parser can go
without a larger rewrite. Each one trades a specific complexity cost
for its current behaviour, and each has a concrete path forward if the
bound starts to pinch.

### Tape representation (deferred, path documented)

The fastest contemporary JSON parsers store parsed values in a flat
slot array ("tape") with parent/child relationships encoded as sibling
offsets. That layout is maximally cache-dense for traversal but assumes
the whole tree is written once and never mutated, "insert a child"
would invalidate every downstream sibling offset.

Our `json_object_set` / `json_array_add` mutation APIs let callers add
arbitrary children to parsed trees, including heap-created values from
`json_create_*`. A pure tape can't absorb those without a full rebuild.

The path forward is **dual representation with copy-on-mutate**:

- `json.parse` produces a flat tape for the parsed document.
- `json.array_add` / `json.object_set` on a tape-backed value promotes
  the affected subtree to the existing tree representation once (one
  deep-copy), flips a flag on the root, then mutates as today.
- Subsequent calls on the promoted root skip the conversion.
- Accessors (`object_get`, `array_get`, `get_*`, …) branch on the
  tape/tree flag once to pick the right code path.

What's deferred about this is the breadth of the change, not the
design. Every accessor gains a dispatch branch; the mutation path gets
a new OOM mode (copy allocation can fail mid-tree); every test needs
two flavours. That's worth doing, but it's a standalone review cycle,
not a rider on an incremental perf pass. The change-point names the
files in "When to change what" below.

### Full SIMD parsing (not on the roadmap)

Vectorised parsers classify every byte into
"structural / whitespace / string-char / escape" with a single wide
compare, producing a bitmap the parser walks. The bitmap is only useful
if the parser writes tape slots; producing a tree of `JsonValue*` from
the bitmap adds back all the branches the SIMD was meant to save.

So a full-SIMD path is a tape-path with different input. The targeted
SIMD kernel we do ship (`scan_str_safe`) lives inside the string parser
where it drops in cleanly and coexists with a scalar fallback that's
always compiled. Anything beyond that depends on tape landing first.

### Things we deliberately don't do

- **Streaming / incremental parsing**, everything is parsed into
  memory in one go. JSON payloads that don't fit in RAM are rare and
  streaming complicates both the arena and the error-reporting path.
- **UTF-16 / UTF-32 input**, RFC 8259 mandates UTF-8. We reject
  anything else at the byte layer.
- **Canonical JSON output / pretty-printing**, `json_stringify_raw`
  emits compact JSON. A pretty option is a trivial add if asked for.
- **Schema validation**, not a parser concern.
- **Runtime CPU feature detection**, compile-time dispatch only. If
  a binary is built for a target that has SSE2, it uses SSE2
  everywhere that target runs. No dispatch cost on the happy path.

## Testing surface

- **`tests/runtime/test_runtime_json.c`**, C-level unit tests,
  8 tests covering parse/create/stringify for each type.
- **`tests/regression/test_json_*.ae`**, Aether-level regression
  tests for edge cases (escapes, Unicode, RFC rejection, position
  errors, error tuples, edge-case wrappers).
- **`tests/conformance/json/`**, 318-file JSONTestSuite corpus.
  `make test-json-conformance` checks every file. Gates CI on
  `y_*` + `n_*` (283 cases); records `i_*` (35 cases) as
  implementation-defined.
- **`make test-json-asan`**, parses the bench corpus under
  `-fsanitize=address,undefined`. Catches leaks, OOB, UB.
- **`make test-json-valgrind`**, Valgrind run where available.
- **`make bench-json` / `make bench-json-compare`**, reproducible
  benchmark harness; see
  [benchmarks/json/baseline.md](../benchmarks/json/baseline.md) for
  the methodology. Absolute throughput numbers are not committed, run
  the harness on the machine you care about.

## When to change what

| If you want to …                           | Change                                   |
| ---                                        | ---                                      |
| Land tape representation                   | New `TapeSlot` struct + `JV_FLAG_TAPE` flag on `JsonValue`; rewrite `parse_value_depth`/`parse_array`/`parse_object` to produce tape slots; add `tape_promote_to_tree()` helper called lazily from the mutators; branch in every accessor on `flags & JV_FLAG_TAPE`. |
| Add SIMD for structural scan (tape-only)   | New `scan_structurals()` beside `scan_str_safe`, called from the tape parser. Needs tape to land first. |
| Improve object lookup for large objects    | Threshold-based switch in `json_object_get_raw`: linear scan below ~32 keys, open-addressed hash above. |
| Resize the object backing block            | `JsonObjBlock` lives behind `JsonValue::data.obj.blk`. Adding a fourth parallel array (e.g. cached hash) goes here. |
| Support streaming                          | New `json_parse_stream` API + state object. Would likely need its own arena-resetting allocator. |
| Change max depth                           | `JSON_MAX_DEPTH` near top of file.        |
| Add a pretty-print mode                    | New flag in a `json_stringify_opts` struct, new builder path. |
| Tune arena chunk sizes                     | `ARENA_INITIAL_CHUNK`, `ARENA_MAX_AUTO_CHUNK`. |
| Track another error field                  | Extend `JSON_ERR_REASON_BUF`, update `err_set`. |
| Raise the fast-double digit cap            | `FAST_INT_SAFE_DIGITS`; only safe up to 15 for double-exactness. Past that, `strtod` is the only correct answer. |
| Add AVX2 string scan (32-byte block)       | New `#if defined(__AVX2__)` branch in `scan_str_safe`. Same shape as SSE2, wider vectors. Keep all existing branches. |

Each of these is a localized change. The one-file design is the whole
point: a new contributor reads this doc, scans the source, and can
change any single row of the table above without touching the others.
