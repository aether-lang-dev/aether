#!/usr/bin/env python3
"""std is silent (#3131): library code never writes to stdout or stderr.

A program built on std, or a .so built with --emit=lib and loaded by a JVM,
CPython or Node, should find nothing on its standard streams that it did not
put there. A std module reports through its return value (the error string of
a `(value, err)`), an accessor (`http_server_error`), or a listener the
program registers, and the program decides what, if anything, to print.
docs/stdlib-module-pattern.md ("std is silent") has the rule.

This gate reads the C under std/ and fails on a call that writes to a
standard stream: printf, puts, putchar, perror, vprintf, or fprintf, fputs,
fputc, putc, fwrite, vfprintf whose statement names stdout or stderr.
Comments and string literals are set aside first.

Not checked:
  * modules whose output is the product: std/io and std/log;
  * vendored sources (PCRE2, utf8proc, miniaudio, libfyaml).

A write that is exempt by the rule (output the program asked for, or the
last line before abort() or a panic) carries `silent-ok: <why>` on the line
that makes the call or on a comment line right above it, so each one has a
reason a reviewer can read.

Run from the repository root; exit 1 listing every offender.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WAIVER = "silent-ok:"

# Modules whose output is their purpose.
PRODUCT = ("std/io/", "std/log/")
# Third-party sources, kept as their upstream wrote them.
VENDORED = ("std/regex/pcre2/", "std/unicode/utf8proc/", "std/audio/miniaudio.h",
            "std/yaml/libfyaml/")

ALWAYS = re.compile(r"\b(printf|puts|putchar|perror|vprintf)\s*\(")
TO_STREAM = re.compile(r"\b(fprintf|fputs|fputc|putc|fwrite|vfprintf)\s*\(")
STREAM = re.compile(r"\b(stdout|stderr)\b")


def blank(src):
    """`src` with comments and string/char literals replaced by spaces,
    newlines kept, so offsets and line numbers still match."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", src[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + re.sub(r"[^\n]", " ", src[i + 1:j - 1]) + (c if j - 1 > i else ""))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def statement_end(code, i):
    """The index just past the `;` ending the statement that starts at i,
    counting parentheses so a `;` inside a call's arguments does not end it."""
    depth = 0
    n = len(code)
    while i < n:
        c = code[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif c == ";" and depth <= 0:
            return i + 1
        i += 1
    return n


def check_source(src):
    """Writes to a standard stream in one C source, as (lineno, callee)."""
    code = blank(src)
    lines = src.split("\n")
    found = []
    for pat, needs_stream in ((ALWAYS, False), (TO_STREAM, True)):
        for m in pat.finditer(code):
            # A member or a longer name (`ma_printf`, `x.puts`) is not the libc call.
            if m.start() > 0 and (code[m.start() - 1].isalnum() or code[m.start() - 1] in "_.>"):
                continue
            if needs_stream and not STREAM.search(code[m.start():statement_end(code, m.end())]):
                continue
            found.append((code.count("\n", 0, m.start()) + 1, m.group(1)))
    out = []
    for n, callee in sorted(set(found)):
        here = lines[n - 1]
        above = lines[n - 2].strip() if n >= 2 else ""
        if WAIVER in here or (above.startswith(("//", "/*", "*")) and WAIVER in above):
            continue
        out.append((n, callee))
    return out


def sources(root):
    out = []
    for dirpath, _, names in os.walk(os.path.join(root, "std")):
        for name in names:
            if not name.endswith((".c", ".h")):
                continue
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            if rel.startswith(PRODUCT) or rel.startswith(VENDORED):
                continue
            out.append((rel, path))
    return sorted(out)


def main():
    offences = []
    for rel, path in sources(ROOT):
        with open(path, encoding="utf-8", errors="replace") as f:
            src = f.read()
        for n, callee in check_source(src):
            offences.append("%s:%d: %s() writes to a standard stream" % (rel, n, callee))
    if offences:
        print("std silent: %d write(s) to stdout/stderr from std (#3131):" % len(offences))
        for o in offences:
            print("  " + o)
        print("  report through the return value or an accessor instead; a write the rule")
        print("  exempts (output the program asked for, the last line before an abort)")
        print("  carries `silent-ok: <why>`. See docs/stdlib-module-pattern.md.")
        return 1
    print("std silent: no std module writes to stdout or stderr unasked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
