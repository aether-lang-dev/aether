- **`std.xml` no longer reads a control byte, a quote or `&` as part of a
  name (#3749).** Inside a start tag the reader took every byte up to a
  space, `<`, `>`, `/`, `=` or `?` as the element's or attribute's name, so
  `<b\x0f>` was an element called `b\x0f`, a name XML cannot hold, and the
  pretty printer wrote it back out. Such a byte now ends the name and the
  tag is refused as malformed. Names are otherwise read as leniently as
  before. Found by the XML fuzz target.
  An empty CDATA section (`<![CDATA[]]>`) is no text event: the reader
  gave one with no characters, which the builder's copy of the document
  (writing empty text as nothing) did not have.
