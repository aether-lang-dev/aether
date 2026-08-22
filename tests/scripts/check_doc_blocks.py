#!/usr/bin/env python3
"""Compile the documentation's complete code blocks (#1522).

`docs/`, the README, and each module's co-located README carry hundreds of
```aether blocks. Some are whole
programs a reader can copy and run; most are excerpts. Only the first kind can
be compiled, and until every block said which it was, neither kind was checked:
`http.server_listen`, a `cryptography.base64_*` that never existed, a reserved
word as a parameter name and a tutorial teaching a non-boolean `if` all shipped
in documentation and were found by hand.

The convention is the fence's info string:

    ```aether             a complete program. Must compile. CHECKED HERE.
    ```aether,run         a complete program that is also RUN, and whose stdout
                          must match the ```output block immediately after it.
                          Use for anything whose value is what it prints.
    ```aether,fragment    an excerpt: no main, or it references names the page
                          established earlier, or it contains a literal `...`.
    ```aether,fails       a deliberate counter-example. Must NOT compile, and
                          this fails if it starts compiling.

A `run` block is followed by its expected output:

    ```aether,run
    main() { println("hi") }
    ```
    ```output
    hi
    ```

Compiling proves a function exists; running proves it does what the prose
claims. The base64 example that shipped wrong in two files would have been
caught by either, but an example whose OUTPUT drifted needs the second.

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
KNOWN = {"", "run", "fragment", "fails"}
OUTPUT_FENCE = re.compile(r"\A\s*```output\n(.*?)^```", re.S | re.M)


def doc_files(root):
    """Every markdown file whose Aether blocks are checked.

    docs/ and the top-level README are the prose documentation. std/ and
    contrib/ are included because a module's examples live beside it
    (#1523) — co-located the same way its tests are, so the module owns
    its documentation and the generated index in docs/stdlib-reference.md
    just points at it. Walking them here is what makes those examples
    verified rather than merely present: three blocks in
    std/schema/README.md did not compile for exactly as long as this
    checker ignored the directory.
    """
    out = []
    # contrib/ is deliberately NOT walked yet. Adding it surfaces 40
    # pre-existing broken blocks across 15 module READMEs — real, but a
    # separate piece of work from #1523, and folding it in here would
    # bury the std/ change under contrib churn. Tracked as a follow-up.
    for sub in ("docs", "std"):
        base = os.path.join(root, sub)
        if not os.path.isdir(base):
            continue
        for dirpath, _dirs, files in os.walk(base):
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
        # A `run` block takes its expected stdout from an ```output fence
        # immediately after it (blank lines between are fine). None when
        # there isn't one, which is an error for `run` and ignored
        # otherwise.
        tail = src[m.end():]
        om = OUTPUT_FENCE.match(tail)
        expected = om.group(1) if om else None
        yield line, label, m.group(2), expected


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


def runs(ae, code, expected, workdir):
    """Compile AND run `code`, comparing stdout with `expected`.

    Trailing whitespace on each line and at the end is ignored — a
    reader copying the block out of markdown should not have to
    reproduce it byte for byte — but nothing else is normalised, so
    output that drifts is a failure rather than a shrug.
    """
    path = os.path.join(workdir, "block.ae")
    with open(path, "w", encoding="utf-8") as f:
        f.write(code)
    try:
        r = subprocess.run([ae, "run", path], capture_output=True, timeout=120)
    except subprocess.TimeoutExpired:
        return False, "timed out (a run block must terminate on its own)"
    out = r.stdout.decode("utf-8", "replace")
    err = r.stderr.decode("utf-8", "replace")
    if r.returncode != 0:
        first = next((l for l in (err + out).split("\n") if "error" in l), "")
        return False, first or f"exited {r.returncode}"

    def norm(t):
        return "\n".join(l.rstrip() for l in t.rstrip().split("\n"))

    if norm(out) != norm(expected):
        return False, ("output does not match the ```output block\n"
                       f"      expected: {norm(expected)!r}\n"
                       f"      actual:   {norm(out)!r}")
    return True, ""


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

    checked = skipped = counter = ran = 0
    failures = []
    unknown = []

    with tempfile.TemporaryDirectory() as workdir:
        for path in doc_files(root):
            rel = os.path.relpath(path, root)
            for line, label, code, expected in blocks_in(path):
                if label not in KNOWN:
                    unknown.append((rel, line, label))
                    continue
                if label == "fragment":
                    skipped += 1
                    continue
                if label == "run":
                    if expected is None:
                        failures.append(
                            (rel, line,
                             "labelled `run` but no ```output block follows "
                             "it: a run block declares what it prints"))
                        continue
                    ran += 1
                    ok, err = runs(ae, code, expected, workdir)
                    if not ok:
                        failures.append((rel, line, err))
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
              f"A complete block must compile; a `run` block must also "
              f"print what its ```output block says; mark an excerpt "
              f"```aether,fragment and a counter-example ```aether,fails.")
        return 1

    print(f"doc blocks: {checked} complete blocks compile, {ran} run and "
          f"match their output, {counter} counter-examples still fail, "
          f"{skipped} fragments skipped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
