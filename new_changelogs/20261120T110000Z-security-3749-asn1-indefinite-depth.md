- **A certificate with deeply nested indefinite lengths no longer overflows
  the stack (#3749).** `std.cryptography.asn1` read an indefinite-length TLV
  by calling itself for each TLV inside it, with no bound, so 30000 nested
  `30 80` (60 KB, a certificate a TLS peer may send) crashed `read_tlv` and
  `tls13_cert.parse_certificate` with a stack overflow. Indefinite lengths
  now nest at most 64 deep; past that the read is an error. Found by the new
  X.509 fuzz target.
