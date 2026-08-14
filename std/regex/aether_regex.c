/*
 * std.regex — Perl-compatible regular expressions via libpcre2-8.
 *
 * Detection: AETHER_HAS_PCRE2 is defined by the Makefile when
 * `pkg-config libpcre2-8` succeeds. Without it, every entry point
 * compiles to a safe stub and `regex.last_error()` returns
 * "regex: built without libpcre2-8 (install libpcre2-dev and rebuild)" —
 * so consumers of std.regex never break the build; they just see a
 * clean diagnostic at runtime.
 *
 * Handles:
 *   - RegexHandle   — opaque, holds the compiled pcre2_code*.
 *   - RegexCaptures — opaque, holds match_data + a copy of the subject
 *                     (so capture(i) is safe even after the caller frees
 *                     the original Aether string).
 *   - RegexFindAll  — opaque, holds an owned span array + subject copy.
 *
 * Strings returned from `capture` and `replace*` are AetherString refcounts
 * (marked @heap on the Aether side) — the auto-cleanup releases them.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../string/aether_string.h"
#include "../../runtime/aether_resource_caps.h"

/* Mirror the file-static helpers in aether_string.c so we accept both
 * AetherString* and plain const char* at the FFI boundary, identically
 * to every other std C wrapper. */
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

/* Thread-local last-error slot. Set on any compile/match/replace error
 * path; cleared on the next successful entry. Aether reads it via
 * `regex.last_error()`. */
#if defined(_MSC_VER)
#  define TLS __declspec(thread)
#else
#  define TLS __thread
#endif
static TLS char g_last_error[256] = "";

static void set_last_error(const char* msg) {
    snprintf(g_last_error, sizeof(g_last_error), "%s", msg ? msg : "");
}
static void clear_last_error(void) { g_last_error[0] = '\0'; }

const char* aether_regex_last_error(void) { return g_last_error; }
void aether_regex_clear_last_error(void) { clear_last_error(); }

#ifdef AETHER_HAS_PCRE2

#define PCRE2_CODE_UNIT_WIDTH 8
#ifdef AETHER_VENDOR_PCRE2
/* Vendored engine (#1389): the header ships in-tree and the code is
 * compiled into libaether.a by aether_pcre2_vendored.c, always as a
 * static library — hence PCRE2_STATIC, which on Windows stops these
 * prototypes defaulting to __declspec(dllimport) and sending the
 * linker after __imp_pcre2_* stubs that a static build doesn't have. */
#define PCRE2_STATIC
#include "pcre2/pcre2.h"
#else
#include <pcre2.h>
#endif

typedef struct {
    pcre2_code* code;
    int ngroups;   /* captures + 1 (group 0 = whole match) */
} RegexHandle;

typedef struct {
    pcre2_match_data* md;
    char* subject;       /* owned copy, NUL-terminated */
    size_t subject_len;
    int ngroups;          /* matched groups returned by pcre2_match */
} RegexCaptures;

typedef struct {
    int count;
    PCRE2_SIZE* spans;   /* 2*count entries: [s0,e0, s1,e1, ...] */
    char* subject;        /* owned copy */
    size_t subject_len;
} RegexFindAll;

static void set_pcre2_error(int errnum, PCRE2_SIZE offset) {
    PCRE2_UCHAR buf[200];
    int n = pcre2_get_error_message(errnum, buf, sizeof(buf));
    if (n < 0) snprintf((char*)buf, sizeof(buf), "pcre2 error %d", errnum);
    if (offset != (PCRE2_SIZE)-1) {
        snprintf(g_last_error, sizeof(g_last_error),
                 "regex: %s (at offset %zu)", (char*)buf, (size_t)offset);
    } else {
        snprintf(g_last_error, sizeof(g_last_error), "regex: %s", (char*)buf);
    }
}

void* aether_regex_compile(const void* pattern_s, int flags) {
    clear_last_error();
    if (!pattern_s) { set_last_error("regex: null pattern"); return NULL; }
    const char* pattern = str_data_local(pattern_s);
    size_t plen = str_len_local(pattern_s);
    int errnum;
    PCRE2_SIZE offset;
    pcre2_code* code = pcre2_compile((PCRE2_SPTR)pattern, plen,
                                      (uint32_t)flags,
                                      &errnum, &offset, NULL);
    if (!code) { set_pcre2_error(errnum, offset); return NULL; }
    uint32_t ngroups = 0;
    pcre2_pattern_info(code, PCRE2_INFO_CAPTURECOUNT, &ngroups);
    RegexHandle* h = (RegexHandle*)aether_caps_calloc(1, sizeof(RegexHandle));
    if (!h) { pcre2_code_free(code); set_last_error("regex: out of memory"); return NULL; }
    h->code = code;
    h->ngroups = (int)ngroups + 1;
    return h;
}

void aether_regex_free(void* h_) {
    if (!h_) return;
    RegexHandle* h = (RegexHandle*)h_;
    if (h->code) pcre2_code_free(h->code);
    aether_caps_free(h, sizeof(RegexHandle));
}

int aether_regex_matches(void* h_, const void* s_) {
    if (!h_ || !s_) return 0;
    RegexHandle* h = (RegexHandle*)h_;
    const char* s = str_data_local(s_);
    size_t slen = str_len_local(s_);
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(h->code, NULL);
    if (!md) return 0;
    int rc = pcre2_match(h->code, (PCRE2_SPTR)s, slen, 0, 0, md, NULL);
    pcre2_match_data_free(md);
    return rc > 0 ? 1 : 0;
}

void* aether_regex_captures(void* h_, const void* s_) {
    clear_last_error();
    if (!h_ || !s_) return NULL;
    RegexHandle* h = (RegexHandle*)h_;
    const char* s = str_data_local(s_);
    size_t slen = str_len_local(s_);
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(h->code, NULL);
    if (!md) { set_last_error("regex: match data alloc failed"); return NULL; }
    int rc = pcre2_match(h->code, (PCRE2_SPTR)s, slen, 0, 0, md, NULL);
    if (rc < 0) {
        if (rc != PCRE2_ERROR_NOMATCH) set_pcre2_error(rc, (PCRE2_SIZE)-1);
        pcre2_match_data_free(md);
        return NULL;  /* null + empty last_error = no match */
    }
    char* copy = (char*)malloc(slen + 1);
    if (!copy) { pcre2_match_data_free(md); set_last_error("regex: out of memory"); return NULL; }
    memcpy(copy, s, slen); copy[slen] = '\0';
    RegexCaptures* c = (RegexCaptures*)aether_caps_calloc(1, sizeof(RegexCaptures));
    if (!c) { free(copy); pcre2_match_data_free(md); set_last_error("regex: out of memory"); return NULL; }
    c->md = md;
    c->subject = copy;
    c->subject_len = slen;
    c->ngroups = rc;
    return c;
}

int aether_regex_captures_count(void* c_) {
    return c_ ? ((RegexCaptures*)c_)->ngroups : 0;
}

/* Returns 1 + writes span if valid; 0 if OOB or PCRE2_UNSET. */
static int caps_span(RegexCaptures* c, int index, PCRE2_SIZE* os, PCRE2_SIZE* oe) {
    if (!c || index < 0 || index >= c->ngroups) return 0;
    PCRE2_SIZE* ov = pcre2_get_ovector_pointer(c->md);
    PCRE2_SIZE s = ov[2*index], e = ov[2*index + 1];
    if (s == PCRE2_UNSET || e == PCRE2_UNSET) return 0;
    *os = s; *oe = e;
    return 1;
}

const char* aether_regex_captures_get(void* c_, int index) {
    PCRE2_SIZE s, e;
    if (!caps_span((RegexCaptures*)c_, index, &s, &e))
        return (const char*)string_new_with_length("", 0);
    RegexCaptures* c = (RegexCaptures*)c_;
    return (const char*)string_new_with_length(c->subject + s, (size_t)(e - s));
}

int aether_regex_captures_start(void* c_, int index) {
    PCRE2_SIZE s, e;
    if (!caps_span((RegexCaptures*)c_, index, &s, &e)) return -1;
    return (int)s;
}
int aether_regex_captures_end(void* c_, int index) {
    PCRE2_SIZE s, e;
    if (!caps_span((RegexCaptures*)c_, index, &s, &e)) return -1;
    return (int)e;
}

void aether_regex_captures_free(void* c_) {
    if (!c_) return;
    RegexCaptures* c = (RegexCaptures*)c_;
    if (c->md) pcre2_match_data_free(c->md);
    if (c->subject) free(c->subject);
    aether_caps_free(c, sizeof(RegexCaptures));
}

void* aether_regex_find_all(void* h_, const void* s_) {
    clear_last_error();
    if (!h_ || !s_) return NULL;
    RegexHandle* h = (RegexHandle*)h_;
    const char* s = str_data_local(s_);
    size_t slen = str_len_local(s_);
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(h->code, NULL);
    if (!md) { set_last_error("regex: match data alloc failed"); return NULL; }

    int cap = 16, count = 0;
    PCRE2_SIZE* spans = (PCRE2_SIZE*)malloc(sizeof(PCRE2_SIZE) * 2 * cap);
    if (!spans) { pcre2_match_data_free(md); set_last_error("regex: out of memory"); return NULL; }

    PCRE2_SIZE pos = 0;
    while (pos <= slen) {
        int rc = pcre2_match(h->code, (PCRE2_SPTR)s, slen, pos, 0, md, NULL);
        if (rc < 0) {
            if (rc != PCRE2_ERROR_NOMATCH) set_pcre2_error(rc, (PCRE2_SIZE)-1);
            break;
        }
        PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
        PCRE2_SIZE ms = ov[0], me = ov[1];
        if (count == cap) {
            cap *= 2;
            PCRE2_SIZE* ns = (PCRE2_SIZE*)realloc(spans, sizeof(PCRE2_SIZE) * 2 * cap);
            if (!ns) { free(spans); pcre2_match_data_free(md); set_last_error("regex: out of memory"); return NULL; }
            spans = ns;
        }
        spans[2*count] = ms;
        spans[2*count + 1] = me;
        count++;
        /* Empty match? advance one byte to avoid an infinite loop. */
        pos = (me == ms) ? me + 1 : me;
    }
    pcre2_match_data_free(md);

    char* copy = (char*)malloc(slen + 1);
    if (!copy) { free(spans); set_last_error("regex: out of memory"); return NULL; }
    memcpy(copy, s, slen); copy[slen] = '\0';

    RegexFindAll* fa = (RegexFindAll*)aether_caps_calloc(1, sizeof(RegexFindAll));
    if (!fa) { free(copy); free(spans); set_last_error("regex: out of memory"); return NULL; }
    fa->count = count;
    fa->spans = spans;
    fa->subject = copy;
    fa->subject_len = slen;
    return fa;
}

int aether_regex_find_all_count(void* fa_) {
    return fa_ ? ((RegexFindAll*)fa_)->count : 0;
}
int aether_regex_find_all_start(void* fa_, int i) {
    RegexFindAll* fa = (RegexFindAll*)fa_;
    if (!fa || i < 0 || i >= fa->count) return -1;
    return (int)fa->spans[2*i];
}
int aether_regex_find_all_end(void* fa_, int i) {
    RegexFindAll* fa = (RegexFindAll*)fa_;
    if (!fa || i < 0 || i >= fa->count) return -1;
    return (int)fa->spans[2*i + 1];
}
void aether_regex_find_all_free(void* fa_) {
    if (!fa_) return;
    RegexFindAll* fa = (RegexFindAll*)fa_;
    if (fa->spans) free(fa->spans);
    if (fa->subject) free(fa->subject);
    aether_caps_free(fa, sizeof(RegexFindAll));
}

/* Internal substitute: shared by replace and replace_all (option toggles
 * PCRE2_SUBSTITUTE_GLOBAL). PCRE2_SUBSTITUTE_EXTENDED enables the
 * familiar $1, $2, ${name} replacement syntax. */
static const char* regex_substitute(void* h_, const void* s_, const void* repl_,
                                    uint32_t extra_options) {
    clear_last_error();
    if (!h_ || !s_) return (const char*)string_new_with_length("", 0);
    RegexHandle* h = (RegexHandle*)h_;
    const char* s = str_data_local(s_);
    size_t slen = str_len_local(s_);
    const char* repl = repl_ ? str_data_local(repl_) : "";
    size_t rlen = repl_ ? str_len_local(repl_) : 0;

    PCRE2_SIZE outlen = slen + rlen + 64;
    PCRE2_UCHAR* out = (PCRE2_UCHAR*)malloc(outlen);
    if (!out) { set_last_error("regex: out of memory"); return (const char*)string_new_with_length("", 0); }
    uint32_t opts = PCRE2_SUBSTITUTE_EXTENDED | extra_options;
    int rc = pcre2_substitute(h->code, (PCRE2_SPTR)s, slen, 0, opts,
                               NULL, NULL,
                               (PCRE2_SPTR)repl, rlen, out, &outlen);
    if (rc == PCRE2_ERROR_NOMEMORY) {
        /* PCRE2 wrote the required size into outlen — grow + retry. */
        PCRE2_UCHAR* grown = (PCRE2_UCHAR*)realloc(out, outlen);
        if (!grown) { free(out); set_last_error("regex: out of memory"); return (const char*)string_new_with_length("", 0); }
        out = grown;
        rc = pcre2_substitute(h->code, (PCRE2_SPTR)s, slen, 0, opts,
                               NULL, NULL,
                               (PCRE2_SPTR)repl, rlen, out, &outlen);
    }
    if (rc < 0) {
        set_pcre2_error(rc, (PCRE2_SIZE)-1);
        free(out);
        return (const char*)string_new_with_length("", 0);
    }
    const char* result = (const char*)string_new_with_length((char*)out, (size_t)outlen);
    free(out);
    return result;
}

const char* aether_regex_replace(void* h_, const void* s_, const void* repl_) {
    return regex_substitute(h_, s_, repl_, 0);
}
const char* aether_regex_replace_all(void* h_, const void* s_, const void* repl_) {
    return regex_substitute(h_, s_, repl_, PCRE2_SUBSTITUTE_GLOBAL);
}

#else /* !AETHER_HAS_PCRE2 — stub mode: every call returns a safe sentinel. */

static void set_unavailable(void) {
    set_last_error("regex: built without libpcre2-8 (install libpcre2-dev and rebuild)");
}

void* aether_regex_compile(const void* p, int f) { (void)p; (void)f; set_unavailable(); return NULL; }
void  aether_regex_free(void* h) { (void)h; }
int   aether_regex_matches(void* h, const void* s) { (void)h; (void)s; return 0; }

void* aether_regex_captures(void* h, const void* s) { (void)h; (void)s; set_unavailable(); return NULL; }
int   aether_regex_captures_count(void* c) { (void)c; return 0; }
const char* aether_regex_captures_get(void* c, int i) { (void)c; (void)i; return (const char*)string_new_with_length("", 0); }
int   aether_regex_captures_start(void* c, int i) { (void)c; (void)i; return -1; }
int   aether_regex_captures_end(void* c, int i) { (void)c; (void)i; return -1; }
void  aether_regex_captures_free(void* c) { (void)c; }

void* aether_regex_find_all(void* h, const void* s) { (void)h; (void)s; set_unavailable(); return NULL; }
int   aether_regex_find_all_count(void* fa) { (void)fa; return 0; }
int   aether_regex_find_all_start(void* fa, int i) { (void)fa; (void)i; return -1; }
int   aether_regex_find_all_end(void* fa, int i) { (void)fa; (void)i; return -1; }
void  aether_regex_find_all_free(void* fa) { (void)fa; }

const char* aether_regex_replace(void* h, const void* s, const void* r) {
    (void)h; (void)s; (void)r; set_unavailable();
    return (const char*)string_new_with_length("", 0);
}
const char* aether_regex_replace_all(void* h, const void* s, const void* r) {
    (void)h; (void)s; (void)r; set_unavailable();
    return (const char*)string_new_with_length("", 0);
}

#endif /* AETHER_HAS_PCRE2 */
