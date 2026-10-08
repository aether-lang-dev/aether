#include "codegen_internal.h"
#include "../analysis/sandbox_trust.h"
#include "../aether_defines.h"
#include "../aether_error.h"
#include "../../std/string/aether_string_abi.h"
#include <errno.h>
#include <limits.h>

/* Argument-temp lifetime management for nested heap-returning calls.
 *
 * A call like `combine(heap_value(), heap_value())` produces two
 * anonymous heap temporaries (the two `heap_value()` results) that
 * flow into `combine` and have nowhere to be reclaimed — pre-fix
 * this is a leak per call. The mechanism here hoists each heap-
 * returning function-call appearing in argument position into a
 * named temporary, calls the parent with the temporaries, and
 * frees them after the parent call returns. Wrapped in a GCC
 * statement-expression `({ ... })` so the call still composes in
 * any expression context.
 *
 * Generated shape (parent returns non-void):
 *
 *     ({ const char* _ad_0 = heap_value();
 *        const char* _ad_1 = heap_value();
 *        <ret_type> _ad_r = combine(_ad_0, _ad_1);
 *        free((void*)_ad_0);
 *        free((void*)_ad_1);
 *        _ad_r; })
 *
 * The substitution registry below is consulted at the very top of
 * generate_expression: when an AST_FUNCTION_CALL node has been
 * hoisted, its emission becomes the bare temp name instead of a
 * fresh call. Stack-disciplined — nested wraps push new entries and
 * pop them when their parent-call emission completes.
 *
 * Module-local static state: the codegen runs single-threaded per
 * process and `generate_expression` is the sole entry point. */
typedef struct ArgDrainSub {
    ASTNode* node;
    char*    name;
} ArgDrainSub;

static ArgDrainSub* g_arg_drain_subs = NULL;
static int g_arg_drain_count = 0;
static int g_arg_drain_cap   = 0;
static int g_arg_drain_counter = 0;

/* #1417: unique ids for the *StringSeq literal fold temps. */
static int g_seq_lit_counter = 0;

static const char* arg_drain_lookup(ASTNode* node) {
    if (!node) return NULL;
    for (int i = g_arg_drain_count - 1; i >= 0; i--) {
        if (g_arg_drain_subs[i].node == node) return g_arg_drain_subs[i].name;
    }
    return NULL;
}

/* #2478: operands evaluated into temps ahead of the construct that uses
 * them (emit_in_operand_order). generate_expression emits the temp for any
 * node bound here, of any type, so every emitter of the construct reads it.
 * Kept apart from the drain registry: a heap argument hoisted for order is
 * still drained (freed) by its call, which only skips arguments it finds
 * bound in its own registry. */
static ArgDrainSub* g_order_subs = NULL;
static int g_order_count = 0;
static int g_order_cap = 0;
static int g_order_counter = 0;
static ASTNode* g_order_emitting = NULL;

static const char* order_lookup(ASTNode* node) {
    for (int i = g_order_count - 1; i >= 0; i--) {
        if (g_order_subs[i].node == node) return g_order_subs[i].name;
    }
    return NULL;
}

/* A bare reference to a top-level Aether function, as opposed to a closure or
 * an fn-typed variable. Its address is a real C symbol, which is the only thing
 * a C function-pointer field can hold. */
/* Is `name` a parameter of the function currently being emitted?
 *
 * Parameters are AST_PATTERN_VARIABLE children of the AST_FUNCTION_DEFINITION,
 * ahead of its BLOCK. Closures carry AST_CLOSURE_PARAM instead, so both are
 * accepted; a closure's own parameters shadow just as a function's do. */
static int name_is_enclosing_param(CodeGenerator* gen, const char* name) {
    if (!gen || !gen->current_function || !name) return 0;
    ASTNode* fn = gen->current_function;
    for (int i = 0; i < fn->child_count; i++) {
        ASTNode* c = fn->children[i];
        if (!c) continue;
        if (c->type == AST_BLOCK) break;   /* params precede the body */
        if ((c->type == AST_PATTERN_VARIABLE || c->type == AST_CLOSURE_PARAM) &&
            c->value && strcmp(c->value, name) == 0) return 1;
    }
    return 0;
}

/* #datastar-10: does `name` come from `<name> = <expr> as *Struct` in the
 * function being emitted?
 *
 * Member access decides `->` vs `.` from the base's node_type. That type is
 * resolved for a `*T` PARAMETER, and for a cast local in ordinary statement
 * position -- but a cast local used inside a STRUCT LITERAL initialiser
 * arrives with TYPE_UNKNOWN, so the pointer deref silently lowered to `.` and
 * gcc rejected the generated C ("'c' is a pointer; did you mean to use '->'?").
 * The Aether source is valid and the error names C the user never wrote.
 *
 * Recovering the declaration here keeps the fix at the point of use and cannot
 * change any case that already had a resolved type: this is consulted ONLY on
 * the fallback path, after every typed branch has declined. */
static int name_is_ptr_struct_cast_local(CodeGenerator* gen, const char* name) {
    if (!gen || !gen->current_function || !name) return 0;
    ASTNode* fn = gen->current_function;
    for (int i = 0; i < fn->child_count; i++) {
        ASTNode* blk = fn->children[i];
        if (!blk || blk->type != AST_BLOCK) continue;
        for (int j = 0; j < blk->child_count; j++) {
            ASTNode* st = blk->children[j];
            if (!st) continue;
            /* `c = p as *Config` parses as a declaration or an assignment
             * depending on context; accept both shapes. */
            ASTNode* rhs = NULL;
            if (st->type == AST_VARIABLE_DECLARATION && st->value &&
                !strcmp(st->value, name) && st->child_count > 0) {
                rhs = st->children[0];
            } else if (st->type == AST_ASSIGNMENT && st->child_count >= 2 &&
                       st->children[0] && st->children[0]->type == AST_IDENTIFIER &&
                       st->children[0]->value && !strcmp(st->children[0]->value, name)) {
                rhs = st->children[1];
            }
            if (rhs && rhs->type == AST_PTR_AS_STRUCT_CAST) return 1;
        }
    }
    return 0;
}

static ASTNode* bare_top_level_fn(CodeGenerator* gen, ASTNode* node) {
    if (!gen || !gen->program || !node ||
        node->type != AST_IDENTIFIER || !node->value) return NULL;
    /* #1657: a LOCAL BINDING WINS. This function asks "is there a top-level
     * function with this name?", and after module merging that program
     * contains every module's functions — including non-exported ones from a
     * CONSUMING module. So a library function's parameter `on_press` matched
     * an app's unrelated module-level `on_press`, and the call site emitted a
     * bare-fn adapter for the app's function in place of the caller's
     * closure, discarding .env with it. Silent: the C is well-formed, the
     * call returns, and nothing happens.
     *
     * A parameter is the innermost binding there is, so nothing outside the
     * function may be reachable under that name. Checking the enclosing
     * function's own parameters is the scope rule this was missing; the
     * declared-locals list is consulted too, so a local `on_press = ...`
     * shadows equally.
     *
     * Same family as #1606, which fixed the closure-parameter case in the
     * module renamer. This is the codegen half, and an ordinary function
     * parameter shadowed from a DIFFERENT module, which survived that fix. */
    if (name_is_enclosing_param(gen, node->value) ||
        is_var_declared(gen, node->value)) return NULL;
    return find_function_definition_by_name(gen->program, node->value);
}

/* #1240: is `macc` a field of a C-owned struct (`extern struct`, with or
 * without @c_import / @packed)?
 *
 * It decides what may be stored in a function-pointer field. C reads those
 * fields and calls through them itself, so the field has to hold the
 * function's real address; the `_AeClosure` box used for Aether-owned callback
 * fields is a heap pointer, and C jumping to it faults on the first callback
 * with no diagnostic anywhere upstream. */
static int member_field_is_c_owned(CodeGenerator* gen, ASTNode* macc) {
    if (!gen || !gen->program || !macc ||
        macc->type != AST_MEMBER_ACCESS || macc->child_count < 1) return 0;
    Type* rt = macc->children[0] ? macc->children[0]->node_type : NULL;
    const char* sname = NULL;
    if (rt && rt->kind == TYPE_STRUCT) {
        sname = rt->struct_name;
    } else if (rt && rt->kind == TYPE_PTR && rt->element_type &&
               rt->element_type->kind == TYPE_STRUCT) {
        sname = rt->element_type->struct_name;
    }
    if (!sname) return 0;
    for (int i = 0; i < gen->program->child_count; i++) {
        ASTNode* sd = gen->program->children[i];
        if (sd && sd->type == AST_STRUCT_DEFINITION && sd->value &&
            strcmp(sd->value, sname) == 0) {
            return sd->annotation && strncmp(sd->annotation, "extern", 6) == 0;
        }
    }
    return 0;
}

/* Mint a unique temp name without registering it. The caller uses
 * this name for the temp's C declaration, then later registers the
 * substitution via arg_drain_bind. Splitting these lets the caller
 * reserve names BEFORE recursing into generate_expression (which
 * may itself mint inner temps and would otherwise collide). */
static char* arg_drain_mint_name(void) {
    char buf[64];
    snprintf(buf, sizeof(buf), "_ad_%d", g_arg_drain_counter++);
    return strdup(buf);
}

/* Bind a pre-minted temp name to an AST node. The substitution
 * lasts until arg_drain_truncate trims the registry back. */
static void arg_drain_bind(ASTNode* node, char* name) {
    if (g_arg_drain_count >= g_arg_drain_cap) {
        int new_cap = g_arg_drain_cap ? g_arg_drain_cap * 2 : 8;
        ArgDrainSub* bigger = (ArgDrainSub*)realloc(g_arg_drain_subs,
                                                    sizeof(ArgDrainSub) * (size_t)new_cap);
        /* CRITICAL: this used to free `name` and return. The caller in
         * emit_closure_env_drained_call keeps using that pointer to emit the
         * env teardown, so the failure path handed it freed memory. Returning
         * without binding is not recoverable either: the substitution never
         * happens and the call is emitted against a temp that was never
         * declared. Fail the way add_child does for the same situation. */
        if (!bigger) {
            fprintf(stderr, "Fatal: out of memory binding a call argument\n");
            exit(1);
        }
        g_arg_drain_subs = bigger;
        g_arg_drain_cap  = new_cap;
    }
    g_arg_drain_subs[g_arg_drain_count].node = node;
    g_arg_drain_subs[g_arg_drain_count].name = name;
    g_arg_drain_count++;
}

static void arg_drain_truncate(int target_count) {
    while (g_arg_drain_count > target_count) {
        g_arg_drain_count--;
        free(g_arg_drain_subs[g_arg_drain_count].name);
        g_arg_drain_subs[g_arg_drain_count].name = NULL;
        g_arg_drain_subs[g_arg_drain_count].node = NULL;
    }
}

/* One call's argument-temp wrap (see ArgDrainSub above): which arguments
 * are hoisted into `_ad_N` temps and freed after the call, and what the
 * wrap yields. A named call and a closure call (`call(f, ...)`, `f(...)` on
 * a closure local) share it; the closure call used to pass a heap-string
 * argument straight through, so nothing freed it (#2493). */
typedef struct ArgDrainWrap {
    int saved_count;    /* registry depth to truncate back to */
    int count;          /* hoisted arguments */
    int idx[16];        /* their child indices in the call node */
    int identity[16];   /* 1 = identity-guarded release (return-escape-only param) */
    int closure[16];    /* 1 = an owned closure (#2507), released through its env */
    int have_value;     /* the call yields a value: of C type ret_ct, or
                         * when that is NULL, of type ret_type */
    const char* ret_ct;
    Type* ret_type;
    int discarded;      /* the value is discarded at statement level */
} ArgDrainWrap;

/* Should the fresh heap argument at child `ai` of a call be freed after the
 * call? -1: no, the parameter keeps it; 0: yes; 1: yes unless the call
 * returns that very pointer. The callee is the named function `func_name`,
 * or, for a closure call, the closure literal `closure` whose parameter
 * `ai - first_arg` receives it. A closure call whose literal is not known
 * (an `fn` parameter, a variable bound to several closures) has no body to
 * read: its argument is freed only when no closure in the program keeps
 * one (gen->closure_args_borrowed, #2499), and otherwise taken to escape,
 * a leak, never a free under a parameter that kept the pointer.
 *
 * Escape decision for the arg's heap pointer. When the callee has a
 * VISIBLE BODY the body-walk is authoritative: it detects every storage
 * sink (return, assignment RHS, aggregate element, struct field, closure
 * capture, escaping nested call-arg), so a "does not escape" verdict is a
 * proof, and a ptr-typed param that is only read (e.g. an assert helper's
 * `got: ptr` compared via string.equals) can be safely drained. Without a
 * visible body (extern / unknown) we fall back to the conservative
 * call_arg_escapes heuristic, the same gate the escape walker uses; a
 * storage-shaped param is assumed to stash the pointer.
 *
 * Soundness: we only ADD a drain where non-escape is proven; anything
 * unprovable stays "escapes". False-escape = leak (safe); false-non-escape
 * = UAF (never introduced). */
static int arg_drain_verdict(CodeGenerator* gen, const char* func_name, ASTNode* closure,
                             int ai, int first_arg, const ArgDrainWrap* w) {
    if (!func_name) {
        if (!closure) return gen->closure_args_borrowed ? 0 : -1;
        int pi = ai - first_arg;
        /* A closure keeps a `string` parameter only through a reference of
         * its own (closure_string_param_kept), so the caller's is free. */
        for (int k = 0, seen = 0; k < closure->child_count; k++) {
            ASTNode* p = closure->children[k];
            if (!p || p->type != AST_CLOSURE_PARAM) continue;
            if (seen++ == pi) {
                if (p->node_type && p->node_type->kind == TYPE_STRING) return 0;
                break;
            }
        }
        if (!closure_param_escapes_via_body(gen, closure, pi, 1)) return 0;
        /* Return-escape only, into a string result: identity-guarded,
         * as for a named callee below. */
        if (!w->have_value || closure_param_escapes_via_body(gen, closure, pi, 0) ||
            !w->ret_ct || strcmp(w->ret_ct, "const char*") != 0) {
            return -1;
        }
        return 1;
    }
    /* @retain parameter: callee stores the pointer (list_add, map_put's
     * key, etc.). The heap value's lifetime is now the recipient's
     * responsibility, and freeing here would dangle the stored copy. Same
     * gate the escape walker uses at codegen_stmt.c:1339-1346. */
    if (is_retain_extern_param(gen, func_name, ai)) return -1;
    if (callee_has_visible_body(gen, func_name)) {
        /* A `string` argument the callee does not keep (callee_keeps_
         * string_arg: it captures it, reads it, or captures it in an env
         * of its own) is free after the call; a kept one goes on to the
         * identity guard below. */
        if (callee_param_is_string(gen, func_name, ai) &&
            !callee_keeps_string_arg(gen, func_name, ai, 0)) return 0;
        if (!callee_param_escapes_via_body(gen, func_name, ai, 0)) return 0;
        /* The param escapes. If it ONLY return-escapes (the callee passes
         * the value through / may return it) and does NOT store-escape,
         * and the callee returns a string, we can still reclaim this FRESH
         * temp at the call site with a pointer-identity guard: free it iff
         * the call did not return it (r != t). This is the sound fix for
         * the recursive accumulator (walk_join(t, concat(acc,sep,h))): the
         * intermediate concat is freed when consumed, preserved when
         * returned. Only FRESH heap-expr args reach here (the
         * AST_FUNCTION_CALL/INTERP gate in arg_drain_select), never a
         * borrowed var, so this can never free a value the caller still
         * holds.
         *
         * Otherwise (store-escape: a container/@retain/field owns it; or a
         * non-string / value-less call: no result pointer to compare)
         * leave it. */
        if (!w->have_value ||
            callee_param_store_escapes_via_body(gen, func_name, ai) ||
            !callee_returns_string(gen, func_name)) {
            return -1;
        }
        return 1;
    }
    TypeKind param_kind = lookup_callee_param_kind(gen, func_name, ai);
    if (call_arg_escapes(param_kind)) return -1;
    if (callee_param_escapes_via_body(gen, func_name, ai, 0)) return -1;
    return 0;
}

/* #2519: does a call to user function `func_name` yield a value in C? 1 yes,
 * 0 no (a void function), -1 not known (no single visible definition). The
 * rule generate_function emits the signature by: the declared type, else
 * `int` when the body returns a value, else void. */
static int callee_result_shape(CodeGenerator* gen, const char* func_name) {
    if (!callee_has_visible_body(gen, func_name)) return -1;
    const char* fn = codegen_normalise_callee(func_name);
    const DefClauses* dc = program_index_clauses(gen->program, fn);
    if (!dc || dc->count != 1 || !dc->nodes[0]) return -1;
    ASTNode* fdef = dc->nodes[0];
    Type* rt = fdef->node_type;
    if (rt && rt->kind != TYPE_VOID && rt->kind != TYPE_UNKNOWN) return 1;
    return has_return_value(fdef) ? 1 : 0;
}

/* #2507: may the owned closure at child `ai` of call `expr` be released once
 * the call returns? Only with proof its parameter keeps nothing: an extern
 * parameter declared `@noescape` (#2523), a named callee with a visible
 * body whose parameter neither escapes nor is returned (a builder's
 * injected `_ctx` shifts its parameters, so builders are left alone), or a
 * known closure literal's parameter, likewise. */
static int owned_closure_arg_drainable(CodeGenerator* gen, ASTNode* expr, const char* func_name,
                                       ASTNode* closure, int ai, int first_arg) {
    if (func_name) {
        if (is_noescape_extern_param(gen, func_name, ai)) return 1;
        if (!callee_has_visible_body(gen, func_name)) return 0;
        const char* fn = codegen_normalise_callee(func_name);
        ASTNode* fdef = find_function_definition_by_name(gen->program, fn);
        if (!fdef || fdef->child_count == 0 || !fdef->children[0] ||
            (fdef->children[0]->value && strcmp(fdef->children[0]->value, "_ctx") == 0)) return 0;
        for (int i = 0; i < expr->child_count; i++) {
            ASTNode* a = expr->children[i];
            if (a && (a->type == AST_NAMED_ARG ||
                      (a->type == AST_CLOSURE && a->value && strcmp(a->value, "trailing") == 0))) return 0;
        }
        return !callee_param_escapes_via_body(gen, func_name, ai, 0);
    }
    if (!closure) return 0;
    return !closure_param_escapes_via_body(gen, closure, ai - first_arg, 1);
}

/* Pick the arguments of `expr` (from child `first_arg` on) to hoist. The
 * caller has set have_value, ret_ct and discarded. A VOID parent (e.g. an
 * assert-style helper `check(label, string.from_long(n), want)`) yields no
 * value, but its heap-returning inline args still leak: they are drained
 * by a statement-expression that yields void, `({ T t=...; call(...);
 * free(t); })`, valid wherever a void call appears (it is always a
 * statement, so `({...});` is well-formed C). A value of unknown type is
 * drained only when it is discarded at the statement level, where the wrap
 * yields void and is safe regardless of the declared type. Args that are
 * themselves substitution-registered (inside a parent wrap already) are
 * left to the registered entry. */
static void arg_drain_select(CodeGenerator* gen, ASTNode* expr, int first_arg,
                             const char* func_name, ASTNode* closure, int is_void,
                             ArgDrainWrap* w) {
    w->saved_count = g_arg_drain_count;
    w->count = 0;
    if (!w->have_value && !is_void) return;
    for (int ai = first_arg; ai < expr->child_count && w->count < 16; ai++) {
        ASTNode* arg = expr->children[ai];
        if (!arg) continue;
        /* Arg-temp wrapping fires on heap-classified subexpressions:
         * function calls, AST_STRING_INTERP (`_aether_interp(...)`
         * allocates a heap buffer the caller is expected to own; inline as
         * an argument it leaked ~34 bytes per call on the
         * `outer(string_returning_call("seed-${i}"))` shape), and
         * AST_OR_ELSE (`f(g() or { ... })` yields a uniformly-heap string,
         * since the `or` lowering boxes both paths, with no consumer). */
        /* #2518: a capturing closure literal, as #2507 does a handed-over
         * closure: its only reference is the argument (a list that stores
         * it takes its own). */
        int literal_arg = arg->type == AST_CLOSURE &&
                          !(arg->value && strcmp(arg->value, "trailing") == 0);
        if (arg->type != AST_FUNCTION_CALL &&
            arg->type != AST_STRING_INTERP &&
            arg->type != AST_OR_ELSE && !literal_arg) continue;
        if (arg_drain_lookup(arg)) continue;
        /* #2507: a closure a call hands over (call_returns_owned_closure),
         * passed on as an argument anywhere in an expression: dead after
         * the call when the parameter keeps nothing. */
        if (literal_arg || call_returns_owned_closure(gen, arg)) {
            if (owned_closure_arg_drainable(gen, expr, func_name, closure, ai, first_arg)) {
                w->identity[w->count] = 0;
                w->closure[w->count] = 1;
                w->idx[w->count++] = ai;
            }
            continue;
        }
        if (!is_heap_string_expr(gen, arg)) continue;
        int verdict = arg_drain_verdict(gen, func_name, closure, ai, first_arg, w);
        if (verdict < 0) continue;
        w->identity[w->count] = verdict;
        w->closure[w->count] = 0;
        w->idx[w->count++] = ai;
    }
}

/* Open the wrap: declare and fill each temp, bind the arguments to them
 * (generate_expression then emits the temp's name for each), and start the
 * `_ad_r` result. The callee's own emission follows. */
static void arg_drain_open(CodeGenerator* gen, ASTNode* expr, ArgDrainWrap* w) {
    if (w->count == 0) return;
    char* names[16] = {0};
    fprintf(gen->output, "({ ");
    /* Pre-mint all temp names BEFORE recursing into generate_expression:
     * the recursion may itself open inner wraps and mint their own temps,
     * advancing g_arg_drain_counter. Reserving names up front keeps the
     * outer and inner names distinct. */
    for (int h = 0; h < w->count; h++) {
        names[h] = arg_drain_mint_name();
    }
    /* Emit each temp's decl. The arg's generate_expression may register
     * inner substitutions on the registry stack; those are bound to
     * inner-arg nodes and won't collide with our outer names because the
     * counter advanced. */
    for (int h = 0; h < w->count; h++) {
        ASTNode* arg = expr->children[w->idx[h]];
        /* Non-const: the temp stands in for the argument at whatever
         * parameter it feeds, and a callee taking `void*` (or `char*`) got
         * -Wincompatible-pointer-types-discards-qualifiers from a `const
         * char*` temp. The value is a fresh heap string this wrap owns and
         * frees, so there is nothing const about it. */
        if (w->closure[h]) {
            fprintf(gen->output, "_AeClosure %s = ", names[h]);
            generate_expression(gen, arg);
            fprintf(gen->output, "; ");
            continue;
        }
        fprintf(gen->output, "char* %s = (char*)(", names[h]);
        generate_expression(gen, arg);
        fprintf(gen->output, "); ");
    }
    /* Now bind the outer-arg to outer-temp substitutions. The bind
     * transfers ownership of the name string; arg_drain_truncate frees
     * it. */
    for (int h = 0; h < w->count; h++) {
        arg_drain_bind(expr->children[w->idx[h]], names[h]);
    }
    /* A void parent emits the call bare (no _ad_r); the statement-
     * expression yields void via the final free in arg_drain_close. */
    if (w->have_value) {
        fprintf(gen->output, "%s _ad_r = ", w->ret_ct ? w->ret_ct : get_c_type(w->ret_type));
    }
}

/* Close the wrap, if one was opened: the per-temp free, the statement-
 * expression's yield value, and the closing brace. */
static void arg_drain_close(CodeGenerator* gen, ASTNode* expr, ArgDrainWrap* w) {
    if (w->count == 0) return;
    fprintf(gen->output, "; ");
    for (int h = 0; h < w->count; h++) {
        /* Look up the temp name we registered. Names are stable across
         * the wrap's scope. */
        const char* nm = arg_drain_lookup(expr->children[w->idx[h]]);
        if (!nm) continue;
        if (w->closure[h]) {
            fprintf(gen->output, "_aether_closure_env_release(%s.env); ", nm);
        } else if (w->identity[h] && !w->discarded) {
            /* Return-escape-only param: free the fresh temp ONLY if the
             * call did not return it (string_release is magic-guarded; the
             * temp is always a magic string-op result here, never a
             * literal). */
            fprintf(gen->output,
                    "if ((const char*)_ad_r != %s) string_release(%s); ", nm, nm);
        } else {
            fprintf(gen->output, "aether_heap_str_free(%s); ", nm);
        }
    }
    if (w->have_value) {
        /* In statement position the yield is discarded, and a bare
         * `_ad_r;` there is -Wunused-value in the user's own build
         * (`fs.delete("${dir}/f")` warned). Cast it away: the wrap then
         * yields void, which is all a statement wants. */
        fprintf(gen->output, w->discarded ? "(void)_ad_r; })" : "_ad_r; })");
    } else {
        /* void parent: the trailing free is the last statement, so the
         * ({...}) yields void. */
        fprintf(gen->output, "})");
    }
    arg_drain_truncate(w->saved_count);
}

/* Returns 1 if the expression has any side effects (function calls, sends).
 * Shared by the series-collapse optimizer in codegen_stmt.c and the
 * interpolation-segment hoist below. */
int codegen_expr_has_side_effects(ASTNode* node) {
    if (!node) return 0;
    if (node->type == AST_FUNCTION_CALL ||
        node->type == AST_SEND_FIRE_FORGET ||
        node->type == AST_SEND_ASK ||
        // va_arg advances the va_list each evaluation; va_start/va_end
        // mutate it too. Treating them as impure stops the series-
        // collapse optimizer from hoisting/folding them (which would
        // read the wrong number of varargs). Issue #536.
        node->type == AST_VA_ARG ||
        node->type == AST_VA_START ||
        node->type == AST_VA_END) return 1;
    for (int i = 0; i < node->child_count; i++) {
        if (codegen_expr_has_side_effects(node->children[i])) return 1;
    }
    return 0;
}

/* --- String-interpolation segment helpers (#2195) --------------------
 *
 * An AST_STRING_INTERP's children alternate between text pieces (string
 * literals, spliced into the format string) and `${expr}` segments (the
 * printf varargs). */

/* A text piece of the interpolation, as opposed to a `${expr}` segment. */
int interp_segment_is_text(ASTNode* ch) {
    return ch && ch->type == AST_LITERAL && ch->node_type &&
           ch->node_type->kind == TYPE_STRING;
}

/* A `${expr}` segment that is a CALL producing an owned heap string, or a
 * nested interpolation (always heap), which the enclosing interpolation
 * must free once printf / _aether_interp has read it. */
int interp_segment_is_heap_call(CodeGenerator* gen, ASTNode* ch) {
    if (!ch) return 0;
    if (ch->type != AST_FUNCTION_CALL && ch->type != AST_STRING_INTERP) return 0;
    TypeKind tk = ch->node_type ? ch->node_type->kind : TYPE_UNKNOWN;
    if (tk != TYPE_STRING && tk != TYPE_PTR) return 0;
    return is_heap_string_expr(gen, ch);
}

/* --- Variables bound to a closure literal (#2513) -----------------------
 *
 * closure_var_map records which closure literal a variable holds, so
 * `call(f)` can call that closure's C function directly. A variable is
 * named by the scope that declares it as well as by its name. Keyed by the
 * name alone, `f()` on an `fn` parameter of one function dispatched to the
 * closure bound to a local `f` of an unrelated function, with the
 * parameter's env. A scope is named as discover_closures_scoped names it:
 * a function's name, "main", or a receive arm's or a hoisted closure's
 * synthetic name. In a closure's scope, a name the closure captures is the
 * enclosing scope's variable; any other name (a parameter, a local) is the
 * closure's own. */

static void closure_scope_name(ASTNode* closure, char* buf, size_t n);

/* The gen->closures entry of the hoisted closure whose scope is `scope`. */
static int closure_info_for_scope(CodeGenerator* gen, const char* scope) {
    if (!scope || strncmp(scope, "__closure_", 10) != 0) return -1;
    for (int k = 0; k < gen->closure_count; k++) {
        char nm[64];
        closure_scope_name(gen->closures[k].closure_node, nm, sizeof(nm));
        if (strcmp(nm, scope) == 0) return k;
    }
    return -1;
}

/* The scope whose variable `name` is, seen from `scope`. */
static const char* closure_var_owner(CodeGenerator* gen, const char* scope,
                                     const char* name) {
    for (int depth = 0; depth < 256; depth++) {
        int k = closure_info_for_scope(gen, scope);
        if (k < 0) return scope;
        int captured = 0;
        for (int j = 0; j < gen->closures[k].capture_count && !captured; j++)
            captured = strcmp(gen->closures[k].captures[j], name) == 0;
        if (!captured) return scope;
        scope = gen->closures[k].parent_func;
    }
    return scope;
}

static int closure_var_index(CodeGenerator* gen, const char* scope, const char* name) {
    if (!name) return -1;
    const char* owner = closure_var_owner(gen, scope, name);
    for (int i = 0; i < gen->closure_var_count; i++) {
        const struct ClosureVarMap* e = &gen->closure_var_map[i];
        if (!e->var_name || strcmp(e->var_name, name) != 0) continue;
        if (e->scope ? (owner && strcmp(e->scope, owner) == 0) : !owner) return i;
    }
    return -1;
}

/* The closure literal `name` holds in `scope`, or -1 when it holds none
 * this program can name (a parameter, a call's result, or a variable bound
 * to more than one literal). */
int closure_var_id(CodeGenerator* gen, const char* scope, const char* name) {
    int i = closure_var_index(gen, scope, name);
    return i >= 0 ? gen->closure_var_map[i].closure_id : -1;
}

/* Record that `name` in `scope` is bound to closure `cid`. A variable
 * bound to two different literals has no single one: -1. */
void closure_var_bind(CodeGenerator* gen, const char* scope, const char* name, int cid) {
    int i = closure_var_index(gen, scope, name);
    if (i >= 0) {
        if (gen->closure_var_map[i].closure_id != cid) gen->closure_var_map[i].closure_id = -1;
        return;
    }
    if (gen->closure_var_count >= gen->closure_var_capacity) {
        gen->closure_var_capacity = gen->closure_var_capacity ? gen->closure_var_capacity * 2 : 16;
        gen->closure_var_map = aether_xrealloc(gen->closure_var_map,
            gen->closure_var_capacity * sizeof(gen->closure_var_map[0]));
    }
    const char* owner = closure_var_owner(gen, scope, name);
    gen->closure_var_map[gen->closure_var_count].scope = owner ? strdup(owner) : NULL;
    gen->closure_var_map[gen->closure_var_count].var_name = strdup(name);
    gen->closure_var_map[gen->closure_var_count].closure_id = cid;
    gen->closure_var_count++;
}

/* --- Left-to-right operand order (#2478) ------------------------------
 *
 * C leaves unspecified the order in which a call's arguments, the two
 * operands of `+` or `<`, and the initialisers of a compound literal or an
 * array are evaluated, and two unsequenced writes of one object
 * (`f(i++, i++)`) are undefined behaviour. GCC evaluates call arguments
 * right to left on the Windows target, so `f(i++, i)` read the incremented
 * `i` first. Aether evaluates operands left to right: when an operand
 * conflicts with a later one (one writes a variable the other reads or
 * writes, or a call in one can change what the other reads), the earlier
 * one is evaluated first, into a temporary, in source order. Operands no
 * later one conflicts with stay inline, where they run after every
 * temporary, so a list without such a pair is emitted as it always was. */

static int is_assignment_op(const char* op);

/* The variable an assignment target or a `++` / `--` operand writes: the
 * name itself, or the root of a target reached through a field or an
 * index. */
static const char* order_lvalue_root(ASTNode* lhs) {
    while (lhs) {
        if (lhs->type == AST_IDENTIFIER) return lhs->value;
        if ((lhs->type == AST_MEMBER_ACCESS || lhs->type == AST_ARRAY_ACCESS) &&
            lhs->child_count > 0) {
            lhs = lhs->children[0];
            continue;
        }
        return NULL;
    }
    return NULL;
}

/* The variable `node` itself writes (`v++`, `--v`, `v = e`, `a[i] += e`),
 * or NULL. */
static const char* order_write_target(ASTNode* node) {
    if (node->type == AST_UNARY_EXPRESSION && node->value && node->child_count == 1 &&
        (strcmp(node->value, "++") == 0 || strcmp(node->value, "--") == 0))
        return order_lvalue_root(node->children[0]);
    if (((node->type == AST_BINARY_EXPRESSION && is_assignment_op(node->value)) ||
         node->type == AST_ASSIGNMENT) && node->child_count >= 1)
        return order_lvalue_root(node->children[0]);
    if (node->type == AST_COMPOUND_ASSIGNMENT || node->type == AST_VARIABLE_DECLARATION)
        return node->value;
    /* `va_arg(vap, T)` advances the list it reads. */
    if (node->type == AST_VA_ARG && node->child_count >= 1)
        return order_lvalue_root(node->children[0]);
    return NULL;
}

/* A closure literal's body runs when it is called, not where the literal
 * stands, so it writes nothing and calls nothing there. It does read the
 * variables it captures as it is made (a copy of each, unless a closure
 * writes it), so a mention inside it is a read at the literal (#2516):
 * `f(i++, || { return i })` makes the closure after the step. */

static int order_mentions(ASTNode* node, const char* name) {
    if (!node || !name) return 0;
    if (node->type == AST_IDENTIFIER && node->value && strcmp(node->value, name) == 0) return 1;
    for (int i = 0; i < node->child_count; i++)
        if (order_mentions(node->children[i], name)) return 1;
    return 0;
}

/* Does `x` write a variable that `y` reads or writes? */
static int order_writes_what_other_uses(ASTNode* x, ASTNode* y) {
    if (!x || x->type == AST_CLOSURE) return 0;
    const char* t = order_write_target(x);
    if (t && order_mentions(y, t)) return 1;
    for (int i = 0; i < x->child_count; i++)
        if (order_writes_what_other_uses(x->children[i], y)) return 1;
    return 0;
}

static int order_has_call(ASTNode* node) {
    if (!node || node->type == AST_CLOSURE) return 0;
    if (node->type == AST_FUNCTION_CALL || node->type == AST_SEND_FIRE_FORGET ||
        node->type == AST_SEND_ASK || node->type == AST_VA_ARG ||
        node->type == AST_VA_START || node->type == AST_VA_END) return 1;
    for (int i = 0; i < node->child_count; i++)
        if (order_has_call(node->children[i])) return 1;
    return 0;
}

/* What a call can change. A call reaches no plain local of its caller: it
 * can write a module global, a variable it shares with a closure it runs
 * (a promoted capture), and memory it is handed by reference (a pointer, an
 * array, a slice). What a function of this program writes is read off its
 * body, through the functions it calls. A call whose body the compiler
 * cannot see is opaque: a C extern, a C function pointer, a closure, and a
 * function of the program that makes such a call. An opaque call may write
 * anything (a C global, a buffer reached through a stored pointer, the
 * state behind a handle it is given), so it is ordered against every later
 * operand that calls anything, reads a shared variable, or reads memory
 * through a pointer, a field or an index (#2524). Nothing in a declaration
 * narrows it: a mark on the parameters an extern writes through would still
 * leave its other effects unknown, and holding the earlier operand in a
 * temporary costs nothing once the C compiler has optimised. Operands that
 * read only plain locals and literals stay inline. */

typedef struct OrderFnEffects {
    ASTNode* fn;
    int opaque;                /* makes a call the compiler cannot see */
    int writes_shared;         /* a module global, or runs a closure */
    unsigned writes_through;   /* bit k: memory its parameter k reaches */
} OrderFnEffects;

/* The definition a call runs, when it is a function of this program, or
 * the declaration of the extern it is. */
static ASTNode* order_callee_def(CodeGenerator* gen, ASTNode* call) {
    if (!call->value || !gen->program) return NULL;
    const char* fn = codegen_normalise_callee(call->value);
    if (!fn) return NULL;
    ASTNode* def = find_function_definition_by_name(gen->program, fn);
    if (def) return def;
    ProgramIndex* ix = program_index(gen->program);
    return ix ? (ASTNode*)strmap_get(&ix->externs, fn) : NULL;
}

static int order_is_closure_type(Type* t) {
    return t && t->kind == TYPE_FUNCTION && !t->is_fnptr;
}

/* Does `call` run a closure: `call(f, ...)`, or a callee handed a closure
 * it may run? A closure called by its name (`f(x)`) is `call(f, x)` here:
 * the typechecker rewrites every call of a closure-typed name to it. */
static int order_call_runs_closure(ASTNode* call) {
    for (int i = 0; i < call->child_count; i++) {
        ASTNode* a = call->children[i];
        if (a && (a->type == AST_CLOSURE || order_is_closure_type(a->node_type))) return 1;
    }
    return !call->value || strcmp(call->value, "call") == 0;
}

/* The variable an argument hands a call by reference (a pointer, an array
 * or a slice, or a field or element reached from one), or NULL. */
static const char* order_ref_arg_root(ASTNode* arg) {
    while (arg && (arg->type == AST_SLICE_FROM_ARRAY || arg->type == AST_SLICE_TO_PTR ||
                   arg->type == AST_NAMED_ARG) &&
           arg->child_count > 0)
        arg = arg->children[0];
    if (!arg || !arg->node_type ||
        (arg->node_type->kind != TYPE_PTR && arg->node_type->kind != TYPE_ARRAY)) return NULL;
    return order_lvalue_root(arg);
}

/* The lvalue `node` writes through (`p.x = v`, `a[i]++`), or NULL for a
 * bare variable, whose write a callee keeps to itself. */
static ASTNode* order_through_lvalue(ASTNode* node) {
    ASTNode* lv = NULL;
    if (node->type == AST_UNARY_EXPRESSION && node->value && node->child_count == 1 &&
        (strcmp(node->value, "++") == 0 || strcmp(node->value, "--") == 0))
        lv = node->children[0];
    else if (((node->type == AST_BINARY_EXPRESSION && is_assignment_op(node->value)) ||
              node->type == AST_ASSIGNMENT) && node->child_count >= 1)
        lv = node->children[0];
    return (lv && (lv->type == AST_MEMBER_ACCESS || lv->type == AST_ARRAY_ACCESS)) ? lv : NULL;
}

static int order_param_index(ASTNode* fn, const char* name) {
    for (int i = 0, k = 0; name && i < fn->child_count; i++) {
        ASTNode* p = fn->children[i];
        if (!p || p->type != AST_PATTERN_VARIABLE) continue;
        if (p->value && strcmp(p->value, name) == 0) return k < 32 ? k : -1;
        k++;
    }
    return -1;
}

static int order_fn_effects(CodeGenerator* gen, ASTNode* fn);

/* The effects of the call `call`: its callee's entry in gen->order_fn_effects,
 * or -1 for a call the compiler cannot see through (an extern, a C function
 * pointer, a closure, a callee it cannot find). */
static int order_call_effects(CodeGenerator* gen, ASTNode* call) {
    if (order_call_runs_closure(call)) return -1;
    ASTNode* def = order_callee_def(gen, call);
    if (!def || def->type == AST_EXTERN_FUNCTION) return -1;
    int idx = order_fn_effects(gen, def);
    return gen->order_fn_effects[idx].opaque ? -1 : idx;
}

/* The effects of the calls in `node` as seen from `fn`'s own body. */
static void order_scan_body(CodeGenerator* gen, ASTNode* fn, ASTNode* node,
                            int* opaque, int* shared, unsigned* through) {
    if (!node || node->type == AST_CLOSURE) return;
    const char* t = order_write_target(node);
    if (t && is_module_global_var(gen, t)) *shared = 1;
    ASTNode* lv = order_through_lvalue(node);
    int k = lv ? order_param_index(fn, order_lvalue_root(lv)) : -1;
    if (k >= 0) *through |= 1u << k;
    if (node->type == AST_FUNCTION_CALL) {
        int idx = order_call_effects(gen, node);
        if (idx < 0) {
            *opaque = 1;
        } else {
            if (gen->order_fn_effects[idx].writes_shared) *shared = 1;
            unsigned c_through = gen->order_fn_effects[idx].writes_through;
            for (int i = 0, a = 0; i < node->child_count && a < 32; i++) {
                ASTNode* arg = node->children[i];
                if (arg && arg->type == AST_CLOSURE && arg->value &&
                    strcmp(arg->value, "trailing") == 0) continue;
                if (c_through & (1u << a)) {
                    int pk = order_param_index(fn, order_ref_arg_root(arg));
                    if (pk >= 0) *through |= 1u << pk;
                }
                a++;
            }
        }
    } else if (node->type == AST_SEND_FIRE_FORGET || node->type == AST_SEND_ASK ||
               node->type == AST_VA_ARG) {
        /* A handler runs code the body does not show; a varargs read
         * advances state the caller handed over. */
        *opaque = 1;
    }
    for (int i = 0; i < node->child_count; i++)
        order_scan_body(gen, fn, node->children[i], opaque, shared, through);
}

/* The memoised effects of calling `fn`, a function of this program: an
 * index into gen->order_fn_effects. A function reached again while its own
 * body is read (recursion) answers with what is known so far. */
static int order_fn_effects(CodeGenerator* gen, ASTNode* fn) {
    for (int i = 0; i < gen->order_fn_effect_count; i++)
        if (gen->order_fn_effects[i].fn == fn) return i;
    if (gen->order_fn_effect_count >= gen->order_fn_effect_capacity) {
        int cap = gen->order_fn_effect_capacity ? gen->order_fn_effect_capacity * 2 : 32;
        gen->order_fn_effects = aether_xrealloc(gen->order_fn_effects,
                                                (size_t)cap * sizeof(OrderFnEffects));
        gen->order_fn_effect_capacity = cap;
    }
    int idx = gen->order_fn_effect_count++;
    gen->order_fn_effects[idx] = (OrderFnEffects){ fn, 0, 0, 0 };
    int opaque = 0;
    int shared = 0;
    unsigned through = 0;
    for (int i = 0; i < fn->child_count; i++)
        if (fn->children[i] && fn->children[i]->type == AST_BLOCK)
            order_scan_body(gen, fn, fn->children[i], &opaque, &shared, &through);
    gen->order_fn_effects[idx].opaque = opaque;
    gen->order_fn_effects[idx].writes_shared = shared;
    gen->order_fn_effects[idx].writes_through = through;
    return idx;
}

/* A module global or a variable shared with a closure. */
static int order_is_shared_var(CodeGenerator* gen, const char* name) {
    return name && (is_module_global_var(gen, name) || is_promoted_capture(gen, name));
}

static int order_reads_shared_var(CodeGenerator* gen, ASTNode* node) {
    if (!node || node->type == AST_CLOSURE) return 0;
    if (node->type == AST_IDENTIFIER && order_is_shared_var(gen, node->value)) return 1;
    for (int i = 0; i < node->child_count; i++)
        if (order_reads_shared_var(gen, node->children[i])) return 1;
    return 0;
}

static int order_writes_shared_var(CodeGenerator* gen, ASTNode* node) {
    if (!node || node->type == AST_CLOSURE) return 0;
    if (order_is_shared_var(gen, order_write_target(node))) return 1;
    for (int i = 0; i < node->child_count; i++)
        if (order_writes_shared_var(gen, node->children[i])) return 1;
    return 0;
}

/* Does `node` read memory reached from `root`: a field or an element of it,
 * or `root` handed to a call by reference (a callee may read through it)?
 * `root` NULL matches any such read. */
static int order_reads_through(ASTNode* node, const char* root) {
    if (!node || node->type == AST_CLOSURE) return 0;
    if (node->type == AST_MEMBER_ACCESS || node->type == AST_ARRAY_ACCESS) {
        const char* r = order_lvalue_root(node);
        if (r && (!root || strcmp(r, root) == 0)) return 1;
    }
    if (node->type == AST_FUNCTION_CALL) {
        for (int i = 0; i < node->child_count; i++) {
            const char* r = order_ref_arg_root(node->children[i]);
            if (r && (!root || strcmp(r, root) == 0)) return 1;
        }
    }
    for (int i = 0; i < node->child_count; i++)
        if (order_reads_through(node->children[i], root)) return 1;
    return 0;
}

/* Can a call in `x` change what `y` reads, or read what `y` writes? */
static int order_call_affects(CodeGenerator* gen, ASTNode* x, ASTNode* y) {
    if (!x || x->type == AST_CLOSURE) return 0;
    if (x->type == AST_FUNCTION_CALL || x->type == AST_SEND_ASK ||
        x->type == AST_SEND_FIRE_FORGET || x->type == AST_VA_ARG) {
        /* Any call may read a shared variable `y` writes. */
        if (order_writes_shared_var(gen, y)) return 1;
        /* A send runs a handler, a varargs read advances the list: opaque. */
        int idx = x->type == AST_FUNCTION_CALL ? order_call_effects(gen, x) : -1;
        int shared = idx < 0 || gen->order_fn_effects[idx].writes_shared;
        unsigned through = idx < 0 ? 0 : gen->order_fn_effects[idx].writes_through;
        /* A shared variable it writes, read by `y` or by a call in `y`. */
        if (shared && (order_reads_shared_var(gen, y) || order_has_call(y))) return 1;
        /* An opaque call may write any memory `y` reads through. */
        if (idx < 0 && order_reads_through(y, NULL)) return 1;
        for (int i = 0, a = 0; i < x->child_count && a < 32; i++) {
            ASTNode* arg = x->children[i];
            if (arg && arg->type == AST_CLOSURE && arg->value &&
                strcmp(arg->value, "trailing") == 0) continue;
            if (through & (1u << a)) {
                const char* r = order_ref_arg_root(arg);
                if (r && order_reads_through(y, r)) return 1;
            }
            a++;
        }
    }
    for (int i = 0; i < x->child_count; i++)
        if (order_call_affects(gen, x->children[i], y)) return 1;
    return 0;
}

/* Must `x` and `y` run in source order? `gen` NULL weighs the writes
 * operands make themselves only. */
static int order_conflict(CodeGenerator* gen, ASTNode* x, ASTNode* y) {
    if (order_writes_what_other_uses(x, y) || order_writes_what_other_uses(y, x)) return 1;
    if (!gen) return 0;
    return order_call_affects(gen, x, y) || order_call_affects(gen, y, x);
}

/* An operand that can be held in a temporary. A literal has no order to
 * keep, and a fixed-size array's value is where it lives, which no operand
 * changes. A closure literal is a value like any other: the temp holds the
 * same env (#2516). */
static int order_hoistable(ASTNode* op) {
    if (!op || op->type == AST_LITERAL || op->type == AST_NULL_LITERAL) return 0;
    return !(op->node_type && type_is_sized_array(op->node_type));
}

/* An operand already standing for a temp: evaluated, with nothing left to
 * order. */
static int order_bound(ASTNode* op) {
    return op && (order_lookup(op) || arg_drain_lookup(op));
}

/* Mark in `hoist` the operands a later operand conflicts with; 1 if any. */
static int order_hoist_set(CodeGenerator* gen, ASTNode** ops, int n, int* hoist) {
    int any = 0;
    for (int x = 0; x < n; x++) {
        hoist[x] = 0;
        if (!order_hoistable(ops[x]) || order_bound(ops[x])) continue;
        for (int y = x + 1; y < n; y++) {
            if (ops[y] && !order_bound(ops[y]) && order_conflict(gen, ops[x], ops[y])) {
                hoist[x] = 1;
                any = 1;
                break;
            }
        }
    }
    return any;
}

#define ORDER_MAX_OPERANDS 32

/* The indexes an assignment's target evaluates before the store, in source
 * order (`a[i][j] = v` reads `i` then `j`), appended to `ops` (#2516). */
static int order_target_operands(ASTNode* lhs, ASTNode** ops, int n, int cap) {
    if (!lhs) return n;
    if (lhs->type == AST_ARRAY_ACCESS && lhs->child_count >= 2) {
        n = order_target_operands(lhs->children[0], ops, n, cap);
        if (n < cap) ops[n++] = lhs->children[1];
    } else if (lhs->type == AST_MEMBER_ACCESS && lhs->child_count > 0) {
        n = order_target_operands(lhs->children[0], ops, n, cap);
    }
    return n;
}

/* The operands of a construct whose C spelling leaves their order open: a
 * call's arguments (its receiver among them, a named argument's value in
 * its place), the two sides of a binary operator other than `&&` / `||`
 * (sequenced in C), the indexes of an assignment's target and then its
 * value (`arr[i++] = i` stores at the old `i`, #2516), the field values of
 * a struct literal or a message, and a send's target with its message's
 * fields. 0 for anything else, and for a platform `select`, which emits
 * only the value it picks. */
static int order_operands(ASTNode* expr, ASTNode** ops, int cap) {
    int n = 0;
    switch (expr->type) {
        case AST_FUNCTION_CALL:
            if (expr->value && strcmp(expr->value, "select") == 0) return 0;
            for (int i = 0; i < expr->child_count; i++) {
                ASTNode* a = expr->children[i];
                if (a && a->type == AST_CLOSURE && a->value &&
                    strcmp(a->value, "trailing") == 0) continue;
                if (a && a->type == AST_NAMED_ARG) a = a->child_count > 0 ? a->children[0] : NULL;
                if (n == cap) return 0;
                ops[n++] = a;
            }
            return n;
        case AST_BINARY_EXPRESSION:
            if (expr->child_count != 2 || !expr->value ||
                strcmp(expr->value, "&&") == 0 || strcmp(expr->value, "||") == 0) return 0;
            if (is_assignment_op(expr->value)) {
                n = order_target_operands(expr->children[0], ops, 0, cap);
                if (n == 0 || n == cap) return 0;
                ops[n++] = expr->children[1];
                return n;
            }
            ops[0] = expr->children[0];
            ops[1] = expr->children[1];
            return 2;
        case AST_SEND_FIRE_FORGET:
        case AST_SEND_ASK:
        case AST_STRUCT_LITERAL:
        case AST_MESSAGE_CONSTRUCTOR: {
            ASTNode* fields = expr;
            if (expr->type == AST_SEND_FIRE_FORGET || expr->type == AST_SEND_ASK) {
                if (expr->child_count < 2 || !expr->children[1] ||
                    expr->children[1]->type != AST_MESSAGE_CONSTRUCTOR) return 0;
                ops[n++] = expr->children[0];
                fields = expr->children[1];
            }
            for (int i = 0; i < fields->child_count; i++) {
                ASTNode* fi = fields->children[i];
                if (!fi || (fi->type != AST_ASSIGNMENT && fi->type != AST_FIELD_INIT) ||
                    fi->child_count < 1) continue;
                if (n == cap) return 0;
                ops[n++] = fi->children[0];
            }
            return n;
        }
        default:
            return 0;
    }
}

static void order_bind(ASTNode* node, const char* name) {
    if (g_order_count >= g_order_cap) {
        int cap = g_order_cap ? g_order_cap * 2 : 8;
        ArgDrainSub* bigger = (ArgDrainSub*)realloc(g_order_subs, sizeof(ArgDrainSub) * (size_t)cap);
        if (!bigger) {
            fprintf(stderr, "Fatal: out of memory ordering operands\n");
            exit(1);
        }
        g_order_subs = bigger;
        g_order_cap = cap;
    }
    g_order_subs[g_order_count].node = node;
    g_order_subs[g_order_count].name = strdup(name);
    g_order_count++;
}

/* Evaluate operand `op` into a fresh temp and bind it: `__auto_type _eoN = (op); `. */
static void order_hoist_operand(CodeGenerator* gen, ASTNode* op) {
    char buf[32];
    snprintf(buf, sizeof(buf), "_eo%d", g_order_counter++);
    fprintf(gen->output, "__auto_type %s = (", buf);
    generate_expression(gen, op);
    fprintf(gen->output, "); ");
    order_bind(op, buf);
}

/* A field read evaluated ahead of a call that frees it: handed over where
 * it is read (emit_string_field_handoff), once, in its place in the order.
 * At the call it is the temp, and the struct the path named then may not
 * be the one a later operand left there. */
static void order_hoist_handoff(CodeGenerator* gen, ASTNode* op) {
    char buf[32];
    snprintf(buf, sizeof(buf), "_eo%d", g_order_counter++);
    fprintf(gen->output, "__auto_type %s = (", buf);
    emit_string_field_handoff(gen, op);
    fprintf(gen->output, "); ");
    order_bind(op, buf);
}

/* Is operand `k` of `call` a field read handed to a parameter the callee
 * frees? Only where operands and parameters line up one to one: no named
 * argument, trailing block or injected builder context. */
static int order_operand_hands_off(CodeGenerator* gen, ASTNode* call, int k) {
    if (!call || call->type != AST_FUNCTION_CALL || !call->value ||
        is_builder_func_reg(gen, call->value)) return 0;
    for (int i = 0; i < call->child_count; i++) {
        ASTNode* a = call->children[i];
        if (!a || a->type == AST_NAMED_ARG ||
            (a->type == AST_CLOSURE && a->value && strcmp(a->value, "trailing") == 0)) return 0;
    }
    return k < call->child_count && field_read_can_hand_off(call->children[k]) &&
           callee_consumes_string_arg(gen, call->value, k);
}

int order_prelude_depth(void) {
    return g_order_count;
}

void order_prelude_end(int depth) {
    while (g_order_count > depth) {
        g_order_count--;
        free(g_order_subs[g_order_count].name);
    }
}

/* #2478: an initializer list (an array literal's `{...}`, the tuple a
 * multi-value `return` builds) leaves the order of its elements open as a
 * call's arguments do, and cannot be wrapped in a statement expression. So
 * the elements a later one conflicts with are evaluated into temps as
 * statements ahead of the declaration, the stores or the `return` the list
 * feeds, bound until the statement ends (generate_statement gives them
 * back). `target` is the array an element-by-element store writes (#2289),
 * or NULL: an element that reads it is evaluated before the first store,
 * so `a = [a[1], a[0]]` swaps. */
void order_prelude_begin(CodeGenerator* gen, ASTNode** items, int n, const char* target) {
    if (!items || n < 1 || n > ORDER_MAX_OPERANDS) return;
    int hoist[ORDER_MAX_OPERANDS];
    int any = n >= 2 ? order_hoist_set(gen, items, n, hoist) : 0;
    if (n < 2) hoist[0] = 0;
    for (int k = 0; target && k < n; k++) {
        if (order_hoistable(items[k]) && !order_bound(items[k]) &&
            order_mentions(items[k], target)) {
            hoist[k] = 1;
            any = 1;
        }
    }
    if (!any) return;
    for (int k = 0; k < n; k++)
        if (hoist[k]) order_hoist_operand(gen, items[k]);
}

/* Emit `expr` with the operands a later one conflicts with evaluated first,
 * in source order, into temps it then reads:
 *     ({ __auto_type _eo0 = (i++); f(_eo0, i); })
 * Returns 0, emitting nothing, when no operand needs it. */
static int emit_in_operand_order(CodeGenerator* gen, ASTNode* expr) {
    ASTNode* ops[ORDER_MAX_OPERANDS];
    int hoist[ORDER_MAX_OPERANDS];
    if (order_bound(expr)) return 0;   /* emitted as the temp that holds it */
    int n = order_operands(expr, ops, ORDER_MAX_OPERANDS);
    if (n < 2 || !order_hoist_set(gen, ops, n, hoist)) return 0;
    int saved = g_order_count;
    fprintf(gen->output, "({ ");
    for (int k = 0; k < n; k++) {
        if (!hoist[k]) continue;
        if (order_operand_hands_off(gen, expr, k)) order_hoist_handoff(gen, ops[k]);
        else order_hoist_operand(gen, ops[k]);
    }
    ASTNode* saved_emitting = g_order_emitting;
    g_order_emitting = expr;
    generate_expression(gen, expr);
    g_order_emitting = saved_emitting;
    fprintf(gen->output, "; })");
    order_prelude_end(saved);
    return 1;
}

/* Index of the last child that must be evaluated into a temp to keep the
 * segments in source order, or -1 when none must. C leaves vararg
 * evaluation order unspecified, so once any segment has a side effect
 * every `${}` segment of the interpolation is hoisted: the impure ones so
 * they run left to right, and the pure ones because a read of `c.n` on
 * either side of `bump(c)` sees a different value depending on which ran
 * first. An all-pure interpolation, or one whose only `${}` segment is
 * the impure one, has no order to observe and stays inline. Heap-call
 * freeing is a separate concern, see interp_segment_is_heap_call.
 *
 * #2478: a write is an effect too. `"${j++} ${j++} ${j}"` printed `1 0 2`:
 * a segment that writes a variable a later segment reads or writes is
 * hoisted, up to the last such segment. */
int interp_order_hoist_boundary(ASTNode* interp) {
    int expr_count = 0, last_expr = -1, impure = 0, last_written = -1;
    for (int i = 0; i < interp->child_count; i++) {
        ASTNode* ch = interp->children[i];
        if (interp_segment_is_text(ch)) continue;
        expr_count++;
        last_expr = i;
        if (codegen_expr_has_side_effects(ch)) impure = 1;
        for (int j = i + 1; j < interp->child_count; j++) {
            ASTNode* later = interp->children[j];
            if (interp_segment_is_text(later)) continue;
            if (order_conflict(NULL, ch, later)) { last_written = i; break; }
        }
    }
    if (!impure && last_written >= 0) return last_written;
    return (impure && expr_count >= 2) ? last_expr : -1;
}

/* The C type a `${expr}` segment's temp is declared with, matching the
 * cast EMIT_INTERP_ARGS applies for the same TypeKind, or NULL for a kind
 * the interpolation has no scalar/string temp shape for (left inline). */
const char* interp_temp_c_type(Type* t) {
    if (!t) return "int";   /* untyped segment: formatted with %d */
    switch (t->kind) {
        case TYPE_STRING:
        case TYPE_PTR:      return "const char*";
        case TYPE_BOOL:
        case TYPE_INT:      return "int";
        case TYPE_INT64:
        case TYPE_DURATION: return "long long";
        case TYPE_UINT32:   return "unsigned int";
        case TYPE_UINT64:   return "unsigned long long";
        case TYPE_FLOAT:    return "double";
        case TYPE_FLOAT32:  return "float";
        case TYPE_LONGDOUBLE: return "long double";
        default:            return NULL;
    }
}

/* Emit one `${expr}` segment as a printf vararg: the hoisted temp's name
 * when the segment was evaluated ahead of the call, else the expression
 * itself. */
static void interp_emit_segment(CodeGenerator* gen, ASTNode* ch, const char* hoisted) {
    if (hoisted) fprintf(gen->output, "%s", hoisted);
    else generate_expression(gen, ch);
}

/* Emit `call` as a block that frees the env of a TRANSIENT capturing
 * closure argument after the call returns. The closure is hoisted into an
 * `_AeClosure` temp, the call is emitted with the closure substituted by
 * that temp (so the receiver gets the same value), and the temp's heap
 * env is freed once the call has run:
 *
 *   { _AeClosure _ad_N = <closure>; callee(.., _ad_N, ..);
 *     if (_ad_N.env) free((void*)_ad_N.env); }
 *
 * The env free is conditional, so a zero-capture closure (env == NULL) is
 * a no-op. SOUNDNESS is the caller's responsibility: it must only invoke
 * this when the receiving parameter neither stores nor returns the closure
 * (verified via callee_param_escapes_via_body) — otherwise the receiver
 * would keep a pointer into the freed env. */
void emit_closure_env_drained_call(CodeGenerator* gen, ASTNode* call,
                                   ASTNode* closure_node) {
    int saved = g_arg_drain_count;
    char* nm = arg_drain_mint_name();
    fprintf(gen->output, "{ _AeClosure %s = ", nm);
    generate_expression(gen, closure_node);   /* unbound here -> real closure */
    fprintf(gen->output, "; ");
    arg_drain_bind(closure_node, nm);          /* now substitutes in the call */
    generate_expression(gen, call);
    /* #1398: member-aware teardown, so the env gives back the references its
       string captures own. A value that is not a literal in the registry (an
       owned closure a call returned, #2506) is released through its env
       header: envs are reference-counted (#2494), so a plain free() would
       ignore another holder. */
    int env_id = -1;
    for (int ci = 0; ci < gen->closure_count; ci++) {
        if (gen->closures[ci].closure_node == closure_node) {
            env_id = gen->closures[ci].id;
            break;
        }
    }
    if (env_id >= 0) {
        fprintf(gen->output, "; if (%s.env) _closure_env_%d_free((void*)%s.env); }",
                nm, env_id, nm);
    } else {
        fprintf(gen->output, "; _aether_closure_env_release(%s.env); }", nm);
    }
    arg_drain_truncate(saved);                 /* frees nm */
}

/* Return 1 when `c_func_name` is a stdlib C function that already
 * dispatches on the AetherString magic header internally (via
 * `str_data` / `str_len` in std/string/aether_string.c). Such functions
 * MUST receive the wrapped pointer — unwrapping at the call site
 * defeats their length-aware path and falls back to strlen, which
 * truncates binary content at the first NUL.
 *
 * The list is pragmatic: every stdlib C function whose param is
 * declared `string` accepts both AetherString* and char* via the
 * dispatch helpers. By contrast, user-defined C externs (the
 * primary motivation for #297) typically just `memcpy` /  `strlen`
 * the input and need the unwrapped payload pointer.
 *
 * Maintained as a name-prefix check for now. A cleaner alternative
 * — annotating the extern declaration ("this param expects raw
 * bytes" vs. "this param dispatches") — is deferred until the
 * stdlib settles which extern shapes are part of the public ABI.
 */
static int is_stdlib_string_aware_extern(const char* c_func_name) {
    if (!c_func_name) return 0;
    /* The stdlib's string-aware C functions all live in
     * std/string/aether_string.c and either start with "string_" or
     * "aether_string_". A handful of other stdlib helpers also use
     * str_data/str_len internally (json_*, http_*, fs_*, etc.) — but
     * the safe default is "wrap unless prefix-matched." If a
     * downstream wrapper turns out to need the unwrapped form, the
     * fix is to add it here; if a user-defined function happens to
     * match a prefix and wants the raw form, it can be renamed. */
    if (strncmp(c_func_name, "string_", 7) == 0) return 1;
    if (strncmp(c_func_name, "aether_string_", 14) == 0) return 1;
    return 0;
}

/* #2210: the arguments of a call through a typed function pointer
 * (`fn(T1, ...) -> R`): a cast local, a `fn(...)` parameter or a struct
 * field. The callee is C, so a `string` parameter is its `const char*` and
 * the argument goes as its bytes through aether_string_data, as a call to
 * an extern passes one. A heap string (interpolated, concatenated, built by
 * a string op) is an AetherString whose header would otherwise reach the
 * callee in place of the characters; aether_string_data yields the payload
 * for either shape. Every other parameter takes the expression as written. */
static void generate_fnptr_call_args(CodeGenerator* gen, Type* sig, ASTNode* call) {
    for (int i = 0; i < call->child_count; i++) {
        ASTNode* arg = call->children[i];
        if (i > 0) fprintf(gen->output, ", ");
        Type* param = (sig && sig->param_types && i < sig->param_count)
                      ? sig->param_types[i] : NULL;
        if (param && param->kind == TYPE_STRING && arg && arg->node_type &&
            (arg->node_type->kind == TYPE_STRING || arg->node_type->kind == TYPE_PTR)) {
            fprintf(gen->output, "aether_string_data(");
            generate_expression(gen, arg);
            fprintf(gen->output, ")");
        } else {
            generate_expression(gen, arg);
        }
    }
}

/* A call through a typed fn-pointer local: `((R (*)(T1, T2))(fp))(a, b)`.
 * The local stores a void*, so the cast gives the C compiler the signature
 * the checker recorded for it. The local is spelled as its declaration
 * spelled it (safe_value_name: keyword mangling only). safe_c_name would
 * rename a local called `free` to `ae_free` while its declaration kept
 * `free`, so the emitted C referenced a variable that did not exist. */
/* 1 when a call through a typed C function pointer returns `bool`.
 *
 * An Aether `bool` is a C `int`, so the call is typed `int (*)(...)`, but
 * the function behind the pointer is usually C, and a C `bool` return
 * defines only the low byte of the return register: the upper bytes are
 * whatever was there before. On Windows x64 `is_even(7)` read true that
 * way (#2200). So the call's result is narrowed to that byte and back to
 * 0/1 — `(_Bool)(unsigned char)(call)` — which is also exact for an Aether
 * function in the pointer, whose full-int 0/1 has the same low byte. The
 * value is fixed rather than the pointer type: a `_Bool (*)(...)` type
 * would no longer accept an Aether function's `int (*)(...)` address.
 * Only when the value is used: a narrowed call as a bare statement is a
 * computed-but-unused value, which -Wunused-value (an error under
 * HARDEN=1) rejects, and a discarded result needs no narrowing anyway. */
static int fnptr_returns_bool(Type* sig) {
    return sig && sig->return_type && sig->return_type->kind == TYPE_BOOL;
}

static void generate_fnptr_local_call(CodeGenerator* gen, Type* sig,
                                      const char* local_name, ASTNode* call,
                                      int discarded) {
    const char* ret_c = sig->return_type ? get_c_type(sig->return_type) : "void";
    int narrow = !discarded && fnptr_returns_bool(sig);
    if (narrow) fprintf(gen->output, "((_Bool)(unsigned char)(");
    fprintf(gen->output, "((%s(*)(", ret_c);
    for (int pi = 0; pi < sig->param_count; pi++) {
        if (pi > 0) fprintf(gen->output, ", ");
        fprintf(gen->output, "%s", get_c_type(sig->param_types[pi]));
    }
    if (sig->param_count == 0) fprintf(gen->output, "void");
    fprintf(gen->output, "))(%s))(", safe_value_name(local_name));
    generate_fnptr_call_args(gen, sig, call);
    fprintf(gen->output, ")");
    if (narrow) fprintf(gen->output, "))");
}

/* The declaration of `name` inside `n`: a parameter, a local or a closure
 * parameter, carrying the type the checker stamped on it. */
static ASTNode* find_declaration_in(ASTNode* n, const char* name) {
    if (!n) return NULL;
    if ((n->type == AST_VARIABLE_DECLARATION || n->type == AST_PATTERN_VARIABLE ||
         n->type == AST_CLOSURE_PARAM) && n->value && strcmp(n->value, name) == 0) {
        return n;
    }
    for (int i = 0; i < n->child_count; i++) {
        ASTNode* found = find_declaration_in(n->children[i], name);
        if (found) return found;
    }
    return NULL;
}

/* #2210: the signature of the function-pointer field `recv.field`, where
 * `recv` is a struct or pointer-to-struct local of the function being
 * emitted (the receiver the checker admitted for a "fnfield_" call). The
 * type is read off the struct definition, as the checker read it; NULL
 * when the receiver's declaration or the field is not found, and the call
 * then passes its arguments as written. */
static Type* fnptr_field_signature(CodeGenerator* gen, const char* recv, const char* field) {
    ASTNode* decl = find_declaration_in(gen->current_function, recv);
    Type* t = decl ? decl->node_type : NULL;
    const char* sname = NULL;
    if (t && t->kind == TYPE_STRUCT) sname = t->struct_name;
    else if (t && t->kind == TYPE_PTR && t->element_type &&
             t->element_type->kind == TYPE_STRUCT) sname = t->element_type->struct_name;
    ASTNode* sdef = sname ? find_struct_definition_by_name(gen->program, sname) : NULL;
    if (!sdef) return NULL;
    for (int i = 0; i < sdef->child_count; i++) {
        ASTNode* f = sdef->children[i];
        if (f && f->value && strcmp(f->value, field) == 0) {
            return is_fnptr_type(f->node_type) ? f->node_type : NULL;
        }
    }
    return NULL;
}

/* Translate an Aether integer-literal text into a form C accepts.
 *
 *   0o777   → 0777        (C uses bare leading-zero for octal)
 *   0O777   → 0777
 *   0b1010  → 0xA          (C99 has no binary; transcode to hex,
 *                           ULL suffix when wider than 32 bits so
 *                           the C compiler picks a wide-enough type
 *                           for shifts at width 32+)
 *   0x...   → unchanged    (already valid C)
 *   123     → unchanged    (decimal — the C compiler widens as needed)
 *   18446744073709551615 → ...ULL (a decimal past LLONG_MAX has no C
 *                           type without the suffix; gcc takes it as
 *                           unsigned with a warning, which -Werror
 *                           builds of the generated C turn into an
 *                           error)
 *
 * Returns the translated form (interned), or the original `value` if no
 * translation is needed. An octal literal's digits have no length bound
 * (leading zeros), so the form is never built in a fixed buffer: one that
 * did not fit fell back to the untranslated `0o...`, which is not C (#2539).
 * The previous lexer eagerly decimalised these forms,
 * so this path is new — pre-cherry-pick code never had to worry
 * about it because the literal had been collapsed by the time it
 * reached codegen.
 */
static const char* translate_integer_literal(const char* value) {
    if (!value || !value[0]) return value;
    if (value[0] != '0' || !value[1]) {
        if (value[0] < '1' || value[0] > '9') return value;
        errno = 0;
        char* end = NULL;
        unsigned long long magnitude = strtoull(value, &end, 10);
        if (errno == 0 && end && *end == '\0' && magnitude > (unsigned long long)LLONG_MAX) {
            return cg_internf("%sULL", value);
        }
        return value;
    }
    char p = value[1];
    if (p == 'x' || p == 'X') return value;
    if (p == 'o' || p == 'O') {
        /* 0o777 → 0777. A bare '0' is also valid C octal for zero. */
        return cg_internf("0%s", value + 2);
    }
    if (p == 'b' || p == 'B') {
        /* Walk the binary digits and accumulate into uint64_t, then
         * format as hex. Aether binary literals top out at 64 bits
         * (the wider-uint inference picks UINT64 above that). */
        unsigned long long acc = 0;
        for (const char* q = value + 2; *q; q++) {
            if (*q == '0' || *q == '1') {
                acc = (acc << 1) | (unsigned)(*q - '0');
            } else {
                /* Unexpected char in a binary literal — fall back. */
                return value;
            }
        }
        const char* suffix = (acc > 0xFFFFFFFFULL) ? "ULL" : "";
        return cg_internf("0x%llx%s", acc, suffix);
    }
    return value;
}

/* Is `expr`'s left operand a link of the same operator chain — a binary
 * node whose operator sits at the same C precedence level, on plain
 * numbers? C is left-associative at every binary level, so `a - b - c`
 * already groups as `(a - b) - c` and the link needs no parentheses of
 * its own. Parenthesising it anyway made every link of `x0 + x1 + … + xN`
 * one bracket deeper, and clang stops at 256 (#2071). Numeric only:
 * a bit_set `-`, an optional `==` or a string operand takes a lowering
 * of its own above the generic path. */
static int left_operand_is_chain_link(ASTNode* expr) {
    static const char* const classes[][3] = {
        {"+", "-", NULL}, {"*", "/", "%"}, {"<<", ">>", NULL},
        {"&", NULL, NULL}, {"|", NULL, NULL}, {"^", NULL, NULL},
    };
    ASTNode* left = expr->children[0];
    if (!expr->value || left->type != AST_BINARY_EXPRESSION || !left->value ||
        left->child_count < 2 || !left->node_type) {
        return 0;
    }
    switch (left->node_type->kind) {
        case TYPE_INT: case TYPE_INT64: case TYPE_UINT64: case TYPE_FLOAT:
        case TYPE_BYTE: case TYPE_UINT8: case TYPE_UINT16: case TYPE_UINT32:
            break;
        default:
            return 0;
    }
    for (size_t c = 0; c < sizeof(classes) / sizeof(classes[0]); c++) {
        int parent = 0, child = 0;
        for (int i = 0; i < 3 && classes[c][i]; i++) {
            if (strcmp(expr->value, classes[c][i]) == 0) parent = 1;
            if (strcmp(left->value, classes[c][i]) == 0) child = 1;
        }
        if (parent && child) return 1;
    }
    return 0;
}

static int duration_unit_ns_codegen(const char* unit, long long* out) {
    if (!unit || !out) return 0;
    if (strcmp(unit, "ns") == 0) { *out = 1LL; return 1; }
    if (strcmp(unit, "us") == 0) { *out = 1000LL; return 1; }
    if (strcmp(unit, "ms") == 0) { *out = 1000000LL; return 1; }
    if (strcmp(unit, "s") == 0)  { *out = 1000000000LL; return 1; }
    if (strcmp(unit, "m") == 0)  { *out = 60LL * 1000000000LL; return 1; }
    if (strcmp(unit, "h") == 0)  { *out = 60LL * 60LL * 1000000000LL; return 1; }
    if (strcmp(unit, "d") == 0)  { *out = 24LL * 60LL * 60LL * 1000000000LL; return 1; }
    return 0;
}

static long long parse_duration_literal_ns(const char* value) {
    if (!value) return 0;
    const char* p = value;
    long double total = 0.0L;
    while (*p) {
        char* end = NULL;
        long double amount = strtold(p, &end);
        if (end == p) break;
        p = end;
        char unit[3] = {0, 0, 0};
        if ((p[0] == 'n' || p[0] == 'u' || p[0] == 'm') && p[1] == 's') {
            unit[0] = p[0]; unit[1] = p[1]; p += 2;
        } else if (*p == 's' || *p == 'm' || *p == 'h' || *p == 'd') {
            unit[0] = *p; p++;
        } else {
            break;
        }
        long long scale = 0;
        if (!duration_unit_ns_codegen(unit, &scale)) break;
        total += amount * (long double)scale;
    }
    return (long long)total;
}

static long long duration_accessor_scale(const char* field) {
    long long scale = 0;
    return duration_unit_ns_codegen(field, &scale) ? scale : 0;
}

// ---- Closure support ----

// Collect identifiers (reads) referenced in an AST subtree. Does NOT
// collect assignment targets — a name that only appears as an LHS and
// never as an RHS is handled separately by collect_write_targets below.
static int is_local_var(ASTNode* block, const char* name);
static int is_closure_param(ASTNode* closure, const char* name);
static ASTNode* closure_body_block(ASTNode* closure);

/* `_` is the discard binding (`_ = f()`, `a, _ = g()`): it names no storage,
 * so it is never a free variable of a closure. Each `_ = ...` reads like a
 * declaration of `_`, so without this a closure nested in another closure
 * that also discards treated the inner `_ = ...` as a write through to the
 * outer's `_`, captured it, and emitted C that referenced an undeclared `_`. */
static int is_discard_name(const char* name) {
    return name && name[0] == '_' && name[1] == '\0';
}

static void collect_ident_append(const char* name, char*** names, int* count, int* cap) {
    for (int i = 0; i < *count; i++) {
        if (strcmp((*names)[i], name) == 0) return;
    }
    if (*count >= *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *names = aether_xrealloc(*names, *cap * sizeof(char*));
    }
    (*names)[(*count)++] = strdup(name);
}

static void collect_identifiers(ASTNode* node, char*** names, int* count, int* cap) {
    if (!node) return;
    if (node->type == AST_IDENTIFIER && node->value) {
        collect_ident_append(node->value, names, count, cap);
    }
    /* `name op= x` reads and writes `name`, held in node->value rather than
     * an identifier child: without this, a closure whose only use of a
     * variable was `+=` did not capture it. */
    if (node->type == AST_COMPOUND_ASSIGNMENT && node->value) {
        collect_ident_append(node->value, names, count, cap);
    }
    for (int i = 0; i < node->child_count; i++) {
        ASTNode* child = node->children[i];
        if (child && child->type == AST_CLOSURE) {
            /* A nested closure's reads of its OWN params and fresh locals are
             * not free variables of the enclosing closure. Collecting them
             * here made the enclosing closure claim, e.g., a `use(nm)` inside a
             * nested closure that binds `nm = item` as a capture of a
             * same-named binding two levels out, promoting that outer binding
             * and emitting C that referenced an undeclared name. #2189. Gather
             * the nested closure's identifiers, then drop the ones that are its
             * own; a name genuinely free in the nested closure (a capture it
             * takes from here, such as the enclosing closure's param) survives
             * and flows up. */
            char** inner = NULL;
            int inner_n = 0, inner_cap = 0;
            for (int j = 0; j < child->child_count; j++) {
                collect_identifiers(child->children[j], &inner, &inner_n, &inner_cap);
            }
            ASTNode* inner_body = closure_body_block(child);
            for (int k = 0; k < inner_n; k++) {
                if (!is_closure_param(child, inner[k]) &&
                    !(inner_body && is_local_var(inner_body, inner[k]))) {
                    collect_ident_append(inner[k], names, count, cap);
                }
                free(inner[k]);
            }
            free(inner);
        } else {
            collect_identifiers(child, names, count, cap);
        }
    }
}

/* Callee names of the calls in a body (`step(v)` -> "step"), appended to
 * `names` without duplicates; see the capture filter for why they are
 * kept apart from plain identifiers. */
static void collect_callee_names(ASTNode* node, char*** names, int* count, int* cap) {
    if (!node) return;
    if (node->type == AST_FUNCTION_CALL && node->value) {
        int seen = 0;
        for (int i = 0; i < *count; i++) {
            if (strcmp((*names)[i], node->value) == 0) { seen = 1; break; }
        }
        if (!seen) {
            if (*count >= *cap) {
                *cap = *cap ? *cap * 2 : 16;
                *names = aether_xrealloc(*names, *cap * sizeof(char*));
            }
            (*names)[(*count)++] = strdup(node->value);
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        collect_callee_names(node->children[i], names, count, cap);
    }
}

// Collect write-target names (AST_VARIABLE_DECLARATION.value) in a closure
// body, stopping at nested closures. Used by the capture filter so that
// `x = expr` in a closure body — where x never appears on a read side —
// still gets a chance to be classified as a capture if x exists in the
// enclosing scope.
static void collect_write_targets(ASTNode* node, char*** names, int* count, int* cap) {
    if (!node) return;
    if (node->type == AST_CLOSURE) return;  // nested closures have their own scope
    if ((node->type == AST_VARIABLE_DECLARATION || node->type == AST_CONST_DECLARATION) &&
        node->value) {
        for (int i = 0; i < *count; i++) {
            if (strcmp((*names)[i], node->value) == 0) goto skip;
        }
        if (*count >= *cap) {
            *cap = *cap ? *cap * 2 : 8;
            *names = aether_xrealloc(*names, *cap * sizeof(char*));
        }
        (*names)[(*count)++] = strdup(node->value);
    skip:;
    }
    for (int i = 0; i < node->child_count; i++) {
        collect_write_targets(node->children[i], names, count, cap);
    }
}

// The AST_BLOCK body of a closure (its last block child), or NULL.
static ASTNode* closure_body_block(ASTNode* closure) {
    if (!closure) return NULL;
    for (int i = closure->child_count - 1; i >= 0; i--) {
        if (closure->children[i] && closure->children[i]->type == AST_BLOCK) {
            return closure->children[i];
        }
    }
    return NULL;
}

// Check if a name is a closure parameter
static int is_closure_param(ASTNode* closure, const char* name) {
    for (int i = 0; i < closure->child_count; i++) {
        ASTNode* child = closure->children[i];
        if (child && child->type == AST_CLOSURE_PARAM && child->value &&
            strcmp(child->value, name) == 0) {
            return 1;
        }
    }
    return 0;
}

// Return 1 if the subtree reads `name` (as an AST_IDENTIFIER). Does not
// descend into inner closures.
static int subtree_reads(ASTNode* node, const char* name) {
    if (!node || !name) return 0;
    if (node->type == AST_CLOSURE) return 0;
    if (node->type == AST_IDENTIFIER && node->value &&
        strcmp(node->value, name) == 0) {
        return 1;
    }
    for (int i = 0; i < node->child_count; i++) {
        if (subtree_reads(node->children[i], name)) return 1;
    }
    return 0;
}

// Ordered tri-state scan of a closure body for the FIRST occurrence of `name`,
// deciding whether the name is the closure's own fresh local or a capture of a
// same-named enclosing binding. Result codes:
//   FL_FRESH   (1) first occurrence is a WRITE: `name = expr` whose RHS does
//                  not itself read `name` — a fresh local declaration.
//   FL_CAPTURE (2) first occurrence is a READ (including a read inside the RHS
//                  of that same `name = ...` statement, i.e. `name = name+1`) —
//                  the closure consumes an incoming binding, so it is a capture.
//   FL_NONE    (0) `name` does not occur in the body.
//
// The FIRST-occurrence rule is what discriminates the two shapes that a
// write-anywhere test conflates:
//   - `idx = index_of(..)` then `entry = substring(.., idx)` — idx's first
//     occurrence is its own write; the later read is of its fresh local.  FRESH.
//   - `if depth > max_depth { max_depth = depth }` — max_depth is READ (in the
//     condition) before/independent of its write.  CAPTURE, must promote.
// Descends nested if/while blocks (they hoist to the closure's C frame) but
// stops at nested real closures; trailing blocks inline and are traversed.
#define FL_NONE 0
#define FL_FRESH 1
#define FL_CAPTURE 2
static int first_occurrence_kind(ASTNode* node, const char* name) {
    if (!node) return FL_NONE;
    // A nested real closure is opaque — its own uses of `name` don't count.
    if (node->type == AST_CLOSURE &&
        !(node->value && strcmp(node->value, "trailing") == 0)) {
        return FL_NONE;
    }
    // A declaration statement `name = RHS`: the RHS is evaluated first, so a
    // read of `name` in the RHS is the first occurrence (capture); otherwise
    // this write is the first occurrence (fresh).
    if ((node->type == AST_VARIABLE_DECLARATION || node->type == AST_CONST_DECLARATION) &&
        node->value && strcmp(node->value, name) == 0) {
        for (int c = 0; c < node->child_count; c++) {
            if (subtree_reads(node->children[c], name)) return FL_CAPTURE;
        }
        return FL_FRESH;
    }
    // A bare read of `name`.
    if (node->type == AST_IDENTIFIER && node->value && strcmp(node->value, name) == 0) {
        return FL_CAPTURE;
    }
    // Otherwise recurse in source order; the first child that resolves wins.
    for (int i = 0; i < node->child_count; i++) {
        int r = first_occurrence_kind(node->children[i], name);
        if (r != FL_NONE) return r;
    }
    return FL_NONE;
}

// Check if `name` is a fresh local of a closure body `block`, scanning the whole
// body scope (nested blocks included). A name is the closure's own local only if
// its FIRST occurrence in the body is a fresh write — a name read as an incoming
// value (even if later written) is a capture, not a fresh local. See
// first_occurrence_kind for the discriminator and why write-anywhere is wrong.
static int is_local_var(ASTNode* block, const char* name) {
    if (!block || !name) return 0;
    return first_occurrence_kind(block, name) == FL_FRESH;
}

/* #2492: the two receive-arm shapes generate_actor_definition lowers to a
 * handler. V2 `Ping(n) -> { ... }` is an AST_RECEIVE_ARM [pattern, body];
 * the V1 shape is an AST_BLOCK holding an AST_MESSAGE_PATTERN whose last
 * child is the body. Each is the scope `__recv_arm_<node>` of the closures
 * in it, and both bind the pattern's names, so every scope query below
 * goes through these. */
static ASTNode* v1_arm_pattern(ASTNode* node) {
    if (!node || node->type != AST_BLOCK) return NULL;
    for (int k = 0; k < node->child_count; k++) {
        if (node->children[k] && node->children[k]->type == AST_MESSAGE_PATTERN) {
            return node->children[k];
        }
    }
    return NULL;
}

static int is_receive_arm_scope(ASTNode* node) {
    return node && (node->type == AST_RECEIVE_ARM || v1_arm_pattern(node));
}

static ASTNode* receive_arm_pattern(ASTNode* arm) {
    if (arm && arm->type == AST_RECEIVE_ARM) {
        return arm->child_count > 0 ? arm->children[0] : NULL;
    }
    return v1_arm_pattern(arm);
}

static ASTNode* receive_arm_body(ASTNode* arm) {
    if (arm && arm->type == AST_RECEIVE_ARM) {
        return arm->child_count > 1 ? arm->children[1] : NULL;
    }
    ASTNode* pattern = v1_arm_pattern(arm);
    ASTNode* last = (pattern && pattern->child_count > 0)
                        ? pattern->children[pattern->child_count - 1] : NULL;
    return (last && last->type == AST_BLOCK) ? last : NULL;
}

/* The AST_PATTERN_FIELD of `arm`'s message pattern that binds `name`
 * (`Ping(n)` binds n, `Ping(n: m)` binds m), or NULL. The handler declares
 * each binding as a C local read from the message (codegen_actor.c), so to
 * a closure in the arm it is a binding of the arm, like a parameter. */
static ASTNode* receive_arm_binding(ASTNode* arm, const char* name) {
    ASTNode* pattern = receive_arm_pattern(arm);
    if (!pattern || pattern->type != AST_MESSAGE_PATTERN || !name) return NULL;
    for (int k = 0; k < pattern->child_count; k++) {
        ASTNode* pf = pattern->children[k];
        if (!pf || pf->type != AST_PATTERN_FIELD || !pf->value) continue;
        const char* bound = pf->value;
        if (pf->child_count > 0 && pf->children[0] &&
            pf->children[0]->type == AST_PATTERN_VARIABLE && pf->children[0]->value) {
            bound = pf->children[0]->value;
        }
        if (strcmp(bound, name) == 0) return pf;
    }
    return NULL;
}

/* The declared type of field `field` of message `msg`, or NULL. */
static Type* message_field_type(ASTNode* program, const char* msg, const char* field) {
    if (!program || !msg || !field) return NULL;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* def = program->children[i];
        if (def && def->type == AST_EXPORT_STATEMENT && def->child_count > 0) {
            def = def->children[0];
        }
        if (!def || def->type != AST_MESSAGE_DEFINITION || !def->value ||
            strcmp(def->value, msg) != 0) continue;
        for (int j = 0; j < def->child_count; j++) {
            ASTNode* f = def->children[j];
            if (f && f->type == AST_MESSAGE_FIELD && f->value &&
                strcmp(f->value, field) == 0) {
                return f->node_type;
            }
        }
        return NULL;
    }
    return NULL;
}

// Find a receive arm anywhere in the program whose synthetic name
// (format `__recv_arm_<pointer>`) matches `func_name`. Returns NULL if
// not found. Used so that actor message handlers — which are effectively
// mini-functions for closure-promotion purposes — can be looked up the
// same way as top-level functions.
static ASTNode* find_receive_arm_by_name(ASTNode* node, const char* func_name) {
    if (!node || !func_name) return NULL;
    if (is_receive_arm_scope(node)) {
        char arm_name[256];
        snprintf(arm_name, sizeof(arm_name), "__recv_arm_%p", (void*)node);
        if (strcmp(arm_name, func_name) == 0) return node;
    }
    for (int i = 0; i < node->child_count; i++) {
        ASTNode* found = find_receive_arm_by_name(node->children[i], func_name);
        if (found) return found;
    }
    return NULL;
}

// A hoisted closure is its own lexical scope: its params and body locals
// become real C params/locals of `_closure_fn_<id>`, and a closure created
// inside its body captures from THAT C frame, not from the enclosing
// function's. Give each such closure a synthetic scope name — same scheme as
// the receive-arm names above — so `parent_func` can name a closure and the
// declaration lookups below resolve captures against it. Without this, an
// inner closure's captures were looked up in the enclosing *function* (where
// the outer closure's locals do not exist), so nothing was captured and the
// emitted C referenced undeclared names.
static void closure_scope_name(ASTNode* closure, char* buf, size_t n) {
    snprintf(buf, n, "__closure_%p", (void*)closure);
}

// Find the AST_CLOSURE whose synthetic scope name matches `func_name`.
static ASTNode* find_closure_by_name(ASTNode* node, const char* func_name) {
    if (!node || !func_name) return NULL;
    if (node->type == AST_CLOSURE) {
        char nm[64];
        closure_scope_name(node, nm, sizeof(nm));
        if (strcmp(nm, func_name) == 0) return node;
    }
    for (int i = 0; i < node->child_count; i++) {
        ASTNode* found = find_closure_by_name(node->children[i], func_name);
        if (found) return found;
    }
    return NULL;
}

// The body of a closure/receive-arm is its last AST_BLOCK child.
static ASTNode* last_block_child(ASTNode* node) {
    if (!node) return NULL;
    for (int i = node->child_count - 1; i >= 0; i--) {
        if (node->children[i] && node->children[i]->type == AST_BLOCK) {
            return node->children[i];
        }
    }
    return NULL;
}

// Is `node` a real (hoisted) closure, as opposed to a trailing block? Trailing
// blocks inline at their call site and so are NOT a scope boundary.
static int is_hoisted_closure(ASTNode* node) {
    return node && node->type == AST_CLOSURE &&
           !(node->value && strcmp(node->value, "trailing") == 0);
}

// Walk down from `node` (carrying the scope name in force there) to find
// `target`, and return target's ENCLOSING scope name, interned, or NULL when
// there is none. The scope name uses the same vocabulary as `parent_func`
// everywhere else: a function name, "main", `__recv_arm_<ptr>`, or
// `__closure_<ptr>`. A function name has no length bound, so it is never
// copied into a fixed buffer (#2539).
/* #2130: the fn-pointer registry is per C function, and a closure body
 * is emitted in its own pass, after every function. Rebuild the registry
 * a closure's body sees from the scopes it sits in: every fn-typed
 * parameter and fn-typed local of the enclosing closures and of the
 * function / main / receive handler at the top of the chain, so a call
 * through a captured `step: fn(int) -> int` still lowers through its
 * type. Walks the whole body of each level (a name is one variable per
 * function; a nested closure's own parameters would only shadow, and
 * are registered again when that closure's body is emitted). */
static const char* find_enclosing_scope_name(ASTNode* node, const char* scope,
                                             ASTNode* target);

static void register_fnptr_decls_in(CodeGenerator* gen, ASTNode* n) {
    if (!n) return;
    if ((n->type == AST_VARIABLE_DECLARATION || n->type == AST_PATTERN_VARIABLE ||
         n->type == AST_CLOSURE_PARAM) && n->value) {
        Type* sig = NULL;
        if (is_fnptr_type(n->node_type)) sig = n->node_type;
        else if (n->child_count > 0 && n->children[0] &&
                 n->children[0]->type == AST_PTR_AS_FN_CAST &&
                 is_fnptr_type(n->children[0]->node_type)) sig = n->children[0]->node_type;
        if (sig) register_fnptr_local(gen, n->value, sig);
    }
    for (int i = 0; i < n->child_count; i++) register_fnptr_decls_in(gen, n->children[i]);
}

static ASTNode* find_scope_node_by_name(ASTNode* program, const char* scope) {
    if (!program || !scope) return NULL;
    if (strncmp(scope, "__closure_", 10) == 0) return find_closure_by_name(program, scope);
    if (strncmp(scope, "__recv_arm_", 11) == 0) return find_receive_arm_by_name(program, scope);
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* top = program->children[i];
        if (!top) continue;
        if (strcmp(scope, "main") == 0 && top->type == AST_MAIN_FUNCTION) return top;
        if ((top->type == AST_FUNCTION_DEFINITION || top->type == AST_BUILDER_FUNCTION) &&
            top->value && strcmp(top->value, scope) == 0) return top;
        if (top->type == AST_EXPORT_STATEMENT && top->child_count > 0 && top->children[0] &&
            top->children[0]->type == AST_FUNCTION_DEFINITION && top->children[0]->value &&
            strcmp(top->children[0]->value, scope) == 0) return top->children[0];
    }
    return NULL;
}

static void restore_fnptr_scope_for_closure(CodeGenerator* gen, const char* parent_func) {
    clear_fnptr_locals(gen);
    const char* cur = parent_func;
    for (int depth = 0; cur && depth < 16; depth++) {
        ASTNode* owner = find_scope_node_by_name(gen->program, cur);
        if (!owner) break;
        register_fnptr_decls_in(gen, owner);
        if (strncmp(cur, "__closure_", 10) != 0) break;   /* reached the function */
        cur = find_enclosing_scope_name(gen->program, NULL, owner);
    }
}

static const char* find_enclosing_scope_name(ASTNode* node, const char* scope,
                                             ASTNode* target) {
    if (!node) return NULL;
    char here[64];   /* `__recv_arm_<ptr>` / `__closure_<ptr>`: bounded */
    const char* child_scope = scope;
    if (node->type == AST_FUNCTION_DEFINITION || node->type == AST_BUILDER_FUNCTION) {
        child_scope = node->value ? node->value : scope;
    } else if (node->type == AST_MAIN_FUNCTION) {
        child_scope = "main";
    } else if (is_receive_arm_scope(node)) {
        snprintf(here, sizeof(here), "__recv_arm_%p", (void*)node);
        child_scope = here;
    } else if (is_hoisted_closure(node)) {
        closure_scope_name(node, here, sizeof(here));
        child_scope = here;
    }
    for (int i = 0; i < node->child_count; i++) {
        ASTNode* c = node->children[i];
        if (c == target) {
            /* Interned: `here` dies with this frame. */
            return child_scope ? cg_intern(child_scope) : NULL;
        }
        const char* found = find_enclosing_scope_name(c, child_scope, target);
        if (found) return found;
    }
    return NULL;
}

// Forward declaration — subtree_declares is defined below but used here
// to recurse through trailing-block closures while stopping at real
// closures.
static int subtree_declares(ASTNode* node, const char* var_name);

// Does `target` sit anywhere under `node`?
static int subtree_contains(ASTNode* node, ASTNode* target) {
    if (!node) return 0;
    if (node == target) return 1;
    for (int i = 0; i < node->child_count; i++) {
        if (subtree_contains(node->children[i], target)) return 1;
    }
    return 0;
}

// The trailing-block closure argument of a call statement `s`, if `s` is a
// call with one (`root = grid() { ... }`, `grid() { ... }`), else NULL.
static ASTNode* trailing_block_of_statement(ASTNode* s) {
    ASTNode* call = NULL;
    if (s->type == AST_VARIABLE_DECLARATION && s->child_count > 0 &&
        s->children[0] && s->children[0]->type == AST_FUNCTION_CALL) {
        call = s->children[0];
    } else if (s->type == AST_EXPRESSION_STATEMENT && s->child_count > 0 &&
               s->children[0] && s->children[0]->type == AST_FUNCTION_CALL) {
        call = s->children[0];
    } else if (s->type == AST_FUNCTION_CALL) {
        call = s;
    }
    if (!call) return NULL;
    for (int ci = 0; ci < call->child_count; ci++) {
        ASTNode* arg = call->children[ci];
        if (arg && arg->type == AST_CLOSURE && arg->value &&
            strcmp(arg->value, "trailing") == 0) {
            return arg;
        }
    }
    return NULL;
}

// The line of the first declaration of `var_name` that is VISIBLE from
// `viewer` (a closure node) among the top-level statements of `block`, or
// INT_MAX if there is none. A trailing block (`grid() { ... }`) inlines at its
// call site as a C `{ ... }` block, so a declaration inside it is in scope for
// a closure nested inside that same trailing block and for nothing else: the
// walk descends only into the trailing block that contains `viewer`. A
// declaration in a sibling trailing block, or in an if/for/while body, shares
// the name by coincidence and is skipped. #2189: treating every trailing block
// as transparent compiled a closure's own `ml = ...` as a capture of the `ml`
// a sibling `describe` block had declared, and the generated C referenced a
// name that had gone out of scope.
static int visible_decl_line(ASTNode* block, const char* var_name, ASTNode* viewer) {
    if (!block) return INT_MAX;
    for (int k = 0; k < block->child_count; k++) {
        ASTNode* s = block->children[k];
        if (!s) continue;
        if ((s->type == AST_VARIABLE_DECLARATION || s->type == AST_CONST_DECLARATION) &&
            s->value && strcmp(s->value, var_name) == 0) {
            return s->line;
        }
        ASTNode* trailing = trailing_block_of_statement(s);
        if (trailing && subtree_contains(trailing, viewer)) {
            int inner = visible_decl_line(last_block_child(trailing), var_name, viewer);
            if (inner != INT_MAX) return inner;
        }
    }
    return INT_MAX;
}

// Where, in the scope named `func_name` (main / a function / a hoisted closure
// / a receive arm), is `var_name` bound as seen from `viewer`, a closure that
// sits in that scope? Returns 0 for a parameter, the declaration's line for a
// local visible from `viewer` (see visible_decl_line), and INT_MAX when the
// scope binds no such name. A hoisted closure's scope chains up to its
// enclosing scope, with that closure as the new viewer, so an inner closure
// sees a binding declared in an outer closure or in the function two levels
// out, and its position there. The caller compares the result against the
// viewer's own line: only a binding that precedes the closure is one it
// mutates through (#2189).
static int enclosing_decl_line(ASTNode* program, const char* func_name,
                               const char* var_name, ASTNode* viewer) {
    if (!program || !func_name || !var_name) return INT_MAX;
    if (strncmp(func_name, "__closure_", 10) == 0) {
        ASTNode* c = find_closure_by_name(program, func_name);
        if (!c) return INT_MAX;
        if (is_closure_param(c, var_name)) return 0;
        int here = visible_decl_line(last_block_child(c), var_name, viewer);
        if (here != INT_MAX) return here;
        const char* outer = find_enclosing_scope_name(program, NULL, c);
        if (outer) return enclosing_decl_line(program, outer, var_name, c);
        return INT_MAX;
    }
    // Actor receive arms use synthetic function names `__recv_arm_<ptr>`.
    // A name the message pattern binds precedes the whole body, as a
    // parameter does (#2492).
    if (strncmp(func_name, "__recv_arm_", 11) == 0) {
        ASTNode* arm = find_receive_arm_by_name(program, func_name);
        if (!arm) return INT_MAX;
        if (receive_arm_binding(arm, var_name)) return 0;
        ASTNode* body = receive_arm_body(arm);
        if (!body || body->type != AST_BLOCK) return INT_MAX;
        return visible_decl_line(body, var_name, viewer);
    }
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* top = program->children[i];
        if (!top) continue;
        int matches =
            (strcmp(func_name, "main") == 0 && top->type == AST_MAIN_FUNCTION) ||
            ((top->type == AST_FUNCTION_DEFINITION || top->type == AST_BUILDER_FUNCTION) &&
             top->value && strcmp(top->value, func_name) == 0);
        if (!matches) continue;
        // Parameters count as bindings that precede everything in the body
        // (main has no declared params).
        if (top->type != AST_MAIN_FUNCTION) {
            for (int j = 0; j < top->child_count; j++) {
                ASTNode* p = top->children[j];
                if (p && p->type == AST_PATTERN_VARIABLE && p->value &&
                    strcmp(p->value, var_name) == 0) {
                    return 0;
                }
            }
        }
        for (int j = 0; j < top->child_count; j++) {
            ASTNode* body = top->children[j];
            if (body && body->type == AST_BLOCK) {
                return visible_decl_line(body, var_name, viewer);
            }
        }
        return INT_MAX;
    }
    return INT_MAX;
}

// Recursively scan an AST subtree for a declaration whose value matches
// `var_name`. Stops descending into AST_CLOSURE nodes — their locals belong
// to an inner scope, not the enclosing function's.
static int subtree_declares(ASTNode* node, const char* var_name) {
    if (!node) return 0;
    // Stop at real closures — their locals don't belong to the enclosing
    // function. But trailing-block closures (value == "trailing") are
    // inlined at the call site, so they DO contribute declarations to
    // the enclosing function's scope and must be traversed.
    if (node->type == AST_CLOSURE &&
        !(node->value && strcmp(node->value, "trailing") == 0)) {
        return 0;
    }
    if ((node->type == AST_VARIABLE_DECLARATION || node->type == AST_CONST_DECLARATION) &&
        node->value && strcmp(node->value, var_name) == 0) {
        return 1;
    }
    for (int i = 0; i < node->child_count; i++) {
        if (subtree_declares(node->children[i], var_name)) return 1;
    }
    return 0;
}

// Does the named function define or declare `var_name` (as a parameter or
// local). Used to distinguish captures (names from an enclosing scope) from
// fresh locals that happen to share a name. Scans nested blocks (for loops,
// if/else bodies) but does not descend into inner closures.
static int is_declared_in_function(ASTNode* program, const char* func_name, const char* var_name) {
    if (!program || !func_name || !var_name) return 0;
    if (strncmp(func_name, "__closure_", 10) == 0) {
        ASTNode* c = find_closure_by_name(program, func_name);
        if (!c) return 0;
        if (is_closure_param(c, var_name)) return 1;
        if (subtree_declares(last_block_child(c), var_name)) return 1;
        // Not local to the outer closure — but a name the OUTER closure
        // captures is live in its C frame (prologue alias `T name = _env->
        // name;`), so an inner closure can and must re-capture it. Chain up
        // to the outer closure's own scope: this is what makes a capture
        // transit an arbitrarily deep closure nest, one env hop per level.
        const char* outer = find_enclosing_scope_name(program, NULL, c);
        if (outer) return is_declared_in_function(program, outer, var_name);
        return 0;
    }
    if (strncmp(func_name, "__recv_arm_", 11) == 0) {
        ASTNode* arm = find_receive_arm_by_name(program, func_name);
        if (!arm) return 0;
        /* #2492: the message pattern's bindings are the arm's, declared at
         * the top of the handler. They were not counted, so a closure in
         * the arm did not capture `n` from `Ping(n)` and its body read an
         * undeclared `n`. */
        if (receive_arm_binding(arm, var_name)) return 1;
        ASTNode* body = receive_arm_body(arm);
        if (!body) return 0;
        return subtree_declares(body, var_name);
    }
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* top = program->children[i];
        if (!top) continue;
        int matches = 0;
        if (strcmp(func_name, "main") == 0 && top->type == AST_MAIN_FUNCTION) {
            matches = 1;
        } else if ((top->type == AST_FUNCTION_DEFINITION || top->type == AST_BUILDER_FUNCTION) &&
                   top->value && strcmp(top->value, func_name) == 0) {
            matches = 1;
        }
        if (!matches) continue;
        // Parameters (skip for main — main has no declared params).
        if (top->type != AST_MAIN_FUNCTION) {
            for (int j = 0; j < top->child_count; j++) {
                ASTNode* p = top->children[j];
                if (p && p->type == AST_PATTERN_VARIABLE && p->value &&
                    strcmp(p->value, var_name) == 0) {
                    return 1;
                }
            }
        }
        // Declarations anywhere in the function's body, including nested
        // blocks, but not inside inner closures.
        for (int j = 0; j < top->child_count; j++) {
            ASTNode* body = top->children[j];
            if (!body || body->type != AST_BLOCK) continue;
            if (subtree_declares(body, var_name)) return 1;
        }
        return 0; // Matched function but name not found — definitely not declared here.
    }
    return 0;
}

// Walk subtree and return the expression of the first return statement
// carrying a non-print value. Does not descend into inner closures.
static ASTNode* find_first_return_expr(ASTNode* node);

/* #2501: the first return statement under `node` that carries a value, not
 * looking into nested closures (find_first_return_expr's walk). */
static ASTNode* find_first_value_return(ASTNode* node) {
    if (!node || node->type == AST_CLOSURE) return NULL;
    if (node->type == AST_RETURN_STATEMENT && node->child_count > 0 &&
        node->children[0] && node->children[0]->type != AST_PRINT_STATEMENT) {
        return node;
    }
    for (int i = 0; i < node->child_count; i++) {
        ASTNode* found = find_first_value_return(node->children[i]);
        if (found) return found;
    }
    return NULL;
}

/* #2501: the tuple a closure returns when its first value return is a
 * multi-value one, built from the returned expressions' types (the caller
 * frees it), or NULL. The typechecker types a call of the closure the same
 * way (closure_literal_result_type). */
static Type* closure_tuple_return_type(ASTNode* closure) {
    ASTNode* body = NULL;
    for (int i = closure ? closure->child_count - 1 : -1; i >= 0; i--) {
        if (closure->children[i] && closure->children[i]->type == AST_BLOCK) {
            body = closure->children[i];
            break;
        }
    }
    ASTNode* ret = find_first_value_return(body);
    if (!ret || ret->child_count < 2) return NULL;
    Type* t = create_type(TYPE_TUPLE);
    t->tuple_count = ret->child_count;
    t->tuple_types = malloc((size_t)ret->child_count * sizeof(Type*));
    for (int j = 0; j < ret->child_count; j++) {
        Type* et = ret->children[j] ? ret->children[j]->node_type : NULL;
        t->tuple_types[j] = (et && et->kind != TYPE_UNKNOWN) ? clone_type(et) : create_type(TYPE_INT);
    }
    return t;
}

/* A tuple's C name lives in get_c_type's rotating buffers; a closure's
 * return type is held across other get_c_type calls, so keep one copy. */
static const char* closure_tuple_c_name(Type* t) {
    static char** names = NULL;
    static int count = 0;
    const char* nm = get_c_type(t);
    for (int i = 0; i < count; i++) {
        if (strcmp(names[i], nm) == 0) return names[i];
    }
    names = aether_xrealloc(names, (size_t)(count + 1) * sizeof(char*));
    names[count] = strdup(nm);
    return names[count++];
}

static ASTNode* find_first_return_expr(ASTNode* node) {
    if (!node) return NULL;
    if (node->type == AST_CLOSURE) return NULL;
    if (node->type == AST_RETURN_STATEMENT && node->child_count > 0 &&
        node->children[0] && node->children[0]->type != AST_PRINT_STATEMENT) {
        return node->children[0];
    }
    for (int i = 0; i < node->child_count; i++) {
        ASTNode* found = find_first_return_expr(node->children[i]);
        if (found) return found;
    }
    return NULL;
}

/* Does ANY return site under `node` (not descending into nested closures)
 * carry a `string`-typed expression?
 *
 * resolve_closure_return_type picks the closure's C return type from its
 * FIRST return, which is the right type for the signature only when every
 * return agrees. A closure that returns `string.from_int(x)` on one path and
 * a struct's string field on another is typed by the first: `void*` (the
 * builtin is declared `-> ptr`), so it is not a "string closure" and none of
 * its returns get the uniform-heap wrap of #2054 — while its caller, a
 * `-> string` function returning `cb(...)`, takes ownership of whatever
 * comes back and frees it. The field-returning path handed over a literal,
 * and the caller free()d it (aether-ui's table cell callback, every row;
 * a heap-corruption abort on macOS and Windows). Any string-typed return
 * makes the closure a string closure, so every path is wrapped. */
static int any_return_is_string(ASTNode* node) {
    if (!node) return 0;
    if (node->type == AST_CLOSURE) return 0;
    if (node->type == AST_RETURN_STATEMENT && node->child_count > 0 &&
        node->children[0] && node->children[0]->type != AST_PRINT_STATEMENT) {
        ASTNode* e = node->children[0];
        if (e->node_type && e->node_type->kind == TYPE_STRING) return 1;
        return 0;
    }
    for (int i = 0; i < node->child_count; i++) {
        if (any_return_is_string(node->children[i])) return 1;
    }
    return 0;
}

// Return 1 if any AST_VARIABLE_DECLARATION node under `node` assigns to
// `name` (i.e., appears as its `value`). Used by closure codegen to detect
// which captures are mutated inside the body — those captures cannot use
// the read-only alias prologue and must route writes through _env->.
/* The variable a field or element write goes through: `p` for `p.x`,
 * `p.a.b`, `arr[i]` and `p.xs[i].y`; NULL for a bare name or any other
 * shape. */
static const char* write_through_root(ASTNode* lhs) {
    if (!lhs || (lhs->type != AST_MEMBER_ACCESS && lhs->type != AST_ARRAY_ACCESS)) return NULL;
    while (lhs) {
        if (lhs->type == AST_IDENTIFIER) return lhs->value;
        if ((lhs->type == AST_MEMBER_ACCESS || lhs->type == AST_ARRAY_ACCESS) &&
            lhs->child_count > 0) {
            lhs = lhs->children[0];
            continue;
        }
        return NULL;
    }
    return NULL;
}

static int is_assignment_op(const char* op) {
    if (!op || !op[0]) return 0;
    if (op[0] == '=' && op[1] == '\0') return 1;
    if (op[1] == '=' && op[2] == '\0' &&
        strchr("+-*/%&|^", op[0]) != NULL) return 1;
    if ((strcmp(op, "<<=") == 0) || (strcmp(op, ">>=") == 0)) return 1;
    return 0;
}

/* True when `node` writes a field or an element through `name`: `p.x = v`,
 * `p.x += v`, `p.inner.y *= 3`, `b.vals[i] = v`, `p.x++` (#2458). For a
 * struct held by value that changes the variable itself, so a closure doing
 * it needs the shared cell a bare `p = ...` gets; through a pointer it
 * changes the pointee, which is shared already (compute_promoted_captures). */
static int is_written_through(ASTNode* node, const char* name) {
    if (!node) return 0;
    ASTNode* target = NULL;
    if ((node->type == AST_BINARY_EXPRESSION && is_assignment_op(node->value)) ||
        node->type == AST_ASSIGNMENT) {
        target = node->child_count >= 1 ? node->children[0] : NULL;
    } else if (node->type == AST_UNARY_EXPRESSION && node->value && node->child_count == 1 &&
               (strcmp(node->value, "++") == 0 || strcmp(node->value, "--") == 0)) {
        target = node->children[0];
    }
    const char* root = write_through_root(target);
    if (root && strcmp(root, name) == 0) return 1;
    for (int i = 0; i < node->child_count; i++) {
        if (is_written_through(node->children[i], name)) return 1;
    }
    return 0;
}

static int is_assigned_to(ASTNode* node, const char* name) {
    if (!node) return 0;
    if (node->type == AST_VARIABLE_DECLARATION && node->value &&
        strcmp(node->value, name) == 0) {
        return 1;
    }
    /* `name op= x` writes `name` too; its target is node->value. */
    if (node->type == AST_COMPOUND_ASSIGNMENT && node->value &&
        strcmp(node->value, name) == 0) {
        return 1;
    }
    /* `name++` / `--name` writes `name` as much as `name += 1` does (#2457). */
    if (node->type == AST_UNARY_EXPRESSION && node->value && node->child_count == 1 &&
        (strcmp(node->value, "++") == 0 || strcmp(node->value, "--") == 0) &&
        node->children[0] && node->children[0]->type == AST_IDENTIFIER &&
        node->children[0]->value && strcmp(node->children[0]->value, name) == 0) {
        return 1;
    }
    // Tuple destructure assigns to each non-discard target; if `name` is
    // any of them, treat as assignment for promotion analysis. Without
    // this, `out, status, _ = sh(...)` inside a closure body would
    // miscompile against an unpromoted outer-scope `out` (closure-shadow
    // -tuple-destructure bug, svn-aether porter Round 238/239).
    if (node->type == AST_TUPLE_DESTRUCTURE && node->child_count >= 2) {
        int var_count = node->child_count - 1;
        for (int j = 0; j < var_count; j++) {
            ASTNode* var = node->children[j];
            if (var && var->value && strcmp(var->value, name) == 0 &&
                strcmp(var->value, "_") != 0) {
                return 1;
            }
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        if (is_assigned_to(node->children[i], name)) return 1;
    }
    return 0;
}

// Built-in function names that should not be treated as captures
static int is_builtin_name(const char* name) {
    static const char* builtins[] = {
        "print", "println", "make", "spawn", "exit", "sleep", "free",
        "getenv", "atoi", "clock_ns", "typeof", "is_type", "convert_type",
        "print_char", "wait_for_idle", "each", "map", "filter",
        NULL
    };
    for (int i = 0; builtins[i]; i++) {
        if (strcmp(name, builtins[i]) == 0) return 1;
    }
    return 0;
}

// Internal recursive worker that tracks the enclosing function name.
static void discover_closures_scoped(CodeGenerator* gen, ASTNode* node, const char* enclosing_func) {
    if (!node) return;
    // Entering a function body switches the enclosing function for descendants.
    if (node->type == AST_FUNCTION_DEFINITION || node->type == AST_BUILDER_FUNCTION) {
        const char* new_enc = node->value ? node->value : enclosing_func;
        for (int i = 0; i < node->child_count; i++) {
            discover_closures_scoped(gen, node->children[i], new_enc);
        }
        return;
    }
    if (node->type == AST_MAIN_FUNCTION) {
        for (int i = 0; i < node->child_count; i++) {
            discover_closures_scoped(gen, node->children[i], "main");
        }
        return;
    }
    // Actor message handlers are mini-functions for promotion purposes.
    // Give each receive arm a synthetic enclosing-function name so captures
    // inside closures in a handler are promoted in that arm's scope alone.
    // Arm locals don't escape; each arm starts fresh. The name shape
    // `__actor_<ActorName>__arm_<idx>` is emitted by actor codegen when it
    // publishes the promoted set at the handler's generate_statement site.
    if (is_receive_arm_scope(node)) {   /* V2 and V1 shapes (#2492) */
        char arm_name[256];
        // Use the pointer as a quasi-unique disambiguator since we don't
        // have an arm index accessible here. Actor codegen will use the
        // same scheme.
        snprintf(arm_name, sizeof(arm_name), "__recv_arm_%p", (void*)node);
        for (int i = 0; i < node->child_count; i++) {
            discover_closures_scoped(gen, node->children[i], arm_name);
        }
        return;
    }
    if (node->type == AST_CLOSURE) {
        // Skip trailing blocks — they are inlined at the call site, not hoisted
        if (node->value && strcmp(node->value, "trailing") == 0) {
            // Still recurse into children to find nested non-trailing closures
            for (int i = 0; i < node->child_count; i++) {
                discover_closures_scoped(gen, node->children[i], enclosing_func);
            }
            return;
        }
        int id = gen->closure_counter++;
        // Store ID in closure node's value field for later reference
        char id_str[32];
        snprintf(id_str, sizeof(id_str), "%d", id);
        if (node->value) free(node->value);
        node->value = strdup(id_str);

        // Find the body (last child, should be AST_BLOCK)
        ASTNode* body = NULL;
        for (int i = node->child_count - 1; i >= 0; i--) {
            if (node->children[i] && node->children[i]->type == AST_BLOCK) {
                body = node->children[i];
                break;
            }
        }

        // Collect all identifiers in the body
        char** all_ids = NULL;
        int id_count = 0, id_cap = 0;
        collect_identifiers(body, &all_ids, &id_count, &id_cap);
        /* A call's callee is the call node's own name, not an identifier
         * child: `step(v)` through a captured `step: fn(int) -> int` must
         * capture `step` too (#2130 — it was left out of the env and the
         * body called an undeclared `step`). Unlike an identifier, a
         * callee is a capture only when the enclosing scope is known to
         * declare it: most callees are top-level functions, and the
         * unknown-scope fallback below must not turn those into
         * captures. */
        int first_callee = id_count;
        collect_callee_names(body, &all_ids, &id_count, &id_cap);

        // Filter to captures. A name is a capture iff it:
        //   - is not a parameter of this closure,
        //   - is not a built-in function,
        //   - is not declared at top-level of the closure's own body
        //     (implicit local — `x = expr` inside the closure shadows any
        //     same-named outer binding),
        //   - refers to a binding in the enclosing scope.
        // When enclosing_func is unknown (top-level closures etc.), fall back
        // to the old body-local heuristic.
        char** captures = NULL;
        int cap_count = 0, cap_cap = 0;
        for (int i = 0; i < id_count; i++) {
            int is_cap = 0;
            if (!is_discard_name(all_ids[i]) &&
                !is_closure_param(node, all_ids[i]) &&
                !is_builtin_name(all_ids[i]) &&
                !is_local_var(body, all_ids[i])) {
                if (enclosing_func) {
                    is_cap = is_declared_in_function(gen->program, enclosing_func, all_ids[i]);
                } else {
                    is_cap = i < first_callee;
                }
            }
            if (is_cap) {
                if (cap_count >= cap_cap) {
                    cap_cap = cap_cap ? cap_cap * 2 : 8;
                    captures = aether_xrealloc(captures, cap_cap * sizeof(char*));
                }
                captures[cap_count++] = strdup(all_ids[i]);
            }
            free(all_ids[i]);
        }
        free(all_ids);

        // Second pass for write-only captures: names that appear as
        // AST_VARIABLE_DECLARATION targets but are NOT captured via
        // the read path. `msg = "world"` in a closure where outer scope
        // declares `msg` at the top level is a mutation of the outer
        // binding, not a fresh local.
        //
        // Use TOP-LEVEL-ONLY declaration lookup here: if `v` is declared
        // at function top-level, a closure's `v = ...` captures it. If
        // `v` is only declared inside a nested block of the enclosing
        // function (e.g. main's `if key == EQUAL { v = ref_get(num) }`),
        // the two `v`s share a name by coincidence and are
        // independently-scoped locals — the closure's `v` is a fresh
        // local. This matches JavaScript/Ruby closure semantics where
        // captures lift from the function's own scope, not arbitrary
        // inner blocks.
        //
        // The reverse case (name appears as both read and write) is
        // handled above via the read-path — is_local_var returns false
        // when init_reads_self, so the read-path's enclosing-scope
        // check makes it a capture.
        if (enclosing_func) {
            char** writes = NULL;
            int write_count = 0, write_cap = 0;
            collect_write_targets(body, &writes, &write_count, &write_cap);
            for (int i = 0; i < write_count; i++) {
                // Already captured via the read path?
                int already = 0;
                for (int k = 0; k < cap_count; k++) {
                    if (strcmp(captures[k], writes[i]) == 0) { already = 1; break; }
                }
                if (already) { free(writes[i]); continue; }
                // Skip the discard `_`, closure params and builtins.
                if (is_discard_name(writes[i]) ||
                    is_closure_param(node, writes[i]) || is_builtin_name(writes[i])) {
                    free(writes[i]);
                    continue;
                }
                // A write `name = expr` in the closure captures an enclosing
                // binding only when that binding is VISIBLE from the closure
                // (the enclosing scope's own top level, or a trailing block the
                // closure sits inside) AND *precedes* it. A same-named binding
                // that comes after the closure, or lives in a sibling trailing
                // block or an if/for/while body, is an unrelated local
                // (#2189): the closure's `name = expr` is then its own fresh
                // local, so promoting it emitted C referencing a name declared
                // later or already out of scope (an undeclared use), or, for a
                // nested closure two levels in, a write-through into a cell
                // that closure's env owns and frees -> heap-use-after-free. A
                // binding that precedes the closure in its scope is genuinely
                // mutated through, and stays a capture.
                int decl_line = enclosing_decl_line(gen->program, enclosing_func,
                                                    writes[i], node);
                if (decl_line <= node->line) {
                    if (cap_count >= cap_cap) {
                        cap_cap = cap_cap ? cap_cap * 2 : 8;
                        captures = aether_xrealloc(captures, cap_cap * sizeof(char*));
                    }
                    captures[cap_count++] = strdup(writes[i]);
                }
                free(writes[i]);
            }
            free(writes);
        }

        // Register closure
        if (gen->closure_count >= gen->closure_capacity) {
            gen->closure_capacity = gen->closure_capacity ? gen->closure_capacity * 2 : 16;
            gen->closures = aether_xrealloc(gen->closures, gen->closure_capacity * sizeof(gen->closures[0]));
        }
        gen->closures[gen->closure_count].id = id;
        gen->closures[gen->closure_count].closure_node = node;
        gen->closures[gen->closure_count].captures = captures;
        gen->closures[gen->closure_count].capture_types = NULL; // resolved during emit
        gen->closures[gen->closure_count].capture_count = cap_count;
        gen->closures[gen->closure_count].parent_func = enclosing_func ? strdup(enclosing_func) : NULL;
        gen->closure_count++;
    }

    // Recurse into children first. An AST_VARIABLE_DECLARATION whose RHS is
    // an AST_CLOSURE needs the closure to be discovered (and its value set to
    // the id string) before we can seed closure_var_map below.
    //
    // A hoisted closure is a scope boundary: everything below it captures from
    // the closure's own C frame, so descendants get its synthetic scope name.
    // (Note this runs AFTER the capture analysis above, which correctly used
    // the closure's own enclosing scope.) Trailing blocks are not a boundary —
    // they inline at the call site — and are handled by the early return above.
    char child_scope[64];
    const char* inner_scope = enclosing_func;
    if (is_hoisted_closure(node)) {
        closure_scope_name(node, child_scope, sizeof(child_scope));
        inner_scope = child_scope;
    }
    for (int i = 0; i < node->child_count; i++) {
        discover_closures_scoped(gen, node->children[i], inner_scope);
    }

    // Seed closure_var_map so call() emission inside other closure bodies
    // (which runs before the main statement walk) can resolve captured
    // closures back to their concrete id.
    if (node->type == AST_VARIABLE_DECLARATION && node->value && node->child_count > 0) {
        ASTNode* rhs = node->children[0];
        int cid_to_bind = -1;
        if (rhs && rhs->type == AST_CLOSURE && rhs->value) {
            cid_to_bind = atoi(rhs->value);
        } else if (rhs && rhs->type == AST_FUNCTION_CALL && rhs->value) {
            // If the initializer is a call to a user function that returns
            // a closure variable, bind this var to that closure's id too.
            // Example: w = build_pair() where build_pair ends in `return wrapped`
            // and wrapped is a known closure variable.
            ASTNode* target_fn = find_function_definition_by_name(gen->program, rhs->value);
            if (target_fn) {
                for (int i = 0; i < target_fn->child_count; i++) {
                    ASTNode* body = target_fn->children[i];
                    if (!body || body->type != AST_BLOCK) continue;
                    ASTNode* ret_expr = find_first_return_expr(body);
                    if (ret_expr && ret_expr->type == AST_IDENTIFIER && ret_expr->value)
                        cid_to_bind = closure_var_id(gen, target_fn->value, ret_expr->value);
                    break;
                }
            }
        }
        // A variable previously bound to a different closure (either via
        // declaration or via an earlier reassignment) has no single
        // identity: the bind marks it ambiguous so call() falls back to
        // generic function-pointer dispatch through .fn. A binding to a
        // closure value this walk cannot name (a struct field, a parameter,
        // a call whose result it does not know) is bound as -1 for the same
        // reason: left out, the variable kept the literal it was bound to
        // before (or after, in a loop), and call() ran that one.
        Type* vt = node->node_type ? node->node_type : (rhs ? rhs->node_type : NULL);
        int closure_typed = vt && vt->kind == TYPE_FUNCTION && !vt->is_fnptr;
        if (cid_to_bind >= 0 || closure_typed) {
            closure_var_bind(gen, enclosing_func, node->value, cid_to_bind);
        }
    }
}

// Resolve call(<closure_var>) in `scope` to the concrete return type, or NULL.
static Type* resolve_call_type(CodeGenerator* gen, ASTNode* call_expr, const char* scope) {
    if (!call_expr || call_expr->type != AST_FUNCTION_CALL ||
        !call_expr->value || strcmp(call_expr->value, "call") != 0 ||
        call_expr->child_count < 1 || !call_expr->children[0] ||
        call_expr->children[0]->type != AST_IDENTIFIER ||
        !call_expr->children[0]->value) return NULL;
    int callee_id = closure_var_id(gen, scope, call_expr->children[0]->value);
    if (callee_id < 0) return NULL;
    for (int cj = 0; cj < gen->closure_count; cj++) {
        if (gen->closures[cj].id != callee_id) continue;
        ASTNode* cnode = gen->closures[cj].closure_node;
        ASTNode* cbody = NULL;
        for (int k = cnode->child_count - 1; k >= 0; k--) {
            if (cnode->children[k] && cnode->children[k]->type == AST_BLOCK) {
                cbody = cnode->children[k];
                break;
            }
        }
        /* #2501: a tuple result is the typechecker's (the first value
         * alone is not what the call yields). */
        ASTNode* first_ret = cbody ? find_first_value_return(cbody) : NULL;
        if (first_ret && first_ret->child_count > 1) return NULL;
        ASTNode* ret = cbody ? find_first_return_expr(cbody) : NULL;
        if (ret && ret->node_type && ret->node_type->kind != TYPE_UNKNOWN &&
            ret->node_type->kind != TYPE_INT) {
            // TYPE_INT is the typechecker default and may be wrong for
            // call-of-call chains — prefer anything else.
            return ret->node_type;
        }
        break;
    }
    return NULL;
}

// Walk the AST and patch AST_FUNCTION_CALL nodes of the form `call(x, ...)`
// where `x` resolves through closure_var_map to a known closure. Sets the
// call expression's node_type to match the closure's return type so that
// downstream consumers (print/println format selection, variable-decl C
// type selection, etc.) generate correct C. The global `call` symbol is
// typed TYPE_INT, which is wrong for any closure that returns a string or
// pointer.
static void propagate_call_return_types_in(CodeGenerator* gen, ASTNode* node,
                                           Type* fn_ret, const char* scope);

static void propagate_call_return_types(CodeGenerator* gen, ASTNode* node) {
    propagate_call_return_types_in(gen, node, NULL, NULL);
}

/* `fn_ret` is the declared return type of the function/closure whose body we
 * are inside, or NULL at the top level. It resolves the one case
 * resolve_call_type cannot: `call(f, ...)` where `f` is a `fn` PARAMETER, so
 * there is no closure body to read a type from. In `-> ptr f(...) { return
 * call(f, v) }` the context supplies it. Without this the global `call`
 * symbol's TYPE_INT default reaches codegen, which casts the closure to an
 * int-returning function pointer and truncates a returned pointer.
 *
 * `scope` names the scope the walk is in, as discover_closures_scoped
 * names it, which is where a `call(f)` resolves `f` (#2513). */
static void propagate_call_return_types_in(CodeGenerator* gen, ASTNode* node,
                                           Type* fn_ret, const char* scope) {
    if (!node) return;
    if (node->type == AST_FUNCTION_DEFINITION ||
        node->type == AST_BUILDER_FUNCTION) {
        Type* inner = node->node_type;
        for (int i = 0; i < node->child_count; i++) {
            propagate_call_return_types_in(gen, node->children[i], inner,
                                           node->value ? node->value : scope);
        }
        return;
    }
    if (node->type == AST_MAIN_FUNCTION || is_receive_arm_scope(node)) {
        char arm_name[64];
        snprintf(arm_name, sizeof(arm_name), "__recv_arm_%p", (void*)node);
        const char* inner = node->type == AST_MAIN_FUNCTION ? "main" : arm_name;
        for (int i = 0; i < node->child_count; i++) {
            propagate_call_return_types_in(gen, node->children[i], fn_ret, inner);
        }
        return;
    }
    /* A closure body is its OWN return context: its type comes from
     * resolve_closure_return_type, not from the enclosing function.
     * Carrying the outer type in would stamp `return call(...)` inside
     * a closure with the enclosing function's return type, which is how
     * `bump = || { return call(digit, 1) }` inside a closure-returning
     * builder got typed as the closure struct instead of int. */
    if (node->type == AST_CLOSURE) {
        char own[64];
        const char* inner = scope;
        if (is_hoisted_closure(node)) {
            closure_scope_name(node, own, sizeof(own));
            inner = own;
        }
        for (int i = 0; i < node->child_count; i++) {
            propagate_call_return_types_in(gen, node->children[i], NULL, inner);
        }
        return;
    }
    if (node->type == AST_RETURN_STATEMENT && node->child_count == 1 &&
        fn_ret && fn_ret->kind != TYPE_UNKNOWN && fn_ret->kind != TYPE_INT) {
        ASTNode* r = node->children[0];
        if (r && r->type == AST_FUNCTION_CALL && r->value &&
            strcmp(r->value, "call") == 0 &&
            (!r->node_type || r->node_type->kind == TYPE_INT ||
             r->node_type->kind == TYPE_UNKNOWN) &&
            !resolve_call_type(gen, r, scope)) {
            r->node_type = clone_type(fn_ret);
        }
    }
    Type* resolved = resolve_call_type(gen, node, scope);
    if (resolved && (!node->node_type || node->node_type->kind != resolved->kind)) {
        node->node_type = clone_type(resolved);
    }
    // Back-propagate into variable declarations whose initializer is a
    // call(<closure_var>) — otherwise the var is declared `int` based on
    // the typechecker's stale default and later casts or format-string
    // selection go wrong.
    if (node->type == AST_VARIABLE_DECLARATION && node->child_count > 0) {
        Type* init_resolved = resolve_call_type(gen, node->children[0], scope);
        if (init_resolved && (!node->node_type || node->node_type->kind == TYPE_INT)) {
            node->node_type = clone_type(init_resolved);
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        propagate_call_return_types_in(gen, node->children[i], fn_ret, scope);
    }
}

// Add `name` to a function's promoted-names entry in gen->promoted_funcs,
// creating the entry if absent, de-duplicating names within it.
static void add_promoted_name(CodeGenerator* gen, const char* func_name, const char* name) {
    if (!func_name || !name) return;
    int idx = -1;
    for (int i = 0; i < gen->promoted_func_count; i++) {
        if (strcmp(gen->promoted_funcs[i].func_name, func_name) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (gen->promoted_func_count >= gen->promoted_func_capacity) {
            gen->promoted_func_capacity = gen->promoted_func_capacity ? gen->promoted_func_capacity * 2 : 8;
            gen->promoted_funcs = aether_xrealloc(gen->promoted_funcs,
                gen->promoted_func_capacity * sizeof(gen->promoted_funcs[0]));
        }
        idx = gen->promoted_func_count++;
        gen->promoted_funcs[idx].func_name = strdup(func_name);
        gen->promoted_funcs[idx].names = NULL;
        gen->promoted_funcs[idx].count = 0;
    }
    for (int i = 0; i < gen->promoted_funcs[idx].count; i++) {
        if (strcmp(gen->promoted_funcs[idx].names[i], name) == 0) return;
    }
    gen->promoted_funcs[idx].names = aether_xrealloc(gen->promoted_funcs[idx].names,
        (gen->promoted_funcs[idx].count + 1) * sizeof(char*));
    gen->promoted_funcs[idx].names[gen->promoted_funcs[idx].count++] = strdup(name);
}

// Route 1 promotion analysis. After discover_closures has run, we know every
// closure's captures and its parent function. Scan each closure's body for
// captures that are assigned to; those names must be heap-promoted in the
// parent function (so outer reads/writes, and sibling closures, all share
// the same cell).
//
// When the parent scope is itself a closure, the promotion has to be recorded
// at EVERY scope from the writer up to the one that actually declares the name.
// The declaring scope mints the heap cell; each closure in between holds a `T*`
// env slot and forwards the pointer. Stopping at the immediate parent would
// give the intermediate closure a `T*` slot fed from a plain `T` local — a
// pointer-from-integer miscompile.
static void promote_up_from(CodeGenerator* gen, const char* start_scope, const char* cap) {
    const char* scope = start_scope;
    for (;;) {
        add_promoted_name(gen, scope, cap);
        if (strncmp(scope, "__closure_", 10) != 0) return;  // reached a real function
        ASTNode* c = find_closure_by_name(gen->program, scope);
        if (!c) return;
        // The scope that declares the name owns the cell — stop there.
        if (is_closure_param(c, cap)) return;
        if (subtree_declares(last_block_child(c), cap)) return;
        // Otherwise the name is this closure's own capture: keep climbing.
        scope = find_enclosing_scope_name(gen->program, NULL, c);
        if (!scope) return;
    }
}

static Type* lookup_var_type(CodeGenerator* gen, const char* var_name, const char* parent_func);

/* A capture holding a struct or a fixed-size array by value (not through a
 * pointer, a slice or a reference): a field or element write in a closure
 * changes the variable itself, so it needs the shared cell as a bare
 * `name = ...` does (#2458; arrays #2474). */
static int capture_is_held_by_value(CodeGenerator* gen, const char* name,
                                    const char* parent_func) {
    Type* t = lookup_var_type(gen, name, parent_func);
    return t && (t->kind == TYPE_STRUCT ||
                 (type_is_sized_array(t) && t->array_size > 0));
}

static void compute_promoted_captures(CodeGenerator* gen) {
    for (int ci = 0; ci < gen->closure_count; ci++) {
        const char* parent_func = gen->closures[ci].parent_func;
        if (!parent_func) continue;
        ASTNode* body = last_block_child(gen->closures[ci].closure_node);
        if (!body) continue;
        for (int j = 0; j < gen->closures[ci].capture_count; j++) {
            const char* cap = gen->closures[ci].captures[j];
            if (!cap) continue;
            if (is_assigned_to(body, cap) ||
                (is_written_through(body, cap) &&
                 capture_is_held_by_value(gen, cap, parent_func))) {
                promote_up_from(gen, parent_func, cap);
            }
        }
    }
}

// Lookup: are the promoted names for `func_name` non-empty? Returns the list
// and count via out params; both may be NULL/0 when the function has none.
void get_promoted_names_for_func(CodeGenerator* gen, const char* func_name,
                                 char*** out_names, int* out_count) {
    *out_names = NULL;
    *out_count = 0;
    if (!func_name) return;
    for (int i = 0; i < gen->promoted_func_count; i++) {
        if (strcmp(gen->promoted_funcs[i].func_name, func_name) == 0) {
            *out_names = gen->promoted_funcs[i].names;
            *out_count = gen->promoted_funcs[i].count;
            return;
        }
    }
}

// Convenience: is `name` promoted in the current codegen context?
int is_promoted_capture(CodeGenerator* gen, const char* name) {
    if (!name) return 0;
    for (int i = 0; i < gen->current_promoted_capture_count; i++) {
        if (gen->current_promoted_captures[i] &&
            strcmp(gen->current_promoted_captures[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

// Add `name` to closure ci's capture list if not already present.
static void add_capture(CodeGenerator* gen, int ci, const char* name) {
    if (is_discard_name(name)) return;
    for (int i = 0; i < gen->closures[ci].capture_count; i++) {
        if (strcmp(gen->closures[ci].captures[i], name) == 0) return;
    }
    int n = gen->closures[ci].capture_count;
    gen->closures[ci].captures = aether_xrealloc(gen->closures[ci].captures,
                                         (n + 1) * sizeof(char*));
    gen->closures[ci].captures[n] = strdup(name);
    gen->closures[ci].capture_count = n + 1;
}

// Transitive capture: a closure must also capture everything its NESTED
// closures capture from scopes further out.
//
// The inner closure's construction site is emitted inside the outer closure's C
// function and reads each captured name raw (`_e->nm = nm;`), so every name the
// inner env needs must be live in the outer frame — as the outer's own local,
// its param, or its own capture (the `T nm = _env->nm;` prologue alias). The
// per-closure analysis can't see this: it stops at nested closures, by design,
// because their locals are a different scope.
//
// So propagate outward to a fixpoint: for each closure whose parent scope is
// another closure, any capture that is not local to that parent closure must be
// captured by the parent too. Iterating to a fixpoint (rather than one pass)
// carries a name up through an arbitrarily deep nest, one level per round.
static void propagate_nested_captures(CodeGenerator* gen) {
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int ci = 0; ci < gen->closure_count; ci++) {
            const char* parent = gen->closures[ci].parent_func;
            if (!parent || strncmp(parent, "__closure_", 10) != 0) continue;
            // Find the enclosing closure's own entry.
            int pi = -1;
            for (int k = 0; k < gen->closure_count; k++) {
                char nm[64];
                closure_scope_name(gen->closures[k].closure_node, nm, sizeof(nm));
                if (strcmp(nm, parent) == 0) { pi = k; break; }
            }
            if (pi < 0) continue;
            ASTNode* pnode = gen->closures[pi].closure_node;
            ASTNode* pbody = last_block_child(pnode);
            for (int j = 0; j < gen->closures[ci].capture_count; j++) {
                const char* cap = gen->closures[ci].captures[j];
                // Already live in the parent closure's own frame — as a param
                // or a local it declares anywhere in its body. The chain ends.
                if (is_closure_param(pnode, cap)) continue;
                if (subtree_declares(pbody, cap)) continue;
                int before = gen->closures[pi].capture_count;
                add_capture(gen, pi, cap);
                if (gen->closures[pi].capture_count != before) changed = 1;
            }
        }
    }
}

// Public entry point — starts at program root with no enclosing function.
void discover_closures(CodeGenerator* gen, ASTNode* node) {
    discover_closures_scoped(gen, node, NULL);
    // Second pass: now that closure_var_map is fully populated, propagate
    // return types back onto call() expressions the typechecker left as int.
    propagate_call_return_types(gen, node);
    // Third pass: carry captures of nested closures out to their enclosing
    // closure, whose C frame the inner env is built from.
    propagate_nested_captures(gen);
    // Fourth pass: compute which captures need heap promotion per function.
    compute_promoted_captures(gen);
}

// Find the enclosing AST_ACTOR_DEFINITION that contains `arm_node`.
// Returns NULL if arm_node isn't inside any actor.
static ASTNode* find_enclosing_actor(ASTNode* root, ASTNode* arm_node) {
    if (!root || !arm_node) return NULL;
    if (root->type == AST_ACTOR_DEFINITION) {
        // Check if arm_node is a descendant of this actor.
        for (int i = 0; i < root->child_count; i++) {
            ASTNode* child = root->children[i];
            if (!child) continue;
            if (child == arm_node) return root;
            // Dive one level deeper — the receive block sits under actor,
            // arms sit under the receive block.
            for (int j = 0; j < child->child_count; j++) {
                if (child->children[j] == arm_node) return root;
            }
        }
    }
    for (int i = 0; i < root->child_count; i++) {
        ASTNode* found = find_enclosing_actor(root->children[i], arm_node);
        if (found) return found;
    }
    return NULL;
}

// L4 validation: a closure inside an actor handler that writes to an
// actor state field is currently miscompiled (the closure has no access
// to `self`, so `state_field = ...` emits a stale-local write). Until
// threading self through the closure env is implemented, reject the
// pattern at compile time with a clear error. Returns 0 on failure
// (errors were reported via aether_error_report), 1 otherwise.
int validate_closure_state_mutations(CodeGenerator* gen, ASTNode* program) {
    int ok = 1;
    for (int ci = 0; ci < gen->closure_count; ci++) {
        const char* parent_func = gen->closures[ci].parent_func;
        if (!parent_func || strncmp(parent_func, "__recv_arm_", 11) != 0) continue;

        // Find the arm node, then its enclosing actor.
        ASTNode* arm = find_receive_arm_by_name(program, parent_func);
        if (!arm) continue;
        ASTNode* actor = find_enclosing_actor(program, arm);
        if (!actor) continue;

        // Collect state field names: AST_STATE_DECLARATION or
        // AST_VARIABLE_DECLARATION children of the actor with
        // annotation marking them as state vars. Actors typically list
        // state decls as top-level children of AST_ACTOR_DEFINITION.
        const char* state_names[64];
        int state_count = 0;
        for (int j = 0; j < actor->child_count && state_count < 64; j++) {
            ASTNode* c = actor->children[j];
            if (!c || !c->value) continue;
            if (c->type == AST_STATE_DECLARATION ||
                (c->type == AST_VARIABLE_DECLARATION && c->annotation &&
                 strcmp(c->annotation, "state") == 0)) {
                state_names[state_count++] = c->value;
            }
        }
        if (state_count == 0) continue;

        // Walk the closure body looking for assignments to any of the
        // state field names.
        ASTNode* closure = gen->closures[ci].closure_node;
        ASTNode* body = NULL;
        for (int k = closure->child_count - 1; k >= 0; k--) {
            if (closure->children[k] && closure->children[k]->type == AST_BLOCK) {
                body = closure->children[k];
                break;
            }
        }
        if (!body) continue;

        for (int n = 0; n < state_count; n++) {
            /* An element or field write (`hist[i] = v`, `pos.x += 1`)
             * writes the state field as much as `hist = ...` does. A state
             * array is no capture the closure could share in a cell (#2474):
             * it lives in the actor, which the closure cannot reach. */
            if (!is_assigned_to(body, state_names[n]) &&
                !is_written_through(body, state_names[n])) continue;
            // Report error. Location: closure node.
            char msg[512];
            const char* actor_name = actor->value ? actor->value : "actor";
            snprintf(msg, sizeof(msg),
                "closure inside actor '%s' handler writes state field '%s': "
                "not supported (closures can't mutate actor state; the "
                "closure has no access to self)",
                actor_name, state_names[n]);
            char suggestion[256];
            snprintf(suggestion, sizeof(suggestion),
                "copy '%s' into an arm-local, mutate the local, then write "
                "back. See tests/syntax/README_closure_actor_state_limitation.md "
                "for the workaround pattern.",
                state_names[n]);
            aether_error_full(msg, closure->line, closure->column,
                              suggestion, "in actor handler",
                              AETHER_ERR_ACTOR_ERROR);
            ok = 0;
        }
    }
    return ok;
}

// Find the type of a declaration of `var_name` anywhere in `node`'s subtree,
// including inside nested if/for/while blocks, but not inside a hoisted
// closure (whose locals are a different scope). Returns NULL if not found.
//
// The recursion into nested blocks is load-bearing and must stay in lockstep
// with `subtree_declares`, which is what decides a name IS a capture: a name
// declared inside a loop body (`while … { nm = string.concat(…) }`) is captured
// by a closure in that loop, so its type has to be resolvable from the same
// scope. A top-level-statements-only scan fell through to the "int" default and
// silently captured a string as an int — a -Wint-conversion warning and a
// segfault at run time, not a compile error.
static Type* decl_type_in_scope(ASTNode* node, const char* var_name) {
    if (!node) return NULL;
    if (is_hoisted_closure(node)) return NULL;
    if ((node->type == AST_VARIABLE_DECLARATION || node->type == AST_CONST_DECLARATION) &&
        node->value && strcmp(node->value, var_name) == 0) {
        if (node->node_type && node->node_type->kind != TYPE_UNKNOWN) {
            return node->node_type;
        }
        if (node->child_count > 0 && node->children[0] &&
            node->children[0]->node_type &&
            node->children[0]->node_type->kind != TYPE_UNKNOWN) {
            return node->children[0]->node_type;
        }
        // Declaration found but untyped — keep looking; a later re-declaration
        // or assignment of the same name may carry the resolved type.
    }
    for (int i = 0; i < node->child_count; i++) {
        Type* t = decl_type_in_scope(node->children[i], var_name);
        if (t) return t;
    }
    return NULL;
}

// Search a single function node for `var_name` as either a parameter
// (AST_PATTERN_VARIABLE directly under the function) or a local variable
// declaration inside the function body. Returns its type or NULL.
static Type* lookup_in_function(ASTNode* func, const char* var_name) {
    if (!func) return NULL;
    int is_main = (func->type == AST_MAIN_FUNCTION);
    // Parameters: for regular functions, direct children that are
    // AST_PATTERN_VARIABLE with matching value. main has no params.
    if (!is_main) {
        for (int i = 0; i < func->child_count; i++) {
            ASTNode* p = func->children[i];
            if (p && p->type == AST_PATTERN_VARIABLE && p->value &&
                strcmp(p->value, var_name) == 0 &&
                p->node_type && p->node_type->kind != TYPE_UNKNOWN) {
                return p->node_type;
            }
        }
    }
    // Locals: walk the body block(s), nested blocks included.
    for (int j = 0; j < func->child_count; j++) {
        ASTNode* body = func->children[j];
        if (!body || body->type != AST_BLOCK) continue;
        Type* t = decl_type_in_scope(body, var_name);
        if (t) return t;
    }
    return NULL;
}

// Look up a variable's type (NULL when unknown). If `parent_func` is non-NULL, prefer the
// parameters and locals of that function — this is the closure's lexical
// parent and the only correct place to resolve its captures. Fall back to a
// program-wide search for backward compatibility with call sites that don't
// yet pass a parent.
static Type* lookup_var_type(CodeGenerator* gen, const char* var_name, const char* parent_func) {
    if (!gen->program || !var_name) return NULL;
    // The parent scope may be another closure (`__closure_<ptr>`) — resolve
    // against its params/body, then chain up to ITS parent for names it in
    // turn captures. Falls through to the program-wide search below only if
    // the whole chain comes up empty.
    if (parent_func && strncmp(parent_func, "__closure_", 10) == 0) {
        ASTNode* c = find_closure_by_name(gen->program, parent_func);
        if (c) {
            for (int i = 0; i < c->child_count; i++) {
                ASTNode* p = c->children[i];
                if (p && p->type == AST_CLOSURE_PARAM && p->value &&
                    strcmp(p->value, var_name) == 0 &&
                    p->node_type && p->node_type->kind != TYPE_UNKNOWN) {
                    return p->node_type;
                }
            }
            Type* t = decl_type_in_scope(last_block_child(c), var_name);
            if (t) return t;
            const char* outer = find_enclosing_scope_name(gen->program, NULL, c);
            if (outer) return lookup_var_type(gen, var_name, outer);
        }
        return NULL;
    }
    /* #2492: a receive arm binds its message pattern's names, typed by the
     * message's fields, and its body's locals. It resolves them itself: a
     * handler is no function, so the search below looked through every
     * function for the name and fell back to int. */
    if (parent_func && strncmp(parent_func, "__recv_arm_", 11) == 0) {
        ASTNode* arm = find_receive_arm_by_name(gen->program, parent_func);
        if (!arm) return NULL;
        ASTNode* pf = receive_arm_binding(arm, var_name);
        if (pf) {
            ASTNode* pattern = receive_arm_pattern(arm);
            return message_field_type(gen->program, pattern->value, pf->value);
        }
        return decl_type_in_scope(receive_arm_body(arm), var_name);
    }
    // Parent-function-first lookup, through the program index (#2007).
    if (parent_func) {
        ASTNode* top = NULL;
        if (strcmp(parent_func, "main") == 0) {
            ProgramIndex* ix = program_index(gen->program);
            top = ix ? ix->main_fn : NULL;
        } else {
            top = find_function_definition_by_name(gen->program, parent_func);
        }
        if (top) {
            Type* t = lookup_in_function(top, var_name);
            if (t) return t;
            // don't scan other functions — captured names resolve lexically
        }
    }
    // Fallback: program-wide search (kept for safety when parent_func is NULL
    // or when the var was declared at an unexpected location).
    for (int i = 0; i < gen->program->child_count; i++) {
        ASTNode* top = gen->program->children[i];
        if (!top) continue;
        if (top->type == AST_FUNCTION_DEFINITION || top->type == AST_BUILDER_FUNCTION || top->type == AST_MAIN_FUNCTION) {
            Type* t = lookup_in_function(top, var_name);
            if (t) return t;
        }
    }
    return NULL;
}

// The C type of a variable, through lookup_var_type; "int" when it has none.
const char* lookup_var_c_type(CodeGenerator* gen, const char* var_name, const char* parent_func) {
    Type* t = lookup_var_type(gen, var_name, parent_func);
    return t ? get_c_type(t) : "int"; // fallback
}

/* #2464: a captured fixed-size array (`int[3] arr`), or NULL. Its C type
 * spells as `int[3]`, which is not a declarator: the env field, the
 * constructor parameter and the body alias each spell `int arr[3]` / the
 * element type instead, as a struct field does (generate_extern_struct_field),
 * and the capture copies it with memcpy, since a C array does not assign. */
static Type* capture_sized_array_type(CodeGenerator* gen, const char* name,
                                      const char* parent_func) {
    Type* t = lookup_var_type(gen, name, parent_func);
    return (t && type_is_sized_array(t) && t->array_size > 0) ? t : NULL;
}

// Resolve a closure's C return type from its body. Extracted so the
// pre-pass (forward declarations) and main pass (bodies) agree on the
// same signature. A closure with no return-value statements is void.
// A closure whose return expression is `call(<captured_closure>)` gets
// resolved through the captured closure's own body — the typechecker
// leaves those as TYPE_INT by default which is almost always wrong for
// call-of-call chains.
static const char* resolve_closure_return_type(CodeGenerator* gen, int ci) {
    ASTNode* closure = gen->closures[ci].closure_node;
    const char* parent_func = gen->closures[ci].parent_func;
    ASTNode* body_check = NULL;
    for (int i = closure->child_count - 1; i >= 0; i--) {
        if (closure->children[i] && closure->children[i]->type == AST_BLOCK) {
            body_check = closure->children[i];
            break;
        }
    }
    int has_return = body_check ? has_return_value(body_check) : 0;
    if (!has_return) return "void";
    /* #2501: `return a, b` makes the closure return a tuple. */
    Type* tuple = closure_tuple_return_type(closure);
    if (tuple) {
        const char* nm = closure_tuple_c_name(tuple);
        free_type(tuple);
        return nm;
    }
    const char* ret_type = "int";
    ASTNode* ret_expr = find_first_return_expr(body_check);
    int resolved = 0;
    if (ret_expr && ret_expr->type == AST_FUNCTION_CALL && ret_expr->value &&
        strcmp(ret_expr->value, "call") == 0 &&
        ret_expr->child_count >= 1 &&
        ret_expr->children[0] &&
        ret_expr->children[0]->type == AST_IDENTIFIER &&
        ret_expr->children[0]->value) {
        char own_scope[64];
        closure_scope_name(closure, own_scope, sizeof(own_scope));
        /* #2513: the callee as this closure's body sees the name. */
        int callee_id = closure_var_id(gen, own_scope, ret_expr->children[0]->value);
        for (int cj = 0; callee_id >= 0 && cj < gen->closure_count; cj++) {
            if (gen->closures[cj].id != callee_id) continue;
            ASTNode* callee_node = gen->closures[cj].closure_node;
            ASTNode* callee_body = NULL;
            for (int k = callee_node->child_count - 1; k >= 0; k--) {
                if (callee_node->children[k] &&
                    callee_node->children[k]->type == AST_BLOCK) {
                    callee_body = callee_node->children[k];
                    break;
                }
            }
            ASTNode* callee_ret = callee_body ? find_first_return_expr(callee_body) : NULL;
            Type* callee_tuple = closure_tuple_return_type(callee_node);   /* #2501 */
            if (callee_tuple) {
                ret_type = closure_tuple_c_name(callee_tuple);
                free_type(callee_tuple);
                resolved = 1;
            } else if (callee_ret) {
                if (callee_ret->node_type && callee_ret->node_type->kind != TYPE_UNKNOWN) {
                    ret_type = get_c_type(callee_ret->node_type);
                    resolved = 1;
                } else if (callee_ret->type == AST_IDENTIFIER && callee_ret->value) {
                    ret_type = lookup_var_c_type(gen, callee_ret->value,
                                                 gen->closures[cj].parent_func);
                    resolved = 1;
                }
            }
            break;
        }
    }
    if (!resolved && ret_expr) {
        if (ret_expr->node_type && ret_expr->node_type->kind != TYPE_UNKNOWN) {
            ret_type = get_c_type(ret_expr->node_type);
        } else if (ret_expr->type == AST_IDENTIFIER && ret_expr->value) {
            /* The closure's OWN parameters come first: `|x: ptr| { return x }`
             * returns the parameter, which lives in the closure's signature,
             * not in the parent function's scope. Without this the parent
             * lookup misses and the signature defaults to `int`, truncating a
             * returned pointer at the ABI boundary (the closure compiles, the
             * caller silently gets 32 bits of a 64-bit value). */
            for (int pi = 0; pi < closure->child_count; pi++) {
                ASTNode* p = closure->children[pi];
                if (p && p->type == AST_CLOSURE_PARAM && p->value &&
                    strcmp(p->value, ret_expr->value) == 0) {
                    if (p->node_type) ret_type = get_c_type(p->node_type);
                    resolved = 1;
                    break;
                }
            }
            if (!resolved) {
                ret_type = lookup_var_c_type(gen, ret_expr->value, parent_func);
            }
        }
    }
    /* Mixed returns: one string-typed return site makes this a string
     * closure whatever the first site was typed (see any_return_is_string).
     * A `void*` first site is an AetherString from a `-> ptr` builtin, which
     * the uniform-heap wrap copies out by its header, so `const char*` is
     * correct for both. */
    if (strcmp(ret_type, "const char*") != 0 && body_check &&
        any_return_is_string(body_check)) {
        ret_type = "const char*";
    }
    return ret_type;
}

// True when a captured variable is PROMOTED: the closure assigns to it, so
// it lives in a reference-counted heap cell the env points at (#2019). The
// env field is `ctype*`, the env takes a reference when it is built and
// gives it back in its destructor.
static int capture_is_promoted(CodeGenerator* gen, const char* name,
                               const char* parent_func) {
    char** promoted = NULL;
    int promoted_count = 0;
    get_promoted_names_for_func(gen, parent_func, &promoted, &promoted_count);
    for (int p = 0; p < promoted_count; p++) {
        if (promoted[p] && strcmp(promoted[p], name) == 0) return 1;
    }
    return 0;
}

// True when a captured variable is a READ-ONLY string capture — the case
// whose env store must go through aether_str_capture() so the env owns a
// reference (asks/closure-captured-heap-string-dangles.md: the enclosing
// scope releases its own reference on loop-carried reassignment and at
// scope exit, so a borrowed pointer dangles by the time a stored closure
// fires). Promoted (assigned-to) captures share a heap cell — the env
// field is `ctype*` and must NOT be routed through the string retain.
static int capture_is_retained_string(CodeGenerator* gen, const char* name,
                                      const char* parent_func) {
    if (capture_is_promoted(gen, name, parent_func)) return 0;
    const char* ctype = lookup_var_c_type(gen, name, parent_func);
    return ctype && (strcmp(ctype, "const char*") == 0 ||
                     strcmp(ctype, "char*") == 0);
}

/* #2494: a captured closure VALUE (not a promoted closure cell). The env
 * copies the `_AeClosure`, so it takes a reference to that closure's env
 * when it is built and gives it back in its destructor, as it does for a
 * string or a cell: the captured env then lives as long as either holder,
 * whoever frees first. */
static int capture_is_closure_value(CodeGenerator* gen, const char* name,
                                    const char* parent_func) {
    if (capture_is_promoted(gen, name, parent_func) ||
        capture_sized_array_type(gen, name, parent_func)) return 0;
    const char* ctype = lookup_var_c_type(gen, name, parent_func);
    return ctype && strcmp(ctype, "_AeClosure") == 0;
}

/* #2504: a captured struct VALUE (not a promoted struct cell) whose type
 * owns heap strings: the struct's name, else NULL. The env must hold a copy
 * with strings of its own (`<Name>_dup`) and destroy it in its destructor.
 * A plain copy shared the declaring scope's strings, which its destroy frees
 * while a returned or stored closure still reads them. */
static const char* capture_owning_struct(CodeGenerator* gen, const char* name,
                                         const char* parent_func) {
    if (capture_is_promoted(gen, name, parent_func) ||
        capture_sized_array_type(gen, name, parent_func)) return NULL;
    const char* ctype = lookup_var_c_type(gen, name, parent_func);
    if (!ctype) return NULL;
    Type t;
    memset(&t, 0, sizeof(t));
    t.kind = TYPE_STRUCT;
    t.struct_name = (char*)ctype;
    return struct_owning_strings(gen, &t);
}

/* #2463: a parameter of `closure` that a closure nested in it writes. The
 * nesting closure owns the shared cell (promote_up_from stops at the scope
 * declaring the name), so it takes the value as `_param_<name>` and the body
 * declares the cell under the plain name, as a function does for its own
 * promoted parameters (codegen_func.c). */
static int closure_param_is_promoted(CodeGenerator* gen, ASTNode* closure,
                                     const char* name) {
    char own_scope[64];
    closure_scope_name(closure, own_scope, sizeof(own_scope));
    return capture_is_promoted(gen, name, own_scope);
}

// Emit just the signature (no trailing `;` or `{`) of a closure function.
// Caller appends `;\n` for forward decls or ` {\n` for bodies.
//
// A parameter is a VALUE name: it is spelled with safe_value_name, the same
// mangling every reference in the body gets, so the two agree. This used
// to go through safe_c_name — the FUNCTION-name mangler, whose list also
// carries libc symbols — so `|index: int|` declared `ae_index` while the
// body read `index` (#2087): libc's index() on Linux, undeclared on
// Windows. A C parameter may shadow a libc function; only keywords need
// the rename.
static void emit_closure_signature(CodeGenerator* gen, int ci, const char* ret_type) {
    int id = gen->closures[ci].id;
    ASTNode* closure = gen->closures[ci].closure_node;
    fprintf(gen->output, "static %s _closure_fn_%d(_closure_env_%d* _env", ret_type, id, id);
    for (int i = 0; i < closure->child_count; i++) {
        ASTNode* p = closure->children[i];
        if (p && p->type == AST_CLOSURE_PARAM) {
            const char* ptype = "int";
            if (p->node_type) {
                ptype = get_c_type(p->node_type);
            }
            if (is_sized_array_param(p->node_type)) {
                /* #2516: copied into the body's own array (or a cell). */
                fprintf(gen->output, ", ");
                emit_sized_array_param_declarator(gen, p->node_type, p->value);
            } else if (closure_param_is_promoted(gen, closure, p->value)) {
                fprintf(gen->output, ", %s _param_%s", ptype, p->value);
            } else {
                fprintf(gen->output, ", %s %s", ptype, safe_value_name(p->value));
            }
        }
    }
    fprintf(gen->output, ")");
}

// Emit the env typedef for a closure.
static void emit_closure_env_typedef(CodeGenerator* gen, int ci) {
    int id = gen->closures[ci].id;
    char** captures = gen->closures[ci].captures;
    int cap_count = gen->closures[ci].capture_count;
    const char* parent_func = gen->closures[ci].parent_func;
    /* Captured-variable C names are emitted RAW throughout the closure
     * lowering — env struct field, prologue alias, `_aether_make_
     * closure` param, the `_e->field = value` stores, and the
     * construction-site argument. They must NOT go through
     * `safe_c_name`: that helper renames libc-colliding identifiers
     * (`dup`, `read`, `bind`, …) and is correct for *function* symbols
     * (link collisions), but a captured variable is referenced raw
     * everywhere else — its parent-scope declaration and the closure
     * body's use site both emit the plain name. Applying `safe_c_name`
     * on only some of the capture sites produced a half-renamed
     * `ae_dup` (struct field + make-param) against a raw `dup` (parent
     * value + body use) — `error: 'ae_dup' undeclared`. A captured
     * variable named `dup` is a perfectly legal C struct field / local
     * / parameter (no link symbol involved), so raw is both correct
     * and consistent. See new_string_len_something.md §3. */
    fprintf(gen->output, "typedef struct {\n");
    /* #1398: first field by contract, so a runtime owner (list_free, the
       worker pool) can release a captured env it has no type for. Must stay
       first and must match _AeEnvHeader in the runtime. */
    fprintf(gen->output, "    void (*_dtor)(void*);\n");
    /* #2494: second by contract (_AeEnvHead in the prologue). One reference
       per holder: the value's owner, and every env that captured it. */
    fprintf(gen->output, "    atomic_long _refs;\n");
    if (cap_count == 0) {
        fprintf(gen->output, "    int _dummy;\n");
    } else {
        for (int i = 0; i < cap_count; i++) {
            const char* ctype = lookup_var_c_type(gen, captures[i], parent_func);
            Type* arr = capture_sized_array_type(gen, captures[i], parent_func);
            if (capture_is_promoted(gen, captures[i], parent_func)) {
                const char* cell = promoted_cell_pointer(ctype, captures[i]);
                fprintf(gen->output, "    %s;\n", cell);
            } else if (arr) {
                fprintf(gen->output, "    %s %s[%d];\n",
                        get_c_type(arr->element_type), captures[i], arr->array_size);
            } else {
                fprintf(gen->output, "    %s %s;\n", ctype, captures[i]);
            }
        }
    }
    fprintf(gen->output, "} _closure_env_%d;\n\n", id);

    /* The env owns a reference per retained-string capture (#1398), per
       promoted cell (#2019) and per captured closure value (#2494), and a
       copy of each captured struct's strings (#2504), so
       teardown has to be member-aware: each is given back here, and a cell
       or a captured env is freed by whichever holder releases last. The env
       itself is reference-counted too (#2494): an env that captured this
       closure holds a reference, so this destructor is a release and only
       the last one tears the env down. */
    fprintf(gen->output, "static void _closure_env_%d_free(void* _p) {\n", id);
    fprintf(gen->output, "    if (!_p) return;\n");
    fprintf(gen->output, "    _closure_env_%d* _e = (_closure_env_%d*)_p;\n", id, id);
    fprintf(gen->output, "    if (atomic_fetch_sub_explicit(&_e->_refs, 1, memory_order_acq_rel) != 1) return;\n");
    {
        for (int i = 0; i < cap_count; i++) {
            int promoted = capture_is_promoted(gen, captures[i], parent_func);
            if (capture_is_closure_value(gen, captures[i], parent_func)) {
                fprintf(gen->output, "    _aether_closure_env_release(_e->%s.env);\n", captures[i]);
                continue;
            }
            const char* owning = capture_owning_struct(gen, captures[i], parent_func);
            if (owning) {
                fprintf(gen->output, "    %s_destroy(&_e->%s);\n", owning, captures[i]);
                continue;
            }
            if (!promoted && !capture_is_retained_string(gen, captures[i], parent_func)) continue;
            if (promoted) {
                /* A string cell owns its heap string and a struct cell the
                 * struct's owned fields: the last releaser frees them. */
                const char* release_fn =
                    promoted_cell_release_fn(gen, lookup_var_c_type(gen, captures[i], parent_func));
                fprintf(gen->output, "    %s(_e->%s);\n", release_fn, captures[i]);
            } else {
                fprintf(gen->output, "    aether_string_release_captured(_e->%s);\n", captures[i]);
            }
        }
    }
    fprintf(gen->output, "    free(_p);\n");
    fprintf(gen->output, "}\n\n");
}

// Emit all hoisted closure environment structs and static functions.
// Two passes: pass 1 emits every env typedef and every function
// prototype so a closure body can reference a later-numbered closure
// (e.g. when an inline `|a,b| { ... }` lambda is passed as an argument
// inside the outer closure's body). Pass 2 emits bodies + MSVC
// constructor helpers.
/* Pass 1 only: env typedefs + forward declarations.
 *
 * Split out from the bodies so the two halves can straddle the message
 * definitions. A closure body containing `a ? Msg {}` lowers through the
 * codegen message registry, which is not populated until the
 * AST_MESSAGE_DEFINITION arm runs — so emitting bodies before that arm
 * produced an "unknown message type" error comment where the expression
 * belonged, and the C compiler stopped at "expected expression before
 * ';'". The declarations
 * have no such dependency, so they stay early (other emitted code
 * references them) while the bodies move after the messages. (#1626) */
/* The parameter list of `_aether_make_closure_<id>`, the non-GCC
 * constructor of closure `ci`'s value (one argument per capture): shared by
 * its prototype (emit_closure_declarations) and its definition
 * (emit_closure_definitions), which must agree. A promoted capture arrives
 * as the cell pointer, `ctype*`, which is also what the env field is. A
 * fixed-size array arrives as a pointer to its elements (#2464). */
static void emit_make_closure_params(CodeGenerator* gen, int ci) {
    char** captures = gen->closures[ci].captures;
    int cap_count = gen->closures[ci].capture_count;
    const char* parent_func = gen->closures[ci].parent_func;
    for (int i = 0; i < cap_count; i++) {
        if (i > 0) fprintf(gen->output, ", ");
        const char* ctype = lookup_var_c_type(gen, captures[i], parent_func);
        Type* arr = capture_sized_array_type(gen, captures[i], parent_func);
        if (arr && !capture_is_promoted(gen, captures[i], parent_func)) {
            fprintf(gen->output, "%s* %s", get_c_type(arr->element_type), captures[i]);
            continue;
        }
        if (capture_is_promoted(gen, captures[i], parent_func)) {
            const char* cell = promoted_cell_pointer(ctype, captures[i]);
            fprintf(gen->output, "%s", cell);
            continue;
        }
        fprintf(gen->output, "%s %s", ctype, captures[i]);
    }
}

void emit_closure_declarations(CodeGenerator* gen) {
    for (int ci = 0; ci < gen->closure_count; ci++) {
        emit_closure_env_typedef(gen, ci);
        /* #2501: a tuple result's typedef precedes the prototype. */
        Type* tuple = closure_tuple_return_type(gen->closures[ci].closure_node);
        if (tuple) {
            ensure_tuple_typedef(gen, tuple);
            free_type(tuple);
        }
        const char* ret_type = resolve_closure_return_type(gen, ci);
        emit_closure_signature(gen, ci, ret_type);
        fprintf(gen->output, ";\n");
        /* #2528: the non-GCC constructor is defined with the bodies, after
         * the functions that build the closure; without this prototype a
         * capturing closure made in a function did not compile on that
         * path (an implicit declaration, then conflicting types). */
        if (gen->closures[ci].capture_count > 0) {
            fprintf(gen->output, "#if !AETHER_GCC_COMPAT\nstatic _AeClosure _aether_make_closure_%d(",
                    gen->closures[ci].id);
            emit_make_closure_params(gen, ci);
            fprintf(gen->output, ");\n#endif\n");
        }
    }
    if (gen->closure_count > 0) fprintf(gen->output, "\n");
}

void emit_closure_definitions(CodeGenerator* gen) {
    /* A closure body is not a continuation of whatever function was
     * emitted last, so it must not inherit that function's return type.
     * gen->current_func_return_type feeds the `_builder_ret` type in
     * return lowering; leaving a stale value there made a closure that
     * `return 0`s emit `_tuple_int_string _builder_ret = 0;` when the
     * previously-emitted function happened to return a tuple.
     *
     * This was latent while bodies were emitted before the top-level
     * function loop (the field was still NULL then). Moving them after
     * the message definitions for #1626 exposed it, so it is saved and
     * cleared here rather than relying on emission position. */
    Type* saved_ret = gen->current_func_return_type;
    gen->current_func_return_type = NULL;

    // Pass 2: bodies and constructors.
    for (int ci = 0; ci < gen->closure_count; ci++) {
        int id = gen->closures[ci].id;
        ASTNode* closure = gen->closures[ci].closure_node;
        char** captures = gen->closures[ci].captures;
        int cap_count = gen->closures[ci].capture_count;
        const char* parent_func = gen->closures[ci].parent_func;

        // Look up the parent function's promoted names — captures matching
        // them get a pointer-typed env slot and pointer-typed body alias.
        char** parent_promoted = NULL;
        int parent_promoted_count = 0;
        get_promoted_names_for_func(gen, parent_func, &parent_promoted, &parent_promoted_count);

        // This closure's own scope name — a closure nested inside it records
        // promotions of ITS locals here.
        char own_scope[64];
        closure_scope_name(closure, own_scope, sizeof(own_scope));

        const char* ret_type = resolve_closure_return_type(gen, ci);
        restore_fnptr_scope_for_closure(gen, parent_func);
        emit_closure_signature(gen, ci, ret_type);
        fprintf(gen->output, " {\n");
        gen->in_string_closure = strcmp(ret_type, "const char*") == 0;
        /* #2501: a multi-value return builds the closure's tuple. */
        Type* closure_ret_tuple = closure_tuple_return_type(closure);
        gen->current_func_return_type = closure_ret_tuple;

        // Find body first so we can detect which captures are mutated.
        ASTNode* body = NULL;
        for (int i = closure->child_count - 1; i >= 0; i--) {
            if (closure->children[i] && closure->children[i]->type == AST_BLOCK) {
                body = closure->children[i];
                break;
            }
        }

        // Partition captures into mutated (env-backed), promoted (heap cell
        // alias), and read-only (value alias).
        // - Promoted: parent function has this name in its promoted set.
        //   Env slot is already `T*`; prologue aliases as `T* name`;
        //   reads/writes in body dereference (is_promoted_capture path).
        // - Env-backed (pre-Route-1 path): assigned-to in body but NOT
        //   promoted. Skip the alias and route writes through _env->. Only
        //   fires when a closure writes a capture that isn't promoted
        //   in its parent — shouldn't happen after Route 1, but kept as
        //   a safety net.
        // - Read-only: value-typed alias `T name = _env->name;`.
        char** env_captures = NULL;
        int env_capture_count = 0;
        if (body && cap_count > 0) {
            env_captures = malloc(cap_count * sizeof(char*));
            for (int i = 0; i < cap_count; i++) {
                int is_promoted_for_parent = 0;
                for (int p = 0; p < parent_promoted_count; p++) {
                    if (parent_promoted[p] && strcmp(parent_promoted[p], captures[i]) == 0) {
                        is_promoted_for_parent = 1;
                        break;
                    }
                }
                if (is_assigned_to(body, captures[i]) && !is_promoted_for_parent) {
                    env_captures[env_capture_count++] = captures[i];
                }
            }
        }

        // Emit capture aliases.
        for (int i = 0; i < cap_count; i++) {
            int is_env_backed = 0;
            for (int j = 0; j < env_capture_count; j++) {
                if (env_captures[j] == captures[i]) { is_env_backed = 1; break; }
            }
            if (is_env_backed) continue;
            int is_promoted_for_parent = 0;
            for (int p = 0; p < parent_promoted_count; p++) {
                if (parent_promoted[p] && strcmp(parent_promoted[p], captures[i]) == 0) {
                    is_promoted_for_parent = 1;
                    break;
                }
            }
            const char* ctype = lookup_var_c_type(gen, captures[i], parent_func);
            Type* arr = capture_sized_array_type(gen, captures[i], parent_func);
            if (is_promoted_for_parent) {
                // Pointer alias: body reads/writes dereference through the
                // AST_IDENTIFIER emit path when the name is in
                // current_promoted_captures.
                const char* cell = promoted_cell_pointer(ctype, captures[i]);
                fprintf(gen->output, "    %s = _env->%s;\n", cell, captures[i]);
            } else if (arr) {
                // #2464: the env holds the array; the body indexes it in
                // place through an element pointer, as C passes an array.
                fprintf(gen->output, "    %s* %s = _env->%s;\n",
                        get_c_type(arr->element_type), captures[i], captures[i]);
            } else {
                fprintf(gen->output, "    %s %s = _env->%s;\n",
                        ctype, captures[i], captures[i]);
            }
        }

        if (body) {
            gen->indent_level = 1;
            // Closures called from trailing blocks need builder context injection
            // for _ctx: ptr functions. Set the flag so codegen injects _aether_ctx_get().
            gen->in_trailing_block++;
            // Save and reset the declared-vars set so closure body declarations
            // don't bleed into sibling closures or the outer function body.
            // We then register the promoted captures (they're "declared" via
            // the prologue alias) plus any closure params.
            char** prev_declared = gen->declared_vars;
            Type** prev_declared_types = gen->declared_var_types;
            int prev_declared_count = gen->declared_var_count;
            gen->declared_vars = NULL;
            gen->declared_var_types = NULL;
            gen->declared_var_count = 0;
            /* #2124: the closure body is a C function of its own; its
             * hoisted locals join over ITS bindings, not the enclosing
             * function's. */
            ASTNode* prev_hoist_scope = gen->hoist_scope_body;
            gen->hoist_scope_body = body;
            // Same reset for the heap-string-tracker set. Each closure
            // body is its own C function, so its `int _heap_<name>`
            // tracker declarations must be emitted afresh. Without
            // this reset the set leaks across sibling closures: two
            // closures that each declare a heap local of the same
            // name would emit `int _heap_<name>` in the first closure
            // and a bare `_heap_<name> = ...` (no declaration) in the
            // second, since `mark_heap_string_var` had already flagged
            // the name globally — a hard C-compile error
            // (`'_heap_<name>' undeclared`). See
            // new_string_len_something.md §2.
            char** prev_heap = gen->heap_string_vars;
            int prev_heap_count = gen->heap_string_var_count;
            gen->heap_string_vars = NULL;
            gen->heap_string_var_count = 0;
            /* The escape sets belong to the body too, as a function clears
             * them at its start (codegen_func.c). Closures are emitted after
             * every function, so they inherited the last one's: main's
             * escape walk reaches into the closures it holds, and a `return
             * s` in one closure made every string closure's return drain
             * `if (_heap_s) ...`, an undeclared `s` there. */
            char** prev_escaped = gen->escaped_string_vars;
            int prev_escaped_count = gen->escaped_string_var_count;
            char** prev_ret_escaped = gen->return_escaped_string_vars;
            int prev_ret_escaped_count = gen->return_escaped_string_var_count;
            char** prev_ret_escaped_struct = gen->return_escaped_struct_vars;
            int prev_ret_escaped_struct_count = gen->return_escaped_struct_var_count;
            gen->escaped_string_vars = NULL;
            gen->escaped_string_var_count = 0;
            gen->return_escaped_string_vars = NULL;
            gen->return_escaped_string_var_count = 0;
            char** prev_captured = gen->captured_string_params;
            int prev_captured_count = gen->captured_string_param_count;
            gen->captured_string_params = NULL;
            gen->captured_string_param_count = 0;
            gen->return_escaped_struct_vars = NULL;
            gen->return_escaped_struct_var_count = 0;
            /* Track the closure as the current function so
             * body-structural queries (current_fn_body_block /
             * body_assigns_var_from_heap) resolve a value identifier
             * against the closure's own body, not the enclosing
             * function's. */
            ASTNode* prev_current_function = gen->current_function;
            gen->current_function = closure;
            /* #2513: the closure's own variables, and through its captures
             * the enclosing scope's. */
            char own_var_scope[64];
            closure_scope_name(closure, own_var_scope, sizeof(own_var_scope));
            const char* prev_closure_var_scope = gen->closure_var_scope;
            gen->closure_var_scope = own_var_scope;
            // Publish env-backed captures so generate_statement routes writes
            // through _env-> instead of a local alias.
            char** prev_env = gen->current_env_captures;
            int prev_env_count = gen->current_env_capture_count;
            gen->current_env_captures = env_captures;
            gen->current_env_capture_count = env_capture_count;
            // Publish promoted names visible to this closure body so
            // reads/writes dereference through the pointer alias we just
            // emitted above.
            //
            // Two sources, and they behave differently:
            //  - the PARENT scope's promoted names: they arrive as `T*` env
            //    slots with a `T* name = _env->name;` prologue alias, and are
            //    pre-marked declared below. One this closure's own parameter
            //    shadows is not the parent's variable here, and is skipped;
            //  - this closure's OWN promoted names (recorded against its
            //    `__closure_<ptr>` scope because a closure nested inside it
            //    writes one of its locals or parameters): they go into the
            //    promoted set, so reads and writes dereference, and get no
            //    prologue alias. A local's cell is minted by the ordinary
            //    declaration path; a parameter's right after enter_scope
            //    below (#2463).
            char** own_promoted = NULL;
            int own_promoted_count = 0;
            get_promoted_names_for_func(gen, own_scope, &own_promoted, &own_promoted_count);
            char** body_promoted = NULL;
            int body_promoted_count = 0;
            if (parent_promoted_count + own_promoted_count > 0) {
                body_promoted = malloc((parent_promoted_count + own_promoted_count) * sizeof(char*));
                for (int p = 0; p < parent_promoted_count; p++) {
                    if (!parent_promoted[p]) continue;
                    if (is_closure_param(closure, parent_promoted[p])) continue;
                    // A parent-promoted name is a promoted capture of THIS
                    // closure only if the closure actually captures it — i.e.
                    // it has the `T* name = _env->name;` prologue alias emitted
                    // above. A parent-promoted name the closure does NOT capture
                    // is either unused here or SHADOWED by a same-named local of
                    // this closure's own body; inheriting it would (a) put a
                    // dereferencing `*name` promoted-write on that own local and
                    // (b) mark it pre-declared, so the local is never minted and
                    // the emitted C references an undeclared name. Exclude it so
                    // the own local declares normally.
                    int captured = 0;
                    for (int c = 0; c < cap_count; c++) {
                        if (captures[c] && strcmp(captures[c], parent_promoted[p]) == 0) {
                            captured = 1; break;
                        }
                    }
                    if (!captured) continue;
                    body_promoted[body_promoted_count++] = parent_promoted[p];
                }
                for (int p = 0; p < own_promoted_count; p++) {
                    if (!own_promoted[p]) continue;
                    int dup = 0;
                    for (int q = 0; q < body_promoted_count; q++) {
                        if (strcmp(body_promoted[q], own_promoted[p]) == 0) { dup = 1; break; }
                    }
                    if (!dup) body_promoted[body_promoted_count++] = own_promoted[p];
                }
            }
            char** prev_promoted = gen->current_promoted_captures;
            int prev_promoted_count = gen->current_promoted_capture_count;
            gen->current_promoted_captures = body_promoted;
            gen->current_promoted_capture_count = body_promoted_count;
            // Mark promoted captures as already-declared in this local scope
            // so writes in the body hit the reassignment branch (emits
            // *name = ...) rather than trying to declare+malloc again.
            // The prologue alias `T* name = _env->name;` is the declaration —
            // so only names this closure actually captures are pre-declared.
            // A parent-promoted name the closure does NOT capture has no alias
            // and may be shadowed by a same-named own local, which must declare
            // normally (see the body_promoted filter above).
            for (int p = 0; p < parent_promoted_count; p++) {
                if (!parent_promoted[p]) continue;
                int captured = 0;
                for (int c = 0; c < cap_count; c++) {
                    if (captures[c] && strcmp(captures[c], parent_promoted[p]) == 0) {
                        captured = 1; break;
                    }
                }
                if (!captured) continue;
                /* #2474: an array cell records its type, for a whole-array
                 * store in the body (emit_cell_array_store). */
                Type* arr = capture_sized_array_type(gen, parent_promoted[p], parent_func);
                if (arr) mark_var_declared_typed(gen, parent_promoted[p], arr);
                else mark_var_declared(gen, parent_promoted[p]);
            }
            /* #2462: the closure's parameters are declared by its C
             * signature, the way a function's are (codegen_func.c), so an
             * assignment to one in the body is a reassignment rather than a
             * redeclaration, and the heap-string hoist skips them. */
            for (int i = 0; i < closure->child_count; i++) {
                ASTNode* p = closure->children[i];
                if (!p || p->type != AST_CLOSURE_PARAM || !p->value) continue;
                /* #2516: an array parameter records its type. */
                if (is_sized_array_param(p->node_type))
                    mark_var_declared_typed(gen, p->value, p->node_type);
                else
                    mark_var_declared(gen, p->value);
            }
            /* A closure body is its own C function — it needs the same
             * heap-string lifecycle as a top-level function, or heap
             * locals it mints (e.g. `trimmed = string.trim(out)`) leak
             * when the closure returns. Hoist the `_heap_<name>` trackers,
             * mark return/store escapes, and push the scope-exit defer-
             * frees; exit_scope below emits them (and the per-return
             * emit_all_defers handles explicit returns). Balanced
             * enter/exit_scope keeps the defer stack closure-local. */
            enter_scope(gen);
            /* #2463: the cell for each parameter a nested closure writes,
             * seeded from `_param_<name>` (emit_closure_signature). After
             * enter_scope, so its release lands in this closure's scope. */
            for (int i = 0; i < closure->child_count; i++) {
                ASTNode* p = closure->children[i];
                if (!p || p->type != AST_CLOSURE_PARAM || !p->value) continue;
                if (!closure_param_is_promoted(gen, closure, p->value)) {
                    /* #2516: an array parameter's own copy. */
                    if (is_sized_array_param(p->node_type)) {
                        print_indent(gen);
                        emit_sized_array_param_copy(gen, p->node_type, p->value);
                    }
                    continue;
                }
                const char* param_cname = cg_internf("_param_%s", p->value);
                print_indent(gen);
                emit_promoted_param_cell(gen, p->value,
                                         p->node_type ? get_c_type(p->node_type) : "int",
                                         param_cname, p->line, p->column);
            }
            /* #2499 copy-on-keep: a `string` parameter the closure keeps
             * past the call becomes a reference of its own (a refcounted
             * string retained, a plain buffer copied), so its caller may
             * free the one it passed. A promoted one already has that in
             * its cell. */
            for (int i = 0, pi = 0; i < closure->child_count; i++) {
                ASTNode* p = closure->children[i];
                if (!p || p->type != AST_CLOSURE_PARAM) continue;
                int idx = pi++;
                if (!p->value || !p->node_type || p->node_type->kind != TYPE_STRING ||
                    closure_param_is_promoted(gen, closure, p->value) ||
                    !closure_string_param_kept(gen, closure, idx)) continue;
                print_indent(gen);
                fprintf(gen->output, "%s = aether_str_capture(%s);\n",
                        safe_value_name(p->value), safe_value_name(p->value));
                /* That reference is the closure's to give back: the
                 * parameter is a heap-tracked string from here on, so a
                 * keep that is only an alias (`nm = s`) or a store moves it
                 * as a local's would, a return hands it to the caller, and
                 * a parameter still holding it at exit is freed. Untracked,
                 * every keep that did not hand it over leaked it. The
                 * tracker is spelled with the raw name, as every heap
                 * local's is, so a renamed one stays untracked. */
                if (strcmp(safe_value_name(p->value), p->value) == 0 &&
                    !is_heap_string_var(gen, p->value)) {
                    print_indent(gen);
                    fprintf(gen->output, "int _heap_%s = 1; (void)_heap_%s;\n",
                            p->value, p->value);
                    mark_heap_string_var(gen, p->value);
                    mark_captured_string_param(gen, p->value);
                }
            }
            hoist_heap_string_trackers(gen, body);
            mark_escaped_heap_string_vars(gen, body);
            push_heap_string_exit_free_defers(gen, body);
            for (int i = 0; i < body->child_count; i++) {
                generate_statement(gen, body->children[i]);
            }
            exit_scope(gen);
            gen->current_env_captures = prev_env;
            gen->current_env_capture_count = prev_env_count;
            gen->current_promoted_captures = prev_promoted;
            gen->current_promoted_capture_count = prev_promoted_count;
            free(body_promoted);
            // Free the body's declared_vars and restore the outer scope's set.
            clear_declared_vars(gen);
            gen->declared_vars = prev_declared;
            gen->declared_var_types = prev_declared_types;
            gen->declared_var_count = prev_declared_count;
            gen->hoist_scope_body = prev_hoist_scope;
            // Free this closure body's heap-string set and restore the
            // enclosing scope's (see the matching reset above).
            clear_heap_string_vars(gen);
    clear_seq_vars(gen);
            gen->heap_string_vars = prev_heap;
            gen->heap_string_var_count = prev_heap_count;
            clear_escaped_string_vars(gen);
            gen->escaped_string_vars = prev_escaped;
            gen->escaped_string_var_count = prev_escaped_count;
            clear_captured_string_params(gen);
            gen->captured_string_params = prev_captured;
            gen->captured_string_param_count = prev_captured_count;
            gen->return_escaped_string_vars = prev_ret_escaped;
            gen->return_escaped_string_var_count = prev_ret_escaped_count;
            gen->return_escaped_struct_vars = prev_ret_escaped_struct;
            gen->return_escaped_struct_var_count = prev_ret_escaped_struct_count;
            gen->current_function = prev_current_function;
            gen->closure_var_scope = prev_closure_var_scope;
            gen->in_trailing_block--;
            gen->indent_level = 0;
        }
        gen->in_string_closure = 0;
        gen->current_func_return_type = NULL;
        if (closure_ret_tuple) free_type(closure_ret_tuple);

        free(env_captures);

        fprintf(gen->output, "}\n\n");

        // Emit MSVC-compatible closure constructor function (avoids statement expressions)
        if (cap_count > 0) {
            fprintf(gen->output, "#if !AETHER_GCC_COMPAT\n");
            fprintf(gen->output, "static _AeClosure _aether_make_closure_%d(", id);
            emit_make_closure_params(gen, ci);
            fprintf(gen->output, ") {\n");
            fprintf(gen->output, "    _closure_env_%d* _e = malloc(sizeof(_closure_env_%d));\n", id, id);
            fprintf(gen->output, "    _e->_dtor = _closure_env_%d_free;\n", id);
            fprintf(gen->output, "    atomic_init(&_e->_refs, 1);\n");
            for (int i = 0; i < cap_count; i++) {
                if (capture_is_closure_value(gen, captures[i], parent_func)) {
                    /* env owns a reference to the captured env (#2494) */
                    fprintf(gen->output, "    _e->%s = %s; _aether_closure_env_retain(%s.env);\n",
                            captures[i], captures[i], captures[i]);
                } else if (capture_owning_struct(gen, captures[i], parent_func)) {
                    /* env owns a copy of the struct's strings (#2504) */
                    fprintf(gen->output, "    _e->%s = %s_dup(%s);\n", captures[i],
                            capture_owning_struct(gen, captures[i], parent_func), captures[i]);
                } else if (capture_is_promoted(gen, captures[i], parent_func)) {
                    /* env owns a reference to the shared cell (#2019) */
                    const char* cell = promoted_cell_pointer(lookup_var_c_type(gen, captures[i], parent_func),
                                                             NULL);
                    fprintf(gen->output, "    _e->%s = (%s)_aether_cell_retain(%s);\n",
                            captures[i], cell, captures[i]);
                } else if (capture_is_retained_string(gen, captures[i], parent_func)) {
                    /* env owns a reference — see aether_str_capture preamble */
                    const char* ctype = lookup_var_c_type(gen, captures[i], parent_func);
                    fprintf(gen->output, "    _e->%s = (%s)aether_str_capture(%s);\n",
                            captures[i], ctype, captures[i]);
                } else if (capture_sized_array_type(gen, captures[i], parent_func)) {
                    /* #2464: an array does not assign; copy its bytes. */
                    fprintf(gen->output, "    memcpy(_e->%s, %s, sizeof(_e->%s));\n",
                            captures[i], captures[i], captures[i]);
                } else {
                    fprintf(gen->output, "    _e->%s = %s;\n", captures[i], captures[i]);
                }
            }
            fprintf(gen->output, "    _AeClosure _c = { (void(*)(void))_closure_fn_%d, _e };\n", id);
            fprintf(gen->output, "    return _c;\n");
            fprintf(gen->output, "}\n");
            fprintf(gen->output, "#endif\n\n");
        }
    }

    gen->current_func_return_type = saved_ret;
}

// Look up a message field definition by name. Returns NULL if missing.
MessageFieldDef* find_msg_field(MessageDef* msg_def, const char* name) {
    if (!msg_def || !name) return NULL;
    MessageFieldDef* f = msg_def->fields;
    while (f) {
        if (f->name && strcmp(f->name, name) == 0) return f;
        f = f->next;
    }
    return NULL;
}

// Emit a message field initializer RHS.
//
// Array-literal RHS assigned to an array-typed field needs special
// handling: a compound literal `(T[]){...}` has block-scoped lifetime,
// which dies when the enclosing send-expression block exits. Messages
// are queued for later processing, so the receiver would dereference
// freed memory. Instead, we hoist the array to a `static` local
// variable allocated before the struct init — static storage has
// program lifetime and the send can safely copy the pointer.
//
// The hoist is driven by `emit_message_array_hoists`, which pre-walks
// the field inits and writes one `static const T _aether_arr_N[] = {...};`
// declaration per array field at the start of the send-expression block.
// `emit_message_field_init` then emits the corresponding `_aether_arr_N`
// name instead of the compound literal.
//
// For any non-array or non-literal cases, this behaves exactly like
// `generate_expression`.
//
// The msg_arr_id_for_field map is stored on the gen state as a sparse
// per-send table (reset via `reset_msg_arr_map`).

#define MAX_MSG_ARR_FIELDS 16
static int msg_arr_ids[MAX_MSG_ARR_FIELDS];
static const char* msg_arr_field_names[MAX_MSG_ARR_FIELDS];
static int msg_arr_count = 0;

static void reset_msg_arr_map(void) {
    msg_arr_count = 0;
}

static int lookup_msg_arr_id(const char* field_name) {
    for (int i = 0; i < msg_arr_count; i++) {
        if (msg_arr_field_names[i] && strcmp(msg_arr_field_names[i], field_name) == 0) {
            return msg_arr_ids[i];
        }
    }
    return -1;
}

// Pre-walk: for each AST_FIELD_INIT in the message constructor whose RHS
// is an AST_ARRAY_LITERAL and whose target field is a composite-type
// message field (has element_c_type), emit a static local declaration
// and record the hoisted variable ID. Call this after opening the
// send-expression block, before emitting the `Msg _msg = {...}` line.
void emit_message_array_hoists(CodeGenerator* gen, ASTNode* message, MessageDef* msg_def) {
    reset_msg_arr_map();
    if (!message || !msg_def) return;

    for (int i = 0; i < message->child_count; i++) {
        ASTNode* field_init = message->children[i];
        if (!field_init || field_init->type != AST_FIELD_INIT || field_init->child_count == 0) {
            continue;
        }
        ASTNode* rhs = field_init->children[0];
        if (!rhs || rhs->type != AST_ARRAY_LITERAL) continue;

        MessageFieldDef* fdef = find_msg_field(msg_def, field_init->value);
        if (!fdef || !fdef->element_c_type) continue;

        // Hoist to a static local. Static storage class gives program
        // lifetime, so the receiver can safely read through the pointer.
        int id = gen->msg_arr_counter++;
        fprintf(gen->output, "static %s _aether_arr_%d[] = {", fdef->element_c_type, id);
        for (int j = 0; j < rhs->child_count; j++) {
            if (j > 0) fprintf(gen->output, ", ");
            generate_expression(gen, rhs->children[j]);
        }
        fprintf(gen->output, "}; ");

        if (msg_arr_count < MAX_MSG_ARR_FIELDS) {
            msg_arr_ids[msg_arr_count] = id;
            msg_arr_field_names[msg_arr_count] = field_init->value;
            msg_arr_count++;
        }
    }
}

/* The deep copy a string crossing an actor boundary gets (#466), into the
 * slot `lv` (`_msg.text`, `_reply_val`). `init` is the expression the slot
 * was filled from: when it is an owned temporary (a call or interpolation
 * result held by nothing else), the temporary is freed once it is copied.
 * Nothing else frees it, so every such send leaked one string. */
void emit_message_string_copy(CodeGenerator* gen, const char* lv, ASTNode* init) {
    int owned_temp = init &&
        (init->type == AST_FUNCTION_CALL || init->type == AST_STRING_INTERP) &&
        is_heap_string_expr(gen, init);
    fprintf(gen->output,
            "if (%s) { const char* _mt = %s; size_t _ml = aether_string_length(_mt); "
            "%s = (const char*)string_new_with_length(aether_string_data(_mt), (int)_ml); ",
            lv, lv, lv);
    if (owned_temp) fprintf(gen->output, "aether_heap_str_free(_mt); ");
    fprintf(gen->output, "} ");
}

/* The expression a message constructor gives field `name`, or NULL. */
ASTNode* message_field_init_expr(ASTNode* message, const char* name) {
    for (int i = 0; message && i < message->child_count; i++) {
        ASTNode* fi = message->children[i];
        if (fi && fi->type == AST_FIELD_INIT && fi->value && strcmp(fi->value, name) == 0) {
            return fi->child_count > 0 ? fi->children[0] : NULL;
        }
    }
    return NULL;
}

/* The C symbol a call to `func_name` is emitted as: the whole name, from
 * the extern table, the source or the intern table (#2539: a 256-byte copy
 * cut a longer one, so the call named a function nothing defined). */
const char* call_c_name(CodeGenerator* gen, const char* func_name) {
    // Don't mangle extern functions: they refer to real C symbols.
    // For @extern("c_symbol") aether_name(...), translate the
    // Aether-side name to its bound C symbol. See #234.
    const char* mangled = is_extern_func(gen, func_name)
        ? lookup_extern_c_name(gen, func_name)
        : safe_c_name(func_name);
    const char* out = codegen_normalise_callee(mangled);
    /* #1383: substituting '_' for the dot assumes the C symbol is
       `<module>_<name>`. It is not when the export already carries
       the module (`intarr.intarr_new_raw`) or carries none
       (`os.aether_args_count`). Prefer the declared extern. */
    const char* qdot = strchr(func_name, '.');
    /* `<module>_<name>` may be an Aether wrapper rather than an
       extern; redirecting past it would call the raw extern and
       skip the wrapper's ownership handling. */
    int qknown = is_extern_func(gen, out);
    if (!qknown && gen->program &&
        find_function_definition_by_name(gen->program, out)) {
        qknown = 1;
    }
    if (qdot && qdot[1] && !qknown) {
        const char* after = qdot + 1;
        if (is_extern_func(gen, after)) {
            const char* real = lookup_extern_c_name(gen, after);
            if (real) out = real;
        }
    }
    return out;
}

/* #2518: the container entry a call that stores a closure VALUE is lowered
 * to (closure_container_store_value): the owning add, set or put, whose
 * wrapper form returns the `"" | error` string. The value is at child
 * `val_idx`. */
typedef struct {
    const char* c_name;
    int val_idx;
    const char* owned;      /* the raw owning entry (returns int, or nothing) */
    const char* wrapper;    /* the string-returning form, NULL when `c_name` is raw */
} ClosureStoreEntry;

static const ClosureStoreEntry g_closure_store_entries[] = {
    { "list_add_raw",          1, "list_add_closure_owned", NULL },
    { "list_add",              1, "list_add_closure_owned", "_aether_list_add_closure" },
    { "list_add_string_owned", 1, "list_add_closure_owned", NULL },
    { "list_set",              2, "list_set_closure_owned", NULL },
    { "map_put_raw",           2, "map_put_closure_owned",  NULL },
    { "map_put",               2, "map_put_closure_owned",  "_aether_map_put_closure" },
    { "map_put_string_owned",  2, "map_put_closure_owned",  NULL },
};

static const ClosureStoreEntry* closure_store_entry(CodeGenerator* gen, ASTNode* call) {
    if (!gen || !call || call->type != AST_FUNCTION_CALL || !call->value) return NULL;
    const char* c_name = call_c_name(gen, call->value);
    for (size_t i = 0; i < sizeof(g_closure_store_entries) / sizeof(g_closure_store_entries[0]); i++) {
        const ClosureStoreEntry* e = &g_closure_store_entries[i];
        if (strcmp(c_name, e->c_name) == 0 && call->child_count == e->val_idx + 1) return e;
    }
    return NULL;
}

/* #2518: is `call` a list add or set, or a map put, that stores a closure
 * VALUE (`fn`-typed, not a raw fn-ptr)? Then the container takes a
 * reference of its own to the closure's env and releases it when the
 * element goes, so storing it there is no hand-off of the caller's
 * reference. Returns the stored value, else NULL. The escape walks
 * (env_scan, param_escapes_in_subtree) and the drains ask this same
 * question, so they agree with what emit_closure_container_store emits. */
ASTNode* closure_container_store_value(CodeGenerator* gen, ASTNode* call) {
    const ClosureStoreEntry* e = closure_store_entry(gen, call);
    if (!e) return NULL;
    ASTNode* val = call->children[e->val_idx];
    /* `list.add(l, box_closure(f))` stores f as `list.add(l, f)` does: the
     * explicit box is the one the coercion would make. Stored as a raw
     * pointer, neither the box nor the env was ever reclaimed. */
    if (val && val->type == AST_FUNCTION_CALL && val->value &&
        strcmp(val->value, "box_closure") == 0 && val->child_count == 1) {
        val = val->children[0];
    }
    if (!val || !val->node_type || val->node_type->kind != TYPE_FUNCTION ||
        val->node_type->is_fnptr) return NULL;
    return val;
}

/* The child index of the value a list add, list set or map put stores
 * (closure_store_entry's shapes), or -1: the slot the owning rewrite takes,
 * which the keep walks count as a tracked keep, not an opaque sink. */
int container_store_slot(CodeGenerator* gen, ASTNode* call) {
    const ClosureStoreEntry* e = closure_store_entry(gen, call);
    return e ? e->val_idx : -1;
}

/* The string local a list add or a map put stores, when the store takes it
 * as every other owning slot does (emit_string_take): moved on its last
 * use, copied otherwise, so the local keeps whatever it did not hand over
 * and the container holds a reference of its own. Only a local some
 * binding of the body may leave owning a heap value, the take reading its
 * tracker (one that only ever holds a literal stays on the raw path, as
 * before), or a `string` parameter of
 * the closure being emitted that took a reference of its own on entry
 * (copy-on-keep, #2499), which is a heap-tracked local from there on.
 * Returns the value node, else NULL. The escape walk asks this same
 * question, so the local's own frees stay in place. Adopting the single
 * reference and marking the local escaped freed it twice when it was
 * stored twice, or stored in a loop, and left a kept parameter's
 * reference to nobody. */
static ASTNode* current_fn_body_block(CodeGenerator* gen);
ASTNode* string_container_store_value(CodeGenerator* gen, ASTNode* call) {
    const ClosureStoreEntry* e = closure_store_entry(gen, call);
    if (!e) return NULL;
    ASTNode* val = call->children[e->val_idx];
    if (!val || val->type != AST_IDENTIFIER || !val->value ||
        !is_heap_string_var(gen, val->value)) return NULL;
    if (body_may_assign_var_from_heap(gen, current_fn_body_block(gen), val->value)) return val;
    /* A `string` parameter the closure or function keeps took its own
     * reference on entry (copy-on-keep), so it is heap-tracked here. */
    ASTNode* fn = gen->current_function;
    if (fn && fn->type == AST_CLOSURE && is_closure_param(fn, val->value)) return val;
    for (int i = 0; fn && fn->type != AST_CLOSURE && i < fn->child_count; i++) {
        ASTNode* c = fn->children[i];
        if (c && (c->type == AST_PATTERN_VARIABLE || c->type == AST_VARIABLE_DECLARATION) &&
            c->value && strcmp(c->value, val->value) == 0) return val;
    }
    return NULL;
}

/* Emit the store of closure value `val` (closure_container_store_value) by
 * `call`. The value is heap-boxed (the fn -> ptr coercion) and the
 * container owns the box and a reference of its own to the env (#2518). A
 * closure literal or a handed-over one (#2506) has no reference but the
 * one it arrives with; the container takes its own, so that one is
 * released after the store. In a statement the drained-call form already
 * holds it in a temporary, which is released by the drain. */
static void emit_closure_container_store(CodeGenerator* gen, ASTNode* call, ASTNode* val,
                                         int discarded) {
    const ClosureStoreEntry* e = closure_store_entry(gen, call);
    const char* fn = e->wrapper ? e->wrapper : e->owned;
    int temp_val = !arg_drain_lookup(val) &&
        ((val->type == AST_CLOSURE && !(val->value && strcmp(val->value, "trailing") == 0)) ||
         call_returns_owned_closure(gen, val));
    if (temp_val) {
        fprintf(gen->output, "({ _AeClosure _ae_lv = ");
        generate_expression(gen, val);
        fprintf(gen->output, "; %s _ae_lr = ", e->wrapper ? "const char*" : "int");
    }
    fprintf(gen->output, "%s(", fn);
    for (int i = 0; i < e->val_idx; i++) {
        if (i) fprintf(gen->output, ", ");
        if (i == 1 && strcmp(e->owned, "map_put_closure_owned") == 0) {
            /* The key as it is, an AetherString with its length or a plain
             * char*: the map reads either shape (#2469). */
            fprintf(gen->output, "(const char*)(");
            generate_expression(gen, call->children[i]);
            fprintf(gen->output, ")");
        } else {
            generate_expression(gen, call->children[i]);
        }
    }
    fprintf(gen->output, ", (void*)_aether_box_closure(");
    if (temp_val) {
        /* A store whose result nobody reads (a statement) yields nothing:
         * a trailing value there is an unused expression to the C
         * compiler (clang -Wunused-value). */
        fprintf(gen->output, "_ae_lv)); _aether_closure_env_release(_ae_lv.env); %s })",
                discarded ? "(void)_ae_lr;" : "_ae_lr;");
    } else {
        generate_expression(gen, val);
        fprintf(gen->output, "))");
    }
}

void emit_message_field_init(CodeGenerator* gen, MessageFieldDef* fdef, ASTNode* rhs) {
    // If this field was hoisted by emit_message_array_hoists, emit the
    // hoisted variable name instead of inlining the compound literal.
    // (Requires the pre-walk to have populated the map for this field.)
    if (rhs && rhs->type == AST_ARRAY_LITERAL && fdef && fdef->element_c_type) {
        /* #1286: a `T[]` message field is a slice over the hoisted array. */
        int as_slice = fdef->c_type && strcmp(fdef->c_type, "AetherSlice") == 0;
        int id = lookup_msg_arr_id(fdef->name);
        if (id >= 0) {
            if (as_slice)
                fprintf(gen->output, "aether_slice_make(_aether_arr_%d, %d)", id, rhs->child_count);
            else
                fprintf(gen->output, "_aether_arr_%d", id);
            return;
        }
        // Fall-through: no hoist set up (e.g. reply statement). Use a
        // compound literal — still wrong for cross-thread sends, but
        // fine for synchronous ask/reply where the sender stays alive.
        if (as_slice) {
            fprintf(gen->output, "aether_slice_make((%s[])", fdef->element_c_type);
            generate_expression(gen, rhs);
            fprintf(gen->output, ", %d)", rhs->child_count);
            return;
        }
        fprintf(gen->output, "(%s[])", fdef->element_c_type);
    }
    /* Cons-cell context: when the target field is `*StringSeq` and the
     * RHS is an array literal, stamp the literal's node_type so the
     * AST_ARRAY_LITERAL codegen case takes the cons-chain branch
     * rather than the static-array one. Same disambiguation rule as
     * variable declarations — keeps the user-visible syntax consistent
     * across all literal-target contexts. See typechecker.c
     * (AST_VARIABLE_DECLARATION) and codegen_expr.c
     * (AST_ARRAY_LITERAL) for the matching code paths. */
    if (rhs && rhs->type == AST_ARRAY_LITERAL && fdef && fdef->c_type &&
        strcmp(fdef->c_type, "StringSeq*") == 0) {
        if (rhs->node_type) free_type(rhs->node_type);
        rhs->node_type = make_string_seq_ptr_type();
    }
    /* #2525: a closure field holds a reference of its own, released by
     * `<Msg>_release_fields` once the handler is done: a fresh closure's
     * is adopted, a local's or a field's retained. */
    if (rhs && rhs->node_type && rhs->node_type->kind == TYPE_FUNCTION &&
        !rhs->node_type->is_fnptr && fdef && fdef->c_type &&
        strcmp(fdef->c_type, "_AeClosure") == 0) {
        emit_closure_take(gen, rhs);
        return;
    }
    generate_expression(gen, rhs);
}

// Emit a send target expression with the correct C cast.
// Actor refs produce (ActorBase*)(expr) directly.
// Int/int64 values (actor refs stored in int message fields or state) need
// (ActorBase*)(intptr_t)(expr) to avoid pointer-width conversion warnings.
static void emit_send_target(CodeGenerator* gen, ASTNode* target, const char* cast_type) {
    int needs_intptr = target->node_type &&
        (target->node_type->kind == TYPE_INT || target->node_type->kind == TYPE_INT64);
    fprintf(gen->output, "(%s)(", cast_type);
    if (needs_intptr) fprintf(gen->output, "(intptr_t)");
    generate_expression(gen, target);
    fprintf(gen->output, ")");
}

/* The AST_BLOCK body of the function / `main` / closure currently
 * being generated, or NULL. `gen->current_function` is the enclosing
 * AST_FUNCTION_DEFINITION / AST_MAIN_FUNCTION / AST_CLOSURE; the body
 * is its AST_BLOCK child. Used by the map/list owned-value routing to
 * resolve a bare identifier's heap-ness structurally. */
static ASTNode* current_fn_body_block(CodeGenerator* gen) {
    if (!gen || !gen->current_function) return NULL;
    ASTNode* fn = gen->current_function;
    for (int i = fn->child_count - 1; i >= 0; i--) {
        if (fn->children[i] && fn->children[i]->type == AST_BLOCK) {
            return fn->children[i];
        }
    }
    return NULL;
}

/* Length of the valid UTF-8 sequence starting at `s`, or 1 if the
 * bytes do not form one (lone continuation byte, truncated sequence,
 * overlong-agnostic on purpose: structural validity is enough here).
 * Used by the string-literal emitter to keep human text readable in
 * generated C while byte-escaping everything that is not valid text. */
static int utf8_sequence_length(const char* s) {
    unsigned char c0 = (unsigned char)s[0];
    int len;
    if ((c0 & 0xE0) == 0xC0) len = 2;
    else if ((c0 & 0xF0) == 0xE0) len = 3;
    else if ((c0 & 0xF8) == 0xF0) len = 4;
    else return 1;
    for (int i = 1; i < len; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) return 1;
    }
    return len;
}

/* The bytes of `str` as the inside of a C string literal, every byte C
 * cannot carry raw escaped. With `printf_format` a '%' is doubled, for the
 * format string of an interpolation (printf / _aether_interp). The text of
 * an interpolation is the same decoded bytes as a plain literal's, so it is
 * spelled the same way here: it used to be read again as escapes, so an
 * escaped backslash before `0`, `n` or `x` became a NUL, a newline or a
 * byte (#2512). */
void emit_c_string_body(CodeGenerator* gen, const char* str, int printf_format) {
    emit_c_string_bytes(gen, str, strlen(str), printf_format);
}

/* As emit_c_string_body, for `len` bytes that may include a NUL (#2520). In
 * a printf format a NUL cannot be carried (it would end the format), so it
 * is written as `%c`, and the caller passes a 0 argument for each one
 * (emit_text_nul_args); in a C literal it is the octal `\000`. */
void emit_c_string_bytes(CodeGenerator* gen, const char* str, size_t len, int printf_format) {
    const char* end = str + len;
    while (str < end) {
        unsigned char ch = (unsigned char)*str;
        switch (*str) {
            case '\0': fprintf(gen->output, printf_format ? "%%c" : "\\000"); break;
            case '\n': fprintf(gen->output, "\\n"); break;
            case '\t': fprintf(gen->output, "\\t"); break;
            case '\r': fprintf(gen->output, "\\r"); break;
            case '\\': fprintf(gen->output, "\\\\"); break;
            case '"': fprintf(gen->output, "\\\""); break;
            case '%': fprintf(gen->output, printf_format ? "%%%%" : "%%"); break;
            default:
                if (ch < 0x20 || ch == 0x7F) {
                    /* Zero-padded OCTAL, never \x: a C hex escape has no
                     * length limit, so "\x01a" re-lexes as byte 0x1A
                     * (silent corruption when the next char is a hex
                     * digit). \001 is exactly three digits and cannot
                     * munch. */
                    fprintf(gen->output, "\\%03o", ch);
                } else if (ch >= 0x80) {
                    /* Bytes above ASCII: emit a VALID UTF-8 sequence raw so
                     * human text stays readable in the generated C; escape
                     * anything else (decoded \x binary, e.g. CBOR/MsgPack
                     * test vectors) so the output stays valid text and
                     * every downstream tool (grep/awk/editors) treats it
                     * uniformly on all platforms. */
                    int seq = utf8_sequence_length(str);
                    if (seq > 1) {
                        for (int b = 0; b < seq; b++) {
                            fprintf(gen->output, "%c", str[b]);
                        }
                        str += seq - 1;
                    } else {
                        fprintf(gen->output, "\\%03o", ch);
                    }
                } else {
                    fprintf(gen->output, "%c", *str);
                }
                break;
        }
        str++;
    }
}

/* Emit `s` as a C string literal: quoted, with every byte C cannot carry
 * raw escaped. The one spelling of an Aether string literal in the C, so a
 * literal reads the same wherever codegen writes one (an expression, a
 * function-clause pattern, a guard). */
void emit_c_string_literal(CodeGenerator* gen, const char* str) {
    fprintf(gen->output, "\"");
    emit_c_string_body(gen, str, 0);
    fprintf(gen->output, "\"");
}

/* #2520: the index of the static AetherString carrying `bytes`, registering
 * it on first sight. The registry is filled by the pre-pass of
 * emit_static_string_literals before any program text is emitted, so a use
 * always finds its entry; a literal first seen after that pass would name
 * an object the C compiler has not seen, which fails the build rather than
 * cutting the literal. */
int static_string_literal_index(CodeGenerator* gen, const char* bytes, int len) {
    for (int i = 0; i < gen->static_str_count; i++) {
        if (gen->static_str_lens[i] == len &&
            memcmp(gen->static_str_bytes[i], bytes, (size_t)len) == 0) return i;
    }
    if (gen->static_str_count >= gen->static_str_capacity) {
        gen->static_str_capacity = gen->static_str_capacity ? gen->static_str_capacity * 2 : 8;
        gen->static_str_bytes = aether_xrealloc(gen->static_str_bytes,
                                                gen->static_str_capacity * sizeof(char*));
        gen->static_str_lens = aether_xrealloc(gen->static_str_lens,
                                               gen->static_str_capacity * sizeof(int));
    }
    char* copy = malloc((size_t)len + 1);
    if (!copy) {
        fprintf(stderr, "Fatal: out of memory registering a string literal\n");
        exit(1);
    }
    memcpy(copy, bytes, (size_t)len);
    copy[len] = '\0';
    gen->static_str_bytes[gen->static_str_count] = copy;
    gen->static_str_lens[gen->static_str_count] = len;
    return gen->static_str_count++;
}

static void collect_static_string_literals(CodeGenerator* gen, ASTNode* node) {
    if (!node) return;
    if (node->value_len > 0 &&
        (node->type == AST_LITERAL || node->type == AST_PATTERN_LITERAL) &&
        node->node_type && node->node_type->kind == TYPE_STRING) {
        static_string_literal_index(gen, node->value, node->value_len);
    }
    for (int i = 0; i < node->child_count; i++) {
        collect_static_string_literals(gen, node->children[i]);
    }
}

/* #2520: a string literal that holds a NUL cannot be a C string literal,
 * which ends at its first NUL. Each such literal of the program is emitted
 * here, once, as a static AetherString with its length, in the layout of
 * std/string/aether_string.h, pinned (AETHER_STRING_PINNED_REFS) so that
 * string_retain and string_release leave it alone and it is never freed.
 * Every string function then sees all of its bytes through str_len and
 * str_data, as it does for a heap string. A literal without a NUL is not
 * here: it stays a plain C string literal. */
void emit_static_string_literals(CodeGenerator* gen, ASTNode* program) {
    collect_static_string_literals(gen, program);
    if (gen->static_str_count == 0) return;
    fprintf(gen->output, "/* String literals holding a NUL: static, pinned AetherStrings (#2520). */\n");
    /* The runtime declares AetherString from the same field list, so the
     * layout written here is the layout the string functions read. */
#define AE_STR_ABI_TEXT_(...) #__VA_ARGS__
#define AE_STR_ABI_TEXT(...) AE_STR_ABI_TEXT_(__VA_ARGS__)
    fprintf(gen->output, "typedef struct { %s } _AeStaticStr;\n",
            AE_STR_ABI_TEXT(AETHER_STRING_FIELDS));
#undef AE_STR_ABI_TEXT
#undef AE_STR_ABI_TEXT_
    for (int i = 0; i < gen->static_str_count; i++) {
        int len = gen->static_str_lens[i];
        fprintf(gen->output, "static const char _ae_slit_%d_bytes[] = \"", i);
        emit_c_string_bytes(gen, gen->static_str_bytes[i], (size_t)len, 0);
        fprintf(gen->output, "\";\n");
        fprintf(gen->output, "static _AeStaticStr _ae_slit_%d = { 0x%Xu, 0x%X, %d, %d, (char*)_ae_slit_%d_bytes };\n",
                i, (unsigned)AETHER_STRING_MAGIC, (unsigned)AETHER_STRING_PINNED_REFS,
                len, len + 1, i);
    }
}

void emit_string_literal_node(CodeGenerator* gen, const ASTNode* lit) {
    if (lit->value_len > 0) {
        int idx = static_string_literal_index(gen, lit->value, lit->value_len);
        fprintf(gen->output, "((const char*)&_ae_slit_%d)", idx);
    } else {
        emit_c_string_literal(gen, lit->value);
    }
}

void emit_print_literal_format(CodeGenerator* gen, const ASTNode* lit) {
    /* A print with only a literal format has no conversions (the type
     * checker refuses one without an argument), so its text is known here:
     * `%%` is a percent sign and every other byte, a NUL included (#2520),
     * is written as is, with no printf at run time. */
    int len = ast_literal_length(lit);
    char* text = (char*)malloc((size_t)len + 1);
    if (!text) {
        fprintf(stderr, "Fatal: out of memory decoding a print format\n");
        exit(1);
    }
    int n = 0;
    for (int i = 0; i < len; i++) {
        text[n++] = lit->value[i];
        if (lit->value[i] == '%' && i + 1 < len && lit->value[i + 1] == '%') i++;
    }
    fprintf(gen->output, "aether_write_bytes(stdout, \"");
    emit_c_string_bytes(gen, text, (size_t)n, 0);
    fprintf(gen->output, "\", %d, 0)", n);
    free(text);
}

void emit_string_literal_write(CodeGenerator* gen, const ASTNode* lit, int newline) {
    fprintf(gen->output, "aether_write_bytes(stdout, \"");
    emit_c_string_bytes(gen, lit->value, (size_t)lit->value_len, 0);
    fprintf(gen->output, "\", %d, %d)", lit->value_len, newline ? 1 : 0);
}

/* #2520: the `0` argument for each `%c` that emit_c_string_bytes wrote for
 * a NUL of an interpolation's text segment. */
static void emit_text_nul_args(CodeGenerator* gen, const ASTNode* text) {
    for (int i = 0; i < text->value_len; i++) {
        if (text->value[i] == '\0') fprintf(gen->output, ", 0");
    }
}

/* True when `n` spells the null pointer in a comparison: the identifier
 * `NULL`, or a bare literal `0` — but NOT the one-character string
 * literal `"0"`. Both literals carry the text `0` in `value`; only the
 * string one is TYPE_STRING at parse time (create_literal_node), so
 * that is what tells them apart. Pre-#2206 `s != "0"` was taken for a
 * null check and emitted a bare pointer compare against a `.rodata`
 * literal, which was never equal (and drew clang's -Wstring-compare). */
static int is_null_compare_operand(const ASTNode* n) {
    if (!n || !n->value) return 0;
    if (n->type == AST_IDENTIFIER) return strcmp(n->value, "NULL") == 0;
    if (n->type != AST_LITERAL) return 0;
    if (n->node_type && n->node_type->kind == TYPE_STRING) return 0;
    return strcmp(n->value, "0") == 0;
}

/* 1 when the binary expression `expr` compares two strings: ==, !=, <, >,
 * <= or >= with both sides strings, or one a string and the other a `ptr`
 * that may carry an AetherString (string.from_int, fs.read_binary, ...;
 * #267), and neither side a null literal (that is a null check). Shared by
 * every place a binary expression is emitted, so a comparison means the
 * same thing in a function-clause guard as anywhere else (#2515). */
int binary_is_string_compare(const ASTNode* expr) {
    if (!expr || !expr->value || expr->child_count < 2) return 0;
    const char* op = expr->value;
    if (strcmp(op, "==") != 0 && strcmp(op, "!=") != 0 && strcmp(op, "<") != 0 &&
        strcmp(op, ">") != 0 && strcmp(op, "<=") != 0 && strcmp(op, ">=") != 0) return 0;
    const ASTNode* l = expr->children[0];
    const ASTNode* r = expr->children[1];
    if (is_null_compare_operand(l) || is_null_compare_operand(r)) return 0;
    Type* lt = l ? l->node_type : NULL;
    Type* rt = r ? r->node_type : NULL;
    int ls = lt && lt->kind == TYPE_STRING, rs = rt && rt->kind == TYPE_STRING;
    int lp = lt && lt->kind == TYPE_PTR,    rp = rt && rt->kind == TYPE_PTR;
    return (ls && rs) || (ls && rp) || (lp && rs);
}

/* The two halves around the operands of a string comparison `a OP b`. A
 * string carries its length, so it compares by length and bytes through
 * string_equals / string_compare, which read either string shape: strcmp
 * stopped at the first NUL, so "x\0y" == "x" was true (#2515). Both keep a
 * one-pass strcmp for two plain C strings. */
void emit_string_compare_open(CodeGenerator* gen, const char* op) {
    if (strcmp(op, "==") == 0)      fprintf(gen->output, "string_equals(");
    else if (strcmp(op, "!=") == 0) fprintf(gen->output, "!string_equals(");
    else                            fprintf(gen->output, "(string_compare(");
}

void emit_string_compare_close(CodeGenerator* gen, const char* op) {
    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) fprintf(gen->output, ")");
    else fprintf(gen->output, ") %s 0)", get_c_operator(op));
}

/* ---- #1286 first-class slices ------------------------------------------ */

static const char* slice_elem_c_type(const Type* t) {
    return (t && t->kind == TYPE_ARRAY && t->element_type)
           ? get_c_type(t->element_type) : "char";
}

/* The source path a bounds-check failure names, as a C string literal
 * (#2305). A path is written into generated C, so it is escaped like any
 * string: a Windows path's backslashes were emitted raw, and each `\G`,
 * `\s`, `\d` became an unknown-escape warning per access (a `\a` a BEL in
 * the message). Control characters are written as octal escapes. */
static void emit_slice_diag_file(CodeGenerator* gen, const ASTNode* n) {
    const char* path = (n && n->source_file) ? n->source_file : "?";
    fputc('"', gen->output);
    for (const unsigned char* c = (const unsigned char*)path; *c; c++) {
        if (*c == '\\' || *c == '"') {
            fputc('\\', gen->output);
            fputc(*c, gen->output);
        } else if (*c < 0x20 || *c == 0x7f) {
            fprintf(gen->output, "\\%03o", *c);
        } else {
            fputc(*c, gen->output);
        }
    }
    fputc('"', gen->output);
}

void generate_expression_as_elem_ptr(CodeGenerator* gen, ASTNode* expr) {
    if (expr && type_is_slice(expr->node_type)) {
        fprintf(gen->output, "((%s*)(", slice_elem_c_type(expr->node_type));
        generate_expression(gen, expr);
        fprintf(gen->output, ").ptr)");
        return;
    }
    generate_expression(gen, expr);
}

void generate_expression_as_slice(CodeGenerator* gen, ASTNode* expr) {
    if (expr && type_is_sized_array(expr->node_type) && expr->node_type->array_size > 0) {
        fprintf(gen->output, "aether_slice_make((void*)(");
        generate_expression(gen, expr);
        fprintf(gen->output, "), %d)", expr->node_type->array_size);
        return;
    }
    generate_expression(gen, expr);
}

/* The struct definition a member access reads from, when its base is a
 * struct value or a `*Struct` pointer; NULL otherwise. */
static ASTNode* member_base_struct_def(CodeGenerator* gen, const ASTNode* macc) {
    if (!gen || !gen->program || !macc || macc->child_count < 1) return NULL;
    const Type* bt = macc->children[0] ? macc->children[0]->node_type : NULL;
    if (!bt) return NULL;
    const char* sname = NULL;
    if (bt->kind == TYPE_STRUCT) sname = bt->struct_name;
    else if (bt->kind == TYPE_PTR && bt->element_type &&
             bt->element_type->kind == TYPE_STRUCT) sname = bt->element_type->struct_name;
    if (!sname) return NULL;
    return find_struct_definition_by_name(gen->program, sname);
}

static int struct_def_is_extern(const ASTNode* sdef) {
    return sdef && sdef->annotation && strncmp(sdef->annotation, "extern", 6) == 0;
}

/* Does the C value of `expr` arrive as a bare `T*` although its Aether
 * type is a `T[]` slice? True for a call into an extern C function
 * declared `-> T[]`, and for a read of an `extern struct`'s `T[]` field.
 * Such a value is wrapped into an unbounded slice view. */
static int expr_is_c_view_of_slice(CodeGenerator* gen, const ASTNode* expr) {
    if (!expr || !type_is_slice(expr->node_type)) return 0;
    if (expr->type == AST_FUNCTION_CALL && expr->value) {
        const char* name = expr->value;
        if (gen->program && find_function_definition_by_name(gen->program, name)) return 0;
        Type* rt = lookup_extern_return_type(gen, name);
        if (!rt) {
            const char* dot = strrchr(name, '.');
            if (dot && dot[1]) {
                const char* norm = codegen_normalise_callee(name);
                if (gen->program && find_function_definition_by_name(gen->program, norm)) return 0;
                rt = lookup_extern_return_type(gen, norm);
                if (!rt) rt = lookup_extern_return_type(gen, dot + 1);
            }
        }
        return rt && type_is_slice(rt);
    }
    if (expr->type == AST_MEMBER_ACCESS && !gen->generating_lvalue) {
        return struct_def_is_extern(member_base_struct_def(gen, expr));
    }
    return 0;
}

/* Re-entrancy guard for the view wrap below: the node being wrapped is
 * generated once more through the ordinary path. */
static const ASTNode* g_slice_view_wrapping = NULL;
/* The enforce call whose site tag is being emitted (see AST_FUNCTION_CALL). */
static const ASTNode* g_sandbox_site_wrapping = NULL;

/* The block of a call's trailing DSL closure, or NULL. A closure the
 * function declares a `fn` parameter for, where the block sits, is an
 * ordinary argument, not a DSL block (the same test the argument loop of
 * the call path applies). */
ASTNode* trailing_dsl_block(CodeGenerator* gen, ASTNode* call) {
    if (!call || call->type != AST_FUNCTION_CALL) return NULL;
    for (int i = 0; i < call->child_count; i++) {
        ASTNode* arg = call->children[i];
        if (!arg || arg->type != AST_CLOSURE || !arg->value ||
            strcmp(arg->value, "trailing") != 0) continue;
        if (call->value && gen->program) {
            ASTNode* fdef = find_function_definition_by_name(gen->program, call->value);
            if (fdef) {
                int pi = 0;
                for (int fj = 0; fj < fdef->child_count; fj++) {
                    ASTNode* p = fdef->children[fj];
                    if (p->type == AST_GUARD_CLAUSE || p->type == AST_BLOCK) continue;
                    if (pi == i && p->node_type && p->node_type->kind == TYPE_FUNCTION)
                        return NULL;
                    pi++;
                }
            }
        }
        for (int bi = 0; bi < arg->child_count; bi++) {
            if (arg->children[bi] && arg->children[bi]->type == AST_BLOCK)
                return arg->children[bi];
        }
    }
    return NULL;
}

/* `call(args) { block }` where the value is used inside an expression:
 * `return build() { ... }`, `f(build() { ... })`. A declaration, an
 * assignment and an expression statement run the block themselves
 * (generate_statement marks that call); anywhere else the block was
 * skipped with the argument loop's DSL test and silently dropped. Lowered
 * here to a statement expression that runs it the way those statements
 * do: a builder's block configures it before the call; any other call's
 * value is the block's context. Returns 0 when there is no such block. */
static int g_trailing_tmp = 0;
static int emit_trailing_call_expression(CodeGenerator* gen, ASTNode* call) {
    ASTNode* block = trailing_dsl_block(gen, call);
    /* An empty block runs nothing and configures nothing: the plain call,
     * as before (and a builder's config factory is not needed for it). */
    if (!block || block->child_count == 0) return 0;
    int n = g_trailing_tmp++;
    if (call->value && is_builder_func_reg(gen, call->value)) {
        fprintf(gen->output, "({ void* _tcfg%d = (void*)(intptr_t)%s(); _aether_ctx_push(_tcfg%d);\n",
                n, get_builder_factory(gen, call->value), n);
        emit_trailing_block_body(gen, block);
        fprintf(gen->output, "%s(", codegen_normalise_callee(safe_c_name(call->value)));
        int argc = 0;
        for (int i = 0; i < call->child_count; i++) {
            ASTNode* arg = call->children[i];
            if (arg && arg->type == AST_CLOSURE && arg->value &&
                strcmp(arg->value, "trailing") == 0) continue;
            if (argc++ > 0) fprintf(gen->output, ", ");
            generate_expression(gen, arg);
        }
        fprintf(gen->output, "%s_tcfg%d); })", argc > 0 ? ", " : "", n);
        return 1;
    }
    ASTNode* saved = gen->trailing_stmt_call;
    fprintf(gen->output, "({ __auto_type _tcv%d = ", n);
    gen->trailing_stmt_call = call;      /* the plain call, block skipped */
    generate_expression(gen, call);
    gen->trailing_stmt_call = saved;
    fprintf(gen->output, "; _aether_ctx_push((void*)(intptr_t)_tcv%d);\n", n);
    emit_trailing_block_body(gen, block);
    fprintf(gen->output, "_tcv%d; })", n);
    return 1;
}

/* Calls whose owning-struct result is a temporary of the expression
 * statement being emitted (stmt_struct_temps_begin, codegen_stmt.c): each is
 * emitted as `(<temp> = <call>)`, and the statement destroys the temp once it
 * is done. */
static ASTNode** g_stmt_temp_nodes = NULL;
static const char** g_stmt_temp_names = NULL;
static int g_stmt_temp_count = 0;
static const ASTNode* g_stmt_temp_wrapping = NULL;

/* Is a call to `fn` the runtime free `sym`, by name or `@extern` alias? */
static int cfree_sym_is(CodeGenerator* gen, const char* fn, const char* sym) {
    const char* got = consuming_free_symbol(gen, fn);
    return got && strcmp(got, sym) == 0;
}

void stmt_struct_temps_set(ASTNode** nodes, const char** names, int count) {
    g_stmt_temp_nodes = nodes;
    g_stmt_temp_names = names;
    g_stmt_temp_count = count;
}

/* The temporary holding call `expr`'s struct result in the statement being
 * emitted, or NULL. */
const char* stmt_struct_temp_of(const ASTNode* expr) {
    for (int i = 0; expr && i < g_stmt_temp_count; i++) {
        if (g_stmt_temp_nodes[i] == expr) return g_stmt_temp_names[i];
    }
    return NULL;
}

void generate_expression(CodeGenerator* gen, ASTNode* expr) {
    if (!expr) return;

    codegen_note_diag_pos(expr);

    /* #2478: an operand already evaluated into a temp, in source order. */
    if (g_order_count > 0) {
        const char* sub = order_lookup(expr);
        if (sub) {
            fprintf(gen->output, "%s", sub);
            return;
        }
    }
    if (expr != g_order_emitting && emit_in_operand_order(gen, expr)) return;

    /* #1286: a `T*` from C, read as a `T[]`, becomes an unbounded view. */
    if (g_slice_view_wrapping != expr && expr_is_c_view_of_slice(gen, expr) &&
        !arg_drain_lookup(expr)) {
        const ASTNode* saved = g_slice_view_wrapping;
        g_slice_view_wrapping = expr;
        fprintf(gen->output, "aether_slice_view((void*)(");
        generate_expression(gen, expr);
        fprintf(gen->output, "))");
        g_slice_view_wrapping = saved;
        return;
    }

    if (g_stmt_temp_count > 0 && g_stmt_temp_wrapping != expr) {
        for (int i = 0; i < g_stmt_temp_count; i++) {
            if (g_stmt_temp_nodes[i] != expr) continue;
            const ASTNode* saved = g_stmt_temp_wrapping;
            g_stmt_temp_wrapping = expr;
            fprintf(gen->output, "(%s = ", g_stmt_temp_names[i]);
            generate_expression(gen, expr);
            fprintf(gen->output, ")");
            g_stmt_temp_wrapping = saved;
            return;
        }
    }

    /* Argument-temp lifetime substitution. If this AST_FUNCTION_CALL
     * node has been hoisted by a parent-call wrap (see
     * arg_drain_register at the AST_FUNCTION_CALL fallthrough below),
     * emit the temp name instead of re-evaluating the call. Keeps
     * the parent's call site syntactically intact while the temp's
     * lifetime is managed by the wrap. */
    if (expr->type == AST_FUNCTION_CALL ||
        expr->type == AST_STRING_INTERP ||
        expr->type == AST_CLOSURE ||
        expr->type == AST_OR_ELSE) {
        const char* sub = arg_drain_lookup(expr);
        if (sub) {
            fprintf(gen->output, "%s", sub);
            return;
        }
    }

    switch (expr->type) {
        case AST_LITERAL:
            if (expr->node_type && expr->node_type->kind == TYPE_STRING) {
                emit_string_literal_node(gen, expr);
            } else if (expr->node_type && expr->node_type->kind == TYPE_DURATION) {
                fprintf(gen->output, "%lldLL", parse_duration_literal_ns(expr->value));
            } else if (expr->node_type && expr->node_type->kind == TYPE_FLOAT32) {
                /* #2151: a literal typed f32 (it stood beside an f32 operand)
                 * is a C float constant, so the operation is a float one.
                 * `0.5f` for a decimal spelling; an integer spelling cannot
                 * take the suffix, so it is cast. */
                if (strpbrk(expr->value, ".eE"))
                    fprintf(gen->output, "%sf", expr->value);
                else
                    fprintf(gen->output, "((float)%s)", expr->value);
            } else {
                /* Numeric literals: translate 0o / 0b prefixes that C
                 * doesn't accept and suffix a decimal past LLONG_MAX;
                 * everything else passes through unchanged. */
                fprintf(gen->output, "%s", translate_integer_literal(expr->value));
            }
            break;

        case AST_NULL_LITERAL:
            fprintf(gen->output, "NULL");
            break;

        case AST_HEAP_NEW: {
            /* heap.new(T) — zero-init heap allocation of a POD struct,
             * yielding `*T`. calloc guarantees the zero-init the safety
             * review (issue #564, H5) requires: with no string fields
             * (enforced POD-only by the typechecker) there are no hidden
             * `_heap_<field>` trackers to mis-seed, but calloc keeps every
             * scalar / ptr / array field a clean zero. Cast to T* so member
             * access (`p.field`) and `heap.free(p)` see the right type. */
            /* #1860: the struct name is on the node itself (the parser puts
             * it there), so use it whenever the inferred pointer type is not
             * available — which is exactly the `return heap.new(T)` case
             * outside main, where the node's type never got resolved. Falling
             * back to "void" emitted `calloc(1, sizeof(void))`: a ONE BYTE
             * allocation for the whole struct. Every field write then ran past
             * the end of it and the `_heap_<field>` ownership trackers read
             * back as zero, so the box's destructor believed it owned nothing.
             * The reported string leak was the visible symptom of that. */
            const char* struct_c = NULL;
            if (expr->node_type && expr->node_type->kind == TYPE_PTR &&
                expr->node_type->element_type) {
                struct_c = get_c_type(expr->node_type->element_type);
            }
            if ((!struct_c || strcmp(struct_c, "void") == 0) && expr->value) {
                struct_c = expr->value;
            }
            if (!struct_c || strcmp(struct_c, "void") == 0) {
                /* Never silently allocate one byte and call it a struct. */
                aether_error_full(
                    "heap.new: cannot determine the struct type to allocate",
                    expr->line, expr->column,
                    "name the struct explicitly, e.g. heap.new(MyStruct)",
                    "in heap.new", AETHER_ERR_NONE);
                struct_c = "void";
            }
            fprintf(gen->output, "((%s*)calloc(1, sizeof(%s)))",
                    struct_c, struct_c);
            break;
        }

        case AST_OR_ELSE: {
            /* #913: `fallible or handler`. Evaluate the (value, err) tuple
             * once; on a non-empty error slot run the handler, else yield the
             * value. A block handler runs its statements (with `err` bound) and
             * is expected to exit (return/break/…) — matching the codebase's
             * block-body convention; a bare expression handler is the default
             * value. Single-eval via a GCC statement-expression. */
            if (expr->child_count < 2) break;
            ASTNode* fallible = expr->children[0];
            ASTNode* handler = expr->children[1];
            Type* tup = fallible->node_type;
            if (!tup || tup->kind != TYPE_TUPLE || tup->tuple_count < 2 ||
                !tup->tuple_types[0]) {
                generate_expression(gen, fallible);   // malformed; already flagged
                break;
            }
            const char* tuple_c = get_c_type(tup);
            const char* val_c = get_c_type(tup->tuple_types[0]);
            int err_idx = tup->tuple_count - 1;
            static int oe_counter = 0;
            int id = oe_counter++;
            /* The trailing error slot is discarded on BOTH paths — on the
             * error path the handler consumes it (as `err`) then it is
             * dead; on the success path it is the `""` success sentinel.
             * When the fallible's error position is classified HEAP, every
             * return (both paths) was wrapped in `aether_uniform_heap_str`
             * (emit_tuple_return_position), so the slot is always a
             * malloc-owned pointer — an AetherString or a plain malloc'd
             * copy — and leaks unless freed. `aether_heap_str_free`
             * reclaims both shapes. When the error position is NON-heap
             * (e.g. `string.to_long`'s raw literal "invalid long"), the
             * slot is a `.rodata` literal and must NOT be freed — so gate
             * the free on the SAME static classification the `v, e = f()`
             * destructure site uses for `_heap_e`, keeping the two forms
             * consistent. */
            int err_slot_heap = or_fallible_error_slot_is_heap(gen, fallible);
            /* Uniform-heap boxing of the RESULT: when the success value
             * slot is a heap string, the success path yields a malloc-owned
             * pointer but the handler's default (`"" `, a literal) is not —
             * heterogeneous ownership the caller's single `_heap_<lhs>`
             * tracker can't represent, so it leaked the success value. Box
             * the handler's value through `aether_uniform_heap_str(_, 0)`
             * (malloc-copies a literal, passes a heap value through) so BOTH
             * paths yield a uniformly malloc-owned pointer; then the
             * AST_OR_ELSE case in is_heap_string_expr classifies the whole
             * expression heap and the caller frees it at scope exit. Only
             * for a string result whose value slot is statically heap. */
            int box_val = (tup->tuple_types[0] &&
                           tup->tuple_types[0]->kind == TYPE_STRING &&
                           or_fallible_value_slot_is_heap(gen, fallible));
            fprintf(gen->output, "({ %s _oe%d = ", tuple_c, id);
            generate_expression(gen, fallible);
            /* "Is this an error" is "is the error slot non-empty", and the
             * slot holds either physical string shape. Indexing it raw reads
             * the AetherString magic header as content, so every refcounted
             * error slot looks non-empty whatever it says: `parse_strict`
             * returns an empty one on SUCCESS and `or` took the handler.
             * aether_string_data yields the payload for either shape. */
            fprintf(gen->output,
                    "; %s _oer%d;\nif (_oe%d._%d && aether_string_data((const void*)_oe%d._%d)[0]) {\n",
                    val_c, id, id, err_idx, id, err_idx);
            if (handler->type == AST_BLOCK) {
                /* Block statements emit their own `#line` directives, which
                 * must begin a line — hence the newlines bracketing them.
                 *
                 * The block's LAST statement is the handler's value: a bare
                 * trailing expression (wrapped by the parser in an
                 * AST_EXPRESSION_STATEMENT) is assigned to `_oer` so
                 * `x = f() or { log(err) -1 }` yields -1 on the error path.
                 * Before this, the trailing expression was emitted as a
                 * discarded statement and `_oer` was read UNINITIALIZED — a
                 * silent miscompile whenever the block didn't exit. A block
                 * ending in an exit statement (return / panic / break /
                 * continue) never falls through, so no assignment is needed
                 * there; the typechecker rejects every other ending, so by
                 * the time we get here the last child is one or the other. */
                fprintf(gen->output, "const char* err = _oe%d._%d; (void)err;\n",
                        id, err_idx);
                int last = handler->child_count - 1;
                for (int j = 0; j < last; j++) {
                    generate_statement(gen, handler->children[j]);
                }
                if (last >= 0 && handler->children[last] &&
                    handler->children[last]->type == AST_EXPRESSION_STATEMENT &&
                    handler->children[last]->child_count > 0) {
                    ASTNode* hv = handler->children[last]->children[0];
                    /* is_heap flag = whether hv is ALREADY heap: a heap
                     * value passes through uniform_heap_str untouched
                     * (must NOT re-copy — that would leak the original);
                     * a literal is malloc-copied. */
                    int hv_heap = is_heap_string_expr(gen, hv);
                    print_indent(gen);
                    fprintf(gen->output, "_oer%d = ", id);
                    if (box_val) fprintf(gen->output, "aether_uniform_heap_str((const char*)(");
                    generate_expression(gen, hv);
                    if (box_val) fprintf(gen->output, "), %d)", hv_heap ? 1 : 0);
                    fprintf(gen->output, ";\n");
                } else if (last >= 0 && handler->children[last]) {
                    generate_statement(gen, handler->children[last]);
                }
                fprintf(gen->output, "\n");
            } else {
                int hv_heap = is_heap_string_expr(gen, handler);
                fprintf(gen->output, "_oer%d = ", id);
                if (box_val) fprintf(gen->output, "aether_uniform_heap_str((const char*)(");
                generate_expression(gen, handler);
                if (box_val) fprintf(gen->output, "), %d)", hv_heap ? 1 : 0);
                fprintf(gen->output, ";\n");
            }
            /* Error path done. Free the discarded slots the handler
             * replaced:
             *   - the error slot `err`, now dead, if statically heap;
             *   - the failed call's VALUE slot `_oe._0`, when it is a
             *     uniformly-heap string (box_val): the handler produced a
             *     fresh `_oer` value, so `_oe._0` (the failed call's own
             *     value, itself uniform-heap-wrapped — e.g. json's
             *     `("", err)` empty sentinel) is genuinely discarded and
             *     non-aliasing here, so it must be reclaimed. Without
             *     box_val the value slot's ownership is unknown, so it is
             *     left alone (as before). */
            if (box_val) {
                fprintf(gen->output, "aether_heap_str_free((void*)_oe%d._0);\n",
                        id);
            }
            if (err_slot_heap) {
                fprintf(gen->output, "aether_heap_str_free((void*)_oe%d._%d);\n",
                        id, err_idx);
            }
            /* Success path: yield the value slot as the result (must NOT be
             * freed — it is what the expression evaluates to); free only the
             * discarded success-sentinel error slot, again only when heap. */
            if (err_slot_heap) {
                fprintf(gen->output,
                        "} else { _oer%d = _oe%d._0; aether_heap_str_free((void*)_oe%d._%d); } _oer%d; })",
                        id, id, id, err_idx, id);
            } else {
                fprintf(gen->output, "} else { _oer%d = _oe%d._0; } _oer%d; })",
                        id, id, id);
            }
            break;
        }

        case AST_TUPLE_UNWRAP: {
            /* `expr!` — unwrap-or-trap. Emit a GCC statement-expression
             * that evaluates the tuple once, panics if the trailing
             * (string) error slot is non-empty, and yields the first
             * slot:
             *
             *   ({ _tuple_T_string _u = <operand>;
             *      if (_u._N && _u._N[0]) aether_panic("...");
             *      _u._0; })
             *
             * The error slot is non-empty iff its pointer is non-NULL AND
             * its first byte is not '\0' — matching the `err != ""`
             * convention every (value, err) wrapper uses (the "" success
             * sentinel is a non-NULL empty string). */
            if (expr->child_count == 0 || !expr->children[0]) break;
            ASTNode* operand = expr->children[0];
            /* #340: postfix `!` is polymorphic on the operand type. When the
             * operand is an optional `T?`, this is force-unwrap — yield the
             * wrapped value, panic on `none`, single-eval via a statement-
             * expression. The (value, err) tuple-unwrap form follows below. */
            Type* ot = operand->node_type;
            if (ot && ot->kind == TYPE_OPTIONAL) {
                static int fu_counter = 0;
                int id = fu_counter++;
                fprintf(gen->output, "({ %s _fu%d = ", get_c_type(ot), id);
                generate_expression(gen, operand);
                fprintf(gen->output, "; if (!_fu%d.has) aether_panic(\"forced_unwrap_none: forced unwrap of `none`\"); _fu%d.val; })",
                        id, id);
                break;
            }
            Type* tup = operand->node_type;
            if (!tup || tup->kind != TYPE_TUPLE || tup->tuple_count < 2) {
                /* Typechecker already rejected this; emit the operand
                 * bare so codegen doesn't crash on a malformed tree. */
                generate_expression(gen, operand);
                break;
            }
            ensure_tuple_typedef(gen, tup);
            const char* tuple_c = get_c_type(tup);
            int err_idx = tup->tuple_count - 1;
            static int unwrap_tmp_counter = 0;
            int uid = unwrap_tmp_counter++;

            fprintf(gen->output, "({ %s _unw%d = ", tuple_c, uid);
            generate_expression(gen, operand);
            fprintf(gen->output, "; ");
            /* #913: `expr!` on a (value, err) result. In a function whose
             * return type is itself a result (`T!`), PROPAGATE — return the
             * enclosing result with the error slot set (value slot zero-init),
             * V-style one-char propagation. Otherwise keep the unwrap-or-PANIC
             * semantics, so `expr!` in a non-result function is unchanged. The
             * `return` inside the statement-expression returns from the
             * enclosing function, which is exactly the propagation we want. */
            if (gen->current_func_return_type &&
                gen->current_func_return_type->is_result) {
                /* The propagation `return` is a genuine function exit, so it
                 * must run the same cleanup every other `return` site runs.
                 * Before this it ran NONE of it: a `T!` function that
                 * propagated an error skipped its user `defer`s AND the
                 * synthetic RAII carriers the compiler pushes (heap-string /
                 * *StringSeq / struct-destroy exit frees), leaking everything
                 * the function was holding — silently, on the error path only.
                 *
                 * The defers are emitted INSIDE the statement-expression's
                 * `if`, immediately before the return, which needs no
                 * restructuring of the expression — they are ordinary
                 * statements. The trailing newline before them is load-bearing:
                 * a defer body carries `#line` directives, and a `#` is only a
                 * preprocessor directive at the START of a line. Emitted
                 * mid-line (right after `if (...) {`) it is a stray `#` and the
                 * C compiler rejects the file. */
                fprintf(gen->output,
                        "if (_unw%d._%d && _unw%d._%d[0]) {\n",
                        uid, err_idx, uid, err_idx);
                /* #1140: propagation is unambiguously an ERROR exit — we are
                 * inside the `if` that tested the error slot. So `defer catch`
                 * fires and `defer try` does not, and it is known statically,
                 * with no runtime guard needed. */
                {
                    DeferExit prev_exit = gen->defer_exit;
                    gen->defer_exit = DEFER_EXIT_ERROR;
                    emit_all_defers(gen);
                    gen->defer_exit = prev_exit;
                }
                /* Issue #501: drain in-flight try frames, exactly as the
                 * ordinary return path does — a propagation is just as
                 * non-local an exit as a `return`. */
                emit_try_pops_for_nonlocal_exit(gen);
                /* Leading newline for the same reason: a defer body can end
                 * mid-line, and the next thing must not be glued onto it. */
                fprintf(gen->output,
                        "\nreturn (%s){ ._1 = _unw%d._%d }; } ",
                        get_c_type(gen->current_func_return_type), uid, err_idx);
            } else {
                fprintf(gen->output,
                        "if (_unw%d._%d && _unw%d._%d[0]) "
                        "aether_panic(_unw%d._%d); ",
                        uid, err_idx, uid, err_idx, uid, err_idx);
            }
            fprintf(gen->output, "_unw%d._0; })", uid);
            break;
        }

        case AST_NONE_LITERAL: {
            // #340: `none` — zero-init compound literal of the pinned optional
            // type (`{0}` sets has=0). Bare `{0}` only if unpinned (an error
            // path the typechecker already flagged).
            if (expr->node_type && expr->node_type->kind == TYPE_OPTIONAL &&
                expr->node_type->element_type &&
                expr->node_type->element_type->kind != TYPE_UNKNOWN) {
                fprintf(gen->output, "(%s){0}", get_c_type(expr->node_type));
            } else {
                fprintf(gen->output, "{0}");
            }
            break;
        }

        case AST_NULL_COALESCE: {
            // #340: `opt ?? default` -> opt.val if present, else default.
            if (expr->child_count < 2) break;
            ASTNode* operand = expr->children[0];
            Type* ot = operand->node_type;
            if (!ot || ot->kind != TYPE_OPTIONAL) { generate_expression(gen, operand); break; }
            static int nc_counter = 0;
            int id = nc_counter++;
            fprintf(gen->output, "({ %s _nc%d = ", get_c_type(ot), id);
            generate_expression(gen, operand);
            fprintf(gen->output, "; _nc%d.has ? _nc%d.val : (", id, id);
            generate_expression(gen, expr->children[1]);
            fprintf(gen->output, "); })");
            break;
        }

        case AST_OPTIONAL_CHAIN: {
            // #340: `opt?.field` -> fieldT? (none-propagating).
            if (expr->child_count == 0 || !expr->value) break;
            ASTNode* operand = expr->children[0];
            Type* ot = operand->node_type;   // optional<struct> | optional<*struct>
            Type* rt = expr->node_type;      // fieldT?
            if (!ot || ot->kind != TYPE_OPTIONAL || !rt || rt->kind != TYPE_OPTIONAL) {
                generate_expression(gen, operand); break;
            }
            Type* inner = ot->element_type;
            const char* acc = (inner && inner->kind == TYPE_PTR) ? "->" : ".";
            const char* rc = get_c_type(rt);   /* interned, kept across the calls below */
            static int oc_counter = 0;
            int id = oc_counter++;
            fprintf(gen->output, "({ %s _oc%d = ", get_c_type(ot), id);
            generate_expression(gen, operand);
            fprintf(gen->output, "; _oc%d.has ? (%s){ .has = 1, .val = _oc%d.val%s%s } : (%s){0}; })",
                    id, rc, id, acc, expr->value, rc);
            break;
        }

        case AST_IF_EXPRESSION:
            // if cond { then } else { else } → C ternary: (cond) ? (then) : (else)
            if (expr->child_count >= 3) {
                fprintf(gen->output, "(");
                generate_expression(gen, expr->children[0]);
                fprintf(gen->output, ") ? (");
                generate_expression(gen, expr->children[1]);
                fprintf(gen->output, ") : (");
                generate_expression(gen, expr->children[2]);
                fprintf(gen->output, ")");
            }
            break;

        case AST_SIZEOF:
            // sizeof(TypeName) → C sizeof(struct TypeName). Extern/struct
            // types emit as `struct <Name>` (same convention as
            // `as *StructName`), so the value always tracks the real C
            // layout.
            fprintf(gen->output, "((int)sizeof(struct %s))", expr->value);
            break;

        case AST_SCHEMA_OF:
            // #2298: the body of a synthesized `T_schema()`: the address of
            // T's static field table (codegen_schema.c emits the getter).
            fprintf(gen->output, "((void*)_ae_schema_%s())", expr->value);
            break;

        case AST_OFFSETOF:
            // offsetof(TypeName, field) → C offsetof(struct TypeName, field).
            if (expr->child_count >= 1 && expr->children[0]->value) {
                fprintf(gen->output, "((int)offsetof(struct %s, %s))",
                        expr->value, expr->children[0]->value);
            } else {
                fprintf(gen->output, "/* malformed offsetof */0");
            }
            break;

        case AST_BITSET_LITERAL:
            // #1046 `bit_set[E]{ E.A, E.B }` → `((1ULL<<E_A) | (1ULL<<E_B))`.
            // Each member is the enum constant (= its bit position); the empty
            // set is `0ULL`. Fully constant-foldable by the C compiler.
            if (expr->child_count == 0) {
                fprintf(gen->output, "0ULL");
            } else {
                fprintf(gen->output, "(");
                for (int i = 0; i < expr->child_count; i++) {
                    if (i > 0) fprintf(gen->output, " | ");
                    fprintf(gen->output, "(1ULL << (");
                    generate_expression(gen, expr->children[i]);
                    fprintf(gen->output, "))");
                }
                fprintf(gen->output, ")");
            }
            break;

        case AST_BITSET_CARD:
            // #1046 `card(s)` → popcount of the backing word.
            if (expr->child_count >= 1) {
                fprintf(gen->output, "((int)__builtin_popcountll((unsigned long long)(");
                generate_expression(gen, expr->children[0]);
                fprintf(gen->output, ")))");
            } else {
                fprintf(gen->output, "/* malformed card */0");
            }
            break;

        case AST_VA_START:
            // The variadic function's prologue declared `va_list __ae_va`
            // and called va_start. This expression just yields its
            // address as the opaque cookie ptr the va_arg/va_end
            // intrinsics consume.
            fprintf(gen->output, "((void*)&__ae_va)");
            break;

        case AST_VA_ARG:
            // va_arg(vap, T) → va_arg(*(va_list*)(vap), <ctype>).
            if (expr->child_count >= 1) {
                fprintf(gen->output, "va_arg(*(va_list*)(");
                generate_expression(gen, expr->children[0]);
                fprintf(gen->output, "), %s)",
                        expr->node_type ? get_c_type(expr->node_type) : "void*");
            } else {
                fprintf(gen->output, "/* malformed va_arg */0");
            }
            break;

        case AST_VA_END:
            // va_end(vap) → va_end(*(va_list*)(vap)).
            if (expr->child_count >= 1) {
                fprintf(gen->output, "va_end(*(va_list*)(");
                generate_expression(gen, expr->children[0]);
                fprintf(gen->output, "))");
            } else {
                fprintf(gen->output, "/* malformed va_end */");
            }
            break;

        case AST_IDENTIFIER:
            if (!expr->value) { fprintf(gen->output, "/* NULL identifier */0"); break; }
            // #1068 flow-narrowed optional: inside `if x != none { ... }` the
            // typechecker marked this read of `x` as narrowed, so emit the inner
            // value `x.val` of the `ae_opt` struct. Presence is proven by the
            // guard, so there is NO runtime none-check (zero cost).
            if (expr->annotation && strcmp(expr->annotation, "__opt_narrowed") == 0) {
                fprintf(gen->output, "%s.val", expr->value);
                break;
            }
            // Source-location intrinsics (#265) — `__LINE__` / `__FILE__` /
            // `__func__` substitute literal AST-node line, source-file path,
            // and C-side function name (which mirrors the Aether function
            // name in most cases). No call syntax — they're spelled as
            // identifiers but produce literal values at codegen.
            //
            // Caller-site capture (Phase A2.2): when used as a default
            // function argument — `f(msg, line: int = __LINE__)` —
            // `f(msg)` substitutes the call site's line, not the
            // function definition's. The typechecker's default-fill
            // path clones the default expression at the call site and
            // calls rewrite_caller_site_intrinsics() on the clone to
            // overwrite `expr->line` with the call's line BEFORE
            // codegen sees it. So this codegen path always emits the
            // right number whether the intrinsic is at an explicit
            // call site or substituted from a default.
            if (strcmp(expr->value, "__LINE__") == 0) {
                fprintf(gen->output, "%d", expr->line);
                break;
            }
            if (strcmp(expr->value, "__FILE__") == 0) {
                /* Use C string literal escaping for safety against `\` and `"`
                 * in path components (Windows paths in particular). */
                const char* path = gen->source_file ? gen->source_file : "(unknown)";
                fputc('"', gen->output);
                for (const char* p = path; *p; p++) {
                    if (*p == '\\' || *p == '"') fputc('\\', gen->output);
                    fputc(*p, gen->output);
                }
                fputc('"', gen->output);
                break;
            }
            if (strcmp(expr->value, "__func__") == 0) {
                /* C99 `__func__` — expands at compile time to the enclosing
                 * function's name. Since codegen mirrors Aether function
                 * names to C, this gives the Aether-side function name in
                 * the common case. Closure / arrow-function bodies get the
                 * generated wrapper's name; acceptable for v1. */
                fprintf(gen->output, "__func__");
                break;
            }
            // Route 1: promoted captures are `int* name` — dereference on read.
            // Applies uniformly in outer function bodies and in closure bodies;
            // the difference is only at declaration time (outer: malloc+init;
            // closure: alias from _env->name).
            // Exception: nodes annotated "raw_promoted" are passing the raw
            // pointer (e.g. to free()) and must not be dereferenced.
            if (is_promoted_capture(gen, expr->value) &&
                !(expr->annotation && strcmp(expr->annotation, "raw_promoted") == 0)) {
                fprintf(gen->output, "(*%s)", expr->value);
                break;
            }
            // Env-backed captures (mutated inside a closure body) have no local
            // alias — reads and writes must go through _env->name.
            // NOTE: with Route 1, mutated captures are promoted instead, so
            // this path is only taken when current_env_captures is populated
            // with a name that is NOT also promoted (legacy fallback).
            {
                int is_env_cap = 0;
                for (int i = 0; i < gen->current_env_capture_count; i++) {
                    if (gen->current_env_captures[i] &&
                        strcmp(gen->current_env_captures[i], expr->value) == 0) {
                        is_env_cap = 1;
                        break;
                    }
                }
                if (is_env_cap) {
                    fprintf(gen->output, "_env->%s", expr->value);
                    break;
                }
            }
            /* #2055: a function named as a value -- the typechecker marked
             * it -- is an _AeClosure over the bare-fn adapter, the same
             * lowering a bare function gets as a `fn` argument. */
            if (expr->annotation && strcmp(expr->annotation, "fn_value") == 0 &&
                find_function_definition_by_name(gen->program, expr->value)) {
                register_bare_fn_adapter(gen, expr->value);
                fprintf(gen->output,
                        "(_AeClosure){ .fn = (void(*)(void))_aether_bare_adapter_%s, .env = NULL }",
                        expr->value);
                break;
            }
            // Identifier-as-value naming a @c_callback function: emit
            // the C symbol the annotation binds to (#235), so passing
            // an Aether function as a function pointer to a C extern
            // resolves at link time. Handles both in-file callbacks
            // (Aether-side name == AST value) and imported-module ones
            // (AST value is the post-merge prefixed form).
            {
                const char* cb_sym = lookup_c_callback_symbol(gen, expr->value);
                if (cb_sym) {
                    fprintf(gen->output, "%s", cb_sym);
                    break;
                }
            }
            /* A function's address under `as fn(...)`: the definition's C
             * spelling, which safe_c_name mangles away from libc symbols.
             * After the @c_callback lookup, whose bound symbol is the
             * definition's spelling for those functions. */
            if (expr->annotation && strcmp(expr->annotation, "fn_addr") == 0 &&
                find_function_definition_by_name(gen->program, expr->value)) {
                fprintf(gen->output, "%s", safe_c_name(expr->value));
                break;
            }
            if (gen->current_actor) {
                int is_state_var = 0;
                for (int i = 0; i < gen->state_var_count; i++) {
                    if (strcmp(expr->value, gen->actor_state_vars[i]) == 0) {
                        is_state_var = 1;
                        break;
                    }
                }
                if (is_state_var) {
                    // `state_self_alias` lets the spawn-time timeout
                    // expression resolve state fields against the
                    // local `actor` (the only handle in scope at the
                    // alloc site). Everywhere else the alias is NULL
                    // and we emit the canonical `self->field`.
                    const char* alias = gen->state_self_alias
                                      ? gen->state_self_alias
                                      : "self";
                    fprintf(gen->output, "%s->%s", alias, expr->value);
                } else {
                    fprintf(gen->output, "%s", expr->value);
                }
            } else {
                fprintf(gen->output, "%s", expr->value);
            }
            break;
        
        case AST_MEMBER_ACCESS:
            if (expr->child_count > 0) {
                ASTNode* child = expr->children[0];
                /* #1286: `s.len` — a slice's element count, an array's size. */
                if (expr->value && strcmp(expr->value, "len") == 0 &&
                    child->node_type && child->node_type->kind == TYPE_ARRAY) {
                    if (type_is_slice(child->node_type)) {
                        fprintf(gen->output, "((int)aether_slice_len(");
                        generate_expression(gen, child);
                        fprintf(gen->output, "))");
                    } else {
                        fprintf(gen->output, "(%d)", child->node_type->array_size);
                    }
                    break;
                }
                /* #2146: a lane read — `v.x` is the C vector's `v[0]`. */
                if (child->node_type && expr->value) {
                    int lane = lane_accessor_index(child->node_type->kind, expr->value);
                    if (lane >= 0 && (child->node_type->kind == TYPE_F32X8 ||
                                      child->node_type->kind == TYPE_I32X8)) {
                        /* #2428: the halves form is a struct. The macro is
                         * `v[i]` natively and `v.lo[i]` in halves (.x-.w are
                         * lanes 0-3), an lvalue either way, so a lane write
                         * works as a read does. */
                        fprintf(gen->output, "_AE_LANE8_LOW(");
                        generate_expression(gen, child);
                        fprintf(gen->output, ", %d)", lane);
                        break;
                    }
                    if (lane >= 0) {
                        fprintf(gen->output, "(");
                        generate_expression(gen, child);
                        fprintf(gen->output, ")[%d]", lane);
                        break;
                    }
                }
                /* #891 @c_struct overlay read (handles nested chains
                 * `s.a.b.c`): flatten to the overlay-pointer root + dotted
                 * field path, then emit aether_mem_get_<width> at the
                 * cumulative offset. Width is DERIVED from the field type. */
                if (expr->value) {
                    const char* cpath = NULL;
                    ASTNode* root = aether_c_struct_chain(expr, &cpath);
                    if (root) {
                        long off = 0; const char* width = NULL;
                        const char* sname = root->node_type->element_type->struct_name;
                        if (aether_c_struct_resolve(sname, cpath, &off, &width) && width) {
                            fprintf(gen->output, "aether_mem_get_%s((void*)(", width);
                            generate_expression(gen, root);
                            fprintf(gen->output, "), %ld)", off);
                        } else {
                            fprintf(gen->output, "/* @c_struct: unknown field %s.%s */0",
                                    sname, cpath);
                        }
                        break;
                    }
                }
                /* #1132 bitstruct field read: `b.f` -> `((b >> lo) & mask)`.
                 *
                 * The mask is applied AFTER the shift and the backing word is
                 * unsigned, so the result can never be sign-extended — which is
                 * exactly the bug a C bitfield has (gcc gives `int x : 3` a
                 * SIGNED representation, so a stored 0b111 reads back as -1).
                 * A bool field compares against 0 so the result is a clean 0/1. */
                if (expr->value) {
                    const char* bname = aether_bitstruct_base_name(expr);
                    if (bname) {
                        int lo = 0, hi = 0, is_bool = 0;
                        const char* backing = NULL;
                        if (aether_bitstruct_resolve(bname, expr->value, &lo, &hi,
                                                     &is_bool, &backing)) {
                            unsigned long long mask = aether_bitstruct_mask(lo, hi);
                            /* Fully parenthesised: this can be embedded in any
                             * larger expression without precedence surprises. */
                            fprintf(gen->output, "(((");
                            generate_expression(gen, child);
                            if (lo > 0) fprintf(gen->output, " >> %d", lo);
                            fprintf(gen->output, ") & 0x%llxULL)%s)", mask,
                                    is_bool ? " != 0" : "");
                            break;
                        }
                        fprintf(gen->output, "/* bitstruct: unknown field %s.%s */0",
                                bname, expr->value);
                        break;
                    }
                }
                if (child->node_type && child->node_type->kind == TYPE_DURATION && expr->value) {
                    long long scale = duration_accessor_scale(expr->value);
                    if (scale == 1) {
                        generate_expression(gen, child);
                    } else if (scale > 1) {
                        fprintf(gen->output, "((double)(");
                        generate_expression(gen, child);
                        fprintf(gen->output, ") / %.1f)", (double)scale);
                    } else {
                        fprintf(gen->output, "0");
                    }
                    break;
                }

                int needs_atomic = 0;
                if (child->node_type && child->node_type->kind == TYPE_ACTOR_REF && expr->value &&
                    !gen->current_actor && !gen->generating_lvalue) {
                    /* #2466: an atomic_load only for a field the actor struct
                     * declares atomic, decided by its type, not its name. */
                    const Type* at = child->node_type->element_type;
                    needs_atomic = at && at->kind == TYPE_STRUCT &&
                        actor_state_field_is_atomic(gen, at->struct_name, expr->value);
                }

                if (needs_atomic) {
                    fprintf(gen->output, "atomic_load(&");
                    generate_expression(gen, child);
                    fprintf(gen->output, "->%s)", expr->value);
                } else if (child->node_type && child->node_type->kind == TYPE_ACTOR_REF) {
                    generate_expression(gen, child);
                    fprintf(gen->output, "->%s", expr->value);
                } else if (child->node_type && child->node_type->kind == TYPE_PTR &&
                           child->node_type->element_type &&
                           child->node_type->element_type->kind == TYPE_STRUCT) {
                    /* Pointer-to-struct (`*StructName`) — emit `->field`.
                     * Produced by `expr as *StructName`, by `*T` type
                     * annotations on locals/params, and by struct fields
                     * of pointer-to-struct type. */
                    generate_expression(gen, child);
                    fprintf(gen->output, "->%s", expr->value);
                } else if (child->type == AST_IDENTIFIER && child->value &&
                           (!child->node_type ||
                            child->node_type->kind == TYPE_UNKNOWN) &&
                           name_is_ptr_struct_cast_local(gen, child->value)) {
                    /* #datastar-10: an `as *Struct` local whose type did not
                     * survive into this context (a struct-literal initialiser
                     * is the case that bit). It is still a pointer. */
                    generate_expression(gen, child);
                    fprintf(gen->output, "->%s", expr->value);
                } else {
                    generate_expression(gen, child);
                    fprintf(gen->output, ".%s", expr->value);
                }
            }
            break;

        case AST_PTR_AS_STRUCT_CAST:
            /* `expr as *StructName` — emit `((StructName*)(expr))`.
             * The result is consumed by member-access codegen above,
             * which dispatches on TYPE_PTR{element=TYPE_STRUCT} and
             * emits `->field`.
             *
             * For `@c_import` structs (no aetherc-emitted typedef),
             * use `struct StructName*` instead — bare `StructName*`
             * fails for headers that don't ship `typedef struct N N;`. */
            if (expr->child_count > 0 && expr->value) {
                if (aether_is_c_struct_overlay(expr->value)) {
                    /* #891: a @c_struct overlay has NO C struct type — it's a
                     * pure-offset lens. The cast is just the raw pointer;
                     * member access lowers to mem_get_* / set_* at offsets. */
                    fprintf(gen->output, "((void*)(");
                } else if (aether_is_c_import_struct(expr->value)) {
                    fprintf(gen->output, "((struct %s*)(", expr->value);
                } else {
                    fprintf(gen->output, "((%s*)(", expr->value);
                }
                generate_expression(gen, expr->children[0]);
                fprintf(gen->output, "))");
            }
            break;

        case AST_SLICE_EXPR: {
            /* #1286 `s[lo..hi]` — a checked sub-slice sharing the backing
             * store; `[..]` alone is the whole thing as a slice. */
            if (expr->child_count < 1 || !expr->value) break;
            ASTNode* base = expr->children[0];
            const char* shape = expr->value;
            const char* elem = slice_elem_c_type(expr->node_type);
            ASTNode* lo = NULL; ASTNode* hi = NULL;
            if (strcmp(shape, "lo..hi") == 0) { lo = expr->children[1]; hi = expr->children[2]; }
            else if (strcmp(shape, "lo..") == 0) { lo = expr->children[1]; }
            else if (strcmp(shape, "..hi") == 0) { hi = expr->children[1]; }
            if (!lo && !hi) {
                generate_expression_as_slice(gen, base);
                break;
            }
            fprintf(gen->output, hi ? "aether_slice_sub(" : "aether_slice_from(");
            generate_expression_as_slice(gen, base);
            fprintf(gen->output, ", (int64_t)(");
            if (lo) generate_expression(gen, lo); else fprintf(gen->output, "0");
            fprintf(gen->output, ")");
            if (hi) {
                fprintf(gen->output, ", (int64_t)(");
                generate_expression(gen, hi);
                fprintf(gen->output, ")");
            }
            fprintf(gen->output, ", sizeof(%s), ", elem);
            emit_slice_diag_file(gen, expr);
            fprintf(gen->output, ", %d)", expr->line);
            break;
        }

        case AST_SLICE_FROM_ARRAY:
            /* #1286: a `T[N]` array (or `null`, length 0) into a `T[]` slot.
             * An array LITERAL becomes a C compound literal, which lives as
             * long as the enclosing block, the same as the `T[N]` it is. */
            if (expr->child_count > 0) {
                ASTNode* src = expr->children[0];
                fprintf(gen->output, "aether_slice_make((void*)(");
                if (src->type == AST_ARRAY_LITERAL && src->child_count > 0)
                    fprintf(gen->output, "(%s[])", slice_elem_c_type(expr->node_type));
                generate_expression(gen, src);
                fprintf(gen->output, "), %s)", expr->value ? expr->value : "0");
            }
            break;

        case AST_SLICE_TO_PTR:
            /* #1286: a slice into a raw-pointer slot — its element pointer. */
            if (expr->child_count > 0) generate_expression_as_elem_ptr(gen, expr->children[0]);
            break;

        case AST_PTR_AS_ARRAY_CAST:
            /* `expr as T[]` — an unbounded slice VIEW over a raw pointer
             * (#1286): `aether_slice_view((void*)(expr))`. Indexing it is
             * unchecked, as it always was for this cast; `.len` is -1;
             * sub-slicing with an explicit end bounds it. No allocation.
             * Same systems-programming escape hatch as `as *StructName`,
             * just at a buffer-element granularity. */
            if (expr->child_count > 0 && expr->node_type &&
                expr->node_type->kind == TYPE_ARRAY &&
                expr->node_type->element_type) {
                fprintf(gen->output, "aether_slice_view((void*)(");
                generate_expression(gen, expr->children[0]);
                fprintf(gen->output, "))");
            }
            break;

        case AST_VALUE_CAST:
            /* `expr as T` (#480) — zero-cost nominal (un)wrap or numeric
             * conversion. Emit a plain C cast to the target's machine type;
             * for a distinct target, node_type->kind is the base kind, so
             * get_c_type yields the base C type (no runtime cost). */
            if (expr->child_count > 0 && expr->node_type) {
                fprintf(gen->output, "((%s)(", get_c_type(expr->node_type));
                /* #2304: `s as ptr` on a slice is its element pointer, as a
                 * slice passed to a `ptr` slot is; a C cast of the
                 * AetherSlice struct itself does not compile. */
                if (expr->node_type->kind == TYPE_PTR)
                    generate_expression_as_elem_ptr(gen, expr->children[0]);
                else
                    generate_expression(gen, expr->children[0]);
                fprintf(gen->output, "))");
            } else if (expr->child_count > 0) {
                generate_expression(gen, expr->children[0]);
            }
            break;

        case AST_PTR_AS_FN_CAST:
            /* `expr as fn(T1, T2, ...) -> R` — at this node we emit
             * just `((void*)(expr))`.  The signature is carried on
             * expr->node_type (TYPE_FUNCTION with is_fnptr=1) and is
             * consulted at the CALL site (AST_FUNCTION_CALL) to emit
             * the typed C function-pointer cast around the invocation.
             * This split keeps storage of fn-pointer locals uniform
             * (`void*`) regardless of signature, so locals/params can
             * be reassigned across compatible signatures without
             * C-side typedef churn. */
            if (expr->child_count > 0) {
                fprintf(gen->output, "((void*)(");
                generate_expression(gen, expr->children[0]);
                fprintf(gen->output, "))");
            }
            break;
            
        case AST_BINARY_EXPRESSION:
            if (expr->child_count >= 2 &&
                ((expr->node_type && expr->node_type->kind == TYPE_FLOAT32) ||
                 (expr->children[0]->node_type && expr->children[0]->node_type->kind == TYPE_FLOAT32) ||
                 (expr->children[1]->node_type && expr->children[1]->node_type->kind == TYPE_FLOAT32)) &&
                expr->value && (strcmp(expr->value, "+") == 0 || strcmp(expr->value, "-") == 0 ||
                                strcmp(expr->value, "*") == 0 || strcmp(expr->value, "/") == 0 ||
                                strcmp(expr->value, "%") == 0 ||
                                strcmp(expr->value, "<") == 0 || strcmp(expr->value, "<=") == 0 ||
                                strcmp(expr->value, ">") == 0 || strcmp(expr->value, ">=") == 0 ||
                                strcmp(expr->value, "==") == 0 || strcmp(expr->value, "!=") == 0)) {
                /* #2151: an f32 operand makes the operation a C float one,
                 * so a float literal on the other side (bare, or under a
                 * unary minus) is emitted as a float constant. The
                 * typechecker retypes such literals where it types the
                 * expression; this is the authority for every context —
                 * `return v * 0.5` reaches the statement walker and not
                 * infer_binary_type, and a `v * -0.5` there reaches neither
                 * literal rule (the early pass types the unary node as the
                 * literal's double), so the decision is made from the
                 * operands, retyping the unary node with its literal. A comparison
                 * takes it too: `v < 0.5` compares floats. */
                for (int k = 0; k < 2; k++) {
                    ASTNode* opnd = expr->children[k];
                    ASTNode* lit = opnd;
                    if (lit && lit->type == AST_UNARY_EXPRESSION && lit->child_count == 1 &&
                        lit->value && (strcmp(lit->value, "-") == 0 || strcmp(lit->value, "+") == 0))
                        lit = lit->children[0];
                    if (lit && lit->type == AST_LITERAL && lit->node_type &&
                        lit->node_type->kind == TYPE_FLOAT) {
                        lit->node_type->kind = TYPE_FLOAT32;
                        if (opnd != lit && opnd->node_type && opnd->node_type->kind == TYPE_FLOAT)
                            opnd->node_type->kind = TYPE_FLOAT32;
                    }
                }
            }
            /* #2428: an operator on an eight-lane value is a call to its
             * _ae_f32x8_* / _ae_i32x8_* helper, never C's operator: without
             * AVX2 the type is a struct of two halves, which C's operators do
             * not apply to (see the typedefs in codegen.c). With AVX2 the
             * helper is the operator. A scalar operand splats, as it does for
             * the four-lane types, which keep C's operators: their width is
             * native on every target. */
            if (expr->child_count >= 2 && expr->value) {
                static const char* const lane8_ops[11][2] = {
                    {"+", "add"}, {"-", "sub"}, {"*", "mul"}, {"/", "div"}, {"%", "mod"},
                    {"<", "lt"}, {"<=", "le"}, {">", "gt"}, {">=", "ge"}, {"==", "eq"}, {"!=", "ne"}
                };
                const char* lane8_fn = NULL;
                for (int k = 0; k < 11; k++)
                    if (strcmp(expr->value, lane8_ops[k][0]) == 0) lane8_fn = lane8_ops[k][1];
                Type* lt8 = expr->children[0]->node_type;
                Type* rt8 = expr->children[1]->node_type;
                TypeKind vk8 = TYPE_UNKNOWN;
                if (lt8 && (lt8->kind == TYPE_F32X8 || lt8->kind == TYPE_I32X8)) vk8 = lt8->kind;
                else if (rt8 && (rt8->kind == TYPE_F32X8 || rt8->kind == TYPE_I32X8)) vk8 = rt8->kind;
                if (lane8_fn && vk8 != TYPE_UNKNOWN) {
                    const char* pfx = vk8 == TYPE_F32X8 ? "f32x8" : "i32x8";
                    const char* scalar_cast = vk8 == TYPE_F32X8 ? "float" : "int";
                    fprintf(gen->output, "_ae_%s_%s(", pfx, lane8_fn);
                    for (int k = 0; k < 2; k++) {
                        Type* ot = expr->children[k]->node_type;
                        if (k) fprintf(gen->output, ", ");
                        if (ot && ot->kind == vk8) {
                            generate_expression(gen, expr->children[k]);
                        } else {
                            fprintf(gen->output, "_ae_%s_splat((%s)(", pfx, scalar_cast);
                            generate_expression(gen, expr->children[k]);
                            fprintf(gen->output, "))");
                        }
                    }
                    fprintf(gen->output, ")");
                    break;
                }
            }
            if (expr->child_count >= 2) {
                // #1046 bit_set operators lower to bitwise ops on the backing
                // `unsigned long long`. Handled before the generic paths since a
                // bit_set must never fall through to numeric `<=`/`-` semantics.
                // `==`/`!=` are left to the default integer compare (which is
                // exactly set equality). Subset/superset bind each operand once
                // via a statement-expression so a side-effecting operand (e.g. a
                // call returning a set) is evaluated exactly once.
                if (expr->value) {
                    ASTNode* L = expr->children[0];
                    ASTNode* R = expr->children[1];
                    int l_bs = L->node_type && L->node_type->kind == TYPE_BITSET;
                    int r_bs = R->node_type && R->node_type->kind == TYPE_BITSET;
                    if (strcmp(expr->value, "in") == 0 && r_bs) {
                        // member in set -> test the member's bit
                        fprintf(gen->output, "((((");
                        generate_expression(gen, R);
                        fprintf(gen->output, ") >> (");
                        generate_expression(gen, L);
                        fprintf(gen->output, ")) & 1ULL) != 0)");
                        break;
                    }
                    if (l_bs || r_bs) {
                        if (strcmp(expr->value, "+") == 0) {          // union
                            fprintf(gen->output, "(");
                            generate_expression(gen, L);
                            fprintf(gen->output, " | ");
                            generate_expression(gen, R);
                            fprintf(gen->output, ")");
                            break;
                        }
                        if (strcmp(expr->value, "-") == 0) {          // difference
                            fprintf(gen->output, "(");
                            generate_expression(gen, L);
                            fprintf(gen->output, " & ~(");
                            generate_expression(gen, R);
                            fprintf(gen->output, "))");
                            break;
                        }
                        if (strcmp(expr->value, "<=") == 0 ||
                            strcmp(expr->value, ">=") == 0) {         // subset/superset
                            int subset = strcmp(expr->value, "<=") == 0;
                            fprintf(gen->output, "({ unsigned long long _a = (");
                            generate_expression(gen, L);
                            fprintf(gen->output, "); unsigned long long _b = (");
                            generate_expression(gen, R);
                            fprintf(gen->output, "); (_a & _b) == %s; })", subset ? "_a" : "_b");
                            break;
                        }
                    }
                }
                // #340: equality against `none` / between optionals. A struct
                // `==` is invalid C, so compare the `has` flag (and value).
                if (expr->value && (strcmp(expr->value, "==") == 0 ||
                                    strcmp(expr->value, "!=") == 0)) {
                    ASTNode* L = expr->children[0];
                    ASTNode* R = expr->children[1];
                    int l_none = L->type == AST_NONE_LITERAL;
                    int r_none = R->type == AST_NONE_LITERAL;
                    int l_opt  = L->node_type && L->node_type->kind == TYPE_OPTIONAL;
                    int r_opt  = R->node_type && R->node_type->kind == TYPE_OPTIONAL;
                    int is_eq  = strcmp(expr->value, "==") == 0;
                    if ((l_opt && r_none) || (r_opt && l_none)) {
                        // `x == none` -> !x.has ;  `x != none` -> x.has
                        ASTNode* opt = l_opt ? L : R;
                        fprintf(gen->output, "(%s(", is_eq ? "!" : "");
                        generate_expression(gen, opt);
                        fprintf(gen->output, ").has)");
                        break;
                    }
                    if (l_opt && r_opt) {
                        // equal iff same presence and (when present) equal value.
                        // The value compare must match the element type: a plain
                        // C `==` is right for scalars but a POINTER compare for
                        // `string?` — two distinct string objects with equal
                        // bytes would wrongly test unequal (and, since a
                        // string-valued `.val` is `const char*` carrying an
                        // AetherString header, `==` isn't even a meaningful C
                        // comparison). For string element types, compare by
                        // length and bytes through string_equals, exactly as the
                        // ordinary string-comparison path below does (#2515).
                        Type* elem = L->node_type ? L->node_type->element_type : NULL;
                        int str_val = elem && elem->kind == TYPE_STRING;
                        fprintf(gen->output, "(({ %s _l = ", get_c_type(L->node_type));
                        generate_expression(gen, L);
                        fprintf(gen->output, "; %s _r = ", get_c_type(R->node_type));
                        generate_expression(gen, R);
                        if (str_val) {
                            fprintf(gen->output,
                                "; %s(_l.has == _r.has && (!_l.has || "
                                "string_equals(_l.val, _r.val))); }))",
                                is_eq ? "" : "!");
                        } else {
                            fprintf(gen->output,
                                "; %s(_l.has == _r.has && (!_l.has || _l.val == _r.val)); }))",
                                is_eq ? "" : "!");
                        }
                        break;
                    }
                }
                int skip_parens = gen->in_condition;
                gen->in_condition = 0;

                int is_assignment = (expr->value && strcmp(expr->value, "=") == 0);

                // String comparison, not pointer ==: by length and bytes
                // (binary_is_string_compare says when, the
                // emit_string_compare_* pair how). A ptr-vs-ptr opaque-
                // handle comparison stays a bare pointer compare (the
                // else-branch below).
                int is_string_cmp = binary_is_string_compare(expr);

                if (is_string_cmp) {
                    /* Operand drain — the binary-operator analogue of the
                     * call-argument drain (ArgDrainSub). A heap-returning
                     * string expression used directly as a comparison
                     * operand (`string.substring(s, i, j) == "lit"`,
                     * `a.concat(b) != c`, an interpolation) is an anonymous
                     * allocation with no owner: evaluated, read by
                     * _aether_safe_str, then leaked. Inside a loop this is
                     * one leak per iteration (observed in test_fs_realpath's
                     * substring-scan). Capture each such operand in a temp,
                     * run the compare, then free it. Gated exactly like the
                     * arg-drain: only AST_FUNCTION_CALL / AST_STRING_INTERP
                     * operands that is_heap_string_expr classifies as
                     * fresh heap — never a tracked local (it owns its own
                     * lifetime), a borrowed return (string.to_cstr), or a
                     * literal. So no double-free is possible. */
                    ASTNode* cl = expr->children[0];
                    ASTNode* cr = expr->children[1];
                    int drain_l = (cl->type == AST_FUNCTION_CALL ||
                                   cl->type == AST_STRING_INTERP) &&
                                  is_heap_string_expr(gen, cl);
                    int drain_r = (cr->type == AST_FUNCTION_CALL ||
                                   cr->type == AST_STRING_INTERP) &&
                                  is_heap_string_expr(gen, cr);
                    if (drain_l || drain_r) {
                        if (!skip_parens) fprintf(gen->output, "(");
                        fprintf(gen->output, "({ const char* _se_l = (const char*)(");
                        generate_expression(gen, cl);
                        fprintf(gen->output, "); const char* _se_r = (const char*)(");
                        generate_expression(gen, cr);
                        fprintf(gen->output, "); int _se_v = ");
                        emit_string_compare_open(gen, expr->value);
                        fprintf(gen->output, "_se_l, _se_r");
                        emit_string_compare_close(gen, expr->value);
                        fprintf(gen->output, "; ");
                        if (drain_l) fprintf(gen->output, "aether_heap_str_free(_se_l); ");
                        if (drain_r) fprintf(gen->output, "aether_heap_str_free(_se_r); ");
                        fprintf(gen->output, "_se_v; })");
                        if (!skip_parens) fprintf(gen->output, ")");
                    } else {
                        if (!skip_parens) fprintf(gen->output, "(");
                        emit_string_compare_open(gen, expr->value);
                        generate_expression(gen, expr->children[0]);
                        fprintf(gen->output, ", ");
                        generate_expression(gen, expr->children[1]);
                        emit_string_compare_close(gen, expr->value);
                        if (!skip_parens) fprintf(gen->output, ")");
                    }
                } else if (is_assignment && expr->children[0] &&
                           expr->children[0]->type == AST_MEMBER_ACCESS &&
                           aether_c_struct_overlay_lhs(expr->children[0])) {
                    /* #891 @c_struct overlay write (incl. nested `s.a.b = v`):
                     * flatten to overlay-pointer root + dotted path, emit
                     * aether_mem_set_<width> at the cumulative offset. The
                     * parser lands a member-access store as a binary-`=`. */
                    const char* cpath = NULL;
                    ASTNode* root = aether_c_struct_chain(expr->children[0], &cpath);
                    const char* sname = root->node_type->element_type->struct_name;
                    long off = 0; const char* width = NULL;
                    if (!skip_parens) fprintf(gen->output, "(");
                    if (aether_c_struct_resolve(sname, cpath, &off, &width) && width) {
                        fprintf(gen->output, "aether_mem_set_%s((void*)(", width);
                        generate_expression(gen, root);
                        fprintf(gen->output, "), %ld, ", off);
                        generate_expression(gen, expr->children[1]);
                        fprintf(gen->output, ")");
                    } else {
                        fprintf(gen->output, "/* @c_struct: unknown field %s.%s */0",
                                sname, cpath);
                    }
                    if (!skip_parens) fprintf(gen->output, ")");
                } else if (is_assignment && expr->children[0] &&
                           expr->children[0]->type == AST_MEMBER_ACCESS &&
                           expr->children[0]->value &&
                           aether_bitstruct_base_name(expr->children[0])) {
                    /* #1132 bitstruct field write as an EXPRESSION. The parser
                     * lands a member-access store as a binary-`=`, so this is the
                     * path an ordinary `b.f = v` statement actually takes (the
                     * AST_ASSIGNMENT site in codegen_stmt.c catches the other
                     * shape). Read-modify-write on the backing word:
                     *   (b = (b & ~(mask << lo)) | ((v & mask) << lo))
                     * The RHS is masked BEFORE shifting so an over-wide value
                     * truncates to its own field instead of corrupting the
                     * neighbours. */
                    ASTNode* macc = expr->children[0];
                    ASTNode* base = macc->children[0];
                    const char* bname = aether_bitstruct_base_name(macc);
                    int lo = 0, hi = 0, is_bool = 0;
                    const char* backing = NULL;
                    if (!skip_parens) fprintf(gen->output, "(");
                    if (aether_bitstruct_resolve(bname, macc->value, &lo, &hi,
                                                 &is_bool, &backing)) {
                        unsigned long long mask = aether_bitstruct_mask(lo, hi);
                        generate_expression(gen, base);
                        fprintf(gen->output, " = (%s)((",
                                backing ? backing : "unsigned char");
                        generate_expression(gen, base);
                        fprintf(gen->output, " & ~(0x%llxULL << %d)) | ((((unsigned long long)(",
                                mask, lo);
                        generate_expression(gen, expr->children[1]);
                        fprintf(gen->output, ")) & 0x%llxULL) << %d))", mask, lo);
                    } else {
                        fprintf(gen->output, "/* bitstruct: unknown field %s.%s */0",
                                bname, macc->value);
                    }
                    if (!skip_parens) fprintf(gen->output, ")");
                } else {
                    if (!skip_parens) fprintf(gen->output, "(");

                    // Detect ptr/int mixed comparisons and cast ptr to intptr_t
                    // to suppress -Wpointer-integer-compare warnings.
                    // Common case: list.get() returns void*, compared to int literal.
                    int is_comparison = expr->value && (
                        strcmp(expr->value, "==") == 0 || strcmp(expr->value, "!=") == 0 ||
                        strcmp(expr->value, "<") == 0  || strcmp(expr->value, ">") == 0  ||
                        strcmp(expr->value, "<=") == 0 || strcmp(expr->value, ">=") == 0);
                    Type* ltype = expr->children[0]->node_type;
                    Type* rtype = expr->children[1]->node_type;
                    int lhs_is_ptr = ltype && ltype->kind == TYPE_PTR;
                    int rhs_is_ptr = rtype && rtype->kind == TYPE_PTR;
                    int lhs_is_int = ltype && (ltype->kind == TYPE_INT || ltype->kind == TYPE_INT64);
                    int rhs_is_int = rtype && (rtype->kind == TYPE_INT || rtype->kind == TYPE_INT64);
                    int ptr_int_cmp = is_comparison && ((lhs_is_ptr && rhs_is_int) || (rhs_is_ptr && lhs_is_int));

                    if (is_assignment) {
                        gen->generating_lvalue = 1;
                    }
                    int duration_ratio = expr->value && strcmp(expr->value, "/") == 0 &&
                        ltype && rtype && ltype->kind == TYPE_DURATION && rtype->kind == TYPE_DURATION;
                    /* #697: a 64-bit integer arithmetic/bitwise/shift op whose
                     * operand is a narrower 32-bit int must compute in 64-bit,
                     * or C promotes the operand in 32-bit and sign-extends it
                     * into the high half (e.g. `byte << 24` polluting a uint64).
                     * The typechecker (propagate_int_width_64) already re-typed
                     * computed sub-expressions; here we cast narrow leaf/value
                     * operands at the use site. Not for assignment (the `=`
                     * conversion is handled by the LHS type) or comparisons
                     * (bool result). */
                    const char* wide_cast = NULL;
                    if (!is_assignment && expr->node_type &&
                        (expr->node_type->kind == TYPE_INT64 ||
                         expr->node_type->kind == TYPE_UINT64)) {
                        wide_cast = (expr->node_type->kind == TYPE_UINT64)
                                    ? "(uint64_t)" : "(int64_t)";
                    }
                    #define AE_IS_NARROW_INT(t) ((t) && ((t)->kind == TYPE_INT || \
                        (t)->kind == TYPE_BYTE || (t)->kind == TYPE_UINT32 || \
                        (t)->kind == TYPE_UINT16 || (t)->kind == TYPE_UINT8))
                    int left_prefixed = duration_ratio || (ptr_int_cmp && lhs_is_ptr) ||
                                        (wide_cast && AE_IS_NARROW_INT(ltype));
                    if (duration_ratio) fprintf(gen->output, "(double)");
                    if (ptr_int_cmp && lhs_is_ptr) fprintf(gen->output, "(intptr_t)");
                    if (wide_cast && AE_IS_NARROW_INT(ltype)) fprintf(gen->output, "%s", wide_cast);
                    /* A chain link keeps its own brackets only when a cast
                     * was prefixed to it, so the cast still covers the
                     * whole link. The skip rides the flag an if/while
                     * condition uses for the same purpose. */
                    if (!is_assignment && !left_prefixed && left_operand_is_chain_link(expr)) {
                        gen->in_condition = 1;
                    }
                    generate_expression(gen, expr->children[0]);
                    gen->in_condition = 0;
                    if (is_assignment) {
                        gen->generating_lvalue = 0;
                    }

                    fprintf(gen->output, " %s ", get_c_operator(expr->value));
                    if (duration_ratio) fprintf(gen->output, "(double)");
                    if (ptr_int_cmp && rhs_is_ptr) fprintf(gen->output, "(intptr_t)");
                    /* fn ↔ ptr coercion at struct-field assignment.
                     * Mirror of the call-site coercion path. The
                     * parser lands `h.cb = c` as
                     * AST_BINARY_EXPRESSION (op="="). When the LHS
                     * is a `ptr`-typed value and the RHS is a `fn`-
                     * shaped value (is_fnptr=0) or a bare named
                     * function, wrap the RHS in
                     * _aether_box_closure(...) so the closure
                     * round-trips through the field with the env
                     * slot intact. */
                    int assign_box_struct = 0;
                    int assign_box_bare_fn = 0;
                    /* #1240: a C-owned struct's field is the exception to all
                     * the boxing below. C calls through it directly, so it gets
                     * the function's real address, cast to whatever type the
                     * header declared for that field. __typeof__ names that type
                     * exactly, which no cast synthesised from the Aether side
                     * can do (`ptr` is not `const void*`), and it does not
                     * evaluate its operand, so re-emitting the LHS inside it is
                     * side-effect free. */
                    int assign_c_fnptr_field =
                        is_assignment &&
                        bare_top_level_fn(gen, expr->children[1]) != NULL &&
                        member_field_is_c_owned(gen, expr->children[0]);
                    if (is_assignment && lhs_is_ptr && !assign_c_fnptr_field) {
                        if (rtype && rtype->kind == TYPE_FUNCTION && !rtype->is_fnptr) {
                            assign_box_struct = 1;
                        } else if (bare_top_level_fn(gen, expr->children[1])) {
                            assign_box_bare_fn = 1;
                        }
                    }
                    if (assign_c_fnptr_field) {
                        fprintf(gen->output, "(__typeof__(");
                        generate_expression(gen, expr->children[0]);
                        fprintf(gen->output, "))");
                        generate_expression(gen, expr->children[1]);
                    } else if (assign_box_struct) {
                        fprintf(gen->output, "_aether_box_closure(");
                        generate_expression(gen, expr->children[1]);
                        fprintf(gen->output, ")");
                    } else if (assign_box_bare_fn) {
                        /* Register an env-ignoring adapter for this
                         * bare fn so the wrap embeds the adapter
                         * address (not the bare fn's address) into
                         * .fn. See ASK 3 in aether/new_aevg_asks.md. */
                        const char* bn = expr->children[1] && expr->children[1]->value
                                         ? expr->children[1]->value : NULL;
                        if (bn) register_bare_fn_adapter(gen, bn);
                        fprintf(gen->output,
                                "_aether_box_closure((_AeClosure){ .fn = (void(*)(void))_aether_bare_adapter_%s, .env = NULL })",
                                bn ? bn : "unknown");
                    } else {
                        if (wide_cast && AE_IS_NARROW_INT(rtype)) fprintf(gen->output, "%s", wide_cast);
                        generate_expression(gen, expr->children[1]);
                    }
                    #undef AE_IS_NARROW_INT
                    if (!skip_parens) fprintf(gen->output, ")");
                }
            }
            break;
            
        case AST_UNARY_EXPRESSION:
            /* #2428: `-v` / `~m` on an eight-lane value, through its helper
             * (the halves form is a struct; see the binary case). */
            if (expr->child_count >= 1 && expr->value && expr->children[0]->node_type &&
                (expr->children[0]->node_type->kind == TYPE_F32X8 ||
                 expr->children[0]->node_type->kind == TYPE_I32X8) &&
                (strcmp(expr->value, "-") == 0 || strcmp(expr->value, "~") == 0)) {
                const char* pfx = expr->children[0]->node_type->kind == TYPE_F32X8 ? "f32x8" : "i32x8";
                fprintf(gen->output, "_ae_%s_%s(", pfx, expr->value[0] == '-' ? "neg" : "not");
                generate_expression(gen, expr->children[0]);
                fprintf(gen->output, ")");
                break;
            }
            /* #2457: postfix `i++` / `i--` yields the old value, so it must
             * reach C as postfix too, not as the prefix form. */
            if (expr->child_count >= 1 && annotation_has_marker(expr->annotation, "postfix")) {
                fprintf(gen->output, "((");
                generate_expression(gen, expr->children[0]);
                fprintf(gen->output, ")%s)", get_c_operator(expr->value));
                break;
            }
            if (expr->child_count >= 1) {
                // Wrap the entire unary expression in parens: (!x) not !(x).
                // This prevents GCC -Wlogical-not-parentheses when the unary
                // result is compared: (!x) != y  instead of  !x != y.
                fprintf(gen->output, "(%s(", get_c_operator(expr->value));
                generate_expression(gen, expr->children[0]);
                fprintf(gen->output, "))");
            }
            break;
            
        case AST_FUNCTION_CALL:
            /* `sandbox.enforce(perms, foo) { ... }` with trusted names: tag the
             * push the call is about to make with its site, so a trusted call
             * in the block can find the level to drop back to. The args are
             * evaluated after the tag is set, but only a push consumes it, and
             * enforce's own push is the first one it reaches. */
            if (gen->uses_sandbox && expr != g_sandbox_site_wrapping) {
                int site = sandbox_trust_site_of_enforce(expr);
                if (site) {
                    const ASTNode* saved_site = g_sandbox_site_wrapping;
                    g_sandbox_site_wrapping = expr;
                    fprintf(gen->output, "(_aether_sandbox_pending_site = %d, ", site);
                    generate_expression(gen, expr);
                    fprintf(gen->output, ")");
                    g_sandbox_site_wrapping = saved_site;
                    break;
                }
            }
            if (expr != gen->trailing_stmt_call && emit_trailing_call_expression(gen, expr)) {
                break;
            }
            /* heap.free(p) — counterpart to heap.new(T) (issue #564, #790).
             * A POD box owns no heap fields, so a plain free(p) reclaims it.
             * A box whose struct has string fields (#790) routes through the
             * generated `<Name>_heap_free`, which releases every owned field
             * then frees the box. NULL-safe either way (free(NULL) is a no-op;
             * the typed free early-returns on NULL). One positional arg. */
            if (expr->value && strcmp(expr->value, "heap.free") == 0 &&
                expr->child_count == 1) {
                ASTNode* arg = expr->children[0];
                const char* typed_free = NULL;
                int observable = 0;
                if (arg && arg->node_type && arg->node_type->kind == TYPE_PTR &&
                    arg->node_type->element_type &&
                    arg->node_type->element_type->kind == TYPE_STRUCT &&
                    arg->node_type->element_type->struct_name && gen->program) {
                    const char* sname = arg->node_type->element_type->struct_name;
                    ASTNode* sdef = find_struct_definition_by_name(gen->program, sname);
                    if (sdef && struct_owns_heap_strings(gen, sdef)) typed_free = sname;
                    observable = struct_is_observable(gen, sname);
                }
                if (observable) {
                    /* An @observable box's observers go with it: they hold
                     * closure environments, and an object later allocated at
                     * the same address would inherit them (std.observe). */
                    fprintf(gen->output, "({ void* _ae_hf = (void*)(");
                    generate_expression(gen, arg);
                    fprintf(gen->output, "); aether_unobserve_all(_ae_hf); ");
                    if (typed_free)
                        fprintf(gen->output, "%s_heap_free((%s*)_ae_hf); })", typed_free, typed_free);
                    else
                        fprintf(gen->output, "free(_ae_hf); })");
                    break;
                }
                if (typed_free) {
                    fprintf(gen->output, "%s_heap_free(", typed_free);
                } else {
                    fprintf(gen->output, "free(");
                }
                generate_expression(gen, arg);
                fprintf(gen->output, ")");
                break;
            }
            /* #749: dispatch through a function-pointer struct field.
             * The typechecker tagged `recv.field(args)` calls whose
             * `field` is an fn-ptr member with "fnfield_ptr"/"fnfield_val"
             * (receiver is a pointer-to-struct vs a value struct). Emit
             * the indirect call `(recv->field)(args)` / `(recv.field)(args)`
             * — the field already has a real C fn-ptr type (#749 codegen),
             * so no cast is needed. A `string` argument goes as its bytes
             * (generate_fnptr_call_args, #2210). */
            if (expr->annotation && expr->value &&
                strncmp(expr->annotation, "fnfield_", 8) == 0) {
                int is_ptr = strcmp(expr->annotation, "fnfield_ptr") == 0;
                const char* dot = strrchr(expr->value, '.');
                if (dot) {
                    /* Whole receiver path, however long (#2539). */
                    const char* recv = cg_intern_n(expr->value, (size_t)(dot - expr->value));
                    /* Keyword mangling only (safe_value_name): the receiver
                     * is a local and the field is a struct member, neither is
                     * a linker symbol, so the libc-collision rename in
                     * safe_c_name must not apply. It renamed a field spelled
                     * `read` to `ae_read` here while the struct definition
                     * kept `read`, so the emitted C referenced a member that
                     * does not exist (#1251). */
                    const char* recv_c = safe_value_name(recv);
                    const char* field_c = safe_value_name(dot + 1);
                    Type* field_sig = fnptr_field_signature(gen, recv, dot + 1);
                    int narrow = !gen->discard_call_value && fnptr_returns_bool(field_sig);
                    if (narrow) fprintf(gen->output, "((_Bool)(unsigned char)(");
                    fprintf(gen->output, "(%s%s%s)(",
                            recv_c, is_ptr ? "->" : ".", field_c);
                    generate_fnptr_call_args(gen, field_sig, expr);
                    fprintf(gen->output, ")");
                    if (narrow) fprintf(gen->output, "))");
                    break;
                }
            }
            if (expr->value) {
                const char* func_name = expr->value;
                /* Dotted source callees (`string.seq_free`) normalised to
                 * the underscored C/registry form for handlers that match
                 * stdlib functions by name. */
                const char* func_name_norm =
                    codegen_normalise_callee(func_name);
                /* Capture + clear the discarded-value flag at the top of
                 * the call codegen so it governs THIS call only and never
                 * leaks into nested argument calls (whose values ARE
                 * consumed). When set, the arg-temp drain below treats the
                 * parent as void-yielding so heap inline args still free.
                 * See discard_call_value in codegen.h. */
                int ad_call_discarded = gen->discard_call_value;
                gen->discard_call_value = 0;

                /* Typed fn-pointer local call: `fp(a, b)` where `fp` was
                 * declared as `fn(T1, T2, ...) -> R` (or initialised from
                 * an `expr as fn(...)` cast). Emit a typed C function-pointer
                 * cast around the stored void* so the C compiler sees the
                 * correct signature. The cast and call are inlined here; no
                 * per-signature shim is needed. A `string` argument goes as
                 * its bytes (#2210).
                 *
                 * This must come BEFORE the by-name builtin dispatch below.
                 * The typechecker resolves a call through the innermost
                 * symbol, so a local named `release` or `free` holding a
                 * function pointer shadows the builtin of that name; when
                 * this branch sat after the name chain, codegen lowered the
                 * call as the builtin instead, printed a type error for the
                 * argument, and still emitted a binary (#2211). */
                Type* fnptr_sig = lookup_fnptr_local(gen, func_name);
                if (!fnptr_sig) fnptr_sig = lookup_fnptr_global(gen, func_name);   /* #2200 */
                if (fnptr_sig && fnptr_sig->kind == TYPE_FUNCTION &&
                    fnptr_sig->is_fnptr) {
                    generate_fnptr_local_call(gen, fnptr_sig, func_name, expr, ad_call_discarded);
                    break;
                }

                if (strcmp(func_name, "make") == 0 && expr->node_type && expr->node_type->kind == TYPE_ARRAY) {
                    /* #1286: `make([]T, n)` is a bounded slice over a fresh
                     * zeroed buffer of n elements; `free(s)` releases it. */
                    fprintf(gen->output, "aether_slice_alloc((int64_t)(");
                    if (expr->child_count > 0) generate_expression(gen, expr->children[0]);
                    else fprintf(gen->output, "0");
                    fprintf(gen->output, "), sizeof(%s))", get_c_type(expr->node_type->element_type));
                }
                else if (strcmp(func_name, "typeof") == 0) {
                    fprintf(gen->output, "aether_typeof(");
                    if (expr->child_count > 0) {
                        generate_expression(gen, expr->children[0]);
                    }
                    fprintf(gen->output, ")");
                }
                else if (strcmp(func_name, "is_type") == 0) {
                    fprintf(gen->output, "aether_is_type(");
                    for (int i = 0; i < expr->child_count; i++) {
                        if (i > 0) fprintf(gen->output, ", ");
                        generate_expression(gen, expr->children[i]);
                    }
                    fprintf(gen->output, ")");
                }
                else if (strcmp(func_name, "convert_type") == 0) {
                    fprintf(gen->output, "aether_convert_type(");
                    for (int i = 0; i < expr->child_count; i++) {
                        if (i > 0) fprintf(gen->output, ", ");
                        generate_expression(gen, expr->children[i]);
                    }
                    fprintf(gen->output, ")");
                }
                else if (strcmp(func_name, "print") == 0) {
                    if (expr->child_count == 1 && expr->children[0]->type == AST_STRING_INTERP) {
                        // print("Hello ${name}!") — use printf mode for interp
                        gen->interp_as_printf = 1;
                        generate_expression(gen, expr->children[0]);
                        gen->interp_as_printf = 0;
                    } else
                    if (expr->child_count == 1 && expr->children[0]->node_type) {
                        ASTNode* arg = expr->children[0];
                        Type* arg_type = arg->node_type;

                        if (arg_type->kind == TYPE_INT) {
                            fprintf(gen->output, "printf(\"%%d\", ");
                            generate_expression(gen, arg);
                            fprintf(gen->output, ")");
                        } else if (arg_type->kind == TYPE_UINT32) {
                            /* A uint32 through %d prints values past 2^31 as
                             * negative; uint32_t is unsigned int on every
                             * target, so %u is its conversion. */
                            fprintf(gen->output, "printf(\"%%u\", ");
                            generate_expression(gen, arg);
                            fprintf(gen->output, ")");
                        } else if (arg_type->kind == TYPE_INT64) {
                            fprintf(gen->output, "printf(\"%%lld\", (long long)");
                            generate_expression(gen, arg);
                            fprintf(gen->output, ")");
                        } else if (arg_type->kind == TYPE_UINT64) {
                            fprintf(gen->output, "printf(\"%%llu\", (unsigned long long)");
                            generate_expression(gen, arg);
                            fprintf(gen->output, ")");
                        } else if (arg_type->kind == TYPE_DURATION) {
                            fprintf(gen->output, "printf(\"%%s\", _aether_duration_repr(");
                            generate_expression(gen, arg);
                            fprintf(gen->output, "))");
                        } else if (arg_type->kind == TYPE_FLOAT || arg_type->kind == TYPE_FLOAT32) {
                            fprintf(gen->output, "printf(\"%%f\", ");
                            generate_expression(gen, arg);
                            fprintf(gen->output, ")");
                        } else if (arg_type->kind == TYPE_LONGDOUBLE) {
                            fprintf(gen->output, "printf(\"%%Lf\", ");
                            generate_expression(gen, arg);
                            fprintf(gen->output, ")");
                        } else if (arg_type->kind == TYPE_STRING) {
                            if (arg->type == AST_LITERAL) {
                                // String literal — never NULL, use printf directly
                                fprintf(gen->output, "printf(");
                                generate_expression(gen, arg);
                                fprintf(gen->output, ")");
                            } else {
                                // Runtime string, could be NULL; by length (#2521)
                                fprintf(gen->output, "_aether_print_str(");
                                generate_expression(gen, arg);
                                fprintf(gen->output, ")");
                            }
                        } else if (arg_type->kind == TYPE_PTR) {
                            // Runtime pointer, could be NULL; by length (#2521)
                            fprintf(gen->output, "_aether_print_str(");
                            generate_expression(gen, arg);
                            fprintf(gen->output, ")");
                        } else if (arg_type->kind == TYPE_BOOL) {
                            fprintf(gen->output, "printf(\"%%s\", ");
                            generate_expression(gen, arg);
                            fprintf(gen->output, " ? \"true\" : \"false\")");
                        } else {
                            fprintf(gen->output, "printf(\"%%d\", ");
                            generate_expression(gen, arg);
                            fprintf(gen->output, ")");
                        }
                    } else if (expr->child_count == 1) {
                        ASTNode* a = expr->children[0];
                        if (a->type == AST_LITERAL && a->node_type && a->node_type->kind == TYPE_STRING) {
                            emit_print_literal_format(gen, a);
                        } else {
                            fprintf(gen->output, "printf(\"%%d\", ");
                            generate_expression(gen, a);
                            fprintf(gen->output, ")");
                        }
                    } else if (expr->child_count >= 2 && expr->children[0]->type == AST_LITERAL &&
                               expr->children[0]->node_type && expr->children[0]->node_type->kind == TYPE_STRING &&
                               expr->children[0]->value) {
                        // Multi-arg with literal format string: auto-fix specifiers
                        const char* fmt = expr->children[0]->value;
                        fprintf(gen->output, "printf(\"");
                        int arg_idx = 1;
                        for (int fi = 0; fmt[fi]; fi++) {
                            if (fmt[fi] == '%' && fmt[fi + 1]) {
                                fi++;
                                while (fmt[fi] == '-' || fmt[fi] == '+' || fmt[fi] == ' ' ||
                                       fmt[fi] == '#' || fmt[fi] == '0') fi++;
                                while (fmt[fi] >= '0' && fmt[fi] <= '9') fi++;
                                if (fmt[fi] == '.') { fi++; while (fmt[fi] >= '0' && fmt[fi] <= '9') fi++; }
                                if (fmt[fi] == '%') {
                                    fprintf(gen->output, "%%%%");
                                } else if (arg_idx < expr->child_count) {
                                    Type* atype = expr->children[arg_idx]->node_type;
                                    if (atype && atype->kind == TYPE_LONGDOUBLE) fprintf(gen->output, "%%Lf");
                                    else if (atype && (atype->kind == TYPE_FLOAT || atype->kind == TYPE_FLOAT32)) fprintf(gen->output, "%%f");
                                    else if (atype && atype->kind == TYPE_INT64) fprintf(gen->output, "%%lld");
                                    else if (atype && atype->kind == TYPE_UINT32) fprintf(gen->output, "%%u");
                                    else if (atype && atype->kind == TYPE_DURATION) fprintf(gen->output, "%%s");
                                    else if (atype && (atype->kind == TYPE_STRING || atype->kind == TYPE_PTR)) fprintf(gen->output, "%%s");
                                    else if (atype && atype->kind == TYPE_BOOL) fprintf(gen->output, "%%s");
                                    else fprintf(gen->output, "%%d");
                                    arg_idx++;
                                } else {
                                    fprintf(gen->output, "%%%c", fmt[fi]);
                                }
                            } else {
                                switch (fmt[fi]) {
                                    case '\n': fprintf(gen->output, "\\n"); break;
                                    case '\t': fprintf(gen->output, "\\t"); break;
                                    case '\r': fprintf(gen->output, "\\r"); break;
                                    case '\\': fprintf(gen->output, "\\\\"); break;
                                    case '"':  fprintf(gen->output, "\\\""); break;
                                    default:   fprintf(gen->output, "%c", fmt[fi]); break;
                                }
                            }
                        }
                        fprintf(gen->output, "\", ");
                        for (int i = 1; i < expr->child_count; i++) {
                            if (i > 1) fprintf(gen->output, ", ");
                            Type* atype = expr->children[i]->node_type;
                            if (atype && atype->kind == TYPE_INT64) { fprintf(gen->output, "(long long)"); generate_expression(gen, expr->children[i]); }
                            else if (atype && atype->kind == TYPE_DURATION) { fprintf(gen->output, "_aether_duration_repr("); generate_expression(gen, expr->children[i]); fprintf(gen->output, ")"); }
                            else if (atype && atype->kind == TYPE_BOOL) { generate_expression(gen, expr->children[i]); fprintf(gen->output, " ? \"true\" : \"false\""); }
                            else if (atype && (atype->kind == TYPE_STRING || atype->kind == TYPE_PTR)) { fprintf(gen->output, "_aether_safe_str("); generate_expression(gen, expr->children[i]); fprintf(gen->output, ")"); }
                            else generate_expression(gen, expr->children[i]);
                        }
                        fprintf(gen->output, ")");
                    } else {
                        // Non-literal format string — use %s to prevent format injection
                        fprintf(gen->output, "printf(\"%%s\", ");
                        generate_expression(gen, expr->children[0]);
                        fprintf(gen->output, ")");
                    }
                }
                else if (strcmp(func_name, "println") == 0) {
                    // println("...${expr}..."): the interpolation is printed
                    // with its newline in the same format, so the line is
                    // one write under the stream's lock (#2521).
                    if (expr->child_count == 1 && expr->children[0]->type == AST_STRING_INTERP) {
                        gen->interp_as_printf = 2;
                        generate_expression(gen, expr->children[0]);
                        gen->interp_as_printf = 0;
                    } else
                    if (expr->child_count == 1 && expr->children[0]->node_type) {
                        ASTNode* arg = expr->children[0];
                        Type* arg_type = arg->node_type;
                        if (arg_type->kind == TYPE_INT) {
                            fprintf(gen->output, "printf(\"%%d\\n\", ");
                            generate_expression(gen, arg);
                            fprintf(gen->output, ")");
                        } else if (arg_type->kind == TYPE_UINT32) {
                            fprintf(gen->output, "printf(\"%%u\\n\", ");
                            generate_expression(gen, arg);
                            fprintf(gen->output, ")");
                        } else if (arg_type->kind == TYPE_INT64) {
                            fprintf(gen->output, "printf(\"%%lld\\n\", (long long)");
                            generate_expression(gen, arg);
                            fprintf(gen->output, ")");
                        } else if (arg_type->kind == TYPE_UINT64) {
                            fprintf(gen->output, "printf(\"%%llu\\n\", (unsigned long long)");
                            generate_expression(gen, arg);
                            fprintf(gen->output, ")");
                        } else if (arg_type->kind == TYPE_DURATION) {
                            fprintf(gen->output, "printf(\"%%s\\n\", _aether_duration_repr(");
                            generate_expression(gen, arg);
                            fprintf(gen->output, "))");
                        } else if (arg_type->kind == TYPE_FLOAT || arg_type->kind == TYPE_FLOAT32) {
                            fprintf(gen->output, "printf(\"%%f\\n\", ");
                            generate_expression(gen, arg);
                            fprintf(gen->output, ")");
                        } else if (arg_type->kind == TYPE_STRING) {
                            if (arg->type == AST_LITERAL && arg->value_len > 0) {
                                /* Every byte, NULs included (#2520), and
                                 * the newline in the same write (#2521). */
                                emit_string_literal_write(gen, arg, 1);
                            } else if (arg->type == AST_LITERAL) {
                                // String literal — never NULL, use puts() directly
                                fprintf(gen->output, "puts(");
                                generate_expression(gen, arg);
                                fprintf(gen->output, ")");
                            } else if (arg->type == AST_FUNCTION_CALL &&
                                       is_heap_string_expr(gen, arg)) {
                                /* Heap-producing call in bare argument
                                 * position: the temporary is owned by no
                                 * binding, print-and-free via the owned
                                 * helper or it leaks per call (#1331
                                 * review). Identifiers stay on the plain
                                 * path; scope exit owns their free. */
                                fprintf(gen->output, "_aether_println_owned(");
                                generate_expression(gen, arg);
                                fprintf(gen->output, ")");
                            } else {
                                // Runtime string, could be NULL; by length (#2521)
                                fprintf(gen->output, "_aether_println_str(");
                                generate_expression(gen, arg);
                                fprintf(gen->output, ")");
                            }
                        } else if (arg_type->kind == TYPE_PTR) {
                            // Runtime pointer, could be NULL; by length (#2521)
                            fprintf(gen->output, "_aether_println_str(");
                            generate_expression(gen, arg);
                            fprintf(gen->output, ")");
                        } else if (arg_type->kind == TYPE_BOOL) {
                            fprintf(gen->output, "printf(\"%%s\\n\", ");
                            generate_expression(gen, arg);
                            fprintf(gen->output, " ? \"true\" : \"false\")");
                        } else {
                            fprintf(gen->output, "printf(\"%%d\\n\", ");
                            generate_expression(gen, arg);
                            fprintf(gen->output, ")");
                        }
                    } else if (expr->child_count == 1) {
                        ASTNode* a = expr->children[0];
                        if (a->type == AST_LITERAL && a->node_type && a->node_type->kind == TYPE_STRING) {
                            // println("text") → puts("text") which adds \n automatically
                            fprintf(gen->output, "puts(");
                            generate_expression(gen, a);
                            fprintf(gen->output, ")");
                        } else {
                            fprintf(gen->output, "printf(\"%%d\\n\", ");
                            generate_expression(gen, a);
                            fprintf(gen->output, ")");
                        }
                    } else if (expr->child_count == 0) {
                        fprintf(gen->output, "putchar('\\n')");
                    } else if (expr->child_count >= 2 && expr->children[0]->type == AST_LITERAL &&
                               expr->children[0]->node_type && expr->children[0]->node_type->kind == TYPE_STRING &&
                               expr->children[0]->value) {
                        // Multi-arg with literal format: auto-fix specifiers + newline
                        const char* fmt = expr->children[0]->value;
                        fprintf(gen->output, "printf(\"");
                        int arg_idx = 1;
                        for (int fi = 0; fmt[fi]; fi++) {
                            if (fmt[fi] == '%' && fmt[fi + 1]) {
                                fi++;
                                while (fmt[fi] == '-' || fmt[fi] == '+' || fmt[fi] == ' ' ||
                                       fmt[fi] == '#' || fmt[fi] == '0') fi++;
                                while (fmt[fi] >= '0' && fmt[fi] <= '9') fi++;
                                if (fmt[fi] == '.') { fi++; while (fmt[fi] >= '0' && fmt[fi] <= '9') fi++; }
                                if (fmt[fi] == '%') {
                                    fprintf(gen->output, "%%%%");
                                } else if (arg_idx < expr->child_count) {
                                    Type* atype = expr->children[arg_idx]->node_type;
                                    if (atype && atype->kind == TYPE_LONGDOUBLE) fprintf(gen->output, "%%Lf");
                                    else if (atype && (atype->kind == TYPE_FLOAT || atype->kind == TYPE_FLOAT32)) fprintf(gen->output, "%%f");
                                    else if (atype && atype->kind == TYPE_INT64) fprintf(gen->output, "%%lld");
                                    else if (atype && atype->kind == TYPE_UINT32) fprintf(gen->output, "%%u");
                                    else if (atype && atype->kind == TYPE_DURATION) fprintf(gen->output, "%%s");
                                    else if (atype && (atype->kind == TYPE_STRING || atype->kind == TYPE_PTR)) fprintf(gen->output, "%%s");
                                    else if (atype && atype->kind == TYPE_BOOL) fprintf(gen->output, "%%s");
                                    else fprintf(gen->output, "%%d");
                                    arg_idx++;
                                } else {
                                    fprintf(gen->output, "%%%c", fmt[fi]);
                                }
                            } else {
                                switch (fmt[fi]) {
                                    case '\n': fprintf(gen->output, "\\n"); break;
                                    case '\t': fprintf(gen->output, "\\t"); break;
                                    case '\r': fprintf(gen->output, "\\r"); break;
                                    case '\\': fprintf(gen->output, "\\\\"); break;
                                    case '"':  fprintf(gen->output, "\\\""); break;
                                    default:   fprintf(gen->output, "%c", fmt[fi]); break;
                                }
                            }
                        }
                        fprintf(gen->output, "\\n\", ");
                        for (int i = 1; i < expr->child_count; i++) {
                            if (i > 1) fprintf(gen->output, ", ");
                            Type* atype = expr->children[i]->node_type;
                            if (atype && atype->kind == TYPE_INT64) { fprintf(gen->output, "(long long)"); generate_expression(gen, expr->children[i]); }
                            else if (atype && atype->kind == TYPE_DURATION) { fprintf(gen->output, "_aether_duration_repr("); generate_expression(gen, expr->children[i]); fprintf(gen->output, ")"); }
                            else if (atype && atype->kind == TYPE_BOOL) { generate_expression(gen, expr->children[i]); fprintf(gen->output, " ? \"true\" : \"false\""); }
                            else if (atype && (atype->kind == TYPE_STRING || atype->kind == TYPE_PTR)) { fprintf(gen->output, "_aether_safe_str("); generate_expression(gen, expr->children[i]); fprintf(gen->output, ")"); }
                            else generate_expression(gen, expr->children[i]);
                        }
                        fprintf(gen->output, ")");
                    } else {
                        // Non-literal format string — use %s to prevent format injection
                        fprintf(gen->output, "printf(\"%%s\\n\", ");
                        generate_expression(gen, expr->children[0]);
                        fprintf(gen->output, ")");
                    }
                }
                else if (strcmp(func_name, "wait_for_idle") == 0) {
                    fprintf(gen->output, "scheduler_wait()");
                }
                else if (strcmp(func_name, "sleep") == 0 && expr->child_count == 1) {
                    // Route through the runtime's aether_sleep_ms wrapper —
                    // a stable, prefixed symbol that won't collide with
                    // libc's sleep() if user code declares `extern sleep`
                    // for an unrelated binding. See issue #233.
                    fprintf(gen->output, "aether_sleep_ms(");
                    generate_expression(gen, expr->children[0]);
                    fprintf(gen->output, ")");
                }
                else if (strcmp(func_name, "getenv") == 0 && expr->child_count == 1) {
                    fprintf(gen->output, "getenv(");
                    generate_expression(gen, expr->children[0]);
                    fprintf(gen->output, ")");
                }
                /* #1993: the builtin only claims this name while the program
                   has not defined one of its own. `atoi` is an ordinary
                   identifier, and a program that defines it had the definition
                   emitted (mangled, since atoi is a reserved libc name) while
                   every CALL still went to the builtin, so the function was
                   dead code and the call carried libc's signature. A program's
                   own function wins, which is the same rule #1967 settled for
                   types. */
                else if (strcmp(func_name, "atoi") == 0 && expr->child_count == 1 &&
                         !find_function_definition_by_name(gen->program, "atoi")) {
                    fprintf(gen->output, "atoi(");
                    generate_expression(gen, expr->children[0]);
                    fprintf(gen->output, ")");
                }
                else if (strcmp(func_name, "exit") == 0) {
                    fprintf(gen->output, "exit(");
                    if (expr->child_count == 1) {
                        generate_expression(gen, expr->children[0]);
                    } else {
                        fprintf(gen->output, "0");
                    }
                    fprintf(gen->output, ")");
                }
                else if (strcmp(func_name, "free") == 0 && expr->child_count == 1) {
                    fprintf(gen->output, "free((void*)");
                    generate_expression_as_elem_ptr(gen, expr->children[0]);   /* #1286 */
                    fprintf(gen->output, ")");
                }
                // release(X) — explicit release sugar for heap strings,
                // for `defer release(body)` after `body, err =
                // http.get(url)` without reaching for std.string.
                //
                // Two lowerings, chosen by what the codegen knows about
                // the argument:
                //
                //  1. A heap-tracked local (is_heap_string_var) is freed
                //     through its runtime ownership flag:
                //         ({ if (_heap_X) { aether_heap_str_free(X);
                //              X = NULL; _heap_X = 0; } })
                //     aether_heap_str_free reclaims BOTH a magic-tagged
                //     AetherString* (string.from_int / concat_wrapped /
                //     fs.read_binary) AND a plain malloc'd char*
                //     (string.concat / substring / to_upper / to_lower /
                //     trim) — the latter is exactly what string_release
                //     alone cannot free, because a plain heap char* is
                //     indistinguishable at runtime from a .rodata
                //     literal. The `_heap_X` guard is what makes freeing
                //     a plain char* safe here: a variable currently
                //     holding a literal has _heap_X == 0, so nothing is
                //     freed (this is what kept `release("literal")` from
                //     crashing — the literal arm below — and equally
                //     protects `s = "lit"` after a heap assignment).
                //     Clearing the flag means the restored function-exit
                //     defer-free (release no longer marks its arg
                //     escaped — see is_nonstoring_builtin) sees _heap_X
                //     == 0 and does not double-free. The reassignment
                //     wrapper frees each prior value, so a `defer
                //     release(s)` accumulator in a loop frees every
                //     iteration's buffer exactly once.
                //
                //  2. A literal / borrowed param / non-tracked
                //     expression falls back to string_release, which is
                //     literal-safe (no-ops unless the value carries the
                //     AetherString magic header).
                else if ((strcmp(func_name, "isolate") == 0 ||
                          strcmp(func_name, "consume") == 0) &&
                         expr->child_count == 1) {
                    /* #479 Isolated[T] is a compile-time-only, move-only
                     * wrapper. isolate() and consume() are both the identity at
                     * runtime (TYPE_ISOLATED lowers to the wrapped type's C
                     * type, see get_c_type), so emit the argument unchanged with
                     * zero runtime cost. The move-only linearity guarantee is
                     * enforced entirely in the type checker's move pass. */
                    generate_expression(gen, expr->children[0]);
                }
                else if (strcmp(func_name, "release") == 0 && expr->child_count == 1) {
                    ASTNode* arg = expr->children[0];
                    if (arg->node_type && arg->node_type->kind == TYPE_STRING) {
                        if (arg->type == AST_IDENTIFIER && arg->value &&
                            is_heap_string_var(gen, arg->value)) {
                            fprintf(gen->output,
                                /* The else arm matters: a tracked local can hold a value the
                                 * tracker does not own, the ordinary case being one
                                 * read out of a struct field. The flag is 0 there, so
                                 * without this the release silently did nothing and
                                 * the caller's explicit free never happened (#1977).
                                 * string_release is literal-safe, so the fallback
                                 * cannot hurt a borrowed literal, and it is what the
                                 * untracked-identifier path a few lines down already
                                 * does. string.free below has had this arm all along. */
                                "({ if (_heap_%s) { aether_heap_str_free((void*)%s); "
                                "%s = NULL; _heap_%s = 0; } else { string_release(%s); } })",
                                arg->value, arg->value, arg->value, arg->value, arg->value);
                        } else if (field_read_can_hand_off(arg)) {
                            emit_string_field_free(gen, arg, "string_release");
                        } else {
                            fprintf(gen->output, "string_release(");
                            generate_expression(gen, arg);
                            fprintf(gen->output, ")");
                        }
                    } else {
                        /* A reported error, not a bare stderr line: the
                         * driver bails when codegen raised the error count,
                         * so the build exits non-zero instead of handing a
                         * binary that skips the release to the user
                         * (#2211). */
                        aether_error_full(
                            "release(): only `string` is supported today",
                            expr->line, expr->column,
                            "for other heap types call the typed release "
                            "function: *StringSeq -> string.string_seq_free, "
                            "*Map -> hashmap.free",
                            "in release() call", AETHER_ERR_TYPE_MISMATCH);
                        fprintf(gen->output, "0 /* release() type error */");
                    }
                }
                // string.release(X) — the namespaced sibling of the bare
                // release() builtin (resolves to the string_release
                // extern). When X is a heap-tracked string local, lower it
                // through the SAME flag-guarded free so it frees once and
                // clears _heap_X. This is required because string_release
                // no longer marks its argument escaped (is_nonstoring_
                // builtin), so X now also receives an automatic
                // function-exit defer-free: without clearing the flag
                // here, `defer string.release(s)` on a magic AetherString
                // would be freed twice (explicit + auto), a double-free.
                // Non-heap-var arguments (literals, borrowed params) fall
                // through to the plain, literal-safe string_release call.
                else if ((strcmp(func_name, "string_release") == 0 ||
                          strcmp(func_name, "string.release") == 0 ||
                          cfree_sym_is(gen, func_name_norm, "string_release")) &&
                         expr->child_count == 1 &&
                         expr->children[0]->type == AST_IDENTIFIER &&
                         expr->children[0]->value &&
                         is_heap_string_var(gen, expr->children[0]->value)) {
                    /* Any heap-tracked local, NOT only string-typed.
                     * string.from_int / from_long / new return a magic
                     * AetherString that the classifier tracks but types
                     * `-> ptr`; those too get an auto defer-free now, so
                     * `defer string.release(num)` must clear the flag here
                     * or the magic string is string_release'd twice. The
                     * _heap_X guard makes aether_heap_str_free safe for
                     * both the magic and plain-char* representations. */
                    ASTNode* arg = expr->children[0];
                    fprintf(gen->output,
                        /* Same else arm as the bare release() above: a tracked local
                         * holding a field-read value has a 0 flag, and the release
                         * has to still happen (#1977). */
                        "({ if (_heap_%s) { aether_heap_str_free((void*)%s); "
                        "%s = NULL; _heap_%s = 0; } else { string_release(%s); } })",
                        arg->value, arg->value, arg->value, arg->value, arg->value);
                }
                // string.free(X) frees what the runtime cannot: given a
                // plain malloc'd payload, string_release cannot tell a heap
                // buffer from a static literal, so it no-ops and the value
                // leaks. An owned value goes through the shape-aware free
                // and clears the flag, so scope exit cannot free it twice.
                // The else keeps the runtime call for values the flag does
                // not cover, which is what a magic AetherString arriving
                // unflagged needs (string.from_double returns one).
                else if ((strcmp(func_name, "string_free") == 0 ||
                          strcmp(func_name, "string.free") == 0 ||
                          cfree_sym_is(gen, func_name_norm, "string_free")) &&
                         expr->child_count == 1 &&
                         expr->children[0]->type == AST_IDENTIFIER &&
                         expr->children[0]->value &&
                         is_heap_string_var(gen, expr->children[0]->value)) {
                    ASTNode* arg = expr->children[0];
                    fprintf(gen->output,
                        "({ if (_heap_%s) { aether_heap_str_free((void*)%s); "
                        "%s = NULL; _heap_%s = 0; } else { string_free(%s); } })",
                        arg->value, arg->value, arg->value, arg->value, arg->value);
                }
                // The same for a struct's string field, which owns its value
                // as a tracked local does: freed here, it must not be freed
                // again by the next store into the field or by the struct's
                // destructor (emit_string_field_free).
                else if ((cfree_sym_is(gen, func_name_norm, "string_free") ||
                          cfree_sym_is(gen, func_name_norm, "string_release")) &&
                         expr->child_count == 1 &&
                         field_read_can_hand_off(expr->children[0])) {
                    emit_string_field_free(gen, expr->children[0],
                                           consuming_free_symbol(gen, func_name_norm));
                }
                // string.seq_free(seq) — explicit refcount-decrement on a
                // *StringSeq. For a tracked seq local, clear the ownership
                // flag and NULL the slot so the scope-exit defer-free does
                // not decrement a spine this call already released (a
                // double-free glibc aborts on — exposed once scope-exit
                // defers run before exit()). Normalise the callee: the
                // source form is the dotted `string.seq_free`, not the
                // underscored C name this handler historically compared
                // against, so the flag-clear silently never fired.
                else if ((strcmp(func_name_norm, "string_seq_free") == 0 ||
                          strcmp(func_name, "string_seq_free") == 0) &&
                         expr->child_count == 1) {
                    ASTNode* arg = expr->children[0];
                    if (arg->type == AST_IDENTIFIER && arg->value &&
                        is_seq_var(gen, arg->value)) {
                        fprintf(gen->output,
                            "({ string_seq_free(%s); %s = NULL; _seqheap_%s = 0; })",
                            arg->value, arg->value, arg->value);
                    } else {
                        fprintf(gen->output, "string_seq_free(");
                        generate_expression(gen, arg);
                        fprintf(gen->output, ")");
                    }
                }
                // ref(value) — create a heap-allocated mutable cell
                else if (strcmp(func_name, "ref") == 0 && expr->child_count == 1) {
                    fprintf(gen->output, "\n#if AETHER_GCC_COMPAT\n");
                    fprintf(gen->output, "({ intptr_t* _r = malloc(sizeof(intptr_t)); *_r = (intptr_t)(");
                    generate_expression(gen, expr->children[0]);
                    fprintf(gen->output, "); (void*)_r; })");
                    fprintf(gen->output, "\n#else\n");
                    fprintf(gen->output, "_aether_ref_new((intptr_t)(");
                    generate_expression(gen, expr->children[0]);
                    fprintf(gen->output, "))");
                    fprintf(gen->output, "\n#endif\n");
                }
                // ref_get(r) — read from a ref cell
                else if (strcmp(func_name, "ref_get") == 0 && expr->child_count == 1) {
                    fprintf(gen->output, "(*(intptr_t*)");
                    generate_expression(gen, expr->children[0]);
                    fprintf(gen->output, ")");
                }
                // ref_set(r, value) — write to a ref cell
                else if (strcmp(func_name, "ref_set") == 0 && expr->child_count == 2) {
                    fprintf(gen->output, "(*(intptr_t*)");
                    generate_expression(gen, expr->children[0]);
                    fprintf(gen->output, " = (intptr_t)(");
                    generate_expression(gen, expr->children[1]);
                    fprintf(gen->output, "))");
                }
                // ref_free(r) — free a ref cell
                else if (strcmp(func_name, "ref_free") == 0 && expr->child_count == 1) {
                    fprintf(gen->output, "free(");
                    generate_expression(gen, expr->children[0]);
                    fprintf(gen->output, ")");
                }
                // lazy(closure) — create a thunk (deferred computation)
                else if (strcmp(func_name, "lazy") == 0 && expr->child_count == 1) {
                    fprintf(gen->output, "_aether_thunk_new(");
                    generate_expression(gen, expr->children[0]);
                    fprintf(gen->output, ")");
                }
                // force(thunk) — evaluate if needed, return cached value
                // Returns intptr_t — the assignment context determines the C type
                else if (strcmp(func_name, "force") == 0 && expr->child_count == 1) {
                    fprintf(gen->output, "_aether_thunk_force(");
                    generate_expression(gen, expr->children[0]);
                    fprintf(gen->output, ")");
                }
                // thunk_free(t) — free a thunk and its closure environment
                else if (strcmp(func_name, "thunk_free") == 0 && expr->child_count == 1) {
                    fprintf(gen->output, "_aether_thunk_free(");
                    generate_expression(gen, expr->children[0]);
                    fprintf(gen->output, ")");
                }
                else if (strcmp(func_name, "clock_ns") == 0 && expr->child_count == 0) {
                    // Always call the helper. The previous `#if AETHER_GCC_COMPAT`
                    // split inlined a statement-expression on GCC/Clang; that
                    // emitted preprocessor directives in the middle of an
                    // expression, which is fragile (any surrounding context that
                    // doesn't put the `#` at column 0 — e.g. macro expansion or
                    // a stale include order — collapses to an empty RHS and a
                    // spurious `undeclared identifier` on the lhs). The helper
                    // has the same per-platform `clock_gettime` / Windows /
                    // freestanding variants; modern compilers inline it anyway.
                    fprintf(gen->output, "_aether_clock_ns()");
                }
                else if (strcmp(func_name, "print_char") == 0 && expr->child_count >= 1) {
                    fprintf(gen->output, "putchar(");
                    generate_expression(gen, expr->children[0]);
                    fprintf(gen->output, ")");
                }
                // select(linux: val, windows: val, macos: val, default: val)
                // Compile-time platform selection via #ifdef chain
                else if (strcmp(func_name, "select") == 0 && expr->child_count >= 1) {
                    // Find the matching platform and default
                    ASTNode* linux_val = NULL;
                    ASTNode* windows_val = NULL;
                    ASTNode* macos_val = NULL;
                    ASTNode* default_val = NULL;
                    /* A key that is not a platform is a build symbol
                     * (#1527). Those are known now, so a matching one wins
                     * outright and the value is emitted with no directive:
                     * the platform chain only decides what a build symbol did
                     * not already answer. First match wins, left to right. */
                    ASTNode* defined_val = NULL;
                    for (int i = 0; i < expr->child_count; i++) {
                        ASTNode* arg = expr->children[i];
                        if (arg && arg->type == AST_NAMED_ARG && arg->value) {
                            if (strcmp(arg->value, "linux") == 0)
                                linux_val = arg->children[0];
                            else if (strcmp(arg->value, "windows") == 0)
                                windows_val = arg->children[0];
                            else if (strcmp(arg->value, "macos") == 0)
                                macos_val = arg->children[0];
                            else if (strcmp(arg->value, "other") == 0)
                                default_val = arg->children[0];
                            else if (!defined_val && aether_define_is_set(arg->value))
                                defined_val = arg->children[0];
                        }
                    }
                    if (defined_val) {
                        generate_expression(gen, defined_val);
                        break;
                    }
                    // Validate: every platform must have a value or other: must be set
                    if (!default_val) {
                        if (!linux_val || !windows_val || !macos_val) {
                            fprintf(stderr,
                                "error: select() at line %d: missing platform without 'other:' fallback.\n"
                                "  Provide all platforms (linux:, windows:, macos:) or add other: for the default.\n",
                                expr->line);
                            // Still emit code so compilation continues and shows all errors
                        }
                    }
                    // Emit #ifdef chain
                    fprintf(gen->output, "\n#ifdef _WIN32\n");
                    if (windows_val) {
                        generate_expression(gen, windows_val);
                    } else if (default_val) {
                        generate_expression(gen, default_val);
                    } else {
                        fprintf(gen->output, "#error \"select() has no value for windows and no other: fallback\"");
                    }
                    fprintf(gen->output, "\n#elif defined(__APPLE__)\n");
                    if (macos_val) {
                        generate_expression(gen, macos_val);
                    } else if (default_val) {
                        generate_expression(gen, default_val);
                    } else {
                        fprintf(gen->output, "#error \"select() has no value for macos and no other: fallback\"");
                    }
                    fprintf(gen->output, "\n#else\n");
                    if (linux_val) {
                        generate_expression(gen, linux_val);
                    } else if (default_val) {
                        generate_expression(gen, default_val);
                    } else {
                        fprintf(gen->output, "#error \"select() has no value for linux and no other: fallback\"");
                    }
                    fprintf(gen->output, "\n#endif\n");
                }
                // each(array, count, closure) — iterate array calling closure for each element
                // Usage: each(items, count) |item| { ... }
                // The trailing block becomes the last child (a closure)
                // box_closure(closure) — heap-allocate a closure so it can be stored in a list
                else if (strcmp(func_name, "box_closure") == 0 && expr->child_count == 1) {
                    /* A bare function has no environment, so it is not an
                     * _AeClosure and C rejected it outright. Wrap it in the
                     * same env-ignoring adapter a ptr-typed struct field
                     * assignment uses, so the one operation that exists to
                     * make a value safe for unbox_closure accepts the value
                     * most likely to need it. */
                    if (bare_top_level_fn(gen, expr->children[0])) {
                        const char* bn = expr->children[0]->value;
                        register_bare_fn_adapter(gen, bn);
                        fprintf(gen->output,
                                "_aether_box_closure((_AeClosure){ .fn = (void(*)(void))_aether_bare_adapter_%s, .env = NULL })",
                                bn);
                    } else {
                        fprintf(gen->output, "_aether_box_closure(");
                        generate_expression(gen, expr->children[0]);
                        fprintf(gen->output, ")");
                    }
                }
                // unbox_closure(ptr) — retrieve a closure from a heap pointer
                else if (strcmp(func_name, "unbox_closure") == 0 && expr->child_count == 1) {
                    fprintf(gen->output, "_aether_unbox_closure(");
                    generate_expression(gen, expr->children[0]);
                    fprintf(gen->output, ")");
                }
                // read_char() — read a single character from stdin (blocking)
                else if (strcmp(func_name, "read_char") == 0 && expr->child_count == 0) {
                    fprintf(gen->output, "getchar()");
                }
                // char_at(str, index) — ASCII value of character at position.
                // Route through the magic-aware string_char_at: the operand
                // may now be a magic AetherString (string ops return magic),
                // so a raw `(const char*)expr[idx]` would index into the
                // struct header instead of the payload.
                else if (strcmp(func_name, "char_at") == 0 && expr->child_count >= 1) {
                    fprintf(gen->output, "((int)string_char_at(");
                    generate_expression(gen, expr->children[0]);
                    fprintf(gen->output, ", ");
                    if (expr->child_count >= 2) {
                        generate_expression(gen, expr->children[1]);
                    } else {
                        fprintf(gen->output, "0");
                    }
                    fprintf(gen->output, "))");
                }
                // str_eq(a, b) — string equality (returns 1 or 0). Route
                // through magic-aware string_equals: operands may be magic
                // AetherStrings; raw strcmp would compare header bytes.
                else if (strcmp(func_name, "str_eq") == 0 && expr->child_count == 2) {
                    fprintf(gen->output, "string_equals(");
                    generate_expression(gen, expr->children[0]);
                    fprintf(gen->output, ", ");
                    generate_expression(gen, expr->children[1]);
                    fprintf(gen->output, ")");
                }
                // raw_mode() / cooked_mode() — terminal mode control
                else if (strcmp(func_name, "raw_mode") == 0 && expr->child_count == 0) {
                    fprintf(gen->output, "_aether_raw_mode()");
                }
                else if (strcmp(func_name, "cooked_mode") == 0 && expr->child_count == 0) {
                    fprintf(gen->output, "_aether_cooked_mode()");
                }
                // builder_context() — returns the current builder context from the stack
                else if (strcmp(func_name, "builder_context") == 0) {
                    fprintf(gen->output, "_aether_ctx_get()");
                }
                // spawn_sandboxed(grants, program, arg) — launch sandboxed child process
                else if (strcmp(func_name, "spawn_sandboxed") == 0 && expr->child_count >= 2) {
                    fprintf(gen->output, "aether_spawn_sandboxed(");
                    generate_expression(gen, expr->children[0]);
                    fprintf(gen->output, ", ");
                    generate_expression(gen, expr->children[1]);
                    if (expr->child_count >= 3) {
                        fprintf(gen->output, ", ");
                        generate_expression(gen, expr->children[2]);
                    } else {
                        fprintf(gen->output, ", NULL");
                    }
                    fprintf(gen->output, ")");
                }
                // ctx_push(ptr) / ctx_pop() — explicit context stack manipulation
                else if (strcmp(func_name, "sandbox_push") == 0 && expr->child_count == 1) {
                    fprintf(gen->output, "_aether_sandbox_push((void*)(intptr_t)");
                    generate_expression(gen, expr->children[0]);
                    fprintf(gen->output, ")");
                }
                else if (strcmp(func_name, "sandbox_pop") == 0 && expr->child_count == 0) {
                    fprintf(gen->output, "_aether_sandbox_pop()");
                }
                // sandbox_install() — activate runtime sandbox checking
                else if (strcmp(func_name, "sandbox_install") == 0 && expr->child_count == 0) {
                    fprintf(gen->output, "_aether_sandbox_install()");
                }
                // sandbox_uninstall() — deactivate runtime sandbox checking
                else if (strcmp(func_name, "sandbox_uninstall") == 0 && expr->child_count == 0) {
                    fprintf(gen->output, "_aether_sandbox_uninstall()");
                }
                // builder_depth() — returns the current builder nesting depth
                else if (strcmp(func_name, "builder_depth") == 0) {
                    fprintf(gen->output, "_aether_ctx_depth");
                }
                // call(closure_var, args...) — invoke a closure stored in a variable
                // Looks up the closure's hoisted function signature and calls through it
                else if (strcmp(func_name, "call") == 0 && expr->child_count >= 1) {
                    ASTNode* closure_arg = expr->children[0];
                    // Look up the closure ID from the variable name. An entry
                    // with closure_id == -1 means the variable was reassigned
                    // to a different closure and has no single static identity;
                    // treat it the same as "not found" and fall back to generic
                    // function-pointer dispatch.
                    // A closure variable that is ALSO a Route 1 promoted name
                    // is reassignable by construction (some closure writes it)
                    // and must go through the generic path too.
                    int found_id = -1;
                    if (closure_arg && closure_arg->type == AST_IDENTIFIER && closure_arg->value &&
                        !is_promoted_capture(gen, closure_arg->value)) {
                        /* #2513: the variable of this scope, not any of the name. */
                        found_id = closure_var_id(gen, gen->closure_var_scope, closure_arg->value);
                    }

                    if (found_id >= 0) {
                        /* #2493: a heap-string argument the closure's
                         * parameter does not keep is freed after the call,
                         * by the wrap a named call gets. The literal's
                         * body decides, as a function's does; the wrap
                         * yields what the closure's C function returns. */
                        ArgDrainWrap ad;
                        ad.have_value = 0;
                        ad.ret_ct = NULL;
                        ad.ret_type = NULL;
                        ad.discarded = ad_call_discarded;
                        ASTNode* literal = NULL;
                        for (int cj = 0; cj < gen->closure_count; cj++) {
                            if (gen->closures[cj].id != found_id) continue;
                            literal = gen->closures[cj].closure_node;
                            ad.ret_ct = resolve_closure_return_type(gen, cj);
                            ad.have_value = strcmp(ad.ret_ct, "void") != 0;
                            break;
                        }
                        arg_drain_select(gen, expr, 1, NULL, literal,
                                         !ad.have_value || ad_call_discarded, &ad);
                        arg_drain_open(gen, expr, &ad);
                        // Generate typed call: _closure_fn_N((_closure_env_N*)closure.env, args...)
                        fprintf(gen->output, "_closure_fn_%d((_closure_env_%d*)",
                                found_id, found_id);
                        generate_expression(gen, closure_arg);
                        fprintf(gen->output, ".env");
                        for (int i = 1; i < expr->child_count; i++) {
                            ASTNode* arg = expr->children[i];
                            // Skip trailing-DSL-block closures (handled via
                            // _ctx injection, not passed as args). Regular
                            // closure-literal args are real arguments and
                            // must be forwarded.
                            if (arg && arg->type == AST_CLOSURE &&
                                arg->value && strcmp(arg->value, "trailing") == 0) continue;
                            fprintf(gen->output, ", ");
                            generate_expression(gen, arg);
                        }
                        fprintf(gen->output, ")");
                        arg_drain_close(gen, expr, &ad);
                    } else {
                        /* Fallback: generic closure invocation via
                         * function-pointer cast.  Determine the
                         * cast's return type from (in order):
                         *
                         *   1. the call expression's node_type (the
                         *      typechecker fills this in when the
                         *      `fn`-typed variable has a known
                         *      return-type signature).
                         *   2. the closure_arg's declared
                         *      `return_type` (when `cb` has a typed
                         *      `fn(args) -> ret` signature).
                         *   3. the **enclosing function's** declared
                         *      return type — handles the bare-`fn`
                         *      shape `f(cb: fn) -> R { return cb(...) }`
                         *      that has no signature info to read.
                         *
                         * Pre-fix: when all three were UNKNOWN the
                         * cast defaulted to `int`.  For non-int
                         * returns this was a silent miscompile —
                         * float, string, ptr all wrong:
                         *   - float: int return slot reads rax instead
                         *     of xmm0 (x86-64 SysV) → silent zero.
                         *   - string: int reinterpreted as
                         *     AetherString* → segfault when the
                         *     heap-tracker dereferences it.
                         *   - ptr: UB but happens to survive on
                         *     x86-64 SysV (int and ptr both in rax);
                         *     gcc warns "returning 'int' from a
                         *     function with return type 'const
                         *     char *'", and on Windows LLP64 the
                         *     upper 32 bits truncate.
                         * Filed in aether/fn_return_float_cast.md
                         * (widened to "any non-int return") from
                         * the AeVG (CVG → Aether) port.
                         *
                         * The arg-type slots get the same
                         * closure_arg-declared-signature path when
                         * available, falling back to the supplied
                         * arg's `node_type`. */
                        Type* closure_sig = (closure_arg && closure_arg->node_type &&
                                             closure_arg->node_type->kind == TYPE_FUNCTION)
                                            ? closure_arg->node_type : NULL;
                        /* The result slot is what the typechecker stamped:
                         * the callee's signature, or the binding / return
                         * type that annotates an erased call (#2054). With
                         * nothing stamped the result is int, the language's
                         * default for an erased call, and the checker has
                         * said so where it matters. The enclosing function's
                         * return type is not consulted: a call compared or
                         * combined inside a function returning a struct is
                         * not returning that struct. */
                        Type* ret_t = NULL;
                        if (expr->node_type && expr->node_type->kind != TYPE_VOID &&
                            expr->node_type->kind != TYPE_UNKNOWN) {
                            ret_t = expr->node_type;
                        } else if (closure_sig && closure_sig->return_type &&
                                   closure_sig->return_type->kind != TYPE_UNKNOWN) {
                            ret_t = closure_sig->return_type;
                        }
                        const char* ret = ret_t ? get_c_type(ret_t) : "int";
                        /* #2499: no literal to read, so an owned string
                         * argument is freed after the call only under
                         * the program-wide closure-argument convention
                         * (arg_drain_verdict). */
                        ArgDrainWrap ad;
                        ad.ret_ct = ret;
                        ad.ret_type = NULL;
                        ad.have_value = strcmp(ret, "void") != 0;
                        ad.discarded = ad_call_discarded;
                        arg_drain_select(gen, expr, 1, NULL, NULL,
                                         !ad.have_value || ad_call_discarded, &ad);
                        /* #2519: an owned closure used as the callee (a
                         * literal, or one a call hands over: `call(
                         * make_counter())`) is held by nothing else. It
                         * is hoisted into the wrap's temp, which the two
                         * reads below (`.fn`, `.env`) name instead of
                         * evaluating the callee twice, and released after
                         * the call. */
                        if (closure_arg && !arg_drain_lookup(closure_arg) && ad.count < 16 &&
                            ((closure_arg->type == AST_CLOSURE &&
                              !(closure_arg->value && strcmp(closure_arg->value, "trailing") == 0)) ||
                             call_returns_owned_closure(gen, closure_arg))) {
                            ad.identity[ad.count] = 0;
                            ad.closure[ad.count] = 1;
                            ad.idx[ad.count++] = 0;
                        }
                        arg_drain_open(gen, expr, &ad);
                        fprintf(gen->output, "((%s(*)(void*", ret);
                        int sig_pi = 0;
                        for (int i = 1; i < expr->child_count; i++) {
                            ASTNode* arg = expr->children[i];
                            if (arg && arg->type == AST_CLOSURE &&
                                arg->value && strcmp(arg->value, "trailing") == 0) continue;
                            const char* atype = "int";
                            if (closure_sig && sig_pi < closure_sig->param_count &&
                                closure_sig->param_types &&
                                closure_sig->param_types[sig_pi] &&
                                closure_sig->param_types[sig_pi]->kind != TYPE_UNKNOWN) {
                                atype = get_c_type(closure_sig->param_types[sig_pi]);
                            } else if (arg && arg->node_type) {
                                atype = get_c_type(arg->node_type);
                            } else if (arg && arg->type == AST_CLOSURE) {
                                atype = "_AeClosure";
                            }
                            fprintf(gen->output, ", %s", atype);
                            sig_pi++;
                        }
                        fprintf(gen->output, "))");
                        generate_expression(gen, closure_arg);
                        fprintf(gen->output, ".fn)(");
                        generate_expression(gen, closure_arg);
                        fprintf(gen->output, ".env");
                        for (int i = 1; i < expr->child_count; i++) {
                            ASTNode* arg = expr->children[i];
                            if (arg && arg->type == AST_CLOSURE &&
                                arg->value && strcmp(arg->value, "trailing") == 0) continue;
                            fprintf(gen->output, ", ");
                            generate_expression(gen, arg);
                        }
                        fprintf(gen->output, ")");
                        arg_drain_close(gen, expr, &ad);
                    }
                }
                else {
                    const char* c_func_name = call_c_name(gen, func_name);

                    // spawn_ActorName(preferred_core) — pass core hint or -1.
                    // Only the generated actor spawner: a user function or
                    // an extern that merely starts with `spawn_` is an
                    // ordinary call (#2126 — `spawn_once(1, 2, p)` was
                    // emitted as `spawn_once(1)`, the rest dropped without
                    // a word, and the C compiler complained).
                    if (strncmp(func_name, "spawn_", 6) == 0 &&
                        strcmp(func_name, "spawn_sandboxed") != 0 &&
                        !(gen->program && find_function_definition_by_name(gen->program, func_name)) &&
                        !is_extern_func(gen, func_name)) {
                        fprintf(gen->output, "%s(", c_func_name);
                        if (expr->child_count > 0 && expr->children[0]) {
                            generate_expression(gen, expr->children[0]);
                        } else {
                            fprintf(gen->output, "-1");
                        }
                        fprintf(gen->output, ")");
                        break;
                    }

                    /* List owned-string element auto-routing (#467).
                     * `list.add(l, heap_string_expr)` (a.k.a.
                     * `list_add_raw` after the Aether wrapper's
                     * `list_add(list_add_raw(...))` dispatch
                     * collapses) lands here. When the second arg is
                     * a heap-classified string expression, route to
                     * `list_add_string_adopted` so the list adopts
                     * the value and releases it at `list.free`
                     * time. Pre-fix, the heap-string lived forever
                     * in the list (escape walker suppressed the
                     * source's free; list_free didn't walk
                     * elements) — leak. */
                    /* List & map heap-string-value auto-routing (#467).
                     *
                     * `list.add(l, heap)` / `map.put(m, k, heap)`
                     * lands here. When the value arg is a heap-
                     * classified string, route to the `_string_owned`
                     * variant so the container retains + releases at
                     * free time. Two callee shapes to handle:
                     *
                     *   - `_raw` externs (`list_add_raw`, `map_put_raw`)
                     *     return `int`. The owned variant also returns
                     *     `int` — direct rewrite.
                     *   - Wrapper functions (`list_add`, `map_put`)
                     *     return `string` (the Go-style `"" | error`
                     *     shape). The owned variant returns `int`, so
                     *     a direct rewrite would change the return
                     *     type and break the caller's
                     *     `err = list.add(...)` assignment. Wrap in
                     *     a ternary that preserves the string-return
                     *     contract: `(owned(...) ? "" : "<err>")`. */
                    /* Classify the callee: list-shape (2 args, value
                     * at index 1) vs map-shape (3 args, value at
                     * index 2); wrapper (`list_add` / `map_put`
                     * returns string) vs raw extern (`list_add_raw`
                     * / `map_put_raw` returns int). */
                    /* An EXPLICIT owned-add whose value is a fresh heap
                     * expression is the same ownership transfer as the
                     * auto-routed `list.add(l, heap)`: the temporary has no
                     * other owner, so the container must adopt it rather
                     * than acquire a second reference (which would leak the
                     * caller's). Routing it through the same branch picks
                     * the adopting entry below; a borrowed / literal /
                     * read-back value falls through to the owning entry,
                     * which is what makes sharing across containers safe. */
                    int is_explicit_owned_list = (strcmp(c_func_name, "list_add_string_owned") == 0);
                    int is_explicit_owned_map  = (strcmp(c_func_name, "map_put_string_owned") == 0);
                    int is_list_shape = (strcmp(c_func_name, "list_add_raw") == 0 ||
                                         strcmp(c_func_name, "list_add") == 0 ||
                                         is_explicit_owned_list);
                    int is_map_shape  = (strcmp(c_func_name, "map_put_raw") == 0 ||
                                         strcmp(c_func_name, "map_put") == 0 ||
                                         is_explicit_owned_map);
                    int is_wrapper    = (strcmp(c_func_name, "list_add") == 0 ||
                                         strcmp(c_func_name, "map_put") == 0);
                    /* `list.set(l, i, v)` owns a string as `list.add` does:
                     * the old element is released, the new one adopted or
                     * taken (list_set_string_adopted / _owned). */
                    int is_set_shape  = (strcmp(c_func_name, "list_set") == 0);
                    /* A closure value (`fn`-typed, not a raw fn-ptr) stored
                     * into a list or a map is heap-boxed (the fn -> ptr
                     * coercion) and the container owns the box and a
                     * reference of its own to the env (#2518), released
                     * when the element goes. A bare fn -> ptr (is_fnptr)
                     * is a code address, not heap; it stays on the raw
                     * path. Decided by closure_container_store_value,
                     * which the escape walks also ask. */
                    {
                        ASTNode* cval = closure_container_store_value(gen, expr);
                        if (cval) {
                            emit_closure_container_store(gen, expr, cval, ad_call_discarded);
                            break;
                        }
                    }
                    if (is_list_shape || is_map_shape || is_set_shape) {
                        int val_idx           = is_list_shape ? 1 : 2;
                        int expected_arg_count = is_list_shape ? 2 : 3;
                        if (expr->child_count == expected_arg_count) {
                            ASTNode* val = expr->children[val_idx];
                            /* Route the value into the container's
                             * owning variant only when it is genuinely
                             * a heap allocation the container should
                             * free.
                             *
                             * For a NON-identifier expression
                             * (`string.concat(...)`, interpolation, a
                             * heap-returning call) `is_heap_string_expr`
                             * is exact — it is statically a fresh
                             * allocation.
                             *
                             * For a bare IDENTIFIER `is_heap_string_expr`
                             * is too coarse: it answers "is this name
                             * heap-TRACKED" (every assigned string
                             * variable is), which is true even for a
                             * variable that only ever holds a literal
                             * (`s = "1"`). Routing such a variable into
                             * `_string_owned` tagged a `.rodata` literal
                             * as owned and `free()`d it at container
                             * teardown — a crash (map-put-raw-rewritten-
                             * to-owned.md). The runtime `_heap_<name>`
                             * flag can't rescue this either: the value
                             * has escaped into the container call, so
                             * the reassignment wrapper that maintains
                             * the flag is suppressed and it reads stale.
                             * Resolve the identifier structurally
                             * instead — it is genuinely heap only if
                             * some assignment in the enclosing function
                             * body sets it from a heap source. A
                             * variable that is only ever literal-
                             * assigned is left un-rewritten (the
                             * container does not own it). */
                            int val_is_heap;
                            /* A heap-tracked local is taken below, as a
                             * struct field or an `if` is: moved on its last
                             * use, copied otherwise (string_container_store_value). */
                            ASTNode* taken = string_container_store_value(gen, expr);
                            if (val && val->type == AST_IDENTIFIER && val->value) {
                                val_is_heap = !taken && body_assigns_var_from_heap(
                                    gen, current_fn_body_block(gen), val->value);
                            } else {
                                val_is_heap = val && is_heap_string_expr(gen, val);
                            }
                            if (val_is_heap) {
                                if (is_list_shape) {
                                    /* The wrapper shape returns `string`, so
                                     * it goes through the prelude helper that
                                     * converts the int result. Emitting the
                                     * conversion inline would leave every
                                     * statement-position add discarding a
                                     * ternary, which is -Wunused-value in the
                                     * user's own build. */
                                    fprintf(gen->output, is_wrapper
                                            ? "_aether_list_add_adopted("
                                            : "list_add_string_adopted(");
                                    generate_expression(gen, expr->children[0]);
                                    fprintf(gen->output, ", (void*)");
                                    generate_expression(gen, val);
                                    fprintf(gen->output, ")");
                                } else if (is_set_shape) {
                                    fprintf(gen->output, "list_set_string_adopted(");
                                    generate_expression(gen, expr->children[0]);
                                    fprintf(gen->output, ", ");
                                    generate_expression(gen, expr->children[1]);
                                    fprintf(gen->output, ", (void*)");
                                    generate_expression(gen, val);
                                    fprintf(gen->output, ")");
                                } else {
                                    fprintf(gen->output, is_wrapper
                                            ? "_aether_map_put_adopted("
                                            : "map_put_string_adopted(");
                                    generate_expression(gen, expr->children[0]);
                                    /* The key goes as it is, an AetherString
                                     * with its length or a plain char*: the
                                     * map reads either shape, and unwrapping
                                     * to the payload here cut a key at its
                                     * first NUL (#2469). */
                                    fprintf(gen->output, ", (const char*)(");
                                    generate_expression(gen, expr->children[1]);
                                    fprintf(gen->output, "), (void*)");
                                    generate_expression(gen, val);
                                    fprintf(gen->output, ")");
                                }
                                break;
                            }
                            /* #2497: a value that views a string something
                             * else owns: a struct field (the struct frees it
                             * when the field changes or the struct goes), or
                             * an `if` whose arm is such a view or a local.
                             * Stored raw, the container kept a pointer the
                             * owner then freed. A field read goes to the
                             * owning add, which takes the container's own
                             * reference (retains a refcounted string, copies
                             * a plain one). An `if` is taken as a binding
                             * takes it (emit_string_take): what the take
                             * owns is adopted, what it borrows is added
                             * through the owning entry. */
                            if (val && is_owned_string_field_read(val)) {
                                if (is_list_shape) {
                                    fprintf(gen->output, is_wrapper
                                            ? "_aether_list_add_owned("
                                            : "list_add_string_owned(");
                                    generate_expression(gen, expr->children[0]);
                                    fprintf(gen->output, ", (void*)");
                                } else if (is_set_shape) {
                                    fprintf(gen->output, "list_set_string_owned(");
                                    generate_expression(gen, expr->children[0]);
                                    fprintf(gen->output, ", ");
                                    generate_expression(gen, expr->children[1]);
                                    fprintf(gen->output, ", (void*)");
                                } else {
                                    fprintf(gen->output, is_wrapper
                                            ? "_aether_map_put_owned("
                                            : "map_put_string_owned(");
                                    generate_expression(gen, expr->children[0]);
                                    /* The key as it is (#2469), as above. */
                                    fprintf(gen->output, ", (const char*)(");
                                    generate_expression(gen, expr->children[1]);
                                    fprintf(gen->output, "), (void*)");
                                }
                                generate_expression(gen, val);
                                fprintf(gen->output, ")");
                                break;
                            }
                            if (val && (taken == val || string_take_is_view(gen, val))) {
                                char own[32];
                                string_take_new_flag(own, sizeof(own));
                                fprintf(gen->output, "({ void* _ae_cc = (void*)(");
                                generate_expression(gen, expr->children[0]);
                                fprintf(gen->output, ");");
                                if (is_set_shape) {
                                    fprintf(gen->output, " int _ae_ci = (");
                                    generate_expression(gen, expr->children[1]);
                                    fprintf(gen->output, ");");
                                } else if (!is_list_shape) {
                                    fprintf(gen->output, " const char* _ae_ck = (const char*)(");
                                    generate_expression(gen, expr->children[1]);
                                    fprintf(gen->output, ");");
                                }
                                fprintf(gen->output, " int %s = 0; void* _ae_cv = (void*)", own);
                                emit_string_take(gen, val, own, NULL);
                                if (is_list_shape) {
                                    fprintf(gen->output, "; %s ? %s(_ae_cc, _ae_cv) : %s(_ae_cc, _ae_cv); })",
                                            own,
                                            is_wrapper ? "_aether_list_add_adopted" : "list_add_string_adopted",
                                            is_wrapper ? "_aether_list_add_owned" : "list_add_string_owned");
                                } else if (is_set_shape) {
                                    fprintf(gen->output, "; %s ? list_set_string_adopted(_ae_cc, _ae_ci, _ae_cv) : list_set_string_owned(_ae_cc, _ae_ci, _ae_cv); })",
                                            own);
                                } else {
                                    fprintf(gen->output, "; %s ? %s(_ae_cc, _ae_ck, _ae_cv) : %s(_ae_cc, _ae_ck, _ae_cv); })",
                                            own,
                                            is_wrapper ? "_aether_map_put_adopted" : "map_put_string_adopted",
                                            is_wrapper ? "_aether_map_put_owned" : "map_put_string_owned");
                                }
                                break;
                            }
                            /* `list.set` of any other string (a literal, a
                             * borrowed view) owns a copy: only an owning set
                             * can release the element the slot held, and a
                             * raw list_set cannot know what the slot held. */
                            if (is_set_shape && val && val->node_type &&
                                val->node_type->kind == TYPE_STRING) {
                                fprintf(gen->output, "list_set_string_owned(");
                                generate_expression(gen, expr->children[0]);
                                fprintf(gen->output, ", ");
                                generate_expression(gen, expr->children[1]);
                                fprintf(gen->output, ", (void*)");
                                generate_expression(gen, val);
                                fprintf(gen->output, ")");
                                break;
                            }
                            /* A `string` PARAMETER of a named function reaches
                             * here only when the function does not keep it
                             * (callee_string_param_captures): a function that
                             * stores its parameter takes a reference of its
                             * own on entry, after which the parameter is a
                             * heap-tracked local and is taken above. What
                             * reaches this raw path is a borrow the container
                             * must not free: the magic header proves heap
                             * representation, not ownership. */
                        }
                    }

                    /* Argument-temp lifetime wrap. Any heap-returning
                     * AST_FUNCTION_CALL appearing in argument position
                     * is an anonymous heap allocation with no consumer
                     * — pre-fix this is a leak per call. Hoist each
                     * such arg into a named temporary, run the parent
                     * call, then free the temps in a GCC statement-
                     * expression wrapper. See ArgDrainSub at the top
                     * of this file for the full rationale.
                     *
                     * Skip when the parent's return type is void /
                     * unknown — statement-expressions need a final
                     * value and a void parent has nothing to yield.
                     * Skip when the args are themselves substitution-
                     * registered (we're inside a parent wrap already
                     * — the registered entry handles the lifetime). */
                    ArgDrainWrap ad;
                    ad.have_value = expr->node_type &&
                                    expr->node_type->kind != TYPE_VOID &&
                                    expr->node_type->kind != TYPE_UNKNOWN;
                    ad.ret_ct = NULL;
                    ad.ret_type = expr->node_type;
                    ad.discarded = ad_call_discarded;
                    int ad_is_void = (expr->node_type && expr->node_type->kind == TYPE_VOID) ||
                                     ad_call_discarded;
                    if (!ad.have_value && !ad_is_void) {
                        /* #2519: the checker stamped no type on the call
                         * (an unannotated callee), so the wrap could not
                         * name its result's type and drained nothing. The
                         * callee's definition says whether its C function
                         * returns a value (the rule generate_function
                         * emits the signature by), and the temp takes
                         * whatever type that is. */
                        int shape = callee_result_shape(gen, func_name);
                        if (shape > 0) {
                            ad.have_value = 1;
                            ad.ret_ct = "__auto_type";
                        } else if (shape == 0) {
                            ad_is_void = 1;
                        }
                    }
                    arg_drain_select(gen, expr, 0, func_name, NULL, ad_is_void, &ad);
                    arg_drain_open(gen, expr, &ad);

                    /* A trusted call in an enforced block goes through its
                     * wrapper (codegen.c emit_sandbox_trust_wrappers), which
                     * has the same signature, so everything below that is
                     * keyed on the callee still holds. */
                    if (gen->uses_sandbox) {
                        int tsite = sandbox_trust_site_of_call(expr);
                        ASTNode* tdef = tsite ? sandbox_trust_target(gen, expr) : NULL;
                        if (tdef) {
                            c_func_name = sandbox_trust_wrapper_name(tdef, tsite);
                        }
                    }
                    fprintf(gen->output, "%s(", c_func_name);
                    int arg_printed = 0;
                    // Auto-inject builder context for builder functions
                    // (functions with _ctx: ptr as first param). Inject only
                    // when the user's arg count is exactly one less than the
                    // function's declared param count — that means the user
                    // omitted _ctx and expects the codegen to fill it in.
                    // If the user-arg count matches the param count exactly,
                    // they passed _ctx explicitly (e.g. forwarding from a
                    // surrounding builder body) and we trust them.
                    //
                    // _aether_ctx_get() returns NULL at the top of the stack,
                    // so a top-level builder call gets NULL injected, which
                    // builders that ignore _ctx (like std.host's manifest
                    // builders) handle correctly. That's what makes the
                    // outermost call in `abi() { describe("trading") { ... }
                    // }` work.
                    {
                        // builder_funcs registry is keyed on the underscored form.
                        const char* bf_normalized = codegen_normalise_callee(func_name);
                        int is_builder = 0;
                        for (int bi = 0; bi < gen->builder_func_count; bi++) {
                            if (strcmp(gen->builder_funcs[bi], bf_normalized) == 0) {
                                is_builder = 1;
                                break;
                            }
                        }
                        if (is_builder) {
                            // Find the function's declared param count (counting
                            // both regular function params and extern params).
                            int declared_params = -1;
                            ASTNode* program = gen->program;
                            for (int fi = 0; program && fi < program->child_count; fi++) {
                                ASTNode* fdef = program->children[fi];
                                if (!fdef || !fdef->value) continue;
                                int matches = (strcmp(fdef->value, bf_normalized) == 0);
                                if (matches && (fdef->type == AST_FUNCTION_DEFINITION
                                             || fdef->type == AST_EXTERN_FUNCTION
                                             || fdef->type == AST_BUILDER_FUNCTION)) {
                                    declared_params = 0;
                                    for (int pi = 0; pi < fdef->child_count; pi++) {
                                        ASTNode* p = fdef->children[pi];
                                        if (!p) continue;
                                        if (p->type == AST_GUARD_CLAUSE) continue;
                                        if (p->type == AST_BLOCK) continue;
                                        declared_params++;
                                    }
                                    break;
                                }
                            }
                            // If we couldn't find the definition (e.g. extern
                            // imported via std.host that's not in program->children),
                            // fall back to "always inject" — the original behavior.
                            // The looser rule may break in pathological cases but
                            // works for our manifest builders.
                            int user_args = 0;
                            for (int ai = 0; ai < expr->child_count; ai++) {
                                ASTNode* a = expr->children[ai];
                                /* Trailing DSL blocks aren't user args. */
                                if (a && a->type == AST_CLOSURE && a->value
                                  && strcmp(a->value, "trailing") == 0) continue;
                                user_args++;
                            }
                            int should_inject =
                                (declared_params < 0)
                             || (user_args == declared_params - 1);
                            if (should_inject) {
                                fprintf(gen->output, "_aether_ctx_get()");
                                arg_printed++;
                            }
                        }
                    }
                    for (int i = 0; i < expr->child_count; i++) {
                        ASTNode* arg = expr->children[i];
                        // Skip trailing DSL blocks that are just inline syntax sugar
                        // (value == "trailing" AND function doesn't expect fn param)
                        if (arg && arg->type == AST_CLOSURE &&
                            arg->value && strcmp(arg->value, "trailing") == 0) {
                            // Check if function expects this arg as fn type
                            // by looking up the function definition
                            int func_wants_fn = 0;
                            {
                                ASTNode* fdef = find_function_definition_by_name(gen->program, func_name);
                                if (fdef) {
                                    int pi = 0;
                                    for (int fj = 0; fj < fdef->child_count; fj++) {
                                        ASTNode* p = fdef->children[fj];
                                        if (p->type == AST_GUARD_CLAUSE || p->type == AST_BLOCK) continue;
                                        if (pi == i && p->node_type &&
                                            p->node_type->kind == TYPE_FUNCTION) {
                                            func_wants_fn = 1;
                                        }
                                        pi++;
                                    }
                                }
                            }
                            if (!func_wants_fn) continue; // skip DSL trailing block
                        }
                        if (arg_printed > 0) fprintf(gen->output, ", ");
                        // Cast int→void* when param expects void* (TYPE_PTR).
                        // Check extern registry first, then user-defined function params.
                        TypeKind expected = lookup_extern_param_kind(gen, c_func_name, arg_printed);
                        /* For TYPE_FUNCTION params we also need the full
                         * Type* so we can read `is_fnptr` — the boxing
                         * coercions below only apply to the closure
                         * shape (is_fnptr=0), NOT to raw C fn pointers
                         * declared as `fn(T1, T2, ...) -> R` (is_fnptr=1,
                         * storage = void*). Without this gate, passing
                         * a bare named function to a `fn(args)->ret`
                         * param would emit an _AeClosure struct literal
                         * where the receiver expects void*. */
                        Type* expected_type = NULL;
                        if (expected == TYPE_UNKNOWN) {
                            // Look up user-defined function's param type.
                            // Try both the original call-site name and the
                            // dot-normalized C name so merged stdlib wrappers
                            // (e.g. list.add -> list_add in the program AST)
                            // also get their ptr params auto-cast.
                            /* A definition is never spelt with a dot in the
                               merged program, so the dotted call-site name can
                               only match through its normalised form. */
                            {
                                ASTNode* fdef = find_function_definition_by_name(gen->program, func_name);
                                if (!fdef) fdef = find_function_definition_by_name(gen->program, c_func_name);
                                if (fdef) {
                                    int pi = 0;
                                    for (int fj = 0; fj < fdef->child_count; fj++) {
                                        ASTNode* fp = fdef->children[fj];
                                        if (fp->type == AST_GUARD_CLAUSE || fp->type == AST_BLOCK) continue;
                                        if (pi == arg_printed && fp->node_type) {
                                            expected = fp->node_type->kind;
                                            expected_type = fp->node_type;
                                        }
                                        pi++;
                                    }
                                }
                            }
                        }
                        int expected_is_fnptr_form = (expected == TYPE_FUNCTION &&
                            expected_type && expected_type->is_fnptr);
                        /* #1033: tuple literal → by-value `_tuple_*` struct
                         * for a tuple-typed extern parameter. The element
                         * list comes from the registry's full param type,
                         * falling back to the tuple type the typechecker
                         * stamped on the literal. Each element gets an
                         * explicit cast to its field's C type, so Aether
                         * doubles land in `float` fields and ints in
                         * `unsigned char` ones without warnings. */
                        Type* tuple_param = NULL;
                        if (arg->type == AST_TUPLE_LITERAL) {
                            if (expected == TYPE_TUPLE) {
                                tuple_param = lookup_extern_param_type(
                                    gen, c_func_name, arg_printed);
                            }
                            if ((!tuple_param || tuple_param->kind != TYPE_TUPLE) &&
                                arg->node_type &&
                                arg->node_type->kind == TYPE_TUPLE) {
                                tuple_param = arg->node_type;
                            }
                        }
                        /* #1244: a `va_list` parameter receives the va_list
                         * itself. va_start() yields a cookie that POINTS at the
                         * function's `va_list` (so it can be passed around as a
                         * plain ptr), and va_arg / va_end already dereference
                         * it. Forwarding to a C `v*` callee has to do the same,
                         * or vprintf reads the cookie as if it were the
                         * argument list: it compiles clean and prints garbage.
                         *
                         * Looked up separately rather than by widening
                         * `expected_type` to externs: that variable gates the
                         * fn-ptr, optional-coercion and tuple branches below,
                         * which have only ever seen user-function params. */
                        Type* extern_param_t =
                            lookup_extern_param_type(gen, c_func_name, arg_printed);
                        if (extern_param_t && extern_param_t->c_alias &&
                            strcmp(extern_param_t->c_alias, "va_list") == 0) {
                            fprintf(gen->output, "*(va_list*)(");
                            generate_expression(gen, arg);
                            fprintf(gen->output, ")");
                        } else
                        if (tuple_param && tuple_param->kind == TYPE_TUPLE) {
                            fprintf(gen->output, "(%s){", get_c_type(tuple_param));
                            for (int ti = 0; ti < arg->child_count; ti++) {
                                if (ti > 0) fprintf(gen->output, ", ");
                                Type* et = (ti < tuple_param->tuple_count)
                                           ? tuple_param->tuple_types[ti] : NULL;
                                fprintf(gen->output, "(%s)(",
                                        et ? get_c_type(et) : "int");
                                generate_expression(gen, arg->children[ti]);
                                fprintf(gen->output, ")");
                            }
                            fprintf(gen->output, "}");
                        } else
                        if (expected_type && expected_type->kind == TYPE_OPTIONAL &&
                            needs_optional_coerce(arg, expected_type)) {
                            /* #340: a bare `T` (or `none`) flowing into a
                             * `T?` parameter — wrap it into the `ae_opt_T`
                             * struct so the C argument matches the param
                             * slot. The typedef was already emitted by the
                             * collect_optional_typedefs pre-pass (it walks
                             * param-node types), so this never emits one
                             * mid-expression. */
                            emit_optional_coerced(gen, arg, expected_type);
                        } else if (expected_type && expected_type->kind == TYPE_SUM &&
                                   needs_sum_coerce(arg, expected_type)) {
                            /* #914: a variant struct flowing into a sum
                             * parameter wraps into the tagged union. */
                            emit_sum_coerced(gen, arg, expected_type);
                        } else if (expected == TYPE_PTR && arg->node_type &&
                            arg->node_type->kind == TYPE_FUNCTION &&
                            !arg->node_type->is_fnptr) {
                            /* Closure (`_AeClosure` struct) → ptr slot.
                             * Heap-box the struct so the receiver can
                             * stash it in a list/map/struct ptr field
                             * with the env slot intact, and the
                             * inverse coercion (TYPE_FUNCTION expected,
                             * TYPE_PTR arg, below) can recover it.
                             *
                             * `is_fnptr=1` means the source is a raw C
                             * function pointer (produced by `expr as
                             * fn(...)` casts — storage is `void*`, not
                             * `_AeClosure`); that path falls through
                             * to the default identity emit, which is
                             * the established working behaviour from
                             * tests/regression/test_fn_address_via_as_fn.ae
                             * (Aether-fn address handed to qsort, etc.).
                             * The filing's repro (fn_ptr_coercion.md)
                             * was on the is_fnptr=0 closure path —
                             * a `fn`-typed local that's a real
                             * _AeClosure struct value.
                             *
                             * Pre-fix: `fn`-typed local arg into a
                             * `ptr` slot caused gcc "expected void* but
                             * argument is of type _AeClosure" — silent
                             * type-check accept, hard fail at C compile.
                             *
                             * #2523: a `@noescape` extern parameter reads
                             * the box only during the call, so it gets one
                             * on this stack frame (a compound literal of
                             * the enclosing block) and the closure's env
                             * stays its owner's: a literal's is released
                             * after the call by the drain, a local's at its
                             * scope end. */
                            if (is_noescape_extern_param(gen, c_func_name, arg_printed)) {
                                fprintf(gen->output, "_aether_box_closure_in(&(_AeClosureBox){ .tag = 0 }, ");
                            } else {
                                fprintf(gen->output, "_aether_box_closure(");
                            }
                            generate_expression(gen, arg);
                            fprintf(gen->output, ")");
                        } else if (expected == TYPE_FUNCTION &&
                                   !expected_is_fnptr_form &&
                                   !(arg->node_type && arg->node_type->is_fnptr)) {
                            /* The receiver slot is `fn` (an _AeClosure
                             * struct — bare `fn` with no signature).
                             * The raw-fnptr form `fn(T1, ...) -> R`
                             * (expected_is_fnptr_form) is `void*`-shaped
                             * and falls through to the default emit:
                             * a bare named function decays to its
                             * address, which the C side reads as
                             * `void*`. The argument shape can be:
                             *
                             *   (a) a bare named function — emit a
                             *       struct literal with env=NULL so
                             *       the receiver gets a real
                             *       _AeClosure value.
                             *   (b) a `fn`-typed local — already an
                             *       _AeClosure; pass through.
                             *   (c) a `ptr` value (boxed closure
                             *       recovered from list/map/struct
                             *       field) — unbox.
                             *
                             * Pre-fix: (a) caused gcc "expected
                             * _AeClosure but argument is of type int
                             * (*)(void)" — the silent type-check pass
                             * that the fn_ptr_coercion.md filing
                             * surfaced. */
                            /* #1657: use the shared helper rather than an
                             * inline whole-program scan. This site had its own
                             * copy of that loop, so it kept substituting an
                             * adapter for a name that is really a PARAMETER of
                             * the enclosing function — see bare_top_level_fn
                             * for why a local binding must win. Duplicating
                             * the search is what let the scope rule be fixed
                             * in one place and still be violated here. */
                            int arg_is_bare_fn = bare_top_level_fn(gen, arg) != NULL;
                            if (arg_is_bare_fn) {
                                const char* bn = arg && arg->value ? arg->value : NULL;
                                if (bn) register_bare_fn_adapter(gen, bn);
                                fprintf(gen->output,
                                        "(_AeClosure){ .fn = (void(*)(void))_aether_bare_adapter_%s, .env = NULL }",
                                        bn ? bn : "unknown");
                            } else if (arg->node_type && arg->node_type->kind == TYPE_PTR) {
                                fprintf(gen->output, "_aether_unbox_closure(");
                                generate_expression(gen, arg);
                                fprintf(gen->output, ")");
                            } else {
                                generate_expression(gen, arg);
                            }
                        } else if (emit_int_ptr_bridged(gen, arg, expected)) {
                            /* int <-> ptr parameter: cast emitted. */
                        } else if (expected == TYPE_PTR && arg->node_type &&
                                   arg->node_type->kind == TYPE_STRING) {
                            if (arg->type == AST_LITERAL) {
                                /* A string literal is char[] in C: it decays
                                 * and converts to void* implicitly with no
                                 * qualifier warning, so it needs no cast, and
                                 * casting it to void* destroys the constant
                                 * the C compiler's -Wformat check reads. With
                                 * the cast, a format/argument bug in a printf
                                 *-family extern call compiled silently
                                 * (#1252). libc externs keep their real
                                 * attributed prototypes (declaration skipped),
                                 * so the check fires end to end. */
                                generate_expression(gen, arg);
                            } else {
                                // Cast const char* expressions to void* to
                                // silence C's "discards qualifiers" warning
                                // when passing into a ptr parameter.
                                fprintf(gen->output, "(void*)(");
                                generate_expression(gen, arg);
                                fprintf(gen->output, ")");
                            }
                        } else if (expected == TYPE_STRING && arg->node_type &&
                                   (arg->node_type->kind == TYPE_STRING ||
                                    arg->node_type->kind == TYPE_PTR) &&
                                   is_extern_func(gen, func_name) &&
                                   !is_stdlib_string_aware_extern(c_func_name) &&
                                   !is_aether_extern_param(gen, func_name, arg_printed)) {
                            // The Aether-side value typed `string` may be a
                            // wrapped AetherString* (from string.from_int,
                            // string_concat_wrapped, fs.read_binary, etc.)
                            // or a bare const char* (literal, string_concat).
                            // A naive C extern's `const char*` parameter
                            // expects payload bytes — passing the AetherString
                            // header pointer leaks magic+refcount+lengths
                            // into memcpy/strlen calls on the C side.
                            //
                            // aether_string_data() dispatches on the magic
                            // header: returns ->data for wrapped strings,
                            // the bare pointer for plain char*. Idempotent
                            // on either shape.
                            //
                            // Skipped for:
                            //   1. stdlib externs that already go through
                            //      str_data/str_len internally (they need
                            //      the header to recover the stored length
                            //      on binary content). See
                            //      is_stdlib_string_aware_extern below.
                            //   2. params declared `name: @aether string`
                            //      — receiver is Aether-emitted C and
                            //      dispatches on AetherString magic via
                            //      str_len. Without this, binary content
                            //      with embedded NULs strlen-truncates at
                            //      the boundary (#351).
                            // Closes #297.
                            fprintf(gen->output, "aether_string_data(");
                            generate_expression(gen, arg);
                            fprintf(gen->output, ")");
                        } else if (expected == TYPE_STRING && !order_bound(arg) &&
                                   field_read_can_hand_off(arg) &&
                                   callee_consumes_string_arg(gen, func_name, arg_printed)) {
                            /* The callee frees what it is handed: the field
                             * gives its reference up (callee_consumes_string_arg). */
                            emit_string_field_handoff(gen, arg);
                        } else {
                            generate_expression(gen, arg);
                        }
                        arg_printed++;
                    }
                    // Defer functions get (void*)0 as last arg when called without trailing block
                    if (is_builder_func_reg(gen, func_name)) {
                        if (arg_printed > 0) fprintf(gen->output, ", ");
                        fprintf(gen->output, "(void*)0");
                    }
                    fprintf(gen->output, ")");
                    arg_drain_close(gen, expr, &ad);
                }
            }
            break;

        case AST_STRING_INTERP: {
            // Children alternate: AST_LITERAL (string) and expression nodes.
            // Two modes:
            //   1. interp_as_printf: emit printf() directly (used by print/println)
            //   2. default: emit snprintf+malloc → returns (void*) heap string (TYPE_PTR)
            //
            // The printf mode is for THIS interpolation only. The flag is taken
            // down before any segment is generated, so an interpolation nested
            // in a segment — `println("${f("${base}/x")}")` — builds its
            // string; left up, it printed to stdout and handed `f` printf's
            // return count as a pointer.
            int as_printf = gen->interp_as_printf;
            gen->interp_as_printf = 0;

            // Helper macro: emit the format string for both modes
            #define EMIT_INTERP_FMT() do { \
                for (int i = 0; i < expr->child_count; i++) { \
                    ASTNode* ch = expr->children[i]; \
                    if (interp_segment_is_text(ch)) { \
                        /* Decoded bytes, escaped for C as a plain literal \
                         * is; never read again as escapes (#2512). A NUL \
                         * of the text is a `%c` fed a 0 (#2520). */ \
                        emit_c_string_bytes(gen, ch->value ? ch->value : "", \
                                            (size_t)ast_literal_length(ch), 1); \
                    } else { \
                        TypeKind tk = (ch->node_type) ? ch->node_type->kind : TYPE_UNKNOWN; \
                        switch (tk) { \
                            case TYPE_INT:    fprintf(gen->output, "%%d");  break; \
                            case TYPE_INT64:  fprintf(gen->output, "%%lld"); break; \
                            case TYPE_UINT32: fprintf(gen->output, "%%u");  break; \
                            case TYPE_UINT64: fprintf(gen->output, "%%llu"); break; \
                            case TYPE_DURATION: fprintf(gen->output, "%%s"); break; \
                            case TYPE_FLOAT:  fprintf(gen->output, "%%g");  break; \
                            case TYPE_FLOAT32: fprintf(gen->output, "%%g");  break; \
                            case TYPE_LONGDOUBLE: fprintf(gen->output, "%%Lg"); break; \
                            case TYPE_BOOL:   fprintf(gen->output, "%%s");  break; \
                            case TYPE_STRING: fprintf(gen->output, "%%s");  break; \
                            case TYPE_PTR:    fprintf(gen->output, "%%s");  break; \
                            default:          fprintf(gen->output, "%%d");  break; \
                        } \
                    } \
                } \
            } while(0)

            // Helper macro: emit the arguments for both modes
            #define EMIT_INTERP_ARGS() do { \
                for (int i = 0; i < expr->child_count; i++) { \
                    ASTNode* ch = expr->children[i]; \
                    if (interp_segment_is_text(ch)) { \
                        emit_text_nul_args(gen, ch); \
                        continue; \
                    } \
                    fprintf(gen->output, ", "); \
                    TypeKind tk = ch->node_type ? ch->node_type->kind : TYPE_UNKNOWN; \
                    if (tk == TYPE_BOOL) { \
                        interp_emit_segment(gen, ch, it_names[i]); \
                        fprintf(gen->output, " ? \"true\" : \"false\""); \
                    } else if (tk == TYPE_STRING || tk == TYPE_PTR) { \
                        /* As held, an AetherString or a char*: the \
                         * formatter writes it by its length (#2521). */ \
                        fprintf(gen->output, "(const void*)("); \
                        interp_emit_segment(gen, ch, it_names[i]); \
                        fprintf(gen->output, ")"); \
                    } else if (tk == TYPE_INT64) { \
                        fprintf(gen->output, "(long long)"); \
                        interp_emit_segment(gen, ch, it_names[i]); \
                    } else if (tk == TYPE_INT) { \
                        /* Aether int is C `int` and %d expects one, but a \
                         * TYPE_INT value can be stored wider: a single-scalar \
                         * message field rides the intptr_t payload slot. Narrow \
                         * to (int) to match %d, as INT64 casts to (long long). \
                         * Only genuine TYPE_INT values reach here (actor-ref / \
                         * ptr fields are TYPE_PTR and print via %s), so no \
                         * pointer is ever truncated. */ \
                        fprintf(gen->output, "(int)"); \
                        interp_emit_segment(gen, ch, it_names[i]); \
                    } else if (tk == TYPE_UINT64) { \
                        fprintf(gen->output, "(unsigned long long)"); \
                        interp_emit_segment(gen, ch, it_names[i]); \
                    } else if (tk == TYPE_DURATION) { \
                        fprintf(gen->output, "_aether_duration_repr("); \
                        interp_emit_segment(gen, ch, it_names[i]); \
                        fprintf(gen->output, ")"); \
                    } else { \
                        interp_emit_segment(gen, ch, it_names[i]); \
                    } \
                } \
            } while(0)

            /* Segments become C varargs, and C leaves argument evaluation
             * order unspecified: gcc evaluates right to left, so
             * `"${bump(c)} ${bump(c)} ${bump(c)}"` printed `3 2 1` (#2195).
             * Once any segment has a side effect, every segment is
             * evaluated into a temp in source order ahead of the printf /
             * _aether_interp call (a pure read on either side of the
             * impure one observes it). An all-pure interpolation stays
             * inline, so the common `"${name} is ${age}"` costs nothing
             * extra.
             *
             * Independently, a heap-producing CALL segment
             * ("[${string.join(parts, ",")}]") is a temporary nothing owns:
             * it is bound to a temp so it can be freed after the consumer
             * has read it, or it leaks once per interpolation. Identifiers
             * are never freed (their owner frees them); only calls the
             * classifier proves heap-producing qualify. Same
             * registry-substitution pattern as the closure-env and
             * call-argument drains above. */
            int it_saved = g_arg_drain_count;
            int it_hoist_upto = interp_order_hoist_boundary(expr);
            const char** it_names = (const char**)calloc(
                (size_t)(expr->child_count > 0 ? expr->child_count : 1),
                sizeof(const char*));
            int* it_heap = (int*)calloc(
                (size_t)(expr->child_count > 0 ? expr->child_count : 1),
                sizeof(int));
            if (!it_names || !it_heap) {
                fprintf(stderr, "Fatal: out of memory hoisting interpolation segments\n");
                exit(1);
            }
            int it_hoisted = 0;
            for (int di = 0; di < expr->child_count; di++) {
                ASTNode* ch = expr->children[di];
                if (interp_segment_is_text(ch)) continue;
                int heap = interp_segment_is_heap_call(gen, ch);
                if (!heap && di > it_hoist_upto) continue;
                const char* ctype = interp_temp_c_type(ch->node_type);
                if (!ctype) continue;   /* no scalar/string temp shape: leave inline */
                if (it_hoisted == 0) fprintf(gen->output, as_printf ? "{ " : "({ ");
                it_hoisted++;
                char* nm = arg_drain_mint_name();
                fprintf(gen->output, "%s %s = (%s)(", ctype, nm, ctype);
                generate_expression(gen, ch);
                fprintf(gen->output, "); ");
                arg_drain_bind(ch, nm);   /* registry owns nm; truncate frees it */
                it_names[di] = nm;
                it_heap[di] = heap;
            }
            if (as_printf) {
                // Mode 1: written to stdout (for print/println), by the
                // runtime formatter so a string segment keeps its NULs; 2
                // is println, whose newline ends the format (#2521)
                fprintf(gen->output, "_aether_interp_print(\"");
                EMIT_INTERP_FMT();
                if (as_printf == 2) fprintf(gen->output, "\\n");
                fprintf(gen->output, "\"");
                EMIT_INTERP_ARGS();
                fprintf(gen->output, ")");
            } else {
                // Mode 2: heap-allocated C string — always use portable helper function
                if (it_hoisted > 0) {
                    fprintf(gen->output, "const char* _it_r = ");
                }
                fprintf(gen->output, "_aether_interp(\"");
                EMIT_INTERP_FMT();
                fprintf(gen->output, "\"");
                EMIT_INTERP_ARGS();
                fprintf(gen->output, ")");
            }
            if (it_hoisted > 0) {
                fprintf(gen->output, "; ");
                for (int di = 0; di < expr->child_count; di++) {
                    if (it_names[di] && it_heap[di]) {
                        fprintf(gen->output, "aether_heap_str_free(%s); ", it_names[di]);
                    }
                }
                if (as_printf) {
                    fprintf(gen->output, "}");
                } else {
                    fprintf(gen->output, "_it_r; })");
                }
                arg_drain_truncate(it_saved);
            }
            free(it_names);
            free(it_heap);
            gen->interp_as_printf = as_printf;

            #undef EMIT_INTERP_FMT
            #undef EMIT_INTERP_ARGS
            break;
        }

        case AST_ACTOR_REF:
            if (!expr->value) { fprintf(gen->output, "NULL"); break; }
            if (strcmp(expr->value, "self") == 0) {
                if (gen->current_actor) {
                    // Inside actor handler: self is the function parameter
                    fprintf(gen->output, "(ActorBase*)self");
                } else {
                    fprintf(gen->output, "NULL /* self outside actor */");
                }
            } else {
                fprintf(gen->output, "%s", expr->value);
            }
            break;
        
        case AST_NAMED_ARG:
            // Named argument: emit just the value (name is for readability)
            if (expr->child_count > 0) {
                generate_expression(gen, expr->children[0]);
            }
            break;

        case AST_ARRAY_LITERAL:
            /* Context-sensitive literal: when the typechecker stamped
             * `*StringSeq` on this literal (LHS of a variable decl
             * typed `*StringSeq`), emit a right-fold cons chain
             * instead of a static array initialiser. `[]` lowers to
             * `string_seq_empty()` (NULL); `[a, b, c]` lowers to
             * `string_seq_cons("a", string_seq_cons("b", string_seq_cons("c", string_seq_empty())))`.
             * Same source syntax, two C lowerings — see typechecker.c
             * AST_VARIABLE_DECLARATION case for the type-stamping
             * branch and docs/sequences.md § Literal disambiguation
             * for the user-visible rule. */
            if (is_string_seq_ptr_type(expr->node_type)) {
                /* #1417: two or more elements need the tail dropped as the
                 * chain is built. `string_seq_cons` takes its OWN retain on
                 * the tail, so the canonical builder is
                 * `cons(h, t); string_seq_free(t)`. The nested-expression
                 * form has no name for the intermediate results, so nothing
                 * ever dropped them: every cell but the head ended at
                 * refcount 2, and a correct `string_seq_free(head)` stopped
                 * at the first cell that stayed >0 (the shared-tail
                 * protection), leaking the rest.
                 *
                 * Fold into a local instead, dropping each handle after the
                 * next cell has retained it. Elements are evaluated into
                 * temps in SOURCE order first, so an element with a side
                 * effect still runs left to right even though the chain is
                 * built right to left. */
                if (expr->child_count >= 2) {
                    int id = g_seq_lit_counter++;
                    fprintf(gen->output, "({ ");
                    for (int i = 0; i < expr->child_count; i++) {
                        fprintf(gen->output, "const char* _sqe%d_%d = (const char*)(", id, i);
                        generate_expression(gen, expr->children[i]);
                        fprintf(gen->output, "); ");
                    }
                    fprintf(gen->output,
                            "StringSeq* _sqa%d = string_seq_empty(); StringSeq* _sqn%d; ",
                            id, id);
                    for (int i = expr->child_count - 1; i >= 0; i--) {
                        fprintf(gen->output,
                                "_sqn%d = string_seq_cons(_sqe%d_%d, _sqa%d); "
                                "string_seq_free(_sqa%d); _sqa%d = _sqn%d; ",
                                id, id, i, id, id, id, id);
                    }
                    fprintf(gen->output, "_sqa%d; })", id);
                    break;
                }
                /* 0 or 1 element: no intermediate exists to drop. */
                for (int i = 0; i < expr->child_count; i++) {
                    fprintf(gen->output, "string_seq_cons(");
                    generate_expression(gen, expr->children[i]);
                    fprintf(gen->output, ", ");
                }
                fprintf(gen->output, "string_seq_empty()");
                for (int i = 0; i < expr->child_count; i++) {
                    fprintf(gen->output, ")");
                }
                break;
            }
            fprintf(gen->output, "{");
            for (int i = 0; i < expr->child_count; i++) {
                if (i > 0) fprintf(gen->output, ", ");
                /* #2525: an element that views a struct owned elsewhere is
                 * taken (copied, or moved out of a local on its last use),
                 * as a struct field init is, so the array's elements are
                 * values of their own. */
                ASTNode* el = expr->children[i];
                const char* el_struct = struct_owning_strings(gen, el ? el->node_type : NULL);
                if (el_struct && struct_take_shape(el)) emit_struct_take(gen, el, el_struct, NULL);
                else generate_expression(gen, el);
            }
            fprintf(gen->output, "}");
            break;

        case AST_STRUCT_LITERAL: {
            /* Struct-field heap-string ownership (#465): for each
             * field-init whose expression is heap-classified, also
             * emit `._heap_<field> = 1` so the auto-emitted
             * <Struct>_destroy() reclaims the buffer at scope exit.
             * Plain initializers (literal strings, scalars) default
             * to _heap_<field> = 0 via C99 designated-init's zero-
             * fill of unmentioned fields.
             *
             * #911: when a field ADOPTS a heap-string *variable* (`.f = v`
             * with v a tracked heap-string local), ownership MOVES from the
             * variable into the struct. The variable's own function-exit
             * free must then be suppressed, or the same buffer is freed
             * twice (once via the struct's owned-field free, once via the
             * variable's deferred free) — the double-free crash. We collect
             * the adopted variable names and, after the literal, clear their
             * `_heap_<v>` flags inside a statement-expression so the exit
             * free's `if (_heap_<v>)` guard skips them. */
            const char* moved_vars[16];
            int moved_count = 0;
            int any_moved = 0;
            /* A header-defined struct (`extern struct ... @c_import`) has no
             * `_heap_<field>` trackers and no destructor: its string fields
             * BORROW, as a C struct's `char*` does. Its literal is spelled
             * `(struct Name){...}` because the header need not typedef the
             * tag. */
            int c_imported = aether_is_c_import_struct(expr->value);
            for (int i = 0; !c_imported && i < expr->child_count; i++) {
                ASTNode* fi = expr->children[i];
                if (fi && fi->type == AST_ASSIGNMENT && fi->child_count > 0 &&
                    fi->children[0]->type == AST_IDENTIFIER &&
                    fi->children[0]->value &&
                    is_heap_string_var(gen, fi->children[0]->value)) {
                    any_moved = 1; break;
                }
            }
            /* #2461: a field initialised from a view (another struct's field,
             * an `if` over owned values) takes the value with
             * emit_string_take, whose flag sets the field's tracker once the
             * literal is built. The literal used to borrow it: the source
             * struct then freed the buffer this one still pointed at. */
            char (*take_own)[32] = NULL;
            int any_take = 0;
            for (int i = 0; !c_imported && i < expr->child_count; i++) {
                ASTNode* fi = expr->children[i];
                if (fi && fi->type == AST_ASSIGNMENT && fi->value && fi->child_count > 0 &&
                    string_take_is_view(gen, fi->children[0])) {
                    if (!take_own) take_own = calloc((size_t)expr->child_count, sizeof(*take_own));
                    if (!take_own) break;
                    string_take_new_flag(take_own[i], sizeof(take_own[i]));
                    any_take = 1;
                }
            }
            /* When a variable is moved into a field, build the struct into a
             * temp inside a statement-expression, clear the moved-from vars'
             * heap flags, then yield the temp. Otherwise emit the literal
             * directly. */
            if (any_moved || any_take) {
                fprintf(gen->output, "({ ");
                for (int i = 0; any_take && i < expr->child_count; i++) {
                    if (take_own[i][0]) fprintf(gen->output, "int %s = 0; ", take_own[i]);
                }
                fprintf(gen->output, "%s _ae_slit = ", expr->value);
            }
            fprintf(gen->output, c_imported ? "(struct %s){" : "(%s){", expr->value);
            int emitted = 0;
            for (int i = 0; i < expr->child_count; i++) {
                ASTNode* field_init = expr->children[i];
                if (field_init && field_init->type == AST_ASSIGNMENT) {
                    if (emitted > 0) fprintf(gen->output, ", ");
                    fprintf(gen->output, ".%s = ", field_init->value);
                    if (field_init->child_count > 0) {
                        /* A header-defined struct's string field is C's
                         * `const char*`: the payload, not a wrapped
                         * AetherString (see emit_c_import_string_field_store). */
                        ASTNode* fv = field_init->children[0];
                        int unwrap = c_imported && fv && fv->node_type &&
                                     fv->node_type->kind == TYPE_STRING;
                        /* NULL stays NULL, as in the field store. */
                        if (unwrap) fprintf(gen->output, "({ const void* _ae_cs = (const void*)(");
                        /* #2497: a struct field held by value that owns
                         * strings takes the struct it is given. */
                        const char* fv_struct = c_imported ? NULL
                                                : struct_owning_strings(gen, fv ? fv->node_type : NULL);
                        /* #2525: a closure field holds a reference of its
                         * own: a fresh closure's, or one taken on a view. */
                        int fv_closure = !c_imported && fv && fv->node_type &&
                                         fv->node_type->kind == TYPE_FUNCTION &&
                                         !fv->node_type->is_fnptr;
                        /* #2528: a `string[N]` / `fn[N]` field owns every
                         * element: a literal's elements are copied or
                         * taken. */
                        int fv_owned_array = 0;
                        if (!c_imported && fv && fv->type == AST_ARRAY_LITERAL && gen->program) {
                            ASTNode* sdef = find_struct_definition_by_name(gen->program, expr->value);
                            for (int fi = 0; sdef && fi < sdef->child_count; fi++) {
                                ASTNode* f = sdef->children[fi];
                                if (f && f->type == AST_STRUCT_FIELD && f->value &&
                                    strcmp(f->value, field_init->value) == 0) {
                                    fv_owned_array = struct_field_owned_array(f, NULL);
                                    break;
                                }
                            }
                        }
                        if (take_own && take_own[i][0]) emit_string_take(gen, fv, take_own[i], NULL);
                        else if (fv_struct && struct_take_shape(fv)) emit_struct_take(gen, fv, fv_struct, NULL);
                        else if (fv_closure) emit_closure_take(gen, fv);
                        else if (fv_owned_array) emit_owned_array_literal(gen, fv, fv_owned_array);
                        else generate_expression(gen, fv);
                        if (unwrap) fprintf(gen->output, "); _ae_cs ? aether_string_data(_ae_cs) : (const char*)0; })");
                    }
                    emitted++;
                    if (take_own && take_own[i][0]) continue;  /* tracker set below */
                    /* If the init is heap-classified, also set the hidden
                     * tracker. For a heap-tracked *variable* source, the
                     * ownership is RUNTIME-conditional on the variable's own
                     * `_heap_<v>` flag (#911): `e = s` leaves a borrowed,
                     * non-heap value, so `._heap_<field> = 1` would make the
                     * struct free a string it never owned (a bad free of the
                     * caller's literal). Mirror the runtime flag instead, and
                     * record the variable as moved-from so its flag is cleared
                     * (the deferred free becomes a no-op). For non-variable
                     * heap sources (an interp/concat temp), the value is freshly
                     * owned, so a constant 1 is correct. */
                    if (!c_imported && field_init->child_count > 0 &&
                        is_heap_string_expr(gen, field_init->children[0])) {
                        ASTNode* src = field_init->children[0];
                        if (src->type == AST_IDENTIFIER && src->value &&
                            is_heap_string_var(gen, src->value)) {
                            fprintf(gen->output, ", ._heap_%s = _heap_%s",
                                    field_init->value, src->value);
                            if (moved_count < 16) moved_vars[moved_count++] = src->value;
                        } else {
                            fprintf(gen->output, ", ._heap_%s = 1", field_init->value);
                        }
                        emitted++;
                    }
                }
            }
            fprintf(gen->output, "}");
            if (any_moved || any_take) {
                /* #911: disown each moved-from variable so its function-exit
                 * `if (_heap_<v>)` free is a no-op (ownership now in the
                 * struct). Then yield the built struct. */
                fprintf(gen->output, ";");
                for (int i = 0; any_take && i < expr->child_count; i++) {
                    if (take_own[i][0])
                        fprintf(gen->output, " _ae_slit._heap_%s = %s;",
                                expr->children[i]->value, take_own[i]);
                }
                /* #1301: the struct literal now owns each moved buffer;
                 * the moved-from var's defer is disarmed by the flag
                 * clear, so the unwind journal must drop it too or a
                 * panic drain would free the struct's field. */
                for (int m = 0; m < moved_count; m++)
                    fprintf(gen->output,
                            " _heap_%s = 0; aether_unwind_forget(%s);",
                            moved_vars[m], moved_vars[m]);
                fprintf(gen->output, " _ae_slit; })");
            }
            free(take_own);
            break;
        }

        case AST_ARRAY_ACCESS:
            if (expr->child_count >= 2) {
                /* #1380: a string may be a char* or an AetherString*; index the payload. */
                ASTNode* base = expr->children[0];
                if (base && type_is_slice(base->node_type)) {
                    /* #1286: bounds-checked element access on a slice. The
                     * helper returns the element's address, so the result
                     * stays an lvalue and the slice is evaluated once. */
                    const char* elem = slice_elem_c_type(base->node_type);
                    fprintf(gen->output, "(*(%s*)aether_slice_at(", elem);
                    /* The base is READ even when `s[i]` is a store target,
                     * so an extern-struct field there still becomes a view. */
                    int saved_lvalue = gen->generating_lvalue;
                    gen->generating_lvalue = 0;
                    generate_expression(gen, base);
                    gen->generating_lvalue = saved_lvalue;
                    fprintf(gen->output, ", (int64_t)(");
                    generate_expression(gen, expr->children[1]);
                    fprintf(gen->output, "), sizeof(%s), ", elem);
                    emit_slice_diag_file(gen, expr);
                    fprintf(gen->output, ", %d))", expr->line);
                    break;
                }
                int str_base = base && base->node_type &&
                               base->node_type->kind == TYPE_STRING;
                if (str_base) fprintf(gen->output, "_aether_safe_str(");
                generate_expression(gen, base);
                if (str_base) fprintf(gen->output, ")");
                fprintf(gen->output, "[");
                generate_expression(gen, expr->children[1]);
                fprintf(gen->output, "]");
            }
            break;
        
        case AST_SEND_FIRE_FORGET:
            if (expr->child_count >= 2) {
                ASTNode* target = expr->children[0];
                ASTNode* message = expr->children[1];
                
                if (message && message->type == AST_MESSAGE_CONSTRUCTOR) {
                    MessageDef* msg_def = lookup_message(gen->message_registry, message->value);
                    if (msg_def) {
                        const char* single_int = get_single_int_field(msg_def);
                        if (single_int) {
                            // Single-field inline: value stored in payload_int (no malloc)
                            fprintf(gen->output, "{ Message _imsg = {%d, 0, ", msg_def->message_id);
                            for (int i = 0; i < message->child_count; i++) {
                                ASTNode* field_init = message->children[i];
                                if (field_init && field_init->type == AST_FIELD_INIT && field_init->child_count > 0) {
                                    int fk = msg_def->fields ? msg_def->fields->type_kind : TYPE_INT;
                                    ASTNode* val = field_init->children[0];
                                    int is_actor_ref = val->node_type && val->node_type->kind == TYPE_ACTOR_REF;
                                    if (is_actor_ref || fk == TYPE_INT64 || fk == TYPE_PTR || fk == TYPE_ACTOR_REF)
                                        fprintf(gen->output, "(intptr_t)");
                                    generate_expression(gen, val);
                                    break;
                                }
                            }
                            fprintf(gen->output, ", NULL, {NULL, 0, 0}}; ");

                            if (gen->in_main_loop) {
                                fprintf(gen->output, "scheduler_send_batch_add(");
                                emit_send_target(gen, target, "ActorBase*");
                                fprintf(gen->output, ", _imsg); }");
                            } else if (gen->current_actor == NULL) {
                                fprintf(gen->output, "scheduler_send_remote(");
                                emit_send_target(gen, target, "ActorBase*");
                                fprintf(gen->output, ", _imsg, aether_core_id_get()); }");
                            } else {
                                fprintf(gen->output, "ActorBase* _send_target = ");
                                emit_send_target(gen, target, "ActorBase*");
                                fprintf(gen->output, "; ");
                                fprintf(gen->output, "{ int _cid = aether_core_id_get(); if (_cid >= 0 && _cid == _send_target->assigned_core) { ");
                                fprintf(gen->output, "scheduler_send_local(_send_target, _imsg); } else { ");
                                fprintf(gen->output, "scheduler_send_remote(_send_target, _imsg, _cid); } } }");
                            }
                        } else {
                            // Heap-allocated path (2+ fields or non-scalar types).
                            // Hoist any array literals to static locals first so
                            // their storage outlives the send-expression block.
                            fprintf(gen->output, "{ ");
                            emit_message_array_hoists(gen, message, msg_def);
                            fprintf(gen->output, "%s _msg = { ._message_id = %d",
                                    message->value, msg_def->message_id);
                            for (int i = 0; i < message->child_count; i++) {
                                ASTNode* field_init = message->children[i];
                                if (field_init && field_init->type == AST_FIELD_INIT) {
                                    fprintf(gen->output, ", .%s = ", field_init->value);
                                    if (field_init->child_count > 0) {
                                        MessageFieldDef* fdef = find_msg_field(msg_def, field_init->value);
                                        emit_message_field_init(gen, fdef, field_init->children[0]);
                                    }
                                }
                            }
                            fprintf(gen->output, " }; ");
                            /* Deep-copy heap-string fields (#466).
                             * The shallow `_msg.text = original_ptr`
                             * assignment leaves the receiver pointing
                             * at the sender's heap; the sender's
                             * defer-free / reassign-wrapper would
                             * dangle the receiver's pointer. Re-stamp
                             * each string field with a refcounted
                             * AetherString clone via string_new_with_
                             * length, sized from the source's
                             * length-aware header so binary content
                             * with embedded NULs round-trips intact.
                             * The receiver's <Msg>_release_fields
                             * call (emitted at the receive-handler
                             * dispatch) string_releases the clone
                             * when the message is consumed. */
                            int has_str = 0;
                            for (MessageFieldDef* f = msg_def->fields; f; f = f->next) {
                                if (f->type_kind == TYPE_STRING) { has_str = 1; break; }
                            }
                            if (has_str) {
                                /* Prototypes (string_new_with_length,
                                 * aether_string_data, aether_string_
                                 * length) are emitted once in the
                                 * codegen prologue (codegen.c) — no
                                 * per-call-site re-declaration. */
                                for (MessageFieldDef* f = msg_def->fields; f; f = f->next) {
                                    if (f->type_kind == TYPE_STRING) {
                                        emit_message_string_copy(gen,
                                            cg_internf("_msg.%s", f->name),
                                            message_field_init_expr(message, f->name));
                                    }
                                }
                            }
                            fprintf(gen->output, "aether_send_message(");
                            emit_send_target(gen, target, "void*");
                            fprintf(gen->output, ", &_msg, sizeof(%s)); }", message->value);
                        }
                    } else {
                        fprintf(gen->output, "/* ERROR: unknown message type %s */", message->value ? message->value : "<?>");
                    }
                }
            }
            break;

        case AST_SEND_ASK:
            if (expr->child_count >= 2) {
                ASTNode* target = expr->children[0];
                ASTNode* message = expr->children[1];
                
                if (message && message->type == AST_MESSAGE_CONSTRUCTOR) {
                    MessageDef* msg_def = lookup_message(gen->message_registry, message->value);
                    if (msg_def) {
                        // Look up the reply shape from the pre-built map:
                        // a reply message name, or the scalar TypeKind of
                        // an expression reply (#1324).
                        const char* reply_msg_name = NULL;
                        int reply_scalar_kind = TYPE_UNKNOWN;
                        for (int r = 0; r < gen->reply_type_count; r++) {
                            if (strcmp(gen->reply_type_map[r].request_msg, message->value) == 0) {
                                reply_msg_name = gen->reply_type_map[r].reply_msg;
                                reply_scalar_kind = gen->reply_type_map[r].scalar_kind;
                                break;
                            }
                        }

                        // Find the first non-_message_id field of the reply message
                        const char* reply_field = NULL;
                        int reply_field_type = TYPE_INT;
                        if (reply_msg_name) {
                            MessageDef* reply_def = lookup_message(gen->message_registry, reply_msg_name);
                            if (reply_def && reply_def->fields) {
                                reply_field = reply_def->fields->name;
                                reply_field_type = reply_def->fields->type_kind;
                            }
                        }
                        /* The C type the asker reads the reply as, one rule
                         * for a message field, an expression reply and the
                         * no-reply fallback, on both paths (#2528). Keep in
                         * sync with the AST_REPLY_STATEMENT scalar emission. */
                        const char* c_type = "int";
                        const char* c_zero = "0";
                        {
                            int k = (reply_msg_name && reply_field) ? reply_field_type
                                  : (reply_scalar_kind != TYPE_UNKNOWN) ? reply_scalar_kind : TYPE_INT;
                            switch (k) {
                                case TYPE_FLOAT:      c_type = "double"; c_zero = "0.0"; break;
                                case TYPE_LONGDOUBLE: c_type = "long double"; c_zero = "0.0L"; break;
                                case TYPE_BOOL:       c_type = "int"; c_zero = "0"; break;
                                case TYPE_STRING:     c_type = "const char*"; c_zero = "NULL"; break;
                                case TYPE_INT64:      c_type = "int64_t"; c_zero = "0"; break;
                                case TYPE_UINT64:     c_type = "uint64_t"; c_zero = "0"; break;
                                case TYPE_DURATION:   c_type = "int64_t"; c_zero = "0"; break;
                                case TYPE_PTR:        c_type = "void*"; c_zero = "NULL"; break;
                                case TYPE_FUNCTION:   c_type = "_AeClosure"; c_zero = "(_AeClosure){0}"; break;
                                default:              c_type = "int"; c_zero = "0"; break;
                            }
                        }

                        int timeout_ms = 5000;
                        if (expr->child_count >= 3 && expr->children[2] &&
                            expr->children[2]->value) {
                            timeout_ms = atoi(expr->children[2]->value);
                        }

                        /* #2528: the reply's release, one rule for both
                         * paths: a reply message that owns strings or
                         * closures releases what the asker does not take. */
                        const char* reply_release = "NULL";
                        if (reply_msg_name) {
                            MessageDef* rdef = lookup_message(gen->message_registry, reply_msg_name);
                            for (MessageFieldDef* f = rdef ? rdef->fields : NULL; f; f = f->next) {
                                if (f->type_kind == TYPE_STRING ||
                                    (f->type_kind == TYPE_FUNCTION && f->c_type &&
                                     strcmp(f->c_type, "_AeClosure") == 0)) {
                                    reply_release = cg_internf(
                                        "(void (*)(void*))%s_release_fields", reply_msg_name);
                                    break;
                                }
                            }
                        }

                        // Emit the ask expression with GCC/MSVC guards
                        fprintf(gen->output, "\n#if AETHER_GCC_COMPAT\n");
                        // GCC/Clang: statement expression
                        fprintf(gen->output, "({ %s _msg = { ._message_id = %d",
                                message->value, msg_def->message_id);

                        for (int i = 0; i < message->child_count; i++) {
                            ASTNode* field_init = message->children[i];
                            if (field_init && field_init->type == AST_FIELD_INIT) {
                                fprintf(gen->output, ", .%s = ", field_init->value);
                                if (field_init->child_count > 0) {
                                    MessageFieldDef* fdef = find_msg_field(msg_def, field_init->value);
                                    emit_message_field_init(gen, fdef, field_init->children[0]);
                                }
                            }
                        }

                        fprintf(gen->output, " }; ");

                        /* Deep-copy heap-string request-message fields
                         * (#466). Same shape as the AST_SEND_FIRE_
                         * FORGET path — the request message carries
                         * the sender's local heap-string pointers
                         * into the receiver's mailbox; without deep-
                         * copy the sender's defer-free dangles the
                         * receiver's references. */
                        for (MessageFieldDef* f = msg_def->fields; f; f = f->next) {
                            if (f->type_kind == TYPE_STRING) {
                                emit_message_string_copy(gen,
                                    cg_internf("_msg.%s", f->name),
                                    message_field_init_expr(message, f->name));
                            }
                        }

                        fprintf(gen->output, "void* _ask_r = scheduler_ask_message(");
                        emit_send_target(gen, target, "ActorBase*");
                        fprintf(gen->output, ", &_msg, sizeof(%s), %d); ", message->value, timeout_ms);

                        if (reply_msg_name && reply_field) {
                            fprintf(gen->output, "%s _ask_val = _ask_r ? ((%s*)_ask_r)->%s : %s; ",
                                    c_type, reply_msg_name, reply_field, c_zero);
                            /* #2528: the reply's strings were copied and its
                             * closures taken for the asker: the field read
                             * out is the asker's now (cleared so the release
                             * skips it), every other owned field goes with
                             * the buffer. */
                            if (strcmp(reply_release, "NULL") != 0) {
                                fprintf(gen->output, "if (_ask_r) { ((%s*)_ask_r)->%s = %s; %s_release_fields((%s*)_ask_r); } ",
                                        reply_msg_name, reply_field, c_zero, reply_msg_name, reply_msg_name);
                            }
                            fprintf(gen->output, "free(_ask_r); _ask_val; })");
                        } else if (reply_scalar_kind != TYPE_UNKNOWN) {
                            /* Expression reply (#1324): the handler sent a
                             * typed copy; deref the buffer as that type. */
                            fprintf(gen->output, "%s _ask_val = _ask_r ? *(%s*)_ask_r : %s; ",
                                    c_type, c_type, c_zero);
                            fprintf(gen->output, "free(_ask_r); _ask_val; })");
                        } else {
                            /* No reply statement found in the target's
                             * handler: any reply that still arrives (e.g.
                             * via a helper function the scan cannot see)
                             * is deref'd as int, mirroring the MSVC
                             * _aether_ask_helper semantics; a timeout
                             * yields 0. */
                            fprintf(gen->output, "int _ask_val = _ask_r ? *(int*)_ask_r : 0; free(_ask_r); _ask_val; })");
                        }

                        fprintf(gen->output, "\n#else\n");
                        /* MSVC: the helper delivers the field whole into a
                         * zeroed compound literal of its type (block
                         * lifetime) and returns it, read here as that type:
                         * the same value and the same release as the GCC
                         * path (#2528). */
                        gen->ask_temp_counter++;
                        fprintf(gen->output, "(*(%s*)_aether_ask_helper(", c_type);
                        emit_send_target(gen, target, "ActorBase*");
                        fprintf(gen->output, ", &(%s){ ._message_id = %d",
                                message->value, msg_def->message_id);
                        for (int i = 0; i < message->child_count; i++) {
                            ASTNode* field_init = message->children[i];
                            if (field_init && field_init->type == AST_FIELD_INIT) {
                                fprintf(gen->output, ", .%s = ", field_init->value);
                                if (field_init->child_count > 0) {
                                    MessageFieldDef* fdef = find_msg_field(msg_def, field_init->value);
                                    emit_message_field_init(gen, fdef, field_init->children[0]);
                                }
                            }
                        }
                        fprintf(gen->output, " }, sizeof(%s), %d, ", message->value, timeout_ms);
                        if (reply_msg_name && reply_field) {
                            fprintf(gen->output, "offsetof(%s, %s), sizeof(%s), %s, (void*)&(%s){0}))",
                                    reply_msg_name, reply_field, c_type, reply_release, c_type);
                        } else {
                            fprintf(gen->output, "0, sizeof(%s), NULL, (void*)&(%s){0}))", c_type, c_type);
                        }
                        fprintf(gen->output, "\n#endif\n");
                    } else {
                        fprintf(gen->output, "/* ERROR: unknown message type %s */", message->value ? message->value : "<?>");
                    }
                }
            }
            break;

        case AST_CLOSURE: {
            // Emit inline closure construction
            // The closure's value field was set to its ID by discover_closures
            int id = expr->value ? atoi(expr->value) : 0;
            int cap_count = 0;
            char** captures = NULL;
            const char* cl_parent_func = NULL;
            // Find this closure's info
            for (int ci = 0; ci < gen->closure_count; ci++) {
                if (gen->closures[ci].id == id) {
                    cap_count = gen->closures[ci].capture_count;
                    captures = gen->closures[ci].captures;
                    cl_parent_func = gen->closures[ci].parent_func;
                    break;
                }
            }
            if (cap_count == 0) {
                // Zero-capture closure: NULL env is safe
                fprintf(gen->output, "(_AeClosure){ .fn = (void(*)(void))_closure_fn_%d, .env = NULL }",
                        id);
            } else {
                // Heap-allocate the environment (portable, no use-after-free)
                fprintf(gen->output, "\n#if AETHER_GCC_COMPAT\n");
                fprintf(gen->output, "({ _closure_env_%d* _e = malloc(sizeof(_closure_env_%d)); _e->_dtor = _closure_env_%d_free; atomic_init(&_e->_refs, 1); ", id, id, id);
                for (int i = 0; i < cap_count; i++) {
                    if (capture_is_closure_value(gen, captures[i], cl_parent_func)) {
                        /* env owns a reference to the captured env (#2494) */
                        fprintf(gen->output, "_e->%s = %s; _aether_closure_env_retain(%s.env); ",
                                captures[i], captures[i], captures[i]);
                    } else if (capture_owning_struct(gen, captures[i], cl_parent_func)) {
                        /* env owns a copy of the struct's strings (#2504) */
                        fprintf(gen->output, "_e->%s = %s_dup(%s); ", captures[i],
                                capture_owning_struct(gen, captures[i], cl_parent_func), captures[i]);
                    } else if (capture_is_promoted(gen, captures[i], cl_parent_func)) {
                        /* env owns a reference to the shared cell (#2019) */
                        const char* cell = promoted_cell_pointer(lookup_var_c_type(gen, captures[i], cl_parent_func),
                                                                 NULL);
                        fprintf(gen->output, "_e->%s = (%s)_aether_cell_retain(%s); ",
                                captures[i], cell, captures[i]);
                    } else if (capture_is_retained_string(gen, captures[i], cl_parent_func)) {
                        /* env owns a reference — see aether_str_capture preamble */
                        const char* ctype = lookup_var_c_type(gen, captures[i], cl_parent_func);
                        fprintf(gen->output, "_e->%s = (%s)aether_str_capture(%s); ",
                                captures[i], ctype, captures[i]);
                    } else if (capture_sized_array_type(gen, captures[i], cl_parent_func)) {
                        /* #2464: an array does not assign; copy its bytes. */
                        fprintf(gen->output, "memcpy(_e->%s, %s, sizeof(_e->%s)); ",
                                captures[i], captures[i], captures[i]);
                    } else {
                        fprintf(gen->output, "_e->%s = %s; ", captures[i], captures[i]);
                    }
                }
                fprintf(gen->output, "(_AeClosure){ .fn = (void(*)(void))_closure_fn_%d, .env = _e }; })", id);
                fprintf(gen->output, "\n#else\n");
                // MSVC: use _aether_make_closure helper (emitted in preamble)
                fprintf(gen->output, "_aether_make_closure_%d(", id);
                for (int i = 0; i < cap_count; i++) {
                    if (i > 0) fprintf(gen->output, ", ");
                    fprintf(gen->output, "%s", captures[i]);
                }
                fprintf(gen->output, ")");
                fprintf(gen->output, "\n#endif\n");
            }
            break;
        }

        case AST_CLOSURE_PARAM:
            // Should not be generated directly
            break;

        default:
            for (int i = 0; i < expr->child_count; i++) {
                generate_expression(gen, expr->children[i]);
            }
            break;
    }
}

/* Emits `expr` bridged across the int <-> pointer boundary when `target`
 * and the expression's type sit on opposite sides of it; returns 0 and
 * emits nothing otherwise. Handles such as the injected `_builder`
 * (typed `ptr`, lowered to `void*`) flow into `int` slots and back, and a
 * bare conversion is rejected by GCC 14+/MinGW64 under default
 * -Werror=int-conversion. Used for call arguments and for returns (#2218:
 * `return _builder` from a builder declared `-> int`). See
 * builder-ctx-handle-void-ptr-int-conversion.md. */
int emit_int_ptr_bridged(CodeGenerator* gen, ASTNode* expr, TypeKind target) {
    if (!expr || !expr->node_type) return 0;
    TypeKind have = expr->node_type->kind;
    const char* cast = NULL;
    if (target == TYPE_PTR && (have == TYPE_INT || have == TYPE_BOOL)) {
        cast = "(void*)(intptr_t)(";
    } else if ((target == TYPE_INT || target == TYPE_BOOL) && have == TYPE_PTR) {
        cast = "(int)(intptr_t)(";
    }
    if (!cast) return 0;
    fprintf(gen->output, "%s", cast);
    generate_expression(gen, expr);
    fprintf(gen->output, ")");
    return 1;
}
