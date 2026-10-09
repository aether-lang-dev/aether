- **A closure's struct parameter borrows the caller's strings, as a
  function's does.** Its string trackers were left set on entry, so a closure
  that returned its parameter handed the caller's strings back still owned:
  `cb(make_item("w")).name` freed them twice and crashed with heap corruption,
  and a field store into the parameter freed the caller's string. The
  parameter is now disowned on entry, and the closure frees at its exit only
  the strings it stored itself (#2606).
- **A struct literal copies a string local that is used again.** The literal
  moved the local's string into the field and cleared the local's flag even
  when the local was read or freed later, so `h = Holder { text: text }`
  followed by `string.free(text)` freed the string the struct owned, and the
  struct freed it again (a segfault). A local used again is now copied, as an
  alias of it is; one the literal uses last is still moved (#2602).
- **A struct literal takes any number of string locals.** It recorded at most
  16 moved locals, so the 17th kept its flag and its string was freed by both
  the local and the struct (#2610).
- **On Windows, a reaped spawn token is never taken for a pid.** `os.kill`
  and `os.wait_pid_timeout` take a token or a real pid, and a token the table
  no longer held was opened as a pid: tokens counted from 1, so a stale one
  reached whatever process had that number, on an elevated CI runner another
  test's or the runner's own. Tokens now start above any pid Windows hands out,
  and a reaped one is reported gone (#2609).
