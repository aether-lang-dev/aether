- **The examples path teaches what batch 9 added (#3476).** Six new
  examples, each with its output checked by `make check-examples`:
  ending an actor (`actors.stop`, `actors.kill`, a panic, and the reason
  a monitor's `Down` carries), links and trapping exits, a bounded mailbox
  with `try_send`, a std.stream producer held to a window of credits,
  std.tcp (a server and a client, reading a stream up to where a message
  ends), the pure-Aether cryptography (SHA-256, an HMAC, an Ed25519
  signature), and a C host's limits on the code it runs (a deadline, a
  cancel from another thread, an execution account's memory budget).
  Links now come before the supervisor built on them: `12-supervisor.ae`
  is `13-supervisor.ae`, and `11-sharded-map.ae` is `16-sharded-map.ae`.
