- **Setting into a parsed document's inner containers no longer leaks.** A
  value set into an object or array inside a parsed document (an object in
  an array, say) gave that container an arena of its own, which freeing the
  document never destroyed: 20 KB a document. A parsed or copied container
  now allocates from its document's arena, and only the document's root
  destroys it (#2719).
