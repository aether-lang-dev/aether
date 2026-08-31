# The proxy must not let an upstream inject headers into the client's response.
#
# A bare CR or LF inside a header value does not end a line on the wire, so it
# survives parsing as part of that value and reaches the code that writes the
# client's head. Writing it out verbatim would end the head early and let the
# rest be read as headers the upstream never sent (CWE-113, response
# splitting). This stands a hostile upstream in front of the proxy and reads
# the raw bytes the client actually gets.

import socket
import sys
import threading
import time

UPSTREAM_PORT = 19001
PROXY_PORT = 19000

# One hostile header per response, on purpose. A bare CR ends the header scan
# on both paths, so a response carrying both would never reach the check that
# rejects a value holding a bare LF, and removing that check would not fail
# this test.
CASES = [
    ("bare CR",
     b"X-Evil-CR: before\rX-Injected-A: yes\r\n",
     b"X-Evil-CR", b"X-Injected-A"),
    ("bare LF",
     b"X-Evil-LF: before\nX-Injected-B: yes\r\n",
     b"X-Evil-LF", b"X-Injected-B"),
]


def response_for(evil):
    return (b"HTTP/1.1 200 OK\r\n"
            b"Content-Type: text/plain\r\n"
            + evil +
            b"Content-Length: 2\r\n"
            b"\r\nok")

ready = threading.Event()
serve = {"body": b""}


def upstream():
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", UPSTREAM_PORT))
    s.listen(8)
    s.settimeout(30)
    ready.set()
    # Kept open between requests. An upstream that closes after every response
    # exercises the proxy's pooled-connection handling, which is a different
    # matter from header injection and would decide this test's outcome for
    # reasons that have nothing to do with what it is checking.
    try:
        while True:
            c, _ = s.accept()
            try:
                c.settimeout(10)
                while True:
                    req = c.recv(65536)
                    if not req:
                        break
                    c.sendall(serve["body"])
            except OSError:
                pass
            finally:
                c.close()
    except OSError:
        pass
    finally:
        s.close()


t = threading.Thread(target=upstream, daemon=True)
t.start()
if not ready.wait(10):
    print("hostile upstream never bound")
    sys.exit(1)


def fetch():
    c = socket.create_connection(("127.0.0.1", PROXY_PORT), timeout=10)
    c.sendall(b"GET /echo HTTP/1.1\r\nHost: x\r\n\r\n")
    c.settimeout(8)
    buf = b""
    try:
        while b"\r\n\r\n" not in buf:
            d = c.recv(65536)
            if not d:
                break
            buf += d
    except socket.timeout:
        pass
    c.close()
    return buf


for label, evil, evil_name, injected in CASES:
    serve["body"] = response_for(evil)
    buf = fetch()

    if b"\r\n\r\n" not in buf:
        print("%s: no complete response from the proxy: %r" % (label, buf[:200]))
        sys.exit(1)
    head = buf.split(b"\r\n\r\n")[0]

    if injected in head:
        print("%s: upstream injected %s into the client's head: %r"
              % (label, injected.decode(), head))
        sys.exit(1)

    # Dropping the header is the defence. Emitting a cleaned-up version would
    # still be a header whose shape the upstream chose.
    if evil_name in head:
        print("%s: header carrying a bare line ending was forwarded: %r"
              % (label, head))
        sys.exit(1)

    # And the response still has to work.
    if b"Content-Type: text/plain" not in head:
        print("%s: a legitimate header was lost: %r" % (label, head))
        sys.exit(1)
    if not buf.endswith(b"ok"):
        print("%s: body did not arrive: %r" % (label, buf[-40:]))
        sys.exit(1)

print("ok")
