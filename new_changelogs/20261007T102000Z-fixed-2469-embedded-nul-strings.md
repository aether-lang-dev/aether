- **A string with an embedded NUL keeps all its bytes in maps, sets,
  string lists, `fs.read`, number parsing and `std.json` (#2469).** A
  `string` reached these C functions as a bare `char*` measured with
  `strlen`, so everything after the first NUL was dropped without an error:
  `"a\0x"` and `"a\0y"` were one map or set key, `url.parse_query("a=x%00y")`
  stored `a` as `"x"`, a 5-byte file `"ab\0cd"` read back as 2 bytes,
  `string.to_int` of `"12\0"` + `"99"` returned 12, a JSON string or key
  holding `\u0000` read back as its first part, `json.str` of such a string
  wrote only that part, and `json.parse` of `[1]` followed by a NUL and
  garbage succeeded. The string now crosses with its length: the map, set
  and JSON externs take `@aether string` and read the AetherString's length
  (a plain C string still reads to its NUL), string lists and map keys store
  length-carrying copies, `fs.read` returns the file as a string of all its
  bytes, `to_int`, `to_long`, `to_float` and `to_double` require the number
  to end where the string does, and `json.get_string` and
  `json.object_entry` copy by length.
