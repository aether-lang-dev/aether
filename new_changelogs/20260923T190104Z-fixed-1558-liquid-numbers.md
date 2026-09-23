- **`contrib.templating.liquid`: `abs`, `ceil`, `floor` and `round` exist,
  numbers are literals, and each missing feature has an issue (#1558).**
  The README documented `abs`, `ceil`, `floor` and `round`, down to their
  outputs, but none of the four was implemented. As unknown filters they
  passed their input through, so `{{ 3.7 | floor }}` printed `3.7`. They
  now follow the reference implementation:
  - A value that is exactly `-?digits.digits` is a decimal; anything else
    is read as Ruby's `to_i` reads it.
  - Halves round away from zero.
  - `round: N` rounds in decimal, so `1.005 | round: 2` is `1.01` as in
    Liquid, where binary floating point would give `1.0`. It prints as
    Liquid does (`3.1`, `10.0`, `-0.0`).

  Three input bugs went with them:
  - A decimal literal (`{{ 3.7 }}`) rendered as nothing: it was looked up
    as a variable named `3.7`.
  - `{{ -5 }}` rendered `5` and swallowed the space before it. The lexer
    stripped whitespace before looking for the `{{-` trim marker, so the
    minus sign was taken for one.
  - A filter argument had to be a quoted string (`plus: "3"`). Bare numbers
    are accepted now, as Liquid writes them (`round: 2`, `times: -2`).

  The module's limitations used to live in a `TODO.md` build log that
  described its first 200-line slice. Each one is now an issue: arrays and
  objects in the context (#2177), path access (#2178), `for` over an array
  (#2179), the array filters (#2180), `date` (#2181), variable filter
  arguments (#2182), multi-level layouts (#2183) and partial caching
  (#2184). The README's "What's not supported yet" links each one, and the
  module header points there.
