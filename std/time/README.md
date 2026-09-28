# std.time

Civil dates and times, and the arithmetic between them.

A `DateTime` is a civil timestamp — year, month, day, hour, minute, second —
plus the derived weekday and day-of-year. `to_unix` converts to seconds since
the epoch; `from_unix` goes back.

The `add_*` helpers return a new `DateTime` and normalise as they go, so
adding 10 days to the 25th rolls into the next month without the caller
doing calendar arithmetic.

```aether,run
import std.time

main() {
    dt = time.from_civil(2026, 8, 22, 14, 30, 0)

    println("unix:    ${time.to_unix(dt)}")
    println("weekday: ${time.weekday(dt)}")

    // Leap years and month lengths, without a table of your own.
    println("2024 leap: ${time.is_leap_year(2024)}")
    println("2026 leap: ${time.is_leap_year(2026)}")
    println("Feb 2024:  ${time.days_in_month(2024, 2)} days")
}
```
```output
unix:    1787409000
weekday: 6
2024 leap: true
2026 leap: false
Feb 2024:  29 days
```

`weekday` is 0-based from Sunday, so 6 is Saturday — 22 August 2026 is indeed
a Saturday.

## Formatting and parsing

`strftime` writes a `DateTime` through a pattern of Ruby's `Time#strftime`
conversions: `%Y %m %d %H %M %S`, the names `%B %b %A %a`, `%j %e %I %l %p`,
the offset `%z` / `%:z`, ISO weeks `%V %G`, `%s` for epoch seconds, the
combinations `%F %T %D %c`, and Ruby's flags between the `%` and the letter
(`-` for no padding, `_` for spaces, `^` for upper case). A conversion it does
not know is written as it stands.

`parse_iso8601_offset` reads the ISO-8601 forms people write — a date alone,
`T` or a space before the clock, seconds and a fraction optional, and a zone
of `Z`, `UTC` or `+HH:MM` — and returns the instant with the offset it was
written at. `strftime_at` prints an instant at such an offset.

```aether,run
import std.time

main() {
    dt = time.from_civil(2024, 3, 5, 7, 4, 9)
    println(time.strftime(dt, "%A %-d %B %Y, %-I:%M %p"))
    println(time.strftime(dt, "%F %T %z"))

    at, offset, err = time.parse_iso8601_offset("2024-03-05T12:34:09+05:30")
    println("${time.to_iso8601(at)} ${offset} [${err}]")
    println(time.strftime_at(at, offset, "%H:%M %:z"))
}
```
```output
Tuesday 5 March 2024, 7:04 AM
2024-03-05 07:04:09 +0000
2024-03-05T07:04:09Z 19800 []
12:34 +05:30
```

## Without the clock: `std.time.calendar`

Everything above except `now` and `now_ms` is `std.time.calendar`, which
std.time builds on. The clock comes through `std.os`, which `--emit=lib`
gates behind `--with=os`. Code that only converts, computes and formats
dates, such as a template engine, imports `std.time.calendar` and needs no
such grant. Its functions have the same names (`calendar.strftime(dt, fmt)`).

## Exports

`DateTime`, `now`, `now_ms`, `from_civil`, `from_unix`, `to_unix`, `weekday`,
`day_of_year`, `is_leap_year`, `days_in_month`, `add_seconds`, `add_minutes`,
`add_hours`, `add_days`, `diff_seconds`, `is_before`, `is_after`,
`to_iso8601`, `parse_iso8601`, `parse_iso8601_offset`, `strftime`,
`strftime_at`.
