/* The toolchain's AddressSanitizer defaults (#3539).
 *
 * `make test-asan-ae` builds aetherc, ae and every program the sweep makes
 * with AddressSanitizer, and stops a program at its first memory error and
 * at any block it never freed. What that sweep checks for leaks is the
 * programs. aetherc's own leaks are gated a program shape at a time by
 * tests/scripts/check_compiler_leaks.sh (#1667), and ae keeps some strings
 * for the life of the process by design. So the two run with leak detection
 * off unless ASAN_OPTIONS asks for it, as check_compiler_leaks.sh does;
 * their memory errors still stop them. Included by exactly one file of each
 * binary (aetherc.c, ae.c), never by the runtime a program links: a program
 * keeps the platform's default, which reports leaks on Linux. */
#ifndef AETHER_TOOLCHAIN_ASAN_H
#define AETHER_TOOLCHAIN_ASAN_H

#if defined(__SANITIZE_ADDRESS__)
#  define AETHER_TOOLCHAIN_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define AETHER_TOOLCHAIN_ASAN 1
#  endif
#endif

#ifdef AETHER_TOOLCHAIN_ASAN
const char* __asan_default_options(void);
const char* __asan_default_options(void) { return "detect_leaks=0"; }
#endif

#endif /* AETHER_TOOLCHAIN_ASAN_H */
