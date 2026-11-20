#ifndef AETHER_TYPE_LAYOUT_H
#define AETHER_TYPE_LAYOUT_H

/* The size and alignment of an Aether type as the generated C lays it out
 * (#3773, #3775). aetherc names C types and the C compiler places them;
 * this is the one table of what that placement is, per target, for the
 * types codegen emits: the primitive C types (codegen.c get_c_type), slices,
 * closures, optionals, tuples and Aether structs, recursively. Message field
 * order (codegen.c message_struct_fields) and `ae pad` read it. A type whose
 * layout comes from somewhere this does not model (a @c_import struct, an
 * opaque type, a bitfield, a sum's payload) is reported as unknown, never
 * guessed. */

#include "../ast.h"

typedef struct {
    int ptr_size;     /* void*, size_t, a string, an actor reference */
    int i64_align;    /* int64_t and double inside a struct: 8, or 4 on i386 SysV */
    int ldbl_size;    /* long double */
    int ldbl_align;
} LayoutTarget;

typedef struct {
    long long size;
    int align;
} TypeLayout;

/* A 64-bit target with 16-byte long double (x86_64 Linux, MinGW, aarch64
 * Linux): what aetherc assumes when no target is named. */
void layout_target_default(LayoutTarget* t);

/* 1 and the layout in *out when it is known, 0 when it is not. `program`
 * resolves struct names; it may be NULL, and then a struct is unknown. */
int type_layout_of(const Type* t, ASTNode* program, const LayoutTarget* tgt, TypeLayout* out);

/* An Aether struct definition (AST_STRUCT_DEFINITION) laid out as
 * generate_struct_definition emits it: fields in declaration order, `@packed`
 * with no padding. `offsets`, when not NULL, has room for one entry per
 * child of the definition and receives each field's offset (-1 for a child
 * that is not a field). 0 when any field's layout is unknown, or the struct
 * is opaque or imported from a C header. */
int struct_layout_of(ASTNode* struct_def, ASTNode* program, const LayoutTarget* tgt,
                     TypeLayout* out, long long* offsets);

#endif
