# std.truetype

Reads a TrueType font from its bytes: which glyph draws a character, how far
the pen moves, how two glyphs kern, and the glyph's outline, either as the
font's own contours or flattened to line segments at a scale. It is the font
layer a text renderer builds on. A vector UI strokes the contours as paths;
a game bakes the segments into a signed-distance-field atlas.

What it reads: the table directory (and the first font of a `.ttc`
collection), `head`, `maxp`, `hhea`, `hmtx`, `cmap` formats 4 and 12, `loca`
and `glyf` (simple and compound glyphs), `kern` format 0, and the face's own
declarations: its full name (`name`), fixed pitch (`post`) and serif style
(`OS/2` PANOSE). It does not read CFF outlines (`.otf`), GPOS or hinting. A
CFF font fails to load and says so.

Every read is bounds-checked against the file, and the work one glyph can
claim is capped, so a malformed font fails with a message naming what is
wrong.

```aether
import std.truetype

main() {
    face = truetype.load("DejaVuSans.ttf")
    if face == null {
        println("cannot load: ${truetype.last_error()}")
        return
    }
    println("${truetype.full_name(face)}: ${face.units_per_em} units per em")

    a = truetype.glyph_index(face, 65)          // 'A'; 0 is the missing glyph
    v = truetype.glyph_index(face, 86)          // 'V'
    println("advance ${truetype.advance(face, a)}, kern A-V ${truetype.kerning(face, a, v)}")

    // The contours as the font gives them: font units, on and off the curve.
    p = truetype.glyph_points(face, a)
    if p != null {
        println("${p.count} points in ${p.contours} contours")
        truetype.points_free(p)
    }

    // Flattened at 64 pixels per em, every curve within 0.02 px of true.
    scale = 64.0 / (face.units_per_em as float)
    o = truetype.outline(face, a, scale, 0.02)
    if o != null {
        x0, y0, x1, y1 = truetype.segment(o, 0)
        println("${o.count} segments, the first (${x0}, ${y0}) to (${x1}, ${y1})")
        truetype.outline_free(o)
    }

    truetype.typeface_free(face)
}
```

`load_bytes(data, size, name)` reads a font already in memory, for example an
entry in an asset pack. The bytes are copied.

Metrics are in font units (`face.units_per_em` to the em), and y points up.
The `Typeface` fields `ascender`, `descender` and `line_gap` give the line
height. `glyph_bounds` gives a glyph's box as the font declares it. An
outline's `x_min`/`y_min`/`x_max`/`y_max` give the box of the segments
actually drawn.
