- **`std.yaml` emits a block scalar that ends the input whole, and a
  document no longer borrows the text it was parsed from (#3749).** A
  document whose last line was a block scalar's, with no line break after
  it (`k: >\n  abc`), emitted as `k: >` with the content gone, and read back
  as an empty scalar: libfyaml's emitter writes a block scalar from its
  source lines and takes the last one only when a line break follows it.
  `parse` now gives libfyaml its own copy of the text, ending in a line
  break, which the document owns and `free` releases; values read as before.
  The document used to point into the caller's string, which it kept
  reading to emit. Found by the YAML fuzz target.
