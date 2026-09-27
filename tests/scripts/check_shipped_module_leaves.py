#!/usr/bin/env python3
"""No two shipped modules may need the same namespace (#2255).

When two loaded modules end in the same path segment (`mine.vk`, `gfx.vk`),
the compiler gives each its full path as a namespace (`mine_vk`, `gfx_vk`) so
their merged symbols cannot collide (#2209). One kind of module cannot move:
one whose externs carry its last segment as a prefix. std.math declares
`extern math_floor`, and `math.floor` reaches it as `<namespace>_floor`, so
only the namespace `math` finds it (#2190). The compiler keeps such a module
on its last segment and renames whatever collides with it.

Two such modules sharing a last segment cannot both keep it, and neither can
move. A program importing both would merge them into one namespace. That can
only be prevented where the modules are written, so this census fails when
two shipped modules (under std/ and contrib/) that pin their last segment
share it.

A shipped module is a package facade (`<dir>/module.ae`, named by its
directory) or a file a package imports beside it (`contrib/jq/parser.ae`,
`contrib.jq.parser`). Pure-Aether modules that merely share a last segment,
like std.jsonpath.parser and contrib.jq.parser, are fine: both move.

Usage: check_shipped_module_leaves.py [ROOT]   (ROOT defaults to the repo)
"""

import os
import re
import sys
from collections import defaultdict

SHIPPED_ROOTS = ("std", "contrib")


def modules(root):
    """(dotted name, leaf, path) for every shipped module under root."""
    for top in SHIPPED_ROOTS:
        base = os.path.join(root, top)
        if not os.path.isdir(base):
            continue
        for dirpath, _dirs, files in os.walk(base):
            rel = os.path.relpath(dirpath, root).replace(os.sep, ".")
            for f in sorted(files):
                if not f.endswith(".ae"):
                    continue
                if f.startswith("test_") or f.startswith("example"):
                    continue
                path = os.path.join(dirpath, f)
                if f == "module.ae":
                    name = rel
                else:
                    name = rel + "." + f[:-3]
                yield name, name.rsplit(".", 1)[-1], path


def pins_leaf(path, leaf):
    """Does the module declare an extern whose Aether name is `<leaf>_...`?
    Both `extern leaf_x(...)` and `@extern("sym") leaf_x(...)`."""
    pat = re.compile(
        r"^\s*(?:export\s+)?(?:extern\s+|@extern\s*\([^)]*\)\s*)"
        + re.escape(leaf) + r"_\w*\s*\(",
        re.MULTILINE)
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            return pat.search(fh.read()) is not None
    except OSError:
        return False


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..")
    root = os.path.normpath(root)

    pinned = defaultdict(list)
    total = 0
    for name, leaf, path in modules(root):
        total += 1
        if pins_leaf(path, leaf):
            pinned[leaf].append((name, os.path.relpath(path, root)))

    clashes = {leaf: mods for leaf, mods in pinned.items() if len(mods) > 1}
    if clashes:
        for leaf, mods in sorted(clashes.items()):
            print(f"  [FAIL] {len(mods)} shipped modules need the namespace '{leaf}':")
            for name, path in mods:
                print(f"           {name}  ({path})")
        print("  Each declares externs named '<leaf>_...', which only the namespace")
        print("  '<leaf>' reaches, so neither can take its full path and a program")
        print("  importing both would merge them. Give one of them a different last")
        print("  path segment (or rename its externs so they do not carry it).")
        return 1

    n_pinned = sum(len(m) for m in pinned.values())
    print(f"  [PASS] shipped module leaves: {total} modules, {n_pinned} tied to "
          f"their last segment by externs, none sharing one")
    return 0


if __name__ == "__main__":
    sys.exit(main())
