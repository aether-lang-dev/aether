- **Ten more Wycheproof drivers free their vector file, and the pure-server
  key test's server frees itself before it refuses.** The ASan and LSan sweep
  (`SAN_LEAKS=fail`, as CI's leak gates run it) failed `wycheproof_ed448_cmac`
  and `wycheproof_rsa_decrypt`: `wp_aes_cmac`, `wp_ecdh_secp256k1`,
  `wp_ecdsa_p256`, `wp_ecdsa_p256_der`, `wp_ecdsa_p384`,
  `wp_ecdsa_secp256k1`, `wp_ecdsa_secp256k1_der`, `wp_rsa_oaep`, `wp_x25519`
  and `wp_x448` parsed their file with `json.parse` and never `json.free`d
  it (an arena of up to half a megabyte each), the same leak as the ten drivers
  already fixed. `https_pure_server_keys/server.ae` left its `http.server_create`
  allocation to the OS on each of its three refusal exits; under LeakSanitizer
  the leak report ends the process before libc flushes stdout, so the `FAIL:
  server_set_tls: ...` line the test reads was lost and "an RSA-1024 key"
  read "(no output; the server exited)". No behaviour of any test changes.
