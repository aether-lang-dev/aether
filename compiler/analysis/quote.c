/* `quote`: a trailing block handed over as a tree (docs/quote.md).
 *
 * The type checker has checked the body as a closure's. Here it is
 *   1. checked against version 1's subset (each construct the tree has no
 *      node for is an error naming it, at its line);
 *   2. lowered to the tree it stands for, every node carrying the type the
 *      checker gave it, each outer variable it reads recorded as a capture;
 *   3. hashed, so a back end can cache what it makes from it.
 * Codegen then emits one builder per quote (quote_emit_builder) that makes
 * the same tree at run time through std.quote's runtime. */

#include "quote.h"
#include "typechecker.h"
#include "../aether_module.h"

#include <stdlib.h>
#include <string.h>

static QuoteInfo** g_quotes = NULL;
static QHelper** g_helpers = NULL;
static int g_helper_count = 0;
static int g_helper_cap = 0;
static int g_quote_count = 0;
static int g_quote_cap = 0;

int quote_count(void) { return g_quote_count; }
QuoteInfo* quote_at(int index) {
    return (index >= 0 && index < g_quote_count) ? g_quotes[index] : NULL;
}

QuoteInfo* quote_of(ASTNode* node) {
    if (!node || node->type != AST_QUOTE || !node->value) return NULL;
    QuoteInfo* q = quote_at(atoi(node->value));
    return (q && q->node == node) ? q : NULL;
}

static void free_qnode(QNode* n) {
    if (!n) return;
    for (int i = 0; i < n->nkids; i++) free_qnode(n->kids[i]);
    free(n->kids);
    free(n);
}

void quote_reset(void) {
    for (int i = 0; i < g_quote_count; i++) {
        QuoteInfo* q = g_quotes[i];
        free_qnode(q->body);
        free(q->param_names);
        free(q->param_types);
        free(q->caps);
        free(q->helpers);
        free(q);
    }
    free(g_quotes);
    g_quotes = NULL;
    g_quote_count = g_quote_cap = 0;
    for (int i = 0; i < g_helper_count; i++) {
        QHelper* h = g_helpers[i];
        free_qnode(h->body);
        free(h->param_names);
        free(h->param_types);
        free(h->callees);
        free(h);
    }
    free(g_helpers);
    g_helpers = NULL;
    g_helper_count = g_helper_cap = 0;
}

/* ---- types ---- */

const char* quote_type_name(Type* t) {
    if (!t) return "";
    if (t->distinct_name) return t->distinct_name;
    switch (t->kind) {
        case TYPE_F32X4: return "f32x4";
        case TYPE_F64X2: return "f64x2";
        case TYPE_I32X4: return "i32x4";
        case TYPE_I64X2: return "i64x2";
        case TYPE_I16X8: return "i16x8";
        case TYPE_F32X8: return "f32x8";
        case TYPE_I32X8: return "i32x8";
        /* The struct as written: a module's struct is emitted as
         * `<ns>_<Name>`, and a tree spells it `Name`, so its hash and what
         * a back end writes from it do not depend on the C name. */
        case TYPE_STRUCT: return t->struct_name ? ast_struct_source_name(t->struct_name) : "struct";
        /* type_to_string has no name for these (it says UNKNOWN); the
         * tree spells them as Aether does. A C ABI alias (`uint32_t`) is
         * its kind's name too. */
        case TYPE_UINT32: return "uint32";
        case TYPE_UINT16: return "uint16";
        case TYPE_UINT8: return "uint8";
        /* An array spells its element as the tree does: type_to_string has
         * no name for a distinct type (`vec4 = distinct f32x4`), so a
         * struct's `vec4[4]` field came out as UNKNOWN[4]. */
        case TYPE_ARRAY:
            if (!t->index_enum_name && t->element_type)
                return aether_internf("%s[%d]", quote_type_name(t->element_type), t->array_size);
            return type_to_string(t);
        case TYPE_VOID: return "";
        case TYPE_UNKNOWN: return "";
        default: return type_to_string(t);
    }
}

/* The type a typed getter reads the cell as: the distinct type's base. */
static const char* quote_base_name(Type* t) {
    if (!t) return "";
    if (!t->distinct_name) return quote_type_name(t);
    char* saved = t->distinct_name;
    t->distinct_name = NULL;
    const char* base = quote_type_name(t);
    t->distinct_name = saved;
    return base;
}

int quote_is_tree_type(Type* t) {
    return t && t->kind == TYPE_PTR && t->distinct_name &&
           strcmp(t->distinct_name, "Tree") == 0;
}

/* ---- lowering ---- */

typedef struct {
    ASTNode* quote;
    SymbolTable* outer;
    QuoteErrorFn report;
    int errors;
    /* The quote's own names (parameters, then locals in scope), a stack
     * that each block pops back to on exit. */
    const char** locals;
    int nlocals;
    int caplocals;
    QCapture* caps;
    int ncaps;
    int capcaps;
    /* Lowering a @quotable function rather than a quote: its definition. */
    QHelper* helper;
    /* The @quotable functions the body calls. */
    QHelper** called;
    int ncalled;
    int capcalled;
} Lower;

/* ---- @quotable functions (version 2) ---- */


/* A helper's name in a tree: the same in every program. A merged module's
 * function is `<ns>_<fn>`, and its namespace `ns` is the module's last
 * segment or, when another loaded module ends the same way, its full path
 * (#2209), so the C name depends on the rest of the program. The tree names
 * it by the full path always: `ae3d.shaders.sky`'s `oct_decode` is
 * `ae3d_shaders_sky_oct_decode` whether or not `ae3d.sky` is loaded too. */
static const char* helper_tree_name(ASTNode* def) {
    if (!def->origin_module) return aether_intern(def->value);
    const char* ns = module_namespace_of(def->origin_module);
    size_t n = strlen(ns);
    const char* fn = def->value;
    if (strncmp(fn, ns, n) == 0 && fn[n] == '_' && fn[n + 1]) fn += n + 1;
    /* As long as it is (#3574): 512 bytes cut it, and two helpers took one
     * tree name. */
    char* buf = strdup(aether_internf("%s_%s", def->origin_module, fn));
    if (!buf) { fprintf(stderr, "aetherc: out of memory registering a @quotable function\n"); exit(1); }
    for (char* c = buf; *c; c++) if (*c == '.') *c = '_';
    const char* r = aether_intern(buf);
    free(buf);
    return r;
}

void quote_register_helper(ASTNode* def) {
    if (!def || !def->value) return;
    for (int i = 0; i < g_helper_count; i++)
        if (g_helpers[i]->def == def) return;
    QHelper* h = (QHelper*)calloc(1, sizeof(QHelper));
    if (!h) { fprintf(stderr, "aetherc: out of memory registering a @quotable function\n"); exit(1); }
    h->def = def;
    h->c_name = aether_intern(def->value);
    h->name = helper_tree_name(def);
    if (g_helper_count == g_helper_cap) {
        g_helper_cap = g_helper_cap ? g_helper_cap * 2 : 8;
        g_helpers = (QHelper**)aether_xrealloc(g_helpers, (size_t)g_helper_cap * sizeof(QHelper*));
    }
    g_helpers[g_helper_count++] = h;
}

int quote_helper_count(void) { return g_helper_count; }
QHelper* quote_helper_at(int index) {
    return (index >= 0 && index < g_helper_count) ? g_helpers[index] : NULL;
}

/* The @quotable function a call names, or NULL. A call in a merged module
 * names it as the module's code does (`fxaa_luma`), a qualified one as
 * written (`shaderlib.luma`, which is `shaderlib_luma` merged). */
static QHelper* helper_named(const char* name) {
    if (!name || !*name) return NULL;
    /* As long as the name (#3574): 256 bytes cut it, and the helper was not
     * found. */
    char* flat = strdup(name);
    if (!flat) { fprintf(stderr, "aetherc: out of memory lowering a quote\n"); exit(1); }
    for (char* c = flat; *c; c++) if (*c == '.') *c = '_';
    QHelper* found = NULL;
    for (int i = 0; i < g_helper_count && !found; i++) {
        if (strcmp(g_helpers[i]->c_name, name) == 0 || strcmp(g_helpers[i]->c_name, flat) == 0)
            found = g_helpers[i];
    }
    free(flat);
    return found;
}

static void note_call(Lower* L, QHelper* h) {
    for (int i = 0; i < L->ncalled; i++) if (L->called[i] == h) return;
    if (L->ncalled == L->capcalled) {
        L->capcalled = L->capcalled ? L->capcalled * 2 : 4;
        L->called = (QHelper**)aether_xrealloc(L->called, (size_t)L->capcalled * sizeof(QHelper*));
    }
    L->called[L->ncalled++] = h;
}

static QNode* qnode(int kind, const char* type, const char* name, const char* op,
                    const char* lit, int line) {
    QNode* n = (QNode*)calloc(1, sizeof(QNode));
    if (!n) { fprintf(stderr, "aetherc: out of memory lowering a quote\n"); exit(1); }
    n->kind = kind;
    n->type = aether_intern(type ? type : "");
    n->name = aether_intern(name ? name : "");
    n->op = aether_intern(op ? op : "");
    n->lit = aether_intern(lit ? lit : "");
    n->line = line;
    return n;
}

static QNode* qadd(QNode* parent, QNode* kid) {
    if (!parent) { free_qnode(kid); return NULL; }
    if (!kid) return parent;
    if (parent->nkids == parent->capkids) {
        parent->capkids = parent->capkids ? parent->capkids * 2 : 4;
        parent->kids = (QNode**)aether_xrealloc(parent->kids, (size_t)parent->capkids * sizeof(QNode*));
    }
    parent->kids[parent->nkids++] = kid;
    return parent;
}

/* The first line a node or its children carry (some statements, `match`
 * among them, are made without one). */
static ASTNode* with_line(ASTNode* n) {
    if (!n || n->line > 0) return n;
    for (int i = 0; i < n->child_count; i++) {
        ASTNode* k = with_line(n->children[i]);
        if (k && k->line > 0) return k;
    }
    return NULL;
}

static void lerror(Lower* L, ASTNode* at, AetherErrorCode code, const char* msg, const char* hint) {
    L->errors++;
    if (at && at->line <= 0) at = with_line(at);
    if (L->report) L->report(msg, hint, at ? at->line : L->quote->line,
                             at ? at->column : L->quote->column, code);
}

/* A construct the tree has no node for. */
static QNode* reject(Lower* L, ASTNode* at, const char* what, const char* why) {
    char msg[320], hint[320];
    if (L->helper) {
        snprintf(msg, sizeof msg, "%s can't be in a @quotable function", what);
        snprintf(hint, sizeof hint,
                 "its body becomes a tree, as a quote's does; %s", why);
    } else {
        snprintf(msg, sizeof msg, "%s can't be quoted", what);
        snprintf(hint, sizeof hint,
                 "a quote becomes a tree for a library to read; %s", why);
    }
    lerror(L, at, AETHER_ERR_QUOTE_SUBSET, msg, hint);
    return NULL;
}

static int is_local(Lower* L, const char* name) {
    for (int i = L->nlocals - 1; i >= 0; i--)
        if (strcmp(L->locals[i], name) == 0) return 1;
    return 0;
}

static void push_local(Lower* L, const char* name) {
    if (L->nlocals == L->caplocals) {
        L->caplocals = L->caplocals ? L->caplocals * 2 : 16;
        L->locals = (const char**)aether_xrealloc(L->locals, (size_t)L->caplocals * sizeof(char*));
    }
    L->locals[L->nlocals++] = name;
}

/* Where the quote's scope resolves `name`, when it is not one of the
 * quote's own: 1 for a local of an enclosing function (a capture), 2 for a
 * program-level name, 0 for nothing. */
static Symbol* outer_symbol(Lower* L, const char* name, int* is_local_scope) {
    for (SymbolTable* t = L->outer; t; t = t->parent) {
        Symbol* s = lookup_symbol_local(t, name);
        /* A bare write to a program-level `var` binds the name in the
         * function's scope, but the variable is still the global. */
        if (s && t->parent && s->global_write) continue;
        if (s) {
            *is_local_scope = t->parent != NULL;
            return s;
        }
    }
    *is_local_scope = 0;
    return NULL;
}

static void add_capture(Lower* L, const char* name, Type* t, int global) {
    for (int i = 0; i < L->ncaps; i++)
        if (strcmp(L->caps[i].name, name) == 0) return;
    if (L->ncaps == L->capcaps) {
        L->capcaps = L->capcaps ? L->capcaps * 2 : 4;
        L->caps = (QCapture*)aether_xrealloc(L->caps, (size_t)L->capcaps * sizeof(QCapture));
    }
    QCapture* c = &L->caps[L->ncaps++];
    c->name = aether_intern(name);
    c->type = aether_intern(quote_type_name(t));
    c->base = aether_intern(quote_base_name(t));
    c->var_type = t ? clone_type(t) : NULL;
    c->global = global;
}

/* Is `name`, not one of the quote's own, a variable outside it: a local of
 * an enclosing function or a program-level `var`? (What a quote may read
 * as a capture and may not write.) */
static int is_outer_variable(Lower* L, const char* name) {
    if (!name || is_local(L, name)) return 0;
    int local_scope = 0;
    Symbol* s = outer_symbol(L, name, &local_scope);
    if (!s || s->is_function || s->is_actor || s->is_module_alias) return 0;
    return local_scope || tc_program_global_var(name) != NULL;
}

/* The outer variable a write or `&` reaches through, `p` in `p.x.y`. */
static ASTNode* lvalue_root(ASTNode* e) {
    while (e && (e->type == AST_MEMBER_ACCESS || e->type == AST_ARRAY_ACCESS) && e->child_count > 0)
        e = e->children[0];
    return e;
}

static int names_capture(Lower* L, ASTNode* target) {
    ASTNode* root = lvalue_root(target);
    if (!root || root->type != AST_IDENTIFIER || !root->value) return 0;
    return is_outer_variable(L, root->value);
}

static void capture_write_error(Lower* L, ASTNode* at, const char* name, const char* how) {
    char msg[320];
    snprintf(msg, sizeof msg, "a quote can't %s '%s', a variable it captures", how, name);
    lerror(L, at, AETHER_ERR_QUOTE_CAPTURE, msg,
           "the captured variable's cell is shared with the program; a quote reads it. "
           "Copy it into a local of the quote and change that");
}

static QNode* lower_expr(Lower* L, ASTNode* e);
static QNode* lower_stmt(Lower* L, ASTNode* s);

static const char* node_type_name(ASTNode* e) {
    return quote_type_name(e ? e->node_type : NULL);
}

/* A @quotable function reads only its parameters and locals: it is emitted
 * as a function of its own (GLSL, MSL), which has no way to reach a variable
 * of the program, and the uniforms stay the quote's own captures. The one
 * place that rule lives; drop the call in lower_identifier to lift it. */
static QNode* helper_reads_outer(Lower* L, ASTNode* e, const char* name) {
    char msg[320];
    snprintf(msg, sizeof msg, "@quotable function '%s' reads '%s', a variable outside it",
             L->helper->def->value, name);
    lerror(L, e, AETHER_ERR_QUOTE_HELPER_READ, msg,
           "a @quotable function is emitted as a function of its own; pass the value "
           "in as a parameter, and let the quote that calls it capture the variable");
    return NULL;
}

static QNode* lower_identifier(Lower* L, ASTNode* e) {
    const char* name = e->value;
    if (is_local(L, name)) return qnode(QK_LOCAL, node_type_name(e), name, NULL, NULL, e->line);
    int local_scope = 0;
    Symbol* s = outer_symbol(L, name, &local_scope);
    if (L->helper && s && !s->is_function && !s->is_actor && !s->is_module_alias &&
        (local_scope || tc_program_global_var(name)))
        return helper_reads_outer(L, e, name);
    if (s && local_scope && !s->is_function && !s->is_actor && !s->is_module_alias) {
        if (s->type && s->type->kind == TYPE_FUNCTION) {
            return reject(L, e, "a closure value",
                          "it has no node for a closure; call a named function instead");
        }
        add_capture(L, name, s->type ? s->type : e->node_type, 0);
        return qnode(QK_CAPTURE, quote_type_name(s->type ? s->type : e->node_type), name,
                     NULL, NULL, e->line);
    }
    /* A program-level `var` is a capture too, whose cell is the global
     * itself: it lives as long as the program, so nothing promotes it and
     * the tree holds no reference to it. */
    ASTNode* gdecl = (s && !local_scope && !s->is_function) ? tc_program_global_var(name) : NULL;
    if (gdecl) {
        Type* t = s->type ? s->type : e->node_type;
        if (t && t->kind == TYPE_FUNCTION) {
            return reject(L, e, "a closure value",
                          "it has no node for a closure; call a named function instead");
        }
        add_capture(L, name, t, 1);
        return qnode(QK_CAPTURE, quote_type_name(t), name, NULL, NULL, e->line);
    }
    /* A program-level constant whose value is a literal reads as that
     * literal: the tree has no node for a global, and a constant does not
     * change, so nothing is lost. */
    ASTNode* cdecl = (s && !local_scope) ? tc_program_const(name) : NULL;
    if (cdecl && cdecl->child_count > 0 && cdecl->children[0] &&
        cdecl->children[0]->type == AST_LITERAL) {
        ASTNode* lit = cdecl->children[0];
        Type* t = s->type ? s->type : lit->node_type;
        return qnode(QK_LIT, quote_type_name(t), NULL, NULL, lit->value, e->line);
    }
    char what[200];
    snprintf(what, sizeof what, "'%s' (not a local, a parameter, a captured variable or a literal constant)",
             name);
    return reject(L, e, what,
                  "it can read its own locals and parameters, the enclosing function's "
                  "variables and the program's (as captures), and constants with a literal value");
}

static int is_actor_call(const char* name) {
    return name && (strcmp(name, "spawn") == 0 || strcmp(name, "send") == 0 ||
                    strcmp(name, "reply") == 0 || strcmp(name, "receive") == 0);
}

static QNode* lower_expr(Lower* L, ASTNode* e) {
    if (!e) return NULL;
    switch (e->type) {
        case AST_LITERAL:
            return qnode(QK_LIT, node_type_name(e), NULL, NULL, e->value, e->line);
        case AST_NULL_LITERAL:
            return qnode(QK_LIT, "ptr", NULL, NULL, "null", e->line);
        case AST_IDENTIFIER:
            if (!e->value) break;
            return lower_identifier(L, e);
        case AST_UNARY_EXPRESSION: {
            const char* op = e->value ? e->value : "";
            if (e->child_count < 1) break;
            if (strcmp(op, "&") == 0 && names_capture(L, e->children[0])) {
                capture_write_error(L, e, lvalue_root(e->children[0])->value, "take the address of");
                return NULL;
            }
            if (strcmp(op, "-") != 0 && strcmp(op, "!") != 0 && strcmp(op, "~") != 0) {
                char what[64];
                snprintf(what, sizeof what, "the operator '%s' here", op);
                return reject(L, e, what, "it has unary '-', '!' and '~'; write a step as `x += 1`");
            }
            QNode* n = qnode(QK_UNARY, node_type_name(e), NULL, op, NULL, e->line);
            QNode* x = lower_expr(L, e->children[0]);
            if (!x) { free_qnode(n); return NULL; }
            return qadd(n, x);
        }
        case AST_BINARY_EXPRESSION: {
            if (e->child_count < 2) break;
            const char* op = e->value ? e->value : "";
            if (strcmp(op, "=") == 0) {
                return reject(L, e, "an assignment used as a value",
                              "an assignment is a statement in the tree");
            }
            QNode* n = qnode(QK_BINARY, node_type_name(e), NULL, op, NULL, e->line);
            QNode* l = lower_expr(L, e->children[0]);
            QNode* r = lower_expr(L, e->children[1]);
            if (!l || !r) { free_qnode(l); free_qnode(r); free_qnode(n); return NULL; }
            return qadd(qadd(n, l), r);
        }
        case AST_IF_EXPRESSION: {
            if (e->child_count < 3) break;
            QNode* n = qnode(QK_CHOOSE, node_type_name(e), NULL, NULL, NULL, e->line);
            int ok = 1;
            for (int i = 0; i < 3; i++) {
                QNode* k = lower_expr(L, e->children[i]);
                if (!k) ok = 0;
                n = qadd(n, k);
            }
            if (!ok) { free_qnode(n); return NULL; }
            return n;
        }
        case AST_FUNCTION_CALL: {
            const char* written = e->source_name ? e->source_name : e->value;
            if (!written) break;
            if (strcmp(written, "panic") == 0)
                return reject(L, e, "'panic'", "it has no node for unwinding");
            if (is_actor_call(written)) {
                char what[64];
                snprintf(what, sizeof what, "'%s'", written);
                return reject(L, e, what, "an actor operation has no meaning in a tree");
            }
            if (e->node_type && e->node_type->kind == TYPE_ACTOR_REF)
                return reject(L, e, "'spawn'", "an actor operation has no meaning in a tree");
            if (strcmp(written, "call") == 0)
                return reject(L, e, "a closure call", "it has no node for a closure");
            /* A @quotable function: a HELPER_CALL naming it as its tree
             * names it, which a back end emits as a call of the function it
             * writes out from the helper's tree. */
            QHelper* h = helper_named(e->value);
            if (h) note_call(L, h);
            QNode* n = h ? qnode(QK_HELPER_CALL, node_type_name(e), h->name, NULL, NULL, e->line)
                         : qnode(QK_CALL, node_type_name(e), written, NULL, NULL, e->line);
            int ok = 1;
            for (int i = 0; i < e->child_count; i++) {
                ASTNode* a = e->children[i];
                if (a && a->type == AST_NAMED_ARG && a->child_count > 0) a = a->children[0];
                QNode* k = lower_expr(L, a);
                if (!k) ok = 0;
                n = qadd(n, k);
            }
            if (!ok) { free_qnode(n); return NULL; }
            return n;
        }
        case AST_MEMBER_ACCESS: {
            if (e->child_count < 1 || !e->value) break;
            QNode* n = qnode(QK_FIELD, node_type_name(e), e->value, NULL, NULL, e->line);
            QNode* o = lower_expr(L, e->children[0]);
            if (!o) { free_qnode(n); return NULL; }
            return qadd(n, o);
        }
        case AST_ARRAY_ACCESS: {
            if (e->child_count < 2) break;
            QNode* n = qnode(QK_INDEX, node_type_name(e), NULL, NULL, NULL, e->line);
            QNode* o = lower_expr(L, e->children[0]);
            QNode* i = lower_expr(L, e->children[1]);
            if (!o || !i) { free_qnode(o); free_qnode(i); free_qnode(n); return NULL; }
            return qadd(qadd(n, o), i);
        }
        case AST_STRUCT_LITERAL: {
            /* MAKE: `type` the struct; each child a FIELD_VALUE whose
             * `name` is the field it sets and whose one child the value. */
            Type* st = e->node_type;
            const char* tname = (st && st->kind == TYPE_STRUCT && st->struct_name)
                                    ? st->struct_name : (e->value ? e->value : "");
            tname = ast_struct_source_name(tname);
            QNode* n = qnode(QK_MAKE, tname, NULL, NULL, NULL, e->line);
            int ok = 1;
            for (int i = 0; i < e->child_count; i++) {
                ASTNode* f = e->children[i];
                if (!f || f->child_count < 1 || !f->value) { ok = 0; continue; }
                QNode* v = lower_expr(L, f->children[0]);
                if (!v) { ok = 0; continue; }
                QNode* fv = qnode(QK_FIELD_VALUE, v->type, f->value, NULL, NULL,
                                  f->line > 0 ? f->line : v->line);
                n = qadd(n, qadd(fv, v));
            }
            if (!ok) { free_qnode(n); return NULL; }
            return n;
        }
        case AST_STRING_INTERP:
            return reject(L, e, "string interpolation",
                          "version 1 has string literals only");
        case AST_ARRAY_LITERAL:
            return reject(L, e, "an array literal", "version 1 has no node for one");
        case AST_HEAP_NEW:
            return reject(L, e, "'new'", "version 1 has no node for an allocation");
        case AST_CLOSURE: {
            int trailing = e->value && strcmp(e->value, "trailing") == 0;
            return reject(L, e, trailing ? "a trailing block" : "a closure or callback",
                          "a tree of trees has no back end to go to");
        }
        case AST_QUOTE:
            return reject(L, e, "a quote inside a quote", "a tree of trees has no back end to go to");
        case AST_MATCH_STATEMENT:
            return reject(L, e, "'match'", "version 1 has no node for patterns");
        case AST_SEND_FIRE_FORGET:
        case AST_SEND_ASK:
        case AST_SEND_STATEMENT:
            return reject(L, e, "an actor send", "an actor operation has no meaning in a tree");
        case AST_SPAWN_ACTOR_STATEMENT:
            return reject(L, e, "'spawn'", "an actor operation has no meaning in a tree");
        default:
            break;
    }
    char what[96];
    snprintf(what, sizeof what, "this expression (%s)", ast_node_type_to_string(e->type));
    return reject(L, e, what, "version 1's tree has no node for it");
}

static QNode* lower_block(Lower* L, ASTNode* b) {
    QNode* n = qnode(QK_BLOCK, "", NULL, NULL, NULL, b ? b->line : L->quote->line);
    if (!b) return n;
    int mark = L->nlocals;
    int ok = 1;
    for (int i = 0; i < b->child_count; i++) {
        if (!b->children[i]) continue;
        QNode* k = lower_stmt(L, b->children[i]);
        if (!k) { ok = 0; continue; }
        n = qadd(n, k);
    }
    L->nlocals = mark;
    if (!ok) { free_qnode(n); return NULL; }
    return n;
}

/* A block child, or a statement standing where a block is (an `else if`). */
static QNode* lower_branch(Lower* L, ASTNode* s) {
    if (s && s->type == AST_BLOCK) return lower_block(L, s);
    return lower_stmt(L, s);
}

/* `x++` / `x--`, `x += v`: an ASSIGN with the operator. */
static QNode* lower_step(Lower* L, ASTNode* target, const char* op, ASTNode* value,
                         const char* one_type, int line) {
    if (names_capture(L, target)) {
        capture_write_error(L, target, lvalue_root(target)->value, "write");
        return NULL;
    }
    QNode* n = qnode(QK_ASSIGN, "", NULL, op, NULL, line);
    QNode* t = lower_expr(L, target);
    QNode* v = value ? lower_expr(L, value) : qnode(QK_LIT, one_type, NULL, NULL, "1", line);
    if (!t || !v) { free_qnode(t); free_qnode(v); free_qnode(n); return NULL; }
    return qadd(qadd(n, t), v);
}

static QNode* lower_stmt(Lower* L, ASTNode* s) {
    switch (s->type) {
        case AST_BLOCK:
            return lower_block(L, s);
        case AST_VARIABLE_DECLARATION: {
            if (!s->value || s->child_count < 1 || !s->children[0]) {
                return reject(L, s, "a declaration without a value",
                              "every local it has starts with a value");
            }
            ASTNode* init = s->children[0];
            if (is_local(L, s->value)) {
                QNode* n = qnode(QK_ASSIGN, "", NULL, "=", NULL, s->line);
                QNode* t = qnode(QK_LOCAL, node_type_name(s), s->value, NULL, NULL, s->line);
                if (!t->type[0]) t->type = aether_intern(node_type_name(init));
                QNode* v = lower_expr(L, init);
                if (!v) { free_qnode(t); free_qnode(n); return NULL; }
                return qadd(qadd(n, t), v);
            }
            if (is_outer_variable(L, s->value)) {
                capture_write_error(L, s, s->value, "write");
                return NULL;
            }
            QNode* v = lower_expr(L, init);
            if (!v) return NULL;
            const char* t = node_type_name(s);
            if (!t[0]) t = v->type;
            QNode* n = qnode(QK_LET, t, s->value, NULL, NULL, s->line);
            push_local(L, aether_intern(s->value));
            return qadd(n, v);
        }
        case AST_ASSIGNMENT: {
            if (s->child_count < 2) break;
            return lower_step(L, s->children[0], "=", s->children[1], "", s->line);
        }
        case AST_COMPOUND_ASSIGNMENT: {
            if (!s->value || s->child_count < 2 || !s->children[0] || !s->children[0]->value) break;
            ASTNode target = {0};
            target.type = AST_IDENTIFIER;
            target.value = s->value;
            target.line = s->line;
            target.column = s->column;
            target.node_type = s->node_type;
            if (is_outer_variable(L, s->value)) {
                capture_write_error(L, s, s->value, "write");
                return NULL;
            }
            return lower_step(L, &target, s->children[0]->value, s->children[1], "", s->line);
        }
        case AST_EXPRESSION_STATEMENT: {
            if (s->child_count < 1 || !s->children[0]) break;
            ASTNode* x = s->children[0];
            if (x->type == AST_UNARY_EXPRESSION && x->value && x->child_count == 1 &&
                (strcmp(x->value, "++") == 0 || strcmp(x->value, "--") == 0)) {
                return lower_step(L, x->children[0], x->value[0] == '+' ? "+=" : "-=", NULL,
                                  node_type_name(x->children[0]), s->line);
            }
            if (x->type == AST_BINARY_EXPRESSION && x->value && x->child_count == 2 &&
                strcmp(x->value, "=") == 0) {
                return lower_step(L, x->children[0], "=", x->children[1], "", s->line);
            }
            if (x->type == AST_ASSIGNMENT && x->child_count >= 2)
                return lower_step(L, x->children[0], "=", x->children[1], "", s->line);
            if (x->type == AST_CLOSURE || x->type == AST_QUOTE || x->type == AST_STRING_INTERP ||
                x->type == AST_MATCH_STATEMENT || x->type == AST_SEND_FIRE_FORGET ||
                x->type == AST_SEND_ASK || x->type == AST_SEND_STATEMENT) {
                return lower_expr(L, x);  /* reports it */
            }
            QNode* v = lower_expr(L, x);
            if (!v) return NULL;
            return qadd(qnode(QK_EXPR, "", NULL, NULL, NULL, s->line), v);
        }
        case AST_RETURN_STATEMENT: {
            QNode* n = qnode(QK_RETURN, "", NULL, NULL, NULL, s->line);
            if (s->child_count > 1)
                return free_qnode(n), reject(L, s, "returning several values",
                                             "a RETURN has one value");
            if (s->child_count == 1 && s->children[0]) {
                QNode* v = lower_expr(L, s->children[0]);
                if (!v) { free_qnode(n); return NULL; }
                n->type = v->type;
                qadd(n, v);
            }
            return n;
        }
        case AST_IF_STATEMENT: {
            if (s->child_count < 2) break;
            QNode* n = qnode(QK_IF, "", NULL, NULL, NULL, s->line);
            QNode* c = lower_expr(L, s->children[0]);
            QNode* t = lower_branch(L, s->children[1]);
            QNode* f = (s->child_count > 2 && s->children[2]) ? lower_branch(L, s->children[2]) : NULL;
            if (!c || !t || (s->child_count > 2 && s->children[2] && !f)) {
                free_qnode(c); free_qnode(t); free_qnode(f); free_qnode(n);
                return NULL;
            }
            qadd(qadd(n, c), t);
            if (f) qadd(n, f);
            return n;
        }
        case AST_WHILE_LOOP: {
            if (s->child_count < 2) break;
            QNode* n = qnode(QK_WHILE, "", NULL, NULL, NULL, s->line);
            QNode* c = lower_expr(L, s->children[0]);
            QNode* b = lower_branch(L, s->children[1]);
            if (!c || !b) { free_qnode(c); free_qnode(b); free_qnode(n); return NULL; }
            return qadd(qadd(n, c), b);
        }
        case AST_FOR_LOOP: {
            if (s->child_count != 4 || !s->children[0] || !s->children[1] || !s->children[2])
                return reject(L, s, "this form of 'for'",
                              "the tree has the C-style `for (init; cond; step)` only");
            int mark = L->nlocals;
            QNode* n = qnode(QK_FOR, "", NULL, NULL, NULL, s->line);
            QNode* init = lower_stmt(L, s->children[0]);
            QNode* cond = lower_expr(L, s->children[1]);
            ASTNode* step = s->children[2];
            QNode* st = NULL;
            if (step->type == AST_UNARY_EXPRESSION || step->type == AST_BINARY_EXPRESSION ||
                step->type == AST_ASSIGNMENT) {
                ASTNode wrap = {0};
                ASTNode* kids[1] = { step };
                wrap.type = AST_EXPRESSION_STATEMENT;
                wrap.children = kids;
                wrap.child_count = 1;
                wrap.line = step->line;
                wrap.column = step->column;
                st = lower_stmt(L, &wrap);
            } else {
                st = lower_stmt(L, step);
            }
            QNode* body = lower_branch(L, s->children[3]);
            L->nlocals = mark;
            if (!init || !cond || !st || !body) {
                free_qnode(init); free_qnode(cond); free_qnode(st); free_qnode(body); free_qnode(n);
                return NULL;
            }
            return qadd(qadd(qadd(qadd(n, init), cond), st), body);
        }
        case AST_BREAK_STATEMENT:
            return qnode(QK_BREAK, "", NULL, NULL, NULL, s->line);
        case AST_CONTINUE_STATEMENT:
            return qnode(QK_CONTINUE, "", NULL, NULL, NULL, s->line);
        case AST_DEFER_STATEMENT:
            return reject(L, s, "'defer'", "it has no 'defer' node");
        case AST_TRY_STATEMENT:
            return reject(L, s, "'try'/'catch'", "it has no node for unwinding");
        case AST_PANIC_STATEMENT:
            return reject(L, s, "'panic'", "it has no node for unwinding");
        case AST_SEND_STATEMENT:
        case AST_SEND_FIRE_FORGET:
        case AST_SEND_ASK:
            return reject(L, s, "an actor send", "an actor operation has no meaning in a tree");
        case AST_SPAWN_ACTOR_STATEMENT:
            return reject(L, s, "'spawn'", "an actor operation has no meaning in a tree");
        case AST_RECEIVE_STATEMENT:
            return reject(L, s, "'receive'", "an actor operation has no meaning in a tree");
        case AST_REPLY_STATEMENT:
            return reject(L, s, "'reply'", "an actor operation has no meaning in a tree");
        case AST_MATCH_STATEMENT:
        case AST_SWITCH_STATEMENT:
            return reject(L, s, s->type == AST_MATCH_STATEMENT ? "'match'" : "'switch'",
                          "version 1 has no node for patterns");
        case AST_TUPLE_DESTRUCTURE:
            return reject(L, s, "a tuple destructure", "version 1 has no node for tuples");
        case AST_PRINT_STATEMENT:
            return reject(L, s, "'print'", "it has no node for output");
        default:
            if (s->type == AST_FUNCTION_CALL || s->type == AST_CLOSURE || s->type == AST_QUOTE ||
                s->type == AST_STRING_INTERP || s->type == AST_IF_EXPRESSION) {
                QNode* v = lower_expr(L, s);
                if (!v) return NULL;
                return qadd(qnode(QK_EXPR, "", NULL, NULL, NULL, s->line), v);
            }
            break;
    }
    char what[96];
    snprintf(what, sizeof what, "this statement (%s)", ast_node_type_to_string(s->type));
    return reject(L, s, what, "version 1's tree has no node for it");
}

/* ---- hash ---- */

static void fnv(uint64_t* h, const char* s) {
    for (; s && *s; s++) { *h ^= (unsigned char)*s; *h *= 1099511628211ULL; }
    *h ^= 0xff; *h *= 1099511628211ULL;   /* field separator */
}

static void fnv_int(uint64_t* h, int v) {
    char buf[24];
    snprintf(buf, sizeof buf, "%d", v);
    fnv(h, buf);
}

static void hash_node(uint64_t* h, QNode* n) {
    fnv_int(h, n->kind);
    fnv(h, n->type);
    fnv(h, n->name);
    fnv(h, n->op);
    fnv(h, n->lit);
    fnv_int(h, n->nkids);
    for (int i = 0; i < n->nkids; i++) hash_node(h, n->kids[i]);
}

/* The hash: the format version, the parameters, the captures, the body,
 * then each helper the quote reaches (its name, return type, parameters and
 * body), callees first. Editing a helper changes the hash of every quote
 * that reaches it, so a back end's cache keyed on it goes stale with it. */
static uint64_t quote_hash(QuoteInfo* q) {
    uint64_t h = 14695981039346656037ULL;
    fnv_int(&h, QUOTE_FORMAT_VERSION);
    for (int i = 0; i < q->nparams; i++) { fnv(&h, q->param_names[i]); fnv(&h, q->param_types[i]); }
    for (int i = 0; i < q->ncaps; i++) { fnv(&h, q->caps[i].name); fnv(&h, q->caps[i].type); }
    hash_node(&h, q->body);
    for (int i = 0; i < q->nhelpers; i++) {
        QHelper* x = q->helpers[i];
        fnv(&h, x->name);
        fnv(&h, x->ret);
        for (int k = 0; k < x->nparams; k++) { fnv(&h, x->param_names[k]); fnv(&h, x->param_types[k]); }
        if (x->body) hash_node(&h, x->body);
    }
    return h;
}

int quote_lower_helper(QHelper* hp, SymbolTable* global, QuoteErrorFn report) {
    if (!hp || hp->lowered) return hp && hp->lowered < 0 ? 1 : 0;
    ASTNode* def = hp->def;
    Lower L;
    memset(&L, 0, sizeof L);
    L.quote = def;
    L.outer = global;
    L.report = report;
    L.helper = hp;

    ASTNode* body = NULL;
    int nparams = 0;
    for (int i = 0; i < def->child_count; i++) {
        ASTNode* c = def->children[i];
        if (!c) continue;
        if (c->type == AST_BLOCK) body = c;
        else if (c->type != AST_GUARD_CLAUSE && c->type != AST_REQUIRES_CLAUSE &&
                 c->type != AST_ENSURES_CLAUSE) nparams++;
    }
    hp->param_names = (const char**)calloc((size_t)(nparams ? nparams : 1), sizeof(char*));
    hp->param_types = (const char**)calloc((size_t)(nparams ? nparams : 1), sizeof(char*));
    for (int i = 0; i < def->child_count; i++) {
        ASTNode* c = def->children[i];
        if (!c || c->type == AST_BLOCK || c->type == AST_GUARD_CLAUSE ||
            c->type == AST_REQUIRES_CLAUSE || c->type == AST_ENSURES_CLAUSE) continue;
        if (c->type != AST_PATTERN_VARIABLE) {
            reject(&L, c, "a parameter pattern in a @quotable function",
                   "a function written out from the tree takes named parameters");
            continue;
        }
        const char* pn = aether_intern(c->value ? c->value : "");
        push_local(&L, pn);
        hp->param_names[hp->nparams] = pn;
        hp->param_types[hp->nparams] = aether_intern(quote_type_name(c->node_type));
        hp->nparams++;
    }
    for (int i = 0; i < def->child_count; i++) {
        ASTNode* c = def->children[i];
        if (c && (c->type == AST_GUARD_CLAUSE || c->type == AST_REQUIRES_CLAUSE ||
                  c->type == AST_ENSURES_CLAUSE))
            reject(&L, c, "a guard or contract on a @quotable function",
                   "the tree has no node for a precondition");
    }
    hp->ret = aether_intern(quote_type_name(def->node_type));
    QNode* tree = lower_block(&L, body);
    free(L.locals);
    free(L.caps);
    if (L.errors) {
        free_qnode(tree);
        free(L.called);
        hp->lowered = -1;
        return L.errors;
    }
    hp->body = tree;
    hp->callees = L.called;
    hp->ncallees = L.ncalled;
    hp->capcallees = L.capcalled;
    hp->lowered = 1;
    return 0;
}

/* Neither GLSL nor MSL allows recursion, and a tree that reaches itself has
 * no order to emit its functions in: a @quotable function reached again
 * through @quotable calls is an error, reported once per cycle at the
 * function that closes it. */
static int recursion_dfs(QHelper* h, QHelper** stack, int depth, QuoteErrorFn report) {
    if (h->mark == 2) return 0;
    if (h->mark == 1) {
        int start = depth - 1;
        while (start > 0 && stack[start] != h) start--;
        char path[512] = "";
        for (int i = start; i < depth; i++) {
            size_t len = strlen(path);
            snprintf(path + len, sizeof path - len, "%s -> ", stack[i]->def->value);
        }
        size_t len = strlen(path);
        snprintf(path + len, sizeof path - len, "%s", h->def->value);
        char msg[640];
        snprintf(msg, sizeof msg, "@quotable function '%s' reaches itself: %s",
                 h->def->value, path);
        if (report) report(msg, "GLSL and MSL have no recursion, so a quote's helpers can't "
                           "either; write the repetition as a loop",
                           stack[depth - 1]->def->line, stack[depth - 1]->def->column,
                           AETHER_ERR_QUOTE_RECURSION);
        return 1;
    }
    h->mark = 1;
    stack[depth] = h;
    int errors = 0;
    for (int i = 0; i < h->ncallees; i++)
        errors += recursion_dfs(h->callees[i], stack, depth + 1, report);
    h->mark = 2;
    return errors;
}

int quote_check_recursion(QuoteErrorFn report) {
    /* A path holds each helper once, so it is never longer than the helpers
     * (#3574): 256 slots stopped following past them, and a cycle deeper
     * than that went unreported. */
    QHelper** stack = (QHelper**)aether_xrealloc(NULL, sizeof(QHelper*) *
                                                           (size_t)(g_helper_count + 1));
    int errors = 0;
    for (int i = 0; i < g_helper_count; i++) g_helpers[i]->mark = 0;
    for (int i = 0; i < g_helper_count; i++)
        if (g_helpers[i]->mark == 0) errors += recursion_dfs(g_helpers[i], stack, 0, report);
    free(stack);
    return errors;
}

/* Every helper `h` reaches, callees before callers, appended once each.
 * A cycle is already an error (quote_check_recursion), but quote_finish still
 * runs after it, so a helper on the current path is skipped: mark 3 = on it. */
static void reach(QHelper* h, QHelper*** out, int* n, int* cap) {
    if (h->mark == 3) return;
    for (int i = 0; i < *n; i++) if ((*out)[i] == h) return;
    int saved = h->mark;
    h->mark = 3;
    for (int i = 0; i < h->ncallees; i++) reach(h->callees[i], out, n, cap);
    h->mark = saved;
    for (int i = 0; i < *n; i++) if ((*out)[i] == h) return;
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 4;
        *out = (QHelper**)aether_xrealloc(*out, (size_t)*cap * sizeof(QHelper*));
    }
    (*out)[(*n)++] = h;
}

void quote_finish(void) {
    for (int q = 0; q < g_quote_count; q++) {
        QuoteInfo* qi = g_quotes[q];
        QHelper** all = NULL;
        int n = 0, cap = 0;
        for (int i = 0; i < qi->nhelpers; i++) reach(qi->helpers[i], &all, &n, &cap);
        free(qi->helpers);
        qi->helpers = all;
        qi->nhelpers = n;
        qi->hash = quote_hash(qi);
    }
}

/* ---- entry ---- */

int quote_lower(ASTNode* quote, SymbolTable* outer, const char* file, QuoteErrorFn report) {
    Lower L;
    memset(&L, 0, sizeof L);
    L.quote = quote;
    L.outer = outer;
    L.report = report;

    ASTNode* body = NULL;
    int nparams = 0;
    for (int i = 0; i < quote->child_count; i++) {
        ASTNode* c = quote->children[i];
        if (!c) continue;
        if (c->type == AST_CLOSURE_PARAM) {
            nparams++;
            if (c->value) push_local(&L, aether_intern(c->value));
        } else if (c->type == AST_BLOCK) {
            body = c;
        }
    }
    QNode* tree = lower_block(&L, body);
    if (L.errors) {
        free_qnode(tree);
        free(L.locals);
        free(L.caps);
        free(L.called);
        return L.errors;
    }

    QuoteInfo* q = (QuoteInfo*)calloc(1, sizeof(QuoteInfo));
    if (!q) { fprintf(stderr, "aetherc: out of memory lowering a quote\n"); exit(1); }
    q->node = quote;
    q->body = tree;
    q->nparams = nparams;
    q->param_names = (const char**)calloc((size_t)(nparams ? nparams : 1), sizeof(char*));
    q->param_types = (const char**)calloc((size_t)(nparams ? nparams : 1), sizeof(char*));
    int pi = 0;
    for (int i = 0; i < quote->child_count; i++) {
        ASTNode* c = quote->children[i];
        if (!c || c->type != AST_CLOSURE_PARAM) continue;
        q->param_names[pi] = aether_intern(c->value ? c->value : "");
        q->param_types[pi] = aether_intern(quote_type_name(c->node_type));
        pi++;
    }
    q->caps = L.caps;
    q->ncaps = L.ncaps;
    const char* f = file ? file : "";
    const char* slash = strrchr(f, '/');
    q->source = aether_internf("%s:%d", slash ? slash + 1 : f, quote->line);

    q->helpers = L.called;
    q->nhelpers = L.ncalled;
    q->hash = quote_hash(q);   /* final once quote_finish adds the helpers it reaches */
    free(L.locals);

    if (g_quote_count == g_quote_cap) {
        g_quote_cap = g_quote_cap ? g_quote_cap * 2 : 8;
        g_quotes = (QuoteInfo**)aether_xrealloc(g_quotes, (size_t)g_quote_cap * sizeof(QuoteInfo*));
    }
    g_quotes[g_quote_count] = q;
    char idx[24];
    snprintf(idx, sizeof idx, "%d", g_quote_count);
    g_quote_count++;
    free(quote->value);
    quote->value = strdup(idx);
    return 0;
}

/* ---- codegen ---- */

static void emit_c_string(FILE* out, const char* s) {
    fputc('"', out);
    for (const unsigned char* p = (const unsigned char*)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\') { fputc('\\', out); fputc(*p, out); }
        else if (*p == '\n') fputs("\\n", out);
        else if (*p == '\t') fputs("\\t", out);
        else if (*p == '\r') fputs("\\r", out);
        else if (*p < 32 || *p == '?' || *p >= 127) fprintf(out, "\\%03o", *p);
        else fputc(*p, out);
    }
    fputc('"', out);
}

static int emit_qnode(FILE* out, QNode* n, int* counter) {
    int id = (*counter)++;
    fprintf(out, "    void* _n%d = aether_quote_node(%d, ", id, n->kind);
    emit_c_string(out, n->type);
    fputs(", ", out);
    emit_c_string(out, n->name);
    fputs(", ", out);
    emit_c_string(out, n->op);
    fputs(", ", out);
    emit_c_string(out, n->lit);
    fprintf(out, ", %d);\n", n->line);
    for (int i = 0; i < n->nkids; i++) {
        int k = emit_qnode(out, n->kids[i], counter);
        fprintf(out, "    aether_quote_add(_n%d, _n%d);\n", id, k);
    }
    return id;
}

void quote_emit_prototypes(FILE* out) {
    fputs("/* std.quote's constructors (docs/quote.md), called by the quote builders. */\n"
          "void* aether_quote_tree(const char* source, int64_t hash);\n"
          "void* aether_quote_node(int kind, const char* type, const char* name, const char* op, const char* lit, int line);\n"
          "void* aether_quote_add(void* parent, void* child);\n"
          "void aether_quote_set_body(void* tree, void* body);\n"
          "void aether_quote_param(void* tree, const char* name, const char* type);\n"
          "void aether_quote_capture(void* tree, const char* name, const char* type, const char* base, void* cell, void (*release)(const void*));\n"
          "void* aether_quote_helper(const char* name, const char* ret);\n"
          "void aether_quote_add_helper(void* tree, void* helper);\n",
          out);
}

void quote_emit_builder(FILE* out, int index, const char* const* release_fns) {
    QuoteInfo* q = quote_at(index);
    if (!q) return;
    fprintf(out, "/* the tree of the quote at %s */\n", q->source);
    fprintf(out, "static void* _quote_build_%d(void** _cells) {\n", index);
    fprintf(out, "    (void)_cells;\n    void* _t = aether_quote_tree(");
    emit_c_string(out, q->source);
    fprintf(out, ", (int64_t)0x%016llxULL);\n", (unsigned long long)q->hash);
    for (int i = 0; i < q->nparams; i++) {
        fputs("    aether_quote_param(_t, ", out);
        emit_c_string(out, q->param_names[i]);
        fputs(", ", out);
        emit_c_string(out, q->param_types[i]);
        fputs(");\n", out);
    }
    for (int i = 0; i < q->ncaps; i++) {
        fputs("    aether_quote_capture(_t, ", out);
        emit_c_string(out, q->caps[i].name);
        fputs(", ", out);
        emit_c_string(out, q->caps[i].type);
        fputs(", ", out);
        emit_c_string(out, q->caps[i].base);
        if (q->caps[i].global) {
            /* A global's cell is the global: no reference to take. */
            fprintf(out, ", _cells[%d], NULL);\n", i);
            continue;
        }
        fprintf(out, ", _aether_cell_retain(_cells[%d]), %s);\n", i,
                release_fns && release_fns[i] ? release_fns[i] : "_aether_cell_release");
    }
    int counter = 0;
    /* Version 2: the @quotable functions it reaches, callees first. */
    for (int i = 0; i < q->nhelpers; i++) {
        QHelper* h = q->helpers[i];
        fprintf(out, "    void* _h%d = aether_quote_helper(", i);
        emit_c_string(out, h->name);
        fputs(", ", out);
        emit_c_string(out, h->ret);
        fputs(");\n", out);
        for (int k = 0; k < h->nparams; k++) {
            fprintf(out, "    aether_quote_param(_h%d, ", i);
            emit_c_string(out, h->param_names[k]);
            fputs(", ", out);
            emit_c_string(out, h->param_types[k]);
            fputs(");\n", out);
        }
        int hb = emit_qnode(out, h->body, &counter);
        fprintf(out, "    aether_quote_set_body(_h%d, _n%d);\n    aether_quote_add_helper(_t, _h%d);\n",
                i, hb, i);
    }
    int body = emit_qnode(out, q->body, &counter);
    fprintf(out, "    aether_quote_set_body(_t, _n%d);\n    return _t;\n}\n", body);
}
