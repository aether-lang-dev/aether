#!/usr/bin/env python3
"""Census: every std module has a co-located README.md, or a waiver (#1523).

`docs/stdlib-reference.md` is a generated index — an accurate list of what
ships, with a one-line purpose and an export count. What it cannot carry is a
worked example per module, and 37 modules had none anywhere.

The fix is co-location, the same shape #1584 used for tests: a module owns its
example, `std/<mod>/README.md`, and `check_doc_blocks.py` compiles and runs the
```aether blocks inside it. An example that lives beside the code it documents
is one a reader finds, and one the next person to change that code sees in the
same diff.

This script is the census that keeps the gap visible. It does NOT generate a
README: a worked example is a thing someone has to write, and a scraped header
would be filler that passes a check while teaching nothing — the same reason
check_stdlib_index.py refuses to invent a purpose.

Modules that genuinely should not carry one are listed in WAIVED with the
reason, so the exception is a decision on the record rather than an omission.

Exit status is 0 while the backlog is being worked through: a missing README is
reported, not fatal. Flip REQUIRE_ALL once the list is empty.
"""

import os
import sys

# Modules with no co-located README, and why. A waiver is a claim that the
# module is better documented somewhere else — not that documenting it is
# hard.
WAIVED = {}

# While the #1523 backlog is worked through, a missing README is reported but
# does not fail the build. Set to True once WAIVED plus the READMEs cover
# every module, so a NEW module cannot ship undocumented.
REQUIRE_ALL = False


def modules(std_dir):
    out = []
    for name in sorted(os.listdir(std_dir)):
        path = os.path.join(std_dir, name)
        if not os.path.isdir(path):
            continue
        if os.path.exists(os.path.join(path, "module.ae")):
            out.append(name)
    return out


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    std_dir = os.path.join(root, "std")
    if not os.path.isdir(std_dir):
        print("  [SKIP] module READMEs: no std/ directory")
        return 0

    have = []
    missing = []
    for mod in modules(std_dir):
        if os.path.exists(os.path.join(std_dir, mod, "README.md")):
            have.append(mod)
        elif mod in WAIVED:
            continue
        else:
            missing.append(mod)

    stale = sorted(set(WAIVED) - set(modules(std_dir)))
    for mod in stale:
        print(f"  waiver for std.{mod}, which no longer exists — drop it")

    if missing:
        print(f"  {len(missing)} module(s) without a co-located README.md:")
        print("    " + " ".join(missing))
        print("  Each wants a short page with at least one ```aether,run")
        print("  block — check_doc_blocks.py compiles and runs it. See")
        print("  std/url/README.md for the shape. Tracked by #1523.")

    covered = len(have) + len(WAIVED)
    total = len(modules(std_dir))
    print(f"module READMEs: {len(have)} written, {len(WAIVED)} waived, "
          f"{covered}/{total} covered")

    if stale:
        return 1
    if missing and REQUIRE_ALL:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
