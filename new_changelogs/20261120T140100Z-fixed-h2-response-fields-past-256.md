- **An HTTP/2 response keeps every field the handler set.** The HTTP/2
  glue clamped a response's fields at 256 and dropped the rest without a
  word; with the response's field arrays growing as fields are set, a
  handler could set more and the client never saw them. The nghttp2 field
  array is now sized from the count.
