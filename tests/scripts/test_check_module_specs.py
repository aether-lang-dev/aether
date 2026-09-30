#!/usr/bin/env python3
"""Unit tests for check_module_specs.py (#1584): the census logic on a
synthetic tree, so a rule change here is a red test rather than a green
`make check-tests` that quietly stopped seeing a gap.

Run from anywhere: `python3 tests/scripts/test_check_module_specs.py`.
"""

import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_module_specs as cms  # noqa: E402


def touch(path, text=""):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


class TreeCase(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="specs-census-")
        self.std = os.path.join(self.root, "std")
        self.tests = os.path.join(self.root, "tests")
        os.makedirs(self.std)
        os.makedirs(self.tests)

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)


class ModulesTest(TreeCase):
    def test_every_module_ae_is_a_module_including_nested_ones(self):
        touch(os.path.join(self.std, "http", "module.ae"))
        touch(os.path.join(self.std, "http", "server", "lb", "module.ae"))
        touch(os.path.join(self.std, "regex", "module.ae"))
        touch(os.path.join(self.std, "regex", "pcre2", "vendored.c"))
        self.assertEqual(cms.modules(self.std),
                         ["std.http", "std.http.server.lb", "std.regex"])

    def test_a_directory_without_module_ae_is_not_a_module(self):
        touch(os.path.join(self.std, "notes", "README.md"))
        self.assertEqual(cms.modules(self.std), [])


class HasSpecTest(TreeCase):
    def test_test_file_beside_module_ae_counts(self):
        touch(os.path.join(self.std, "deque", "module.ae"))
        touch(os.path.join(self.std, "deque", "test_deque.ae"))
        self.assertTrue(cms.has_spec(self.std, "std.deque"))

    def test_only_the_test_prefix_counts(self):
        touch(os.path.join(self.std, "deque", "module.ae"))
        touch(os.path.join(self.std, "deque", "example_deque.ae"))
        self.assertFalse(cms.has_spec(self.std, "std.deque"))

    def test_a_child_modules_spec_does_not_cover_the_parent(self):
        touch(os.path.join(self.std, "http", "module.ae"))
        touch(os.path.join(self.std, "http", "server", "module.ae"))
        touch(os.path.join(self.std, "http", "server", "test_server.ae"))
        self.assertFalse(cms.has_spec(self.std, "std.http"))
        self.assertTrue(cms.has_spec(self.std, "std.http.server"))

    def test_a_missing_directory_is_not_a_spec(self):
        self.assertFalse(cms.has_spec(self.std, "std.ghost"))


class CentralImportsTest(TreeCase):
    def test_reads_ae_and_sh_and_ignores_other_files(self):
        touch(os.path.join(self.tests, "regression", "test_a.ae"),
              "import std.string\nimport std.json\nmain() {}\n")
        touch(os.path.join(self.tests, "integration", "x", "test_x.sh"),
              "cat > t.ae <<'EOF'\n    import std.lanes\nEOF\n")
        touch(os.path.join(self.tests, "notes.md"), "import std.ghost\n")
        self.assertEqual(cms.central_imports(self.tests),
                         {"std.string", "std.json", "std.lanes"})

    def test_a_nested_import_names_the_leaf_module_only(self):
        touch(os.path.join(self.tests, "t.ae"),
              "import std.cryptography.sha2\n")
        self.assertEqual(cms.central_imports(self.tests),
                         {"std.cryptography.sha2"})

    def test_contrib_and_local_imports_are_ignored(self):
        touch(os.path.join(self.tests, "t.ae"),
              "import contrib.sqlite\nimport pkg.math\nimport stdx.y\n")
        self.assertEqual(cms.central_imports(self.tests), set())


class CensusTest(unittest.TestCase):
    def test_each_module_lands_in_exactly_one_state(self):
        r = cms.census(
            mods=["std.a", "std.b", "std.c", "std.d"],
            specced={"std.a"},
            central={"std.b"},
            waived={"std.c": "why"},
        )
        self.assertEqual(r["spec"], ["std.a"])
        self.assertEqual(r["central"], ["std.b"])
        self.assertEqual(r["waived"], ["std.c"])
        self.assertEqual(r["missing"], ["std.d"])
        self.assertEqual(r["stale"], [])

    def test_a_spec_outranks_central_and_waiver(self):
        r = cms.census(["std.a"], specced={"std.a"}, central={"std.a"},
                       waived={"std.a": "why"})
        self.assertEqual(r["spec"], ["std.a"])
        self.assertEqual(r["central"], [])
        self.assertEqual(r["waived"], [])

    def test_central_outranks_a_waiver_without_making_it_stale(self):
        r = cms.census(["std.a"], specced=set(), central={"std.a"},
                       waived={"std.a": "why"})
        self.assertEqual(r["central"], ["std.a"])
        self.assertEqual(r["stale"], [])

    def test_waiver_for_a_gone_module_is_stale(self):
        r = cms.census(["std.a"], specced=set(), central=set(),
                       waived={"std.gone": "why"})
        self.assertEqual(r["stale"], ["std.gone"])

    def test_waiver_for_a_module_that_gained_a_spec_is_stale(self):
        r = cms.census(["std.a"], specced={"std.a"}, central=set(),
                       waived={"std.a": "why"})
        self.assertEqual(r["stale"], ["std.a"])


class RealTreeTest(unittest.TestCase):
    """The repository itself passes: the waiver list is current and no
    module is untested. Runs the same entry point `make check-tests` does."""

    def test_repository_census_passes(self):
        self.assertEqual(cms.main(), 0)


if __name__ == "__main__":
    unittest.main()
