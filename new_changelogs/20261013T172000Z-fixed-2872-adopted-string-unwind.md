- **A string a container adopts is no longer freed again by a caught
  panic.** A function that keeps its `string` parameter copies it on entry
  and journals the copy for unwinding. Handed to a map or a list by a raw
  `map_put_raw`, `list_add_raw` or `list.set`, the container adopted it
  but the journal kept it, so once the container was freed, a panic caught
  later in the same `try` freed the string a second time (heap
  corruption). A builder setter putting its parameter into the config did
  the same. Every adoption the code generator writes now takes the string
  out of the journal, as `map.put` and `list.add` already did (#2872).
