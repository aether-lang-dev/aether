/* Build-time symbols: `aetherc -D NAME`, `ae build -D NAME`, or aether.toml's
 * `[build] defines`.
 *
 * A symbol is present or absent, nothing more. `when defined(NAME) { … }`
 * tests one, and a false region is DROPPED FROM THE AST rather than wrapped in
 * a preprocessor `#if`. That is the point of the feature (#1527): a subsystem
 * an application does not enable should not be in its binary at all, and a
 * region that only reaches the C compiler behind an `#ifdef` still has to
 * parse, type-check and resolve every name it mentions. Dropping it means the
 * excluded code needs nothing it references to exist.
 */

#ifndef AETHER_DEFINES_H
#define AETHER_DEFINES_H

/* 1 when NAME could be written inside `defined(...)`: an identifier, so
 * letters, digits and underscore, not starting with a digit. */
int aether_define_is_valid_name(const char* name);

/* Records NAME as defined. Repeats are harmless. Returns 0 when the name is
 * empty or the table is full. */
int aether_define_add(const char* name);

/* 1 when NAME was defined for this build. */
int aether_define_is_set(const char* name);

/* Every defined name, for diagnostics that list what the build has. */
int         aether_define_count(void);
const char* aether_define_at(int index);

/* Drops every symbol. Tests and the LSP reuse one process across builds. */
void aether_defines_clear(void);

#endif /* AETHER_DEFINES_H */
