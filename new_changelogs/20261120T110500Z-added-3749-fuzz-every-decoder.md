- **Every decoder that reads untrusted input is fuzzed (#3749).** Eleven new
  libFuzzer targets under `tests/fuzz/`, each checking its decoder's
  contract rather than only that it does not crash: the WebSocket frame
  reader, the HTTP/2 server glue, the reverse proxy's handling of an
  upstream response, std.xml, std.yaml with the vendored libfyaml,
  std.msgpack, std.tar and std.zip extraction (into a scratch directory,
  checked to write nothing outside it), the TLS 1.3 record layer and
  handshake parsers, and X.509, ASN.1 and PEM. Seeds come from the existing
  tests. The fuzz workflow runs in three shards: a pull request fuzzes the
  targets its changes reach (`tests/fuzz/select_targets.sh`), main fuzzes
  them all, and a weekly run gives each 10 minutes. `make fuzz-replay` runs
  every seed through the property checks where libFuzzer is not available.
