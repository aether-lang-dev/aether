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

## Exports

`DateTime`, `now`, `now_ms`, `from_civil`, `from_unix`, `to_unix`, `weekday`,
`day_of_year`, `is_leap_year`, `days_in_month`, `add_seconds`, `add_minutes`,
`add_hours`, `add_days`.
