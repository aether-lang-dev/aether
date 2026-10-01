/* #1286 first-class slices: representation coercions. See slice_coerce.h. */
#include "slice_coerce.h"
#include "typechecker.h"

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

/* The element type a slice slot sees, or NULL when it is not yet known. */
static Type* known_element(const Type* t) {
    if (!t || t->kind != TYPE_ARRAY || !t->element_type) return NULL;
    return t->element_type->kind == TYPE_UNKNOWN ? NULL : t->element_type;
}

/* #2330: a slice is a view of memory, so the array or slice behind it must
 * hold elements of the slot's type. Element compatibility (int into long,
 * int into byte) is right for a value and wrong for a view: an int[3] passed
 * as a long[] was read as three 8-byte longs out of 12 bytes. An array
 * LITERAL has no memory yet, so it takes the slot's element type instead;
 * anything else with a different element type is an error. Returns 0 after
 * reporting one. */
static int slice_elements_agree(ASTNode* e, const Type* target) {
    Type* want = known_element(target);
    Type* have = known_element(e->node_type);
    if (!want || !have || types_equal(have, want)) return 1;
    if (e->type == AST_ARRAY_LITERAL && is_type_compatible(have, want)) {
        free_type(e->node_type->element_type);
        e->node_type->element_type = clone_type(want);
        return 1;
    }
    char want_s[64], have_s[64], msg[512];
    snprintf(want_s, sizeof want_s, "%s", type_to_string(want));
    snprintf(have_s, sizeof have_s, "%s", type_to_string(have));
    snprintf(msg, sizeof msg,
             "cannot view %s elements as %s[]: a slice reads the array's own memory, "
             "so its element type must be exactly %s. Build a %s array from them "
             "instead (an array literal converts on its own)",
             have_s, want_s, want_s, want_s);
    type_error(msg, e->line, e->column);
    return 0;
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
        if ((type_is_sized_array(e->node_type) || type_is_slice(e->node_type)) &&
            !slice_elements_agree(e, target)) {
            return 0;
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
