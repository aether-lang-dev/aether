- **`free` of a struct parameter's slice field gives back nothing, and says
  so (#3568).** The field is the caller's (parameters borrow), so the copy of
  the struct a function is given holds no reference of its own, yet `free`
  gave back the caller's only one: the buffer was freed under the caller's
  field, which then read freed memory, and the caller's scope exit released
  it again. Such a `free` now leaves the buffer, or the string a field views,
  to the caller, and the compiler warns where it is written (W1007).
