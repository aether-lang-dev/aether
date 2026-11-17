- **A `const` or `var` name declared twice at the top of one file is a
  compile error (#3708).** Nothing diagnosed it: in a module the second
  declaration was dropped without a word and every use read the first value,
  and in a program both reached the generated C, where only the C compiler
  objected. The second declaration now gets E0400 at its own line, naming
  the line of the first. One name per arm of a top-level `when` is still one
  declaration.
