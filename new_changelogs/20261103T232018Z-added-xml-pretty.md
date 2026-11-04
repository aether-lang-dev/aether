- **`std.xml` pretty-prints.** `xml.pretty(src, indent)` returns
  `(formatted, err)`: the document re-indented with `indent` spaces per level
  (0 to 16), or `""` and a well-formedness error with its line, column and
  byte. It changes only whitespace between elements: text, attribute values,
  references, CDATA, comments and processing instructions are copied as
  written, mixed content and `xml:space="preserve"` stay as they are, and
  formatting its own output gives the same output. `xml.writer_pretty(indent)`
  is a builder whose `finish` returns the document indented; an unfinished
  one comes back as written. For reading and recording (Servirtium's SOAP
  tapes), not for signed XML: changing whitespace breaks XML signatures.
