import asyncio, ssl, sys, websockets
async def echo(ws, path=None):
    async for m in ws:
        await ws.send("echo:" + m)
async def main():
    port = int(sys.argv[1])
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(sys.argv[2], sys.argv[3])
    async with websockets.serve(echo, "127.0.0.1", port, ssl=ctx):
        print("READY", flush=True)
        await asyncio.Future()
asyncio.run(main())
