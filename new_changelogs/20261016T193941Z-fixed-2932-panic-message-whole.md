- **A failed bounds check's panic message is whole.** It was formatted into a
  192-byte static buffer, so a long source path cut off the part saying what
  went wrong, and two threads failing a check at once wrote the same buffer
  (#2932).
