/* Slice ownership: every place that holds a slice of a counted buffer holds
 * one reference to it, released when the place goes out of scope (#3568).
 * See slice_own.h for the rules. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "slice_own.h"
#include "hoist.h"
#include "slice_coerce.h"
#include "../aether_error.h"
#include "../aether_strmap.h"

/* ---- what a local is ---- */

typedef enum {
    K_NONE,   /* not a slice */
    K_OWNER,  /* holds a reference, which a move takes out (E0906 after):
               * released at every exit of its scope */
    K_VIEW,   /* only ever given views and copies: holds a reference of its
               * own, released at every exit; a copy of it into another
               * place takes another, so it is never moved */
    K_OTHER,  /* bound by a pattern, a destructure, a `catch`, or outside a
               * block: the reference stays with what it was bound from */
    K_PARAM,  /* a parameter: borrows the caller's */
    K_KEEP,   /* a `@keep` parameter: holds the caller's reference, as a
               * local that owns */
    /* A local a closure assigns, so a cell the frame and its closures share
     * (codegen promotes it, by name): the cell holds the reference. Every
     * store into it, in the frame or a closure, gives back what it held;
     * the cell's last release gives back what it holds then (#3549). */
    K_CELL
} Kind;

/* How a slice is used where it stands: borrowed (read, indexed, measured,
 * viewed, compared), moved into a place (bound, returned, stored, put in a
 * struct, a tuple, an array or a message), or C_LEND, given to a place this
 * pass does not know, which a view is lent to and a local that owns moves
 * into, as before ownership. */
typedef enum { C_BORROW, C_MOVE, C_LEND } Use;

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
    int fresh;          /* some binding may give it a buffer of its own */
    int target;         /* `free`, `append` or `push` is applied to it */
    int viewed;         /* a view of what it holds may be taken (views_of) */
    int destructured;   /* declared by a tuple destructure (#3568) */
    ASTNode* arm;       /* bound by the pattern of this match arm, whose body is a block */
    int written;        /* a pattern's binding its scope binds again or pushes into */
    int hold;           /* takes a reference of its own where its body starts (classify) */
    int pidx;           /* a parameter: its position, else -1 */
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

/* ---- the program and the frame being walked ---- */

typedef struct {
    ASTNode* program;
    StrMap globals;     /* module-level `var` names */
    StrMap state;       /* the state fields of the actor being walked */
    StrMap fns;         /* name -> function definition */
    int has_append;     /* the program defines a function `append` */
    int errors;
    PtrMap reported;    /* the nodes an error was reported at */
    StrMap structs;     /* struct name -> its definition */
    StrMap cells;       /* the slice cells of the frames a closure being checked is
                         * written in, by name (#3549) */
} Ctx;

typedef struct Frame {
    ASTNode* owner;
    ASTNode* body;
    Local** locals;
    int nlocals, cap;
    PtrMap uses;         /* identifier -> the local it names */
    PtrMap binds;        /* binding node -> the local it binds */
    const StrMap* outer; /* the names of the frames a closure is written in */
    StrMap promoted;     /* the names a closure written here assigns (#3549) */
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
           is_call(n, "__slice_copy") || is_call(n, "__slice_drop") ||
           is_call(n, "__slice_set") || is_call(n, "__slice_set_shared") ||
           is_call(n, "__slice_hold");
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
/* ---- the closures written in a frame ---- */

/* Does `n`, a closure's body, assign `name`, as codegen's promotion reads it
 * (a binding of the name, `name op= x`, `name++`, a destructure into it)? A
 * closure that declares the name itself (a parameter, a typed local) holds
 * its own, which is not the frame's. */
static int closure_assigns(ASTNode* n, const char* name) {
    if (!n || is_opaque(n)) return 0;
    if (n->type == AST_VARIABLE_DECLARATION && n->value && strcmp(n->value, name) == 0)
        return n->type_inferred;
    if (n->type == AST_COMPOUND_ASSIGNMENT && n->value && strcmp(n->value, name) == 0) return 1;
    /* `name.push(v)` writes `name`: its length, and its buffer when it grows
     * (#3568, ast_pushes_name). */
    if (ast_pushes_name(n, name)) return 1;
    if (n->type == AST_UNARY_EXPRESSION && n->value && n->child_count == 1 &&
        (strcmp(n->value, "++") == 0 || strcmp(n->value, "--") == 0) && n->children[0] &&
        n->children[0]->type == AST_IDENTIFIER && n->children[0]->value &&
        strcmp(n->children[0]->value, name) == 0)
        return 1;
    if (n->type == AST_TUPLE_DESTRUCTURE) {
        for (int i = 0; i + 1 < n->child_count; i++)
            if (n->children[i] && n->children[i]->value &&
                strcmp(n->children[i]->value, name) == 0)
                return 1;
    }
    if (n->type == AST_CLOSURE_PARAM && n->value && strcmp(n->value, name) == 0) return 0;
    for (int i = 0; i < n->child_count; i++)
        if (closure_assigns(n->children[i], name)) return 1;
    return 0;
}

/* Does closure `cl` declare `name` itself, as a parameter or a typed
 * local? */
static int closure_declares(ASTNode* cl, const char* name) {
    if (!cl || is_opaque(cl)) return 0;
    if ((cl->type == AST_CLOSURE_PARAM ||
         (cl->type == AST_VARIABLE_DECLARATION && !cl->type_inferred)) &&
        cl->value && strcmp(cl->value, name) == 0)
        return 1;
    for (int i = 0; i < cl->child_count; i++)
        if (closure_declares(cl->children[i], name)) return 1;
    return 0;
}

/* A closure written in the frame: every slice local of the frame it
 * assigns is a cell the frame and the closure share (#3549), by name, as
 * codegen promotes it. */
static void note_closure(Frame* f, Scope* s, ASTNode* closure) {
    StrMap names;
    strmap_init(&names);
    collect_names(closure, &names);
    for (int i = 0; i < strmap_count(&names); i++) {
        const char* nm = strmap_key_at(&names, i);
        Local* l = scope_lookup(s, nm);
        if (!l || !l->is_slice || strmap_has(&f->promoted, nm)) continue;
        if (!closure_declares(closure, nm) && closure_assigns(closure, nm))
            strmap_put(&f->promoted, nm, (void*)1);
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
    ASTNode* arm;      /* the match arm whose pattern is being resolved (#3568) */
    ASTNode* subject;  /* the value the innermost match matches */
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
    /* `_ = v` binds nothing: codegen throws `v` away (and a slice's
     * reference with it). */
    if (strcmp(n->value, "_") == 0) return;
    ASTNode* init = n->child_count > 0 ? n->children[0] : NULL;
    Local* l = scope_lookup(s, n->value);
    if (l && !n->type_inferred && !l->hoisted) l = NULL;   /* a new, typed declaration */
    if (l) {
        pm_put(&f->binds, n, l);
        if (l->pattern) l->written = 1;
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
        if (n->type == AST_CLOSURE) note_closure(r->f, s, n);
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
                if (l) {
                    pm_put(&r->f->binds, v, l);
                } else if (store_elsewhere(r, v->value)) {
                    continue;
                } else {
                    /* #3568: codegen gives a slice local a destructure
                     * declares a reference of its own (the part a call's
                     * tuple hands over, or a copy of one another place
                     * holds), which its scope's exit gives back. */
                    l = new_local(r->f, s, v, block);
                    if (l->is_slice && strcmp(v->value, "_") != 0) l->destructured = 1;
                    else l->pattern = 1;
                }
            }
            return;
        }
        case AST_PATTERN_VARIABLE:
            if (n->value) {
                Local* l = new_local(r->f, s, n, NULL);
                l->pattern = 1;
                l->arm = r->arm;
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
        case AST_MATCH_STATEMENT: {
            ASTNode* saved_subject = r->subject;
            r->subject = n->child_count > 0 ? n->children[0] : NULL;
            resolve_children(r, s, n);
            r->subject = saved_subject;
            return;
        }
        case AST_MATCH_ARM: {
            /* #3568: a slice its pattern binds views what the match was
             * given, which the arm's body may let go (bind again, free):
             * where the body is a block, the local takes a reference of its
             * own there (rewrite's AST_MATCH_ARM). */
            ASTNode* saved = r->arm;
            r->arm = n->child_count > 1 && n->children[1] && n->children[1]->type == AST_BLOCK
                         ? n : NULL;
            Scope in;
            strmap_init(&in.names);
            in.up = s;
            in.depth = s ? s->depth + 1 : 1;
            int first = r->f->nlocals, pat_end = r->f->nlocals;
            for (int i = 0; i < n->child_count; i++) {
                if (i == 1) r->arm = saved;   /* the body's own patterns are its own */
                resolve(r, &in, n->children[i], NULL);
                if (i == 0) pat_end = r->f->nlocals;
            }
            r->arm = saved;
            /* A slice the pattern binds that a body that is no block binds
             * again or pushes into holds a reference of its own as well:
             * the body becomes a block of that one statement, whose start
             * takes the reference (rewrite's AST_MATCH_ARM). */
            ASTNode* body = n->child_count > 1 ? n->children[1] : NULL;
            if (body && body->type != AST_BLOCK) {
                int wrap_body = 0;
                for (int k = first; k < pat_end && !wrap_body; k++) {
                    Local* pl = r->f->locals[k];
                    wrap_body = pl->pattern && pl->written && !pl->arm;
                }
                if (wrap_body) {
                    ASTNode* blk = create_ast_node(AST_BLOCK, NULL, body->line, body->column);
                    ASTNode* es = create_ast_node(AST_EXPRESSION_STATEMENT, NULL, body->line,
                                                  body->column);
                    if (body->source_file) {
                        blk->source_file = strdup(body->source_file);
                        es->source_file = strdup(body->source_file);
                    }
                    add_child(es, body);
                    add_child(blk, es);
                    n->children[1] = blk;
                    for (int k = first; k < pat_end; k++)
                        if (r->f->locals[k]->pattern && !r->f->locals[k]->arm)
                            r->f->locals[k]->arm = n;
                }
            }
            /* A list pattern's tail is a slice of the subject's type, and
             * each element it names one of the subject's element type, as
             * the checker types them in the arm's table alone. */
            ASTNode* p = n->child_count > 0 ? n->children[0] : NULL;
            Type* st = r->subject ? r->subject->node_type : NULL;
            if (p && (p->type == AST_PATTERN_CONS || p->type == AST_PATTERN_LIST) &&
                type_is_slice(st)) {
                for (int k = 0; k < p->child_count; k++) {
                    int tail = p->type == AST_PATTERN_CONS && k == 1;
                    Type* t = tail ? st : st->element_type;
                    Local* pl = p->children[k] ? pm_get(&r->f->binds, p->children[k]) : NULL;
                    if (pl && !pl->is_slice && type_is_slice(t)) {
                        pl->type = t;
                        pl->is_slice = 1;
                    }
                }
            }
            strmap_free(&in.names);
            return;
        }
        case AST_FOR_LOOP: case AST_CASE_STATEMENT:
            resolve_scoped(r, s, n);
            return;
        case AST_IDENTIFIER: {
            Local* l = n->value ? scope_lookup(s, n->value) : NULL;
            if (l) pm_put(&r->f->uses, n, l);
            /* A pattern's name carries no type of its own (the checker
             * types it in the arm's table): its uses do. */
            if (l && l->pattern && !l->type && n->node_type) {
                l->type = n->node_type;
                l->is_slice = type_is_slice(n->node_type);
            }
            return;
        }
        case AST_FUNCTION_CALL:
            /* What `free`, `append` and `push` take a buffer from owns it. */
            if ((is_free(n) || is_append(r->c, n) || is_push(n)) && n->children[0] &&
                n->children[0]->type == AST_IDENTIFIER && n->children[0]->value) {
                Local* l = scope_lookup(s, n->children[0]->value);
                if (l) l->target = 1;
                if (l && l->pattern && is_push(n)) l->written = 1;
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
        /* A `@keep` parameter a closure assigns too: its cell takes over the
         * parameter's reference (codegen's emit_promoted_cell_declaration
         * seeded with it), and the cell's last holder gives it back. */
        else if (strmap_has(&f->promoted, l->name)) l->kind = K_CELL;
        else if (l->param) l->kind = l->keep ? K_KEEP : K_PARAM;
        else if (l->destructured) l->kind = K_VIEW;
        /* A binding of a match arm's pattern takes a reference of its own
         * where the arm's body starts (rewrite's AST_MATCH_ARM), and so does
         * a receive arm's field its body binds again or pushes into
         * (check_frame): each is a copy of what was matched, or of the
         * message's field, which the message releases. `hold` stays
         * whatever kind the fixpoint below gives it: one an owner is moved
         * into is still a copy until the hold makes it its own. */
        else if (l->pattern && (l->arm || (l->written && !l->block && f->owner &&
                                           (f->owner->type == AST_RECEIVE_ARM ||
                                            f->owner->type == AST_TIMEOUT_ARM)))) {
            l->kind = K_VIEW;
            l->hold = 1;
        }
        else if (l->pattern || !l->block) l->kind = K_OTHER;
        else if (l->fresh || l->target) l->kind = K_OWNER;
        else l->kind = K_VIEW;
    }
    /* A local a local that owns is moved into may be given its buffer. */
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

static int keeps_arg(Ctx* c, ASTNode* call, int i);

/* Every slice local of the frame named in `n` may be viewed. */
static void mark_viewed(Frame* f, ASTNode* n) {
    if (!n || is_opaque(n)) return;
    if (n->type == AST_IDENTIFIER) {
        Local* l = pm_get(&f->uses, n);
        if (l) l->viewed = 1;
        return;
    }
    for (int i = 0; i < n->child_count; i++) mark_viewed(f, n->children[i]);
}

static Use child_use(ASTNode* p, int i);

/* Is a call one that takes what it is given (`free`, `append`, `push`)? */
static int consuming_call(Ctx* c, ASTNode* n) {
    return is_intrinsic(n) || is_free(n) || is_append(c, n) || is_push(n);
}

/* The locals a view of whose buffer may be held past the expression that
 * takes it, as the analysis before #3568 found them and more, read from the
 * frame alone: a local under a sub-slice or a row read (`xs[a..b]`,
 * `xs[..]`, `rows[i]`) that is bound, stored, returned or given to a call,
 * a local given to a call (other than `free`, `append`, `push` and a `@keep`
 * parameter, which take it), and a local a closure names. A view read in
 * place (`rows[i].len`, `xs[1..][0]`, a store into `rows[i]`) is none. */
static void views_of(Ctx* c, Frame* f, ASTNode* n, ASTNode* parent, int idx) {
    if (!n || is_opaque(n)) return;
    if (is_frame(n)) {
        StrMap names;
        strmap_init(&names);
        collect_names(n, &names);
        for (int i = 0; i < f->nlocals; i++)
            if (strmap_has(&names, f->locals[i]->name)) f->locals[i]->viewed = 1;
        strmap_free(&names);
        return;
    }
    int held = parent && (parent->type == AST_FUNCTION_CALL ? !consuming_call(c, parent)
                                                             : child_use(parent, idx) != C_BORROW);
    if (held && n->child_count > 0 &&
        (n->type == AST_SLICE_EXPR ||
         (n->type == AST_ARRAY_ACCESS && type_is_slice(n->node_type))))
        mark_viewed(f, n->children[0]);
    if (n->type == AST_FUNCTION_CALL && !consuming_call(c, n)) {
        for (int i = 0; i < n->child_count; i++) {
            ASTNode* a = n->children[i];
            if (a && a->type == AST_NAMED_ARG && a->child_count > 0) a = a->children[0];
            if (a && a->type == AST_IDENTIFIER && type_is_slice(a->node_type) &&
                !keeps_arg(c, n, i)) {
                Local* l = pm_get(&f->uses, a);
                if (l) l->viewed = 1;
            }
        }
    }
    for (int i = 0; i < n->child_count; i++) {
        ASTNode* ch = n->children[i];
        /* A named argument is its call's. */
        if (ch && ch->type == AST_NAMED_ARG && n->type == AST_FUNCTION_CALL) {
            for (int k = 0; k < ch->child_count; k++) views_of(c, f, ch->children[k], n, i);
            continue;
        }
        views_of(c, f, ch, n, i);
    }
}

/* Does `l` own what it holds, so that a copy of it into another place is a
 * move (E0906 after)? Not where a view of it may be taken (views_of): that
 * local is copied, each place given a reference of its own, as it gave a
 * view before #3568, so a program that kept a view of it and used it again
 * still compiles. */
static int owns(const Local* l) {
    return l && (l->kind == K_OWNER || l->kind == K_KEEP) && !l->viewed;
}

/* Does the place `l` hold a reference, which its scope's exit releases? */
static int holds(const Local* l) {
    return l && (l->kind == K_OWNER || l->kind == K_VIEW || l->kind == K_KEEP);
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

static void warn(Ctx* c, Frame* f, ASTNode* at, AetherErrorCode code, const char* msg,
                 const char* hint) {
    if (pm_get(&c->reported, at)) return;
    pm_put(&c->reported, at, (Local*)1);
    AetherError w = {at && at->source_file ? at->source_file
                                           : (f->owner ? f->owner->source_file : NULL),
                     NULL, node_line(at), at ? at->column : 0, msg, hint, NULL, code};
    aether_warning_report(&w);
}

/* ---- where a value goes ---- */

/* How the `i`th child of `p` is used when it is a slice: borrowed (read,
 * indexed, measured, viewed, compared), or moved into a place (bound,
 * returned, stored, put in a struct, a tuple, an array or a message). A
 * place this does not know is lent to (C_LEND). A call's arguments are the
 * call's to decide (keeps_arg). */
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
            return C_LEND;
    }
}

static ASTNode* call_def(Ctx* c, ASTNode* call) {
    if (!call->value) return NULL;
    ASTNode* d = (ASTNode*)strmap_get(&c->fns, call->value);
    if (!d && strchr(call->value, '.')) {
        /* The whole name (#3574): 256 bytes cut a long qualified one, and
         * the call found no definition, so a `@keep` it has went unseen. */
        char* flat = strdup(call->value);
        if (!flat) return NULL;
        for (char* q = flat; *q; q++)
            if (*q == '.') *q = '_';
        d = (ASTNode*)strmap_get(&c->fns, flat);
        free(flat);
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

/* A loop walked from `entry`, which left `exit` (flow_loop). */
typedef struct FlowMemo {
    ASTNode* body;
    ASTNode* cond;
    State entry, exit;
    struct FlowMemo* next;
} FlowMemo;

typedef struct {
    Ctx* c;
    Frame* f;
    int n;
    Loop* loop;
    FlowMemo* memo;     /* the loops walked, by entry state */
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
    char howbuf[240];
    const char* msg = ""; const char* hint = "";
    const char* how = "it was moved";
    if (at && is_free(at)) how = "it was freed";
    else if (at && is_append(w->c, at)) how = "`append` consumed it";
    else if (at && at->type == AST_FUNCTION_CALL && at->value) {
        snprintf(howbuf, sizeof(howbuf), "it was given to '%s', which keeps it", at->value);
        how = howbuf;
    }
    msg = aether_internf("'%s' is used after %s%s (line %d): an owned slice has one owner",
             l->name, s->st[l->idx] == S_MAYBE ? "a path where " : "", how, node_line(at));
    if (at && is_append(w->c, at))
        hint = aether_internf(
                 "use the slice `append` returned (`%s = append(%s, v)`), or grow it in place "
                 "with `%s.push(v)`",
                 l->name, l->name, l->name);
    else
        hint = aether_internf(
                 "use the slice that holds it now, take a view first (`v = %s[..]`), or bind "
                 "'%s' again before this",
                 l->name, l->name);
    report(w->c, w->f, use, AETHER_ERR_USE_AFTER_MOVE, msg, hint);
}

static void use_local(Flow* w, ASTNode* id, State* s) {
    Local* l = pm_get(&w->f->uses, id);
    if (!l || s->dead) return;   /* a local consume_local left empty too */
    if (s->st[l->idx] != S_LIVE) moved_message(w, l, id, s);
}

static void move_local(Flow* w, ASTNode* id, ASTNode* at, State* s) {
    Local* l = pm_get(&w->f->uses, id);
    if (!owns(l) || s->dead) return;
    use_local(w, id, s);
    s->st[l->idx] = S_MOVED;
    s->at[l->idx] = at;
}

/* `free(xs)` and `append(xs, v)` consume the local `xs` that holds a
 * reference: it is left empty (rewrite_call's `__slice_take`,
 * `slice_consumes`), so reading it before it is bound again is E0906,
 * whether or not a view of it was taken or it was given to a function
 * (`viewed`, which makes a move elsewhere a copy). */
static void consume_local(Flow* w, ASTNode* id, ASTNode* at, State* s) {
    Local* l = pm_get(&w->f->uses, id);
    if (!l || !holds(l) || s->dead) return;
    use_local(w, id, s);
    s->st[l->idx] = S_MOVED;
    s->at[l->idx] = at;
}

/* `free`, `append` and `push` release or grow a buffer: a parameter's is
 * the caller's (E0901), as a string parameter's count is. */
static void borrowed_param_error(Flow* w, ASTNode* id, const char* what) {
    Local* l = pm_get(&w->f->uses, id);
    if (!l || l->kind != K_PARAM) return;
    const char* msg = ""; const char* hint = "";
    msg = aether_internf(
             "cannot %s '%s': it is a parameter, which borrows the caller's slice", what,
             l->name);
    hint = aether_internf(
             "declare it `%s: @keep %s` to take the caller's buffer (the caller gives it up), "
             "or copy the elements into a buffer of the function's own",
             l->name, type_spelled(l->type));
    report(w->c, w->f, id, AETHER_ERR_FREES_BORROWED, msg, hint);
}

static void flow_value(Flow* w, ASTNode* e, State* s, Use use) {
    if (!e) return;
    if (e->type == AST_IDENTIFIER) {
        if (use != C_BORROW && type_is_slice(e->node_type)) move_local(w, e, e, s);
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
            else consume_local(w, xs, n, s);
        } else {
            flow(w, xs, s, C_BORROW);
        }
        return;
    }
    if (is_free(n)) {
        ASTNode* xs = n->children[0];
        if (xs && xs->type == AST_IDENTIFIER) {
            borrowed_param_error(w, xs, "free");
            consume_local(w, xs, n, s);
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
                const char* msg = ""; const char* hint = "";
                msg = aether_internf(
                         "cannot give '%s' to '%s', which keeps it (`@keep`): '%s' is a "
                         "parameter, which borrows the caller's slice",
                         l->name, n->value, l->name);
                hint = aether_internf(
                         "declare it `%s: @keep %s` too, so the buffer passes through",
                         l->name, type_spelled(l->type));
                report(c, w->f, a, AETHER_ERR_FREES_BORROWED, msg, hint);
            }
            move_local(w, a, n, s);
        } else if (keeps_arg(c, n, i)) {
            /* Any other value a `@keep` parameter takes moves as a plain
             * argument does (rewrite_call gives it the parameter): an `if`'s
             * branch that is a local that owns is moved there. */
            flow_value(w, a, s, C_MOVE);
        } else {
            flow_value(w, a, s, C_BORROW);
        }
    }
}

static void flow_loop_walk(Flow* w, ASTNode* cond, ASTNode* body, ASTNode* step, State* s) {
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

/* A loop's walk from `s`. A loop walked once already from the same state
 * gives the same state after it, and has reported what it reports, so that
 * walk is reused (#3574): each walk walks the body twice, each walking the
 * loops inside, and loops nested 20 deep took a second, doubling with each
 * level. The breaks and continues of a loop join only its own (the innermost
 * loop), so a walk makes no join outside itself to make again. */
static void flow_loop(Flow* w, ASTNode* cond, ASTNode* body, ASTNode* step, State* s) {
    size_t n = (size_t)w->n;
    for (FlowMemo* m = w->memo; m; m = m->next) {
        if (m->body != body || m->cond != cond || m->entry.dead != s->dead ||
            memcmp(m->entry.st, s->st, n) != 0 ||
            memcmp(m->entry.at, s->at, n * sizeof(ASTNode*)) != 0)
            continue;
        st_copy(w, s, &m->exit);
        return;
    }
    FlowMemo* m = (FlowMemo*)calloc(1, sizeof *m);
    if (!m) {
        fprintf(stderr, "aetherc: out of memory in the slice ownership check\n");
        exit(1);
    }
    m->body = body;
    m->cond = cond;
    m->entry = st_clone(w, s);
    flow_loop_walk(w, cond, body, step, s);
    m->exit = st_clone(w, s);
    m->next = w->memo;
    w->memo = m;
}

/* The loops a frame's walk kept. */
static void flow_memo_free(FlowMemo* m) {
    while (m) {
        FlowMemo* next = m->next;
        st_free(&m->entry);
        st_free(&m->exit);
        free(m);
        m = next;
    }
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

/* ---- the rewrite: moves, copies, releases ---- */

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

/* A `match` bound, assigned or returned is its statement's, lowered with the
 * statement; one an intrinsic takes is an operand of it, which codegen runs
 * ahead of the statement into a temporary (emit_match_operand) the
 * intrinsic reads. Not marked, it was a statement expression, which only a
 * GCC-compatible compiler takes (#3568). */
static void own_operand(ASTNode* v) {
    if (v && v->type == AST_MATCH_STATEMENT && !ast_match_is_operand(v))
        v->annotation = annotation_add_marker(v->annotation, "operand");
}

/* *slot becomes `name(*slot)`, of *slot's type. */
static void wrap(ASTNode** slot, const char* name) {
    ASTNode* e = *slot;
    ASTNode* c = mk_call(name, e->node_type, e);
    own_operand(e);
    add_child(c, e);
    *slot = c;
}

/* `name(place, v)`, of `t`: the store of `v` into `place`, which gives back
 * the reference `place` held. */
static ASTNode* mk_store(const char* name, Type* t, ASTNode* place, ASTNode* v) {
    own_operand(v);
    ASTNode* set = mk_call(name, t, v);
    add_child(set, place);
    add_child(set, v);
    return set;
}

static void rewrite(Ctx* c, Frame* f, ASTNode** slot);

/* A pattern binds names and holds no value to own: a struct pattern's
 * `field: name` is no store. */
static int is_pattern(const ASTNode* n) {
    return n->type == AST_PATTERN_VARIABLE || n->type == AST_PATTERN_STRUCT ||
           n->type == AST_PATTERN_LIST || n->type == AST_PATTERN_CONS ||
           n->type == AST_PATTERN_LITERAL;
}

/* Is `e` a view (`f()[a..b]`, `string.bytes(s)[1..]`, one of one) of a call's
 * result, which holds the reference the call handed over and nothing else
 * does? The view takes that reference over (#3568, #3570). */
static int view_of_fresh(ASTNode* e) {
    if (!e || e->type != AST_SLICE_EXPR) return 0;
    while (e && e->type == AST_SLICE_EXPR && e->child_count > 0) e = e->children[0];
    return e && e->type == AST_FUNCTION_CALL && type_is_slice(e->node_type) && e->value &&
           strncmp(e->value, "__slice_", 8) != 0;
}

/* The arms' values of the `match` values being rewritten (rewrite_match_value):
 * each is the match's value, not a statement's, so none is read in place
 * (mark_in_place). A stack, for a `match` in an arm's statements. */
static ASTNode** g_arm_values = NULL;
static int g_arm_values_n = 0, g_arm_values_cap = 0;

static int is_arm_value(const ASTNode* e) {
    for (int i = 0; i < g_arm_values_n; i++)
        if (g_arm_values[i] == e) return 1;
    return 0;
}

static void push_arm_values(ASTNode* m) {
    for (int i = 1; i < m->child_count; i++) {
        ASTNode* nested = NULL;
        ASTNode** slot = m->children[i] ? match_arm_value_slot(m->children[i], &nested) : NULL;
        if (nested) {
            push_arm_values(nested);
            continue;
        }
        if (!slot || !*slot) continue;
        if (g_arm_values_n == g_arm_values_cap) {
            g_arm_values_cap = g_arm_values_cap ? g_arm_values_cap * 2 : 16;
            g_arm_values = (ASTNode**)realloc(g_arm_values,
                                              (size_t)g_arm_values_cap * sizeof(ASTNode*));
        }
        g_arm_values[g_arm_values_n++] = *slot;
    }
}

/* What a `match` arm's value, its parts rewritten, gives the place the
 * `match` moves into: a local, a place or a view of one is copied, a call's
 * result is the reference it hands over, as is a view of one. */
static void move_rewritten(ASTNode** slot);

static void move_arm_values(ASTNode* m) {
    for (int i = 1; i < m->child_count; i++) {
        ASTNode* nested = NULL;
        ASTNode** slot = m->children[i] ? match_arm_value_slot(m->children[i], &nested) : NULL;
        if (nested) move_arm_values(nested);
        else if (slot && *slot) move_rewritten(slot);
    }
}

static void move_rewritten(ASTNode** slot) {
    ASTNode* e = *slot;
    if (!e || !type_is_slice(e->node_type)) return;
    switch (e->type) {
        case AST_IDENTIFIER:
            /* Copied, a local that owns too: the move checks (E0906) read
             * a `match` arm's local as borrowed, so the local stays whole
             * for its later reads. */
            wrap(slot, "__slice_copy");
            return;
        case AST_SLICE_EXPR:
            if (!view_of_fresh(e)) wrap(slot, "__slice_copy");
            return;
        case AST_IF_EXPRESSION:
            for (int i = 1; i < e->child_count; i++) move_rewritten(&e->children[i]);
            return;
        case AST_MATCH_STATEMENT:
            move_arm_values(e);
            return;
        case AST_FUNCTION_CALL: case AST_SEND_ASK: case AST_NULL_LITERAL: case AST_ARRAY_LITERAL:
        case AST_SLICE_FROM_ARRAY: case AST_PTR_AS_ARRAY_CAST:
            return;
        default:
            wrap(slot, "__slice_copy");
            return;
    }
}

/* #3568: a `match` whose value moves into a place gives it what each arm
 * gives, arm by arm, as an `if`'s branches do: an arm's call hands its
 * reference over, an arm's place is copied. The whole `match` copied held
 * an arm's call result twice, which leaked (`vs = match k { 0 -> mk(4) _ ->
 * xs }`), and nested the `match` in the copy, where codegen lowered it as a
 * statement expression. */
static void rewrite_match_value(Ctx* c, Frame* f, ASTNode* m) {
    int mark = g_arm_values_n;
    push_arm_values(m);
    ASTNode* slot = m;
    rewrite(c, f, &slot);
    g_arm_values_n = mark;
    if (mark == 0) {
        free(g_arm_values);
        g_arm_values = NULL;
        g_arm_values_cap = 0;
    }
    move_arm_values(m);
}

/* A slice moved into a place gives the place one reference (#3568): taken
 * out of a local that owns it (which is left empty), and copied, with a
 * reference of its own, out of any other place (a parameter, a view, a
 * field, an element, a module-level `var`, a cell) and out of a view of
 * one. A call's result is the reference it hands over. */
static void rewrite_value(Ctx* c, Frame* f, ASTNode** slot, Use use) {
    ASTNode* e = *slot;
    if (!e || is_pattern(e)) return;
    if (use == C_BORROW || !type_is_slice(e->node_type)) {
        rewrite(c, f, slot);
        return;
    }
    /* C_LEND: a place this pass does not know, which is given a view, as
     * before ownership; a local that owns still moves there. */
    switch (e->type) {
        case AST_IDENTIFIER: {
            Local* l = pm_get(&f->uses, e);
            if (owns(l)) wrap(slot, "__slice_take");
            else wrap(slot, use == C_MOVE ? "__slice_copy" : "__slice_lend");
            return;
        }
        case AST_MEMBER_ACCESS: case AST_ARRAY_ACCESS:
            rewrite(c, f, slot);
            wrap(slot, use == C_MOVE ? "__slice_copy" : "__slice_lend");
            return;
        case AST_SLICE_EXPR:
            rewrite(c, f, slot);
            /* A view of a call's result takes over the reference the result
             * handed over, which nothing else holds. */
            if (use == C_MOVE && !view_of_fresh(e)) wrap(slot, "__slice_copy");
            return;
        case AST_IF_EXPRESSION:
            /* Each branch is the value, going where the `if` goes. */
            if (e->child_count > 0) rewrite(c, f, &e->children[0]);
            for (int i = 1; i < e->child_count; i++) rewrite_value(c, f, &e->children[i], use);
            return;
        case AST_MATCH_STATEMENT:
            if (use == C_MOVE) {
                rewrite_match_value(c, f, e);
                return;
            }
            rewrite(c, f, slot);
            return;
        case AST_FUNCTION_CALL: case AST_SEND_ASK: case AST_NULL_LITERAL: case AST_ARRAY_LITERAL:
        case AST_SLICE_FROM_ARRAY: case AST_PTR_AS_ARRAY_CAST:
            /* A call's result is the reference it hands over; the rest are
             * no counted buffer. */
            rewrite(c, f, slot);
            return;
        default:
            /* Any other read of a slice (an optional's value, a cast) is a
             * copy of what a place holds. */
            rewrite(c, f, slot);
            if (use == C_MOVE) wrap(slot, "__slice_copy");
            return;
    }
}

/* Is `a`, argument `i` of `call`, a buffer made for the call and only lent
 * to it? `a` is a call's slice, which holds the reference it hands over
 * (`make`, `append`, a function's result, as a local bound to it would),
 * given to a parameter that does not keep it (`@keep`). The call releases
 * it once it returns (codegen's argument temporaries, through the
 * `slice_temp` marker), as the local it would otherwise be bound to is
 * released: a callee that keeps it took a reference of its own (#3568). An
 * extern's or a builtin's result that may view memory (a slice, a pointer)
 * is left alone, C taking no reference, and so is a call whose positions do
 * not map onto the parameters one for one (a builder's `_ctx`, named
 * arguments). */
/* Does every arm of the `match` value `m` give a call's result? */
static int match_gives_fresh(ASTNode* m) {
    for (int i = 1; i < m->child_count; i++) {
        ASTNode* nested = NULL;
        ASTNode** slot = match_arm_value_slot(m->children[i], &nested);
        if (nested) {
            if (!match_gives_fresh(nested)) return 0;
            continue;
        }
        if (!slot || !*slot || (*slot)->type != AST_FUNCTION_CALL || !(*slot)->value ||
            strncmp((*slot)->value, "__slice_", 8) == 0)
            return 0;
    }
    return m->child_count > 1;
}

/* Does the parameter argument `i` of `call` is given wrap it (an optional, a
 * tuple)? Codegen gives a value there that is no place (a call's result, a
 * view, an `if`) an argument temporary of the parameter's type, which drops
 * what it holds after the call (fresh_composite_arg_type,
 * emit_composite_arg_temp); a place it passes as it is, borrowed. */
static int wraps_arg(Ctx* c, ASTNode* call, int i) {
    ASTNode* d = call_def(c, call);
    if (!d) return 0;
    int k = 0;
    for (int j = 0; j < d->child_count; j++) {
        ASTNode* p = d->children[j];
        if (!p || (p->type != AST_PATTERN_VARIABLE && p->type != AST_VARIABLE_DECLARATION))
            continue;
        if (k++ == i)
            return p->node_type && (p->node_type->kind == TYPE_OPTIONAL ||
                                    p->node_type->kind == TYPE_TUPLE);
    }
    return 0;
}

static int slice_temp_arg(Ctx* c, ASTNode* call, int i) {
    ASTNode* a = call->children[i];
    /* An `if` that gives the slice holds the reference its branch gave
     * (each branch moved in, rewrite_call), and so does a `match` each arm
     * of which gives a call's result. */
    int branches = a && type_is_slice(a->node_type) &&
                   (a->type == AST_IF_EXPRESSION ||
                    (a->type == AST_MATCH_STATEMENT && match_gives_fresh(a)) ||
                    view_of_fresh(a));
    if (!a || (!branches && (a->type != AST_FUNCTION_CALL || !type_is_slice(a->node_type) ||
                             !a->value || strncmp(a->value, "__slice_", 8) == 0)))
        return 0;
    if (keeps_arg(c, call, i)) return 0;
    for (int k = 0; k < call->child_count; k++)
        if (call->children[k] && call->children[k]->type == AST_NAMED_ARG) return 0;
    if (call->value && strcmp(call->value, "call") == 0) return 1;
    ASTNode* d = call_def(c, call);
    if (!d) {
        Type* t = call->node_type;
        return t && t->kind != TYPE_PTR && !type_is_slice(t) && t->kind != TYPE_STRING;
    }
    for (int j = 0; j < d->child_count; j++) {
        ASTNode* p = d->children[j];
        if (!p || (p->type != AST_PATTERN_VARIABLE && p->type != AST_VARIABLE_DECLARATION))
            continue;
        if (p->value && strcmp(p->value, "_ctx") == 0) return 0;
        break;
    }
    /* A parameter that wraps the slice (an optional, a tuple) is given an
     * argument temporary of its own, which holds the reference and drops it
     * after the call (wraps_arg), as a string there is: released here too,
     * it was given back twice. */
    return !wraps_arg(c, call, i);
}

/* Is `e` a field of a struct parameter (`b.items`, `b.inner.items`)? The
 * field is its caller's: the copy of the struct the callee is given holds
 * no reference of its own (R3: parameters borrow). A parameter its function
 * writes into is a value of its own (ast_struct_param_written, as codegen
 * makes it), whose fields hold theirs. */
static int param_struct_field(Frame* f, ASTNode* e) {
    if (!e || e->type != AST_MEMBER_ACCESS) return 0;
    ASTNode* b = e;
    while (b && b->type == AST_MEMBER_ACCESS && b->child_count > 0) b = b->children[0];
    Local* l = b && b->type == AST_IDENTIFIER ? pm_get(&f->uses, b) : NULL;
    if (!l || !l->param || !l->type || l->type->kind != TYPE_STRUCT) return 0;
    return !ast_struct_param_written(f->body, l->name);
}

/* Does the value `e` hold a reference of its own: a local, a cell, a
 * field, an element, a module-level `var` or a state field (each place
 * holds one), a call's result (which it hands over), or a view of one
 * (which takes it over)? */
static int holds_reference(Ctx* c, Frame* f, ASTNode* e) {
    if (!e) return 0;
    switch (e->type) {
        case AST_IDENTIFIER: {
            Local* l = pm_get(&f->uses, e);
            if (l) return holds(l) || l->kind == K_CELL;
            return e->value && (strmap_has(&c->cells, e->value) ||
                                strmap_has(&c->globals, e->value) ||
                                strmap_has(&c->state, e->value));
        }
        case AST_MEMBER_ACCESS: case AST_ARRAY_ACCESS:
            return !param_struct_field(f, e);
        case AST_SLICE_EXPR:
            return view_of_fresh(e);
        case AST_FUNCTION_CALL:
            return e->value && strcmp(e->value, "__slice_lend") != 0;
        default:
            return 0;
    }
}

static void rewrite_call(Ctx* c, Frame* f, ASTNode* n) {
    if (is_intrinsic(n)) return;
    if (is_append(c, n) || is_push(n) || is_free(n)) {
        if (n->child_count > 1) rewrite_value(c, f, &n->children[1], C_MOVE);
        ASTNode** xs = &n->children[0];
        rewrite(c, f, xs);
        if (is_push(n)) {
            /* A place that holds no reference of its own (a struct
             * parameter's field) takes one before it grows, so the growth
             * copies and leaves the buffer to its owner (`slice_push_copy`,
             * as an append of such a value does below). A closure's push
             * into a slice it captures is a write, which makes the slice a
             * cell the frames share (closure_assigns), and a cell holds. */
            if (!holds_reference(c, f, *xs))
                n->annotation = annotation_add_marker(n->annotation, "slice_push_copy");
            return;
        }
        if (is_free(n)) {
            /* `free` gives back the reference its place holds. A value that
             * holds none (a pattern's view, a sub-slice, a name this pass
             * does not know) is a view `free` refuses by its mark
             * (`slice_free_view`, aether_slice_free_view). */
            if (!holds_reference(c, f, *xs))
                n->annotation = annotation_add_marker(n->annotation, "slice_free_view");
            /* A struct parameter's field is its caller's, who releases it:
             * the `free` gives back nothing (aether_slice_free_view). */
            if (param_struct_field(f, *xs))
                warn(c, f, n, AETHER_WARN_FREE_NOT_OWNED,
                     "this `free` gives back nothing: the slice is a field of a struct "
                     "parameter, which its caller owns and releases (parameters borrow)",
                     "free it where the struct is owned, or leave it to the caller's scope");
            /* The place `free` releases is left empty, so nothing releases
             * it again. */
            if (pure_place(*xs)) wrap(xs, "__slice_take");
            return;
        }
        /* A value that holds no reference of its own (a struct parameter's
         * field, a view of a place) grows a copy: its own reference makes
         * the buffer shared, so the growth moves to storage of its own
         * (aether_slice_grow1) and leaves the buffer to its owner. It grew
         * in place, under the owner, who then held freed storage. */
        /* A module-level `var` grows a copy too (#3568): it is read under
         * its lock, which an emptying of the var in place would not be, and
         * the store of the result swaps the new buffer in under the lock. */
        int module_var = (*xs)->type == AST_IDENTIFIER && (*xs)->value &&
                         !pm_get(&f->uses, *xs) && strmap_has(&c->globals, (*xs)->value);
        if (!holds_reference(c, f, *xs) || module_var) {
            wrap(xs, "__slice_copy");
            return;
        }
        /* `append` reads `v` before it grows the slice, so the place it
         * consumes is left empty there, by codegen, not by a `__slice_take`
         * argument the operand-order rule (#2765) would evaluate first; the
         * place holds no reference to the storage the growth may move. */
        if (pure_place(*xs) && !annotation_has_marker(n->annotation, "slice_consumes"))
            n->annotation = annotation_add_marker(n->annotation, "slice_consumes");
        return;
    }
    for (int i = 0; i < n->child_count; i++) {
        ASTNode** slot = &n->children[i];
        if (*slot && (*slot)->type == AST_NAMED_ARG && (*slot)->child_count > 0)
            slot = &(*slot)->children[0];
        if (!*slot) continue;
        /* Given to a `@keep` parameter: the parameter holds a reference,
         * moved out of a local that owns, or a copy of its own. */
        if (keeps_arg(c, n, i)) {
            rewrite_value(c, f, slot, C_MOVE);
            continue;
        }
        /* #3568: given to a parameter that wraps it (an optional, a tuple),
         * a value that is no place goes into an argument temporary, which
         * drops what it holds after the call (wraps_arg): it takes a
         * reference of its own, a view or a branch's place a copy, a call's
         * result the one it hands over. A view (`count(xs[1..])`) or an
         * `if` (`count(if c { xs } else { mk(2) })`) was put there as it
         * stood, and the drop gave back the buffer's reference it never
         * took. */
        ASTNode* av = *slot;
        if (wraps_arg(c, n, i) && type_is_slice(av->node_type) &&
            av->type != AST_IDENTIFIER && av->type != AST_MEMBER_ACCESS &&
            av->type != AST_ARRAY_ACCESS && av->type != AST_TUPLE_UNWRAP) {
            rewrite(c, f, slot);
            move_rewritten(slot);
            continue;
        }
        if (slice_temp_arg(c, n, i)) {
            /* An `if`'s branches each give the call a reference, released
             * after it: a call's result its own, a place a copy. The
             * parameter borrows, as the move checks read it, so a local
             * that owns is copied, never moved (it read empty after). */
            rewrite(c, f, slot);
            if ((*slot)->type == AST_IF_EXPRESSION) move_rewritten(slot);
            (*slot)->annotation = annotation_add_marker((*slot)->annotation, "slice_temp");
        } else {
            rewrite(c, f, slot);
        }
    }
}

/* `defer __slice_drop(x)`: the local `l` gives back its reference on every
 * exit of the scope it is declared in. */
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

/* Is `lhs` a field of a struct C lays out (`extern struct`, `@c_import`),
 * where a slice is the bare pointer C declares, which holds no reference? */
static int c_field(Ctx* c, ASTNode* lhs) {
    if (!lhs || lhs->type != AST_MEMBER_ACCESS || lhs->child_count < 1 || !lhs->children[0])
        return 0;
    Type* t = lhs->children[0]->node_type;
    while (t && t->kind == TYPE_PTR && t->element_type) t = t->element_type;
    if (!t || t->kind != TYPE_STRUCT || !t->struct_name) return 0;
    /* `extern`, `extern_packed`, `extern_c_import`, `extern_c_import_union` */
    ASTNode* d = (ASTNode*)strmap_get(&c->structs, t->struct_name);
    return d && d->annotation && strncmp(d->annotation, "extern", 6) == 0;
}

/* Is `place` one other threads may read: a module-level `var`, an actor's
 * state field, or a place reached through a pointer, an actor's reference
 * or a slice's element? A store there publishes what it stores. */
static int shared_place(Ctx* c, ASTNode* place) {
    while (place && (place->type == AST_MEMBER_ACCESS || place->type == AST_ARRAY_ACCESS) &&
           place->child_count > 0) {
        /* #3568: the rule a string store follows (#3398, codegen's
         * store_publishes): a place reached through a pointer, an actor's
         * reference or a slice's element is memory another actor can reach,
         * so what is stored there counts atomically from then on. */
        Type* ht = place->children[0] ? place->children[0]->node_type : NULL;
        if (ht && (ht->kind == TYPE_PTR || ht->kind == TYPE_ACTOR_REF ||
                   (place->type == AST_ARRAY_ACCESS && type_is_slice(ht))))
            return 1;
        place = place->children[0];
    }
    return place && place->type == AST_IDENTIFIER && place->value &&
           (strmap_has(&c->globals, place->value) || strmap_has(&c->state, place->value));
}

static void rewrite_binding(Ctx* c, Frame* f, ASTNode* n) {
    Local* l = pm_get(&f->binds, n);
    /* A pattern's local holds what it was bound from, as before: bound
     * again, it is given a view. */
    if (n->child_count > 0)
        rewrite_value(c, f, &n->children[0], l && l->kind == K_OTHER ? C_LEND : C_MOVE);
    if (n->child_count == 0 || !n->value) return;
    /* A store into a local, a cell, a module-level `var` or an actor's state
     * field gives back what it held; a declaration holds nothing yet. */
    int store;
    if (l) store = (holds(l) || l->kind == K_CELL) && n != l->decl;
    else store = n->type_inferred && type_is_slice(n->node_type) &&
                 (strmap_has(&c->cells, n->value) || strmap_has(&c->globals, n->value) ||
                  strmap_has(&c->state, n->value));
    if (!store) return;
    ASTNode* v = n->children[0];
    /* `xs = append(xs, v)` of that very local: `append` left it empty. */
    if (l && is_append(c, v) && annotation_has_marker(v->annotation, "slice_consumes") &&
        v->children[0]->type == AST_IDENTIFIER && pm_get(&f->uses, v->children[0]) == l)
        return;
    ASTNode* place = create_ast_node(AST_IDENTIFIER, n->value, n->line, n->column);
    place->node_type = clone_type(n->node_type ? n->node_type : (l ? l->type : NULL));
    if (n->source_file) place->source_file = strdup(n->source_file);
    if (l) pm_put(&f->uses, place, l);
    int shared = !l && (strmap_has(&c->globals, n->value) || strmap_has(&c->state, n->value));
    n->children[0] = mk_store(shared ? "__slice_set_shared" : "__slice_set", place->node_type,
                              place, v);
}

/* A call's fresh slice read in place, which nothing binds (`f().len`,
 * `f()[i]`, `for x in f()`, `f()[1..].len`, `f()` as a statement): the
 * reference it hands over is the statement's, released at its end (codegen
 * holds it in a temporary, `slice_stmt_temp`), where it used to leak. */
static void mark_in_place(ASTNode* e) {
    /* A view of the result used in place leaves it in place too. A view
     * moved somewhere takes the result's reference over (view_of_fresh),
     * so the base of a view is judged by how the view is used, never here
     * for the view's own sake (rewrite's default loop). A `match` arm's
     * value is the match's (rewrite_match_value). */
    if (e && is_arm_value(e)) return;
    while (e && e->type == AST_SLICE_EXPR && e->child_count > 0) e = e->children[0];
    if (!e || e->type != AST_FUNCTION_CALL || !type_is_slice(e->node_type) || !e->value ||
        strncmp(e->value, "__slice_", 8) == 0 ||
        annotation_has_marker(e->annotation, "slice_temp") ||
        annotation_has_marker(e->annotation, "slice_stmt_temp"))
        return;
    e->annotation = annotation_add_marker(e->annotation, "slice_stmt_temp");
}

/* At the start of `body`, the binding `l` takes a reference of its own
 * (`__slice_hold`), which every exit of `body` gives back. */
static void insert_hold(Local* l, ASTNode* body) {
    insert_child(body, 0, mk_drop_defer(l, body));
    ASTNode* hold = mk_call("__slice_hold", NULL, body);
    add_child(hold, mk_ident(l, body));
    ASTNode* es = create_ast_node(AST_EXPRESSION_STATEMENT, NULL, hold->line, hold->column);
    add_child(es, hold);
    if (body->source_file) es->source_file = strdup(body->source_file);
    insert_child(body, 0, es);
}

static void rewrite(Ctx* c, Frame* f, ASTNode** slot) {
    ASTNode* n = *slot;
    if (!n || is_frame(n) || is_opaque(n) || is_pattern(n)) return;
    switch (n->type) {
        case AST_BLOCK:
            /* A hoisted local that holds a reference is declared here, where
             * codegen would declare it, and released from there on. */
            for (int k = f->nlocals - 1; k >= 0; k--) {
                Local* h = f->locals[k];
                if (h->hoisted && holds(h) && h->block == n && !h->anchor) {
                    insert_child(n, 0, mk_drop_defer(h, n));
                    insert_child(n, 0, mk_hoisted_decl(h, n));
                }
            }
            for (int i = 0; i < n->child_count; i++) {
                ASTNode* ch = n->children[i];
                for (int k = 0; k < f->nlocals; k++) {
                    Local* h = f->locals[k];
                    if (h->hoisted && holds(h) && h->block == n && h->anchor == ch) {
                        insert_child(n, i, mk_drop_defer(h, ch));
                        insert_child(n, i, mk_hoisted_decl(h, ch));
                        i += 2;
                    }
                }
                rewrite(c, f, &n->children[i]);
                Local* l = ch ? pm_get(&f->binds, ch) : NULL;
                if (l && holds(l) && !l->param && l->decl == ch && l->block == n) {
                    insert_child(n, i + 1, mk_drop_defer(l, ch));
                    i++;
                }
                /* The slice locals a destructure declares (#3568). */
                for (int k = 0; ch && ch->type == AST_TUPLE_DESTRUCTURE && k + 1 < ch->child_count;
                     k++) {
                    Local* d = pm_get(&f->binds, ch->children[k]);
                    if (d && d->destructured && holds(d) && d->decl == ch->children[k] &&
                        d->block == n) {
                        insert_child(n, i + 1, mk_drop_defer(d, ch));
                        i++;
                    }
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
            /* A local that owns hands its reference to the caller; one a
             * view of which may be held (a `defer` may still read it) is
             * copied, and released at the exit. */
            for (int i = 0; i < n->child_count; i++)
                if (n->children[i]) rewrite_value(c, f, &n->children[i], C_MOVE);
            return;
        case AST_IF_EXPRESSION:
            /* Read where it stands (rewrite_value takes a moved one). */
            for (int i = 0; i < n->child_count; i++) rewrite(c, f, &n->children[i]);
            return;
        case AST_MATCH_ARM: {
            for (int i = 0; i < n->child_count; i++)
                if (n->children[i]) rewrite_value(c, f, &n->children[i], child_use(n, i));
            /* #3568: each slice the pattern binds takes a reference of its
             * own where the body starts, given back at the body's every
             * exit: the body may let go of what the match was given. */
            ASTNode* body = n->child_count > 1 ? n->children[1] : NULL;
            if (!body || body->type != AST_BLOCK) return;
            for (int k = f->nlocals - 1; k >= 0; k--) {
                Local* l = f->locals[k];
                if (l->arm == n && l->hold) insert_hold(l, body);
            }
            return;
        }
        case AST_STRUCT_LITERAL: case AST_TUPLE_LITERAL: case AST_MESSAGE_CONSTRUCTOR:
            for (int i = 0; i < n->child_count; i++) {
                ASTNode** slot2 = &n->children[i];
                if (!*slot2) continue;
                if (((*slot2)->type == AST_ASSIGNMENT || (*slot2)->type == AST_FIELD_INIT) &&
                    (*slot2)->child_count > 0)
                    slot2 = &(*slot2)->children[(*slot2)->child_count - 1];
                rewrite_value(c, f, slot2, C_MOVE);
            }
            return;
        case AST_BINARY_EXPRESSION:
            if (n->value && strcmp(n->value, "=") == 0 && n->child_count == 2) {
                rewrite(c, f, &n->children[0]);
                rewrite_value(c, f, &n->children[1], C_MOVE);
                /* A store into a field, an element, a module-level `var` or
                 * a state field gives back what the place held. */
                ASTNode* lhs = n->children[0];
                if (type_is_slice(lhs->node_type) && pure_place(lhs) && !c_field(c, lhs) &&
                    (lhs->type == AST_MEMBER_ACCESS || lhs->type == AST_ARRAY_ACCESS)) {
                    ASTNode* v = n->children[1];
                    n->children[1] = mk_store(shared_place(c, lhs) ? "__slice_set_shared"
                                                                   : "__slice_set",
                                              lhs->node_type, clone_ast_node(lhs), v);
                }
                return;
            }
            break;
        default:
            break;
    }
    for (int i = 0; i < n->child_count; i++) {
        if (!n->children[i]) continue;
        Use u = child_use(n, i);
        if (u == C_BORROW && n->type != AST_SLICE_EXPR) mark_in_place(n->children[i]);
        rewrite_value(c, f, &n->children[i], u);
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
    strmap_init(&f.promoted);
    Scope top;
    strmap_init(&top.names);
    top.up = NULL;
    top.depth = 0;
    int npar = 0;
    R r = {c, &f, NULL, NULL};
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
    views_of(c, &f, body, NULL, 0);

    {
        int before = c->errors;
        Flow w = {c, &f, f.nlocals, NULL, NULL};
        State s;
        st_init(&w, &s);
        flow(&w, body, &s, C_BORROW);
        st_free(&s);
        flow_memo_free(w.memo);
        if (c->errors == before) {
            ASTNode* b = body;
            rewrite(c, &f, &b);
            /* A `@keep` parameter gives back its reference as a local that
             * owns does. */
            for (int i = f.nlocals - 1; i >= 0; i--)
                if (f.locals[i]->kind == K_KEEP)
                    insert_child(body, 0, mk_drop_defer(f.locals[i], body));
            /* A receive arm's field its body writes holds a reference of
             * its own for the body (classify). */
            for (int i = f.nlocals - 1; i >= 0; i--) {
                Local* l = f.locals[i];
                if (l->hold && !l->arm) insert_hold(l, body);
            }
        }
    }

    /* The closures written here, each a frame of its own, with the cells of
     * this frame and of the frames it is written in, but for the names its
     * own locals take (#3549). */
    StrMap names;
    strmap_init(&names);
    if (outer)
        for (int i = 0; i < strmap_count(outer); i++)
            strmap_put(&names, strmap_key_at(outer, i), (void*)1);
    for (int i = 0; i < f.nlocals; i++) strmap_put(&names, f.locals[i]->name, (void*)1);
    StrMap saved_cells = c->cells;
    strmap_init(&c->cells);
    for (int i = 0; i < strmap_count(&saved_cells); i++) {
        const char* nm = strmap_key_at(&saved_cells, i);
        int own = 0;
        for (int k = 0; k < f.nlocals && !own; k++) own = strcmp(f.locals[k]->name, nm) == 0;
        if (!own) strmap_put(&c->cells, nm, (void*)1);
    }
    for (int i = 0; i < f.nlocals; i++)
        if (f.locals[i]->kind == K_CELL) strmap_put(&c->cells, f.locals[i]->name, (void*)1);
    for (int i = 0; i < body->child_count; i++) frame_nested(c, body->children[i], &names);
    strmap_free(&c->cells);
    c->cells = saved_cells;
    strmap_free(&names);

    for (int i = 0; i < f.nlocals; i++) {
        free(f.locals[i]->srcs);
        free(f.locals[i]);
    }
    free(f.locals);
    pm_free(&f.uses);
    pm_free(&f.binds);
    strmap_free(&f.promoted);
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

int check_slice_ownership(ASTNode* program) {
    if (!program) return 0;
    Ctx c;
    memset(&c, 0, sizeof(c));
    c.program = program;
    strmap_init(&c.globals);
    strmap_init(&c.state);
    strmap_init(&c.fns);
    strmap_init(&c.cells);
    strmap_init(&c.structs);
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* d = unwrap_export(program->children[i]);
        if (!d || !d->value) continue;
        if (d->type == AST_CONST_DECLARATION && d->annotation &&
            strcmp(d->annotation, "global_var") == 0)
            strmap_put(&c.globals, d->value, d);
        if (d->type == AST_STRUCT_DEFINITION && !strmap_has(&c.structs, d->value))
            strmap_put(&c.structs, d->value, d);
        if (d->type == AST_FUNCTION_DEFINITION || d->type == AST_BUILDER_FUNCTION) {
            if (!strmap_has(&c.fns, d->value)) strmap_put(&c.fns, d->value, d);
            if (strcmp(d->value, "append") == 0) c.has_append = 1;
        }
    }
    for (int i = 0; i < program->child_count; i++) walk_program(&c, program->children[i]);
    strmap_free(&c.globals);
    strmap_free(&c.state);
    strmap_free(&c.fns);
    strmap_free(&c.cells);
    strmap_free(&c.structs);
    pm_free(&c.reported);
    return c.errors;
}
