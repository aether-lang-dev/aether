#!/bin/sh
# std.fs's durable calls flush to the device: fs.fsync, fs.fd_fsync and
# write_atomic's staged file. On macOS fsync(2) leaves the data in the
# drive's write cache, and fcntl(F_FULLFSYNC) is what flushes it; std had
# no F_FULLFSYNC, so those calls were not durable there. probe.ae runs each
# call (and expects an error for a closed descriptor); on macOS it runs with
# count_fullfsync.c interposed on fcntl, and each call must have asked for
# F_FULLFSYNC. Elsewhere the probe only runs.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] fs_full_fsync: $AE not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/probe.ae" -o "$TMPDIR/probe" \
        >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] fs_full_fsync: build failed"
    sed 's/^/    /' "$TMPDIR/build.log" | head -15
    exit 1
fi

if [ "$(uname -s)" != "Darwin" ]; then
    if ! "$TMPDIR/probe" >"$TMPDIR/run.log" 2>&1; then
        echo "  [FAIL] fs_full_fsync: probe failed"
        sed 's/^/    /' "$TMPDIR/run.log" | head -20
        exit 1
    fi
    echo "  [PASS] fs_full_fsync (the durable calls run; F_FULLFSYNC is macOS's)"
    exit 0
fi

if ! cc -dynamiclib -o "$TMPDIR/count_fullfsync.dylib" "$SCRIPT_DIR/count_fullfsync.c" \
        >"$TMPDIR/cc.log" 2>&1; then
    echo "  [FAIL] fs_full_fsync: the counting library did not build"
    sed 's/^/    /' "$TMPDIR/cc.log" | head -10
    exit 1
fi
if ! DYLD_INSERT_LIBRARIES="$TMPDIR/count_fullfsync.dylib" "$TMPDIR/probe" \
        >"$TMPDIR/run.log" 2>&1; then
    echo "  [FAIL] fs_full_fsync: probe failed"
    sed 's/^/    /' "$TMPDIR/run.log" | head -20
    exit 1
fi
n="$(sed -n 's/^F_FULLFSYNC calls: //p' "$TMPDIR/run.log")"
if [ -z "$n" ]; then
    echo "  [SKIP] fs_full_fsync: the counting library was not loaded (interposition refused here)"
    exit 0
fi
# fs.fsync, fs.fd_fsync, write_atomic, and the closed descriptor's attempt.
if [ "$n" -lt 3 ]; then
    echo "  [FAIL] fs_full_fsync: $n F_FULLFSYNC calls, expected one per durable call (3 at least)"
    exit 1
fi
echo "  [PASS] fs_full_fsync ($n F_FULLFSYNC calls on macOS)"
