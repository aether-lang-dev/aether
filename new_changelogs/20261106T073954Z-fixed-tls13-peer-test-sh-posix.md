- **`sh tests/integration/crypto_tls13_peer/test_tls13_peer.sh` no longer fails
  26-30 cases.** The test passes its per-case settings as `CENV=... run_case ...`
  (also `CL`, `SENV`, `SV`, `POST`, `HRR`, `GROUP`, `EARLY`). In bash such an
  assignment lasts for that one call, but under `sh` on macOS (bash in POSIX
  mode) it stays set, so every later case inherited e.g. `CLIENT_TICKET=1` or
  `SERVER_READ=1` and failed (ticket, server-alert and HelloRetryRequest cases,
  with intermittent `send failed`). The sweep runs the test with `bash`, which
  is why `make test-ae` passed. The test now clears the settings after each
  case, so it behaves the same under either shell.
