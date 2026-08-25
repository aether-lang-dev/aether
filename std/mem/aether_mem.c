/* aether_mem.c — byte-level access to caller-allocated raw pointers.
 *
 * Companion to std.bytes (Aether-managed mutable buffers). std.mem
 * is for the case where the buffer is NOT Aether's — a raw char*
 * passed across an FFI boundary, a malloc'd block from a C call,
 * a buffer the platform handed back. Aether code needs to read/
 * write bytes at offsets without owning the storage.
 *
 * Module name: `std.mem` rather than `std.ptr` because `ptr` is a
 * reserved keyword in Aether (the raw-pointer type). `mem` reads
 * cleanly as "raw memory access" alongside `std.bytes` ("owned
 * mutable bytes").
 *
 * Initial use case: porting C codebases (mquickjs, similar) that
 * fundamentally operate on raw pointers — every UTF-8 decoder,
 * bytecode interpreter, packed-format parser ends up doing
 * `((uint8_t*)p)[i]`. Aether parses `p[i]` syntactically but
 * lowers it to `*(void*)p` which is invalid C. mem.get_byte /
 * set_byte give the same result through an extern detour.
 *
 * No bounds checking — the caller owns the buffer and knows its
 * size. If the caller passes garbage, the program crashes the
 * same way it would in C. Mirrors POSIX read(2) / write(2)
 * semantics — the kernel doesn't second-guess your buffer.
 *
 * Sized typed-array accessors (int8/uint8/int16/uint16/uint32/
 * float32/float64) ship at the bottom — added once mquickjs's
 * TypedArray load/store paths surfaced the need.  Same byte-offset
 * convention as the rest of the module.
 *
 * Future additions: pointer arithmetic helpers (add(p,n), sub(p,q)),
 * little-endian variants when a target needs them. Ship as concrete
 * needs surface.
 */

#include <stdint.h>
#include <stddef.h>

/* Read unsigned byte at offset i. Returns 0..255. Returns -1 if
 * `p` is NULL — sole defensive check, since a NULL deref would
 * SIGSEGV. Negative or too-large `i` is the caller's problem. */
int aether_mem_get_byte(void* p, int i) {
    if (!p) return -1;
    return (int)((uint8_t*)p)[i];
}

/* Write low 8 bits of `value` at offset i. Returns 1 on success,
 * 0 if `p` is NULL. Same NULL-only defence as get_byte. */
int aether_mem_set_byte(void* p, int i, int value) {
    if (!p) return 0;
    ((uint8_t*)p)[i] = (uint8_t)(value & 0xff);
    return 1;
}

/* size_t-indexed companions to get_byte / set_byte. The `int` index
 * tops out at 2 GiB; codebases whose natural index width is size_t
 * (Redis SDS, lzf, siphash, syncio, anything walking large mmap'd
 * regions) need the wider form to avoid a per-port `_get_byte`
 * shim that exists only to declare a size_t parameter. Aether has
 * no primitive narrowing via `as`, so the caller cannot convert
 * size_t to int at the call site — the only fix is to take the
 * wider index here. Same NULL defence; out-of-range `i` still the
 * caller's problem. */
int aether_mem_get_byte_sz(void* p, size_t i) {
    if (!p) return -1;
    return (int)((uint8_t*)p)[i];
}

int aether_mem_set_byte_sz(void* p, size_t i, int value) {
    if (!p) return 0;
    ((uint8_t*)p)[i] = (uint8_t)(value & 0xff);
    return 1;
}

/* Read a void*-sized pointer value at byte offset `offset` of `p`.
 * Returns NULL if `p` is NULL or the loaded value is itself NULL.
 * Note: returning NULL conflates "p was null" with "the loaded value
 * was null" — callers that need to distinguish should check `p` first.
 *
 * Use for C-side `(void**)p`-style out-parameters: when a C function
 * (e.g. mquickjs's strstart's `const char **ptr`) writes a pointer
 * through a slot the Aether caller passed in, the read-back uses
 * this. Caller is responsible for the offset being aligned for
 * pointer-size access. */
void* aether_mem_get_ptr(void* p, int offset) {
    if (!p) return NULL;
    return *(void**)((char*)p + offset);
}

/* Write `value` (a pointer) into the slot at byte offset `offset`
 * of `p`. Returns 1 on success, 0 if `p` is NULL. The slot must be
 * pointer-aligned and at least `sizeof(void*)` bytes (caller's
 * responsibility — same contract as the rest of std.mem).
 *
 * Drives the `*(void**)out_pp = value` pattern for out-parameters
 * across the FFI boundary, used by ported C functions whose
 * signatures take `T**` arguments. */
int aether_mem_set_ptr(void* p, int offset, void* value) {
    if (!p) return 0;
    *(void**)((char*)p + offset) = value;
    return 1;
}

/* Read an int64_t at byte offset `offset` of `p`. Returns 0 if `p`
 * is NULL — note this conflates "p is null" with "stored value is 0";
 * callers that need to distinguish should null-check `p` first.
 *
 * Sized to int64_t (not long, not size_t) — `long` is 4 bytes on
 * 64-bit Windows and 8 bytes elsewhere, which would be an ABI
 * landmine. int64_t is 8 bytes everywhere. On 64-bit Linux/macOS
 * (mquickjs's targets), size_t is also 8 bytes, so this matches a
 * `size_t*` slot at the C boundary.
 *
 * Driver use case: C functions with `size_t *plen` out-parameters
 * (mquickjs's __unicode_from_utf8 and friends). Aether caller
 * mallocs a slot, the C signature sees size_t*, the Aether code
 * reads/writes via this. */
int64_t aether_mem_get_long(void* p, int offset) {
    if (!p) return 0;
    return *(int64_t*)((char*)p + offset);
}

/* Write `value` as an int64_t into the slot at byte offset `offset`
 * of `p`. Returns 1 on success, 0 if `p` is NULL. Slot must be
 * 8-byte-aligned and at least 8 bytes (caller's responsibility). */
int aether_mem_set_long(void* p, int offset, int64_t value) {
    if (!p) return 0;
    *(int64_t*)((char*)p + offset) = value;
    return 1;
}

/* Read an int32_t at byte offset `offset` of `p`. Returns 0 if `p`
 * is NULL — same conflation caveat as the long versions. Slot must
 * be 4-byte-aligned. Driver use case: C functions with `int *out`
 * out-parameters (mquickjs's get_pc2line et al). */
int aether_mem_get_int(void* p, int offset) {
    if (!p) return 0;
    return *(int32_t*)((char*)p + offset);
}

/* Write `value` as an int32_t at byte offset `offset` of `p`.
 * Returns 1 on success, 0 if `p` is NULL. */
int aether_mem_set_int(void* p, int offset, int value) {
    if (!p) return 0;
    *(int32_t*)((char*)p + offset) = (int32_t)value;
    return 1;
}

/* Count leading zeros in a 32-bit value. Undefined behavior if
 * value is 0 — same contract as __builtin_clz, which this wraps.
 * For Aether-side use the bit pattern only; signedness doesn't
 * matter for the count. */
int aether_mem_clz32(int value) {
    return __builtin_clz((unsigned int)value);
}

/* Count leading zeros in a 64-bit value. UB if value is 0. */
int aether_mem_clz64(int64_t value) {
    return __builtin_clzll((unsigned long long)value);
}

/* Invoke a C-style function pointer with a (size_t, size_t, void*)
 * signature, returning int. Bridges Aether's `fn` (which lowers to
 * a closure struct, not a bare function pointer) with C callers
 * that pass real bare-fnptr callbacks.
 *
 * Driver: porting C functions like rqsort_idx that receive
 * comparator + swap callbacks from C and need to invoke them.
 * The Aether port receives the callback as a `ptr` (the bare fn
 * address), and calls back through this shim.
 *
 * Caller is responsible for matching the actual signature. v1
 * supports just the 3-param shape used by rqsort_idx; future
 * shapes can grow alongside additional ports. */
int aether_mem_call_fn3_int(void* fn, int64_t a, int64_t b, void* opaque) {
    if (!fn) return 0;
    typedef int (*fn_t)(size_t, size_t, void*);
    return ((fn_t)fn)((size_t)a, (size_t)b, opaque);
}

/* Same shape but for void-returning callbacks (e.g. swap fn). */
void aether_mem_call_fn3_void(void* fn, int64_t a, int64_t b, void* opaque) {
    if (!fn) return;
    typedef void (*fn_t)(size_t, size_t, void*);
    ((fn_t)fn)((size_t)a, (size_t)b, opaque);
}

/* Two-arg fnptr shim, void return. Driver case: GC user-finalizer
 * tables in the mquickjs port are arrays of `void (*)(JSContext*,
 * void*)`. The host context pointer and the per-object opaque both
 * cross the boundary as `ptr`. */
void aether_mem_call_fn2_void(void* fn, void* a, void* b) {
    if (!fn) return;
    typedef void (*fn_t)(void*, void*);
    ((fn_t)fn)(a, b);
}

/* Unsigned 64-bit divide by 32-bit divisor. Returns the quotient
 * (low 32 bits) and writes the remainder to *premainder.
 *
 * Aether's `long` is signed int64. Doing `long / int` for a
 * dividend with bit 63 set produces wrong results because gcc
 * sign-extends the dividend before the division. This wrapper
 * does a true unsigned division by reinterpreting both operands
 * as unsigned.
 *
 * Driver: dtoa's mp_div1 — divides a uint64 (built from two
 * uint32 limbs) by a uint32 divisor for each step of the
 * multi-precision long-division loop. The high limb shifted left
 * 32 + low limb routinely has bit 63 set, so signed division
 * fails; this primitive supplies the unsigned semantic. */
int aether_mem_udiv64_32(int64_t dividend, int divisor, void* premainder) {
    if (!premainder || divisor == 0) return 0;
    uint64_t a = (uint64_t)dividend;
    uint32_t b = (uint32_t)divisor;
    uint32_t q = (uint32_t)(a / b);
    uint32_t r = (uint32_t)(a % b);
    *(int32_t*)premainder = (int32_t)r;
    return (int32_t)q;
}

/* Reinterpret an IEEE-754 double's bit pattern as a 64-bit integer.
 * Type-puns via memcpy (UB-safe in C; the canonical idiom for this
 * since C99). NaN payloads, signed-zero, infinities — all preserved
 * exactly, since this is a bitwise copy.
 *
 * Aether `float` lowers to C `double` (8 bytes, IEEE-754 binary64),
 * so `value: float` here matches libm's `double` argument.
 *
 * Driver use case: porting mquickjs's libm.c — every operation
 * that reads exponent, sign, mantissa goes through
 * `float64_as_uint64(d)` (defined as a union-pun in cutils.h).
 * Aether has no `union { double; int64; }` equivalent, so this
 * extern is the same primitive. */
int64_t aether_mem_bits_of_float(double value) {
    int64_t bits;
    /* memcpy is the strict-aliasing-safe way to reinterpret. Modern
     * compilers (gcc -O1+, clang -O1+) emit a single `movq xmm,r`
     * instruction — same code as the union-pun, just without the
     * aliasing footgun. */
    __builtin_memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/* Inverse of aether_mem_bits_of_float — reconstitute a double from
 * its 64-bit bit pattern. Same memcpy idiom, same compiled code. */
double aether_mem_float_from_bits(int64_t bits) {
    double value;
    __builtin_memcpy(&value, &bits, sizeof(value));
    return value;
}

/* Sized typed-array accessors. Aether ports of C code that walks
 * typed-array element buffers (mquickjs's TypedArray load/store paths)
 * need int8/uint8/int16/uint16/uint32/float32/float64 reads at byte
 * offsets. Without these, every port adds bespoke C wrappers — the
 * mquickjs leftover_bundle ended up adding 8 mqjs_load_* helpers
 * before this surfaced.
 *
 * Naming convention matches std.mem's existing get_byte/get_int/
 * get_long: `get_<type>(p, offset_bytes)`. The byte offset is
 * literal; the caller is responsible for alignment when targeting
 * strict-alignment platforms (mquickjs TypedArrays are
 * alignment-guaranteed by their constructor's offset computation,
 * so this is safe in the driver case).
 *
 * NULL `p` returns 0 (or 0.0) — same conflation caveat as the
 * existing get_int/get_long. Out-of-range offset is the caller's
 * problem.
 *
 * `get_byte` already covers the uint8 case (returns 0..255); we add
 * a parallel `get_uint8` so type-driven code reads naturally
 * (caller doesn't have to mentally translate "byte" to "uint8").
 * `get_int8` is the new sign-extending variant that returns -128..127.
 *
 * Float reads widen to C `double` (Aether `float`); float writes
 * narrow on store. The standard IEEE rounding rules apply to the
 * narrowing — the C compiler emits the platform's float instruction.
 */
int aether_mem_get_int8(void* p, int offset) {
    if (!p) return 0;
    return (int)((int8_t*)p)[offset];
}
int aether_mem_get_uint8(void* p, int offset) {
    if (!p) return 0;
    return (int)((uint8_t*)p)[offset];
}
int aether_mem_set_int8(void* p, int offset, int value) {
    if (!p) return 0;
    ((int8_t*)p)[offset] = (int8_t)(value & 0xff);
    return 1;
}
int aether_mem_set_uint8(void* p, int offset, int value) {
    if (!p) return 0;
    ((uint8_t*)p)[offset] = (uint8_t)(value & 0xff);
    return 1;
}

int aether_mem_get_int16(void* p, int offset) {
    if (!p) return 0;
    int16_t v;
    __builtin_memcpy(&v, (char*)p + offset, sizeof(v));
    return (int)v;
}
int aether_mem_get_uint16(void* p, int offset) {
    if (!p) return 0;
    uint16_t v;
    __builtin_memcpy(&v, (char*)p + offset, sizeof(v));
    return (int)v;
}
int aether_mem_set_int16(void* p, int offset, int value) {
    if (!p) return 0;
    int16_t v = (int16_t)(value & 0xffff);
    __builtin_memcpy((char*)p + offset, &v, sizeof(v));
    return 1;
}
int aether_mem_set_uint16(void* p, int offset, int value) {
    if (!p) return 0;
    uint16_t v = (uint16_t)(value & 0xffff);
    __builtin_memcpy((char*)p + offset, &v, sizeof(v));
    return 1;
}

/* uint32 read/write, returned and taken as int64 (Aether `long`).
 *
 * This used to return `int`, which cannot represent the top half of the
 * range it exists to read: 0xFFFFFFFF came back as -1 (#1699). The bit
 * pattern was right and the comment told callers to widen via long
 * themselves, but nothing enforced it, and the sibling accessors do not
 * ask: get_uint16 fits its range in an int, and get_u32_le/get_u32_be
 * already return int64_t for exactly this reason. The same four bytes
 * therefore read as 4294967295 through get_u32_le and -1 through
 * get_uint32 — an inconsistency between accessors of the same width.
 *
 * The setter takes int64 to match, masking to 32 bits on the way in, so
 * set_uint32(p, off, 4294967295) round-trips through get_uint32 rather
 * than needing the caller to pass -1.
 *
 * The signed counterpart already exists as get_int / set_int. */
int64_t aether_mem_get_uint32(void* p, int offset) {
    if (!p) return 0;
    uint32_t v;
    __builtin_memcpy(&v, (char*)p + offset, sizeof(v));
    return (int64_t)v;
}
int aether_mem_set_uint32(void* p, int offset, int64_t value) {
    if (!p) return 0;
    uint32_t v = (uint32_t)(value & 0xFFFFFFFF);
    __builtin_memcpy((char*)p + offset, &v, sizeof(v));
    return 1;
}

/* Float32 / float64 reads at a byte offset.  Float32 widens to
 * C `double` (Aether `float`); the IEEE-754 round-to-nearest is
 * implicit in the cast. Float64 is exact (no conversion). */
double aether_mem_get_float32(void* p, int offset) {
    if (!p) return 0.0;
    float v;
    __builtin_memcpy(&v, (char*)p + offset, sizeof(v));
    return (double)v;
}
double aether_mem_get_float64(void* p, int offset) {
    if (!p) return 0.0;
    double v;
    __builtin_memcpy(&v, (char*)p + offset, sizeof(v));
    return v;
}
int aether_mem_set_float32(void* p, int offset, double value) {
    if (!p) return 0;
    float v = (float)value;
    __builtin_memcpy((char*)p + offset, &v, sizeof(v));
    return 1;
}
int aether_mem_set_float64(void* p, int offset, double value) {
    if (!p) return 0;
    __builtin_memcpy((char*)p + offset, &value, sizeof(value));
    return 1;
}

/* Reinterpret a raw pointer as a 64-bit integer address. Driver case:
 * porting C code that does `(uintptr_t)p1 < (uintptr_t)p2` — bound
 * checks against memory-region boundaries. Aether's `ptr < ptr`
 * already lowers to the right C `<` on pointers, but some patterns
 * (tag-bit testing on a tagged-pointer-as-int representation that
 * came back through mem.get_long) need the full integer view, then
 * convert back to ptr for the deref. */
int64_t aether_mem_ptr_to_long(void* p) {
    return (int64_t)(uintptr_t)p;
}

/* Inverse of aether_mem_ptr_to_long. Used after stripping a tag-bit
 * from a JSValue-style tagged pointer that was loaded as a long
 * (so the tag arithmetic could happen as integer ops); the un-tagged
 * address then gets converted back to ptr for indexed reads via
 * mem.get_long / mem.get_ptr. */
void* aether_mem_long_to_ptr(int64_t addr) {
    return (void*)(uintptr_t)addr;
}

/* Bulk operations — mem.copy / mem.move / mem.compare / mem.set.
 *
 * Added per docs/redis-porting-language-gaps.md §P2: the Redis port
 * had to reach for libc memcpy/memmove/memcmp/memset via project-
 * specific shims, or hand-roll byte loops in Aether (correct but
 * noisy and slow). These wrappers do exactly what their libc cousins
 * do, with the same arg order and same return-value conventions.
 * Length is `size_t` on the C side, lowered from Aether `long` —
 * negative lengths are passed through to libc, which is undefined
 * behaviour (same contract as calling memcpy with a negative size
 * directly).
 *
 * NULL pointer is defended against (no-op for void-returning;
 * mem.compare returns 0 on either-NULL — distinguishable from "equal
 * bytes" only by the caller checking pointers themselves; mem.set
 * returns NULL).
 *
 * These are the bulk primitives. They differ from std.bytes (which
 * owns its buffer) and from get/set_byte (which moves one byte at a
 * time): mem.copy etc. operate on caller-owned buffers in libc-grade
 * bulk. */
void* aether_mem_copy(void* dst, const void* src, int64_t n) {
    if (!dst || !src) return dst;
    return __builtin_memcpy(dst, src, (size_t)n);
}
void* aether_mem_move(void* dst, const void* src, int64_t n) {
    if (!dst || !src) return dst;
    return __builtin_memmove(dst, src, (size_t)n);
}
int aether_mem_compare(const void* a, const void* b, int64_t n) {
    if (!a || !b) return 0;
    return __builtin_memcmp(a, b, (size_t)n);
}
void* aether_mem_set_bulk(void* dst, int value, int64_t n) {
    if (!dst) return dst;
    return __builtin_memset(dst, value, (size_t)n);
}

/* Offset forms of the three bulk writers (#1733 ask 2).
 *
 * The offset-less versions above can only start at byte 0 of each buffer,
 * and Aether has no pointer arithmetic — so there was no way to express a
 * bulk operation on an INTERIOR span. Copying one scanline of a sub-region
 * out of a larger framebuffer, or filling a rect's row, had to go byte at a
 * time through get_byte/set_byte even though the operation is memcpy-shaped.
 *
 * Contracts match their offset-less twins exactly: NULL in either buffer is
 * a no-op returning `dst`, and the offsets are the caller's problem in the
 * same way `n` already is — these add no range checking, because the
 * offset-less forms do none either and a bulk primitive that silently
 * clamped would be worse than one that does what it is told.
 *
 * `move_at` exists alongside `copy_at` rather than after it: an image
 * scrolling within its own buffer is an OVERLAPPING interior copy, so a
 * copy_at-only surface would make the common case undefined behaviour. */
void* aether_mem_copy_at(void* dst, int64_t dst_off,
                         const void* src, int64_t src_off, int64_t n) {
    if (!dst || !src) return dst;
    return __builtin_memcpy((char*)dst + dst_off, (const char*)src + src_off,
                            (size_t)n);
}

void* aether_mem_move_at(void* dst, int64_t dst_off,
                         const void* src, int64_t src_off, int64_t n) {
    if (!dst || !src) return dst;
    return __builtin_memmove((char*)dst + dst_off, (const char*)src + src_off,
                             (size_t)n);
}

void* aether_mem_fill_at(void* dst, int64_t off, int value, int64_t n) {
    if (!dst) return dst;
    return __builtin_memset((char*)dst + off, value, (size_t)n);
}

/* Offset form of compare, for symmetry with the writers: comparing an
 * interior span had the same hole. Returns 0 when either buffer is NULL,
 * exactly as aether_mem_compare does. */
int aether_mem_compare_at(const void* a, int64_t a_off,
                          const void* b, int64_t b_off, int64_t n) {
    if (!a || !b) return 0;
    return __builtin_memcmp((const char*)a + a_off, (const char*)b + b_off,
                            (size_t)n);
}

/* Endian-aware 16/32/64-bit load/store at a byte offset.
 *
 * Unaligned-safe — each read/write goes through __builtin_memcpy,
 * so the underlying byte stream can be at any alignment. The shift
 * pattern is the canonical "build the integer from bytes in N-byte
 * order"; modern compilers fold each function down to a single
 * load+bswap (or single load on a matching-endian target).
 *
 * `_le` = little-endian (low byte first); `_be` = big-endian (high
 * byte first). Names match the Redis usage: ziplist/listpack/RDB
 * write little-endian; network frames are big-endian.
 *
 * Offset is bytes from `p` (same convention as the rest of std.mem);
 * the slot must have at least 2/4/8 bytes available. NULL `p` is
 * defended for the loaders (return 0) and the stores (no-op). */
int aether_mem_get_u16_le(const void* p, int offset) {
    if (!p) return 0;
    const uint8_t* b = (const uint8_t*)p + offset;
    return ((int)b[0]) | ((int)b[1] << 8);
}
int aether_mem_get_u16_be(const void* p, int offset) {
    if (!p) return 0;
    const uint8_t* b = (const uint8_t*)p + offset;
    return ((int)b[0] << 8) | ((int)b[1]);
}
int aether_mem_set_u16_le(void* p, int offset, int value) {
    if (!p) return 0;
    uint8_t* b = (uint8_t*)p + offset;
    b[0] = (uint8_t)(value & 0xff);
    b[1] = (uint8_t)((value >> 8) & 0xff);
    return 1;
}
int aether_mem_set_u16_be(void* p, int offset, int value) {
    if (!p) return 0;
    uint8_t* b = (uint8_t*)p + offset;
    b[0] = (uint8_t)((value >> 8) & 0xff);
    b[1] = (uint8_t)(value & 0xff);
    return 1;
}

/* 32-bit return is `int64_t` so the full 0..0xFFFFFFFF range is
 * representable (Aether's `int` is 32-bit signed; bit 31 set would
 * otherwise sign-extend). Callers narrowing to `int` get a value
 * that may look negative for top-bit-set inputs — that matches the
 * direct C behaviour of `int32_t = (int32_t)u32`. */
int64_t aether_mem_get_u32_le(const void* p, int offset) {
    if (!p) return 0;
    const uint8_t* b = (const uint8_t*)p + offset;
    return ((int64_t)b[0])        |
           ((int64_t)b[1] << 8)   |
           ((int64_t)b[2] << 16)  |
           ((int64_t)b[3] << 24);
}
int64_t aether_mem_get_u32_be(const void* p, int offset) {
    if (!p) return 0;
    const uint8_t* b = (const uint8_t*)p + offset;
    return ((int64_t)b[0] << 24)  |
           ((int64_t)b[1] << 16)  |
           ((int64_t)b[2] << 8)   |
           ((int64_t)b[3]);
}
int aether_mem_set_u32_le(void* p, int offset, int64_t value) {
    if (!p) return 0;
    uint8_t* b = (uint8_t*)p + offset;
    b[0] = (uint8_t)(value & 0xff);
    b[1] = (uint8_t)((value >> 8) & 0xff);
    b[2] = (uint8_t)((value >> 16) & 0xff);
    b[3] = (uint8_t)((value >> 24) & 0xff);
    return 1;
}
int aether_mem_set_u32_be(void* p, int offset, int64_t value) {
    if (!p) return 0;
    uint8_t* b = (uint8_t*)p + offset;
    b[0] = (uint8_t)((value >> 24) & 0xff);
    b[1] = (uint8_t)((value >> 16) & 0xff);
    b[2] = (uint8_t)((value >> 8) & 0xff);
    b[3] = (uint8_t)(value & 0xff);
    return 1;
}

int64_t aether_mem_get_u64_le(const void* p, int offset) {
    if (!p) return 0;
    const uint8_t* b = (const uint8_t*)p + offset;
    return ((int64_t)b[0])        |
           ((int64_t)b[1] << 8)   |
           ((int64_t)b[2] << 16)  |
           ((int64_t)b[3] << 24)  |
           ((int64_t)b[4] << 32)  |
           ((int64_t)b[5] << 40)  |
           ((int64_t)b[6] << 48)  |
           ((int64_t)b[7] << 56);
}
int64_t aether_mem_get_u64_be(const void* p, int offset) {
    if (!p) return 0;
    const uint8_t* b = (const uint8_t*)p + offset;
    return ((int64_t)b[0] << 56)  |
           ((int64_t)b[1] << 48)  |
           ((int64_t)b[2] << 40)  |
           ((int64_t)b[3] << 32)  |
           ((int64_t)b[4] << 24)  |
           ((int64_t)b[5] << 16)  |
           ((int64_t)b[6] << 8)   |
           ((int64_t)b[7]);
}
int aether_mem_set_u64_le(void* p, int offset, int64_t value) {
    if (!p) return 0;
    uint8_t* b = (uint8_t*)p + offset;
    for (int i = 0; i < 8; i++) {
        b[i] = (uint8_t)((value >> (i * 8)) & 0xff);
    }
    return 1;
}
int aether_mem_set_u64_be(void* p, int offset, int64_t value) {
    if (!p) return 0;
    uint8_t* b = (uint8_t*)p + offset;
    for (int i = 0; i < 8; i++) {
        b[7 - i] = (uint8_t)((value >> (i * 8)) & 0xff);
    }
    return 1;
}
