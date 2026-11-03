#!/bin/sh
# fork granted, exec not: the execve refusal itself.
#
# fork, and nothing else. Without a fork grant (test_stunt_no_grants.sh) the
# contained child's exec-bin-sh is refused at the fork, before any execve;
# here the fork goes through, so it is the execve of /bin/sh in the forked
# child that must be refused (the preload's execve, and the exec fence the
# spawn installs when the grants have no exec).
# The checks are stunt's (contrib/host/stunt); stunt_lib.sh runs each one in
# every mode that applies here and stops at the first mismatch.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
STUNT_TEST=stunt_fork
. "$SCRIPT_DIR/stunt_lib.sh"

policy "fork"
stunt fork succeed
stunt exec-bin-sh refused
stunt open-etc-passwd refused
done_checks
