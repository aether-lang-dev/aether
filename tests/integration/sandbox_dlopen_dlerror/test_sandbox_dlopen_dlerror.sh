#!/bin/sh
# A dlopen the sandbox preload refuses leaves a dlerror() string, as dlopen's
# contract has it. It used to return NULL with errno = EACCES and nothing for
# dlerror(): the loader had never been asked, so it had nothing to say. A host
# that formats the reason with %s then crashed on the NULL (Tcl 8.6's `load`
# segfaulted; Ruby's Fiddle reported a NULL handle with no reason). Found
# while moving contrib/host's languages to aether-guest-languages.
#
# Asserts, with no native grant:
#   - a refusal by name (libc.so.6) and one by handle (a symlink with an
#     innocent name, found on LD_LIBRARY_PATH) each give a message naming
#     what was asked for and saying the sandbox refused it
#   - the message is returned once, then dlerror() is NULL again
#   - a refusal does not shadow the loader's own later error
#   - the message is the refused thread's, not another thread's
#   - a library that is not libc still loads, with no message
#
# Linux only: the preload is built on Linux and FreeBSD, and the probe's
# library names (libc.so.6, libm.so.6) are glibc's.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LIB="$ROOT/build/libaether_sandbox.so"

case "$(uname -s 2>/dev/null)" in
    Linux) ;;
    *)
        echo "  [SKIP] sandbox_dlopen_dlerror: the probe's library names are glibc's (this is $(uname -s))"
        exit 0 ;;
esac
if [ ! -f "$LIB" ]; then
    echo "  [SKIP] sandbox_dlopen_dlerror: preload library not built"
    exit 0
fi

TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT
if ! ${CC:-cc} -std=c11 -Wall -Wextra -Werror \
        "$SCRIPT_DIR/probe.c" -o "$TMP/probe" -ldl -pthread; then
    echo "  [FAIL] sandbox_dlopen_dlerror: probe did not compile"
    exit 1
fi

# The libc this probe runs on, for the by-handle refusal's symlink.
LIBC=$(ldd "$TMP/probe" 2>/dev/null | awk '/libc\.so/ { print $3; exit }')
if [ -z "$LIBC" ] || [ ! -f "$LIBC" ]; then
    echo "  [SKIP] sandbox_dlopen_dlerror: cannot find the probe's libc (not glibc?)"
    exit 0
fi
ln -s "$LIBC" "$TMP/libplain.so"
printf 'env:HOME\n' > "$TMP/none.grants"

LD_PRELOAD="$LIB" AETHER_SANDBOX_GRANTS="$TMP/none.grants" AETHER_SANDBOX_LOG=none \
    LD_LIBRARY_PATH="$TMP" "$TMP/probe" "$TMP" > "$TMP/boxed.out" 2>&1
rc=$?

fail=0
if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] sandbox_dlopen_dlerror: the probe exited $rc (a crash on the NULL?)"
    fail=1
fi
for label in bare-null bare-message bare-cleared handle-null handle-message handle-cleared \
             loader-error thread-local own-thread libm; do
    line=$(grep "^$label " "$TMP/boxed.out")
    if [ "$line" = "$label ok" ]; then
        echo "  [PASS] $label"
    else
        echo "  [FAIL] $label: ${line:-not reached}"
        fail=1
    fi
done
if [ "$fail" -ne 0 ]; then
    sed 's/^/        boxed: /' "$TMP/boxed.out"
    exit 1
fi
echo "  [PASS] sandbox_dlopen_dlerror"
