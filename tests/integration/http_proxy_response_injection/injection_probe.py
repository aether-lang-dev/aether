# The proxy must not let an upstream inject headers into the client's response.
#
# A bare CR or LF inside a header value does not end a line on the wire, so it
# survives parsing as part of that value and reaches the code that writes the
# client's head. Writing it out verbatim would end the head early and let the
# rest be read as headers the upstream never sent (CWE-113, response
# splitting). This stands a hostile upstream in front of the proxy and reads
# the raw bytes the client actually gets.
#
# Two roles, because the proxy has to be told the port its upstream landed on
# and the kernel only names that port once the upstream has bound it:
#
#   upstream <portfile>  bind port 0, write the port, answer as the upstream
#   client <proxy_port>  drive the requests and check the bytes that come back
#
# One hostile header per response, on purpose, so each defence is the only
# one a case meets.
#
# A bare CR in a header section is read as SP (#3572): a recipient refuses
# the message or replaces each bare CR with SP before it processes or
# forwards it (RFC 9110 5.5, RFC 9112 2.2), and the client's response
# reader, which the proxy reads upstreams with, replaces it. The field's
# value then holds the rest of the line, with no CR in it, and the proxy
# forwards the 200 with that value: nothing behind the CR becomes a field of
# its own. A bare LF ends a line, so that case is the dropped header.

import os
import socket
import sys
import threading

# (label, hostile header, its name, the header it tries to inject, the field
# the client gets in its place, or None when the header is dropped)
CASES = [
    ("bare CR",
     b"X-Evil-CR: before\rX-Injected-A: yes\r\n",
     b"X-Evil-CR", b"X-Injected-A", b"X-Evil-CR: before X-Injected-A: yes"),
    ("bare LF",
     b"X-Evil-LF: before\nX-Injected-B: yes\r\n",
     b"X-Evil-LF", b"X-Injected-B", None),
]


def response_for(index, evil):
    # X-Case names which hostile response this is. The client checks it, so a
    # case that never reached the upstream cannot pass by having its evil
    # header absent for the wrong reason.
    return (b"HTTP/1.1 200 OK\r\n"
            b"Content-Type: text/plain\r\n"
            b"X-Case: %d\r\n" % index +
            evil +
            b"Content-Length: 2\r\n"
            b"\r\nok")


def case_index(req):
    # The client asks for /case<N>, so which hostile response comes back is
    # decided by the request rather than by how many have arrived: a retry or
    # a pooled connection cannot shift the cases out from under the checks.
    line = req.split(b"\r\n", 1)[0]
    for i in range(len(CASES)):
        if b"/case%d" % i in line:
            return i
    return 0


def serve_upstream(portfile):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0))
    s.listen(8)
    port = s.getsockname()[1]
    # Written whole and then renamed: the reader polls for this file, and a
    # partial line would be read as a truncated port number.
    tmp = portfile + ".tmp"
    with open(tmp, "w") as f:
        f.write("%d\n" % port)
    os.rename(tmp, portfile)

    # Kept open between requests. An upstream that closes after every response
    # exercises the proxy's pooled-connection handling, which is a different
    # matter from header injection and would decide this test's outcome for
    # reasons that have nothing to do with what it is checking.
    try:
        while True:
            c, _ = s.accept()
            threading.Thread(target=answer, args=(c,), daemon=True).start()
    except OSError:
        pass
    finally:
        s.close()


def answer(c):
    try:
        c.settimeout(30)
        while True:
            req = c.recv(65536)
            if not req:
                break
            i = case_index(req)
            c.sendall(response_for(i, CASES[i][1]))
    except OSError:
        pass
    finally:
        c.close()


def fetch(proxy_port, path):
    c = socket.create_connection(("127.0.0.1", proxy_port), timeout=10)
    c.sendall(b"GET " + path + b" HTTP/1.1\r\nHost: x\r\n\r\n")
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


def run_client(proxy_port):
    for i, (label, _evil, evil_name, injected, repaired) in enumerate(CASES):
        buf = fetch(proxy_port, b"/case%d" % i)

        if b"\r\n\r\n" not in buf:
            print("%s: no complete response from the proxy: %r" % (label, buf[:200]))
            return 1
        head = buf.split(b"\r\n\r\n")[0]
        lines = head.split(b"\r\n")

        # No field of the upstream's choosing: no line of the head starts
        # with the injected name, and no CR or LF stands alone in it.
        if any(line.startswith(injected + b":") for line in lines):
            print("%s: upstream injected %s into the client's head: %r"
                  % (label, injected.decode(), head))
            return 1
        if any(b"\r" in line or b"\n" in line for line in lines):
            print("%s: a bare line ending reached the client's head: %r" % (label, head))
            return 1

        if not head.startswith(b"HTTP/1.1 200 "):
            print("%s: the proxy did not forward the upstream's 200: %r" % (label, head))
            return 1

        if repaired is None:
            # The header with a bare LF is dropped: a cleaned-up version would
            # still be a header whose shape the upstream chose.
            if evil_name in head:
                print("%s: header carrying a bare line ending was forwarded: %r"
                      % (label, head))
                return 1
        elif repaired not in lines:
            print("%s: the field was not forwarded with its bare CR as SP: %r"
                  % (label, head))
            return 1

        if b"X-Case: %d" % i not in head:
            print("%s: the proxy answered from a different case: %r" % (label, head))
            return 1

        # And the response still has to work.
        if b"Content-Type: text/plain" not in head:
            print("%s: a legitimate header was lost: %r" % (label, head))
            return 1
        if not buf.endswith(b"ok"):
            print("%s: body did not arrive: %r" % (label, buf[-40:]))
            return 1

    print("ok")
    return 0


if len(sys.argv) < 3:
    print("usage: injection_probe.py <upstream PORTFILE | client PROXY_PORT>")
    sys.exit(2)

if sys.argv[1] == "upstream":
    serve_upstream(sys.argv[2])
    sys.exit(0)
if sys.argv[1] == "client":
    sys.exit(run_client(int(sys.argv[2])))

print("unknown role: %s" % sys.argv[1])
sys.exit(2)
