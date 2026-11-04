#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "ast.h"
#include "analysis/hoist.h"
#include <limits.h>
#include "../std/mem/aether_grow.h"

// Type functions
Type* create_type(TypeKind kind) {
    Type* type = malloc(sizeof(Type));
    type->kind = kind;
    type->element_type = NULL;
    type->array_size = -1;
    type->struct_name = NULL;
    type->c_alias = NULL;
    type->distinct_name = NULL;
    type->index_enum_name = NULL;   // #1044 enum-indexed array marker
    type->tuple_types = NULL;
    type->tuple_count = 0;
    type->tuple_heap_flags = NULL;
    type->param_types = NULL;
    type->param_count = 0;
    type->param_modes = NULL;   /* #2980: every parameter borrows */
    type->return_type = NULL;
    /* Default to closure (`_AeClosure`-shaped). The fnptr-cast parser
     * path (`expr as fn(...)`) flips this on a freshly-created
     * TYPE_FUNCTION; closure types created via other paths (typechecker
     * builtins like `unbox_closure`, function-definition return types)
     * must NOT inherit garbage from malloc. */
    type->is_fnptr = 0;
    type->compound_node = NULL;
    type->closure_literal = NULL;
    type->is_result = 0;   /* #913 `T!` marker — must not inherit malloc garbage */
    type->qualifier = NULL;
    return type;
}

int type_is_slice(const Type* t) {
    return t && t->kind == TYPE_ARRAY && t->array_size <= 0 && !t->index_enum_name;
}

int type_is_sized_array(const Type* t) {
    return t && t->kind == TYPE_ARRAY && !type_is_slice(t);
}

Type* create_array_type(Type* element_type, int size) {
    Type* type = create_type(TYPE_ARRAY);
    type->element_type = element_type;
    type->array_size = size;
    return type;
}

// #340 `T?` — optional over `inner`. `inner` is adopted (not cloned).
Type* create_optional_type(Type* inner) {
    Type* type = create_type(TYPE_OPTIONAL);
    type->element_type = inner;
    return type;
}

Type* create_actor_ref_type(Type* actor_type) {
    Type* type = create_type(TYPE_ACTOR_REF);
    type->element_type = actor_type;
    return type;
}

// #1046 `bit_set[E]`, a set of members of enum `element_enum`, backed by an
// unsigned 64-bit word. `element_enum` is adopted (not cloned). Nominal;
// clone_type/free_type handle element_type generically.
Type* create_bitset_type(Type* element_enum) {
    Type* type = create_type(TYPE_BITSET);
    type->element_type = element_enum;
    return type;
}

// #1132 `bitstruct Name : uint8_t { ... }`. `struct_name` is the bitstruct's
// name; `element_type` is the backing integer type (adopted, not cloned).
// Nominal — see types_equal below. clone_type/free_type already handle both
// slots generically.
Type* create_bitstruct_type(const char* name, Type* backing) {
    Type* type = create_type(TYPE_BITSTRUCT);
    type->struct_name = name ? strdup(name) : NULL;
    type->element_type = backing;
    return type;
}

Type* create_tuple_type(int count, ...) {
    Type* type = create_type(TYPE_TUPLE);
    type->tuple_count = count;
    type->tuple_types = malloc(count * sizeof(Type*));
    va_list args;
    va_start(args, count);
    for (int i = 0; i < count; i++) {
        type->tuple_types[i] = va_arg(args, Type*);
    }
    va_end(args);
    return type;
}

// #913 fallible result `T!`. Represented as the existing `(T, string)`
// (value, err) tuple so it is ABI-interchangeable with the stdlib convention;
// the `is_result` flag distinguishes it from a plain 2-tuple so `expr!`
// propagates (not panics) in a result-returning function and `or {}` applies.
Type* create_result_type(Type* inner) {
    Type* t = create_tuple_type(2, inner, create_type(TYPE_STRING));
    t->is_result = 1;
    return t;
}

// #914 sum/variant type. `struct_name` holds the sum's name; the variant
// Types are filled into `tuple_types[]` by the typechecker once the named
// variant structs resolve. clone_type/free_type already handle both fields
// generically, so a TYPE_SUM round-trips like any other Type.
Type* create_sum_type(const char* name) {
    Type* type = create_type(TYPE_SUM);
    if (name) type->struct_name = strdup(name);
    type->tuple_types = NULL;
    type->tuple_count = 0;
    return type;
}

Type* create_function_type(int param_count, Type** param_types, Type* return_type) {
    Type* type = create_type(TYPE_FUNCTION);
    type->param_count = param_count;
    if (param_count > 0 && param_types) {
        type->param_types = malloc(param_count * sizeof(Type*));
        for (int i = 0; i < param_count; i++) {
            type->param_types[i] = param_types[i];
        }
    }
    type->return_type = return_type;
    return type;
}

int fn_type_param_mode(const Type* t, int i) {
    if (!t || t->kind != TYPE_FUNCTION || !t->param_modes || i < 0 || i >= t->param_count)
        return FN_MODE_BORROWED;
    return t->param_modes[i];
}

void fn_type_set_param_mode(Type* t, int i, int mode) {
    if (!t || i < 0 || i >= t->param_count) return;
    if (!t->param_modes) {
        if (mode == FN_MODE_BORROWED) return;
        t->param_modes = calloc((size_t)t->param_count, sizeof(int));
        if (!t->param_modes) return;
    }
    t->param_modes[i] = mode;
}

int fn_type_has_modes(const Type* t) {
    for (int i = 0; t && t->param_modes && i < t->param_count; i++)
        if (t->param_modes[i] != FN_MODE_BORROWED) return 1;
    return 0;
}

void free_type(Type* type) {
    if (type) {
        if (type->element_type) {
            free_type(type->element_type);
        }
        if (type->struct_name) {
            free(type->struct_name);
        }
        if (type->c_alias) {
            free(type->c_alias);
        }
        if (type->distinct_name) {
            free(type->distinct_name);
        }
        if (type->index_enum_name) {
            free(type->index_enum_name);
        }
        if (type->tuple_types) {
            for (int i = 0; i < type->tuple_count; i++) {
                free_type(type->tuple_types[i]);
            }
            free(type->tuple_types);
        }
        /* Per-tuple-element heap-ownership flags (issue #420). NULL
         * unless the parser saw at least one `@heap` / `@borrow`
         * marker on the tuple type. */
        if (type->tuple_heap_flags) {
            free(type->tuple_heap_flags);
        }
        if (type->param_types) {
            for (int i = 0; i < type->param_count; i++) {
                free_type(type->param_types[i]);
            }
            free(type->param_types);
        }
        free(type->param_modes);
        if (type->return_type) {
            free_type(type->return_type);
        }
        free(type);
    }
}

/* #2297: `type` spelled as Aether source that parse_type reads back to the
 * same type -- unlike type_to_string, which is for display and flattens a
 * typed pointer to "ptr" and a struct to "struct X". Used where a type has to
 * cross a binary boundary and be re-declared on the other side (the
 * --emit=lib catalog, from which `ae` synthesizes an importer's interface).
 *
 * Writes into buf[*pos..cap). Returns 1 when the type has a faithful
 * spelling, 0 when it does not (an enum, sum or distinct type whose
 * definition the reader would also need, a const-qualified C spelling, an
 * actor ref, ...): the caller then leaves the record out rather than describe
 * the type wrongly. */
static int type_src_put(char* buf, size_t cap, size_t* pos, const char* s) {
    size_t n = strlen(s);
    if (*pos + n + 1 > cap) return 0;
    memcpy(buf + *pos, s, n + 1);
    *pos += n;
    return 1;
}

static int type_src_write(const Type* t, char* buf, size_t cap, size_t* pos) {
    if (!t) return 0;
    if (t->distinct_name) return 0;
    if (t->c_alias) {
        const char* a = t->c_alias;
        if (strcmp(a, "char*") == 0)       return type_src_put(buf, cap, pos, "cstring");
        if (strcmp(a, "const char*") == 0) return type_src_put(buf, cap, pos, "cstring_const");
        if (strcmp(a, "long long") == 0)   return type_src_put(buf, cap, pos, "long long");
        if (strcmp(a, "va_list") == 0)     return type_src_put(buf, cap, pos, "va_list");
        /* A C ABI scalar alias (size_t, uint32_t, ...) is spelled as itself;
         * anything else (a `const`-qualified spelling) has no source form. */
        for (const char* c = a; *c; c++) {
            if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                  (*c >= '0' && *c <= '9') || *c == '_')) return 0;
        }
        return type_src_put(buf, cap, pos, a);
    }
    switch (t->kind) {
        case TYPE_INT:        return type_src_put(buf, cap, pos, "int");
        case TYPE_INT64:      return type_src_put(buf, cap, pos, "long");
        case TYPE_UINT64:     return type_src_put(buf, cap, pos, "uint64");
        case TYPE_UINT32:     return type_src_put(buf, cap, pos, "uint32");
        case TYPE_UINT16:     return type_src_put(buf, cap, pos, "uint16");
        case TYPE_UINT8:      return type_src_put(buf, cap, pos, "uint8");
        case TYPE_DURATION:   return type_src_put(buf, cap, pos, "Duration");
        case TYPE_FLOAT:      return type_src_put(buf, cap, pos, "float");
        case TYPE_LONGDOUBLE: return type_src_put(buf, cap, pos, "longdouble");
        case TYPE_FLOAT32:    return type_src_put(buf, cap, pos, "f32");
        case TYPE_F32X4:      return type_src_put(buf, cap, pos, "f32x4");
        case TYPE_F64X2:      return type_src_put(buf, cap, pos, "f64x2");
        case TYPE_I32X4:      return type_src_put(buf, cap, pos, "i32x4");
        case TYPE_I64X2:      return type_src_put(buf, cap, pos, "i64x2");
        case TYPE_I16X8:      return type_src_put(buf, cap, pos, "i16x8");
        case TYPE_F32X8:      return type_src_put(buf, cap, pos, "f32x8");
        case TYPE_I32X8:      return type_src_put(buf, cap, pos, "i32x8");
        case TYPE_BOOL:       return type_src_put(buf, cap, pos, "bool");
        case TYPE_BYTE:       return type_src_put(buf, cap, pos, "byte");
        case TYPE_STRING:     return type_src_put(buf, cap, pos, "string");
        case TYPE_PTR:
            if (t->element_type && t->element_type->kind == TYPE_STRUCT &&
                t->element_type->struct_name) {
                return type_src_put(buf, cap, pos, "*") &&
                       type_src_put(buf, cap, pos, t->element_type->struct_name);
            }
            return type_src_put(buf, cap, pos, "ptr");
        case TYPE_STRUCT:
            return t->struct_name && type_src_put(buf, cap, pos, t->struct_name);
        case TYPE_ARRAY: {
            if (t->index_enum_name) return 0;
            if (!type_src_write(t->element_type, buf, cap, pos)) return 0;
            if (t->array_size < 0) return type_src_put(buf, cap, pos, "[]");
            char n[32];
            snprintf(n, sizeof(n), "[%d]", t->array_size);
            return type_src_put(buf, cap, pos, n);
        }
        case TYPE_FUNCTION:
            /* A closure value is spelled bare `fn`, unless a parameter names
             * a mode (#2980): `fn(@keep ptr, float)` is a closure's type
             * whose signature is part of it, and a binary import's stub
             * read as `fn` checked a keeping function as borrowing (#3013).
             * A C function pointer carries its signature in source. */
            if (!t->is_fnptr && !fn_type_has_modes(t)) return type_src_put(buf, cap, pos, "fn");
            if (!type_src_put(buf, cap, pos, "fn(")) return 0;
            for (int i = 0; i < t->param_count; i++) {
                if (i > 0 && !type_src_put(buf, cap, pos, ", ")) return 0;
                int mode = fn_type_param_mode(t, i);
                if (mode == FN_MODE_KEEP && !type_src_put(buf, cap, pos, "@keep ")) return 0;
                if (mode == FN_MODE_INTO && !type_src_put(buf, cap, pos, "@into ")) return 0;
                if (!type_src_write(t->param_types ? t->param_types[i] : NULL, buf, cap, pos))
                    return 0;
            }
            if (!type_src_put(buf, cap, pos, ")")) return 0;
            if (t->return_type && t->return_type->kind != TYPE_VOID) {
                return type_src_put(buf, cap, pos, " -> ") &&
                       type_src_write(t->return_type, buf, cap, pos);
            }
            return 1;
        case TYPE_OPTIONAL:
            return type_src_write(t->element_type, buf, cap, pos) &&
                   type_src_put(buf, cap, pos, "?");
        case TYPE_TUPLE:
            if (t->is_result && t->tuple_count == 2) {
                return type_src_write(t->tuple_types[0], buf, cap, pos) &&
                       type_src_put(buf, cap, pos, "!");
            }
            if (!type_src_put(buf, cap, pos, "(")) return 0;
            for (int i = 0; i < t->tuple_count; i++) {
                if (i > 0 && !type_src_put(buf, cap, pos, ", ")) return 0;
                if (!type_src_write(t->tuple_types[i], buf, cap, pos)) return 0;
            }
            return type_src_put(buf, cap, pos, ")");
        case TYPE_ISOLATED:
            return type_src_put(buf, cap, pos, "Isolated[") &&
                   type_src_write(t->element_type, buf, cap, pos) &&
                   type_src_put(buf, cap, pos, "]");
        default:
            return 0;
    }
}

int type_to_aether_source(const Type* type, char* buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    buf[0] = '\0';
    size_t pos = 0;
    if (!type_src_write(type, buf, cap, &pos)) { buf[0] = '\0'; return 0; }
    return 1;
}

/* A type's spelling, interned (#2539): whole however long its names, and
 * never in a buffer a later call reuses. The static buffers this had cut a
 * long struct or tuple name, and a nested array or tuple was spelled into
 * the buffer its own element's spelling was read from. */
static const char* spelled_list_by(const char* (*spell)(Type*), const char* open,
                                   Type** items, int count, const char* sep,
                                   const char* close) {
    size_t cap = 64, len = 0;
    char* buf = (char*)aether_xrealloc(NULL, cap);
    buf[0] = '\0';
    for (int i = -1; i <= count; i++) {
        const char* piece = i < 0 ? open : i == count ? close
                          : items ? spell(items[i]) : "?";
        const char* lead = (i > 0 && i < count) ? sep : "";
        size_t need = len + strlen(lead) + strlen(piece) + 1;
        if (need > cap) {
            while (cap < need) cap *= 2;
            buf = (char*)aether_xrealloc(buf, cap);
        }
        len += (size_t)sprintf(buf + len, "%s%s", lead, piece);
    }
    const char* r = aether_intern_n(buf, len);
    free(buf);
    return r;
}

static const char* spelled_list(const char* open, Type** items, int count,
                                const char* sep, const char* close) {
    return spelled_list_by(type_to_string, open, items, count, sep, close);
}

/* Module-scoped struct types: a module's struct is merged and emitted as
 * `<ns>_<Name>`. The merge records each such name with the name the struct
 * was written with and its module-qualified spelling, so what speaks the
 * source language (diagnostics, a quote's tree, derived names, schema
 * tables) can say `Shader` / `gpu.Shader` rather than `gpu_Shader`. */
typedef struct { const char* cname; const char* source; const char* display; } StructNameEntry;
static StructNameEntry* g_struct_names = NULL;
static int g_struct_name_count = 0, g_struct_name_cap = 0;

void ast_struct_names_reset(void) {
    free(g_struct_names);
    g_struct_names = NULL;
    g_struct_name_count = g_struct_name_cap = 0;
}

void ast_struct_name_register(const char* cname, const char* source, const char* display) {
    if (!cname || !source || strcmp(cname, source) == 0) return;
    for (int i = 0; i < g_struct_name_count; i++)
        if (strcmp(g_struct_names[i].cname, cname) == 0) return;
    if (g_struct_name_count == g_struct_name_cap) {
        g_struct_name_cap = g_struct_name_cap ? g_struct_name_cap * 2 : 32;
        g_struct_names = aether_xrealloc(g_struct_names,
                                         (size_t)g_struct_name_cap * sizeof(StructNameEntry));
    }
    StructNameEntry* e = &g_struct_names[g_struct_name_count++];
    e->cname = aether_intern(cname);
    e->source = aether_intern(source);
    e->display = aether_intern(display ? display : source);
}

static const StructNameEntry* struct_name_entry(const char* cname) {
    if (!cname) return NULL;
    for (int i = 0; i < g_struct_name_count; i++)
        if (strcmp(g_struct_names[i].cname, cname) == 0) return &g_struct_names[i];
    return NULL;
}

const char* ast_struct_source_name(const char* cname) {
    const StructNameEntry* e = struct_name_entry(cname);
    return e ? e->source : cname;
}

const char* ast_struct_display_name(const char* cname) {
    const StructNameEntry* e = struct_name_entry(cname);
    return e ? e->display : cname;
}

int ast_names_without_binding(const ASTNode* n) {
    switch (n->type) {
        case AST_IDENTIFIER:
        case AST_LITERAL:
        case AST_FUNCTION_CALL:
        case AST_MEMBER_ACCESS:
        case AST_FIELD_INIT:
        case AST_NAMED_ARG:
        case AST_MESSAGE_CONSTRUCTOR:
        case AST_STRUCT_LITERAL:
        case AST_BINARY_EXPRESSION:
        case AST_UNARY_EXPRESSION:
        case AST_STRING_INTERP:
            return 1;
        default:
            return 0;
    }
}

int ast_binds_name(const ASTNode* n, const char* name) {
    if (!n || !name) return 0;
    if (n->value && !ast_names_without_binding(n) && strcmp(n->value, name) == 0) return 1;
    for (int i = 0; i < n->child_count; i++)
        if (ast_binds_name(n->children[i], name)) return 1;
    return 0;
}

const char* type_to_string(Type* type) {
    if (!type) return "UNKNOWN";

    switch (type->kind) {
        case TYPE_INT: return "int";
        case TYPE_INT64: return "long";
        case TYPE_UINT64: return "uint64";
        case TYPE_DURATION: return "Duration";
        case TYPE_FLOAT: return "float";
        case TYPE_LONGDOUBLE: return "longdouble";
        case TYPE_FLOAT32: return "f32";
        case TYPE_BOOL: return "bool";
        case TYPE_BYTE: return "byte";
        case TYPE_STRING: return "string";
        case TYPE_VOID: return "void";
        case TYPE_PTR: return "ptr";
        case TYPE_MESSAGE: return "Message";
        case TYPE_STRUCT:
            return aether_internf("struct %s", type->struct_name
                                                   ? ast_struct_display_name(type->struct_name)
                                                   : "unnamed");
        case TYPE_ARRAY:
            if (type->index_enum_name) {
                // #1044 enum-indexed array `[E]T`
                return aether_internf("[%s]%s", type->index_enum_name,
                                      type->element_type ? type_to_string(type->element_type) : "?");
            }
            return aether_internf("%s[%d]", type_to_string(type->element_type), type->array_size);
        case TYPE_ACTOR_REF:
            return aether_internf("ActorRef<%s>", type_to_string(type->element_type));
        case TYPE_TUPLE:
            return spelled_list("(", type->tuple_types, type->tuple_count, ", ", ")");
        case TYPE_FUNCTION: {
            const char* params = spelled_list("|", type->param_types, type->param_count, ", ", "|");
            return aether_internf("%s -> %s", params,
                                  type->return_type ? type_to_string(type->return_type) : "void");
        }
        case TYPE_OPTIONAL:
            return aether_internf("%s?", type->element_type ? type_to_string(type->element_type) : "?");
        case TYPE_SUM:
            return type->struct_name ? type->struct_name : "sum";
        case TYPE_ENUM:
            return type->struct_name ? type->struct_name : "enum";
        case TYPE_BITSTRUCT:
            return type->struct_name ? type->struct_name : "bitstruct";
        case TYPE_ISOLATED:
            return aether_internf("Isolated[%s]",
                                  type->element_type ? type_to_string(type->element_type) : "?");
        case TYPE_BITSET:
            return aether_internf("bit_set[%s]",
                                  type->element_type ? type_to_string(type->element_type) : "?");
        default: return "UNKNOWN";
    }
}

const char* type_spelled(Type* type) {
    if (!type) return "unknown";
    if (type->distinct_name) return type->distinct_name;
    if (type->c_alias) return type->c_alias;
    switch (type->kind) {
        case TYPE_STRUCT:
            return type->struct_name ? ast_struct_display_name(type->struct_name) : "struct";
        case TYPE_PTR:
            if (type->element_type && type->element_type->kind != TYPE_UNKNOWN)
                return aether_internf("*%s", type_spelled(type->element_type));
            return "ptr";
        case TYPE_ARRAY:
            if (type->index_enum_name)
                return aether_internf("[%s]%s", type->index_enum_name,
                                      type_spelled(type->element_type));
            if (type->array_size < 0)
                return aether_internf("%s[]", type_spelled(type->element_type));
            return aether_internf("%s[%d]", type_spelled(type->element_type), type->array_size);
        case TYPE_FUNCTION: {
            /* A closure whose signature was erased is a bare `fn`; any
             * other function type, a closure's or a C function pointer's,
             * is `fn(P, ...) -> R` (no arrow for no result). */
            if (!type->is_fnptr && type->param_count == 0 && !type->return_type) return "fn";
            const char* params = spelled_list_by(type_spelled, "fn(", type->param_types,
                                                 type->param_count, ", ", ")");
            /* #2980: a parameter's mode is part of the type. */
            if (fn_type_has_modes(type)) {
                params = "fn(";
                for (int i = 0; i < type->param_count; i++) {
                    int m = fn_type_param_mode(type, i);
                    params = aether_internf("%s%s%s%s", params, i ? ", " : "",
                                            m == FN_MODE_KEEP   ? "@keep "
                                            : m == FN_MODE_INTO ? "@into "
                                                                : "",
                                            type_spelled(type->param_types ? type->param_types[i]
                                                                           : NULL));
                }
                params = aether_internf("%s)", params);
            }
            /* A result not known yet (a function value whose result is left
             * to inference) is not spelled, as none is. */
            if (!type->return_type || type->return_type->kind == TYPE_VOID ||
                (fn_type_has_modes(type) && type->return_type->kind == TYPE_UNKNOWN))
                return params;
            return aether_internf("%s -> %s", params, type_spelled(type->return_type));
        }
        case TYPE_OPTIONAL:
            return aether_internf("%s?", type_spelled(type->element_type));
        case TYPE_TUPLE:
            if (type->is_result && type->tuple_count == 2 && type->tuple_types)
                return aether_internf("%s!", type_spelled(type->tuple_types[0]));
            return spelled_list_by(type_spelled, "(", type->tuple_types, type->tuple_count,
                                   ", ", ")");
        case TYPE_ISOLATED:
            return aether_internf("Isolated[%s]", type_spelled(type->element_type));
        case TYPE_BITSET:
            return aether_internf("bit_set[%s]", type_spelled(type->element_type));
        case TYPE_UNKNOWN:
            return "unknown";
        default:
            return type_to_string(type);
    }
}

int types_equal(Type* a, Type* b) {
    if (!a || !b) return a == b;
    if (a->kind != b->kind) return 0;
    if (a->array_size != b->array_size) return 0;

    // #480 distinct types: a distinct type equals only the same-named distinct
    // type; a distinct type and its base (one name set, the other NULL) are
    // not equal even when `kind` matches.
    {
        const char* da = a->distinct_name;
        const char* db = b->distinct_name;
        if (da || db) {
            if (!da || !db) return 0;
            return strcmp(da, db) == 0;
        }
    }
    
    // For struct types, compare names
    if (a->kind == TYPE_STRUCT) {
        if (!a->struct_name || !b->struct_name) return 0;
        return strcmp(a->struct_name, b->struct_name) == 0;
    }

    // #914 sum types are nominal: equal iff they name the same sum.
    if (a->kind == TYPE_SUM) {
        if (!a->struct_name || !b->struct_name) return 0;
        return strcmp(a->struct_name, b->struct_name) == 0;
    }

    // #1044 enums are nominal: equal iff they name the same enum.
    if (a->kind == TYPE_ENUM) {
        if (!a->struct_name || !b->struct_name) return 0;
        return strcmp(a->struct_name, b->struct_name) == 0;
    }

    // #1132 bitstructs are nominal too: equal iff they name the same bitstruct.
    // Two bitstructs over the same backing integer are still different types.
    if (a->kind == TYPE_BITSTRUCT) {
        if (!a->struct_name || !b->struct_name) return 0;
        return strcmp(a->struct_name, b->struct_name) == 0;
    }

    // For function types, compare param types and return type
    if (a->kind == TYPE_FUNCTION) {
        if (a->param_count != b->param_count) return 0;
        for (int i = 0; i < a->param_count; i++) {
            if (!types_equal(a->param_types[i], b->param_types[i])) return 0;
            if (fn_type_param_mode(a, i) != fn_type_param_mode(b, i)) return 0;   /* #2980 */
        }
        return types_equal(a->return_type, b->return_type);
    }

    // #1044 enum-indexed arrays are distinguished by their index enum: a
    // `[Dir]string` is neither a plain `string[]` nor a `[Color]string`.
    if (a->index_enum_name || b->index_enum_name) {
        if (!a->index_enum_name || !b->index_enum_name) return 0;
        if (strcmp(a->index_enum_name, b->index_enum_name) != 0) return 0;
    }

    return types_equal(a->element_type, b->element_type);
}

static int numeric_join_rank(TypeKind k) {
    switch (k) {
        case TYPE_LONGDOUBLE: return 6;
        case TYPE_FLOAT:      return 5;
        case TYPE_FLOAT32:    return 4;
        case TYPE_UINT64:     return 3;
        case TYPE_INT64:      return 3;   /* signed and unsigned 64-bit share a rank: neither widens into the other */
        case TYPE_UINT32:     return 2;
        case TYPE_INT:        return 1;
        case TYPE_UINT16: case TYPE_UINT8: case TYPE_BYTE: return 0;
        default:              return -1;
    }
}

Type* numeric_join_type(Type* a, Type* b) {
    if (!a || !b) return NULL;
    int ra = numeric_join_rank(a->kind), rb = numeric_join_rank(b->kind);
    if (ra < 0 || rb < 0) return NULL;
    return clone_type(rb > ra ? b : a);
}

/* #3064: an integer type's width in bits, and whether it is signed; 0 for
 * any other type. A `byte` is unsigned. A C ABI alias is its C type's
 * width: `int8_t` and `int16_t` are an int's kind and narrower. */
static int integer_kind_bits(const Type* t, int* is_signed) {
    *is_signed = 0;
    if (t->c_alias && t->kind == TYPE_INT) {
        *is_signed = 1;
        if (strcmp(t->c_alias, "int8_t") == 0) return 8;
        if (strcmp(t->c_alias, "int16_t") == 0) return 16;
        return 32;
    }
    switch (t->kind) {
        case TYPE_BYTE: case TYPE_UINT8: return 8;
        case TYPE_UINT16: return 16;
        case TYPE_UINT32: return 32;
        case TYPE_UINT64: return 64;
        case TYPE_INT: *is_signed = 1; return 32;
        case TYPE_INT64: *is_signed = 1; return 64;
        default: return 0;
    }
}

/* #3064: is `n` an integer literal (`5`, `0x10`, `1_000`, `-1`)? Its sign
 * in `*neg`, its magnitude in `*mag`. */
static int integer_literal_arm(const ASTNode* n, int* neg, unsigned long long* mag) {
    *neg = 0;
    if (n && n->type == AST_UNARY_EXPRESSION && n->child_count == 1 && n->value &&
        (strcmp(n->value, "-") == 0 || strcmp(n->value, "+") == 0)) {
        *neg = n->value[0] == '-';
        n = n->children[0];
    }
    if (!n || n->type != AST_LITERAL || !n->value || !n->node_type ||
        (n->node_type->kind != TYPE_INT && n->node_type->kind != TYPE_INT64))
        return 0;
    char digits[72];
    size_t k = 0;
    for (const char* c = n->value; *c && k + 1 < sizeof digits; c++)
        if (*c != '_') digits[k++] = *c;
    digits[k] = '\0';
    int base = 0;
    const char* s = digits;
    if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) { base = 2; s += 2; }
    else if (s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) { base = 8; s += 2; }
    char* end = NULL;
    unsigned long long v = strtoull(s, &end, base);
    if (!end || *end || end == s) return 0;
    *mag = v;
    if (v == 0) *neg = 0;
    return 1;
}

/* #3064: two integer types as one, of a value that is either (an
 * if-expression's branches, a value match's arms, a clause set's results).
 * Of one sign, the wider. A signed one beside an unsigned one is the
 * smallest signed kind that holds both: an int holds a byte or a uint16, a
 * long a uint32, and nothing signed holds a uint64. C's usual conversions
 * (numeric_join_type) take the unsigned kind instead, so `-1` beside a
 * uint32 came out 4294967295. A literal beside a uint32 or a uint64 that it
 * fits is that type, as `n == 0` reads the 0 for a uint32 `n`: `if c { 0 }
 * else { n }` stays a uint32. Sets `*handled` for two integer kinds, and
 * returns NULL for two that no integer type holds. */
static Type* integer_branch_join(Type* a, const ASTNode* a_arm, Type* b, const ASTNode* b_arm,
                                 int* handled) {
    *handled = 0;
    if (a->distinct_name || b->distinct_name) return NULL;
    int sa, sb;
    int wa = integer_kind_bits(a, &sa), wb = integer_kind_bits(b, &sb);
    if (!wa || !wb) return NULL;
    *handled = 1;
    if (sa == sb) return clone_type(wb > wa ? b : a);
    Type* s = sa ? a : b;
    Type* u = sa ? b : a;
    const ASTNode* s_arm = sa ? a_arm : b_arm;
    int ws = sa ? wa : wb, wu = sa ? wb : wa;
    int neg;
    unsigned long long mag;
    if ((u->kind == TYPE_UINT32 || u->kind == TYPE_UINT64) && integer_literal_arm(s_arm, &neg, &mag) &&
        !neg && (u->kind == TYPE_UINT64 || mag <= 0xFFFFFFFFull))
        return clone_type(u);
    if (ws > wu) return clone_type(s);
    if (wu < 32) return create_type(TYPE_INT);
    if (wu < 64) return create_type(TYPE_INT64);
    return NULL;
}

/* A float literal, or one under a unary minus or plus. */
static int is_float_literal_arm(const ASTNode* n) {
    if (n && n->type == AST_UNARY_EXPRESSION && n->child_count == 1 && n->value &&
        (strcmp(n->value, "-") == 0 || strcmp(n->value, "+") == 0))
        n = n->children[0];
    return n && n->type == AST_LITERAL && n->node_type && n->node_type->kind == TYPE_FLOAT;
}

/* #2996: the program's sum types, each with its variants, so two variants of
 * one sum join to it. The checker registers them as it resolves them
 * (resolve_sum_types). */
static Type** g_sums = NULL;
static int g_sum_count = 0;
static int g_sum_cap = 0;

void ast_sums_reset(void) {
    for (int i = 0; i < g_sum_count; i++) free_type(g_sums[i]);
    free(g_sums);
    g_sums = NULL;
    g_sum_count = g_sum_cap = 0;
}

void ast_sum_register(const Type* sum) {
    if (!sum || sum->kind != TYPE_SUM || !sum->struct_name) return;
    for (int i = 0; i < g_sum_count; i++)
        if (strcmp(g_sums[i]->struct_name, sum->struct_name) == 0) return;
    if (g_sum_count == g_sum_cap) {
        g_sum_cap = g_sum_cap ? g_sum_cap * 2 : 16;
        g_sums = aether_xrealloc(g_sums, (size_t)g_sum_cap * sizeof(Type*));
    }
    g_sums[g_sum_count++] = clone_type((Type*)sum);
}

static int sum_has_variant(const Type* sum, const char* name) {
    for (int i = 0; name && sum->tuple_types && i < sum->tuple_count; i++)
        if (sum->tuple_types[i] && sum->tuple_types[i]->struct_name &&
            strcmp(sum->tuple_types[i]->struct_name, name) == 0) return 1;
    return 0;
}

/* The one registered sum both variants belong to; NULL for none, or for more
 * than one (which sum is meant is then the binding's to say). */
static const Type* sum_of_variants(const char* a, const char* b) {
    const Type* found = NULL;
    for (int i = 0; i < g_sum_count; i++) {
        if (!sum_has_variant(g_sums[i], a) || !sum_has_variant(g_sums[i], b)) continue;
        if (found) return NULL;
        found = g_sums[i];
    }
    return found;
}

static int type_known(const Type* t) {
    return t && t->kind != TYPE_UNKNOWN && t->kind != TYPE_VOID;
}

/* The arm a tuple literal arm gives at position `i`, for the joins of its
 * elements (a float literal's rule); NULL for any other arm. */
static const ASTNode* tuple_arm_part(const ASTNode* arm, int i) {
    return arm && arm->type == AST_TUPLE_LITERAL && i < arm->child_count ? arm->children[i] : NULL;
}

Type* fn_type_mode_join(const Type* a, const Type* b) {
    if (!a || !b || a->kind != TYPE_FUNCTION || b->kind != TYPE_FUNCTION ||
        a->is_fnptr || b->is_fnptr)
        return NULL;
    if (!fn_type_has_modes(a) && !fn_type_has_modes(b)) return NULL;
    /* An erased `fn` (a closure literal whose parameters name no mode)
     * stands where the moded type is expected. */
    int a_erased = a->param_count == 0 && !a->return_type;
    int b_erased = b->param_count == 0 && !b->return_type;
    Type* j = NULL;
    if (a_erased != b_erased) {
        j = clone_type((Type*)(a_erased ? b : a));
    } else if (a->param_count == b->param_count) {
        Type* ja = clone_type((Type*)a);
        Type* jb = clone_type((Type*)b);
        for (int i = 0; i < a->param_count; i++) {
            int ma = fn_type_param_mode(a, i), mb = fn_type_param_mode(b, i);
            int m = ma > mb ? ma : mb;
            fn_type_set_param_mode(ja, i, m);
            fn_type_set_param_mode(jb, i, m);
        }
        ja->closure_literal = jb->closure_literal = NULL;
        if (types_equal(ja, jb)) j = ja;
        else free_type(ja);
        free_type(jb);
    }
    /* Either value's: a call result through it is no one literal's. */
    if (j) j->closure_literal = NULL;
    return j;
}

Type* branch_join_type(Type* a, const ASTNode* a_arm, Type* b, const ASTNode* b_arm) {
    if (!a || !b) return NULL;
    if (a->kind == TYPE_FLOAT32 && b->kind == TYPE_FLOAT && is_float_literal_arm(b_arm))
        return clone_type(a);
    if (b->kind == TYPE_FLOAT32 && a->kind == TYPE_FLOAT && is_float_literal_arm(a_arm))
        return clone_type(b);
    int integers = 0;
    Type* ij = integer_branch_join(a, a_arm, b, b_arm, &integers);   /* #3064 */
    if (integers) return ij;
    Type* num = numeric_join_type(a, b);
    if (num || !type_known(a) || !type_known(b)) return num;
    /* #3003: tuples of one length join position by position. A tuple's
     * elements are not compared by types_equal, so this comes first. */
    if (a->kind == TYPE_TUPLE || b->kind == TYPE_TUPLE) {
        if (a->kind != b->kind || a->tuple_count != b->tuple_count || a->tuple_count <= 0 ||
            !a->tuple_types || !b->tuple_types || a->is_result != b->is_result) return NULL;
        Type* t = create_type(TYPE_TUPLE);
        t->tuple_count = a->tuple_count;
        t->tuple_types = (Type**)aether_xrealloc(NULL, (size_t)a->tuple_count * sizeof(Type*));
        t->is_result = a->is_result;
        for (int i = 0; i < a->tuple_count; i++) t->tuple_types[i] = NULL;
        for (int i = 0; i < a->tuple_count; i++) {
            t->tuple_types[i] = branch_join_type(a->tuple_types[i], tuple_arm_part(a_arm, i),
                                                 b->tuple_types[i], tuple_arm_part(b_arm, i));
            if (!t->tuple_types[i]) {
                free_type(t);
                return NULL;
            }
        }
        return t;
    }
    if (types_equal(a, b)) return clone_type(a);
    /* #2980 review: either of two functions is called through the stronger
     * mode at each parameter; the then branch's type alone lost a `@keep`. */
    Type* fj = fn_type_mode_join(a, b);
    if (fj) return fj;
    /* #2983: an optional beside a value its element joins with, or beside
     * `none` (an optional of nothing known yet), is that optional. */
    if (a->kind == TYPE_OPTIONAL || b->kind == TYPE_OPTIONAL) {
        Type* ea = a->kind == TYPE_OPTIONAL ? a->element_type : a;
        Type* eb = b->kind == TYPE_OPTIONAL ? b->element_type : b;
        if (!type_known(ea)) return type_known(eb) ? create_optional_type(clone_type(eb)) : NULL;
        if (!type_known(eb)) return create_optional_type(clone_type(ea));
        Type* e = branch_join_type(ea, a->kind == TYPE_OPTIONAL ? NULL : a_arm,
                                   eb, b->kind == TYPE_OPTIONAL ? NULL : b_arm);
        return e ? create_optional_type(e) : NULL;
    }
    /* #2983, #2996: a sum beside one of its variants is the sum, and two
     * variants of one sum are it. */
    if (a->kind == TYPE_SUM && b->kind == TYPE_STRUCT && sum_has_variant(a, b->struct_name))
        return clone_type(a);
    if (b->kind == TYPE_SUM && a->kind == TYPE_STRUCT && sum_has_variant(b, a->struct_name))
        return clone_type(b);
    if (a->kind == TYPE_STRUCT && b->kind == TYPE_STRUCT && a->struct_name && b->struct_name) {
        const Type* s = sum_of_variants(a->struct_name, b->struct_name);
        return s ? clone_type((Type*)s) : NULL;
    }
    /* #3004: fixed-size arrays of one element type and length (types_equal
     * above); any other two arrays do not join. */
    return NULL;
}

Type* clone_type(Type* type) {
    if (!type) return NULL;

    Type* new_type = create_type(type->kind);
    new_type->array_size = type->array_size;
    
    if (type->element_type) {
        new_type->element_type = clone_type(type->element_type);
    }
    
    if (type->struct_name) {
        new_type->struct_name = strdup(type->struct_name);
    }
    new_type->qualifier = type->qualifier;   /* interned */
    if (type->c_alias) {
        new_type->c_alias = strdup(type->c_alias);
    }
    if (type->distinct_name) {
        new_type->distinct_name = strdup(type->distinct_name);
    }
    if (type->index_enum_name) {
        new_type->index_enum_name = strdup(type->index_enum_name);
    }

    if (type->tuple_types && type->tuple_count > 0) {
        new_type->tuple_count = type->tuple_count;
        new_type->tuple_types = malloc(type->tuple_count * sizeof(Type*));
        for (int i = 0; i < type->tuple_count; i++) {
            new_type->tuple_types[i] = clone_type(type->tuple_types[i]);
        }
        /* Clone per-position heap flags (issue #420). The flags
         * array is parallel to tuple_types; allocated lazily, so
         * NULL on the source means "all unannotated, default to
         * borrow" — clone preserves that NULL. */
        if (type->tuple_heap_flags) {
            new_type->tuple_heap_flags = malloc(type->tuple_count * sizeof(int));
            for (int i = 0; i < type->tuple_count; i++) {
                new_type->tuple_heap_flags[i] = type->tuple_heap_flags[i];
            }
        }
    }

    if (type->param_types && type->param_count > 0) {
        new_type->param_count = type->param_count;
        new_type->param_types = malloc(type->param_count * sizeof(Type*));
        for (int i = 0; i < type->param_count; i++) {
            new_type->param_types[i] = clone_type(type->param_types[i]);
        }
        for (int i = 0; type->param_modes && i < type->param_count; i++)
            fn_type_set_param_mode(new_type, i, type->param_modes[i]);
    }
    if (type->return_type) {
        new_type->return_type = clone_type(type->return_type);
    }
    new_type->is_fnptr = type->is_fnptr;
    new_type->compound_node = type->compound_node;  // borrowed; AST owns it.
    new_type->closure_literal = type->closure_literal;  // borrowed, as above (#2460)
    new_type->is_result = type->is_result;          // #913 `T!` marker

    return new_type;
}

int is_string_seq_ptr_type(const Type* t) {
    return t && t->kind == TYPE_PTR && t->element_type &&
           t->element_type->kind == TYPE_STRUCT &&
           t->element_type->struct_name &&
           strcmp(t->element_type->struct_name, "StringSeq") == 0;
}

Type* make_string_seq_ptr_type(void) {
    Type* t = create_type(TYPE_PTR);
    t->element_type = create_type(TYPE_STRUCT);
    t->element_type->struct_name = strdup("StringSeq");
    return t;
}

/* ---- interned names (#2539) ---------------------------------------------
 *
 * Names built while compiling (C types, mangled and normalised names, type
 * spellings) used to go into fixed buffers: 256 bytes for a C type, a
 * callee or a field path, 280 for a mangled name, and rotating static sets
 * that a later call overwrote while a caller still held one. A longer name
 * was cut, so two that shared a prefix became one C identifier. An interned
 * name has no length limit and is never reused for anything else. Kept for
 * the life of the process: the distinct names of a program are few, and
 * nothing has to know when to free one. */
typedef struct { char** slots; size_t cap; size_t count; } InternTable;
static InternTable g_intern;

static unsigned long long intern_hash(const char* s, size_t n) {
    unsigned long long h = 14695981039346656037ULL;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ULL; }
    return h;
}

const char* aether_intern_n(const char* s, size_t n) {
    if (g_intern.count * 4 >= g_intern.cap * 3) {
        size_t ncap = g_intern.cap ? g_intern.cap * 2 : 1024;
        char** slots = (char**)aether_xrealloc(NULL, ncap * sizeof(char*));
        memset(slots, 0, ncap * sizeof(char*));
        for (size_t i = 0; i < g_intern.cap; i++) {
            char* e = g_intern.slots[i];
            if (!e) continue;
            size_t j = (size_t)intern_hash(e, strlen(e)) & (ncap - 1);
            while (slots[j]) j = (j + 1) & (ncap - 1);
            slots[j] = e;
        }
        free(g_intern.slots);
        g_intern.slots = slots;
        g_intern.cap = ncap;
    }
    size_t j = (size_t)intern_hash(s, n) & (g_intern.cap - 1);
    for (char* e; (e = g_intern.slots[j]) != NULL; j = (j + 1) & (g_intern.cap - 1)) {
        if (strncmp(e, s, n) == 0 && e[n] == '\0') return e;
    }
    char* copy = (char*)aether_xrealloc(NULL, n + 1);
    memcpy(copy, s, n);
    copy[n] = '\0';
    g_intern.slots[j] = copy;
    g_intern.count++;
    return copy;
}

const char* aether_intern(const char* s) {
    return aether_intern_n(s, strlen(s));
}

const char* aether_internv(const char* fmt, va_list ap) {
    char small[256];
    va_list again;
    va_copy(again, ap);
    int n = vsnprintf(small, sizeof(small), fmt, ap);
    if (n < 0) n = 0;
    if ((size_t)n < sizeof(small)) {
        va_end(again);
        return aether_intern_n(small, (size_t)n);
    }
    char* big = (char*)aether_xrealloc(NULL, (size_t)n + 1);
    vsnprintf(big, (size_t)n + 1, fmt, again);
    va_end(again);
    const char* r = aether_intern_n(big, (size_t)n);
    free(big);
    return r;
}

const char* aether_internf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const char* r = aether_internv(fmt, ap);
    va_end(ap);
    return r;
}

void* aether_xrealloc(void* ptr, size_t size) {
    void* grown = realloc(ptr, size);
    if (!grown && size != 0) {
        fprintf(stderr, "Fatal: out of memory (requested %zu bytes)\n", size);
        free(ptr);
        exit(1);
    }
    return grown;
}

// AST Node functions
ASTNode* create_ast_node(ASTNodeType type, const char* value, int line, int column) {
    ASTNode* node = malloc(sizeof(ASTNode));
    node->type = type;
    node->value = value ? strdup(value) : NULL;
    node->node_type = NULL;
    node->children = NULL;
    node->child_count = 0;
    node->child_capacity = 0;
    node->line = line;
    node->column = column;
    node->annotation = NULL;
    node->is_imported = 0;
    node->bit_width = 0;
    node->bit_lo = 0;
    node->bit_hi = 0;
    node->source_file = NULL;
    node->type_inferred = 0;
    node->end_line = 0;
    node->end_column = 0;
    node->warned = 0;
    node->source_name = NULL;
    node->value_len = 0;
    node->origin_module = NULL;
    node->arg_order = NULL;
    node->arg_order_count = 0;
    node->callee_extern = NULL;
    return node;
}

void ast_set_literal_bytes(ASTNode* node, const char* bytes, int len) {
    char* copy = malloc((size_t)len + 1);
    if (!copy) {
        fprintf(stderr, "Fatal: out of memory copying a string literal\n");
        exit(1);
    }
    memcpy(copy, bytes, (size_t)len);
    copy[len] = '\0';
    free(node->value);
    node->value = copy;
    /* Only a literal with a NUL records its length, so a literal without
     * one is handled exactly as before, as a C string (#2520). */
    node->value_len = memchr(bytes, '\0', (size_t)len) ? len : 0;
}

/* Appends `child`, which may be NULL: an empty slot a node reads by position
 * (the missing init, condition or step of `for (;;)`). */
static void append_child(ASTNode* parent, ASTNode* child) {
    if (parent->child_count >= parent->child_capacity) {
        /* Reallocating to exactly count+1 on every append made building a
         * node with n children O(n^2); this is the compiler's hottest
         * allocation path. */
        size_t want = aether_buf_grow_capacity((size_t)parent->child_capacity, 4,
                                               (size_t)parent->child_count + 1,
                                               sizeof(ASTNode*));
        if (want == 0 || want > (size_t)INT_MAX) {
            fprintf(stderr, "Fatal: AST child array too large\n");
            exit(1);
        }
        ASTNode** new_children = realloc(parent->children, want * sizeof(ASTNode*));
        if (!new_children) {
            fprintf(stderr, "Fatal: out of memory adding AST child\n");
            exit(1);
        }
        parent->children = new_children;
        parent->child_capacity = (int)want;
    }
    parent->children[parent->child_count] = child;
    parent->child_count++;
}

void add_child(ASTNode* parent, ASTNode* child) {
    if (!parent || !child) return;
    append_child(parent, child);
}

ASTNode* clone_ast_node(ASTNode* node) {
    if (!node) return NULL;

    ASTNode* clone = create_ast_node(node->type, node->value, node->line, node->column);
    clone->node_type = clone_type(node->node_type);
    clone->annotation = node->annotation ? strdup(node->annotation) : NULL;
    clone->is_imported = node->is_imported;
    clone->bit_width = node->bit_width;
    clone->bit_lo = node->bit_lo;
    clone->bit_hi = node->bit_hi;
    clone->source_file = node->source_file ? strdup(node->source_file) : NULL;
    clone->type_inferred = node->type_inferred;
    clone->end_line = node->end_line;
    clone->end_column = node->end_column;
    clone->source_name = node->source_name ? strdup(node->source_name) : NULL;
    clone->origin_module = node->origin_module ? strdup(node->origin_module) : NULL;
    if (node->arg_order && node->arg_order_count > 0) {
        clone->arg_order = malloc(sizeof(int) * (size_t)node->arg_order_count);
        if (!clone->arg_order) {
            fprintf(stderr, "Fatal: out of memory cloning a call\n");
            exit(1);
        }
        memcpy(clone->arg_order, node->arg_order, sizeof(int) * (size_t)node->arg_order_count);
        clone->arg_order_count = node->arg_order_count;
    }
    clone->callee_extern = node->callee_extern;
    if (node->value_len > 0) ast_set_literal_bytes(clone, node->value, node->value_len);   /* #2520 */

    /* Slot for slot: an empty one stays where it is, as the parser left it. */
    for (int i = 0; i < node->child_count; i++) {
        append_child(clone, clone_ast_node(node->children[i]));
    }

    return clone;
}

void free_ast_node(ASTNode* node) {
    if (!node) return;
    
    if (node->value) {
        free(node->value);
    }

    if (node->annotation) {
        free(node->annotation);
    }

    if (node->source_file) {
        free(node->source_file);
    }

    if (node->source_name) {
        free(node->source_name);
    }

    free(node->origin_module);
    free(node->arg_order);

    if (node->node_type) {
        free_type(node->node_type);
    }
    
    for (int i = 0; i < node->child_count; i++) {
        free_ast_node(node->children[i]);
    }
    
    if (node->children) {
        free(node->children);
    }

    hoist_node_freed(node);
    free(node);
}

void ast_stamp_source_file(ASTNode* node, const char* path) {
    if (!node || !path) return;
    if (!node->source_file) {
        node->source_file = strdup(path);
    }
    for (int i = 0; i < node->child_count; i++) {
        ast_stamp_source_file(node->children[i], path);
    }
}

const char* ast_source_name(const ASTNode* def, char* buf, size_t cap) {
    const char* name = (def && def->value) ? def->value : "?";
    if (!def || !def->origin_module || !buf || cap == 0) return name;
    const char* ns = strrchr(def->origin_module, '.');
    ns = ns ? ns + 1 : def->origin_module;
    /* #2978: a merged function under a C name of its own keeps the name it
     * was written with. */
    if (def->source_name && (def->type == AST_FUNCTION_DEFINITION ||
                             def->type == AST_BUILDER_FUNCTION)) {
        snprintf(buf, cap, "%s.%s", ns, def->source_name);
        return buf;
    }
    size_t n = strlen(ns);
    if (strncmp(name, ns, n) != 0 || name[n] != '_' || !name[n + 1]) return name;
    snprintf(buf, cap, "%s.%s", ns, name + n + 1);
    return buf;
}

int ast_is_trailing_block(const ASTNode* n) {
    return n && n->type == AST_CLOSURE && n->value && strcmp(n->value, "trailing") == 0;
}

int ast_is_own_function(const ASTNode* n) {
    if (n && n->type == AST_QUOTE) return 1;
    return n && n->type == AST_CLOSURE && !ast_is_trailing_block(n);
}

int ast_has_value_return(const ASTNode* node) {
    if (!node || ast_is_own_function(node)) return 0;   /* a trailing block's are ours (#2766) */
    if (node->type == AST_RETURN_STATEMENT && node->child_count > 0 && node->children[0])
        return node->children[0]->type != AST_PRINT_STATEMENT;
    for (int i = 0; i < node->child_count; i++)
        if (ast_has_value_return(node->children[i])) return 1;
    return 0;
}

/* The `return`s of a value in `node` that give a literal of kind `lit`, and
 * those that give anything else. */
static void count_literal_returns(const ASTNode* node, ASTNodeType lit, int* hits, int* other) {
    if (!node || ast_is_own_function(node)) return;
    if (node->type == AST_RETURN_STATEMENT && node->child_count > 0 && node->children[0]) {
        if (node->children[0]->type == AST_PRINT_STATEMENT) return;
        if (node->children[0]->type == lit) (*hits)++;
        else (*other)++;
        return;
    }
    for (int i = 0; i < node->child_count; i++)
        count_literal_returns(node->children[i], lit, hits, other);
}

int ast_returns_only_literal(const ASTNode* node, ASTNodeType lit) {
    int hits = 0, other = 0;
    count_literal_returns(node, lit, &hits, &other);
    return hits > 0 && other == 0;
}

int ast_returns_literal(const ASTNode* node, ASTNodeType lit) {
    int hits = 0, other = 0;
    count_literal_returns(node, lit, &hits, &other);
    return hits > 0;
}

int ast_type_takes_null(const Type* t) {
    /* #3077: a `fn?` takes it as the empty closure, as a closure does. */
    if (t && t->kind == TYPE_OPTIONAL && t->element_type &&
        t->element_type->kind == TYPE_FUNCTION && !t->element_type->is_fnptr)
        return 1;
    return t && (t->kind == TYPE_PTR || t->kind == TYPE_STRING || t->kind == TYPE_FUNCTION ||
                 type_is_slice(t));
}

/* A clause whose result is inferred and every one of whose value `return`s
 * gives `null`: its result is whatever `null` goes into beside it. On 0.835
 * such a set took its first clause's type, and `node_or_null(0, p: *Node)
 * -> null` beside `node_or_null(k, p: *Node) -> p` compiled (#3061 review). */
static int clause_gives_only_null(const ASTNode* cl) {
    return cl->type_inferred && cl->node_type && cl->node_type->kind != TYPE_UNKNOWN &&
           ast_returns_only_literal(cl, AST_NULL_LITERAL);
}

/* #3063: a clause whose result is inferred and every one of whose value
 * `return`s gives `none`: it has no type of its own (the checker's
 * refine_inferred_result waits past `none`, an optional of no type yet),
 * and joins the others' as their optional. Known by its returns, as a
 * `null` clause is, since the early pass gives every clause the set's
 * result once it is settled: a `none` clause typed `string?` was then a
 * `string?` clause, beside which a `null` clause was refused. */
static int clause_gives_only_none(const ASTNode* cl) {
    return cl->type_inferred && ast_returns_only_literal(cl, AST_NONE_LITERAL);
}

/* The value `return`s in `node` that give a call of `name`, and the others. */
static void count_call_returns(const ASTNode* node, const char* name, int* hits, int* other) {
    if (!node || ast_is_own_function(node)) return;
    if (node->type == AST_RETURN_STATEMENT && node->child_count > 0 && node->children[0]) {
        const ASTNode* v = node->children[0];
        if (v->type == AST_PRINT_STATEMENT) return;
        if (v->type == AST_FUNCTION_CALL && v->value && !strcmp(v->value, name)) (*hits)++;
        else (*other)++;
        return;
    }
    for (int i = 0; i < node->child_count; i++) count_call_returns(node->children[i], name, hits, other);
}

/* #3063: a clause whose result is inferred and every one of whose value
 * `return`s calls its own set (`last(k) -> last(k - 1)`): it gives the
 * set's result, and no type of its own. Waited for as a clause still to
 * be inferred, it held the set unsettled for good when no other clause
 * gave a type: `last(0) -> none` beside it left `last(3) ?? -1` refused as
 * no optional. */
static int clause_only_recurses(const ASTNode* cl) {
    if (!cl->type_inferred || !cl->value) return 0;
    int hits = 0, other = 0;
    count_call_returns(cl, cl->value, &hits, &other);
    return hits > 0 && other == 0;
}

int clause_takes_set_result(const ASTNode* cl) {
    if (clause_gives_only_none(cl) || clause_only_recurses(cl)) return 1;
    return cl->node_type && cl->node_type->kind != TYPE_UNKNOWN &&
           cl->node_type->kind != TYPE_VOID;
}

/* The value of the one value `return` under `node`, counting them in `*n`. */
static void find_value_returns(const ASTNode* node, const ASTNode** only, int* n) {
    if (!node || ast_is_own_function(node)) return;
    if (node->type == AST_RETURN_STATEMENT && node->child_count > 0 && node->children[0]) {
        if (node->children[0]->type == AST_PRINT_STATEMENT) return;
        if ((*n)++ == 0) *only = node->children[0];
        return;
    }
    for (int i = 0; i < node->child_count; i++)
        find_value_returns(node->children[i], only, n);
}

/* #3064: what clause `cl` gives, when it has one value `return` (`f(0) ->
 * 0` has): the arm a join reads a literal from, as an if-expression's
 * branch is. NULL for a clause with several. */
static const ASTNode* clause_value_arm(const ASTNode* cl) {
    const ASTNode* only = NULL;
    int n = 0;
    find_value_returns(cl, &only, &n);
    return n == 1 ? only : NULL;
}

/* A literal, or one under a unary minus or plus. */
static int literal_arm(const ASTNode* n) {
    if (n && n->type == AST_UNARY_EXPRESSION && n->child_count == 1 && n->value &&
        (strcmp(n->value, "-") == 0 || strcmp(n->value, "+") == 0))
        n = n->children[0];
    return n && n->type == AST_LITERAL;
}

const ASTNode* join_arm_after(const ASTNode* kept, const ASTNode* next) {
    if (!literal_arm(next)) return NULL;
    int kneg, nneg;
    unsigned long long kmag, nmag;
    if (!integer_literal_arm(kept, &kneg, &kmag) || !integer_literal_arm(next, &nneg, &nmag))
        return kept;
    if (kneg != nneg) return kneg ? kept : next;
    return nmag > kmag ? next : kept;
}

int ast_number_holds(Type* w, Type* t, const ASTNode* v) {
    if (!w || !t) return 1;
    int is_signed, neg;
    unsigned long long mag;
    int bits = integer_kind_bits(w, &is_signed);
    if (bits && integer_literal_arm(v, &neg, &mag)) {
        if (!is_signed) return !neg && (bits == 64 || mag <= (1ull << bits) - 1);
        unsigned long long top = 1ull << (bits - 1);
        return neg ? mag <= top : mag < top;
    }
    Type* j = branch_join_type(w, NULL, t, v);
    int holds = j && types_equal(j, w);
    if (j) free_type(j);
    return holds;
}

static void clause_join_fail(ClauseJoin* out, ClauseJoinStatus st, ASTNode* at, Type* other) {
    out->status = st;
    out->at = at;
    out->other = other;
}

void clause_set_join(ASTNode* const* nodes, int count, ClauseJoin* out) {
    memset(out, 0, sizeof(*out));
    ASTNode* none_at = NULL;
    ASTNode* null_at = NULL;
    ASTNode* recurse_at = NULL;
    for (int c = 0; c < count; c++) {
        ASTNode* cl = nodes[c];
        Type* t = cl->node_type;
        if (clause_gives_only_none(cl)) {
            if (!none_at) none_at = cl;
            continue;
        }
        if (clause_only_recurses(cl)) {   /* gives the set's result, no type of its own */
            if (!recurse_at) recurse_at = cl;
            continue;
        }
        if (!t || t->kind == TYPE_UNKNOWN) {
            if (ast_has_value_return(cl)) {   /* still to be inferred (#3056) */
                out->status = CLAUSE_JOIN_WAIT;
                return;
            }
            continue;
        }
        if (clause_gives_only_null(cl)) {
            if (!null_at) null_at = cl;
            continue;
        }
        if (!out->written && !cl->type_inferred && t->kind != TYPE_VOID) out->written = cl;
    }
    ASTNode* written = out->written;
    Type* join = written ? clone_type(written->node_type) : NULL;
    ASTNode* join_at = written;
    /* #3064: each clause's value is its arm, as an if-expression's branches
     * are, so a literal clause beside a uint64 one is a uint64, as `if c { 0
     * } else { u }` is: the value the join so far came from, and while every
     * clause after it gives a literal, the literal the join must hold most
     * (join_arm_after), as a value match's arms are read. A written type has
     * none. */
    const ASTNode* join_arm = NULL;
    for (int c = 0; c < count; c++) {
        ASTNode* cl = nodes[c];
        Type* t = cl->node_type;
        if (!t || t->kind == TYPE_UNKNOWN || t->kind == TYPE_VOID || cl == written ||
            clause_gives_only_null(cl) || clause_gives_only_none(cl) || clause_only_recurses(cl))
            continue;
        const ASTNode* arm = clause_value_arm(cl);
        if (!join) {
            join = clone_type(t);
            join_at = cl;
            join_arm = arm;
            continue;
        }
        Type* k = branch_join_type(join, join_arm, t, arm);
        join_arm = join_arm_after(join_arm, arm);
        /* Beside a written type, an inferred clause fits it (their join is
         * it, and one with no join does not); a second written type is the
         * same one or none. #3082: a value the written type holds fits it
         * (ast_number_holds), as a local's written type holds a binding
         * (#3070): `g(0) -> 5` beside `-> uint8`, which the join of an int
         * and a uint8 (an int) refused. */
        if (written && cl->type_inferred && ast_number_holds(join, t, arm)) {
            if (k) free_type(k);
            k = clone_type(join);
        }
        int unfit = written &&
                    (!k || !types_equal(k, join) || (!cl->type_inferred && !types_equal(t, join)));
        if (!k || unfit) {
            if (k) free_type(k);
            out->join = join;
            out->join_at = join_at;
            clause_join_fail(out, unfit ? CLAUSE_JOIN_UNFIT : CLAUSE_JOIN_APART, cl, t);
            return;
        }
        if (!types_equal(k, join)) join_at = cl;
        free_type(join);
        join = k;
    }
    /* `null` takes the pointer, string or closure type beside it, as a slot
     * of that type takes `null`; alone, it is the `ptr` it is. #3063: and
     * the optional of one, as `return null` from `-> string?` gives the
     * string: the early pass joins `null` with `"s"` and then `none`, and
     * gives every clause that `string?`, so the checker's join of the
     * same clauses meets `null` beside a `string?` clause. */
    if (null_at) {
        int takes = join && (ast_type_takes_null(join) ||
                             (join->kind == TYPE_OPTIONAL && ast_type_takes_null(join->element_type)));
        if (!join) {
            join = clone_type(null_at->node_type);
            join_at = null_at;
        } else if (!takes) {
            out->join = join;
            out->join_at = join_at;
            clause_join_fail(out, written ? CLAUSE_JOIN_NULL_UNFIT : CLAUSE_JOIN_NULL_APART,
                             null_at, NULL);
            return;
        }
    }
    /* `none` joins the rest as `T?`, as an if-expression's `none` arm does. */
    if (none_at) {
        if (!join) {
            out->recurse_at = recurse_at;
            clause_join_fail(out, CLAUSE_JOIN_NONE_ONLY, none_at, NULL);
            return;
        }
        Type* none_t = create_optional_type(create_type(TYPE_UNKNOWN));
        Type* k = branch_join_type(join, NULL, none_t, NULL);
        free_type(none_t);
        int unfit = written && k && !types_equal(k, join);
        if (!k || unfit) {
            if (k) free_type(k);
            out->join = join;
            out->join_at = join_at;
            clause_join_fail(out, unfit ? CLAUSE_JOIN_NONE_UNFIT : CLAUSE_JOIN_NONE_APART,
                             none_at, NULL);
            return;
        }
        free_type(join);
        join = k;
    }
    if (!join) {
        out->status = CLAUSE_JOIN_NO_VALUE;
        return;
    }
    out->status = CLAUSE_JOIN_OK;
    out->join = join;
    out->join_at = join_at;
}

/* The arm codegen lowers to an unconditional `else`. The string literal "_"
 * is a string it compares with, not the wildcard. */
static int ast_is_wildcard_pattern(const ASTNode* p) {
    if (!p) return 0;
    if (p->node_type && p->node_type->kind == TYPE_WILDCARD) return 1;
    return p->type == AST_IDENTIFIER && p->value && strcmp(p->value, "_") == 0;
}

/* A list-pattern element that matches any value: a binding or `_`. */
static int pattern_binds_any(const ASTNode* p) {
    return p && (p->type == AST_PATTERN_VARIABLE || ast_is_wildcard_pattern(p));
}

int ast_struct_pattern_tests(const ASTNode* p) {
    for (int k = 0; p && k < p->child_count; k++) {
        const ASTNode* fp = p->children[k];
        if (!fp || fp->type != AST_ASSIGNMENT || fp->child_count < 1 || !fp->children[0]) continue;
        const ASTNode* sub = fp->children[0];
        if (sub->type == AST_PATTERN_LITERAL && !ast_is_wildcard_pattern(sub)) return 1;
        if (sub->type == AST_PATTERN_STRUCT && ast_struct_pattern_tests(sub)) return 1;
    }
    return 0;
}

int ast_match_covers(const ASTNode* m) {
    if (!m || m->type != AST_MATCH_STATEMENT) return 0;
    Type* t = (m->child_count > 0 && m->children[0]) ? m->children[0]->node_type : NULL;
    int has_none = 0, has_some = 0, has_true = 0, has_false = 0;
    int has_empty = 0, has_cons = 0;
    for (int i = 1; i < m->child_count; i++) {
        ASTNode* arm = m->children[i];
        if (!arm || arm->type != AST_MATCH_ARM || arm->child_count < 1) continue;
        ASTNode* p = arm->children[0];
        if (!p) continue;
        if (ast_is_wildcard_pattern(p)) return 1;
        /* #2992: a struct pattern that tests no field (each `_`, a name or
         * such a pattern) takes every value of its struct; one with a
         * literal takes only some. */
        if (p->type == AST_PATTERN_STRUCT && !ast_struct_pattern_tests(p)) return 1;
        if (p->type == AST_NONE_LITERAL) has_none = 1;
        else if (p->type == AST_PATTERN_VARIABLE && p->annotation &&
                 strcmp(p->annotation, "some_pattern") == 0) has_some = 1;
        else if (p->type == AST_LITERAL && p->value && strcmp(p->value, "true") == 0) has_true = 1;
        else if (p->type == AST_LITERAL && p->value && strcmp(p->value, "false") == 0) has_false = 1;
        else if (p->type == AST_PATTERN_LIST && p->child_count == 0) has_empty = 1;
        else if (p->type == AST_PATTERN_CONS && p->child_count == 2 &&
                 pattern_binds_any(p->children[0]) && pattern_binds_any(p->children[1]))
            has_cons = 1;
    }
    if (t) {
        /* The checker rejects a match over an enum or a sum that misses a
         * case, so one that reaches here covers it. */
        if (t->kind == TYPE_ENUM || t->kind == TYPE_SUM) return 1;
        if (t->kind == TYPE_OPTIONAL) return has_none && has_some;
        if (t->kind == TYPE_BOOL) return has_true && has_false;
    }
    return has_empty && has_cons;
}

/* Is `n` an access chain (fields, elements) rooted at identifier `pname`? */
static int access_chain_rooted_at(const ASTNode* n, const char* pname) {
    while (n && (n->type == AST_MEMBER_ACCESS || n->type == AST_ARRAY_ACCESS) &&
           n->child_count >= 1)
        n = n->children[0];
    return n && n->type == AST_IDENTIFIER && n->value && strcmp(n->value, pname) == 0;
}

/* R3 (#2582): does `n` write into the struct parameter `pname`: rebind it,
 * store into a field or an element of it (`p.f = v`, `p.xs[i] = v`), or grow
 * a slice field of it in place (`p.xs.push(v)`, #3568)? Codegen makes such a
 * parameter a value of its own on entry, destroyed at its exit, so a store
 * or a growth releases nothing of its caller's, and slice ownership counts
 * its fields as holding references; any other borrows. */
int ast_struct_param_written(const ASTNode* n, const char* pname) {
    if (!n || !pname) return 0;
    if (n->type == AST_VARIABLE_DECLARATION && n->value && strcmp(n->value, pname) == 0) return 1;
    if (n->type == AST_BINARY_EXPRESSION && n->value && strcmp(n->value, "=") == 0 &&
        n->child_count >= 1 && n->children[0] &&
        (n->children[0]->type == AST_MEMBER_ACCESS || n->children[0]->type == AST_ARRAY_ACCESS) &&
        access_chain_rooted_at(n->children[0], pname)) return 1;
    if (n->type == AST_FUNCTION_CALL && n->value && strcmp(n->value, "__slice_push") == 0 &&
        n->child_count >= 1 && n->children[0] && n->children[0]->type == AST_MEMBER_ACCESS &&
        access_chain_rooted_at(n->children[0], pname)) return 1;
    for (int i = 0; i < n->child_count; i++)
        if (ast_struct_param_written(n->children[i], pname)) return 1;
    return 0;
}

int ast_match_is_operand(const ASTNode* m) {
    return m && m->type == AST_MATCH_STATEMENT && annotation_has_marker(m->annotation, "operand");
}

int annotation_has_marker(const char* annotation, const char* marker) {
    if (!annotation || !marker || !*marker) return 0;
    size_t n = strlen(marker);
    const char* p = annotation;
    while ((p = strstr(p, marker)) != NULL) {
        int left_ok  = (p == annotation) || (p[-1] == ';');
        int right_ok = (p[n] == '\0') || (p[n] == ';');
        if (left_ok && right_ok) return 1;
        p += n;
    }
    return 0;
}

ASTNode* match_arm_value(ASTNode* body) {
    if (!body) return NULL;
    switch (body->type) {
        case AST_PRINT_STATEMENT:
        case AST_RETURN_STATEMENT:
        case AST_VARIABLE_DECLARATION:
        case AST_ASSIGNMENT:
        case AST_EXPRESSION_STATEMENT:
            return NULL;
        case AST_BLOCK: {
            if (body->child_count == 0) return NULL;
            ASTNode* last = body->children[body->child_count - 1];
            if (!last) return NULL;
            if (last->type == AST_MATCH_STATEMENT) return last;
            if (last->type != AST_EXPRESSION_STATEMENT || last->child_count < 1) return NULL;
            ASTNode* e = last->children[0];
            if (!e || !e->node_type || e->node_type->kind == TYPE_VOID) return NULL;
            if (e->type == AST_BINARY_EXPRESSION && e->value &&
                strcmp(e->value, "=") == 0) return NULL;
            return e;
        }
        default:
            return body;
    }
}

char* annotation_add_marker(char* annotation, const char* marker) {
    if (!marker || !*marker) return annotation;
    if (!annotation) return strdup(marker);
    if (annotation_has_marker(annotation, marker)) return annotation;

    size_t cur = strlen(annotation);
    size_t n = strlen(marker);
    char* combined = (char*)malloc(cur + 1 + n + 1);
    if (!combined) return annotation;
    memcpy(combined, annotation, cur);
    combined[cur] = ';';
    memcpy(combined + cur + 1, marker, n);
    combined[cur + 1 + n] = '\0';
    free(annotation);
    return combined;
}

void print_ast(ASTNode* node, int indent) {
    if (!node) return;
    
    for (int i = 0; i < indent; i++) {
        printf("  ");
    }
    
    printf("%s", ast_node_type_to_string(node->type));
    
    if (node->value) {
        printf(": %s", node->value);
    }
    
    if (node->node_type) {
        printf(" [%s]", type_to_string(node->node_type));
    }
    
    printf(" (line %d, col %d)\n", node->line, node->column);
    
    for (int i = 0; i < node->child_count; i++) {
        print_ast(node->children[i], indent + 1);
    }
}

int lane_accessor_index(TypeKind kind, const char* field) {
    int lanes;
    switch (kind) {
        case TYPE_F32X4: case TYPE_I32X4: lanes = 4; break;
        case TYPE_F32X8: case TYPE_I32X8: lanes = 8; break;
        case TYPE_F64X2: case TYPE_I64X2: lanes = 2; break;
        default: return -1;
    }
    if (!field || !field[0] || field[1]) return -1;
    int idx;
    switch (field[0]) {
        case 'x': idx = 0; break;
        case 'y': idx = 1; break;
        case 'z': idx = 2; break;
        case 'w': idx = 3; break;
        default: return -1;
    }
    return idx < lanes ? idx : -1;
}

const char* ast_node_type_to_string(ASTNodeType type) {
    switch (type) {
        case AST_PROGRAM: return "PROGRAM";
        case AST_MODULE_DECLARATION: return "MODULE_DECLARATION";
        case AST_IMPORT_STATEMENT: return "IMPORT_STATEMENT";
        case AST_EXPORT_STATEMENT: return "EXPORT_STATEMENT";
        case AST_ACTOR_DEFINITION: return "ACTOR_DEFINITION";
        case AST_FUNCTION_DEFINITION: return "FUNCTION_DEFINITION";
        case AST_FUNCTION_CLAUSE: return "FUNCTION_CLAUSE";
        case AST_MAIN_FUNCTION: return "MAIN_FUNCTION";
        case AST_STRUCT_DEFINITION: return "STRUCT_DEFINITION";
        case AST_C_STRUCT_DEF: return "C_STRUCT_DEF";
        case AST_STRUCT_FIELD: return "STRUCT_FIELD";
        case AST_STRUCT_FIELD_UNION: return "STRUCT_FIELD_UNION";
        case AST_STRUCT_FIELD_NESTED: return "STRUCT_FIELD_NESTED";
        case AST_BLOCK: return "BLOCK";
        case AST_VARIABLE_DECLARATION: return "VARIABLE_DECLARATION";
        case AST_TUPLE_DESTRUCTURE: return "TUPLE_DESTRUCTURE";
        case AST_ASSIGNMENT: return "ASSIGNMENT";
        case AST_COMPOUND_ASSIGNMENT: return "COMPOUND_ASSIGNMENT";
        case AST_IF_STATEMENT: return "IF_STATEMENT";
        case AST_WHEN_STATEMENT: return "WHEN_STATEMENT";
        case AST_FOR_LOOP: return "FOR_LOOP";
        case AST_WHILE_LOOP: return "WHILE_LOOP";
        case AST_SWITCH_STATEMENT: return "SWITCH_STATEMENT";
        case AST_CASE_STATEMENT: return "CASE_STATEMENT";
        case AST_RETURN_STATEMENT: return "RETURN_STATEMENT";
        case AST_BREAK_STATEMENT: return "BREAK_STATEMENT";
        case AST_CONTINUE_STATEMENT: return "CONTINUE_STATEMENT";
        case AST_EXPRESSION_STATEMENT: return "EXPRESSION_STATEMENT";
        case AST_MATCH_STATEMENT: return "MATCH_STATEMENT";
        case AST_MATCH_ARM: return "MATCH_ARM";
        case AST_PATTERN_LITERAL: return "PATTERN_LITERAL";
        case AST_PATTERN_VARIABLE: return "PATTERN_VARIABLE";
        case AST_PATTERN_STRUCT: return "PATTERN_STRUCT";
        case AST_PATTERN_LIST: return "PATTERN_LIST";
        case AST_PATTERN_CONS: return "PATTERN_CONS";
        case AST_GUARD_CLAUSE: return "GUARD_CLAUSE";
        case AST_RECEIVE_STATEMENT: return "RECEIVE_STATEMENT";
        case AST_SEND_STATEMENT: return "SEND_STATEMENT";
        case AST_SPAWN_ACTOR_STATEMENT: return "SPAWN_ACTOR_STATEMENT";
        case AST_STATE_DECLARATION: return "STATE_DECLARATION";
        case AST_HIDE_DIRECTIVE: return "HIDE_DIRECTIVE";
        case AST_SEAL_DIRECTIVE: return "SEAL_DIRECTIVE";
        case AST_TRY_STATEMENT: return "TRY_STATEMENT";
        case AST_CATCH_CLAUSE: return "CATCH_CLAUSE";
        case AST_PANIC_STATEMENT: return "PANIC_STATEMENT";
        case AST_MESSAGE_DEFINITION: return "MESSAGE_DEFINITION";
        case AST_MESSAGE_FIELD: return "MESSAGE_FIELD";
        case AST_RECEIVE_ARM: return "RECEIVE_ARM";
        case AST_MESSAGE_PATTERN: return "MESSAGE_PATTERN";
        case AST_PATTERN_FIELD: return "PATTERN_FIELD";
        case AST_WILDCARD_PATTERN: return "WILDCARD_PATTERN";
        case AST_REPLY_STATEMENT: return "REPLY_STATEMENT";
        case AST_MESSAGE_CONSTRUCTOR: return "MESSAGE_CONSTRUCTOR";
        case AST_FIELD_INIT: return "FIELD_INIT";
        case AST_SEND_FIRE_FORGET: return "SEND_FIRE_FORGET";
        case AST_SEND_ASK: return "SEND_ASK";
        case AST_BINARY_EXPRESSION: return "BINARY_EXPRESSION";
        case AST_UNARY_EXPRESSION: return "UNARY_EXPRESSION";
        case AST_TUPLE_UNWRAP: return "TUPLE_UNWRAP";
        case AST_HEAP_NEW: return "HEAP_NEW";
        case AST_FUNCTION_CALL: return "FUNCTION_CALL";
        case AST_ACTOR_REF: return "ACTOR_REF";
        case AST_IDENTIFIER: return "IDENTIFIER";
        case AST_LITERAL: return "LITERAL";
        case AST_ARRAY_LITERAL: return "ARRAY_LITERAL";
        case AST_ARRAY_ACCESS: return "ARRAY_ACCESS";
        case AST_SLICE_EXPR: return "SLICE_EXPR";
        case AST_SLICE_FROM_ARRAY: return "SLICE_FROM_ARRAY";
        case AST_SLICE_TO_PTR: return "SLICE_TO_PTR";
        case AST_MEMBER_ACCESS: return "MEMBER_ACCESS";
        case AST_STRUCT_LITERAL: return "STRUCT_LITERAL";
        case AST_TYPE_ANNOTATION: return "TYPE_ANNOTATION";
        case AST_ACTOR_REF_TYPE: return "ACTOR_REF_TYPE";
        case AST_ARRAY_TYPE: return "ARRAY_TYPE";
        case AST_PRINT_STATEMENT: return "PRINT_STATEMENT";
        case AST_NULL_LITERAL: return "NULL_LITERAL";
        case AST_IF_EXPRESSION: return "IF_EXPRESSION";
        case AST_SIZEOF: return "SIZEOF";
        case AST_OFFSETOF: return "OFFSETOF";
        case AST_VA_START: return "VA_START";
        case AST_VA_ARG: return "VA_ARG";
        case AST_VA_END: return "VA_END";
        case AST_CONST_DECLARATION: return "CONST_DECLARATION";
        case AST_STRING_INTERP: return "STRING_INTERP";
        case AST_EXTERN_FUNCTION: return "EXTERN_FUNCTION";
        case AST_DEFER_STATEMENT: return "DEFER_STATEMENT";
        case AST_CLOSURE: return "CLOSURE";
        case AST_CLOSURE_PARAM: return "CLOSURE_PARAM";
        case AST_NAMED_ARG: return "NAMED_ARG";
        case AST_NONE_LITERAL: return "NONE_LITERAL";       // #340
        case AST_NULL_COALESCE: return "NULL_COALESCE";     // #340
        case AST_OPTIONAL_CHAIN: return "OPTIONAL_CHAIN";   // #340
        case AST_SUM_TYPE_DEF: return "SUM_TYPE_DEF";       // #914
        case AST_CFN_TYPE_DEF: return "CFN_TYPE_DEF";       // #2200
        case AST_FIELD_ATTRIBUTE: return "FIELD_ATTRIBUTE"; // #2298
        case AST_SCHEMA_OF: return "SCHEMA_OF";             // #2298
        case AST_QUOTE: return "QUOTE";                     // docs/quote.md
        case AST_PROTOCOL_DEFINITION: return "PROTOCOL_DEFINITION";   // #385
        case AST_PROTOCOL_TRANSITION: return "PROTOCOL_TRANSITION";
        default: return "UNKNOWN";
    }
}

// Utility functions
ASTNode* create_literal_node(Token* token) {
    Type* type = NULL;
    
    switch (token->type) {
        case TOKEN_NUMBER:
            // Let type inference determine if it's int or float
            type = create_type(TYPE_UNKNOWN);
            break;
        case TOKEN_STRING_LITERAL:
            type = create_type(TYPE_STRING);
            break;
        case TOKEN_TRUE:
        case TOKEN_FALSE:
            type = create_type(TYPE_BOOL);
            break;
        default:
            type = create_type(TYPE_UNKNOWN);
            break;
    }
    
    ASTNode* node = create_ast_node(AST_LITERAL, token->value, token->line, token->column);
    node->node_type = type;
    /* A string literal holding a NUL keeps every byte (#2520). */
    if (token->value_len > 0) ast_set_literal_bytes(node, token->value, token->value_len);
    return node;
}

ASTNode* create_identifier_node(Token* token) {
    ASTNode* node = create_ast_node(AST_IDENTIFIER, token->value, token->line, token->column);
    node->node_type = create_type(TYPE_UNKNOWN); // Will be resolved during type checking
    return node;
}

ASTNode* create_binary_expression(ASTNode* left, ASTNode* right, Token* operator) {
    ASTNode* node = create_ast_node(AST_BINARY_EXPRESSION, operator->value, operator->line, operator->column);
    add_child(node, left);
    add_child(node, right);
    return node;
}

ASTNode* create_unary_expression(ASTNode* operand, Token* operator) {
    ASTNode* node = create_ast_node(AST_UNARY_EXPRESSION, operator->value, operator->line, operator->column);
    add_child(node, operand);
    return node;
}
