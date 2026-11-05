- **A published slice publishes what its elements hold (#3568).** A slice
  other threads can reach (a module `var`, a message, a closure, actor
  state, or a place reached through a pointer or another slice) has an
  atomic count, but its rows, or the slices and strings its struct elements
  hold, kept plain ones: two actors copying a row of a shared slice of
  slices lost counts, and the row was freed under one of them. Publishing
  now reaches every element once, with the published bit set atomically,
  and a store or a push into a slice's element publishes what it stores, as
  a string store there does.
