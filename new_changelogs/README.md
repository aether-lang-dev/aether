# Changelog fragments

A PR records what it changed by **dropping a file in here**, not by editing
`CHANGELOG.md`.

```sh
make add-changelog SECTION=fixed SLUG=2162-struct-collision
$EDITOR new_changelogs/20260922T174500Z-fixed-2162-struct-collision.md
```

Write the bullet exactly as it should appear under the version heading:

```markdown
- **A module's struct no longer collides with a runtime type.** `@c_include`
  put `struct IntArray` into the translation unit of every program that
  imports `std.intarr`, so a module carrying its own `IntArray` stopped
  compiling with an error naming neither of them.
```

A daily job folds settled fragments into `CHANGELOG.md` under
`## [current]` and removes them; a release takes everything pending.

## Why this exists

Every open PR used to edit the same shared lines — `## [current]` at the top
of `CHANGELOG.md`. When a release is cut it *renames* those lines
(`[current]` → `[0.708.0]`), so merging the new `main` back into any open
branch folds that branch's entry **into the released section**. Git reports
no conflict: the headings merge cleanly and the bullets land under the wrong
one.

It has happened to four PRs in a row across 0.661–0.664, to three more
since, and twice in a single day to the batch that added this directory —
once as a rebase conflict and once as a genuine silent fold, caught only
because a gate compares released sections against `main`.

Many writers, one file, one shared heading. With a file per change, two PRs
can never touch the same line, and a release cannot fold an entry that is
not in `CHANGELOG.md` yet.

## The name

    <UTC timestamp>-<section>-<slug>.md

- **timestamp** — `YYYYMMDDTHHMMSSZ`, written by `make add-changelog`. It
  orders entries within a section; nothing else depends on it.
- **section** — `added`, `changed`, `deprecated`, `removed`, `fixed`,
  `security` or `performance`, matched case-insensitively.
- **slug** — anything readable. An issue number first is the convention.

## When a fragment is folded

Once it has been committed for longer than the cooling-off window (4 hours),
so a change that is reverted or followed up within the window never becomes
a permanent line.

Age comes from the fragment's **commit** time, not its filename and not its
mtime. An mtime is rewritten by any checkout or clone, so it says nothing; a
filename is written by the author and survives a force-push, so it cannot
tell "landed an hour ago" from "written yesterday on a branch that just
merged". The commit time is when it actually arrived and is not something an
author sets.

A **release takes every pending fragment regardless of age**. Cutting a
release is a deliberate "ship what is here", and a change merged minutes
before one must not be missing from it.

Both the window and the section list are constants at the top of
`tests/scripts/collect_changelogs.py`.

## Commands

| | |
|---|---|
| `make add-changelog SECTION=fixed SLUG=...` | write a new fragment |
| `make check-changelog` | validate pending fragments, and catch a release-fold |
| `make collect-changelogs` | fold what has settled (what the daily job runs) |
| `python3 tests/scripts/collect_changelogs.py --dry-run` | say what would be folded |

## Editing `CHANGELOG.md` directly

Still allowed, and still checked for the fold — a release entry, a
correction to a released section's wording, or a one-line change where a
fragment would be ceremony. The gate accepts either. What it will not accept
is shipped-code changes with neither.
