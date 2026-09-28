#ifndef AETHER_JSON_H
#define AETHER_JSON_H

#include <stddef.h>

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

// Parse a JSON document. Returns the root value on success (caller owns
// and must free with json_free), or NULL on failure — retrieve the
// reason from json_last_error().
JsonValue* json_parse_raw(const char* json_str);

// Length-taking parse variant. Safe for inputs without a trailing null
// byte (e.g. fuzzer-provided buffers, memory-mapped files). Same return
// semantics as json_parse_raw.
JsonValue* json_parse_raw_n(const char* data, size_t len);

// Most recent parse error from json_parse_raw* on this thread. Returns
// a "<reason> at <line>:<col>" string, or "" if the last parse
// succeeded. The pointer is owned by the runtime; do not free it.
const char* json_last_error(void);

// Issue #392 — structured-error pilot. Programmatic accessors for
// the kind / line / column of the last parse failure. Mirrors the
// thread-local triple set by err_set in aether_json.c. KIND_OK = 0;
// non-zero values mean a parse failure. Kinds:
//   1 = parse error (any malformed JSON)
//   2 = out of memory (arena allocation failed)
//   3 = invalid input (NULL / empty handed to json_parse_raw)
// The full enum is exported from std.json as `KIND_*` constants.
int json_last_error_kind(void);
int json_last_error_line(void);
int json_last_error_col(void);

// Serialize `value` to a newly malloc'd compact JSON string. Caller
// must free. Returns a non-NULL empty string on allocation failure.
// The Aether-side wrapper `json.stringify` copies into an owned
// Aether string and frees the raw C buffer; use that from Aether code.
char* json_stringify_raw(JsonValue* value);

// Free the value and every node reachable from it. For parsed roots
// this releases the parse arena in O(chunks). For heap-allocated values
// (from json_create_*) it recurses and frees each node.
void json_free(JsonValue* value);

JsonType json_type(JsonValue* value);
int json_is_null(JsonValue* value);
// 1 for a number written as an integer (`42`, `-7`) within int64, or built
// with json_create_int; 0 for one with a fraction or exponent (`42.0`,
// `4e1`), one past int64, `-0` (kept as a double, sign and all), and
// anything that is not a number.
int json_is_integer(JsonValue* value);

int json_get_bool(JsonValue* value);
double json_get_number(JsonValue* value);
int json_get_int(JsonValue* value);
long long json_get_long(JsonValue* value);
// Borrowed pointer into the JsonValue's internal string. NULL when
// `value` is not a JSON_STRING. Valid until json_free(root) is called.
// The Aether wrapper `json.get_string` copies this into an owned
// Aether string.
const char* json_get_string_raw(JsonValue* value);

JsonValue* json_object_get_raw(JsonValue* obj, const char* key);
// Returns 1 on success, 0 if `obj` is not a JSON_OBJECT, null key/
// value, or allocation failure. Ownership of `value` transfers to
// the container — do NOT json_free it separately.
int json_object_set_raw(JsonValue* obj, const char* key, JsonValue* value);
int json_object_has(JsonValue* obj, const char* key);

// Object-key iteration. Keys are yielded in insertion order: the order
// they appeared in the parsed JSON, and the order `json_object_set_raw`
// was called on builder-constructed objects. Mutating the object during
// iteration is not supported — the observed order is undefined and
// entries may be skipped or revisited. Indices are valid in [0, size).

// Number of entries in the object. Returns -1 if `obj` is not a
// JSON_OBJECT — distinguishable from "empty object" (returns 0).
// Named `_raw` for consistency with the other object accessors, and
// so the Aether-side wrapper `object_size` doesn't collide with it
// after module-prefix mangling.
int json_object_size_raw(JsonValue* obj);

// Key at index `i` — a borrowed, NUL-terminated pointer into the
// object's internal storage, valid until json_free(root) is called.
// Returns NULL if `obj` is not a JSON_OBJECT or `i` is out of range.
// Keys are NUL-terminated even for inputs parsed via json_parse_raw_n
// (the parser arena-copies each key and writes a trailing '\0').
const char* json_object_key_at(JsonValue* obj, int i);

// Length of the key at index `i`, matching json_object_key_at(obj, i).
// Returns -1 if `obj` is not a JSON_OBJECT or `i` is out of range.
// Useful for callers that want to avoid an extra strlen; JSON per
// RFC 8259 disallows embedded NULs in keys, so strlen is also correct.
int json_object_key_len_at(JsonValue* obj, int i);

// Value at index `i` — borrowed from the object. Returns NULL only
// when `obj` is not a JSON_OBJECT or `i` is out of range. A valid
// entry whose value is a JSON null still returns a non-NULL JsonValue*;
// use json_is_null() to distinguish that from the out-of-range NULL.
JsonValue* json_object_value_at(JsonValue* obj, int i);

JsonValue* json_array_get_raw(JsonValue* arr, int index);
// Returns 1 on success, 0 if `arr` is not a JSON_ARRAY, null value,
// or allocation failure. Ownership of `value` transfers to the array.
int json_array_add_raw(JsonValue* arr, JsonValue* value);
int json_array_size(JsonValue* arr);

JsonValue* json_create_null(void);
JsonValue* json_create_bool(int value);
JsonValue* json_create_number(double value);
/* Integer-flavoured number — serialised as a bare integer (`%lld`),
 * not scientific notation. Full int64 range (separate union slot from
 * `data.number`). Use this for byte-counts, IDs, totals — anything the
 * consumer expects to read as a plain integer. */
JsonValue* json_create_int(long long value);
JsonValue* json_create_string(const char* value);
JsonValue* json_create_array(void);
JsonValue* json_create_object(void);

#endif
