- **`std.number.bytes(n, style, locale)` renders a human-readable byte size.**
  `BYTES_SI` divides by 1000 and uses SI units (`1_070_000_000` -> `1.07 GB`);
  `BYTES_IEC` divides by 1024 and uses IEC units (`1_073_741_824` -> `1.00
  GiB`). The decimal-vs-binary choice is explicit in the style — `GB` and `GiB`
  are not interchangeable. The mantissa is 3 significant figures (the
  Finder / Nautilus / ByteCountFormatter convention) and its decimal separator
  follows `locale`; a count below one unit is a plain integer (`512 B`).
  `bytes_si` / `bytes_iec` are en-US convenience forms. Requested by the
  OpenDisk-ae port, which was reproducing "1.07 GB" by hand. (Thousands-grouped
  number formatting was already available via `format_decimal` /
  `format_decimal_default`.)
