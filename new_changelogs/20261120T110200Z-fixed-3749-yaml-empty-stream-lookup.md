- **std.yaml: an empty stream is an empty document, and keys are looked up
  as text (#3749).** `yaml.parse` of a stream with no document in it (empty,
  or only comments) returned no document and no error; it returns a
  document with no root, which `emit_document` writes as the empty text.
  `mapping_lookup` read its key as YAML source, so a key such as `a: b`,
  `[x]`, `#c` or a block scalar's text was not found in a mapping holding
  it; keys are compared as the text they are (YAML source naming a
  non-scalar key is still looked up as before). Found by the new YAML fuzz
  target.
