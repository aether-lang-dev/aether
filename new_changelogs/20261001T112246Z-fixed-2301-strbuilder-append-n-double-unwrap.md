- **`strbuilder.append_n` no longer corrupts binary content that begins with
  the string-header magic.** The generated call already unwraps an
  AetherString to its payload, and the C side then checked that payload for
  a header again, so content starting `DE C0 57 AE` was taken for a header
  and the append copied from a pointer read out of the bytes that follow. It
  surfaced as `resp.encode` of such a bulk string reading uninitialised
  memory. The payload is now copied as raw bytes; `strbuilder.append`, whose
  parameter is `@aether string` and so does receive the header, unwraps it
  exactly once. `tests/regression/test_issue2301_format_slices.ae` covers it
  and fails against the old C.
