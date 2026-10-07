- **Three macOS leak-gate caps tightened to what the tests actually leak.**
  `tests/leaks_known.txt` allowed `test_rsa_pkcs1` 130 leaks and
  `test_closure_local_shadows_promoted_capture` 1, and both report 0 on every
  macOS ARM64 CI run and locally, so both entries are gone and the tests are
  held to 0 like every unlisted test; `test_closure_local_alloc_capture` is
  capped at its measured 15 instead of 20. A cap far above the real count let a
  new leak of that size through: three blocks injected into `test_rsa_pkcs1`
  passed the old 130 and fail now.
