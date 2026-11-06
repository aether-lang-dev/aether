#!/usr/bin/env python3
"""Shell tests do not put an assignment in front of a call to a shell function.

`NAME=value some_function args` sets NAME for that one call in bash. Under
POSIX mode, which is what macOS's /bin/sh runs, the assignment stays set after
the function returns, so every later call inherits it. A test written that way
passes under the sweep (which runs `bash test_x.sh`) and fails 26-30 cases when
run by hand as `sh test_x.sh` on a Mac (crypto_tls13_peer, #3608).

This gate reads every `*.sh` under tests/ and tools/, collects the functions
each defines (and those of tests/lib/*.sh, which tests source), and fails on a
line that starts a command (or follows `;`, `&&`, `||`, `|`, `then`, `do`) with
one or more assignments and then the name of such a function. An assignment in
front of an external command (`FOO=1 ./prog`) is fine: it never leaks.

Fix: pass the setting some other way (an argument), or have the function clear
the variable itself (or wrap it: `run() { _run "$@"; NAME=""; }`). A line can
carry `prefix-ok: <why>` if the leak is wanted; a function whose definition
line does (because it clears the variables itself) is exempt everywhere.

Run from the repository root; exit 1 listing every offender.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCAN = ("tests", "tools")
WAIVER = "prefix-ok:"
HEREDOC = re.compile(r"<<-?\s*(['\"]?)(\w+)\1")
DEF = re.compile(r"^\s*(?:function\s+([A-Za-z_][\w.-]*)|([A-Za-z_][\w.-]*)\s*\(\s*\))")
ASSIGN = r"[A-Za-z_]\w*=(?:\"(?:[^\"\\]|\\.)*\"|'[^']*'|(?:\$\([^)]*\)|[^\s\"';&|])+)?"
CALL = re.compile(r"(?:^|[;&|(]|\bthen\b|\bdo\b|\{)\s*((?:" + ASSIGN + r"\s+)+)([A-Za-z_][\w.-]*)(?=\s|;|$)")


def code_lines(text):
    """(number, line) of each line outside a heredoc body and comment lines."""
    end = None
    for n, line in enumerate(text.split("\n"), 1):
        if end is not None:
            if line.strip() == end:
                end = None
            continue
        m = HEREDOC.search(line)
        if m and not line.lstrip().startswith("#"):
            end = m.group(2)
        if line.lstrip().startswith("#"):
            continue
        yield n, line


def functions_in(text):
    names = set()
    for _, line in code_lines(text):
        m = DEF.match(line)
        if m and WAIVER not in line:
            names.add(m.group(1) or m.group(2))
    return names


def offenders_in(text, extra=frozenset()):
    """(line number, line) of each assignment prefix on a shell function."""
    funcs = functions_in(text) | set(extra)
    found = []
    for n, line in code_lines(text):
        if WAIVER in line:
            continue
        for m in CALL.finditer(line):
            if m.group(2) in funcs:
                found.append((n, line.strip()))
                break
    return found


def shell_files():
    for top in SCAN:
        for dirpath, dirs, files in os.walk(os.path.join(ROOT, top)):
            dirs[:] = [d for d in dirs if d not in ("node_modules", ".git", "target")]
            for f in files:
                if f.endswith(".sh"):
                    yield os.path.join(dirpath, f)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def main():
    libfuncs = set()
    libdir = os.path.join(ROOT, "tests", "lib")
    if os.path.isdir(libdir):
        for f in os.listdir(libdir):
            if f.endswith(".sh"):
                libfuncs |= functions_in(read(os.path.join(libdir, f)))
    bad = 0
    for path in sorted(shell_files()):
        for n, line in offenders_in(read(path), libfuncs):
            print("  %s:%d: %s" % (os.path.relpath(path, ROOT), n, line))
            bad += 1
    if bad:
        print("\n%d assignment(s) in front of a shell function call." % bad)
        print("macOS /bin/sh (bash in POSIX mode) keeps `NAME=value func` set after the")
        print("call, so later calls inherit it and the test fails under `sh test.sh` while")
        print("passing under the sweep's `bash`. Pass the value as an argument, or have the")
        print("function clear it (e.g. wrap: run() { _run \"$@\"; NAME=\"\"; }), or add a")
        print("`prefix-ok: <why>` note on the line (or on the function's definition).")
        return 1
    print("  [PASS] no assignment prefixes a shell function call in a test")
    return 0


if __name__ == "__main__":
    sys.exit(main())
