#!/bin/sh
# A directory granted for writing: writes there, reads nowhere.
#
# fs_write of the work directory. stunt-written.txt is created; the fixture
# beside it cannot be read, since a write grant is not a read grant.
# The checks are stunt's (contrib/host/stunt); stunt_lib.sh runs each one in
# every mode that applies here and stops at the first mismatch.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
STUNT_TEST=stunt_write_granted
. "$SCRIPT_DIR/stunt_lib.sh"

policy "fs_write=$WORK/*"
stunt write-granted-file succeed
stunt open-granted-file refused
stunt binding-read-granted-file refused
done_checks
