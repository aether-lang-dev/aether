- **A duration interpolated or printed shows its own text, however many share
  a line and whichever thread prints it (#3526).** Durations were formatted
  into a ring of eight static buffers in the generated program, so a ninth
  duration in one interpolation showed another's text, and two actors
  printing durations at once could print each other's. The interpolation
  formatter now writes a duration's text itself, from the stack, and its
  fraction uses '.' whatever the locale.
