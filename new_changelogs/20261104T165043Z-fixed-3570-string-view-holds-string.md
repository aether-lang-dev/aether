- **A view of a string's bytes holds the string (#3570).** `string.bytes(s)`
  gave a read-only `byte[]` over the string's characters that held nothing,
  so once the string's last holder let it go (`s` bound again, released in
  a callee, a function returning `string.bytes` of its own local) the view
  read freed memory. A view of a counted string now holds one reference to
  it, by the rule a view of a `make` buffer holds its buffer (#3568): it is
  marked `AETHER_SLICE_STRING`, carries the string's header where a
  buffer's view carries its offset, and gives the reference back when the
  place that holds it goes. A literal's bytes are static and held by
  nothing. `string.bytes(s)` costs one count up and down. A view of a fresh
  call's result (`f()[1..]`, `string.bytes(s)[2..6]`) bound or passed to a
  call takes over the reference the result handed over, where it used to
  leak it.
