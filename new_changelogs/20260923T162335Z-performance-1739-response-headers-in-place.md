- **A reused server response rebuilds its headers in place (#1739).** The
  allocation census found response headers costing two `strdup`s apiece on
  every request, most of them rewriting text already sitting in the
  response from the last one: a reset freed the two default headers and the
  status text, then allocated identical copies of all three. Now the
  defaults and `"OK"` are written into the strings already there, and
  `set_header` overwrites a value in place when the new one fits — a
  handler's `Content-Type: text/plain` over the default costs nothing.

  Putting a header's name and value in one allocation would have cut more,
  and was deliberately not done: each header string being its own
  allocation is a convention C outside the library relies on —
  `tests/integration/http_external_ptr` frees them one by one — and
  breaking it would be the same kind of ABI break as #2169. That test used
  to skip on Windows with no reason recorded; it passes there, so it now
  runs everywhere and guards the convention. A unit test asserts the reuse
  by pointer identity.

  `std.http.server.lb`'s header comment listed the upstream picker as its
  top development direction. #1739's census showed the picker is one
  atomic add with its only mutex skipped unless rate limiting is on; the
  comment now says where the per-request cost actually was, and that it is
  addressed.
