- **`std.cryptography.tls13_record` enforces the RFC 8446 record-size and
  sequence-number limits.** `seal_record` refused nothing and `open_record`
  trusted the peer's declared length, so a record claiming a huge size drove
  the buffer allocation, and the 64-bit sequence counter could wrap. Now the
  seal path rejects a plaintext fragment over 2^14 bytes (§5.1), the open path
  rejects a TLSCiphertext over 2^14 + 256 bytes *before* allocating (§5.2), and
  both refuse to proceed at the sequence-number ceiling rather than reuse a
  (key, nonce) pair (§5.3); a null context is rejected rather than
  dereferenced. `tests/integration/crypto_tls13_record` gains the oversized,
  null-context and boundary cases; the existing round-trip and tamper checks
  are unchanged. (The handshake-parser bounds checks from the same source were
  already present on main and are not duplicated.)
