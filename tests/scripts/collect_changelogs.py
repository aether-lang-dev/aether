#!/usr/bin/env python3
"""Fold settled `new_changelogs/` fragments into CHANGELOG.md (#2002).

WHY FRAGMENTS EXIST
===================

Every open PR used to edit the same shared lines — `## [current]` in
CHANGELOG.md. When a release is cut it renames those lines
(`[current]` -> `[0.708.0]`), so merging the new main back into any open
branch folds that branch's entry INTO the released section. Git reports no
conflict: the headings merge, and the bullets land under the wrong one.

That is not theoretical. It has happened to four PRs in a row across
0.661-0.664, to three more since, and twice in one day to the batch that
wrote this script — once as a rebase conflict and once as a genuine silent
fold, caught only because a gate compares released sections against main.

Many writers, one file, one shared heading. A PR drops a file of its own
here instead, so two PRs can never touch the same line, and a release
cannot fold an entry that is not in CHANGELOG.md yet.

FRAGMENT FORMAT
===============

    new_changelogs/<UTC timestamp>-<section>-<slug>.md

e.g. `20260922T174500Z-fixed-2162-struct-collision.md`. The section is one
of added / changed / fixed / removed / security / performance, matched
case-insensitively; the timestamp orders entries within a section. The file
body is the bullet(s), markdown, exactly as they should appear.

`make add-changelog` writes the name for you.

WHAT "SETTLED" MEANS
====================

A fragment is folded once it has been on the branch being collected for
longer than the cooling-off window, so a change that is reverted or
followed up within the window is never recorded as a permanent line.

Age is taken from the fragment's COMMIT time, not from its filename and
not from its mtime:

  - mtime is rewritten by any checkout or clone, so it says nothing;
  - the filename is written by the author and a force-push does not
    change it, so it cannot tell "landed an hour ago" from "written
    yesterday on a branch that just merged";
  - the commit time is when it actually arrived, is not something the
    author sets, and costs one `git log -1` per fragment.

The issue proposed the filename timestamp with a reset on every push. This
is the same intent by a more robust route, and one that needs no hook
installed on anybody's machine. Both knobs are constants below.

A RELEASE TAKES EVERYTHING
==========================

`--all` ignores the window: cutting a release is a deliberate "ship what is
here", and a change merged minutes before one must not be missing from it.
The window applies only to the routine daily fold.
"""

import argparse
import os
import re
import subprocess
import sys
from datetime import datetime, timezone

FRAGMENT_DIR = "new_changelogs"
COOLING_OFF_HOURS = 4

# The order sections appear under a version heading, Keep a Changelog's.
SECTION_ORDER = ["Added", "Changed", "Deprecated", "Removed", "Fixed",
                 "Security", "Performance"]
SECTIONS = {s.lower(): s for s in SECTION_ORDER}

NAME_RE = re.compile(
    r"^(?P<stamp>\d{8}T\d{6}Z)-(?P<section>[a-zA-Z]+)-(?P<slug>.+)\.md$")

# What `make add-changelog` seeds the file with. A fragment still carrying
# it was created and never written, and nothing else in the pipeline would
# notice: the name is valid and the body is non-empty, so it would pass the
# PR gate and be published verbatim into a release section.
PLACEHOLDER = "Say what changed, and why it mattered."


def git_env():
    """The environment for a git call, with any inherited repository unset.

    `git -C <dir>` does NOT override GIT_DIR or GIT_WORK_TREE: with either
    set, every call below would answer about a different repository than
    the one asked about, and a committed fragment would read as
    uncommitted -- held back for ever, silently. Whoever invokes this (a
    scheduled job, a release step, a test that made its own scratch repo)
    has no reason to want that inherited.
    """
    env = dict(os.environ)
    for k in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE",
              "GIT_OBJECT_DIRECTORY", "GIT_COMMON_DIR"):
        env.pop(k, None)
    return env


def run(*args):
    return subprocess.run(args, capture_output=True, text=True,
                          env=git_env()).stdout.strip()


def fragment_files(root):
    d = os.path.join(root, FRAGMENT_DIR)
    if not os.path.isdir(d):
        return []
    return sorted(f for f in os.listdir(d) if f.endswith(".md")
                  and f != "README.md")


def tracked(root, path):
    """Is this fragment committed at all?"""
    return subprocess.run(["git", "-C", root, "ls-files", "--error-unmatch",
                           "--", path],
                          capture_output=True,
                          env=git_env()).returncode == 0


def committed_at(root, path):
    """When this fragment arrived.

    Returns (datetime, None) once it is known, or (None, reason). The reason
    matters: a fragment nobody has committed is somebody's working tree and
    is rightly left alone, but a fragment that IS tracked and still has no
    date is an anomaly -- a shallow clone with no history for it, a git that
    could not run -- and must be said out loud. Silently treating it as
    "not settled yet" would hold it back for ever, and a changelog entry
    that never appears is the failure this whole mechanism exists to
    prevent.
    """
    # %ct -- the commit time as a Unix timestamp -- rather than %cI, the
    # ISO string. There is nothing to misparse in an integer.
    #
    # %cI was tried first and failed on CI: the runners' git writes UTC as
    # `2026-09-22T23:18:38Z`, and `datetime.fromisoformat` did not accept a
    # `Z` suffix before Python 3.11. Special-casing that would have left the
    # next dialect (a `+0000` without the colon, a locale-formatted date)
    # to be found the same way. An epoch second has no dialects.
    ct = run("git", "-C", root, "log", "-1", "--format=%ct", "--", path)
    if ct:
        try:
            return datetime.fromtimestamp(int(ct), timezone.utc), None
        except (ValueError, OverflowError, OSError):
            return None, "git gave an unreadable commit time: %r" % ct
    if not tracked(root, path):
        return None, "uncommitted"
    return None, ("tracked, but git reports no commit date for it -- a "
                  "shallow clone, or git could not run here")


def parse(root, name):
    m = NAME_RE.match(name)
    if not m:
        return None, ("name is not <UTC timestamp>-<section>-<slug>.md "
                      "(e.g. 20260922T174500Z-fixed-2162-struct-collision.md)")
    section = SECTIONS.get(m.group("section").lower())
    if not section:
        return None, ("section '%s' is not one of: %s"
                      % (m.group("section"), ", ".join(SECTION_ORDER)))
    path = os.path.join(FRAGMENT_DIR, name)
    with open(os.path.join(root, path), encoding="utf-8") as f:
        body = f.read().strip(chr(10))
    if not body.strip():
        return None, "the fragment is empty"
    if PLACEHOLDER in body:
        return None, ("it still has the placeholder text from "
                      "`make add-changelog` -- write the entry, or delete "
                      "the file if this change needs no changelog line")
    return {"name": name, "path": path, "section": section,
            "stamp": m.group("stamp"), "body": body}, None


def insert(changelog_text, entries, newline):
    """Put each entry under its section inside `## [current]`."""
    lines = changelog_text.split(newline)
    try:
        start = lines.index("## [current]")
    except ValueError:
        raise SystemExit("error: CHANGELOG.md has no '## [current]' heading")
    end = next((i for i in range(start + 1, len(lines))
                if lines[i].startswith("## [")), len(lines))

    block = lines[start + 1:end]

    for section in SECTION_ORDER:
        bodies = [e["body"] for e in entries if e["section"] == section]
        if not bodies:
            continue
        heading = "### " + section
        if heading in block:
            at = block.index(heading) + 1
            while at < len(block) and block[at] == "":
                at += 1
            addition = []
            for b in bodies:
                addition.extend(b.split(chr(10)))
                addition.append("")
            block[at:at] = addition
        else:
            # A new section goes in Keep a Changelog's order, not at the end.
            later = [s for s in SECTION_ORDER[SECTION_ORDER.index(section) + 1:]]
            at = len(block)
            for i, l in enumerate(block):
                if l.startswith("### ") and l[4:] in later:
                    at = i
                    break
            addition = [heading, ""]
            for b in bodies:
                addition.extend(b.split(chr(10)))
                addition.append("")
            block[at:at] = addition

    while block and block[-1] == "":
        block.pop()
    block.append("")

    lines[start + 1:end] = block
    return newline.join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--all", action="store_true",
                    help="fold every fragment regardless of age (release)")
    ap.add_argument("--check", action="store_true",
                    help="validate fragment names and bodies, change nothing")
    ap.add_argument("--dry-run", action="store_true",
                    help="say what would be folded, change nothing")
    ap.add_argument("--root", default=".")
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    names = fragment_files(root)

    parsed, problems = [], []
    for name in names:
        entry, err = parse(root, name)
        if err:
            problems.append("%s/%s: %s" % (FRAGMENT_DIR, name, err))
        else:
            parsed.append(entry)

    if problems:
        for p in problems:
            print("error: " + p, file=sys.stderr)
        return 1

    if args.check:
        print("changelog fragments: %d valid" % len(parsed))
        return 0

    if not parsed:
        print("changelog fragments: none pending")
        return 0

    now = datetime.now(timezone.utc)
    ready, waiting = [], []
    for e in parsed:
        if args.all:
            ready.append(e)
            continue
        when, why = committed_at(root, e["path"])
        if when is None:
            if why != "uncommitted":
                # Not a normal wait. Say so on stderr so a scheduled run
                # that can never make progress is visible in its log.
                print("warning: %s: %s" % (e["name"], why), file=sys.stderr)
            waiting.append((e, why))
            continue
        hours = (now - when).total_seconds() / 3600.0
        if hours >= COOLING_OFF_HOURS:
            ready.append(e)
        else:
            waiting.append((e, "%.1fh old, window is %dh"
                            % (hours, COOLING_OFF_HOURS)))

    for e, why in waiting:
        print("waiting: %s (%s)" % (e["name"], why))

    if not ready:
        print("changelog fragments: nothing settled yet")
        return 0

    ready.sort(key=lambda e: (SECTION_ORDER.index(e["section"]), e["stamp"]))

    if args.dry_run:
        for e in ready:
            print("would fold: %s -> ### %s" % (e["name"], e["section"]))
        return 0

    path = os.path.join(root, "CHANGELOG.md")
    with open(path, encoding="utf-8", newline="") as f:
        text = f.read()
    newline = chr(13) + chr(10) if chr(13) + chr(10) in text else chr(10)

    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(insert(text, ready, newline))

    for e in ready:
        full = os.path.join(root, e["path"])
        if subprocess.run(["git", "-C", root, "rm", "-q", "--", e["path"]],
                          capture_output=True,
                          env=git_env()).returncode != 0:
            os.remove(full)
        print("folded: %s -> ### %s" % (e["name"], e["section"]))

    print("changelog fragments: folded %d, %d still waiting"
          % (len(ready), len(waiting)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
