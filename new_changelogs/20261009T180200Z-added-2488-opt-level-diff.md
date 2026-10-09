- **The `.ae` test corpus now also runs at -O0, and must agree with -O2.**
  `ae run` compiles at `-O0` while the sweep only ever compiled at `-O2`, so a
  bug that shows only at `-O0` passed every run. `make test-ae-opt-diff` builds
  every program `make test-ae` builds at both levels (the two read one list,
  `tests/scripts/ae_sweep_list.sh`), runs both from the same path, and compares
  stdout and exit code; the programs whose output varies from run to run
  (clocks, pids, thread interleaving) have their stdout excused in a commented
  carveout file and their exit codes still compared. CI runs it on the
  Linux / GCC leg after `make ci`. Its first run found an arrow body that
  returned a freed string (fixed separately) and
  `tests/integration/test_http_client_v2.ae` printing strings it had borrowed
  from a response after freeing the response (#2488).
