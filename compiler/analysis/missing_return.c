/* Missing-return check (#2684). See missing_return.h.
 *
 * The question asked of each statement is whether it can complete normally:
 * whether control can go on to the statement after it. A function whose body
 * can complete normally lets control reach its end. The rules:
 *
 *   - `return`, `panic(...)`, `exit(...)`, `break` and `continue` cannot.
 *   - A block can when every statement in it can (the code after one that
 *     cannot is unreachable, and W1002 says so).
 *   - An `if` can when it has no `else`, or when either branch can. A
 *     condition the compile-time evaluator decides (`true`, `1 == 1`, a
 *     `const`) leaves only the branch it selects, as the optimizer does.
 *   - A `match` can when one of its arms can, or when it may run no arm: it
 *     has no `_` arm and does not cover its type. A match over an enum or a
 *     sum covers it (the checker rejects one that misses a case), as do
 *     `none` and `some(v)` over an optional, `true` and `false` over a bool,
 *     and `[]` with `[h|t]` over a list.
 *   - A `switch` can when it has no `default`, when one of its cases can, or
 *     when a `break` in a case leaves it.
 *   - A `while` or `for` loop can when its condition is not always true (a
 *     `for` with no condition is always true), or when a `break` leaves it:
 *     an unlabeled one in its body outside any inner loop or switch, or one
 *     naming its label. `continue` does not end a loop.
 *   - `try` can when its body can or its `catch` can: a panic anywhere in the
 *     body enters the `catch`.
 *   - A call's trailing block, `f(x) { ... }`, runs inline: the statement
 *     around it can when the block can, and a `return` in it returns from
 *     the function around it.
 *   - Everything else can, `defer` included. A closure's body is a function
 *     of its own and is checked as one; nothing in it ends the statement it
 *     is written in.
 *
 * A definition claims a result when it writes a result type other than void,
 * or, with none written, when its body returns a value (the result is then
 * inferred from those returns). A clause of a set that writes no type and
 * returns nothing claims none: its caller gets the set's default value when
 * it ends (#2645). A closure claims one when its body returns a value. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "missing_return.h"
#include "contract_eval.h"
#include "../aether_error.h"

/* A loop or switch a `break` can leave, and the first `break` that does. */
typedef struct {
    ASTNode* node;
    const char* label;      /* a loop's label; NULL for none, and for a switch */
    int is_switch;          /* only an unlabeled `break` leaves a switch */
    ASTNode* first_break;
} BreakTarget;

#define MR_MAX_TARGETS 256

typedef struct {
    BreakTarget targets[MR_MAX_TARGETS];
    int depth;              /* targets past MR_MAX_TARGETS are counted, not kept */
    ASTNode* program;       /* resolves a `const` in a loop condition */
} Flow;

static int completes(Flow* f, ASTNode* n);

static int has_value_type(Type* t) {
    return t && t->kind != TYPE_VOID && t->kind != TYPE_UNKNOWN;
}

/* A `return` with a value anywhere in `n`, outside closures: a closure's
 * `return` is its own. The same test codegen's has_return_value makes. */
static int returns_value(ASTNode* n) {
    if (!n || n->type == AST_CLOSURE) return 0;
    if (n->type == AST_RETURN_STATEMENT && n->child_count > 0 && n->children[0] &&
        n->children[0]->type != AST_PRINT_STATEMENT)
        return 1;
    for (int i = 0; i < n->child_count; i++)
        if (returns_value(n->children[i])) return 1;
    return 0;
}

/* A call's trailing block, `f(x) { ... }`: a closure node in the tree, but a
 * block that runs inline at the call (the checker refuses one where the
 * callee takes an `fn`), so it is no function of its own. */
static int is_trailing_block(ASTNode* n) {
    return n && n->type == AST_CLOSURE && n->value && strcmp(n->value, "trailing") == 0;
}

static ASTNode* closure_body(ASTNode* closure) {
    for (int i = closure->child_count - 1; i >= 0; i--)
        if (closure->children[i] && closure->children[i]->type == AST_BLOCK)
            return closure->children[i];
    return NULL;
}

/* `exit(...)`: the C library's exit ends the process and never returns. */
static int is_exit_call(ASTNode* n) {
    return n && n->type == AST_FUNCTION_CALL && n->value && strcmp(n->value, "exit") == 0;
}

/* The first line a node or its subtree carries: several statements (a
 * `while`, a `match`, a block) record no position of their own. */
static int node_line(ASTNode* n) {
    if (!n) return 0;
    if (n->line > 0) return n->line;
    for (int i = 0; i < n->child_count; i++) {
        int l = node_line(n->children[i]);
        if (l > 0) return l;
    }
    return 0;
}

static BreakTarget* push_target(Flow* f, ASTNode* node, const char* label, int is_switch) {
    BreakTarget* t = NULL;
    if (f->depth < MR_MAX_TARGETS) {
        t = &f->targets[f->depth];
        t->node = node;
        t->label = label;
        t->is_switch = is_switch;
        t->first_break = NULL;
    }
    f->depth++;
    return t;
}

/* Pops the innermost target, returning the `break` that left it; for a
 * target too deep to keep, the node itself, so it is taken as left. */
static ASTNode* pop_target(Flow* f, BreakTarget* t, ASTNode* node) {
    f->depth--;
    return t ? t->first_break : node;
}

static void note_break(Flow* f, ASTNode* brk) {
    int top = f->depth < MR_MAX_TARGETS ? f->depth : MR_MAX_TARGETS;
    for (int i = top - 1; i >= 0; i--) {
        BreakTarget* t = &f->targets[i];
        int hit = brk->value
            ? (!t->is_switch && t->label && strcmp(t->label, brk->value) == 0)
            : 1;
        if (hit) {
            if (!t->first_break) t->first_break = brk;
            return;
        }
    }
}

/* A condition's value when the compile-time evaluator can decide it: `true`,
 * `1 == 1` or a `const` it names; CONTRACT_UNKNOWN otherwise. */
static ContractTri decide(Flow* f, ASTNode* cond) {
    ContractEnv env;
    memset(&env, 0, sizeof env);
    env.program = f->program;
    return contract_eval_predicate(cond, &env);
}

/* Whether a loop's condition is true whenever it is tested: none at all (a
 * `for (;;)`), or one decided true. */
static int always_true(Flow* f, ASTNode* cond) {
    return !cond || decide(f, cond) == CONTRACT_TRUE;
}

/* The branch of an `if` that can run: 1 for only the `then`, 2 for only the
 * `else` (or nothing, with no `else`), 3 for either. */
static int if_branches(Flow* f, ASTNode* ifs) {
    ASTNode* cond = ifs->child_count > 0 ? ifs->children[0] : NULL;
    ContractTri t = cond ? decide(f, cond) : CONTRACT_UNKNOWN;
    return t == CONTRACT_TRUE ? 1 : t == CONTRACT_FALSE ? 2 : 3;
}

static ASTNode* loop_cond(ASTNode* loop) {
    if (loop->type == AST_WHILE_LOOP) return loop->child_count > 0 ? loop->children[0] : NULL;
    return loop->child_count > 1 ? loop->children[1] : NULL;
}

static ASTNode* loop_body(ASTNode* loop) {
    if (loop->type == AST_WHILE_LOOP) return loop->child_count > 1 ? loop->children[1] : NULL;
    return loop->child_count > 3 ? loop->children[3] : NULL;
}

/* The first `break` that leaves `loop`, or NULL when none does. */
static ASTNode* loop_break(Flow* f, ASTNode* loop) {
    BreakTarget* t = push_target(f, loop, loop->value, 0);
    completes(f, loop_body(loop));
    return pop_target(f, t, loop);
}

static int loop_completes(Flow* f, ASTNode* loop) {
    ASTNode* brk = loop_break(f, loop);
    return brk || !always_true(f, loop_cond(loop));
}

/* The statements of a switch case: a `default` holds only statements, a
 * `case` its selector first. */
static int case_first_stmt(ASTNode* c) {
    return (c->value && strcmp(c->value, "default") == 0) ? 0 : 1;
}

static int case_is_default(ASTNode* c) {
    return c->value && strcmp(c->value, "default") == 0;
}

static int case_completes(Flow* f, ASTNode* c) {
    for (int i = case_first_stmt(c); i < c->child_count; i++)
        if (!completes(f, c->children[i])) return 0;
    return 1;
}

/* Runs every case of `sw`, returning whether one completes; *brk is the
 * first `break` that leaves the switch. */
static int switch_cases_complete(Flow* f, ASTNode* sw, ASTNode** brk) {
    BreakTarget* t = push_target(f, sw, NULL, 1);
    int any = 0;
    for (int i = 1; i < sw->child_count; i++) {
        ASTNode* c = sw->children[i];
        if (c && c->type == AST_CASE_STATEMENT && case_completes(f, c)) any = 1;
    }
    *brk = pop_target(f, t, sw);
    return any;
}

static int switch_has_default(ASTNode* sw) {
    for (int i = 1; i < sw->child_count; i++) {
        ASTNode* c = sw->children[i];
        if (c && c->type == AST_CASE_STATEMENT && case_is_default(c)) return 1;
    }
    return 0;
}

/* The arm codegen lowers to an unconditional `else`. */
static int is_wildcard_pattern(ASTNode* p) {
    if (!p) return 0;
    if (p->node_type && p->node_type->kind == TYPE_WILDCARD) return 1;
    return p->type == AST_LITERAL && p->value && strcmp(p->value, "_") == 0;
}

/* A list-pattern element that matches any value: a binding or `_`. */
static int binds_any(ASTNode* p) {
    return p && (p->type == AST_PATTERN_VARIABLE || is_wildcard_pattern(p));
}

/* Whether some arm of `m` runs for every value it can match. */
static int match_covers(ASTNode* m) {
    Type* t = (m->child_count > 0 && m->children[0]) ? m->children[0]->node_type : NULL;
    int has_none = 0, has_some = 0, has_true = 0, has_false = 0;
    int has_empty = 0, has_cons = 0;
    for (int i = 1; i < m->child_count; i++) {
        ASTNode* arm = m->children[i];
        if (!arm || arm->type != AST_MATCH_ARM || arm->child_count < 1) continue;
        ASTNode* p = arm->children[0];
        if (!p) continue;
        if (is_wildcard_pattern(p)) return 1;
        if (p->type == AST_NONE_LITERAL) has_none = 1;
        else if (p->type == AST_PATTERN_VARIABLE && p->annotation &&
                 strcmp(p->annotation, "some_pattern") == 0) has_some = 1;
        else if (p->type == AST_LITERAL && p->value && strcmp(p->value, "true") == 0) has_true = 1;
        else if (p->type == AST_LITERAL && p->value && strcmp(p->value, "false") == 0) has_false = 1;
        else if (p->type == AST_PATTERN_LIST && p->child_count == 0) has_empty = 1;
        else if (p->type == AST_PATTERN_CONS && p->child_count == 2 &&
                 binds_any(p->children[0]) && binds_any(p->children[1])) has_cons = 1;
    }
    if (t) {
        /* The checker rejects a match over an enum or a sum that misses a
         * case, so one that reaches here covers it. */
        if (t->kind == TYPE_ENUM || t->kind == TYPE_SUM) return 1;
        if (t->kind == TYPE_OPTIONAL) return has_none && has_some;
        if (t->kind == TYPE_BOOL) return has_true && has_false;
    }
    return has_empty && has_cons;
}

static int match_arms_complete(Flow* f, ASTNode* m) {
    int any = 0;
    for (int i = 1; i < m->child_count; i++) {
        ASTNode* arm = m->children[i];
        if (arm && arm->type == AST_MATCH_ARM && arm->child_count > 1 &&
            completes(f, arm->children[1])) any = 1;
    }
    return any;
}

static int completes(Flow* f, ASTNode* n) {
    if (!n) return 1;
    switch (n->type) {
        case AST_RETURN_STATEMENT:
        case AST_PANIC_STATEMENT:
        case AST_CONTINUE_STATEMENT:
            return 0;
        case AST_BREAK_STATEMENT:
            note_break(f, n);
            return 0;
        case AST_BLOCK:
            for (int i = 0; i < n->child_count; i++)
                if (!completes(f, n->children[i])) return 0;
            return 1;
        case AST_IF_STATEMENT: {
            ASTNode* then_b = n->child_count > 1 ? n->children[1] : NULL;
            ASTNode* else_b = n->child_count > 2 ? n->children[2] : NULL;
            int runs = if_branches(f, n);
            int t = (runs & 1) ? completes(f, then_b) : 0;
            int e = (runs & 2) ? (else_b ? completes(f, else_b) : 1) : 0;
            return t || e;
        }
        case AST_WHILE_LOOP:
        case AST_FOR_LOOP:
            return loop_completes(f, n);
        case AST_MATCH_STATEMENT: {
            int any = match_arms_complete(f, n);
            return any || !match_covers(n);
        }
        case AST_SWITCH_STATEMENT: {
            ASTNode* brk = NULL;
            int any = switch_cases_complete(f, n, &brk);
            return any || brk || !switch_has_default(n);
        }
        case AST_TRY_STATEMENT: {
            int any = completes(f, n->child_count > 0 ? n->children[0] : NULL);
            for (int i = 1; i < n->child_count; i++) {
                ASTNode* c = n->children[i];
                if (c && c->type == AST_CATCH_CLAUSE && c->child_count > 0 &&
                    completes(f, c->children[0])) any = 1;
            }
            return any;
        }
        case AST_EXPRESSION_STATEMENT:
            if (n->child_count > 0 && is_exit_call(n->children[0])) return 0;
            break;
        case AST_CLOSURE:
            /* A trailing block runs inline, once, where it is written: its
             * `return` returns from the function around it. */
            if (is_trailing_block(n)) return completes(f, closure_body(n));
            return 1;
        case AST_DEFER_STATEMENT:
            return 1;
        case AST_IF_EXPRESSION: {
            int t = completes(f, n->child_count > 1 ? n->children[1] : NULL);
            int e = completes(f, n->child_count > 2 ? n->children[2] : NULL);
            return t || e;
        }
        case AST_OR_ELSE:
            /* The handler runs only on an error; the value goes on. */
            for (int i = 0; i < n->child_count; i++) completes(f, n->children[i]);
            return 1;
        default:
            break;
    }
    /* Any other statement or expression completes when what it evaluates
     * does: only a trailing block in it can end the function. */
    int all = 1;
    for (int i = 0; i < n->child_count; i++)
        if (!completes(f, n->children[i])) all = 0;
    return all;
}

/* ---- saying why ---- */

typedef enum {
    WHY_EMPTY,       /* the body has no statements */
    WHY_STATEMENT,   /* its last statement goes on to the end */
    WHY_VALUE,       /* its last statement is a value nothing returns */
    WHY_IF,          /* an `if` with no `else` */
    WHY_LOOP,        /* a loop whose condition can end it */
    WHY_BREAK,       /* a `break` that leaves a loop or switch */
    WHY_MATCH,       /* a `match` that may run no arm */
    WHY_SWITCH       /* a `switch` with no `default` */
} Why;

typedef struct {
    Why why;
    ASTNode* at;       /* the statement */
    ASTNode* target;   /* WHY_BREAK: the loop or switch it leaves */
} Reason;

static Reason reason(Why why, ASTNode* at, ASTNode* target) {
    Reason r = { why, at, target };
    return r;
}

/* An expression with a value, as a match arm or an expression statement
 * holds one; a declaration or an assignment has a type and is no value. */
static int yields_value(ASTNode* e) {
    if (!e) return 0;
    switch (e->type) {
        case AST_BLOCK: case AST_PRINT_STATEMENT: case AST_VARIABLE_DECLARATION:
        case AST_ASSIGNMENT: case AST_COMPOUND_ASSIGNMENT: case AST_TUPLE_DESTRUCTURE:
        case AST_EXPRESSION_STATEMENT: case AST_DEFER_STATEMENT:
            return 0;
        default:
            return has_value_type(e->node_type);
    }
}

/* The first trailing block in the expressions of statement `n`. */
static ASTNode* trailing_block_in(ASTNode* n) {
    for (int i = 0; n && i < n->child_count; i++) {
        ASTNode* c = n->children[i];
        if (!c || (c->type == AST_CLOSURE && !is_trailing_block(c)) || c->type == AST_BLOCK)
            continue;
        if (is_trailing_block(c) && closure_body(c)) return c;
        ASTNode* inner = trailing_block_in(c);
        if (inner) return inner;
    }
    return NULL;
}

/* Why `n`, which completes, lets control through. */
static Reason explain(Flow* f, ASTNode* n) {
    switch (n->type) {
        case AST_BLOCK:
            if (n->child_count == 0) return reason(WHY_EMPTY, n, NULL);
            return explain(f, n->children[n->child_count - 1]);
        case AST_IF_STATEMENT: {
            ASTNode* then_b = n->child_count > 1 ? n->children[1] : NULL;
            ASTNode* else_b = n->child_count > 2 ? n->children[2] : NULL;
            int runs = if_branches(f, n);
            if ((runs & 1) && then_b && completes(f, then_b)) return explain(f, then_b);
            if ((runs & 2) && else_b) return explain(f, else_b);
            if (runs & 2) return reason(WHY_IF, n, NULL);
            break;
        }
        case AST_WHILE_LOOP:
        case AST_FOR_LOOP: {
            ASTNode* brk = loop_break(f, n);
            if (brk && brk != n) return reason(WHY_BREAK, brk, n);
            return reason(WHY_LOOP, n, NULL);
        }
        case AST_MATCH_STATEMENT:
            if (!match_covers(n)) return reason(WHY_MATCH, n, NULL);
            for (int i = 1; i < n->child_count; i++) {
                ASTNode* arm = n->children[i];
                if (!arm || arm->type != AST_MATCH_ARM || arm->child_count < 2) continue;
                ASTNode* body = arm->children[1];
                if (!completes(f, body)) continue;
                if (yields_value(body)) return reason(WHY_VALUE, n, NULL);
                return explain(f, body);
            }
            break;
        case AST_SWITCH_STATEMENT: {
            if (!switch_has_default(n)) return reason(WHY_SWITCH, n, NULL);
            ASTNode* brk = NULL;
            switch_cases_complete(f, n, &brk);
            if (brk && brk != n) return reason(WHY_BREAK, brk, n);
            for (int i = 1; i < n->child_count; i++) {
                ASTNode* c = n->children[i];
                if (!c || c->type != AST_CASE_STATEMENT || !case_completes(f, c)) continue;
                if (c->child_count > case_first_stmt(c))
                    return explain(f, c->children[c->child_count - 1]);
                return reason(WHY_STATEMENT, n, NULL);
            }
            break;
        }
        case AST_TRY_STATEMENT:
            if (n->child_count > 0 && completes(f, n->children[0]))
                return explain(f, n->children[0]);
            for (int i = 1; i < n->child_count; i++) {
                ASTNode* c = n->children[i];
                if (c && c->type == AST_CATCH_CLAUSE && c->child_count > 0 &&
                    completes(f, c->children[0]))
                    return explain(f, c->children[0]);
            }
            break;
        default:
            break;
    }
    /* A statement around a trailing block lets control through where the
     * block does. */
    ASTNode* block = trailing_block_in(n);
    if (block) return explain(f, closure_body(block));
    if (n->type == AST_EXPRESSION_STATEMENT && n->child_count > 0 &&
        yields_value(n->children[0]))
        return reason(WHY_VALUE, n, NULL);
    return reason(WHY_STATEMENT, n, NULL);
}

/* The expression `stmt` evaluates, as source, when it is short and plainly
 * spelled (a name or a field); NULL otherwise. */
static const char* plain_source(ASTNode* stmt, char* buf, size_t cap) {
    ASTNode* e = stmt;
    if (e && e->type == AST_EXPRESSION_STATEMENT && e->child_count > 0) e = e->children[0];
    if (!e || (e->type != AST_IDENTIFIER && e->type != AST_MEMBER_ACCESS)) return NULL;
    ContractStr s = { buf, cap, 0 };
    contract_sprint_expr(&s, e);
    contract_str_terminate(&s);
    if (s.off >= cap || strchr(buf, '?')) return NULL;
    return buf;
}

static void describe(Reason r, char* out, size_t cap) {
    int line = node_line(r.at);
    switch (r.why) {
        case WHY_EMPTY:
            snprintf(out, cap, "the body is empty: return a value");
            return;
        case WHY_VALUE: {
            char src[64];
            if (r.at->type == AST_MATCH_STATEMENT) {
                snprintf(out, cap,
                         "the value of the `match` on line %d is not returned: only an "
                         "arrow body, `-> { ... }`, returns its last expression; write "
                         "`return match ...`", line);
            } else if (plain_source(r.at, src, sizeof src)) {
                snprintf(out, cap,
                         "`%s` on line %d is not returned: only an arrow body, "
                         "`-> { ... }`, returns its last expression; write `return %s`",
                         src, line, src);
            } else {
                snprintf(out, cap,
                         "the value on line %d is not returned: only an arrow body, "
                         "`-> { ... }`, returns its last expression; write `return` "
                         "before it", line);
            }
            return;
        }
        case WHY_IF:
            snprintf(out, cap,
                     "the `if` on line %d has no `else`, so control goes on past it "
                     "when its condition is false: return a value after it", line);
            return;
        case WHY_LOOP:
            snprintf(out, cap,
                     "the loop on line %d ends when its condition is false: return a "
                     "value after it", line);
            return;
        case WHY_BREAK:
            snprintf(out, cap,
                     "the `break` on line %d leaves the %s on line %d: return a value "
                     "after it", line,
                     r.target->type == AST_SWITCH_STATEMENT ? "`switch`" : "loop",
                     node_line(r.target));
            return;
        case WHY_MATCH:
            snprintf(out, cap,
                     "the `match` on line %d has no `_` arm and may run no arm: add a "
                     "`_` arm that returns, or return a value after it", line);
            return;
        case WHY_SWITCH:
            snprintf(out, cap,
                     "the `switch` on line %d has no `default`: add one that returns, "
                     "or return a value after it", line);
            return;
        case WHY_STATEMENT:
        default:
            snprintf(out, cap,
                     "control goes on past line %d to the end: return a value on that "
                     "path", line);
            return;
    }
}

/* ---- the definitions ---- */

static ASTNode* unwrap_export(ASTNode* n) {
    if (n && n->type == AST_EXPORT_STATEMENT && n->child_count > 0) return n->children[0];
    return n;
}

static int is_definition(ASTNode* n) {
    return n && (n->type == AST_FUNCTION_DEFINITION || n->type == AST_BUILDER_FUNCTION);
}

/* Whether `fn` is one clause of a set: another definition shares its name. */
static int is_clause(ASTNode* program, ASTNode* fn) {
    if (!fn->value) return 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* d = unwrap_export(program->children[i]);
        if (d != fn && is_definition(d) && d->value && strcmp(d->value, fn->value) == 0)
            return 1;
    }
    return 0;
}

static void report(Flow* f, ASTNode* owner, ASTNode* body, const char* message) {
    char help[400];
    describe(explain(f, body), help, sizeof help);
    int line = body->end_line > 0 ? body->end_line : owner->line;
    int column = body->end_line > 0 ? body->end_column : owner->column;
    AetherError e = {
        owner->source_file, NULL, line, column, message, help, NULL,
        AETHER_ERR_MISSING_RETURN
    };
    aether_error_report(&e);
}

/* `fn`'s name as a caller writes it: a function merged in from a module is
 * named `<ns>_<name>` and is called `<ns>.<name>`. */
static const char* display_name(ASTNode* fn, char* buf, size_t cap) {
    const char* name = fn->value ? fn->value : "?";
    if (!fn->origin_module) return name;
    const char* ns = strrchr(fn->origin_module, '.');
    ns = ns ? ns + 1 : fn->origin_module;
    size_t n = strlen(ns);
    if (strncmp(name, ns, n) != 0 || name[n] != '_' || !name[n + 1]) return name;
    snprintf(buf, cap, "%s.%s", ns, name + n + 1);
    return buf;
}

static int check_definition(Flow* f, ASTNode* fn) {
    if (fn->child_count == 0) return 0;
    ASTNode* body = fn->children[fn->child_count - 1];
    if (!body || body->type != AST_BLOCK) return 0;
    int written = !fn->type_inferred && has_value_type(fn->node_type);
    if (!written && !returns_value(body)) return 0;
    if (!completes(f, body)) return 0;

    char message[400];
    char shown[160];
    const char* name = display_name(fn, shown, sizeof shown);
    const char* what = is_clause(f->program, fn) ? "this clause of " : "";
    if (has_value_type(fn->node_type)) {
        snprintf(message, sizeof message,
                 "missing return: control can reach the end of %s'%s', whose result is %s",
                 what, name, type_to_string(fn->node_type));
    } else {
        snprintf(message, sizeof message,
                 "missing return: control can reach the end of %s'%s', which returns a "
                 "value on another path", what, name);
    }
    report(f, fn, body, message);
    return 1;
}

static int check_closure(Flow* f, ASTNode* closure) {
    if (is_trailing_block(closure)) return 0;
    ASTNode* body = closure_body(closure);
    if (!body || !returns_value(body) || !completes(f, body)) return 0;
    report(f, closure, body,
           "missing return: control can reach the end of a closure that returns a "
           "value on another path");
    return 1;
}

static int check_closures_in(Flow* f, ASTNode* n) {
    int errors = 0;
    for (int i = 0; n && i < n->child_count; i++) {
        ASTNode* c = n->children[i];
        if (!c) continue;
        if (c->type == AST_CLOSURE) errors += check_closure(f, c);
        errors += check_closures_in(f, c);
    }
    return errors;
}

int check_missing_returns(ASTNode* program) {
    if (!program) return 0;
    Flow* f = calloc(1, sizeof(Flow));
    if (!f) return 0;
    f->program = program;
    int errors = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* d = unwrap_export(program->children[i]);
        if (!d) continue;
        if (is_definition(d)) errors += check_definition(f, d);
        errors += check_closures_in(f, d);
    }
    free(f);
    return errors;
}
