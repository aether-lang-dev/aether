#ifndef AETHER_STRING_H
#define AETHER_STRING_H

#include <stddef.h>

// Magic number to distinguish AetherString* from raw char*
#define AETHER_STRING_MAGIC 0xAE57C0DE

// String structure - immutable, reference counted
typedef struct AetherString {
    unsigned int magic;     // Always AETHER_STRING_MAGIC for valid AetherString
    int ref_count;
    size_t length;
    size_t capacity;
    char* data;
} AetherString;

// Check if a pointer is an AetherString (vs raw char*).
//
// Reads bytes from the pointer to look for the magic header. Done
// byte-by-byte with short-circuit so that for the 99.6% of inputs
// whose first byte mismatches, we read exactly one byte — keeping
// the dispatch ASan-clean on short string allocations like "hi" (2
// bytes) or "x" (1 byte) where a naive `s->magic` 4-byte read
// overshoots the allocation.
//
// On a confirmed magic match, also validate that the rest of the
// header looks like a real AetherString — non-negative ref_count,
// capacity ≥ length, non-NULL data — to harden against the 1-in-2^32
// chance that arbitrary content happens to begin with the magic
// bytes. This catches spurious dispatch before we deref s->data.
static inline int is_aether_string(const void* ptr) {
    if (!ptr) return 0;
    const unsigned char* p = (const unsigned char*)ptr;
    /* Magic 0xAE57C0DE in little-endian = DE C0 57 AE. */
    if (p[0] != 0xDE) return 0;
    if (p[1] != 0xC0) return 0;
    if (p[2] != 0x57) return 0;
    if (p[3] != 0xAE) return 0;
    /* All four bytes matched. Validate the header for plausibility
     * to reject coincidental magic-bytes-in-payload before any
     * downstream s->data deref. */
    const AetherString* s = (const AetherString*)ptr;
    if (s->ref_count < 0) return 0;
    if (s->capacity < s->length) return 0;
    if (!s->data) return 0;
    return 1;
}

// String creation
AetherString* string_new(const char* cstr);
AetherString* string_from_cstr(const char* cstr);  // Alias for new
AetherString* string_from_literal(const char* cstr);  // Alias for new
AetherString* string_new_with_length(const char* data, size_t length);
AetherString* string_empty();

// Reference counting — safe to call with plain char* (no-op)
void string_retain(const void* str);
void string_release(const void* str);
void string_free(const void* str);  // Alias for release

// String operations — accept both AetherString* and plain char*.
// `string_concat` returns a magic AetherString* (refcounted,
// length-aware, binary-safe) — the unified owned-heap representation.
// Freed via string_release (dispatched by aether_heap_str_free on the
// magic header). `string_concat_wrapped` is retained as an explicit
// alias for the same contract. See docs/c-interop.md § AetherString
// return-type contract.
AetherString* string_concat(const void* a, const void* b);
AetherString* string_concat_wrapped(const void* a, const void* b);
int string_length(const void* str);
char string_char_at(const void* str, int index);
int string_equals(const void* a, const void* b);
int string_compare(const void* a, const void* b);

// String methods — accept both AetherString* and plain char*
// Return plain char* (caller owns memory, free with free())
// `prefix` / `suffix` accept either AetherString* or plain const char*
// (binary-safe + handles the mixed-representation hazard at the extern
// boundary — the #297 auto-unwrap skips `string_*`-prefixed externs).
int string_starts_with(const void* str, const void* prefix);
int string_ends_with(const void* str, const void* suffix);
int string_contains(const void* str, const void* substring);
int string_index_of(const void* str, const void* substring);
// Like string_index_of but starts scanning at byte offset `start`.
// Returns the absolute offset of the hit (not relative to `start`),
// or -1. Same binary-safety split as string_index_of: the haystack
// is data-aware (handles AetherString with embedded NULs), the
// needle is strlen-based.
int string_index_of_from(const void* str, const void* substring, int start);
AetherString* string_substring(const void* str, int start, int end);
AetherString* string_replace(const void* str, const void* old_sub, const void* new_sub);
AetherString* string_replace_all(const void* str, const void* old_sub, const void* new_sub);

/* Length-aware sibling — caller supplies the source length explicitly.
 * Use when `str` arrives as a `string`-typed parameter at a function
 * boundary (where #297's auto-unwrap may have stripped the
 * AetherString header) AND the content may contain embedded NULs.
 * The plain string_substring would call str_len() on the unwrapped
 * data and fall through to strlen, truncating at the first NUL. */
AetherString* string_substring_n(const void* str, int str_len_bytes, int start, int end);

/* Identity helper documenting intent: in code that receives a
 * `string` parameter plus an explicit length, the explicit length
 * is the truth — don't consult the AetherString header. Pure no-op
 * at the C level; clamps negative input to 0. */
int string_length_n(const void* str, int known_length);

/* Length-aware sibling of string_char_at. Caller supplies the
 * source length explicitly, so the bounds check skips the per-call
 * strlen that string_char_at pays when `str` arrives as a plain
 * `const char*` (the auto-unwrap at function-parameter boundary
 * strips the AetherString header). Returns the byte at `index`, or
 * '\0' on out-of-range / NULL input. Filed in
 * string-length-aware-accessors.md after avn's 5000-commit bench
 * showed 87%+ of CPU in __strlen_avx2 driven by string_char_at on
 * a multi-KB body whose length the caller had already cached. */
char string_char_at_n(const void* str, int known_length, int index);

/* Length-aware sibling of string_index_of_from. Same motivation as
 * string_char_at_n — caller has already computed `known_length`
 * once and passes it in, avoiding per-call strlen of the haystack.
 * The needle is still strlen'd (small / fixed in practice). Returns
 * the absolute offset of the first match at or after `start`, or
 * -1 on miss. `start` is clamped to [0, known_length]; needle
 * longer than the searchable range returns -1. */
int string_index_of_from_n(const void* str, int known_length,
                           const char* substring, int start);

// Construct a 1-byte AetherString from a byte code (0..255).
// Primary use: emitting known single-byte markers (\x01, \x02, etc.)
// into packed-string record formats without routing through a
// NUL-terminated literal. Always length 1, even for code=0.
AetherString* string_from_char(int code);
AetherString* string_to_upper(const void* str);
AetherString* string_to_lower(const void* str);
AetherString* string_trim(const void* str);

// String array operations (for split)
typedef struct {
    AetherString** strings;
    size_t count;
} AetherStringArray;

AetherStringArray* string_split(const void* str, const void* delimiter);
int string_array_size(AetherStringArray* arr);
const char* string_array_get(AetherStringArray* arr, int index);
void string_array_free(AetherStringArray* arr);

/* Sibling of `string_split` that returns the result as a cons-cell
 * `*StringSeq` (see std/collections/aether_stringseq.h). Same split
 * semantics — empty input gives a single-cell list with "", a delim
 * longer than the input gives a single-cell list with the whole
 * input, trailing/leading delimiters yield empty pieces — but the
 * shape on return is the Erlang-style sequence rather than the
 * dynamic-array AetherStringArray. Useful when the result will be
 * pattern-matched, walked recursively, or sent across an actor
 * boundary as a message field. The returned pointer is a `void*`
 * to keep this header free of a layering dependency on
 * aether_stringseq.h; callers cast to `StringSeq*` (or Aether-side
 * declared as `*StringSeq`). NULL on OOM or NULL inputs. */
void* string_split_to_seq(const void* str, const void* delimiter);

// Conversion
const char* string_to_cstr(const void* str);

// Public FFI accessors for consuming a `-> string` return from C.
//
// A function that an Aether program declares as `-> string` returns an
// AetherString* — a 24-byte magic-tagged header whose `data` field
// points at the payload — NOT a plain char*. C shims that type the
// same extern as `extern const char* foo(...)` and hand the result to
// memcpy/strlen read into the struct header and get garbage (the
// magic bytes, not the content). These helpers accept either shape
// (AetherString* or raw char*), return the byte pointer / length the
// shim actually wanted, and are safe to call on NULL.
//
// Preferred over `string_to_cstr` / `string_length` in FFI code
// because the `aether_` prefix matches the ABI-mangled export names
// and signals intent to reviewers. See docs/aether-string-abi.md for
// the full ABI contract.
const char* aether_string_data(const void* s);
size_t      aether_string_length(const void* s);

AetherString* string_from_int(int value);
// 64-bit sibling of string_from_int. Uses `long long` so it covers
// Aether's `long` type across platforms where `long` is 32-bit (MSVC).
AetherString* string_from_long(long long value);
// Aether's `float` lowers to C `double`, so the parameter is `double`
// (not C `float`). Symmetric to `string_get_float` (PR #562); the
// ABI-mismatch hazard caused `from_float(1.0)` to serialise as `"0"`
// — see the .c-side comment for the full diagnosis.
AetherString* string_from_float(double value);

// Inverse of string_to_int_radix: render `value` in base `radix`
// (2..36). Empty string on out-of-range radix; '-' prefix for
// negatives. long long for LLP64 safety (matches the to_int_radix
// shape).
AetherString* string_from_int_radix(long long value, int radix);

// Pad to a fixed width with `pad_char` (single-byte char code).
// If s is already >= total_width chars, returns a fresh copy (no
// truncation). pad_start prepends; pad_end appends. Allocation
// behaviour is uniform across both branches so callers always
// release the result the same way.
AetherString* string_pad_start(const void* s, int total_width, int pad_char);
AetherString* string_pad_end(const void* s, int total_width, int pad_char);

// Parsing (string -> number) — raw form with out-parameter.
// Returns 1 on success, 0 on failure. Result stored in out_value.
// Aether code should prefer the Go-style wrappers in std.string
// (`string.to_int`, etc.) which return `(value, error)` tuples.
int string_to_int_raw(const void* str, int* out_value);
int string_to_long_raw(const void* str, long long* out_value);  // 64-bit slot (Windows `long` is 32-bit)
int string_to_float_raw(const void* str, float* out_value);
int string_to_double_raw(const void* str, double* out_value);

// Base-N parse, radix in 2..36 (strtoll's range). Out-slot is
// `long long*` for LLP64 safety, matching string_to_long_raw. No
// "0x" / "0b" prefix recognition — caller passes the digit-only
// substring. Returns 1 on success, 0 on null/empty/radix-out-of-range
// /parse-failure/overflow/trailing-garbage.
int string_to_int_radix_raw(const void* str, int radix, long long* out_value);

// Split-return helpers used by the Go-style wrappers. `_try` returns
// 1 if parseable, 0 otherwise. `_get` returns the parsed value or
// zero-value on failure.
int    string_try_int(const void* s);
int    string_get_int(const void* s);
int       string_try_long(const void* s);
long long string_get_long(const void* s);  // 64-bit on every platform
int    string_try_float(const void* s);
double string_get_float(const void* s);  // 64-bit: Aether `float` lowers to C double
int    string_try_double(const void* s);
double string_get_double(const void* s);
// Base-N split-return — same shape as try_long/get_long, with a radix
// parameter (2..36). 64-bit out-slot; safe across LLP64.
int       string_try_int_radix(const void* s, int radix);
long long string_get_int_radix(const void* s, int radix);

// Formatting (printf-style — C-only, varargs)
AetherString* string_format(const char* fmt, ...);

// Aether-callable formatter. Walks `fmt` substituting `{}` with each
// entry of `args` (an Aether ArrayList of strings). `{{` and `}}`
// are literal braces. Returns a refcounted AetherString. Closes #272.
AetherString* string_format_list(const char* fmt, void* args);

// Glob-pattern matching: does `pattern` match `s`? Aether-side
// equivalent of POSIX fnmatch(3). Pattern syntax:
//   - `*`          — zero or more characters
//   - `?`          — exactly one character
//   - `[abc]`      — any of the listed characters (char class)
//   - `[a-z]`      — character range
//   - `[!abc]`     — negated class (also `[^abc]`)
//   - `\*` / `\?`  — literal `*` / `?`
// Used for svn:ignore matching and similar string-pattern globs —
// distinct from `fs.glob` (which enumerates matching files on disk).
//
// `flags` mirrors the libc fnmatch flag word; pass 0 for the plain
// form, or 1 (FNM_PATHNAME on POSIX) for the path-aware form where
// `*` / `?` / `[…]` do not match `/`. Returns 1 on match, 0 on
// no-match, -1 on glob-syntax error (unmatched bracket).
// Section A.2 of aether_changes_needed.md.
// `pattern` and `s` accept either a magic AetherString* or a plain
// const char* — dispatched internally via str_data, matching the
// rest of the string_* family. Required because the call-site #297
// auto-unwrap skips string_*-prefixed externs, so the args arrive
// in their native representation.
int string_glob_match_raw(const void* pattern, const void* s, int flags);

#endif // AETHER_STRING_H
