- **`asn1.last_tag_class`, `last_tag_pc` and `last_tag_number` exist
  (#3749).** `std.cryptography.asn1` exported them and defined none, so a
  program calling one did not compile. They give the class, the constructed
  bit and the number of the tag `read_tlv` read last.
