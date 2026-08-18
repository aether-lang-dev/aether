#!/usr/bin/env python3
"""Static checks over the examples in stdlib module doc comments (#1500).

Module examples are deliberately fragments: they contain `...`, they reference
names declared elsewhere, and rewriting them into compilable programs would
make them worse to read. So they cannot be compiled. Two checks catch the
class of defect that matters anyway, without that cost:

  A. a `word {` at statement position where `word` is not a control keyword.
     A documented `loop { ... }` is not Aether, and three modules had one.

  B. a documented `mod.fn(...)` call that the module does not have. One module
     documented `http1.read_response(...)`, which does not exist.

Check B is only useful with its false-positive rules, each of which cost a
wrong first attempt when this was prototyped:

  - the module-prefix convention: `string.concat` is the exported
    `string_concat`, so a bare-name lookup must try `<mod>_<name>` too;
  - every definition form counts: plain `name(...)`, `fn name(...)`,
    `builder name(...)`, and `@extern("c_symbol") name(...)`;
  - comments inside `exports(...)` must be stripped before splitting, or
    prose inside the list is read as exported names;
  - only fully lower-case names are calls. Stdlib functions are snake_case, so
    a capital marks a metavariable: `string.seq_X(...)` in std.string stands
    for the seq_empty/seq_cons/seq_head family, not a function.

Without all three, six modules report falsely.

Runs over `std/**/module.ae` in well under a second and needs no toolchain.
"""

import os
import re
import sys

# From editor/vscode/aether.tmLanguage.json's control-keywords + storage
# keywords: the words that legitimately introduce a `{` block.
BLOCK_KEYWORDS = {
    "if", "else", "for", "in", "while", "switch", "case", "default", "break",
    "continue", "return", "match", "receive", "reply", "send", "spawn_actor",
    "spawn", "make", "defer", "panic", "try", "catch", "after", "when",
    "requires", "ensures",
    "actor", "struct", "message", "enum", "union", "bitstruct", "module",
    "fn", "builder", "export", "exports", "extern", "import", "const", "var",
    "let", "state", "hide", "seal", "distinct", "sum", "fault", "test",
    "describe", "it", "before", "before_each", "after_each", "template",
    "each", "unless", "do", "then",
}

DOC_LINE = re.compile(r"^\s*//\s?(.*)$")
# `word {` at the start of a documented statement, allowing a leading marker.
STMT_BLOCK = re.compile(r"^\s*(?:[-*>]\s*)?([A-Za-z_]\w*)\s*\{\s*$")
CALL = re.compile(r"\b([a-z_][a-z0-9_]*)\.([a-z_][a-z0-9_]*)\s*\(")


def module_doc_lines(path):
    """Yield (lineno, text) for every `//` comment line in the file."""
    with open(path, encoding="utf-8", errors="ignore") as f:
        for n, line in enumerate(f, 1):
            m = DOC_LINE.match(line)
            if m:
                yield n, m.group(1)


def strip_comments(text):
    """Remove `//` and `/* */` comments. exports(...) lists carry both."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def module_names(path):
    """Every name a module offers: its exports plus its definitions."""
    with open(path, encoding="utf-8", errors="ignore") as f:
        src = f.read()

    names = set()

    # exports( a, b, c ) — comments inside the list are stripped first.
    for m in re.finditer(r"\bexports?\s*\(([^)]*)\)", src, re.S):
        for part in strip_comments(m.group(1)).split(","):
            part = part.strip()
            if re.fullmatch(r"[A-Za-z_]\w*", part):
                names.add(part)

    body = strip_comments(src)

    # Definition forms, all at line start:
    #   name(...)            plain
    #   fn name(...)         explicit
    #   builder name(...)    builder DSL
    #   @extern("sym") name(...)
    #   const NAME = ...     / var NAME = ...
    for m in re.finditer(r"^\s*(?:@extern\([^)]*\)\s*)?(?:fn|builder)?\s*"
                         r"([A-Za-z_]\w*)\s*\(", body, re.M):
        names.add(m.group(1))
    for m in re.finditer(r"^\s*(?:const|var)\s+([A-Za-z_]\w*)", body, re.M):
        names.add(m.group(1))
    for m in re.finditer(r"^\s*extern\s+([A-Za-z_]\w*)\s*\(", body, re.M):
        names.add(m.group(1))

    return names


def check_module(path, ns, findings):
    names = module_names(path)

    for lineno, text in module_doc_lines(path):
        # Check A: a block introduced by something that is not a keyword.
        m = STMT_BLOCK.match(text)
        if m and m.group(1) not in BLOCK_KEYWORDS:
            findings.append(
                (path, lineno,
                 f"`{m.group(1)} {{` is not an Aether block: no such keyword"))

        # Check B: a documented call into this module that does not resolve.
        for call in CALL.finditer(text):
            mod, fn = call.group(1), call.group(2)
            if mod != ns:
                continue                      # another module's surface
            # The module-prefix convention: `string.concat` is `string_concat`.
            if fn in names or f"{ns}_{fn}" in names:
                continue
            findings.append(
                (path, lineno,
                 f"`{ns}.{fn}(` is documented but the module has no `{fn}` "
                 f"or `{ns}_{fn}`"))


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    std = os.path.join(root, "std")

    modules = []
    for dirpath, _dirs, files in os.walk(std):
        if "module.ae" in files:
            rel = os.path.relpath(dirpath, std).replace(os.sep, ".")
            ns = rel.split(".")[-1]
            modules.append((os.path.join(dirpath, "module.ae"), ns))
    modules.sort()

    findings = []
    for path, ns in modules:
        check_module(path, ns, findings)

    if findings:
        print("Documentation examples that would not work:")
        for path, lineno, msg in findings:
            print(f"  {os.path.relpath(path, root)}:{lineno}: {msg}")
        print()
        print(f"{len(findings)} finding(s) across {len(modules)} modules.")
        print("Fix the example, or the module, whichever is wrong.")
        return 1

    print(f"doc examples: {len(modules)} stdlib modules, no unresolvable "
          f"calls and no non-keyword blocks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
