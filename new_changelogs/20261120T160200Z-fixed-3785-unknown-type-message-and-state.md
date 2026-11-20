- **An unknown type on a message field or a state field is refused at its
  declaration.** A struct field, a parameter and a local of a type the
  program does not define were E0302 already; a message field and a state
  field's written type were let through and reported only at a use, as
  "expected florp, got int" or "'d' is duration, but this value is
  Duration", and a message never sent reached C as an unknown type name
  (#3785).
