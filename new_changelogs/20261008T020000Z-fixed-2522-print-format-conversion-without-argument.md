- **A `print` format conversion with no argument is a compile error
  (#2522).** `print`'s literal is a printf format, but a conversion with
  nothing to fill it compiled and read whatever the C stack held:
  `print("100% done\n")` printed `100 1501462000one` and
  `print("a %s\n")` crashed. The compiler now names the conversion and
  says to write `%%` for a percent sign. A `print` whose only argument is a
  literal is written as its decoded text, with no printf at run time, so
  `%%` prints `%` whether or not the literal holds a NUL.
