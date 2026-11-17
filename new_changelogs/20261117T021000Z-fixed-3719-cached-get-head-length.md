- **A HEAD the proxy answers from a cached GET states that GET's length
  (#3719).** With the `url` cache key a HEAD can be served a stored GET.
  Since #3713 it kept the stored fields as they were, and a GET the upstream
  sent chunked left none that states a length, so the answer stated none.
  It now states the stored body's length when the fields state none.
