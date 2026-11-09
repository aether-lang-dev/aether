- **The Windows HTTP client leak checks no longer fail on Winsock's
  context cache.** Winsock keeps a closed socket's context (two 256-byte
  blocks of the process heap, from ws2_32 and mswsock) and reuses it. It
  makes new ones only when more sockets are open at once than ever before,
  so a round of requests could grow the heap by 512 bytes a socket with
  nothing leaked. `test_http_client_framing_error_no_leak` failed on it
  (v0.901.0's release; main's Windows build on 2026-11-05), and
  `test_http_client_dial_failure_no_leak` was exposed to it too. Both now
  hold 16 connections open at once before measuring, which gives Winsock
  more contexts than any round uses.
