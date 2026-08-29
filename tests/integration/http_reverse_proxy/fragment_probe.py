import socket, sys, time

host, port = "127.0.0.1", int(sys.argv[1])
body = b"fragmented-body-payload"
req = (
    b"POST /echo HTTP/1.1\r\n"
    b"Host: 127.0.0.1\r\n"
    b"Content-Type: application/octet-stream\r\n"
    b"Content-Length: " + str(len(body)).encode() + b"\r\n"
    b"\r\n"
) + body

# Cut so one boundary lands inside the CRLFCRLF terminator and the body
# arrives in a write of its own. A driver that resumes its header scan
# without remembering where the terminator was will never find it again
# once it is behind the resume point, and this request hangs.
term = req.index(b"\r\n\r\n")
cuts = [term + 3, len(req) - len(body)]
prev = 0
s = socket.create_connection((host, port), timeout=10)
for c in cuts + [len(req)]:
    s.sendall(req[prev:c])
    prev = c
    time.sleep(0.15)   # force separate reads on the server side

s.settimeout(10)
buf = b""
try:
    while b"\r\n\r\n" not in buf or len(buf.split(b"\r\n\r\n", 1)[1]) < len(body):
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
except socket.timeout:
    print("TIMEOUT")
    sys.exit(1)
finally:
    s.close()

status = buf.split(b"\r\n", 1)[0].decode(errors="replace")
got = buf.split(b"\r\n\r\n", 1)[1] if b"\r\n\r\n" in buf else b""
if b"200" not in status.encode():
    print("STATUS " + status); sys.exit(1)
if got != body:
    print("BODY %r != %r" % (got, body)); sys.exit(1)
print("OK")
