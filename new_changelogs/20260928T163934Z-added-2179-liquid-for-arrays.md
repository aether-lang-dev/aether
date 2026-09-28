- **contrib.templating.liquid's `for` iterates arrays, objects and strings
  (#2179).** An object iterates as `[key, value]` pairs, in key order.
  nil and an empty collection iterate zero times and render the loop's new
  `{% else %}` branch. `limit` / `offset` take variables, and
  `forloop.name` and `forloop.parentloop` are bound. `tablerow` takes the
  same sources. A loop over an unbound name used to be a render error, and
  now renders nothing, as in Liquid. The scan for a block's branches now
  steps over every block nested inside it, so an `{% else %}` of an inner
  `if`, `case` or `for` is no longer taken for the outer block's.
