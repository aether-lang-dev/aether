# Is the installed `websockets` actually USABLE, not merely importable?
#
# Ubuntu 22.04's python3-websockets is 9.1, which passes loop= to
# asyncio.Lock() / asyncio.sleep() -- an argument removed in Python 3.10. The
# server raises TypeError during connection setup and never replies, which
# from the client side is indistinguishable from a broken handshake. Probing
# with `import websockets` would call that healthy and blame our client.
#
# So complete one real loopback round-trip instead.
#
# exit 0 = usable, 2 = not installed, 3 = installed but broken.
import asyncio, sys

try:
    import websockets
except Exception:
    sys.exit(2)

async def echo(ws, path=None):
    async for m in ws:
        await ws.send(m)

async def main():
    async with websockets.serve(echo, "127.0.0.1", 0) as s:
        port = list(s.sockets)[0].getsockname()[1]
        async with websockets.connect("ws://127.0.0.1:%d/" % port) as c:
            await c.send("x")
            if await c.recv() != "x":
                sys.exit(3)

try:
    asyncio.run(asyncio.wait_for(main(), 15))
except SystemExit:
    raise
except Exception:
    sys.exit(3)
