- **`std.cbor` and `std.msgpack` refuse what is not one well-formed item,
  and `cbor.diagnose` writes values exactly (#2470).** `cbor.parse` and
  `msgpack.unpack` ignored bytes after the item, so `0102` decoded as 1;
  that is now an error. `cbor.parse` also accepted a nested
  indefinite-length string chunk (`5f5f4101ffff`), simple value 20 in two
  bytes (`f814`) and invalid UTF-8 in a text string (`62c328`), and refused
  the well-formed `simple(0)` to `simple(19)`. It now refuses every case of
  RFC 8949 Appendix F and invalid UTF-8, and accepts every example of
  Appendix A except the two integers past 2^63, which `get_long` cannot
  hold. `diagnose` printed floats through `%g` (`123456789.0` came out as
  `1.23457e+08`), wrote control characters raw, and showed indefinite-length
  items as definite ones. It now writes the shortest decimal that reads back
  as the same float, JSON escapes, and the RFC 8949 §8.1 encoding indicators
  (`[_ 1, 2]`, `(_ h'01', h'02')`, `1.5_3`). A parsed value keeps how it was
  encoded, so `encode` writes it back byte for byte, and a float built with
  `num` encodes in the narrowest exact width instead of always as a double.
  `map_get` now matches an array or map key by content. `msgpack.map_set`
  freed the old value even when it was the value being set, leaving the map
  holding freed memory.
- **`std.xml` reports the first well-formedness error and decodes character
  references per XML 1.0 (#2471).** A mismatched end tag (`<a><b></a>`), an
  end tag with nothing open (`</x>`) or no name (`<a></>`), a document ending
  inside an element (`<root><item>`) and attributes not separated by
  whitespace (`<a x="1"y="2"/>`) all reached EOF with no error. They are now
  `EVENT_ERROR`, and `xml.error` gives the line, column and byte offset. A
  character reference must name a legal XML Char: `&#0;` used to cut the
  text short, a surrogate came out as invalid UTF-8 and a reference past
  U+10FFFF vanished; these and malformed references are now errors.
