- **The `[]`-on-a-bare-`ptr` diagnostic now names the view first.** It told
  the reader to call `intarr.intarr_get_unchecked(a, i)` — correct, but it
  sent someone who wrote `a[i]` to a function call when `v[i]` was
  available all along. It now says to take a typed view and index that,
  and offers the accessor for a single access. The `asks/` reply that
  deferred this ask carries an update saying so: its reasoning that `[]`
  cannot work on the *handle* was right, and its conclusion that a distinct
  handle type was therefore needed was wrong, because the handle is not the
  only thing you can index. The original text is left as written, because
  what it got wrong is worth seeing.
