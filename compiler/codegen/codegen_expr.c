#include "../analysis/quote.h"
#include "../analysis/hoist.h"
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
 * frees them after the parent call returns. A sequence of temporaries
 * (TempSeq, #2765), so the call still composes in any expression
 * context: a comma expression, the temporaries declared ahead of the
 * statement.
 *
 * Generated shape (parent returns non-void):
 *
 *     (_ad_0 = (char*)(heap_value()),
 *      _ad_1 = (char*)(heap_value()),
 *      _ad_r2 = combine(_ad_0, _ad_1),
 *      aether_heap_str_free(_ad_0),
 *      aether_heap_str_free(_ad_1),
 *      _ad_r2)
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

/* A scalar kind that splats across a lane value's lanes. */
int is_scalar_splat_kind(TypeKind kind) {
    return kind == TYPE_INT || kind == TYPE_INT64 || kind == TYPE_UINT64 ||
           kind == TYPE_UINT32 || kind == TYPE_UINT16 || kind == TYPE_UINT8 ||
           kind == TYPE_BYTE || kind == TYPE_FLOAT || kind == TYPE_FLOAT32;
}

/* The C element type of a 16-byte lane vector, which C's own operators apply
 * to (f32x4, f64x2, i32x4, i64x2, i16x8), NULL for anything else. f32x8 /
 * i32x8 go through their _ae_*_ helpers, which take a splat of their own. */
const char* lane_element_c_type(TypeKind kind) {
    switch (kind) {
        case TYPE_F32X4: return "float";
        case TYPE_F64X2: return "double";
        case TYPE_I32X4: return "int32_t";
        case TYPE_I64X2: return "int64_t";
        case TYPE_I16X8: return "int16_t";
        default: return NULL;
    }
}

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

/* The field reads evaluated ahead of the call that takes them, a free or a
 * keep (order_hoist_handoff, ctl_hoist_operand): their temporary owns the
 * string from the read on, and `carrier` is the defer that frees it on an
 * exit taken before `call` runs (push_handover_exit_free), dropped once
 * the call is written. An entry lives as long as the binding at `order_at`
 * does. */
typedef struct {
    ASTNode* node;
    ASTNode* call;
    ASTNode* carrier;
    int order_at;
    int taken;   /* STRING_FIELD_TAKEN: the release stays until the scope exits */
} OrderHandover;
static OrderHandover* g_handovers = NULL;
static int g_handover_count = 0;
static int g_handover_cap = 0;
static const ASTNode* g_handover_call_emitting = NULL;
static CodeGenerator* g_handover_gen = NULL;   /* whose defer stack the entries' carriers are on */

/* Is `call` the call of a field read evaluated ahead whose exit free is
 * still on the defer stack? */
static int order_handover_pending(const ASTNode* call) {
    for (int i = g_handover_count - 1; call && i >= 0; i--)
        if (g_handovers[i].call == call && g_handovers[i].carrier) return 1;
    return 0;
}

/* `call` is written: the strings it takes are its own from here on, so
 * the exit frees of the fields read ahead for it are dropped. */
static void order_handover_done(CodeGenerator* gen, const ASTNode* call) {
    for (int i = 0; i < g_handover_count; i++) {
        if (g_handovers[i].call != call || !g_handovers[i].carrier) continue;
        if (!g_handovers[i].taken) drop_defer(gen, g_handovers[i].carrier);
        g_handovers[i].carrier = NULL;
    }
}

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

/* #2890: a closure stored into a `ptr` slot (a struct field, a module
 * global, a message field) is boxed, as one given to a `ptr` parameter
 * is: the slot holds `_aether_box_closure(...)`, which unbox_closure gives
 * back. The box is a holder like a `fn` field (#2525) and keeps a
 * reference of its own to the env, taken as emit_closure_take takes one.
 * A bare top-level function (a `@c_callback` one among them) is no
 * closure: in a `ptr` slot it stays the raw code address C calls through
 * (`qsort(buf, n, sz, s.cmp)`), as docs/memory-management.md states and
 * unbox_closure's panic says; box_closure or an `fn` parameter boxes one.
 * Emits the boxed closure or the function's address and returns 1 when
 * `v` is either (fn_value_into_ptr), else emits nothing and returns
 * 0, and the caller emits the value as it is. Every `ptr` store goes
 * through here, so none of them hands C an `_AeClosure` where it expects a
 * `void*`, nor a box where it expects code. */
int fn_value_into_ptr(CodeGenerator* gen, ASTNode* v) {
    Type* t = v ? v->node_type : NULL;
    return (t && t->kind == TYPE_FUNCTION && !t->is_fnptr) || bare_top_level_fn(gen, v) != NULL;
}

int emit_closure_into_ptr(CodeGenerator* gen, ASTNode* v) {
    if (!fn_value_into_ptr(gen, v)) return 0;
    if (bare_top_level_fn(gen, v)) {
        /* The address the function is called at: its `@c_callback`
         * symbol, else its C name, as `f as fn(...)` spells it. An
         * identifier marked as a function value would be wrapped in a
         * closure struct by generate_expression. */
        const char* cb = lookup_c_callback_symbol(gen, v->value);
        fprintf(gen->output, "%s", cb ? cb : safe_c_name(v->value));
        return 1;
    }
    fprintf(gen->output, "_aether_box_closure(");
    emit_closure_take(gen, v);
    fprintf(gen->output, ")");
    return 1;
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
    int closure[16];    /* 1 = an owned closure (#2507), released through its env;
                         * 2 = a closure cell's value, held for the call (#2755):
                         * retained into the temp, released the same way */
    int scoped[16];     /* 1 = lives to its scope's exit: the call returns a view, which
                         * may point into it (call_returns_view) */
    int held[16];       /* 1 = a string read from a place the callee can rewrite, taken
                         * for the call (string_arg_held) */
    int have_value;     /* the call yields a value: of C type ret_ct, or
                         * when that is NULL, of type ret_type */
    const char* ret_ct;
    Type* ret_type;
    int discarded;      /* the value is discarded at statement level */
    TempSeq seq;        /* the temporaries (#2765) */
    const char* r;      /* the call's value, when it has one */
} ArgDrainWrap;

/* R3: a call borrows its arguments, and a callee that keeps or returns one
 * takes a reference of its own, so a fresh argument is the caller's to free
 * after the call. Not when the call takes it over: an `@retain` extern
 * parameter (R4) or a function that frees its argument. */
static int arg_taken_by_call(CodeGenerator* gen, const char* func_name, int ai) {
    return func_name && (is_retain_extern_param(gen, func_name, ai) ||
                         call_frees_arg(gen, codegen_normalise_callee(func_name)));
}

/* #2519: does a call to user function `func_name` yield a value in C? 1 yes,
 * its C type then in `*ct`, 0 no (a void function), -1 not known (no visible
 * definition). The rule the signature is emitted by (fn_result_type): the
 * declared type, else `int` when the body returns a value, else void,
 * decided over every clause of a set (#2645). */
static int callee_result_shape(CodeGenerator* gen, const char* func_name, const char** ct) {
    if (!callee_has_visible_body(gen, func_name)) return -1;
    const char* fn = codegen_normalise_callee(func_name);
    const DefClauses* dc = program_index_clauses(gen->program, fn);
    if (!dc || dc->count < 1 || !dc->nodes[0]) return -1;
    Type* t = fn_result_type(gen, dc->nodes[0]);
    if (!t) return 0;
    *ct = get_c_type(t);
    return 1;
}

/* #2507: is the owned closure at child `ai` of call `expr` released once the
 * call returns? Yes (R3), unless the call takes it over (arg_taken_by_call)
 * or a `ptr` parameter that keeps it is given its box. A function taking a
 * builder's `_ctx` first is called with it, or without it (injected, its
 * parameters then one position on); named and DSL trailing arguments
 * shift the positions in ways the call does not show, so those calls leave
 * it to the box or the builder. */
static int owned_closure_arg_drainable(CodeGenerator* gen, ASTNode* expr, const char* func_name,
                                       int ai) {
    if (!func_name) return 1;
    for (int i = 0; i < expr->child_count; i++) {
        ASTNode* a = expr->children[i];
        if (a && (a->type == AST_NAMED_ARG ||
                  (a->type == AST_CLOSURE && a->value && strcmp(a->value, "trailing") == 0))) return 0;
    }
    const char* fn = codegen_normalise_callee(func_name);
    ASTNode* fdef = find_function_definition_by_name(gen->program, fn);
    int pi = ai;
    if (fdef && fdef->child_count > 0 && fdef->children[0] && fdef->children[0]->value &&
        strcmp(fdef->children[0]->value, "_ctx") == 0) {
        int np = fn_param_count(fdef);
        if (expr->child_count == np - 1) pi = ai + 1;
        else if (expr->child_count != np) return 0;
    }
    return !arg_taken_by_call(gen, func_name, pi) && !call_param_keeps(gen, func_name, pi);
}

/* Pick the arguments of `expr` (from child `first_arg` on) to hoist. The
 * caller has set have_value, ret_ct and discarded. A VOID parent (e.g. an
 * assert-style helper `check(label, string.from_long(n), want)`) yields no
 * value, but its heap-returning inline args still leak: they are drained
 * by a wrap that yields void, `(t = ..., call(...), free(t))`, valid
 * wherever a void call appears. A value of unknown type is
 * drained only when it is discarded at the statement level, where the wrap
 * yields void and is safe regardless of the declared type. Args that are
 * themselves substitution-registered (inside a parent wrap already) are
 * left to the registered entry. */
/* A call whose result is a view (a slice or a `ptr`) may return one into a
 * string argument, as `string.bytes(s)` does: a temporary passed to it lives
 * as a temporary does in C++, to the end of the statement that made it, so
 * the view is good for that statement. The statement's block releases it on
 * every exit, and a statement run again first releases the one before
 * (arg_drain_open). */
static int call_returns_view(ASTNode* call) {
    Type* t = call ? call->node_type : NULL;
    return t && (type_is_slice(t) || t->kind == TYPE_PTR);
}

/* R3: a callee borrows its arguments, so each must outlive the call. A
 * string read from a place the callee can reach and rewrite (a module `var`,
 * a field or an element, by any path) does not on its own: storing into that
 * place inside the call releases what the argument points at. The caller
 * holds such an argument for the call, a reference of its own released
 * after it. A local or a temporary of the caller needs no hold: the callee
 * cannot reach it. */
/* Is `e` a place a call can reach and store into: a field or an element, by
 * any path, or a module `var` no local shadows? */
static int reachable_place(CodeGenerator* gen, ASTNode* e) {
    if (!e) return 0;
    if (e->type == AST_MEMBER_ACCESS || e->type == AST_ARRAY_ACCESS) return 1;
    return e->type == AST_IDENTIFIER && e->value && !is_var_declared(gen, e->value) &&
           is_module_global_var(gen, e->value);
}

/* Is `e` a string read from such a place: the place itself, or `o ?? d` /
 * `o!` over a `string?` one? */
int string_reads_place(CodeGenerator* gen, ASTNode* e) {
    if (!e || !e->node_type || e->node_type->kind != TYPE_STRING) return 0;
    if ((e->type == AST_NULL_COALESCE || e->type == AST_TUPLE_UNWRAP) && e->child_count > 0)
        return reachable_place(gen, e->children[0]);
    return reachable_place(gen, e);
}

static int string_arg_held(CodeGenerator* gen, ASTNode* arg) {
    return string_reads_place(gen, arg);
}

static void arg_drain_select(CodeGenerator* gen, ASTNode* expr, int first_arg,
                             const char* func_name, ASTNode* closure, int is_void,
                             ArgDrainWrap* w) {
    w->saved_count = g_arg_drain_count;
    w->count = 0;
    memset(w->scoped, 0, sizeof(w->scoped));
    memset(w->held, 0, sizeof(w->held));
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
        /* #2755: a closure cell's value lent to the call is held for it:
         * the call may rebind the cell (the closure it runs replacing
         * itself), and the store gives back the cell's reference. */
        if (is_closure_cell_read(gen, arg)) {
            if (!arg_drain_lookup(arg)) {
                w->closure[w->count] = 2;
                w->idx[w->count++] = ai;
            }
            continue;
        }
        /* A callee that frees or keeps its argument takes the place's own
         * reference, so a hold of it would be what it takes instead. */
        if (string_arg_held(gen, arg) && !arg_taken_by_call(gen, func_name, ai)) {
            if (!arg_drain_lookup(arg)) {
                w->closure[w->count] = 0;
                w->held[w->count] = 1;
                w->scoped[w->count] = call_returns_view(expr) && hoist_ready(gen);
                w->idx[w->count++] = ai;
            }
            continue;
        }
        /* #2518: a capturing closure literal, as #2507 does a handed-over
         * closure: its only reference is the argument (a list that stores
         * it takes its own). */
        int literal_arg = arg->type == AST_CLOSURE &&
                          !(arg->value && strcmp(arg->value, "trailing") == 0);
        if (arg->type != AST_FUNCTION_CALL &&
            arg->type != AST_STRING_INTERP &&
            arg->type != AST_OR_ELSE && !literal_arg &&
            !is_owned_string_unwrap(gen, arg)) continue;
        if (arg_drain_lookup(arg)) continue;
        /* #2507: a closure a call hands over (call_returns_owned_closure),
         * passed on as an argument anywhere in an expression: dead after
         * the call when the parameter keeps nothing. */
        if (literal_arg || call_returns_owned_closure(gen, arg)) {
            if (owned_closure_arg_drainable(gen, expr, func_name, ai)) {
                w->closure[w->count] = 1;
                w->idx[w->count++] = ai;
            }
            continue;
        }
        if (!is_heap_string_expr(gen, arg) || arg_taken_by_call(gen, func_name, ai)) continue;
        /* Wrapped into an optional or a tuple parameter, the string is that
         * argument's temporary's (emit_composite_arg_temp). */
        TypeKind pk = func_name ? lookup_callee_param_kind(gen, func_name, ai) : TYPE_UNKNOWN;
        if (pk == TYPE_OPTIONAL || pk == TYPE_TUPLE) continue;
        w->scoped[w->count] = call_returns_view(expr) && hoist_ready(gen);
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
    seq_open(gen, &w->seq);
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
            seq_let_named(gen, &w->seq, "_AeClosure", names[h]);
            if (w->closure[h] == 2) fprintf(gen->output, "_aether_closure_retain(");
            generate_expression(gen, arg);
            if (w->closure[h] == 2) fprintf(gen->output, ")");
        } else if (w->scoped[h]) {
            /* Journaled where it is set, at the frame of the block that
             * releases it: the declaration is hoisted ahead of the whole
             * statement, which may be a `try` around this block. */
            hoist_declare_null(gen, "char*", names[h]);
            fprintf(gen->output, "aether_unwind_track_str_place((const void*)&%s, NULL), "
                    "aether_heap_str_free(%s), %s = (char*)(", names[h], names[h], names[h]);
            if (w->held[h]) fprintf(gen->output, "aether_uniform_heap_str((const char*)(");
            generate_expression(gen, arg);
            if (w->held[h]) fprintf(gen->output, "), 0)");
            fprintf(gen->output, ")");
            push_scoped_temp_free(gen, names[h]);
        } else {
            seq_let_named(gen, &w->seq, "char*", names[h]);
            fprintf(gen->output, w->held[h] ? "(char*)aether_uniform_heap_str((const char*)(" : "(char*)(");
            generate_expression(gen, arg);
            fprintf(gen->output, w->held[h] ? "), 0)" : ")");
        }
        seq_next(gen, &w->seq);
        /* #2923: journaled for the call, which may panic. */
        if (w->closure[h])
            fprintf(gen->output, "aether_unwind_track((const void*)&%s, _aether_closure_drop_unwind)",
                    names[h]);
        else if (!w->scoped[h])
            fprintf(gen->output, "aether_unwind_track_str_place((const void*)&%s, NULL)", names[h]);
        else
            continue;
        seq_next(gen, &w->seq);
    }
    /* Now bind the outer-arg to outer-temp substitutions. The bind
     * transfers ownership of the name string; arg_drain_truncate frees
     * it. */
    for (int h = 0; h < w->count; h++) {
        arg_drain_bind(expr->children[w->idx[h]], names[h]);
    }
    /* A void parent emits the call bare (no _ad_r); the wrap yields void
     * through the final free in arg_drain_close. */
    w->r = NULL;
    if (w->have_value) {
        w->r = seq_let(gen, &w->seq, w->ret_ct ? w->ret_ct : get_c_type(w->ret_type), "_ad_r");
    }
}

/* Close the wrap, if one was opened: the per-temp free, the wrap's value,
 * and its end. */
static void arg_drain_close(CodeGenerator* gen, ASTNode* expr, ArgDrainWrap* w) {
    if (w->count == 0) return;
    for (int h = 0; h < w->count; h++) {
        /* Look up the temp name we registered. Names are stable across
         * the wrap's scope. */
        const char* nm = arg_drain_lookup(expr->children[w->idx[h]]);
        if (!nm || w->scoped[h]) continue;
        seq_next(gen, &w->seq);
        fprintf(gen->output, "aether_unwind_forget((const void*)&%s)", nm);
        seq_next(gen, &w->seq);
        if (w->closure[h]) {
            fprintf(gen->output, "_aether_closure_env_release(%s.env)", nm);
        } else {
            fprintf(gen->output, "aether_heap_str_free(%s)", nm);
        }
    }
    if (w->have_value) {
        /* In statement position the value is discarded, and a bare
         * `_ad_r;` there is -Wunused-value in the user's own build
         * (`fs.delete("${dir}/f")` warned). Cast it away: the wrap then
         * yields void, which is all a statement wants. */
        seq_next(gen, &w->seq);
        fprintf(gen->output, w->discarded ? "(void)%s" : "%s", w->r);
    }
    /* A void parent: the trailing free is the last part, so the wrap
     * yields void. */
    seq_close(gen, &w->seq);
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
    /* An `or` over a heap value slot yields a string of its own (the
     * handler's default boxed): read by the interpolation, it leaked. Its
     * node's own type is not reliably stamped; is_heap_string_expr reads the
     * fallible's value slot. */
    if (ch->type == AST_OR_ELSE) return is_heap_string_expr(gen, ch);
    if (ch->type != AST_FUNCTION_CALL && ch->type != AST_STRING_INTERP &&
        !is_owned_string_unwrap(gen, ch)) return 0;   /* #2750 */
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
 * read only plain locals and literals stay inline.
 *
 * Memory a call writes through a pointer or a slice may be what another
 * pointer of the caller reaches: one it keeps in a module variable, one it
 * was handed under another name, one read out of a field (#2813). So a
 * call that writes through any pointer or slice is ordered against every
 * operand that reads through one (a field of a pointer, an element of a
 * slice, a pointer or slice handed to a call, a call that reads through
 * one, a call the compiler cannot see), not only against reads rooted at
 * the argument it writes through. A call that reads through a pointer it
 * was not handed (one kept in a module variable, one inside a struct value
 * it was handed) reads memory a call that writes through a pointer may
 * change, in either order (#2813).
 *
 * The effects are a property of the call graph: a function's are its
 * body's and its callees'. They are solved over the functions a query
 * reaches, component by component (order_fn_effects), so mutually
 * recursive functions get the same answer whichever is asked about first,
 * in time linear in the size of the call graph. */

typedef struct OrderFnEffects {
    ASTNode* fn;
    int opaque;                /* makes a call the compiler cannot see */
    int writes_shared;         /* a module global, or runs a closure */
    unsigned writes_through;   /* bit k: memory its parameter k reaches */
    int writes_memory;         /* writes through any pointer or slice */
    int reads_memory;          /* reads through any pointer or slice */
    int solved;                /* the fields above are final */
    int* callees;              /* the entries its body calls (once each) */
    int ncallees;
    int scc_index;             /* Tarjan: order of discovery, -1 unseen */
    int scc_low;
    int scc_on_stack;
} OrderFnEffects;

/* The definition a call runs, when it is a function of this program, or
 * the declaration of the extern it is. For a clause set it is the first
 * clause, whose entry stands for the whole set (order_fn_clause). */
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

/* Is `t` a reference to memory the caller may share: a pointer, or a slice
 * (an unsized array, `{ ptr, len }`)? A fixed-size array is a value. */
static int order_is_memory_ref(const Type* t) {
    return t && (t->kind == TYPE_PTR || (t->kind == TYPE_ARRAY && t->array_size <= 0));
}

/* Does the lvalue or read `node` (a field or an element, at any depth)
 * reach its memory through a pointer or a slice? */
static int order_access_through_memory(const ASTNode* node) {
    while (node && (node->type == AST_MEMBER_ACCESS || node->type == AST_ARRAY_ACCESS) &&
           node->child_count > 0) {
        const ASTNode* obj = node->children[0];
        if (obj && order_is_memory_ref(obj->node_type)) return 1;
        node = obj;
    }
    return 0;
}

/* The position of `fn`'s parameter `name` among its parameters, which is
 * the position of the argument a call hands it: a literal or a list pattern
 * takes a position too (`wr(0, b: *Box)` has `b` second), and a guard or a
 * contract between them takes none. */
static int order_param_index(ASTNode* fn, const char* name) {
    for (int k = 0; name && k < 32; k++) {
        ASTNode* p = fn_param_at(fn, k);
        if (!p) return -1;
        if ((p->type == AST_PATTERN_VARIABLE || p->type == AST_VARIABLE_DECLARATION) &&
            p->value && strcmp(p->value, name) == 0) return k;
    }
    return -1;
}

/* order_param_index for a write through the parameter `name`, which reaches
 * its caller's memory: -1 for a fixed-size array parameter, the function's
 * own copy of the caller's array (#2516, as E0900 treats it, #2785). A store
 * into it, or into it by a callee handed it, changes nothing a caller reads,
 * and marked as one, the caller's later reads of the array it passed were
 * held in temporaries for nothing (#2887). */
static int order_shared_param_index(ASTNode* fn, const char* name) {
    int k = order_param_index(fn, name);
    ASTNode* p = k >= 0 ? fn_param_at(fn, k) : NULL;
    return p && type_is_sized_array(p->node_type) ? -1 : k;
}

/* What a call of entry `fn` runs (#2857). `fn` is the definition
 * order_callee_def finds for a name: the first clause of a clause set, and a
 * call of the set may run any clause, so its effects are every clause's.
 * Clause `k` of the set `fn` is, or `fn` itself for a single definition. */
static int order_fn_clause_count(CodeGenerator* gen, ASTNode* fn) {
    const DefClauses* dc = fn_def_clause_set(gen->program, fn);
    return dc ? dc->count : 1;
}

static ASTNode* order_fn_clause(CodeGenerator* gen, ASTNode* fn, int k) {
    const DefClauses* dc = fn_def_clause_set(gen->program, fn);
    return dc ? (k < dc->count ? dc->nodes[k] : NULL) : (k == 0 ? fn : NULL);
}

/* A child of a clause that a call of it evaluates: its body, its guard
 * (which the set's dispatcher tests), a `requires` or an `ensures` check
 * (unless --no-contracts drops them). Its parameter patterns only compare
 * and bind what the call was handed. */
static int order_clause_part_runs(CodeGenerator* gen, const ASTNode* c) {
    if (!c) return 0;
    if (c->type == AST_BLOCK || c->type == AST_GUARD_CLAUSE) return 1;
    return (c->type == AST_REQUIRES_CLAUSE || c->type == AST_ENSURES_CLAUSE) &&
           !gen->no_contracts;
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

/* The effects of `node`, a part of `fn`'s own body, and of the calls in it,
 * ORed into `e`. */
static void order_scan_body(CodeGenerator* gen, ASTNode* fn, ASTNode* node, OrderFnEffects* e) {
    if (!node || node->type == AST_CLOSURE) return;
    const char* t = order_write_target(node);
    if (t && is_module_global_var(gen, t)) e->writes_shared = 1;
    ASTNode* lv = order_through_lvalue(node);
    int k = lv ? order_shared_param_index(fn, order_lvalue_root(lv)) : -1;
    if (k >= 0) e->writes_through |= 1u << k;
    if (lv && order_access_through_memory(lv)) e->writes_memory = 1;
    if ((node->type == AST_MEMBER_ACCESS || node->type == AST_ARRAY_ACCESS) &&
        order_access_through_memory(node)) e->reads_memory = 1;
    if (node->type == AST_FUNCTION_CALL) {
        int idx = order_call_effects(gen, node);
        if (idx < 0) {
            e->opaque = 1;
        } else {
            if (gen->order_fn_effects[idx].writes_shared) e->writes_shared = 1;
            if (gen->order_fn_effects[idx].writes_memory) e->writes_memory = 1;
            if (gen->order_fn_effects[idx].reads_memory) e->reads_memory = 1;
            unsigned c_through = gen->order_fn_effects[idx].writes_through;
            for (int i = 0, a = 0; i < node->child_count && a < 32; i++) {
                ASTNode* arg = node->children[i];
                if (arg && arg->type == AST_CLOSURE && arg->value &&
                    strcmp(arg->value, "trailing") == 0) continue;
                if (c_through & (1u << a)) {
                    int pk = order_shared_param_index(fn, order_ref_arg_root(arg));
                    if (pk >= 0) e->writes_through |= 1u << pk;
                }
                a++;
            }
        }
    } else if (node->type == AST_SEND_FIRE_FORGET || node->type == AST_SEND_ASK ||
               node->type == AST_VA_ARG) {
        /* A handler runs code the body does not show; a varargs read
         * advances state the caller handed over. */
        e->opaque = 1;
    }
    for (int i = 0; i < node->child_count; i++)
        order_scan_body(gen, fn, node->children[i], e);
}

/* The entry of `fn`, a function of this program, by a hash of its
 * definition (gen->order_fn_index, open addressing, an index + 1 per slot):
 * found, or added unsolved. */
static size_t order_fn_hash(const ASTNode* fn, int cap) {
    uint64_t h = (uint64_t)(uintptr_t)fn >> 4;
    h *= 0x9E3779B97F4A7C15ull;
    return (size_t)(h ^ (h >> 31)) & (size_t)(cap - 1);
}

static int order_fn_entry(CodeGenerator* gen, ASTNode* fn) {
    int cap = gen->order_fn_index_cap;
    for (size_t j = cap ? order_fn_hash(fn, cap) : 0; cap && gen->order_fn_index[j];
         j = (j + 1) & (size_t)(cap - 1)) {
        int i = gen->order_fn_index[j] - 1;
        if (gen->order_fn_effects[i].fn == fn) return i;
    }
    if (gen->order_fn_effect_count >= gen->order_fn_effect_capacity) {
        int ncap = gen->order_fn_effect_capacity ? gen->order_fn_effect_capacity * 2 : 32;
        gen->order_fn_effects = aether_xrealloc(gen->order_fn_effects,
                                                (size_t)ncap * sizeof(OrderFnEffects));
        gen->order_fn_effect_capacity = ncap;
    }
    int idx = gen->order_fn_effect_count++;
    gen->order_fn_effects[idx] = (OrderFnEffects){ .fn = fn, .scc_index = -1 };
    if ((gen->order_fn_effect_count) * 2 > cap) {
        int ncap = cap ? cap * 2 : 64;
        while (gen->order_fn_effect_count * 2 > ncap) ncap *= 2;
        int* t = (int*)calloc((size_t)ncap, sizeof(int));
        if (!t) {
            fprintf(stderr, "Fatal: out of memory indexing functions\n");
            exit(1);
        }
        for (int i = 0; i < gen->order_fn_effect_count; i++) {
            size_t j = order_fn_hash(gen->order_fn_effects[i].fn, ncap);
            while (t[j]) j = (j + 1) & (size_t)(ncap - 1);
            t[j] = i + 1;
        }
        free(gen->order_fn_index);
        gen->order_fn_index = t;
        gen->order_fn_index_cap = ncap;
    } else {
        size_t j = order_fn_hash(fn, cap);
        while (gen->order_fn_index[j]) j = (j + 1) & (size_t)(cap - 1);
        gen->order_fn_index[j] = idx + 1;
    }
    return idx;
}

/* The entries the calls in `node` (a part of a body) reach: the functions of
 * the program order_call_effects would look up, the edges of the graph the
 * effects are solved over. */
static void order_collect_callees(CodeGenerator* gen, ASTNode* node, int idx) {
    if (!node || node->type == AST_CLOSURE) return;
    if (node->type == AST_FUNCTION_CALL && !order_call_runs_closure(node)) {
        ASTNode* def = order_callee_def(gen, node);
        if (def && def->type != AST_EXTERN_FUNCTION) {
            int c = order_fn_entry(gen, def);
            OrderFnEffects* e = &gen->order_fn_effects[idx];
            e->callees = (int*)aether_xrealloc(e->callees, sizeof(int) * (size_t)(e->ncallees + 1));
            e->callees[e->ncallees++] = c;
        }
    }
    for (int i = 0; i < node->child_count; i++)
        order_collect_callees(gen, node->children[i], idx);
}

/* The edges of entry `i`: the calls in every part of every clause a call
 * of it runs. */
static void order_fn_collect_callees(CodeGenerator* gen, int i) {
    ASTNode* fn = gen->order_fn_effects[i].fn;
    int n = order_fn_clause_count(gen, fn);
    for (int k = 0; k < n; k++) {
        ASTNode* cl = order_fn_clause(gen, fn, k);
        for (int c = 0; cl && c < cl->child_count; c++)
            if (order_clause_part_runs(gen, cl->children[c]))
                order_collect_callees(gen, cl->children[c], i);
    }
}

/* Read entry `i`'s clauses once more with what their callees are known to
 * do; 1 when that adds an effect. Each clause is read against its own
 * parameters: a set's clauses may name them differently. */
static int order_fn_rescan(CodeGenerator* gen, int i) {
    OrderFnEffects e = gen->order_fn_effects[i];
    int n = order_fn_clause_count(gen, e.fn);
    for (int k = 0; k < n; k++) {
        ASTNode* cl = order_fn_clause(gen, e.fn, k);
        for (int c = 0; cl && c < cl->child_count; c++)
            if (order_clause_part_runs(gen, cl->children[c]))
                order_scan_body(gen, cl, cl->children[c], &e);
    }
    OrderFnEffects* cur = &gen->order_fn_effects[i];
    if (e.opaque == cur->opaque && e.writes_shared == cur->writes_shared &&
        e.writes_through == cur->writes_through && e.writes_memory == cur->writes_memory &&
        e.reads_memory == cur->reads_memory) return 0;
    cur->opaque = e.opaque;
    cur->writes_shared = e.writes_shared;
    cur->writes_through = e.writes_through;
    cur->writes_memory = e.writes_memory;
    cur->reads_memory = e.reads_memory;
    return 1;
}

/* Set while order_fn_effects solves: a query from a body being read gets
 * the entry as it stands. */
static int g_order_solving = 0;

/* The effects of calling `fn`, a function of this program: an index into
 * gen->order_fn_effects. The first query that reaches a function solves it
 * and every unsolved function its body reaches, by strongly connected
 * component of the call graph (Tarjan's algorithm, iterative), callees'
 * components first: a component's bodies are read with what the
 * components it calls do, final by then, and, when it is a cycle, reread
 * until a pass adds nothing. Each body is read once, a cycle's a few times,
 * so the time is linear in the size of the graph; one fixed point over
 * every reached function took a pass per level of a call chain. Answered
 * from a partial read instead, a function reached again while its own body
 * was being read (mutual recursion) fixed its caller's effects early, and
 * the answer depended on which function was asked about first (#2813). */
static int order_fn_effects(CodeGenerator* gen, ASTNode* fn) {
    int root = order_fn_entry(gen, fn);
    if (gen->order_fn_effects[root].solved || g_order_solving) return root;
    g_order_solving = 1;
    int* stack = NULL;         /* Tarjan's stack of entries */
    int nstack = 0, capstack = 0;
    int* frames = NULL;        /* the DFS: an entry, then its next edge */
    int nframes = 0, capframes = 0;
    int counter = 0;
#define ORDER_PUSH(arr, n, cap, v) do { \
        if ((n) == (cap)) { (cap) = (cap) ? (cap) * 2 : 64; \
            (arr) = (int*)aether_xrealloc((arr), sizeof(int) * (size_t)(cap)); } \
        (arr)[(n)++] = (v); } while (0)
    /* Visit: number it, read its edges, put it on both stacks. */
#define ORDER_VISIT(v) do { \
        OrderFnEffects* ve = &gen->order_fn_effects[(v)]; \
        ve->scc_index = ve->scc_low = counter++; \
        ve->scc_on_stack = 1; \
        ORDER_PUSH(stack, nstack, capstack, (v)); \
        order_fn_collect_callees(gen, (v)); \
        ORDER_PUSH(frames, nframes, capframes, (v)); \
        ORDER_PUSH(frames, nframes, capframes, 0); } while (0)
    ORDER_VISIT(root);
    while (nframes > 0) {
        int v = frames[nframes - 2];
        int pos = frames[nframes - 1];
        if (pos < gen->order_fn_effects[v].ncallees) {
            frames[nframes - 1] = pos + 1;
            int w = gen->order_fn_effects[v].callees[pos];
            OrderFnEffects* we = &gen->order_fn_effects[w];
            if (we->solved) continue;
            if (we->scc_index < 0) {
                ORDER_VISIT(w);
            } else if (we->scc_on_stack && we->scc_index < gen->order_fn_effects[v].scc_low) {
                gen->order_fn_effects[v].scc_low = we->scc_index;
            }
            continue;
        }
        nframes -= 2;
        OrderFnEffects* ve = &gen->order_fn_effects[v];
        if (nframes > 0) {
            OrderFnEffects* pe = &gen->order_fn_effects[frames[nframes - 2]];
            if (ve->scc_low < pe->scc_low) pe->scc_low = ve->scc_low;
        }
        if (ve->scc_low != ve->scc_index) continue;
        /* `v` roots a component: the entries above it on the stack. */
        int base = nstack;
        while (base > 0 && stack[base - 1] != v) base--;
        base--;
        int cycle = nstack - base > 1;
        for (int k = 0; k < gen->order_fn_effects[v].ncallees && !cycle; k++)
            cycle = gen->order_fn_effects[v].callees[k] == v;
        int changed;
        do {
            changed = 0;
            for (int k = base; k < nstack; k++)
                if (order_fn_rescan(gen, stack[k])) changed = 1;
        } while (cycle && changed);
        for (int k = base; k < nstack; k++) {
            OrderFnEffects* me = &gen->order_fn_effects[stack[k]];
            me->solved = 1;
            me->scc_on_stack = 0;
            free(me->callees);
            me->callees = NULL;
            me->ncallees = 0;
        }
        nstack = base;
    }
#undef ORDER_VISIT
#undef ORDER_PUSH
    free(stack);
    free(frames);
    g_order_solving = 0;
    return root;
}

void order_fn_effects_release(CodeGenerator* gen) {
    for (int i = 0; i < gen->order_fn_effect_count; i++)
        free(gen->order_fn_effects[i].callees);
    free(gen->order_fn_effects);
    gen->order_fn_effects = NULL;
    gen->order_fn_effect_count = 0;
    gen->order_fn_effect_capacity = 0;
    free(gen->order_fn_index);
    gen->order_fn_index = NULL;
    gen->order_fn_index_cap = 0;
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

/* Does `node` read memory through a pointer or a slice: a field or an
 * element reached through one, one handed to a call (a callee may read
 * through it), or a call that reads through one it reaches some other way
 * (a module variable, a field of a struct it was handed), or that the
 * compiler cannot see? */
static int order_reads_memory(CodeGenerator* gen, ASTNode* node) {
    if (!node || node->type == AST_CLOSURE) return 0;
    if ((node->type == AST_MEMBER_ACCESS || node->type == AST_ARRAY_ACCESS) &&
        order_access_through_memory(node)) return 1;
    if (node->type == AST_FUNCTION_CALL) {
        for (int i = 0; i < node->child_count; i++) {
            ASTNode* a = node->children[i];
            while (a && (a->type == AST_SLICE_FROM_ARRAY || a->type == AST_SLICE_TO_PTR ||
                         a->type == AST_NAMED_ARG) && a->child_count > 0)
                a = a->children[0];
            if (a && order_is_memory_ref(a->node_type)) return 1;
        }
        int idx = order_call_effects(gen, node);
        if (idx < 0 || gen->order_fn_effects[idx].reads_memory) return 1;
    }
    if (node->type == AST_SEND_ASK || node->type == AST_SEND_FIRE_FORGET) return 1;
    for (int i = 0; i < node->child_count; i++)
        if (order_reads_memory(gen, node->children[i])) return 1;
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
        /* One that writes through a pointer or a slice may write what `y`
         * reaches through another, read there or by a call (#2813). */
        if (idx >= 0 && gen->order_fn_effects[idx].writes_memory && order_reads_memory(gen, y))
            return 1;
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

static const char* ctl_lookup(const ASTNode* node);

/* An operand already standing for a temp: evaluated, with nothing left to
 * order. A construct run ahead of its statement (ctl_lookup) is one too. */
static int order_bound(ASTNode* op) {
    return op && (order_lookup(op) || arg_drain_lookup(op) || ctl_lookup(op));
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

static void order_handover_add(CodeGenerator* gen, ASTNode* node, ASTNode* call, ASTNode* carrier,
                               int kind) {
    g_handover_gen = gen;
    if (g_handover_count >= g_handover_cap) {
        int cap = g_handover_cap ? g_handover_cap * 2 : 8;
        g_handovers = (OrderHandover*)aether_xrealloc(g_handovers, sizeof(OrderHandover) * (size_t)cap);
        g_handover_cap = cap;
    }
    g_handovers[g_handover_count++] =
        (OrderHandover){ node, call, carrier, g_order_count - 1, kind == STRING_FIELD_TAKEN };
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

/* Does C promote a value of Aether type `t` to int wherever it is an
 * operand? An integer type narrower than int (byte, uint8_t, uint16_t,
 * int8_t, int16_t, bool, an enum, a bitstruct over one) is, and so is an
 * int under any alias. */
static int type_promotes_to_int(const Type* t) {
    switch (t->kind) {
        case TYPE_INT: case TYPE_BOOL: case TYPE_BYTE: case TYPE_UINT8: case TYPE_UINT16:
        case TYPE_ENUM:
            return 1;
        case TYPE_BITSTRUCT:
            return !t->element_type || type_promotes_to_int(t->element_type);
        default:
            return 0;
    }
}

/* #2765: the C type an operand's temp is declared with where __auto_type
 * cannot infer it: the type C gives the operand, so the temp holds the
 * value the operation would have read in place. That is the operand's
 * Aether type after C's integer promotions: the checker types `b + b`, `~b`
 * and `-b` on a byte as a byte, which C computes in int, and a temp of the
 * narrow type cut the value (`(b + b) + f()` with b = 200 gave 145, not
 * 401). An operand of a narrow type that is no computation (a variable, a
 * call) holds the same value in an int, and every operator promotes it
 * there anyway. NULL when the operand has no type a declaration can spell
 * (an unresolved type, an array, which the operand decays from). */
static const char* order_c11_type(ASTNode* op) {
    Type* t = op ? op->node_type : NULL;
    while (t && t->kind == TYPE_ISOLATED && t->element_type) t = t->element_type;
    if (!t || t->kind == TYPE_UNKNOWN || t->kind == TYPE_VOID || t->kind == TYPE_WILDCARD ||
        (t->kind == TYPE_ARRAY && t->array_size > 0)) return NULL;
    if (type_promotes_to_int(t)) return "int";
    return get_c_type(t);
}

/* Evaluate operand `op` into a fresh temp of sequence `s` and bind it:
 * `_eoN = (op)`, then the separator. */
static void order_hoist_operand(CodeGenerator* gen, TempSeq* s, ASTNode* op) {
    const char* name = cg_internf("_eo%d", g_order_counter++);
    if (hoist_ready(gen) && string_reads_place(gen, op)) {
        /* R3: a string read from a place a later operand can store into
         * holds a reference of its own until its statement is done, so
         * the store cannot free what it reads; a statement run again
         * releases the one before. */
        hoist_declare_null(gen, "const char*", name);
        fprintf(gen->output, "aether_unwind_track_str_place((const void*)&%s, NULL), "
                "aether_heap_str_free(%s), %s = aether_uniform_heap_str((const char*)(",
                name, name, name);
        generate_expression(gen, op);
        fprintf(gen->output, "), 0)");
        push_scoped_temp_free(gen, name);
        seq_next(gen, s);
        order_bind(op, name);
        return;
    }
    seq_let_named(gen, s, order_c11_type(op), name);
    fprintf(gen->output, "(");
    generate_expression(gen, op);
    fprintf(gen->output, ")");
    seq_next(gen, s);
    order_bind(op, name);
}

/* A field read evaluated ahead of `call`, which frees it or keeps it
 * (string_field_handoff_kind, `kind`): handed over in its place in the
 * order, where it is read, to the temporary that holds it until the call
 * (emit_string_field_handover_ahead), so a later operand that stores into
 * the field, or replaces its struct, leaves it alone. An exit taken before
 * the call runs frees it (order_handover_add). */
static void order_hoist_handoff(CodeGenerator* gen, TempSeq* s, ASTNode* op, int kind,
                                ASTNode* call) {
    const char* name = cg_internf("_eo%d", g_order_counter++);
    const char* own = emit_string_field_handover_ahead(gen, op, kind, s, name);
    order_bind(op, name);
    order_handover_add(gen, op, call, push_handover_exit_free(gen, own, name), kind);
}

/* How operand `k` of `call`, a field read, is handed to the callee
 * (string_field_handoff_kind), or 0. Only where operands and parameters
 * line up one to one: no named argument or injected builder context. A
 * trailing block is the last child and no operand, so the arguments
 * before it line up. */
static int order_operand_hands_off(CodeGenerator* gen, ASTNode* call, int k) {
    if (!call || call->type != AST_FUNCTION_CALL || !call->value ||
        is_builder_func_reg(gen, call->value)) return 0;
    for (int i = 0; i < call->child_count; i++) {
        ASTNode* a = call->children[i];
        if (!a || a->type == AST_NAMED_ARG) return 0;
    }
    if (k >= call->child_count) return 0;
    ASTNode* a = call->children[k];
    if (a->type == AST_CLOSURE && a->value && strcmp(a->value, "trailing") == 0) return 0;
    int kind = string_field_handoff_kind(gen, call->value, k, a);
    if (!kind && hoist_ready(gen) && field_read_can_hand_off(a) &&
        callee_param_is_string(gen, call->value, k))
        kind = STRING_FIELD_TAKEN;
    return kind;
}

int order_prelude_depth(void) {
    return g_order_count;
}

void order_prelude_end(int depth) {
    while (g_order_count > depth) {
        g_order_count--;
        free(g_order_subs[g_order_count].name);
    }
    while (g_handover_count > 0 && g_handovers[g_handover_count - 1].order_at >= depth) {
        /* A call never written (none takes the binding any more): its
         * exit free goes with it, so no later exit frees what it held. */
        OrderHandover* h = &g_handovers[g_handover_count - 1];
        if (h->carrier) drop_defer(g_handover_gen, h->carrier);
        g_handover_count--;
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
    /* Statements, ahead of the one the list belongs to: `T _eoN = (op);`,
     * T inferred (__auto_type) where the compiler can, and the operand's
     * own type elsewhere (#2765). */
    for (int k = 0; k < n; k++) {
        if (!hoist[k]) continue;
        const char* name = cg_internf("_eo%d", g_order_counter++);
        const char* c11 = order_c11_type(items[k]);
        if (c11) fprintf(gen->output, "_AE_AUTO(%s) %s = (", c11, name);
        else fprintf(gen->output, "__auto_type %s = (", name);
        generate_expression(gen, items[k]);
        fprintf(gen->output, "); ");
        order_bind(items[k], name);
    }
}

/* Emit `expr` with the operands a later one conflicts with evaluated first,
 * in source order, into temps it then reads, an ordered sequence (#2765):
 *     ({ __auto_type _eo0 = (i++); f(_eo0, i); })     GCC-compatible
 *     (_eo0 = (i++), f(_eo0, i))                       any other compiler
 * Returns 0, emitting nothing, when no operand needs it. */
static int emit_in_operand_order(CodeGenerator* gen, ASTNode* expr) {
    ASTNode* ops[ORDER_MAX_OPERANDS];
    int hoist[ORDER_MAX_OPERANDS];
    if (order_bound(expr)) return 0;   /* emitted as the temp that holds it */
    int n = order_operands(expr, ops, ORDER_MAX_OPERANDS);
    if (n < 2 || !order_hoist_set(gen, ops, n, hoist)) return 0;
    int saved = g_order_count;
    if (expr->type == AST_SEND_FIRE_FORGET) {
        /* A fire-and-forget send is written as a block, a statement, which
         * no comma expression can hold (#2765): its operands go ahead of it
         * as declarations in a block of their own, typed as an initializer
         * list's are (order_prelude_begin). */
        fprintf(gen->output, "{ ");
        for (int k = 0; k < n; k++) {
            if (!hoist[k]) continue;
            const char* name = cg_internf("_eo%d", g_order_counter++);
            const char* c11 = order_c11_type(ops[k]);
            if (c11) fprintf(gen->output, "_AE_AUTO(%s) %s = (", c11, name);
            else fprintf(gen->output, "__auto_type %s = (", name);
            generate_expression(gen, ops[k]);
            fprintf(gen->output, "); ");
            order_bind(ops[k], name);
        }
        ASTNode* saved_emitting = g_order_emitting;
        g_order_emitting = expr;
        generate_expression(gen, expr);
        g_order_emitting = saved_emitting;
        fprintf(gen->output, " }");
        order_prelude_end(saved);
        return 1;
    }
    int typed = 1;
    for (int k = 0; k < n; k++)
        if (hoist[k] && !order_operand_hands_off(gen, expr, k) && !order_c11_type(ops[k]))
            typed = 0;
    TempSeq s;
    seq_open_ordered(gen, &s, typed);
    for (int k = 0; k < n; k++) {
        if (!hoist[k]) continue;
        int kind = order_operand_hands_off(gen, expr, k);
        if (kind) order_hoist_handoff(gen, &s, ops[k], kind, expr);
        else order_hoist_operand(gen, &s, ops[k]);
    }
    ASTNode* saved_emitting = g_order_emitting;
    g_order_emitting = expr;
    generate_expression(gen, expr);
    g_order_emitting = saved_emitting;
    seq_close(gen, &s);
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
        /* #2813: formatted with %d, read as the int a vararg promotes them
         * to. Left inline, a byte or a uint16 segment was read after a call
         * in a later segment had changed it. */
        case TYPE_BYTE: case TYPE_UINT8: case TYPE_UINT16: case TYPE_ENUM:
            return "int";
        case TYPE_BITSTRUCT:
            return type_promotes_to_int(t) ? "int" : get_c_type(t);
        case TYPE_BITSET:   return "unsigned long long";
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

/* #2586: open and close the take of a string a call through a typed fn
 * pointer returns (aether_fnptr_take in the prelude). The mark is cleared
 * after the argument temporaries the drain evaluated and right before the
 * call, so only the callee can leave it. */
static int emit_fnptr_take_open(CodeGenerator* gen, ASTNode* call) {
    if (!fnptr_call_returns_string(gen, call)) return 0;
    fprintf(gen->output, "aether_fnptr_take((aether_fnptr_reset(), ");
    return 1;
}

static void emit_fnptr_take_close(CodeGenerator* gen, int took) {
    if (took) fprintf(gen->output, "))");
}

static void generate_fnptr_local_call(CodeGenerator* gen, Type* sig,
                                      const char* local_name, ASTNode* call,
                                      int discarded) {
    const char* ret_c = sig->return_type ? get_c_type(sig->return_type) : "void";
    /* #2586: an owned string argument is freed after the call (R3). */
    ArgDrainWrap ad;
    ad.ret_ct = ret_c;
    ad.ret_type = NULL;
    ad.have_value = strcmp(ret_c, "void") != 0;
    ad.discarded = discarded;
    arg_drain_select(gen, call, 0, NULL, NULL, !ad.have_value || discarded, &ad);
    arg_drain_open(gen, call, &ad);
    int took = emit_fnptr_take_open(gen, call);
    int narrow = !discarded && fnptr_returns_bool(sig);
    if (narrow) fprintf(gen->output, "((_Bool)(unsigned char)(");
    /* #2651: spelled as the declarators spell it (fnptr_c_spelling). */
    fprintf(gen->output, "((%s)(%s))(", fnptr_c_spelling(sig, NULL),
            safe_value_name(local_name));
    generate_fnptr_call_args(gen, sig, call);
    fprintf(gen->output, ")");
    if (narrow) fprintf(gen->output, "))");
    emit_fnptr_take_close(gen, took);
    arg_drain_close(gen, call, &ad);
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

/* #2586: is `call` a call through a typed fn pointer (a local, a
 * parameter, a global or a struct field)? Its arguments are its children
 * from 0 (a field call names its receiver in `value`). Decided from the
 * tree alone, as the analyses that ask (a function's returns and
 * parameters judged from outside its body) run before its locals are
 * registered: the checker stamps a call through a typed fn-pointer local or
 * field. */
int typed_fnptr_call(CodeGenerator* gen, ASTNode* call) {
    if (!call || call->type != AST_FUNCTION_CALL || !call->value) return 0;
    if (call->annotation && (strcmp(call->annotation, "fnptr_local_call") == 0 ||
                             strncmp(call->annotation, "fnfield_", 8) == 0)) return 1;
    Type* sig = lookup_fnptr_global(gen, call->value);
    return sig && sig->kind == TYPE_FUNCTION && sig->is_fnptr;
}

/* #2586: is `call` a call through a typed fn pointer that returns a string?
 * Its result is the caller's: the call takes it (emit_fnptr_take_open), as
 * it is when the callee marked it owned, copied otherwise. The emitters
 * below wrap exactly the calls this says, so the analyses and the C agree. */
int fnptr_call_returns_string(CodeGenerator* gen, ASTNode* call) {
    if (!call || !call->node_type || call->node_type->kind != TYPE_STRING) return 0;
    return typed_fnptr_call(gen, call);
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
    /* A negative literal is the constant folder's (`(0 - 2147483647) - 1`
     * folds to `-2147483648`); the source has no negative literal. C has
     * none either: `-2147483648` is `-` applied to 2147483648, which does
     * not fit an int, so the expression is a long and `printf("%d", ...)`
     * of it is a -Wformat warning; `-9223372036854775808` is `-` applied
     * to a decimal that fits no signed type, which clang warns is taken as
     * unsigned (-Wimplicitly-unsigned-literal) and gcc as __int128 or
     * unsigned. The two minimums are spelled the way <stdint.h> spells
     * them, every other negative value already has its type. */
    if (value[0] == '-') {
        if (strcmp(value, "-2147483648") == 0) return "(-2147483647 - 1)";
        if (strcmp(value, "-9223372036854775808") == 0) return "(-9223372036854775807LL - 1)";
        return value;
    }
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
/* #2769: a `catch` clause's binding and a match arm's `some(v)` binding
 * declare their name as a local does, for the clause or the arm. */
static int binds_scoped_name(ASTNode* node, const char* name) {
    if (!node || !name) return 0;
    if (node->type == AST_CATCH_CLAUSE)
        return node->value && strcmp(node->value, name) == 0;
    return node->type == AST_PATTERN_VARIABLE && node->value && node->annotation &&
           strcmp(node->annotation, "some_pattern") == 0 && strcmp(node->value, name) == 0;
}

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
    // A binding of its own: the closure's local.
    if (binds_scoped_name(node, name)) return FL_FRESH;
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
Type* message_field_type(ASTNode* program, const char* msg, const char* field) {
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
    /* #2633: a receive arm's binding of a typed fn-pointer message field,
     * typed by the message definition (the handler registers it the same
     * way, codegen_actor.c). */
    if (n->type == AST_MESSAGE_PATTERN && n->value) {
        for (int k = 0; k < n->child_count; k++) {
            ASTNode* pf = n->children[k];
            if (!pf || pf->type != AST_PATTERN_FIELD || !pf->value) continue;
            const char* bound = pf->value;
            if (pf->child_count > 0 && pf->children[0] &&
                pf->children[0]->type == AST_PATTERN_VARIABLE && pf->children[0]->value)
                bound = pf->children[0]->value;
            Type* fsig = message_field_type(gen->program, n->value, pf->value);
            if (is_fnptr_type(fsig)) register_fnptr_local(gen, bound, fsig);
        }
    }
    for (int i = 0; i < n->child_count; i++) register_fnptr_decls_in(gen, n->children[i]);
}

static ASTNode* find_scope_node_by_name(ASTNode* program, const char* scope) {
    if (!program || !scope) return NULL;
    if (strncmp(scope, "__closure_", 10) == 0) return find_closure_by_name(program, scope);
    if (strncmp(scope, "__recv_arm_", 11) == 0) return find_receive_arm_by_name(program, scope);
    if (strncmp(scope, "__clause_", 9) == 0) return find_clause_by_scope_name(program, scope);
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

static const char* find_enclosing_scope_name_in(ASTNode* program, ASTNode* node,
                                                const char* scope, ASTNode* scope_fn,
                                                ASTNode* target);

/* #2755: the scope that declares the promoted variable `name` seen from
 * `scope` (a scope name, as parent_func spells one): a closure that
 * captures the name reaches its parent's cell, so the walk climbs through
 * each such closure to the closure, function, `main` or receive arm whose
 * own variable it is, and *body is set to that scope's body. NULL when the
 * scope is not found. */
ASTNode* promoted_cell_declaring_scope(CodeGenerator* gen, const char* scope, const char* name,
                                       ASTNode** body) {
    *body = NULL;
    if (!gen || !gen->program || !name) return NULL;
    for (int depth = 0; scope && depth < 64; depth++) {
        if (strncmp(scope, "__closure_", 10) != 0) {
            ASTNode* node = find_scope_node_by_name(gen->program, scope);
            *body = is_receive_arm_scope(node) ? receive_arm_body(node) : last_block_child(node);
            return node;
        }
        int ci = 0;
        char nm[64];
        for (; ci < gen->closure_count; ci++) {
            closure_scope_name(gen->closures[ci].closure_node, nm, sizeof(nm));
            if (strcmp(nm, scope) == 0) break;
        }
        if (ci == gen->closure_count) return NULL;
        int captured = 0;
        for (int k = 0; k < gen->closures[ci].capture_count; k++) {
            if (gen->closures[ci].captures[k] &&
                strcmp(gen->closures[ci].captures[k], name) == 0) { captured = 1; break; }
        }
        if (!captured) {
            *body = last_block_child(gen->closures[ci].closure_node);
            return gen->closures[ci].closure_node;
        }
        scope = gen->closures[ci].parent_func;
    }
    return NULL;
}

/* `program` is the root the walk starts from. */
static const char* find_enclosing_scope_name(ASTNode* program, const char* scope,
                                             ASTNode* target) {
    return find_enclosing_scope_name_in(program, program, scope, NULL, target);
}

/* `scope_fn`, when set, is the function definition whose scope the walk is
 * in: its name is made (fn_scope_name, a clause's its own, #2644) only for
 * the scope the target turns out to be in. */
static const char* find_enclosing_scope_name_in(ASTNode* program, ASTNode* node,
                                                const char* scope, ASTNode* scope_fn,
                                                ASTNode* target) {
    if (!node) return NULL;
    char here[64];   /* `__recv_arm_<ptr>` / `__closure_<ptr>`: bounded */
    const char* child_scope = scope;
    ASTNode* child_fn = scope_fn;
    if (node->type == AST_FUNCTION_DEFINITION || node->type == AST_BUILDER_FUNCTION) {
        if (node->value) child_fn = node;
    } else if (node->type == AST_MAIN_FUNCTION) {
        child_scope = "main";
        child_fn = NULL;
    } else if (is_receive_arm_scope(node)) {
        snprintf(here, sizeof(here), "__recv_arm_%p", (void*)node);
        child_scope = here;
        child_fn = NULL;
    } else if (is_hoisted_closure(node)) {
        closure_scope_name(node, here, sizeof(here));
        child_scope = here;
        child_fn = NULL;
    }
    for (int i = 0; i < node->child_count; i++) {
        ASTNode* c = node->children[i];
        if (c == target) {
            if (child_fn) return fn_scope_name(program, child_fn);
            /* Interned: `here` dies with this frame. */
            return child_scope ? cg_intern(child_scope) : NULL;
        }
        const char* found = find_enclosing_scope_name_in(program, c, child_scope, child_fn, target);
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

/* Does statement `s` declare `var_name`: `name = v`, a constant, or a slot
 * of a tuple destructure `a, name = f()` (#2762)? The destructure's slots
 * are its children before the value; missed, a closure's own `a, b = g()`
 * found no binding of `a` before it and declared fresh locals, so the
 * outer `a` and `b` a destructure had bound never changed. */
static int declares_name(ASTNode* s, const char* var_name) {
    if (!s) return 0;
    if (s->type == AST_TUPLE_DESTRUCTURE) {
        for (int i = 0; i + 1 < s->child_count; i++) {
            if (declares_name(s->children[i], var_name)) return 1;
        }
        return 0;
    }
    return (s->type == AST_VARIABLE_DECLARATION || s->type == AST_CONST_DECLARATION) &&
           s->value && strcmp(s->value, var_name) == 0;
}

static int visible_decl_line_on_path(ASTNode* s, const char* var_name, ASTNode* viewer);

typedef struct { const char* name; ASTNode* decl; } HoistNameHunt;

static void hoist_name_hunt(ASTNode* decl, void* user) {
    HoistNameHunt* h = (HoistNameHunt*)user;
    if (!h->decl && decl && decl->value && strcmp(decl->value, h->name) == 0) h->decl = decl;
}

/* #2901: the binding of `var_name` that statement `s` declares in the
 * block it stands in, because codegen hoists the name out of `s`
 * (analysis/hoist.h): every arm of an if/else, a switch with a default or
 * a covering match binds it (hoist_common_decl_names), or a while body
 * binds it (hoist_loop_decls). NULL for none. The C variable is then the
 * block's, so a closure after that binding that writes the name writes
 * it, as it does a name the block binds itself. */
static ASTNode* stmt_hoisted_decl(ASTNode* s, const char* var_name) {
    if (!s || !var_name) return NULL;
    if (s->type == AST_WHILE_LOOP) {
        HoistNameHunt h = { var_name, NULL };
        if (s->child_count > 1) hoist_loop_decls(s->children[1], hoist_name_hunt, &h);
        return h.decl;
    }
    const char* names[HOIST_MAX_NAMES];
    int n = hoist_common_decl_names(s, names, HOIST_MAX_NAMES);
    for (int i = 0; i < n; i++)
        if (names[i] && strcmp(names[i], var_name) == 0)
            return hoist_common_first_decl(s, var_name);
    return NULL;
}

// The line of the first declaration of `var_name` that is VISIBLE from
// `viewer` (a closure node) in `block`, or INT_MAX if there is none: one of
// its statements, or one in a block on the way from it down to `viewer` (a
// `while`, `for` or `if` body, a `match` arm, a trailing block), which is the
// closure's own enclosing scope. A block that does not hold `viewer` is a
// sibling: a declaration in it, in another loop or branch body or in a
// sibling trailing block (`grid() { ... }`, which inlines at its call site as
// a C `{ ... }` block), shares the name by coincidence and is skipped. #2189:
// treating every trailing block as transparent compiled a closure's own
// `ml = ...` as a capture of the `ml` a sibling `describe` block had
// declared, and the generated C referenced a name that had gone out of
// scope. #2659: the blocks holding `viewer` were skipped as well, except a
// trailing one, so `c = 0; f = || { c = 5 }` in a loop body or a branch gave
// the closure a fresh `c` of its own and the loop's `c` never changed (a
// closure that also read `c` captured it, through the read path).
static int visible_decl_line(ASTNode* block, const char* var_name, ASTNode* viewer) {
    if (!block) return INT_MAX;
    for (int k = 0; k < block->child_count; k++) {
        ASTNode* s = block->children[k];
        if (!s) continue;
        if (declares_name(s, var_name)) return s->line;
        /* #2901: a name hoisted out of a statement that does not hold the
         * closure is the block's too, from its binding on. */
        if (s != viewer && !subtree_contains(s, viewer)) {
            ASTNode* hd = stmt_hoisted_decl(s, var_name);
            if (hd) return hd->line;
        }
        if (s != viewer && !is_hoisted_closure(s) && subtree_contains(s, viewer)) {
            int inner = visible_decl_line_on_path(s, var_name, viewer);
            if (inner != INT_MAX) return inner;
        }
    }
    return INT_MAX;
}

// The part of statement `s` (which holds `viewer`) on the way down to it: the
// block holding `viewer`, scanned as a scope, or a declaration that comes
// before it among `s`'s own parts (a C-style `for`'s initialiser). Another
// hoisted closure is a scope of its own, reached through its scope name.
static int visible_decl_line_on_path(ASTNode* s, const char* var_name, ASTNode* viewer) {
    for (int i = 0; i < s->child_count; i++) {
        ASTNode* c = s->children[i];
        if (!c || c == viewer || is_hoisted_closure(c)) continue;
        if (c->type == AST_BLOCK) {
            if (subtree_contains(c, viewer)) return visible_decl_line(c, var_name, viewer);
            continue;
        }
        if (declares_name(c, var_name)) return c->line;
        if (subtree_contains(c, viewer)) return visible_decl_line_on_path(c, var_name, viewer);
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
    /* A clause of a set is a scope of its own (#2644). */
    ASTNode* clause = find_clause_by_scope_name(program, func_name);
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* top = program->children[i];
        if (!top) continue;
        int matches = clause ? top == clause :
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
                int line = visible_decl_line(body, var_name, viewer);
                if (line != INT_MAX) return line;
                /* #2901: a name first bound in one arm of a top-level if,
                 * switch or match and read after it is declared at the top
                 * of the function (hoist_if_branch_vars): visible from its
                 * first binding on. */
                const char* names[HOIST_MAX_NAMES];
                int n = hoist_if_branch_candidates(body, names, HOIST_MAX_NAMES);
                for (int c = 0; c < n; c++) {
                    if (!names[c] || strcmp(names[c], var_name) != 0) continue;
                    ASTNode* first = hoist_if_branch_first_decl(body, var_name);
                    if (first && !subtree_contains(first, viewer)) return first->line;
                }
                return INT_MAX;
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
    /* A closure in a `catch` handler or a `some(v)` arm captures the
     * binding as it captures any local (#2769): it was missed, so the
     * closure's C body read an undeclared name. */
    if (binds_scoped_name(node, var_name)) return 1;
    for (int i = 0; i < node->child_count; i++) {
        if (subtree_declares(node->children[i], var_name)) return 1;
    }
    return 0;
}

// Does the named function define or declare `var_name` (as a parameter or
// local). Used to distinguish captures (names from an enclosing scope) from
// fresh locals that happen to share a name. Scans nested blocks (for loops,
// if/else bodies) but does not descend into inner closures.
static ASTNode* find_enclosing_actor(ASTNode* root, ASTNode* arm_node);

/* #2890: the `state` declaration of `name` in the actor whose receive arm
 * is `arm`, or NULL. A closure made in the arm reads it as a capture: the
 * env takes the state's value when the closure is made (capture_source
 * spells it `self->name` there), as it takes a local's. Writing it is
 * refused (validate_closure_state_mutations, L4). */
static ASTNode* receive_arm_state_decl(ASTNode* program, ASTNode* arm, const char* name) {
    ASTNode* actor = find_enclosing_actor(program, arm);
    for (int i = 0; actor && name && i < actor->child_count; i++) {
        ASTNode* c = actor->children[i];
        if (c && c->type == AST_STATE_DECLARATION && c->value && strcmp(c->value, name) == 0)
            return c;
    }
    return NULL;
}

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
        /* #2890: so is the actor's state: a closure that read it captured
         * nothing, and its body read an undeclared name. */
        if (receive_arm_state_decl(program, arm, var_name)) return 1;
        ASTNode* body = receive_arm_body(arm);
        if (!body) return 0;
        return subtree_declares(body, var_name);
    }
    /* A clause of a set is a scope of its own (#2644). */
    ASTNode* clause = find_clause_by_scope_name(program, func_name);
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* top = program->children[i];
        if (!top) continue;
        int matches = 0;
        if (clause) {
            matches = top == clause;
        } else if (strcmp(func_name, "main") == 0 && top->type == AST_MAIN_FUNCTION) {
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
// carrying a non-print value. Does not descend into inner closures; a
// trailing block's returns are the walked function's (#2766), as in every
// walk here over a function's returns.
static ASTNode* find_first_return_expr(ASTNode* node);

/* #2501: the first return statement under `node` that carries a value, not
 * looking into nested closures (find_first_return_expr's walk). */
static ASTNode* find_first_value_return(ASTNode* node) {
    if (!node || ast_is_own_function(node)) return NULL;
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

/* The closure every `return` under `node` gives, as a variable of `scope`
 * bound to one literal (closure_var_id): its id; -1 when two returns give
 * different ones or one gives anything else; `seen` (-2 for none yet) when
 * there is no return. Nested functions and closures are not walked. */
static int returns_one_closure(CodeGenerator* gen, ASTNode* node, const char* scope, int seen) {
    if (!node || seen == -1 || ast_is_own_function(node)) return seen;
    if (node->type == AST_RETURN_STATEMENT) {
        ASTNode* v = node->child_count == 1 ? node->children[0] : NULL;
        int id = !v ? -1
                 : v->type == AST_CLOSURE && v->value ? atoi(v->value)
                 : v->type == AST_IDENTIFIER && v->value ? closure_var_id(gen, scope, v->value)
                 : -1;
        return (id < 0 || (seen >= 0 && seen != id)) ? -1 : id;
    }
    for (int i = 0; i < node->child_count; i++) seen = returns_one_closure(gen, node->children[i], scope, seen);
    return seen;
}

static ASTNode* find_first_return_expr(ASTNode* node) {
    if (!node) return NULL;
    if (ast_is_own_function(node)) return NULL;
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
    if (ast_is_own_function(node)) return 0;
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

/* #2820: the declared type of field `field` of the struct `sname`, or
 * NULL. */
static Type* struct_literal_field_type(CodeGenerator* gen, const char* sname, const char* field) {
    ASTNode* sdef = (gen && gen->program && sname)
                        ? find_struct_definition_by_name(gen->program, sname) : NULL;
    for (int i = 0; sdef && field && i < sdef->child_count; i++) {
        ASTNode* f = sdef->children[i];
        if (f && f->type == AST_STRUCT_FIELD && f->value && strcmp(f->value, field) == 0)
            return f->node_type;
    }
    return NULL;
}

/* #2805: is `name`, used by the closure `viewer` in scope `scope`, a
 * module-level `var`, with no parameter of the scope chain spelled the same
 * (enclosing_decl_line says 0 for one)? Every `name = ...` in a function
 * writes the global (#701), so the enclosing function's bindings of the
 * name are the global's, and the closure reads and writes the global
 * itself. Taken for a capture, the name became a cell, or an env copy,
 * that shadowed the global for the whole enclosing function: neither the
 * closure's stores nor the function's own reached it. */
static int closure_name_is_module_global(CodeGenerator* gen, const char* scope,
                                         const char* name, ASTNode* viewer) {
    if (!is_module_global_var(gen, name)) return 0;
    return enclosing_decl_line(gen->program, scope, name, viewer) != 0;
}

// Internal recursive worker that tracks the enclosing function name.
static void discover_closures_scoped(CodeGenerator* gen, ASTNode* node, const char* enclosing_func) {
    if (!node) return;
    // Entering a function body switches the enclosing function for descendants.
    if (node->type == AST_FUNCTION_DEFINITION || node->type == AST_BUILDER_FUNCTION) {
        /* A clause of a set is a scope of its own (#2644). */
        const char* new_enc = node->value ? fn_scope_name(gen->program, node) : enclosing_func;
        /* #2805: the function's module decides which globals its closures
         * reach (is_module_global_var): in a function merged from a module,
         * a name the program also declares as a `var` is the function's
         * own local, captured as any other. */
        const char* saved_origin = gen->current_origin_module;
        gen->current_origin_module = node->origin_module;
        for (int i = 0; i < node->child_count; i++) {
            discover_closures_scoped(gen, node->children[i], new_enc);
        }
        gen->current_origin_module = saved_origin;
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
    /* docs/quote.md: a quote's captures are cells of the scope it is
     * written in; quote_promote_captures promotes them once discovery is
     * done. Its body is the tree's, and the node has no children left. */
    if (node->type == AST_QUOTE) {
        QuoteInfo* q = quote_of(node);
        if (q) q->scope = enclosing_func ? aether_intern(enclosing_func) : NULL;
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
                    is_cap = is_declared_in_function(gen->program, enclosing_func, all_ids[i]) &&
                             !closure_name_is_module_global(gen, enclosing_func, all_ids[i], node);
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
                if (decl_line <= node->line &&
                    !closure_name_is_module_global(gen, enclosing_func, writes[i], node)) {
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
            /* A call to a function every `return` of which gives the same
             * closure variable binds to that closure: `w = build_pair()`
             * where build_pair ends in `return wrapped`. One that returns
             * another closure on some path has no single one. */
            ASTNode* target_fn = find_function_definition_by_name(gen->program, rhs->value);
            for (int i = 0; target_fn && i < target_fn->child_count; i++) {
                ASTNode* body = target_fn->children[i];
                if (!body || body->type != AST_BLOCK) continue;
                cid_to_bind = returns_one_closure(gen, body,
                                                  fn_scope_name(gen->program, target_fn), -2);
                if (cid_to_bind == -2) cid_to_bind = -1;
                break;
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
    /* #2903: a slot of a tuple destructure binds its variable too, to the
     * closure at its position of the tuple, which this walk cannot name
     * (`f, n = pair(5)`). The slot has no initializer of its own, so the
     * declaration case above left it out, the variable kept the literal it
     * was bound to before, and call(f) ran that one. */
    if (node->type == AST_TUPLE_DESTRUCTURE && node->child_count >= 2) {
        ASTNode* rhs = node->children[node->child_count - 1];
        Type* tt = rhs ? rhs->node_type : NULL;
        for (int i = 0; i + 1 < node->child_count; i++) {
            ASTNode* slot = node->children[i];
            if (!slot || !slot->value || strcmp(slot->value, "_") == 0) continue;
            Type* st = slot->node_type;
            if ((!st || st->kind == TYPE_UNKNOWN) && tt && tt->kind == TYPE_TUPLE &&
                i < tt->tuple_count)
                st = tt->tuple_types[i];
            if (st && st->kind == TYPE_FUNCTION && !st->is_fnptr)
                closure_var_bind(gen, enclosing_func, slot->value, -1);
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
                                           node->value ? fn_scope_name(gen->program, node) : scope);
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
     * builder got typed as the closure struct instead of int. A call's
     * trailing block runs inline: its returns are the function's, so it
     * keeps the function's type (#2766). */
    if (node->type == AST_CLOSURE) {
        char own[64];
        const char* inner = scope;
        if (is_hoisted_closure(node)) {
            closure_scope_name(node, own, sizeof(own));
            inner = own;
        }
        Type* ret = ast_is_own_function(node) ? NULL : fn_ret;
        for (int i = 0; i < node->child_count; i++) {
            propagate_call_return_types_in(gen, node->children[i], ret, inner);
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

/* docs/quote.md: every variable a quote captures lives in a shared cell for
 * as long as the tree does, so the tree reads the variable itself, not a
 * copy, and sees the program change it. A closure promotes a capture only
 * when it writes it; a quote promotes every one. A quote inside a hoisted
 * closure needs the variable in that closure's env too, which the nested-
 * capture pass then carries further out. */
static void quote_add_closure_captures(CodeGenerator* gen) {
    for (int qi = 0; qi < quote_count(); qi++) {
        QuoteInfo* q = quote_at(qi);
        if (!q || !q->scope || strncmp(q->scope, "__closure_", 10) != 0) continue;
        for (int ci = 0; ci < gen->closure_count; ci++) {
            char nm[64];
            closure_scope_name(gen->closures[ci].closure_node, nm, sizeof(nm));
            if (strcmp(nm, q->scope) != 0) continue;
            ASTNode* cnode = gen->closures[ci].closure_node;
            for (int k = 0; k < q->ncaps; k++) {
                const char* cap = q->caps[k].name;
                if (q->caps[k].global) continue;
                if (is_closure_param(cnode, cap)) continue;
                if (subtree_declares(last_block_child(cnode), cap)) continue;
                add_capture(gen, ci, cap);
            }
        }
    }
}

static void quote_promote_captures(CodeGenerator* gen) {
    for (int qi = 0; qi < quote_count(); qi++) {
        QuoteInfo* q = quote_at(qi);
        if (!q || !q->scope) continue;
        for (int k = 0; k < q->ncaps; k++) {
            if (q->caps[k].global) continue;   /* a global is its own cell */
            promote_up_from(gen, q->scope, q->caps[k].name);
        }
    }
}

// Public entry point — starts at program root with no enclosing function.
void discover_closures(CodeGenerator* gen, ASTNode* node) {
    discover_closures_scoped(gen, node, NULL);
    // Second pass: now that closure_var_map is fully populated, propagate
    // return types back onto call() expressions the typechecker left as int.
    propagate_call_return_types(gen, node);
    quote_add_closure_captures(gen);
    // Third pass: carry captures of nested closures out to their enclosing
    // closure, whose C frame the inner env is built from.
    propagate_nested_captures(gen);
    quote_promote_captures(gen);
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

/* #2890: why a closure made in a receive arm cannot read the state field
 * `sd` declares, or NULL when it can. The closure copies the field when it
 * is made (capture_source), and the copy must hold everything it reads,
 * since the next store into the state releases what the field held:
 *   - a scalar, an enum or a bitset is its value;
 *   - a `ptr` or an actor reference is a handle no slot owns, copied as a
 *     local's is;
 *   - a string is retained (aether_str_capture), a closure too (#2494);
 *   - a struct is copied, by `_aether_copy_<Name>` when it owns strings, closures
 *     or arrays (#2504), so the copy holds its own;
 *   - a tuple, an optional and a fixed-size array are copied with a
 *     reference taken to each part they own (capture_take, #2921).
 * A sum or a slice is not judged here and is refused. */
static const char* arm_closure_state_read_refusal(CodeGenerator* gen, ASTNode* closure,
                                                  ASTNode* sd) {
    Type* t = sd ? sd->node_type : NULL;
    if (!t) return "its type is not known here";
    switch (t->kind) {
        case TYPE_INT: case TYPE_INT64: case TYPE_UINT64: case TYPE_UINT32:
        case TYPE_UINT16: case TYPE_UINT8: case TYPE_DURATION: case TYPE_FLOAT:
        case TYPE_LONGDOUBLE: case TYPE_FLOAT32: case TYPE_BOOL: case TYPE_BYTE:
        case TYPE_ENUM: case TYPE_BITSET: case TYPE_BITSTRUCT:
        case TYPE_PTR: case TYPE_ACTOR_REF: case TYPE_STRING: case TYPE_FUNCTION:
        case TYPE_STRUCT: case TYPE_TUPLE: case TYPE_OPTIONAL:
            return NULL;
        case TYPE_ARRAY: {
            Type* e = t->element_type;
            if (t->array_size <= 0 || !e) return "a slice is not judged here";
            switch (e->kind) {
                case TYPE_INT: case TYPE_INT64: case TYPE_UINT64: case TYPE_UINT32:
                case TYPE_UINT16: case TYPE_UINT8: case TYPE_DURATION: case TYPE_FLOAT:
                case TYPE_LONGDOUBLE: case TYPE_FLOAT32: case TYPE_BOOL: case TYPE_BYTE:
                case TYPE_ENUM: case TYPE_BITSET: case TYPE_BITSTRUCT:
                case TYPE_PTR: case TYPE_ACTOR_REF:
                    return NULL;
                case TYPE_STRING: case TYPE_FUNCTION: case TYPE_STRUCT:
                case TYPE_TUPLE: case TYPE_OPTIONAL:
                    /* The copy takes a reference to each element (#2921). */
                    return NULL;
                default:
                    return "an array of these is not judged here";
            }
        }
        default:
            return "a value of this kind is not judged here";
    }
}

// L4 validation: a closure inside an actor handler that writes to an
// actor state field is currently miscompiled (the closure has no access
// to `self`, so `state_field = ...` emits a stale-local write). Until
// threading self through the closure env is implemented, reject the
// pattern at compile time with a clear error. Returns 0 on failure
// (errors were reported via aether_error_report), 1 otherwise.
// #2890: a closure that reads a state field it cannot copy safely
// (arm_closure_state_read_refusal) is refused the same way.
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
        ASTNode* state_decls[64];
        int state_count = 0;
        for (int j = 0; j < actor->child_count && state_count < 64; j++) {
            ASTNode* c = actor->children[j];
            if (!c || !c->value) continue;
            if (c->type == AST_STATE_DECLARATION ||
                (c->type == AST_VARIABLE_DECLARATION && c->annotation &&
                 strcmp(c->annotation, "state") == 0)) {
                state_decls[state_count] = c;
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
                !is_written_through(body, state_names[n])) {
                /* #2890: a field the closure only reads is copied into it;
                 * a copy that would not hold what it reads is refused. */
                int captured = 0;
                for (int k = 0; k < gen->closures[ci].capture_count && !captured; k++)
                    captured = gen->closures[ci].captures[k] &&
                               strcmp(gen->closures[ci].captures[k], state_names[n]) == 0;
                const char* why = captured
                    ? arm_closure_state_read_refusal(gen, closure, state_decls[n]) : NULL;
                if (!why) continue;
                char rmsg[512];
                snprintf(rmsg, sizeof(rmsg),
                    "closure inside actor '%s' handler reads state field '%s' (%s): "
                    "not supported, the closure copies the field when it is made and %s, "
                    "so a later store into the state would free what it reads",
                    actor->value ? actor->value : "actor", state_names[n],
                    state_decls[n]->node_type ? type_to_string(state_decls[n]->node_type)
                                              : "unknown",
                    why);
                char rsug[256];
                snprintf(rsug, sizeof(rsug),
                    "read what the closure needs out of '%s' into arm locals first "
                    "(an element `f = %s[0]`, a field `name = %s[0].name`) and use those",
                    state_names[n], state_names[n], state_names[n]);
                aether_error_full(rmsg, closure->line, closure->column, rsug,
                                  "in actor handler", AETHER_ERR_ACTOR_ERROR);
                ok = 0;
                continue;
            }
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
    /* #2769: a `catch` binding is the panic's reason, a string; a `some(v)`
     * binding is the matched optional's value. */
    if (node->type == AST_CATCH_CLAUSE && binds_scoped_name(node, var_name)) {
        static Type catch_reason_type = { .kind = TYPE_STRING };
        return &catch_reason_type;
    }
    if (node->type == AST_MATCH_STATEMENT && node->child_count > 0 && node->children[0] &&
        node->children[0]->node_type && node->children[0]->node_type->kind == TYPE_OPTIONAL &&
        node->children[0]->node_type->element_type) {
        for (int i = 1; i < node->child_count; i++) {
            ASTNode* arm = node->children[i];
            if (arm && arm->type == AST_MATCH_ARM && arm->child_count > 0 &&
                binds_scoped_name(arm->children[0], var_name))
                return node->children[0]->node_type->element_type;
        }
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
        /* #2890: a state field has its declared type; a store into it in
         * the arm is no local of another one. */
        ASTNode* sd = receive_arm_state_decl(gen->program, arm, var_name);
        if (sd && sd->node_type && sd->node_type->kind != TYPE_UNKNOWN) return sd->node_type;
        return decl_type_in_scope(receive_arm_body(arm), var_name);
    }
    // Parent-function-first lookup, through the program index (#2007).
    // A clause of a set is its own scope (#2644): two clauses may bind
    // one name to values of different types.
    if (parent_func) {
        ASTNode* top = find_clause_by_scope_name(gen->program, parent_func);
        if (!top && strcmp(parent_func, "main") == 0) {
            ProgramIndex* ix = program_index(gen->program);
            top = ix ? ix->main_fn : NULL;
        } else if (!top) {
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

/* #2797: the type the cell of promoted variable `name` was made with, seen
 * from the scope being emitted: a closure that captures the name reaches
 * its parent's cell, typed as the closure's prologue types it
 * (lookup_var_type in the parent scope), and the scope that declares the
 * name types it by its own first binding. A store into the cell must take
 * the cell's type: the value's may be another one C converts to it (a
 * string into a `ptr`), and its own binding in a closure's body says the
 * value's. NULL when unknown. */
Type* promoted_cell_type(CodeGenerator* gen, const char* name) {
    const char* scope = gen ? gen->closure_var_scope : NULL;
    if (!scope || !name) return NULL;
    for (int depth = 0; scope && depth < 64 && strncmp(scope, "__closure_", 10) == 0; depth++) {
        int ci = 0, captured = 0;
        char nm[64];
        for (; ci < gen->closure_count; ci++) {
            closure_scope_name(gen->closures[ci].closure_node, nm, sizeof(nm));
            if (strcmp(nm, scope) == 0) break;
        }
        if (ci == gen->closure_count) return NULL;
        for (int k = 0; k < gen->closures[ci].capture_count && !captured; k++) {
            captured = gen->closures[ci].captures[k] &&
                       strcmp(gen->closures[ci].captures[k], name) == 0;
        }
        if (!captured) break;
        scope = gen->closures[ci].parent_func;
    }
    return scope ? lookup_var_type(gen, name, scope) : NULL;
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

/* #2464: a closure's copy of the array `name` it captures, element by
 * element. An array a `try` body writes is volatile (#2618), which memcpy's
 * plain source pointer would not accept (#2691); each element is read as
 * the array's type says it must be. `src` spells the array where the
 * closure is made (capture_source). */
static void emit_capture_array_copy(CodeGenerator* gen, const char* name, const char* src,
                                    Type* t) {
    fprintf(gen->output, "for (int _ae_k = 0; _ae_k < %d; _ae_k++) _e->%s[_ae_k] = %s[_ae_k];",
            t->array_size, name, src);
}

/* #2890: the capture `name` as the code that makes the closure reads it: a
 * state field of the actor whose handler makes it is `self->name` there,
 * as the arm's own reads spell it; anything else is its name. */
static const char* capture_source(CodeGenerator* gen, const char* name) {
    if (!is_actor_state_var(gen, name)) return name;
    return cg_internf("%s->%s", gen->state_self_alias ? gen->state_self_alias : "self", name);
}

static int capture_is_promoted(CodeGenerator* gen, const char* name, const char* parent_func);

/* #2921: a capture of an owning tuple, optional, string sequence or
 * fixed-size array (not a shared cell): its type, or NULL. The environment's copy takes a reference
 * to each part (capture_take) and drops them with itself. */
static Type* capture_composite_type(CodeGenerator* gen, const char* name, const char* parent_func) {
    if (capture_is_promoted(gen, name, parent_func)) return NULL;
    Type* t = lookup_var_type(gen, name, parent_func);
    if (!t || !own_type_owns(gen, t)) return NULL;
    if (t->kind == TYPE_TUPLE || t->kind == TYPE_OPTIONAL || t->kind == TYPE_PTR) return t;
    return type_is_sized_array(t) && t->array_size > 0 ? t : NULL;
}

/* The retain of the environment's copy of `name`, of composite type `t`. */
static void capture_take(CodeGenerator* gen, const char* name, Type* t) {
    if (t->kind == TYPE_PTR)
        fprintf(gen->output, " _e->%s = string_seq_retain(_e->%s);", name, name);
    else if (t->kind == TYPE_ARRAY)
        fprintf(gen->output, " %s((%s*)_e->%s);", own_fn(gen, t, "retain"),
                get_c_type(t->element_type), name);
    else
        fprintf(gen->output, " %s(&_e->%s);", own_fn(gen, t, "retain"), name);
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
 * with strings of its own (`_aether_copy_<Name>`) and destroy it in its destructor.
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
/* `definition`: the parameters are the body's, and one its `try` writes is
 * volatile (#2690), as a function's is; the prototype leaves them plain,
 * which C counts as the same type. */
static void emit_closure_signature(CodeGenerator* gen, int ci, const char* ret_type,
                                   int definition) {
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
                const char* vq = definition ? try_volatile_qual_for(gen, p->value) : "";
                fprintf(gen->output, ", %s %s", volatile_decl_type(vq, ptype),
                        safe_value_name(p->value));
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
       the last one tears the env down. The teardown runs through
       aether_teardown, which queues it when another env's teardown reaches
       it: a chain of closures each holding the one before is torn down in
       constant stack. */
    fprintf(gen->output, "static void _closure_env_%d_teardown(void* _p);\n", id);
    fprintf(gen->output, "static void _closure_env_%d_free(void* _p) {\n", id);
    fprintf(gen->output, "    if (!_p) return;\n");
    fprintf(gen->output, "    _closure_env_%d* _e = (_closure_env_%d*)_p;\n", id, id);
    fprintf(gen->output, "    if (atomic_fetch_sub_explicit(&_e->_refs, 1, memory_order_acq_rel) != 1) return;\n");
    fprintf(gen->output, "    _e->_dtor = _closure_env_%d_teardown;\n", id);
    fprintf(gen->output, "    aether_teardown(_p);\n");
    fprintf(gen->output, "}\n\n");
    fprintf(gen->output, "static void _closure_env_%d_teardown(void* _p) {\n", id);
    fprintf(gen->output, "    _closure_env_%d* _e = (_closure_env_%d*)_p;\n", id, id);
    {
        for (int i = 0; i < cap_count; i++) {
            int promoted = capture_is_promoted(gen, captures[i], parent_func);
            if (capture_is_closure_value(gen, captures[i], parent_func)) {
                fprintf(gen->output, "    _aether_closure_env_release(_e->%s.env);\n", captures[i]);
                continue;
            }
            /* #2921: the env's copy of a tuple, an optional or an array
             * holds a reference to each part (capture_take). */
            Type* ct = capture_composite_type(gen, captures[i], parent_func);
            if (ct) {
                own_emit_part(gen, ct, cg_internf("_e->%s", captures[i]), OWN_DROP);
                continue;
            }
            const char* owning = capture_owning_struct(gen, captures[i], parent_func);
            if (owning) {
                fprintf(gen->output, "    _aether_drop_%s(&_e->%s);\n", owning, captures[i]);
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
/* The parameter list of `_aether_make_env_<id>`, the non-GCC constructor
 * of closure `ci`'s environment (one argument per capture): shared by
 * its prototype (emit_closure_declarations) and its definition
 * (emit_closure_definitions), which must agree. A promoted capture arrives
 * as the cell pointer, `ctype*`, which is also what the env field is. A
 * fixed-size array arrives as a pointer to its elements (#2464), read only
 * and volatile: an array a `try` body writes has volatile elements, which a
 * plain pointer would not accept (#2691), and a plain array converts to it
 * as well. */
static void emit_make_closure_params(CodeGenerator* gen, int ci) {
    char** captures = gen->closures[ci].captures;
    int cap_count = gen->closures[ci].capture_count;
    const char* parent_func = gen->closures[ci].parent_func;
    for (int i = 0; i < cap_count; i++) {
        if (i > 0) fprintf(gen->output, ", ");
        const char* ctype = lookup_var_c_type(gen, captures[i], parent_func);
        Type* arr = capture_sized_array_type(gen, captures[i], parent_func);
        if (arr && !capture_is_promoted(gen, captures[i], parent_func)) {
            fprintf(gen->output, "%s const volatile* %s", get_c_type(arr->element_type),
                    captures[i]);
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
        emit_closure_signature(gen, ci, ret_type, 0);
        fprintf(gen->output, ";\n");
        /* #2528: the non-GCC constructor is defined with the bodies, after
         * the functions that build the closure; without this prototype a
         * capturing closure made in a function did not compile on that
         * path (an implicit declaration, then conflicting types). */
        if (gen->closures[ci].capture_count > 0) {
            fprintf(gen->output, "#if !AETHER_GCC_COMPAT\nstatic void* _aether_make_env_%d(",
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

        // Find body first so we can detect which captures are mutated.
        ASTNode* body = NULL;
        for (int i = closure->child_count - 1; i >= 0; i--) {
            if (closure->children[i] && closure->children[i]->type == AST_BLOCK) {
                body = closure->children[i];
                break;
            }
        }

        /* #2690: the closure is a C function of its own, and so is the set
         * of names its `try` bodies write: marked before its parameters are
         * declared, and nobody else's (the last function's, or a sibling
         * closure's, made unrelated locals volatile). */
        TryClobberedSet outer_try_set;
        save_try_clobbered_vars(gen, &outer_try_set);
        mark_try_clobbered_vars(gen, body);

        const char* ret_type = resolve_closure_return_type(gen, ci);
        restore_fnptr_scope_for_closure(gen, parent_func);
        emit_closure_signature(gen, ci, ret_type, 1);
        fprintf(gen->output, " {\n");
        gen->in_string_closure = strcmp(ret_type, "const char*") == 0;
        /* #2501: a multi-value return builds the closure's tuple. */
        Type* closure_ret_tuple = closure_tuple_return_type(closure);
        gen->current_func_return_type = closure_ret_tuple;

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
        const char** alias_caps = NULL;
        int alias_cap_count = 0;
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
                alias_caps = aether_xrealloc(alias_caps, sizeof(char*) * (size_t)(alias_cap_count + 1));
                alias_caps[alias_cap_count++] = captures[i];
            }
        }
        if (!body) {
            free(alias_caps);
            alias_caps = NULL;
            alias_cap_count = 0;
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
            // Same reset for the heap-string set. Each closure body is
            // its own C function and hoists its own string locals, so a
            // name marked in a sibling closure or the enclosing function
            // must not carry over.
            char** prev_heap = gen->heap_string_vars;
            int prev_heap_count = gen->heap_string_var_count;
            int* prev_heap_depths = gen->heap_string_var_try_depths;
            gen->heap_string_vars = NULL;
            gen->heap_string_var_count = 0;
            gen->heap_string_var_try_depths = NULL;
            /* The seq escape set belongs to the body too, as a function
             * clears it at its start (codegen_func.c). */
            char** prev_captured = gen->captured_string_params;
            int prev_captured_count = gen->captured_string_param_count;
            gen->captured_string_params = NULL;
            gen->captured_string_param_count = 0;
            /* Track the closure as the current function so
             * body-structural queries (current_fn_body_block) resolve a
             * value identifier against the closure's own body, not the
             * enclosing function's. */
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
            const char** prev_alias = gen->current_alias_captures;
            int prev_alias_count = gen->current_alias_capture_count;
            gen->current_alias_captures = alias_caps;
            gen->current_alias_capture_count = alias_cap_count;
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
             * when the closure returns. Hoist the string locals and push
             * the scope-exit releases; exit_scope below emits them (and
             * the per-return emit_all_defers handles explicit returns). Balanced
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
                        emit_sized_array_param_copy(gen, p->node_type, p->value, body);
                    }
                    continue;
                }
                const char* param_cname = cg_internf("_param_%s", p->value);
                print_indent(gen);
                emit_promoted_param_cell(gen, p->value,
                                         p->node_type ? get_c_type(p->node_type) : "int",
                                         param_cname, p->line, p->column);
            }
            /* R3: a struct parameter borrows the caller's value, as a
             * function's does (codegen_func.c); one the body writes into, or
             * a promoted cell, is made a value of its own on entry. */
            for (int i = 0; i < closure->child_count; i++) {
                ASTNode* p = closure->children[i];
                if (!p || p->type != AST_CLOSURE_PARAM || !p->value) continue;
                const char* owning = struct_owning_strings(gen, p->node_type);
                if (!owning) {
                    if (!closure_param_is_promoted(gen, closure, p->value))
                        own_rebound_binding_take(gen, safe_value_name(p->value), p->node_type,
                                                 body, p->line, p->column);
                    continue;
                }
                /* A parameter named like a C keyword is renamed in the C
                 * signature and in the body (`volatile` is `ae_volatile`),
                 * so the walk, the capture and the destroy all use that
                 * name. */
                const char* cname = safe_value_name(p->value);
                if (closure_param_is_promoted(gen, closure, p->value)) {
                    const char* cell = cg_internf("(*%s)", p->value);
                    print_indent(gen);
                    fprintf(gen->output, "%s = _aether_copy_%s(%s);\n", cell, owning, cell);
                    continue;
                }
                if (!struct_param_written(body, cname)) continue;
                print_indent(gen);
                fprintf(gen->output, "%s = _aether_copy_%s(%s);\n", cname, owning, cname);
                push_struct_destroy_defer(gen, cname, p->node_type, p->line, p->column);
            }
            /* #2761: the `string?` pass too, in the order a function body
             * runs them (codegen_func.c). */
            hoist_heap_string_trackers(gen, body);
            hoist_opt_string_locals(gen, body);
            push_heap_string_exit_free_defers(gen, body);
            for (int i = 0; i < body->child_count; i++) {
                generate_statement(gen, body->children[i]);
            }
            exit_scope(gen);
            gen->current_env_captures = prev_env;
            gen->current_env_capture_count = prev_env_count;
            gen->current_alias_captures = prev_alias;
            gen->current_alias_capture_count = prev_alias_count;
            free(alias_caps);
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
            gen->heap_string_vars = prev_heap;
            gen->heap_string_var_count = prev_heap_count;
            gen->heap_string_var_try_depths = prev_heap_depths;
            clear_captured_string_params(gen);
            gen->captured_string_params = prev_captured;
            gen->captured_string_param_count = prev_captured_count;
            gen->current_function = prev_current_function;
            gen->closure_var_scope = prev_closure_var_scope;
            gen->in_trailing_block--;
            gen->indent_level = 0;
        }
        gen->in_string_closure = 0;
        gen->current_func_return_type = NULL;
        if (closure_ret_tuple) free_type(closure_ret_tuple);
        restore_try_clobbered_vars(gen, &outer_try_set);

        free(env_captures);

        fprintf(gen->output, "}\n\n");

        /* The env constructor for compilers without statement expressions
         * (MSVC, AETHER_GCC_COMPAT=0): it builds and returns the env, and
         * the closure literal wraps it in a compound literal (#2889). */
        if (cap_count > 0) {
            fprintf(gen->output, "#if !AETHER_GCC_COMPAT\n");
            fprintf(gen->output, "static void* _aether_make_env_%d(", id);
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
                    fprintf(gen->output, "    _e->%s = _aether_copy_%s(%s);\n", captures[i],
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
                    /* #2464: an array does not assign; copy its elements. */
                    fprintf(gen->output, "    ");
                    Type* sat = capture_sized_array_type(gen, captures[i], parent_func);
                    emit_capture_array_copy(gen, captures[i], captures[i], sat);
                    if (capture_composite_type(gen, captures[i], parent_func))
                        capture_take(gen, captures[i], sat);
                    fprintf(gen->output, "\n");
                } else {
                    fprintf(gen->output, "    _e->%s = %s;", captures[i], captures[i]);
                    Type* ct = capture_composite_type(gen, captures[i], parent_func);
                    if (ct) capture_take(gen, captures[i], ct);
                    fprintf(gen->output, "\n");
                }
            }
            fprintf(gen->output, "    return _e;\n");
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
static Type* msg_array_field(CodeGenerator* gen, const char* msg, const char* name);

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
        if (!fdef || !fdef->element_c_type ||
            msg_array_field(gen, message->value, field_init->value)) continue;

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
        (init->type == AST_FUNCTION_CALL || init->type == AST_STRING_INTERP ||
         is_owned_string_unwrap(gen, init)) &&
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
    /* #2664: a @c_callback function is the C symbol its annotation binds
     * (#235), whatever it is called by: its own name, or `mod.f` from a
     * module importing it (the post-merge `<ns>_f`). A use of the name as a
     * value was emitted so already; a call was emitted by the Aether name,
     * which no definition carries when the annotation names a symbol. */
    const char* cb_sym = lookup_c_callback_symbol(gen, codegen_normalise_callee(func_name));
    if (cb_sym) return cb_sym;
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
 * reference. Returns the stored value, else NULL. */
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
    TempSeq s;
    const char* lv = NULL;
    const char* lr = NULL;
    if (temp_val) {
        seq_open(gen, &s);
        lv = seq_let(gen, &s, "_AeClosure", "_ae_lv");
        generate_expression(gen, val);
        seq_next(gen, &s);
        lr = seq_let(gen, &s, e->wrapper ? "const char*" : "int", "_ae_lr");
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
        fprintf(gen->output, "%s))", lv);
        seq_next(gen, &s);
        fprintf(gen->output, "_aether_closure_env_release(%s.env)", lv);
        seq_next(gen, &s);
        fprintf(gen->output, discarded ? "(void)%s" : "%s", lr);
        seq_close(gen, &s);
    } else {
        generate_expression(gen, val);
        fprintf(gen->output, "))");
    }
}

/* The type of message `msg`'s field `name` when it is a fixed-size array,
 * which the message holds by value and owns the elements of; else NULL. */
static Type* msg_array_field(CodeGenerator* gen, const char* msg, const char* name) {
    Type* t = message_field_type(gen->program, msg, name);
    return type_is_sized_array(t) ? t : NULL;
}

int message_has_array_fields(CodeGenerator* gen, ASTNode* message) {
    for (int i = 0; i < message->child_count; i++) {
        ASTNode* fi = message->children[i];
        if (fi && fi->type == AST_FIELD_INIT && msg_array_field(gen, message->value, fi->value))
            return 1;
    }
    return 0;
}

/* The initializers of message literal `message` after its `._message_id`.
 * A fixed-size array is left out: C initializes no array from an array
 * value, so emit_message_array_fills fills it once the message exists. */
void emit_message_field_inits(CodeGenerator* gen, ASTNode* message, MessageDef* msg_def) {
    for (int i = 0; i < message->child_count; i++) {
        ASTNode* fi = message->children[i];
        if (!fi || fi->type != AST_FIELD_INIT || msg_array_field(gen, message->value, fi->value))
            continue;
        fprintf(gen->output, ", .%s = ", fi->value);
        if (fi->child_count == 0) continue;
        /* A struct, an optional or a tuple that owns is taken as any place
         * of its type takes it; an optional's bare value is wrapped. */
        Type* ft = message_field_type(gen->program, message->value, fi->value);
        if (ft && ft->kind != TYPE_STRING && ft->kind != TYPE_FUNCTION && own_type_owns(gen, ft)) {
            emit_value_take(gen, fi->children[0], ft);
        } else if (ft && ft->kind == TYPE_OPTIONAL) {
            emit_optional_coerced(gen, fi->children[0], ft);
        } else {
            emit_message_field_init(gen, find_msg_field(msg_def, fi->value), fi->children[0]);
        }
    }
}

/* Each fixed-size array field of message literal `message` filled into the
 * message `lv`, each element taken: parts of the sequence `s`, or
 * statements when it is NULL. */
void emit_message_array_fills(CodeGenerator* gen, ASTNode* message, const char* lv, TempSeq* s) {
    for (int i = 0; i < message->child_count; i++) {
        ASTNode* fi = message->children[i];
        Type* t = fi && fi->type == AST_FIELD_INIT && fi->child_count > 0
                      ? msg_array_field(gen, message->value, fi->value) : NULL;
        if (!t) continue;
        if (s) seq_next(gen, s);
        emit_array_fill(gen, cg_internf("%s.%s", lv, fi->value), fi->children[0], t);
        if (!s) fprintf(gen->output, "; ");
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
     * `_aether_msg_release_<Msg>` once the handler is done: a fresh closure's
     * is adopted, a local's or a field's retained. */
    if (rhs && rhs->node_type && rhs->node_type->kind == TYPE_FUNCTION &&
        !rhs->node_type->is_fnptr && fdef && fdef->c_type &&
        strcmp(fdef->c_type, "_AeClosure") == 0) {
        emit_closure_take(gen, rhs);
        return;
    }
    /* #2890: a `ptr` field takes a closure boxed, as a `ptr` parameter
     * does, and a bare function as its code address. */
    if (fdef && fdef->type_kind == TYPE_PTR && emit_closure_into_ptr(gen, rhs)) return;
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
    if (node->value &&
        (node->type == AST_LITERAL || node->type == AST_PATTERN_LITERAL) &&
        node->node_type && node->node_type->kind == TYPE_STRING) {
        static_string_literal_index(gen, node->value, ast_literal_length(node));
    }
    for (int i = 0; i < node->child_count; i++) {
        collect_static_string_literals(gen, node->children[i]);
    }
}

/* R1 of the ownership model: every string literal of the program is a
 * counted string like any other, so `release` is right on any string value.
 * Each distinct literal is emitted here, once, as a static AetherString with
 * its length, in the layout of std/string/aether_string.h, pinned
 * (AETHER_STRING_PINNED_REFS) so that string_retain and string_release leave
 * it alone and it is never freed. Its bytes keep every NUL (#2520), since the
 * length, not a terminator, says where it ends. Where a literal goes to C as
 * characters (an extern's argument, a C struct's field), it is written as
 * those characters instead (emit_string_literal_chars). */
void emit_static_string_literals(CodeGenerator* gen, ASTNode* program) {
    collect_static_string_literals(gen, program);
    if (gen->static_str_count == 0) return;
    fprintf(gen->output, "/* The program's string literals: static, pinned AetherStrings. */\n");
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
        fprintf(gen->output, "static const _AeStaticStr _ae_slit_%d = { 0x%Xu, 0x%X, %d, %d, (char*)_ae_slit_%d_bytes };\n",
                i, (unsigned)AETHER_STRING_MAGIC, (unsigned)AETHER_STRING_PINNED_REFS,
                len, len + 1, i);
    }
}

void emit_string_literal_node(CodeGenerator* gen, const ASTNode* lit) {
    int idx = static_string_literal_index(gen, lit->value, ast_literal_length(lit));
    fprintf(gen->output, "((const char*)&_ae_slit_%d)", idx);
}

void emit_string_literal_chars(CodeGenerator* gen, const ASTNode* lit) {
    if (lit->value_len > 0) {
        /* Its bytes, a NUL included, as the pinned string holds them. */
        int idx = static_string_literal_index(gen, lit->value, lit->value_len);
        fprintf(gen->output, "_ae_slit_%d_bytes", idx);
    } else {
        emit_c_string_literal(gen, lit->value);
    }
}

/* A string value as the characters C reads (R4): a literal as its C
 * literal, anything else, a counted or pinned string, through
 * aether_string_data, which also passes a plain C string as it is. */
void emit_string_as_c_chars(CodeGenerator* gen, ASTNode* e) {
    if (e && e->type == AST_LITERAL) {
        emit_string_literal_chars(gen, e);
        return;
    }
    fprintf(gen->output, "aether_string_data(");
    generate_expression(gen, e);
    fprintf(gen->output, ")");
}

/* An argument of a builtin the runtime implements in C: a string as its
 * characters (R4), anything else as it is. */
static void emit_c_builtin_arg(CodeGenerator* gen, ASTNode* e) {
    if (e && e->node_type && e->node_type->kind == TYPE_STRING) emit_string_as_c_chars(gen, e);
    else generate_expression(gen, e);
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

/* #2723: an argument for a fixed-size array parameter (`a: int[4]`). Such a
 * parameter is a value: the callee copies its caller's elements into an
 * array of its own (#2516), through a plain `E _param_a[N]`. An array a
 * `try` body writes has volatile elements (#2690), and passed as it stands
 * it discarded the qualifier (an error under -Werror). The storage may be
 * volatile when the array's root variable (`xs`, or `b` of `b.v`) is
 * written in a `try` body of this function, whatever the depth: such an
 * argument is copied into an array of the call's own, a compound literal
 * that lives as long as the call's block, read byte by byte through a
 * volatile pointer (_aether_array_arg_copy). Any other argument passes
 * as before, since copying it again would only cost. */
static int sized_array_arg_may_be_volatile(CodeGenerator* gen, ASTNode* arg) {
    if (!arg || !type_is_sized_array(arg->node_type) || arg->node_type->array_size <= 0)
        return 0;
    for (ASTNode* e = arg; e; ) {
        if (e->type == AST_IDENTIFIER) return e->value && is_try_clobbered_var(gen, e->value);
        if ((e->type == AST_MEMBER_ACCESS || e->type == AST_ARRAY_ACCESS) && e->child_count > 0) {
            e = e->children[0];
            continue;
        }
        return 0;
    }
    return 0;
}

void generate_sized_array_argument(CodeGenerator* gen, ASTNode* arg, Type* param_type) {
    if (!is_sized_array_param(param_type) || !sized_array_arg_may_be_volatile(gen, arg)) {
        generate_expression(gen, arg);
        return;
    }
    const char* et = get_c_type(param_type->element_type);
    fprintf(gen->output, "((%s*)_aether_array_arg_copy((%s[%d]){0}, ", et, et,
            param_type->array_size);
    generate_expression(gen, arg);
    fprintf(gen->output, ", sizeof(%s[%d])))", et, param_type->array_size);
}

int is_empty_slice_value(const ASTNode* e) {
    if (!e || e->type != AST_SLICE_FROM_ARRAY || e->child_count < 1 || !e->children[0])
        return 0;
    const ASTNode* src = e->children[0];
    return src->type == AST_NULL_LITERAL ||
           (src->type == AST_ARRAY_LITERAL && src->child_count == 0);
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
int expr_is_c_view_of_slice(CodeGenerator* gen, const ASTNode* expr) {
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

/* --- Values computed ahead of their statement (#2765) --------------------
 *
 * Three constructs run statements as part of an expression: an `or` whose
 * handler is a block, a `!` that propagates an error by returning, and a
 * call with a trailing block where its value is used. As expressions they
 * are GNU statement expressions, which only GCC-compatible compilers take.
 * Instead each runs as statements ahead of its statement (generate_statement
 * finds them: stmt_control_prelude), its value left in a temporary, and the
 * statement reads the temporary where the construct was. What the statement
 * evaluates before one is evaluated ahead of it too (#2798, below): the same
 * order of evaluation, in plain C. Where that cannot be done (see
 * ctl_linearize) it stays a statement expression.
 *
 * g_ctl_emitting is the construct whose statements are being emitted;
 * g_ctl_subs maps each one done to the text of its value, until its
 * statement ends. */
static const ASTNode* g_ctl_emitting = NULL;
static ArgDrainSub* g_ctl_subs = NULL;
static int g_ctl_count = 0;
static int g_ctl_cap = 0;

static const char* ctl_lookup(const ASTNode* node) {
    for (int i = g_ctl_count - 1; node && i >= 0; i--)
        if (g_ctl_subs[i].node == node) return g_ctl_subs[i].name;
    return NULL;
}

static void ctl_bind(ASTNode* node, const char* text) {
    if (g_ctl_count >= g_ctl_cap) {
        int cap = g_ctl_cap ? g_ctl_cap * 2 : 8;
        g_ctl_subs = (ArgDrainSub*)aether_xrealloc(g_ctl_subs, sizeof(ArgDrainSub) * (size_t)cap);
        g_ctl_cap = cap;
    }
    g_ctl_subs[g_ctl_count].node = node;
    g_ctl_subs[g_ctl_count].name = strdup(text);
    if (!g_ctl_subs[g_ctl_count].name) {
        fprintf(stderr, "Fatal: out of memory binding a value\n");
        exit(1);
    }
    g_ctl_count++;
}

int control_prelude_depth(void) {
    return g_ctl_count;
}

void control_prelude_end(int depth) {
    while (g_ctl_count > depth) {
        g_ctl_count--;
        free(g_ctl_subs[g_ctl_count].name);
    }
}

/* The C type of the value of call `call` with a trailing block: its checked
 * type, else the type its definition returns (fn_result_type); NULL when
 * neither names one. */
static const char* trailing_call_c_type(CodeGenerator* gen, ASTNode* call) {
    Type* t = call->node_type;
    if (t && t->kind != TYPE_UNKNOWN && t->kind != TYPE_VOID) return get_c_type(t);
    if (!call->value || !gen->program) return NULL;
    ASTNode* def = find_function_definition_by_name(gen->program,
                                                    codegen_normalise_callee(call->value));
    Type* r = def ? fn_result_type(gen, def) : NULL;
    return r ? get_c_type(r) : NULL;
}

/* Is `e` one of the three constructs, in the form that runs statements, and
 * one emit_control_prelude can run ahead of its statement? */
int is_control_value(CodeGenerator* gen, ASTNode* e) {
    if (!e) return 0;
    if (e->type == AST_OR_ELSE) {
        if (e->child_count < 2 || !e->children[1] || e->children[1]->type != AST_BLOCK) return 0;
        Type* tup = e->children[0] ? e->children[0]->node_type : NULL;
        return tup && tup->kind == TYPE_TUPLE && tup->tuple_count >= 2 && tup->tuple_types[0];
    }
    if (e->type == AST_TUPLE_UNWRAP) {
        Type* tup = e->child_count > 0 && e->children[0] ? e->children[0]->node_type : NULL;
        return tup && tup->kind == TYPE_TUPLE && tup->tuple_count >= 2 &&
               gen->current_func_return_type && gen->current_func_return_type->is_result;
    }
    if (e->type == AST_FUNCTION_CALL) {
        if (e == gen->trailing_stmt_call) return 0;
        ASTNode* block = trailing_dsl_block(gen, e);
        if (!block || block->child_count == 0 || !trailing_call_c_type(gen, e)) return 0;
        /* A call generate_expression wraps before it reaches the block (its
         * arguments evaluated in order, its result taken as owned or
         * copied, a statement's struct temporary) is left as it is: the
         * wrap reads what the call does inside it. */
        ASTNode* ops[ORDER_MAX_OPERANDS];
        int hoist[ORDER_MAX_OPERANDS];
        int n = order_operands(e, ops, ORDER_MAX_OPERANDS);
        /* A builder's lowering orders its arguments itself, after its
         * block (ctl_builder_args). */
        if (n >= 2 && !(e->value && is_builder_func_reg(gen, e->value)) &&
            order_hoist_set(gen, ops, n, hoist))
            return 0;
        return !call_returns_view_of_temp(gen, e) &&
               !call_returns_tuple_view_of_temp(gen, e) && !expr_is_c_view_of_slice(gen, e) &&
               !stmt_struct_temp_of(e);
    }
    return 0;
}

static int emit_trailing_call_expression(CodeGenerator* gen, ASTNode* call);

/* Run `e` (is_control_value) as statements here, ahead of its statement,
 * and bind its value: `discarded`, the statement throws the value away, so
 * it reads it as a void expression. Returns 0, emitting nothing, for any
 * other node or when temporaries cannot be declared ahead of the
 * statement. */
int emit_control_prelude(CodeGenerator* gen, ASTNode* e, int discarded) {
    if (!hoist_ready(gen) || !is_control_value(gen, e)) return 0;
    int mark = g_ctl_count;
    const ASTNode* saved = g_ctl_emitting;
    g_ctl_emitting = e;
    /* A call goes straight to its block's lowering: nothing wraps it here
     * (is_control_value). */
    if (e->type == AST_FUNCTION_CALL) emit_trailing_call_expression(gen, e);
    else generate_expression(gen, e);
    g_ctl_emitting = saved;
    if (g_ctl_count > mark && discarded) {
        char* v = g_ctl_subs[g_ctl_count - 1].name;
        g_ctl_subs[g_ctl_count - 1].name = strdup(cg_internf("(void)%s", v));
        free(v);
    }
    return g_ctl_count > mark;
}

/* --- The same anywhere in a statement (#2798) ----------------------------
 *
 * A construct that is not the first thing its statement evaluates runs
 * ahead of the statement too, once everything the statement evaluates
 * before it has been: each operand to its left (an earlier argument, the
 * left side of an operation, an earlier field) is evaluated into a
 * temporary first, in source order, and read from there (bound as an
 * operand evaluated ahead is, #2478). A construct on the right of `&&` or
 * `||` runs only when that side would: the operation becomes an `if` ahead
 * of the statement, its value a temporary. So does one in a part of an
 * expression that runs on a test (a branch of an `if` expression, the
 * default of `??`, an `or` handler that is an expression): the test's
 * operands are evaluated first, and the part under the same test, into a
 * temporary the expression reads where it reads the part. A loop condition
 * holding one is evaluated, with it, at the top of each pass (the loop
 * emitters).
 *
 * ctl_contains: does `e` hold such a construct? A closure literal's body,
 * a block's and a match's statements see to their own. */
int ctl_contains(CodeGenerator* gen, ASTNode* e) {
    if (!e || e->type == AST_CLOSURE || e->type == AST_BLOCK ||
        e->type == AST_MATCH_STATEMENT) return 0;
    if (is_control_value(gen, e)) return 1;
    for (int i = 0; i < e->child_count; i++)
        if (ctl_contains(gen, e->children[i])) return 1;
    return 0;
}

/* Is `call` (one with a trailing block) a builder's, whose block runs
 * before its arguments are evaluated? */
static int trailing_call_is_builder(CodeGenerator* gen, ASTNode* call) {
    return call->value && is_builder_func_reg(gen, call->value);
}

/* Evaluate operand `op` now, ahead of the statement, into a temporary the
 * statement reads: `_eoN = (op);`. 0 when its type has no C spelling. */
static int ctl_emit_hoist(CodeGenerator* gen, ASTNode* op) {
    const char* ct = order_c11_type(op);
    if (!ct) return 0;
    const char* name = cg_internf("_eo%d", g_order_counter++);
    hoist_declare(gen, ct, name);
    print_indent(gen);
    fprintf(gen->output, "%s = (", name);
    generate_expression(gen, op);
    fprintf(gen->output, ");\n");
    order_bind(op, name);
    return 1;
}

/* The same where there is something to evaluate: a literal, an operand
 * already bound, or a function of the program named as a value (a constant:
 * its call site writes the function's adapter, and a temporary of the
 * operand's type could not hold it) has nothing. */
static int ctl_hoist(CodeGenerator* gen, ASTNode* op) {
    if (!op || !order_hoistable(op) || order_bound(op) || ctl_lookup(op)) return 1;
    if (op->type == AST_IDENTIFIER && bare_top_level_fn(gen, op)) return 1;
    return ctl_emit_hoist(gen, op);
}

/* Operand `k` of `parent` (a call, or NULL) evaluated ahead: a field read
 * the call frees or keeps is handed over as it is read, as the ordering
 * wrapper hands it over (order_hoist_handoff); read into a plain temporary,
 * the field kept its tracker and its struct freed it a second time. */
static int ctl_hoist_operand(CodeGenerator* gen, ASTNode* parent, int k, ASTNode* op) {
    /* A field the call frees or keeps: handed over to its temporary as it
     * is read (order_hoist_handoff), a statement ahead of the statement. */
    int kind = parent ? order_operand_hands_off(gen, parent, k) : 0;
    if (!kind && parent && parent->type == AST_FUNCTION_CALL && parent->value && op &&
        !order_bound(op) && field_read_can_hand_off(op)) {
        /* Where the parameter it goes to is not known here (named
         * arguments), an owned field read is not evaluated ahead: it stays
         * in place at the call, and the construct after it a statement
         * expression. (The call does not hand a named argument over
         * either, #2849.) */
        for (int i = 0; i < parent->child_count; i++)
            if (parent->children[i] && parent->children[i]->type == AST_NAMED_ARG) return 0;
    }
    if (!kind || !op || order_bound(op) || ctl_lookup(op)) return ctl_hoist(gen, op);
    const char* name = cg_internf("_eo%d", g_order_counter++);
    const char* own = emit_string_field_handover_ahead(gen, op, kind, NULL, name);
    order_bind(op, name);
    order_handover_add(gen, op, parent, push_handover_exit_free(gen, own, name), kind);
    return 1;
}

/* The operands `e` evaluates unconditionally, in source order, into `ops`:
 * an operation whose C spelling leaves their order open (order_operands),
 * and the ones C orders or that have one: a unary operation's, a field's
 * or an element's object (and index), an array literal's elements, an
 * interpolation's segments, the operand of `!`, `??`, `?.` and `or` and an
 * `if` expression's condition (what they run otherwise is conditional). -1
 * for anything else. */
static int ctl_operands(CodeGenerator* gen, ASTNode* e, ASTNode** ops, int cap) {
    (void)gen;
    int n = 0;
    if (e->type == AST_BINARY_EXPRESSION && e->child_count == 2 && e->value &&
        is_assignment_op(e->value)) {
        /* The target's indexes, then the value: a target with none
         * (`o.f = v`) is no operand list for order_operands. */
        n = order_target_operands(e->children[0], ops, 0, cap - 1);
        ops[n++] = e->children[1];
        return n;
    }
    switch (e->type) {
        case AST_FUNCTION_CALL: case AST_BINARY_EXPRESSION: case AST_SEND_FIRE_FORGET:
        case AST_SEND_ASK: case AST_STRUCT_LITERAL: case AST_MESSAGE_CONSTRUCTOR:
            n = order_operands(e, ops, cap);
            return n > 0 ? n : -1;
        case AST_UNARY_EXPRESSION: case AST_MEMBER_ACCESS: case AST_VALUE_CAST:
        case AST_TUPLE_UNWRAP: case AST_NULL_COALESCE: case AST_OR_ELSE:
        case AST_OPTIONAL_CHAIN: case AST_IF_EXPRESSION:
            if (e->child_count < 1) return -1;
            ops[0] = e->children[0];
            return 1;
        case AST_ARRAY_ACCESS:
            if (e->child_count < 2) return -1;
            ops[0] = e->children[0];
            ops[1] = e->children[1];
            return 2;
        case AST_ARRAY_LITERAL: case AST_STRING_INTERP:
            for (int i = 0; i < e->child_count; i++) {
                ASTNode* c = e->children[i];
                if (e->type == AST_STRING_INTERP && interp_segment_is_text(c)) continue;
                if (n == cap) return -1;
                ops[n++] = c;
            }
            return n;
        default:
            return -1;
    }
}

static int ctl_linearize(CodeGenerator* gen, ASTNode* e, int discarded);

/* Is there a construct in `c`, a child of an expression whose operands are
 * `ops`, that those operands do not reach? A listed operand is reached, and
 * so is a node made of listed operands only (a named argument, a field's
 * initializer, the message a send carries, an assignment's target); a
 * construct anywhere else is not (it runs on a condition the list does not
 * have, or in a part no list covers). */
static int ctl_unreached(CodeGenerator* gen, ASTNode* c, ASTNode** ops, int n) {
    if (!c || c->type == AST_CLOSURE || c->type == AST_BLOCK ||
        c->type == AST_MATCH_STATEMENT) return 0;
    for (int k = 0; k < n; k++)
        if (ops[k] == c) return 0;
    if (is_control_value(gen, c)) return 1;
    for (int i = 0; i < c->child_count; i++)
        if (ctl_unreached(gen, c->children[i], ops, n)) return 1;
    return 0;
}

/* Operand `op` evaluated now into a temporary, a literal too (a test reads
 * it by name), as the C text that reads it; NULL when it cannot be. */
static const char* ctl_value_name(CodeGenerator* gen, ASTNode* op) {
    if (!op) return NULL;
    if (!order_bound(op) && !ctl_lookup(op) &&
        (order_hoistable(op) || op->type == AST_LITERAL) && !ctl_emit_hoist(gen, op))
        return NULL;
    const char* name = order_lookup(op);
    if (!name) name = ctl_lookup(op);
    if (!name) name = arg_drain_lookup(op);
    return name;
}

/* `c`, a part of an expression that runs only when `test` holds (a C
 * condition on operands already evaluated into temporaries, the one the
 * expression tests), run under that test ahead of the statement, its
 * constructs and then its value into a temporary: the expression reads the
 * temporary on the path where it was set. */
static int ctl_guarded(CodeGenerator* gen, const char* test, ASTNode* c) {
    print_indent(gen);
    fprintf(gen->output, "if (%s) {\n", test);
    int ok = ctl_linearize(gen, c, 0) && ctl_hoist(gen, c);
    print_indent(gen);
    fprintf(gen->output, "}");
    /* The expression reads the temporary only where the test holds, which
     * a C compiler cannot always tell (-Wmaybe-uninitialized): it is zero
     * on the other path. */
    const char* name = order_lookup(c);
    if (!name) name = ctl_lookup(c);
    const char* ct = order_c11_type(c);
    if (ok && name && ct) fprintf(gen->output, " else { %s = (%s){0}; }", name, ct);
    fprintf(gen->output, "\n");
    return ok;
}

/* The conditional parts of `e` that hold a construct, run each under the
 * test that selects it: the branches of an `if` expression, the default of
 * `??` and an `or` handler that is an expression. -1 when `e` is none of
 * those, or none of its conditional parts holds one. */
static int ctl_conditional(CodeGenerator* gen, ASTNode* e) {
    if (e->type == AST_IF_EXPRESSION && e->child_count >= 3 &&
        (ctl_contains(gen, e->children[1]) || ctl_contains(gen, e->children[2]))) {
        if (!ctl_linearize(gen, e->children[0], 0)) return 0;
        const char* c = ctl_value_name(gen, e->children[0]);
        if (!c) return 0;
        int ok = 1;
        if (ctl_contains(gen, e->children[1]))
            ok = ctl_guarded(gen, c, e->children[1]);
        if (ok && ctl_contains(gen, e->children[2]))
            ok = ctl_guarded(gen, cg_internf("!%s", c), e->children[2]);
        return ok;
    }
    if (e->type == AST_NULL_COALESCE && e->child_count >= 2 &&
        ctl_contains(gen, e->children[1])) {
        Type* ot = e->children[0] ? e->children[0]->node_type : NULL;
        if (!ot || ot->kind != TYPE_OPTIONAL) return -1;   /* the default is not emitted */
        if (!ctl_linearize(gen, e->children[0], 0)) return 0;
        const char* o = ctl_value_name(gen, e->children[0]);
        if (!o) return 0;
        return ctl_guarded(gen, cg_internf("!%s.has", o), e->children[1]);
    }
    if (e->type == AST_OR_ELSE && e->child_count >= 2 && e->children[1] &&
        e->children[1]->type != AST_BLOCK && ctl_contains(gen, e->children[1])) {
        Type* tup = e->children[0] ? e->children[0]->node_type : NULL;
        if (!tup || tup->kind != TYPE_TUPLE || tup->tuple_count < 2 || !tup->tuple_types[0])
            return 0;
        if (!ctl_linearize(gen, e->children[0], 0)) return 0;
        const char* f = ctl_value_name(gen, e->children[0]);
        if (!f) return 0;
        /* The test the `or` makes: a non-empty error slot. */
        int k = tup->tuple_count - 1;
        return ctl_guarded(gen, cg_internf("%s._%d && aether_string_data((const void*)%s._%d)[0]",
                                           f, k, f, k), e->children[1]);
    }
    return -1;
}

/* Run the constructs in operands `ops` (evaluated in this order) of
 * `parent` (the call or operation they belong to, or NULL for a
 * statement's own list) ahead of the statement, each operand to the left
 * of the last one that holds one evaluated into a temporary first. */
static int ctl_linearize_ops(CodeGenerator* gen, ASTNode* parent, ASTNode** ops, int n) {
    int last = -1;
    for (int i = 0; i < n; i++)
        if (ctl_contains(gen, ops[i])) last = i;
    for (int i = 0; i < last; i++) {
        if (ctl_contains(gen, ops[i]) && !ctl_linearize(gen, ops[i], 0)) return 0;
        if (!ctl_hoist_operand(gen, parent, i, ops[i])) return 0;
    }
    return last < 0 || ctl_linearize(gen, ops[last], 0);
}

int ctl_linearize_list(CodeGenerator* gen, ASTNode** ops, int n) {
    return ctl_linearize_ops(gen, NULL, ops, n);
}

/* Is `e` an `&&` or an `||`, whose right side runs only on its left's
 * say? */
static int ctl_short_circuit(ASTNode* e) {
    return e->type == AST_BINARY_EXPRESSION && e->child_count == 2 && e->value &&
           (strcmp(e->value, "&&") == 0 || strcmp(e->value, "||") == 0);
}

/* Run the constructs `e` holds ahead of the statement, and what it
 * evaluates before them. 1 when every one is bound, 0 when one sits where
 * this does not reach (an argument of a call whose trailing block runs as a
 * value): what was run stays bound, and the rest stays a statement
 * expression.
 * `discarded`: the statement throws `e`'s value away. */
static int ctl_linearize(CodeGenerator* gen, ASTNode* e, int discarded) {
    if (!e || !ctl_contains(gen, e)) return 1;
    if (is_control_value(gen, e)) {
        /* What the construct evaluates first: the tuple an `or` or a `!`
         * tests, the arguments of a call whose trailing block runs after
         * it. A builder's block runs first, and its arguments after it, in
         * its lowering (emit_trailing_call_expression). */
        if (e->type == AST_FUNCTION_CALL) {
            if (!trailing_call_is_builder(gen, e)) {
                ASTNode* ops[ORDER_MAX_OPERANDS];
                int n = order_operands(e, ops, ORDER_MAX_OPERANDS);
                if (n > 0 && !ctl_linearize_ops(gen, e, ops, n)) return 0;
            }
        } else if (e->child_count > 0 && !ctl_linearize(gen, e->children[0], 0)) {
            return 0;
        }
        return emit_control_prelude(gen, e, discarded);
    }
    if (ctl_short_circuit(e)) {
        ASTNode* l = e->children[0];
        ASTNode* r = e->children[1];
        if (!ctl_contains(gen, r)) return ctl_linearize(gen, l, 0);
        if (!ctl_linearize(gen, l, 0)) return 0;
        int is_and = strcmp(e->value, "&&") == 0;
        const char* t = cg_internf("_ae_sc%d", g_order_counter++);
        hoist_declare(gen, "int", t);
        print_indent(gen);
        fprintf(gen->output, "%s = !!(", t);
        generate_expression(gen, l);
        fprintf(gen->output, ");\n");
        print_indent(gen);
        fprintf(gen->output, "if (%s%s) {\n", is_and ? "" : "!", t);
        int ok = ctl_linearize(gen, r, 0);
        print_indent(gen);
        fprintf(gen->output, "%s = !!(", t);
        generate_expression(gen, r);
        fprintf(gen->output, ");\n");
        print_indent(gen);
        fprintf(gen->output, "}\n");
        order_bind(e, t);
        return ok;
    }
    int cond = ctl_conditional(gen, e);
    if (cond >= 0) return cond;
    ASTNode* ops[ORDER_MAX_OPERANDS];
    int n = ctl_operands(gen, e, ops, ORDER_MAX_OPERANDS);
    if (n < 0) return 0;
    for (int i = 0; i < e->child_count; i++)
        if (ctl_unreached(gen, e->children[i], ops, n)) return 0;
    if (!ctl_linearize_ops(gen, e, ops, n)) return 0;
    /* A call with a trailing block whose arguments kept it from running
     * ahead (is_control_value: their order needed a wrap) may now, with
     * them evaluated. Not a builder's, whose block runs before them. */
    if (e->type == AST_FUNCTION_CALL && !trailing_call_is_builder(gen, e) &&
        is_control_value(gen, e))
        return emit_control_prelude(gen, e, discarded);
    return 1;
}

int ctl_linearize_value(CodeGenerator* gen, ASTNode* e, int discarded) {
    return hoist_ready(gen) ? ctl_linearize(gen, e, discarded) : 0;
}

/* The arguments of builder call `call`, which it evaluates after the
 * builder's block, the call written as a value (emit_trailing_call_expression)
 * or run by its statement (emit_builder_args_prelude): when one holds a
 * construct that runs statements, or their order matters
 * (order_hoist_set), each is evaluated ahead of the call, left to right,
 * its constructs first, and all but the last into a temporary. The two
 * forms evaluate them the same way: the block, then the arguments left to
 * right. */
void ctl_builder_args(CodeGenerator* gen, ASTNode* call) {
    if (!hoist_ready(gen)) return;
    ASTNode* ops[ORDER_MAX_OPERANDS];
    int hoist[ORDER_MAX_OPERANDS];
    int n = order_operands(call, ops, ORDER_MAX_OPERANDS);
    if (n <= 0) return;
    int need = n >= 2 && order_hoist_set(gen, ops, n, hoist);
    for (int i = 0; i < n && !need; i++) need = ctl_contains(gen, ops[i]);
    if (!need) return;
    for (int i = 0; i < n; i++) {
        if (ctl_contains(gen, ops[i]) && !ctl_linearize(gen, ops[i], 0)) return;
        if (i < n - 1 && !ctl_hoist(gen, ops[i])) return;
    }
}

/* `call(args) { block }` where the value is used inside an expression:
 * `return build() { ... }`, `f(build() { ... })`. A declaration, an
 * assignment and an expression statement run the block themselves
 * (generate_statement marks that call); anywhere else the block was
 * skipped with the argument loop's DSL test and silently dropped. Lowered
 * here to a statement expression that runs it the way those statements
 * do: a builder's block configures it before the call; any other call's
 * value is the block's context. Run ahead of its statement instead where
 * it can be (emit_control_prelude, #2765, #2798).
 * Returns 0 when there is no such block. */
static int g_trailing_tmp = 0;
/* An argument of `call(f, ...)` for the closure's parameter of type `pt`,
 * coerced as a direct call's argument is: a bare value or `none` into an
 * optional parameter, a variant into a sum parameter (#2840); else as it is,
 * a fixed-size array copied for a `T[N]` parameter (#2723). */
static void emit_closure_argument(CodeGenerator* gen, ASTNode* arg, Type* pt) {
    if (pt && pt->kind == TYPE_OPTIONAL && needs_optional_coerce(arg, pt)) {
        emit_optional_coerced(gen, arg, pt);
        return;
    }
    if (pt && pt->kind == TYPE_SUM && needs_sum_coerce(arg, pt)) {
        emit_sum_coerced(gen, arg, pt);
        return;
    }
    generate_sized_array_argument(gen, arg, pt);
}

/* The builder call emit_builder_call is emitting, and the C name of the
 * config its block filled: the call's last argument, `(void*)0` for any
 * other builder call. */
static ASTNode* g_builder_cfg_call = NULL;
static const char* g_builder_cfg_var = NULL;

/* `call`, a builder call with a trailing block, as `fn(args..., cfg)`:
 * through the ordinary call emitter, so its arguments get what any call's
 * do (a fresh string freed after the call, a field handed over, the order
 * wrapper), with `cfg` for the config the block filled. Its arguments were
 * evaluated ahead, after the block, where they had to be
 * (ctl_builder_args). Emitted by hand before, the call left every fresh
 * argument to leak. */
void emit_builder_call(CodeGenerator* gen, ASTNode* call, const char* cfg) {
    BuilderCfg saved = builder_cfg_begin(gen, call, cfg);
    generate_expression(gen, call);
    builder_cfg_end(gen, saved);
}

/* From here to builder_cfg_end, `call` is emitted as the plain call with
 * `cfg` (a statement's own emitter generating its value as it would any
 * call's). */
BuilderCfg builder_cfg_begin(CodeGenerator* gen, ASTNode* call, const char* cfg) {
    BuilderCfg saved = { gen->trailing_stmt_call, g_builder_cfg_call, g_builder_cfg_var };
    gen->trailing_stmt_call = call;      /* the call alone, its block run */
    g_builder_cfg_call = call;
    g_builder_cfg_var = cfg;
    return saved;
}

void builder_cfg_end(CodeGenerator* gen, BuilderCfg saved) {
    gen->trailing_stmt_call = saved.trailing_stmt_call;
    g_builder_cfg_call = saved.call;
    g_builder_cfg_var = saved.var;
}

/* Is `call` the builder call emit_builder_call is emitting, its block run
 * and its config in hand? Its statement then emits it as a plain call. */
int builder_call_configured(ASTNode* call) {
    return call && call == g_builder_cfg_call;
}

static int emit_trailing_call_expression(CodeGenerator* gen, ASTNode* call) {
    ASTNode* block = trailing_dsl_block(gen, call);
    /* An empty block runs nothing and configures nothing: the plain call,
     * as before (and a builder's config factory is not needed for it). */
    if (!block || block->child_count == 0) return 0;
    /* #2765: run ahead of the statement (emit_control_prelude): statements,
     * the value left in a temporary the statement reads. */
    const char* done = ctl_lookup(call);
    if (done) {
        fprintf(gen->output, "%s", done);
        return 1;
    }
    int as_stmts = g_ctl_emitting == call;
    int n = g_trailing_tmp++;
    if (call->value && is_builder_func_reg(gen, call->value)) {
        if (as_stmts) {
            hoist_declare(gen, "void*", cg_internf("_tcfg%d", n));
            hoist_declare(gen, trailing_call_c_type(gen, call), cg_internf("_tcr%d", n));
            print_indent(gen);
            fprintf(gen->output, "_tcfg%d = (void*)(intptr_t)%s(); _aether_ctx_push(_tcfg%d);\n",
                    n, get_builder_factory(gen, call->value), n);
        } else {
            fprintf(gen->output, "({ void* _tcfg%d = (void*)(intptr_t)%s(); _aether_ctx_push(_tcfg%d);\n",
                    n, get_builder_factory(gen, call->value), n);
        }
        emit_builder_block_body(gen, block, cg_internf("_tcfg%d", n), call->value);
        if (as_stmts) {
            /* #2798: the arguments, which the call evaluates after the
             * block, left to right as the statement form does. */
            ctl_builder_args(gen, call);
            print_indent(gen);
            fprintf(gen->output, "_tcr%d = ", n);
        }
        emit_builder_call(gen, call, cg_internf("_tcfg%d", n));
        fprintf(gen->output, "%s", as_stmts ? ";\n" : "; })");
        if (as_stmts) ctl_bind(call, cg_internf("_tcr%d", n));
        return 1;
    }
    ASTNode* saved = gen->trailing_stmt_call;
    if (as_stmts) {
        hoist_declare(gen, trailing_call_c_type(gen, call), cg_internf("_tcv%d", n));
        print_indent(gen);
        fprintf(gen->output, "_tcv%d = ", n);
    } else {
        fprintf(gen->output, "({ __auto_type _tcv%d = ", n);
    }
    gen->trailing_stmt_call = call;      /* the plain call, block skipped */
    generate_expression(gen, call);
    gen->trailing_stmt_call = saved;
    fprintf(gen->output, "; _aether_ctx_push((void*)(intptr_t)_tcv%d);\n", n);
    emit_trailing_block_body(gen, block);
    if (as_stmts) ctl_bind(call, cg_internf("_tcv%d", n));
    else fprintf(gen->output, "_tcv%d; })", n);
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
static const ASTNode* g_view_copy_wrapping = NULL;   /* #2619 */

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

/* The temporaries in force, so a statement emitted inside another one's
 * value (an `or` handler's, a closure body's) can put them back when it is
 * done rather than clearing them under the outer statement (#2582). */
void stmt_struct_temps_get(ASTNode*** nodes, const char*** names, int* count) {
    *nodes = g_stmt_temp_nodes;
    *names = g_stmt_temp_names;
    *count = g_stmt_temp_count;
}

/* The temporary holding call `expr`'s struct result in the statement being
 * emitted, or NULL. */
const char* stmt_struct_temp_of(const ASTNode* expr) {
    for (int i = 0; expr && i < g_stmt_temp_count; i++) {
        if (g_stmt_temp_nodes[i] == expr) return g_stmt_temp_names[i];
    }
    return NULL;
}

/* #2369: is `call` a call of C's malloc, under any Aether name bound to it
 * (`extern malloc(n)`, `@extern("malloc") m(n)`)? */
static int call_is_c_malloc(CodeGenerator* gen, ASTNode* call) {
    if (!call->value || call->child_count != 1 || !call->children[0] ||
        !is_extern_func(gen, call->value)) return 0;
    const char* sym = lookup_extern_c_name(gen, call->value);
    return sym && strcmp(sym, "malloc") == 0;
}

/* The owning optional, tuple or string sequence parameter type `t` when
 * argument `arg` passes it a value nothing holds (a call's result, a
 * literal, a fresh string wrapped); NULL for a place, which the callee
 * borrows as it is, and for a sequence a call lends (`string.seq_tail`). */
static Type* fresh_composite_arg_type(CodeGenerator* gen, ASTNode* arg, Type* t) {
    if (!t || (t->kind != TYPE_OPTIONAL && t->kind != TYPE_TUPLE && !is_string_seq_ptr_type(t)) ||
        !own_type_owns(gen, t))
        return NULL;
    if (!arg || arg->type == AST_NONE_LITERAL || !hoist_ready(gen)) return NULL;
    if (t->kind == TYPE_PTR) return seq_value_is_fresh(gen, arg) ? t : NULL;
    /* `o!` reads the place `o`'s value. */
    ASTNode* place = arg->type == AST_TUPLE_UNWRAP && arg->child_count > 0 ? arg->children[0] : arg;
    if (place && (place->type == AST_IDENTIFIER || place->type == AST_MEMBER_ACCESS ||
                  place->type == AST_ARRAY_ACCESS)) return NULL;
    return t;
}

/* An array literal of owning elements passed as an argument is a temporary
 * of the statement, as a call's array result is: filled, each element
 * taken, and dropped when its block is done. */
static int array_literal_arg_temp(CodeGenerator* gen, ASTNode* arg) {
    if (arg->type != AST_ARRAY_LITERAL || !type_is_sized_array(arg->node_type) ||
        !own_type_owns(gen, arg->node_type) || !hoist_ready(gen)) return 0;
    static int alt_seq = 0;
    Type* at = arg->node_type;
    const char* t = cg_internf("_ae_alt%d", alt_seq++);
    hoist_append_text(gen, cg_internf("%s = {0}; ", c_member_decl(at, t)));
    fprintf(gen->output, "(aether_unwind_track((const void*)%s, %s), %s(%s), ", t,
            own_fn(gen, at, "drop_unwind"), own_fn(gen, at, "drop"), t);
    emit_array_fill(gen, t, arg, at);
    fprintf(gen->output, ", %s)", t);
    push_own_drop_carrier(gen, t, at, 0, 0);
    return 1;
}

/* R3: the caller owns what it passes. A fresh optional or tuple argument is
 * a temporary of the statement, as an array literal is: taken, journaled,
 * and dropped when its block is done. */
static void emit_composite_arg_temp(CodeGenerator* gen, ASTNode* arg, Type* t) {
    static int cat_seq = 0;
    const char* tv = cg_internf("_ae_cat%d", cat_seq++);
    hoist_append_text(gen, cg_internf("%s %s = {0}; ", get_c_type(t), tv));
    if (t->kind == TYPE_PTR)
        fprintf(gen->output, "(aether_unwind_track((const void*)&%s, _aether_seq_drop_unwind), "
                             "string_seq_free(%s), %s = ", tv, tv, tv);
    else
        fprintf(gen->output, "(aether_unwind_track((const void*)&%s, %s), %s(&%s), %s = ", tv,
                own_fn(gen, t, "drop_unwind"), own_fn(gen, t, "drop"), tv, tv);
    emit_composite_take(gen, arg, t);
    fprintf(gen->output, ", %s)", tv);
    push_own_drop_carrier(gen, tv, t, 0, 0);
}

void generate_expression(CodeGenerator* gen, ASTNode* expr) {
    if (!expr) return;

    codegen_note_diag_pos(expr);

    /* #2894: a call returning an array, used as an array (indexed, passed,
     * thrown away), is held in a temporary of its result struct until its
     * statement is done; its block releases the elements on every exit, and
     * a statement run again releases the ones before. */
    if (expr->type == AST_FUNCTION_CALL && is_sized_array_param(expr->node_type) &&
        !gen->array_call_raw && hoist_ready(gen)) {
        static int act_seq = 0;
        Type* at = expr->node_type;
        int owns = own_type_owns(gen, at);
        const char* t = cg_internf("_ae_act%d", act_seq++);
        hoist_append_text(gen, cg_internf("%s %s = {0}; ", c_result_type(at), t));
        fprintf(gen->output, "(");
        if (owns)
            fprintf(gen->output, "aether_unwind_track((const void*)%s.a, %s), %s((%s*)%s.a), ", t,
                    own_fn(gen, at, "drop_unwind"), own_fn(gen, at, "drop"),
                    get_c_type(at->element_type), t);
        fprintf(gen->output, "%s = ", t);
        gen->array_call_raw++;
        generate_expression(gen, expr);
        gen->array_call_raw--;
        fprintf(gen->output, ", %s.a)", t);
        if (owns) push_own_drop_carrier(gen, cg_internf("%s.a", t), at, 0, 0);
        return;
    }

    /* #2478: an operand already evaluated into a temp, in source order. */
    if (g_order_count > 0) {
        const char* sub = order_lookup(expr);
        if (sub) {
            fprintf(gen->output, "%s", sub);
            return;
        }
    }
    /* #2765, #2798: a construct run ahead of its statement is its
     * temporary, before any wrap: the wrap's temporaries would hold what it
     * already evaluated, and go unused. */
    {
        const char* done = ctl_lookup(expr);
        if (done) {
            fprintf(gen->output, "%s", done);
            return;
        }
    }
    /* The call of fields read ahead and handed to it: once it is written,
     * an exit no longer frees them (order_handover_done). */
    if (g_handover_count > 0 && expr != g_handover_call_emitting && order_handover_pending(expr)) {
        const ASTNode* saved = g_handover_call_emitting;
        g_handover_call_emitting = expr;
        generate_expression(gen, expr);
        g_handover_call_emitting = saved;
        order_handover_done(gen, expr);
        return;
    }
    if (expr != g_order_emitting && emit_in_operand_order(gen, expr)) return;

    /* #2619: a string a call may hand back from a struct temporary is
     * copied here, so it outlives the temporary (call_returns_view_of_temp,
     * which is_heap_string_expr counts as a fresh heap string). Once: a call
     * an enclosing wrap hoisted was copied where the hoist evaluated it, and
     * copied again where it is passed, the second copy leaked. */
    if (expr->type == AST_FUNCTION_CALL && g_view_copy_wrapping != expr &&
        !arg_drain_lookup(expr) && call_returns_view_of_temp(gen, expr)) {
        const ASTNode* saved = g_view_copy_wrapping;
        g_view_copy_wrapping = expr;
        fprintf(gen->output, "aether_uniform_heap_str((const char*)(");
        generate_expression(gen, expr);
        fprintf(gen->output, "), 0)");
        g_view_copy_wrapping = saved;
        return;
    }
    /* #2619: the same for each string position of a tuple. */
    if (expr->type == AST_FUNCTION_CALL && g_view_copy_wrapping != expr &&
        call_returns_tuple_view_of_temp(gen, expr)) {
        static int vt_seq = 0;
        int id = vt_seq++;
        const ASTNode* saved = g_view_copy_wrapping;
        g_view_copy_wrapping = expr;
        Type* tt = expr->node_type;
        TempSeq s;
        seq_open(gen, &s);
        seq_let_named(gen, &s, get_c_type(tt), cg_internf("_ae_vt%d", id));
        generate_expression(gen, expr);
        for (int j = 0; j < tt->tuple_count; j++) {
            if (!tt->tuple_types[j] || tt->tuple_types[j]->kind != TYPE_STRING) continue;
            seq_next(gen, &s);
            fprintf(gen->output, "_ae_vt%d._%d = aether_uniform_heap_str((const char*)(_ae_vt%d._%d), %d)",
                    id, j, id, j, tuple_call_returns_heap_at(gen, expr, j) ? 1 : 0);
        }
        seq_next(gen, &s);
        fprintf(gen->output, "_ae_vt%d", id);
        seq_close(gen, &s);
        g_view_copy_wrapping = saved;
        return;
    }

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
    /* A closure cell's value held for a call (#2755) is an identifier, and
     * a string held for one (string_arg_held) is a place: an identifier, a
     * field or an element, which the call reads through its hold rather
     * than evaluating the place again. */
    if (expr->type == AST_FUNCTION_CALL ||
        expr->type == AST_STRING_INTERP ||
        expr->type == AST_CLOSURE ||
        expr->type == AST_OR_ELSE ||
        expr->type == AST_TUPLE_UNWRAP ||
        expr->type == AST_NULL_COALESCE ||
        ((expr->type == AST_IDENTIFIER || expr->type == AST_MEMBER_ACCESS ||
          expr->type == AST_ARRAY_ACCESS) && g_arg_drain_count > 0)) {
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
             * (enforced POD-only by the typechecker), calloc keeps every
             * scalar / ptr / array field a clean zero. Cast to T* so member
             * access (`p.field`) and `heap.free(p)` see the right type. */
            /* #1860: the struct name is on the node itself (the parser puts
             * it there), so use it whenever the inferred pointer type is not
             * available — which is exactly the `return heap.new(T)` case
             * outside main, where the node's type never got resolved. Falling
             * back to "void" emitted `calloc(1, sizeof(void))`: a ONE BYTE
             * allocation for the whole struct. Every field write then ran past
             * the end of it; the reported string leak was the visible symptom
             * of that. */
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
             * value. Single-eval through a temporary (#2765: see below for
             * the forms). */
            if (expr->child_count < 2) break;
            ASTNode* fallible = expr->children[0];
            ASTNode* handler = expr->children[1];
            Type* tup = fallible->node_type;
            if (!tup || tup->kind != TYPE_TUPLE || tup->tuple_count < 2 ||
                !tup->tuple_types[0]) {
                generate_expression(gen, fallible);   // malformed; already flagged
                break;
            }
            /* #2765: done ahead of its statement (emit_control_prelude). */
            const char* done = ctl_lookup(expr);
            if (done) {
                fprintf(gen->output, "%s", done);
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
             * return (both paths) hands the caller one reference
             * (emit_tuple_return_position), released here. When the error
             * position is NON-heap (e.g. `string.to_long`'s raw literal
             * "invalid long"), the slot is not the caller's and must NOT be
             * freed, so gate the free on the SAME static classification the
             * `v, e = f()` destructure site uses for `e`, keeping the two
             * forms consistent. */
            int err_slot_heap = or_fallible_error_slot_is_heap(gen, fallible);
            /* Uniform-heap boxing of the RESULT: when the success value
             * slot is a heap string, the success path yields a reference of
             * the caller's own, so the handler's value is taken through
             * `aether_uniform_heap_str(_, 0)` and BOTH paths hand over one;
             * then the AST_OR_ELSE case in is_heap_string_expr classifies
             * the whole expression heap and the caller owns it. Only for a
             * string result whose value slot is statically heap. */
            int box_val = (tup->tuple_types[0] &&
                           tup->tuple_types[0]->kind == TYPE_STRING &&
                           or_fallible_value_slot_is_heap(gen, fallible));
            if (handler->type != AST_BLOCK) {
                /* #2765: a handler that is an expression makes the whole
                 * `or` one: a sequence of temporaries, either path a
                 * conditional. A block handler holds statements (and may
                 * leave the function), so it stays a statement expression. */
                TempSeq s;
                seq_open(gen, &s);
                const char* oe = cg_internf("_oe%d", id);
                const char* oer = cg_internf("_oer%d", id);
                seq_let_named(gen, &s, tuple_c, oe);
                generate_expression(gen, fallible);
                seq_next(gen, &s);
                int hv_heap = is_heap_string_expr(gen, handler);
                seq_decl(gen, &s, val_c, oer);
                fprintf(gen->output, "(%s._%d && aether_string_data((const void*)%s._%d)[0]) ? (%s = ",
                        oe, err_idx, oe, err_idx, oer);
                if (box_val) fprintf(gen->output, "aether_uniform_heap_str((const char*)(");
                generate_expression(gen, handler);
                if (box_val) fprintf(gen->output, "), %d)", hv_heap ? 1 : 0);
                if (box_val) fprintf(gen->output, ", aether_heap_str_free((void*)%s._0)", oe);
                if (err_slot_heap)
                    fprintf(gen->output, ", aether_heap_str_free((void*)%s._%d)", oe, err_idx);
                fprintf(gen->output, ", (void)0) : (%s = %s._0", oer, oe);
                if (err_slot_heap)
                    fprintf(gen->output, ", aether_heap_str_free((void*)%s._%d)", oe, err_idx);
                fprintf(gen->output, ", (void)0)");
                seq_next(gen, &s);
                fprintf(gen->output, "%s", oer);
                seq_close(gen, &s);
                break;
            }
            /* #2765, #2798: as statements ahead of the statement
             * (emit_control_prelude), a statement expression where that
             * cannot be done. */
            int as_stmts = g_ctl_emitting == expr;
            if (as_stmts) {
                hoist_declare(gen, tuple_c, cg_internf("_oe%d", id));
                hoist_declare(gen, val_c, cg_internf("_oer%d", id));
                print_indent(gen);
                fprintf(gen->output, "_oe%d = ", id);
            } else {
                fprintf(gen->output, "({ %s _oe%d = ", tuple_c, id);
            }
            generate_expression(gen, fallible);
            /* "Is this an error" is "is the error slot non-empty", and the
             * slot holds either physical string shape. Indexing it raw reads
             * the AetherString magic header as content, so every refcounted
             * error slot looks non-empty whatever it says: `parse_strict`
             * returns an empty one on SUCCESS and `or` took the handler.
             * aether_string_data yields the payload for either shape. */
            if (as_stmts) fprintf(gen->output, ";\n");
            else fprintf(gen->output, "; %s _oer%d;\n", val_c, id);
            fprintf(gen->output, "if (_oe%d._%d && aether_string_data((const void*)_oe%d._%d)[0]) {\n",
                    id, err_idx, id, err_idx);
            /* The handler is a block (an expression is emitted above).
             * Block statements emit their own `#line` directives, which
             * must begin a line, hence the newlines bracketing them.
             *
             * The block's LAST statement is the handler's value: a bare
             * trailing expression (wrapped by the parser in an
             * AST_EXPRESSION_STATEMENT) is assigned to `_oer` so
             * `x = f() or { log(err) -1 }` yields -1 on the error path.
             * Before this, the trailing expression was emitted as a
             * discarded statement and `_oer` was read UNINITIALIZED: a
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
                 * (must NOT re-copy: that would leak the original);
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
                        "} else { _oer%d = _oe%d._0; aether_heap_str_free((void*)_oe%d._%d); }",
                        id, id, id, err_idx);
            } else {
                fprintf(gen->output, "} else { _oer%d = _oe%d._0; }", id, id);
            }
            if (as_stmts) {
                fprintf(gen->output, "\n");
                ctl_bind(expr, cg_internf("_oer%d", id));
            } else {
                fprintf(gen->output, " _oer%d; })", id);
            }
            break;
        }

        case AST_TUPLE_UNWRAP: {
            /* `expr!`, unwrap-or-trap. Evaluate the tuple once into a
             * temporary, panic if the trailing (string) error slot is
             * non-empty, and yield the first slot (#2765: a sequence of
             * temporaries, plain C):
             *
             *   (_u = <operand>,
             *    (_u._N && _u._N[0]) ? aether_panic(_u._N) : (void)0,
             *    _u._0)
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
                TempSeq s;
                seq_open(gen, &s);
                seq_let_named(gen, &s, get_c_type(ot), cg_internf("_fu%d", id));
                generate_expression(gen, operand);
                seq_next(gen, &s);
                fprintf(gen->output, "_fu%d.has ? (void)0 : aether_panic(\"forced_unwrap_none: forced unwrap of `none`\")",
                        id);
                seq_next(gen, &s);
                fprintf(gen->output, "_fu%d.val", id);
                seq_close(gen, &s);
                break;
            }
            Type* tup = operand->node_type;
            if (!tup || tup->kind != TYPE_TUPLE || tup->tuple_count < 2) {
                /* Typechecker already rejected this; emit the operand
                 * bare so codegen doesn't crash on a malformed tree. */
                generate_expression(gen, operand);
                break;
            }
            /* #2765: done ahead of its statement (emit_control_prelude). */
            const char* done = ctl_lookup(expr);
            if (done) {
                fprintf(gen->output, "%s", done);
                break;
            }
            ensure_tuple_typedef(gen, tup);
            const char* tuple_c = get_c_type(tup);
            int err_idx = tup->tuple_count - 1;
            static int unwrap_tmp_counter = 0;
            int uid = unwrap_tmp_counter++;
            /* The error slot is read through aether_string_data, as the `or`
             * reads it: a heap one is a counted AetherString whose first
             * byte is its header's, so an empty one read raw looked like an
             * error and `x!` panicked on success. On success a heap slot is
             * the caller's `""` to free, as the `or` frees it. */
            int unw_err_heap = or_fallible_error_slot_is_heap(gen, operand);

            if (!(gen->current_func_return_type && gen->current_func_return_type->is_result)) {
                /* #2765: unwrap or panic, a sequence of temporaries. The
                 * propagating form below returns from the function, a
                 * statement, so it stays a statement expression. */
                TempSeq s;
                seq_open(gen, &s);
                seq_let_named(gen, &s, tuple_c, cg_internf("_unw%d", uid));
                generate_expression(gen, operand);
                seq_next(gen, &s);
                /* A heap error string goes to the catcher with the panic
                 * (aether_panic_owned), as a heap-built message does. */
                fprintf(gen->output,
                        unw_err_heap
                            ? "(_unw%d._%d && aether_string_data((const void*)_unw%d._%d)[0]) ? aether_panic_owned(_unw%d._%d, aether_unwind_free_str) : "
                            : "(_unw%d._%d && aether_string_data((const void*)_unw%d._%d)[0]) ? aether_panic(_unw%d._%d) : ",
                        uid, err_idx, uid, err_idx, uid, err_idx);
                if (unw_err_heap)
                    fprintf(gen->output, "aether_heap_str_free((void*)_unw%d._%d)", uid, err_idx);
                else
                    fprintf(gen->output, "(void)0");
                seq_next(gen, &s);
                fprintf(gen->output, "_unw%d._0", uid);
                seq_close(gen, &s);
                break;
            }
            /* Propagating: as statements ahead of the statement
             * (emit_control_prelude), a statement expression where that
             * cannot be done (#2765, #2798). */
            int as_stmts = g_ctl_emitting == expr;
            if (as_stmts) {
                hoist_declare(gen, tuple_c, cg_internf("_unw%d", uid));
                print_indent(gen);
                fprintf(gen->output, "_unw%d = ", uid);
            } else {
                fprintf(gen->output, "({ %s _unw%d = ", tuple_c, uid);
            }
            generate_expression(gen, operand);
            fprintf(gen->output, as_stmts ? ";\n" : "; ");
            /* #913: `expr!` on a (value, err) result. In a function whose
             * return type is itself a result (`T!`), PROPAGATE — return the
             * enclosing result with the error slot set (value slot zero-init),
             * V-style one-char propagation (a non-result function unwraps or
             * panics, above). The `return` inside the statement-expression
             * returns from the enclosing function, which is exactly the
             * propagation we want. */
            /* The propagation `return` is a genuine function exit, so it
             * must run the same cleanup every other `return` site runs.
             * Before this it ran NONE of it: a `T!` function that
             * propagated an error skipped its user `defer`s AND the
             * synthetic RAII carriers the compiler pushes (heap-string /
             * *StringSeq / struct-destroy exit frees), leaking everything
             * the function was holding, silently, on the error path only.
             *
             * The defers are emitted INSIDE the statement-expression's
             * `if`, immediately before the return, which needs no
             * restructuring of the expression: they are ordinary
             * statements. The trailing newline before them is load-bearing:
             * a defer body carries `#line` directives, and a `#` is only a
             * preprocessor directive at the START of a line. Emitted
             * mid-line (right after `if (...) {`) it is a stray `#` and the
             * C compiler rejects the file. */
            fprintf(gen->output,
                    "if (_unw%d._%d && aether_string_data((const void*)_unw%d._%d)[0]) {\n",
                    uid, err_idx, uid, err_idx);
            /* #1140: propagation is unambiguously an ERROR exit: we are
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
             * ordinary return path does: a propagation is just as
             * non-local an exit as a `return`. */
            emit_try_pops_for_nonlocal_exit(gen);
            /* The error slot handed on is the enclosing function's to give:
             * when its error position is heap (function_def_returns_heap_at
             * counts this propagation), it is wrapped as every other return
             * there is, a literal the operand returned copied. The operand's
             * value slot, discarded here, is freed when it is a heap string,
             * as the `or` frees it. */
            {
                Type* crt = gen->current_func_return_type;
                int cerr = crt->tuple_count - 1;
                int cur_err_heap =
                    cerr > 0 && crt->tuple_types && crt->tuple_types[cerr] &&
                    crt->tuple_types[cerr]->kind == TYPE_STRING && gen->current_function &&
                    !gen->in_main_function &&
                    (gen->current_function->type == AST_FUNCTION_DEFINITION ||
                     gen->current_function->type == AST_BUILDER_FUNCTION) &&
                    function_def_returns_heap_at(gen, gen->current_function, cerr);
                if (tup->tuple_types[0] && tup->tuple_types[0]->kind == TYPE_STRING &&
                    or_fallible_value_slot_is_heap(gen, operand)) {
                    fprintf(gen->output, "\naether_heap_str_free((void*)_unw%d._0);", uid);
                }
                /* Leading newline for the same reason: a defer body can end
                 * mid-line, and the next thing must not be glued onto it. */
                if (cur_err_heap) {
                    fprintf(gen->output,
                            "\nreturn (%s){ ._1 = aether_uniform_heap_str((const char*)(_unw%d._%d), %d) }; } ",
                            get_c_type(crt), uid, err_idx, unw_err_heap ? 1 : 0);
                } else {
                    fprintf(gen->output,
                            "\nreturn (%s){ ._1 = _unw%d._%d }; } ",
                            get_c_type(crt), uid, err_idx);
                }
                if (unw_err_heap)
                    fprintf(gen->output, "aether_heap_str_free((void*)_unw%d._%d); ", uid, err_idx);
            }
            if (as_stmts) {
                fprintf(gen->output, "\n");
                ctl_bind(expr, cg_internf("_unw%d._0", uid));
            } else {
                fprintf(gen->output, "_unw%d._0; })", uid);
            }
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
            TempSeq s;
            seq_open(gen, &s);
            seq_let_named(gen, &s, get_c_type(ot), cg_internf("_nc%d", id));
            generate_expression(gen, operand);
            seq_next(gen, &s);
            fprintf(gen->output, "_nc%d.has ? _nc%d.val : (", id, id);
            if (is_owned_string_unwrap(gen, expr)) {
                /* #2750: over an owned `string?` call the value is the
                 * call's string, a fresh one, so the default is made one of
                 * its own too (a fresh default adopted, anything else
                 * copied): the expression is uniformly a heap string its
                 * consumer frees, as a fallible's `or` is. */
                ASTNode* dflt = expr->children[1];
                fprintf(gen->output, "aether_uniform_heap_str((const char*)(");
                generate_expression(gen, dflt);
                fprintf(gen->output, "), %d)", is_fresh_heap_string_temp(gen, dflt) ? 1 : 0);
            } else if (ot->element_type && ot->element_type->kind == TYPE_SUM &&
                       needs_sum_coerce(expr->children[1], ot->element_type)) {
                /* A variant default of a sum's optional is the sum's, as
                 * the value is: wrapped (#914), or the two arms of the
                 * conditional had no common type (#2839). */
                emit_sum_coerced(gen, expr->children[1], ot->element_type);
            } else {
                generate_expression(gen, expr->children[1]);
            }
            fprintf(gen->output, ")");
            seq_close(gen, &s);
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
            TempSeq s;
            seq_open(gen, &s);
            seq_let_named(gen, &s, get_c_type(ot), cg_internf("_oc%d", id));
            generate_expression(gen, operand);
            seq_next(gen, &s);
            fprintf(gen->output, "_oc%d.has ? (%s){ .has = 1, .val = _oc%d.val%s%s } : (%s){0}",
                    id, rc, id, acc, expr->value, rc);
            seq_close(gen, &s);
            break;
        }

        case AST_IF_EXPRESSION:
            // if cond { then } else { else } → C ternary: ((cond) ? (then) : (else))
            /* #2824: parenthesised whole. The source's parentheses are not
             * a node, and `?:` binds looser than any operator around it:
             * `y + (if c { 10 } else { 20 })` was `(y + c) ? 10 : 20`. */
            if (expr->child_count >= 3) {
                fprintf(gen->output, "((");
                generate_expression(gen, expr->children[0]);
                fprintf(gen->output, ") ? (");
                generate_expression(gen, expr->children[1]);
                fprintf(gen->output, ") : (");
                generate_expression(gen, expr->children[2]);
                fprintf(gen->output, "))");
            }
            break;

        case AST_SIZEOF:
            // sizeof(TypeName) → C sizeof(struct TypeName). Extern/struct
            // types emit as `struct <Name>` (same convention as
            // `as *StructName`), so the value always tracks the real C
            // layout.
            fprintf(gen->output, "((int)sizeof(%s %s))", aether_c_tag(expr->value), expr->value);
            break;

        case AST_SCHEMA_OF:
            // #2298: the body of a synthesized `T_schema()`: the address of
            // T's static field table (codegen_schema.c emits the getter).
            fprintf(gen->output, "((void*)_ae_schema_%s())", expr->value);
            break;

        case AST_OFFSETOF:
            // offsetof(TypeName, field) → C offsetof(struct TypeName, field).
            if (expr->child_count >= 1 && expr->children[0]->value) {
                fprintf(gen->output, "((int)offsetof(%s %s, %s))", aether_c_tag(expr->value),
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
            /* #error-unification P3: a fault member is its qualified name
             * (`"<ns>.<name>"`), so it prints and compares through the value
             * itself, across `.so`s too. Under R1 that is the name's pinned
             * string: one object per distinct name, which keeps a pointer
             * fast path for two uses of one fault. A local of the same name
             * is the local. */
            if (strmap_has(&gen->fault_members, expr->value) && !is_var_declared(gen, expr->value)) {
                emit_string_literal_node(gen, strmap_get(&gen->fault_members, expr->value));
                break;
            }
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
                    fprintf(gen->output, "((%s %s*)(", aether_c_tag(expr->value), expr->value);
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
             * long as the enclosing block, the same as the `T[N]` it is. The
             * empty literal `[]` has no elements to hold: like `null`, it is
             * the empty slice over no storage (#2767). */
            if (expr->child_count > 0) {
                ASTNode* src = expr->children[0];
                if (is_empty_slice_value(expr)) {
                    fprintf(gen->output, "aether_slice_make((void*)0, 0)");
                    break;
                }
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
                /* A scalar beside a four-lane (or two-lane) value splats, as
                 * the language reference says. C's vector extensions splat a
                 * scalar operand only when it converts to the element type
                 * without loss: a literal passes that test as a constant,
                 * but a `float` (C double) VARIABLE beside an f32x4 is
                 * rejected by clang and GCC alike ("implicit conversion would
                 * cause truncation"). Convert the scalar to the element type
                 * first; the typechecker has already decided the pairing is
                 * legal (it rejects a scalar of another kind). */
                TypeKind vk4 = TYPE_UNKNOWN;
                int scalar_side = -1;
                for (int k = 0; k < 2; k++) {
                    Type* vt = expr->children[k]->node_type;
                    Type* st = expr->children[1 - k]->node_type;
                    if (vt && st && lane_element_c_type(vt->kind) &&
                        is_scalar_splat_kind(st->kind)) {
                        vk4 = vt->kind;
                        scalar_side = 1 - k;
                        break;
                    }
                }
                if (lane8_fn && vk4 != TYPE_UNKNOWN) {
                    fprintf(gen->output, "(");
                    for (int k = 0; k < 2; k++) {
                        if (k) fprintf(gen->output, " %s ", expr->value);
                        if (k == scalar_side) {
                            fprintf(gen->output, "((%s)(", lane_element_c_type(vk4));
                            generate_expression(gen, expr->children[k]);
                            fprintf(gen->output, "))");
                        } else {
                            generate_expression(gen, expr->children[k]);
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
                // through a temporary so a side-effecting operand (e.g. a
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
                            TempSeq s;
                            seq_open(gen, &s);
                            const char* a = seq_let(gen, &s, "unsigned long long", "_ae_bsa");
                            fprintf(gen->output, "(");
                            generate_expression(gen, L);
                            fprintf(gen->output, ")");
                            seq_next(gen, &s);
                            const char* b = seq_let(gen, &s, "unsigned long long", "_ae_bsb");
                            fprintf(gen->output, "(");
                            generate_expression(gen, R);
                            fprintf(gen->output, ")");
                            seq_next(gen, &s);
                            fprintf(gen->output, "(%s & %s) == %s", a, b, subset ? a : b);
                            seq_close(gen, &s);
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
                        if (is_owned_opt_string_call(gen, opt)) {
                            /* #2750: the call's string is the comparison's
                             * to free once the flag is read. */
                            TempSeq s;
                            seq_open(gen, &s);
                            const char* on = seq_let(gen, &s, get_c_type(opt->node_type), "_ae_on");
                            generate_expression(gen, opt);
                            seq_next(gen, &s);
                            const char* onv = seq_let(gen, &s, "int", "_ae_onv");
                            fprintf(gen->output, "%s%s.has", is_eq ? "!" : "", on);
                            seq_next(gen, &s);
                            fprintf(gen->output, "%s.has ? aether_heap_str_free((void*)%s.val) : (void)0",
                                    on, on);
                            seq_next(gen, &s);
                            fprintf(gen->output, "%s", onv);
                            seq_close(gen, &s);
                            break;
                        }
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
                        TempSeq s;
                        fprintf(gen->output, "(");
                        seq_open(gen, &s);
                        const char* l = seq_let(gen, &s, get_c_type(L->node_type), "_ae_ol");
                        generate_expression(gen, L);
                        seq_next(gen, &s);
                        const char* r = seq_let(gen, &s, get_c_type(R->node_type), "_ae_or");
                        generate_expression(gen, R);
                        seq_next(gen, &s);
                        if (str_val) {
                            /* #2750: an owned `string?` call's string is
                             * freed once compared. */
                            const char* lrv = seq_let(gen, &s, "int", "_ae_olrv");
                            fprintf(gen->output,
                                "%s(%s.has == %s.has && (!%s.has || string_equals(%s.val, %s.val)))",
                                is_eq ? "" : "!", l, r, l, l, r);
                            if (is_owned_opt_string_call(gen, L)) {
                                seq_next(gen, &s);
                                fprintf(gen->output, "%s.has ? aether_heap_str_free((void*)%s.val) : (void)0",
                                        l, l);
                            }
                            if (is_owned_opt_string_call(gen, R)) {
                                seq_next(gen, &s);
                                fprintf(gen->output, "%s.has ? aether_heap_str_free((void*)%s.val) : (void)0",
                                        r, r);
                            }
                            seq_next(gen, &s);
                            fprintf(gen->output, "%s", lrv);
                        } else {
                            fprintf(gen->output, "%s(%s.has == %s.has && (!%s.has || %s.val == %s.val))",
                                    is_eq ? "" : "!", l, r, l, l, r);
                        }
                        seq_close(gen, &s);
                        fprintf(gen->output, ")");
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
                    /* An `or` over a heap value slot is one too: compared in
                     * place (`(f() or { "d" }) == "d"`), it leaked. */
                    int drain_l = is_fresh_heap_string_temp(gen, cl);
                    int drain_r = is_fresh_heap_string_temp(gen, cr);
                    if (drain_l || drain_r) {
                        if (!skip_parens) fprintf(gen->output, "(");
                        TempSeq s;
                        seq_open(gen, &s);
                        const char* sl = seq_let(gen, &s, "const char*", "_se_l");
                        fprintf(gen->output, "(const char*)(");
                        generate_expression(gen, cl);
                        fprintf(gen->output, ")");
                        seq_next(gen, &s);
                        const char* sr = seq_let(gen, &s, "const char*", "_se_r");
                        fprintf(gen->output, "(const char*)(");
                        generate_expression(gen, cr);
                        fprintf(gen->output, ")");
                        seq_next(gen, &s);
                        const char* sv = seq_let(gen, &s, "int", "_se_v");
                        emit_string_compare_open(gen, expr->value);
                        fprintf(gen->output, "%s, %s", sl, sr);
                        emit_string_compare_close(gen, expr->value);
                        if (drain_l) {
                            seq_next(gen, &s);
                            fprintf(gen->output, "aether_heap_str_free(%s)", sl);
                        }
                        if (drain_r) {
                            seq_next(gen, &s);
                            fprintf(gen->output, "aether_heap_str_free(%s)", sr);
                        }
                        seq_next(gen, &s);
                        fprintf(gen->output, "%s", sv);
                        seq_close(gen, &s);
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
                     * shaped value (is_fnptr=0), wrap the RHS in
                     * _aether_box_closure(...) so the closure
                     * round-trips through the field with the env
                     * slot intact. A bare named function is stored as
                     * its code address (#2890, emit_closure_into_ptr). */
                    int assign_box = 0;
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
                    if (is_assignment && lhs_is_ptr && !assign_c_fnptr_field)
                        assign_box = fn_value_into_ptr(gen, expr->children[1]);
                    if (assign_c_fnptr_field) {
                        fprintf(gen->output, "(__typeof__(");
                        generate_expression(gen, expr->children[0]);
                        fprintf(gen->output, "))");
                        generate_expression(gen, expr->children[1]);
                    } else if (assign_box) {
                        /* The box is a holder like a `fn` field (#2525): it
                         * keeps a reference of its own to the env, taken
                         * as emit_closure_take takes one (a fresh literal's
                         * is adopted, a view of a local or a parameter is
                         * retained, #2528). The caller releases its own
                         * reference after the call; boxed without one, that
                         * release freed the env the field still pointed at
                         * (aether-ui asks/aether-closure-drain-through-fn-store.md). */
                        emit_closure_into_ptr(gen, expr->children[1]);
                    } else if (is_assignment && rtype && rtype->kind == TYPE_STRING &&
                               expr->children[0]->type == AST_ARRAY_ACCESS &&
                               expr->children[0]->child_count > 0 &&
                               (expr->children[0]->children[0]->type == AST_PTR_AS_ARRAY_CAST ||
                                expr_is_c_view_of_slice(gen, expr->children[0]->children[0]))) {
                        /* R4: a `string[]` view of a `ptr` is C's memory, a
                         * `const char**` C reads (Vulkan's extension names):
                         * a string stored through it is its characters. */
                        emit_string_as_c_chars(gen, expr->children[1]);
                    } else if (is_assignment && ltype && ltype->kind == TYPE_OPTIONAL &&
                               expr->children[0]->type == AST_MEMBER_ACCESS) {
                        /* #2820: a store into an optional field takes the
                         * value as an optional binding does, a bare value
                         * wrapped and `none` the empty one. */
                        emit_optional_coerced(gen, expr->children[1], ltype);
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
            /* #2369: memory Aether code allocates with malloc starts zeroed,
             * as a heap.new box does. A struct built in it, however the
             * pointer later reaches its type (`malloc(n) as *T`, or a `ptr`
             * bound first and cast after), then has its fields read by a
             * field store, which releases the value it replaces; malloc left
             * them garbage (#1873). */
            if (call_is_c_malloc(gen, expr)) {
                fprintf(gen->output, "calloc(1, (size_t)(");
                generate_expression(gen, expr->children[0]);
                fprintf(gen->output, "))");
                break;
            }
            /* heap.free(p) — counterpart to heap.new(T) (issue #564, #790).
             * A POD box owns no heap fields, so a plain free(p) reclaims it.
             * A box whose struct has string fields (#790) routes through the
             * generated `_aether_heap_free_<Name>`, which releases every owned field
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
                    TempSeq s;
                    seq_open(gen, &s);
                    const char* hf = seq_let(gen, &s, "void*", "_ae_hf");
                    fprintf(gen->output, "(void*)(");
                    generate_expression(gen, arg);
                    fprintf(gen->output, ")");
                    seq_next(gen, &s);
                    fprintf(gen->output, "aether_unobserve_all(%s)", hf);
                    seq_next(gen, &s);
                    if (typed_free)
                        fprintf(gen->output, "_aether_heap_free_%s((%s*)%s)", typed_free, typed_free, hf);
                    else
                        fprintf(gen->output, "free(%s)", hf);
                    seq_close(gen, &s);
                    break;
                }
                if (typed_free) {
                    fprintf(gen->output, "_aether_heap_free_%s(", typed_free);
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
                    /* #2586: owned string arguments freed after the call. */
                    ArgDrainWrap fad;
                    fad.ret_ct = field_sig && field_sig->return_type
                                 ? get_c_type(field_sig->return_type) : NULL;
                    fad.ret_type = NULL;
                    fad.have_value = fad.ret_ct && strcmp(fad.ret_ct, "void") != 0;
                    fad.discarded = gen->discard_call_node == expr && !stmt_struct_temp_of(expr);
                    arg_drain_select(gen, expr, 0, NULL, NULL,
                                     !fad.have_value || fad.discarded, &fad);
                    arg_drain_open(gen, expr, &fad);
                    int took = emit_fnptr_take_open(gen, expr);
                    int narrow = gen->discard_call_node != expr && fnptr_returns_bool(field_sig);
                    if (narrow) fprintf(gen->output, "((_Bool)(unsigned char)(");
                    fprintf(gen->output, "(%s%s%s)(",
                            recv_c, is_ptr ? "->" : ".", field_c);
                    generate_fnptr_call_args(gen, field_sig, expr);
                    fprintf(gen->output, ")");
                    if (narrow) fprintf(gen->output, "))");
                    emit_fnptr_take_close(gen, took);
                    arg_drain_close(gen, expr, &fad);
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
                /* Is this the call its statement throws the value of away?
                 * Then the arg-temp drain below treats it as void-yielding,
                 * so heap inline args still free. See discard_call_node in
                 * codegen.h. */
                int ad_call_discarded = gen->discard_call_node == expr;
                /* ...unless the statement keeps it in a struct temporary to
                 * destroy (codegen_stmt.c, `_ae_stmpN = call`): that needs
                 * the value, so the wrap must yield `_ad_r`, not void. */
                if (ad_call_discarded && stmt_struct_temp_of(expr)) ad_call_discarded = 0;

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
                        emit_c_builtin_arg(gen, expr->children[i]);
                    }
                    fprintf(gen->output, ")");
                }
                else if (strcmp(func_name, "convert_type") == 0) {
                    fprintf(gen->output, "aether_convert_type(");
                    for (int i = 0; i < expr->child_count; i++) {
                        if (i > 0) fprintf(gen->output, ", ");
                        emit_c_builtin_arg(gen, expr->children[i]);
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
                                emit_string_literal_chars(gen, arg);
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
                                emit_string_literal_chars(gen, arg);
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
                            emit_string_literal_chars(gen, a);
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
                    emit_string_as_c_chars(gen, expr->children[0]);
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
                    emit_string_as_c_chars(gen, expr->children[0]);
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
                /* A string is released as `release` releases it (below): the
                 * place it is read from owns a counted string (R2). */
                else if (strcmp(func_name, "free") == 0 && expr->child_count == 1 &&
                         !(expr->children[0]->node_type &&
                           expr->children[0]->node_type->kind == TYPE_STRING)) {
                    fprintf(gen->output, "free((void*)");
                    generate_expression_as_elem_ptr(gen, expr->children[0]);   /* #1286 */
                    fprintf(gen->output, ")");
                }
                // release(X) — explicit release sugar for heap strings,
                // for `defer release(body)` after `body, err =
                // http.get(url)` without reaching for std.string.
                //
                // Lowerings, chosen by what the codegen knows about
                // the argument:
                //
                //  1. A string local (is_heap_string_var) releases its
                //     reference and is left empty, so its scope exit
                //     releases nothing.
                //
                //  2. A field read or an element releases that place's
                //     reference.
                //
                //  3. A literal / borrowed param / any other expression
                //     falls back to string_release, which is
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
                else if (expr->child_count == 1 && consuming_free_symbol(gen, func_name_norm) &&
                         expr->children[0]->type == AST_IDENTIFIER && expr->children[0]->value &&
                         !is_var_declared(gen, expr->children[0]->value) &&
                         is_module_global_var(gen, expr->children[0]->value) &&
                         expr->children[0]->node_type &&
                         expr->children[0]->node_type->kind == TYPE_STRING) {
                    /* R3: a module `var` owns its string as a local does:
                     * freed, it gives its reference back and is left empty,
                     * so the next store finds nothing to release. */
                    const char* g = expr->children[0]->value;
                    fprintf(gen->output, "(aether_heap_str_free((void*)%s), %s = NULL, (void)0)", g, g);
                }
                else if ((strcmp(func_name, "release") == 0 || strcmp(func_name, "free") == 0) &&
                         expr->child_count == 1) {
                    ASTNode* arg = expr->children[0];
                    if (arg->node_type && arg->node_type->kind == TYPE_STRING) {
                        if (arg->type == AST_IDENTIFIER && arg->value &&
                            is_heap_string_var(gen, arg->value)) {
                            /* A string local owns what it holds (R2): its
                             * reference is released and it is left empty. */
                            fprintf(gen->output, "(aether_heap_str_free((void*)%s), %s = NULL, (void)0)",
                                    arg->value, arg->value);
                        } else if (field_read_can_hand_off(arg)) {
                            emit_string_field_free(gen, arg);
                        } else if (string_element_free_kind(gen, arg)) {
                            emit_string_element_free(gen, arg);   /* #2618 */
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
                // extern). When X is a string local, it is lowered as
                // release() lowers one, so it is released once and its
                // scope exit releases nothing. Other arguments (literals,
                // borrowed params) fall through to the plain,
                // literal-safe string_release call.
                else if ((strcmp(func_name, "string_release") == 0 ||
                          strcmp(func_name, "string.release") == 0 ||
                          cfree_sym_is(gen, func_name_norm, "string_release")) &&
                         expr->child_count == 1 &&
                         expr->children[0]->type == AST_IDENTIFIER &&
                         expr->children[0]->value &&
                         is_heap_string_var(gen, expr->children[0]->value)) {
                    /* A string local: its reference released, the local
                     * left empty, so its scope exit releases nothing. */
                    ASTNode* arg = expr->children[0];
                    fprintf(gen->output, "(aether_heap_str_free((void*)%s), %s = NULL, (void)0)",
                            arg->value, arg->value);
                }
                // string.free(X) on a string local: as string.release(X).
                else if ((strcmp(func_name, "string_free") == 0 ||
                          strcmp(func_name, "string.free") == 0 ||
                          cfree_sym_is(gen, func_name_norm, "string_free")) &&
                         expr->child_count == 1 &&
                         expr->children[0]->type == AST_IDENTIFIER &&
                         expr->children[0]->value &&
                         is_heap_string_var(gen, expr->children[0]->value)) {
                    ASTNode* arg = expr->children[0];
                    fprintf(gen->output, "(aether_heap_str_free((void*)%s), %s = NULL, (void)0)",
                            arg->value, arg->value);
                }
                // The same for a struct's string field, which owns its value
                // as a tracked local does: freed here, it must not be freed
                // again by the next store into the field or by the struct's
                // destructor (emit_string_field_free).
                else if ((cfree_sym_is(gen, func_name_norm, "string_free") ||
                          cfree_sym_is(gen, func_name_norm, "string_release")) &&
                         expr->child_count == 1 &&
                         field_read_can_hand_off(expr->children[0])) {
                    emit_string_field_free(gen, expr->children[0]);
                }
                // #2618: and for an element of an array that owns its
                // elements, which the array would free again.
                else if ((cfree_sym_is(gen, func_name_norm, "string_free") ||
                          cfree_sym_is(gen, func_name_norm, "string_release")) &&
                         expr->child_count == 1 &&
                         string_element_free_kind(gen, expr->children[0])) {
                    emit_string_element_free(gen, expr->children[0]);
                }
                // string.seq_free(seq) on a place (a local, a field, an
                // element) releases what the place holds and leaves it
                // empty, so the place's own release finds nothing (R2).
                else if ((strcmp(func_name_norm, "string_seq_free") == 0 ||
                          strcmp(func_name, "string_seq_free") == 0) &&
                         expr->child_count == 1) {
                    ASTNode* arg = expr->children[0];
                    if (arg->type == AST_IDENTIFIER || arg->type == AST_MEMBER_ACCESS ||
                        arg->type == AST_ARRAY_ACCESS) {
                        fprintf(gen->output, "_aether_seq_free_place((StringSeq**)&(");
                        generate_expression(gen, arg);
                        fprintf(gen->output, "))");
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
                        /* The box holds a reference of its own. */
                        fprintf(gen->output, "_aether_box_closure(");
                        emit_closure_take(gen, expr->children[0]);
                        fprintf(gen->output, ")");
                    }
                }
                else if (strcmp(func_name, "release_closure") == 0 && expr->child_count == 1) {
                    fprintf(gen->output, "_aether_release_closure(");
                    generate_expression(gen, expr->children[0]);
                    fprintf(gen->output, ")");
                }
                // unbox_closure(ptr) — retrieve a closure from a heap pointer
                else if (strcmp(func_name, "unbox_closure") == 0 && expr->child_count == 1) {
                    fprintf(gen->output, "_aether_unbox_closure_owned(");
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
                    emit_c_builtin_arg(gen, expr->children[1]);
                    if (expr->child_count >= 3) {
                        fprintf(gen->output, ", ");
                        emit_c_builtin_arg(gen, expr->children[2]);
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
                        int lit_pi = 0;   /* the literal's parameter the argument binds */
                        for (int i = 1; i < expr->child_count; i++) {
                            ASTNode* arg = expr->children[i];
                            // Skip trailing-DSL-block closures (handled via
                            // _ctx injection, not passed as args). Regular
                            // closure-literal args are real arguments and
                            // must be forwarded.
                            if (arg && arg->type == AST_CLOSURE &&
                                arg->value && strcmp(arg->value, "trailing") == 0) continue;
                            fprintf(gen->output, ", ");
                            Type* lit_pt = NULL;
                            for (int pj = 0, seen = 0; literal && pj < literal->child_count; pj++) {
                                ASTNode* p = literal->children[pj];
                                if (!p || p->type != AST_CLOSURE_PARAM) continue;
                                if (seen++ == lit_pi) { lit_pt = p->node_type; break; }
                            }
                            lit_pi++;
                            emit_closure_argument(gen, arg, lit_pt);
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
                            ad.closure[ad.count] = 1;
                            ad.idx[ad.count++] = 0;
                        } else if (closure_arg && !arg_drain_lookup(closure_arg) && ad.count < 16 &&
                                   is_closure_cell_read(gen, closure_arg)) {
                            /* #2755: a closure a cell holds is held for its
                             * own call, which may rebind the cell and give
                             * back the cell's reference to it. */
                            ad.closure[ad.count] = 2;
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
                        int arg_pi = 0;
                        for (int i = 1; i < expr->child_count; i++) {
                            ASTNode* arg = expr->children[i];
                            if (arg && arg->type == AST_CLOSURE &&
                                arg->value && strcmp(arg->value, "trailing") == 0) continue;
                            fprintf(gen->output, ", ");
                            /* #2723: the parameter the cast above declares:
                             * the signature's, else the argument's own. */
                            Type* sig_pt = (closure_sig && arg_pi < closure_sig->param_count &&
                                            closure_sig->param_types &&
                                            closure_sig->param_types[arg_pi] &&
                                            closure_sig->param_types[arg_pi]->kind != TYPE_UNKNOWN)
                                           ? closure_sig->param_types[arg_pi] : NULL;
                            arg_pi++;
                            /* Coerced only into a parameter type the signature
                             * gives: with none, the argument's own type is no
                             * slot to wrap it for (an erased `fn`, #2840). */
                            if (sig_pt) emit_closure_argument(gen, arg, sig_pt);
                            else generate_sized_array_argument(gen, arg, arg ? arg->node_type : NULL);
                        }
                        fprintf(gen->output, ")");
                        arg_drain_close(gen, expr, &ad);
                    }
                }
                else {
                    const char* c_func_name = call_c_name(gen, func_name);
                    /* The extern table entry the call reaches, by its Aether
                     * name: what its parameters are looked up by (#2915). */
                    const char* ext_name = extern_name_for_call(gen, func_name);

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
                     * time. Before, the string lived forever in the
                     * list (list_free did not walk elements). */
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
                     * path. Decided by closure_container_store_value. */
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
                            /* R3: a container element owns its string. The
                             * value is taken (moved when fresh or a local's
                             * last use, else retained; a literal is pinned)
                             * and the container adopts that reference, which
                             * it releases when the element is replaced or
                             * removed or the container is freed. So does a
                             * counted string returned as `ptr` (`@heap`,
                             * `string.from_int`): the call hands it over. */
                            /* R5: a literal is pinned (R1), so a list
                             * adds it taking nothing and never releases
                             * it: the plain add of the pinned string, an
                             * element the list does not own. A set or a
                             * put replaces an element, so it keeps the
                             * owning entry. */
                            if (is_list_shape && !is_explicit_owned_list && val &&
                                val->type == AST_LITERAL && val->value && val->node_type &&
                                val->node_type->kind == TYPE_STRING) {
                                fprintf(gen->output, "%s((void*)(", c_func_name);
                                generate_expression(gen, expr->children[0]);
                                fprintf(gen->output, "), (void*)");
                                emit_string_literal_node(gen, val);
                                fprintf(gen->output, ")");
                                break;
                            }
                            if (val && val->node_type &&
                                (val->node_type->kind == TYPE_STRING ||
                                 (val->node_type->kind == TYPE_PTR && is_heap_string_expr(gen, val)))) {
                                TempSeq s;
                                seq_open(gen, &s);
                                const char* cc = seq_let(gen, &s, "void*", "_ae_cc");
                                fprintf(gen->output, "(void*)(");
                                generate_expression(gen, expr->children[0]);
                                fprintf(gen->output, ")");
                                seq_next(gen, &s);
                                const char* ck = NULL;
                                if (is_set_shape) {
                                    ck = seq_let(gen, &s, "int", "_ae_ci");
                                    fprintf(gen->output, "(");
                                    generate_expression(gen, expr->children[1]);
                                    fprintf(gen->output, ")");
                                    seq_next(gen, &s);
                                } else if (!is_list_shape) {
                                    /* The key as it is, an AetherString with
                                     * its length or a plain char*: the map
                                     * reads either shape (#2469). */
                                    ck = seq_let(gen, &s, "const char*", "_ae_ck");
                                    fprintf(gen->output, "(const char*)(");
                                    generate_expression(gen, expr->children[1]);
                                    fprintf(gen->output, ")");
                                    seq_next(gen, &s);
                                }
                                const char* cv = seq_let(gen, &s, "void*", "_ae_cv");
                                fprintf(gen->output, "(void*)");
                                emit_string_take(gen, val, NULL);
                                seq_next(gen, &s);
                                if (is_list_shape) {
                                    fprintf(gen->output, "%s(%s, %s)",
                                            is_wrapper ? "_aether_list_add_adopted" : "list_add_string_adopted",
                                            cc, cv);
                                } else if (is_set_shape) {
                                    fprintf(gen->output, "list_set_string_adopted(%s, %s, %s)", cc, ck, cv);
                                } else {
                                    fprintf(gen->output, "%s(%s, %s, %s)",
                                            is_wrapper ? "_aether_map_put_adopted" : "map_put_string_adopted",
                                            cc, ck, cv);
                                }
                                seq_close(gen, &s);
                                break;
                            }
                        }
                    }

                    /* Argument-temp lifetime wrap. Any heap-returning
                     * AST_FUNCTION_CALL appearing in argument position
                     * is an anonymous heap allocation with no consumer
                     * — pre-fix this is a leak per call. Hoist each
                     * such arg into a named temporary, run the parent
                     * call, then free the temps (the wrap, a sequence
                     * of temporaries). See ArgDrainSub at the top of
                     * this file for the full rationale.
                     *
                     * Skip when the parent's return type is unknown:
                     * the wrap holds the value in a temporary of its
                     * type (a void parent yields nothing, see below).
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
                         * returns a value and of what type (the rule
                         * generate_function emits the signature by), and
                         * the temp is declared with that type (#2765: not
                         * inferred, which only GCC-compatible compilers
                         * can). */
                        const char* rct = NULL;
                        int shape = callee_result_shape(gen, func_name, &rct);
                        if (shape > 0) {
                            ad.have_value = 1;
                            ad.ret_ct = rct;
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
                    /* R4: an `@heap` extern's string is adopted once, so
                     * the caller owns a counted string whatever C made. */
                    ASTNode* ext_decl = ext_name ? find_extern_declaration_by_name(gen->program, ext_name)
                                                 : NULL;
                    int adopt = ext_decl && ext_decl->annotation &&
                                annotation_has_marker(ext_decl->annotation, "heap_return") &&
                                expr->node_type && expr->node_type->kind == TYPE_STRING;
                    if (adopt) fprintf(gen->output, "aether_string_adopt(");
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
                                    /* Parameters only: a `where` contract on
                                     * one is an AST_REQUIRES_CLAUSE among
                                     * them (as are `requires`/`ensures`), and
                                     * counting it made `set_n(_ctx, n: int
                                     * where n >= 0)` look like three
                                     * parameters, so the bare `set_n(3)` in a
                                     * trailing block got no _ctx and gcc
                                     * reported too few arguments (#2706).
                                     * An extern's children are all
                                     * parameters, as its registry entry
                                     * counts them; fn_param_count knows
                                     * only a definition's parameter nodes
                                     * and would count none. */
                                    declared_params = fdef->type == AST_EXTERN_FUNCTION
                                        ? fdef->child_count
                                        : fn_param_count(fdef);
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
                        TypeKind expected = ext_name ? lookup_extern_param_kind(gen, ext_name, arg_printed)
                                                      : TYPE_UNKNOWN;
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
                        /* #2677: an extern's typed fn-pointer parameter
                         * (`fn(int)`) is a plain C function pointer: C has
                         * no closures, so a bare function passed to it goes
                         * in as its address, the C `as fn(int)` produces.
                         * Read from the extern's own parameter type, since
                         * `expected_type` is kept to user-function params
                         * (see the va_list note below). As a closure it
                         * named an adapter nothing emitted, and handed C an
                         * _AeClosure where it reads a function pointer. */
                        if (!expected_is_fnptr_form && expected == TYPE_FUNCTION && !expected_type) {
                            Type* ext_fn = ext_name ? lookup_extern_param_type(gen, ext_name, arg_printed) : NULL;
                            if (ext_fn && ext_fn->is_fnptr) expected_is_fnptr_form = 1;
                        }
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
                                tuple_param = ext_name ? lookup_extern_param_type(gen, ext_name, arg_printed)
                                                       : NULL;
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
                            ext_name ? lookup_extern_param_type(gen, ext_name, arg_printed) : NULL;
                        Type* fresh_t = arg_taken_by_call(gen, func_name, arg_printed)
                                            ? NULL
                                            : fresh_composite_arg_type(gen, arg,
                                                  ext_name ? extern_param_t : expected_type);
                        if (fresh_t) {
                            emit_composite_arg_temp(gen, arg, fresh_t);
                        } else if (array_literal_arg_temp(gen, arg)) {
                        } else
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
                             * R3, R4: a parameter that keeps it (`@keep`,
                             * an extern's `@retain`) is given a heap box
                             * holding a reference of its own. Any other
                             * reads the box only during the call, so it
                             * gets one on this stack frame (a compound
                             * literal of the enclosing block, #2523) and
                             * the closure's env stays its owner's: a
                             * literal's is released after the call by the
                             * drain, a local's at its scope end. */
                            if (call_param_keeps(gen, ext_name ? ext_name : func_name, arg_printed)) {
                                fprintf(gen->output, "_aether_box_closure(");
                                emit_closure_take(gen, arg);
                            } else {
                                fprintf(gen->output, "_aether_box_closure_in(&(_AeClosureBox){ .tag = 0 }, ");
                                generate_expression(gen, arg);
                            }
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
                            } else if (ext_name && is_retain_extern_param(gen, ext_name, arg_printed)) {
                                /* R4: an `@retain` extern keeps the closure,
                                 * so the call hands it a reference. */
                                emit_closure_take(gen, arg);
                            } else {
                                generate_expression(gen, arg);
                            }
                        } else if (ext_name && arg->node_type && arg->node_type->kind == TYPE_STRING &&
                                   (expected == TYPE_STRING || expected == TYPE_PTR) &&
                                   is_retain_extern_param(gen, ext_name, arg_printed) &&
                                   !call_frees_arg(gen, codegen_normalise_callee(ext_name)) &&
                                   arg->type != AST_LITERAL && !is_heap_string_expr(gen, arg)) {
                            /* R4: an `@retain` extern keeps the string, so the
                             * call hands it a reference of its own (a fresh
                             * one is handed over as it is). A free consumes
                             * the place's own reference instead. */
                            int header = is_aether_extern_param(gen, ext_name, arg_printed) ||
                                         is_stdlib_string_aware_extern(c_func_name);
                            fprintf(gen->output, "%s%saether_uniform_heap_str((const char*)(",
                                    expected == TYPE_PTR ? "(void*)" : "",
                                    header ? "(" : "aether_string_data(");
                            generate_expression(gen, arg);
                            fprintf(gen->output, "), 0))");
                        } else if (emit_int_ptr_bridged(gen, arg, expected)) {
                            /* int <-> ptr parameter: cast emitted. */
                        } else if (expected == TYPE_PTR && arg->node_type &&
                                   arg->node_type->kind == TYPE_STRING) {
                            /* A string-aware C function (string_*, aether_string_*)
                             * takes the string as it is held, as for a `string`
                             * parameter, and so does an `@aether` one. */
                            int header = ext_name && (is_aether_extern_param(gen, ext_name, arg_printed) ||
                                                      is_stdlib_string_aware_extern(c_func_name));
                            if (arg->type == AST_LITERAL && !header) {
                                /* A literal used as a `ptr` is its static C
                                 * characters, whoever receives it, uncast so a
                                 * printf-family format keeps its -Wformat
                                 * check (#1252); an Aether function's `ptr`
                                 * may pass it on to C (#1244). */
                                emit_string_literal_chars(gen, arg);
                            } else if (ext_name && !header) {
                                /* R4: a string handed to C as a `ptr` is its
                                 * characters, as a `string` parameter's is
                                 * (#297). */
                                fprintf(gen->output, "(void*)aether_string_data(");
                                generate_expression(gen, arg);
                                fprintf(gen->output, ")");
                            } else {
                                /* An Aether function's `ptr`, or an `@aether`
                                 * C parameter: the string as it is held. The
                                 * cast silences C's "discards qualifiers". */
                                fprintf(gen->output, "(void*)(");
                                generate_expression(gen, arg);
                                fprintf(gen->output, ")");
                            }
                        } else if (arg->node_type && !is_stdlib_string_aware_extern(c_func_name) &&
                                   ((ext_name && expected == TYPE_STRING &&
                                     (arg->node_type->kind == TYPE_STRING ||
                                      arg->node_type->kind == TYPE_PTR) &&
                                     !is_aether_extern_param(gen, ext_name, arg_printed)) ||
                                    (arg->node_type->kind == TYPE_STRING &&
                                     is_vararg_call_position(gen, func_name, arg_printed)))) {
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
                            // Closes #297. A string in a C-variadic `...`
                            // goes as its characters too, an extern's
                            // (`printf(fmt, s)`, #2914) or an Aether
                            // variadic function's, whose `va_arg` or
                            // forwarded `va_list` reads it as C does.
                            /* A literal goes as its C literal, which also
                             * keeps -Wformat reading a printf-family format
                             * (#1252). */
                            emit_string_as_c_chars(gen, arg);
                        } else if (expected == TYPE_STRING && !order_bound(arg) &&
                                   string_field_handoff_kind(gen, func_name, arg_printed, arg)) {
                            /* The callee frees what it is handed: the field
                             * gives its reference up (callee_consumes_string_arg). */
                            emit_string_field_handoff(gen, arg);
                        } else {
                            generate_sized_array_argument(gen, arg, expected_type);   /* #2723 */
                        }
                        arg_printed++;
                    }
                    // Defer functions get (void*)0 as last arg when called
                    // without trailing block, the config its block filled
                    // when emit_builder_call emits one with it.
                    if (is_builder_func_reg(gen, func_name)) {
                        if (arg_printed > 0) fprintf(gen->output, ", ");
                        fprintf(gen->output, "%s", expr == g_builder_cfg_call && g_builder_cfg_var
                                                       ? g_builder_cfg_var : "(void*)0");
                    }
                    fprintf(gen->output, adopt ? "))" : ")");
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
            /* Printed, the temps are a block's locals (print is a statement);
             * as a value they are a sequence's (#2765). */
            int it_hoisted = 0;
            TempSeq it_seq;
            for (int di = 0; di < expr->child_count; di++) {
                ASTNode* ch = expr->children[di];
                if (interp_segment_is_text(ch)) continue;
                int heap = interp_segment_is_heap_call(gen, ch);
                if (!heap && di > it_hoist_upto) continue;
                const char* ctype = interp_temp_c_type(ch->node_type);
                if (!ctype) continue;   /* no scalar/string temp shape: leave inline */
                /* R3: a string read from a place a later segment can store
                 * into is evaluated ahead holding a reference of its own,
                 * released once the interpolation has read it. */
                int held = !heap && string_reads_place(gen, ch);
                if (it_hoisted == 0) {
                    if (as_printf) fprintf(gen->output, "{ ");
                    else seq_open(gen, &it_seq);
                }
                it_hoisted++;
                char* nm = arg_drain_mint_name();
                if (as_printf) fprintf(gen->output, "%s %s = ", ctype, nm);
                else seq_let_named(gen, &it_seq, ctype, nm);
                fprintf(gen->output, held ? "(%s)aether_uniform_heap_str((const char*)(" : "(%s)(",
                        ctype);
                generate_expression(gen, ch);
                fprintf(gen->output, held ? "), 0)" : ")");
                if (as_printf) fprintf(gen->output, "; ");
                else seq_next(gen, &it_seq);
                arg_drain_bind(ch, nm);   /* registry owns nm; truncate frees it */
                it_names[di] = nm;
                it_heap[di] = heap || held;
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
                const char* it_r = NULL;
                if (it_hoisted > 0) {
                    it_r = seq_let(gen, &it_seq, "const char*", "_it_r");
                }
                fprintf(gen->output, "_aether_interp(\"");
                EMIT_INTERP_FMT();
                fprintf(gen->output, "\"");
                EMIT_INTERP_ARGS();
                fprintf(gen->output, ")");
                if (it_hoisted > 0) {
                    for (int di = 0; di < expr->child_count; di++) {
                        if (it_names[di] && it_heap[di]) {
                            seq_next(gen, &it_seq);
                            fprintf(gen->output, "aether_heap_str_free(%s)", it_names[di]);
                        }
                    }
                    seq_next(gen, &it_seq);
                    fprintf(gen->output, "%s", it_r);
                    seq_close(gen, &it_seq);
                }
            }
            if (it_hoisted > 0) {
                if (as_printf) {
                    fprintf(gen->output, "; ");
                    for (int di = 0; di < expr->child_count; di++) {
                        if (it_names[di] && it_heap[di]) {
                            fprintf(gen->output, "aether_heap_str_free(%s); ", it_names[di]);
                        }
                    }
                    fprintf(gen->output, "}");
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
                 * built right to left. An element built here (a fresh string)
                 * is released once its cell holds its own reference. */
                int fresh_elem = 0;
                for (int i = 0; i < expr->child_count; i++)
                    if (is_heap_string_expr(gen, expr->children[i])) fresh_elem = 1;
                if (expr->child_count >= 2 || fresh_elem) {
                    int id = g_seq_lit_counter++;
                    TempSeq sq;
                    seq_open(gen, &sq);
                    for (int i = 0; i < expr->child_count; i++) {
                        seq_let_named(gen, &sq, "const char*", cg_internf("_sqe%d_%d", id, i));
                        fprintf(gen->output, "(const char*)(");
                        generate_expression(gen, expr->children[i]);
                        fprintf(gen->output, ")");
                        seq_next(gen, &sq);
                    }
                    const char* sqa = cg_internf("_sqa%d", id);
                    const char* sqn = cg_internf("_sqn%d", id);
                    seq_let_named(gen, &sq, "StringSeq*", sqa);
                    fprintf(gen->output, "string_seq_empty()");
                    for (int i = expr->child_count - 1; i >= 0; i--) {
                        seq_next(gen, &sq);
                        if (i == expr->child_count - 1) seq_let_named(gen, &sq, "StringSeq*", sqn);
                        else fprintf(gen->output, "%s = ", sqn);
                        fprintf(gen->output, "string_seq_cons(_sqe%d_%d, %s)", id, i, sqa);
                        seq_next(gen, &sq);
                        fprintf(gen->output, "string_seq_free(%s)", sqa);
                        seq_next(gen, &sq);
                        fprintf(gen->output, "%s = %s", sqa, sqn);
                    }
                    for (int i = 0; i < expr->child_count; i++) {
                        if (!is_heap_string_expr(gen, expr->children[i])) continue;
                        seq_next(gen, &sq);
                        fprintf(gen->output, "aether_heap_str_free(_sqe%d_%d)", id, i);
                    }
                    seq_next(gen, &sq);
                    fprintf(gen->output, "%s", sqa);
                    seq_close(gen, &sq);
                    break;
                }
                /* 0 or 1 element, borrowed: nothing to drop. */
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
            /* R2: each field owns what it holds, so it takes its value: a
             * string as a store takes it (moved when fresh or a local's last
             * use, retained otherwise), a struct, closure or composite as
             * its own take does. A header-defined struct (`extern struct ...
             * @c_import`) owns nothing: its string fields are C's `const
             * char*`, the payload, and its literal is spelled `(struct
             * Name){...}` because the header need not typedef the tag. */
            int c_imported = aether_is_c_import_struct(expr->value);
            /* #2688: a fixed-size array field given another array is filled
             * once the literal is built (emit_struct_literal_array_field). */
            int any_array_field = 0;
            for (int i = 0; i < expr->child_count; i++) {
                ASTNode* fi = expr->children[i];
                if (fi && fi->type == AST_ASSIGNMENT &&
                    struct_literal_field_takes_array(gen, expr->value, fi)) any_array_field = 1;
            }
            TempSeq sl;
            const char* slit = NULL;
            if (any_array_field) {
                static int slit_seq = 0;
                seq_open(gen, &sl);
                slit = cg_internf("_ae_slit%d", slit_seq++);
                seq_let_named(gen, &sl, c_imported ? cg_internf("%s %s", aether_c_tag(expr->value),
                                                                expr->value)
                                                   : expr->value, slit);
            }
            if (c_imported) fprintf(gen->output, "(%s %s){", aether_c_tag(expr->value), expr->value);
            else fprintf(gen->output, "(%s){", expr->value);
            int emitted = 0;
            for (int i = 0; i < expr->child_count; i++) {
                ASTNode* field_init = expr->children[i];
                if (!field_init || field_init->type != AST_ASSIGNMENT) continue;
                if (emitted > 0) fprintf(gen->output, ", ");
                fprintf(gen->output, ".%s = ", field_init->value);
                emitted++;
                if (field_init->child_count == 0) continue;
                ASTNode* fv = field_init->children[0];
                Type* fdecl = c_imported ? NULL
                              : struct_literal_field_type(gen, expr->value, field_init->value);
                if (c_imported) {
                    /* NULL stays NULL, as in the field store, and C's field
                     * is given a reference of its own (R4). */
                    if (fv && fv->node_type && fv->node_type->kind == TYPE_STRING) {
                        TempSeq cs_seq;
                        seq_open(gen, &cs_seq);
                        const char* cs = seq_let(gen, &cs_seq, "const void*", "_ae_cs");
                        fprintf(gen->output, "(const void*)(");
                        emit_string_take_owned(gen, fv, 0);
                        fprintf(gen->output, ")");
                        seq_next(gen, &cs_seq);
                        fprintf(gen->output, "%s ? aether_string_data(%s) : (const char*)0", cs, cs);
                        seq_close(gen, &cs_seq);
                    } else {
                        generate_expression(gen, fv);
                    }
                    continue;
                }
                /* #2820: an optional field takes its value as an optional
                 * binding does, a bare value wrapped and `none` the empty
                 * one. */
                if (fdecl && fdecl->kind == TYPE_OPTIONAL && own_type_owns(gen, fdecl)) {
                    emit_composite_take(gen, fv, fdecl);   /* #2921 */
                } else if (fdecl && fdecl->kind == TYPE_OPTIONAL) {
                    emit_optional_coerced(gen, fv, fdecl);
                } else if (fdecl && fdecl->kind == TYPE_STRING && fv && fv->node_type &&
                           fv->node_type->kind == TYPE_STRING) {
                    emit_string_take_owned(gen, fv, 0);
                } else if (struct_literal_field_takes_array(gen, expr->value, field_init)) {
                    fprintf(gen->output, "{0}");   /* #2688: filled below */
                } else if (fdecl && fdecl->kind == TYPE_PTR && fn_value_into_ptr(gen, fv)) {
                    /* #2890: a `ptr` field takes a closure boxed, as `h.p = f`
                     * does, and a bare function as its address. */
                    emit_closure_into_ptr(gen, fv);
                } else if (fv && fv->type == AST_ARRAY_LITERAL && fdecl &&
                           fdecl->kind == TYPE_ARRAY && own_type_owns(gen, fdecl->element_type) &&
                           (fdecl->element_type->kind == TYPE_STRING ||
                            (fdecl->element_type->kind == TYPE_FUNCTION &&
                             !fdecl->element_type->is_fnptr))) {
                    /* #2528: a `string[N]` / `fn[N]` field owns every element:
                     * a literal's elements are copied or taken. */
                    emit_owned_array_literal(gen, fv, fdecl->element_type->kind == TYPE_STRING ? 1 : 2);
                } else if (struct_owning_strings(gen, fv ? fv->node_type : NULL) && struct_take_shape(fv)) {
                    /* #2497: a struct field held by value takes the struct. */
                    emit_struct_take(gen, fv, struct_owning_strings(gen, fv->node_type), NULL);
                } else if ((fdecl && fdecl->kind == TYPE_FUNCTION && !fdecl->is_fnptr) ||
                           (fv && fv->node_type && fv->node_type->kind == TYPE_FUNCTION &&
                            !fv->node_type->is_fnptr)) {
                    /* #2525: a closure field holds a reference of its own. */
                    emit_closure_take(gen, fv);
                } else if (fdecl && (fdecl->kind == TYPE_TUPLE || is_string_seq_ptr_type(fdecl)) &&
                           own_type_owns(gen, fdecl)) {
                    emit_composite_take(gen, fv, fdecl);   /* #2921 */
                } else {
                    generate_expression(gen, fv);
                }
            }
            fprintf(gen->output, "}");
            if (any_array_field) {
                for (int i = 0; i < expr->child_count; i++) {
                    ASTNode* fi = expr->children[i];
                    if (fi && fi->type == AST_ASSIGNMENT &&
                        struct_literal_field_takes_array(gen, expr->value, fi)) {
                        seq_next(gen, &sl);
                        emit_struct_literal_array_field(gen, expr->value, fi, slit);
                    }
                }
                seq_next(gen, &sl);
                fprintf(gen->output, "%s", slit);
                seq_close(gen, &sl);
            }
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
                                    /* #2890: a closure into a `ptr` field is
                                     * boxed, a bare function its address. */
                                    if (!(fk == TYPE_PTR && emit_closure_into_ptr(gen, val)))
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
                            emit_message_field_inits(gen, message, msg_def);
                            fprintf(gen->output, " }; ");
                            emit_message_array_fills(gen, message, "_msg", NULL);
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
                             * The receiver's _aether_msg_release_<Msg>
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
                                        "(void (*)(void*))_aether_msg_release_%s", reply_msg_name);
                                    break;
                                }
                            }
                        }

                        // Emit the ask expression with GCC/MSVC guards
                        fprintf(gen->output, "\n#if AETHER_GCC_COMPAT\n");
                        // GCC/Clang: statement expression
                        fprintf(gen->output, "({ %s _msg = { ._message_id = %d",
                                message->value, msg_def->message_id);

                        emit_message_field_inits(gen, message, msg_def);
                        fprintf(gen->output, " }; ");
                        emit_message_array_fills(gen, message, "_msg", NULL);

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
                                fprintf(gen->output, "if (_ask_r) { ((%s*)_ask_r)->%s = %s; _aether_msg_release_%s((%s*)_ask_r); } ",
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
                        if (message_has_array_fields(gen, message)) {
                            /* Its arrays are filled once it exists, so the
                             * message is a temporary, not a literal. */
                            TempSeq s;
                            fprintf(gen->output, ", ");
                            seq_open(gen, &s);
                            const char* mv = seq_let(gen, &s, message->value, "_ae_am");
                            fprintf(gen->output, "(%s){ ._message_id = %d", message->value,
                                    msg_def->message_id);
                            emit_message_field_inits(gen, message, msg_def);
                            fprintf(gen->output, " }");
                            emit_message_array_fills(gen, message, mv, &s);
                            seq_next(gen, &s);
                            fprintf(gen->output, "&%s", mv);
                            seq_close(gen, &s);
                        } else {
                            fprintf(gen->output, ", &(%s){ ._message_id = %d",
                                    message->value, msg_def->message_id);
                            emit_message_field_inits(gen, message, msg_def);
                            fprintf(gen->output, " }");
                        }
                        fprintf(gen->output, ", sizeof(%s), %d, ", message->value, timeout_ms);
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

        case AST_QUOTE: {
            /* docs/quote.md: build the tree, handing the builder each
             * captured variable's cell (the raw pointer, not its value). */
            QuoteInfo* q = quote_of(expr);
            if (!q) {
                fprintf(gen->output, "((void*)0)");
                break;
            }
            fprintf(gen->output, "_quote_build_%s(", expr->value);
            if (q->ncaps == 0) {
                fprintf(gen->output, "NULL");
            } else {
                fprintf(gen->output, "(void*[]){ ");
                for (int k = 0; k < q->ncaps; k++) {
                    ASTNode* id = create_ast_node(AST_IDENTIFIER, q->caps[k].name,
                                                  expr->line, expr->column);
                    codegen_own_node(gen, id);
                    if (k) fprintf(gen->output, ", ");
                    if (q->caps[k].global) {
                        /* A global's cell is the global itself. */
                        fprintf(gen->output, "(void*)&(");
                        generate_expression(gen, id);
                        fprintf(gen->output, ")");
                        continue;
                    }
                    id->annotation = strdup("raw_promoted");
                    fprintf(gen->output, "(void*)");
                    generate_expression(gen, id);
                }
                fprintf(gen->output, " }");
            }
            fprintf(gen->output, ")");
            break;
        }
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
                    const char* src = capture_source(gen, captures[i]);   /* #2890 */
                    if (capture_is_closure_value(gen, captures[i], cl_parent_func)) {
                        /* env owns a reference to the captured env (#2494) */
                        fprintf(gen->output, "_e->%s = %s; _aether_closure_env_retain(%s.env); ",
                                captures[i], src, src);
                    } else if (capture_owning_struct(gen, captures[i], cl_parent_func)) {
                        /* env owns a copy of the struct's strings (#2504) */
                        fprintf(gen->output, "_e->%s = _aether_copy_%s(%s); ", captures[i],
                                capture_owning_struct(gen, captures[i], cl_parent_func), src);
                    } else if (capture_is_promoted(gen, captures[i], cl_parent_func)) {
                        /* env owns a reference to the shared cell (#2019) */
                        const char* cell = promoted_cell_pointer(lookup_var_c_type(gen, captures[i], cl_parent_func),
                                                                 NULL);
                        fprintf(gen->output, "_e->%s = (%s)_aether_cell_retain(%s); ",
                                captures[i], cell, src);
                    } else if (capture_is_retained_string(gen, captures[i], cl_parent_func)) {
                        /* env owns a reference — see aether_str_capture preamble */
                        const char* ctype = lookup_var_c_type(gen, captures[i], cl_parent_func);
                        fprintf(gen->output, "_e->%s = (%s)aether_str_capture(%s); ",
                                captures[i], ctype, src);
                    } else if (capture_sized_array_type(gen, captures[i], cl_parent_func)) {
                        /* #2464: an array does not assign; copy its elements. */
                        Type* sat = capture_sized_array_type(gen, captures[i], cl_parent_func);
                        emit_capture_array_copy(gen, captures[i], src, sat);
                        if (capture_composite_type(gen, captures[i], cl_parent_func))
                            capture_take(gen, captures[i], sat);
                        fprintf(gen->output, " ");
                    } else {
                        fprintf(gen->output, "_e->%s = %s;", captures[i], src);
                        Type* ct = capture_composite_type(gen, captures[i], cl_parent_func);
                        if (ct) capture_take(gen, captures[i], ct);
                        fprintf(gen->output, " ");
                    }
                }
                fprintf(gen->output, "(_AeClosure){ .fn = (void(*)(void))_closure_fn_%d, .env = _e }; })", id);
                fprintf(gen->output, "\n#else\n");
                /* Without statement expressions the env is built by
                 * `_aether_make_env_<id>`, and the value is a compound
                 * literal around it, as above. A constructor returning the
                 * `_AeClosure` itself made the closure a call returning a
                 * struct through a hidden slot, and a volatile local a `try`
                 * body writes, used as that slot, lost the store: gcc 16 at
                 * -O1 inlined the call and dropped it, so after the catch
                 * the local still held what it held before the `try`
                 * (#2889). */
                fprintf(gen->output, "(_AeClosure){ .fn = (void(*)(void))_closure_fn_%d, .env = _aether_make_env_%d(",
                        id, id);
                for (int i = 0; i < cap_count; i++) {
                    if (i > 0) fprintf(gen->output, ", ");
                    fprintf(gen->output, "%s", capture_source(gen, captures[i]));
                }
                fprintf(gen->output, ") }");
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
