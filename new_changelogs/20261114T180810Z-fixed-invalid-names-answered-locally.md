- **A name in the `.invalid` domain fails at once, without asking DNS** (RFC 6761
  6.4), beside the existing `.localhost` rule in `aether_addr_resolve`. Some
  system resolvers took longer than a caller's whole budget to say such a name
  does not exist, so a proxy to `nohost.invalid` answered 504 instead of 502
  (macOS CI). `tests/regression/test_tcp_invalid_names.ae`.
