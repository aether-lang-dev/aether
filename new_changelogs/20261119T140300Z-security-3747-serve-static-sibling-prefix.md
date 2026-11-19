- **`http.serve_static` no longer serves a sibling directory that shares
  the root's name as a prefix.** The check that a resolved path lies inside
  the document root was a bare prefix match, so a symlink in `/srv/www`
  that resolved into `/srv/www-private` passed it. The match must now end
  at a path separator, on POSIX and on Windows (#3747).
