- **A server frees its access log's and metrics' state.** `http_server_free`
  left behind the `MetricsState` (with its mutex and every route it recorded)
  of a server given metrics, and the format and open file of one given an
  access log (#2910).
