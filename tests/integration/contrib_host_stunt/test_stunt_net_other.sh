#!/bin/sh
# The network granted to another address: not this one, in either spelling.
#
# tcp 198.51.100.1:443 (TEST-NET-2). A connect to 192.0.2.1 is refused, and
# spelling it ::ffff:192.0.2.1 does not get round that.
# The checks are stunt's (contrib/host/stunt); stunt_lib.sh runs each one in
# every mode that applies here and stops at the first mismatch.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
STUNT_TEST=stunt_net_other
. "$SCRIPT_DIR/stunt_lib.sh"

policy "tcp=198.51.100.1:443"
stunt connect-192.0.2.1 refused
stunt connect-ipv4-mapped refused
done_checks
