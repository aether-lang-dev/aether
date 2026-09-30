/* #1286 first-class slices: representation coercions. See slice_coerce.h. */
#include "slice_coerce.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int slice_is_coercion_node(const ASTNode* n) {
    return n && (n->type == AST_SLICE_FROM_ARRAY || n->type == AST_SLICE_TO_PTR);
}

static ASTNode* wrap(ASTNode* inner, ASTNodeType kind, const char* value, Type* result) {
    ASTNode* w = create_ast_node(kind, value, inner->line, inner->column);
    w->source_file = inner->source_file ? strdup(inner->source_file) : NULL;
    add_child(w, inner);
    w->node_type = result;
    return w;
}

int slice_coerce_to_ptr(ASTNode** slot) {
    if (!slot || !*slot) return 0;
    ASTNode* e = *slot;
    if (slice_is_coercion_node(e)) return 0;
    if (!type_is_slice(e->node_type)) return 0;
    Type* ptr = create_type(TYPE_PTR);
    *slot = wrap(e, AST_SLICE_TO_PTR, NULL, ptr);
    return 1;
}

int slice_coerce_slot(ASTNode** slot, const Type* target, int target_is_c_view) {
    if (!slot || !*slot || !target) return 0;
    ASTNode* e = *slot;
    if (slice_is_coercion_node(e)) return 0;

    int slot_is_slice = type_is_slice(target) && !target_is_c_view;
    int slot_is_raw_ptr = target->kind == TYPE_PTR ||
                          (target->kind == TYPE_ARRAY && target_is_c_view);

    if (slot_is_slice) {
        if (e->type == AST_NULL_LITERAL) {
            *slot = wrap(e, AST_SLICE_FROM_ARRAY, "0", clone_type((Type*)target));
            return 1;
        }
        if (type_is_sized_array(e->node_type) && e->node_type->array_size > 0) {
            char len[32];
            snprintf(len, sizeof len, "%d", e->node_type->array_size);
            /* The slice keeps the ARRAY's element type: `T[N]` into a `T[]`
             * slot never changes what an element is. */
            Type* st = create_array_type(clone_type(e->node_type->element_type), -1);
            *slot = wrap(e, AST_SLICE_FROM_ARRAY, len, st);
            return 1;
        }
        return 0;
    }
    if (slot_is_raw_ptr) return slice_coerce_to_ptr(slot);
    return 0;
}
