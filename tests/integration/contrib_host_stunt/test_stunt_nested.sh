#!/bin/sh
# Nested: a bridge called inside an enforce block is held to both levels.
#
# In-process only (host.ae's '<outer>|<inner>' policy): the host runs the
# bridge inside sandbox.enforce(<outer>), the bridge pushes <inner> as a
# second level with aether_sandbox_push and pops it after. Only what both
# grant is allowed, and the host checks the depth is back where it was
# after every run.
# The checks are stunt's (contrib/host/stunt); stunt_lib.sh runs each one in
# every mode that applies here and stops at the first mismatch.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
STUNT_TEST=stunt_nested
. "$SCRIPT_DIR/stunt_lib.sh"

# The inner level grants /etc/passwd; the outer does not.
policy "fs_read=$WORK/*|fs_read=/etc/passwd"
stunt open-etc-passwd refused
stunt binding-read-etc-passwd refused
stunt open-granted-file refused
stunt binding-read-granted-file refused
# Both grant the fixture; only the outer grants /etc/passwd.
policy "fs_read=$WORK/*,fs_read=/etc/passwd|fs_read=$WORK/*"
stunt open-granted-file succeed
stunt binding-read-granted-file succeed
stunt open-etc-passwd refused
stunt binding-read-etc-passwd refused
done_checks
