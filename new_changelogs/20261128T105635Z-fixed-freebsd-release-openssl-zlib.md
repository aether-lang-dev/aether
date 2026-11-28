- **The FreeBSD release links OpenSSL and zlib.** The FreeBSD binary is
  cross-built on Linux, and that build turned every optional library off
  because the host's pkg-config answers for the wrong system, OpenSSL and
  zlib included. So every FreeBSD release up to 0.949.0 shipped `std.zlib`
  as its stub (`zlib.available()` was 0, gzip refused, and anything decoding
  a gzipped HTTP body reported zlib unavailable) and had no OpenSSL: an
  `https://` request worked only through the pure-Aether TLS 1.3 client, set
  as the backend by hand, TLS 1.2 peers could not be reached, and a program
  linking `-lssl -lcrypto` itself did not link. Both are part of FreeBSD's
  base system, so the cross build now links the base sysroot's
  `-lssl -lcrypto -lz`, as a FreeBSD build from source always has, and the
  release and pull-request FreeBSD legs fail if the archive is built without
  them (`scripts/check_base_libs.sh`). The pure-Aether TLS client and server
  are unchanged.
