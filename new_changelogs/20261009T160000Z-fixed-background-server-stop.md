- **Stopping a background HTTP server waits for it, and a server can be
  started again in the same process.** On Windows `http_server_stop` called
  `WSACleanup` on every stop, while Winsock is started once per process: it
  ran under the server's own threads (an intermittent access violation in
  CI) and left the next server unable to create its socket. The background
  thread was detached, so stop returned while the accept loop and its
  workers were still running, and a worker serving an idle keep-alive client
  waited up to the idle timeout. Stop now leaves Winsock up, shuts down the
  connections its workers are serving, and joins the background thread, so
  the server is done when stop returns and can be freed at once (#2672).
