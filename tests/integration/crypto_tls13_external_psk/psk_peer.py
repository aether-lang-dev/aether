"""An independent TLS 1.3 external-PSK server for the pure client, on
Python's ssl module (OpenSSL underneath), which takes an external PSK
from Python 3.13 on (SSLContext.set_psk_server_callback, ssl.HAS_PSK).

    psk_peer.py PORT_FILE CASE

CASE: "psk" takes the identity "plug-01" with 32 bytes of 0x11; "psk-p256"
the same with P-256 alone, so the client's x25519-only first ClientHello
gets a HelloRetryRequest; "cert" has a certificate and no PSK callback
(argv[3] the PEM cert, argv[4] its key), a server that would fall back to
certificate authentication; "tls12-psk" takes the PSK over TLS 1.2 only.
Serves one connection: prints "GOT <first line>" when application data
arrives (and answers "HTTP/1.1 200 OK ... external-psk-ok"), or
"SERVER-ERR <reason>" when the handshake fails on its side."""
import socket
import ssl
import sys

port_file, case = sys.argv[1], sys.argv[2]
KEY = bytes([0x11]) * 32

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
if case == "tls12-psk":
    ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    ctx.set_ciphers("PSK")
else:
    ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_3
if case == "cert":
    ctx.load_cert_chain(sys.argv[3], sys.argv[4])
else:
    ctx.set_psk_server_callback(lambda identity: KEY if identity == "plug-01" else b"")
if case == "psk-p256":
    ctx.set_ecdh_curve("prime256v1")

srv = socket.socket()
srv.bind(("127.0.0.1", 0))
srv.listen(1)
srv.settimeout(20)
with open(port_file, "w") as f:
    f.write(str(srv.getsockname()[1]))
conn, _ = srv.accept()
conn.settimeout(10)
try:
    with ctx.wrap_socket(conn, server_side=True) as tls:
        data = tls.recv(4096)
        line = data.split(b"\r\n")[0].decode("latin-1")
        print("GOT %s" % line, flush=True)
        body = b"external-psk-ok"
        tls.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s" % (len(body), body))
except (ssl.SSLError, OSError) as e:
    print("SERVER-ERR %s" % (getattr(e, "reason", None) or type(e).__name__), flush=True)
srv.close()
