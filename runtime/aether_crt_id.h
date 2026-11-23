/* Which C runtime a C compiler builds against, by the macros its C library's
 * own headers define (#3813). A static archive is compiled against one C
 * runtime's headers and imports that runtime's symbols: MinGW GCC's msvcrt
 * headers declare _setjmp as an msvcrt import, which a UCRT link (zig cc's)
 * cannot resolve, and the objects of two C runtimes in one program disagree
 * on FILE, errno and the heap besides.
 *
 * Read by two compiles and no program: `make` preprocesses it with the
 * compiler that builds libaether.a and writes AETHER_CRT_NAME to the first
 * line of libaether.crt beside the archive, and `mingw-w64 <major>` to the
 * second where there is one; `ae` compiles it with the build's C compiler,
 * asking whether AETHER_CRT_IS_<that name> is defined (and the same mingw-w64
 * major), and links the archive only when it is (tools/ae.c,
 * archive_crt_check). The name stays alone on its line, so an ae that reads
 * only the first line compares the runtime as before. A platform with one
 * C runtime defines no name, and its archive carries no record.
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

/* On Windows the import libraries come from mingw-w64, whose major versions
 * differ in what they import: an archive MSYS2's mingw-w64 15 built imported
 * _strtof_l from msvcrt, which mingw-w64 12's libmsvcrt.a (WinLibs GCC 14.2)
 * does not provide, so the same C runtime by name still failed to link
 * (the runtime no longer calls it on msvcrt, #3818).
 * Recorded beside the runtime's name, and compared with it. */
#if defined(__MINGW64_VERSION_MAJOR)
#  define AETHER_CRT_MINGW_MAJOR __MINGW64_VERSION_MAJOR
#endif

#endif /* AETHER_CRT_ID_H */
