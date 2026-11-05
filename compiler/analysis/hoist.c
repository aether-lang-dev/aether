/* Shared hoisting rules -- see hoist.h (#2186). The logic here is what
 * codegen's hoist_if_branch_vars, hoist_common_vars and
 * hoist_loop_vars used to compute inline; they now call it, and so does the
 * typechecker. */

#include "hoist.h"
#include "../aether_strmap.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Type* hoist_decl_type(ASTNode* decl) {
    if (!decl) return NULL;
    Type* t = decl->node_type;
    if ((!t || t->kind == TYPE_VOID || t->kind == TYPE_UNKNOWN)
        && decl->child_count > 0 && decl->children[0] && decl->children[0]->node_type) {
        t = decl->children[0]->node_type;
    }
    return t;
}

/* Is `t` a number kind (numeric_join_type's)? */
static int is_number_type(Type* t) {
    Type* probe = t ? numeric_join_type(t, t) : NULL;
    if (!probe) return 0;
    free_type(probe);
    return 1;
}

/* #3064: what binding `n` of a name gives a join: its type when that is a
 * number, NULL otherwise. A declaration that writes its type is that type
 * (`*written`), and its value no literal arm; an inferred one is its
 * value's type until it has its own. `*arm` is the value a literal is read
 * from. */
static Type* join_part(const ASTNode* n, const ASTNode** arm, int* written) {
    Type* t = n->node_type;
    const ASTNode* v = n->type_inferred && n->child_count > 0 ? n->children[0] : NULL;
    if ((!t || t->kind == TYPE_UNKNOWN) && v) t = v->node_type;
    *arm = v;
    *written = !n->type_inferred;
    return is_number_type(t) ? t : NULL;
}

/* The join of a name's bindings, part by part. */
typedef struct {
    Type* acc;           /* the inferred parts' join */
    const ASTNode* arm;  /* the arm it is read by (join_arm_after) */
    int apart;           /* two inferred parts no type holds */
    Type* written;       /* #3070: the first written part */
} JoinFold;

/* One part `t` (arm `v`) into the fold. Inferred parts join as an
 * if-expression's branches do (branch_join_type), each part's value the
 * arm a literal is read from (join_arm_after): `x = -1` in one branch and
 * `x = u` (a uint32) in another is a long, `x = 0` beside it a uint32. Two
 * that no type holds (a signed one beside a uint64) are apart. #3070: a
 * written type is the variable's type, whatever the inferred parts are:
 * one the written type does not hold is the checker's to refuse
 * (ast_number_holds). No part (NULL) leaves the fold as it is. */
static void fold_add(JoinFold* f, Type* t, const ASTNode* v, int written) {
    if (!t) return;
    if (written) {
        if (!f->written) f->written = clone_type(t);
        return;
    }
    if (f->apart) return;
    if (!f->acc) {
        f->acc = clone_type(t);
        f->arm = v;
        return;
    }
    Type* j = branch_join_type(f->acc, f->arm, t, v);
    if (!j) {
        f->apart = 1;
        return;
    }
    free_type(f->acc);
    f->acc = j;
    f->arm = join_arm_after(f->arm, v);
}

/* What the fold joins to, a fresh Type: the written type, else the
 * inferred parts' join; NULL for none, or two apart. */
static Type* fold_result(const JoinFold* f) {
    if (f->written) return clone_type(f->written);
    return f->apart || !f->acc ? NULL : clone_type(f->acc);
}

static void fold_copy(JoinFold* to, const JoinFold* from) {
    to->acc = from->acc ? clone_type(from->acc) : NULL;
    to->arm = from->arm;
    to->apart = from->apart;
    to->written = from->written ? clone_type(from->written) : NULL;
}

static void fold_free(JoinFold* f) {
    if (f->acc) free_type(f->acc);
    if (f->written) free_type(f->written);
    memset(f, 0, sizeof(*f));
}

static void fold_bind(JoinFold* f, const ASTNode* n) {
    const ASTNode* v = NULL;
    int written = 0;
    Type* t = join_part(n, &v, &written);
    fold_add(f, t, v, written);
}

/* #3064: the bindings of one name under a HoistJoins' root, in the order
 * a walk of the root meets them, and what the walk typing them has made of
 * them.
 * The walk types binds[0..typed) in that order; their parts are final for
 * it, folded in `head`. A binding not typed yet changes only when the walk
 * types it, so the parts of binds[typed..count) that `join` read (`rest`,
 * those that are numbers) stand until then, and `join` with them. */
typedef struct {
    const ASTNode* bind;
    const ASTNode* arm;
    Type* part;
    int written;
} JoinRest;

typedef struct {
    const ASTNode** binds;
    int count;
    int cap;
    int typed;
    int unordered;        /* typed out of order, or twice: each ask joins them all */
    int started;          /* `head` and `rest` are kept from the first ask on */
    JoinFold head;
    JoinRest* rest;
    int rest_count;
    int rest_next;        /* rest[0..rest_next) are typed now, and in `head` */
    int fresh;            /* `join` is the join of `head` and the rest */
    Type* join;
} HoistNameJoin;

struct HoistJoins {
    StrMap names;   /* name -> HoistNameJoin* */
};

static void joins_collect(ASTNode* n, HoistJoins* j) {
    if (!n) return;
    if (n->type == AST_CLOSURE || n->type == AST_QUOTE) return;   /* its own function */
    if (n->type == AST_VARIABLE_DECLARATION && n->value) {
        HoistNameJoin* nj = (HoistNameJoin*)strmap_get(&j->names, n->value);
        if (!nj) {
            nj = (HoistNameJoin*)calloc(1, sizeof(HoistNameJoin));
            if (!nj) return;
            strmap_put(&j->names, n->value, nj);
        }
        if (nj->count == nj->cap) {
            nj->cap = nj->cap ? nj->cap * 2 : 4;
            nj->binds = (const ASTNode**)aether_xrealloc((void*)nj->binds,
                                                         (size_t)nj->cap * sizeof(ASTNode*));
        }
        nj->binds[nj->count++] = n;
    }
    for (int i = 0; i < n->child_count; i++) joins_collect(n->children[i], j);
}

HoistJoins* hoist_joins_new(ASTNode* root) {
    HoistJoins* j = (HoistJoins*)calloc(1, sizeof(HoistJoins));
    if (!j) return NULL;
    strmap_init(&j->names);
    for (int i = 0; root && i < root->child_count; i++) joins_collect(root->children[i], j);
    return j;
}

static void name_join_forget(HoistNameJoin* nj) {
    fold_free(&nj->head);
    for (int i = 0; i < nj->rest_count; i++) free_type(nj->rest[i].part);
    free(nj->rest);
    nj->rest = NULL;
    nj->rest_count = nj->rest_next = 0;
    if (nj->join) free_type(nj->join);
    nj->join = NULL;
    nj->fresh = 0;
}

void hoist_joins_typed(HoistJoins* j, const ASTNode* decl) {
    HoistNameJoin* nj = j && decl && decl->value
                        ? (HoistNameJoin*)strmap_get(&j->names, decl->value) : NULL;
    if (!nj || nj->unordered) return;
    if (nj->typed >= nj->count || nj->binds[nj->typed] != decl) {
        /* Typed again, or before a binding written ahead of it (one in
         * another's value): every ask joins them all from here on. */
        nj->unordered = 1;
        name_join_forget(nj);
        return;
    }
    nj->typed++;
    if (!nj->started) return;
    const ASTNode* v = NULL;
    int written = 0;
    Type* t = join_part(decl, &v, &written);
    fold_add(&nj->head, t, v, written);
    /* Its part as `join` read it: none, or the one `rest` holds. */
    JoinRest* r = nj->rest_next < nj->rest_count && nj->rest[nj->rest_next].bind == decl
                  ? &nj->rest[nj->rest_next] : NULL;
    if (r) nj->rest_next++;
    if (nj->fresh && (r ? !t || !types_equal(t, r->part) : t != NULL)) nj->fresh = 0;
}

Type* hoist_joins_get(HoistJoins* j, const char* name) {
    HoistNameJoin* nj = j && name ? (HoistNameJoin*)strmap_get(&j->names, name) : NULL;
    if (!nj) return NULL;
    JoinFold f;
    memset(&f, 0, sizeof(f));
    if (nj->unordered) {
        for (int i = 0; i < nj->count; i++) fold_bind(&f, nj->binds[i]);
        Type* r = fold_result(&f);
        fold_free(&f);
        return r;
    }
    if (!nj->started) {
        nj->started = 1;
        if (nj->count > nj->typed)
            nj->rest = (JoinRest*)aether_xrealloc(NULL, (size_t)(nj->count - nj->typed) *
                                                            sizeof(JoinRest));
        for (int i = 0; i < nj->count; i++) {
            const ASTNode* v = NULL;
            int written = 0;
            Type* t = join_part(nj->binds[i], &v, &written);
            if (i < nj->typed) {
                fold_add(&nj->head, t, v, written);
            } else if (t) {
                JoinRest* r = &nj->rest[nj->rest_count++];
                r->bind = nj->binds[i];
                r->arm = v;
                r->part = clone_type(t);
                r->written = written;
            }
        }
    }
    if (!nj->fresh) {
        fold_copy(&f, &nj->head);
        for (int i = nj->rest_next; i < nj->rest_count; i++)
            fold_add(&f, nj->rest[i].part, nj->rest[i].arm, nj->rest[i].written);
        if (nj->join) free_type(nj->join);
        nj->join = fold_result(&f);
        fold_free(&f);
        nj->fresh = 1;
    }
    return nj->join ? clone_type(nj->join) : NULL;
}

void hoist_joins_free(HoistJoins* j) {
    if (!j) return;
    for (int i = 0; i < strmap_count(&j->names); i++) {
        HoistNameJoin* nj = (HoistNameJoin*)strmap_value_at(&j->names, i);
        if (!nj) continue;
        name_join_forget(nj);
        free((void*)nj->binds);
        free(nj);
    }
    strmap_free(&j->names);
    free(j);
}

/* ---- #3083: one walk per body or statement ------------------------------
 * The questions below are asked once per name, and a generated body binds
 * thousands of names: walking the body for each was quadratic in them. Each
 * answer is read from what one walk of the body (or the statement) gathered,
 * kept until the next checker or codegen run (hoist_cache_reset). A walk
 * gathers the tree's shape, which neither pass changes after codegen's
 * renames; a binding's type, which the checker fills in as it goes, is read
 * when it is asked for. The answers are kept by node, and a node's go with
 * it (hoist_node_freed): one allocated later at its address is a new node. */

struct HoistNodeCache {
    HoistJoins* binds;   /* as a body: its bindings by name */
    HoistNames* branch;  /* as a body: hoist_if_branch_hoists */
    HoistNames* common;  /* as a statement: hoist_common_hoists */
};

/* Open addressing over the node's address; a removed slot is a tombstone
 * until the table next grows. */
typedef struct {
    const ASTNode* node;
    struct HoistNodeCache* cache;
} CacheSlot;

#define CACHE_TOMBSTONE ((const ASTNode*)(uintptr_t)1)

static CacheSlot* g_cache;
static size_t g_cache_cap;   /* a power of two, or 0 */
static size_t g_cache_used;  /* live and tombstone slots */

static size_t cache_hash(const ASTNode* n) {
    uint64_t h = (uint64_t)(uintptr_t)n;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (size_t)h;
}

static CacheSlot* cache_slot(const ASTNode* n) {
    if (!g_cache_cap) return NULL;
    for (size_t i = cache_hash(n) & (g_cache_cap - 1);; i = (i + 1) & (g_cache_cap - 1)) {
        if (g_cache[i].node == n) return &g_cache[i];
        if (!g_cache[i].node) return NULL;
    }
}

static void names_free(HoistNames* h) {
    if (!h) return;
    free((void*)h->names);
    free(h->decls);
    strmap_free(&h->at);
    free(h);
}

static void cache_free(struct HoistNodeCache* c) {
    hoist_joins_free(c->binds);
    names_free(c->branch);
    names_free(c->common);
    free(c);
}

static struct HoistNodeCache* node_cache(ASTNode* n) {
    CacheSlot* s = cache_slot(n);
    if (s) return s->cache;
    if ((g_cache_used + 1) * 2 > g_cache_cap) {
        size_t cap = g_cache_cap ? g_cache_cap * 2 : 256;
        CacheSlot* grown = (CacheSlot*)calloc(cap, sizeof(CacheSlot));
        if (!grown) {
            fprintf(stderr, "Fatal: out of memory in the hoisting cache\n");
            exit(1);
        }
        size_t used = 0;
        for (size_t i = 0; i < g_cache_cap; i++) {
            const ASTNode* k = g_cache[i].node;
            if (!k || k == CACHE_TOMBSTONE) continue;
            size_t j = cache_hash(k) & (cap - 1);
            while (grown[j].node) j = (j + 1) & (cap - 1);
            grown[j] = g_cache[i];
            used++;
        }
        free(g_cache);
        g_cache = grown;
        g_cache_cap = cap;
        g_cache_used = used;
    }
    struct HoistNodeCache* c = (struct HoistNodeCache*)calloc(1, sizeof(*c));
    if (!c) {
        fprintf(stderr, "Fatal: out of memory in the hoisting cache\n");
        exit(1);
    }
    size_t j = cache_hash(n) & (g_cache_cap - 1);
    while (g_cache[j].node && g_cache[j].node != CACHE_TOMBSTONE) j = (j + 1) & (g_cache_cap - 1);
    if (!g_cache[j].node) g_cache_used++;
    g_cache[j].node = n;
    g_cache[j].cache = c;
    return c;
}

void hoist_node_freed(const ASTNode* n) {
    CacheSlot* s = cache_slot(n);
    if (!s) return;
    cache_free(s->cache);
    s->node = CACHE_TOMBSTONE;
    s->cache = NULL;
}

void hoist_cache_reset(void) {
    for (size_t i = 0; i < g_cache_cap; i++) {
        if (g_cache[i].node && g_cache[i].node != CACHE_TOMBSTONE) cache_free(g_cache[i].cache);
    }
    free(g_cache);
    g_cache = NULL;
    g_cache_cap = 0;
    g_cache_used = 0;
}

/* The bindings of `name` under `root` (closures excluded), in the order a
 * walk of the body meets them; NULL for none. */
static const HoistNameJoin* body_binds(ASTNode* root, const char* name) {
    if (!root || !name) return NULL;
    struct HoistNodeCache* c = node_cache(root);
    if (!c->binds) c->binds = hoist_joins_new(root);
    if (!c->binds) return NULL;
    return (const HoistNameJoin*)strmap_get(&c->binds->names, name);
}

Type* hoist_numeric_join(ASTNode* root, const char* name) {
    const HoistNameJoin* nj = body_binds(root, name);
    JoinFold f;
    memset(&f, 0, sizeof(f));
    for (int i = 0; nj && i < nj->count; i++) fold_bind(&f, nj->binds[i]);
    Type* r = fold_result(&f);
    fold_free(&f);
    return r;
}

Type* hoist_written_type(ASTNode* root, const char* name) {
    const HoistNameJoin* nj = body_binds(root, name);
    for (int i = 0; nj && i < nj->count; i++) {
        const ASTNode* n = nj->binds[i];
        if (!n->type_inferred && n->node_type && n->node_type->kind != TYPE_UNKNOWN &&
            n->node_type->kind != TYPE_VOID)
            return clone_type(n->node_type);
    }
    return NULL;
}

/* Over the inferred bindings of `name` under `root`: sets `*strings` when
 * one is a string, `*others` when one is neither a string nor `null`. */
static void null_string_binds(ASTNode* root, const char* name, int* strings, int* others) {
    const HoistNameJoin* nj = body_binds(root, name);
    for (int i = 0; nj && i < nj->count; i++) {
        ASTNode* n = (ASTNode*)nj->binds[i];
        if (!n->type_inferred) continue;
        ASTNode* init = n->child_count > 0 ? n->children[0] : NULL;
        Type* t = hoist_decl_type(n);
        if (init && init->type == AST_NULL_LITERAL) {
            /* `null` fits any pointer kind, a string included */
        } else if (t && t->kind == TYPE_STRING) {
            *strings = 1;
        } else {
            *others = 1;
        }
    }
}

/* #2980 review: the inferred bindings of `name` under `root` that are
 * function values, joined with `acc` by fn_type_mode_join. */
static Type* fn_join_binds(ASTNode* root, const char* name, Type* acc) {
    const HoistNameJoin* nj = body_binds(root, name);
    for (int i = 0; nj && i < nj->count; i++) {
        ASTNode* n = (ASTNode*)nj->binds[i];
        if (!n->type_inferred) continue;
        Type* j = fn_type_mode_join(acc, hoist_decl_type(n));
        if (j) { free_type(acc); acc = j; }
    }
    return acc;
}

/* #3153: among the bindings of `name` under `root`, the optional of `seed`
 * (a binding of the plain type), or NULL when none binds one. */
static Type* optional_of_binds(ASTNode* root, const char* name, Type* seed) {
    const HoistNameJoin* nj = body_binds(root, name);
    for (int i = 0; nj && i < nj->count; i++) {
        Type* t = hoist_decl_type((ASTNode*)nj->binds[i]);
        if (t && t->kind == TYPE_OPTIONAL && t->element_type && types_equal(t->element_type, seed))
            return clone_type(t);
    }
    return NULL;
}

Type* hoist_join_type(ASTNode* scope_body, const char* name, Type* seed) {
    if (!scope_body || !seed) return NULL;
    /* #3070: a type written for one binding is the local's type, whatever
     * the others are: `x = "s"` in one branch beside `int x = 1` in the
     * other made `x` the string's `const char*`, and the int was stored
     * into it. The checker refuses a binding the written type does not
     * hold (report_unheld_binding). */
    Type* written = hoist_written_type(scope_body, name);
    if (written) {
        if (!types_equal(written, seed)) return written;
        free_type(written);
        return NULL;
    }
    /* #2980 review: a local bound to functions in two branches is called
     * through the stronger mode at each parameter, where either may stand;
     * typed from the first binding alone, a `@keep` in the other was lost. */
    if (seed->kind == TYPE_FUNCTION) {
        Type* acc = fn_join_binds(scope_body, name, clone_type(seed));
        if (acc && types_equal(acc, seed)) { free_type(acc); return NULL; }
        return acc;
    }
    /* #3153: a `T` beside a `T?` is the `T?`: the value wraps into it as it
     * does into an optional local bound twice in one block, and the optional
     * fits no `T`. Typed from a first binding of the `T`, the C variable was
     * a `const char*` the other binding's `ae_opt_string` could not go in. */
    if (seed->kind != TYPE_OPTIONAL) {
        Type* opt = optional_of_binds(scope_body, name, seed);
        if (opt) return opt;
    }
    /* #3032: a `null` binding names no type of its own. A local hoisted
     * from `t = null` in one branch and `t = "b"` in another is a string,
     * as the same two bindings in one block make it. Joined as the `ptr` of
     * its first binding, it was a `ptr` that codegen managed as a string,
     * which a `string` local could not take (E0202). */
    if (seed->kind == TYPE_PTR && !seed->element_type) {
        int strings = 0, others = 0;
        null_string_binds(scope_body, name, &strings, &others);
        if (strings && !others) return create_type(TYPE_STRING);
    }
    if (!is_number_type(seed)) return NULL;   /* not a numeric kind: nothing to join */
    /* #3064: the bindings' own join, as an if-expression's; with two that
     * no type holds, the seed stands and the checker refuses the other
     * (sibling_binding_other_type). #3070: a written type is the join, and
     * the checker refuses a binding it does not hold. */
    Type* acc = hoist_numeric_join(scope_body, name);
    if (acc && types_equal(acc, seed)) { free_type(acc); return NULL; }
    return acc;
}

/* The arms of a branching statement: an if's then and else blocks, a
 * switch's case statements, whose children (after a case's selector) are
 * its body, or a match's arm bodies. A case's selector is never a
 * declaration, so the case node itself is the body whose direct
 * declarations are read (#2732). A match arm that binds a name has a
 * block body (the parser makes `1 -> v = 10` the block `{ v = 10 }`); any
 * other arm body is a value or a statement that binds nothing, and counts
 * as an arm all the same (#2768). Returns the count written to `arms` (at
 * most `cap`), 0 for any other statement. */
static int branch_arms(ASTNode* stmt, ASTNode** arms, int cap);

/* Every arm of `stmt`, malloc'd for the caller to free, *n of them (#3574):
 * 256 slots left the arms past them out of every hoist, so a name bound
 * only there was declared nowhere. NULL with *n 0 for no arms. */
static ASTNode** branch_arms_all(ASTNode* stmt, int* n) {
    *n = 0;
    if (!stmt || stmt->child_count == 0) return NULL;
    ASTNode** arms = (ASTNode**)aether_xrealloc(NULL, sizeof(ASTNode*) * (size_t)stmt->child_count);
    *n = branch_arms(stmt, arms, stmt->child_count);
    return arms;
}

static int branch_arms(ASTNode* stmt, ASTNode** arms, int cap) {
    int n = 0;
    if (!stmt) return 0;
    if (stmt->type == AST_IF_STATEMENT) {
        for (int j = 1; j < stmt->child_count && j < 3 && n < cap; j++)
            arms[n++] = stmt->children[j];
    } else if (stmt->type == AST_SWITCH_STATEMENT) {
        for (int j = 1; j < stmt->child_count && n < cap; j++) {
            ASTNode* c = stmt->children[j];
            if (c && c->type == AST_CASE_STATEMENT) arms[n++] = c;
        }
    } else if (stmt->type == AST_MATCH_STATEMENT) {
        for (int j = 1; j < stmt->child_count && n < cap; j++) {
            ASTNode* a = stmt->children[j];
            if (a && a->type == AST_MATCH_ARM && a->child_count > 1 && a->children[1])
                arms[n++] = a->children[1];
        }
    }
    return n;
}

/* Is `stmt` a statement whose arms branch_arms lists? */
static int is_branching(ASTNode* stmt) {
    return stmt && (stmt->type == AST_IF_STATEMENT || stmt->type == AST_SWITCH_STATEMENT ||
                    stmt->type == AST_MATCH_STATEMENT);
}

/* Each name the arm pattern `p` binds, to `visit` until it returns nonzero
 * (which this returns): `some(name)`, an element of a list pattern (`[name,
 * y]`, `[name | t]`), or a field of a struct pattern (`Pt { x: name }`). A
 * bare name in a pattern is a value compared with, or a sum's variant, and
 * binds nothing. */
typedef int (*BoundNameVisit)(const char* name, void* user);

static int pattern_each_bound(const ASTNode* p, BoundNameVisit visit, void* user) {
    if (!p) return 0;
    if (p->type == AST_PATTERN_VARIABLE)
        return p->value && p->annotation && strcmp(p->annotation, "some_pattern") == 0 &&
               visit(p->value, user);
    /* #2992: a struct pattern binds the names its fields take, and those of
     * a struct pattern over a field. */
    if (p->type == AST_PATTERN_STRUCT) {
        for (int i = 0; i < p->child_count; i++) {
            const ASTNode* fp = p->children[i];
            const ASTNode* sub = (fp && fp->type == AST_ASSIGNMENT && fp->child_count > 0)
                                 ? fp->children[0] : NULL;
            if (!sub) continue;
            if (sub->type == AST_PATTERN_VARIABLE && sub->value && visit(sub->value, user))
                return 1;
            if (sub->type == AST_PATTERN_STRUCT && pattern_each_bound(sub, visit, user)) return 1;
        }
        return 0;
    }
    if (p->type != AST_PATTERN_LIST && p->type != AST_PATTERN_CONS) return 0;
    for (int i = 0; i < p->child_count; i++) {
        const ASTNode* e = p->children[i];
        if (e && e->type == AST_PATTERN_VARIABLE && e->value && visit(e->value, user)) return 1;
    }
    return 0;
}

static int is_name(const char* name, void* want) { return strcmp(name, (const char*)want) == 0; }

static int pattern_binds_name(const ASTNode* p, const char* name) {
    return name && pattern_each_bound(p, is_name, (void*)name);
}

/* #2768: does an arm pattern of `stmt`, a match, bind `name`? Such a name
 * is the arm's own, and is not hoisted out of the match by any rule: a
 * binding of it in that arm rebinds the pattern's value, and one in another
 * arm stays in its arm too, so a read after the match is no read of either
 * (the checker's "Undefined variable"). */
static int match_pattern_binds(const ASTNode* stmt, const char* name) {
    if (!stmt || stmt->type != AST_MATCH_STATEMENT || !name) return 0;
    for (int i = 1; i < stmt->child_count; i++) {
        const ASTNode* a = stmt->children[i];
        if (a && a->type == AST_MATCH_ARM && a->child_count > 0 &&
            pattern_binds_name(a->children[0], name))
            return 1;
    }
    return 0;
}


static HoistNames* names_new(int per) {
    HoistNames* h = (HoistNames*)calloc(1, sizeof(HoistNames));
    if (!h) return NULL;
    h->per = per;
    strmap_init(&h->at);
    return h;
}

/* Add `name` (one not in `h` yet), its bindings unset; returns its index. */
static int names_push(HoistNames* h, const char* name) {
    if (h->count == h->cap) {
        int cap = h->cap ? h->cap * 2 : 8;
        h->names = (const char**)aether_xrealloc((void*)h->names, (size_t)cap * sizeof(char*));
        h->decls = (ASTNode**)aether_xrealloc(h->decls, (size_t)cap * (size_t)h->per *
                                                        sizeof(ASTNode*));
        memset(h->decls + (size_t)h->cap * (size_t)h->per, 0,
               (size_t)(cap - h->cap) * (size_t)h->per * sizeof(ASTNode*));
        h->cap = cap;
    }
    h->names[h->count] = name;
    strmap_put(&h->at, name, (void*)(intptr_t)(h->count + 1));
    return h->count++;
}

int hoist_names_find(const HoistNames* h, const char* name) {
    void* at = h && name ? strmap_get(&h->at, name) : NULL;
    return at ? (int)(intptr_t)at - 1 : -1;
}

ASTNode* hoist_names_decl(const HoistNames* h, int i) {
    if (!h || i < 0 || i >= h->count) return NULL;
    ASTNode* any = NULL;
    for (int k = 0; k < h->per; k++) {
        ASTNode* d = h->decls[(size_t)i * (size_t)h->per + (size_t)k];
        if (!d) continue;
        if (!any) any = d;
        Type* t = hoist_decl_type(d);
        if (t && t->kind != TYPE_VOID && t->kind != TYPE_UNKNOWN) return d;
    }
    return any;
}

static int put_name(const char* name, void* out) {
    strmap_put((StrMap*)out, name, (void*)1);
    return 0;
}

static void pattern_bound_names(const ASTNode* p, StrMap* out) {
    pattern_each_bound(p, put_name, out);
}

/* The names the arm patterns of `stmt`, a match, bind (match_pattern_binds
 * for every name at once). */
static void pattern_names(const ASTNode* stmt, StrMap* out) {
    if (!stmt || stmt->type != AST_MATCH_STATEMENT) return;
    for (int i = 1; i < stmt->child_count; i++) {
        const ASTNode* a = stmt->children[i];
        if (a && a->type == AST_MATCH_ARM && a->child_count > 0)
            pattern_bound_names(a->children[0], out);
    }
}

/* Every identifier name under `n` (closures included: any use counts). */
static void identifier_names(const ASTNode* n, StrMap* out) {
    if (!n) return;
    if (n->type == AST_IDENTIFIER && n->value) strmap_put(out, n->value, (void*)1);
    for (int i = 0; i < n->child_count; i++) identifier_names(n->children[i], out);
}

static HoistNames* branch_hoists_build(ASTNode* body) {
    HoistNames* all = names_new(1);
    HoistNames* kept = names_new(1);
    if (!all || !kept) { names_free(all); names_free(kept); return NULL; }
    /* Names a top-level statement declares get their own function-scope
     * declaration (and heap tracker) where they are; hoisting them from an
     * arm as well would declare them twice. Only a name read outside the
     * arms -- by a top-level statement that does not branch, or by an if's
     * condition or a switch's or a match's subject -- is hoisted. */
    StrMap top, outside;
    strmap_init(&top);
    strmap_init(&outside);
    for (int i = 0; i < body->child_count; i++) {
        ASTNode* child = body->children[i];
        if (!child) continue;
        if (child->type == AST_VARIABLE_DECLARATION && child->value)
            strmap_put(&top, child->value, (void*)1);
        if (is_branching(child)) {
            if (child->child_count > 0) identifier_names(child->children[0], &outside);
        } else {
            identifier_names(child, &outside);
        }
    }
    /* Each name an arm binds, in the order the arms bind them, with its
     * first binding in an arm of a statement whose patterns do not bind it
     * (a name a match's pattern binds comes out of no arm of that match). */
    for (int i = 0; i < body->child_count; i++) {
        ASTNode* stmt = body->children[i];
        int na = 0;
        ASTNode** arms = branch_arms_all(stmt, &na);
        if (na == 0) {
            free(arms);
            continue;
        }
        StrMap bound;
        strmap_init(&bound);
        pattern_names(stmt, &bound);
        for (int j = 0; j < na; j++) {
            for (int k = 0; arms[j] && k < arms[j]->child_count; k++) {
                ASTNode* d = arms[j]->children[k];
                if (!d || d->type != AST_VARIABLE_DECLARATION || !d->value) continue;
                int at = hoist_names_find(all, d->value);
                if (at < 0) at = names_push(all, d->value);
                if (!all->decls[at] && !strmap_has(&bound, d->value)) all->decls[at] = d;
            }
        }
        strmap_free(&bound);
        free(arms);
    }
    for (int n = 0; n < all->count; n++) {
        const char* name = all->names[n];
        if (strmap_has(&top, name) || !strmap_has(&outside, name) || !all->decls[n]) continue;
        int at = names_push(kept, name);
        kept->decls[at] = all->decls[n];
    }
    strmap_free(&top);
    strmap_free(&outside);
    names_free(all);
    return kept;
}

const HoistNames* hoist_if_branch_hoists(ASTNode* body) {
    if (!body) return NULL;
    struct HoistNodeCache* c = node_cache(body);
    if (!c->branch) c->branch = branch_hoists_build(body);
    return c->branch;
}

static HoistNames* common_hoists_build_arms(ASTNode* stmt, ASTNode** arms, int na);

static HoistNames* common_hoists_build(ASTNode* stmt) {
    int na = 0;
    ASTNode** arms = branch_arms_all(stmt, &na);
    HoistNames* h = common_hoists_build_arms(stmt, arms, na);
    free(arms);
    return h;
}

static HoistNames* common_hoists_build_arms(ASTNode* stmt, ASTNode** arms, int na) {
    HoistNames* h = names_new(na > 0 ? na : 1);
    if (!h) return NULL;
    /* Every path must pass through an arm: an if needs its else, a switch
     * its default, a match a `_` arm or arms that cover its type (#2768). */
    if (na == 0) return h;
    if (stmt->type == AST_IF_STATEMENT && na < 2) return h;
    if (stmt->type == AST_SWITCH_STATEMENT) {
        int has_default = 0;
        for (int j = 0; j < na; j++) {
            if (arms[j]->value && strcmp(arms[j]->value, "default") == 0) has_default = 1;
        }
        if (!has_default || na < 2) return h;
    }
    if (stmt->type == AST_MATCH_STATEMENT && !ast_match_covers(stmt)) return h;
    /* Each arm's first direct binding of each name it binds. */
    StrMap* firsts = (StrMap*)calloc((size_t)na, sizeof(StrMap));
    if (!firsts) return h;
    for (int j = 0; j < na; j++) {
        strmap_init(&firsts[j]);
        for (int k = 0; arms[j] && k < arms[j]->child_count; k++) {
            ASTNode* d = arms[j]->children[k];
            if (d && d->type == AST_VARIABLE_DECLARATION && d->value &&
                !strmap_has(&firsts[j], d->value))
                strmap_put(&firsts[j], d->value, d);
        }
    }
    StrMap bound;
    strmap_init(&bound);
    pattern_names(stmt, &bound);
    /* The names the first arm binds that every other arm binds too. */
    for (int i = 0; i < strmap_count(&firsts[0]); i++) {
        const char* name = strmap_key_at(&firsts[0], i);
        int everywhere = !strmap_has(&bound, name);
        for (int j = 1; j < na && everywhere; j++)
            if (!strmap_has(&firsts[j], name)) everywhere = 0;
        if (!everywhere) continue;
        ASTNode* d0 = (ASTNode*)strmap_value_at(&firsts[0], i);
        int at = names_push(h, d0->value);
        for (int j = 0; j < na; j++)
            h->decls[(size_t)at * (size_t)na + (size_t)j] = (ASTNode*)strmap_get(&firsts[j], name);
    }
    strmap_free(&bound);
    for (int j = 0; j < na; j++) strmap_free(&firsts[j]);
    free(firsts);
    return h;
}

const HoistNames* hoist_common_hoists(ASTNode* stmt) {
    if (!stmt) return NULL;
    struct HoistNodeCache* c = node_cache(stmt);
    if (!c->common) c->common = common_hoists_build(stmt);
    return c->common;
}

/* The matches whose arms a hoist_loop_decls walk is inside, innermost last:
 * a name one of their patterns binds is declared in no loop hoist. */
/* As deep as the matches nest (#3574): past 64 the walk stopped going into
 * them, and a name bound in a loop there was hoisted nowhere. */
typedef struct {
    HoistDeclVisitor visit;
    void* user;
    const ASTNode** matches;
    int depth;
    int cap;
} LoopWalk;

static void loop_decls_walk(ASTNode* body, LoopWalk* w) {
    if (!body) return;
    for (int i = 0; i < body->child_count; i++) {
        ASTNode* child = body->children[i];
        if (!child) continue;
        if (child->type == AST_VARIABLE_DECLARATION && child->value) {
            int own = 0;
            for (int m = 0; m < w->depth && !own; m++)
                own = match_pattern_binds(w->matches[m], child->value);
            if (!own) w->visit(child, w->user);
        }
        /* A switch's children are its cases, whose own children are their
         * bodies: they are visited as an if's arms are (#2732). A match's
         * are its arms, whose bodies are visited the same way (#2768). */
        if (child->type == AST_IF_STATEMENT || child->type == AST_WHILE_LOOP ||
            child->type == AST_FOR_LOOP || child->type == AST_SWITCH_STATEMENT) {
            for (int j = 0; j < child->child_count; j++) loop_decls_walk(child->children[j], w);
        } else if (child->type == AST_MATCH_STATEMENT) {
            int na = 0;
            ASTNode** arms = branch_arms_all(child, &na);
            if (w->depth == w->cap) {
                w->cap = w->cap ? w->cap * 2 : 16;
                w->matches = (const ASTNode**)aether_xrealloc((void*)w->matches,
                                                              sizeof(ASTNode*) * (size_t)w->cap);
            }
            w->matches[w->depth++] = child;
            for (int j = 0; j < na; j++) loop_decls_walk(arms[j], w);
            w->depth--;
            free(arms);
        }
    }
}

void hoist_loop_decls(ASTNode* body, HoistDeclVisitor visit, void* user) {
    if (!body || !visit) return;
    LoopWalk w;
    w.visit = visit;
    w.user = user;
    w.matches = NULL;
    w.depth = 0;
    w.cap = 0;
    loop_decls_walk(body, &w);
    free((void*)w.matches);
}
