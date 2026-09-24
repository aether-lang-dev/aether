- **A module's `@source` file that the program also names with `--extra` is
  compiled once.** `ae build` joined the `--extra` / `extra_sources` list and
  the module-declared sources as they came. A program that still named a
  module's C file itself, which modules asked for before `@source` existed
  (#2125), and imported that module compiled the file twice. The link then
  failed with every symbol in it defined twice. The lists are now merged by
  file identity, so a relative path, a path with `..` and an absolute one
  count as the same file. Cross builds share the merge. This is what lets
  `contrib.vulkan` declare its C file with `@source` without breaking
  projects that list it in `extra_sources`. New test:
  `tests/integration/source_dedupe`.
