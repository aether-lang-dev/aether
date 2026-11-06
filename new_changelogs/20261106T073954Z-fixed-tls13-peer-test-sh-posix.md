- **Shell tests no longer put `NAME=value` in front of a shell function.** Under
  macOS `/bin/sh` (bash in POSIX mode) such an assignment stays set after the
  call, so later calls inherit it; bash, which the sweep uses, drops it. Run by
  hand as `sh test.sh`, `crypto_tls13_peer` failed 26-30 cases (ticket,
  server-alert and HelloRetryRequest cases got settings left by earlier ones)
  and `aea_artifacts` failed 2. Both are fixed, along with the same idiom in
  `crypto_tls13_mtls_signature`, `crypto_tls13_resume`,
  `crypto_tls13_signature_check`, `https_pure_server_keys`,
  `https_server_tls_pem` and `https_tls_reload`. `make check-tests` now runs
  `tests/scripts/check_shell_prefix.py`, which fails on a new one and says why.
