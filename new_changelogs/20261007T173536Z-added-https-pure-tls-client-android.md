- **`std.http.client` HTTPS on the pure-Aether TLS client works on Android, and
  can be selected anywhere.** A build without OpenSSL (every
  `ae build --target=` cross-build, including aarch64-linux-android) already
  routed https through `std.cryptography.tls13_client` once the program
  imported it, but its trust store had to be a PEM bundle file, and Android has
  none, so every https request there failed. The pure client now also loads
  directories of PEM files: `SSL_CERT_DIR` (a `:`-separated list, `;` on
  Windows, as OpenSSL reads it) alongside `SSL_CERT_FILE`, and when neither is
  set, after the system bundles, Android's `/apex/com.android.conscrypt/cacerts`
  and `/system/etc/security/cacerts` and then `/etc/ssl/certs`. Nothing loading
  still fails closed. `tls13_cert.trust_store_load` accepts a directory, and the
  new `tls13_cert.trust_store_add` appends a file or directory to an existing
  anchor list. In a build WITH OpenSSL, `AETHER_PURE_TLS=1`, which already
  selected the pure server, now selects the pure client too (OpenSSL stays the
  default; a request fails naming the import if the program does not link the
  pure client). The pool keys connections by backend, so the two never share
  one. The public `std.http.client` API is unchanged
  (tests/integration/https_client_pure_tls).
