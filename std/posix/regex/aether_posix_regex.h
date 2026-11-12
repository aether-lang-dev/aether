/*
 * std.posix.regex — the C interface of the vendored FreeBSD regex engine.
 *
 * Every name is prefixed (aether_posix_*) so the engine cannot clash with a
 * host libc's regcomp/regexec when libaether.a is linked into a bigger
 * program. The vendored sources (freebsd/) include this header in place of
 * <regex.h>; when AETHER_POSIX_REGEX_INTERNAL is defined (only by the four
 * aether_posix_reg*.c translation units), the classic names are mapped onto
 * the prefixed ones so those sources compile with few edits.
 *
 * The flag and error values are FreeBSD's <regex.h> values.
 */
#ifndef AETHER_POSIX_REGEX_H
#define AETHER_POSIX_REGEX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int64_t aether_posix_regoff_t;

typedef struct {
    int re_magic;
    size_t re_nsub;          /* number of parenthesized subexpressions */
    const char* re_endp;     /* end pointer for REG_PEND */
    struct re_guts* re_g;    /* none of your business :-) */
} aether_posix_regex_t;

typedef struct {
    aether_posix_regoff_t rm_so; /* start of match */
    aether_posix_regoff_t rm_eo; /* end of match */
} aether_posix_regmatch_t;

/* regcomp() flags */
#define AETHER_POSIX_REG_BASIC    0000
#define AETHER_POSIX_REG_EXTENDED 0001
#define AETHER_POSIX_REG_ICASE    0002
#define AETHER_POSIX_REG_NOSUB    0004
#define AETHER_POSIX_REG_NEWLINE  0010
#define AETHER_POSIX_REG_NOSPEC   0020
#define AETHER_POSIX_REG_PEND     0040
#define AETHER_POSIX_REG_DUMP     0200

/* regerror() flags */
#define AETHER_POSIX_REG_ENOSYS   (-1)
#define AETHER_POSIX_REG_NOMATCH  1
#define AETHER_POSIX_REG_BADPAT   2
#define AETHER_POSIX_REG_ECOLLATE 3
#define AETHER_POSIX_REG_ECTYPE   4
#define AETHER_POSIX_REG_EESCAPE  5
#define AETHER_POSIX_REG_ESUBREG  6
#define AETHER_POSIX_REG_EBRACK   7
#define AETHER_POSIX_REG_EPAREN   8
#define AETHER_POSIX_REG_EBRACE   9
#define AETHER_POSIX_REG_BADBR    10
#define AETHER_POSIX_REG_ERANGE   11
#define AETHER_POSIX_REG_ESPACE   12
#define AETHER_POSIX_REG_BADRPT   13
#define AETHER_POSIX_REG_EMPTY    14
#define AETHER_POSIX_REG_ASSERT   15
#define AETHER_POSIX_REG_INVARG   16
#define AETHER_POSIX_REG_ILLSEQ   17
#define AETHER_POSIX_REG_ATOI     255 /* convert name to number (!) */
#define AETHER_POSIX_REG_ITOA     0400 /* convert number to name (!) */

/* regexec() flags */
#define AETHER_POSIX_REG_NOTBOL   00001
#define AETHER_POSIX_REG_NOTEOL   00002
#define AETHER_POSIX_REG_STARTEND 00004
#define AETHER_POSIX_REG_TRACE    00400 /* tracing of execution */
#define AETHER_POSIX_REG_LARGE    01000 /* force large representation */
#define AETHER_POSIX_REG_BACKR    02000 /* force use of backref code */

int aether_posix_regcomp(aether_posix_regex_t* preg, const char* pattern, int cflags);
size_t aether_posix_regerror(int errcode, const aether_posix_regex_t* preg,
                             char* errbuf, size_t errbuf_size);
int aether_posix_regexec(const aether_posix_regex_t* preg, const char* string,
                         size_t nmatch, aether_posix_regmatch_t pmatch[], int eflags);
void aether_posix_regfree(aether_posix_regex_t* preg);

#ifdef AETHER_POSIX_REGEX_INTERNAL
/* Map the names the FreeBSD sources use onto the prefixed ones. */
#define regoff_t   aether_posix_regoff_t
#define regex_t    aether_posix_regex_t
#define regmatch_t aether_posix_regmatch_t
#define regcomp    aether_posix_regcomp
#define regerror   aether_posix_regerror
#define regexec    aether_posix_regexec
#define regfree    aether_posix_regfree
#define REG_BASIC    AETHER_POSIX_REG_BASIC
#define REG_EXTENDED AETHER_POSIX_REG_EXTENDED
#define REG_ICASE    AETHER_POSIX_REG_ICASE
#define REG_NOSUB    AETHER_POSIX_REG_NOSUB
#define REG_NEWLINE  AETHER_POSIX_REG_NEWLINE
#define REG_NOSPEC   AETHER_POSIX_REG_NOSPEC
#define REG_PEND     AETHER_POSIX_REG_PEND
#define REG_DUMP     AETHER_POSIX_REG_DUMP
#define REG_ENOSYS   AETHER_POSIX_REG_ENOSYS
#define REG_NOMATCH  AETHER_POSIX_REG_NOMATCH
#define REG_BADPAT   AETHER_POSIX_REG_BADPAT
#define REG_ECOLLATE AETHER_POSIX_REG_ECOLLATE
#define REG_ECTYPE   AETHER_POSIX_REG_ECTYPE
#define REG_EESCAPE  AETHER_POSIX_REG_EESCAPE
#define REG_ESUBREG  AETHER_POSIX_REG_ESUBREG
#define REG_EBRACK   AETHER_POSIX_REG_EBRACK
#define REG_EPAREN   AETHER_POSIX_REG_EPAREN
#define REG_EBRACE   AETHER_POSIX_REG_EBRACE
#define REG_BADBR    AETHER_POSIX_REG_BADBR
#define REG_ERANGE   AETHER_POSIX_REG_ERANGE
#define REG_ESPACE   AETHER_POSIX_REG_ESPACE
#define REG_BADRPT   AETHER_POSIX_REG_BADRPT
#define REG_EMPTY    AETHER_POSIX_REG_EMPTY
#define REG_ASSERT   AETHER_POSIX_REG_ASSERT
#define REG_INVARG   AETHER_POSIX_REG_INVARG
#define REG_ILLSEQ   AETHER_POSIX_REG_ILLSEQ
#define REG_ATOI     AETHER_POSIX_REG_ATOI
#define REG_ITOA     AETHER_POSIX_REG_ITOA
#define REG_NOTBOL   AETHER_POSIX_REG_NOTBOL
#define REG_NOTEOL   AETHER_POSIX_REG_NOTEOL
#define REG_STARTEND AETHER_POSIX_REG_STARTEND
#define REG_TRACE    AETHER_POSIX_REG_TRACE
#define REG_LARGE    AETHER_POSIX_REG_LARGE
#define REG_BACKR    AETHER_POSIX_REG_BACKR
#endif /* AETHER_POSIX_REGEX_INTERNAL */

#ifdef __cplusplus
}
#endif

#endif /* AETHER_POSIX_REGEX_H */
