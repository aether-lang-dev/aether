- **A response refused for its framing no longer leaks the request head.**
  `std.http`'s client keeps the head it sent until it knows whether a pooled
  connection must be dialled again, but the returns for a response whose
  framing is ambiguous and for running out of memory while reading one came
  before it was freed. Every response with two different Content-Length
  headers leaked the head; LeakSanitizer found it in the client's framing test.
