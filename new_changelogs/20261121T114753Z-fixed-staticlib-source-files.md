- **`ae build --emit=staticlib` archives the modules' `@source` C files and
  the `--extra` files.** The static archive held the program's object and the
  runtime's and nothing else, so a library importing `contrib.quickjs` left
  every `qjs_*` symbol undefined and a consumer linking the archive without an
  Aether toolchain failed, while `--emit=lib` linked the same files in. Each
  source file is now compiled for the target with the program's flags and
  archived beside it; an object is archived as it is; an archive named with
  `--extra` cannot be a member of one, and the build says to link it beside
  the result. Reported by servirtium-vcr, whose `core/embed.ae` archive had 15
  undefined `qjs_*` symbols and now has none.
