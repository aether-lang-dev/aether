#!/usr/bin/env python3
"""Print the crashed thread of a macOS crash report, for a CI log.

    print_crash_report.py <process-name> [<since-epoch-seconds>]

When a test dies from a signal on macOS, ReportCrash writes a report under
~/Library/Logs/DiagnosticReports within a few seconds. The runner is gone by
the time anyone reads the log, and the exit code alone ("exit 139") does not
say where the process died. This waits briefly for that report and prints the
exception and the crashed thread's frames, so the log carries the backtrace.

Prints nothing and exits 0 when no report for the process appears, so it can
only ever add to a failure's output.
"""

import glob
import json
import os
import sys
import time

REPORT_DIR = os.path.expanduser("~/Library/Logs/DiagnosticReports")
WAIT_SECONDS = 15
MAX_FRAMES = 40


def newest_report(name, since):
    # Reports are named after the process, which the kernel truncates to
    # MAXCOMLEN (16) characters; `since` keeps a shared prefix from matching
    # an older report.
    prefix = glob.escape(name[:16])
    paths = glob.glob(os.path.join(REPORT_DIR, prefix + "*.ips"))
    paths += glob.glob(os.path.join(REPORT_DIR, "Retired", prefix + "*.ips"))
    fresh = [p for p in paths if os.path.getmtime(p) >= since]
    if not fresh:
        return None
    return max(fresh, key=os.path.getmtime)


def load(path):
    # An .ips file is a one-line JSON header followed by the JSON body.
    with open(path, encoding="utf-8", errors="replace") as f:
        header = f.readline()
        body = f.read()
    try:
        return json.loads(header), json.loads(body)
    except ValueError:
        return None, None


def frame_text(frame, images):
    idx = frame.get("imageIndex")
    image = "?"
    if isinstance(idx, int) and 0 <= idx < len(images):
        image = images[idx].get("name") or images[idx].get("path") or "?"
    symbol = frame.get("symbol")
    if symbol:
        return "%s  %s + %s" % (image, symbol, frame.get("symbolLocation", 0))
    return "%s  +0x%x" % (image, frame.get("imageOffset", 0))


def main():
    if len(sys.argv) < 2:
        return 0
    name = os.path.basename(sys.argv[1])
    since = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0

    path = None
    deadline = time.time() + WAIT_SECONDS
    while path is None and time.time() < deadline:
        path = newest_report(name, since)
        if path is None:
            time.sleep(1)
    if path is None:
        return 0

    header, body = load(path)
    if body is None:
        print("    crash report %s could not be parsed" % path)
        return 0

    print("    crash report: %s" % os.path.basename(path))
    exc = body.get("exception") or {}
    if exc:
        print("    exception: %s %s %s" % (exc.get("type", ""), exc.get("signal", ""),
                                          exc.get("subtype", "")))
    images = body.get("usedImages") or []
    threads = body.get("threads") or []
    crashed = [t for t in threads if t.get("triggered")]
    if not crashed and isinstance(body.get("faultingThread"), int):
        ft = body["faultingThread"]
        if 0 <= ft < len(threads):
            crashed = [threads[ft]]
    for t in crashed:
        label = t.get("name") or t.get("queue") or ""
        print("    crashed thread %s %s" % (t.get("id", ""), label))
        for i, frame in enumerate((t.get("frames") or [])[:MAX_FRAMES]):
            print("      %2d  %s" % (i, frame_text(frame, images)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
