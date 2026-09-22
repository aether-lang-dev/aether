#!/usr/bin/env python3
"""The sweep prune list is consistent with the tree (#2132).

`tests/ae_sweep_prune.txt` names the directories the bulk `.ae` sweep
skips; its contract is that each one is driven by its own `test_*.sh`.
The list is hand-maintained with no check in either direction, and both
directions went wrong: a self-contained test was pruned under that
premise and did not run for three weeks, while five server halves of
shell-driven tests were not pruned and cost the sweep 300 worker-seconds
each run, sleeping.

Two checks, from the repository root:

  * FAIL: a pruned directory under tests/ that exists and holds `.ae`
    files but has no `test_*.sh` — nothing runs those tests.
  * WARN: a directory the sweep runs whose `.ae` has a `server.ae` or
    `client.ae` sibling, or sleeps for more than 10 s (unless the file
    carries a `sweep-ok:` comment saying why that sleep is not the sweep's
    wait) — a fixture the
    sweep runs standalone to completion.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SLEEP = re.compile(r'\bsleep\(\s*(\d+)\s*\)')


def prune_patterns():
    out = []
    with open(os.path.join(ROOT, 'tests', 'ae_sweep_prune.txt'), encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith('#'):
                out.append(line)
    return out


def main():
    prunes = prune_patterns()
    failures = []
    warnings = []

    # Pruned directories must have a driver.
    for pat in prunes:
        if not pat.startswith('tests/'):
            continue  # fixture-path substrings such as /lib/ and /custom_lib_dir/
        d = os.path.join(ROOT, pat.strip('/'))
        if not os.path.isdir(d):
            continue
        names = os.listdir(d)
        if not any(n.endswith('.ae') for n in names):
            continue
        if not any(n.startswith('test_') and n.endswith('.sh') for n in names):
            failures.append("%s is pruned but has no test_*.sh: its tests never run" % pat)

    # Sweep-run fixtures that should be pruned.
    for sub in ('syntax', 'compiler', 'integration', 'regression'):
        for dirpath, _, names in os.walk(os.path.join(ROOT, 'tests', sub)):
            rel = os.path.relpath(dirpath, ROOT).replace(os.sep, '/') + '/'
            if any(pat in rel for pat in prunes):
                continue
            ae = [n for n in names if n.endswith('.ae')]
            if not ae:
                continue
            if 'server.ae' in names or 'client.ae' in names:
                warnings.append("%s runs in the sweep but holds a server.ae/client.ae half" % rel)
                continue
            for n in ae:
                with open(os.path.join(dirpath, n), encoding='utf-8', errors='replace') as f:
                    raw = f.read()
                # A long sleep the sweep does not actually wait for — a child
                # mode the parent times out, say — says so in a `sweep-ok:`
                # comment giving the measured runtime. The heuristic stays for
                # every file that does not make that claim.
                if 'sweep-ok:' in raw:
                    continue
                src = re.sub(r'//[^\n]*', '', raw)
                for m in SLEEP.finditer(src):
                    if int(m.group(1)) > 10000:
                        warnings.append("%s%s sleeps %s ms in the sweep" % (rel, n, m.group(1)))
                        break

    for w in warnings:
        print("  [WARN] " + w)
    if failures:
        print("sweep prune list: %d problem(s)" % len(failures))
        for f in failures:
            print("  " + f)
        return 1
    print("sweep prune list: every pruned directory has a driver (%d warning(s))" % len(warnings))
    return 0


if __name__ == '__main__':
    sys.exit(main())
