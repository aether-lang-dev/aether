- **Cryptography and TLS buffers use `byte[]` slices (#2301).** Byte inputs
  and outputs across the cryptographic modules now carry their bounds with
  them, replacing separate pointer-and-length pairs where the data extent is
  known. TLS record, handshake and certificate paths pass bounded views
  through the Aether layer while retaining the existing C callback ABI.
  Cryptographic size checks and buffer ownership remain explicit.
