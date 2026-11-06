- **`json.pretty(src, indent)` re-indents a JSON document.** The JSON
  counterpart of `xml.pretty`: `JSON.stringify(v, null, indent)` layout at
  1 to 16, compact at 0. It re-indents the document's own text rather than
  re-serialising it, so key order, number spellings (`1.0`, `1e3`, integers
  past 2^53), escapes and duplicate keys come out as they went in; a
  malformed document returns the parser's message with its line and column.
  Asked for by servirtium-vcr, which pretty-prints recorded XML bodies and
  had no way to do the same for JSON.
