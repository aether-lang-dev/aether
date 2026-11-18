- **A file that ends inside a block is a syntax error.** aetherc took the
  end of the file for a missing `}` and compiled a function body, an `if`
  body or a `main` whose last brace was gone: nib-code's `.tests.ae` lost
  one and aeb then ran 72 of its 82 tests without a word. The error is
  said at the `{` that opened the block left open, since the end of the
  file is not where the mistake is.
  `tests/integration/parser_unclosed_block_at_eof`.
