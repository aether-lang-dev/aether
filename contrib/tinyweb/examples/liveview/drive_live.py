#!/usr/bin/env python3
# Drives the LiveView-lite spike over its WebSocket channel and asserts the loop:
#   mount -> 0, inc -> 1, inc -> 2, dec -> 1, reset -> 0
import sys, re, time
from websocket import create_connection

URL = "ws://127.0.0.1:8126/live"

def count_of(html):
    m = re.search(r"count:\s*(-?\d+)", html)
    assert m, f"no count in frame: {html!r}"
    return int(m.group(1))

def step(ws, msg, expect):
    ws.send(msg)
    frame = ws.recv()
    got = count_of(frame)
    ok = "OK" if got == expect else "FAIL"
    print(f"  send {msg!r:24} -> count {got:<3} (expected {expect})  [{ok}]")
    assert got == expect, f"expected {expect}, got {got}"

ws = create_connection(URL, timeout=5)
try:
    step(ws, "mount", 0)
    step(ws, '{"event":"inc"}', 1)
    step(ws, '{"event":"inc"}', 2)
    step(ws, '{"event":"dec"}', 1)
    step(ws, '{"event":"reset"}', 0)
    print("ALL PASS: the LiveView loop (mount/handle_event/render/push) works")
finally:
    ws.close()
