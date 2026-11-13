- **`test_runner --category=NAME` runs every test of the category (#3660).** The C
  unit runner registered the tests it lists by hand (the scheduler, mailbox,
  work-stealing, ask, actor death and rebirth suites, 69 tests) only on its
  way to the full run, after the `--category` branch had already returned.
  `--category=runtime` ran 110 of the 180 runtime tests and reported a pass.
  The runner now registers every test before it reads its arguments, and a
  new `--list` mode prints each registered test with its category; `make
  test` checks that list against every `register_test_with_category()` call
  in the runner's sources.
