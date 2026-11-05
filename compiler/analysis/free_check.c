/* Use after free, double free and dead views (#1300). See free_check.h.
 *
 * The facts are "on every path" facts, so a join of two paths keeps a fact
 * only when both have it: freed and not freed is "maybe", which reports
 * nothing. That is what keeps the check free of false reports: it speaks
 * only where the fault happens whichever way the program goes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "free_check.h"
#include "contract_eval.h"
#include "missing_return.h"
#include "typechecker.h"
#include "../aether_error.h"

/* How often one variable's facts can rise at a loop's head (#3574). The
 * head only rises, as each pass joins the turn's end into it, and a join
 * raises a variable at most: once to appear, once for `freed` (to maybe),
 * once for `root` (to none), three times for `gone` (to maybe, to clean as
 * its root goes, to maybe again), and once each for `unknown`, `released`
 * and `rebound`. A pass that does not settle the head raises one, so a
 * head of n variables settles within FC_VAR_RISES * n passes. 256 passes
 * cut off a head that needs more (a chain of 300 views does), and past
 * them no local was followed, so a second free in the loop compiled. */
#define FC_VAR_RISES 9

enum { FC_CLEAN = 0, FC_FREED = 1, FC_MAYBE = 2 };

typedef struct {
    const char* name;
    int freed;              /* FC_CLEAN, FC_FREED (on every path), FC_MAYBE */
    ASTNode* freed_at;      /* the call that freed it, when FC_FREED */
    const char* root;       /* a view: the local whose storage it views */
    int gone;               /* a view's storage: FC_CLEAN, FC_FREED (gone), FC_MAYBE */
    ASTNode* gone_at;       /* the call that freed or moved it */
    int gone_moved;         /* it may have moved, rather than been freed */
    int alias;              /* a cast of the root: the same address, so freeing it frees the root */
    int unknown;            /* address taken, captured, deferred: not followed */
    /* A `@frees` parameter of the function walked: */
    int released;           /* freed, or found null, on every path */
    int rebound;            /* bound to something else on some path */
} FVar;

/* Every local the function binds (#3574): 128 slots left the rest
 * unfollowed, so a use after their free or a second free compiled. The
 * variables are on the heap: a state is copied with fs_set and released
 * with fs_release. */
typedef struct {
    FVar* v;
    int n;
    int cap;
    int dead;               /* no path reaches this point */
} FState;

typedef struct {
    ASTNode* program;
    int errors;
    ASTNode** reported;     /* nodes already reported, so each is once */
    int reported_n, reported_cap;
    /* The function walked: its `@frees` parameters' names and its node. */
    ASTNode* fn;
    const char** frees_params;   /* every one (#3574): past 16 one went unchecked */
    int frees_count;
    int frees_reported;
} Ctx;

typedef struct Loop {
    const char* label;
    int is_switch;
    FState brk;
    FState cont;
    struct Loop* up;
} Loop;

/* A join a loop's walk made into a loop or switch around it (a `break` or
 * `continue` leaving it): `up` places past the innermost one around the
 * loop, into its `cont` or its `brk`. */
typedef struct {
    int up;
    int to_cont;
    FState st;
} Escape;

/* A loop walked from `entry`, which left `exit` and made `esc`: the same
 * walk again is the same answer (LoopMemo, loop()). */
typedef struct LoopMemo {
    ASTNode* node;
    int report, carried;
    FState entry, exit;
    Escape* esc;
    int esc_n, esc_cap;
    struct LoopMemo* next;
} LoopMemo;

/* A loop's walk being recorded: the loops around it at its start, and the
 * memo its escapes go into. */
typedef struct Rec {
    Loop* base;
    LoopMemo* m;
    struct Rec* up;
} Rec;

typedef struct {
    Ctx* c;
    int report;
    int carried;            /* walking a loop's next turn */
    Loop* loops;
    LoopMemo* memo;         /* the loops walked, by entry state */
    Rec* rec;               /* the loop walks being recorded, innermost first */
} Walk;

/* ------------------------------------------------------------ the state */

static FVar* find(FState* s, const char* name) {
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->v[i].name, name) == 0) return &s->v[i];
    return NULL;
}

/* The entry for `name`, made clean when there is none. */
static FVar* get(FState* s, const char* name) {
    FVar* v = find(s, name);
    if (v) return v;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->v = (FVar*)realloc(s->v, sizeof(FVar) * (size_t)s->cap);
        if (!s->v) {
            fprintf(stderr, "aetherc: out of memory in the free check\n");
            exit(1);
        }
    }
    v = &s->v[s->n++];
    memset(v, 0, sizeof *v);
    v->name = name;
    return v;
}

static void join_var(FVar* a, const FVar* b) {
    a->unknown = a->unknown || b->unknown;
    if (a->freed != b->freed) a->freed = FC_MAYBE;
    if ((a->root == NULL) != (b->root == NULL) ||
        (a->root && strcmp(a->root, b->root) != 0)) {
        a->root = NULL;
        a->gone = FC_CLEAN;
    } else if (a->gone != b->gone) {
        a->gone = FC_MAYBE;
    } else if (a->gone == FC_FREED && b->gone_moved == 0) {
        a->gone_moved = 0;   /* freed on some path: say freed */
    }
    a->released = a->released && b->released;
    a->alias = a->alias && b->alias && a->root != NULL;
    a->rebound = a->rebound || b->rebound;
}

/* The facts of every path through `a` and `b`. A local missing from one is
 * clean there. */
/* `dst` becomes a copy of `src`: its own variables, the same facts. */
static void fs_set(FState* dst, const FState* src) {
    if (dst == src) return;
    if (dst->cap < src->n) {
        dst->cap = src->n;
        dst->v = (FVar*)realloc(dst->v, sizeof(FVar) * (size_t)dst->cap);
        if (!dst->v) {
            fprintf(stderr, "aetherc: out of memory in the free check\n");
            exit(1);
        }
    }
    if (src->n) memcpy(dst->v, src->v, sizeof(FVar) * (size_t)src->n);
    dst->n = src->n;
    dst->dead = src->dead;
}

static void join(FState* a, const FState* b) {
    if (b->dead) return;
    if (a->dead) { fs_set(a, b); return; }
    FVar clean;
    memset(&clean, 0, sizeof clean);
    for (int i = 0; i < a->n; i++) {
        FVar* av = &a->v[i];
        FVar* bv = find((FState*)b, av->name);
        if (bv) {
            join_var(av, bv);
        } else {
            clean.name = av->name;
            clean.released = 0;
            join_var(av, &clean);
        }
    }
    for (int i = 0; i < b->n; i++) {
        if (find(a, b->v[i].name)) continue;
        FVar* av = get(a, b->v[i].name);
        if (!av) continue;
        FVar copy = b->v[i];
        clean.name = copy.name;
        join_var(&copy, &clean);
        *av = copy;
    }
}

static int same_var(const FVar* a, const FVar* b) {
    return a->freed == b->freed && a->gone == b->gone && a->unknown == b->unknown &&
           a->released == b->released && a->rebound == b->rebound &&
           ((a->root == NULL && b->root == NULL) ||
            (a->root && b->root && strcmp(a->root, b->root) == 0));
}

static int same(FState* a, FState* b) {
    if (a->dead != b->dead) return 0;
    for (int i = 0; i < a->n; i++) {
        FVar* bv = find(b, a->v[i].name);
        if (!bv || !same_var(&a->v[i], bv)) return 0;
    }
    for (int i = 0; i < b->n; i++)
        if (!find(a, b->v[i].name)) return 0;
    return 1;
}

/* ------------------------------------------------------------ reporting */

static int already_reported(Ctx* c, ASTNode* at) {
    for (int i = 0; i < c->reported_n; i++)
        if (c->reported[i] == at) return 1;
    if (c->reported_n == c->reported_cap) {
        int cap = c->reported_cap ? c->reported_cap * 2 : 16;
        ASTNode** r = realloc(c->reported, sizeof(ASTNode*) * (size_t)cap);
        if (!r) return 0;
        c->reported = r;
        c->reported_cap = cap;
    }
    c->reported[c->reported_n++] = at;
    return 0;
}

static void report(Walk* w, ASTNode* at, int code, const char* message, const char* help) {
    if (!w->report || already_reported(w->c, at)) return;
    AetherError e = { at->source_file, NULL, at->line, at->column, message, help, NULL,
                      (AetherErrorCode)code };
    aether_error_report(&e);
    w->c->errors++;
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

/* ---------------------------------------------------------------- walk */

static void walk(Walk* w, ASTNode* n, FState* s);

static int is_null(ASTNode* n) {
    return n && n->type == AST_NULL_LITERAL;
}

/* An argument as the memory it is: a slice given where a pointer is taken
 * is its storage, and a cast (`w as ptr`, `p as *T`) is the same address. */
static ASTNode* arg_place(ASTNode* a) {
    while (a && (a->type == AST_SLICE_TO_PTR || a->type == AST_VALUE_CAST ||
                 a->type == AST_PTR_AS_STRUCT_CAST || a->type == AST_PTR_AS_ARRAY_CAST) &&
           a->child_count > 0)
        a = a->children[0];
    return a;
}

/* A read of local `id`. */
static void use(Walk* w, FState* s, ASTNode* id) {
    FVar* v = find(s, id->value);
    if (!v || v->unknown) return;
    const char* msg = ""; const char* help = "";
    if (v->freed == FC_FREED) {
        msg = aether_internf("'%s' is used after it was freed%s at line %d", v->name,
                 w->carried ? " on the loop's previous turn" : "", node_line(v->freed_at));
        help = aether_internf(
                 "use it before the free, or bind '%s' to a fresh value first", v->name);
        report(w, id, AETHER_ERR_USE_AFTER_FREE, msg, help);
        v->freed = FC_MAYBE;   /* once per path */
    } else if (v->root && v->gone == FC_FREED) {
        msg = aether_internf(
                 "'%s' views the storage of '%s', which %s%s at line %d", v->name, v->root,
                 v->gone_moved ? "may have moved" : "was freed",
                 w->carried ? " on the loop's previous turn" : "", node_line(v->gone_at));
        help = aether_internf(
                 "%s: take the view again after that call",
                 v->gone_moved ? "a call that can grow a buffer may move its storage, so a view "
                                 "taken before it no longer points at it"
                               : "a view lives only as long as the storage it views");
        report(w, id, AETHER_ERR_USE_AFTER_FREE, msg, help);
        v->gone = FC_MAYBE;
    }
}

/* Every view of `root`'s storage: gone, freed or moved, at `at`. */
static void storage_gone(FState* s, const char* root, ASTNode* at, int moved) {
    for (int i = 0; i < s->n; i++) {
        FVar* v = &s->v[i];
        if (v->root && strcmp(v->root, root) == 0 && v->gone != FC_FREED) {
            v->gone = FC_FREED;
            v->gone_at = at;
            v->gone_moved = moved;
        }
    }
}

/* Every local named in `n`: not followed from here (an address taken, a
 * closure's capture, a `defer`). */
static void unfollow_all(FState* s, ASTNode* n) {
    if (!n) return;
    if (n->type == AST_IDENTIFIER && n->value) {
        FVar* v = get(s, n->value);
        if (v) v->unknown = 1;
    }
    for (int i = 0; i < n->child_count; i++) unfollow_all(s, n->children[i]);
}

/* The local whose storage `init` views, or NULL: a slice of it, a cast of
 * it to a slice or a struct pointer, or a call's result that its `@into`
 * argument points into. A view of a view views the same storage. */
static const char* view_root(Walk* w, FState* s, ASTNode* init) {
    ASTNode* base = NULL;
    if (!init) return NULL;
    if ((init->type == AST_SLICE_EXPR || init->type == AST_PTR_AS_ARRAY_CAST ||
         init->type == AST_PTR_AS_STRUCT_CAST || init->type == AST_VALUE_CAST) &&
        init->child_count > 0) {
        base = arg_place(init->children[0]);
    } else if (init->type == AST_FUNCTION_CALL) {
        for (int i = 0; i < init->child_count && !base; i++) {
            int into = 0;
            tc_call_arg_effect(w->c->program, init, i, &into);
            ASTNode* a = arg_place(init->children[i]);
            if (into && a && a->type == AST_IDENTIFIER) base = a;
        }
    }
    if (!base || base->type != AST_IDENTIFIER || !base->value) return NULL;
    FVar* b = find(s, base->value);
    if (b && b->unknown) return NULL;
    return (b && b->root) ? b->root : base->value;
}

/* `name` is bound to what `init` gives. */
static void bind(Walk* w, FState* s, const char* name, ASTNode* init) {
    if (!name) return;
    const char* root = view_root(w, s, init);
    const char* from_name = NULL;
    if (init && root) {
        ASTNode* b = init->child_count > 0 ? arg_place(init->children[0]) : NULL;
        if (b && b->type == AST_IDENTIFIER && b->value) from_name = b->value;
    }
    /* Views of what `name` held view storage it no longer names. */
    for (int i = 0; i < s->n; i++)
        if (s->v[i].root && strcmp(s->v[i].root, name) == 0) {
            s->v[i].root = NULL;
            s->v[i].gone = FC_CLEAN;
        }
    FVar* v = get(s, name);
    if (!v) return;
    /* Found by name past get, which may move the variables (#3574): found
     * before it, they pointed into the array get had freed. */
    FVar* r = root ? find(s, root) : NULL;
    FVar* from = from_name ? find(s, from_name) : NULL;
    int was_frees = 0;
    for (int i = 0; i < w->c->frees_count; i++)
        if (strcmp(w->c->frees_params[i], name) == 0) was_frees = 1;
    v->freed = FC_CLEAN;
    v->freed_at = NULL;
    v->unknown = 0;
    v->root = root && strcmp(root, name) != 0 ? root : NULL;
    v->gone = FC_CLEAN;
    v->gone_at = NULL;
    v->gone_moved = 0;
    v->alias = v->root && init && (init->type == AST_PTR_AS_STRUCT_CAST ||
                                   init->type == AST_PTR_AS_ARRAY_CAST ||
                                   init->type == AST_VALUE_CAST);
    if (v->root) {
        /* A view of a view whose storage is gone, or of a freed local. */
        if (from && from->root && from->gone == FC_FREED) {
            v->gone = FC_FREED;
            v->gone_at = from->gone_at;
            v->gone_moved = from->gone_moved;
        } else if (r && r->freed == FC_FREED) {
            v->gone = FC_FREED;
            v->gone_at = r->freed_at;
        }
    }
    if (was_frees) {
        v->released = 0;
        v->rebound = 1;
    }
}

/* An argument the call frees, as the call is made: one freed already is
 * freed twice, a view of storage that is gone is read. */
static void free_call_check(Walk* w, FState* s, ASTNode* id) {
    FVar* v = get(s, id->value);
    if (!v || v->unknown) return;
    const char* msg = ""; const char* help = "";
    if (v->freed == FC_FREED) {
        msg = aether_internf("'%s' is freed again: it was freed%s at line %d", v->name,
                 w->carried ? " on the loop's previous turn" : "", node_line(v->freed_at));
        help = aether_internf(
                 w->carried ? "bind '%s' to a fresh value each turn before freeing it, or free it "
                              "once after the loop"
                            : "free '%s' once",
                 v->name);
        report(w, id, AETHER_ERR_USE_AFTER_FREE, msg, help);
    } else if (v->root && v->gone == FC_FREED) {
        use(w, s, id);
    }
}

/* What the call has done once it returns: the argument is freed, and with
 * it every view of its storage (and the root a cast of it is). */
static void free_call_mark(FState* s, ASTNode* call, ASTNode* id) {
    FVar* v = get(s, id->value);
    if (!v || v->unknown) return;
    v->freed = FC_FREED;
    v->freed_at = call;
    v->released = 1;
    storage_gone(s, v->name, call, 0);
    /* A cast is its root's address: the root is freed with it. */
    if (v->alias && v->root) {
        const char* root = v->root;
        FVar* r = get(s, root);
        if (r && !r->unknown) {
            r->freed = FC_FREED;
            r->freed_at = call;
            r->released = 1;
            storage_gone(s, root, call, 0);
        }
    }
}

/* A call: its arguments are evaluated first, in order; what it does to
 * them (frees, moves their storage) happens after, so a view passed beside
 * the buffer it views is read before the buffer may move
 * (`bytes.copy_from_slice(b, 2, bytes.view(b))`). A trailing block runs
 * inline, inside the call. */
static void walk_call(Walk* w, ASTNode* n, FState* s) {
    for (int i = 0; i < n->child_count && !s->dead; i++) {
        ASTNode* a = n->children[i];
        if (a && a->type == AST_CLOSURE && ast_is_trailing_block(a)) continue;
        ASTNode* place = arg_place(a);
        if (tc_call_arg_effect(w->c->program, n, i, NULL) == TC_ARG_FREES && place &&
            place->type == AST_IDENTIFIER && place->value) {
            free_call_check(w, s, place);
            continue;
        }
        walk(w, a, s);
    }
    for (int i = 0; i < n->child_count && !s->dead; i++) {
        ASTNode* a = n->children[i];
        if (!a || a->type != AST_CLOSURE || !ast_is_trailing_block(a)) continue;
        for (int k = a->child_count - 1; k >= 0; k--)
            if (a->children[k] && a->children[k]->type == AST_BLOCK) {
                walk(w, a->children[k], s);
                break;
            }
    }
    for (int i = 0; i < n->child_count && !s->dead; i++) {
        ASTNode* place = arg_place(n->children[i]);
        if (!place || place->type != AST_IDENTIFIER || !place->value) continue;
        int effect = tc_call_arg_effect(w->c->program, n, i, NULL);
        if (effect == TC_ARG_FREES) free_call_mark(s, n, place);
        else if (effect == TC_ARG_MOVES) storage_gone(s, place->value, n, 1);
    }
    if (ast_call_never_returns(n)) s->dead = 1;
}

/* At a function's end or a `return`: each `@frees` parameter must be gone. */
static void check_exit(Walk* w, FState* s, ASTNode* at) {
    Ctx* c = w->c;
    if (s->dead || !w->report || c->frees_reported) return;
    for (int i = 0; i < c->frees_count; i++) {
        FVar* v = find(s, c->frees_params[i]);
        int ok = v && v->released && !v->rebound;
        if (ok || (v && v->unknown)) continue;
        const char* msg = ""; const char* help = "";
        if (v && v->rebound)
            msg = aether_internf(
                     "'%s' is declared `@frees`, but it is bound to something else before it is "
                     "freed on a path to here",
                     c->frees_params[i]);
        else
            msg = aether_internf(
                     "'%s' is declared `@frees`, but a path reaches here without freeing it",
                     c->frees_params[i]);
        help = aether_internf(
                 "free it on every path (a null one need not be), or drop `@frees`: a caller "
                 "takes it as freed after the call");
        AetherError e = { at ? at->source_file : NULL, NULL, at ? at->line : 0,
                          at ? at->column : 0, msg, help, NULL, AETHER_ERR_FREES_MODE };
        aether_error_report(&e);
        c->errors++;
        c->frees_reported = 1;
        return;
    }
}

static Loop* loop_target(Walk* w, const char* label, int for_continue) {
    for (Loop* l = w->loops; l; l = l->up) {
        if (label) {
            if (!l->is_switch && l->label && strcmp(l->label, label) == 0) return l;
        } else if (!(for_continue && l->is_switch)) {
            return l;
        }
    }
    return NULL;
}

/* A join into the loop or switch `t` from inside a loop being recorded and
 * leaving it: the recording keeps it, by how far up from the loops around
 * the recorded loop `t` is, to make again when its walk is reused. A join
 * into a loop inside the recorded one is none of its business. */
static void escape_note(Walk* w, Loop* t, int to_cont, const FState* st) {
    for (Rec* r = w->rec; r; r = r->up) {
        int up = 0;
        Loop* l = r->base;
        while (l && l != t) {
            l = l->up;
            up++;
        }
        if (!l) continue;
        LoopMemo* m = r->m;
        /* One per place, its joins joined: a list of them grew with every
         * walk reused inside, by as many again at each level up. */
        Escape* same = NULL;
        for (int i = 0; i < m->esc_n && !same; i++)
            if (m->esc[i].up == up && m->esc[i].to_cont == to_cont) same = &m->esc[i];
        if (same) {
            join(&same->st, st);
            continue;
        }
        if (m->esc_n == m->esc_cap) {
            m->esc_cap = m->esc_cap ? m->esc_cap * 2 : 4;
            m->esc = (Escape*)realloc(m->esc, sizeof(Escape) * (size_t)m->esc_cap);
            if (!m->esc) {
                fprintf(stderr, "aetherc: out of memory in the free check\n");
                exit(1);
            }
        }
        Escape* e = &m->esc[m->esc_n++];
        memset(e, 0, sizeof *e);
        e->up = up;
        e->to_cont = to_cont;
        fs_set(&e->st, st);
    }
}

/* `st` joined into `t`'s `cont` or `brk`, and noted (escape_note). */
static void loop_join(Walk* w, Loop* t, int to_cont, const FState* st) {
    join(to_cont ? &t->cont : &t->brk, st);
    escape_note(w, t, to_cont, st);
}

static ContractTri decide(Walk* w, ASTNode* cond) {
    if (!cond) return CONTRACT_TRUE;
    ContractEnv env;
    memset(&env, 0, sizeof env);
    env.program = w->c->program;
    return contract_eval_predicate(cond, &env);
}

/* The copies of a state a branch, a loop or a switch needs are on the heap,
 * never in a frame: the walk recurses as deep as an expression nests (a
 * chain of 1000 `+ 1` is 1000 frames), and a frame holding even one state
 * overflowed the stack there. */
static FState* fs_new(const FState* src) {
    FState* d = (FState*)calloc(1, sizeof *d);
    if (!d) {
        fprintf(stderr, "aetherc: out of memory in the free check\n");
        exit(1);
    }
    fs_set(d, src);
    return d;
}

static FState* fs_dead(void) {
    FState* d = (FState*)calloc(1, sizeof *d);
    if (!d) {
        fprintf(stderr, "aetherc: out of memory in the free check\n");
        exit(1);
    }
    d->dead = 1;
    return d;
}

/* A state fs_new or fs_dead made, and its variables. */
static void fs_release(FState* s) {
    if (!s) return;
    free(s->v);
    free(s);
}

static Loop* loop_push(Walk* w, const char* label, int is_switch) {
    Loop* l = (Loop*)calloc(1, sizeof *l);
    if (!l) {
        fprintf(stderr, "aetherc: out of memory in the free check\n");
        exit(1);
    }
    l->label = label;
    l->is_switch = is_switch;
    l->brk.dead = 1;
    l->cont.dead = 1;
    l->up = w->loops;
    w->loops = l;
    return l;
}

/* A Loop and the states it holds by value. */
static void loop_free(Loop* l) {
    free(l->brk.v);
    free(l->cont.v);
    free(l);
}

static void loop_pop(Walk* w, Loop* l) {
    w->loops = l->up;
    loop_free(l);
}

/* One turn of a loop from `st`: its condition, body and step. The state at
 * the back edge goes to `back`, what its `break`s leave to `brk`. */
static void loop_turn(Walk* w, ASTNode* node, ASTNode* cond, ASTNode* body, ASTNode* step,
                      FState* st, FState* back, FState* brk) {
    walk(w, cond, st);
    Loop* l = loop_push(w, node->value, 0);
    FState* in = fs_new(st);
    walk(w, body, in);
    join(in, &l->cont);
    walk(w, step, in);
    fs_set(back, in);
    if (brk) fs_set(brk, &l->brk);
    fs_release(in);
    loop_pop(w, l);
}

static void loop_walk(Walk* w, FState* s, ASTNode* node, ASTNode* init, ASTNode* cond,
                      ASTNode* body, ASTNode* step) {
    walk(w, init, s);
    if (s->dead) return;
    int reporting = w->report, carried = w->carried;
    FState* head = fs_new(s);
    FState* st = fs_new(s);
    FState* back = fs_new(s);
    FState* next = fs_new(s);
    int unsettled = 0;
    for (int pass = 0; ; pass++) {
        w->report = 0;
        fs_set(st, head);
        loop_turn(w, node, cond, body, step, st, back, NULL);
        fs_set(next, head);
        join(next, back);
        if (same(next, head)) {
            fs_set(head, next);
            break;
        }
        if (pass >= FC_VAR_RISES * (next->n + 1)) {
            /* More rises than the facts have: a fault in the count above.
             * The loop is refused below, not passed unchecked. */
            unsettled = 1;
            for (int i = 0; i < next->n; i++) next->v[i].unknown = 1;
            fs_set(head, next);
            break;
        }
        fs_set(head, next);
    }
    /* The walk that reports, from the head. */
    w->report = reporting;
    if (unsettled) {
        const char* msg = "";
        msg = aether_internf(
                 "the free check found no settled state for this loop in %d passes",
                 FC_VAR_RISES * (next->n + 1));
        report(w, node, AETHER_ERR_USE_AFTER_FREE, msg,
               "this is a fault in the compiler: please report it with this loop");
    }
    FState* brk = fs_dead();
    fs_set(st, head);
    loop_turn(w, node, cond, body, step, st, back, brk);
    /* The next turn, from what this one leaves: a free it repeats. */
    if (reporting && !back->dead) {
        w->carried = 1;
        fs_set(next, back);
        loop_turn(w, node, cond, body, step, next, head, NULL);
        w->carried = carried;
    }
    /* Out of the loop: the condition is false, or a `break` left. */
    if (decide(w, cond) == CONTRACT_TRUE) st->dead = 1;
    join(st, brk);
    fs_set(s, st);
    fs_release(head);
    fs_release(st);
    fs_release(back);
    fs_release(next);
    fs_release(brk);
}

/* Two states alike in every fact of every variable, in the same order. */
static int fs_alike(const FState* a, const FState* b) {
    if (a->dead != b->dead || a->n != b->n) return 0;
    for (int i = 0; i < a->n; i++) {
        const FVar* x = &a->v[i];
        const FVar* y = &b->v[i];
        if (strcmp(x->name, y->name) != 0 || x->freed != y->freed || x->freed_at != y->freed_at ||
            x->gone != y->gone || x->gone_at != y->gone_at || x->gone_moved != y->gone_moved ||
            x->alias != y->alias || x->unknown != y->unknown || x->released != y->released ||
            x->rebound != y->rebound)
            return 0;
        if ((x->root == NULL) != (y->root == NULL) || (x->root && strcmp(x->root, y->root) != 0))
            return 0;
    }
    return 1;
}

/* A loop's walk from `s`. A loop walked once already from the same state,
 * reporting or not and on a next turn or not alike, gives the same answer
 * and makes the same joins into the loops around it, so that walk is
 * reused (#3574): a loop's walk walks its body two or three times (its
 * passes, the walk that reports, the next turn's), each walking the loops
 * inside, and loops nested 20 deep took seconds, doubling with each level.
 * Every walk that is not reused is kept (LoopMemo, freed with the
 * function's walk). */
static void loop(Walk* w, FState* s, ASTNode* node, ASTNode* init, ASTNode* cond,
                 ASTNode* body, ASTNode* step) {
    for (LoopMemo* m = w->memo; m; m = m->next) {
        if (m->node != node || m->report != w->report || m->carried != w->carried ||
            !fs_alike(&m->entry, s))
            continue;
        for (int i = 0; i < m->esc_n; i++) {
            const Escape* e = &m->esc[i];
            Loop* t = w->loops;
            for (int k = 0; k < e->up && t; k++) t = t->up;
            if (t) loop_join(w, t, e->to_cont, &e->st);
        }
        fs_set(s, &m->exit);
        return;
    }
    LoopMemo* m = (LoopMemo*)calloc(1, sizeof *m);
    if (!m) {
        fprintf(stderr, "aetherc: out of memory in the free check\n");
        exit(1);
    }
    m->node = node;
    m->report = w->report;
    m->carried = w->carried;
    fs_set(&m->entry, s);
    Rec r = { w->loops, m, w->rec };
    w->rec = &r;
    loop_walk(w, s, node, init, cond, body, step);
    w->rec = r.up;
    fs_set(&m->exit, s);
    m->next = w->memo;
    w->memo = m;
}

/* The loops a function's walk kept. */
static void loop_memo_free(LoopMemo* m) {
    while (m) {
        LoopMemo* next = m->next;
        free(m->entry.v);
        free(m->exit.v);
        for (int i = 0; i < m->esc_n; i++) free(m->esc[i].st.v);
        free(m->esc);
        free(m);
        m = next;
    }
}

static ASTNode* closure_body(ASTNode* closure) {
    for (int i = closure->child_count - 1; i >= 0; i--)
        if (closure->children[i] && closure->children[i]->type == AST_BLOCK)
            return closure->children[i];
    return NULL;
}

static void check_body(Ctx* c, ASTNode* fn, ASTNode* body);

/* `x == null`, `x != null` (either way round): the local `x`. */
static ASTNode* null_test(ASTNode* cond, int* eq) {
    if (!cond || cond->type != AST_BINARY_EXPRESSION || !cond->value || cond->child_count != 2)
        return NULL;
    int is_eq = strcmp(cond->value, "==") == 0, is_ne = strcmp(cond->value, "!=") == 0;
    if (!is_eq && !is_ne) return NULL;
    ASTNode* a = cond->children[0];
    ASTNode* b = cond->children[1];
    ASTNode* id = is_null(b) ? a : is_null(a) ? b : NULL;
    if (!id || id->type != AST_IDENTIFIER || !id->value) return NULL;
    *eq = is_eq;
    return id;
}

static void walk(Walk* w, ASTNode* n, FState* s) {
    if (!n || s->dead) return;
    switch (n->type) {
        case AST_VARIABLE_DECLARATION: {
            ASTNode* init = n->child_count > 0 ? n->children[0] : NULL;
            walk(w, init, s);
            if (!s->dead) bind(w, s, n->value, init);
            return;
        }
        case AST_TUPLE_DESTRUCTURE:
            for (int i = 0; i < n->child_count; i++) {
                ASTNode* k = n->children[i];
                if (k && k->type != AST_VARIABLE_DECLARATION) walk(w, k, s);
            }
            for (int i = 0; i < n->child_count; i++) {
                ASTNode* k = n->children[i];
                if (k && k->type == AST_VARIABLE_DECLARATION && k->value) bind(w, s, k->value, NULL);
            }
            return;
        case AST_IDENTIFIER:
            if (n->value) use(w, s, n);
            return;
        case AST_MEMBER_ACCESS:
            /* A slice's own length is no read of its storage. */
            if (n->value && strcmp(n->value, "len") == 0 && n->child_count > 0 && n->children[0] &&
                n->children[0]->type == AST_IDENTIFIER)
                return;
            break;
        case AST_UNARY_EXPRESSION:
            if (n->value && strcmp(n->value, "&") == 0) {
                unfollow_all(s, n);
                return;
            }
            break;
        case AST_BINARY_EXPRESSION: {
            int eq = 0;
            if (null_test(n, &eq)) return;   /* comparing a pointer with null reads no memory */
            if (n->value && (strcmp(n->value, "&&") == 0 || strcmp(n->value, "||") == 0) &&
                n->child_count == 2) {
                walk(w, n->children[0], s);
                if (s->dead) return;
                FState* r = fs_new(s);
                walk(w, n->children[1], r);
                join(s, r);
                fs_release(r);
                return;
            }
            break;
        }
        case AST_FUNCTION_CALL:
            walk_call(w, n, s);
            return;
        case AST_IF_STATEMENT:
        case AST_IF_EXPRESSION: {
            ASTNode* cond = n->child_count > 0 ? n->children[0] : NULL;
            walk(w, cond, s);
            if (s->dead) return;
            ContractTri known = cond ? decide(w, cond) : CONTRACT_UNKNOWN;
            FState* t = fs_new(s);
            FState* e = fs_new(s);
            if (known == CONTRACT_FALSE) t->dead = 1;
            if (known == CONTRACT_TRUE) e->dead = 1;
            /* A `@frees` parameter found null needs no free on that side. */
            int eq = 0;
            ASTNode* tested = null_test(cond, &eq);
            if (tested) {
                FVar* v = get(eq ? t : e, tested->value);
                if (v) v->released = 1;
            }
            walk(w, n->child_count > 1 ? n->children[1] : NULL, t);
            walk(w, n->child_count > 2 ? n->children[2] : NULL, e);
            fs_set(s, t);
            join(s, e);
            fs_release(t);
            fs_release(e);
            return;
        }
        case AST_WHILE_LOOP:
            loop(w, s, n, NULL, n->child_count > 0 ? n->children[0] : NULL,
                 n->child_count > 1 ? n->children[1] : NULL, NULL);
            return;
        case AST_FOR_LOOP:
            loop(w, s, n, n->child_count > 0 ? n->children[0] : NULL,
                 n->child_count > 1 ? n->children[1] : NULL,
                 n->child_count > 3 ? n->children[3] : NULL,
                 n->child_count > 2 ? n->children[2] : NULL);
            return;
        case AST_MATCH_STATEMENT: {
            walk(w, n->child_count > 0 ? n->children[0] : NULL, s);
            if (s->dead) return;
            FState* out = fs_new(s);
            FState* a = fs_new(s);
            if (ast_match_covers(n)) out->dead = 1;
            for (int i = 1; i < n->child_count; i++) {
                ASTNode* arm = n->children[i];
                if (!arm || arm->type != AST_MATCH_ARM) continue;
                fs_set(a, s);
                for (int k = 1; k < arm->child_count; k++) walk(w, arm->children[k], a);
                join(out, a);
            }
            fs_set(s, out);
            fs_release(out);
            fs_release(a);
            return;
        }
        case AST_SWITCH_STATEMENT: {
            walk(w, n->child_count > 0 ? n->children[0] : NULL, s);
            if (s->dead) return;
            FState* out = fs_new(s);
            FState* a = fs_new(s);
            Loop* l = loop_push(w, NULL, 1);
            for (int i = 1; i < n->child_count; i++) {
                ASTNode* cs = n->children[i];
                if (!cs || cs->type != AST_CASE_STATEMENT) continue;
                int is_default = cs->value && strcmp(cs->value, "default") == 0;
                if (is_default) out->dead = 1;
                fs_set(a, s);
                for (int k = is_default ? 0 : 1; k < cs->child_count; k++) walk(w, cs->children[k], a);
                join(out, a);
            }
            w->loops = l->up;
            join(out, &l->brk);
            if (!l->cont.dead) {
                Loop* t = loop_target(w, NULL, 1);
                if (t) loop_join(w, t, 1, &l->cont);
            }
            loop_free(l);
            fs_set(s, out);
            fs_release(out);
            fs_release(a);
            return;
        }
        case AST_RETURN_STATEMENT:
            for (int i = 0; i < n->child_count; i++) walk(w, n->children[i], s);
            check_exit(w, s, n);
            s->dead = 1;
            return;
        case AST_PANIC_STATEMENT:
            for (int i = 0; i < n->child_count; i++) walk(w, n->children[i], s);
            s->dead = 1;
            return;
        case AST_BREAK_STATEMENT: {
            Loop* t = loop_target(w, n->value, 0);
            if (t) loop_join(w, t, 0, s);
            s->dead = 1;
            return;
        }
        case AST_CONTINUE_STATEMENT: {
            Loop* t = loop_target(w, n->value, 1);
            if (t) loop_join(w, t, 1, s);
            s->dead = 1;
            return;
        }
        case AST_CLOSURE:
            if (ast_is_trailing_block(n)) {
                walk(w, closure_body(n), s);
                return;
            }
            /* A closure runs later: what it names is out of sight from here.
             * Its body is a function of its own. */
            unfollow_all(s, n);
            if (w->report && !w->carried) check_body(w->c, n, closure_body(n));
            return;
        case AST_QUOTE:
            return;
        case AST_DEFER_STATEMENT:
            unfollow_all(s, n);
            return;
        case AST_TRY_STATEMENT: {
            ASTNode* body = n->child_count > 0 ? n->children[0] : NULL;
            /* A panic can enter the catch from anywhere in the body. */
            FState* mid = fs_new(s);
            walk(w, body, s);
            join(mid, s);
            FState* a = fs_new(mid);
            for (int i = 1; i < n->child_count; i++) {
                ASTNode* cc = n->children[i];
                if (!cc || cc->type != AST_CATCH_CLAUSE) continue;
                fs_set(a, mid);
                for (int k = 0; k < cc->child_count; k++) walk(w, cc->children[k], a);
                join(s, a);
            }
            fs_release(mid);
            fs_release(a);
            return;
        }
        case AST_OR_ELSE: {
            walk(w, n->child_count > 0 ? n->children[0] : NULL, s);
            if (s->dead) return;
            FState* h = fs_new(s);
            for (int i = 1; i < n->child_count; i++) walk(w, n->children[i], h);
            join(s, h);
            fs_release(h);
            return;
        }
        case AST_NULL_COALESCE: {
            walk(w, n->child_count > 0 ? n->children[0] : NULL, s);
            if (s->dead) return;
            FState* r = fs_new(s);
            for (int i = 1; i < n->child_count; i++) walk(w, n->children[i], r);
            join(s, r);
            fs_release(r);
            return;
        }
        default:
            break;
    }
    for (int i = 0; i < n->child_count; i++) walk(w, n->children[i], s);
}

/* Whether `body` passes `name` to a parameter that may move or free it. */
static int passes_to_mover(Ctx* c, ASTNode* n, const char* name) {
    if (!n) return 0;
    if (n->type == AST_FUNCTION_CALL) {
        for (int i = 0; i < n->child_count; i++) {
            ASTNode* a = arg_place(n->children[i]);
            if (a && a->type == AST_IDENTIFIER && a->value && strcmp(a->value, name) == 0 &&
                tc_call_arg_effect(c->program, n, i, NULL) != 0)
                return 1;
        }
    }
    for (int i = 0; i < n->child_count; i++)
        if (passes_to_mover(c, n->children[i], name)) return 1;
    return 0;
}

static int is_param_node(ASTNode* p) {
    return p && (p->type == AST_PATTERN_VARIABLE || p->type == AST_CLOSURE_PARAM ||
                 p->type == AST_VARIABLE_DECLARATION);
}

static void check_body(Ctx* c, ASTNode* fn, ASTNode* body) {
    if (!body) return;
    /* The function's own `@frees` parameters, kept for the walk; a closure
     * walked inside it has its own. */
    ASTNode* saved_fn = c->fn;
    const char** saved_params = c->frees_params;
    int saved_count = c->frees_count, saved_reported = c->frees_reported;
    c->frees_params = (const char**)calloc((size_t)(fn ? fn->child_count : 0) + 1, sizeof(char*));
    if (!c->frees_params) {
        fprintf(stderr, "aetherc: out of memory in the free check\n");
        exit(1);
    }
    c->fn = fn;
    c->frees_count = 0;
    c->frees_reported = 0;
    for (int i = 0; fn && i < fn->child_count; i++) {
        ASTNode* p = fn->children[i];
        if (!is_param_node(p) || p == body || !p->value || !p->annotation) continue;
        if (strstr(p->annotation, "frees_param"))
            c->frees_params[c->frees_count++] = p->value;
        if (strstr(p->annotation, "moves_param") && !passes_to_mover(c, body, p->value)) {
            const char* msg = "";
            msg = aether_internf(
                     "'%s' is declared `@moves`, but no call here may move or free what it "
                     "points at",
                     p->value);
            AetherError e = { p->source_file, NULL, p->line, p->column, msg,
                              "drop `@moves`: a caller's views of it stay valid after the call",
                              NULL, AETHER_ERR_FREES_MODE };
            aether_error_report(&e);
            c->errors++;
        }
    }
    FState* s = fs_dead();
    s->dead = 0;
    Walk w;
    memset(&w, 0, sizeof w);
    w.c = c;
    w.report = 1;
    walk(&w, body, s);
    int end_line = body->end_line > 0 ? body->end_line : node_line(body);
    ASTNode at = *body;
    at.line = end_line;
    at.column = body->end_line > 0 ? body->end_column : 1;
    check_exit(&w, s, &at);
    fs_release(s);
    loop_memo_free(w.memo);
    c->fn = saved_fn;
    c->frees_count = saved_count;
    c->frees_reported = saved_reported;
    free(c->frees_params);
    c->frees_params = saved_params;
}

static ASTNode* unwrap_export(ASTNode* n) {
    if (n && n->type == AST_EXPORT_STATEMENT && n->child_count > 0) return n->children[0];
    return n;
}

static ASTNode* last_block(ASTNode* n) {
    for (int i = n->child_count - 1; i >= 0; i--)
        if (n->children[i] && n->children[i]->type == AST_BLOCK) return n->children[i];
    return NULL;
}

int check_frees(ASTNode* program) {
    if (!program) return 0;
    Ctx c;
    memset(&c, 0, sizeof c);
    c.program = program;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* d = unwrap_export(program->children[i]);
        if (!d) continue;
        switch (d->type) {
            case AST_FUNCTION_DEFINITION:
            case AST_BUILDER_FUNCTION:
            case AST_FUNCTION_CLAUSE:
                check_body(&c, d, last_block(d));
                break;
            case AST_MAIN_FUNCTION:
                check_body(&c, NULL, d->child_count > 0 ? d->children[0] : NULL);
                break;
            case AST_ACTOR_DEFINITION:
                for (int k = 0; k < d->child_count; k++) {
                    ASTNode* rs = d->children[k];
                    if (!rs || rs->type != AST_RECEIVE_STATEMENT) continue;
                    for (int j = 0; j < rs->child_count; j++) {
                        ASTNode* arm = rs->children[j];
                        if (arm && (arm->type == AST_RECEIVE_ARM || arm->type == AST_TIMEOUT_ARM) &&
                            arm->child_count >= 2)
                            check_body(&c, NULL, arm->children[1]);
                    }
                }
                break;
            default:
                break;
        }
    }
    free(c.reported);
    return c.errors;
}
