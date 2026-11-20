- **A file a sandboxed call holds open on Windows can be opened by others
  again (#3751).** An open that may create a file asked for DELETE, so a
  refused new file could be removed through its handle, and kept it for the
  life of the handle; every later open of that file that does not share
  delete (`fopen`, `_open`, another process, a second sandboxed process)
  failed with a sharing violation while it was open, which broke
  `fs.open` with "a" or "w", `fs.lock_open` across processes,
  `fs.create_exclusive`, `fs.write_binary` and tailing a log. A file that
  exists is now opened as it is, with no DELETE; a new one is verified and
  then reopened without DELETE (`ReOpenFile`), shared for reading and
  writing as `fopen` shares it. Where DELETE cannot be had, the new file is
  made by `NtCreateFile` in the verified directory, so it never lands
  outside the grant.
