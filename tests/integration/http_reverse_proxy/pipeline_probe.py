# T13: two HTTP/1.1 requests sent in a single segment (pipelining, RFC 9112
# section 9.3.2).
#
# The two requests ask for different paths, so the second response proves the
# second request was really served rather than the first answer being read
# twice. The event driver used to reset its input buffer to empty after each
# request, discarding any bytes of the next one that arrived in the same
# segment; the client then waited forever for a response that was never coming.

import socket
import sys

def pipelined(port, requests, until):
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    s.sendall(requests)
    s.settimeout(5)
    data = b""
    try:
        while until not in data:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        pass
    s.close()
    return data


port = int(sys.argv[1])

data = pipelined(port,
                 b"GET /echo HTTP/1.1\r\nHost: x\r\n\r\n"
                 b"GET /longheader HTTP/1.1\r\nHost: x\r\n\r\n",
                 b"long-ok")
if b"upstream-ok" not in data:
    print("first pipelined response missing")
    sys.exit(1)
if b"long-ok" not in data:
    print("second pipelined request was dropped; the client waits forever")
    sys.exit(1)
seen = data.count(b"HTTP/1.1 200")
if seen != 2:
    print("expected two 200 responses, saw %d" % seen)
    sys.exit(1)

# A body must be framed by its Content-Length and not by whatever else is in
# the buffer: the bytes of the next request sit directly behind it here, and
# an upstream that echoes the body back reveals any that leaked in.
data = pipelined(port,
                 b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello"
                 b"GET /longheader HTTP/1.1\r\nHost: x\r\n\r\n",
                 b"long-ok")
if b"\r\n\r\nhello" not in data:
    print("pipelined POST body was not echoed back as exactly 'hello'")
    sys.exit(1)
if b"long-ok" not in data:
    print("request after a pipelined POST body was dropped")
    sys.exit(1)

print("ok")
