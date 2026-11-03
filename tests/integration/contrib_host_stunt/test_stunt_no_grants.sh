#!/bin/sh
# No grants: everything a guest reaches for on its own is refused.
#
# An empty grant list. The host's own functions (callbacks, the shared map)
# still answer; everything that reaches the system is refused, whichever
# way it goes: libc, libc's syscall(), the syscall instruction, a handle on
# libc, a cached copy of the environment, the host's checked bindings.
# The checks are stunt's (contrib/host/stunt); stunt_lib.sh runs each one in
# every mode that applies here and stops at the first mismatch.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
STUNT_TEST=stunt_no_grants
. "$SCRIPT_DIR/stunt_lib.sh"

policy "-"
stunt open-etc-passwd refused
stunt open-granted-file refused
stunt write-granted-file refused
stunt getenv-home refused
stunt env-cached refused
stunt connect-192.0.2.1 refused
stunt connect-ipv4-mapped refused
stunt exec-bin-sh refused
stunt fork refused
stunt dlopen-libc-by-name refused
stunt dlopen-libc-by-path refused
stunt syscall-fn-openat refused
stunt syscall-openat refused
stunt syscall-socket refused
stunt binding-read-etc-passwd refused
stunt binding-read-granted-file refused
stunt binding-getenv-home refused
stunt callback-greet succeed
stunt callback-add succeed
stunt callback-unregistered refused
stunt map-get succeed
stunt map-set succeed
stunt map-overwrite-input refused
done_checks
