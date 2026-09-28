- **contrib.templating.liquid reads variable paths (#2178).** `user.name`,
  `items[0]`, `items[-1]`, `user["first name"]` and `items[i]` walk objects
  and arrays, and `size`, `first` and `last` work on arrays, strings and
  objects. A path works everywhere a value can appear: output, conditions,
  `assign`, `for` sources, `case`/`when`, filter arguments, `{% render %}`
  arguments, and `limit` / `offset`. A missing segment renders as nothing,
  and a name bound whole with its dots is still found under that name.
