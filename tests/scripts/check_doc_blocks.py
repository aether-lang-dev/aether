#!/usr/bin/env python3
"""Compile the documentation's complete code blocks (#1522).

`docs/` and the README carry hundreds of ```aether blocks. Some are whole
programs a reader can copy and run; most are excerpts. Only the first kind can
be compiled, and until every block said which it was, neither kind was checked:
`http.server_listen`, a `cryptography.base64_*` that never existed, a reserved
word as a parameter name and a tutorial teaching a non-boolean `if` all shipped
in documentation and were found by hand.

The convention is the fence's info string:

    ```aether             a complete program. Must compile. CHECKED HERE.
    ```aether,fragment    an excerpt: no main, or it references names the page
                          established earlier, or it contains a literal `...`.
    ```aether,fails       a deliberate counter-example. Must NOT compile, and
                          this fails if it starts compiling.

`fragment` is not an escape hatch for a broken example. It says the block is
not a program; if a block has a `main()` and is meant to work, leave it bare so
this compiles it.

Needs a built ./build/ae (skips cleanly without one).
"""

import os
import re
import subprocess
import sys
import tempfile

FENCE = re.compile(r"^```(aether[^\n]*)\n(.*?)^```", re.S | re.M)
KNOWN = {"", "fragment", "fails"}


def doc_files(root):
    out = []
    docs = os.path.join(root, "docs")
    for dirpath, _dirs, files in os.walk(docs):
        for f in files:
            if f.endswith(".md"):
                out.append(os.path.join(dirpath, f))
    readme = os.path.join(root, "README.md")
    if os.path.exists(readme):
        out.append(readme)
    return sorted(out)


def blocks_in(path):
    src = open(path, encoding="utf-8", errors="ignore").read()
    for m in FENCE.finditer(src):
        info = m.group(1).strip()
        label = info[len("aether"):].lstrip(",").strip()
        line = src[:m.start()].count("\n") + 1
        yield line, label, m.group(2)


def compiles(ae, code, workdir):
    path = os.path.join(workdir, "block.ae")
    with open(path, "w", encoding="utf-8") as f:
        f.write(code)
    try:
        r = subprocess.run([ae, "check", path], capture_output=True, timeout=120)
    except subprocess.TimeoutExpired:
        return False, "timed out"
    out = (r.stdout + r.stderr).decode("utf-8", "replace")
    first = next((l for l in out.split("\n") if "error" in l), "")
    return r.returncode == 0, first


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    ae = os.path.join(root, "build", "ae" + (".exe" if os.name == "nt" else ""))
    if not os.path.exists(ae):
        print(f"  [SKIP] doc blocks: {ae} not built")
        return 0

    env_home = dict(os.environ)
    env_home["AETHER_HOME"] = ""
    os.environ.update(env_home)

    checked = skipped = counter = 0
    failures = []
    unknown = []

    with tempfile.TemporaryDirectory() as workdir:
        for path in doc_files(root):
            rel = os.path.relpath(path, root)
            for line, label, code in blocks_in(path):
                if label not in KNOWN:
                    unknown.append((rel, line, label))
                    continue
                if label == "fragment":
                    skipped += 1
                    continue
                ok, err = compiles(ae, code, workdir)
                if label == "fails":
                    counter += 1
                    if ok:
                        failures.append(
                            (rel, line,
                             "labelled `fails` but it compiles: either the "
                             "example is no longer wrong, or the label is"))
                    continue
                checked += 1
                if not ok:
                    failures.append((rel, line, err or "does not compile"))

    for rel, line, label in unknown:
        print(f"  {rel}:{line}: unknown block label `{label}` "
              f"(use nothing, `fragment`, or `fails`)")

    for rel, line, msg in failures:
        print(f"  {rel}:{line}: {msg}")

    if failures or unknown:
        print()
        print(f"doc blocks: {len(failures) + len(unknown)} problem(s). "
              f"A complete block must compile; mark an excerpt "
              f"```aether,fragment and a counter-example ```aether,fails.")
        return 1

    print(f"doc blocks: {checked} complete blocks compile, {counter} "
          f"counter-examples still fail, {skipped} fragments skipped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
