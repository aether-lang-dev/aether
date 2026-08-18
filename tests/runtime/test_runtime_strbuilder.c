/* C-side regression harness for aether_strbuilder_vappend_format —
 * the va_list-accepting companion to append_format. This is a pure
 * C-FFI primitive (no Aether-side wrapper, since va_list does not
 * cross the Aether boundary), so it can only be exercised from C:
 * the Aether-side strbuilder surface is covered separately in
 * std/strbuilder/test_strbuilder.ae.
 *
 * Five cases, mirroring the ask in new_stringbuilder_ask.md:
 *   1. byte-identical to append_format across the conversion set
 *   2. slow path — output exceeds the 256-byte scratch
 *   3. caller owns va_end (the documented consumption contract)
 *   4. caller-side va_copy retry stays clean
 *   5. NULL / empty inputs
 */

#include "test_harness.h"
#include "../../std/strbuilder/aether_strbuilder.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal variadic shim: exactly the shape a downstream C function
 * (js_parse_error, cprintf, ...) uses to bridge into Aether. */
static int shim_vappend(AetherStrBuilder* b, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rc = aether_strbuilder_vappend_format(b, fmt, ap);
    va_end(ap);
    return rc;
}

/* ---- Case 1 — byte-identical to append_format -------------------- */
TEST_CATEGORY(strbuilder_vappend_format_byte_identical, TEST_CATEGORY_STDLIB) {
    AetherStrBuilder* b1 = aether_strbuilder_new(64);
    AetherStrBuilder* b2 = aether_strbuilder_new(64);
    ASSERT_NOT_NULL(b1);
    ASSERT_NOT_NULL(b2);

    /* Exercises %s / %d / %lld / %x / %c / %.Nf / literal %%. */
    int rc1 = aether_strbuilder_append_format(b1,
        "id=%d name=%s hex=%x ch=%c pct=%.2f%% big=%lld",
        42, "alpha", 0xCAFE, '!', 3.14159, (long long)1LL << 40);
    int rc2 = shim_vappend(b2,
        "id=%d name=%s hex=%x ch=%c pct=%.2f%% big=%lld",
        42, "alpha", 0xCAFE, '!', 3.14159, (long long)1LL << 40);
    ASSERT_EQ(rc1, 1);
    ASSERT_EQ(rc2, 1);

    ASSERT_EQ(aether_strbuilder_length(b1), aether_strbuilder_length(b2));

    _tuple_ptr_int t1 = aether_strbuilder_finish_with_length(b1);
    _tuple_ptr_int t2 = aether_strbuilder_finish_with_length(b2);
    ASSERT_EQ(t1._1, t2._1);
    ASSERT_EQ(memcmp(t1._0, t2._0, (size_t)t1._1), 0);
    free(t1._0);
    free(t2._0);
}

/* ---- Case 2 — slow path: output exceeds the 256-byte scratch ----- */
TEST_CATEGORY(strbuilder_vappend_format_slow_path, TEST_CATEGORY_STDLIB) {
    char filler[400];
    memset(filler, 'X', sizeof(filler) - 1);
    filler[sizeof(filler) - 1] = '\0';   /* 399 X's */

    AetherStrBuilder* b = aether_strbuilder_new(0);
    ASSERT_NOT_NULL(b);
    int rc = shim_vappend(b, "prefix:%s:suffix", filler);
    ASSERT_EQ(rc, 1);
    /* "prefix:" (7) + 399 X + ":suffix" (7) = 413, well past 256. */
    ASSERT_EQ(aether_strbuilder_length(b), 7 + 399 + 7);

    /* Reference output via plain vsnprintf into a malloc'd buffer. */
    int n = snprintf(NULL, 0, "prefix:%s:suffix", filler);
    char* ref = (char*)malloc((size_t)n + 1);
    ASSERT_NOT_NULL(ref);
    snprintf(ref, (size_t)n + 1, "prefix:%s:suffix", filler);

    _tuple_ptr_int got = aether_strbuilder_finish_with_length(b);
    ASSERT_EQ(got._1, n);
    ASSERT_EQ(memcmp(got._0, ref, (size_t)n), 0);
    free(got._0);
    free(ref);
}

/* ---- Case 3 — caller owns va_end (consumption contract) ---------- */
static int once(AetherStrBuilder* b, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rc = aether_strbuilder_vappend_format(b, fmt, ap);
    /* vappend does NOT va_end on the caller's behalf — caller owns it. */
    va_end(ap);
    return rc;
}

TEST_CATEGORY(strbuilder_vappend_format_caller_owns_va_end, TEST_CATEGORY_STDLIB) {
    AetherStrBuilder* b = aether_strbuilder_new(32);
    ASSERT_NOT_NULL(b);
    ASSERT_EQ(once(b, "%d %s", 7, "ok"), 1);

    _tuple_ptr_int got = aether_strbuilder_finish_with_length(b);
    ASSERT_EQ(got._1, 4);
    ASSERT_EQ(memcmp(got._0, "7 ok", 4), 0);
    free(got._0);
}

/* ---- Case 4 — caller-side va_copy retry stays clean -------------- */
static void two_builders_via_va_copy(AetherStrBuilder* b1, AetherStrBuilder* b2,
                                     const char* fmt, ...) {
    va_list ap, saved;
    va_start(ap, fmt);
    va_copy(saved, ap);
    aether_strbuilder_vappend_format(b1, fmt, ap);
    va_end(ap);
    aether_strbuilder_vappend_format(b2, fmt, saved);
    va_end(saved);
}

TEST_CATEGORY(strbuilder_vappend_format_va_copy_retry, TEST_CATEGORY_STDLIB) {
    AetherStrBuilder* b1 = aether_strbuilder_new(32);
    AetherStrBuilder* b2 = aether_strbuilder_new(32);
    ASSERT_NOT_NULL(b1);
    ASSERT_NOT_NULL(b2);

    two_builders_via_va_copy(b1, b2, "v=%d msg=%s", 99, "retry");

    _tuple_ptr_int t1 = aether_strbuilder_finish_with_length(b1);
    _tuple_ptr_int t2 = aether_strbuilder_finish_with_length(b2);
    ASSERT_EQ(t1._1, t2._1);
    ASSERT_EQ(memcmp(t1._0, t2._0, (size_t)t1._1), 0);
    free(t1._0);
    free(t2._0);
}

/* A va_copy retry that also crosses the slow path (>256 bytes) on
 * both legs — confirms the internal probe va_copy in vappend_format
 * does not corrupt the caller's surviving copy. */
TEST_CATEGORY(strbuilder_vappend_format_va_copy_retry_slow, TEST_CATEGORY_STDLIB) {
    char filler[400];
    memset(filler, 'Z', sizeof(filler) - 1);
    filler[sizeof(filler) - 1] = '\0';

    AetherStrBuilder* b1 = aether_strbuilder_new(0);
    AetherStrBuilder* b2 = aether_strbuilder_new(0);
    two_builders_via_va_copy(b1, b2, "[%s]", filler);

    _tuple_ptr_int t1 = aether_strbuilder_finish_with_length(b1);
    _tuple_ptr_int t2 = aether_strbuilder_finish_with_length(b2);
    ASSERT_EQ(t1._1, 401);   /* '[' + 399 Z + ']' */
    ASSERT_EQ(t1._1, t2._1);
    ASSERT_EQ(memcmp(t1._0, t2._0, (size_t)t1._1), 0);
    free(t1._0);
    free(t2._0);
}

/* ---- Case 5 — NULL / empty inputs -------------------------------- */
static int call_with_null_b(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rc = aether_strbuilder_vappend_format(NULL, fmt, ap);
    va_end(ap);
    return rc;
}

static int call_with_null_fmt(AetherStrBuilder* b, ...) {
    va_list ap;
    va_start(ap, b);   /* last named arg */
    int rc = aether_strbuilder_vappend_format(b, NULL, ap);
    va_end(ap);
    return rc;
}

static int call_with_empty_fmt(AetherStrBuilder* b, ...) {
    va_list ap;
    va_start(ap, b);
    int rc = aether_strbuilder_vappend_format(b, "", ap);
    va_end(ap);
    return rc;
}

TEST_CATEGORY(strbuilder_vappend_format_null_and_empty, TEST_CATEGORY_STDLIB) {
    AetherStrBuilder* b = aether_strbuilder_new(8);
    ASSERT_NOT_NULL(b);

    /* NULL builder / NULL fmt return 0 without crashing. */
    ASSERT_EQ(call_with_null_b("x"), 0);
    ASSERT_EQ(call_with_null_fmt(b), 0);
    ASSERT_EQ(aether_strbuilder_length(b), 0);

    /* Empty format string appends zero bytes, returns 1. */
    ASSERT_EQ(call_with_empty_fmt(b), 1);
    ASSERT_EQ(aether_strbuilder_length(b), 0);

    aether_strbuilder_free(b);
}
