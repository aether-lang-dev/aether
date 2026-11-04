/* Slice ownership: a `make` or `append` buffer is released when the local
 * that owns it goes out of scope. See slice_own.h for the rules. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "slice_own.h"
#include "hoist.h"
#include "../aether_error.h"
#include "../aether_strmap.h"

/* ---- what a local is ---- */

typedef enum {
    K_NONE,   /* not a slice */
    K_OWNER,  /* owns what it holds: dropped at every exit of its scope */
    K_VIEW,   /* only ever given views: holds nothing to drop */
    K_OTHER,  /* a slice local left to the program (captured, a pattern's) */
    K_PARAM,  /* a parameter: borrowed */
    K_KEEP,   /* a `@keep` parameter: owned, as a local that owns */
    /* An owner a view of whose buffer may outlive it (escape analysis,
     * below): never dropped, as before ownership. */
    K_LOCALVIEW, /* the views stay in the function: a `return` still moves it */
    K_PINNED     /* a view may leave the function: every copy is a loan */
} Kind;

typedef struct Local Local;
struct Local {
    const char* name;
    ASTNode* decl;      /* the node that binds it */
    ASTNode* block;     /* the block `decl` is a statement of, or NULL */
    int hoisted;        /* codegen declares it ahead of where it is bound */
    ASTNode* anchor;    /* hoisted: the statement of `block` it is declared
                         * before (NULL: the start of `block`) */
    Type* type;
    int idx;
    int is_slice;
    int keep;           /* a `@keep` parameter */
    int param;
    int pattern;        /* bound by a pattern, a destructure, a `catch` */
    int captured;       /* a closure written in the frame names it */
    int addr_taken;     /* `&x` */
    int fresh;          /* some binding may give it a buffer to own */
    int target;         /* `free`, `append` or `push` is applied to it */
    int depth;          /* how many blocks deep its scope is */
    int pidx;           /* a parameter: its position, else -1 */
    unsigned char* roots; /* the locals whose buffers what it holds may view */
    int pinned;         /* a view of its buffer may leave the function */
    int scoped_out;     /* a view of its buffer is bound outside its scope */
    int viewed;         /* a view of its buffer is bound to a local */
    int mutated;        /* re-bound, moved, freed, grown, or a row replaced */
    int first_view;     /* the earliest point a view of its buffer is taken */
    int last_mut;       /* the latest point it is mutated */
    int self_view;      /* re-bound to a view of what it held */
    Local** srcs;       /* the locals a binding copies it from */
    int nsrcs, srcs_cap;
    Kind kind;
};

/* A map from a node to a local. */
typedef struct {
    const void** keys;
    Local** vals;
    int cap, count;
} PtrMap;

static unsigned ptr_hash(const void* p) {
    unsigned long long x = (unsigned long long)(size_t)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (unsigned)x;
}

static void pm_put(PtrMap* m, const void* k, Local* v);

static void pm_grow(PtrMap* m) {
    PtrMap n;
    memset(&n, 0, sizeof(n));
    n.cap = m->cap ? m->cap * 2 : 64;
    n.keys = (const void**)calloc((size_t)n.cap, sizeof(void*));
    n.vals = (Local**)calloc((size_t)n.cap, sizeof(Local*));
    for (int i = 0; i < m->cap; i++)
        if (m->keys[i]) pm_put(&n, m->keys[i], m->vals[i]);
    free(m->keys);
    free(m->vals);
    *m = n;
}

static void pm_put(PtrMap* m, const void* k, Local* v) {
    if ((m->count + 1) * 2 > m->cap) pm_grow(m);
    unsigned i = ptr_hash(k) & (unsigned)(m->cap - 1);
    while (m->keys[i] && m->keys[i] != k) i = (i + 1) & (unsigned)(m->cap - 1);
    if (!m->keys[i]) m->count++;
    m->keys[i] = k;
    m->vals[i] = v;
}

static Local* pm_get(const PtrMap* m, const void* k) {
    if (!m->cap) return NULL;
    unsigned i = ptr_hash(k) & (unsigned)(m->cap - 1);
    while (m->keys[i]) {
        if (m->keys[i] == k) return m->vals[i];
        i = (i + 1) & (unsigned)(m->cap - 1);
    }
    return NULL;
}

static void pm_free(PtrMap* m) {
    free(m->keys);
    free(m->vals);
    memset(m, 0, sizeof(*m));
}

/* The names of locals that own a buffer, as the last check found them, for
 * the escape check (slice_own_owner_ident). */
static PtrMap g_owner_idents;

/* ---- the program and the frame being walked ---- */

typedef struct {
    ASTNode* program;
    StrMap globals;     /* module-level `var` names */
    StrMap state;       /* the state fields of the actor being walked */
    StrMap fns;         /* name -> function definition */
    int has_append;     /* the program defines a function `append` */
    int errors;
    PtrMap reported;    /* the nodes an error was reported at */
    PtrMap sums;        /* function definition -> its Summary */
    int final;          /* the pass that reports and rewrites */
    int changed;        /* a summary grew in this pass */
} Ctx;

/* What a function does with its slice parameters, as the escape analysis
 * finds it: whether a view of what parameter i is given may be in its
 * result (res), or stored where it outlives the call (store). */
typedef struct {
    int n;
    unsigned char* res;      /* a view of the buffer itself */
    unsigned char* res_deep; /* what its elements hold (a struct copied out) */
    unsigned char* store;
} Summary;

typedef struct Frame {
    ASTNode* owner;
    ASTNode* body;
    Local** locals;
    int nlocals, cap;
    PtrMap uses;         /* identifier -> the local it names */
    PtrMap binds;        /* binding node -> the local it binds */
    PtrMap row_ok;       /* row stores that release the row they replace */
    const StrMap* outer; /* the names of the frames a closure is written in */
} Frame;

typedef struct Scope {
    StrMap names;
    struct Scope* up;
    int depth;
} Scope;

static Local* scope_lookup(Scope* s, const char* name) {
    for (; s; s = s->up) {
        Local* l = (Local*)strmap_get(&s->names, name);
        if (l) return l;
    }
    return NULL;
}

static Local* new_local(Frame* f, Scope* s, ASTNode* decl, ASTNode* block) {
    Local* l = (Local*)calloc(1, sizeof(Local));
    l->name = decl->value;
    l->decl = decl;
    l->block = block;
    l->type = decl->node_type;
    l->is_slice = type_is_slice(decl->node_type);
    l->idx = f->nlocals;
    l->depth = s ? s->depth : 0;
    l->pidx = -1;
    if (f->nlocals == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 16;
        f->locals = (Local**)realloc(f->locals, (size_t)f->cap * sizeof(Local*));
    }
    f->locals[f->nlocals++] = l;
    if (s) strmap_put(&s->names, decl->value, l);
    pm_put(&f->binds, decl, l);
    return l;
}

/* A local codegen declares ahead of the statement that binds it (hoist.h):
 * before `anchor`, a statement of `block`, or at the start of `block` when
 * `anchor` is NULL. Every binding of it is a store into it. */
static Local* new_hoisted(Frame* f, Scope* s, ASTNode* first, ASTNode* block, ASTNode* anchor) {
    Local* l = (Local*)calloc(1, sizeof(Local));
    l->name = first->value;
    l->decl = NULL;
    l->block = block;
    l->hoisted = 1;
    l->anchor = anchor;
    l->type = hoist_decl_type(first);
    if (!l->type) l->type = first->node_type;
    l->is_slice = type_is_slice(l->type);
    l->idx = f->nlocals;
    l->depth = s->depth;
    l->pidx = -1;
    if (f->nlocals == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 16;
        f->locals = (Local**)realloc(f->locals, (size_t)f->cap * sizeof(Local*));
    }
    f->locals[f->nlocals++] = l;
    strmap_put(&s->names, first->value, l);
    return l;
}

static void add_src(Local* l, Local* src) {
    if (l->nsrcs == l->srcs_cap) {
        l->srcs_cap = l->srcs_cap ? l->srcs_cap * 2 : 4;
        l->srcs = (Local**)realloc(l->srcs, (size_t)l->srcs_cap * sizeof(Local*));
    }
    l->srcs[l->nsrcs++] = src;
}

/* ---- the tree ---- */

static int is_call(ASTNode* n, const char* name) {
    return n && n->type == AST_FUNCTION_CALL && n->value && strcmp(n->value, name) == 0;
}

/* The builtin `append(xs, v)`, not a function of the program's own. */
static int is_append(Ctx* c, ASTNode* n) {
    return is_call(n, "append") && n->child_count == 2 && !c->has_append &&
           type_is_slice(n->node_type) && n->children[0] &&
           type_is_slice(n->children[0]->node_type);
}

static int is_free(ASTNode* n) {
    return is_call(n, "free") && n->child_count == 1 && n->children[0] &&
           type_is_slice(n->children[0]->node_type);
}

static int is_push(ASTNode* n) {
    return is_call(n, "__slice_push") && n->child_count == 2;
}

/* One of the intrinsics this pass writes: a tree walked twice is left as
 * it is. */
static int is_intrinsic(ASTNode* n) {
    return is_call(n, "__slice_take") || is_call(n, "__slice_lend") ||
           is_call(n, "__slice_drop") || is_call(n, "__slice_set");
}

/* A place read the same way twice: a name, a field of one, an element at
 * an index that is a name or a literal. */
static int pure_place(ASTNode* e) {
    if (!e) return 0;
    switch (e->type) {
        case AST_IDENTIFIER:
            return e->value != NULL;
        case AST_MEMBER_ACCESS:
            return e->child_count == 1 && pure_place(e->children[0]);
        case AST_ARRAY_ACCESS:
            return e->child_count == 2 && pure_place(e->children[0]) && e->children[1] &&
                   (e->children[1]->type == AST_IDENTIFIER || e->children[1]->type == AST_LITERAL);
        default:
            return 0;
    }
}

static int node_line(ASTNode* n) {
    if (!n) return 0;
    if (n->line > 0) return n->line;
    for (int i = 0; i < n->child_count; i++) {
        int l = node_line(n->children[i]);
        if (l > 0) return l;
    }
    return 0;
}

static ASTNode* frame_body(ASTNode* owner) {
    if (owner->type == AST_MAIN_FUNCTION)
        return owner->child_count > 0 && owner->children[0] &&
                       owner->children[0]->type == AST_BLOCK
                   ? owner->children[0]
                   : NULL;
    for (int i = owner->child_count - 1; i >= 0; i--)
        if (owner->children[i] && owner->children[i]->type == AST_BLOCK) return owner->children[i];
    return NULL;
}

/* A function of its own: its locals are its own, and it is checked alone. */
static int is_frame(ASTNode* n) {
    if (!n) return 0;
    switch (n->type) {
        case AST_FUNCTION_DEFINITION: case AST_BUILDER_FUNCTION: case AST_MAIN_FUNCTION:
        case AST_RECEIVE_ARM: case AST_TIMEOUT_ARM:
            return 1;
        case AST_CLOSURE:
            return ast_is_own_function(n);
        default:
            return 0;
    }
}

/* A quote's body is a tree, not code of the function (docs/quote.md). */
static int is_opaque(ASTNode* n) {
    return n && n->type == AST_QUOTE;
}

static int has_marker(ASTNode* p, const char* tag) {
    if (!p || !p->annotation) return 0;
    size_t n = strlen(tag);
    for (const char* q = p->annotation; (q = strstr(q, tag)) != NULL; q += n) {
        int left = q == p->annotation || q[-1] == ';' || q[-1] == ',';
        int right = q[n] == '\0' || q[n] == ';' || q[n] == ',';
        if (left && right) return 1;
    }
    return 0;
}

/* ---- which local each name and each binding is ---- */

static void collect_names(ASTNode* n, StrMap* out) {
    if (!n) return;
    if ((n->type == AST_VARIABLE_DECLARATION || n->type == AST_PATTERN_VARIABLE ||
         n->type == AST_CLOSURE_PARAM || n->type == AST_IDENTIFIER) && n->value)
        strmap_put(out, n->value, (void*)1);
    for (int i = 0; i < n->child_count; i++) collect_names(n->children[i], out);
}

/* A closure written in the frame: every local of the frame it names, read
 * or bound, is captured, and left to the program. */
static void mark_captured(Scope* s, ASTNode* closure) {
    StrMap names;
    strmap_init(&names);
    collect_names(closure, &names);
    for (int i = 0; i < strmap_count(&names); i++) {
        Local* l = scope_lookup(s, strmap_key_at(&names, i));
        if (l) l->captured = 1;
    }
    strmap_free(&names);
}

/* Can the value `e` give the slice local `l` a buffer to own? A view (a
 * sub-slice, an array, a field or an element read, a cast) cannot; a call
 * can, and so can a local that owns one. */
static void note_source(Frame* f, Local* l, ASTNode* e) {
    if (!e) return;
    switch (e->type) {
        case AST_SLICE_EXPR: case AST_SLICE_FROM_ARRAY: case AST_MEMBER_ACCESS:
        case AST_ARRAY_ACCESS: case AST_PTR_AS_ARRAY_CAST: case AST_NULL_LITERAL:
        case AST_ARRAY_LITERAL:
            return;
        case AST_IDENTIFIER: {
            Local* s = pm_get(&f->uses, e);
            if (s && !s->param) add_src(l, s);
            if (s && s->keep) add_src(l, s);
            return;
        }
        default:
            l->fresh = 1;
            return;
    }
}

typedef struct {
    Ctx* c;
    Frame* f;
} R;

static void resolve(R* r, Scope* s, ASTNode* n, ASTNode* block);

static void resolve_children(R* r, Scope* s, ASTNode* n) {
    for (int i = 0; i < n->child_count; i++) resolve(r, s, n->children[i], NULL);
}

static void resolve_scoped(R* r, Scope* s, ASTNode* n) {
    Scope in;
    strmap_init(&in.names);
    in.up = s;
    in.depth = s ? s->depth + 1 : 1;
    resolve_children(r, &in, n);
    strmap_free(&in.names);
}

/* Is `name`, bound bare, a store into something that is no local of the
 * frame: a module-level `var`, an actor's state field, a variable of a
 * frame a closure is written in? */
static int store_elsewhere(R* r, const char* name) {
    return strmap_has(&r->c->globals, name) || strmap_has(&r->c->state, name) ||
           (r->f->outer && strmap_has(r->f->outer, name));
}

static void bind(R* r, Scope* s, ASTNode* n, ASTNode* block) {
    Frame* f = r->f;
    resolve_children(r, s, n);
    ASTNode* init = n->child_count > 0 ? n->children[0] : NULL;
    Local* l = scope_lookup(s, n->value);
    if (l && !n->type_inferred && !l->hoisted) l = NULL;   /* a new, typed declaration */
    if (l) {
        pm_put(&f->binds, n, l);
    } else if (n->type_inferred && store_elsewhere(r, n->value)) {
        return;
    } else {
        l = new_local(f, s, n, block);
    }
    if (l->is_slice) note_source(f, l, init);
}

typedef struct {
    R* r;
    Scope* s;
    ASTNode* block;
    ASTNode* anchor;
} Hoist;

static void hoist_one(Hoist* h, ASTNode* first) {
    if (!first || !first->value) return;
    if (scope_lookup(h->s, first->value) || store_elsewhere(h->r, first->value)) return;
    new_hoisted(h->r->f, h->s, first, h->block, h->anchor);
}

static void hoist_visit(ASTNode* decl, void* user) {
    hoist_one((Hoist*)user, decl);
}

/* The names codegen declares ahead of the statement `n` (hoist.h): what
 * every arm of an if, a switch or a match binds, and a while body's
 * locals. They are the locals of the scope around `n`; `block` is the block
 * `n` is a statement of, or NULL. */
static void hoist_before(R* r, Scope* s, ASTNode* n, ASTNode* block) {
    Hoist h = {r, s, block, n};
    if (n->type == AST_WHILE_LOOP) {
        if (n->child_count > 1) hoist_loop_decls(n->children[1], hoist_visit, &h);
        return;
    }
    if (n->type != AST_IF_STATEMENT && n->type != AST_MATCH_STATEMENT &&
        n->type != AST_SWITCH_STATEMENT)
        return;
    const HoistNames* hn = hoist_common_hoists(n);
    for (int i = 0; hn && i < hn->count; i++) hoist_one(&h, hoist_names_decl(hn, i));
}

static void resolve(R* r, Scope* s, ASTNode* n, ASTNode* block) {
    if (!n || is_opaque(n)) return;
    hoist_before(r, s, n, block);
    if (is_frame(n)) {
        mark_captured(s, n);
        return;
    }
    switch (n->type) {
        case AST_BLOCK: {
            Scope in;
            strmap_init(&in.names);
            in.up = s;
            in.depth = s ? s->depth + 1 : 1;
            for (int i = 0; i < n->child_count; i++) resolve(r, &in, n->children[i], n);
            strmap_free(&in.names);
            return;
        }
        case AST_VARIABLE_DECLARATION:
            if (n->value) {
                bind(r, s, n, block);
                return;
            }
            break;
        case AST_TUPLE_DESTRUCTURE: {
            if (n->child_count > 0) resolve(r, s, n->children[n->child_count - 1], NULL);
            for (int i = 0; i + 1 < n->child_count; i++) {
                ASTNode* v = n->children[i];
                if (!v || !v->value) continue;
                Local* l = scope_lookup(s, v->value);
                if (l) pm_put(&r->f->binds, v, l);
                else if (store_elsewhere(r, v->value)) continue;
                else l = new_local(r->f, s, v, NULL);
                l->pattern = 1;
            }
            return;
        }
        case AST_PATTERN_VARIABLE:
            if (n->value) {
                Local* l = new_local(r->f, s, n, NULL);
                l->pattern = 1;
            }
            resolve_children(r, s, n);
            return;
        case AST_CATCH_CLAUSE: {
            Scope in;
            strmap_init(&in.names);
            in.up = s;
            in.depth = s ? s->depth + 1 : 1;
            if (n->value) {
                Local* l = new_local(r->f, &in, n, NULL);
                l->pattern = 1;
            }
            resolve_children(r, &in, n);
            strmap_free(&in.names);
            return;
        }
        case AST_MATCH_ARM: case AST_FOR_LOOP: case AST_CASE_STATEMENT:
            resolve_scoped(r, s, n);
            return;
        case AST_IDENTIFIER: {
            Local* l = n->value ? scope_lookup(s, n->value) : NULL;
            if (l) pm_put(&r->f->uses, n, l);
            return;
        }
        case AST_UNARY_EXPRESSION:
            if (n->value && strcmp(n->value, "&") == 0 && n->child_count == 1 &&
                n->children[0] && n->children[0]->type == AST_IDENTIFIER &&
                n->children[0]->value) {
                Local* l = scope_lookup(s, n->children[0]->value);
                if (l) l->addr_taken = 1;
            }
            break;
        case AST_FUNCTION_CALL:
            /* What `free`, `append` and `push` take a buffer from owns it. */
            if ((is_free(n) || is_append(r->c, n) || is_push(n)) && n->children[0] &&
                n->children[0]->type == AST_IDENTIFIER && n->children[0]->value) {
                Local* l = scope_lookup(s, n->children[0]->value);
                if (l) l->target = 1;
            }
            break;
        default:
            break;
    }
    resolve_children(r, s, n);
}

/* ---- what each local is ---- */

static void classify(Frame* f) {
    for (int i = 0; i < f->nlocals; i++) {
        Local* l = f->locals[i];
        if (!l->is_slice) l->kind = K_NONE;
        else if (l->param) l->kind = !l->keep ? K_PARAM
                                     : (l->captured || l->addr_taken) ? K_OTHER : K_KEEP;
        else if (l->pattern || l->captured || l->addr_taken || !l->block) l->kind = K_OTHER;
        else if (l->fresh || l->target) l->kind = K_OWNER;
        else l->kind = K_VIEW;
    }
    /* A local a local that owns is copied into may be given its buffer. */
    for (int changed = 1; changed;) {
        changed = 0;
        for (int i = 0; i < f->nlocals; i++) {
            Local* l = f->locals[i];
            if (l->kind != K_VIEW) continue;
            for (int j = 0; j < l->nsrcs; j++) {
                Kind k = l->srcs[j]->kind;
                if (k == K_OWNER || k == K_KEEP) {
                    l->kind = K_OWNER;
                    changed = 1;
                    break;
                }
            }
        }
    }
}

static int owns(const Local* l) {
    return l && (l->kind == K_OWNER || l->kind == K_KEEP);
}

static void report(Ctx* c, Frame* f, ASTNode* at, AetherErrorCode code, const char* msg,
                   const char* hint) {
    if (pm_get(&c->reported, at)) return;
    pm_put(&c->reported, at, (Local*)1);
    AetherError e = {at && at->source_file ? at->source_file
                                           : (f->owner ? f->owner->source_file : NULL),
                     NULL, node_line(at), at ? at->column : 0, msg, hint, NULL, code};
    aether_error_report(&e);
    c->errors++;
}

/* ---- where a value goes ---- */

typedef enum { C_BORROW, C_MOVE } Use;

/* How the `i`th child of `p` is used when it is a slice: borrowed (read,
 * indexed, measured, viewed, compared), or moved into a place (bound,
 * returned, stored, put in a struct, a tuple, an array or a message). A
 * place this does not know is taken to be one it moves into. A call's
 * arguments are the call's to decide (keeps_arg). */
static Use child_use(ASTNode* p, int i) {
    switch (p->type) {
        case AST_VARIABLE_DECLARATION: case AST_RETURN_STATEMENT: case AST_FIELD_INIT:
        case AST_TUPLE_LITERAL: case AST_ARRAY_LITERAL: case AST_MESSAGE_CONSTRUCTOR:
        case AST_REPLY_STATEMENT: case AST_STRUCT_LITERAL:
            return C_MOVE;
        case AST_ASSIGNMENT:
            return i == p->child_count - 1 ? C_MOVE : C_BORROW;
        case AST_BINARY_EXPRESSION:
            return p->value && strcmp(p->value, "=") == 0 && i == 1 ? C_MOVE : C_BORROW;
        case AST_IF_EXPRESSION:
            return i == 0 ? C_BORROW : C_MOVE;
        case AST_MEMBER_ACCESS: case AST_ARRAY_ACCESS: case AST_SLICE_EXPR:
        case AST_UNARY_EXPRESSION: case AST_PRINT_STATEMENT: case AST_STRING_INTERP:
        case AST_IF_STATEMENT: case AST_WHILE_LOOP: case AST_FOR_LOOP: case AST_MATCH_STATEMENT:
        case AST_SWITCH_STATEMENT: case AST_CASE_STATEMENT: case AST_EXPRESSION_STATEMENT:
        case AST_BLOCK: case AST_PTR_AS_ARRAY_CAST: case AST_VALUE_CAST: case AST_SLICE_TO_PTR:
        case AST_SLICE_FROM_ARRAY: case AST_SIZEOF: case AST_DEFER_STATEMENT:
        case AST_MATCH_ARM: case AST_GUARD_CLAUSE: case AST_PANIC_STATEMENT:
        case AST_TRY_STATEMENT: case AST_CATCH_CLAUSE: case AST_REQUIRES_CLAUSE:
        case AST_ENSURES_CLAUSE: case AST_PTR_AS_STRUCT_CAST: case AST_WHEN_STATEMENT:
            return C_BORROW;
        default:
            return C_MOVE;
    }
}

static ASTNode* call_def(Ctx* c, ASTNode* call) {
    if (!call->value) return NULL;
    ASTNode* d = (ASTNode*)strmap_get(&c->fns, call->value);
    if (!d && strchr(call->value, '.')) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", call->value);
        for (char* q = buf; *q; q++)
            if (*q == '.') *q = '_';
        d = (ASTNode*)strmap_get(&c->fns, buf);
    }
    return d;
}

/* Does argument `i` of `call` go to a slice parameter declared `@keep`, of
 * the function the call names or of the function type it calls through? */
static int keeps_arg(Ctx* c, ASTNode* call, int i) {
    if (!call->value) return 0;
    ASTNode* a = call->children[i];
    if (a && a->type == AST_NAMED_ARG && a->child_count > 0) a = a->children[0];
    if (!a || !type_is_slice(a->node_type)) return 0;
    if (strcmp(call->value, "call") == 0 && call->child_count > 0 && call->children[0] &&
        call->children[0]->node_type && call->children[0]->node_type->kind == TYPE_FUNCTION)
        return i > 0 && fn_type_param_mode(call->children[0]->node_type, i - 1) == FN_MODE_KEEP;
    ASTNode* d = call_def(c, call);
    if (!d) return 0;
    int k = 0;
    for (int j = 0; j < d->child_count; j++) {
        ASTNode* p = d->children[j];
        if (!p || p->type == AST_BLOCK) continue;
        if (p->type != AST_PATTERN_VARIABLE && p->type != AST_VARIABLE_DECLARATION) continue;
        if (k++ == i) return (has_marker(p, "keep_param") || has_marker(p, "frees_param")) && type_is_slice(p->node_type);
    }
    return 0;
}

/* ---- the move check: a moved slice is not used again ---- */

typedef enum { S_LIVE = 0, S_MOVED = 1, S_MAYBE = 2 } St;

typedef struct {
    unsigned char* st;   /* per local */
    ASTNode** at;        /* where it was moved */
    int dead;            /* no path reaches here */
} State;

typedef struct Loop {
    State brk;
    State cont;
    struct Loop* up;
} Loop;

typedef struct {
    Ctx* c;
    Frame* f;
    int n;
    Loop* loop;
} Flow;

static void st_init(Flow* w, State* s) {
    int n = w->n ? w->n : 1;
    s->st = (unsigned char*)calloc((size_t)n, 1);
    s->at = (ASTNode**)calloc((size_t)n, sizeof(ASTNode*));
    s->dead = 0;
}

static void st_copy(Flow* w, State* d, const State* s) {
    memcpy(d->st, s->st, (size_t)w->n);
    memcpy(d->at, s->at, (size_t)w->n * sizeof(ASTNode*));
    d->dead = s->dead;
}

static State st_clone(Flow* w, const State* s) {
    State d;
    st_init(w, &d);
    st_copy(w, &d, s);
    return d;
}

static void st_free(State* s) {
    free(s->st);
    free(s->at);
}

/* `d` holds what either may hold. */
static void st_join(Flow* w, State* d, const State* s) {
    if (s->dead) return;
    if (d->dead) {
        st_copy(w, d, s);
        return;
    }
    for (int i = 0; i < w->n; i++) {
        if (d->st[i] == s->st[i]) continue;
        if (d->st[i] == S_LIVE) d->at[i] = s->at[i];
        d->st[i] = S_MAYBE;
    }
}

static void flow(Flow* w, ASTNode* n, State* s, Use use);

static void moved_message(Flow* w, Local* l, ASTNode* use, State* s) {
    ASTNode* at = s->at[l->idx];
    char msg[512], hint[400], howbuf[240];
    const char* how = "it was moved";
    if (at && is_free(at)) how = "it was freed";
    else if (at && is_append(w->c, at)) how = "`append` consumed it";
    else if (at && at->type == AST_FUNCTION_CALL && at->value) {
        snprintf(howbuf, sizeof(howbuf), "it was given to '%s', which keeps it", at->value);
        how = howbuf;
    }
    snprintf(msg, sizeof(msg), "'%s' is used after %s%s (line %d): an owned slice has one owner",
             l->name, s->st[l->idx] == S_MAYBE ? "a path where " : "", how, node_line(at));
    if (at && is_append(w->c, at))
        snprintf(hint, sizeof(hint),
                 "use the slice `append` returned (`%s = append(%s, v)`), or grow it in place "
                 "with `%s.push(v)`",
                 l->name, l->name, l->name);
    else
        snprintf(hint, sizeof(hint),
                 "use the slice that holds it now, take a view first (`v = %s[..]`), or bind "
                 "'%s' again before this",
                 l->name, l->name);
    report(w->c, w->f, use, AETHER_ERR_USE_AFTER_MOVE, msg, hint);
}

static void use_local(Flow* w, ASTNode* id, State* s) {
    Local* l = pm_get(&w->f->uses, id);
    if (!owns(l) || s->dead) return;
    if (s->st[l->idx] != S_LIVE) moved_message(w, l, id, s);
}

static void move_local(Flow* w, ASTNode* id, ASTNode* at, State* s) {
    Local* l = pm_get(&w->f->uses, id);
    if (!owns(l) || s->dead) return;
    use_local(w, id, s);
    s->st[l->idx] = S_MOVED;
    s->at[l->idx] = at;
}

/* `free`, `append` and `push` release or grow a buffer: a parameter's is
 * the caller's (E0901), as a string parameter's count is. */
static void borrowed_param_error(Flow* w, ASTNode* id, const char* what) {
    Local* l = pm_get(&w->f->uses, id);
    if (!l || l->kind != K_PARAM) return;
    char msg[400], hint[400];
    snprintf(msg, sizeof(msg),
             "cannot %s '%s': it is a parameter, which borrows the caller's slice", what,
             l->name);
    snprintf(hint, sizeof(hint),
             "declare it `%s: @keep %s` to take the caller's buffer (the caller gives it up), "
             "or copy the elements into a buffer of the function's own",
             l->name, type_spelled(l->type));
    report(w->c, w->f, id, AETHER_ERR_FREES_BORROWED, msg, hint);
}

static void flow_value(Flow* w, ASTNode* e, State* s, Use use) {
    if (!e) return;
    if (e->type == AST_IDENTIFIER) {
        if (use == C_MOVE && type_is_slice(e->node_type)) move_local(w, e, e, s);
        else use_local(w, e, s);
        return;
    }
    flow(w, e, s, use);
}

static void flow_call(Flow* w, ASTNode* n, State* s) {
    Ctx* c = w->c;
    if (is_append(c, n) || is_push(n)) {
        /* `v` is read before the slice grows. */
        flow_value(w, n->children[1], s, C_MOVE);
        ASTNode* xs = n->children[0];
        if (xs && xs->type == AST_IDENTIFIER) {
            borrowed_param_error(w, xs, is_push(n) ? "push to" : "append to");
            if (is_push(n)) use_local(w, xs, s);
            else move_local(w, xs, n, s);
        } else {
            flow(w, xs, s, C_BORROW);
        }
        return;
    }
    if (is_free(n)) {
        ASTNode* xs = n->children[0];
        if (xs && xs->type == AST_IDENTIFIER) {
            borrowed_param_error(w, xs, "free");
            move_local(w, xs, n, s);
        } else {
            flow(w, xs, s, C_BORROW);
        }
        return;
    }
    for (int i = 0; i < n->child_count; i++) {
        ASTNode* a = n->children[i];
        if (a && a->type == AST_NAMED_ARG && a->child_count > 0) a = a->children[0];
        if (keeps_arg(c, n, i) && a && a->type == AST_IDENTIFIER) {
            Local* l = pm_get(&w->f->uses, a);
            if (l && l->kind == K_PARAM) {
                char msg[400], hint[300];
                snprintf(msg, sizeof(msg),
                         "cannot give '%s' to '%s', which keeps it (`@keep`): '%s' is a "
                         "parameter, which borrows the caller's slice",
                         l->name, n->value, l->name);
                snprintf(hint, sizeof(hint),
                         "declare it `%s: @keep %s` too, so the buffer passes through",
                         l->name, type_spelled(l->type));
                report(c, w->f, a, AETHER_ERR_FREES_BORROWED, msg, hint);
            }
            move_local(w, a, n, s);
        } else {
            flow_value(w, a, s, C_BORROW);
        }
    }
}

static void flow_loop(Flow* w, ASTNode* cond, ASTNode* body, ASTNode* step, State* s) {
    Loop lp;
    st_init(w, &lp.brk);
    st_init(w, &lp.cont);
    lp.brk.dead = lp.cont.dead = 1;
    lp.up = w->loop;
    w->loop = &lp;
    /* Twice: the second pass starts from what any pass may leave, so a
     * move late in the body is seen by a use early in it. */
    State head = st_clone(w, s);
    State exits;
    st_init(w, &exits);
    exits.dead = 1;
    for (int pass = 0; pass < 2; pass++) {
        State cur = st_clone(w, &head);
        if (cond) flow_value(w, cond, &cur, C_BORROW);
        st_join(w, &exits, &cur);
        flow(w, body, &cur, C_BORROW);
        st_join(w, &cur, &lp.cont);
        if (step) flow(w, step, &cur, C_BORROW);
        st_join(w, &head, &cur);
        st_free(&cur);
    }
    st_join(w, &exits, &lp.brk);
    if (!cond) st_join(w, &exits, &head);
    st_copy(w, s, &exits);
    w->loop = lp.up;
    st_free(&head);
    st_free(&exits);
    st_free(&lp.brk);
    st_free(&lp.cont);
}

static void flow(Flow* w, ASTNode* n, State* s, Use use) {
    if (!n || is_frame(n) || is_opaque(n)) return;
    switch (n->type) {
        case AST_DEFER_STATEMENT:
            return;   /* it runs at the scope's exit */
        case AST_IDENTIFIER:
            flow_value(w, n, s, use);
            return;
        case AST_VARIABLE_DECLARATION: {
            if (n->child_count > 0) flow_value(w, n->children[0], s, C_MOVE);
            Local* l = pm_get(&w->f->binds, n);
            if (l && !s->dead) s->st[l->idx] = S_LIVE;
            return;
        }
        case AST_FUNCTION_CALL:
            flow_call(w, n, s);
            return;
        case AST_RETURN_STATEMENT:
            for (int i = 0; i < n->child_count; i++) flow_value(w, n->children[i], s, C_MOVE);
            s->dead = 1;
            return;
        case AST_PANIC_STATEMENT:
            for (int i = 0; i < n->child_count; i++) flow_value(w, n->children[i], s, C_BORROW);
            s->dead = 1;
            return;
        case AST_BREAK_STATEMENT:
            if (w->loop) st_join(w, &w->loop->brk, s);
            s->dead = 1;
            return;
        case AST_CONTINUE_STATEMENT:
            if (w->loop) st_join(w, &w->loop->cont, s);
            s->dead = 1;
            return;
        case AST_IF_STATEMENT: case AST_IF_EXPRESSION: {
            if (n->child_count == 0) return;
            flow_value(w, n->children[0], s, C_BORROW);
            Use bu = n->type == AST_IF_EXPRESSION ? use : C_BORROW;
            State other = st_clone(w, s);
            if (n->child_count > 1) flow_value(w, n->children[1], s, bu);
            if (n->child_count > 2) flow_value(w, n->children[2], &other, bu);
            st_join(w, s, &other);
            st_free(&other);
            return;
        }
        case AST_MATCH_STATEMENT: case AST_SWITCH_STATEMENT: {
            if (n->child_count == 0) return;
            flow_value(w, n->children[0], s, C_BORROW);
            State entry = st_clone(w, s);
            State out;
            st_init(w, &out);
            out.dead = 1;
            for (int i = 1; i < n->child_count; i++) {
                State a = st_clone(w, &entry);
                flow(w, n->children[i], &a, use);
                st_join(w, &out, &a);
                st_free(&a);
            }
            /* A value no arm covers leaves the statement as it came. */
            st_join(w, &out, &entry);
            st_copy(w, s, &out);
            st_free(&entry);
            st_free(&out);
            return;
        }
        case AST_WHILE_LOOP:
            flow_loop(w, n->child_count > 0 ? n->children[0] : NULL,
                      n->child_count > 1 ? n->children[1] : NULL, NULL, s);
            return;
        case AST_FOR_LOOP:
            if (n->child_count >= 4) {
                flow(w, n->children[0], s, C_BORROW);
                flow_loop(w, n->children[1], n->children[3], n->children[2], s);
                return;
            }
            break;
        case AST_TRY_STATEMENT: {
            State entry = st_clone(w, s);
            ASTNode* handler = NULL;
            for (int i = 0; i < n->child_count; i++) {
                if (n->children[i] && n->children[i]->type == AST_CATCH_CLAUSE)
                    handler = n->children[i];
                else
                    flow(w, n->children[i], s, C_BORROW);
            }
            if (handler) {
                /* The handler starts from before or after any statement of
                 * the body. */
                State h = st_clone(w, &entry);
                st_join(w, &h, s);
                h.dead = 0;
                flow(w, handler, &h, C_BORROW);
                st_join(w, s, &h);
                st_free(&h);
            }
            st_free(&entry);
            return;
        }
        default:
            break;
    }
    for (int i = 0; i < n->child_count; i++) {
        ASTNode* ch = n->children[i];
        if (ch) flow_value(w, ch, s, child_use(n, i));
    }
}

/* ---- the rewrite: moves, loans, drops ---- */

static ASTNode* mk_ident(Local* l, ASTNode* at) {
    ASTNode* id = create_ast_node(AST_IDENTIFIER, l->name, at ? at->line : 0, at ? at->column : 0);
    id->node_type = clone_type(l->type);
    if (at && at->source_file) id->source_file = strdup(at->source_file);
    return id;
}

static ASTNode* mk_call(const char* name, Type* t, ASTNode* at) {
    ASTNode* c = create_ast_node(AST_FUNCTION_CALL, name, at ? at->line : 0, at ? at->column : 0);
    c->node_type = t ? clone_type(t) : create_type(TYPE_VOID);
    if (at && at->source_file) c->source_file = strdup(at->source_file);
    return c;
}

/* *slot becomes `name(*slot)`, of *slot's type. */
static void wrap(ASTNode** slot, const char* name) {
    ASTNode* e = *slot;
    ASTNode* c = mk_call(name, e->node_type, e);
    add_child(c, e);
    *slot = c;
}

static void rewrite(Ctx* c, Frame* f, ASTNode** slot);

/* A slice moved into a place: taken out of a local that owns it (which is
 * left empty), lent out of any other place (a parameter, a field, an
 * element, a module-level `var`, a local left to the program), so the copy
 * never frees what that place holds. */
static void rewrite_value_at(Ctx* c, Frame* f, ASTNode** slot, Use use, int ret) {
    ASTNode* e = *slot;
    if (!e) return;
    if (use != C_MOVE || !type_is_slice(e->node_type)) {
        rewrite(c, f, slot);
        return;
    }
    switch (e->type) {
        case AST_IDENTIFIER: {
            Local* l = pm_get(&f->uses, e);
            if (owns(l)) wrap(slot, "__slice_take");
            else if (l && l->kind == K_VIEW) return;
            /* Returned, a buffer left to the program goes to the caller as
             * it did before ownership, unless a view of it may outlive the
             * call. */
            else if (ret && l && l->kind == K_LOCALVIEW) wrap(slot, "__slice_take");
            else if (ret == 1 && l && l->kind == K_OTHER && !l->pinned) return;
            /* A field, a tuple part or a message field holds what it is
             * given, as before ownership, and the program frees it: the
             * place it came from is kept (the escape analysis), so it is
             * not lent. */
            else if (ret == 2) return;
            else wrap(slot, "__slice_lend");
            return;
        }
        case AST_MEMBER_ACCESS: case AST_ARRAY_ACCESS:
            rewrite(c, f, slot);
            if (ret != 2) wrap(slot, "__slice_lend");
            return;
        default:
            rewrite(c, f, slot);
            return;
    }
}

static void rewrite_value(Ctx* c, Frame* f, ASTNode** slot, Use use) {
    rewrite_value_at(c, f, slot, use, 0);
}

static Summary* summary_of(Ctx* c, ASTNode* def);
static int may_view(Type* t);
static int unknown_callee(ASTNode* call);

/* #3549: is `a`, argument `i` of `call`, a buffer made for the call and
 * only lent to it? `a` is a call's slice, which may own the buffer it
 * holds (`make`, `append`, a function's result, as a local bound to it
 * would), given to a parameter that does not keep it (`@keep`) and that
 * the callee's Summary says it neither stores nor hands a view of back.
 * The call then drops it once it returns (codegen's argument temporaries,
 * through the `slice_temp` marker), as the owning local it would otherwise
 * be bound to is dropped: a view or a buffer the program did not make is
 * left alone by the drop. A call through a closure value is not seen
 * through, and neither is an extern's or a builtin's result that may view
 * memory (a slice, a pointer), nor a call whose positions do not map onto
 * the parameters one for one (a builder's `_ctx`, named arguments). */
static int slice_temp_arg(Ctx* c, ASTNode* call, int i) {
    ASTNode* a = call->children[i];
    if (!a || a->type != AST_FUNCTION_CALL || !type_is_slice(a->node_type) || !a->value ||
        strncmp(a->value, "__slice_", 8) == 0)
        return 0;
    if (unknown_callee(call) || keeps_arg(c, call, i)) return 0;
    for (int k = 0; k < call->child_count; k++)
        if (call->children[k] && call->children[k]->type == AST_NAMED_ARG) return 0;
    ASTNode* d = call_def(c, call);
    if (!d) {
        Type* t = call->node_type;
        return t && t->kind != TYPE_PTR && !may_view(t);
    }
    for (int j = 0; j < d->child_count; j++) {
        ASTNode* p = d->children[j];
        if (p && (p->type == AST_PATTERN_VARIABLE || p->type == AST_VARIABLE_DECLARATION)) {
            if (p->value && strcmp(p->value, "_ctx") == 0) return 0;
            break;
        }
    }
    Summary* sm = summary_of(c, d);
    return sm && sm->n == call->child_count && !sm->res[i] && !sm->res_deep[i] && !sm->store[i];
}

/* Mark each buffer of `n` made for a call and only lent to it
 * (slice_temp_arg). A nested frame is marked by its own walk. */
static void mark_slice_temps(Ctx* c, ASTNode* n) {
    if (!n || is_frame(n) || is_opaque(n)) return;
    if (n->type == AST_FUNCTION_CALL && !is_intrinsic(n) && !is_append(c, n) && !is_push(n) &&
        !is_free(n)) {
        for (int i = 0; i < n->child_count; i++)
            if (slice_temp_arg(c, n, i))
                n->children[i]->annotation =
                    annotation_add_marker(n->children[i]->annotation, "slice_temp");
    }
    for (int i = 0; i < n->child_count; i++) mark_slice_temps(c, n->children[i]);
}

static void rewrite_call(Ctx* c, Frame* f, ASTNode* n) {
    if (is_intrinsic(n)) return;
    if (is_append(c, n) || is_push(n) || is_free(n)) {
        if (n->child_count > 1) rewrite_value(c, f, &n->children[1], C_MOVE);
        ASTNode** xs = &n->children[0];
        rewrite(c, f, xs);
        Local* l = (*xs)->type == AST_IDENTIFIER ? pm_get(&f->uses, *xs) : NULL;
        if (is_push(n)) return;
        if (is_free(n)) {
            /* The place `free` releases is left empty, so nothing frees it
             * again. */
            if (pure_place(*xs)) wrap(xs, "__slice_take");
            return;
        }
        /* `append` reads `v` before it grows the slice, so the local it
         * consumes is left empty there, by codegen, not by a `__slice_take`
         * argument the operand-order rule (#2765) would evaluate first. */
        if (owns(l) && !annotation_has_marker(n->annotation, "slice_consumes"))
            n->annotation = annotation_add_marker(n->annotation, "slice_consumes");
        return;
    }
    for (int i = 0; i < n->child_count; i++) {
        ASTNode** slot = &n->children[i];
        if (*slot && (*slot)->type == AST_NAMED_ARG && (*slot)->child_count > 0)
            slot = &(*slot)->children[0];
        if (!*slot) continue;
        rewrite(c, f, slot);
        /* Given to a `@keep` parameter: the place gives it up. */
        if (keeps_arg(c, n, i) && pure_place(*slot)) wrap(slot, "__slice_take");
    }
}

/* `defer __slice_drop(x)`: the owning local `l` is dropped on every exit of
 * the scope it is declared in. */
static ASTNode* mk_drop_defer(Local* l, ASTNode* at) {
    ASTNode* call = mk_call("__slice_drop", NULL, at);
    add_child(call, mk_ident(l, at));
    ASTNode* es = create_ast_node(AST_EXPRESSION_STATEMENT, NULL, call->line, call->column);
    add_child(es, call);
    ASTNode* d = create_ast_node(AST_DEFER_STATEMENT, NULL, call->line, call->column);
    add_child(d, es);
    if (at && at->source_file) {
        es->source_file = strdup(at->source_file);
        d->source_file = strdup(at->source_file);
    }
    return d;
}

/* `T[] x`, the declaration codegen would hoist ahead of the statement that
 * binds `x` (hoist.h), written where it is so the drop can follow it. */
static ASTNode* mk_hoisted_decl(Local* l, ASTNode* at) {
    ASTNode* d = create_ast_node(AST_VARIABLE_DECLARATION, l->name, at ? at->line : 0,
                                 at ? at->column : 0);
    d->node_type = clone_type(l->type);
    d->type_inferred = 0;
    if (at && at->source_file) d->source_file = strdup(at->source_file);
    return d;
}

static void insert_child(ASTNode* block, int at, ASTNode* child) {
    add_child(block, child);
    for (int i = block->child_count - 1; i > at; i--) block->children[i] = block->children[i - 1];
    block->children[at] = child;
}

static void rewrite_binding(Ctx* c, Frame* f, ASTNode* n) {
    if (n->child_count > 0) rewrite_value(c, f, &n->children[0], C_MOVE);
    Local* l = pm_get(&f->binds, n);
    if (!owns(l) || n == l->decl || n->child_count == 0) return;
    /* A store into a local that owns: what it held is dropped, unless the
     * value is `append(x, v)` of that very local, which consumed it. */
    ASTNode* v = n->children[0];
    if (is_append(c, v) && annotation_has_marker(v->annotation, "slice_consumes") &&
        pm_get(&f->uses, v->children[0]) == l)
        return;
    ASTNode* set = mk_call("__slice_set", l->type, v);
    add_child(set, mk_ident(l, n));
    add_child(set, v);
    n->children[0] = set;
}

static void rewrite(Ctx* c, Frame* f, ASTNode** slot) {
    ASTNode* n = *slot;
    if (!n || is_frame(n) || is_opaque(n)) return;
    switch (n->type) {
        case AST_BLOCK:
            /* A hoisted local that owns is declared here, where codegen
             * would declare it, and dropped from there on. */
            for (int k = f->nlocals - 1; k >= 0; k--) {
                Local* h = f->locals[k];
                if (h->hoisted && h->kind == K_OWNER && h->block == n && !h->anchor) {
                    insert_child(n, 0, mk_drop_defer(h, n));
                    insert_child(n, 0, mk_hoisted_decl(h, n));
                }
            }
            for (int i = 0; i < n->child_count; i++) {
                ASTNode* ch = n->children[i];
                for (int k = 0; k < f->nlocals; k++) {
                    Local* h = f->locals[k];
                    if (h->hoisted && h->kind == K_OWNER && h->block == n && h->anchor == ch) {
                        insert_child(n, i, mk_drop_defer(h, ch));
                        insert_child(n, i, mk_hoisted_decl(h, ch));
                        i += 2;
                    }
                }
                rewrite(c, f, &n->children[i]);
                Local* l = ch ? pm_get(&f->binds, ch) : NULL;
                if (l && l->kind == K_OWNER && l->decl == ch && l->block == n) {
                    insert_child(n, i + 1, mk_drop_defer(l, ch));
                    i++;
                }
            }
            return;
        case AST_VARIABLE_DECLARATION:
            rewrite_binding(c, f, n);
            return;
        case AST_FUNCTION_CALL:
            rewrite_call(c, f, n);
            return;
        case AST_RETURN_STATEMENT:
            for (int i = 0; i < n->child_count; i++)
                if (n->children[i]) rewrite_value_at(c, f, &n->children[i], C_MOVE, 1);
            return;
        case AST_STRUCT_LITERAL: case AST_TUPLE_LITERAL: case AST_MESSAGE_CONSTRUCTOR:
            for (int i = 0; i < n->child_count; i++) {
                ASTNode** slot2 = &n->children[i];
                if (!*slot2) continue;
                if (((*slot2)->type == AST_ASSIGNMENT || (*slot2)->type == AST_FIELD_INIT) &&
                    (*slot2)->child_count > 0)
                    slot2 = &(*slot2)->children[(*slot2)->child_count - 1];
                rewrite_value_at(c, f, slot2, C_MOVE, 2);
            }
            return;
        case AST_BINARY_EXPRESSION:
            if (n->value && strcmp(n->value, "=") == 0 && n->child_count == 2) {
                rewrite(c, f, &n->children[0]);
                rewrite_value_at(c, f, &n->children[1], C_MOVE,
                                 n->children[0]->type == AST_MEMBER_ACCESS ? 2 : 0);
                /* An element of a slice of slices owns its slice: the store
                 * drops the one it replaces. */
                ASTNode* lhs = n->children[0];
                ASTNode* base = lhs;
                while (base->type == AST_ARRAY_ACCESS && base->child_count > 0) base = base->children[0];
                if (lhs->type == AST_ARRAY_ACCESS && type_is_slice(lhs->node_type) &&
                    base->type == AST_IDENTIFIER &&
                    (owns(pm_get(&f->uses, base)) || pm_get(&f->row_ok, n)) &&
                    lhs->child_count == 2 && type_is_slice(lhs->children[0]->node_type) &&
                    pure_place(lhs)) {
                    ASTNode* v = n->children[1];
                    ASTNode* set = mk_call("__slice_set", lhs->node_type, v);
                    add_child(set, clone_ast_node(lhs));
                    add_child(set, v);
                    n->children[1] = set;
                }
                return;
            }
            break;
        default:
            break;
    }
    for (int i = 0; i < n->child_count; i++)
        if (n->children[i]) rewrite_value(c, f, &n->children[i], child_use(n, i));
}

/* ---- escape analysis: where a view of a buffer may go ----
 *
 * A local owns its buffer only while nothing that outlives it views that
 * buffer. Before ownership nothing was released, so a program could keep a
 * view of a buffer anywhere: in a struct a function returns, through a
 * parameter that stores what it is given, in a local of an outer scope.
 * Each slice-valued expression has roots, the locals whose buffers it may
 * view (a parameter: the caller's). A view of an owner's buffer that may
 * leave the function pins the owner (K_PINNED: never released, every copy a
 * loan); one bound outside the owner's scope, or held while the owner is
 * re-bound, moved, freed, grown or has a row replaced, leaves it to the
 * program (K_LOCALVIEW). Either is the program's memory as it was before
 * ownership, so code that was valid stays valid. A parameter whose view may
 * be stored is noted in the function's Summary, and its callers treat what
 * they pass there as viewed for good; one whose view may be returned makes
 * the call's result a view of the argument. */

static Summary* summary_of(Ctx* c, ASTNode* def) {
    if (!def) return NULL;
    Summary* sm = (Summary*)(void*)pm_get(&c->sums, def);
    if (sm) return sm;
    int n = 0;
    for (int j = 0; j < def->child_count; j++) {
        ASTNode* p = def->children[j];
        if (p && (p->type == AST_PATTERN_VARIABLE || p->type == AST_VARIABLE_DECLARATION)) n++;
    }
    sm = (Summary*)calloc(1, sizeof(Summary));
    sm->n = n;
    sm->res = (unsigned char*)calloc((size_t)(n ? n : 1), 1);
    sm->res_deep = (unsigned char*)calloc((size_t)(n ? n : 1), 1);
    sm->store = (unsigned char*)calloc((size_t)(n ? n : 1), 1);
    pm_put(&c->sums, def, (Local*)(void*)sm);
    return sm;
}

static int may_view(Type* t) {
    if (!t) return 1;
    switch (t->kind) {
        case TYPE_ARRAY: case TYPE_STRUCT: case TYPE_TUPLE: case TYPE_OPTIONAL:
        case TYPE_SUM: case TYPE_FUNCTION: case TYPE_UNKNOWN: case TYPE_WILDCARD:
            return 1;
        default:
            return 0;
    }
}

static int owner_ish(const Local* l) {
    return l && (l->kind == K_OWNER || l->kind == K_KEEP);
}

typedef struct {
    ASTNode* store;
    Local* base;
    int seq;
} RowStore;

/* A root set has a bit for each local (a view of its buffer) and, after
 * them, one for each parameter's elements (what the caller's buffer holds,
 * copied out: a struct read from it, not a view of it). */
typedef struct {
    Ctx* c;
    Frame* f;
    int n;      /* bits in a root set: 2 * nl */
    int nl;     /* the frame's locals */
    int grew;
    /* Points of the walk, in the order the source runs straight through:
     * a view taken in a loop counts from the start of the outermost loop it
     * is in, since a later pass still holds it. */
    int seq;
    int loop_start;
    RowStore* rows;
    int nrows, rows_cap;
} Esc;

static void note_view(Esc* e, Local* x) {
    int at = e->loop_start >= 0 ? e->loop_start : e->seq;
    if (at < x->first_view) x->first_view = at;
}

static void note_mut(Esc* e, Local* x) {
    x->mutated = 1;
    if (e->seq > x->last_mut) x->last_mut = e->seq;
}

static void set_or(Esc* e, unsigned char* d, const unsigned char* s) {
    for (int i = 0; i < e->n; i++) d[i] |= s[i];
}

static void roots_of(Esc* e, ASTNode* x, unsigned char* out);

/* The roots of `x` given where it is only viewed (an argument, a sub-slice's
 * base): a local's own buffer among them. */
static void view_roots(Esc* e, ASTNode* x, unsigned char* out) {
    if (x && x->type == AST_NAMED_ARG && x->child_count > 0) x = x->children[0];
    if (x && x->type == AST_IDENTIFIER) {
        Local* l = pm_get(&e->f->uses, x);
        if (l) {
            out[l->idx] = 1;
            set_or(e, out, l->roots);
        }
        return;
    }
    roots_of(e, x, out);
}

/* The roots of a value copied out of `x`'s elements (a struct, a tuple, a
 * number): what they hold, not `x`'s buffer. */
static void deep_roots(Esc* e, ASTNode* x, unsigned char* out) {
    if (x && x->type == AST_NAMED_ARG && x->child_count > 0) x = x->children[0];
    if (x && x->type == AST_IDENTIFIER) {
        Local* l = pm_get(&e->f->uses, x);
        if (!l) return;
        if (l->param) out[e->nl + l->idx] = 1;
        else if (!owner_ish(l)) out[l->idx] = 1;
        set_or(e, out, l->roots);
        return;
    }
    roots_of(e, x, out);
}

static void frame_names_roots(Esc* e, ASTNode* n, unsigned char* out) {
    if (!n) return;
    if (n->type == AST_IDENTIFIER && n->value) {
        for (int i = 0; i < e->f->nlocals; i++) {
            Local* l = e->f->locals[i];
            if (strcmp(l->name, n->value) == 0) {
                out[l->idx] = 1;
                set_or(e, out, l->roots);
            }
        }
    }
    for (int i = 0; i < n->child_count; i++) frame_names_roots(e, n->children[i], out);
}

/* Is `call` one whose callee the analysis cannot see (a closure value)? */
static int unknown_callee(ASTNode* call) {
    return call->value && strcmp(call->value, "call") == 0;
}

static void roots_of(Esc* e, ASTNode* x, unsigned char* out) {
    if (!x || is_opaque(x)) return;
    if (x->type == AST_CLOSURE) {
        frame_names_roots(e, x, out);
        return;
    }
    if (is_frame(x)) return;
    if (x->node_type && !may_view(x->node_type) && x->type != AST_FUNCTION_CALL) return;
    switch (x->type) {
        case AST_IDENTIFIER: {
            Local* l = pm_get(&e->f->uses, x);
            if (!l) return;
            /* An owner's buffer moves with it; what it holds goes along. */
            if (!owner_ish(l)) out[l->idx] = 1;
            set_or(e, out, l->roots);
            return;
        }
        case AST_SLICE_EXPR:
            if (x->child_count > 0) view_roots(e, x->children[0], out);
            return;
        case AST_MEMBER_ACCESS: case AST_ARRAY_ACCESS:
            /* A row of a slice of slices, or a slice field, is a view into
             * memory its holder owns; a struct copied out holds only what
             * the element holds. */
            if (x->child_count == 0) return;
            if (type_is_slice(x->node_type)) view_roots(e, x->children[0], out);
            else deep_roots(e, x->children[0], out);
            return;
        case AST_FUNCTION_CALL: {
            if (is_append(e->c, x)) {
                roots_of(e, x->children[0], out);
                roots_of(e, x->children[1], out);
                return;
            }
            if (is_free(x) || is_push(x) || is_call(x, "make")) return;
            if (unknown_callee(x)) {
                for (int i = 0; i < x->child_count; i++) view_roots(e, x->children[i], out);
                return;
            }
            ASTNode* d = call_def(e->c, x);
            if (!d) return;   /* an extern or a builtin keeps nothing */
            Summary* sm = summary_of(e->c, d);
            for (int i = 0; i < x->child_count && i < sm->n; i++) {
                if (sm->res[i]) view_roots(e, x->children[i], out);
                else if (sm->res_deep[i]) deep_roots(e, x->children[i], out);
            }
            return;
        }
        case AST_SLICE_FROM_ARRAY: case AST_PTR_AS_ARRAY_CAST: case AST_LITERAL:
        case AST_NULL_LITERAL:
            return;
        default:
            for (int i = 0; i < x->child_count; i++) {
                if (x->type == AST_IF_EXPRESSION && i == 0) continue;
                roots_of(e, x->children[i], out);
            }
            return;
    }
}

/* The local whose value a store into `lhs` writes into, when that is a
 * value of the frame (a struct, a tuple, an array, a buffer it holds), or
 * NULL when the store reaches memory that outlives it (a pointer, a
 * module-level `var`, a parameter's pointee). */
static Local* store_base(Esc* e, ASTNode* lhs) {
    ASTNode* b = lhs;
    while (b && (b->type == AST_MEMBER_ACCESS || b->type == AST_ARRAY_ACCESS) && b->child_count > 0)
        b = b->children[0];
    if (!b || b->type != AST_IDENTIFIER) return NULL;
    Local* l = pm_get(&e->f->uses, b);
    if (!l || l->param) return NULL;
    if (l->type && (l->type->kind == TYPE_PTR)) return NULL;
    return l;
}

/* `l` takes what `r` views. Returns 1 when its roots grew. */
static int bind_roots(Esc* e, Local* l, const unsigned char* r) {
    int grew = 0;
    for (int i = 0; i < e->n; i++)
        if (r[i] && !l->roots[i]) {
            l->roots[i] = 1;
            grew = 1;
        }
    return grew;
}

static void roots_pass(Esc* e, ASTNode* n) {
    if (!n || is_opaque(n) || is_frame(n)) return;
    unsigned char* r = (unsigned char*)calloc((size_t)(e->n ? e->n : 1), 1);
    if (n->type == AST_VARIABLE_DECLARATION && n->child_count > 0) {
        Local* l = pm_get(&e->f->binds, n);
        if (l) {
            roots_of(e, n->children[0], r);
            if (bind_roots(e, l, r)) e->grew = 1;
        }
    } else if (n->type == AST_TUPLE_DESTRUCTURE && n->child_count > 0) {
        roots_of(e, n->children[n->child_count - 1], r);
        for (int i = 0; i + 1 < n->child_count; i++) {
            Local* l = pm_get(&e->f->binds, n->children[i]);
            if (l && bind_roots(e, l, r)) e->grew = 1;
        }
    } else if (n->type == AST_BINARY_EXPRESSION && n->value && strcmp(n->value, "=") == 0 &&
               n->child_count == 2) {
        Local* l = store_base(e, n->children[0]);
        if (l) {
            roots_of(e, n->children[1], r);
            if (bind_roots(e, l, r)) e->grew = 1;
        }
    }
    free(r);
    for (int i = 0; i < n->child_count; i++) roots_pass(e, n->children[i]);
}

static void sink_store(Esc* e, const unsigned char* r) {
    Summary* sm = e->f->owner->type == AST_CLOSURE ? NULL : summary_of(e->c, e->f->owner);
    for (int i = 0; i < e->n; i++) {
        if (!r[i]) continue;
        Local* x = e->f->locals[i % e->nl];
        if (x->param && !x->keep) {
            if (sm && x->pidx >= 0 && x->pidx < sm->n && !sm->store[x->pidx]) {
                sm->store[x->pidx] = 1;
                e->c->changed = 1;
            }
        } else {
            x->pinned = 1;
            note_view(e, x);
        }
    }
}

static void sink_return(Esc* e, const unsigned char* r) {
    Summary* sm = e->f->owner->type == AST_CLOSURE ? NULL : summary_of(e->c, e->f->owner);
    for (int i = 0; i < e->n; i++) {
        if (!r[i]) continue;
        Local* x = e->f->locals[i % e->nl];
        if (x->param && !x->keep) {
            unsigned char* bit = !sm || x->pidx < 0 || x->pidx >= sm->n ? NULL
                                 : i < e->nl ? &sm->res[x->pidx] : &sm->res_deep[x->pidx];
            if (bit && !*bit) {
                *bit = 1;
                e->c->changed = 1;
            }
            /* A closure's result goes where its caller puts it. */
            if (!sm) x->pinned = 1;
        } else {
            x->pinned = 1;
            note_view(e, x);
        }
    }
}

/* A binding of `l` to what `r` views. */
static void sink_bind(Esc* e, Local* l, const unsigned char* r) {
    for (int i = 0; i < e->nl; i++) {
        if (!r[i]) continue;
        Local* x = e->f->locals[i];
        if (x == l) {
            x->viewed = 1;
            x->self_view = 1;   /* re-bound to a view of what it held */
            note_view(e, x);
            note_mut(e, x);
            continue;
        }
        x->viewed = 1;
        note_view(e, x);
        if (l->depth < x->depth || l->param) x->scoped_out = 1;
    }
}

static void sinks(Esc* e, ASTNode* n, Use use);

static void sinks_value(Esc* e, ASTNode* x, Use use) {
    if (!x) return;
    if (x->type == AST_IDENTIFIER) {
        Local* l = pm_get(&e->f->uses, x);
        if (l && use == C_MOVE && type_is_slice(x->node_type)) note_mut(e, l);
        return;
    }
    sinks(e, x, use);
}

static void sinks(Esc* e, ASTNode* n, Use use) {
    if (!n || is_opaque(n)) return;
    int nn = e->n ? e->n : 1;
    /* A closure value carries what it captures (roots_of), so it is judged
     * where the value goes, as a struct holding the same views would be. */
    if (is_frame(n)) return;
    e->seq++;
    if ((n->type == AST_WHILE_LOOP || n->type == AST_FOR_LOOP) && e->loop_start < 0) {
        e->loop_start = e->seq;
        for (int i = 0; i < n->child_count; i++)
            if (n->children[i]) sinks_value(e, n->children[i], child_use(n, i));
        e->loop_start = -1;
        return;
    }
    unsigned char* r = (unsigned char*)calloc((size_t)nn, 1);
    switch (n->type) {
        case AST_VARIABLE_DECLARATION: {
            if (n->child_count > 0) {
                Local* l = pm_get(&e->f->binds, n);
                roots_of(e, n->children[0], r);
                if (l) {
                    sink_bind(e, l, r);
                    if (l->decl != n) note_mut(e, l);
                } else {
                    sink_store(e, r);   /* a module-level `var`, a state field */
                }
                sinks_value(e, n->children[0], C_MOVE);
            }
            free(r);
            return;
        }
        case AST_TUPLE_DESTRUCTURE:
            if (n->child_count > 0) {
                roots_of(e, n->children[n->child_count - 1], r);
                for (int i = 0; i + 1 < n->child_count; i++) {
                    Local* l = pm_get(&e->f->binds, n->children[i]);
                    if (l) sink_bind(e, l, r);
                    else sink_store(e, r);
                }
                sinks(e, n->children[n->child_count - 1], C_BORROW);
            }
            free(r);
            return;
        case AST_BINARY_EXPRESSION:
            if (n->value && strcmp(n->value, "=") == 0 && n->child_count == 2) {
                Local* l = store_base(e, n->children[0]);
                roots_of(e, n->children[1], r);
                if (n->children[0]->type == AST_MEMBER_ACCESS) sink_store(e, r);
                if (l) {
                    sink_bind(e, l, r);
                    if (n->children[0]->type == AST_ARRAY_ACCESS &&
                        type_is_slice(n->children[0]->node_type)) {
                        note_mut(e, l);   /* a row replaced */
                        if (e->nrows == e->rows_cap) {
                            e->rows_cap = e->rows_cap ? e->rows_cap * 2 : 8;
                            e->rows = (RowStore*)realloc(e->rows,
                                                         (size_t)e->rows_cap * sizeof(RowStore));
                        }
                        RowStore rs = {n, l, e->seq};
                        e->rows[e->nrows++] = rs;
                    }
                } else {
                    sink_store(e, r);
                }
                sinks(e, n->children[0], C_BORROW);
                sinks_value(e, n->children[1], C_MOVE);
                free(r);
                return;
            }
            break;
        case AST_RETURN_STATEMENT:
            for (int i = 0; i < n->child_count; i++) {
                ASTNode* ch = n->children[i];
                Local* l = ch && ch->type == AST_IDENTIFIER ? pm_get(&e->f->uses, ch) : NULL;
                /* A local returned whole hands its buffer to the caller; what
                 * it holds of others' goes with it. */
                if (l && !l->param) set_or(e, r, l->roots);
                else roots_of(e, ch, r);
                sinks_value(e, ch, C_MOVE);
            }
            sink_return(e, r);
            free(r);
            return;
        case AST_MESSAGE_CONSTRUCTOR: case AST_SEND_FIRE_FORGET: case AST_SEND_ASK:
        case AST_REPLY_STATEMENT: case AST_STRUCT_LITERAL: case AST_TUPLE_LITERAL:
            /* What a field, a tuple part or a message holds the program
             * frees by hand (it is not lent): what it views is kept. */
            roots_of(e, n, r);
            sink_store(e, r);
            break;
        case AST_FUNCTION_CALL: {
            if (is_free(n) || is_append(e->c, n) || is_push(n)) {
                ASTNode* xs = n->children[0];
                Local* l = xs && xs->type == AST_IDENTIFIER ? pm_get(&e->f->uses, xs) : NULL;
                if (l) note_mut(e, l);
                for (int i = 0; i < n->child_count; i++) sinks_value(e, n->children[i], C_BORROW);
                free(r);
                return;
            }
            ASTNode* d = unknown_callee(n) ? NULL : call_def(e->c, n);
            Summary* sm = d ? summary_of(e->c, d) : NULL;
            for (int i = 0; i < n->child_count; i++) {
                ASTNode* a = n->children[i];
                /* The closure a value call calls is no argument of it. */
                int callee = unknown_callee(n) && i == 0;
                if ((unknown_callee(n) && !callee) || (sm && i < sm->n && sm->store[i])) {
                    memset(r, 0, (size_t)nn);
                    view_roots(e, a, r);
                    sink_store(e, r);
                }
                if (keeps_arg(e->c, n, i)) sinks_value(e, a, C_MOVE);
                else sinks_value(e, a, C_BORROW);
            }
            free(r);
            return;
        }
        default:
            break;
    }
    free(r);
    for (int i = 0; i < n->child_count; i++)
        if (n->children[i]) sinks_value(e, n->children[i], child_use(n, i));
}

/* The escape analysis of a frame whose locals are resolved and classified:
 * the roots of every local, the frame's Summary, and the owners a view of
 * whose buffer may outlive them, left to the program. */
static void escape_frame(Ctx* c, Frame* f) {
    Esc e;
    memset(&e, 0, sizeof(e));
    e.c = c;
    e.f = f;
    e.nl = f->nlocals;
    e.n = 2 * f->nlocals;
    e.loop_start = -1;
    int nn = e.n ? e.n : 1;
    for (int i = 0; i < f->nlocals; i++) {
        f->locals[i]->roots = (unsigned char*)calloc((size_t)nn, 1);
        f->locals[i]->first_view = 0x7fffffff;
        f->locals[i]->last_mut = -1;
    }
    for (int pass = 0; pass < 8; pass++) {
        e.grew = 0;
        roots_pass(&e, f->body);
        if (!e.grew) break;
    }
    sinks(&e, f->body, C_BORROW);
    /* A row replaced before any view of its slice is taken (in the order
     * the code runs, a loop's views counted from its start) releases the
     * row it replaces: nothing can view that row. */
    for (int i = 0; i < e.nrows; i++) {
        RowStore* rs = &e.rows[i];
        if (owner_ish(rs->base) && rs->seq < rs->base->first_view)
            pm_put(&f->row_ok, rs->store, rs->base);
    }
    free(e.rows);
    /* Mutated while a view of it may be held: a view taken before the
     * mutation, or in a loop around it, or the local itself re-bound to a
     * view of what it held. A view taken only after every mutation is not. */
    for (int i = 0; i < f->nlocals; i++) {
        Local* l = f->locals[i];
        if (!owner_ish(l)) continue;
        int held = l->viewed && (l->self_view || l->last_mut > l->first_view);
        if (l->pinned) l->kind = K_PINNED;
        else if (l->scoped_out || held) l->kind = K_LOCALVIEW;
    }
}

/* ---- one frame ---- */

static void check_frame(Ctx* c, ASTNode* owner, const StrMap* outer);

static void frame_nested(Ctx* c, ASTNode* n, const StrMap* names) {
    if (!n || is_opaque(n)) return;
    if (is_frame(n)) {
        check_frame(c, n, names);
        return;
    }
    for (int i = 0; i < n->child_count; i++) frame_nested(c, n->children[i], names);
}

static void check_frame(Ctx* c, ASTNode* owner, const StrMap* outer) {
    ASTNode* body = frame_body(owner);
    if (!body) return;
    Frame f;
    memset(&f, 0, sizeof(f));
    f.owner = owner;
    f.body = body;
    f.outer = outer;
    Scope top;
    strmap_init(&top.names);
    top.up = NULL;
    top.depth = 0;
    int npar = 0;
    R r = {c, &f};
    /* The parameters, and a receive arm's pattern. */
    for (int i = 0; i < owner->child_count; i++) {
        ASTNode* p = owner->children[i];
        if (!p || p == body) continue;
        if ((p->type == AST_PATTERN_VARIABLE || p->type == AST_VARIABLE_DECLARATION ||
             p->type == AST_CLOSURE_PARAM) && p->value) {
            Local* l = new_local(&f, &top, p, NULL);
            l->param = 1;
            l->pidx = npar++;
            l->keep = (has_marker(p, "keep_param") || has_marker(p, "frees_param")) && owner->type != AST_CLOSURE;
        } else if (owner->type == AST_RECEIVE_ARM || owner->type == AST_TIMEOUT_ARM) {
            resolve(&r, &top, p, NULL);
        }
    }
    /* A name first bound in an arm of a top-level if, switch or match of a
     * function's body and read after it is declared at the top of the body. */
    if (owner->type == AST_FUNCTION_DEFINITION || owner->type == AST_BUILDER_FUNCTION ||
        owner->type == AST_MAIN_FUNCTION) {
        const HoistNames* hn = hoist_if_branch_hoists(body);
        Hoist h = {&r, &top, body, NULL};
        for (int i = 0; hn && i < hn->count; i++) hoist_one(&h, hoist_names_decl(hn, i));
    }
    resolve(&r, &top, body, NULL);
    classify(&f);
    escape_frame(c, &f);
    if (!c->final) goto done;
    mark_slice_temps(c, body);

    int any = 0;
    for (int i = 0; i < f.nlocals; i++)
        if (f.locals[i]->kind != K_NONE) any = 1;
    for (int i = 0; i < f.uses.cap; i++)
        if (f.uses.keys[i] && owns(f.uses.vals[i]))
            pm_put(&g_owner_idents, f.uses.keys[i], f.uses.vals[i]);
    if (any) {
        int before = c->errors;
        Flow w = {c, &f, f.nlocals, NULL};
        State s;
        st_init(&w, &s);
        flow(&w, body, &s, C_BORROW);
        st_free(&s);
        if (c->errors == before) {
            ASTNode* b = body;
            rewrite(c, &f, &b);
            /* A `@keep` parameter is dropped as a local that owns is. */
            for (int i = f.nlocals - 1; i >= 0; i--)
                if (f.locals[i]->kind == K_KEEP)
                    insert_child(body, 0, mk_drop_defer(f.locals[i], body));
        }
    }

    /* The closures written here, each a frame of its own. */
    StrMap names;
    strmap_init(&names);
    if (outer)
        for (int i = 0; i < strmap_count(outer); i++)
            strmap_put(&names, strmap_key_at(outer, i), (void*)1);
    for (int i = 0; i < f.nlocals; i++) strmap_put(&names, f.locals[i]->name, (void*)1);
    for (int i = 0; i < body->child_count; i++) frame_nested(c, body->children[i], &names);
    strmap_free(&names);

done:
    for (int i = 0; i < f.nlocals; i++) {
        free(f.locals[i]->roots);
        free(f.locals[i]->srcs);
        free(f.locals[i]);
    }
    free(f.locals);
    pm_free(&f.uses);
    pm_free(&f.binds);
    pm_free(&f.row_ok);
    strmap_free(&top.names);
}

static ASTNode* unwrap_export(ASTNode* n) {
    if (n && n->type == AST_EXPORT_STATEMENT && n->child_count > 0) return n->children[0];
    return n;
}

static void walk_program(Ctx* c, ASTNode* n) {
    n = unwrap_export(n);
    if (!n || is_opaque(n)) return;
    if (n->type == AST_ACTOR_DEFINITION) {
        StrMap saved = c->state;
        strmap_init(&c->state);
        for (int i = 0; i < n->child_count; i++) {
            ASTNode* d = n->children[i];
            if (d && d->type == AST_STATE_DECLARATION && d->value)
                strmap_put(&c->state, d->value, d);
        }
        for (int i = 0; i < n->child_count; i++) walk_program(c, n->children[i]);
        strmap_free(&c->state);
        c->state = saved;
        return;
    }
    if (is_frame(n)) {
        check_frame(c, n, NULL);
        return;
    }
    for (int i = 0; i < n->child_count; i++) walk_program(c, n->children[i]);
}

int slice_own_owner_ident(const ASTNode* n) {
    return n && n->type == AST_IDENTIFIER && pm_get(&g_owner_idents, n) != NULL;
}

int check_slice_ownership(ASTNode* program) {
    pm_free(&g_owner_idents);
    if (!program) return 0;
    Ctx c;
    memset(&c, 0, sizeof(c));
    c.program = program;
    strmap_init(&c.globals);
    strmap_init(&c.state);
    strmap_init(&c.fns);
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* d = unwrap_export(program->children[i]);
        if (!d || !d->value) continue;
        if (d->type == AST_CONST_DECLARATION && d->annotation &&
            strcmp(d->annotation, "global_var") == 0)
            strmap_put(&c.globals, d->value, d);
        if (d->type == AST_FUNCTION_DEFINITION || d->type == AST_BUILDER_FUNCTION) {
            if (!strmap_has(&c.fns, d->value)) strmap_put(&c.fns, d->value, d);
            if (strcmp(d->value, "append") == 0) c.has_append = 1;
        }
    }
    /* The summaries first: what each function does with its slice
     * parameters, to a fixed point over the calls between them. */
    for (int pass = 0; pass < 10; pass++) {
        c.changed = 0;
        for (int i = 0; i < program->child_count; i++) walk_program(&c, program->children[i]);
        if (!c.changed) break;
    }
    c.final = 1;
    for (int i = 0; i < program->child_count; i++) walk_program(&c, program->children[i]);
    for (int i = 0; i < c.sums.cap; i++) {
        if (!c.sums.keys[i]) continue;
        Summary* sm = (Summary*)(void*)c.sums.vals[i];
        free(sm->res);
        free(sm->res_deep);
        free(sm->store);
        free(sm);
    }
    pm_free(&c.sums);
    strmap_free(&c.globals);
    strmap_free(&c.state);
    strmap_free(&c.fns);
    pm_free(&c.reported);
    return c.errors;
}
