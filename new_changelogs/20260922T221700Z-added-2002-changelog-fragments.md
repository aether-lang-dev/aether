- **Changelog fragments in `new_changelogs/` (#2002).** A PR drops a file of
  its own instead of editing the shared `## [current]` lines. Those lines
  are what a release *renames*, so merging a new `main` into any open branch
  folded that branch's entry into the released section — with no conflict,
  because the headings merge cleanly and only the bullets land in the wrong
  place. It had already hit four PRs in a row across 0.661–0.664, three more
  since, and twice in one day the batch that wrote this. With a file per
  change, two PRs cannot touch the same line and a release cannot fold an
  entry that is not in `CHANGELOG.md` yet.

  `make add-changelog SECTION=fixed SLUG=...` writes one;
  `make collect-changelogs` folds what has settled, which a daily
  `[skip actions]` job runs; a release folds everything pending, because a
  change merged minutes before one must not be missing from it. The CI gate
  accepts a fragment or a direct `CHANGELOG.md` edit, and still rejects
  shipped-code changes with neither.

  Two decisions the issue left open, both answered here and both a constant
  at the top of `tests/scripts/collect_changelogs.py`: the cooling-off
  window is 4 hours as proposed, and a fragment's age comes from its
  **commit** time rather than its filename. The issue proposed the filename
  with a reset on every push; the commit time is the same intent by a route
  that needs no hook installed on anyone's machine, cannot be set by the
  author, and cannot mistake "written yesterday on a branch that just
  merged" for "landed an hour ago".
