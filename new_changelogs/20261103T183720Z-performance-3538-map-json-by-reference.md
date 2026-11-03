- **A map keeps a string key by reference and a JSON container keeps a
  document set into it whole (#3538).** `map.put` copied every new key,
  although a key is a counted string the map can hold a reference to; it
  now retains it (a plain C string is still copied), and finds a bucket by
  masking where it divided. `json.set` and `json.push` deep copied the child
  into the parent, so a document built level by level cost O(depth x size),
  and `json.set` found a key by a linear scan, O(k^2) for an object of k
  keys. A document's root now moves in with its arena, a node of another
  document is still copied, and objects past 16 keys are indexed. A
  document set into its own child is refused instead of reading freed
  memory. Best of 5 on this machine: `map.put` 139 to 111 ns, `map.get` 75
  to 64 ns, an object of 5000 keys 11.7 to 0.71 ms, a 300 deep document 0.91
  to 0.09 ms.
