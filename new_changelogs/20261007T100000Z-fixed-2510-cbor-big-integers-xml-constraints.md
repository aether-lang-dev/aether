- **`std.cbor` and `std.msgpack` read integers past the signed 64-bit range
  (#2510).** `cbor.parse` refused major type 0 values of 2^63 and up and
  major type 1 values below -2^63, among them RFC 8949 Appendix A's
  `1bffffffffffffffff` and `3bffffffffffffffff`, and `msgpack.unpack`
  refused a uint 64 of 2^63 and up, all with "integer out of range". They
  now decode, encode back byte for byte, and diagnose exactly
  (`18446744073709551615`, `-18446744073709551616`). `get_long` and
  `get_int` give 0 for them, as before for what is not an integer; the new
  `get_int64` returns the value with an error when it does not fit, and
  `get_uint` (and in CBOR `get_nint`, the argument n of -1 - n) reads them
  as the bit pattern of a long, as std passes unsigned 64-bit values.
  `from_uint` and `from_nint` build them. A CBOR string length or count of
  2^63 or more is refused as longer than the input.
- **`std.xml` enforces more of XML 1.0's well-formedness (#2510).** A
  document with no root element or a second one, text or CDATA outside the
  root element, an attribute named twice in one tag, a raw control
  character other than tab, LF and CR, a `&` that starts no reference, and
  a reference to an entity other than the five predefined ones (`&nbsp;`)
  all read without an error, the last two passed through verbatim. Each is
  now `EVENT_ERROR` with its line, column and byte offset. A UTF-8 byte
  order mark before the root element is accepted.
