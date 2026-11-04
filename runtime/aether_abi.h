/* aether_abi.h: which runtime a library's generated C was made for
 * (#2957; docs/abi-policy.md has the policy).
 *
 * The runtime ABI is what generated C and the runtime it links share: the
 * layouts of the structs both sides read (ActorBase, Message, a try frame,
 * a counted string) and the entry points the code generator calls. It is
 * not stable across releases: a library's generated C runs only on the
 * runtime it was made for. A self-contained library carries that runtime;
 * a --shared-runtime library, and --emit=csrc or object code compiled into
 * a host, must find the same one, and are refused otherwise (at compile
 * time, at import, at load), never run on another.
 *
 * AETHER_RUNTIME_ABI_REV counts the changes to that ABI between releases,
 * since a source tree carries the last release's version until the next is
 * cut: bump it in the commit that changes a layout or an entry point the
 * generated C uses. tests/runtime/test_abi_layout.c records the layouts at
 * this revision, and fails when one changes without the bump. Dependency
 * free (#defines and one declaration), so generated C includes it cheaply. */
#ifndef AETHER_ABI_H
#define AETHER_ABI_H

#include "aether_version.h"

#define AETHER_RUNTIME_ABI_REV 3

/* Build flags that change a layout generated C reads. */
#ifdef AETHER_DEBUG_MAILBOX
#  define AETHER_ABI_FLAGS "+debug-mailbox"
#else
#  define AETHER_ABI_FLAGS ""
#endif

#define AETHER_ABI_STR2(x) #x
#define AETHER_ABI_STR(x) AETHER_ABI_STR2(x)

/* "0.857.0/abi1": the version, the revision and the flags. */
#define AETHER_RUNTIME_ABI_ID \
    AETHER_VERSION "/abi" AETHER_ABI_STR(AETHER_RUNTIME_ABI_REV) AETHER_ABI_FLAGS

/* The runtime's own, compiled into it: what a library compiled against
 * these headers compares its AETHER_RUNTIME_ABI_ID with before its first
 * call runs. */
#ifdef __cplusplus
extern "C" {
#endif
const char* aether_runtime_abi(void);
#ifdef __cplusplus
}
#endif

#endif /* AETHER_ABI_H */
