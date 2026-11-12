/*
 * std.posix.regex — POSIX regular expressions on the vendored FreeBSD engine.
 *
 * The engine is Henry Spencer's, as FreeBSD's libc carries it (freebsd/, see
 * PROVENANCE.md there), compiled by aether_posix_reg{comp,exec,error,free}.c
 * with every symbol prefixed aether_posix_*. This file holds the UTF-8 and
 * character-class functions those sources call in place of the locale-driven
 * C library (aether_posix_regex_compat.h explains why), and the handles the
 * Aether module (module.ae) works with.
 *
 * Handles, all caller-owned, no thread-local or module state:
 *   - PosixRegex — a compiled pattern, or the error code that stopped it.
 *   - PosixMatch — the submatch offsets of one exec, reusable across execs.
 *
 * Subjects and patterns are length-aware (REG_STARTEND / REG_PEND), so an
 * embedded NUL is an ordinary byte, as Aether strings allow.
 */
#include "../../unicode/utf8proc/utf8proc.h"
#include "../../string/aether_string.h"
#include "../../../runtime/aether_resource_caps.h"
#include "aether_posix_regex_compat.h"

/* ---- The C library the engine sees -------------------------------- */

/* Strict UTF-8 (RFC 3629): no overlong forms, no surrogates, nothing above
 * U+10FFFF. Returns the bytes used, 0 for a NUL, (size_t)-1 for an invalid
 * sequence and (size_t)-2 for one cut short by n — mbrtowc's contract. */
size_t aether_posix_mbrtowc(aether_posix_wint_t* pwc, const char* s, size_t n,
                            aether_posix_mbstate_t* ps) {
    (void)ps;
    if (s == NULL) return 0;
    if (n == 0) return (size_t)-2;
    const unsigned char* u = (const unsigned char*)s;
    unsigned char c = u[0];
    int32_t cp;
    size_t need;
    int32_t min;
    if (c < 0x80) {
        if (pwc) *pwc = c;
        return c == 0 ? 0 : 1;
    } else if (c >= 0xC2 && c <= 0xDF) {
        need = 2; cp = c & 0x1F; min = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
        need = 3; cp = c & 0x0F; min = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
        need = 4; cp = c & 0x07; min = 0x10000;
    } else {
        return (size_t)-1;
    }
    for (size_t i = 1; i < need; i++) {
        if (i >= n) return (size_t)-2;
        if ((u[i] & 0xC0) != 0x80) return (size_t)-1;
        cp = (cp << 6) | (u[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return (size_t)-1;
    if (pwc) *pwc = cp;
    return need;
}

size_t aether_posix_wcrtomb(char* s, aether_posix_wint_t wc, aether_posix_mbstate_t* ps) {
    (void)ps;
    unsigned char* o = (unsigned char*)s;
    if (s == NULL) return 1;
    if (wc < 0 || wc > 0x10FFFF || (wc >= 0xD800 && wc <= 0xDFFF)) return (size_t)-1;
    if (wc < 0x80) { o[0] = (unsigned char)wc; return 1; }
    if (wc < 0x800) {
        o[0] = (unsigned char)(0xC0 | (wc >> 6));
        o[1] = (unsigned char)(0x80 | (wc & 0x3F));
        return 2;
    }
    if (wc < 0x10000) {
        o[0] = (unsigned char)(0xE0 | (wc >> 12));
        o[1] = (unsigned char)(0x80 | ((wc >> 6) & 0x3F));
        o[2] = (unsigned char)(0x80 | (wc & 0x3F));
        return 3;
    }
    o[0] = (unsigned char)(0xF0 | (wc >> 18));
    o[1] = (unsigned char)(0x80 | ((wc >> 12) & 0x3F));
    o[2] = (unsigned char)(0x80 | ((wc >> 6) & 0x3F));
    o[3] = (unsigned char)(0x80 | (wc & 0x3F));
    return 4;
}

static int valid_cp(aether_posix_wint_t c) {
    return c >= 0 && c <= 0x10FFFF;
}

aether_posix_wint_t aether_posix_towlower(aether_posix_wint_t c) {
    return valid_cp(c) ? (aether_posix_wint_t)utf8proc_tolower(c) : c;
}

aether_posix_wint_t aether_posix_towupper(aether_posix_wint_t c) {
    return valid_cp(c) ? (aether_posix_wint_t)utf8proc_toupper(c) : c;
}

aether_posix_wctype_t aether_posix_wctype(const char* name) {
    static const char* const names[] = {
        "alnum", "alpha", "blank", "cntrl", "digit", "graph",
        "lower", "print", "punct", "space", "upper", "xdigit",
    };
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
        if (strcmp(name, names[i]) == 0) return i + 1;
    return 0;
}

/* The POSIX classes over Unicode general categories. ASCII keeps the C
 * locale's classes exactly (digit and xdigit stay ASCII-only, as POSIX
 * requires); beyond ASCII, alpha is the letters, space the separators
 * other than the no-break spaces, and so on, so a class means the same
 * on every host whatever its locale tables say. */
int aether_posix_iswctype(aether_posix_wint_t c, aether_posix_wctype_t t) {
    if (!valid_cp(c)) return 0;
    if (c < 0x80) {
        switch (t) {
        case AETHER_POSIX_CT_ALNUM:  return isalnum(c) != 0;
        case AETHER_POSIX_CT_ALPHA:  return isalpha(c) != 0;
        case AETHER_POSIX_CT_BLANK:  return c == ' ' || c == '\t';
        case AETHER_POSIX_CT_CNTRL:  return iscntrl(c) != 0;
        case AETHER_POSIX_CT_DIGIT:  return c >= '0' && c <= '9';
        case AETHER_POSIX_CT_GRAPH:  return isgraph(c) != 0;
        case AETHER_POSIX_CT_LOWER:  return c >= 'a' && c <= 'z';
        case AETHER_POSIX_CT_PRINT:  return isprint(c) != 0;
        case AETHER_POSIX_CT_PUNCT:  return ispunct(c) != 0;
        case AETHER_POSIX_CT_SPACE:  return c == ' ' || (c >= '\t' && c <= '\r');
        case AETHER_POSIX_CT_UPPER:  return c >= 'A' && c <= 'Z';
        case AETHER_POSIX_CT_XDIGIT: return isxdigit(c) != 0;
        default: return 0;
        }
    }
    utf8proc_category_t cat = utf8proc_category(c);
    int letter = cat >= UTF8PROC_CATEGORY_LU && cat <= UTF8PROC_CATEGORY_LO;
    int mark = cat >= UTF8PROC_CATEGORY_MN && cat <= UTF8PROC_CATEGORY_ME;
    int number = cat >= UTF8PROC_CATEGORY_ND && cat <= UTF8PROC_CATEGORY_NO;
    int punct = cat >= UTF8PROC_CATEGORY_PC && cat <= UTF8PROC_CATEGORY_SO;
    int nbsp = c == 0x00A0 || c == 0x2007 || c == 0x202F;
    int sep = cat >= UTF8PROC_CATEGORY_ZS && cat <= UTF8PROC_CATEGORY_ZP;
    int graph = letter || mark || number || punct || cat == UTF8PROC_CATEGORY_CO;
    switch (t) {
    case AETHER_POSIX_CT_ALNUM:  return letter || cat == UTF8PROC_CATEGORY_ND;
    case AETHER_POSIX_CT_ALPHA:  return letter;
    case AETHER_POSIX_CT_BLANK:  return cat == UTF8PROC_CATEGORY_ZS && !nbsp;
    case AETHER_POSIX_CT_CNTRL:  return cat == UTF8PROC_CATEGORY_CC
                                        || cat == UTF8PROC_CATEGORY_ZL
                                        || cat == UTF8PROC_CATEGORY_ZP;
    case AETHER_POSIX_CT_DIGIT:  return 0;
    case AETHER_POSIX_CT_GRAPH:  return graph;
    case AETHER_POSIX_CT_LOWER:  return cat == UTF8PROC_CATEGORY_LL;
    case AETHER_POSIX_CT_PRINT:  return graph || cat == UTF8PROC_CATEGORY_ZS;
    case AETHER_POSIX_CT_PUNCT:  return punct;
    case AETHER_POSIX_CT_SPACE:  return sep && !nbsp;
    case AETHER_POSIX_CT_UPPER:  return cat == UTF8PROC_CATEGORY_LU
                                        || cat == UTF8PROC_CATEGORY_LT;
    case AETHER_POSIX_CT_XDIGIT: return 0;
    default: return 0;
    }
}

void* aether_posix_reallocarray(void* p, size_t n, size_t size) {
    if (size != 0 && n > (size_t)-1 / size) return NULL;
    return realloc(p, n * size);
}

/* ---- Handles for module.ae ---------------------------------------- */

static inline const char* str_data_local(const void* s) {
    if (!s) return "";
    if (is_aether_string(s)) return ((const AetherString*)s)->data;
    return (const char*)s;
}
static inline size_t str_len_local(const void* s) {
    if (!s) return 0;
    if (is_aether_string(s)) return ((const AetherString*)s)->length;
    return strlen((const char*)s);
}

typedef struct {
    aether_posix_regex_t re;
    int code;   /* 0 compiled; otherwise the REG_* error, and re is unset */
    int cflags;
} PosixRegex;

typedef struct {
    int nmatch;                     /* slots in m (at least 1) */
    int count;                      /* slots the last successful exec filled */
    aether_posix_regmatch_t* m;
} PosixMatch;

const char* aether_posix_regex_errtext(int code); /* aether_posix_regerror.c */

/* Compile. NULL only when memory runs out; otherwise a handle that holds
 * either the compiled pattern or the regcomp error (compile_code). */
void* aether_posix_regex_compile(const void* pattern_s, int cflags) {
    PosixRegex* h = (PosixRegex*)aether_caps_calloc(1, sizeof(PosixRegex));
    if (!h) return NULL;
    const char* pattern = str_data_local(pattern_s);
    size_t plen = str_len_local(pattern_s);
    /* Only the documented compile flags reach regcomp; REG_PEND is ours. */
    int flags = cflags & (AETHER_POSIX_REG_EXTENDED | AETHER_POSIX_REG_ICASE
                          | AETHER_POSIX_REG_NOSUB | AETHER_POSIX_REG_NEWLINE
                          | AETHER_POSIX_REG_NOSPEC);
    h->cflags = flags;
    h->re.re_endp = pattern + plen;
    h->code = aether_posix_regcomp(&h->re, pattern, flags | AETHER_POSIX_REG_PEND);
    return h;
}

int aether_posix_regex_compile_code(void* h_) {
    PosixRegex* h = (PosixRegex*)h_;
    return h ? h->code : AETHER_POSIX_REG_ESPACE;
}

int aether_posix_regex_nsub(void* h_) {
    PosixRegex* h = (PosixRegex*)h_;
    if (!h || h->code != 0) return 0;
    return (int)h->re.re_nsub;
}

void aether_posix_regex_free(void* h_) {
    PosixRegex* h = (PosixRegex*)h_;
    if (!h) return;
    if (h->code == 0) aether_posix_regfree(&h->re);
    aether_caps_free(h, sizeof(PosixRegex));
}

/* A match buffer sized for re: the whole match plus every subexpression. */
void* aether_posix_regex_match_new(void* h_) {
    PosixRegex* h = (PosixRegex*)h_;
    int n = 1;
    if (h && h->code == 0) n = (int)h->re.re_nsub + 1;
    PosixMatch* m = (PosixMatch*)aether_caps_calloc(1, sizeof(PosixMatch));
    if (!m) return NULL;
    m->m = (aether_posix_regmatch_t*)aether_caps_calloc((size_t)n, sizeof(aether_posix_regmatch_t));
    if (!m->m) { aether_caps_free(m, sizeof(PosixMatch)); return NULL; }
    m->nmatch = n;
    m->count = 0;
    return m;
}

void aether_posix_regex_match_free(void* m_) {
    PosixMatch* m = (PosixMatch*)m_;
    if (!m) return;
    aether_caps_free(m->m, (size_t)m->nmatch * sizeof(aether_posix_regmatch_t));
    aether_caps_free(m, sizeof(PosixMatch));
}

/* Match subject[start, end) (end < 0 means the subject's length), offsets
 * relative to the start of subject, as REG_STARTEND reports them. Returns 1
 * for a match, 0 for none, and -code for an engine error. */
int aether_posix_regex_exec_into(void* h_, const void* subject_s, int start, int end,
                                 int eflags, void* m_) {
    PosixRegex* h = (PosixRegex*)h_;
    PosixMatch* m = (PosixMatch*)m_;
    if (!h || h->code != 0 || !m) return -AETHER_POSIX_REG_BADPAT;
    const char* s = str_data_local(subject_s);
    size_t slen = str_len_local(subject_s);
    if (end < 0 || (size_t)end > slen) end = (int)slen;
    if (start < 0) start = 0;
    if (start > end) return -AETHER_POSIX_REG_INVARG;
    m->count = 0;
    m->m[0].rm_so = start;
    m->m[0].rm_eo = end;
    int ef = (eflags & (AETHER_POSIX_REG_NOTBOL | AETHER_POSIX_REG_NOTEOL))
             | AETHER_POSIX_REG_STARTEND;
    int rc = aether_posix_regexec(&h->re, s, (size_t)m->nmatch, m->m, ef);
    if (rc == 0) {
        m->count = (h->cflags & AETHER_POSIX_REG_NOSUB) ? 0 : m->nmatch;
        return 1;
    }
    if (rc == AETHER_POSIX_REG_NOMATCH) return 0;
    return -rc;
}

int aether_posix_regex_match_count(void* m_) {
    PosixMatch* m = (PosixMatch*)m_;
    return m ? m->count : 0;
}

int aether_posix_regex_match_start(void* m_, int i) {
    PosixMatch* m = (PosixMatch*)m_;
    if (!m || i < 0 || i >= m->count) return -1;
    return (int)m->m[i].rm_so;
}

int aether_posix_regex_match_end(void* m_, int i) {
    PosixMatch* m = (PosixMatch*)m_;
    if (!m || i < 0 || i >= m->count) return -1;
    return (int)m->m[i].rm_eo;
}
