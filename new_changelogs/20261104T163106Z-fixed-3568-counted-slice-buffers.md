- **A slice buffer is counted, so a view keeps what it views and nothing
  leaks (#3568).** A `make` or `append` buffer had one owner, and a view that
  might outlive it (kept in an outer scope, held while its slice was bound
  again, returned or stored) made the owner keep the buffer for good, so it
  leaked. Every place that holds a slice of a buffer now holds a reference
  (a local, a field, an element, a tuple, an optional, a sum, an array, a
  closure, a cell, a module `var`, actor state, a message), and the last one
  to let go releases it, elements first. `append` to a buffer a view still
  holds goes on in storage of its own: it reallocated the storage under the
  view, a use after free. Widened, refusing nothing that compiled before:
  `free` of a view a place holds gives back that view's reference (it was a
  named panic), `xs = xs[1..]` keeps the buffer (it was a named panic), and
  a copy of a whole buffer can grow. A buffer costs 32 bytes of header; a
  view bound to a local costs a count up and down (1.8 ns against 0.13 in a
  loop the C compiler folded); `make` is 11% faster, an `append` loop 2%
  slower, and indexing and passing a slice are unchanged.
- **Migrating: C never frees a `make` buffer's `.ptr` (#3568).** The buffer
  is counted, its storage starting a 32-byte header before the first
  element, and a slice of it carries the mark `AETHER_SLICE_MADE` and its
  offset into the buffer in the padding after `flags`. A host that holds an
  `AetherSlice` an Aether function returned gives it back with
  `aether_slice_release(&s)`, and makes a view of part of it with
  `aether_slice_sub`, never by moving `.ptr` itself. A slice C builds, an
  extern's `T[]` and `p as T[]` are not counted. The runtime ABI revision is
  3, so C built for revision 2 is refused rather than misread.
