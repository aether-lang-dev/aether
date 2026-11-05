- **The spawn signal-mask test gives back what it captures (#3547).** Its C
  harness never released the string `os_run_capture_raw` hands over, so
  LeakSanitizer ended it with exit 23 before stdout (a pipe) was flushed,
  and the ASan run reported "spawned child inherited the parent's blocked
  mask" with no output, though the child's mask was clear. The harness
  releases the capture and flushes its verdict first, and the script says
  when the harness exits without one.
