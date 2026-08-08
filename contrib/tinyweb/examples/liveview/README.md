# LiveView-lite spike

A proof that Phoenix-LiveView's core loop — `mount → handle_event → render →
push` — works on Aether's own primitives (tinyweb WebSocket + per-connection
state), with **no** `~H` template-diff compiler and **no** JS framework.

This is a *spike*, not the library. The staged plan for turning it into a real
`contrib/tinyweb/live_view` module lives in
[`docs/liveview-lite-roadmap.md`](../../../../docs/liveview-lite-roadmap.md).

## Files

- `example_counter_live.ae` — the server. Drives `tinyweb.ws_accept_loop` directly
  (WS-only) rather than through `tw_start`; see the roadmap's "structural
  blocker" section for why. Per-connection count kept server-side, keyed by the
  connection's socket ptr.
- `drive_live.py` — a Python `websocket-client` driver that asserts the loop:
  `mount → 0, inc → 1, inc → 2, dec → 1, reset → 0`.

## Run it

```sh
# build (needs the WS handshake C extern)
ae build example_counter_live.ae --extra ../../ws_handshake.c -o /tmp/counter_live

# start it detached, then drive it
setsid /tmp/counter_live > /tmp/cl.log 2>&1 < /dev/null &
pip install websocket-client   # if needed
python3 drive_live.py
```

Expected:

```
  send 'mount'                  -> count 0   (expected 0)  [OK]
  send '{"event":"inc"}'        -> count 1   (expected 1)  [OK]
  send '{"event":"inc"}'        -> count 2   (expected 2)  [OK]
  send '{"event":"dec"}'        -> count 1   (expected 1)  [OK]
  send '{"event":"reset"}'      -> count 0   (expected 0)  [OK]
ALL PASS: the LiveView loop (mount/handle_event/render/push) works
```

The spike uses a pre-protocol shape (bare `"mount"` up, raw HTML down). The
roadmap's "Wire protocol" section defines the JSON envelope a real
implementation should adopt from Tier A3 on.
