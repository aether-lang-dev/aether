- **`contrib.templating.liquid`: a filter argument can name a variable
  (#2182).** `{{ price | times: qty }}` and `{{ a | append: sep }}` failed at
  render with `filter arg must be a quoted string or a number`, because a
  filter got its arguments as text with no render context to look a name
  up in. An argument that names a variable is now resolved first, the way
  a condition's operands are, in output and in `assign` alike. A value
  containing quotes passes through intact, and an unbound name reads as
  empty. The module also stops warning `unused variable 'cn'` in every
  program that imports it.
