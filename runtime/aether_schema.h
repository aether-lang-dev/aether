/* aether_schema.h — the field table `@derive(schema)` emits (#2298).
 *
 * `@derive(schema) struct T { ... }` makes the compiler emit, as static
 * constant data in the program, one AetherSchema describing T: its name and
 * size, and per field its name, type, offset, size and any `@attr(...)`
 * attributes written on it. `T_schema()` returns its address. Nothing is
 * built at run time; the offsets and sizes are C's own `offsetof`/`sizeof`,
 * so the table always matches the layout the compiler emitted.
 *
 * Generated code includes this header for the layout; std.schema reads it
 * through the accessors below, which are null- and bounds-safe so a generic
 * inspector or serializer never has to check indices itself. A C host reads
 * the structs directly. */
#ifndef AETHER_SCHEMA_H
#define AETHER_SCHEMA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A field's kind. The width of an integer or float kind is the field's size
 * (an `int` field declared through a C ABI alias such as int16_t is
 * AETHER_SCHEMA_INT with size 2). */
enum {
    AETHER_SCHEMA_OTHER      = 0,   /* optional, tuple, sum, bitset, SIMD, ... */
    AETHER_SCHEMA_BOOL       = 1,
    AETHER_SCHEMA_BYTE       = 2,
    AETHER_SCHEMA_INT        = 3,
    AETHER_SCHEMA_LONG       = 4,
    AETHER_SCHEMA_UINT8      = 5,
    AETHER_SCHEMA_UINT16     = 6,
    AETHER_SCHEMA_UINT32     = 7,
    AETHER_SCHEMA_ULONG      = 8,
    AETHER_SCHEMA_FLOAT      = 9,   /* C double */
    AETHER_SCHEMA_F32        = 10,  /* C float */
    AETHER_SCHEMA_LONGDOUBLE = 11,
    AETHER_SCHEMA_DURATION   = 12,  /* signed 64-bit nanoseconds */
    AETHER_SCHEMA_STRING     = 13,  /* const char*; see heap_offset */
    AETHER_SCHEMA_PTR        = 14,  /* `ptr` or `*T` (nested names T's table) */
    AETHER_SCHEMA_STRUCT     = 15,  /* a struct value (nested names its table) */
    AETHER_SCHEMA_ARRAY      = 16,  /* fixed `T[N]`: len = N, elem_kind = T's */
    AETHER_SCHEMA_SLICE      = 17,  /* `T[]` ({ptr, len}); elem_kind = T's */
    AETHER_SCHEMA_ENUM       = 18,
    AETHER_SCHEMA_FN         = 19   /* closure or C function pointer */
};

/* An attribute argument's kind. Every argument carries all three readings:
 * `i` and `f` are its value as an integer and as a double (a bool is 0/1, a
 * string is 0), `text` its source spelling (a string's value, unquoted). */
enum {
    AETHER_SCHEMA_ARG_INT    = 1,
    AETHER_SCHEMA_ARG_FLOAT  = 2,
    AETHER_SCHEMA_ARG_STRING = 3,
    AETHER_SCHEMA_ARG_BOOL   = 4
};

typedef struct {
    int kind;
    const char* text;
    long long i;
    double f;
} AetherSchemaArg;

/* `@name(args...)` written after a field's type; `@name` alone has argc 0. */
typedef struct {
    const char* name;
    int argc;
    const AetherSchemaArg* args;
} AetherSchemaAttr;

typedef struct AetherSchema AetherSchema;

typedef struct {
    const char* name;
    const char* type;        /* source spelling: "float", "*Inner", "int[4]" */
    int kind;
    int elem_kind;           /* ARRAY / SLICE: the element's kind; else OTHER */
    int len;                 /* ARRAY: the element count; else 0 */
    size_t offset;
    size_t size;
    /* STRING: the offset of the field's heap-ownership flag (an int, 1 when
     * the struct owns the string and frees it on reassignment); -1 for every
     * other kind. A generic writer that stores a heap string sets it. */
    long heap_offset;
    /* STRUCT, PTR to a struct, and ARRAY/SLICE of structs: the struct's
     * table, when it is an Aether struct; NULL otherwise (an extern C
     * struct, a scalar). A function, so tables may refer to each other in
     * any order, cycles through pointers included. */
    const AetherSchema* (*nested)(void);
    int attr_count;
    const AetherSchemaAttr* attrs;
} AetherSchemaField;

struct AetherSchema {
    const char* name;
    size_t size;
    int field_count;
    const AetherSchemaField* fields;
};

/* Generated code defines AETHER_SCHEMA_LAYOUT_ONLY before including this
 * header: it needs the layout, and std.schema's externs declare the
 * accessors themselves. */
#ifndef AETHER_SCHEMA_LAYOUT_ONLY

/* ---- accessors (runtime/aether_schema.c) ----
 * `s` is a table from T_schema() or aether_schema_field_schema; a NULL `s`,
 * an out-of-range field / attribute / argument index, or a name that is not
 * there reads as NULL, 0 or -1 as documented, never as a crash. */
const char* aether_schema_name(const void* s);
int aether_schema_size(const void* s);
int aether_schema_field_count(const void* s);
int aether_schema_field_index(const void* s, const char* name);        /* -1 */
const char* aether_schema_field_name(const void* s, int field);
const char* aether_schema_field_type(const void* s, int field);
int aether_schema_field_kind(const void* s, int field);                /* OTHER */
int aether_schema_field_elem_kind(const void* s, int field);
int aether_schema_field_len(const void* s, int field);
int aether_schema_field_offset(const void* s, int field);             /* -1 */
int aether_schema_field_size(const void* s, int field);               /* 0 */
int aether_schema_field_heap_offset(const void* s, int field);        /* -1 */
const void* aether_schema_field_schema(const void* s, int field);      /* NULL */
int aether_schema_attr_count(const void* s, int field);
int aether_schema_attr_index(const void* s, int field, const char* name);  /* -1 */
const char* aether_schema_attr_name(const void* s, int field, int attr);
int aether_schema_attr_arg_count(const void* s, int field, int attr);
int aether_schema_attr_arg_kind(const void* s, int field, int attr, int arg);  /* 0 */
int64_t aether_schema_attr_int(const void* s, int field, int attr, int arg);
double aether_schema_attr_float(const void* s, int field, int attr, int arg);
const char* aether_schema_attr_text(const void* s, int field, int attr, int arg);

#endif /* AETHER_SCHEMA_LAYOUT_ONLY */

#ifdef __cplusplus
}
#endif

#endif /* AETHER_SCHEMA_H */
