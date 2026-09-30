#ifndef AETHER_STRBUILDER_H
#define AETHER_STRBUILDER_H

#include <stddef.h>
#include <stdarg.h>

/* std.strbuilder — amortised-O(1) string append primitive.
 *
 * (Module is named `strbuilder` rather than `builder` because the
 * latter is reserved in Aether for configure-then-execute DSL
 * functions. The API mirrors Java/Kotlin StringBuilder, Go's
 * strings.Builder, Rust's String::push_str.)
 *
 * The canonical accumulator idiom `out = string.concat(out, chunk)`
 * in a loop is O(N²) per byte because every concat reallocates and
 * memcpy's the full prefix. A correctly-tracked heap-string just
 * means each leak is freed promptly; the algorithmic cost stays.
 *
 * AetherStrBuilder solves that with a growing internal buffer (2×
 * doubling on overflow, amortised-O(1) per byte appended). The
 * lifecycle is:
 *
 *     b = strbuilder.new(cap_hint)
 *     strbuilder.append(b, chunk)     // any number of times
 *     out = strbuilder.finish(b)      // hand the buffer off; b invalid
 *
 * strbuilder.finish hands the accumulated bytes to a refcounted
 * AetherString and destroys the builder. The returned string is
 * heap-owned (caller frees / heap-tracker reclaims at scope exit).
 * If a caller decides mid-build to abandon the work,
 * strbuilder.free(b) discards without producing a string.
 *
 * The append surface mirrors what's needed by the avn-style
 * dir-blob, JSON / log / template / packed-record building patterns
 * — bytes, full strings, int decimals, and explicit-length strings
 * for binary-safe content (embedded NULs round-trip). */

typedef struct AetherStrBuilder AetherStrBuilder;

/* Allocate a new builder with at least `cap_hint` bytes reserved. If
 * `cap_hint` is <= 0, a small default (64) is used. Returns NULL on
 * OOM / cap exceeded. Initial logical length is 0. */
AetherStrBuilder* aether_strbuilder_new(int cap_hint);

/* Append a string. Length is taken from the AetherString header when
 * `s` is a refcounted string, else via strlen. Returns 1 on success,
 * 0 on failure (NULL builder, NULL s, OOM, or cap exceeded). */
int aether_strbuilder_append(AetherStrBuilder* b, const void* s);

/* Append exactly `n` bytes from `s`. Binary-safe — embedded NULs
 * round-trip. `s` may be AetherString* (read via aether_string_data)
 * or plain `const char*`. Returns 1 on success, 0 on failure
 * (negative n, NULL s, OOM, etc.). n == 0 is a no-op success. */
int aether_strbuilder_append_n(AetherStrBuilder* b, const void* s, int n);

/* Append `n` raw bytes from `data` (#2301): the landing point for a `byte[]`
 * slice. Unlike append_n, `data` is never read as an AetherString, so binary
 * bytes that happen to look like a string header are copied as they are.
 * Returns 1 on success, 0 on failure (NULL builder or data, negative n,
 * OOM). */
int aether_strbuilder_append_bytes(AetherStrBuilder* b, const void* data, int n);

/* Append a single byte (low 8 bits of `c`). Useful for separators
 * ('\n', ',', 0x02, ...). Returns 1 on success, 0 on failure. */
int aether_strbuilder_append_byte(AetherStrBuilder* b, int c);

/* Append the decimal representation of `v` (no padding, with a
 * leading '-' for negatives). Avoids the round-trip through
 * string.from_int + strbuilder.append. Returns 1 on success, 0 on
 * failure. */
int aether_strbuilder_append_int(AetherStrBuilder* b, int v);

/* Append the decimal representation of a 64-bit `v`. Sibling of
 * append_int for IDs, timestamps, and offsets that overflow 32 bits.
 * Returns 1 on success, 0 on failure. */
int aether_strbuilder_append_long(AetherStrBuilder* b, long long v);

/* Append `v` as lowercase hexadecimal. `width` is a MINIMUM nibble
 * count: 0 = natural width, >0 = zero-pad to that many nibbles; a
 * value needing more nibbles than `width` grows rather than
 * truncates. Negative `v` is encoded as its unsigned bit pattern.
 * Returns 1 on success, 0 on failure (negative width, overflow). */
int aether_strbuilder_append_hex(AetherStrBuilder* b, long long v, int width);

/* UTF-8 encode the Unicode code point `cp` (1–4 bytes) and append it.
 * Returns 0 — appending nothing — for invalid code points: negative
 * values, UTF-16 surrogate halves (0xD800–0xDFFF), and anything past
 * 0x10FFFF. Returns 1 on success. */
int aether_strbuilder_append_codepoint(AetherStrBuilder* b, int cp);

/* printf-style formatted append. Formats `fmt` + trailing varargs
 * with vsnprintf and appends the result. A 256-byte stack scratch
 * covers the common case in one pass; longer output reserves builder
 * space and formats directly into the tail. Returns 1 on success, 0
 * on failure (NULL args, encoding error, OOM). Thin wrapper over
 * `aether_strbuilder_vappend_format` — byte-output identical. */
int aether_strbuilder_append_format(AetherStrBuilder* b, const char* fmt, ...);

/* va_list-accepting companion to `append_format`. C-FFI ONLY — there
 * is no Aether-side wrapper because a va_list cannot cross the Aether
 * boundary. It exists so that a handwritten variadic C shim (which
 * already holds a va_list from its own va_start) can delegate its
 * formatting to a strbuilder, then route the finished bytes back to
 * Aether — the C->Aether bridge for the *defining* side of the
 * variadic story (Aether deliberately has no defining-varargs).
 *
 * va_list consumption contract: this call CONSUMES the caller's `ap`
 * (matching the vsnprintf contract). The caller must `va_end(ap)`
 * afterwards, and must NOT pass `ap` to a second consumer. A caller
 * that needs to format the same arguments twice (e.g. a retry with a
 * fallback format) must take its own `va_copy` *before* calling:
 *
 *     va_list ap, saved;
 *     va_start(ap, fmt);
 *     va_copy(saved, ap);
 *     aether_strbuilder_vappend_format(b1, fmt, ap);     // consumes ap
 *     va_end(ap);
 *     aether_strbuilder_vappend_format(b2, fmt, saved);  // consumes saved
 *     va_end(saved);
 *
 * Internally a va_copy is taken for the fast-path scratch probe so a
 * 256-byte-overflowing format still has a fresh va_list to re-format
 * from; that is an implementation detail and does not relax the
 * "caller's ap is consumed" contract above. Returns 1 on success, 0
 * on failure (NULL builder, NULL fmt, encoding error, OOM). */
int aether_strbuilder_vappend_format(AetherStrBuilder* b, const char* fmt,
                                     va_list ap);

/* Current accumulated length in bytes, or -1 if `b` is NULL. */
int aether_strbuilder_length(AetherStrBuilder* b);

/* Current buffer capacity in bytes, or -1 if `b` is NULL. The
 * companion to `length` — mainly useful for confirming a `reserve`
 * hint took effect / observing the doubling growth. */
int aether_strbuilder_capacity(AetherStrBuilder* b);

/* Pre-grow the buffer so the next `additional` bytes of appends will
 * not trigger a reallocation. Pure performance hint — the builder
 * grows on demand regardless. `additional` is measured from the
 * current length. Returns 1 on success, 0 on failure (negative
 * `additional`, overflow, OOM). */
int aether_strbuilder_reserve(AetherStrBuilder* b, int additional);

/* Pop accumulated content back to `new_length` bytes. Only shrinks:
 * a `new_length` greater than the current length is rejected (it
 * would expose uninitialised capacity as content). The buffer is
 * retained so a truncate-then-append reuse loop keeps the growth
 * amortisation. Returns 1 on success, 0 on failure (NULL builder,
 * negative or out-of-range `new_length`). */
int aether_strbuilder_truncate(AetherStrBuilder* b, int new_length);

/* Reset accumulated content to empty (equivalent to truncate(b, 0)).
 * The buffer is retained for reuse. Returns 1 on success, 0 if `b`
 * is NULL. */
int aether_strbuilder_clear(AetherStrBuilder* b);

/* Finalise into a refcounted AetherString and destroy the builder.
 * After this call, `b` is invalid (do NOT call any strbuilder.* on
 * it again — not even free). Returns NULL on NULL input or string
 * allocation failure; the builder is still destroyed in the latter
 * case so resources don't leak on an OOM finalise. */
void* aether_strbuilder_finish(AetherStrBuilder* b);

/* Binary-safe finalise. Return shape — layout matches the codegen-
 * emitted `_tuple_ptr_int` typedef for an Aether `(ptr, int)` return.
 * `_0` is the raw data buffer, `_1` its exact byte length. */
typedef struct { void* _0; int _1; } _tuple_ptr_int;

/* Finalise into a raw (buffer, length) pair and destroy the builder.
 * Unlike `finish`, NO NUL terminator is appended — embedded NULs are
 * preserved as content, which is what binary-protocol assembly
 * needs. The caller owns the returned buffer and reclaims it with a
 * plain libc free(); it is NOT codegen-heap-tracked. An empty
 * builder yields a non-NULL 1-byte buffer with length 0; a NULL
 * builder yields {NULL, -1}. After this call `b` is invalid. */
_tuple_ptr_int aether_strbuilder_finish_with_length(AetherStrBuilder* b);

/* Discard the builder without producing a string. Idempotent on
 * NULL. Use this on error paths where the caller has decided
 * partway through that the in-progress output isn't needed. */
void aether_strbuilder_free(AetherStrBuilder* b);

#endif  /* AETHER_STRBUILDER_H */
