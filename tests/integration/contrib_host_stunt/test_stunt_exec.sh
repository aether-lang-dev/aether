#!/bin/sh
# /bin/sh granted, and fork: the guest can start a shell, and only that.
#
# exec /bin/sh and fork. A shell starts; nothing else is granted.
# The checks are stunt's (contrib/host/stunt); stunt_lib.sh runs each one in
# every mode that applies here and stops at the first mismatch.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
STUNT_TEST=stunt_exec
. "$SCRIPT_DIR/stunt_lib.sh"

policy "exec=/bin/sh,fork"
stunt exec-bin-sh succeed
stunt fork succeed
stunt open-etc-passwd refused
done_checks
