import socket, ssl, sys, threading

port = int(sys.argv[1])
n    = int(sys.argv[2])

ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE

req = (b"GET /echo HTTP/1.1\r\nHost: 127.0.0.1\r\n"
       b"Connection: close\r\n\r\n")
results = [None] * n

def one(i):
    try:
        raw = socket.create_connection(("127.0.0.1", port), timeout=20)
        with ctx.wrap_socket(raw, server_hostname="127.0.0.1") as s:
            s.sendall(req)
            data = b""
            while b"\r\n\r\n" not in data:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
        results[i] = data.split(b"\r\n", 1)[0].decode("latin-1")
    except Exception as e:
        results[i] = "ERROR: %s" % e

# All at once, on purpose: a proxy that gives each TLS connection a thread,
# or a handshake that only completes when nothing else is in flight, shows up
# here and nowhere else.
threads = [threading.Thread(target=one, args=(i,)) for i in range(n)]
for t in threads: t.start()
for t in threads: t.join(30)

bad = [(i, r) for i, r in enumerate(results) if not (r or "").startswith("HTTP/1.1 200")]
if bad:
    for i, r in bad[:5]:
        print("request %d: %s" % (i, r))
    print("%d of %d did not return 200" % (len(bad), n))
    sys.exit(1)
print("OK %d/%d" % (n, n))
