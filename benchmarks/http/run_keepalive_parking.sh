#!/bin/bash
# What connection parking is worth to the HTTP server (#1663).
#
# Runs the same load against the same binary twice — parking on, then off
# via AETHER_HTTP_PARKING=0 — so the comparison isolates "parking engaged"
# from "parking code present" and from build-to-build drift. Comparing
# against a separately built pre-parking binary does not: the reference
# number moves under you (one cell varied 11% run to run that way).
#
# Parking releases a worker across a connection's idle gap, so the effect
# only appears above the worker count (cores*2, min 8, max 64). Expect a
# few percent of handoff cost below that line and a large gain above it.
#
# Needs oha (https://github.com/hatoo/oha); wrk or ab would do as well,
# and the ratio between the two columns matters more than the absolutes.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="${ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)}"; cd "$ROOT" || exit 1
OHA="${OHA:-$(command -v oha || echo "$HOME/.cargo/bin/oha")}"
REQ="${REQUESTS:-6000}"
CONC="${CONCURRENCIES:-4 8 16 50}"; PORT="${PORT:-18310}"

[ -x "$OHA" ] || { echo "ERROR: oha is not installed (set OHA=/path/to/oha)." >&2; exit 1; }
[ -x build/ae ] || { echo "ERROR: build/ae is missing (run make)." >&2; exit 1; }

# Parking bounds concurrency on file descriptors, so the default soft
# limit becomes the ceiling it is meant to remove.
ulimit -n 65536 2>/dev/null || ulimit -n 8192 2>/dev/null
TMP=$(mktemp -d); SRV=""
cleanup(){ [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

# One route, fixed small body — the same backend the lb reuse benchmark
# uses, with nothing else on the path.
./build/ae build benchmarks/http/lb_reuse_backend.ae -o "$TMP/b" >/dev/null 2>&1 || {
    echo "ERROR: could not build lb_reuse_backend.ae" >&2; exit 1; }

CORES=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
WORKERS=$((CORES * 2)); [ "$WORKERS" -lt 8 ] && WORKERS=8
[ "$WORKERS" -gt 64 ] && WORKERS=64

echo "=== HTTP keep-alive connection parking (#1663) ==="
echo "date:     $(date)"
echo "host:     $(uname -n)  cores=$CORES  workers=$WORKERS"
echo "commit:   $(git log --oneline -1 2>/dev/null)"
echo "requests: $REQ per cell"
echo
printf '%-8s %12s %12s %8s\n' "conc" "park=off" "park=on" "delta"
for c in $CONC; do
  r=()
  for mode in 0 1; do
    AETHER_HTTP_PARKING=$mode PORT=$PORT "$TMP/b" >/dev/null 2>&1 & SRV=$!
    for i in $(seq 1 50); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && { exec 3<&-; break; }; sleep 0.1; done
    v=$("$OHA" --no-tui -c "$c" -n "$REQ" "http://127.0.0.1:$PORT/" 2>/dev/null | awk '/Requests\/sec/{printf "%.0f",$2}')
    kill -9 $SRV 2>/dev/null; wait $SRV 2>/dev/null; SRV=""; sleep 1
    r+=("${v:-0}")
  done
  d=$(awk -v a="${r[0]}" -v b="${r[1]}" 'BEGIN{if(a>0)printf "%+.0f%%",(b-a)*100/a; else print "-"}')
  printf '%-8s %12s %12s %8s\n' "$c" "${r[0]}" "${r[1]}" "$d"
done
