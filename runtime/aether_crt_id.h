/* Which C runtime a C compiler builds against, by the macros its C library's
 * own headers define (#3813). A static archive is compiled against one C
 * runtime's headers and imports that runtime's symbols: MinGW GCC's msvcrt
 * headers declare _setjmp as an msvcrt import, which a UCRT link (zig cc's)
 * cannot resolve, and the objects of two C runtimes in one program disagree
 * on FILE, errno and the heap besides.
 *
 * Read by two compiles and no program: `make` preprocesses it with the
 * compiler that builds libaether.a and writes AETHER_CRT_NAME to
 * libaether.crt beside the archive; `ae` compiles it with the build's C
 * compiler, asking whether AETHER_CRT_IS_<that name> is defined, and links
 * the archive only when it is (tools/ae.c, archive_crt_check). A platform
 * with one C runtime defines no name, and its archive carries no record.
 *
 * Order matters where one library defines another's macro: uClibc defines
 * __GLIBC__ as well as __UCLIBC__, so it is asked first. Bionic (Android)
 * defines __BIONIC__; musl defines no macro of its own, so it is what is
 * left on Linux. */
#ifndef AETHER_CRT_ID_H
#define AETHER_CRT_ID_H

#include <stdio.h>   /* the C library's headers define what tells it apart */

#if defined(_UCRT) || (defined(_MSC_VER) && !defined(__MINGW32__))
#  define AETHER_CRT_NAME ucrt
#  define AETHER_CRT_IS_ucrt 1
#elif defined(__MSVCRT__)
#  define AETHER_CRT_NAME msvcrt
#  define AETHER_CRT_IS_msvcrt 1
#elif defined(__BIONIC__)
#  define AETHER_CRT_NAME bionic
#  define AETHER_CRT_IS_bionic 1
#elif defined(__UCLIBC__)
#  define AETHER_CRT_NAME uclibc
#  define AETHER_CRT_IS_uclibc 1
#elif defined(__GLIBC__)
#  define AETHER_CRT_NAME glibc
#  define AETHER_CRT_IS_glibc 1
#elif defined(__linux__)
#  define AETHER_CRT_NAME musl
#  define AETHER_CRT_IS_musl 1
#endif

#endif /* AETHER_CRT_ID_H */
