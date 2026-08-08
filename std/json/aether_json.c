// aether_json.c — Clean-room fast JSON parser for std.json.
//
// Design summary (see docs/json-parser-design.md for the full rationale):
//
//   * Arena-allocated per document. One bump-pointer arena holds every
//     JsonValue, every string payload, every container backing array for
//     a parsed document. json_free() releases the arena in O(chunks),
//     not O(nodes).
//
//   * Flat containers. Arrays and objects use a plain `JsonValue**`
//     (plus parallel `char**` + `uint32_t*` for object keys/lengths)
//     instead of the previous ArrayList/HashMap wrappers. Linear lookup
//     for objects — the canonical size range (<32 keys) is cache-friendly
//     enough that the branch-prediction win beats hashing.
//
//   * Character classification via a 256-entry lookup table. Every hot
//     decision ("is this whitespace? structural? safe string byte?") is
//     a single LUT read.
//
//   * Fast string scan. Non-escape, non-ASCII-control, non-" runs are
//     consumed by a tight loop with a LUT guard. Only escapes, control
//     chars, and the closing quote leave the fast loop.
//
//   * UTF-8 DFA. Bjoern Hoehrmann's public-domain state machine
//     validates multi-byte sequences in one pass with no branches per
//     byte (arXiv/Hoehrmann 2010). Rejects overlongs, surrogate halves,
//     and codepoints above U+10FFFF.
//
//   * Integer fast path. Numbers containing only digits (with an
//     optional leading minus) avoid strtod and parse via a tight
//     integer accumulator. strtod is reserved for floats.
//
//   * Position-aware errors. `json_last_error()` returns a thread-local
//     "<reason> at <line>:<col>" string. First error wins (innermost
//     failure is usually the most specific).
//
// The API in aether_json.h is preserved exactly. JsonValue is still
// opaque to callers; only the internals changed. All existing unit
// tests, regression tests, and callers continue to work unchanged.

#include "aether_json.h"
#include "../mem/aether_grow.h"
#include "../../runtime/aether_resource_caps.h"
// RFC 8259 fixes '.' as the decimal separator — number text must not follow
// the ambient LC_NUMERIC in either direction.
#include "../../runtime/aether_locale_num.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Thread-local storage portability shim
// ---------------------------------------------------------------------------

#if defined(_MSC_VER)
    #define JSON_THREAD_LOCAL __declspec(thread)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    #define JSON_THREAD_LOCAL _Thread_local
#else
    #define JSON_THREAD_LOCAL __thread
#endif

// Branch hints. Compiler-agnostic no-ops when unsupported.
#if defined(__GNUC__) || defined(__clang__)
    #define JSON_LIKELY(x)   __builtin_expect(!!(x), 1)
    #define JSON_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define JSON_LIKELY(x)   (x)
    #define JSON_UNLIKELY(x) (x)
#endif

// ---------------------------------------------------------------------------
// Error reporting
// ---------------------------------------------------------------------------

// Error buffer sized to hold reason + " at line:col" without truncation.
// JSON_ERR_REASON_BUF is the space for the reason alone; the outer
// JSON_ERROR_BUF_SIZE adds room for " at %d:%d" (max ~30 chars for
// 32-bit line/col).
#define JSON_ERR_REASON_BUF 200
#define JSON_ERROR_BUF_SIZE 256

// Issue #392 — structured-error kinds. Mirror of the KIND_*
// const block in std/json/module.ae. Update both in lock-step.
// Values are stable; appending new kinds is fine, but existing
// values are part of the surface contract.
#define AETHER_JSON_KIND_OK             0
#define AETHER_JSON_KIND_PARSE_ERROR    1   // ENOENT-of-syntax — any malformed JSON
#define AETHER_JSON_KIND_OUT_OF_MEMORY  2   // arena allocation failed during parse
#define AETHER_JSON_KIND_INVALID_INPUT  3   // NULL / empty input handed to json_parse_raw

static JSON_THREAD_LOCAL char g_json_err_buf[JSON_ERROR_BUF_SIZE];
static JSON_THREAD_LOCAL int  g_json_err_set;
static JSON_THREAD_LOCAL int  g_json_err_kind;
static JSON_THREAD_LOCAL int  g_json_err_line;
static JSON_THREAD_LOCAL int  g_json_err_col;

static void err_clear(void) {
    g_json_err_buf[0] = '\0';
    g_json_err_set = 0;
    g_json_err_kind = AETHER_JSON_KIND_OK;
    g_json_err_line = 0;
    g_json_err_col = 0;
}

const char* json_last_error(void) {
    return g_json_err_set ? g_json_err_buf : "";
}

// Issue #392 — programmatic kind / position accessors. Callers that
// want the structured-error tuple `(value, kind, message)` use the
// `parse_strict` wrapper in module.ae which combines these with
// json_last_error(). Kind constants live in module.ae as exported
// `const KIND_*` (values match the AETHER_JSON_KIND_* macros above).
int json_last_error_kind(void) { return g_json_err_set ? g_json_err_kind : AETHER_JSON_KIND_OK; }
int json_last_error_line(void) { return g_json_err_set ? g_json_err_line : 0; }
int json_last_error_col(void)  { return g_json_err_set ? g_json_err_col  : 0; }

// Every parser error goes through this. First-error-wins so the innermost
// diagnostic is preserved; outer callers that detect a NULL return don't
// overwrite the deeper reason.
//
// Kind classification is single-site: any reason text containing
// "out of memory" → KIND_OUT_OF_MEMORY; everything else →
// KIND_PARSE_ERROR. This keeps the dozens of existing call sites
// (~50 across this file) unchanged while still surfacing OOM
// distinctly to callers who switch on the kind tuple.
static void err_set(int line, int col, const char* fmt, ...) {
    if (g_json_err_set) return;
    char msg[JSON_ERR_REASON_BUF];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    // msg is capped at JSON_ERR_REASON_BUF-1, leaving room for " at N:N"
    // (max ~30 chars for 32-bit line/col) inside the outer buffer.
    snprintf(g_json_err_buf, sizeof(g_json_err_buf),
             "%s at %d:%d", msg, line, col);
    g_json_err_set = 1;
    g_json_err_line = line;
    g_json_err_col = col;
    g_json_err_kind = (strstr(msg, "out of memory") != NULL)
        ? AETHER_JSON_KIND_OUT_OF_MEMORY
        : AETHER_JSON_KIND_PARSE_ERROR;
}

// Used once from json_parse_raw when the caller hands us a NULL pointer.
static void err_set_no_pos(const char* reason) {
    if (g_json_err_set) return;
    snprintf(g_json_err_buf, sizeof(g_json_err_buf), "%s", reason);
    g_json_err_set = 1;
    g_json_err_line = 0;
    g_json_err_col = 0;
    g_json_err_kind = AETHER_JSON_KIND_INVALID_INPUT;
}

// ---------------------------------------------------------------------------
// Arena allocator
//
// Chunked bump allocator. First chunk is sized generously (16 KB) so
// tiny documents never grow the arena. Subsequent chunks double in
// size, capped at 2 MB to avoid allocating absurd amounts on the first
// big value. Allocations bigger than the current chunk's remaining
// capacity spill to a dedicated oversized chunk (no growth doubling)
// so we don't waste gigabytes aligning to a power of two.
//
// All allocations are 8-byte aligned.
// ---------------------------------------------------------------------------

typedef struct ArenaChunk {
    struct ArenaChunk* next;
    size_t cap;
    size_t used;
    // data[] follows as the remainder of the allocation.
} ArenaChunk;

typedef struct Arena {
    ArenaChunk* head;
    ArenaChunk* tail;
} Arena;

#define ARENA_ALIGN 8u
#define ARENA_INITIAL_CHUNK (16u * 1024u)
#define ARENA_MAX_AUTO_CHUNK (2u * 1024u * 1024u)

// Starting capacity for the backing arrays of arrays and objects.
// Realistic JSON objects almost always have more than 4 keys (API
// responses, log records, config nodes all cluster at 5–15); starting
// at 8 skips one doubling cycle for the typical case and wastes only
// a handful of pointers when an object really is tiny.
#define JSON_CONTAINER_INITIAL_CAP 8u

static size_t arena_align_up(size_t n) {
    return (n + (ARENA_ALIGN - 1u)) & ~((size_t)ARENA_ALIGN - 1u);
}

static ArenaChunk* arena_new_chunk(size_t cap) {
    // Allocate the header + payload in one malloc. cap is payload bytes.
    // Cap-aware (#343): JSON parser arena chunks scale with input
    // size; an untrusted document drives cumulative arena size. The
    // chunk's c->cap field stores the payload bytes so the matching
    // free recovers `sizeof(ArenaChunk) + c->cap` for the counter
    // decrement.
    ArenaChunk* c = (ArenaChunk*)aether_caps_malloc(sizeof(ArenaChunk) + cap);
    if (!c) return NULL;
    c->next = NULL;
    c->cap = cap;
    c->used = 0;
    return c;
}

static Arena* arena_create(void) {
    Arena* a = (Arena*)aether_caps_malloc(sizeof(Arena));
    if (!a) return NULL;
    a->head = arena_new_chunk(ARENA_INITIAL_CHUNK);
    if (!a->head) { aether_caps_free(a, sizeof(Arena)); return NULL; }
    a->tail = a->head;
    return a;
}

static void arena_destroy(Arena* a) {
    if (!a) return;
    ArenaChunk* c = a->head;
    while (c) {
        ArenaChunk* next = c->next;
        aether_caps_free(c, sizeof(ArenaChunk) + c->cap);
        c = next;
    }
    aether_caps_free(a, sizeof(Arena));
}

static void* arena_alloc(Arena* a, size_t n) {
    n = arena_align_up(n);
    ArenaChunk* c = a->tail;
    if (JSON_LIKELY(c->used + n <= c->cap)) {
        void* p = (char*)c + sizeof(ArenaChunk) + c->used;
        c->used += n;
        return p;
    }
    // Grow. Decide chunk size: double the previous, clamped, but never
    // smaller than the requested allocation.
    size_t next_cap = c->cap * 2u;
    if (next_cap > ARENA_MAX_AUTO_CHUNK) next_cap = ARENA_MAX_AUTO_CHUNK;
    if (next_cap < n) next_cap = n;
    ArenaChunk* nc = arena_new_chunk(next_cap);
    if (!nc) return NULL;
    a->tail->next = nc;
    a->tail = nc;
    void* p = (char*)nc + sizeof(ArenaChunk);
    nc->used = n;
    return p;
}

// Grow an arena-backed contiguous array. `old` must either be NULL (new
// allocation) or the most recent thing at the tail of the arena's current
// chunk (so we can extend in place). When in-place growth isn't possible
// a new buffer is allocated and the old data is copied. Old space leaks
// into the arena until arena_destroy — acceptable given typical doubling
// growth and short-lived arenas.
static void* arena_grow(Arena* a, void* old, size_t old_bytes, size_t new_bytes) {
    void* np = arena_alloc(a, new_bytes);
    if (!np) return NULL;
    if (old && old_bytes) memcpy(np, old, old_bytes);
    return np;
}

// Duplicate a byte range into the arena. Returns a null-terminated copy
// so callers can treat it as a C string. Returns NULL on arena exhaustion.
static char* arena_strndup(Arena* a, const char* src, size_t n) {
    char* dst = (char*)arena_alloc(a, n + 1);
    if (!dst) return NULL;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
    return dst;
}

// ---------------------------------------------------------------------------
// Character classification lookup table
//
// Eight independent flag bits per byte. Multiple predicates fold into a
// single LUT read. Initialised statically so there's no init-order cost.
// ---------------------------------------------------------------------------

enum {
    CC_WHITESPACE  = 1u << 0,  // space, tab, LF, CR
    CC_DIGIT       = 1u << 1,  // 0-9
    CC_STRUCTURAL  = 1u << 2,  // { } [ ] , :
    CC_STR_OK      = 1u << 3,  // legal unescaped byte in a JSON string
                               //   (>= 0x20, != '"', != '\\', ASCII)
    CC_HEX         = 1u << 4,  // 0-9 a-f A-F (for \uXXXX)
    CC_NUM_START   = 1u << 5   // '-' or 0-9 — can start a number
};

static const uint8_t JSON_CC[256] = {
    // Control chars 0x00-0x1F: nothing set (none are CC_STR_OK,
    //   none are whitespace except specific ones below).
    [0x09] = CC_WHITESPACE,  // \t
    [0x0A] = CC_WHITESPACE,  // \n
    [0x0D] = CC_WHITESPACE,  // \r
    [' ']  = CC_WHITESPACE,

    ['0'] = CC_DIGIT | CC_STR_OK | CC_HEX | CC_NUM_START,
    ['1'] = CC_DIGIT | CC_STR_OK | CC_HEX | CC_NUM_START,
    ['2'] = CC_DIGIT | CC_STR_OK | CC_HEX | CC_NUM_START,
    ['3'] = CC_DIGIT | CC_STR_OK | CC_HEX | CC_NUM_START,
    ['4'] = CC_DIGIT | CC_STR_OK | CC_HEX | CC_NUM_START,
    ['5'] = CC_DIGIT | CC_STR_OK | CC_HEX | CC_NUM_START,
    ['6'] = CC_DIGIT | CC_STR_OK | CC_HEX | CC_NUM_START,
    ['7'] = CC_DIGIT | CC_STR_OK | CC_HEX | CC_NUM_START,
    ['8'] = CC_DIGIT | CC_STR_OK | CC_HEX | CC_NUM_START,
    ['9'] = CC_DIGIT | CC_STR_OK | CC_HEX | CC_NUM_START,

    ['a'] = CC_STR_OK | CC_HEX, ['b'] = CC_STR_OK | CC_HEX,
    ['c'] = CC_STR_OK | CC_HEX, ['d'] = CC_STR_OK | CC_HEX,
    ['e'] = CC_STR_OK | CC_HEX, ['f'] = CC_STR_OK | CC_HEX,
    ['A'] = CC_STR_OK | CC_HEX, ['B'] = CC_STR_OK | CC_HEX,
    ['C'] = CC_STR_OK | CC_HEX, ['D'] = CC_STR_OK | CC_HEX,
    ['E'] = CC_STR_OK | CC_HEX, ['F'] = CC_STR_OK | CC_HEX,

    ['{'] = CC_STRUCTURAL, ['}'] = CC_STRUCTURAL,
    ['['] = CC_STRUCTURAL, [']'] = CC_STRUCTURAL,
    [','] = CC_STRUCTURAL, [':'] = CC_STRUCTURAL,

    ['-'] = CC_STR_OK | CC_NUM_START,
    ['+'] = CC_STR_OK,
    ['.'] = CC_STR_OK,
};

// Fill the remaining printable ASCII range (0x20..0x7E minus '"' and '\\')
// as CC_STR_OK. Done in a constructor so the static init stays readable.
// Because C89 static init can't use loops, we post-patch via a small
// table-completion function called on first parse.
//
// Multi-byte UTF-8 lead bytes (>=0x80) are NOT marked CC_STR_OK — the
// fast string scan falls through to the DFA for them, which is the
// correct behavior (validation required).
static int g_cc_inited = 0;
static uint8_t JSON_CC_RW[256];  // mutable copy used at runtime

static void cc_init(void) {
    if (g_cc_inited) return;
    memcpy(JSON_CC_RW, JSON_CC, sizeof(JSON_CC_RW));
    for (int c = 0x20; c < 0x80; c++) {
        if (c == '"' || c == '\\') continue;
        JSON_CC_RW[c] |= CC_STR_OK;
    }
    g_cc_inited = 1;
}

#define CC(c) (JSON_CC_RW[(unsigned char)(c)])

// ---------------------------------------------------------------------------
// SIMD string fast-loop
//
// Scans forward over "safe" JSON-string bytes (printable ASCII that is
// not '"' and not '\\'). Returns the offset of the first byte that
// needs special handling — quote, backslash, control char, or any
// non-ASCII byte (which falls to the UTF-8 DFA). Purely monotone: a
// byte is classified in isolation, so the scan is parallelisable.
//
// Three implementations selected at compile time. Each produces the
// same result; the scalar path is the baseline, the SIMD paths just
// advance 16 bytes at a time when every byte in the chunk is safe.
//
// Line/col accounting: safe bytes are by definition >= 0x20, so no
// newlines appear. The caller bumps col by the returned count.
// ---------------------------------------------------------------------------

// Classification predicate used by the scalar tail of every variant.
// Inlined for the branch predictor.
static inline int is_str_safe_byte(unsigned char c) {
    // c >= 0x20 AND c < 0x80 AND c != '"' AND c != '\\'.
    return c >= 0x20 && c < 0x80 && c != '"' && c != '\\';
}

#if defined(__SSE2__)
#include <emmintrin.h>
static size_t scan_str_safe(const char* p, size_t max) {
    size_t i = 0;
    // SSE2 gives us 16-byte vectors. For each block, build a per-byte
    // "unsafe" mask via three compares ORed together; if the mask is
    // zero, every byte is safe and we bulk-advance by 16.
    while (max - i >= 16) {
        __m128i v  = _mm_loadu_si128((const __m128i*)(p + i));
        __m128i q  = _mm_cmpeq_epi8(v, _mm_set1_epi8('"'));
        __m128i bs = _mm_cmpeq_epi8(v, _mm_set1_epi8('\\'));
        // byte < 0x20 (unsigned) OR byte >= 0x80:
        //   signed compare `v < 0x20` is true for 0x00-0x1F (non-negative
        //   small) AND 0x80-0xFF (signed-negative). Exactly the "unsafe
        //   low OR high" set.
        __m128i lo = _mm_cmplt_epi8(v, _mm_set1_epi8(0x20));
        __m128i bad = _mm_or_si128(_mm_or_si128(q, bs), lo);
        int mask = _mm_movemask_epi8(bad);
        if (mask == 0) { i += 16; continue; }
        return i + (size_t)__builtin_ctz((unsigned)mask);
    }
    while (i < max && is_str_safe_byte((unsigned char)p[i])) i++;
    return i;
}
#elif defined(__ARM_NEON) && defined(__aarch64__)
#include <arm_neon.h>
static size_t scan_str_safe(const char* p, size_t max) {
    size_t i = 0;
    while (max - i >= 16) {
        uint8x16_t v  = vld1q_u8((const uint8_t*)(p + i));
        uint8x16_t q  = vceqq_u8(v, vdupq_n_u8('"'));
        uint8x16_t bs = vceqq_u8(v, vdupq_n_u8('\\'));
        // Same "signed < 0x20" trick as SSE: catches 0x00-0x1F and 0x80-0xFF.
        int8x16_t  vs  = vreinterpretq_s8_u8(v);
        uint8x16_t lo  = vcltq_s8(vs, vdupq_n_s8(0x20));
        uint8x16_t bad = vorrq_u8(vorrq_u8(q, bs), lo);
        if (vmaxvq_u8(bad) == 0) { i += 16; continue; }
        // NEON lacks a direct movemask. Narrow-shift each u16 pair to a
        // nibble so each input byte contributes 4 bits to a u64; then
        // ctz the u64 and divide by 4 for the byte offset. This is the
        // canonical trick for "find first non-zero byte" on arm64.
        uint8x8_t narrowed = vshrn_n_u16(vreinterpretq_u16_u8(bad), 4);
        uint64_t m = vget_lane_u64(vreinterpret_u64_u8(narrowed), 0);
        return i + (size_t)(__builtin_ctzll(m) >> 2);
    }
    while (i < max && is_str_safe_byte((unsigned char)p[i])) i++;
    return i;
}
#else
// Portable scalar fallback. Used on WASM, embedded, older x86 without
// SSE2 (rare: SSE2 has been baseline on x86_64 since AMD64), or any
// target where neither intrinsic header is present.
static size_t scan_str_safe(const char* p, size_t max) {
    size_t i = 0;
    while (i < max && is_str_safe_byte((unsigned char)p[i])) i++;
    return i;
}
#endif

// ---------------------------------------------------------------------------
// UTF-8 DFA (Bjoern Hoehrmann, public domain)
//
// State transitions for validating UTF-8 byte-by-byte. Two 256+ tables
// drive a state machine that accepts well-formed UTF-8 sequences and
// rejects every malformed case (overlongs, surrogate halves, sequences
// past U+10FFFF, continuation bytes at the start of a sequence).
//
// Usage:
//   uint32_t st = UTF8_ACCEPT, cp = 0;
//   for (each byte) { utf8_step(&st, &cp, byte); }
//   if (st == UTF8_ACCEPT) valid.
//
// On exit:
//   UTF8_ACCEPT (0) = success; `cp` holds the last codepoint decoded.
//   UTF8_REJECT (12) = invalid sequence.
//   any other value = mid-sequence (incomplete input).
// ---------------------------------------------------------------------------

#define UTF8_ACCEPT 0
#define UTF8_REJECT 12

static const uint8_t UTF8_DFA[] = {
    // Byte class table (256 entries): maps byte → class (0..11).
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7, 7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    8,8,2,2,2,2,2,2,2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    10,3,3,3,3,3,3,3,3,3,3,3,3,4,3,3, 11,6,6,6,5,8,8,8,8,8,8,8,8,8,8,8,

    // State transition table (indexed by state*16 + class). 12 * 12 = 144,
    // padded to a nice power of two.
    0,12,24,36,60,96,84,12,12,12,48,72, 12,12,12,12,12,12,12,12,12,12,12,12,
    12, 0,12,12,12,12,12, 0,12, 0,12,12, 12,24,12,12,12,12,12,24,12,24,12,12,
    12,12,12,12,12,12,12,24,12,12,12,12, 12,24,12,12,12,12,12,12,12,24,12,12,
    12,12,12,12,12,12,12,36,12,36,12,12, 12,36,12,12,12,12,12,36,12,36,12,12,
    12,36,12,12,12,12,12,12,12,12,12,12,
};

static inline uint32_t utf8_step(uint32_t* st, uint32_t* cp, uint8_t byte) {
    uint32_t type = UTF8_DFA[byte];
    *cp = (*st != UTF8_ACCEPT)
        ? (byte & 0x3fu) | (*cp << 6u)
        : (0xffu >> type) & byte;
    *st = UTF8_DFA[256 + *st + type];
    return *st;
}

// ---------------------------------------------------------------------------
// JsonValue layout
//
// Opaque to the world; internals can change freely. Every value stores
// its type and a union of type-specific data. The arena pointer is ONLY
// valid on the root value — interior values share the root's arena.
//
// Object backing lives behind a single pointer (JsonObjBlock) rather
// than three parallel array pointers inline in the value. That makes
// the union's widest member 16 bytes — same as `str` and `arr` — so the
// whole JsonValue fits into a 32-byte footprint and two values share one
// 64-byte cache line.
// ---------------------------------------------------------------------------

enum {
    JV_FLAG_ROOT        = 0x01u,
    // Set on a JsonValue whose own struct bytes were allocated via `malloc`
    // (from `heap_new`), not carved out of an arena. These need `free(v)`
    // on top of whatever else `json_free` does. Critical for the case where
    // a heap-created container gets promoted to arena-backed via
    // `ensure_container_arena` — after that, `v->arena` is set but the
    // struct itself still sits on the heap.
    JV_FLAG_HEAP_STRUCT = 0x02u,
    // Marks a JSON_NUMBER whose value originated as an integer (via
    // json_create_int / json.from_int) — the serializer emits it as a
    // bare integer (`%lld`) instead of `%g`. Without this flag,
    // `json_create_number(53248000.0)` serialises to `5.3248e+07`,
    // which is lossy past 2^53 and wrong for consumers expecting a
    // plain integer count (e.g. byte-totals, IDs). Sourced from
    // `stdlib-json-integer-value-ask.md` (fbs-core /metrics handler).
    JV_FLAG_INTEGER     = 0x04u
};

// Parallel arrays for an object's (key, key_len, value) triples.
// Held behind one pointer from JsonValue so the value itself stays small.
// Allocated lazily on first reserve; NULL for empty objects.
typedef struct JsonObjBlock {
    const char** keys;
    uint32_t*    key_lens;
    JsonValue**  values;
} JsonObjBlock;

struct JsonValue {
    uint8_t type;
    uint8_t flags;
    uint8_t _pad[6];
    union {
        int      boolean;
        double   number;
        /* Integer storage for values constructed via json_create_int.
         * Marked by JV_FLAG_INTEGER; the serializer reads `integer`
         * (not `number`) when the flag is set. Keeps the full int64
         * range — the alternative (storing as double in the existing
         * `number` slot) loses precision past 2^53, which is exactly
         * the bug fbs-core's metrics handler tripped on. Shares the
         * union slot so JsonValue stays its 32-byte footprint. */
        long long integer;
        struct {
            const char* data;
            uint32_t    length;
        } str;
        struct {
            JsonValue** items;   // arena-allocated array of pointers
            uint32_t    count;
            uint32_t    capacity;
        } arr;
        struct {
            JsonObjBlock* blk;   // NULL when capacity == 0
            uint32_t      count;
            uint32_t      capacity;
        } obj;
    } data;
    Arena* arena;  // root only; interior values set this to NULL
};

// ---------------------------------------------------------------------------
// Parser state
// ---------------------------------------------------------------------------

#define JSON_MAX_DEPTH 256

typedef struct {
    const char* p;      // current cursor
    const char* end;    // one past last byte
    int line;           // 1-indexed
    int col;            // 1-indexed
    Arena* arena;       // where we allocate values/strings/arrays
} Parser;

static inline int p_eof(const Parser* s) {
    return s->p >= s->end;
}

static inline unsigned char p_peek(const Parser* s) {
    return p_eof(s) ? 0u : (unsigned char)*s->p;
}

static inline void p_advance(Parser* s) {
    // Caller has already checked EOF if needed.
    if (*s->p == '\n') { s->line++; s->col = 1; }
    else { s->col++; }
    s->p++;
}

static void skip_whitespace(Parser* s) {
    while (s->p < s->end && (CC(*s->p) & CC_WHITESPACE)) {
        p_advance(s);
    }
}

// ---------------------------------------------------------------------------
// JsonValue construction helpers (arena-side)
// ---------------------------------------------------------------------------

static JsonValue* jv_new_in(Arena* a, uint8_t type) {
    JsonValue* v = (JsonValue*)arena_alloc(a, sizeof(JsonValue));
    if (!v) return NULL;
    memset(v, 0, sizeof(JsonValue));
    v->type = type;
    return v;
}

static JsonValue* jv_new_null(Arena* a)   { return jv_new_in(a, JSON_NULL); }
static JsonValue* jv_new_bool(Arena* a, int b) {
    JsonValue* v = jv_new_in(a, JSON_BOOL);
    if (v) v->data.boolean = !!b;
    return v;
}
static JsonValue* jv_new_number(Arena* a, double n) {
    JsonValue* v = jv_new_in(a, JSON_NUMBER);
    if (v) v->data.number = n;
    return v;
}
static JsonValue* jv_new_integer(Arena* a, long long n) {
    JsonValue* v = jv_new_in(a, JSON_NUMBER);
    if (v) { v->data.integer = n; v->flags |= JV_FLAG_INTEGER; }
    return v;
}

// ---------------------------------------------------------------------------
// Number parser
//
// RFC 8259 grammar:
//   number = [ minus ] int [ frac ] [ exp ]
//   int    = zero / ( digit1-9 *DIGIT )
//
// Single-pass design. Accumulate the sign, integer part (int64), and
// fractional part (also int64, with digit count) as we validate the
// grammar. Then reconstruct the final double via pow10 lookup plus one
// multiply — no strtod in the hot path.
//
// Fallback: if the input has more digits than an int64 can hold without
// precision loss (19+), or the exponent is too large for an exact pow10,
// we hand off to strtod which gives correctly-rounded IEEE-754 results.
// That guarantees we never silently return a wrong double.
//
// Powers of 10 up to 1e22 are exactly representable in double; beyond
// that strtod is the only correct answer.
// ---------------------------------------------------------------------------

static const double POW10_POS[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
    1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
    1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};
#define POW10_FAST_MAX 22

// Exact representability in double means: no more than ~15.95 decimal
// digits, i.e. 15 safe + 16th sometimes correct. We cap at 18 digits
// because int64 holds 18 digits cleanly, and strtod still gets called
// for the 16-to-18-digit grey zone to guarantee correct rounding.
#define FAST_INT_SAFE_DIGITS 15

static JsonValue* parse_number(Parser* s) {
    const char* start = s->p;
    int line0 = s->line, col0 = s->col;

    int negative = 0;
    if (p_peek(s) == '-') { negative = 1; p_advance(s); }

    if (p_eof(s) || !(CC(*s->p) & CC_DIGIT)) {
        err_set(s->line, s->col, "expected digit in number");
        return NULL;
    }

    // Accumulate the integer-part digits directly. int64 holds 18 safely,
    // so we track overflow and fall through to strtod if the span is
    // longer than that.
    uint64_t int_acc = 0;
    int int_digits = 0;
    int int_overflow = 0;

    if (*s->p == '0') {
        int_acc = 0;
        int_digits = 1;
        p_advance(s);
        if (!p_eof(s) && (CC(*s->p) & CC_DIGIT)) {
            err_set(line0, col0, "leading zeros not allowed in numbers");
            return NULL;
        }
    } else {
        while (!p_eof(s) && (CC(*s->p) & CC_DIGIT)) {
            unsigned d = (unsigned)(*s->p - '0');
            if (int_acc > (UINT64_MAX - d) / 10u) int_overflow = 1;
            else int_acc = int_acc * 10u + d;
            int_digits++;
            p_advance(s);
        }
    }

    int has_decimal = 0;
    int frac_digits = 0;
    uint64_t frac_acc = 0;
    int frac_overflow = 0;

    if (!p_eof(s) && *s->p == '.') {
        has_decimal = 1;
        p_advance(s);
        if (p_eof(s) || !(CC(*s->p) & CC_DIGIT)) {
            err_set(s->line, s->col, "expected digit after decimal point");
            return NULL;
        }
        while (!p_eof(s) && (CC(*s->p) & CC_DIGIT)) {
            unsigned d = (unsigned)(*s->p - '0');
            if (frac_acc > (UINT64_MAX - d) / 10u) frac_overflow = 1;
            else frac_acc = frac_acc * 10u + d;
            frac_digits++;
            p_advance(s);
        }
    }

    int has_exponent = 0;
    int exp_sign = 1;
    int exponent = 0;
    int exp_overflow = 0;

    if (!p_eof(s) && (*s->p == 'e' || *s->p == 'E')) {
        has_exponent = 1;
        p_advance(s);
        if (!p_eof(s) && (*s->p == '+' || *s->p == '-')) {
            if (*s->p == '-') exp_sign = -1;
            p_advance(s);
        }
        if (p_eof(s) || !(CC(*s->p) & CC_DIGIT)) {
            err_set(s->line, s->col, "expected digit in exponent");
            return NULL;
        }
        while (!p_eof(s) && (CC(*s->p) & CC_DIGIT)) {
            int d = *s->p - '0';
            // Clamp the exponent magnitude at something that will force
            // the strtod fallback; no point accumulating to int-overflow.
            if (exponent > 10000) exp_overflow = 1;
            else exponent = exponent * 10 + d;
            p_advance(s);
        }
    }

    int effective_exp = (has_exponent ? exp_sign * exponent : 0) - frac_digits;
    size_t len = (size_t)(s->p - start);

    // Integer-only fast path: no decimal, no exponent, fits in int64.
    if (!has_decimal && !has_exponent && !int_overflow) {
        // Signed result. int_acc <= INT64_MAX for non-negative; for negative
        // we allow the full INT64_MIN magnitude (1 more than INT64_MAX abs).
        if (negative) {
            if (int_acc == 0 || int_acc > (uint64_t)INT64_MAX + 1ULL) {
                /* int_acc==0 is "-0": keep the negative zero via the double
                 * path. A magnitude past INT64_MIN falls through to strtod. */
            } else {
                long long val = (int_acc == (uint64_t)INT64_MAX + 1ULL)
                                    ? INT64_MIN
                                    : -(long long)int_acc;
                JsonValue* v = jv_new_integer(s->arena, val);
                if (!v) { err_set(line0, col0, "out of memory"); return NULL; }
                return v;
            }
        } else {
            if (int_acc <= (uint64_t)INT64_MAX) {
                JsonValue* v = jv_new_integer(s->arena, (long long)int_acc);
                if (!v) { err_set(line0, col0, "out of memory"); return NULL; }
                return v;
            }
        }
    }

    // Fast-double path. Combines the int + frac parts into one integer
    // mantissa, then scales by a single pow10 lookup. Safe only when:
    //   - Neither accumulator overflowed.
    //   - Total significant digits fit in 15 (double's fully-exact range).
    //   - |effective_exp| <= POW10_FAST_MAX so our lookup is exact.
    //   - The exponent literal itself didn't overflow.
    int total_digits = int_digits + frac_digits;
    if (!int_overflow && !frac_overflow && !exp_overflow &&
        total_digits <= FAST_INT_SAFE_DIGITS) {
        // Merge int_acc and frac_acc into a combined mantissa
        // (int_acc * 10^frac_digits + frac_acc), which still fits in
        // uint64 when total_digits <= 19, and within exact-double range
        // when total_digits <= 15.
        uint64_t mantissa = int_acc;
        if (frac_digits > 0) {
            mantissa = int_acc * (uint64_t)POW10_POS[frac_digits] + frac_acc;
        }

        double val = (double)mantissa;
        int e = effective_exp;
        if (e >= -POW10_FAST_MAX && e <= POW10_FAST_MAX) {
            if (e > 0)      val *= POW10_POS[e];
            else if (e < 0) val /= POW10_POS[-e];
            if (negative) val = -val;
            JsonValue* v = jv_new_number(s->arena, val);
            if (!v) { err_set(line0, col0, "out of memory"); return NULL; }
            return v;
        }
    }

    // Slow path: hand off to strtod for correct IEEE-754 rounding on
    // edge cases (> 15 significant digits, huge exponents, denormals).
    // Numbers rarely exceed a handful of characters; use a stack buffer.
    char stackbuf[64];
    char* numbuf;
    if (len < sizeof(stackbuf)) {
        memcpy(stackbuf, start, len);
        stackbuf[len] = '\0';
        numbuf = stackbuf;
    } else {
        numbuf = arena_strndup(s->arena, start, len);
        if (!numbuf) { err_set(line0, col0, "out of memory"); return NULL; }
    }

    // Locale-pinned. RFC 8259 fixes '.' as the decimal separator, so parsing
    // must not follow LC_NUMERIC: under a comma-decimal locale bare strtod
    // stops at the '.', the endp check below fails, and every fractional
    // number in the document becomes a parse error. See aether_locale_num.h.
    char* endp = NULL;
    double val = aether_c_strtod(numbuf, &endp);
    if (endp != numbuf + len) {
        err_set(line0, col0, "number conversion failed");
        return NULL;
    }

    JsonValue* v = jv_new_number(s->arena, val);
    if (!v) { err_set(line0, col0, "out of memory"); return NULL; }
    return v;
}

// ---------------------------------------------------------------------------
// UTF-8 encoding helper (for \uXXXX decoding)
// ---------------------------------------------------------------------------

// Encode a code point as UTF-8 into `buf`. Returns byte count (1-4), or
// 0 for invalid code points (lone surrogates, > U+10FFFF).
static int encode_utf8(uint32_t cp, char* buf) {
    if (cp < 0x80u) {
        buf[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        buf[0] = (char)(0xC0u | (cp >> 6));
        buf[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        if (cp >= 0xD800u && cp <= 0xDFFFu) return 0;  // surrogate
        buf[0] = (char)(0xE0u | (cp >> 12));
        buf[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        buf[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp <= 0x10FFFFu) {
        buf[0] = (char)(0xF0u | (cp >> 18));
        buf[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        buf[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        buf[3] = (char)(0x80u | (cp & 0x3Fu));
        return 4;
    }
    return 0;
}

// Parse exactly 4 hex digits. Returns value 0..0xFFFF, or -1 on error.
static int parse_hex4(Parser* s) {
    if (s->end - s->p < 4) {
        err_set(s->line, s->col, "truncated \\u escape");
        return -1;
    }
    int v = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)s->p[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else {
            err_set(s->line, s->col + i, "expected hex digit in \\u escape");
            return -1;
        }
        v = (v << 4) | d;
    }
    for (int i = 0; i < 4; i++) p_advance(s);
    return v;
}

// ---------------------------------------------------------------------------
// String parser
//
// Two-phase for speed:
//   1. Fast-scan: a tight loop consumes runs of "safe" bytes via the
//      CC_STR_OK lookup. Breaks out only on escape, quote, control char,
//      or non-ASCII (>= 0x80).
//   2. When the fast loop breaks, handle the specific case:
//        - '"'  → close string
//        - '\\' → decode escape (incl. \\uXXXX and surrogate pairs)
//        - < 0x20 → RFC violation, reject
//        - >= 0x80 → validate via UTF-8 DFA, memcpy the bytes
//
// Output goes into a growing arena-allocated buffer; at the end the
// buffer is sliced to the final length.
// ---------------------------------------------------------------------------

static const char* parse_string_raw(Parser* s, uint32_t* out_len) {
    if (p_peek(s) != '"') {
        err_set(s->line, s->col, "expected string");
        return NULL;
    }
    int line0 = s->line, col0 = s->col;
    p_advance(s);  // consume opening quote

    // Pre-scan: locate the closing quote without advancing state. The
    // decoded string cannot be longer than the raw span (escapes only
    // shrink or match), so a single arena_alloc sized to the span is
    // a tight upper bound. Without this, a naive "allocate remaining
    // input bytes per string" strategy blows up to gigabytes on large
    // documents with many strings.
    const char* scan = s->p;
    while (scan < s->end) {
        unsigned char c = (unsigned char)*scan;
        if (c == '"') break;
        if (c == '\\') {
            // Skip backslash + next byte; for \uXXXX the 4 hex digits
            // after 'u' are plain ASCII (no quote, no backslash) and
            // fall through the default branch naturally.
            scan++;
            if (scan >= s->end) {
                err_set(line0, col0, "unterminated escape");
                return NULL;
            }
            scan++;
            continue;
        }
        scan++;
    }
    if (scan >= s->end) {
        err_set(line0, col0, "unterminated string");
        return NULL;
    }

    size_t span = (size_t)(scan - s->p);
    char* dst = (char*)arena_alloc(s->arena, span + 1);
    if (!dst) { err_set(line0, col0, "out of memory"); return NULL; }
    size_t di = 0;

    while (s->p < s->end) {
        // Fast loop: SIMD-accelerated scan over "safe" printable ASCII
        // non-escape bytes. scan_str_safe returns the count of leading
        // safe bytes in the remaining input; SSE2 / NEON process 16 at
        // a time, scalar falls back to the LUT. Safe bytes contain no
        // newlines (0x0A < 0x20 so it's always "unsafe"), so line/col
        // accounting simplifies to bumping col by the run length.
        size_t remaining = (size_t)(s->end - s->p);
        size_t run_len = scan_str_safe(s->p, remaining);
        if (run_len) {
            memcpy(dst + di, s->p, run_len);
            di += run_len;
            s->p += run_len;
            s->col += (int)run_len;
        }

        if (s->p >= s->end) {
            err_set(line0, col0, "unterminated string");
            return NULL;
        }

        unsigned char c = (unsigned char)*s->p;

        if (c == '"') {
            p_advance(s);
            dst[di] = '\0';
            *out_len = (uint32_t)di;
            return dst;
        }

        if (c == '\\') {
            p_advance(s);
            if (p_eof(s)) {
                err_set(s->line, s->col, "unterminated escape");
                return NULL;
            }
            unsigned char esc = (unsigned char)*s->p;
            switch (esc) {
                case '"':  dst[di++] = '"';  p_advance(s); break;
                case '\\': dst[di++] = '\\'; p_advance(s); break;
                case '/':  dst[di++] = '/';  p_advance(s); break;
                case 'b':  dst[di++] = '\b'; p_advance(s); break;
                case 'f':  dst[di++] = '\f'; p_advance(s); break;
                case 'n':  dst[di++] = '\n'; p_advance(s); break;
                case 'r':  dst[di++] = '\r'; p_advance(s); break;
                case 't':  dst[di++] = '\t'; p_advance(s); break;
                case 'u': {
                    p_advance(s);  // consume 'u'
                    int hi = parse_hex4(s);
                    if (hi < 0) return NULL;
                    uint32_t cp;
                    if (hi >= 0xD800 && hi <= 0xDBFF) {
                        // High surrogate — require \\uXXXX low surrogate next.
                        if (s->end - s->p < 2 || s->p[0] != '\\' || s->p[1] != 'u') {
                            err_set(s->line, s->col,
                                    "high surrogate must be followed by \\u low surrogate");
                            return NULL;
                        }
                        p_advance(s); p_advance(s);
                        int lo = parse_hex4(s);
                        if (lo < 0) return NULL;
                        if (lo < 0xDC00 || lo > 0xDFFF) {
                            err_set(s->line, s->col, "expected low surrogate after high");
                            return NULL;
                        }
                        cp = 0x10000u
                             + (((uint32_t)hi - 0xD800u) << 10)
                             + ((uint32_t)lo - 0xDC00u);
                    } else if (hi >= 0xDC00 && hi <= 0xDFFF) {
                        err_set(s->line, s->col, "unpaired low surrogate");
                        return NULL;
                    } else {
                        cp = (uint32_t)hi;
                    }
                    int n = encode_utf8(cp, dst + di);
                    if (n == 0) {
                        err_set(s->line, s->col, "invalid code point in \\u escape");
                        return NULL;
                    }
                    di += (size_t)n;
                    break;
                }
                default:
                    err_set(s->line, s->col, "invalid escape \\%c", esc);
                    return NULL;
            }
            continue;
        }

        if (c < 0x20) {
            err_set(s->line, s->col, "control character U+%04X must be escaped", c);
            return NULL;
        }

        // Non-ASCII (>= 0x80): validate via UTF-8 DFA and memcpy the bytes.
        uint32_t st = UTF8_ACCEPT, cp = 0;
        const char* utf8_start = s->p;
        while (s->p < s->end) {
            utf8_step(&st, &cp, (uint8_t)*s->p);
            p_advance(s);
            if (st == UTF8_ACCEPT || st == UTF8_REJECT) break;
        }
        if (st != UTF8_ACCEPT) {
            err_set(s->line, s->col, "invalid UTF-8 sequence");
            return NULL;
        }
        size_t utf8_len = (size_t)(s->p - utf8_start);
        memcpy(dst + di, utf8_start, utf8_len);
        di += utf8_len;
    }

    err_set(line0, col0, "unterminated string");
    return NULL;
}

static JsonValue* parse_string(Parser* s) {
    uint32_t len = 0;
    const char* data = parse_string_raw(s, &len);
    if (!data) return NULL;
    JsonValue* v = jv_new_in(s->arena, JSON_STRING);
    if (!v) { err_set(s->line, s->col, "out of memory"); return NULL; }
    v->data.str.data = data;
    v->data.str.length = len;
    return v;
}

// ---------------------------------------------------------------------------
// Recursive descent for arrays, objects, and the value dispatcher
// ---------------------------------------------------------------------------

static JsonValue* parse_value_depth(Parser* s, int depth);

static JsonValue* parse_null_lit(Parser* s) {
    int line0 = s->line, col0 = s->col;
    if ((size_t)(s->end - s->p) < 4 || memcmp(s->p, "null", 4) != 0) {
        err_set(line0, col0, "expected 'null'");
        return NULL;
    }
    for (int i = 0; i < 4; i++) p_advance(s);
    JsonValue* v = jv_new_null(s->arena);
    if (!v) { err_set(line0, col0, "out of memory"); return NULL; }
    return v;
}

static JsonValue* parse_bool_lit(Parser* s) {
    int line0 = s->line, col0 = s->col;
    if ((size_t)(s->end - s->p) >= 4 && memcmp(s->p, "true", 4) == 0) {
        for (int i = 0; i < 4; i++) p_advance(s);
        JsonValue* v = jv_new_bool(s->arena, 1);
        if (!v) { err_set(line0, col0, "out of memory"); return NULL; }
        return v;
    }
    if ((size_t)(s->end - s->p) >= 5 && memcmp(s->p, "false", 5) == 0) {
        for (int i = 0; i < 5; i++) p_advance(s);
        JsonValue* v = jv_new_bool(s->arena, 0);
        if (!v) { err_set(line0, col0, "out of memory"); return NULL; }
        return v;
    }
    err_set(line0, col0, "expected 'true' or 'false'");
    return NULL;
}

// Grow the arr->items[] buffer to at least `needed` entries. Power-of-two
// growth to amortise the allocation cost.
static int arr_reserve(JsonValue* arr, Arena* a, uint32_t needed) {
    if (needed <= arr->data.arr.capacity) return 1;
    uint32_t new_cap = arr->data.arr.capacity ? arr->data.arr.capacity
                                              : JSON_CONTAINER_INITIAL_CAP;
    while (new_cap < needed) new_cap *= 2;
    JsonValue** grown = (JsonValue**)arena_grow(
        a,
        arr->data.arr.items,
        sizeof(JsonValue*) * arr->data.arr.capacity,
        sizeof(JsonValue*) * new_cap);
    if (!grown) return 0;
    arr->data.arr.items = grown;
    arr->data.arr.capacity = new_cap;
    return 1;
}

// Analogous for objects — the three parallel arrays grow together,
// reachable through a single JsonObjBlock pointer hanging off the value.
// First reserve allocates the block; subsequent reserves grow the arrays.
static int obj_reserve(JsonValue* obj, Arena* a, uint32_t needed) {
    if (needed <= obj->data.obj.capacity) return 1;
    uint32_t old_cap = obj->data.obj.capacity;
    uint32_t new_cap = old_cap ? old_cap : JSON_CONTAINER_INITIAL_CAP;
    while (new_cap < needed) new_cap *= 2;

    JsonObjBlock* blk = obj->data.obj.blk;
    if (!blk) {
        blk = (JsonObjBlock*)arena_alloc(a, sizeof(JsonObjBlock));
        if (!blk) return 0;
        blk->keys = NULL;
        blk->key_lens = NULL;
        blk->values = NULL;
        obj->data.obj.blk = blk;
    }

    const char** gk = (const char**)arena_grow(
        a, blk->keys,
        sizeof(const char*) * old_cap,
        sizeof(const char*) * new_cap);
    if (!gk) return 0;

    uint32_t* gl = (uint32_t*)arena_grow(
        a, blk->key_lens,
        sizeof(uint32_t) * old_cap,
        sizeof(uint32_t) * new_cap);
    if (!gl) return 0;

    JsonValue** gv = (JsonValue**)arena_grow(
        a, blk->values,
        sizeof(JsonValue*) * old_cap,
        sizeof(JsonValue*) * new_cap);
    if (!gv) return 0;

    blk->keys     = gk;
    blk->key_lens = gl;
    blk->values   = gv;
    obj->data.obj.capacity = new_cap;
    return 1;
}

static JsonValue* parse_array(Parser* s, int depth) {
    int line0 = s->line, col0 = s->col;
    p_advance(s);  // consume '['

    JsonValue* arr = jv_new_in(s->arena, JSON_ARRAY);
    if (!arr) { err_set(line0, col0, "out of memory"); return NULL; }

    skip_whitespace(s);
    if (p_peek(s) == ']') { p_advance(s); return arr; }

    while (1) {
        skip_whitespace(s);
        JsonValue* elem = parse_value_depth(s, depth + 1);
        if (!elem) return NULL;

        if (!arr_reserve(arr, s->arena, arr->data.arr.count + 1)) {
            err_set(s->line, s->col, "out of memory");
            return NULL;
        }
        arr->data.arr.items[arr->data.arr.count++] = elem;

        skip_whitespace(s);
        unsigned char c = p_peek(s);
        if (c == ',') { p_advance(s); continue; }
        if (c == ']') { p_advance(s); return arr; }

        err_set(s->line, s->col, "expected ',' or ']' in array");
        return NULL;
    }
}

static JsonValue* parse_object(Parser* s, int depth) {
    int line0 = s->line, col0 = s->col;
    p_advance(s);  // consume '{'

    JsonValue* obj = jv_new_in(s->arena, JSON_OBJECT);
    if (!obj) { err_set(line0, col0, "out of memory"); return NULL; }

    skip_whitespace(s);
    if (p_peek(s) == '}') { p_advance(s); return obj; }

    while (1) {
        skip_whitespace(s);
        if (p_peek(s) != '"') {
            err_set(s->line, s->col, "expected string key in object");
            return NULL;
        }
        uint32_t key_len = 0;
        const char* key = parse_string_raw(s, &key_len);
        if (!key) return NULL;

        skip_whitespace(s);
        if (p_peek(s) != ':') {
            err_set(s->line, s->col, "expected ':' after object key");
            return NULL;
        }
        p_advance(s);

        skip_whitespace(s);
        JsonValue* val = parse_value_depth(s, depth + 1);
        if (!val) return NULL;

        if (!obj_reserve(obj, s->arena, obj->data.obj.count + 1)) {
            err_set(s->line, s->col, "out of memory");
            return NULL;
        }
        uint32_t idx = obj->data.obj.count++;
        JsonObjBlock* blk = obj->data.obj.blk;
        blk->keys[idx] = key;
        blk->key_lens[idx] = key_len;
        blk->values[idx] = val;

        skip_whitespace(s);
        unsigned char c = p_peek(s);
        if (c == ',') { p_advance(s); continue; }
        if (c == '}') { p_advance(s); return obj; }

        err_set(s->line, s->col, "expected ',' or '}' in object");
        return NULL;
    }
}

static JsonValue* parse_value_depth(Parser* s, int depth) {
    if (depth > JSON_MAX_DEPTH) {
        err_set(s->line, s->col, "nesting depth exceeds limit of %d", JSON_MAX_DEPTH);
        return NULL;
    }
    skip_whitespace(s);
    if (p_eof(s)) {
        err_set(s->line, s->col, "unexpected end of input");
        return NULL;
    }
    unsigned char c = (unsigned char)*s->p;
    switch (c) {
        case 'n':  return parse_null_lit(s);
        case 't':
        case 'f':  return parse_bool_lit(s);
        case '"':  return parse_string(s);
        case '[':  return parse_array(s, depth);
        case '{':  return parse_object(s, depth);
        default:
            if (CC(c) & CC_NUM_START) return parse_number(s);
            err_set(s->line, s->col, "unexpected character '%c'", c);
            return NULL;
    }
}

// ---------------------------------------------------------------------------
// Public parse entry points
// ---------------------------------------------------------------------------

JsonValue* json_parse_raw_n(const char* data, size_t n) {
    cc_init();
    err_clear();

    if (!data) {
        err_set_no_pos("null input");
        return NULL;
    }

    Arena* a = arena_create();
    if (!a) {
        err_set_no_pos("out of memory");
        return NULL;
    }

    Parser s;
    s.p = data;
    s.end = data + n;
    s.line = 1;
    s.col = 1;
    s.arena = a;

    JsonValue* root = parse_value_depth(&s, 0);
    if (!root) {
        arena_destroy(a);
        return NULL;
    }

    skip_whitespace(&s);
    if (!p_eof(&s)) {
        err_set(s.line, s.col, "unexpected trailing content");
        arena_destroy(a);
        return NULL;
    }

    root->flags |= JV_FLAG_ROOT;
    root->arena = a;
    return root;
}

JsonValue* json_parse_raw(const char* s) {
    return json_parse_raw_n(s, s ? strlen(s) : 0);
}

// ---------------------------------------------------------------------------
// Accessors (all NULL-safe)
// ---------------------------------------------------------------------------

JsonType json_type(JsonValue* v) {
    return v ? (JsonType)v->type : JSON_NULL;
}

int json_is_null(JsonValue* v) {
    return !v || v->type == JSON_NULL;
}

int json_get_bool(JsonValue* v) {
    return (v && v->type == JSON_BOOL) ? v->data.boolean : 0;
}

double json_get_number(JsonValue* v) {
    if (!v || v->type != JSON_NUMBER) return 0.0;
    /* Integer-flavoured values (json_create_int) read from the dedicated
     * `integer` slot; widen to double for the float-shaped API. */
    if (v->flags & JV_FLAG_INTEGER) return (double)v->data.integer;
    return v->data.number;
}

long long json_get_long(JsonValue* v) {
    if (!v || v->type != JSON_NUMBER) return 0;
    if (v->flags & JV_FLAG_INTEGER) return v->data.integer;
    return (long long)v->data.number;
}

int json_get_int(JsonValue* v) {
    if (!v || v->type != JSON_NUMBER) return 0;
    if (v->flags & JV_FLAG_INTEGER) {
        long long n = v->data.integer;
        return n > INT32_MAX ? INT32_MAX : (n < INT32_MIN ? INT32_MIN : (int)n);
    }
    double d = v->data.number;
    if (d >= (double)INT32_MAX) return INT32_MAX;
    if (d <= (double)INT32_MIN) return INT32_MIN;
    return (int)d;
}

const char* json_get_string_raw(JsonValue* v) {
    if (!v || v->type != JSON_STRING) return NULL;
    return v->data.str.data;
}

JsonValue* json_object_get_raw(JsonValue* obj, const char* key) {
    if (!obj || obj->type != JSON_OBJECT || !key) return NULL;
    uint32_t n = obj->data.obj.count;
    if (!n) return NULL;
    size_t klen = strlen(key);
    if (klen > UINT32_MAX) return NULL;
    uint32_t k32 = (uint32_t)klen;
    const JsonObjBlock* blk = obj->data.obj.blk;
    for (uint32_t i = 0; i < n; i++) {
        if (blk->key_lens[i] == k32 &&
            memcmp(blk->keys[i], key, k32) == 0) {
            return blk->values[i];
        }
    }
    return NULL;
}

int json_object_has(JsonValue* obj, const char* key) {
    return json_object_get_raw(obj, key) != NULL;
}

// Object-key iteration. Empty objects have blk == NULL (obj_reserve
// allocates the block lazily on the first set), so the NULL guard is
// load-bearing, not defensive.

int json_object_size_raw(JsonValue* obj) {
    if (!obj || obj->type != JSON_OBJECT) return -1;
    return (int)obj->data.obj.count;
}

const char* json_object_key_at(JsonValue* obj, int i) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    if (i < 0 || (uint32_t)i >= obj->data.obj.count) return NULL;
    const JsonObjBlock* blk = obj->data.obj.blk;
    if (!blk) return NULL;
    return blk->keys[i];
}

int json_object_key_len_at(JsonValue* obj, int i) {
    if (!obj || obj->type != JSON_OBJECT) return -1;
    if (i < 0 || (uint32_t)i >= obj->data.obj.count) return -1;
    const JsonObjBlock* blk = obj->data.obj.blk;
    if (!blk) return -1;
    return (int)blk->key_lens[i];
}

JsonValue* json_object_value_at(JsonValue* obj, int i) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    if (i < 0 || (uint32_t)i >= obj->data.obj.count) return NULL;
    JsonObjBlock* blk = obj->data.obj.blk;
    if (!blk) return NULL;
    return blk->values[i];
}

JsonValue* json_array_get_raw(JsonValue* arr, int index) {
    if (!arr || arr->type != JSON_ARRAY) return NULL;
    if (index < 0 || (uint32_t)index >= arr->data.arr.count) return NULL;
    return arr->data.arr.items[(uint32_t)index];
}

int json_array_size(JsonValue* arr) {
    if (!arr || arr->type != JSON_ARRAY) return 0;
    return (int)arr->data.arr.count;
}

// ---------------------------------------------------------------------------
// Mutators — supports adding heap-built JsonValues into arena-backed containers.
//
// Strategy: deep-copy the incoming value (and its entire subtree) into
// the parent's arena. The original JsonValue is then freed. This keeps
// ownership clean — the parent's arena owns everything after insertion —
// at the cost of one O(n) copy per mutation. Mutation is rare compared
// to parsing; this is the right tradeoff.
//
// Exception: if the parent has no arena (itself heap-allocated, from
// json_create_*), we still deep-copy into an arena — we CREATE one for
// the parent if it doesn't have one yet. This unifies the free path.
// ---------------------------------------------------------------------------

// Forward decl for deep_copy which is mutually recursive.
static JsonValue* deep_copy_into_arena(JsonValue* src, Arena* dst_arena);

// Copy a byte range into dst_arena as a null-terminated string.
static const char* arena_copy_str(Arena* a, const char* src, uint32_t len) {
    char* dst = (char*)arena_alloc(a, (size_t)len + 1);
    if (!dst) return NULL;
    if (len) memcpy(dst, src, len);
    dst[len] = '\0';
    return dst;
}

static JsonValue* deep_copy_into_arena(JsonValue* src, Arena* dst_arena) {
    if (!src) return NULL;
    JsonValue* out = (JsonValue*)arena_alloc(dst_arena, sizeof(JsonValue));
    if (!out) return NULL;
    memset(out, 0, sizeof(JsonValue));
    out->type = src->type;

    switch (src->type) {
        case JSON_NULL: break;
        case JSON_BOOL: out->data.boolean = src->data.boolean; break;
        case JSON_NUMBER:
            if (src->flags & JV_FLAG_INTEGER) {
                out->flags |= JV_FLAG_INTEGER;
                out->data.integer = src->data.integer;
            } else {
                out->data.number = src->data.number;
            }
            break;
        case JSON_STRING: {
            uint32_t len = src->data.str.length;
            const char* c = arena_copy_str(dst_arena, src->data.str.data, len);
            if (!c) return NULL;
            out->data.str.data = c;
            out->data.str.length = len;
            break;
        }
        case JSON_ARRAY: {
            uint32_t n = src->data.arr.count;
            if (n) {
                JsonValue** items = (JsonValue**)arena_alloc(
                    dst_arena, sizeof(JsonValue*) * n);
                if (!items) return NULL;
                for (uint32_t i = 0; i < n; i++) {
                    items[i] = deep_copy_into_arena(src->data.arr.items[i], dst_arena);
                    if (!items[i]) return NULL;
                }
                out->data.arr.items = items;
            }
            out->data.arr.count = n;
            out->data.arr.capacity = n;
            break;
        }
        case JSON_OBJECT: {
            uint32_t n = src->data.obj.count;
            if (n) {
                JsonObjBlock* blk = (JsonObjBlock*)arena_alloc(
                    dst_arena, sizeof(JsonObjBlock));
                const char** keys = (const char**)arena_alloc(
                    dst_arena, sizeof(const char*) * n);
                uint32_t* lens = (uint32_t*)arena_alloc(
                    dst_arena, sizeof(uint32_t) * n);
                JsonValue** vals = (JsonValue**)arena_alloc(
                    dst_arena, sizeof(JsonValue*) * n);
                if (!blk || !keys || !lens || !vals) return NULL;
                const JsonObjBlock* src_blk = src->data.obj.blk;
                for (uint32_t i = 0; i < n; i++) {
                    uint32_t kl = src_blk->key_lens[i];
                    keys[i] = arena_copy_str(dst_arena, src_blk->keys[i], kl);
                    if (!keys[i]) return NULL;
                    lens[i] = kl;
                    vals[i] = deep_copy_into_arena(src_blk->values[i], dst_arena);
                    if (!vals[i]) return NULL;
                }
                blk->keys = keys;
                blk->key_lens = lens;
                blk->values = vals;
                out->data.obj.blk = blk;
            }
            out->data.obj.count = n;
            out->data.obj.capacity = n;
            break;
        }
    }
    return out;
}

// Make sure `container` has an arena it can own. If it doesn't, promote
// the container to an arena-backed root: allocate a new arena, deep-copy
// the container into it, and swap the pointers in-place. After this the
// container IS the root of a new arena.
static int ensure_container_arena(JsonValue* container) {
    if (container->arena) return 1;  // already rooted in an arena
    Arena* a = arena_create();
    if (!a) return 0;
    // Deep-copy the container's contents into the new arena. We can't
    // simply point existing heap allocations into the arena — the arena
    // must own everything for a clean free.
    JsonValue* copy = deep_copy_into_arena(container, a);
    if (!copy) { arena_destroy(a); return 0; }
    // Move copy's internals into `container` so the caller's pointer
    // stays valid.
    container->type = copy->type;
    container->data = copy->data;
    container->flags |= JV_FLAG_ROOT;
    container->arena = a;
    return 1;
}

// Recursively free a heap-allocated JsonValue tree. Used to tear down
// values created by json_create_* after they've been absorbed into an
// arena-backed container (or when the caller calls json_free on a
// standalone heap value).
static void heap_free_tree(JsonValue* v);

static void heap_free_tree(JsonValue* v) {
    if (!v) return;
    if (v->arena) {
        // Shouldn't reach here — heap tree means no arena. Defensive
        // cleanup: destroy the arena, and only free the struct itself
        // if it was heap-allocated. Snapshot the flag before
        // arena_destroy so we don't read freed bytes.
        int heap_struct = (v->flags & JV_FLAG_HEAP_STRUCT) != 0;
        arena_destroy(v->arena);
        if (heap_struct) aether_caps_free(v, sizeof(JsonValue));
        return;
    }
    switch (v->type) {
        case JSON_STRING:
            /* Pair with json_create_string's aether_caps_malloc. */
            aether_caps_free((void*)v->data.str.data,
                             (size_t)v->data.str.length + 1);
            break;
        case JSON_ARRAY:
            for (uint32_t i = 0; i < v->data.arr.count; i++) {
                heap_free_tree(v->data.arr.items[i]);
            }
            free(v->data.arr.items);
            break;
        case JSON_OBJECT: {
            JsonObjBlock* blk = v->data.obj.blk;
            if (blk) {
                for (uint32_t i = 0; i < v->data.obj.count; i++) {
                    free((void*)blk->keys[i]);
                    heap_free_tree(blk->values[i]);
                }
                free(blk->keys);
                free(blk->key_lens);
                free(blk->values);
                free(blk);
            }
            break;
        }
        default:
            break;
    }
    /* Pair with heap_new's aether_caps_malloc. */
    aether_caps_free(v, sizeof(JsonValue));
}

/* Reclaim a donated builder value after its contents were deep-copied into a
 * container's arena (object_set / array_add). Two shapes:
 *  - value->arena set: the value acquired its own arena (e.g. json.obj() that
 *    later had children set into it). arena_destroy frees the arena's contents,
 *    but the JsonValue STRUCT itself is a separate heap_new allocation
 *    (JV_FLAG_HEAP_STRUCT — "v->arena is set but the struct still sits on the
 *    heap") and must be freed too, or it leaks (#1447). Snapshot the flag
 *    before arena_destroy since for a parsed root the struct can live inside
 *    the arena; a builder value never does, but be defensive.
 *  - no arena: a plain heap tree — heap_free_tree reclaims struct + children. */
static void free_donated_value(JsonValue* value) {
    if (!value) return;
    if (value->arena) {
        int heap_struct = (value->flags & JV_FLAG_HEAP_STRUCT) != 0;
        arena_destroy(value->arena);
        if (heap_struct) aether_caps_free(value, sizeof(JsonValue));
    } else {
        heap_free_tree(value);
    }
}

int json_array_add_raw(JsonValue* arr, JsonValue* value) {
    if (!arr || arr->type != JSON_ARRAY || !value) return 0;
    if (!ensure_container_arena(arr)) return 0;

    JsonValue* copy = deep_copy_into_arena(value, arr->arena);
    if (!copy) return 0;

    if (!arr_reserve(arr, arr->arena, arr->data.arr.count + 1)) return 0;
    arr->data.arr.items[arr->data.arr.count++] = copy;

    // The caller passed ownership to us. Free the incoming tree.
    free_donated_value(value);

    return 1;
}

int json_object_set_raw(JsonValue* obj, const char* key, JsonValue* value) {
    if (!obj || obj->type != JSON_OBJECT || !key || !value) return 0;
    if (!ensure_container_arena(obj)) return 0;

    size_t klen = strlen(key);
    if (klen > UINT32_MAX) return 0;
    uint32_t k32 = (uint32_t)klen;

    JsonValue* copy = deep_copy_into_arena(value, obj->arena);
    if (!copy) return 0;

    // If the key already exists, replace the value pointer in place.
    JsonObjBlock* blk = obj->data.obj.blk;
    if (blk) {
        for (uint32_t i = 0; i < obj->data.obj.count; i++) {
            if (blk->key_lens[i] == k32 &&
                memcmp(blk->keys[i], key, k32) == 0) {
                blk->values[i] = copy;
                free_donated_value(value);
                return 1;
            }
        }
    }

    // New key: grow arrays and append.
    if (!obj_reserve(obj, obj->arena, obj->data.obj.count + 1)) return 0;
    const char* key_copy = arena_copy_str(obj->arena, key, k32);
    if (!key_copy) return 0;

    uint32_t idx = obj->data.obj.count++;
    blk = obj->data.obj.blk;  // re-read; obj_reserve may have just allocated it
    blk->keys[idx] = key_copy;
    blk->key_lens[idx] = k32;
    blk->values[idx] = copy;

    free_donated_value(value);
    return 1;
}

// ---------------------------------------------------------------------------
// Creators — build values outside of a parse. These are heap-allocated
// standalone trees; adding them to an arena-backed container transfers
// ownership (see mutators above).
// ---------------------------------------------------------------------------

static JsonValue* heap_new(uint8_t type) {
    /* Cap-aware (#343): heap-tree JsonValue used by the json.create_*
     * builder API. Fixed-size struct so the matching free in
     * heap_free_tree passes sizeof(JsonValue). */
    JsonValue* v = (JsonValue*)aether_caps_malloc(sizeof(JsonValue));
    if (!v) return NULL;
    memset(v, 0, sizeof(JsonValue));
    v->type = type;
    v->flags = JV_FLAG_HEAP_STRUCT;
    return v;
}

JsonValue* json_create_null(void) {
    return heap_new(JSON_NULL);
}

JsonValue* json_create_bool(int b) {
    JsonValue* v = heap_new(JSON_BOOL);
    if (v) v->data.boolean = !!b;
    return v;
}

JsonValue* json_create_number(double n) {
    JsonValue* v = heap_new(JSON_NUMBER);
    if (v) v->data.number = n;
    return v;
}

/* Integer-flavoured number constructor (#752-sibling /
 * stdlib-json-integer-value-ask.md). Stores the value in the dedicated
 * `integer` union slot — preserving the full int64 range — and stamps
 * JV_FLAG_INTEGER so the serializer emits `%lld` instead of `%g`. The
 * existing json_create_number(double) path is unchanged; this exists
 * because the float route serialises large integers as scientific
 * notation (53248000.0 -> "5.3248e+07") which is wrong for byte-count
 * / total-bytes / id fields and lossy past 2^53. */
JsonValue* json_create_int(long long n) {
    JsonValue* v = heap_new(JSON_NUMBER);
    if (v) {
        v->data.integer = n;
        v->flags |= JV_FLAG_INTEGER;
    }
    return v;
}

JsonValue* json_create_string(const char* s) {
    JsonValue* v = heap_new(JSON_STRING);
    if (!v) return NULL;
    /* Cap-aware (#343): input string is caller-controlled, plugin-
     * host-reachable. heap_free_tree's JSON_STRING branch passes
     * v->data.str.length + 1 back to the caps allocator. */
    size_t len = s ? strlen(s) : 0;
    char* copy = (char*)aether_caps_malloc(len + 1);
    if (!copy) { aether_caps_free(v, sizeof(JsonValue)); return NULL; }
    if (len && s) memcpy(copy, s, len);
    copy[len] = '\0';
    v->data.str.data = copy;
    v->data.str.length = (uint32_t)len;
    return v;
}

JsonValue* json_create_array(void) {
    return heap_new(JSON_ARRAY);
}

JsonValue* json_create_object(void) {
    return heap_new(JSON_OBJECT);
}

// ---------------------------------------------------------------------------
// json_free — works for both arena-backed (root) and heap-allocated trees.
// ---------------------------------------------------------------------------

void json_free(JsonValue* v) {
    if (!v) return;
    if (v->arena) {
        // Snapshot the "is heap struct" bit BEFORE arena_destroy, because
        // for parsed roots `v` itself lives inside the arena — reading
        // `v->flags` after the arena is freed would be use-after-free.
        // JV_FLAG_HEAP_STRUCT is only set when the struct was malloc'd
        // via heap_new (creation API), then later retrofitted with an
        // arena via ensure_container_arena. Parsed roots have flag == 0.
        int heap_struct = (v->flags & JV_FLAG_HEAP_STRUCT) != 0;
        Arena* a = v->arena;
        arena_destroy(a);
        if (heap_struct) aether_caps_free(v, sizeof(JsonValue));
        return;
    }
    // Heap path — no arena ever attached.
    heap_free_tree(v);
}

// ---------------------------------------------------------------------------
// Stringify
//
// Single growing buffer. Escapes only the required bytes; emits raw
// UTF-8 as-is (no \uXXXX encoding needed since the parser only accepted
// valid UTF-8 in the first place).
// ---------------------------------------------------------------------------

typedef struct {
    char*  data;
    size_t len;
    size_t cap;
    int    oom;
} StrBuf;

static int sb_reserve(StrBuf* b, size_t extra) {
    if (b->oom) return 0;
    if (b->len + extra + 1 > b->cap) {
        size_t need = b->len + extra + 1;
        size_t new_cap = aether_buf_grow_capacity(b->cap, 256, need, 1);
        if (!new_cap) { b->oom = 1; return 0; }
        char* nd = (char*)realloc(b->data, new_cap);
        if (!nd) { b->oom = 1; return 0; }
        b->data = nd;
        b->cap = new_cap;
    }
    return 1;
}

static void sb_append(StrBuf* b, const char* s, size_t n) {
    if (!sb_reserve(b, n)) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void sb_append_char(StrBuf* b, char c) {
    sb_append(b, &c, 1);
}

static void sb_append_cstr(StrBuf* b, const char* s) {
    sb_append(b, s, strlen(s));
}

// Emit a JSON-escaped string literal (with surrounding quotes).
static void sb_emit_string(StrBuf* b, const char* s, uint32_t n) {
    sb_append_char(b, '"');
    const char* p = s;
    const char* end = s + n;
    const char* run_start = p;

    while (p < end) {
        unsigned char c = (unsigned char)*p;
        const char* esc = NULL;
        char short_esc[3];
        if (c == '"')       esc = "\\\"";
        else if (c == '\\') esc = "\\\\";
        else if (c == '\b') esc = "\\b";
        else if (c == '\f') esc = "\\f";
        else if (c == '\n') esc = "\\n";
        else if (c == '\r') esc = "\\r";
        else if (c == '\t') esc = "\\t";
        else if (c < 0x20) {
            // Other controls → \u00XX
            snprintf(short_esc, sizeof(short_esc), "%02X", c);
            if (run_start < p) sb_append(b, run_start, (size_t)(p - run_start));
            sb_append(b, "\\u00", 4);
            sb_append(b, short_esc, 2);
            p++;
            run_start = p;
            continue;
        } else {
            p++;
            continue;
        }
        // Flush accumulated run, then the escape.
        if (run_start < p) sb_append(b, run_start, (size_t)(p - run_start));
        sb_append_cstr(b, esc);
        p++;
        run_start = p;
    }
    if (run_start < end) sb_append(b, run_start, (size_t)(end - run_start));
    sb_append_char(b, '"');
}

static void sb_emit_value(StrBuf* b, JsonValue* v, int depth);

static void sb_emit_array(StrBuf* b, JsonValue* v, int depth) {
    sb_append_char(b, '[');
    for (uint32_t i = 0; i < v->data.arr.count; i++) {
        if (i) sb_append_char(b, ',');
        sb_emit_value(b, v->data.arr.items[i], depth + 1);
    }
    sb_append_char(b, ']');
}

static void sb_emit_object(StrBuf* b, JsonValue* v, int depth) {
    sb_append_char(b, '{');
    uint32_t n = v->data.obj.count;
    if (n) {
        const JsonObjBlock* blk = v->data.obj.blk;
        for (uint32_t i = 0; i < n; i++) {
            if (i) sb_append_char(b, ',');
            sb_emit_string(b, blk->keys[i], blk->key_lens[i]);
            sb_append_char(b, ':');
            sb_emit_value(b, blk->values[i], depth + 1);
        }
    }
    sb_append_char(b, '}');
}

static void sb_emit_value(StrBuf* b, JsonValue* v, int depth) {
    if (!v || depth > JSON_MAX_DEPTH) {
        sb_append(b, "null", 4);
        return;
    }
    switch (v->type) {
        case JSON_NULL:   sb_append(b, "null", 4); break;
        case JSON_BOOL:
            sb_append_cstr(b, v->data.boolean ? "true" : "false");
            break;
        case JSON_NUMBER: {
            char nb[64];
            int n;
            if (v->flags & JV_FLAG_INTEGER) {
                /* Integer-flavoured number — full int64 range, bare
                 * integer wire format, no scientific-notation crossover
                 * past ~1e7. */
                n = snprintf(nb, sizeof(nb), "%lld", v->data.integer);
            } else {
                // Locale-pinned: RFC 8259 mandates '.', so this must not
                // emit "3,14" just because the host set a European locale.
                n = aether_c_snprintf_double(nb, sizeof(nb), "%g", v->data.number);
            }
            if (n > 0) sb_append(b, nb, (size_t)n);
            break;
        }
        case JSON_STRING:
            sb_emit_string(b, v->data.str.data, v->data.str.length);
            break;
        case JSON_ARRAY:  sb_emit_array(b, v, depth); break;
        case JSON_OBJECT: sb_emit_object(b, v, depth); break;
        default:
            sb_append(b, "null", 4);
            break;
    }
}

char* json_stringify_raw(JsonValue* v) {
    StrBuf b = {0};
    sb_emit_value(&b, v, 0);
    if (b.oom) {
        free(b.data);
        // Return an empty string rather than NULL so existing callers
        // that don't check for NULL don't crash — matches prior behavior.
        char* empty = (char*)malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }
    if (!b.data) {
        char* empty = (char*)malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }
    return b.data;
}
