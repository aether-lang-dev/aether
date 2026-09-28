- **Dates: `std.time.strftime`, ISO-8601 with offsets, and Liquid's `date`
  (#2181).** `time.strftime(dt, fmt)` and `time.strftime_at(dt, offset,
  fmt)` implement Ruby's `Time#strftime` conversions and flags, ISO weeks
  included. `time.parse_iso8601_offset(s)` reads a date alone, `T` or a
  space, optional seconds and fraction, and a `Z`, `UTC` or `+HH:MM` zone,
  and returns the offset the time was written with. The calendar is now
  also `std.time.calendar`, which reads no clock and so needs no
  `--with=os` under `--emit=lib`; std.time keeps every name it had.
  contrib.templating.liquid's `date: format` reads ISO dates and Unix
  timestamps through it. `now` is the time the host gives with
  `context_set_now`, so a render stays deterministic.
