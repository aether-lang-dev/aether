- **Builder blocks on different threads no longer share one stack (#3574).**
  The stack of open builder blocks was one for the whole process, so two
  actors running builder blocks at once on different scheduler threads
  mixed them: a block's calls could set another thread's config, and
  `builder_depth()` counted the other thread's blocks too. Two threads
  growing the stack at the same moment could also lose part of it, which
  crashed or leaked. Each thread now has its own stack. Its first 64 levels
  need no allocation; a deeper stack goes on the heap and is freed once the
  blocks are back to half that depth, so a thread whose blocks have all
  ended keeps none of it.
