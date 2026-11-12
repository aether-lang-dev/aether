- **`hide ns.member` and `import ns hiding (a, b)` deny one member of a
  module and leave the rest of it visible.** `hide` worked on whole names and
  on a namespace as a prefix, so a file that imported `ui` to build a window
  could not be held away from `ui.open_url` except by a grep (sae's kernel).
  `hide ui.open_url` refuses that member in a block, at any depth and through
  an import alias; `import ui hiding (open_url)` does it for every body in the
  file, the file's own rule. A hidden qualified call is now reported as hidden
  (`E0304`) rather than undefined (`E0301`), and a namespace's member or
  constant taken as a value (`f = ui.open_url`) is refused under `hide ui`,
  which it used to walk past.
