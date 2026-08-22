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
import re
import sys

# Modules with no co-located README, and why. A waiver is a claim that the
# module is better documented somewhere else — not that documenting it is
# hard.
WAIVED = {}

# While the #1523 backlog is worked through, a missing README is reported but
# does not fail the build. Set to True once WAIVED plus the READMEs cover
# every module, so a NEW module cannot ship undocumented.
REQUIRE_ALL = False


EXPORTS = re.compile(r"^exports\((.*?)^\)", re.S | re.M)
IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
BACKTICKED = re.compile(r"`([a-z_][a-z0-9_]*)`")


def exported_names(module_ae):
    """The names in a module's exports(...) block."""
    try:
        src = open(module_ae, encoding="utf-8", errors="ignore").read()
    except OSError:
        return set()
    m = EXPORTS.search(src)
    if not m:
        return set()
    out = set()
    for tok in m.group(1).split(","):
        tok = re.sub(r"//.*", "", tok).strip()
        if IDENT.match(tok):
            out.add(tok)
    return out


def ghost_names(readme, module_ae):
    """Names a README's `## Exports` section claims that do not exist.

    The example blocks are compiled and run, so a function that does not
    exist is caught there. An EXPORTS LIST is prose — nothing compiles
    it — which is exactly how `math.log2` and `strbuilder.append_char`
    got written into this repo's guides without existing. Checking the
    list against the source closes that gap.

    Only the `## Exports` section is scanned: elsewhere a backticked
    lowercase word is as likely to be prose as an identifier.
    """
    real = exported_names(module_ae)
    if not real:
        return set()
    try:
        text = open(readme, encoding="utf-8", errors="ignore").read()
    except OSError:
        return set()
    if "## Exports" not in text:
        return set()
    tail = text.split("## Exports")[-1]
    ghosts = set()
    for name in BACKTICKED.findall(tail):
        if name in real:
            continue
        # A guide may drop a module-name or `aether_` prefix that the
        # exports list carries.
        if any(r == f"aether_{name}" or r.endswith(f"_{name}") for r in real):
            continue
        ghosts.add(name)
    return ghosts


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
    ghosted = []
    for mod in modules(std_dir):
        readme = os.path.join(std_dir, mod, "README.md")
        if os.path.exists(readme):
            have.append(mod)
            ghosts = ghost_names(readme, os.path.join(std_dir, mod, "module.ae"))
            if ghosts:
                ghosted.append((mod, sorted(ghosts)))
        elif mod in WAIVED:
            continue
        else:
            missing.append(mod)

    stale = sorted(set(WAIVED) - set(modules(std_dir)))
    for mod in stale:
        print(f"  waiver for std.{mod}, which no longer exists — drop it")

    for mod, ghosts in ghosted:
        print(f"  std/{mod}/README.md lists names that std.{mod} does not "
              f"export: {' '.join(ghosts)}")

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

    if stale or ghosted:
        return 1
    if missing and REQUIRE_ALL:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
