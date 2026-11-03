- **`map.put` with a key built for the call (`map.put(m, "k${i}", v)`,
  `map.put(m, key_of(x), v)`) no longer leaks the key (#3539).** The put
  that hands a string value to the map gave only the value to it, and the
  key the call computed was never released; one string leaked per put.
  LeakSanitizer and the macOS leaks gate found it. A local or a literal key
  is untouched, as before.
