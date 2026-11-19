- **HTTP/2 connections are bounded as a whole, close when idle, and age out
  (#3738).** An HTTP/2 connection runs on a worker for its whole life, and
  with the dispatch pool on its loop went round its one-second wait for
  ever. Now:
  - with no stream open for the idle timeout the server sends GOAWAY and
    closes; PINGs and SETTINGS do not count as activity, and a connection
    with streams open but nothing moving for the idle timeout (no bytes
    either way, no handler running or finishing), closes too, so a handler
    that runs past the idle timeout still has its whole response go out as
    the client's flow-control window lets it;
  - a server stop refuses the streams whose requests have not arrived whole
    (REFUSED_STREAM, nothing of them handled) and gives the rest the idle
    timeout to finish: a stream its client kept trickling, PINGs and all,
    held `http_server_stop` for as long as the client liked;
  - a stream whose request does not arrive whole within the head and body
    budgets (#3737) of its first HEADERS is reset with CANCEL, so a body
    sent a byte at a time no longer holds the connection;
  - past `http.server_set_h2_max_age` (1 h by default) a connection gets a
    graceful GOAWAY and closes once its streams finish;
  - one connection holds no more than the body limit of request bodies at
    once (a stream that would pass it is refused with REFUSED_STREAM): its
    100 streams could each hold 64 MiB;
  - a request's header list is at most 32 KiB, advertised in
    SETTINGS_MAX_HEADER_LIST_SIZE and answered 431 past it: HPACK let a few
    bytes on the wire grow it without bound.

  A server stop's GOAWAY used `nghttp2_session_terminate_session`, which
  ends the session as soon as the GOAWAY is out, so it dropped the streams
  a graceful shutdown means to let finish; the GOAWAY is now graceful. A
  stream's body moves to its request instead of being copied, which held it
  twice. nghttp2's flood protections are set explicitly, and a build against
  a libnghttp2 older than 1.61.0 (the CONTINUATION flood fix, CVE-2024-28182;
  rapid reset, CVE-2023-44487, is fixed in 1.57.0) warns rather than fails,
  since distributions backport both fixes under the old version number.
