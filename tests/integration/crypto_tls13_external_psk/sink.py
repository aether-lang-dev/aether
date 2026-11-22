"""A TCP sink for ownclient.ae: accepts connections one after another and
reports, for each, how long after the accept the client's side closed.
Connection 6 is answered with bytes that are not TLS once the client
writes. Writes its port to argv[1]; prints "CONN <n> EOF <ms>" or
"CONN <n> OPEN" (nothing within the window, argv[2] ms)."""
import socket
import sys
import time

port_file, window_ms, count = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
srv = socket.socket()
srv.bind(("127.0.0.1", 0))
srv.listen(8)
with open(port_file, "w") as f:
    f.write(str(srv.getsockname()[1]))
srv.settimeout(30)
for n in range(1, count + 1):
    conn, _ = srv.accept()
    t0 = time.monotonic()
    conn.settimeout(0.05)
    answered = False
    result = "OPEN"
    while (time.monotonic() - t0) * 1000 < window_ms:
        try:
            data = conn.recv(65536)
        except socket.timeout:
            continue
        except OSError:
            result = "EOF %d" % int((time.monotonic() - t0) * 1000)
            break
        if not data:
            result = "EOF %d" % int((time.monotonic() - t0) * 1000)
            break
        if n == 6 and not answered:
            conn.sendall(b"SSH-2.0-not-tls\r\n" * 4)
            answered = True
    print("CONN %d %s" % (n, result), flush=True)
    conn.close()
srv.close()
