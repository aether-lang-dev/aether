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
    char want_s[64], have_s[64];
    const char* msg = "";
    snprintf(want_s, sizeof want_s, "%s", type_spelled(want));
    snprintf(have_s, sizeof have_s, "%s", type_spelled(have));
    msg = aether_internf(
             "cannot view %s elements as %s[]: a slice reads the array's own memory, "
             "so its element type must be exactly %s. Build a %s array from them "
             "instead (an array literal converts on its own)",
             have_s, want_s, want_s, want_s);
    type_error(msg, e->line, e->column);
    return 0;
}

int slice_empty_literal_to_null(ASTNode** slot) {
    if (!slot || !*slot) return 0;
    ASTNode* e = *slot;
    if (e->type != AST_ARRAY_LITERAL || e->child_count != 0) return 0;
    ASTNode* n = create_ast_node(AST_NULL_LITERAL, "null", e->line, e->column);
    n->source_file = e->source_file ? strdup(e->source_file) : NULL;
    n->node_type = create_type(TYPE_PTR);
    free_ast_node(e);
    *slot = n;
    return 1;
}

/* The slot holding the value a match arm gives (match_arm_value's): the
 * arm's body, or the expression ending its block. NULL when it gives none
 * (it ends in a statement), or when it ends in a match of its own, which
 * `nested` then receives. */
static ASTNode** arm_value_slot(ASTNode* arm, ASTNode** nested) {
    *nested = NULL;
    if (!arm || arm->type != AST_MATCH_ARM || arm->child_count < 2 || !arm->children[1])
        return NULL;
    ASTNode* body = arm->children[1];
    if (body->type != AST_BLOCK)
        return match_arm_value(body) == body ? &arm->children[1] : NULL;
    if (body->child_count == 0) return NULL;
    ASTNode* last = body->children[body->child_count - 1];
    if (!last) return NULL;
    if (last->type == AST_MATCH_STATEMENT) {
        *nested = last;
        return NULL;
    }
    /* The block's last expression, as match_arm_value reads it; `[]` has
     * no type of its own yet, so it is taken here by its shape. */
    if (last->type != AST_EXPRESSION_STATEMENT || last->child_count < 1 || !last->children[0])
        return NULL;
    ASTNode* v = last->children[0];
    if (v->type == AST_ARRAY_LITERAL || match_arm_value(body) == v) return &last->children[0];
    return NULL;
}

ASTNode** match_arm_value_slot(ASTNode* arm, ASTNode** nested) {
    return arm_value_slot(arm, nested);
}

/* #2789: a value that branches (an if-expression, a `match` whose value is
 * used) goes into the slot arm by arm, as each arm's value would on its own:
 * a `T[N]` arm is viewed, a `[]` or `null` arm is the empty slice. */
static int coerce_arms(ASTNode* e, const Type* target, int target_is_c_view) {
    int changed = 0;
    if (e->type == AST_IF_EXPRESSION) {
        for (int i = 1; i < e->child_count && i < 3; i++)
            changed |= slice_coerce_slot(&e->children[i], target, target_is_c_view);
    } else if (e->type == AST_MATCH_STATEMENT) {
        for (int i = 1; i < e->child_count; i++) {
            ASTNode* nested = NULL;
            ASTNode** vs = arm_value_slot(e->children[i], &nested);
            if (vs) changed |= slice_coerce_slot(vs, target, target_is_c_view);
            else if (nested) changed |= coerce_arms(nested, target, target_is_c_view);
        }
    }
    if (changed) {
        /* The value is now what its arms are: a slice, or in a raw-pointer
         * slot a pointer. */
        if (e->node_type) free_type(e->node_type);
        e->node_type = (type_is_slice(target) && !target_is_c_view)
            ? clone_type((Type*)target) : create_type(TYPE_PTR);
    }
    return changed;
}

/* #3062: `null` into a closure slot is the empty closure, whose function
 * pointer is NULL (what `c == null` tests): it takes the slot's type, which
 * codegen writes as that closure. An `if` or a `match` gives it arm by
 * arm. */
static int closure_null_slot(ASTNode** slot, const Type* target) {
    ASTNode* e = *slot;
    if (e->type == AST_NULL_LITERAL) {
        if (e->node_type) free_type(e->node_type);
        e->node_type = clone_type((Type*)target);
        return 1;
    }
    int changed = 0;
    if (e->type == AST_IF_EXPRESSION) {
        for (int i = 1; i < e->child_count && i < 3; i++)
            if (e->children[i]) changed |= closure_null_slot(&e->children[i], target);
    } else if (e->type == AST_MATCH_STATEMENT) {
        for (int i = 1; i < e->child_count; i++) {
            ASTNode* nested = NULL;
            ASTNode** vs = arm_value_slot(e->children[i], &nested);
            if (vs && *vs) changed |= closure_null_slot(vs, target);
            else if (nested) {
                ASTNode* n = nested;
                changed |= closure_null_slot(&n, target);
            }
        }
    }
    return changed;
}

static int slice_coerce_value(ASTNode** slot, const Type* target, int target_is_c_view);

/* Is `t` a closure type (not a C function pointer)? */
static int is_closure_type(const Type* t) {
    return t && t->kind == TYPE_FUNCTION && !t->is_fnptr;
}

/* #3077: does a slot of type `t` hold a closure: a closure, or a `fn?`? */
static int holds_closure(const Type* t) {
    if (t && t->kind == TYPE_OPTIONAL) t = t->element_type;
    return is_closure_type(t);
}

int slice_coerce_slot(ASTNode** slot, const Type* target, int target_is_c_view) {
    if (!slot || !*slot || !target) return 0;
    ASTNode* e = *slot;
    if (slice_is_coercion_node(e)) return 0;
    if (is_closure_type(target)) return closure_null_slot(slot, target);
    /* #3077: and in every slot that holds a closure. An optional's value
     * (`let o: fn? = null`, `-> fn?`, a `fn?` field, parameter or store) is
     * the empty closure, as a `ptr?` given `null` holds a null pointer; the
     * elements of an array literal in an array of closures, and each part
     * of a tuple literal, go in by the rule of their own slot. */
    if (target->kind == TYPE_OPTIONAL && is_closure_type(target->element_type))
        return closure_null_slot(slot, target->element_type);
    if (e->type == AST_TUPLE_LITERAL && target->kind == TYPE_TUPLE && target->tuple_types &&
        target->tuple_count == e->child_count) {
        int changed = 0;
        for (int i = 0; i < e->child_count; i++)
            changed |= slice_coerce_slot(&e->children[i], target->tuple_types[i],
                                         target_is_c_view);
        return changed;
    }
    int elements = 0;
    if (e->type == AST_ARRAY_LITERAL && target->kind == TYPE_ARRAY &&
        holds_closure(target->element_type)) {
        for (int i = 0; i < e->child_count; i++)
            elements |= slice_coerce_slot(&e->children[i], target->element_type, 0);
    }
    return elements | slice_coerce_value(slot, target, target_is_c_view);
}

static int slice_coerce_value(ASTNode** slot, const Type* target, int target_is_c_view) {
    ASTNode* e = *slot;
    int slot_is_slice = type_is_slice(target) && !target_is_c_view;
    int slot_is_raw_ptr = target->kind == TYPE_PTR ||
                          (target->kind == TYPE_ARRAY && target_is_c_view);

    if ((slot_is_slice || slot_is_raw_ptr) &&
        (e->type == AST_IF_EXPRESSION || e->type == AST_MATCH_STATEMENT))
        return coerce_arms(e, target, target_is_c_view);

    if (slot_is_slice) {
        /* `null`, and the empty literal `[]` (no elements, so no array to
         * view: its type is no `T[N]`), are the empty slice (#2767). */
        if (e->type == AST_NULL_LITERAL ||
            (e->type == AST_ARRAY_LITERAL && e->child_count == 0)) {
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
    /* In a raw-pointer slot (a C function's `T[]`, a `ptr`) the empty
     * literal is the empty slice's pointer, `null` (#2789). */
    if (slot_is_raw_ptr && slice_empty_literal_to_null(slot)) return 1;
    if (slot_is_raw_ptr) return slice_coerce_to_ptr(slot);
    return 0;
}
