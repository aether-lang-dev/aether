- **A failing std.spec suite exits non-zero even when `main()` drops the
  verdict.** `spec.run_summary(fw)` as `main()`'s last statement fell off the
  end and exited 0, so `ae run`, `ae test` and any harness reading the status
  took the failing suite for a passing one. `run_summary` now notes a failure
  with the runtime (`aether_note_failure`), and the generated `main()` exits
  with it where it would have exited 0 (`aether_main_status`). A non-zero
  status `main()` returns, or an `exit()`, is kept.
