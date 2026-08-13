/*
 * std.regex — vendored PCRE2 engine, one translation unit (#1389).
 *
 * Compiles the byte-identical upstream subset in std/regex/pcre2/ (see
 * VENDOR.md there for version, checksum, and re-vendor recipe) so that
 * std.regex works with no system libpcre2-8:
 *
 *   - native builds on boxes without the library (previously std.regex
 *     silently degraded to its "built without libpcre2-8" stub — the
 *     failure mode behind the 8-round vg misdiagnosis in #1389); and
 *   - `ae build --target ...` zig-cc cross builds with no
 *     CROSSBUILD_SYSROOT, for every target zig bundles a libc for.
 *
 * Guarded by AETHER_VENDOR_PCRE2: when the build detects a system
 * libpcre2-8 the Makefile leaves the macro undefined, this file compiles
 * to an empty TU, and the system library is used exactly as before. The
 * unity build (all sources in one TU) is deliberate: it needs no include
 * paths (every #include below resolves relative to this file), no
 * per-file compile flags, and it lands in MANIFEST as a single ordinary
 * std source — so `ae`'s no-libaether.a source fallback and `ae build
 * --target` both compile it with zero special-casing. Verified clean
 * under gcc -Wall -Wextra and zig cc (clang) for linux/windows/macos.
 *
 * Feature selection (the equivalent of pcre2's configure step):
 *   PCRE2_CODE_UNIT_WIDTH 8  — std.regex is byte-oriented (matches the
 *                              system-library path in aether_regex.c)
 *   SUPPORT_UNICODE          — \d/\w/\p{...} Unicode semantics
 *   JIT: off                 — aether_regex.c never calls
 *                              pcre2_jit_compile, so the interpreted
 *                              matcher is what runs on the system-library
 *                              path too; behaviour is identical and the
 *                              architecture-specific JIT never has to
 *                              cross-compile. pcre2_jit_compile.c below
 *                              provides the API's "JIT unavailable"
 *                              stubs.
 */
#ifdef AETHER_VENDOR_PCRE2

#define HAVE_CONFIG_H
#define PCRE2_CODE_UNIT_WIDTH 8
#define PCRE2_STATIC
#define SUPPORT_UNICODE

#include "pcre2/pcre2_auto_possess.c"
#include "pcre2/pcre2_chartables.c"
#include "pcre2/pcre2_chkdint.c"
#include "pcre2/pcre2_compile.c"
#include "pcre2/pcre2_config.c"
#include "pcre2/pcre2_context.c"
#include "pcre2/pcre2_convert.c"
#include "pcre2/pcre2_dfa_match.c"
#include "pcre2/pcre2_error.c"
#include "pcre2/pcre2_extuni.c"
#include "pcre2/pcre2_find_bracket.c"
#include "pcre2/pcre2_jit_compile.c"
#include "pcre2/pcre2_maketables.c"
#include "pcre2/pcre2_match.c"
#include "pcre2/pcre2_match_data.c"
#include "pcre2/pcre2_newline.c"
#include "pcre2/pcre2_ord2utf.c"
#include "pcre2/pcre2_pattern_info.c"
#include "pcre2/pcre2_script_run.c"
#include "pcre2/pcre2_serialize.c"
#include "pcre2/pcre2_string_utils.c"
#include "pcre2/pcre2_study.c"
#include "pcre2/pcre2_substitute.c"
#include "pcre2/pcre2_substring.c"
#include "pcre2/pcre2_tables.c"
#include "pcre2/pcre2_ucd.c"
#include "pcre2/pcre2_valid_utf.c"
#include "pcre2/pcre2_xclass.c"

#else
/* System libpcre2-8 in use (or std.regex disabled): nothing to build,
 * but ISO C forbids an empty translation unit. */
typedef int aether_pcre2_vendored_unused;
#endif /* AETHER_VENDOR_PCRE2 */
