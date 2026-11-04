/* Actor protocols (#385). See actor_protocol.h.
 *
 * Each function body (a function, `main`, a receive arm, a closure) is walked
 * once in order, keeping for every reference that follows a protocol the set
 * of states it may be in at that point:
 *
 *   - `w = spawn(Writer())`, Writer implementing P: the reference starts in
 *     P's first state.
 *   - A parameter or a binding typed `actor_ref[Writer]` or
 *     `actor_ref[FileWriter]`: its state is not known. A send through it is
 *     checked only for being one of the protocol's messages (the type checker
 *     does that); after the send it is in a state that message leads to.
 *   - A send or ask `w ! M { }` is allowed when every state `w` may be in has
 *     a transition on M; then `w` is in the states those transitions lead
 *     to.
 *   - Any other use of `w` (passed, copied, stored, captured by a closure or
 *     a `defer`) lets the state change out of sight: from there `w` is
 *     followed as a reference whose state is not known. Reading an actor's
 *     state through `w.field` sends nothing.
 *   - At a join (after an `if`, a `match`, a loop, a `try`) the states of
 *     every path that reaches it are put together, so a message is allowed
 *     only when every path allows it. A loop is walked until the states at
 *     its head stop growing; the last walk reports.
 *
 * A path that leaves (`return`, `break`, `continue`, a panic, `exit`) adds
 * nothing to the statement after it; `break` and `continue` add to their
 * loop's exit and head.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "actor_protocol.h"
#include "contract_eval.h"
#include "missing_return.h"
#include "../aether_error.h"

#define PROTO_MAX_STATES 64
/* How often one reference can rise at a loop's head (#3574): once to
 * appear, once per state its mask gains, once to go unknown (`top`) and
 * once to be dropped, which it stays for the loop (a reference whose
 * protocol differs from one turn to the next). A pass that does not settle
 * the head raises one, so a head of n references settles within
 * PROTO_REF_RISES * n passes. */
#define PROTO_REF_RISES (PROTO_MAX_STATES + 3)

typedef struct {
    ASTNode* node;                       /* AST_PROTOCOL_DEFINITION */
    const char* states[PROTO_MAX_STATES];
    int state_count;
} Proto;

static Proto* g_protos = NULL;
static int g_proto_count = 0;

static ASTNode* unwrap_export(ASTNode* n) {
    if (n && n->type == AST_EXPORT_STATEMENT && n->child_count > 0) return n->children[0];
    return n;
}

static int state_index(Proto* p, const char* name) {
    for (int i = 0; i < p->state_count; i++)
        if (strcmp(p->states[i], name) == 0) return i;
    return -1;
}

/* Adds `name` as a state of `p` (once); -1 when there are too many. */
static int state_add(Proto* p, const char* name) {
    int i = state_index(p, name);
    if (i >= 0) return i;
    if (p->state_count >= PROTO_MAX_STATES) return -1;
    p->states[p->state_count] = name;
    return p->state_count++;
}

static ASTNode* transition_from(ASTNode* t) {
    return t->child_count > 0 ? t->children[0] : NULL;
}

static ASTNode* transition_to(ASTNode* t) {
    return t->child_count > 1 ? t->children[1] : NULL;
}

void aether_protocols_collect(ASTNode* program) {
    free(g_protos);
    g_protos = NULL;
    g_proto_count = 0;
    if (!program) return;
    int n = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* d = unwrap_export(program->children[i]);
        if (d && d->type == AST_PROTOCOL_DEFINITION && d->value) n++;
    }
    if (n == 0) return;
    g_protos = calloc((size_t)n, sizeof(Proto));
    if (!g_protos) return;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* d = unwrap_export(program->children[i]);
        if (!d || d->type != AST_PROTOCOL_DEFINITION || !d->value) continue;
        Proto* p = &g_protos[g_proto_count++];
        p->node = d;
        for (int k = 0; k < d->child_count; k++) {
            ASTNode* t = d->children[k];
            if (!t || t->type != AST_PROTOCOL_TRANSITION) continue;
            ASTNode* from = transition_from(t);
            ASTNode* to = transition_to(t);
            if (from && from->value) state_add(p, from->value);
            if (to && to->value) state_add(p, to->value);
        }
    }
}

static Proto* proto_named(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < g_proto_count; i++)
        if (strcmp(g_protos[i].node->value, name) == 0) return &g_protos[i];
    return NULL;
}

ASTNode* aether_protocol_lookup(const char* name) {
    Proto* p = proto_named(name);
    return p ? p->node : NULL;
}

int aether_protocol_has_message(ASTNode* proto, const char* msg) {
    if (!proto || !msg) return 0;
    for (int k = 0; k < proto->child_count; k++) {
        ASTNode* t = proto->children[k];
        if (t && t->type == AST_PROTOCOL_TRANSITION && t->value && strcmp(t->value, msg) == 0)
            return 1;
    }
    return 0;
}

const char* aether_actor_protocol_name(const ASTNode* actor) {
    if (!actor || actor->type != AST_ACTOR_DEFINITION || !actor->annotation) return NULL;
    if (strncmp(actor->annotation, "implements:", 11) != 0) return NULL;
    return actor->annotation + 11;
}

/* ------------------------------------------------------------ reporting */

typedef struct {
    ASTNode* program;
    int errors;
} Ctx;

static void report(Ctx* c, ASTNode* at, const char* message, const char* help) {
    AetherError e = { at ? at->source_file : NULL, NULL, at ? at->line : 0, at ? at->column : 0,
                      message, help, NULL, AETHER_ERR_ACTOR_ERROR };
    aether_error_report(&e);
    c->errors++;
}

/* ----------------------------------------------------- the declarations */

static ASTNode* find_top(ASTNode* program, ASTNodeType type, const char* name) {
    for (int i = 0; program && i < program->child_count; i++) {
        ASTNode* d = unwrap_export(program->children[i]);
        if (d && d->type == type && d->value && strcmp(d->value, name) == 0) return d;
    }
    return NULL;
}

static void check_protocol(Ctx* c, Proto* p) {
    ASTNode* d = p->node;
    if (!d->value) return;
    char msg[512];
    /* One protocol per name. */
    if (proto_named(d->value) != p) {
        snprintf(msg, sizeof msg, "protocol '%s' is declared twice", d->value);
        report(c, d, msg, "rename one of them");
        return;
    }
    /* `actor_ref[Name]` says which: an actor or a protocol, not both. */
    if (find_top(c->program, AST_ACTOR_DEFINITION, d->value) ||
        find_top(c->program, AST_MESSAGE_DEFINITION, d->value) ||
        find_top(c->program, AST_STRUCT_DEFINITION, d->value)) {
        snprintf(msg, sizeof msg, "protocol '%s' has the name of another declaration", d->value);
        report(c, d, msg, "a protocol's name is a type name of its own: rename it");
        return;
    }
    if (d->child_count == 0) {
        snprintf(msg, sizeof msg, "protocol '%s' has no transitions", d->value);
        report(c, d, msg, "write one `state -> Message -> state` line for each message it allows");
        return;
    }
    for (int k = 0; k < d->child_count; k++) {
        ASTNode* t = d->children[k];
        if (!t || t->type != AST_PROTOCOL_TRANSITION || !t->value) continue;
        ASTNode* from = transition_from(t);
        ASTNode* to = transition_to(t);
        if (!from || !to || state_index(p, from->value) < 0 || state_index(p, to->value) < 0) {
            snprintf(msg, sizeof msg, "protocol '%s' has more than %d states", d->value,
                     PROTO_MAX_STATES);
            report(c, t, msg, "split it into protocols with fewer states");
            return;
        }
        if (!find_top(c->program, AST_MESSAGE_DEFINITION, t->value)) {
            snprintf(msg, sizeof msg, "protocol '%s' names '%s', which is not a declared message",
                     d->value, t->value);
            report(c, t, msg, "declare it with `message`, or correct the name");
        }
        /* One next state per state and message. */
        for (int j = 0; j < k; j++) {
            ASTNode* u = d->children[j];
            if (!u || u->type != AST_PROTOCOL_TRANSITION || !u->value) continue;
            ASTNode* ufrom = transition_from(u);
            if (strcmp(u->value, t->value) == 0 && ufrom && strcmp(ufrom->value, from->value) == 0) {
                snprintf(msg, sizeof msg,
                         "protocol '%s' has a second transition for '%s' in state '%s'; the "
                         "first is at line %d",
                         d->value, t->value, from->value, u->line);
                report(c, t, msg, "a message leads to one state from a given state: keep one line");
                break;
            }
        }
    }
}

/* Whether a receive arm of `actor` names `msg` (a `_` arm does not count:
 * a protocol's messages are its actor's interface). */
static int actor_names_message(ASTNode* actor, const char* msg) {
    for (int i = 0; i < actor->child_count; i++) {
        ASTNode* rs = actor->children[i];
        if (!rs || rs->type != AST_RECEIVE_STATEMENT) continue;
        for (int j = 0; j < rs->child_count; j++) {
            ASTNode* arm = rs->children[j];
            ASTNode* pattern = NULL;
            if (arm && arm->type == AST_RECEIVE_ARM && arm->child_count >= 2)
                pattern = arm->children[0];
            else if (arm && arm->type == AST_BLOCK)
                for (int k = 0; k < arm->child_count && !pattern; k++)
                    if (arm->children[k] && arm->children[k]->type == AST_MESSAGE_PATTERN)
                        pattern = arm->children[k];
            if (pattern && pattern->type == AST_MESSAGE_PATTERN && pattern->value &&
                strcmp(pattern->value, msg) == 0)
                return 1;
        }
    }
    return 0;
}

static void check_implements(Ctx* c, ASTNode* actor) {
    const char* pname = aether_actor_protocol_name(actor);
    if (!pname) return;
    char msg[512];
    Proto* p = proto_named(pname);
    if (!p) {
        snprintf(msg, sizeof msg, "actor '%s' implements '%s', which is not a declared protocol",
                 actor->value, pname);
        report(c, actor, msg, "declare it with `protocol`, or correct the name");
        return;
    }
    for (int k = 0; k < p->node->child_count; k++) {
        ASTNode* t = p->node->children[k];
        if (!t || t->type != AST_PROTOCOL_TRANSITION || !t->value) continue;
        int seen = 0;
        for (int j = 0; j < k && !seen; j++) {
            ASTNode* u = p->node->children[j];
            seen = u && u->value && strcmp(u->value, t->value) == 0;
        }
        if (seen || actor_names_message(actor, t->value)) continue;
        char help[256];
        snprintf(msg, sizeof msg,
                 "actor '%s' implements protocol '%s' but has no receive arm for '%s'",
                 actor->value, pname, t->value);
        snprintf(help, sizeof help, "add a `%s` arm: a `_` arm does not count for a protocol's messages",
                 t->value);
        report(c, actor, msg, help);
    }
}

/* ------------------------------------------------------------ the states */

typedef struct {
    const char* name;
    Proto* proto;
    uint64_t mask;      /* the states it may be in; meaningful when !top */
    int top;            /* its state is not known */
    int dropped;        /* its protocol differs between the paths that reach here:
                         * followed on none, which a join keeps (#3574) */
} PVar;

/* Every reference the function follows (#3574): 64 slots left the rest
 * unfollowed, so a message its protocol does not allow went unrefused. The
 * variables are on the heap: a state is copied with ps_set and released
 * with ps_release. */
typedef struct {
    PVar* v;
    int n;
    int cap;
    int dead;           /* no path reaches this point */
} PState;

static PVar* var_find(PState* s, const char* name) {
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->v[i].name, name) == 0) return &s->v[i];
    return NULL;
}

static void var_forget(PState* s, const char* name) {
    for (int i = 0; i < s->n; i++) {
        if (strcmp(s->v[i].name, name) == 0) {
            s->v[i] = s->v[--s->n];
            return;
        }
    }
}

static void var_set(PState* s, const char* name, Proto* p, uint64_t mask, int top) {
    PVar* v = var_find(s, name);
    if (!v) {
        if (s->n == s->cap) {
            s->cap = s->cap ? s->cap * 2 : 16;
            s->v = (PVar*)realloc(s->v, sizeof(PVar) * (size_t)s->cap);
            if (!s->v) {
                fprintf(stderr, "aetherc: out of memory in the protocol check\n");
                exit(1);
            }
        }
        v = &s->v[s->n++];
        v->name = name;
    }
    v->proto = p;
    v->mask = mask;
    v->top = top;
    v->dropped = 0;
}

/* `dst` becomes a copy of `src`: its own variables, the same states. */
static void ps_set(PState* dst, const PState* src) {
    if (dst == src) return;
    if (dst->cap < src->n) {
        dst->cap = src->n;
        dst->v = (PVar*)realloc(dst->v, sizeof(PVar) * (size_t)dst->cap);
        if (!dst->v) {
            fprintf(stderr, "aetherc: out of memory in the protocol check\n");
            exit(1);
        }
    }
    if (src->n) memcpy(dst->v, src->v, sizeof(PVar) * (size_t)src->n);
    dst->n = src->n;
    dst->dead = src->dead;
}

/* The states of every path through `a` and `b`. */
static void join(PState* a, const PState* b) {
    if (b->dead) return;
    if (a->dead) { ps_set(a, b); return; }
    for (int i = 0; i < b->n; i++) {
        const PVar* bv = &b->v[i];
        PVar* av = var_find(a, bv->name);
        if (!av) {
            var_set(a, bv->name, bv->proto, bv->mask, bv->top);
            var_find(a, bv->name)->dropped = bv->dropped;
        } else if (av->dropped || bv->dropped || av->proto != bv->proto) {
            /* Under two protocols, or one already: followed on no path. A
             * drop forgot it, and a later join brought it back under the
             * other, so a loop's head never settled and joins in another
             * order gave another answer. */
            av->dropped = 1;
        } else {
            av->top = av->top || bv->top;
            av->mask |= bv->mask;
        }
    }
}

static int same(const PState* a, const PState* b) {
    if (a->dead != b->dead || a->n != b->n) return 0;
    for (int i = 0; i < a->n; i++) {
        PState* bm = (PState*)b;
        PVar* bv = var_find(bm, a->v[i].name);
        if (!bv || bv->dropped != a->v[i].dropped) return 0;
        if (bv->dropped) continue;
        if (bv->proto != a->v[i].proto || bv->top != a->v[i].top ||
            (!bv->top && bv->mask != a->v[i].mask))
            return 0;
    }
    return 1;
}

/* ---------------------------------------------------------- the walk */

typedef struct Loop {
    const char* label;
    int is_switch;
    PState brk;
    PState cont;
    struct Loop* up;
} Loop;

/* A join a loop's walk made into a loop or switch around it (a `break` or
 * `continue` leaving it): `up` places past the innermost one around the
 * loop, into its `cont` or its `brk`, the joins into one place joined. */
typedef struct {
    int up;
    int to_cont;
    PState st;
} Escape;

/* A loop walked from `entry`, which left `exit` and made `esc` (loop()). */
typedef struct LoopMemo {
    ASTNode* node;
    int report;
    PState entry, exit;
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
    int report;         /* report errors on this walk (a loop's last one) */
    Loop* loops;
    LoopMemo* memo;     /* the loops walked, by entry state */
    Rec* rec;           /* the loop walks being recorded, innermost first */
} Walk;

static void walk(Walk* w, ASTNode* n, PState* s);
static void check_body(Ctx* c, ASTNode* body, ASTNode* params_owner);

/* The actor `spawn_X(...)` spawns, or NULL. */
static ASTNode* spawned_actor(Ctx* c, ASTNode* init) {
    if (!init || init->type != AST_FUNCTION_CALL || !init->value ||
        strncmp(init->value, "spawn_", 6) != 0)
        return NULL;
    return find_top(c->program, AST_ACTOR_DEFINITION, init->value + 6);
}

/* The protocol a type `actor_ref[X]` follows: X's own when X is a
 * protocol, the one X implements when X is an actor. */
static Proto* type_protocol(Ctx* c, Type* t) {
    if (!t || t->kind != TYPE_ACTOR_REF || !t->element_type ||
        t->element_type->kind != TYPE_STRUCT || !t->element_type->struct_name)
        return NULL;
    const char* name = t->element_type->struct_name;
    Proto* p = proto_named(name);
    if (p) return p;
    ASTNode* actor = find_top(c->program, AST_ACTOR_DEFINITION, name);
    return actor ? proto_named(aether_actor_protocol_name(actor)) : NULL;
}

/* `name` now holds what `init` gives (declared type `t`). */
static void bind(Walk* w, PState* s, const char* name, ASTNode* init, Type* t) {
    if (!name) return;
    ASTNode* actor = spawned_actor(w->c, init);
    if (actor) {
        Proto* p = proto_named(aether_actor_protocol_name(actor));
        if (p) {
            var_set(s, name, p, 1, 0);   /* the first transition's state is state 0 */
            return;
        }
    }
    Proto* p = type_protocol(w->c, t);
    if (!p && init) p = type_protocol(w->c, init->node_type);
    if (p) var_set(s, name, p, 0, 1);
    else var_forget(s, name);
}

/* Every followed reference named in `n`: its state can now change out of
 * sight. */
static void escape_all(PState* s, ASTNode* n) {
    if (!n) return;
    if (n->type == AST_IDENTIFIER && n->value) {
        PVar* v = var_find(s, n->value);
        if (v) v->top = 1;
    }
    for (int i = 0; i < n->child_count; i++) escape_all(s, n->children[i]);
}

static void names_list(Proto* p, uint64_t mask, char* out, size_t cap) {
    out[0] = '\0';
    int first = 1;
    for (int i = 0; i < p->state_count; i++) {
        if (!(mask & ((uint64_t)1 << i))) continue;
        size_t len = strlen(out);
        snprintf(out + len, cap - len, "%s'%s'", first ? "" : " or ", p->states[i]);
        first = 0;
    }
}

/* What state `st` takes, for the help line. */
static void allowed_in(Proto* p, int st, char* out, size_t cap) {
    out[0] = '\0';
    int any = 0;
    for (int k = 0; k < p->node->child_count; k++) {
        ASTNode* t = p->node->children[k];
        ASTNode* from = t ? transition_from(t) : NULL;
        if (!from || state_index(p, from->value) != st) continue;
        size_t len = strlen(out);
        snprintf(out + len, cap - len, "%s%s", any ? ", " : "", t->value);
        any = 1;
    }
    if (!any) snprintf(out, cap, "no message");
}

/* Kept out of the walk's frame: its message buffers would otherwise sit in
 * every frame of a walk that recurses as deep as an expression nests. */
#if defined(__GNUC__) || defined(__clang__)
#define AP_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define AP_NOINLINE __declspec(noinline)
#else
#define AP_NOINLINE
#endif

/* A send or ask of `msg` through the followed reference `v`. */
static AP_NOINLINE void transition(Walk* w, PVar* v, ASTNode* message) {
    Proto* p = v->proto;
    const char* msg = message->value;
    if (!aether_protocol_has_message(p->node, msg)) {
        /* Through a reference typed `actor_ref[P]` the type checker has
         * said so already; this is one to an actor that implements P. */
        if (w->report) {
            char msgbuf[400], help[400];
            snprintf(msgbuf, sizeof msgbuf,
                     "protocol '%s' has no message '%s', and '%s' follows it",
                     p->node->value, msg, v->name);
            snprintf(help, sizeof help,
                     "add '%s' to the protocol, or send it through a bare `actor_ref` "
                     "(a parameter typed so), which follows none",
                     msg);
            report(w->c, message, msgbuf, help);
        }
        v->top = 1;
        return;
    }
    /* In no state: a send before was wrong on every path (#3575). */
    int lost = !v->top && v->mask == 0;
    uint64_t to = 0, bad = 0, from_mask = (v->top || lost) ? ~(uint64_t)0 : v->mask;
    for (int st = 0; st < p->state_count; st++) {
        if (!(from_mask & ((uint64_t)1 << st))) continue;
        int found = 0;
        for (int k = 0; k < p->node->child_count; k++) {
            ASTNode* t = p->node->children[k];
            ASTNode* from = t ? transition_from(t) : NULL;
            ASTNode* dest = t ? transition_to(t) : NULL;
            if (!from || !dest || !t->value || strcmp(t->value, msg) != 0) continue;
            if (state_index(p, from->value) != st) continue;
            to |= (uint64_t)1 << state_index(p, dest->value);
            found = 1;
        }
        if (!found) bad |= (uint64_t)1 << st;
    }
    if (v->top || lost) {
        /* Not known before, or lost; a message the protocol has leads
         * somewhere it says (one it does not have is the type checker's to
         * report). */
        if (to) { v->top = 0; v->mask = to; }
        return;
    }
    if (bad && w->report) {
        char states[256], msgbuf[600], help[600];
        names_list(p, bad, states, sizeof states);
        int several = 0;
        for (int i = 0; i < p->state_count; i++)
            if (v->mask & ((uint64_t)1 << i)) several++;
        snprintf(msgbuf, sizeof msgbuf,
                 "protocol '%s' does not allow '%s' here: '%s' %s in state %s",
                 p->node->value, msg, v->name, several > 1 ? "may be" : "is", states);
        help[0] = '\0';
        for (int st = 0; st < p->state_count; st++) {
            if (!(bad & ((uint64_t)1 << st))) continue;
            char takes[256];
            allowed_in(p, st, takes, sizeof takes);
            size_t len = strlen(help);
            snprintf(help + len, sizeof help - len, "%sin '%s' it takes %s", len ? "; " : "",
                     p->states[st], takes);
        }
        report(w->c, message, msgbuf, help);
    }
    /* Every path was wrong: lost, in no state, so the next send is not
     * refused for it and leads where the protocol says, as from unknown.
     * A join keeps what the other paths reach. It went unknown (`top`)
     * before, so in a loop the head joined to unknown and the walk that
     * reports, from it, refused nothing (#3575). */
    v->mask = to;
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
static void escape_note(Walk* w, Loop* t, int to_cont, const PState* st) {
    for (Rec* r = w->rec; r; r = r->up) {
        int up = 0;
        Loop* l = r->base;
        while (l && l != t) {
            l = l->up;
            up++;
        }
        if (!l) continue;
        LoopMemo* m = r->m;
        /* One per place, its joins joined (join is a join: in any order, and
         * once or again, it gives one answer): a list of them grew with
         * every walk reused inside, by as many again at each level up. */
        Escape* same_place = NULL;
        for (int i = 0; i < m->esc_n && !same_place; i++)
            if (m->esc[i].up == up && m->esc[i].to_cont == to_cont) same_place = &m->esc[i];
        if (same_place) {
            join(&same_place->st, st);
            continue;
        }
        if (m->esc_n == m->esc_cap) {
            m->esc_cap = m->esc_cap ? m->esc_cap * 2 : 4;
            m->esc = (Escape*)realloc(m->esc, sizeof(Escape) * (size_t)m->esc_cap);
            if (!m->esc) {
                fprintf(stderr, "aetherc: out of memory in the protocol check\n");
                exit(1);
            }
        }
        Escape* e = &m->esc[m->esc_n++];
        memset(e, 0, sizeof *e);
        e->up = up;
        e->to_cont = to_cont;
        ps_set(&e->st, st);
    }
}

/* `st` joined into `t`'s `cont` or `brk`, and noted (escape_note). */
static void loop_join(Walk* w, Loop* t, int to_cont, const PState* st) {
    join(to_cont ? &t->cont : &t->brk, st);
    escape_note(w, t, to_cont, st);
}

static int always_true(Walk* w, ASTNode* cond) {
    if (!cond) return 1;
    ContractEnv env;
    memset(&env, 0, sizeof env);
    env.program = w->c->program;
    return contract_eval_predicate(cond, &env) == CONTRACT_TRUE;
}

/* The copies of a state a branch, a loop or a switch needs are on the heap,
 * never in a frame: the walk recurses as deep as an expression nests (a
 * chain of 1000 `+ 1` is 1000 frames), and a frame holding a state each
 * overflowed the stack there. */
static void* ps_alloc(size_t n) {
    void* p = calloc(1, n);
    if (!p) {
        fprintf(stderr, "aetherc: out of memory in the protocol check\n");
        exit(1);
    }
    return p;
}

static PState* ps_new(const PState* src) {
    PState* d = (PState*)ps_alloc(sizeof *d);
    ps_set(d, src);
    return d;
}

/* A state ps_new or ps_alloc made, and its variables. */
static void ps_release(PState* s) {
    if (!s) return;
    free(s->v);
    free(s);
}

/* A Loop and the states it holds by value. */
static void loop_free(Loop* l) {
    free(l->brk.v);
    free(l->cont.v);
    free(l);
}

static Loop* loop_push(Walk* w, const char* label, int is_switch) {
    Loop* l = (Loop*)ps_alloc(sizeof *l);
    l->label = label;
    l->is_switch = is_switch;
    l->brk.dead = 1;
    l->cont.dead = 1;
    l->up = w->loops;
    w->loops = l;
    return l;
}

/* One turn of a loop over `st`: its body and its step. */
static void loop_turn(Walk* w, ASTNode* node, ASTNode* body, ASTNode* step, PState* st,
                      PState* brk) {
    Loop* l = loop_push(w, node->value, 0);
    walk(w, body, st);
    w->loops = l->up;
    join(st, &l->cont);
    walk(w, step, st);
    if (brk) ps_set(brk, &l->brk);
    loop_free(l);
}

/* A loop: `init` once, then `cond`, `body`, `step` while it runs. */
static void loop_walk(Walk* w, PState* s, ASTNode* node, ASTNode* init, ASTNode* cond,
                      ASTNode* body, ASTNode* step) {
    walk(w, init, s);
    if (s->dead) return;
    int reporting = w->report;
    PState* head = ps_new(s);
    PState* st = ps_new(s);
    PState* next = ps_new(s);
    int unsettled = 0;
    for (int pass = 0; ; pass++) {
        w->report = 0;
        ps_set(st, head);
        walk(w, cond, st);
        loop_turn(w, node, body, step, st, NULL);
        ps_set(next, head);
        join(next, st);
        if (same(next, head)) {
            ps_set(head, next);
            break;
        }
        if (pass >= PROTO_REF_RISES * (next->n + 1)) {
            /* More rises than the states have: a fault in the count above.
             * The loop is refused below, not passed unchecked. */
            unsettled = 1;
            for (int i = 0; i < next->n; i++) next->v[i].top = 1;
            ps_set(head, next);
            break;
        }
        ps_set(head, next);
    }
    /* The walk that reports, from the states at the head. */
    w->report = reporting;
    if (unsettled && reporting) {
        char msg[200];
        snprintf(msg, sizeof msg,
                 "the protocol check found no settled state for this loop in %d passes",
                 PROTO_REF_RISES * (next->n + 1));
        report(w->c, node, msg, "this is a fault in the compiler: please report it with this loop");
    }
    ps_set(st, head);
    walk(w, cond, st);
    PState* out = ps_new(st);                  /* the condition is false */
    if (always_true(w, cond)) out->dead = 1;
    PState* brk = ps_new(st);
    brk->dead = 1;
    loop_turn(w, node, body, step, st, brk);
    join(out, brk);
    ps_set(s, out);
    ps_release(head);
    ps_release(st);
    ps_release(next);
    ps_release(out);
    ps_release(brk);
}

/* Two states alike in every fact of every reference, in the same order. */
static int ps_alike(const PState* a, const PState* b) {
    if (a->dead != b->dead || a->n != b->n) return 0;
    for (int i = 0; i < a->n; i++) {
        const PVar* x = &a->v[i];
        const PVar* y = &b->v[i];
        if (strcmp(x->name, y->name) != 0 || x->proto != y->proto || x->mask != y->mask ||
            x->top != y->top || x->dropped != y->dropped)
            return 0;
    }
    return 1;
}

/* A loop's walk from `s`. A loop walked once already from the same states,
 * reporting or not, gives the same answer and makes the same joins into the
 * loops around it, so that walk is reused (#3574): a loop's walk walks its
 * body at least twice (its passes and the walk that reports), each walking
 * the loops inside, and loops nested 24 deep took seconds, doubling with
 * each level. Every walk that is not reused is kept (LoopMemo, freed with
 * the function's walk). */
static void loop(Walk* w, PState* s, ASTNode* node, ASTNode* init, ASTNode* cond,
                 ASTNode* body, ASTNode* step) {
    for (LoopMemo* m = w->memo; m; m = m->next) {
        if (m->node != node || m->report != w->report || !ps_alike(&m->entry, s)) continue;
        for (int i = 0; i < m->esc_n; i++) {
            const Escape* e = &m->esc[i];
            Loop* t = w->loops;
            for (int k = 0; k < e->up && t; k++) t = t->up;
            if (t) loop_join(w, t, e->to_cont, &e->st);
        }
        ps_set(s, &m->exit);
        return;
    }
    LoopMemo* m = (LoopMemo*)ps_alloc(sizeof *m);
    m->node = node;
    m->report = w->report;
    ps_set(&m->entry, s);
    Rec r = { w->loops, m, w->rec };
    w->rec = &r;
    loop_walk(w, s, node, init, cond, body, step);
    w->rec = r.up;
    ps_set(&m->exit, s);
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

static void walk(Walk* w, ASTNode* n, PState* s) {
    if (!n || s->dead) return;
    switch (n->type) {
        case AST_VARIABLE_DECLARATION: {
            ASTNode* init = n->child_count > 0 ? n->children[0] : NULL;
            walk(w, init, s);
            if (!s->dead) bind(w, s, n->value, init, n->node_type);
            return;
        }
        case AST_TUPLE_DESTRUCTURE:
            for (int i = 0; i < n->child_count; i++) {
                ASTNode* k = n->children[i];
                if (k && k->type == AST_VARIABLE_DECLARATION) continue;
                walk(w, k, s);
            }
            for (int i = 0; i < n->child_count; i++) {
                ASTNode* k = n->children[i];
                if (k && k->type == AST_VARIABLE_DECLARATION && k->value)
                    bind(w, s, k->value, NULL, k->node_type);
            }
            return;
        case AST_SEND_FIRE_FORGET:
        case AST_SEND_ASK: {
            ASTNode* target = n->child_count > 0 ? n->children[0] : NULL;
            ASTNode* message = n->child_count > 1 ? n->children[1] : NULL;
            walk(w, message, s);
            if (s->dead) return;
            PVar* v = (target && target->type == AST_IDENTIFIER && target->value)
                          ? var_find(s, target->value) : NULL;
            if (v && !v->dropped && message && message->type == AST_MESSAGE_CONSTRUCTOR &&
                message->value)
                transition(w, v, message);
            else
                walk(w, target, s);
            return;
        }
        case AST_IDENTIFIER:
            if (n->value) {
                PVar* v = var_find(s, n->value);
                if (v) v->top = 1;
            }
            return;
        case AST_MEMBER_ACCESS:
            /* `w.field` reads the actor's state: nothing is sent. */
            if (n->child_count > 0 && n->children[0] &&
                n->children[0]->type == AST_IDENTIFIER) {
                for (int i = 1; i < n->child_count; i++) walk(w, n->children[i], s);
                return;
            }
            break;
        case AST_IF_STATEMENT:
        case AST_IF_EXPRESSION: {
            ASTNode* cond = n->child_count > 0 ? n->children[0] : NULL;
            walk(w, cond, s);
            if (s->dead) return;
            /* A condition decided at compile time leaves one branch, as the
             * optimizer does (`if DEBUG { ... }`). */
            ContractTri known = CONTRACT_UNKNOWN;
            if (cond) {
                ContractEnv env;
                memset(&env, 0, sizeof env);
                env.program = w->c->program;
                known = contract_eval_predicate(cond, &env);
            }
            PState* t = ps_new(s);
            PState* e = ps_new(s);
            if (known == CONTRACT_FALSE) t->dead = 1;
            if (known == CONTRACT_TRUE) e->dead = 1;
            walk(w, n->child_count > 1 ? n->children[1] : NULL, t);
            walk(w, n->child_count > 2 ? n->children[2] : NULL, e);
            ps_set(s, t);
            join(s, e);
            ps_release(t);
            ps_release(e);
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
            PState* out = ps_new(s);
            PState* a = ps_new(s);
            if (ast_match_covers(n)) out->dead = 1;   /* some arm always runs */
            for (int i = 1; i < n->child_count; i++) {
                ASTNode* arm = n->children[i];
                if (!arm || arm->type != AST_MATCH_ARM) continue;
                ps_set(a, s);
                for (int k = 1; k < arm->child_count; k++) walk(w, arm->children[k], a);
                join(out, a);
            }
            ps_set(s, out);
            ps_release(out);
            ps_release(a);
            return;
        }
        case AST_SWITCH_STATEMENT: {
            walk(w, n->child_count > 0 ? n->children[0] : NULL, s);
            if (s->dead) return;
            PState* out = ps_new(s);
            PState* a = ps_new(s);
            Loop* l = loop_push(w, NULL, 1);
            for (int i = 1; i < n->child_count; i++) {
                ASTNode* cs = n->children[i];
                if (!cs || cs->type != AST_CASE_STATEMENT) continue;
                int is_default = cs->value && strcmp(cs->value, "default") == 0;
                if (is_default) out->dead = 1;
                ps_set(a, s);
                for (int k = is_default ? 0 : 1; k < cs->child_count; k++) walk(w, cs->children[k], a);
                join(out, a);
            }
            w->loops = l->up;
            join(out, &l->brk);
            /* A `continue` inside a switch belongs to the loop around it. */
            if (!l->cont.dead) {
                Loop* t = loop_target(w, NULL, 1);
                if (t) loop_join(w, t, 1, &l->cont);
            }
            loop_free(l);
            ps_set(s, out);
            ps_release(out);
            ps_release(a);
            return;
        }
        case AST_RETURN_STATEMENT:
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
        case AST_FUNCTION_CALL:
            for (int i = 0; i < n->child_count; i++) walk(w, n->children[i], s);
            if (ast_call_never_returns(n)) s->dead = 1;
            return;
        case AST_CLOSURE:
            if (ast_is_trailing_block(n)) {   /* runs inline, once */
                walk(w, closure_body(n), s);
                return;
            }
            /* A closure runs later: what it sends is out of sight here. Its
             * body is a function of its own. */
            escape_all(s, n);
            if (w->report) check_body(w->c, closure_body(n), n);
            return;
        case AST_QUOTE:
            return;
        case AST_DEFER_STATEMENT:
            /* Runs when the scope ends, after what follows here. */
            escape_all(s, n);
            return;
        case AST_TRY_STATEMENT: {
            ASTNode* body = n->child_count > 0 ? n->children[0] : NULL;
            /* A panic can enter the catch from anywhere in the body. */
            PState* entry = ps_new(s);
            escape_all(entry, body);
            walk(w, body, s);
            PState* a = ps_new(entry);
            for (int i = 1; i < n->child_count; i++) {
                ASTNode* cc = n->children[i];
                if (!cc || cc->type != AST_CATCH_CLAUSE) continue;
                ps_set(a, entry);
                for (int k = 0; k < cc->child_count; k++) walk(w, cc->children[k], a);
                join(s, a);
            }
            ps_release(entry);
            ps_release(a);
            return;
        }
        case AST_OR_ELSE: {
            walk(w, n->child_count > 0 ? n->children[0] : NULL, s);
            if (s->dead) return;
            PState* h = ps_new(s);
            for (int i = 1; i < n->child_count; i++) walk(w, n->children[i], h);
            join(s, h);
            ps_release(h);
            return;
        }
        case AST_BINARY_EXPRESSION:
            if (n->value && (strcmp(n->value, "&&") == 0 || strcmp(n->value, "||") == 0) &&
                n->child_count == 2) {
                walk(w, n->children[0], s);
                if (s->dead) return;
                PState* r = ps_new(s);
                walk(w, n->children[1], r);
                join(s, r);
                ps_release(r);
                return;
            }
            break;
        case AST_NULL_COALESCE: {
            walk(w, n->child_count > 0 ? n->children[0] : NULL, s);
            if (s->dead) return;
            PState* r = ps_new(s);
            for (int i = 1; i < n->child_count; i++) walk(w, n->children[i], r);
            join(s, r);
            ps_release(r);
            return;
        }
        default:
            break;
    }
    for (int i = 0; i < n->child_count; i++) walk(w, n->children[i], s);
}

/* Walks one function body: its parameters typed `actor_ref[X]` with a
 * protocol are followed from an unknown state. */
static void check_body(Ctx* c, ASTNode* body, ASTNode* params_owner) {
    if (!body) return;
    PState* s = (PState*)ps_alloc(sizeof *s);
    Walk w;
    memset(&w, 0, sizeof w);
    w.c = c;
    w.report = 1;
    for (int i = 0; params_owner && i < params_owner->child_count; i++) {
        ASTNode* p = params_owner->children[i];
        if (!p || p == body || !p->value) continue;
        if (p->type != AST_PATTERN_VARIABLE && p->type != AST_CLOSURE_PARAM &&
            p->type != AST_VARIABLE_DECLARATION)
            continue;
        Proto* proto = type_protocol(c, p->node_type);
        if (proto) var_set(s, p->value, proto, 0, 1);
    }
    walk(&w, body, s);
    ps_release(s);
    loop_memo_free(w.memo);
}

static ASTNode* last_block(ASTNode* n) {
    for (int i = n->child_count - 1; i >= 0; i--)
        if (n->children[i] && n->children[i]->type == AST_BLOCK) return n->children[i];
    return NULL;
}

int check_actor_protocols(ASTNode* program) {
    if (!program) return 0;
    Ctx c;
    memset(&c, 0, sizeof c);
    c.program = program;
    for (int i = 0; i < g_proto_count; i++) check_protocol(&c, &g_protos[i]);
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* d = unwrap_export(program->children[i]);
        if (d && d->type == AST_ACTOR_DEFINITION) check_implements(&c, d);
    }
    if (c.errors > 0 || g_proto_count == 0) return c.errors;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* d = unwrap_export(program->children[i]);
        if (!d) continue;
        switch (d->type) {
            case AST_FUNCTION_DEFINITION:
            case AST_BUILDER_FUNCTION:
            case AST_FUNCTION_CLAUSE:
                check_body(&c, last_block(d), d);
                break;
            case AST_MAIN_FUNCTION:
                check_body(&c, d->child_count > 0 ? d->children[0] : NULL, NULL);
                break;
            case AST_ACTOR_DEFINITION:
                for (int k = 0; k < d->child_count; k++) {
                    ASTNode* rs = d->children[k];
                    if (!rs || rs->type != AST_RECEIVE_STATEMENT) continue;
                    for (int j = 0; j < rs->child_count; j++) {
                        ASTNode* arm = rs->children[j];
                        if (arm && (arm->type == AST_RECEIVE_ARM || arm->type == AST_TIMEOUT_ARM) &&
                            arm->child_count >= 2)
                            check_body(&c, arm->children[1], NULL);
                    }
                }
                break;
            default:
                break;
        }
    }
    return c.errors;
}
