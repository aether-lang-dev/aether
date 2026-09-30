#!/usr/bin/env python3
"""Census: every std module has tests somewhere, or a waiver (#1584).

The mapping between a stdlib module and its tests was a naming convention
with holes: `std.http.server.lb` sat at 292 lines with no test of any kind
until a manual sweep found it, and nothing structural made that visible.

The fix is co-location, the same shape check_module_readmes.py uses for
docs: a module owns its unit tests, `std/<mod>/test_*.ae`, `make test-ae`
sweeps them, and the install/release copy sites strip them again so the
toolchain never ships its own test corpus (test-release-archive asserts
that). A spec that lives beside the code it covers is one the next person
to change that code sees in the same diff, and `std/foo` changed => run
`std/foo/test_*.ae` is a mechanical mapping a later `make test-changed`
can build on.

This script is the census that keeps the gap visible. Every module.ae
under std/ is in one of four states:

  * spec     — has a co-located `test_*.ae` in its own directory. The
               end state; the adoption rule is that a module being
               touched gains one.
  * central  — no co-located spec, but some `.ae` or `.sh` under tests/
               imports it. The ~340 central regression files stay put
               (moving them breaks `git log --follow` and docs paths), so
               this is not a failure — it is the backlog, printed so it
               shrinks rather than hides.
  * waived   — neither, and WAIVED says why. A waiver is a claim that
               the module is exercised somewhere this script cannot see,
               not that testing it is hard.
  * missing  — none of the above. A build failure: a new module ships
               with a spec, or with a stated reason it does not.

A waiver for a module that no longer exists, or that has since gained a
co-located spec, is stale and also fails: the list only ever shrinks.
"""

import os
import re
import sys

# Modules with no co-located spec and no central test that imports them,
# and why. Keep the reason a pointer to where the coverage actually is.
WAIVED = {
    # std.time re-exports the whole calendar under the same names and
    # std/time/test_time.ae drives from_civil / add_days / ISO-8601 /
    # strftime through it. A second copy of those vectors beside the
    # calendar would be the duplication that makes two files drift.
    "std.time.calendar": "exercised through std.time (std/time/test_time.ae)",
    # An outer driver: it edits a SUT on disk, rebuilds and re-runs a
    # suite per mutant, so its test is a shell harness that runs the
    # documented example (examples/mutation-testing/mutate.ae imports it)
    # against a fixture and checks the exact mutation-score line.
    "std.mutation": "driven end-to-end by tests/integration/mutation_testing",
}

IMPORT = re.compile(r"^\s*import\s+(std(?:\.[A-Za-z_][A-Za-z0-9_]*)+)\b", re.M)


def modules(std_dir):
    """Every module under std/, as dotted names, one per module.ae."""
    out = []
    for dirpath, dirnames, names in os.walk(std_dir):
        dirnames.sort()
        if "module.ae" in names:
            rel = os.path.relpath(dirpath, std_dir).replace(os.sep, "/")
            out.append("std." + rel.replace("/", "."))
    return sorted(out)


def module_dir(std_dir, mod):
    return os.path.join(std_dir, *mod.split(".")[1:])


def has_spec(std_dir, mod):
    """A `test_*.ae` in the module's OWN directory (not a child module's)."""
    d = module_dir(std_dir, mod)
    try:
        names = os.listdir(d)
    except OSError:
        return False
    return any(n.startswith("test_") and n.endswith(".ae")
               and os.path.isfile(os.path.join(d, n)) for n in names)


def central_imports(tests_dir):
    """Every std module some file under tests/ imports.

    `.sh` counts as well as `.ae`: several integration tests carry their
    program as a heredoc inside the driver script (lane_math, mutation).
    """
    found = set()
    for dirpath, _, names in os.walk(tests_dir):
        for n in names:
            if not (n.endswith(".ae") or n.endswith(".sh")):
                continue
            try:
                with open(os.path.join(dirpath, n), encoding="utf-8",
                          errors="ignore") as f:
                    src = f.read()
            except OSError:
                continue
            found.update(IMPORT.findall(src))
    return found


def census(mods, specced, central, waived):
    """Sort modules into their states. Pure: takes sets, returns dict.

    `stale` lists waivers that no longer describe anything: the module is
    gone, or it now has the co-located spec the waiver stood in for.
    """
    mods = set(mods)
    report = {"spec": [], "central": [], "waived": [], "missing": []}
    for mod in sorted(mods):
        if mod in specced:
            report["spec"].append(mod)
        elif mod in central:
            report["central"].append(mod)
        elif mod in waived:
            report["waived"].append(mod)
        else:
            report["missing"].append(mod)
    report["stale"] = sorted(w for w in waived
                             if w not in mods or w in specced)
    return report


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    std_dir = os.path.join(root, "std")
    tests_dir = os.path.join(root, "tests")
    if not os.path.isdir(std_dir):
        print("  [SKIP] module specs: no std/ directory")
        return 0

    mods = modules(std_dir)
    specced = {m for m in mods if has_spec(std_dir, m)}
    report = census(mods, specced, central_imports(tests_dir), WAIVED)

    for mod in report["stale"]:
        if mod in specced:
            print(f"  waiver for {mod}, which now has a co-located spec — drop it")
        else:
            print(f"  waiver for {mod}, which no longer exists — drop it")

    missing = report["missing"]
    if missing:
        print(f"  {len(missing)} module(s) with no test anywhere:")
        print("    " + " ".join(missing))
        print("  Each wants a co-located std/<mod>/test_<mod>.ae (std.spec")
        print("  preferred; `make test-ae` sweeps it, the install strips it),")
        print("  or a WAIVED entry in this script saying where it is covered.")
        print("  See std/deque/test_deque.ae for the shape. Tracked by #1584.")

    total = len(mods)
    print(f"module specs: {len(report['spec'])} co-located, "
          f"{len(report['central'])} central-only, "
          f"{len(report['waived'])} waived, "
          f"{total - len(missing)}/{total} covered")
    if report["central"] and os.environ.get("AE_SPECS_VERBOSE"):
        print("  central-only (the co-location backlog):")
        print("    " + " ".join(report["central"]))

    if report["stale"] or missing:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
