#!/usr/bin/env python3
"""Shell tests compare files through tests/lib/same_bytes.sh (#3110).

cmp and diff come from diffutils, which an MSYS2 install can lack (the
Windows CI shell has neither). A test that called one there failed for the
missing tool and read as a wrong result: `cmp -s a b` exits non-zero both
when the files differ and when there is no cmp. same_bytes compares with
cmp where there is one and with cksum otherwise.

This gate reads every `*.sh` under tests/ except that helper and fails on a
`cmp` or `diff` command, in code with comments, quoted text and heredoc
bodies set aside, unless the file asks `command -v <tool>` first (a diff
shown when one is there, as a failure's detail, is fine).

Not checked: test suites vendored unchanged from another project, which call
diff themselves and are run only by a test that asks `command -v diff` before
it starts them (FreeBSD's sed suite, run by tests/integration/sed_parity/).

Run from the repository root; exit 1 listing every offender.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_shell_kills as csk  # noqa: E402

HELPER = os.path.join("tests", "lib", "same_bytes.sh")
VENDORED = (os.path.join("tests", "integration", "sed_parity", "freebsd") + os.sep,)
TOOL = re.compile(csk.CMD_START + r'(cmp|diff)(?=\s|$)')


def check_source(src):
    """[(line number, message)] for each unguarded cmp or diff command."""
    guarded = set(re.findall(r'command\s+-v\s+(cmp|diff)\b', src))
    found = []
    heredoc_end = None
    for n, raw in enumerate(src.splitlines(), 1):
        if heredoc_end is not None:
            if raw.strip() == heredoc_end:
                heredoc_end = None
            continue
        masked = csk.mask(raw)
        for m in TOOL.finditer(masked):
            if m.group(1) not in guarded:
                found.append((n, "`%s` without `command -v %s`: compare with same_bytes"
                              % (m.group(1), m.group(1))))
        h = csk.HEREDOC.search(raw)
        if h:
            heredoc_end = h.group(2)
    return found


def shell_sources(root):
    out = []
    for dirpath, _, names in os.walk(os.path.join(root, "tests")):
        for name in names:
            if name.endswith(".sh"):
                path = os.path.join(dirpath, name)
                rel = os.path.relpath(path, root)
                if rel != HELPER and not rel.startswith(VENDORED):
                    out.append(path)
    return sorted(out)


def main():
    offences = []
    for path in shell_sources(csk.ROOT):
        with open(path, encoding="utf-8", errors="replace") as f:
            src = f.read()
        rel = os.path.relpath(path, csk.ROOT).replace(os.sep, "/")
        for n, msg in check_source(src):
            offences.append("%s:%d: %s" % (rel, n, msg))
    if offences:
        print("shell tools: %d comparison(s) need a tool the Windows CI shell lacks (#3110):"
              % len(offences))
        for o in offences:
            print("  " + o)
        print("  . tests/lib/same_bytes.sh and call `same_bytes a b`.")
        return 1
    print("shell tools: every shell test compares files with same_bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
