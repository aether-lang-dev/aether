/* Compile-time contract predicate evaluation — see contract_eval.h for the
 * contract (pun intended) and docs/contract-folding.md for the design. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "contract_eval.h"

#define CONTRACT_EVAL_MAX_DEPTH 32

/* ---------------------------------------------------------------------------
 * Value domain: {int64 | double | bool}.
 *
 * Int64-first is load-bearing, not a style choice. The evaluator this
 * replaced worked in double, where every integer above 2^53 loses exactness:
 * `x == 9007199254740993` with x = 9007199254740992 compares EQUAL in double
 * (both round to 2^53) — a diagnostic built on that would stay silent on a
 * violated contract and, worse, error on a satisfied one. Ints stay ints
 * until a float literal genuinely enters the expression.
 * ------------------------------------------------------------------------ */
typedef struct {
    enum { CV_INT, CV_FLOAT, CV_BOOL } kind;
    int64_t i;   /* CV_INT and CV_BOOL (0/1) */
    double  f;   /* CV_FLOAT */
} ConstVal;

static int cv_truthy(const ConstVal* v) {
    return v->kind == CV_FLOAT ? (v->f != 0.0) : (v->i != 0);
}

static double cv_as_double(const ConstVal* v) {
    return v->kind == CV_FLOAT ? v->f : (double)v->i;
}

/* Parse a literal node. Handles bool, decimal, hex (0x…, full uint64 range,
 * cast to int64 exactly as codegen emits hex literals), binary (0b…), and
 * float. Strings and anything unparseable refuse — the caller treats refusal
 * as UNKNOWN. */
static int parse_literal(ASTNode* e, ConstVal* out) {
    const char* v = e->value;
    if (!v || !*v) return 0;
    if (strcmp(v, "true") == 0)  { out->kind = CV_BOOL; out->i = 1; return 1; }
    if (strcmp(v, "false") == 0) { out->kind = CV_BOOL; out->i = 0; return 1; }
    if (e->node_type && e->node_type->kind == TYPE_STRING) return 0;

    int is_hex = (v[0] == '0' && (v[1] == 'x' || v[1] == 'X'));
    if (!is_hex && (strchr(v, '.') || strchr(v, 'e') || strchr(v, 'E'))) {
        char* end = NULL;
        double d = strtod(v, &end);
        if (!end || *end != '\0') return 0;
        out->kind = CV_FLOAT; out->f = d;
        return 1;
    }
    if (v[0] == '0' && (v[1] == 'b' || v[1] == 'B')) {
        if (!v[2]) return 0;
        uint64_t acc = 0;
        for (const char* p = v + 2; *p; p++) {
            if (*p != '0' && *p != '1') return 0;
            acc = (acc << 1) | (uint64_t)(*p - '0');
        }
        out->kind = CV_INT; out->i = (int64_t)acc;
        return 1;
    }
    char* end = NULL;
    uint64_t u = strtoull(v, &end, 0);   /* base 0: decimal, 0x, octal */
    if (!end || *end != '\0' || end == v) return 0;
    out->kind = CV_INT; out->i = (int64_t)u;
    return 1;
}

static int eval_value(ASTNode* e, ContractEnv* env, ConstVal* out, int depth);

/* Resolve an identifier to a compile-time value:
 *   1. a bound parameter (call-site folding) — the argument expression is
 *      evaluated WITHOUT the parameter bindings, since it lives in the
 *      caller's scope where the callee's parameter names mean nothing;
 *   2. a top-level `const` declaration's initializer;
 *   3. an enum member — by typecheck time resolve_enum_types has rewritten
 *      `E.M` to the bare identifier `E_M`, so we reconstruct each member's
 *      qualified name (explicit value, else previous + 1, first 0) and match.
 * Anything else refuses (locals, params of OTHER functions, globals). */
static int resolve_identifier(const char* name, ContractEnv* env,
                              ConstVal* out, int depth) {
    if (!name) return 0;

    if (env) {
        for (int i = 0; i < env->count; i++) {
            if (env->names[i] && strcmp(env->names[i], name) == 0) {
                ContractEnv caller_scope = {{0}, {0}, 0, env->program};
                return env->args[i] &&
                       eval_value(env->args[i], &caller_scope, out, depth + 1);
            }
        }
    }

    ASTNode* program = env ? env->program : NULL;
    if (!program) return 0;
    ContractEnv paramless = {{0}, {0}, 0, program};

    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_CONST_DECLARATION && c->value &&
            strcmp(c->value, name) == 0 && c->child_count > 0) {
            return eval_value(c->children[0], &paramless, out, depth + 1);
        }
    }

    for (int i = 0; i < program->child_count; i++) {
        ASTNode* en = program->children[i];
        if (!en || en->type != AST_ENUM_DEFINITION || !en->value) continue;
        int64_t next = 0;
        for (int m = 0; m < en->child_count; m++) {
            ASTNode* mem = en->children[m];
            if (!mem || mem->type != AST_ENUM_MEMBER || !mem->value) continue;
            if (mem->child_count > 0 && mem->children[0]) {
                ConstVal mv;
                if (!eval_value(mem->children[0], &paramless, &mv, depth + 1) ||
                    mv.kind == CV_FLOAT)
                    break;   /* opaque member value poisons the rest of this enum */
                next = mv.i;
            }
            char qualified[256];
            snprintf(qualified, sizeof(qualified), "%s_%s", en->value, mem->value);
            if (strcmp(qualified, name) == 0) {
                out->kind = CV_INT; out->i = next;
                return 1;
            }
            next++;
        }
    }
    return 0;
}

/* Evaluate an expression to a compile-time value. Returns 1 on success.
 * Deliberately refuses calls, member access, and indexing — the first is a
 * capability boundary (see the header), the rest are simply not compile-time
 * values. Integer arithmetic is performed in uint64 and cast back, so signed
 * overflow (UB in C) cannot occur inside the compiler itself. */
static int eval_value(ASTNode* e, ContractEnv* env, ConstVal* out, int depth) {
    if (!e || depth > CONTRACT_EVAL_MAX_DEPTH) return 0;

    switch (e->type) {
        case AST_LITERAL:
            return parse_literal(e, out);

        case AST_IDENTIFIER:
            return resolve_identifier(e->value, env, out, depth);

        case AST_UNARY_EXPRESSION: {
            if (e->child_count != 1 || !e->value) return 0;
            ConstVal v;
            if (!eval_value(e->children[0], env, &v, depth + 1)) return 0;
            if (strcmp(e->value, "!") == 0) {
                out->kind = CV_BOOL; out->i = !cv_truthy(&v);
                return 1;
            }
            if (strcmp(e->value, "-") == 0) {
                if (v.kind == CV_FLOAT) { out->kind = CV_FLOAT; out->f = -v.f; }
                else { out->kind = CV_INT; out->i = (int64_t)(0 - (uint64_t)v.i); }
                return 1;
            }
            if (strcmp(e->value, "+") == 0) { *out = v; return 1; }
            return 0;
        }

        case AST_BINARY_EXPRESSION: {
            if (e->child_count != 2 || !e->value) return 0;
            const char* op = e->value;
            ConstVal l, r;
            if (!eval_value(e->children[0], env, &l, depth + 1)) return 0;
            if (!eval_value(e->children[1], env, &r, depth + 1)) return 0;

            if (strcmp(op, "&&") == 0) {
                out->kind = CV_BOOL; out->i = cv_truthy(&l) && cv_truthy(&r);
                return 1;
            }
            if (strcmp(op, "||") == 0) {
                out->kind = CV_BOOL; out->i = cv_truthy(&l) || cv_truthy(&r);
                return 1;
            }

            int any_float = (l.kind == CV_FLOAT || r.kind == CV_FLOAT);

            /* Comparisons. Both-int compares exactly in int64 — the entire
             * reason this evaluator exists (see the domain note above). */
            int is_lt = !strcmp(op, "<"),  is_le = !strcmp(op, "<=");
            int is_gt = !strcmp(op, ">"),  is_ge = !strcmp(op, ">=");
            int is_eq = !strcmp(op, "=="), is_ne = !strcmp(op, "!=");
            if (is_lt || is_le || is_gt || is_ge || is_eq || is_ne) {
                int res;
                if (any_float) {
                    double a = cv_as_double(&l), b = cv_as_double(&r);
                    res = is_lt ? (a < b) : is_le ? (a <= b)
                        : is_gt ? (a > b) : is_ge ? (a >= b)
                        : is_eq ? (a == b) : (a != b);
                } else {
                    int64_t a = l.i, b = r.i;
                    res = is_lt ? (a < b) : is_le ? (a <= b)
                        : is_gt ? (a > b) : is_ge ? (a >= b)
                        : is_eq ? (a == b) : (a != b);
                }
                out->kind = CV_BOOL; out->i = res;
                return 1;
            }

            /* Arithmetic. */
            if (any_float) {
                double a = cv_as_double(&l), b = cv_as_double(&r);
                if (!strcmp(op, "+")) { out->f = a + b; }
                else if (!strcmp(op, "-")) { out->f = a - b; }
                else if (!strcmp(op, "*")) { out->f = a * b; }
                else if (!strcmp(op, "/")) {
                    if (b == 0.0) return 0;   /* the predicate judges; we don't trap */
                    out->f = a / b;
                }
                else return 0;               /* no float % */
                out->kind = CV_FLOAT;
                return 1;
            }
            uint64_t a = (uint64_t)l.i, b = (uint64_t)r.i;
            if (!strcmp(op, "+")) { out->i = (int64_t)(a + b); }
            else if (!strcmp(op, "-")) { out->i = (int64_t)(a - b); }
            else if (!strcmp(op, "*")) { out->i = (int64_t)(a * b); }
            else if (!strcmp(op, "/")) {
                if (r.i == 0 || (l.i == INT64_MIN && r.i == -1)) return 0;
                out->i = l.i / r.i;
            }
            else if (!strcmp(op, "%")) {
                if (r.i == 0 || (l.i == INT64_MIN && r.i == -1)) return 0;
                out->i = l.i % r.i;
            }
            else return 0;
            out->kind = CV_INT;
            return 1;
        }

        default:
            return 0;
    }
}

static ContractTri tri_eval(ASTNode* e, ContractEnv* env, int depth) {
    if (!e || depth > CONTRACT_EVAL_MAX_DEPTH) return CONTRACT_UNKNOWN;

    if (e->type == AST_UNARY_EXPRESSION && e->value &&
        strcmp(e->value, "!") == 0 && e->child_count == 1) {
        ContractTri t = tri_eval(e->children[0], env, depth + 1);
        if (t == CONTRACT_TRUE)  return CONTRACT_FALSE;
        if (t == CONTRACT_FALSE) return CONTRACT_TRUE;
        return CONTRACT_UNKNOWN;
    }

    /* Short-circuit connectives. The two verdicts are deliberately
     * ASYMMETRIC, because the two consumers use them differently:
     *
     * FALSE-dominance is symmetric (`unknown && false` is FALSE): a FALSE
     * verdict either becomes a compile error (nothing runs at all) or falls
     * through to the runtime check in codegen (which evaluates everything) —
     * neither path can skip an operand's runtime evaluation.
     *
     * TRUE-dominance is LEFT-TO-RIGHT ONLY (`true || x` is TRUE, but
     * `unknown || true` stays UNKNOWN): a TRUE verdict makes codegen ELIDE
     * the whole check, and a predicate operand can carry a side-effectful
     * call — the contracts doc explicitly promises `x || true` keeps the
     * runtime check for exactly this reason. Left-TRUE mirrors runtime
     * short-circuit order, so eliding never skips an effect the runtime
     * would have run. */
    if (e->type == AST_BINARY_EXPRESSION && e->value && e->child_count == 2) {
        if (strcmp(e->value, "&&") == 0) {
            ContractTri l = tri_eval(e->children[0], env, depth + 1);
            ContractTri r = tri_eval(e->children[1], env, depth + 1);
            if (l == CONTRACT_FALSE || r == CONTRACT_FALSE) return CONTRACT_FALSE;
            if (l == CONTRACT_TRUE && r == CONTRACT_TRUE)   return CONTRACT_TRUE;
            return CONTRACT_UNKNOWN;
        }
        if (strcmp(e->value, "||") == 0) {
            ContractTri l = tri_eval(e->children[0], env, depth + 1);
            if (l == CONTRACT_TRUE) return CONTRACT_TRUE;
            ContractTri r = tri_eval(e->children[1], env, depth + 1);
            if (l == CONTRACT_FALSE && r == CONTRACT_TRUE)  return CONTRACT_TRUE;
            if (l == CONTRACT_FALSE && r == CONTRACT_FALSE) return CONTRACT_FALSE;
            return CONTRACT_UNKNOWN;
        }
    }

    ConstVal v;
    if (eval_value(e, env, &v, depth))
        return cv_truthy(&v) ? CONTRACT_TRUE : CONTRACT_FALSE;
    return CONTRACT_UNKNOWN;
}

ContractTri contract_eval_predicate(ASTNode* pred, ContractEnv* env) {
    return tri_eval(pred, env, 0);
}

/* ---------------------------------------------------------------------------
 * Predicate-to-text rendering. Moved verbatim from codegen_stmt.c (where it
 * fed the runtime panic message) so the typechecker's compile-time error and
 * codegen's runtime panic render the same violation identically.
 * ------------------------------------------------------------------------ */

static void cstr_putc(ContractStr* s, char c) {
    /* Reserve one byte for the trailing NUL. */
    if (s->off + 1 < s->cap) s->buf[s->off++] = c;
}
static void cstr_puts(ContractStr* s, const char* str) {
    while (*str) cstr_putc(s, *str++);
}
void contract_str_terminate(ContractStr* s) {
    if (s->cap == 0) return;
    if (s->off >= s->cap) s->buf[s->cap - 1] = '\0';
    else s->buf[s->off] = '\0';
}

/* Round-trip a predicate-expression AST back to source-like text so the
 * diagnostic names the specific failed check. Best-effort — covers the
 * operator subset most contracts use. */
void contract_sprint_expr(ContractStr* s, ASTNode* e) {
    if (!e) { cstr_puts(s, "?"); return; }
    switch (e->type) {
        case AST_IDENTIFIER:
        case AST_LITERAL:
            if (e->value) cstr_puts(s, e->value);
            else cstr_puts(s, "?");
            return;
        case AST_NULL_LITERAL:
            cstr_puts(s, "null");
            return;
        case AST_BINARY_EXPRESSION:
            if (e->child_count == 2) {
                contract_sprint_expr(s, e->children[0]);
                cstr_putc(s, ' ');
                if (e->value) cstr_puts(s, e->value);
                cstr_putc(s, ' ');
                contract_sprint_expr(s, e->children[1]);
                return;
            }
            break;
        case AST_UNARY_EXPRESSION:
            if (e->child_count == 1) {
                int postfix = annotation_has_marker(e->annotation, "postfix");  /* #2457 */
                if (e->value && !postfix) cstr_puts(s, e->value);
                contract_sprint_expr(s, e->children[0]);
                if (e->value && postfix) cstr_puts(s, e->value);
                return;
            }
            break;
        case AST_MEMBER_ACCESS:
            if (e->child_count == 1) {
                contract_sprint_expr(s, e->children[0]);
                cstr_putc(s, '.');
                if (e->value) cstr_puts(s, e->value);
                return;
            }
            break;
        case AST_FUNCTION_CALL:
            if (e->value) cstr_puts(s, e->value);
            cstr_putc(s, '(');
            for (int i = 0; i < e->child_count; i++) {
                if (i) cstr_puts(s, ", ");
                contract_sprint_expr(s, e->children[i]);
            }
            cstr_putc(s, ')');
            return;
        default:
            break;
    }
    cstr_puts(s, "<expr>");
}

int contract_eval_int64(ASTNode* e, ASTNode* program, int64_t* out) {
    if (!e || !out) return 0;
    ContractEnv env;
    memset(&env, 0, sizeof env);
    env.program = program;
    ConstVal v;
    if (!eval_value(e, &env, &v, 0)) return 0;
    if (v.kind != CV_INT) return 0;
    *out = v.i;
    return 1;
}
