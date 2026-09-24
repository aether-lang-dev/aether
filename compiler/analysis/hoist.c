/* Shared hoisting rules -- see hoist.h (#2186). The logic here is what
 * codegen's hoist_if_branch_vars, hoist_if_else_common_vars and
 * hoist_loop_vars used to compute inline; they now call it, and so does the
 * typechecker. */

#include "hoist.h"

#include <string.h>

int hoist_direct_decl_names(ASTNode* block, const char** out, int count, int cap) {
    if (!block || !out) return count;
    for (int i = 0; i < block->child_count; i++) {
        ASTNode* child = block->children[i];
        if (!child) continue;
        if (child->type == AST_VARIABLE_DECLARATION && child->value && count < cap) {
            int already = 0;
            for (int j = 0; j < count; j++) {
                if (strcmp(out[j], child->value) == 0) { already = 1; break; }
            }
            if (!already) out[count++] = child->value;
        }
    }
    return count;
}

ASTNode* hoist_find_decl(ASTNode* block, const char* name) {
    if (!block || !name) return NULL;
    for (int i = 0; i < block->child_count; i++) {
        ASTNode* child = block->children[i];
        if (!child) continue;
        if (child->type == AST_VARIABLE_DECLARATION && child->value
            && strcmp(child->value, name) == 0) {
            return child;
        }
    }
    return NULL;
}

int hoist_has_identifier_ref(ASTNode* node, const char* name) {
    if (!node || !name) return 0;
    if (node->type == AST_IDENTIFIER && node->value &&
        strcmp(node->value, name) == 0) return 1;
    /* Any identifier counts as a use; declaration order within a subtree is
     * not tracked. Over-eager, and safe: an extra hoisted declaration is a
     * zero-initialised local. */
    for (int i = 0; i < node->child_count; i++) {
        if (hoist_has_identifier_ref(node->children[i], name)) return 1;
    }
    return 0;
}

Type* hoist_decl_type(ASTNode* decl) {
    if (!decl) return NULL;
    Type* t = decl->node_type;
    if ((!t || t->kind == TYPE_VOID || t->kind == TYPE_UNKNOWN)
        && decl->child_count > 0 && decl->children[0] && decl->children[0]->node_type) {
        t = decl->children[0]->node_type;
    }
    return t;
}

static Type* join_walk(ASTNode* n, const char* name, Type* acc) {
    if (!n) return acc;
    if (n->type == AST_CLOSURE) return acc;   /* its own function */
    if (n->type == AST_VARIABLE_DECLARATION && n->value && n->type_inferred &&
        strcmp(n->value, name) == 0) {
        Type* t = n->node_type;
        if ((!t || t->kind == TYPE_UNKNOWN) && n->child_count > 0 && n->children[0])
            t = n->children[0]->node_type;
        Type* j = numeric_join_type(acc, t);
        if (j) { free_type(acc); acc = j; }
    }
    for (int i = 0; i < n->child_count; i++) acc = join_walk(n->children[i], name, acc);
    return acc;
}

Type* hoist_join_type(ASTNode* scope_body, const char* name, Type* seed) {
    if (!scope_body || !seed) return NULL;
    Type* probe = numeric_join_type(seed, seed);
    if (!probe) return NULL;          /* not a numeric kind: nothing to join */
    free_type(probe);
    Type* acc = join_walk(scope_body, name, clone_type(seed));
    if (acc && acc->kind == seed->kind) { free_type(acc); return NULL; }
    return acc;
}

int hoist_if_branch_candidates(ASTNode* body, const char** out, int cap) {
    if (!body || !out) return 0;
    /* Names a top-level statement declares get their own function-scope
     * declaration (and heap tracker) where they are; hoisting them from an
     * arm as well would declare them twice. */
    const char* top_level_decls[HOIST_MAX_NAMES];
    int top_count = 0;
    for (int i = 0; i < body->child_count && top_count < HOIST_MAX_NAMES; i++) {
        ASTNode* child = body->children[i];
        if (!child) continue;
        if (child->type == AST_VARIABLE_DECLARATION && child->value) {
            top_level_decls[top_count++] = child->value;
        }
    }

    const char* names[HOIST_MAX_NAMES];
    int count = 0;
    for (int i = 0; i < body->child_count; i++) {
        ASTNode* child = body->children[i];
        if (!child || child->type != AST_IF_STATEMENT) continue;
        for (int j = 1; j < child->child_count && j < 3; j++) {
            count = hoist_direct_decl_names(child->children[j], names, count, HOIST_MAX_NAMES);
        }
    }

    int kept = 0;
    for (int n = 0; n < count && kept < cap; n++) {
        const char* name = names[n];
        int dup = 0;
        for (int k = 0; k < top_count; k++) {
            if (strcmp(name, top_level_decls[k]) == 0) { dup = 1; break; }
        }
        if (dup) continue;
        /* Only a name read outside the ifs' arms -- by a top-level statement
         * that is not an if, or by an if's condition -- is hoisted. */
        int referenced_outside = 0;
        for (int i = 0; i < body->child_count && !referenced_outside; i++) {
            ASTNode* child = body->children[i];
            if (!child) continue;
            if (child->type == AST_IF_STATEMENT) {
                if (child->child_count > 0 &&
                    hoist_has_identifier_ref(child->children[0], name)) {
                    referenced_outside = 1;
                }
                continue;
            }
            if (hoist_has_identifier_ref(child, name)) referenced_outside = 1;
        }
        if (!referenced_outside) continue;
        if (!hoist_if_branch_first_decl(body, name)) continue;
        out[kept++] = name;
    }
    return kept;
}

ASTNode* hoist_if_branch_first_decl(ASTNode* body, const char* name) {
    if (!body) return NULL;
    for (int i = 0; i < body->child_count; i++) {
        ASTNode* child = body->children[i];
        if (!child || child->type != AST_IF_STATEMENT) continue;
        for (int j = 1; j < child->child_count && j < 3; j++) {
            ASTNode* d = hoist_find_decl(child->children[j], name);
            if (d) return d;
        }
    }
    return NULL;
}

void hoist_loop_decls(ASTNode* body, HoistDeclVisitor visit, void* user) {
    if (!body || !visit) return;
    for (int i = 0; i < body->child_count; i++) {
        ASTNode* child = body->children[i];
        if (!child) continue;
        if (child->type == AST_VARIABLE_DECLARATION && child->value) visit(child, user);
        if (child->type == AST_IF_STATEMENT || child->type == AST_WHILE_LOOP ||
            child->type == AST_FOR_LOOP) {
            for (int j = 0; j < child->child_count; j++) {
                hoist_loop_decls(child->children[j], visit, user);
            }
        }
    }
}
