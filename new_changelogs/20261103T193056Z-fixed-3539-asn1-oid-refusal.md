- **`asn1.encode_oid` no longer leaks when it refuses a string.** It builds
  the content bytes while it reads the arcs, and each refusal (an empty arc,
  an arc out of range, a non-digit, a single arc) returned null without
  freeing them, so every string that was not an OID leaked the buffer.
