# std.http

HTTP client and server.

The server handles keep-alive, connection parking, TLS termination, routing,
middleware and HTTP/2; the client does requests, connection reuse and TLS
verification. This is the largest module in `std`, and
`docs/http-server.md` is the reference for the server side: this page is the
orientation.

The example **compiles but is not run** in CI: a server binds a port and does
not return.

```aether
import std.http

fn handle(req: ptr, res: ptr, ud: ptr) {
    http.response_set_status(res, 200)
    http.response_set_header(res, "Content-Type", "text/plain")
    http.response_set_body(res, "hello\n")
}

main() {
    server = http.server_create(8080)
    if server == 0 {
        println("could not bind 8080")
        return
    }

    http.server_get(server, "/", handle, 0)
    http.server_start(server)
}
```

A handler takes `(request, response, user_data)` and fills the response; the
`user_data` pointer is whatever was passed at registration, which is how a
handler reaches application state without a global.

**Keep-alive is on by default**, and an idle connection is parked in a poller
rather than holding a worker thread, so concurrency bounds on file
descriptors, not on the `cores * 2` worker count. `AETHER_HTTP_PARKING=0`
turns that off if a regression needs bisecting.

For the client, `http.get` / `http.post` return a response object that must be
freed with `http_response_free`. Connection reuse is on by default.

The client sends a URL's path and query, never its fragment. Everything from
the first `#` stays with the client, as RFC 3986 and 9110 say and as curl and
browsers do. Encode a query value that may hold `#`, `&`, `+`, `%` or a space
with `url.encode` from `std.url`: `"?v=${url.encode(text)}"`. A raw `"?v=#FF0000"`
sends `?v=`. Before 0.881.0 the fragment went out on the request line, which
is not a valid request target.

## TLS

`server_set_tls(server, cert_path, key_path)` before `server_start` turns a
server into an HTTPS one. It returns an error string, `""` on success: check
it, because a bad path or an unreadable key is the common failure and the
server otherwise starts happily in plaintext.

```aether,fragment
err = http.server_set_tls(server, "/etc/ssl/cert.pem", "/etc/ssl/key.pem")
if err != "" {
    println("TLS setup failed: ${err}")
    return
}
```

A program that holds its certificate and key in memory, and should not write
the key to disk to hand it over, gives them as PEM text instead:
`server_set_tls_pem(server, cert_pem, key_pem)` makes the same checks with
the same results (#3362). Either way the first certificate is the server's
own and the rest are the chain it sends with it, so a CA's bundle (leaf, then
intermediates) serves as issued on both backends (#3370). Only the
`BEGIN CERTIFICATE` blocks form the chain: any other block in the text, such
as the key in a file that holds both, is skipped, and a certificate block
that does not parse refuses the whole text (#3378). The server keeps
its own copy of the text, for a later `server_set_tls_backend`, and wipes it
when it is replaced or the server is freed.

**Two backends.** OpenSSL is the default wherever it is compiled in. The other
is a pure-Aether TLS 1.3 implementation with no OpenSSL dependency at all,
used automatically when the build has no OpenSSL, which is what lets a
cross-compiled binary (`ae build --target=…`, which links none) serve HTTPS at
all. A program chooses explicitly with `http.server_set_tls_backend(server,
"aether")` (or `"openssl"`, or `""` for the default); `AETHER_PURE_TLS=1`
moves a whole process onto the pure stack, and an explicit choice wins over
it. `server_set_tls_backend` returns an error string when the backend is not
built in or not linked (#1298):

```aether,fragment
err = http.server_set_tls_backend(server, "aether")
if err != "" { println("TLS backend: ${err}"); return }
```

The pure backend is **opt-in at the application**: a program that uses it
imports it, and a plain `std.http` user does not carry it. The server reads
the certificate and key files for both backends, so the pure server reads
no files itself (and needs no `--with=fs` under `--emit=lib`); it parses
them once, when the server is set up, and every connection shares that
(#3362):

```aether,fragment
import std.http
import std.cryptography.tls13_server   // pure TLS server
```

The pure backend has constraints the OpenSSL one does not; they are worth
knowing before debugging a handshake:

- the server key is **ECDSA P-256, P-384 or P-521, Ed25519, or RSA** of 2048
  to 16384 bits (PKCS#8, SEC1 or PKCS#1 PEM, not encrypted), and
  CertificateVerify is signed with the key's own scheme (RSA as
  rsa_pss_rsae_sha256, or rsa_pss_rsae_sha384 or rsa_pss_rsae_sha512 for a
  client that offers only those, #3357): a client that offers none of the
  key's schemes gets `handshake_failure` (#3326). `server_set_tls` checks the
  certificate and key when the server is set up and says why it cannot serve
  them, as the OpenSSL backend does (#3328)
- every CertificateVerify signature is checked against the certificate's
  key before it is sent: a signature computed wrongly (a fault while
  signing, a bit flipped in memory) can disclose the key, so one that does
  not verify is never sent, and the handshake ends with `internal_error`
  (#3376). The check is a verification per handshake, about as costly as
  the signature for ECDSA and Ed25519, about a tenth of it for RSA
- the private key is kept encrypted in memory (`std.cryptography.shield`,
  as OpenSSH keeps its keys), in the clear only for each signature, so a
  memory-reading attack must read a 16 KiB random pre-key without an error
  to get it; a key whose encrypted form changed in memory is not used
  (#3376)
- key exchange is **X25519MLKEM768** (the post-quantum hybrid, preferred
  when the client offers it), **X25519 or secp256r1** (P-256, which RFC 8446
  9.1 requires, #3357); a client that names one of them in supported_groups
  but sends no key share for it gets a **HelloRetryRequest** asking for that
  share (#3325), and a client offering none of them (a P-384-only client,
  say) is refused with a `handshake_failure` alert
- a handshake message or a write over one TLS record (2^14 bytes), such as
  a certificate chain over 16 KiB or a large response, goes out in as many
  records as it takes (#3380), and the pure client reads one split across
  records (#3321) up to 256 KiB for the server's Certificate and 64 KiB for
  its other handshake messages (#3366); the pure server takes a ClientHello
  split across records, up to 64 KiB, and asks for no client certificate

The client side is the mirror, and is now wired the same way (#1849):
`std.http.client` does HTTPS through OpenSSL where it is compiled in, and
falls back to `std.cryptography.tls13_client` where it is not, which is every
`ae build --target=` cross-build, since zig bundles no TLS. Add

```aether,fragment
import std.cryptography.tls13_client
```

to the program and `https://` works in a cross-built binary that links no TLS
library at all. The pure client offers X25519MLKEM768 by default, alongside
X25519 and P-256 key shares (and names P-384 without one, for a server that
asks for it in a HelloRetryRequest, #3369), so it gets post-quantum key
exchange from any server that supports it (#2483). `set_insecure` and
`set_cafile` are honoured on the pure path too, and an https request through
a forward proxy works because the CONNECT tunnel is established before the
handshake either way.

Without that import the pure client is not linked, and an https request in a
no-OpenSSL build fails with an error naming the missing import rather than
returning an empty body. (`std.http.client` does not import it for you:
`std.cryptography` reaches `std.fs`, and an implicit import would make
`--emit=lib --with=net` reject every `std.http.client` user, the same reason
the server side is opt-in.)

**Choosing the client backend per request.** `client.set_tls_backend(req,
"aether")` sends this request through the pure client, `"openssl"` through
OpenSSL, `""` back to the default (OpenSSL where it is built in, unless
`AETHER_PURE_TLS=1`); the choice wins over the environment variable, and
pooled connections are kept apart per backend. It returns an error string
for a backend that cannot be used: `"openssl"` in a build without OpenSSL,
`"aether"` without `import std.cryptography.tls13_client`, an unknown name
(#1298).

```aether,fragment
req = client.request("GET", "https://example.com/")
err = client.set_tls_backend(req, "aether")
if err != "" { println("TLS backend: ${err}"); return }
```

In a build that HAS OpenSSL, `AETHER_PURE_TLS=1` (the variable that also
selects the pure server) moves client requests onto the pure client as well,
provided the program imports it (otherwise the request fails, naming the
import). That is how the pure path is tested on every platform, not only in
cross-builds.

**Trust store.** The pure client verifies against, in order: the `set_cafile`
pin alone, when there is one; else the union of `SSL_CERT_FILE` (a PEM bundle)
and `SSL_CERT_DIR` (a directory of PEM files, or a `:`-separated list (`;` on
Windows) when either is set; else, on Windows, every certificate of the system
ROOT store together with the first bundle file of the list the OpenSSL path
uses too (the bundle the release ships beside the program, MSYS2's, Git for
Windows'; see the README's Windows section); else the first system bundle that
exists (`/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/tls/certs/ca-bundle.crt`,
`/etc/ssl/cert.pem`); else the first
certificate directory that loads) Android's `/apex/com.android.conscrypt/cacerts`
(Android 14+) and `/system/etc/security/cacerts`, which are one certificate per
file with no bundle at all, then `/etc/ssl/certs`. If nothing loads, the
handshake fails closed.

**What it validates.** The server's certificate must name the host
(subjectAltName only, on the OpenSSL backend too since #3313: DNS names, with
a whole-label leading wildcard over at least two labels, and IP
addresses), and there must be a path from it
through the certificates the server sent to one of those anchors, which
must be a self-signed root, on this backend as on OpenSSL's (a trusted
intermediate ends no path; pinning one is #3311)
(`tls13_cert.validate_chain`, RFC 5280 section 6 as the Web PKI profiles it)
on which every signature verifies (RSA PKCS#1 v1.5 with SHA-256, SHA-384 or
SHA-512; ECDSA on P-256, P-384 or P-521; Ed25519), every key is one of those
(RSA of at least 2048 bits, named curves only; the OpenSSL backend refuses
the same RSA keys, at security level 2 or above, #3312), every certificate
is inside its validity period, every issuer below the anchor is a version 3 CA
(basicConstraints cA) allowed to sign certificates (keyUsage keyCertSign
when keyUsage is present) within its pathLenConstraint, the leaf's keyUsage
(when present) allows digitalSignature, which TLS 1.3 signs with, the leaf
and every issuer that lists extendedKeyUsage allow serverAuth (a leaf without
extendedKeyUsage is valid for any purpose, as RFC 5280 says and as OpenSSL
and Go's crypto/x509 read it: the backend a program runs on must not change
which chains it accepts), every issuer's name
constraints hold (DNS names, IP addresses, mailboxes, URI hosts, directory
names; a wildcard is refused under an excluded name it could match), and no
certificate has a critical extension the client does not process (nor a
critical key identifier, which OpenSSL refuses too). Every candidate path is
tried, within a bound, so an extra or cross-signed certificate in the
server's list does not hide a good one. Before #3303 only the names and the signatures were
checked, and a certificate a CA issued to any site could sign one for any
other. The rules are measured against the x509-limbo and bettertls corpora
in CI; `tests/conformance/x509_limbo/expected.txt` lists the cases that
differ and why, each one the OpenSSL backend accepts too. A stapled OCSP response saying revoked fails the handshake;
there is no CRL or online revocation check.

**Scope and errors.** The pure client speaks TLS 1.3 only. A server that does
not is refused with an error that says so (`server does not support TLS 1.3`),
not a hang. Handshake failures carry the real cause (an untrusted chain, a
certificate not valid for the requested host, no trust store) prefixed with
`TLS handshake failed (pure-Aether TLS 1.3):`.

**Performance.** A whole pure-client GET of a real site (github.com,
www.cloudflare.com, www.bing.com: DNS, trust store, handshake with full path
validation, response) takes 0.4 to 0.5 seconds on a desktop, network
included. A P-256 signature check takes about 0.5 ms (`std.cryptography.ecc`,
#2600); earlier versions of this note measured 12 to 22 seconds, then 1.3,
when every field operation went through `std.bignum`. No timeout adjustment is
needed.

See also `std.http.server.lb` for load balancing, `std.tcp` for raw sockets,
and `docs/http-server.md` for routing, middleware, TLS and the HTTP/2 path.

## WebSockets

The server side has shipped since v0.96: `server_websocket(srv, path, handler,
user_data)` registers a route whose handler gets a live `ws` handle and uses
`ws_recv` / `ws_message` / `ws_send_text` / `ws_close`.

`ws_connect` is the other end: it dials a `ws://` URL, completes the
client-side upgrade, and returns a handle that takes **the same verbs**,
because the frame codec underneath is shared.

```aether,fragment
w = http.ws_connect("ws://127.0.0.1:9515/session/abc")
if w == null { return }

_ = http.ws_send_text(w, "{\"id\":1,\"method\":\"session.status\"}")
if http.ws_recv(w) == 1 {
    println(http.ws_message(w))
}

http.ws_close(w, 1000, "done")
http.ws_client_free(w)
```

`ws_recv` returns `1` for a text message, `2` for binary, `-1` once the peer
has closed; ping/pong is handled inside it on both ends. Free a **client**
handle with `ws_client_free`: server-side handles belong to the request loop
and must not be passed to it.

`ws_connect` returns `null` rather than raising: on a malformed URL, an
unreachable host, a response that is not `101`, or a `Sec-WebSocket-Accept`
that does not match the key it sent. That last check is what stops any `101`
being accepted, including one from a server that never saw the handshake.

**`wss://` dials over TLS**, reusing the HTTP client's trust store: the same
system CA discovery and TLS floor as `https`. The peer certificate is verified
and pinned to the host in the URL, so a certificate with a valid chain but the
wrong host is refused. The default port follows the scheme: 80 for `ws`, 443
for `wss`.

One asymmetry is invisible but required: RFC 6455 §5.3 says a client must mask
every frame it sends and a server must not. The client handle does; the server
handle does not.

### Reading without blocking

`ws_recv` blocks until a frame arrives, which is fine for a request/response
exchange and wrong for a **multiplexed** one. WebDriver-BiDi is the motivating
case: many commands in flight at once, each awaited by `id`, plus unsolicited
events on the same socket. That needs one reader routing frames to an id-keyed
table or an event queue, and built on a blocking `ws_recv`, that reader has to
own a thread.

Three calls avoid it:

```aether,fragment
// Bounded: 1 text, 2 binary, 0 nothing arrived in time, -1 closed.
k = http.ws_recv_timeout(w, 50)
if k == 0 { return }          // nothing yet, come back later
if k < 0 { return }           // peer gone

// Readiness, consuming nothing: 1 readable, 0 timed out, -1 closed.
if http.ws_poll(w, 50) == 1 {
    _ = http.ws_recv(w)       // guaranteed not to block
}
```

`ws_recv_timeout(w, 0)` is a true poll and never blocks, which makes it the
building block for a pump: call it, route whatever comes back, return.

The timeout bounds the wait for the **start** of a frame. Once a header has
been read the frame is finished even if that blocks, because WebSocket framing
has no resynchronisation point: abandoning a half-read frame would leave the
next read treating payload as a header. Pings and continuation frames handled
during the wait do not restart the clock, so a chatty peer cannot extend the
call.

**`ws_fd(w)` exposes the underlying socket** for a native event loop
(`asyncio.add_reader`, epoll, kqueue), and it comes with a contract:

> The descriptor is a readiness **hint**, not an authority. A frame can be
> sitting in the connection's buffer, or decrypted inside the TLS layer, with
> nothing pending on the socket. Always drain with `ws_recv_timeout(w, 0)`
> until it returns `0`, then go back to waiting.

That is not a theoretical caveat. Over `wss://`, OpenSSL decrypts an entire
TLS record at once, so a peer that writes two frames together leaves the
second one decrypted and waiting while `poll(2)` on the descriptor reports
nothing: measured, not assumed. `ws_poll` checks the connection buffer, then
the TLS layer, then the socket, so it cannot answer "not ready" while a frame
is already in hand. Prefer it whenever you want one authoritative answer;
reach for `ws_fd` only when an event loop needs a real descriptor.

## Exports

Server: `server_create`, `server_get`, `server_post`, `server_put`,
`server_delete`, `server_start`, `server_set_keepalive`, `server_set_host`,
`response_set_status`, `response_set_header`, `response_set_body`, and the
request accessors. Client: `get`, `post`, `http_response_status`,
`http_response_body`, `http_response_headers`, `http_response_free`.
