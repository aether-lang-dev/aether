- **`fs.make_temp_dir("", prefix)` and `fs.make_temp_file("", prefix)` use the
  OS temp dir, as documented.** The C side defaulted only a NULL directory, so
  the empty string the functions document became the template
  `/<prefix>XXXXXX` at the filesystem root, and the call failed with "cannot
  create temp dir" for any ordinary user. An empty prefix now means `"ae"` as
  documented, too.
