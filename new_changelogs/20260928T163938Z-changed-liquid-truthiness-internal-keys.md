- **contrib.templating.liquid follows Liquid's truthiness, and hides its
  own state from templates.** Only nil and false are false now. The empty
  string, and a bound string reading `"false"`, `"nil"` or `"null"`, are
  true, as they are in Liquid. The string-only context treated them as
  false because it had no booleans, and `context_put_bool` gives a real
  one. The include root and depth, the counters and the cycle positions
  were kept under `_`-prefixed names a template could read and assign
  (`{{ _inc_root }}` printed the include root). They now sit under keys
  that start with a control character, which no template can spell.
