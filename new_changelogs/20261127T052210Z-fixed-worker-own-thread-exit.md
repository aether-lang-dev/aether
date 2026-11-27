- **A program that ends right after a `worker.run_detached` job finishes no
  longer exits while that job's thread is still exiting.** The thread was
  detached at launch, and its job counted as done (`worker.pending()` back
  to 0) before the thread had freed the job, woken waiters and returned, so
  `main` could return into process exit with the thread half way out. Wine
  crashed on that overlap, about once in a thousand runs. Threads std.worker
  starts outside the pool (`run_detached`, and `run` when the pool cannot
  start) are now joined once their job is done: at the next launch, and at
  exit. A job still running at exit is left to end with the process, as
  before.
