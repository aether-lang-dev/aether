- **One base64 decoder instead of three, and a test that Java4Shell's
  trigger does no harm here.** The HTTP basic-auth middleware and the h2c
  upgrade's `HTTP2-Settings` each had a private decoder; both now call
  `std.cryptography`'s, through a new C entry point,
  `cryptography_base64_decode_buf`, that never sniffs its input for an
  AetherString header. The basic-auth copy still skipped `=` anywhere, so
  `Basic YWxp=Y2U6c2VjcmV0` let `alice:secret` in; it is now a 400.
  `tests/regression/base64_padded_tail_no_overflow.ae` feeds the decoder
  OpenJDK's failing tails (`=`, `==`, `X=`, `X==`, `XY=`, `===`, CVE-2026-114938)
  after every prefix length up to 1024 characters, in both alphabets; each is
  rejected, and the ASan sweep checks no store leaves the buffer. The decoder
  stays scalar, and its comment says why a vector path isn't worth adding.
