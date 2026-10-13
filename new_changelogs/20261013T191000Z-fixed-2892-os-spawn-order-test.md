- **The `os.wait_any` completion-order test no longer rests on timing.** It
  checked the order with two `sleep` children a second apart, and under load
  both had exited before `wait_any` looked, so it failed. The slow child now
  never exits on its own: the first `wait_any` can only return the fast one,
  then the test ends the slow child and the second `wait_any` returns it.
  The `wait_any_timeout` check's short child exits at once for the same
  reason (#2892).
