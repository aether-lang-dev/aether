# An INDEPENDENT RFC 6455 server (python3-websockets), used as the peer our
# client dials. The point is third-party validation: the library rejects an
# unmasked client frame itself, per RFC 6455 s5.1, so if our mask were wrong
# or absent this fails rather than quietly interoperating.
import asyncio, sys
import websockets

async def echo(ws, path=None):
    try:
        async for msg in ws:
            await ws.send("echo:" + msg)
    except websockets.ConnectionClosed:
        pass

async def main():
    port = int(sys.argv[1])
    async with websockets.serve(echo, "127.0.0.1", port):
        print("READY", flush=True)
        await asyncio.Future()

asyncio.run(main())
