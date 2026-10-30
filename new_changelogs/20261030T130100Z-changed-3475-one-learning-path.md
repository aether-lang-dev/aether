- **One way to learn Aether (#3475).** Getting started, the tutorial,
  three more tutorials and the examples taught the same things four times,
  in four orders, partly in an older style (the deprecated `export`
  keyword, answers built from `print` chains). Now each has one job:
  `docs/getting-started.md` installs and starts a project, the examples
  are the path, `docs/tutorial.md` is that path told in prose (a chapter
  per part, common mistakes, exercises with answers, every program checked
  by `make check-docs`), and `docs/tutorials/` is folded into it. The
  README's documentation list starts with the path. The package-publishing
  details moved from getting-started to `docs/package-fetching.md`.
  The path also says what the runtime does where it did not: a `make`
  buffer has one owner, the local that releases it (11-arrays-and-slices
  and 17-ownership called it a handle the program frees, and five examples
  freed one by hand), and an ask whose handler returns without replying
  ends at once (03-ask-reply said it waited out the 5-second timeout).
