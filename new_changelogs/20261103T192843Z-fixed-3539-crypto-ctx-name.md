- **Hash, MAC and DRBG contexts give back their algorithm name when freed.**
  The contexts of `std.cryptography.hmac`, `drbg`, `ascon`, `dstu7564`,
  `gost3411_2012` and `sparkle` keep a reference to the name they were made
  with, and were freed with `free`, which releases no field, so a name that
  was not a literal leaked once per context. `tls13_client` builds its hash
  name at run time, so every resumed connection leaked it.
