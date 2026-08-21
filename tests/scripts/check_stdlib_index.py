#!/usr/bin/env python3
"""Hold the stdlib reference's module index to the tree it describes.

`docs/stdlib-reference.md` opens with a table of every module that ships, its
purpose, and how many names it exports. The page told the reader that table
came from the source and therefore could not drift. Nothing generated or
checked it, and it had: eleven wrong export counts, a module missing
altogether (`std.mutation`), and a heading claiming 70 modules over 71 rows.

An index that is wrong is worse than no index, because it is the first thing a
reader trusts. This is what makes the claim true:

  - every module with a `std/<name>/module.ae` has exactly one row,
  - no row names a module that does not exist,
  - each row's count equals the names in that module's `exports(...)`,
  - the heading's module count equals the number of rows,
  - purposes are prose rather than a scraped header: no `Import with:`, no
    issue numbers (docs do not carry them), and nothing left mid-sentence.

`--fix` rewrites the counts and the heading, which are mechanical. It will not
invent a purpose for a new module: that is a sentence someone has to write, so
a missing row is reported and left for a human.
"""

import os
import re
import sys

DOC = os.path.join("docs", "stdlib-reference.md")
ROW = re.compile(r"^\| `std\.([a-z0-9_]+)` \| (.*?) \| (\d+) \| (.*?) \|$", re.M)
HEADING = re.compile(r"^## Module index \((\d+) modules\)$", re.M)


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def exported_names(path):
    """The names in a module's `exports(...)`, which is its public surface.

    Parsed by balancing parentheses rather than by matching to the first `)`:
    a comment inside the list can carry one, and stopping there silently
    reported a module with 45 exports as having none.
    """
    src = open(path, encoding="utf-8").read()
    names = []
    for m in re.finditer(r"(?m)^exports\s*\(", src):
        open_at = m.end() - 1
        depth = 0
        for i in range(open_at, len(src)):
            if src[i] == "(":
                depth += 1
            elif src[i] == ")":
                depth -= 1
                if depth == 0:
                    body = strip_comments(src[open_at + 1:i])
                    names += [p.strip() for p in body.split(",") if p.strip()]
                    break
    return names


def tree_modules(root):
    out = {}
    std = os.path.join(root, "std")
    for name in sorted(os.listdir(std)):
        path = os.path.join(std, name, "module.ae")
        if os.path.isfile(path):
            out[name] = len(exported_names(path))
    return out


def check(root, fix):
    doc_path = os.path.join(root, DOC)
    doc = open(doc_path, encoding="utf-8").read()
    rows = {m.group(1): (m.group(2), int(m.group(3)), m.span())
            for m in ROW.finditer(doc)}
    tree = tree_modules(root)

    problems = []
    for mod in sorted(set(tree) - set(rows)):
        problems.append(
            f"std.{mod} ships but has no row: add one with a purpose "
            f"(one sentence, no issue numbers)")
    for mod in sorted(set(rows) - set(tree)):
        problems.append(f"std.{mod} has a row but no std/{mod}/module.ae")

    stale = [(mod, rows[mod][1], tree[mod])
             for mod in sorted(set(rows) & set(tree))
             if rows[mod][1] != tree[mod]]

    for mod, (purpose, _count, _span) in sorted(rows.items()):
        if "Import with:" in purpose:
            problems.append(
                f"std.{mod}'s purpose is a scraped module header, not prose: "
                f"{purpose!r}")
        if re.search(r"#\d{2,}", purpose):
            problems.append(
                f"std.{mod}'s purpose carries an issue number: {purpose!r}")
        if not purpose.endswith((".", ")")) or len(purpose) < 12:
            problems.append(
                f"std.{mod}'s purpose is not a finished sentence: {purpose!r}")

    heading = HEADING.search(doc)
    if not heading:
        problems.append("no `## Module index (N modules)` heading")
    heading_wrong = heading and int(heading.group(1)) != len(rows)

    if fix and (stale or heading_wrong):
        for mod, _was, now in reversed(stale):     # reversed: spans stay valid
            start, end = rows[mod][2]
            line = doc[start:end]
            doc = doc[:start] + re.sub(r"\| \d+ \|", f"| {now} |", line) + doc[end:]
        doc = HEADING.sub(f"## Module index ({len(rows)} modules)", doc, count=1)
        open(doc_path, "w", encoding="utf-8").write(doc)
        print(f"stdlib index: updated {len(stale)} count(s)"
              + (" and the heading" if heading_wrong else ""))
        stale, heading_wrong = [], False

    for mod, was, now in stale:
        problems.append(f"std.{mod}: index says {was} exports, source has {now}")
    if heading_wrong:
        problems.append(f"heading says {heading.group(1)} modules, "
                        f"the table has {len(rows)} rows")

    if problems:
        print("Stdlib index does not match the tree:")
        for p in problems:
            print(f"  {p}")
        print()
        print(f"{len(problems)} problem(s). Counts and the heading: "
              f"python3 tests/scripts/check_stdlib_index.py --fix")
        return 1

    print(f"stdlib index: {len(rows)} modules, every export count matches "
          f"the source")
    return 0


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    return check(root, "--fix" in sys.argv)


if __name__ == "__main__":
    sys.exit(main())
