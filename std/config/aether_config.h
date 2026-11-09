#ifndef AETHER_CONFIG_H
#define AETHER_CONFIG_H

#include <stddef.h>

/* std.config — process-global immutable string→string KV store.
 *
 * Models BEAM's persistent_term: set during program initialisation,
 * read from anywhere for the rest of the process's lifetime. The
 * canonical use case is CLI flags / config-file values that get
 * parsed once in main() and need to be reachable from request
 * handlers entered later (notably from C-callback handlers where
 * Aether-typed parameters can't flow through).
 *
 * Storage: a single process-global hashmap protected by a pthread
 * rwlock. Reads (the hot path) take the read lock; put / clear take
 * the write lock. The pattern Nico cited in the design call:
 * "set during init, read everywhere".
 *
 * Values are duplicated on put so the caller's string lifetime
 * doesn't matter. Values returned from get / get_or are the
 * caller's own references (counted AetherStrings), so they stay
 * valid across a later put or clear of the same key on any thread.
 *
 * Thread-safe. NUL-terminated values only — for binary content,
 * encode (base64, hex) or use a different mechanism. */

/* Insert / overwrite. Both `key` and `value` are duplicated
 * internally. NULL key or NULL value is a no-op. */
void aether_config_store_put(const char* key, const char* value);

/* Retrieve. Returns the empty string "" if the key isn't set
 * (matches Aether's Go-style "" = absent convention) — never
 * returns NULL. The result is an owned, counted AetherString (the
 * `@heap` return contract): a reference of the caller's own, taken
 * under the read lock, so a put / clear of the same key on any
 * thread cannot free it. Release it with string_release. */
const char* aether_config_store_get(const char* key);

/* Retrieve with a fallback. Returns the registered value if set,
 * otherwise a copy of the caller's `default_value`. NULL default is
 * treated as "". Owned, as aether_config_store_get's result is. */
const char* aether_config_store_get_or(const char* key, const char* default_value);

/* Membership test. 1 if key has been put, 0 otherwise. */
int aether_config_store_has(const char* key);

/* Number of keys currently registered. Mostly for tests / debug. */
int aether_config_store_size(void);

/* Wipe all keys. Tests use this for isolation; production code
 * almost never needs it. */
void aether_config_store_clear(void);

#endif /* AETHER_CONFIG_H */
