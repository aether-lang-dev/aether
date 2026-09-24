# contrib.png

Encodes 8-bit RGBA pixels as a PNG. It is the image writer behind
`contrib.vulkan`'s `save_png`, and it works on any RGBA buffer.

```aether
import std.bytes
import contrib.png

main() {
    w = 64
    h = 32
    pixels = bytes.new(w * h * 4)
    bytes.set_length(pixels, w * h * 4)
    // A horizontal red ramp, opaque.
    y = 0
    while y < h {
        x = 0
        while x < w {
            bytes.set(pixels, (y * w + x) * 4, x * 4)
            bytes.set(pixels, (y * w + x) * 4 + 3, 255)
            x = x + 1
        }
        y = y + 1
    }
    err = png.write_rgba8("ramp.png", bytes.data(pixels), w, h)
    if err != "" { println("could not write: ${err}") }
    bytes.free(pixels)
}
```

## API

| Call | Returns |
|---|---|
| `png.encode_rgba8(pixels, width, height)` | `(data, length, err)`: the file in memory |
| `png.write_rgba8(path, pixels, width, height)` | `err`, `""` on success |

`pixels` points at `width * height * 4` bytes, rows top to bottom, each pixel
R, G, B, A (a `std.bytes` buffer's `bytes.data(b)`, or what
`vulkan.copy_rgba8` fills). Sizes up to 16384 on a side are accepted; zero,
negative and larger sizes, and a null pointer, are refused with a reason.

## What it writes

A standard PNG that any decoder reads: colour type 6 (RGBA), 8 bits a
channel, not interlaced, one `IDAT` chunk. Each row is filtered with the
PNG filter (None, Sub, Up, Average or Paeth) that leaves the smallest sum of
absolute differences, which is the heuristic libpng uses. The filtered rows
are compressed with `std.zlib` at level 9, and the chunk checksums are
`std.hash.crc32`.

The only dependency is `std.zlib`. When the toolchain was built without zlib,
`encode_rgba8` returns its error rather than a file.

## Why contrib

It writes one image format. The rubric in
[docs/stdlib-vs-contrib.md](../../docs/stdlib-vs-contrib.md) keeps that out of
the standard library, and the GPU modules, the only in-tree callers, live in
contrib too. Decoding is not here: nothing in the tree needs it yet.

## Testing

`test_png.ae` runs from `make contrib-check`, leak-gated under valgrind. It
decodes the encoder's output with its own reader, separate from the
encoder's code. The reader walks the chunks, checks every CRC, inflates the
data and undoes the five filters as the specification defines them. Every
byte must come back, from an image built so that each filter gets picked.
`contrib/vulkan`'s format test writes a PNG of a rendered frame through the
same path.
