# An upstream that closes after every response, which is what an idle
# keep-alive connection does when the upstream's timeout fires. nginx does it
# after keepalive_timeout, so a proxy meets this constantly.
#
# The close is only discoverable by using the connection, and it announces
# itself three different ways: a clean end of file on the read, a reset on the
# read, or a failed write. All three mean nothing arrived, so the request has
# to go again down a fresh connection. Getting one of the three wrong shows up
# here as an occasional failure among many successes, which is why this makes
# a lot of requests rather than one.

import socket
import sys
import threading
import time

UPSTREAM_PORT = 19001
PROXY_PORT = 19000
REQUESTS = 60

RESPONSE = (b"HTTP/1.1 200 OK\r\n"
            b"Content-Type: text/plain\r\n"
            b"Content-Length: 2\r\n"
            b"\r\nok")

ready = threading.Event()


def upstream():
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", UPSTREAM_PORT))
    s.listen(128)
    s.settimeout(90)
    ready.set()
    try:
        while True:
            c, _ = s.accept()
            try:
                c.recv(65536)
                c.sendall(RESPONSE)
            finally:
                c.close()
    except OSError:
        pass
    finally:
        s.close()


threading.Thread(target=upstream, daemon=True).start()
if not ready.wait(10):
    print("upstream never bound")
    sys.exit(1)

failures = []
for i in range(REQUESTS):
    try:
        c = socket.create_connection(("127.0.0.1", PROXY_PORT), timeout=8)
        c.sendall(b"GET /echo HTTP/1.1\r\nHost: x\r\n\r\n")
        c.settimeout(6)
        buf = b""
        while b"\r\n\r\n" not in buf:
            d = c.recv(65536)
            if not d:
                break
            buf += d
        c.close()
        if b"200 OK" not in buf:
            failures.append("request %d: %r" % (i + 1, buf[:60] if buf else b"<nothing>"))
    except Exception as e:
        failures.append("request %d: %s: %s" % (i + 1, type(e).__name__, e))
    time.sleep(0.03)

if failures:
    print("%d of %d requests did not get a 200: %s"
          % (len(failures), REQUESTS, "; ".join(failures[:5])))
    sys.exit(1)

print("ok")
