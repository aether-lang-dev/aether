- **Tests signal the servers they started by job, never by a remembered pid
  (#2479).** The `Build + Test (Windows)` jobs have intermittently died
  partway through the shell tests with exit code 2304, which is an MSYS2
  shell killed by SIGKILL, and no test named. In both recorded deaths, one
  on `main` and one on a PR, the long-running test still in flight was
  `http_reverse_proxy_pool`, at the point where it tears down. That test,
  and its `_extra` half, `disown`ed their servers and later sent `kill -9`
  to the remembered pid numbers. A server that had already exited (a lost
  port bind, which the test retries) gives its number back, and MSYS2 reuses
  pid numbers out of order, so that SIGKILL could reach an unrelated
  process. Both tests now keep their servers as jobs of the test shell and
  signal them by job (`tests/lib/server_jobs.sh`): bash resolves a job when
  it signals, and fails once the job is gone, so only a live child of the
  test is ever killed. Liveness checks match running jobs by pid, not by the
  state word, which bash prints in the locale's language. The Windows jobs
  now run the sweep with `AE_SWEEP_RESOURCE_TRACE=1`, so each test's start
  is logged and a recurrence would name every test in flight.
