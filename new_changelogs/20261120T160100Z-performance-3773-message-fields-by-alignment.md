- **A message's fields are laid out by alignment.** They were ranked by
  kind (ints and bools, then pointers, then everything else in declaration
  order), so a small field between two 8-byte ones cost padding:
  `{ a: long, b: byte, c: long }` was 32 bytes, past the 28 a message
  carries inline, and each send allocated a copy the receiver freed. Now the
  4-byte fields follow the id, then the 2- and 1-byte ones, then the 8-byte
  and pointer fields, then wider ones, declaration order kept within each:
  that message is 24 bytes and travels inline (`send_long_byte_long`, 47.6
  to 36.6 ns per message). The order comes from a new table of the C layout
  of Aether types (compiler/analysis/type_layout.c). A C host built against
  a `--emit-header` header from an earlier Aether must generate it again
  (#3773).
