- **`make test-parallel` fails when a category fails, and runs them all (#3661).** It
  started one test runner per category in the background and ended with a
  bare `wait`, whose status is 0 whatever the runners returned, so a failing
  category still printed "All parallel tests complete!" and passed. It also
  left out the `other` category, 267 of the runner's 693 tests on MinGW. It
  now waits for each runner, shows the log of each category that failed, and
  fails with it, over all eight categories.
