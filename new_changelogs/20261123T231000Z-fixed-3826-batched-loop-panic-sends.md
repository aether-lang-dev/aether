- **A panic out of a batched loop sends what the loop sent.** Outside an
  actor, a `while` or `for` loop that only sends keeps its sends in the
  thread's batch, sent at the loop's end. A panic in the loop, caught by a
  `try` around it, jumped past that end: the messages stayed in the batch
  until the thread's next send or wait, and on a thread with nothing more to
  send (a worker's) they were never sent. The open batch is now an entry of
  the unwind journal: the unwind to the `try` sends it, in order, and closes
  it (#3826).
