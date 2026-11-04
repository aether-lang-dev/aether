- **A fresh slice given to an optional parameter is given back once
  (#3568).** `count(mk(3))` with `count(o: int[]?)` wrapped the slice into
  the optional argument's temporary, which gave it back after the call, and
  the calling statement gave it back again as it does for a slice parameter:
  the buffer was freed twice. The wrapped slice is the optional temporary's
  alone now, as a string argument there is.
