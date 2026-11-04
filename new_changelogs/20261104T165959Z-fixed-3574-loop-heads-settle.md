- **The free check and the protocol check follow a loop until it settles
  (#3574).** Each gave up on a loop after 256 passes and then followed no
  local or reference in it, so a buffer freed on every turn of a loop that
  took a view of the next of 300 views compiled, and so did a second
  `Open` sent through a reference in a loop that left a bare `actor_ref`
  under one of three protocols by turns: there the protocol check dropped
  the reference at the loop's head and the next turn brought it back, so
  the head never settled. A reference dropped at a loop's head now stays
  dropped for the loop, and each check's budget is what its facts can rise
  by, so every loop settles inside it. A `@keep` function of a name longer
  than 255 characters, called qualified, was not found by the ownership
  check, and a parameter given to it was not refused; the whole name is
  read now.
