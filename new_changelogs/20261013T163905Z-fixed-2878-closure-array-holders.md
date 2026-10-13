- **An `fn[N]` local releases its closures when another holder takes
  one.** An element handed back by the function it is passed to
  (`g = keep(fs[0])`), the whole array captured by a closure, and a closure
  of the array's own literal that reads the array each kept the array from
  releasing its closures, so every environment it made leaked. Each holder
  now takes a reference of its own: an element, or a call handing one back,
  is judged where it stands as a closure local is, so a local, a global or
  a field that retains it no longer stops the release, and a closure that
  copies an `fn[N]` takes a reference to each closure in it, given back
  when its environment goes. The array releases its own at its scope's end
  and when it is bound again, and the holder can still run the closures
  after that (#2878).
