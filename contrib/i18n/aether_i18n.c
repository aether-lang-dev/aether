/* aether_i18n.c — collation engine for contrib/i18n.
 *
 * Implements a practical subset of the Unicode Collation Algorithm (UTS #10)
 * over the vendored DUCET table (ducet_data.c) and utf8proc for normalization:
 *
 *   1. Normalize the input UTF-8 string to NFD (canonical decomposition) via
 *      utf8proc — so "e" + combining-acute sorts the same as precomposed "é".
 *   2. Walk the NFD code points, emitting collation elements (CEs). A length-2
 *      contraction (e.g. some Latin/Cyrillic digraphs) is matched greedily
 *      against the DUCET contraction table; otherwise a per-code-point lookup
 *      (binary search) is used. Code points absent from the table get an
 *      implicit primary derived from the code point (UCA's implicit-weight
 *      rule, approximated) so unlisted CJK/others still order deterministically.
 *   3. Build a multi-level sort key: all primary weights, a 0x0000 level
 *      separator, all non-zero secondary weights, a separator, all non-zero
 *      tertiary weights. A zero weight at a level is skipped (UCA "ignorable").
 *   4. compare() is a byte compare of the two sort keys.
 *
 * Scope / honesty: this is DUCET-based (language-neutral) collation with
 * canonical normalization and full three-level weighting. It is NOT per-locale
 * *tailored* collation (e.g. Swedish å/ä/ö after z, or Spanish traditional
 * ñ): the `locale` argument is accepted for API stability and future tailoring
 * but currently only selects DUCET ordering. This already gives correct
 * accent- and case-aware ordering for the large majority of scripts.
 *
 * Memory: sort_key returns a malloc'd, NUL-terminated byte string that the
 * Aether side owns and frees. compare allocates two keys internally and frees
 * them before returning.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "utf8proc/utf8proc.h"
#include "ducet/ducet.h"
#include "aether_string.h" /* string_new_with_length — return managed strings */

/* ---- DUCET lookup ------------------------------------------------------- */

/* Binary search the single-code-point table. Returns index or -1. */
static int ducet_find_single(uint32_t cp) {
    int lo = 0, hi = aether_ducet_single_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint32_t v = aether_ducet_single[mid].cp;
        if (v == cp) return mid;
        if (v < cp) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

/* Binary search the length-2 contraction table on (cp0,cp1). Returns idx/-1. */
static int ducet_find_contract(uint32_t cp0, uint32_t cp1) {
    int lo = 0, hi = aether_ducet_contract_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint32_t a = aether_ducet_contract[mid].cp0;
        uint32_t b = aether_ducet_contract[mid].cp1;
        if (a == cp0 && b == cp1) return mid;
        if (a < cp0 || (a == cp0 && b < cp1)) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

/* ---- collation-element buffer ------------------------------------------- */

typedef struct {
    uint16_t *w;   /* flat p,s,t,p,s,t,... */
    size_t n;      /* number of CEs */
    size_t cap;    /* capacity in CEs */
} CEBuf;

static int cebuf_push(CEBuf *b, uint16_t p, uint16_t s, uint16_t t) {
    if (b->n == b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64;
        uint16_t *nw = (uint16_t *)realloc(b->w, nc * 3 * sizeof(uint16_t));
        if (!nw) return 0;
        b->w = nw;
        b->cap = nc;
    }
    b->w[b->n * 3 + 0] = p;
    b->w[b->n * 3 + 1] = s;
    b->w[b->n * 3 + 2] = t;
    b->n++;
    return 1;
}

/* Push the DUCET CEs at [off, off+len). */
static int cebuf_push_ducet(CEBuf *b, uint32_t off, uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        const AetherCE *ce = &aether_ducet_ce[off + i];
        if (!cebuf_push(b, ce->p, ce->s, ce->t)) return 0;
    }
    return 1;
}

/* Implicit weight for a code point not in the table (UCA approximation):
 * a synthetic primary derived from the code point, ordered after all real
 * primaries, with default secondary/tertiary. Keeps ordering deterministic. */
static int cebuf_push_implicit(CEBuf *b, uint32_t cp) {
    /* Fold into 16 bits but keep monotonic-ish: high nibble marks it as
     * beyond the DUCET primary range (0x200..0x5F0E). We use 0xE000+ so these
     * always sort after assigned primaries. */
    uint16_t p = (uint16_t)(0xE000u | (cp & 0x1FFFu));
    return cebuf_push(b, p, 0x0020, 0x0002);
}

/* Decompose `input` (UTF-8) to NFD code points. On success returns the count
 * and sets *out to a malloc'd int32 array (caller frees); returns -1 on error. */
static utf8proc_ssize_t to_nfd(const char *input, utf8proc_int32_t **out) {
    if (!input) { *out = NULL; return 0; }
    utf8proc_ssize_t inlen = (utf8proc_ssize_t)strlen(input);
    if (inlen == 0) { *out = NULL; return 0; }
    /* First pass: required length. */
    utf8proc_ssize_t need = utf8proc_decompose(
        (const utf8proc_uint8_t *)input, inlen, NULL, 0,
        (utf8proc_option_t)(UTF8PROC_DECOMPOSE | UTF8PROC_STABLE));
    if (need < 0) return -1;
    utf8proc_int32_t *buf =
        (utf8proc_int32_t *)malloc((size_t)(need > 0 ? need : 1) * sizeof(utf8proc_int32_t));
    if (!buf) return -1;
    utf8proc_ssize_t got = utf8proc_decompose(
        (const utf8proc_uint8_t *)input, inlen, buf, need,
        (utf8proc_option_t)(UTF8PROC_DECOMPOSE | UTF8PROC_STABLE));
    if (got < 0) { free(buf); return -1; }
    *out = buf;
    return got;
}

/* Build the CE buffer for a string. Returns 1 on success. */
static int collation_elements(const char *input, CEBuf *b) {
    utf8proc_int32_t *cps = NULL;
    utf8proc_ssize_t n = to_nfd(input, &cps);
    if (n < 0) return 0;
    b->w = NULL; b->n = 0; b->cap = 0;
    utf8proc_ssize_t i = 0;
    while (i < n) {
        uint32_t cp0 = (uint32_t)cps[i];
        /* try a length-2 contraction first */
        if (i + 1 < n) {
            int ci = ducet_find_contract(cp0, (uint32_t)cps[i + 1]);
            if (ci >= 0) {
                if (!cebuf_push_ducet(b, aether_ducet_contract[ci].ce_off,
                                      aether_ducet_contract[ci].ce_len)) {
                    free(cps); free(b->w); return 0;
                }
                i += 2;
                continue;
            }
        }
        int si = ducet_find_single(cp0);
        if (si >= 0) {
            if (!cebuf_push_ducet(b, aether_ducet_single[si].ce_off,
                                  aether_ducet_single[si].ce_len)) {
                free(cps); free(b->w); return 0;
            }
        } else {
            if (!cebuf_push_implicit(b, cp0)) { free(cps); free(b->w); return 0; }
        }
        i += 1;
    }
    free(cps);
    return 1;
}

/* ---- sort key ----------------------------------------------------------- */

/* Build a UCA multi-level sort key as a NUL-terminated byte string. Each 16-bit
 * weight is emitted big-endian; a 0x0000 separator divides the primary,
 * secondary, and tertiary levels. Zero weights at a level are skipped
 * (ignorables). The result never contains an interior 0x00 byte from a real
 * weight run *except* the two level separators — but callers must treat it as a
 * length-bearing byte string, not a C string, for comparison; we also append a
 * final NUL so it is safe to store/print. compare() uses the true byte length.
 *
 * Returns malloc'd bytes and sets *out_len to the byte length (excluding the
 * trailing NUL). Caller frees. Returns NULL on error. */
static uint8_t *build_sort_key(const CEBuf *b, size_t *out_len) {
    /* worst case: 3 levels * n weights * 2 bytes, + 2 level separators of 2
     * bytes each (= 4), + trailing NUL. */
    size_t cap = b->n * 3 * 2 + 4 + 1;
    uint8_t *key = (uint8_t *)malloc(cap ? cap : 1);
    if (!key) return NULL;
    size_t k = 0;
    for (int level = 0; level < 3; level++) {
        if (level > 0) {
            key[k++] = 0x00;
            key[k++] = 0x00;
        }
        for (size_t i = 0; i < b->n; i++) {
            uint16_t w = b->w[i * 3 + level];
            if (w == 0) continue; /* ignorable at this level */
            key[k++] = (uint8_t)(w >> 8);
            key[k++] = (uint8_t)(w & 0xFF);
        }
    }
    key[k] = 0x00; /* trailing NUL for safe storage */
    *out_len = k;
    return key;
}

/* ---- public C API (called from Aether) ---------------------------------- */

/* Compare a and b under `locale`. Returns -1, 0, or 1. On allocation failure
 * falls back to a plain byte compare of the inputs so it never crashes. */
int aether_collate_compare(const char *locale, const char *a, const char *b) {
    (void)locale;
    CEBuf ba, bb;
    if (!collation_elements(a, &ba)) return strcmp(a ? a : "", b ? b : "") < 0 ? -1 : (strcmp(a?a:"", b?b:"") > 0 ? 1 : 0);
    if (!collation_elements(b, &bb)) { free(ba.w); return strcmp(a?a:"", b?b:"") < 0 ? -1 : (strcmp(a?a:"", b?b:"") > 0 ? 1 : 0); }
    size_t la = 0, lb = 0;
    uint8_t *ka = build_sort_key(&ba, &la);
    uint8_t *kb = build_sort_key(&bb, &lb);
    free(ba.w); free(bb.w);
    int result;
    if (!ka || !kb) {
        result = strcmp(a ? a : "", b ? b : "");
        result = result < 0 ? -1 : (result > 0 ? 1 : 0);
    } else {
        size_t m = la < lb ? la : lb;
        int c = memcmp(ka, kb, m);
        if (c < 0) result = -1;
        else if (c > 0) result = 1;
        else if (la < lb) result = -1;
        else if (la > lb) result = 1;
        else result = 0;
    }
    free(ka); free(kb);
    return result;
}

/* Sort key as a lowercase-hex AetherString so the Aether side can hold it as
 * an ordinary managed string: two strings' hex keys compare with plain string
 * ordering exactly as their raw keys do (hex is order-preserving over bytes),
 * and the runtime frees it like any other string (no manual free, leak-clean).
 * Returns an empty string on error. */
AetherString *aether_collate_sort_key_hex(const char *locale, const char *input) {
    (void)locale;
    CEBuf b;
    if (!collation_elements(input, &b)) return string_new_with_length("", 0);
    size_t klen = 0;
    uint8_t *key = build_sort_key(&b, &klen);
    free(b.w);
    if (!key) return string_new_with_length("", 0);
    static const char hexd[] = "0123456789abcdef";
    char *hex = (char *)malloc(klen * 2 + 1);
    if (!hex) { free(key); return string_new_with_length("", 0); }
    for (size_t i = 0; i < klen; i++) {
        hex[i * 2 + 0] = hexd[(key[i] >> 4) & 0xF];
        hex[i * 2 + 1] = hexd[key[i] & 0xF];
    }
    hex[klen * 2] = '\0';
    free(key);
    AetherString *result = string_new_with_length(hex, klen * 2);
    free(hex);
    return result;
}
