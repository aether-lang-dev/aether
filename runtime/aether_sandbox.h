// aether_sandbox.h — Runtime containment checks
// The compiler-generated preamble registers a permission checker.
// Stdlib functions (tcp, fs, os) call aether_sandbox_check() before
// performing privileged operations. If no sandbox is active, everything
// is allowed. If a sandbox is active, only granted operations proceed.
//
// When AETHER_HAS_SANDBOX is not defined, aether_sandbox_check is a
// no-op macro that always returns 1 — zero overhead.

#ifndef AETHER_SANDBOX_H
#define AETHER_SANDBOX_H

#include "aether_sandbox_match.h"   /* aether_net_resource, the shared matcher */

#ifdef AETHER_HAS_SANDBOX

// Permission check callback type.
// category: "tcp", "tcp_listen", "fs_read", "fs_write", "exec", "env"
// resource: host, path, command, var name
// Returns: 1 = allowed, 0 = denied
typedef int (*aether_sandbox_check_fn)(const char* category, const char* resource);

// Global: set by compiler-generated code when inside a sandbox block
extern aether_sandbox_check_fn _aether_sandbox_checker;

// The checker, with an fs path resolved to where it leads first (".." and
// symlinks), so a grant for "/box/*" cannot be left through "/box/../x" or a
// symlink in /box; an fs path that cannot be resolved is refused.
int aether_sandbox_check_slow(const char* category, const char* resource);

// A grant pattern as the checks compare it: an fs pattern's directory
// resolved the same way; malloc'd.
char* aether_sandbox_grant_pattern(const char* category, const char* pattern);

// Check if an operation is allowed (returns 1 if no sandbox or if permitted)
static inline int aether_sandbox_check(const char* category, const char* resource) {
    if (!_aether_sandbox_checker) return 1;  // no sandbox = allow all
    return aether_sandbox_check_slow(category, resource);
}

#else

// No sandbox compiled in — always allow. The arguments are evaluated for
// their side effects only, so a resource built in a local buffer
// (aether_net_resource) does not leave that buffer unused.
#define aether_sandbox_check(category, resource) ((void)(category), (void)(resource), 1)

#endif // AETHER_HAS_SANDBOX

#endif // AETHER_SANDBOX_H
