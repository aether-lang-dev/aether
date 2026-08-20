#!/bin/sh
# Print a one-line resource snapshot, for AE_SWEEP_RESOURCE_TRACE=1 in the
# test sweep. Silent (and exit 0) when the trace is off.
#
# This lives in a real file rather than being generated into the sweep's
# temporary runner because the awk it needs does not survive Makefile
# escaping intact — the first attempt emitted `\&\&`, which broke the
# expression on every platform while still "working" in the sense of
# printing something.
#
# Why it exists: the Windows leg has been killed mid-sweep with rc=2304
# (SIGKILL) at a REPRODUCIBLE point — same test count, same last test,
# across DIFFERENT runner VMs with an identical image. That is the shape of
# a resource ceiling rather than a flaky machine, but nothing recorded what
# was exhausted, so the kill was unattributable.
#
# MemAvailable is Linux-only. MSYS2 — the platform this exists to diagnose —
# ships an 8-field /proc/meminfo with MemTotal/MemFree and NO MemAvailable,
# so keying on MemAvailable alone printed "(mem unknown)" on Windows and
# nowhere else. Verified on a real MSYS2 box.
[ "${AE_SWEEP_RESOURCE_TRACE:-0}" = "1" ] || exit 0

label="$1"
mem=""
if [ -r /proc/meminfo ]; then
    mem=$(awk '
        /^MemAvailable:/ { printf "%dMB avail", $2/1024; found = 1 }
        /^MemFree:/      { free = $2 }
        END { if (!found && free) printf "%dMB free", free/1024 }
    ' /proc/meminfo 2>/dev/null)
fi
[ -n "$mem" ] || mem="(mem unknown)"
echo "  [RES] $label  $mem"
