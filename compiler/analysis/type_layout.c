/* The size and alignment of Aether types as the generated C lays them out.
 * See type_layout.h. Every case names the C the codegen emits for it, so a
 * change there has its counterpart here; `ae pad --verify` compiles
 * `_Static_assert`s of these numbers against the real C compiler. */

#include "type_layout.h"

#include <string.h>

#define LAYOUT_MAX_DEPTH 64

static int layout_of(const Type* t, ASTNode* program, const LayoutTarget* tgt,
                     TypeLayout* out, int depth);

void layout_target_default(LayoutTarget* t) {
    t->ptr_size = 8;
    t->i64_align = 8;
    t->ldbl_size = 16;
    t->ldbl_align = 16;
}

static long long align_up(long long n, int a) {
    return a > 1 ? (n + a - 1) / a * a : n;
}

static int set(TypeLayout* out, long long size, int align) {
    out->size = size;
    out->align = align;
    return 1;
}

/* A C struct of `n` members, in order, with C's placement: each member at
 * the next multiple of its alignment, the size rounded to the largest. */
typedef struct {
    long long size;
    int align;
    int packed;
} StructAcc;

static long long acc_add(StructAcc* a, const TypeLayout* m) {
    long long off = a->packed ? a->size : align_up(a->size, m->align);
    a->size = off + m->size;
    if (!a->packed && m->align > a->align) a->align = m->align;
    return off;
}

static void acc_close(StructAcc* a, TypeLayout* out) {
    out->align = a->align;
    out->size = a->packed ? a->size : align_up(a->size, a->align);
}

/* The C spelling a `c_alias` gives a scalar (size_t, uint32_t, ...): its
 * size, or 0 for an alias this table does not know (`long` is 4 bytes on
 * LLP64 and 8 on LP64, so it is not guessed). */
static int alias_size(const char* alias, const LayoutTarget* tgt) {
    static const char* const ptr_sized[] = {
        "size_t", "ssize_t", "intptr_t", "uintptr_t", "ptrdiff_t", NULL
    };
    for (int i = 0; ptr_sized[i]; i++)
        if (strcmp(alias, ptr_sized[i]) == 0) return tgt->ptr_size;
    if (!strcmp(alias, "int64_t") || !strcmp(alias, "uint64_t") ||
        !strcmp(alias, "long long") || !strcmp(alias, "unsigned long long")) return 8;
    if (!strcmp(alias, "int32_t") || !strcmp(alias, "uint32_t") || !strcmp(alias, "int") ||
        !strcmp(alias, "unsigned int") || !strcmp(alias, "unsigned")) return 4;
    if (!strcmp(alias, "int16_t") || !strcmp(alias, "uint16_t") || !strcmp(alias, "short") ||
        !strcmp(alias, "unsigned short")) return 2;
    if (!strcmp(alias, "int8_t") || !strcmp(alias, "uint8_t") || !strcmp(alias, "char") ||
        !strcmp(alias, "signed char") || !strcmp(alias, "unsigned char")) return 1;
    return 0;
}

static int scalar(TypeLayout* out, int size, const LayoutTarget* tgt) {
    return set(out, size, size == 8 ? tgt->i64_align : size);
}

static ASTNode* struct_def_named(ASTNode* program, const char* name) {
    if (!program || !name) return NULL;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_STRUCT_DEFINITION && c->value && strcmp(c->value, name) == 0)
            return c;
    }
    return NULL;
}

static int members_layout(ASTNode* parent, int is_extern, int is_union, int packed,
                          ASTNode* program, const LayoutTarget* tgt, TypeLayout* out,
                          long long* offsets, int depth);
static int struct_layout_at(ASTNode* sd, ASTNode* program, const LayoutTarget* tgt,
                            TypeLayout* out, long long* offsets, int depth);

/* One member of a struct body as generate_extern_struct_field emits it. */
static int field_layout(ASTNode* f, int is_extern, int is_last, ASTNode* program,
                        const LayoutTarget* tgt, TypeLayout* out, int depth) {
    if (f->type == AST_STRUCT_FIELD_UNION || f->type == AST_STRUCT_FIELD_NESTED)
        return members_layout(f, is_extern, f->type == AST_STRUCT_FIELD_UNION, 0,
                              program, tgt, out, NULL, depth + 1);
    if (f->bit_width > 0 || !f->node_type) return 0;   /* a C bitfield: the compiler's choice */
    const Type* t = f->node_type;
    if (t->kind == TYPE_ARRAY && t->array_size <= 0 && is_extern) {
        TypeLayout e;
        if (!layout_of(t->element_type, program, tgt, &e, depth + 1)) return 0;
        if (is_last) return set(out, 0, e.align);         /* `T name[];` */
        return set(out, tgt->ptr_size, tgt->ptr_size);    /* `T* name;` */
    }
    return layout_of(t, program, tgt, out, depth + 1);
}

static int members_layout(ASTNode* parent, int is_extern, int is_union, int packed,
                          ASTNode* program, const LayoutTarget* tgt, TypeLayout* out,
                          long long* offsets, int depth) {
    if (depth > LAYOUT_MAX_DEPTH) return 0;
    StructAcc acc = { 0, 1, packed };
    long long union_size = 0;
    for (int i = 0; i < parent->child_count; i++) {
        ASTNode* f = parent->children[i];
        if (offsets) offsets[i] = -1;
        if (!f || (f->type != AST_STRUCT_FIELD && f->type != AST_STRUCT_FIELD_UNION &&
                   f->type != AST_STRUCT_FIELD_NESTED)) continue;
        TypeLayout m;
        if (!field_layout(f, is_extern, i == parent->child_count - 1, program, tgt, &m, depth))
            return 0;
        if (is_union) {
            if (m.size > union_size) union_size = m.size;
            if (m.align > acc.align) acc.align = m.align;
            if (offsets) offsets[i] = 0;
        } else {
            long long off = acc_add(&acc, &m);
            if (offsets) offsets[i] = off;
        }
    }
    if (is_union) acc.size = union_size;
    acc_close(&acc, out);
    return 1;
}

static int struct_layout_at(ASTNode* sd, ASTNode* program, const LayoutTarget* tgt,
                            TypeLayout* out, long long* offsets, int depth) {
    if (!sd || sd->type != AST_STRUCT_DEFINITION) return 0;
    const char* ann = sd->annotation;
    if (ann && (!strcmp(ann, "extern_opaque") || !strcmp(ann, "extern_c_import") ||
                !strcmp(ann, "extern_c_import_union"))) return 0;
    int is_extern = ann && (!strcmp(ann, "extern") || !strcmp(ann, "extern_packed"));
    int packed = ann && !strcmp(ann, "extern_packed");
    /* A struct with no fields is GNU C's empty struct, size 0; cl.exe gets
     * a byte (generate_struct_definition). */
    return members_layout(sd, is_extern, 0, packed, program, tgt, out, offsets, depth);
}

int struct_layout_of(ASTNode* sd, ASTNode* program, const LayoutTarget* tgt,
                     TypeLayout* out, long long* offsets) {
    return struct_layout_at(sd, program, tgt, out, offsets, 0);
}

static int layout_of(const Type* t, ASTNode* program, const LayoutTarget* tgt,
                     TypeLayout* out, int depth) {
    if (!t || depth > LAYOUT_MAX_DEPTH) return 0;
    if (t->c_alias) {
        int n = alias_size(t->c_alias, tgt);
        return n ? scalar(out, n, tgt) : 0;
    }
    switch (t->kind) {
        case TYPE_INT: case TYPE_UINT32: case TYPE_FLOAT32: case TYPE_ENUM:
        case TYPE_BOOL:                          /* `int` in a struct (get_c_type) */
            return set(out, 4, 4);
        case TYPE_UINT16:
            return set(out, 2, 2);
        case TYPE_BYTE: case TYPE_UINT8:
            return set(out, 1, 1);
        case TYPE_INT64: case TYPE_UINT64: case TYPE_DURATION: case TYPE_FLOAT:
        case TYPE_BITSET:                        /* unsigned long long */
            return set(out, 8, tgt->i64_align);
        case TYPE_LONGDOUBLE:
            return set(out, tgt->ldbl_size, tgt->ldbl_align);
        case TYPE_STRING: case TYPE_PTR: case TYPE_ACTOR_REF:
            return set(out, tgt->ptr_size, tgt->ptr_size);
        case TYPE_FUNCTION:                      /* a C function pointer, or _AeClosure */
            return t->is_fnptr ? set(out, tgt->ptr_size, tgt->ptr_size)
                               : set(out, 2LL * tgt->ptr_size, tgt->ptr_size);
        case TYPE_F32X4: case TYPE_F64X2: case TYPE_I32X4: case TYPE_I64X2: case TYPE_I16X8:
            return set(out, 16, 16);
        case TYPE_F32X8: case TYPE_I32X8:        /* vector_size(32), aligned(16) */
            return set(out, 32, 16);
        case TYPE_BITSTRUCT: case TYPE_ISOLATED:
            return layout_of(t->element_type, program, tgt, out, depth + 1);
        case TYPE_ARRAY: {
            if (t->array_size > 0) {
                TypeLayout e;
                if (!layout_of(t->element_type, program, tgt, &e, depth + 1)) return 0;
                return set(out, e.size * t->array_size, e.align);
            }
            /* AetherSlice: void* ptr; uint64_t len; uint32_t flags, off. */
            StructAcc a = { 0, 1, 0 };
            TypeLayout p = { tgt->ptr_size, tgt->ptr_size }, l = { 8, tgt->i64_align },
                       w = { 4, 4 };
            acc_add(&a, &p); acc_add(&a, &l); acc_add(&a, &w); acc_add(&a, &w);
            acc_close(&a, out);
            return 1;
        }
        case TYPE_OPTIONAL: {                    /* struct { int has; T val; } */
            TypeLayout has = { 4, 4 }, v;
            if (!layout_of(t->element_type, program, tgt, &v, depth + 1)) return 0;
            StructAcc a = { 0, 1, 0 };
            acc_add(&a, &has); acc_add(&a, &v);
            acc_close(&a, out);
            return 1;
        }
        case TYPE_TUPLE: {                       /* struct { T0 _0; T1 _1; ... } */
            StructAcc a = { 0, 1, 0 };
            for (int i = 0; i < t->tuple_count; i++) {
                TypeLayout e;
                if (!layout_of(t->tuple_types[i], program, tgt, &e, depth + 1)) return 0;
                acc_add(&a, &e);
            }
            acc_close(&a, out);
            return 1;
        }
        case TYPE_STRUCT:
            return struct_layout_at(struct_def_named(program, t->struct_name), program, tgt,
                                    out, NULL, depth + 1);
        default:                                 /* sums, messages, unknown: not modelled */
            return 0;
    }
}

int type_layout_of(const Type* t, ASTNode* program, const LayoutTarget* tgt, TypeLayout* out) {
    return layout_of(t, program, tgt, out, 0);
}
