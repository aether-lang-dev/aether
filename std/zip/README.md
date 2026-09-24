# std.zip

A ZIP archive **reader and writer**, over an in-memory byte buffer.

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

## Writing

The mirror of the reader: build an archive in memory, entry by entry, and get
back the raw `.zip` bytes — no path or fd, the same posture as `open`. Writing
an entry to disk is a layer on top and a separate concern.

```aether,fragment
import std.zip

w = zip.writer_new()
zip.writer_add(w, "a.txt", data_a, len_a, zip.METHOD_DEFLATE, 6) // level 0..9
zip.writer_add(w, "b.bin", data_b, len_b, zip.METHOD_STORED, 0)
zip.writer_add_dir(w, "sub")                                     // a directory entry
out, outlen, err = zip.writer_finish(w)   // `out` is a complete, valid .zip
if string.equals(err, "") != 1 { /* handle */ }
// ... use out (outlen bytes): fs.write_binary, a socket, another archive ...
zip.writer_free(w)
```

`create(name, data, len, method, level)` is the one-shot form for a
single-entry archive. A `METHOD_DEFLATE` entry falls back to `stored` when
compression would not shrink it, and ZIP64 records are emitted automatically
when a size, offset, or entry count exceeds its 32-bit field — the same
records the reader parses back, so a written archive round-trips through
`open`.

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

Extraction to disk (which would layer traversal- and zip-bomb-guards over the
reader, as `std.tar`'s `extract` does), and a directory-tree writer on top of
`writer_add`. Filed as follow-ups on the tracking issue.

## Exports

Reader: `METHOD_STORED`, `METHOD_DEFLATE`; `open`, `close`; `count`, `find`;
`entry_name`, `entry_size`, `entry_compressed_size`, `entry_method`,
`entry_crc32`, `entry_is_dir`, `entry_is_encrypted`; `entry_read`.

Writer: `writer_new`, `writer_add`, `writer_add_dir`, `writer_finish`,
`writer_free`; `create`.
