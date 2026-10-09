- **Stopping an HTTP server waits for it, ends idle connections at once,
  and a server can be started again in the same process.** On Windows
  `http_server_stop` called `WSACleanup` on every stop, while Winsock is
  started once per process: it ran under the server's own threads and left
  the next server unable to create its socket. The background thread was
  detached, so stop returned while the accept loop and its workers were
  still running. The accept loop freed the keep-alive parking lot before
  joining the workers, and a worker finishing a request could still hand a
  connection to the freed lot, or add one after the lot had been emptied,
  where nothing closed it
  (#2680). And a worker waiting for a keep-alive client's next request, or
  refused by the lot during the stop, waited out the idle timeout. Stop now
  leaves Winsock up, wakes the workers that are waiting for a request
  (one in the middle of a request finishes it, as a graceful shutdown
  expects), starts no further request, closes the lot before the workers
  are joined and frees it after, and joins the background thread, so the
  server is done when stop returns and can be freed at once (#2672).
