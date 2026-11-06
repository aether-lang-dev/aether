#!/usr/bin/env python3
"""Unit tests for check_shell_prefix.py."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_shell_prefix as c  # noqa: E402


def flagged(text, extra=frozenset()):
    return [n for n, _ in c.offenders_in(text, extra)]


class PrefixOnFunction(unittest.TestCase):
    def test_a_prefix_on_a_function_is_flagged(self):
        self.assertEqual(flagged('run_case() {\n :\n}\nCENV="A=1" run_case x y\n'), [4])
        self.assertEqual(flagged("f() { :; }\nA=1 B='x y' f z"), [2])
        self.assertEqual(flagged("function f { :; }\nA=1 f"), [2])
        self.assertEqual(flagged("f() ( :; )\nif x; then A=1 f; fi"), [2])
        self.assertEqual(flagged("A=1 lib_fn", {"lib_fn"}), [1])

    def test_a_prefix_on_an_external_command_passes(self):
        self.assertEqual(flagged("f() { :; }\nA=1 ./prog f\nA=1 env f"), [])
        self.assertEqual(flagged("A=1 openssl req"), [])

    def test_plain_assignments_and_calls_pass(self):
        self.assertEqual(flagged("f() { :; }\nA=1; f\nf A=1\nx=$(f); f"), [])

    def test_comments_and_heredoc_bodies_are_not_code(self):
        self.assertEqual(flagged("f() { :; }\n# A=1 f\ncat <<E\nA=1 f\nE\n"), [])

    def test_a_waiver_on_the_line_or_the_definition(self):
        self.assertEqual(flagged("f() { :; }\nA=1 f  # prefix-ok: wanted"), [])
        self.assertEqual(flagged("f() { g; A=; }  # prefix-ok: clears A\nA=1 f"), [])


if __name__ == "__main__":
    unittest.main()
