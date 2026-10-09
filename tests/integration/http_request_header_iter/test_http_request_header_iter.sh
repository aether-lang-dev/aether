#!/bin/sh
# http.request_header_* iteration accessors (vcr_request_header_iteration_wish.md):
# a route handler enumerates every received request header (count + name(i) +
# value(i)), preserving wire order and duplicates, with out-of-range → "".
# The driver binds a background server, sends a raw TCP request with a
# hand-built header order (incl. a duplicate the high-level client can't
# send), and asserts the handler echoed the enumeration back faithfully.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

fail() { echo "  [FAIL] $1"; exit 1; }

OUT="$(AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/iter.ae" 2>&1)" \
    || { echo "$OUT"; fail "request-header iteration driver errored"; }

echo "$OUT" | grep -q "^PASS" \
    || { echo "$OUT"; fail "request-header iteration assertions did not pass"; }

echo "  [PASS] http_request_header_iter: enumerate request headers (order, dups, count, OOB)"
