- **A return gives back the strings its statement made once its views hold
  them (#3570).** `return string.bytes(string.concat(w, "x")), 7`, and the
  same return of a single slice, kept the concatenated string for good,
  since a slice result might view it uncounted: the string leaked once a
  call. A view of a counted string holds the string, so the return now gives
  back each temporary string when every slice it returns holds what it
  views (a counted buffer or string) or views nothing, and keeps it only
  when one might view it uncounted.
