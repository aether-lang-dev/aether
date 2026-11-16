- **Tests: the proxy tests' "cannot be reached" upstream is never a port a
  gateway holds (#3709).** `http_proxy_loop_head_answers` took a stopped server's
  port as an upstream nothing listens on and then bound its gateways to
  ports the kernel picks, which can be that same port: the gateway then
  proxied to itself, each hop adding an `X-Forwarded-For` entry until a
  header line was too long, and the test read "431 and 24 bytes after the
  head" (seen once on Linux). A gateway given that port is now set aside and
  another made. `http_proxy_https_upstream` had the same shape; it binds
  every server before it takes its closed port.
