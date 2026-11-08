- **`mem.steady_growth` on Windows no longer charges a round with the
  parallel loader's ~1 KB.** The loader's second worker thread first runs
  some tens of milliseconds into a process and allocates 1048 bytes (136 +
  912) of the process heap; the loader reports no thread event for its
  workers, so `mem.thread_epoch()` did not move and a round it overlapped
  showed the growth (`test_string_keep_registry_steady` failed about 2 runs
  in 100 on Windows 11). `thread_epoch` now also moves with the CPU time of
  threads the loader does not report, and a run spoiled by a thread no
  longer uses up one of `steady_growth`'s tries (up to 8 such reruns), so
  `tries` 1 compares one clean run.
