- **std.audio: the engine can be opened, used and closed from several
  threads at once (#3690).** The engine was a set of plain globals with no
  guard: two opens at once both initialised it, and a close on one thread
  freed it under a load, a play or a render on another (four actors churning
  it crashed every run). `open`, `open_headless` and `close` now run under one
  lock, every other call holds a reference that `close` waits for, and an
  `open` during a `close` waits for it. A source or group still live at
  `close` is taken out of the mix before the engine is freed, where the graph
  used to be freed under it; a call on it afterwards fails with
  `last_error()` "audio: the source's engine was closed", and `unload` and
  `group_free` still free it. A program that opens, plays and closes on one
  thread behaves as before. The vendored miniaudio (0.11.25; 0.11.26, the
  latest, has the same code) detached a re-attached node under the wrong
  bus's lock, so two threads loading sounds onto the same output could
  corrupt its list and hang a later close; it is patched in place, with a
  comment at the patched lines. Covered by
  `std/audio/test_audio_threads.ae`.
