#!/bin/sh
# One file granted: that file and HOME, nothing beside them.
#
# fs_read of stunt-fixture.txt and env HOME. The granted file opens through
# libc and through the host's read_file; /etc/passwd does not, by any route,
# and a read grant is no write grant.
# The checks are stunt's (contrib/host/stunt); stunt_lib.sh runs each one in
# every mode that applies here and stops at the first mismatch.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
STUNT_TEST=stunt_file_granted
. "$SCRIPT_DIR/stunt_lib.sh"

policy "fs_read=$WORK/stunt-fixture.txt,env=HOME"
stunt open-granted-file succeed
stunt binding-read-granted-file succeed
stunt open-etc-passwd refused
stunt binding-read-etc-passwd refused
stunt syscall-fn-openat refused
stunt syscall-openat refused
stunt write-granted-file refused
stunt getenv-home succeed
stunt env-cached succeed
stunt binding-getenv-home succeed
done_checks
