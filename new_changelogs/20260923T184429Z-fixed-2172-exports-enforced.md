- **A module's `exports(...)` list is enforced for standard-library modules,
  and for selective imports (#2172).** The qualified check looked the module
  up by exact name. A qualified use carries the last segment
  (`language.to_title_case`), while a std module registers under its full
  path (`std.language`), so the check found no module for any std module and
  blocked nothing. Every std export list was advisory: `mem.long_to_ptr` was
  called across the tree while missing from `std.mem`'s. A selective import,
  `import m (name)`, was never checked against the list at all, for any
  module. Both now fail with `E0303 'name' is not exported from module 'm'`,
  the selective form at the import line and in the file that wrote it. A
  module may list a name in its prefixed form, `<module>_<name>`, which is
  how `std.math`'s `math_sqrt` stays reachable as `math.sqrt` and through
  `import std.math (sqrt)`.

  Turning it on found what had been relying on the gap, each fixed at the
  right end:
  - Public names missing from their export lists are now exported:
    `bignum.clone` and `bignum.limb` (used by seven crypto modules),
    `blake3.reset_ctx`, the four `tls13_client` helpers `tls13_server` shares,
    and the quality, level and format constants of `std.brotli`, `std.zstd`
    and `std.zlib`.
  - `std.msgpack` gains `get_ext_type(v)`. `ext(type_id, data)` could build
    an extension value, but nothing read its type id back, so the module's
    own test reached into the private struct.
  - Six programs did `import std.io (println)`. `println` is a builtin, not
    something `std.io` exports.

  The reference's `std.msgpack` list also had `bin(s, len)` and
  `ext(type, data, len)` for functions that take no length. New test:
  `tests/integration/exports_enforced`.
