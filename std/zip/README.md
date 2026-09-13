# std.zip

A ZIP archive **reader**, over an in-memory byte buffer.

A `.zip` is a container, not a compressed blob: entries (each *stored* or
*deflate*d) indexed by a central directory, located from an end-of-central-
directory record at the tail. `std.zlib` supplies the decompression; this module
parses the container.

**Buffer, not filesystem.** `open(data, len)` reads a `.zip` already in memory —
downloaded over HTTP, unpacked from another archive, embedded in a payload —
with no path or file descriptor, the same posture as `std.json` / `std.cbor` /
`std.resp`. (An extract-to-disk layer belongs on top and is a separate concern.)

```aether,fragment
import std.zip

// `data` is the raw bytes of a .zip (from std.fs.read_binary, a socket, …).
ar, err = zip.open(data, len)
if string.equals(err, "") != 1 { /* not a zip, or truncated */ }

n = zip.count(ar)
i = 0
while i < n {
    name = zip.entry_name(ar, i)
    if zip.entry_is_dir(ar, i) == 0 {
        body, blen, rerr = zip.entry_read(ar, i)   // decompressed + CRC-checked
        // ... use body (blen bytes) ...
    }
    i = i + 1
}
zip.close(ar)
```

`find(ar, name)` returns an entry index by exact name, or `-1`.

## What it covers

- **Methods**: `stored` (0) and `deflate` (8, via `std.zlib`'s raw inflate).
  Any other method is refused with a clear error, not silently mis-decoded.
- **ZIP64**: 64-bit sizes, offsets and entry counts (archives over 4 GiB or
  past 65535 entries) via the ZIP64 EOCD record and locator.
- **Integrity**: every `entry_read` verifies the entry's CRC-32 (and, for
  deflate, the decompressed size) against the central directory, and returns an
  error on mismatch.
- **Metadata from the central directory**, not the local file headers (a local
  header may zero its sizes and defer them to a trailing data descriptor).

## What it refuses (clearly, not silently)

Encrypted entries (general-purpose bit 0), multi-disk / spanned archives, and
compression methods other than stored/deflate each return a descriptive error
rather than garbage. A buffer with no EOCD record (not a zip, or truncated) is
rejected by `open`.

## Not in scope (yet)

Writing archives, and extraction to disk (which would layer traversal- and
zip-bomb-guards over this reader, as `std.tar`'s `extract` does). Filed as
follow-ups on the tracking issue.

## Exports

`METHOD_STORED`, `METHOD_DEFLATE`; `open`, `close`; `count`, `find`;
`entry_name`, `entry_size`, `entry_compressed_size`, `entry_method`,
`entry_crc32`, `entry_is_dir`, `entry_is_encrypted`; `entry_read`.
