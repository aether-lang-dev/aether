# REPLY: the shared map keeps C strings: a key or a value with a NUL cannot be stored whole

**Re:** aether-guest-languages `asks/shared-map-nul.md`

**RESOLVED** on the branch `batch/fix-runtime`, in full: keys and values are
length-carrying end to end, and the shared memory block is length-prefixed
with a magic and a version. Not the smaller refusal: once read, the change was
one runtime file, its header and its callers here (the stunt bridge, the
`contrib.host.aether` runner), and it touches no safety property of the
sandbox except to tighten two (below).

## The C API (runtime/aether_shared_map.h)

No back compatibility: the C-string functions are gone, same names, new
signatures, so a bridge built against the old header fails to compile.

| was | is |
|---|---|
| `void put(map, key, value)` | `int put(map, key, key_len, value, value_len)`: 0, or -1 refused (a new key in a full map, no memory), the map unchanged |
| `const char* get(map, key)` | `const char* get(map, key, key_len, size_t* value_len)` |
| `const char* get_by_token(token, key)` | `... get_by_token(token, key, key_len, size_t* value_len)` |
| `int put_by_token(token, key, value)` | `int put_by_token(token, key, key_len, value, value_len)`: -1 for a dead token, a frozen input (by its whole key) or a full map |
| `key_at(map, i)`, `value_at(map, i)` and the `_by_token` forms | the same with a `size_t* len` out; `key_len_at` / `value_len_at` give the length alone |
| `void read_outputs_from_shm(...)` | `int`: 0, or -1 refused (the map unchanged) |
| (declared by hand in every bridge) | `aether_shared_map_freeze_inputs_by_token`, `aether_shared_map_token`, `aether_shared_map_find` in the header |
| | `aether_shared_map_encode` / `decode_outputs`: the block's bytes without the shm |

Ownership: the map copies what it stores; what get and `*_at` return is the
map's own copy, NUL-terminated after its length, valid until that key is put
again or the map is freed, never freed by the caller. A NULL value is stored
as an empty one.

From Aether: `std.sharedmap` (new; README beside it), with `string` keys and
values by their length: `new`, `token`, `put -> int`, `get -> (string, int)`,
`has`, `count`, `key_at`, `value_at`, `freeze`, `guest_put`, `free`.

## The block, layout 2

Little-endian uint32s: magic `AESM`; a word that is always 0; version 2;
frozen_count; entry_count; then per entry key_len, value_len, the key's bytes,
the value's. A reader of layout 1 starts parsing at byte 4, finds a NUL there
and sees an empty map, rather than reading lengths as text (it cannot be made
to fail on its own, but it cannot misread). A layout-2 reader refuses anything
else whole: a missing magic (a layout-1 block starts with its frozen count), a
nonzero second word, another version, an entry running past the end, more
than 256 entries, or a nonzero byte after the last entry (zero bytes there are
allowed: macOS rounds a shm object up to a page). There is no negotiation: a
host and a child are built at the same Aether version.

Frozen inputs and the size limit:

- A child's block can no longer overwrite an input. The old reader trusted
  the frozen_count the child wrote back; the new one ignores it for that and
  skips every entry whose key is one of the host map's own frozen inputs.
- The block a host writes is still capped at 64 KiB, as the old stack buffer
  was, but refused loudly (`to_shm` returns NULL with `E2BIG`, and the runner
  says "the map is larger than the 64 KiB block" and does not start). The old
  writer skipped entries past 65000 bytes, and before that wrote past the end
  of its `char buf[65536]` for one large value: AddressSanitizer reported a
  stack-buffer-overflow for a 70,000-byte value. A block read back may be up
  to 64 MiB (a guest's outputs may outgrow its inputs); the old reader had no
  cap at all, and ran `strlen` over the child's bytes.
- The 256-key limit is kept; a put of a new key past it now returns -1
  instead of doing nothing.

Found on the way: on macOS `to_shm` always failed. Its name,
`/aether_map_<pid>_<token>`, ran to 43 characters with the random 64-bit
token, and macOS refuses a shm name over 31 (`ENAMETOOLONG`), so a map never
reached a child there. The name is now `/aem<pid hex>_<token hex>`, 29 at
most.

Platforms: the shm block is POSIX shared memory on Linux and macOS, as before.
Windows, Android and the BSDs take the stubs (unchanged in kind): the map
works in process, `encode`/`decode_outputs` work everywhere, and `to_shm`
returns NULL with `ENOSYS`.

## Tests (seen failing first)

On the old runtime, a C repro: `put(m, "k", "a\0b")` read back 1 byte, and
`to_shm` of a map with a 70,000-byte value was an AddressSanitizer
stack-buffer-overflow; on the Mac, `to_shm` of any map returned NULL
(`File name too long`). Now `tests/runtime/test_runtime_shared_map.c` has
five new cases (NUL in key and value, by token, frozen by whole key; a full
map refuses; the layout-2 bytes exactly, a child's block not overwriting an
input, seven malformed blocks refused with the map unchanged; too large is
`E2BIG`; a real shm round trip, NULs both ways), and
`std/sharedmap/test_sharedmap.ae` four. stunt's map checks (`map-get`,
`map-set`, `map-overwrite-input`) pass in-process and over the socket.

## Downstream, after the release

Every bridge that calls the map passes lengths now, and the two readers of the
block in other languages move to layout 2:

- **Readers and writers of the block** (they parse it themselves):
  `go/rows/cgo.go` 126-240 (`shmLoad`, `next`, `store`: the
  NUL-separated parse and write) and `java/src/AetherMap.java` 45-120
  (`readFromShm`, `writeToShm`). Both need the layout-2 header and
  length-prefixed entries, and should refuse a block without the magic.
- **C bridges** (compile errors until updated): `quickjs/aether_host_quickjs.c`
  1021 and 1042 (and the `WHY_MAP` refusals at 193, 1017-1040 can go: the
  map carries a NUL now), `python/aether_host_python.c` 419 and 432,
  `perl/aether_host_perl.c` 321, 326, 336 (`key_at_by_token` takes a length
  out), `racket/aether_host_racket.c` 582 and 586; the containment
  `host_support.c` of go, racket, python and quickjs (lines 16-37 / 12-33:
  `put` and `get` with lengths), `python/test_support.c` 69 and 75. The
  hand-written `extern void aether_shared_map_freeze_inputs_by_token` in go,
  racket, python, perl and quickjs can become the header's.
- **Aether tests**: `quickjs/test_host_quickjs.ae` 87-173 declares the old
  externs; use `import std.sharedmap` (`sharedmap.put`, `sharedmap.get`).
  The refusal case at 108-133 becomes a round trip.
