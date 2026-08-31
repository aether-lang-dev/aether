import socket, struct, sys, time

host, port = "127.0.0.1", int(sys.argv[1])
req = (b"GET /echo HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n\r\n")

# Send a request and go away before the answer arrives. SO_LINGER with a zero
# timeout makes close() send RST rather than FIN, so the server's write lands
# on a socket with no reader: the exact race a client pressing stop produces,
# and the one that raised SIGPIPE and killed the process.
for _ in range(40):
    try:
        s = socket.create_connection((host, port), timeout=5)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                     struct.pack("ii", 1, 0))
        s.sendall(req)
        s.close()
    except OSError:
        pass          # the point is the server's reaction, not ours
    time.sleep(0.005)

# The server has to still be there, and still answering.
time.sleep(0.3)
try:
    s = socket.create_connection((host, port), timeout=5)
    s.sendall(req)
    s.settimeout(5)
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    s.close()
except OSError as e:
    print("SERVER GONE: %s" % e)
    sys.exit(1)

if b"200" not in data.split(b"\r\n", 1)[0]:
    print("BAD STATUS: %r" % data.split(b"\r\n", 1)[0])
    sys.exit(1)
print("OK")
