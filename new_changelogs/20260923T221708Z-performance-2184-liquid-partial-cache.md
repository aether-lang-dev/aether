- **`contrib.templating.liquid`: a partial cache (#2184).** Every
  `{% include %}`, `{% render %}` and layout parent was read from disk and
  parsed at every use. `liquid.partial_cache_new()`, attached with
  `context_set_partial_cache`, parses each once, keyed by resolved path. The
  include root is still checked at every use, and `{% render %}`'s fresh
  context shares the cache. A page that includes one partial 200 times,
  rendered 20 times, went from 4,000 reads and parses and 162 ms to one and
  39 ms. `liquid.partial_loads()` counts reads and parses, so the saving can
  be measured.

  Parsed templates can now be released too. `liquid.template_free(t)` frees
  what `parse_string` / `parse_file` return, and an uncached include or
  layout parent is freed after use. Each used to stay allocated for the
  life of the program.
