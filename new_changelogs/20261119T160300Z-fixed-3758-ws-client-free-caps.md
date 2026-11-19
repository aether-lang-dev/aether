- **`http.ws_client_free` returns the message buffer to the memory cap
  (#3758).** A WebSocket client handle grows its message buffer through the
  runtime's capped allocator, and freeing the handle released it with plain
  `free()`, so the cap's byte counter kept every freed handle's buffer (4 KiB
  or more each). A program under a memory cap that opened and closed client
  connections eventually ran into the cap while holding nothing. The buffer
  is now released through the cap with the size it holds.
