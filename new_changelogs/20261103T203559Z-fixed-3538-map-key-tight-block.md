- **A map no longer keeps the whole spare capacity of a string key
  (#3538).** `map.put` kept any counted key by reference, so a key from
  `strbuilder.finish` kept the builder's whole block: 5000 keys built with
  `strbuilder.new(4096)` held 20 MB where copies take 0.5 MB. A key is now
  kept by reference only when its block is about as big as its text, as a
  string built to its length is, and copied otherwise. `map.put` keeps its
  gain: best of 5, 111 ns against 142 ns before #3538.
