- **`contrib.templating.liquid`: layouts can have layouts (#2183).** A
  template whose `{% layout %}` (or `{% extends %}`) parent itself declares
  a layout used to fail with `layout: nested layouts are not supported`. The
  whole chain now renders, to any depth: the base (the template with no
  layout) renders the page, and each `{% block %}` in it comes from the most
  derived template that defines it. `{{ block.super }}` is the same block
  one level up, which may use its own `block.super`, down to the base's
  default. A parent reached twice is reported as a cycle, and the chain
  counts against the include depth limit.
