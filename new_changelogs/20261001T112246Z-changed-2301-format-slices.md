- **`std.zip`, `std.resp` and `std.http1` take `byte[]` input (#2301).**
  `zip.open(data)`, `zip.extract(data, dest, opts)`,
  `zip.create(name, data, method, level)` and
  `zip.writer_add(w, name, data, method, level)` drop their length argument;
  the writer takes each entry's CRC-32 straight from the slice instead of
  copying it into a scratch buffer first. `resp.new_bulk(data)` and
  `resp.parse_prefix(data)` take a slice, and the RESP parser now indexes it
  directly instead of reading byte-by-byte through a length-checked string
  accessor; a frame cut short by the slice's bound is reported incomplete.
  `resp.parse(s)` still takes a string. `http1.feed(r, chunk, is_eof)` takes a
  slice, the parser works over a view of its accumulator, and
  `http1.body(r)` returns the decoded body as a borrowed `byte[]` view,
  replacing `body_ptr` / `body_len`. Pass a string as `string.bytes(s)` and a
  `std.bytes` buffer as `bytes.view(buf)`. Every caller and doc example was
  moved.
