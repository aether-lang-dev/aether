#!/bin/sh
# The network granted to one address: that address, however it is spelt.
#
# tcp 1.1.1.1:443. A connect to it goes through (or fails on the network,
# which is not a refusal), and so does one to ::ffff:1.1.1.1, the same
# address as IPv6 spells it: the grant matcher compares the two as one.
# The kernel layer lets an AF_INET socket be made only with a tcp grant.
# The checks are stunt's (contrib/host/stunt); stunt_lib.sh runs each one in
# every mode that applies here and stops at the first mismatch.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
STUNT_TEST=stunt_net_allowed
. "$SCRIPT_DIR/stunt_lib.sh"

policy "tcp=1.1.1.1:443"
stunt connect-1.1.1.1 succeed
stunt connect-ipv4-mapped succeed
stunt syscall-socket succeed
stunt open-etc-passwd refused
done_checks
