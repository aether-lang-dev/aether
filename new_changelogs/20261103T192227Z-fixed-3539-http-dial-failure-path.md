- **A request that cannot connect no longer leaks a copy of its URL path.**
  `std.http`'s client copies the path out of the URL and frees it once the
  request head is built, but a host that did not resolve and a dial that
  failed returned before that, so every request to a server that was down
  leaked the path. LeakSanitizer found it in the client's own tests.
