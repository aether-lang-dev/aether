- **An HTTP/2 connection's streams and their bodies are bounded.**
  `SETTINGS_MAX_CONCURRENT_STREAMS` went out only when the program set it,
  and nghttp2's own local default is unlimited, so one connection could open
  streams without bound; it is now 100 unless `http.server_set_h2` gives
  another number. A stream's DATA was appended with no cap while nghttp2's
  automatic WINDOW_UPDATE kept it flowing; a stream whose body passes the
  server's body limit (`http.server_set_max_body`, 64 MiB by default) is now
  reset with `RST_STREAM` (`CANCEL`) and never dispatched. Idle and absolute
  timeouts for HTTP/2 connections and a minimum nghttp2 version remain open
  (#3738).
