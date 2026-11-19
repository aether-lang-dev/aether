- **`audit.denied_total()`: a denial count that does not roll off.** The
  audit ring keeps the latest 256 checks, allowed ones included, so
  `audit.denied_count()` read 0 once 256 allowed checks followed a denial.
  `denied_total()` (a `long`) counts every denial the in-process sandbox has
  recorded since the process started, on any thread (the ring is
  process-wide, and so is the total); `clear()` does not reset it. Take it
  before and after a piece of work to count that work's denials.
  `std/audit/test_audit.ae`.
