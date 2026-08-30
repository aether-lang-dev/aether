# TLS peer for the wss:// half of the non-blocking client test.
#
# The point of this peer is the BURST: on "burst", it sends two frames
# back-to-back with no await between them, so websockets/asyncio hands both
# to OpenSSL together and they very often land in ONE TLS record. The client
# then decrypts the whole record on the first read, leaving the second frame
# inside OpenSSL's buffer with NOTHING pending on the socket.
#
# That is the SSL_pending case: poll(2) on the file descriptor reports "not
# ready" while a complete frame is already decrypted and waiting. It is the
# reason ws_poll consults SSL_pending, and the reason ws_fd is documented as
# a hint rather than an authority.
import asyncio, ssl, sys, websockets

async def handler(ws, path=None):
    async for m in ws:
        if "burst" in m:
            # Both frames must land in ONE TLS record, or the second is still
            # in flight when the client polls and the SSL_pending path is
            # never reached. `await ws.send(...)` twice does NOT guarantee
            # that -- asyncio may flush between them, and measured here it
            # usually does.
            #
            # So write both frames into the transport in a single call. These
            # are unmasked server-to-client text frames with a payload under
            # 126 bytes, so the header is just [0x81, len] (RFC 6455 5.2).
            a = ("echo:" + m).encode()
            b = ("second:" + m).encode()
            assert len(a) < 126 and len(b) < 126
            raw = bytes([0x81, len(a)]) + a + bytes([0x81, len(b)]) + b
            # ws.transport is the TLS transport; one write -> one record.
            ws.transport.write(raw)
        else:
            await asyncio.sleep(0.9)   # a genuinely quiet window
            await ws.send("echo:" + m)

async def main():
    port = int(sys.argv[1])
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(sys.argv[2], sys.argv[3])
    async with websockets.serve(handler, "127.0.0.1", port, ssl=ctx):
        print("READY", flush=True)
        await asyncio.Future()

asyncio.run(main())
