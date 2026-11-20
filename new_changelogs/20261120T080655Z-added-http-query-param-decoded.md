- **`http.query_param(req, name)` returns a query parameter decoded.**
  `http.get_query_param` returns the value as it came on the wire, still
  percent-encoded (`?q=a%20b` gives `"a%20b"`), matched against the raw key,
  and said so nowhere; `get_path_param` is raw the same way. Both keep that
  contract, now documented. The new accessor decodes as a form field reads
  (`+` a space, the key decoded too) and returns `(value, err)`, with an
  error for a malformed escape.
