- **An owned `map.put` hashes and walks its key once (#3831).** A put of a
  value the map takes over (any `map.put` codegen routes through the
  adopting path, a built string's and a literal's alike) hashed the key and
  walked its bucket for the replace check, again inside the plain put, and a
  third time to find the new entry and flag it; a closure value a fourth.
  One internal insert now does the walk once and stores the ownership flag
  as it goes. The table grows at the same moments, so a map's iteration
  order is unchanged. With 10-byte keys `map_put` takes 4% less; with
  66-byte keys 22% (`map_put`) and 18% (the new `map_put_owned_value`).
