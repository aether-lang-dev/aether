- **A tuple local owns the strings the call it is bound from hands over.**
  `t = pair(w); a, n = t` took pair's string as borrowed from the local and
  nothing freed it, and a returned tuple local was taken as borrowed by its
  caller. The local now owns them, as a struct local owns its string
  fields: it gives them back at its scope exit and when it is bound again,
  a destructure from it takes copies, and `return t` hands them on. A local
  used any other way (an alias, an argument, a capture) owns nothing, as
  before (#2869).
