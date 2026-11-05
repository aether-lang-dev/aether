/* Shim for the extern-tuple-return regression test (#271). Each
 * function returns a struct-by-value with a layout that matches the
 * `_tuple_<T1>_<T2>` typedef the codegen synthesises from
 * `extern foo(...) -> (T1, T2)`. The layout is order-dependent: the
 * fields are named `_0`, `_1`, … in declaration order. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* (int, int) — minimum smoke test. */
typedef struct { int _0; int _1; } _tuple_int_int;

_tuple_int_int divmod_pair(int n, int d) {
    _tuple_int_int t = { n / d, n % d };
    return t;
}

/* (int, string) — Go-style result + error. */
typedef struct { int _0; const char* _1; } _tuple_int_string;

_tuple_int_string parse_int_safe(const char* s) {
    _tuple_int_string t;
    if (!s || !*s) { t._0 = 0; t._1 = "empty"; return t; }
    char* end;
    long n = strtol(s, &end, 10);
    if (end == s) { t._0 = 0; t._1 = "not a number"; return t; }
    t._0 = (int)n;
    t._1 = "";
    return t;
}

/* (ptr, int, string) — same shape fs_read_binary_tuple uses. Confirms
 * 3-element tuples work and that the (ptr, int, string) ordering
 * matches what the codegen names `_tuple_ptr_int_string`. */
typedef struct { void* _0; int _1; const char* _2; } _tuple_ptr_int_string;

_tuple_ptr_int_string echo_bytes(const char* s, int len) {
    _tuple_ptr_int_string t;
    if (!s || len < 0) { t._0 = NULL; t._1 = 0; t._2 = "bad input"; return t; }
    /* Allocate a fresh buffer the Aether side owns: probe.ae frees it
     * with free(). A real caller would route through
     * string_new_with_length to get a refcount lifecycle. */
    char* buf = malloc((size_t)len + 1);
    if (!buf) { t._0 = NULL; t._1 = 0; t._2 = "alloc failed"; return t; }
    memcpy(buf, s, (size_t)len);
    buf[len] = '\0';
    t._0 = buf;
    t._1 = len;
    t._2 = "";
    return t;
}
