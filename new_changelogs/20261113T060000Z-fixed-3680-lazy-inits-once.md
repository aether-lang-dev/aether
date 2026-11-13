- **Runtime and std: every lazy one-time initialisation is a real once
  (#3680).** Twelve more caches and one-time setups were guarded by a plain
  flag that any thread could reach first: the CPU feature record, the
  Windows clock frequency, the NUMA topology, the runtime configuration,
  the hardware capabilities, the trace switch, the SIMD probe, std.json's
  character table, the HTTP/2 base64 table, the OpenSSL legacy provider
  for MD4, the HTTP server's OpenSSL init and the sandbox's one warning.
  Two threads could both run the setup, and one could read a table or
  record half written, or go on before the setup it needed was done. They
  go through a new `aether_once` (runtime/utils/aether_once.h) now, whose
  done flag is published with release and read with acquire, or through
  an atomic where the value is all there is.
