- **Every stage of `make ci` has a time bound, and a hung C test is named.**
  The C unit tests, the doc and standalone checks, the examples, the install,
  differential and archive checks, and each test's build in the `.ae` sweep
  ran without one, so a hang ran for as long as the job could. Five Windows
  runs ended about an hour in with the runner lost and no log (#2608). A
  stage that runs past its bound now fails as `[TIMEOUT] stage <name>` with
  the processes still running and their memory, and the C test harness ends a
  test that runs past `AE_C_TEST_TIMEOUT` seconds (120 by default) with its
  name. The `ae test` smoke check no longer reports a pass when `ae test`
  fails.
