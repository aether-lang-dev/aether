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
                          An exercise stub whose body is `// Your code here`
                          is one of these: it is a program the READER
                          completes, so it does not compile as written.
    ```aether,fails       a deliberate counter-example. Must NOT compile, and
                          this fails if it starts compiling.
    ```aether,nolink      a complete unit that cannot link BY ITSELF: it calls
                          C the reader supplies, or it is a library whose
                          `main` lives in the host app. One with a `main()` is
                          built as a program, one without as the library it
                          is (`--emit=lib`); either way the ONLY failure
                          allowed is unresolved symbols at the link, since
                          that is precisely what lives outside the block.
                          Prefer this to `fragment` for anything that really
                          does compile: a fragment is skipped, this is checked
                          through codegen and the C compiler.

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

The blocks are built and run in parallel, NPROC at a time (the Makefile's
job count, else the machine's), each in a directory of its own; results are
reported in source order. One at a time, they were 4.5 minutes of every
Windows CI leg (#2594).
"""

import concurrent.futures
import os
import re
import subprocess
import sys
import tempfile

FENCE = re.compile(r"^```(aether[^\n]*)\n(.*?)^```", re.S | re.M)
# A program's entry point: a `main(` declaration opening a line (indentation
# allowed). Deciding from the source keeps the build choice here, rather than
# inferring it from the wording of a failed build.
ENTRY = re.compile(r"^[ \t]*main\s*\(", re.M)
KNOWN = {"", "run", "fragment", "fails", "nolink"}
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


LINK_ONLY_MARKERS = (
    "Undefined symbols",           # Apple ld
    "undefined reference to",      # GNU ld / lld
    "unresolved external symbol",  # MSVC link
)


def link_only_failure(out):
    """True when a build got all the way to the link and only symbols were missing.

    That is the whole of what a `nolink` block is allowed to fail on: the
    reader supplies the C definitions, so codegen ran, clang compiled the
    generated C, and only the linker had nothing to resolve against. Any
    Aether diagnostic, or any error reported against a .c file, means it fell
    over EARLIER than the link, which is exactly what this gate exists to
    catch.
    """
    if not any(m in out for m in LINK_ONLY_MARKERS):
        return False
    if "error[E" in out:
        return False
    for line in out.split("\n"):
        if ".c:" in line and ": error:" in line:
            return False
    return True


def compiles(ae, code, workdir, allow_unresolved=False):
    """Build `code`, all the way to an executable.

    This used to run `ae check`, which is the FRONT END ONLY, while the
    convention above promised the block compiles. Anything that type-checked
    but could not be code-generated therefore passed the gate: 14 of the 187
    blocks it called compiled did not build (#1878), including one that had
    been mistaken for a regression because the gate could not see it.

    `allow_unresolved` accepts a failure that is provably nothing but missing
    symbols at the link, for a block calling C the reader supplies. Such a
    block with no main() is the other half of what the label promises -- "a
    library whose main lives in the host app" -- so it is built as the
    library it is. It used to be built as an executable, and passed only
    because the one thing the link lacked was `main` itself, which this gate
    could not tell from a C symbol the reader supplies.
    """
    path = os.path.join(workdir, "block.ae")
    with open(path, "w", encoding="utf-8") as f:
        f.write(code)
    out_bin = os.path.join(workdir, "block_out")
    cmd = [ae, "build", path, "-o", out_bin]
    if allow_unresolved and not ENTRY.search(code):
        cmd = [ae, "build", "--emit=lib", path, "-o", out_bin]
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=180)
    except subprocess.TimeoutExpired:
        return False, "timed out"
    if r.returncode == 0:
        return True, ""
    out = (r.stdout + r.stderr).decode("utf-8", "replace")
    if allow_unresolved and link_only_failure(out):
        return True, ""
    first = next((l for l in out.split("\n") if "error" in l), "")
    return False, first or "does not build"


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

    # Sort the blocks first, then build and run the checkable ones on a pool;
    # each job has a directory of its own and its verdict lands in its slot,
    # so the report keeps the source order.
    jobs = []   # (rel, line, label, code, expected)
    for path in doc_files(root):
        rel = os.path.relpath(path, root)
        for line, label, code, expected in blocks_in(path):
            if label not in KNOWN:
                unknown.append((rel, line, label))
            elif label == "fragment":
                skipped += 1
            elif label == "run" and expected is None:
                failures.append(
                    (rel, line,
                     "labelled `run` but no ```output block follows "
                     "it: a run block declares what it prints"))
            else:
                jobs.append((rel, line, label, code, expected))

    def check(job, workdir):
        _rel, _line, label, code, expected = job
        if label == "run":
            return runs(ae, code, expected, workdir)
        return compiles(ae, code, workdir, allow_unresolved=(label == "nolink"))

    try:
        workers = int(os.environ.get("NPROC", "") or 0)
    except ValueError:
        workers = 0
    workers = max(1, workers or os.cpu_count() or 1)

    with tempfile.TemporaryDirectory() as tmp:
        dirs = [os.path.join(tmp, str(i)) for i in range(len(jobs))]
        for d in dirs:
            os.mkdir(d)
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            verdicts = list(pool.map(check, jobs, dirs))

    for (rel, line, label, _code, _expected), (ok, err) in zip(jobs, verdicts):
        if label == "run":
            ran += 1
            if not ok:
                failures.append((rel, line, err))
        elif label == "fails":
            counter += 1
            if ok:
                failures.append(
                    (rel, line,
                     "labelled `fails` but it compiles: either the "
                     "example is no longer wrong, or the label is"))
        else:
            checked += 1
            if not ok:
                failures.append((rel, line, err or "does not compile"))
    failures.sort(key=lambda f: (f[0], f[1]))

    for rel, line, label in unknown:
        print(f"  {rel}:{line}: unknown block label `{label}` "
              f"(use nothing, `run`, `nolink`, `fragment`, or `fails`)")

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
