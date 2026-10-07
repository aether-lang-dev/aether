- **`std.schema`'s error getters no longer crash on an index past the list.**
  `error_field`, `error_code` and `error_message` dereferenced whatever the
  list returned, so `error_field(errors, 0)` on a passing parse was an access
  violation. An index outside `[0, error_count)` now reads as `""`.
- **`std.schema`'s `INT` is 64-bit, and a value outside it is an error.** The
  type check and the `min`/`max`/`positive`/`nonneg` rules parsed 32-bit while
  `parse_json` read 64-bit, so `{"age": 5000000000}` against `max(120)` passed
  with no error and the field, or the whole array holding it, vanished from
  the values. Such a value now meets its bounds (`too_big` here), one past 64
  bits is `invalid_type` on its path, and `default_to` refuses a default its
  field's type cannot read when the schema is built.
- **`std.schema`'s `email()` accepts a dot before the `@`.** It took the first
  dot anywhere as the domain's and refused `first.last@example.com`. It now
  looks for the domain's dot after the `@`, and also refuses one right after
  it (`a@.com`) or at the end, as it documented.
- **`std.schema`'s `one_of` matches a whole option.** It tested for
  `,value,` inside `,set,`, so `one_of("admin,user,guest")` accepted
  `"admin,user"`. It now compares the value with each option in place.
