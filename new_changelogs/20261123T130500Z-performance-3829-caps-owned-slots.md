- **Memory accounting counts without a locked add (#3829).** Every
  cap-aware allocation and free added its size to the calling thread's slot
  with an atomic add, a locked instruction, although a slot has one writer
  until more than 63 threads count. Each of the first 63 threads now owns a
  slot and counts on it with a plain load and store; later threads share
  the last slot, which keeps the atomic add. Setting a cap still moves every
  slot into the capped counter, by recording how much of each it has moved
  rather than emptying it, so no count is lost. A malloc and free of 40
  bytes: 30.4 to 24.4 ns.
