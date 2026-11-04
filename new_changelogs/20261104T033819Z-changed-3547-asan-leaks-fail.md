- **`make test-asan-ae` fails on a leak (#3547).** LeakSanitizer's leaks
  were only listed (`SAN_LEAKS=report`) while the test programs that never
  freed what they made were fixed: the crypto, TLS, HTTP, syntax,
  regression and integration programs, the C hosts and two tools (vkgen,
  and docgen, which leaked the names of an exports list that never closes).
  A leak now fails the run, as a memory error does, in
  `tests/scripts/sanitizer_sweep.sh`, the Makefile's `test-asan-ae` and
  `asan.yml`; `SAN_LEAKS=report` still lists them without failing.
