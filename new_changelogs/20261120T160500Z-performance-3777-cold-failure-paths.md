- **A failed bounds check's helpers are out of line, cold and noreturn.**
  They were `static inline`, so cl.exe, -O0 and -Og builds inlined the
  message's argument setup and its variadic call into every indexing site;
  the checks themselves stay inline. The runtime's rare paths (a send to an
  actor that has ended, a send while the scheduler is stopped, a message
  dropped, a queue's overflow) are marked cold too, which moves them to
  `.text.unlikely` (#3777).
