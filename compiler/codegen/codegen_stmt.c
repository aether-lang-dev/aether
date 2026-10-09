#include "codegen_internal.h"
#include "optimizer.h"
#include "../aether_module.h"
#include "../analysis/contract_eval.h"
#include "../analysis/typechecker.h"
#include "../analysis/hoist.h"
#include "../aether_error.h"
#include <limits.h>
#include <stdint.h>

// ============================================================================
// ARITHMETIC SERIES LOOP COLLAPSE
//
// Detects while loops of the form:
//   while counter < bound {              // or <=
//       acc1 = acc1 + invariant_expr1    // any number of accumulators
//       acc2 = acc2 + counter            // or counter * C: a linear sum
//       counter = counter + step         // a positive integer literal
//   }
//
// and replaces them with the closed form. With c0 the counter on entry and
// T the trip count,
//   T = ceil((bound - c0) / step)          for <
//   T = floor((bound - c0) / step) + 1     for <=
// the loop leaves
//   acc     = acc + invariant * T
//   acc     = acc + C * (T*c0 + step * T*(T-1)/2)
//             (T*(T+1)/2 when the accumulator follows the increment and so
//             adds the stepped value)
//   counter = c0 + step * T
//
// Works for any starting value and any bound expression, runtime ones
// included; a guard keeps the accumulators unchanged when the loop would
// not run at all.
//
// Only integer loops are collapsed: an `int` or `long` counter, bound,
// accumulators and addends. Those wrap (-fwrapv), and every step of the
// closed form is exact modulo 2^64, so the result is the loop's to the
// bit. A float series is left alone: repeated addition rounds at every step
// and the product does not. (Before #2271 a float loop went through
// (int64_t) and ended at its bound: `while a < 0.0 { a = a + 6.28 }` from
// -0.51 left 0.0, and an integer step that did not divide the distance was
// truncated the same way.)
//
// The one wrap the closed form cannot follow is the counter's own last
// step: when c0 + step*T passes the counter type's maximum, the loop wraps
// round and carries on. That is checked at run time, and the loop then
// runs as written.
// ============================================================================

#define MAX_SERIES_ACCUMULATORS 16

/* printf into a fresh heap string as long as the text needs, for the
 * defer annotations and memo keys built from names. A fixed buffer cut a
 * long name, and the cut text named a different variable or answered for
 * one that shares its prefix (#2539). The caller frees the result; out of
 * memory ends the compile, as aether_xrealloc does. */
static char* heap_strf(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
static char* heap_strf(const char* fmt, ...) {
    size_t cap = 128;
    for (;;) {
        char* s = (char*)aether_xrealloc(NULL, cap);
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(s, cap, fmt, ap);
        va_end(ap);
        if (n >= 0 && (size_t)n < cap) return s;
        free(s);
        /* A C library that answers -1 for a cut (old msvcrt) gets doubling. */
        cap = n >= 0 ? (size_t)n + 1 : cap * 2;
    }
}

/* #1301 allocation journal: emit the unwind-track call for a heap-
 * tracked LOCAL right after its `_heap_<name>` flag is armed. Uses the
 * SAME skip set as the function-exit defer push (escaped, return-
 * escaped, closure-env, promoted): the journal must mirror armed
 * deferred frees exactly. A var whose defer is never emitted must
 * never be journaled, or the panic drain frees a pointer someone else
 * owns. Emits ` aether_unwind_track_str_if(name, _heap_name);` with a
 * leading space and no newline so it composes inside `{ ... }`
 * wrapper emissions and after statement-level flag sets alike. */
static void emit_unwind_track_local(CodeGenerator* gen, const char* name) {
    if (!gen || !name) return;
    if (is_escaped_string_var(gen, name)) return;
    if (is_return_escaped_string_var(gen, name)) return;
    if (is_promoted_capture(gen, name)) return;
    for (int e = 0; e < gen->current_env_capture_count; e++) {
        if (gen->current_env_captures[e] &&
            strcmp(gen->current_env_captures[e], name) == 0) {
            return;
        }
    }
    fprintf(gen->output, " aether_unwind_track_str_if(%s, _heap_%s);", name, name);
}

/* #2333: can a `catch NAME` binding be a heap-tracked local? Not when
 * NAME is reached through something other than a plain C local in the
 * handler (a closure-env field, a promoted capture cell, actor state):
 * those keep the borrowed binding. */
static int catch_binding_can_own(CodeGenerator* gen, const char* name) {
    if (!gen || !name || strcmp(name, "_") == 0) return 0;
    if (is_promoted_capture(gen, name)) return 0;
    for (int e = 0; e < gen->current_env_capture_count; e++) {
        if (gen->current_env_captures[e] &&
            strcmp(gen->current_env_captures[e], name) == 0) return 0;
    }
    if (gen->current_actor) {
        for (int s = 0; s < gen->state_var_count; s++) {
            if (gen->actor_state_vars[s] &&
                strcmp(gen->actor_state_vars[s], name) == 0) return 0;
        }
    }
    return 1;
}

/* Drop `name` from a name set (its entries are strdup'd by the mark_*
 * helpers). */
static void remove_from_name_set(char** names, int* count, const char* name) {
    if (!names || !count || !name) return;
    for (int i = 0; i < *count; i++) {
        if (strcmp(names[i], name) == 0) {
            free(names[i]);
            names[i] = names[*count - 1];
            names[*count - 1] = NULL;
            (*count)--;
            return;
        }
    }
}

/* The catch branch of a try whose binding owns a heap-built reason.
 * Emits, inside the `else` of the setjmp test:
 *
 *   int _heap_NAME = _aether_try_N->reason_release != NULL;
 *   aether_try_pop();
 *   aether_unwind_track_str_if(NAME, _heap_NAME);   // outer frame's journal
 *   { handler ... ; deferred: if (_heap_NAME) aether_heap_str_free(NAME); }
 *
 * The binding is tracked only for the handler: the name is marked as a
 * heap string and run through the escape walk over the handler alone,
 * and the name's own marks are rolled back afterwards so an enclosing
 * variable of the same name keeps its own verdicts. (The walk re-marks
 * other names only as the function-level walk already did.) The C declarations shadow any
 * enclosing ones, and the free is a defer of the handler's own scope,
 * so it runs on fall-through, `break`/`continue`, and `return` alike. */
static void emit_owned_catch_handler(CodeGenerator* gen, ASTNode* catch_clause, int uid) {
    const char* name = catch_clause->value;
    ASTNode* handler = catch_clause->children[0];

    int was_heap = is_heap_string_var(gen, name);
    int was_escaped = is_escaped_string_var(gen, name);
    int was_ret_escaped = is_return_escaped_string_var(gen, name);
    int saved_var_count = gen->declared_var_count;

    print_line(gen, "int _heap_%s = _aether_try_%d->reason_release != NULL; (void)_heap_%s;",
               name, uid, name);
    print_line(gen, "aether_try_pop();");

    mark_heap_string_var(gen, name);
    mark_escaped_heap_string_vars(gen, handler);
    int escapes = is_escaped_string_var(gen, name) ||
                  is_return_escaped_string_var(gen, name);

    print_indent(gen);
    fprintf(gen->output, "/* catch %s owns a heap-built reason */", name);
    emit_unwind_track_local(gen, name);
    fprintf(gen->output, "\n");

    print_line(gen, "{");
    indent(gen);
    enter_scope(gen);
    if (!escapes) {
        ASTNode* carrier = create_ast_node(AST_EXPRESSION_STATEMENT, NULL,
                                           handler->line, handler->column);
        if (carrier) {
            if (carrier->annotation) free(carrier->annotation);
            carrier->annotation = heap_strf("heap_string_exit_free:%s", name);
            codegen_own_node(gen, carrier);
            push_defer(gen, carrier);
        }
    }
    generate_statement(gen, handler);
    exit_scope(gen);
    unindent(gen);
    print_line(gen, "}");
    truncate_declared_vars(gen, saved_var_count);

    if (!was_escaped)
        remove_from_name_set(gen->escaped_string_vars, &gen->escaped_string_var_count, name);
    if (!was_ret_escaped)
        remove_from_name_set(gen->return_escaped_string_vars,
                             &gen->return_escaped_string_var_count, name);
    if (!was_heap) unmark_heap_string_var(gen, name);
}

/* `panic(expr)`. A reason the program built at run time is handed to the
 * catcher to own (#2333): a heap-classified expression goes through
 * aether_panic_owned with the heap-string free, and a heap-tracked local
 * gives its buffer up -- journal entry and flag -- so neither the unwind
 * drain nor its scope-exit free reclaims what the catcher now holds.
 * Anything else (a literal, a borrowed string) is borrowed as before. */
static void emit_panic_call(CodeGenerator* gen, ASTNode* arg) {
    if (arg && arg->type == AST_IDENTIFIER && arg->value &&
        is_heap_string_var(gen, arg->value) && catch_binding_can_own(gen, arg->value)) {
        const char* v = arg->value;
        print_line(gen, "if (_heap_%s) { aether_unwind_forget(%s); _heap_%s = 0; "
                        "aether_panic_owned(%s, aether_unwind_free_str); }",
                   v, v, v, v);
        print_line(gen, "aether_panic(%s);", v);
        return;
    }
    print_indent(gen);
    if (arg && arg->type != AST_IDENTIFIER && is_heap_string_expr(gen, arg)) {
        fprintf(gen->output, "aether_panic_owned(");
        generate_expression(gen, arg);
        fprintf(gen->output, ", aether_unwind_free_str);\n");
        return;
    }
    fprintf(gen->output, "aether_panic(");
    generate_expression(gen, arg);
    fprintf(gen->output, ");\n");
}

// Returns 1 if the expression tree references the named variable.
static int expr_references_var(ASTNode* node, const char* var_name) {
    if (!node || !var_name) return 0;
    if (node->type == AST_IDENTIFIER && node->value &&
        strcmp(node->value, var_name) == 0) return 1;
    for (int i = 0; i < node->child_count; i++) {
        if (expr_references_var(node->children[i], var_name)) return 1;
    }
    return 0;
}

// Counts identifier-node occurrences of `var_name` in the subtree.
// Assignment/declaration targets are carried on `->value` of the
// statement node (not as identifier children), so this naturally
// counts reads/uses, not write targets.
static int count_var_identifier_uses(ASTNode* node, const char* var_name) {
    if (!node || !var_name) return 0;
    int n = (node->type == AST_IDENTIFIER && node->value &&
             strcmp(node->value, var_name) == 0) ? 1 : 0;
    for (int i = 0; i < node->child_count; i++) {
        n += count_var_identifier_uses(node->children[i], var_name);
    }
    return n;
}

// Decides whether `src_name` (the bare-identifier RHS of an alias
// assignment `dest = src`) is still live after the alias point, in
// which case the alias must NOT steal its buffer via an ownership
// move — it must take a defensive copy instead.
//
// The ownership move (`_heap_dest = _heap_src; _heap_src = 0`) is a
// last-use optimization: it transfers the single freeing duty to the
// alias and disowns the source. That is only sound when the source is
// dead after the alias. If the source is read again (e.g. `content`
// read after a `rest = content` alias-then-reassign loop), the move
// frees the source's buffer out from under it on the alias's next
// reassignment — silent heap corruption (see 180-regression.md).
//
// We approximate liveness conservatively: if the source identifier
// appears anywhere in the function besides the single alias-init use,
// assume it may be read later and prefer the copy. Worst case we copy
// when a move would have sufficed (a harmless extra allocation); we
// never wrongly move. With no function context, default to the safe
// copy. A struct literal's field takes a local by the same rule (#2602).
int alias_source_must_copy(CodeGenerator* gen, const char* src_name) {
    if (!gen || !gen->current_function || !src_name) return 1;
    return count_var_identifier_uses(gen->current_function, src_name) > 1;
}

/* #2461: taking a string into an owning slot (a local, a struct field, a
 * return value) from an expression that only VIEWS a buffer someone else
 * owns.
 *
 * A struct owns the strings in its fields (#465): it frees one when the
 * field is reassigned, when the struct is replaced, and when it goes out of
 * scope. A field read is therefore a view of the struct's buffer, and so is
 * an `if` or `match` whose chosen arm is a field read or a heap-tracked
 * local. The slots took such a value as a borrow (`_heap_<slot> = 0`) with
 * nothing keeping the buffer alive, so the value dangled as soon as the
 * source let go of it. A bare alias (`dest = src`) never had the problem:
 * it moves the source's ownership or takes a copy (alias_source_must_copy).
 *
 * The slot now takes such a value the way an alias is taken, arm by arm: a
 * field read is copied (the struct keeps its own buffer, and nothing can
 * tell when it lets go of it), a heap-tracked local is moved or copied as
 * an alias of it would be, a fresh heap value is adopted, anything else
 * (a literal, a parameter) is borrowed as before. Which arm runs is only
 * known at run time, so the take sets an ownership flag the slot's
 * `_heap_` tracker is then set from. */

/* A read of a `string` field of an Aether struct, by value or through a
 * pointer. A header-defined struct's field is the C header's `const char*`,
 * which the struct borrows (it has no `_heap_<field>` tracker), so reading
 * it views nothing the struct can free. */
int is_owned_string_field_read(ASTNode* e) {
    if (!e || e->type != AST_MEMBER_ACCESS || !e->value ||
        e->child_count < 1 || !e->children[0]) return 0;
    if (!e->node_type || e->node_type->kind != TYPE_STRING) return 0;
    Type* ot = e->children[0]->node_type;
    const char* sname = NULL;
    if (ot && ot->kind == TYPE_STRUCT) {
        sname = ot->struct_name;
    } else if (ot && ot->kind == TYPE_PTR && ot->element_type &&
               ot->element_type->kind == TYPE_STRUCT) {
        sname = ot->element_type->struct_name;
    }
    return sname && !aether_is_c_import_struct(sname);
}

/* The type a match expression's arms yield: the first value an arm yields
 * (match_arm_value, so a block arm's final expression counts, #2496). */
static Type* match_value_type(ASTNode* m) {
    for (int i = 1; m && i < m->child_count; i++) {
        ASTNode* arm = m->children[i];
        if (!arm || arm->type != AST_MATCH_ARM || arm->child_count < 2) continue;
        ASTNode* v = match_arm_value(arm->children[1]);
        if (v && v->node_type && v->node_type->kind != TYPE_UNKNOWN) return v->node_type;
    }
    return NULL;
}

static int string_take_join(int a, int b) {
    return a == b ? a : STR_TAKE_RUNTIME;
}

static int is_cell_string_element(CodeGenerator* gen, ASTNode* e);
static ASTNode* handback_leaf_node(CodeGenerator* gen, ASTNode* expr, int depth);
static ASTNode* handback_take_leaf(CodeGenerator* gen, ASTNode* e);

static int is_env_capture_name(CodeGenerator* gen, const char* name);

/* Is `name` a capture the closure being emitted only reads, bound at its
 * entry from the env (`const char* s = _env->s;`)? */
static int is_alias_capture_name(CodeGenerator* gen, const char* name) {
    for (int i = 0; name && i < gen->current_alias_capture_count; i++) {
        if (gen->current_alias_captures[i] &&
            strcmp(gen->current_alias_captures[i], name) == 0) return 1;
    }
    return 0;
}

/* A string a closure reaches through its environment or a shared cell: the
 * env or the cell holds it and frees it with itself, so an owning slot
 * (a struct field, above all) takes a copy (emit_string_take), not the
 * pointer (#2574). */
static int is_captured_string(CodeGenerator* gen, ASTNode* e) {
    return e && e->type == AST_IDENTIFIER && e->value && e->node_type &&
           e->node_type->kind == TYPE_STRING &&
           (is_promoted_capture(gen, e->value) || is_env_capture_name(gen, e->value) ||
            is_alias_capture_name(gen, e->value));
}

int string_take_kind(CodeGenerator* gen, ASTNode* e) {
    if (!e) return STR_TAKE_BORROW;
    if (e->type == AST_IF_EXPRESSION && e->child_count >= 3) {
        return string_take_join(string_take_kind(gen, e->children[1]),
                                string_take_kind(gen, e->children[2]));
    }
    if (e->type == AST_MATCH_STATEMENT) {
        int k = -1;
        for (int i = 1; i < e->child_count; i++) {
            ASTNode* arm = e->children[i];
            if (!arm || arm->type != AST_MATCH_ARM || arm->child_count < 2) continue;
            ASTNode* value = match_arm_value(arm->children[1]);
            int ak = value ? string_take_kind(gen, value) : STR_TAKE_BORROW;
            k = k < 0 ? ak : string_take_join(k, ak);
        }
        return k < 0 ? STR_TAKE_BORROW : k;
    }
    if (is_owned_string_field_read(e) || is_cell_string_element(gen, e)) return STR_TAKE_OWNED;
    if (is_captured_string(gen, e)) return STR_TAKE_OWNED;
    if (e->type == AST_IDENTIFIER) {
        return (e->value && is_heap_string_var(gen, e->value))
               ? STR_TAKE_RUNTIME : STR_TAKE_BORROW;
    }
    if (handback_take_leaf(gen, e)) return STR_TAKE_RUNTIME;
    return is_heap_string_expr(gen, e) ? STR_TAKE_OWNED : STR_TAKE_BORROW;
}

/* Is `e` a view an owning slot must take with emit_string_take rather than
 * by its classic paths (fresh heap value adopted, bare alias moved/copied,
 * anything else borrowed)? A field read or an element of a string array a
 * closure writes (#2474), a call that only hands a heap-tracked local back
 * (#2548), or an `if` with an arm that is not a plain borrow. A `match`
 * binds through its own result variable, which the match binding routes
 * separately. */
int string_take_is_view(CodeGenerator* gen, ASTNode* e) {
    if (!e) return 0;
    if (is_owned_string_field_read(e) || is_cell_string_element(gen, e)) return 1;
    if (is_captured_string(gen, e)) return 1;
    if (handback_take_leaf(gen, e)) return 1;
    return e->type == AST_IF_EXPRESSION &&
           string_take_kind(gen, e) != STR_TAKE_BORROW;
}

/* A fresh name for the ownership flag of one take. Takes nest (an arm may
 * hold a struct literal that takes a field), so the name must not repeat. */
void string_take_new_flag(char* buf, size_t n) {
    static int take_flag_seq = 0;
    snprintf(buf, n, "_ae_own%d", take_flag_seq++);
}

/* Is `name` a capture the closure being emitted reaches through `_env->`? */
static int is_env_capture_name(CodeGenerator* gen, const char* name) {
    for (int e = 0; name && e < gen->current_env_capture_count; e++) {
        if (gen->current_env_captures[e] &&
            strcmp(gen->current_env_captures[e], name) == 0) return 1;
    }
    return 0;
}

/* Emit `e` as the value an owning slot takes, setting the C int `own` to 1
 * when the slot receives a buffer it must free and to 0 when it borrows.
 * `target` is the slot's own name when it is a local, so `w = if c { w }
 * else { ... }` keeps w's buffer rather than freeing what it still holds. */
void emit_string_take(CodeGenerator* gen, ASTNode* e, const char* own,
                      const char* target) {
    if (e && e->type == AST_IF_EXPRESSION && e->child_count >= 3) {
        fprintf(gen->output, "((");
        generate_expression(gen, e->children[0]);
        fprintf(gen->output, ") ? ");
        emit_string_take(gen, e->children[1], own, target);
        fprintf(gen->output, " : ");
        emit_string_take(gen, e->children[2], own, target);
        fprintf(gen->output, ")");
        return;
    }
    if (is_owned_string_field_read(e) || is_cell_string_element(gen, e)) {
        fprintf(gen->output, "(%s = 1, aether_uniform_heap_str((const char*)(", own);
        generate_expression(gen, e);
        fprintf(gen->output, "), 0))");
        return;
    }
    if (e && e->type == AST_IDENTIFIER && e->value &&
        (is_promoted_capture(gen, e->value) || is_env_capture_name(gen, e->value) ||
         is_alias_capture_name(gen, e->value))) {
        /* A closure's shared cell, or its env, frees the string it holds
         * whatever the tracker says, so its value cannot be moved out:
         * copy it. */
        fprintf(gen->output, "(%s = 1, aether_uniform_heap_str((const char*)(", own);
        generate_expression(gen, e);
        fprintf(gen->output, "), 0))");
        return;
    }
    ASTNode* hb = handback_take_leaf(gen, e);
    if (hb) {
        /* #2548: a call that only hands a heap-tracked local back yields
         * the local's buffer, or on another path something the local does
         * not own (`temp_prefix` returns "ae" for an empty prefix). The
         * slot takes the local as an alias of it would when the pointer is
         * the local's, and borrows anything else. Before, the alias
         * borrowed, the local was marked escaped, and the buffer leaked. */
        const char* v = hb->value;
        int self = target && strcmp(v, target) == 0;
        fprintf(gen->output, "({ const char* %s_v = (const char*)(", own);
        generate_expression(gen, e);
        fprintf(gen->output, "); %s = %s_v == (const char*)(", own, own);
        generate_expression(gen, hb);
        fprintf(gen->output, ") && _heap_%s; ", v);
        if (!self && alias_source_must_copy(gen, v)) {
            fprintf(gen->output, "%s ? aether_uniform_heap_str(%s_v, 0) : %s_v; })",
                    own, own, own);
        } else {
            fprintf(gen->output, "if (%s) _heap_%s = 0; %s_v; })", own, v, own);
        }
        return;
    }
    if (e && e->type == AST_IDENTIFIER && e->value && is_heap_string_var(gen, e->value)) {
        const char* v = e->value;
        int self = target && strcmp(v, target) == 0;
        if (!self && alias_source_must_copy(gen, v)) {
            /* The source is read again: copy what it owns, borrow what it
             * borrows (the same live-source guard as a bare alias). */
            fprintf(gen->output, "(%s = _heap_%s, %s ? aether_uniform_heap_str((const char*)(",
                    own, v, own);
            generate_expression(gen, e);
            fprintf(gen->output, "), 0) : (const char*)(");
            generate_expression(gen, e);
            fprintf(gen->output, "))");
        } else {
            /* Last use (or the slot itself): move the ownership. */
            fprintf(gen->output, "(%s = _heap_%s, _heap_%s = 0, (const char*)(", own, v, v);
            generate_expression(gen, e);
            fprintf(gen->output, "))");
        }
        return;
    }
    fprintf(gen->output, "(%s = %d, (const char*)(", own,
            is_heap_string_expr(gen, e) ? 1 : 0);
    generate_expression(gen, e);
    fprintf(gen->output, "))");
}

/* Emit `e` taken as a value the receiver always owns: the take, then a copy
 * of whatever it left borrowed. For a slot with no ownership flag of its
 * own: a return value (the uniform-heap contract), or, with `cell`, a
 * closure's string cell, which frees only the refcounted strings it holds
 * (a literal is stored as it is), so a plain owned buffer (an `@heap`
 * extern's strdup) is turned into one on the way in (_aether_str_cell_own);
 * passed through, nothing freed it. */
static void emit_string_take_owned(CodeGenerator* gen, ASTNode* e, int cell) {
    char own[32];
    string_take_new_flag(own, sizeof(own));
    fprintf(gen->output, "({ int %s = 0; const char* _ae_tv = ", own);
    emit_string_take(gen, e, own, NULL);
    fprintf(gen->output, "; %s(_ae_tv, %s); })",
            cell ? "_aether_str_cell_own" : "aether_uniform_heap_str", own);
}

/* `name = <view>` for a heap-tracked string local: the reassignment wrapper
 * (free the previous value it owned, record what the new one is), with the
 * value taken by emit_string_take. An escaped local's old value may be held
 * by whoever it escaped to, so it is not freed (the escape gate). */
static void emit_string_take_rebind(CodeGenerator* gen, const char* name,
                                    ASTNode* rhs) {
    char own[32];
    string_take_new_flag(own, sizeof(own));
    if (is_escaped_string_var(gen, name)) {
        fprintf(gen->output, "{ int %s = 0; %s = ", own, name);
        emit_string_take(gen, rhs, own, name);
        fprintf(gen->output, "; _heap_%s = %s; }\n", name, own);
        return;
    }
    fprintf(gen->output, "{ const char* _tmp_old = %s; int %s = 0; %s = ", name, own, name);
    emit_string_take(gen, rhs, own, name);
    fprintf(gen->output, "; if (_heap_%s) aether_heap_str_free(_tmp_old); _heap_%s = %s;",
            name, name, own);
    emit_unwind_track_local(gen, name);
    fprintf(gen->output, " }\n");
}


/* Bit width of an integer the series collapse is exact for: Aether's
 * wrapping `int` (C int) and `long` (int64_t); 0 for anything else. */
static int series_int_width(const ASTNode* n) {
    if (!n || !n->node_type) return 0;
    if (n->node_type->kind == TYPE_INT) return 32;
    if (n->node_type->kind == TYPE_INT64) return 64;
    return 0;
}

/* The value of a literal written in plain decimal digits; 0 when the literal
 * is anything else (a float, another base, a separator) or does not fit. */
static int series_decimal_literal(const ASTNode* n, unsigned long long* out) {
    if (!n || n->type != AST_LITERAL || !n->value || !n->value[0]) return 0;
    unsigned long long v = 0;
    for (const char* c = n->value; *c; c++) {
        if (*c < '0' || *c > '9') return 0;
        unsigned d = (unsigned)(*c - '0');
        if (v > (ULLONG_MAX - d) / 10) return 0;
        v = v * 10 + d;
    }
    *out = v;
    return 1;
}

/* A counter or accumulator of the series, read or written: the identifier as
 * the rest of codegen spells it, so an actor's state field is `self->name`, a
 * promoted capture its cell and a closure's capture its env slot. Printing
 * the bare name wrote a local that did not exist (#2505). */
static void series_var(CodeGenerator* gen, ASTNode* id) {
    generate_expression(gen, id);
}

// Try to detect and emit a collapsed arithmetic series loop.
// Returns 1 if the loop was collapsed and emitted; 0 otherwise (caller emits normally).
static int try_emit_series_collapse(CodeGenerator* gen, ASTNode* while_node) {
    if (!while_node || while_node->child_count < 2) return 0;
    if (gen->series_collapse_off) return 0;

    ASTNode* condition = while_node->children[0];
    ASTNode* body      = while_node->children[1];

    // 1. Condition must be "counter < bound" or "counter <= bound"
    if (!condition || condition->type != AST_BINARY_EXPRESSION || !condition->value) return 0;
    int is_lt  = strcmp(condition->value, "<")  == 0;
    int is_lte = strcmp(condition->value, "<=") == 0;
    if (!is_lt && !is_lte) return 0;
    if (condition->child_count < 2) return 0;

    ASTNode* cond_left  = condition->children[0];   // the counter
    ASTNode* cond_right = condition->children[1];   // the bound

    if (!cond_left || cond_left->type != AST_IDENTIFIER || !cond_left->value) return 0;
    const char* counter_var = cond_left->value;

    // Integer counter and bound only: the closed form is exact for nothing else.
    int counter_width = series_int_width(cond_left);
    if (!counter_width || !series_int_width(cond_right)) return 0;

    // Bound must not have side effects
    if (codegen_expr_has_side_effects(cond_right)) return 0;

    // 2. Body: get statement list
    ASTNode** stmts;
    int stmt_count;
    if (!body) return 0;
    if (body->type == AST_BLOCK && body->child_count == 1 &&
        body->children[0] && body->children[0]->type == AST_BLOCK) {
        body = body->children[0];
    }
    if (body->type == AST_BLOCK) {
        stmts      = body->children;
        stmt_count = body->child_count;
    } else {
        stmts      = &body;
        stmt_count = 1;
    }
    if (stmt_count == 0) return 0;

    // 3. Parse each statement
    ASTNode*           acc_ids[MAX_SERIES_ACCUMULATORS];        // its identifier node
    ASTNode*           acc_addends[MAX_SERIES_ACCUMULATORS];
    int                acc_width[MAX_SERIES_ACCUMULATORS];
    int                acc_is_linear[MAX_SERIES_ACCUMULATORS];  // addend is counter or counter*C
    unsigned long long acc_scale[MAX_SERIES_ACCUMULATORS];      // C for a linear addend
    int                acc_stmt[MAX_SERIES_ACCUMULATORS];       // position in the body
    int                acc_count   = 0;
    int                counter_idx = -1;
    unsigned long long counter_step = 0;

    // Also collect the set of target variable names for later checks.
    const char* stmt_targets[MAX_SERIES_ACCUMULATORS + 1];  // +1 for counter
    int stmt_target_count = 0;

    for (int i = 0; i < stmt_count; i++) {
        ASTNode* s = stmts[i];
        if (!s) return 0;

        // Every statement must be an assignment of the form: target = target + expr
        // The parser emits AST_VARIABLE_DECLARATION for all "x = expr" statements:
        //   s->value      = target variable name
        //   s->children[0] = RHS expression
        if (s->type != AST_VARIABLE_DECLARATION) return 0;
        if (!s->value || s->child_count < 1) return 0;

        const char* target = s->value;
        ASTNode*    rhs    = s->children[0];

        if (!rhs || rhs->type != AST_BINARY_EXPRESSION) return 0;
        if (!rhs->value || strcmp(rhs->value, "+") != 0) return 0;
        if (rhs->child_count < 2) return 0;

        ASTNode* rhs_left  = rhs->children[0];
        ASTNode* rhs_right = rhs->children[1];

        // Identify the "self" side and the "addend" side
        int left_is_self  = rhs_left  && rhs_left->type  == AST_IDENTIFIER &&
                            rhs_left->value  && strcmp(rhs_left->value,  target) == 0;
        int right_is_self = rhs_right && rhs_right->type == AST_IDENTIFIER &&
                            rhs_right->value && strcmp(rhs_right->value, target) == 0;
        if (!left_is_self && !right_is_self) return 0;

        ASTNode* self   = left_is_self ? rhs_left : rhs_right;
        ASTNode* addend = left_is_self ? rhs_right : rhs_left;
        if (!addend) return 0;

        // Track this target for bound-mutation check later
        if (stmt_target_count >= MAX_SERIES_ACCUMULATORS + 1) return 0;
        stmt_targets[stmt_target_count++] = target;

        if (strcmp(target, counter_var) == 0) {
            // Counter increment: one of them, by a positive integer literal
            // that fits the counter's type.
            if (counter_idx >= 0) return 0;
            unsigned long long step;
            if (!series_decimal_literal(addend, &step) || step == 0) return 0;
            if (step > (counter_width == 32 ? (unsigned long long)INT32_MAX
                                             : (unsigned long long)INT64_MAX)) return 0;
            counter_step = step;
            counter_idx = i;
        } else {
            // Accumulator: an integer variable, adding either a loop-invariant
            // integer (constant series) or the counter itself / counter*C
            // (linear sum).
            if (acc_count >= MAX_SERIES_ACCUMULATORS) return 0;
            int width = series_int_width(self);
            if (!width) return 0;

            int addend_is_counter = 0;
            unsigned long long scale = 1;

            if (addend->type == AST_IDENTIFIER && addend->value &&
                strcmp(addend->value, counter_var) == 0) {
                // Plain counter addend: acc = acc + i
                addend_is_counter = 1;
            } else if (addend->type == AST_BINARY_EXPRESSION && addend->value &&
                       strcmp(addend->value, "*") == 0 && addend->child_count >= 2) {
                // Possibly scaled counter: acc = acc + i * C  or  acc = acc + C * i
                ASTNode* ml = addend->children[0];
                ASTNode* mr = addend->children[1];
                if (ml && ml->type == AST_IDENTIFIER && ml->value &&
                    strcmp(ml->value, counter_var) == 0 &&
                    series_decimal_literal(mr, &scale)) {
                    addend_is_counter = 1;
                } else if (mr && mr->type == AST_IDENTIFIER && mr->value &&
                           strcmp(mr->value, counter_var) == 0 &&
                           series_decimal_literal(ml, &scale)) {
                    addend_is_counter = 1;
                }
                // The loop computes i * C in the product's own type, which
                // wraps there. That matches the closed form modulo 2^width
                // only when the product is at least as wide as the
                // accumulator.
                if (addend_is_counter && series_int_width(addend) < width) return 0;
            }

            if (!addend_is_counter) {
                // Invariant addend: an integer that does not depend on the counter
                if (!series_int_width(addend)) return 0;
                if (expr_references_var(addend, counter_var)) return 0;
                if (codegen_expr_has_side_effects(addend)) return 0;
            }
            acc_ids[acc_count]       = self;
            acc_addends[acc_count]   = addend;
            acc_width[acc_count]     = width;
            acc_is_linear[acc_count] = addend_is_counter;
            acc_scale[acc_count]     = scale;
            acc_stmt[acc_count]      = i;
            acc_count++;
        }
    }

    if (counter_idx < 0) return 0;

    // 3b. Bound-mutation check: if any loop body statement assigns to a variable
    // referenced in the bound expression, the bound changes per-iteration.
    for (int i = 0; i < stmt_target_count; i++) {
        if (expr_references_var(cond_right, stmt_targets[i])) return 0;
    }

    // 3c. Addend invariance check: verify no addend references a variable modified
    // by any other statement in the loop body.
    // Skip for linear accumulators — their "addend" is the counter itself, which is
    // expected to be in the write-set; the formula accounts for that by design.
    for (int i = 0; i < acc_count; i++) {
        if (acc_is_linear[i]) continue;
        for (int j = 0; j < stmt_target_count; j++) {
            if (expr_references_var(acc_addends[i], stmt_targets[j])) return 0;
        }
    }

    // 4. Emit the closed form, guarded by the loop's own condition so a loop
    // that would not run leaves everything unchanged. Inside, the trip count
    // is at least 1. All arithmetic is in uint64_t, where it is exact modulo
    // 2^64; each result is then narrowed to its variable's type, which is
    // what the loop's wrapping additions produce.
    const char* counter_ctype = counter_width == 32 ? "int" : "int64_t";
    const char* counter_max   = counter_width == 32 ? "INT32_MAX" : "INT64_MAX";

    /* The closed form stands for the loop, so a C error in it is the loop's. */
    codegen_maybe_emit_line(gen, condition);
    print_indent(gen);
    fprintf(gen->output, "if ((");
    series_var(gen, cond_left);
    fprintf(gen->output, ") %s (", is_lte ? "<=" : "<");
    generate_expression(gen, cond_right);
    fprintf(gen->output, ")) {\n");
    indent(gen);

    // Distance to the bound, and how many steps the counter has before it
    // passes its type's maximum.
    print_indent(gen);
    fprintf(gen->output, "uint64_t _ae_sd = (uint64_t)(int64_t)(");
    generate_expression(gen, cond_right);
    fprintf(gen->output, ") - (uint64_t)(int64_t)");
    series_var(gen, cond_left);
    fprintf(gen->output, ";\n");
    print_indent(gen);
    fprintf(gen->output, "uint64_t _ae_sh = ((uint64_t)%s - (uint64_t)(int64_t)", counter_max);
    series_var(gen, cond_left);
    fprintf(gen->output, ") / %lluULL;\n", counter_step);

    // The trip count, when the counter's last step does not wrap.
    print_indent(gen);
    if (is_lte) {
        fprintf(gen->output, "if (_ae_sd / %lluULL < _ae_sh) {\n", counter_step);
    } else {
        fprintf(gen->output, "if (_ae_sd / %lluULL + (_ae_sd %% %lluULL != 0) <= _ae_sh) {\n",
                counter_step, counter_step);
    }
    indent(gen);
    print_indent(gen);
    if (is_lte) {
        fprintf(gen->output, "uint64_t _ae_sn = _ae_sd / %lluULL + 1;\n", counter_step);
    } else {
        fprintf(gen->output, "uint64_t _ae_sn = _ae_sd / %lluULL + (_ae_sd %% %lluULL != 0);\n",
                counter_step, counter_step);
    }

    int emitted_linear = 0;
    for (int i = 0; i < acc_count; i++) {
        const char* acc_ctype = acc_width[i] == 32 ? "int" : "int64_t";
        codegen_maybe_emit_line(gen, stmts[acc_stmt[i]]);
        print_indent(gen);
        if (acc_is_linear[i]) {
            // Sum of the counter values the statement sees: c0 + k*step for
            // k = 0..T-1, or k = 1..T when it follows the increment.
            //   T*c0 + step * T*(T-1)/2     or     T*c0 + step * T*(T+1)/2
            // T*(T±1)/2 halves whichever factor is even, so it is exact
            // modulo 2^64 without a 128-bit product.
            int after = acc_stmt[i] > counter_idx;
            const char* tri = after
                ? "(_ae_sn % 2 == 0 ? (_ae_sn / 2) * (_ae_sn + 1) : _ae_sn * (_ae_sn / 2 + 1))"
                : "(_ae_sn % 2 == 0 ? (_ae_sn / 2) * (_ae_sn - 1) : _ae_sn * ((_ae_sn - 1) / 2))";
            series_var(gen, acc_ids[i]);
            fprintf(gen->output, " = (%s)((uint64_t)(int64_t)", acc_ctype);
            series_var(gen, acc_ids[i]);
            fprintf(gen->output, " + %lluULL * ((uint64_t)(int64_t)", acc_scale[i]);
            series_var(gen, cond_left);
            fprintf(gen->output, " * _ae_sn + %lluULL * ", counter_step);
            fputs(tri, gen->output);
            fprintf(gen->output, "));\n");
            emitted_linear = 1;
        } else {
            // Invariant addend: added once per trip.
            series_var(gen, acc_ids[i]);
            fprintf(gen->output, " = (%s)((uint64_t)(int64_t)", acc_ctype);
            series_var(gen, acc_ids[i]);
            fprintf(gen->output, " + (uint64_t)(int64_t)(");
            generate_expression(gen, acc_addends[i]);
            fprintf(gen->output, ") * _ae_sn);\n");
        }
    }

    // counter = c0 + step * T
    codegen_maybe_emit_line(gen, stmts[counter_idx]);
    print_indent(gen);
    series_var(gen, cond_left);
    fprintf(gen->output, " = (%s)((uint64_t)(int64_t)", counter_ctype);
    series_var(gen, cond_left);
    fprintf(gen->output, " + _ae_sn * %lluULL);\n", counter_step);

    unindent(gen);
    print_indent(gen);
    fprintf(gen->output, "} else {\n");
    indent(gen);
    // The counter would wrap on its last step: run the loop as written.
    gen->series_collapse_off++;
    print_indent(gen);
    generate_statement(gen, while_node);
    gen->series_collapse_off--;
    unindent(gen);
    print_indent(gen);
    fprintf(gen->output, "}\n");

    unindent(gen);
    print_indent(gen);
    fprintf(gen->output, "}\n");

    if (emitted_linear) {
        global_opt_stats.linear_loops_collapsed++;
    } else {
        global_opt_stats.series_loops_collapsed++;
    }
    return 1;
}

/* #1047: emit the boolean condition that a case selector matches `val_var`.
 * Handles a single value (== , or string_equals for strings), an inclusive
 * (`lo..=hi`) / half-open (`lo..<hi`) range, and a comma-list of these (OR).
 * Shared by match arms and the ranged-switch if-chain lowering. */
static void emit_selector_condition(CodeGenerator* gen, ASTNode* sel,
                                    const char* val_var, int is_string) {
    if (!sel) { fprintf(gen->output, "0"); return; }
    if (sel->type == AST_MATCH_ALT) {
        fprintf(gen->output, "(");
        for (int i = 0; i < sel->child_count; i++) {
            if (i > 0) fprintf(gen->output, " || ");
            emit_selector_condition(gen, sel->children[i], val_var, is_string);
        }
        fprintf(gen->output, ")");
        return;
    }
    if (sel->type == AST_MATCH_RANGE && sel->child_count >= 2) {
        int inclusive = sel->annotation && strcmp(sel->annotation, "inclusive") == 0;
        fprintf(gen->output, "(%s >= ", val_var);
        generate_expression(gen, sel->children[0]);
        fprintf(gen->output, " && %s %s ", val_var, inclusive ? "<=" : "<");
        generate_expression(gen, sel->children[1]);
        fprintf(gen->output, ")");
        return;
    }
    if (is_string) {
        fprintf(gen->output, "(%s && string_equals(%s, ", val_var, val_var);
        generate_expression(gen, sel);
        fprintf(gen->output, "))");
    } else {
        /* No wrapping parens: every caller already parenthesises (the
         * if-opener or the ALT joiner), and `if ((x == A))` trips clang's
         * default -Wparentheses-equality on every match a user compiles. */
        fprintf(gen->output, "%s == ", val_var);
        generate_expression(gen, sel);
    }
}

/* #1047: does this case selector contain a range (directly, or as an element
 * of a comma-list)? A C `switch` can't express a range, so a switch with any
 * ranged case is lowered to an if-else chain instead. */
static int selector_has_range(ASTNode* sel) {
    if (!sel) return 0;
    if (sel->type == AST_MATCH_RANGE) return 1;
    if (sel->type == AST_MATCH_ALT) {
        for (int i = 0; i < sel->child_count; i++)
            if (sel->children[i] && sel->children[i]->type == AST_MATCH_RANGE) return 1;
    }
    return 0;
}

/* #1047: the C type of a switch/match scrutinee, for the temp that a lowered
 * if-chain compares against. Mirrors the match `_match_val` typing. */
static const char* scrutinee_c_type(ASTNode* expr) {
    Type* t = expr ? expr->node_type : NULL;
    if (!t) return "int";
    if (t->kind == TYPE_STRING || t->kind == TYPE_PTR) return "const char*";
    if (t->kind == TYPE_FLOAT)      return "double";
    if (t->kind == TYPE_FLOAT32)    return "float";
    if (t->kind == TYPE_LONGDOUBLE) return "long double";
    if (t->kind == TYPE_INT64)      return "int64_t";
    if (t->kind == TYPE_BOOL)       return "bool";
    return "int";
}

static void generate_list_pattern_condition(CodeGenerator* gen, ASTNode* pattern,
                                            const char* len_name,
                                            int is_seq_match) {
    if (!pattern) return;

    if (is_seq_match) {
        /* StringSeq cons cell. Empty list = NULL pointer; non-empty = any
         * non-NULL cell (which by construction has at least one head and
         * a tail — never partially-initialised, see string_seq_cons). */
        if (pattern->type == AST_PATTERN_LIST && pattern->child_count == 0) {
            fprintf(gen->output, "%s == NULL", len_name);
        } else if (pattern->type == AST_PATTERN_LIST) {
            /* Fixed-arity list pattern `[a, b, c]` against a StringSeq —
             * compare cached length so an O(1) test is enough. */
            fprintf(gen->output, "%s != NULL && %s->length == %d",
                    len_name, len_name, pattern->child_count);
        } else if (pattern->type == AST_PATTERN_CONS) {
            fprintf(gen->output, "%s != NULL", len_name);
        }
        return;
    }

    if (pattern->type == AST_PATTERN_LIST) {
        if (pattern->child_count == 0) {
            fprintf(gen->output, "%s == 0", len_name);
        } else {
            fprintf(gen->output, "%s == %d", len_name, pattern->child_count);
        }
    } else if (pattern->type == AST_PATTERN_CONS) {
        fprintf(gen->output, "%s >= 1", len_name);
    }
}

// Check if any binding in the pattern is actually used by the arm body
static int pattern_needs_array(ASTNode* pattern, ASTNode* body) {
    if (!pattern || !body) return 0;
    if (pattern->type == AST_PATTERN_LIST) {
        for (int i = 0; i < pattern->child_count; i++) {
            ASTNode* elem = pattern->children[i];
            if (elem && elem->type == AST_PATTERN_VARIABLE && elem->value &&
                expr_references_var(body, elem->value)) return 1;
        }
    } else if (pattern->type == AST_PATTERN_CONS && pattern->child_count >= 2) {
        ASTNode* head = pattern->children[0];
        ASTNode* tail = pattern->children[1];
        if (head && head->type == AST_PATTERN_VARIABLE && head->value &&
            expr_references_var(body, head->value)) return 1;
        if (tail && tail->type == AST_PATTERN_VARIABLE && tail->value &&
            expr_references_var(body, tail->value)) return 1;
    }
    return 0;
}

static void generate_list_pattern_bindings(CodeGenerator* gen, ASTNode* pattern,
                                           ASTNode* match_expr, const char* len_name,
                                           ASTNode* body, int is_seq_match) {
    if (!pattern) return;

    if (is_seq_match) {
        /* StringSeq cons-cell bindings. The seq pointer is already in
         * scope as `len_name` (which holds the StringSeq* itself for
         * the seq-match path). For `[h|t]` we read s->head and
         * s->tail; for `[a, b, c]` (fixed-arity over a seq) we walk
         * the cells. Skipped per-binding when the body never
         * references the binding name — same `pattern_needs_array`
         * optimisation the int-array path uses. */
        if (pattern->type == AST_PATTERN_CONS && pattern->child_count >= 2) {
            ASTNode* head = pattern->children[0];
            ASTNode* tail = pattern->children[1];
            if (head && head->type == AST_PATTERN_VARIABLE && head->value) {
                if (expr_references_var(body, head->value)) {
                    print_line(gen, "const char* %s = %s->head;",
                               head->value, len_name);
                }
            }
            if (tail && tail->type == AST_PATTERN_VARIABLE && tail->value) {
                if (expr_references_var(body, tail->value)) {
                    print_line(gen, "StringSeq* %s = %s->tail;",
                               tail->value, len_name);
                }
            }
        } else if (pattern->type == AST_PATTERN_LIST && pattern->child_count > 0) {
            /* Fixed-arity over a seq — walk i cells. Cheap because we
             * already know the length matches (see the condition
             * emitted above), so no per-iteration NULL guard. */
            for (int i = 0; i < pattern->child_count; i++) {
                ASTNode* elem = pattern->children[i];
                if (elem && elem->type == AST_PATTERN_VARIABLE && elem->value &&
                    expr_references_var(body, elem->value)) {
                    print_indent(gen);
                    fprintf(gen->output, "const char* %s = ", elem->value);
                    fprintf(gen->output, "%s", len_name);
                    for (int j = 0; j < i; j++) fprintf(gen->output, "->tail");
                    fprintf(gen->output, "->head;\n");
                }
            }
        }
        return;
    }

    // Only declare the array pointer if this arm actually uses element bindings
    int needs_arr = pattern_needs_array(pattern, body);
    if (needs_arr) {
        print_indent(gen);
        fprintf(gen->output, "int* _match_arr = ");
        generate_expression_as_elem_ptr(gen, match_expr);   /* #1286 */
        fprintf(gen->output, ";\n");
    }

    if (pattern->type == AST_PATTERN_LIST && pattern->child_count > 0) {
        for (int i = 0; i < pattern->child_count; i++) {
            ASTNode* elem = pattern->children[i];
            if (elem && elem->type == AST_PATTERN_VARIABLE && elem->value) {
                if (expr_references_var(body, elem->value)) {
                    print_line(gen, "int %s = _match_arr[%d];", elem->value, i);
                }
            }
        }
    } else if (pattern->type == AST_PATTERN_CONS && pattern->child_count >= 2) {
        ASTNode* head = pattern->children[0];
        ASTNode* tail = pattern->children[1];

        if (head && head->type == AST_PATTERN_VARIABLE && head->value) {
            if (expr_references_var(body, head->value)) {
                print_line(gen, "int %s = _match_arr[0];", head->value);
            }
        }
        if (tail && tail->type == AST_PATTERN_VARIABLE && tail->value) {
            if (expr_references_var(body, tail->value)) {
                /* #1286: the tail is a `T[]` slice over the rest; `_len`
                 * is kept for code that still reads the companion. */
                print_line(gen, "AetherSlice %s = aether_slice_make(&_match_arr[1], (uint64_t)(%s - 1));",
                           tail->value, len_name);
                print_line(gen, "int %s_len = %s - 1;", tail->value, len_name);
            }
        }
    }
}

static int has_list_patterns(ASTNode* match_stmt) {
    for (int i = 1; i < match_stmt->child_count; i++) {
        ASTNode* arm = match_stmt->children[i];
        if (arm && arm->type == AST_MATCH_ARM && arm->child_count >= 1) {
            ASTNode* pattern = arm->children[0];
            if (pattern && (pattern->type == AST_PATTERN_LIST ||
                           pattern->type == AST_PATTERN_CONS)) {
                return 1;
            }
        }
    }
    return 0;
}

// Forward declarations.

// The first top-level definition named `name` — for a pattern-matched
// multi-clause function, the first clause, whose body is representative
// for return-type purposes. Called once per user-fn call site from
// several codegen paths; #2007 routed it through the program index, so
// it no longer scans the top level per call.
ASTNode* find_function_definition_by_name(ASTNode* program,
                                          const char* name) {
    const DefClauses* dc = program_index_clauses(program, name);
    return (dc && dc->count > 0) ? dc->nodes[0] : NULL;
}

// Sibling of find_function_definition_by_name for AST_EXTERN_FUNCTION
// nodes. Used by is_heap_string_expr to consult the `@heap` annotation
// on extern returns. Two-step scan because externs don't get cloned
// into the program AST by module_merge_into_program — they stay in
// their owning module's AST (see compiler/aether_module.c:1283-1284).
// Same traversal pattern as codegen_diagnose_ownership at line 4142.
static ASTNode* find_extern_declaration_by_name(ASTNode* program,
                                                const char* name) {
    if (!program || !name) return NULL;
    /* Pass 1 — direct extern declarations in the program AST (the
     * shape user code uses for non-module C bindings). */
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_EXTERN_FUNCTION &&
            c->value && strcmp(c->value, name) == 0) {
            return c;
        }
    }
    /* Pass 2 — externs reachable through `import` statements. The
     * stdlib's `extern http_request_body(...)` lives in std.http's
     * module AST; without this pass, call sites in the consuming
     * program never resolve to the annotated declaration. */
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (!c || c->type != AST_IMPORT_STATEMENT || !c->value) continue;
        AetherModule* mod_entry = module_find(c->value);
        ASTNode* mod_ast = mod_entry ? mod_entry->ast : NULL;
        if (!mod_ast) continue;
        for (int j = 0; j < mod_ast->child_count; j++) {
            ASTNode* decl = mod_ast->children[j];
            if (decl && decl->type == AST_EXTERN_FUNCTION &&
                decl->value && strcmp(decl->value, name) == 0) {
                return decl;
            }
        }
    }
    return NULL;
}

// Does an extern declaration carry a `@heap` return annotation, parsed
// in parse_extern_declaration as the string token "heap_return"
// appended to (or set as) the extern's annotation slot? Used by
// is_heap_string_expr's extern branch.
//
// substring-match rather than exact equality so the annotation can
// coexist with other extern-level annotations (`c_symbol:...` from the
// `@extern("name")` form, or any future tags) — the same combine-
// dedupe pattern the per-param annotation handler uses at
// parser.c:2800-2813.
static int extern_returns_heap_string(ASTNode* ext) {
    if (!ext || ext->type != AST_EXTERN_FUNCTION || !ext->annotation) {
        return 0;
    }
    return annotation_has_marker(ext->annotation, "heap_return");
}

// Heap-allocated string sources. Used by the reassignment / scope-
// exit machinery to decide whether to free the previous value.
//
// Recognised:
//   1. Hardcoded stdlib functions that always malloc
//      (string_concat / string_substring / string_to_upper /
//      string_to_lower / string_trim).
//   2. String interpolation — always allocates via `_aether_interp`.
//   3. A user-defined string-returning function whose body's every
//      return path yields a heap-string-expr (recursive structural
//      check). This closes the bug_repo.md leak referenced from
//      issue #405: `s = my_concat(s, "x")` in a loop now goes
//      through the heap-aware reassignment wrapper. Functions that
//      return a string literal (or forward a borrowed parameter)
//      are explicitly NOT recognised — treating them as heap would
//      free a literal at scope exit and abort.
//
// The recursion is bounded by AST depth and is memoised on the fn
// def's annotation slot; mutual recursion through `-> string`
// functions returns "not heap" conservatively (cycle break).
//
// `gen` may be NULL for unit-test contexts that exercise the
// hardcoded-stdlib + string-interp fast paths in isolation. When
// non-NULL, gen->program is consulted for user-defined-fn lookup;
// when NULL, we fall through to the conservative answer.
/* Does `expr` produce an OWNED *StringSeq — a fresh refcount the caller
 * must release — as opposed to a borrowed pointer into an existing
 * spine? Owned: cons / reverse / concat / take / drop / from_array /
 * split_to_seq / retain / empty, and any user fn returning *StringSeq
 * (its return-escape contract hands the caller a ref). Borrowed:
 * string_seq_tail (returns s->tail with no new ref) — must NOT be
 * tracked as owned, or freeing it would decrement a cell the parent
 * spine still owns. The empty producer yields NULL, which string_seq_free
 * treats as a no-op, so tracking it as owned is harmless. */
int is_seq_owning_expr(CodeGenerator* gen, ASTNode* expr) {
    if (!expr || expr->type != AST_FUNCTION_CALL || !expr->value) return 0;
    if (!expr->node_type || !is_string_seq_ptr_type(expr->node_type)) return 0;
    const char* fn = codegen_normalise_callee(expr->value);
    if (!fn) return 0;
    if (strcmp(fn, "string_seq_tail") == 0) return 0;  /* borrowed */
    if (strcmp(fn, "string_seq_cons") == 0 ||
        strcmp(fn, "string_seq_empty") == 0 ||
        strcmp(fn, "string_seq_reverse") == 0 ||
        strcmp(fn, "string_seq_concat") == 0 ||
        strcmp(fn, "string_seq_take") == 0 ||
        strcmp(fn, "string_seq_drop") == 0 ||
        strcmp(fn, "string_seq_from_array") == 0 ||
        strcmp(fn, "string_seq_retain") == 0 ||
        strcmp(fn, "string_split_to_seq") == 0) {
        return 1;
    }
    /* A user-defined function with a *StringSeq return type hands back an
     * owned ref (its own return-escaped local). */
    if (gen && find_function_definition_by_name(gen->program, fn)) return 1;
    return 0;
}

int or_fallible_value_slot_is_heap(CodeGenerator* gen, ASTNode* fallible);

int is_heap_string_expr(CodeGenerator* gen, ASTNode* expr) {
    if (!expr) return 0;

    /* #2619: a string a call may hand back from a struct temporary of the
     * statement is copied where it is made (call_returns_view_of_temp), so
     * what it yields is a fresh heap string, adopted or freed as one. */
    if (expr->type == AST_FUNCTION_CALL && call_returns_view_of_temp(gen, expr)) return 1;

    /* #2586: a string a call through a typed fn pointer returns is the
     * caller's (the fn-value convention, discover_fn_values). */
    if (expr->type == AST_FUNCTION_CALL && fnptr_call_returns_string(gen, expr)) return 1;

    // String interpolation (non-printf mode) allocates via _aether_interp.
    if (expr->type == AST_STRING_INTERP) {
        return 1;
    }

    /* A string result of call(): a string-returning closure hands every
     * result over owned (see should_uniform_heap_return), so the caller
     * owns what it gets, whichever return site produced it (#2054). */
    if (expr->type == AST_FUNCTION_CALL && expr->value &&
        strcmp(expr->value, "call") == 0 &&
        expr->node_type && expr->node_type->kind == TYPE_STRING) {
        return 1;
    }

    /* An ask whose handler replies with a string. The reply statement
     * deep-copies the text so the asker's read outlives the handler's own
     * scope exit, which makes the value the asker receives owned: without
     * tracking it here, every string reply leaks one copy per ask. */
    if (expr->type == AST_SEND_ASK && expr->node_type &&
        expr->node_type->kind == TYPE_STRING) {
        return 1;
    }

    /* `fallible or handler` whose result is a string and whose success
     * value slot is heap: the `or` lowering boxes the handler's default
     * via aether_uniform_heap_str so BOTH paths yield a malloc-owned
     * pointer, making the whole expression uniformly heap. Classifying it
     * here sets the caller's `_heap_<lhs>` tracker so the yielded value is
     * freed at scope exit (the `v, e = f()` destructure form already frees
     * the value via its own tracker; without this the `or` form leaked
     * it). Must stay in lock-step with the boxing in the AST_OR_ELSE
     * codegen — both gate on or_fallible_value_slot_is_heap. */
    /* The `or`-node's own node_type is not reliably stamped in every
     * position (e.g. a `return f() or { … }` node can reach here with a
     * NULL node_type), so derive string-ness from the fallible's VALUE
     * slot type (`tuple_types[0]`) rather than the `or`-node itself —
     * that is the type the expression actually yields, and it is what
     * or_fallible_value_slot_is_heap already keys on. */
    if (expr->type == AST_OR_ELSE && expr->child_count >= 1 &&
        expr->children[0] && expr->children[0]->node_type &&
        expr->children[0]->node_type->kind == TYPE_TUPLE &&
        expr->children[0]->node_type->tuple_count >= 2 &&
        expr->children[0]->node_type->tuple_types[0] &&
        expr->children[0]->node_type->tuple_types[0]->kind == TYPE_STRING &&
        or_fallible_value_slot_is_heap(gen, expr->children[0])) {
        return 1;
    }

    /* Bare identifier of a heap-tracked local: by construction, the
     * tracker's invariant is "this slot owns a heap allocation when
     * `_heap_<name> == 1`". Treating it as heap here makes the
     * classifier consistent with the walker's special case in
     * walk_returns_for_heap_check and lets the uniform-heap return
     * shim see runtime ownership through `_heap_<name>`. */
    if (expr->type == AST_IDENTIFIER && expr->value &&
        gen && is_heap_string_var(gen, expr->value)) {
        return 1;
    }

    if (expr->type == AST_FUNCTION_CALL && expr->value) {
        // Source-level `string.concat(...)` lands in the AST as the
        // dotted string `"string.concat"`, but stdlib externs and the
        // generated C call sites use the underscore form. Normalise
        // before both the hardcoded allowlist and the user-fn lookup
        // below.
        const char* fn = codegen_normalise_callee(expr->value);
        // Hardcoded stdlib fast-path.
        //
        // `string_new_with_length` is the length-aware AetherString
        // constructor — it mallocs a fresh refcounted AetherString and
        // copies the source bytes in (`aether_bytes_finish` is itself
        // just `string_new_with_length` + copy). Several stdlib modules
        // build their return values with it directly and declare the
        // extern `-> ptr`, with no `@heap` annotation, so without an
        // intrinsic entry here every `zlib`/`cryptography`/`lzf`/`fs`
        // decode result was classified non-heap and leaked at every
        // caller. Recognising it by name — like `string_concat` and
        // interpolation — closes the leak regardless of how each
        // module spells the extern, and stops the next `-> ptr`
        // declaration silently reintroducing it. See
        // string-new-with-length-heap-annotation.md (follow-up to the
        // 0.161.0 `bytes.finish` heap-ownership fix).
        //
        // The `string_substring_n` / `string_from_*` entries complete
        // the sweep `string-new-with-length-heap-annotation.md` asked
        // for: every runtime entry point that mints a fresh owned
        // buffer must be a recognised heap source or it leaks at every
        // call site. `string_substring_n` returns a plain malloc'd
        // `char*` (identical shape to the already-listed
        // `string_substring`); `string_from_int` / `_long` / `_float`
        // / `_char` each `string_new(...)` a fresh refcounted
        // AetherString. `aether_heap_str_free` dispatches on the magic
        // header, so both shapes free correctly through the tracker.
        if (strcmp(fn, "string_concat") == 0 ||
            /* string_concat_wrapped mints a fresh refcounted AetherString
             * via string_new_with_length and returns it (never a borrowed
             * or literal pointer) — its `-> string` result is owned heap,
             * reclaimed via aether_heap_str_free (string_release) at scope
             * exit. Without this the wrapped-concat result local leaked. */
            strcmp(fn, "string_concat_wrapped") == 0 ||
            strcmp(fn, "string_substring") == 0 ||
            strcmp(fn, "string_substring_n") == 0 ||
            /* string_replace / string_replace_all (#1331) always return
             * a fresh managed string via string_new_with_length /
             * string_adopt_caps_buffer, even on the no-match path
             * (which returns a COPY, never the borrowed input). */
            strcmp(fn, "string_replace") == 0 ||
            strcmp(fn, "string_replace_all") == 0 ||
            /* string_seq_join always returns a fresh AetherString
             * built from an exact-size buffer, never a borrowed or
             * literal pointer, same shape as substring. */
            strcmp(fn, "string_seq_join") == 0 ||
            strcmp(fn, "string_join") == 0 ||
            /* string.copy returns `string_concat(s, "")` — always a
             * fresh owned heap buffer (never a borrowed/literal pointer),
             * the same shape as its sibling string_concat already on this
             * list. Classify by name so `x = string.copy(s)` gets
             * _heap_x = 1 and is reclaimed at scope exit / next reassign;
             * the general user-fn return-heap path misses it. */
            strcmp(fn, "string_copy") == 0 ||
            strcmp(fn, "string_to_upper") == 0 ||
            strcmp(fn, "string_to_lower") == 0 ||
            strcmp(fn, "string_trim") == 0 ||
            strcmp(fn, "string_new_with_length") == 0 ||
            strcmp(fn, "string_from_int") == 0 ||
            strcmp(fn, "string_from_long") == 0 ||
            strcmp(fn, "string_from_float") == 0 ||
            strcmp(fn, "string_from_char") == 0 ||
            /* Additional always-fresh-heap string producers (verified
             * each returns owned heap, never a borrowed/literal pointer):
             *   string_from_int_radix : string_new / string_empty
             *   string_pad_start/_end : string_new_with_length / _empty
             *   string_format_list    : fresh AetherString
             *   json_stringify_raw    : fresh malloc'd char*
             * aether_heap_str_free dispatches on the magic header, so
             * AetherString producers string_release and the plain-char*
             * json one free()s — both correct, neither ever a literal.
             * NOT string_to_cstr: it returns a BORROWED pointer into its
             * argument (the AetherString's ->data, or the input itself),
             * so auto-freeing it would corrupt the source. */
            strcmp(fn, "string_from_int_radix") == 0 ||
            strcmp(fn, "string_pad_start") == 0 ||
            strcmp(fn, "string_pad_end") == 0 ||
            strcmp(fn, "string_format_list") == 0 ||
            strcmp(fn, "json_stringify_raw") == 0 ||
            /* std.fs lexical path ops (#632) and std.io whole-file read:
             * each returns a FRESH malloc'd / caps-allocated string the
             * caller owns (path_clean / path_rel build a new normalised
             * path; io_read_file_raw returns the file contents). Verified
             * owned (never a borrowed/literal pointer): the leak each
             * produced was exactly the caller never freeing this result.
             * A new producer is better declared `-> string @heap` at its
             * extern (extern_returns_heap_string), where the ownership is
             * stated next to the signature instead of in this list. */
            strcmp(fn, "path_clean") == 0 ||
            strcmp(fn, "path_rel") == 0 ||
            /* Sibling lexical path ops, same ownership contract as
             * path_clean/path_rel: each returns a FRESH malloc'd/strdup'd
             * buffer (path_join builds a new joined path; path_dirname,
             * path_basename, path_extension each strdup or malloc a slice
             * — never a borrowed pointer into the input, never a literal;
             * NULL on error, which aether_heap_str_free tolerates). They
             * leaked at every call site because the caller never freed the
             * result (std.path/std.fs both expose them). */
            strcmp(fn, "path_join") == 0 ||
            strcmp(fn, "path_dirname") == 0 ||
            strcmp(fn, "path_basename") == 0 ||
            strcmp(fn, "path_extension") == 0 ||
            strcmp(fn, "io_read_file_raw") == 0 ||
            /* Whole-file / command-capture reads: each call returns a
             * FRESH malloc'd buffer of the file contents / process output
             * (NULL on error, which aether_heap_str_free tolerates). Never
             * a borrowed/static pointer. file_read_all_raw also drives the
             * std.fs `read` / `read_or_empty` wrappers, so classifying it
             * propagates ownership out to their callers too. */
            strcmp(fn, "file_read_all_raw") == 0 ||
            strcmp(fn, "os_exec_raw") == 0 ||
            strcmp(fn, "os_run_capture_raw") == 0 ||
            /* std.os string accessors: each returns a FRESH strdup'd buffer
             * (os_getenv copies the env value; os_which the resolved path;
             * os_platform_raw strdup's the platform name — NOT a literal;
             * os_now_utc/local_iso8601_raw strdup the formatted timestamp).
             * NULL on error (tolerated). Verified owned, never borrowed/
             * literal — each leaked at every call site because the caller
             * never freed the copy. */
            strcmp(fn, "os_getenv") == 0 ||
            strcmp(fn, "os_which") == 0 ||
            strcmp(fn, "os_platform_raw") == 0 ||
            strcmp(fn, "os_getcwd_raw") == 0 ||
            strcmp(fn, "os_now_utc_iso8601_raw") == 0 ||
            strcmp(fn, "os_now_local_iso8601_raw") == 0 ||
            /* std.io getenv — sibling of os_getenv, returns a FRESH
             * strdup'd copy of the env value (NULL on miss). Unclassified
             * it leaked at every call site; the std.fs/std.io tests all
             * read io.getenv("TMPDIR"/"TEMP") for a scratch dir, so this
             * one entry clears the shared 2-leak across the fs/io suite. */
            strcmp(fn, "io_getenv") == 0 ||
            /* std.cryptography base64 encoders: each returns a FRESH
             * aether_caps_malloc'd buffer (the caller-owned-return contract,
             * libc-freeable; NULL on error). The `-> string` externs aren't
             * @heap-annotated, so the cryptography.base64_encode[_padded]
             * wrappers' inferred tuple position 0 was non-heap and leaked
             * the result at every call site. */
            strcmp(fn, "cryptography_base64_encode_raw") == 0 ||
            strcmp(fn, "cryptography_base64_encode_padded_raw") == 0 ||
            /* std.io errno-message: returns a FRESH malloc'd strerror copy
             * (NULL on no-error). The io.perror / errno_message wrapper
             * leaked it at every call. */
            strcmp(fn, "io_errno_message_raw") == 0 ||
            /* StringBuilder finalise: hands the caller a plain libc-
             * freeable char* and frees the wrapper (std/strbuilder/
             * aether_strbuilder.c:235). Declared `-> string @heap`, but
             * classify by name here too so the ownership propagates
             * through .ae wrapper chains (strbuilder.finish, and any
             * user fn that returns it) regardless of single-value
             * @heap-annotation handling. */
            strcmp(fn, "aether_strbuilder_finish") == 0 ||
            /* std.xml (#627): xml_builder_finish hands the caller the
             * accumulated document as a FRESH malloc'd char* (detached
             * from the builder, which is freed separately); xml_escape
             * returns a FRESH malloc'd escaped copy. Both are single-value
             * `-> string` externs the std.xml `finish` / `escape` wrappers
             * copy via string_concat — classify by name so the malloc'd
             * original is reclaimed at scope exit (same contract as
             * json_stringify_raw above). NULL on OOM, which
             * aether_heap_str_free tolerates. */
            strcmp(fn, "xml_builder_finish") == 0 ||
            strcmp(fn, "xml_escape") == 0) {
            return 1;
        }
        // User-defined function: only heap if its body provably
        // returns heap strings. Structurally analyse the function
        // definition (memoised on the def node's annotation slot to
        // bound recursion). Without `gen->program` (e.g. unit tests)
        // fall through to the conservative "not heap" answer, which
        // is strictly better than the literal-free abort the naive
        // node_type-only check produced.
        if (gen && gen->program &&
            expr->node_type && expr->node_type->kind == TYPE_STRING) {
            ASTNode* fn_def = find_function_definition_by_name(
                gen->program, fn);
            if (fn_def) {
                return function_def_returns_heap_string(gen, fn_def);
            }
            /* Extern declaration with `@heap` annotation on its
             * `-> string` return. The parser stored "heap_return" in
             * the extern's annotation slot at
             * parser.c:parse_extern_declaration; here we honour it so
             * call sites like `got = tcp_receive_raw(conn, n)` get
             * `_heap_got = 1` and the reassignment-wrapper free fires
             * on the next assignment to `got`.
             *
             * Tuple-returning externs carry the same mark per position
             * in `Type.tuple_heap_flags`, read where the tuple is
             * destructured; this is the single-value complement, the
             * one path that owns the result of externs like
             * http_response_body or fs_readlink_raw. */
            ASTNode* ext_def = find_extern_declaration_by_name(
                gen->program, fn);
            if (ext_def && extern_returns_heap_string(ext_def)) {
                return 1;
            }
        }
    }

    return 0;
}

// Walk a function body; OR-fold each return statement's heap
// classification. After the walk:
//   *any_heap     — at least one return yields a heap-string-expr
//   *any_non_heap — at least one return yields a non-heap value
// A function is classified heap-returning when *any_heap is set.
// `*any_heap && *any_non_heap` flags a mixed-return function that
// needs the per-return uniform-heap shim wrap (Part 4): the caller
// will free unconditionally, so the literal branch must hand back
// a freshly malloc'd buffer to match.
//
// Cycle protection: the AST node uses its `annotation` slot to
// memoise the result via the strings "heap_yes" / "heap_no" /
// "heap_pending". A pending mark means we hit a cycle (two
// mutually-recursive `-> string` user functions); we conservatively
// return 0 in that case.
/* Walk a function body looking for any AST_VARIABLE_DECLARATION
 * whose target name equals `var_name` and whose RHS is heap-
 * classified by `is_heap_string_expr`. Used by the bare-identifier-
 * return check in walk_returns_for_heap_check: when the walker
 * encounters `return foo` it asks "is foo assigned from a heap
 * source anywhere in this function?" without consulting
 * `gen->heap_string_vars` — that set is in the CALLER's context
 * when the classifier runs from a cross-function call site, and
 * the lookup would miss the callee's own local even though it IS
 * heap-tracked there.
 *
 * Skips nested function / closure scopes (their assignments are in
 * a different lexical frame). Stops at the first heap-source
 * assignment to `var_name`. */
static int function_def_returns_heap_at(CodeGenerator* gen, ASTNode* fn_def,
                                         int position);

/* The `catch NAME` bindings in scope at a node, innermost last. A catch
 * binding may own a heap-built panic reason (#2333), so a value taken
 * from one is heap evidence for the return classifiers: the uniform-heap
 * shim at the return site reads the runtime flag and copies a borrowed
 * reason, so over-classifying costs one copy and never a bad free. */
#define CATCH_SCOPE_MAX 16
typedef struct {
    const char* names[CATCH_SCOPE_MAX];
    int count;
} CatchScope;

/* The node the return/heap classifiers below descend into for child `i`
 * of `node`. They skip closures (another function's returns and locals),
 * but a call's trailing DSL block is not one: it is emitted inline in the
 * enclosing function, so its `return`s leave that function and the
 * locals it assigns are that function's. Without this the classifiers
 * never saw a `return v` / `return n, v` inside `f() { ... }`: the
 * function was classified non-heap at that position, the return site did
 * not route the value through `aether_uniform_heap_str`, and the caller
 * never freed what it received, so every call leaked the string. The
 * test is codegen's own (`trailing_dsl_block`), so a closure handed to a
 * `fn` parameter stays a closure. */
static ASTNode* classifier_child(CodeGenerator* gen, ASTNode* node, int i) {
    ASTNode* c = node->children[i];
    if (c && c->type == AST_CLOSURE && c->value &&
        strcmp(c->value, "trailing") == 0 &&
        node->type == AST_FUNCTION_CALL && gen) {
        ASTNode* blk = trailing_dsl_block(gen, node);
        for (int bi = 0; blk && bi < c->child_count; bi++) {
            if (c->children[bi] == blk) return blk;
        }
    }
    return c;
}

static int catch_scope_has(const CatchScope* cs, ASTNode* expr) {
    if (!cs || !expr || expr->type != AST_IDENTIFIER || !expr->value) return 0;
    for (int i = cs->count - 1; i >= 0; i--) {
        if (strcmp(cs->names[i], expr->value) == 0) return 1;
    }
    return 0;
}

static int body_assigns_var_from_heap_in(CodeGenerator* gen, ASTNode* node,
                                         const char* var_name, CatchScope* cs);

/* Catch bindings are NOT evidence here: the container-ownership caller
 * (codegen_expr.c) hands a heap-classified value to an owning container
 * with no runtime flag to consult, and a borrowed reason (a literal
 * panic message) would then be freed at teardown. Only the return
 * classifier, whose uniform-heap shim reads the flag, takes the catch
 * evidence (body_assigns_var_from_heap_or_catch). */
int body_assigns_var_from_heap(CodeGenerator* gen, ASTNode* node,
                               const char* var_name) {
    return body_assigns_var_from_heap_in(gen, node, var_name, NULL);
}

static int body_assigns_var_from_heap_or_catch(CodeGenerator* gen, ASTNode* node,
                                               const char* var_name) {
    CatchScope cs = { {0}, 0 };
    return body_assigns_var_from_heap_in(gen, node, var_name, &cs);
}

/* Does some binding of `var_name` in the body leave it owning a heap value
 * on some path (a fresh value, a caught reason, a take that may own:
 * string_bind_owns with `may`)? For a slot that reads the variable's
 * tracker at run time. */
int body_may_assign_var_from_heap(CodeGenerator* gen, ASTNode* node, const char* var_name) {
    return body_assigns_var_from_heap_or_catch(gen, node, var_name);
}

/* #2461: does binding a local to `e` (a field read, or an `if` / `match`
 * over values) leave the local owning a buffer (emit_string_take)? With
 * `may`, on some path: the return classifier, whose uniform-heap shim reads
 * the runtime flag and copies what is not owned, and the container routing
 * of a local (string_container_store_value), whose take reads it too.
 * Without, on every path. Context-free, as the memoised classifiers above
 * require: an arm that is a local, or a call that only hands one back
 * (#2548), counts as possibly owned whichever function is being emitted. */
static int string_bind_owns_arm(CodeGenerator* gen, ASTNode* e, int may);

static int string_bind_owns(CodeGenerator* gen, ASTNode* e, int may) {
    if (!e) return 0;
    if (is_owned_string_field_read(e)) return 1;
    if (may && e->type == AST_FUNCTION_CALL && handback_leaf_node(gen, e, 0)) return 1;
    if (e->type == AST_IF_EXPRESSION && e->child_count >= 3) {
        int a = string_bind_owns_arm(gen, e->children[1], may);
        int b = string_bind_owns_arm(gen, e->children[2], may);
        return may ? (a || b) : (a && b);
    }
    if (e->type == AST_MATCH_STATEMENT) {
        int any = 0, all = 1, values = 0;
        for (int i = 1; i < e->child_count; i++) {
            ASTNode* arm = e->children[i];
            if (!arm || arm->type != AST_MATCH_ARM || arm->child_count < 2) continue;
            ASTNode* value = match_arm_value(arm->children[1]);
            /* An arm that yields no value leaves the local as it was. */
            int o = value && string_bind_owns_arm(gen, value, may);
            any |= o;
            all &= o;
            values++;
        }
        return may ? any : (values > 0 && all);
    }
    return 0;
}

static int string_bind_owns_arm(CodeGenerator* gen, ASTNode* e, int may) {
    if (!e) return 0;
    if (e->type == AST_IDENTIFIER) {
        return may && (!e->node_type || e->node_type->kind == TYPE_STRING);
    }
    if (e->type == AST_IF_EXPRESSION || e->type == AST_MATCH_STATEMENT ||
        is_owned_string_field_read(e)) {
        return string_bind_owns(gen, e, may);
    }
    if (may && e->type == AST_FUNCTION_CALL && handback_leaf_node(gen, e, 0)) return 1;
    return is_heap_string_expr(gen, e);
}

static int body_assigns_var_from_heap_in(CodeGenerator* gen, ASTNode* node,
                                         const char* var_name, CatchScope* cs) {
    if (!node || !var_name) return 0;
    if (node->type == AST_FUNCTION_DEFINITION ||
        node->type == AST_BUILDER_FUNCTION ||
        node->type == AST_CLOSURE) {
        return 0;
    }
    if (node->type == AST_VARIABLE_DECLARATION && node->value &&
        strcmp(node->value, var_name) == 0 &&
        node->child_count > 0 && node->children[0] &&
        (is_heap_string_expr(gen, node->children[0]) ||
         catch_scope_has(cs, node->children[0]) ||
         string_bind_owns(gen, node->children[0], cs != NULL))) {
        return 1;
    }
    if (cs && node->type == AST_CATCH_CLAUSE && node->value && cs->count < CATCH_SCOPE_MAX) {
        cs->names[cs->count++] = node->value;
        int found = 0;
        for (int i = 0; i < node->child_count && !found; i++) {
            found = body_assigns_var_from_heap_in(gen, node->children[i], var_name, cs);
        }
        cs->count--;
        return found;
    }
    /* `var_name` bound by a tuple destructure — `a, err = g(...)`. It is heap
     * iff g's tuple position for `var_name` is heap-classified. Without this,
     * an error slot fed from a callee's heap tracked-empty (e.g. asn1.read_oid
     * returning `strbuilder.finish(sb), err` where `err` came from
     * `read_tagged`'s heap `""`) is seen as non-heap, so the destructure site
     * sets `_heap_<err> = 0` and the byte leaks at every nested read_* call
     * (issue #1311). Children layout: last is the RHS call, the rest are the
     * target var nodes at their tuple positions. */
    if (node->type == AST_TUPLE_DESTRUCTURE && node->child_count >= 2) {
        int vc = node->child_count - 1;
        ASTNode* rhs = node->children[node->child_count - 1];
        for (int j = 0; j < vc; j++) {
            ASTNode* tgt = node->children[j];
            if (tgt && tgt->value && strcmp(tgt->value, var_name) == 0) {
                if (rhs && rhs->type == AST_FUNCTION_CALL && rhs->value &&
                    gen && gen->program) {
                    const char* fn = codegen_normalise_callee(rhs->value);
                    ASTNode* callee = find_function_definition_by_name(gen->program, fn);
                    if (callee) {
                        if (function_def_returns_heap_at(gen, callee, j)) return 1;
                    } else {
                        ASTNode* ext = find_extern_declaration_by_name(gen->program, fn);
                        if (ext && ext->node_type &&
                            ext->node_type->tuple_heap_flags &&
                            j < ext->node_type->tuple_count &&
                            ext->node_type->tuple_heap_flags[j]) {
                            return 1;
                        }
                    }
                }
            }
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        if (body_assigns_var_from_heap_in(gen, classifier_child(gen, node, i),
                                          var_name, cs)) return 1;
    }
    return 0;
}

static int function_def_returns_heap_at(CodeGenerator* gen, ASTNode* fn_def,
                                         int position);

/* Does the analysed body bind `var_name` at a heap-classified tuple
 * position of some destructure (`..., x, ... = g(...)`)?
 *
 * INVARIANT: this resolution must stay context-free. The return-heap
 * classifiers memoise per callee AST node, and the first classification
 * of a callee can run while ANY function is being emitted (a caller's
 * destructure site), so evidence taken from generation-time state
 * (`gen->heap_string_vars` via is_heap_string_expr on identifiers)
 * poisons the memo with the wrong function's tracker table. That was
 * #1311: a std tuple fn first classified at a user helper's site lost
 * its heap error slot and leaked the tracked-empty on every call. */
static int body_tuple_destructure_binds_heap(CodeGenerator* gen, ASTNode* node,
                                             const char* var_name) {
    if (!node || !var_name || !gen || !gen->program) return 0;
    if (node->type == AST_FUNCTION_DEFINITION ||
        node->type == AST_BUILDER_FUNCTION ||
        node->type == AST_CLOSURE) {
        return 0;
    }
    if (node->type == AST_TUPLE_DESTRUCTURE && node->child_count >= 2) {
        int var_count = node->child_count - 1;
        ASTNode* rhs = node->children[var_count];
        for (int j = 0; j < var_count; j++) {
            ASTNode* v = node->children[j];
            if (!v || !v->value || strcmp(v->value, var_name) != 0) {
                continue;
            }
            if (rhs && rhs->type == AST_FUNCTION_CALL && rhs->value) {
                const char* fn = codegen_normalise_callee(rhs->value);
                ASTNode* callee = find_function_definition_by_name(gen->program, fn);
                if (callee && function_def_returns_heap_at(gen, callee, j)) {
                    return 1;
                }
                if (!callee) {
                    ASTNode* ext = find_extern_declaration_by_name(gen->program, fn);
                    if (ext && ext->node_type &&
                        ext->node_type->kind == TYPE_TUPLE &&
                        j < ext->node_type->tuple_count &&
                        ext->node_type->tuple_heap_flags &&
                        ext->node_type->tuple_heap_flags[j]) {
                        return 1;
                    }
                }
            }
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        if (body_tuple_destructure_binds_heap(gen, classifier_child(gen, node, i),
                                              var_name)) {
            return 1;
        }
    }
    return 0;
}

/* #2514: is `expr` a string variable of `fn_name` that a closure writes?
 * Such a variable lives in a cell that owns what it holds (a store takes
 * the value) and is released at the function's exit, so `return s` hands
 * the caller a copy: the function is heap-returning. Resolved by the
 * function's name, context-free as the INVARIANT above asks. */
static int returns_promoted_string(CodeGenerator* gen, ASTNode* expr, const char* fn_name) {
    if (!expr || expr->type != AST_IDENTIFIER || !expr->value || !fn_name) return 0;
    char** promoted = NULL;
    int count = 0;
    get_promoted_names_for_func(gen, fn_name, &promoted, &count);
    for (int i = 0; i < count; i++) {
        if (!promoted[i] || strcmp(promoted[i], expr->value) != 0) continue;
        const char* ct = lookup_var_c_type(gen, expr->value, fn_name);
        return ct && strcmp(ct, "const char*") == 0;
    }
    return 0;
}

/* Heap evidence for a return-site expression inside the classifiers.
 * Bare identifiers MUST resolve structurally against the analysed
 * function's own body (see the INVARIANT above): is_heap_string_expr
 * consults the currently-emitting function's tracker table for
 * identifiers, which is the wrong function whenever classification is
 * triggered from a caller's destructure site. `fn_name` is the analysed
 * function's. */
static int returned_param_is_captured(CodeGenerator* gen, const char* name, const char* fn_name);

static int return_expr_is_heap(CodeGenerator* gen, ASTNode* expr,
                               ASTNode* fn_body_root, const char* fn_name) {
    if (!expr) return 0;
    if (expr->type == AST_IDENTIFIER) {
        return expr->value && fn_body_root &&
               (body_assigns_var_from_heap_or_catch(gen, fn_body_root, expr->value) ||
                body_tuple_destructure_binds_heap(gen, fn_body_root,
                                                  expr->value) ||
                returns_promoted_string(gen, expr, fn_name) ||
                /* A kept parameter is this function's own reference. */
                returned_param_is_captured(gen, expr->value, fn_name));
    }
    /* #2461: a field read is taken as a copy at the return site (the
     * struct, often this function's own local, frees its buffer at scope
     * exit), so it makes the function heap-returning; an `if` / `match`
     * does whenever one of its value arms does. */
    if (is_owned_string_field_read(expr)) return 1;
    if (expr->type == AST_IF_EXPRESSION && expr->child_count >= 3) {
        return return_expr_is_heap(gen, expr->children[1], fn_body_root, fn_name) ||
               return_expr_is_heap(gen, expr->children[2], fn_body_root, fn_name);
    }
    if (expr->type == AST_MATCH_STATEMENT) {
        for (int i = 1; i < expr->child_count; i++) {
            ASTNode* arm = expr->children[i];
            if (arm && arm->type == AST_MATCH_ARM && arm->child_count >= 2 &&
                match_arm_value(arm->children[1]) &&
                return_expr_is_heap(gen, match_arm_value(arm->children[1]),
                                    fn_body_root, fn_name)) return 1;
        }
        return 0;
    }
    return is_heap_string_expr(gen, expr);
}

static void walk_returns_for_heap_check_in(CodeGenerator* gen, ASTNode* node,
                                            const char* fn_being_analyzed,
                                            ASTNode* fn_body_root,
                                            int* any_heap, int* any_non_heap,
                                            CatchScope* cs);

static void walk_returns_for_heap_check(CodeGenerator* gen, ASTNode* node,
                                         const char* fn_being_analyzed,
                                         ASTNode* fn_body_root,
                                         int* any_heap, int* any_non_heap) {
    CatchScope cs = { {0}, 0 };
    walk_returns_for_heap_check_in(gen, node, fn_being_analyzed, fn_body_root,
                                   any_heap, any_non_heap, &cs);
}

static void walk_returns_for_heap_check_in(CodeGenerator* gen, ASTNode* node,
                                            const char* fn_being_analyzed,
                                            ASTNode* fn_body_root,
                                            int* any_heap, int* any_non_heap,
                                            CatchScope* cs) {
    if (!node) return;
    if (node->type == AST_RETURN_STATEMENT) {
        int is_heap = 0;
        if (node->child_count > 0 && node->children[0]) {
            ASTNode* ret = node->children[0];
            /* Self-recursive call: a `return f(...)` inside `f`'s
             * body. The recursion guard at function_def_returns_
             * heap_string returns 0 for f-during-f's-own-analysis,
             * which would falsely flag the function non-heap. The
             * self-recursive call's heap-ness IS the function's
             * heap-ness — recording it as non-heap here loses the
             * information from non-recursive return shapes. Skip
             * counting self-recursive returns entirely; the
             * function's overall classification is decided by
             * non-recursive returns. */
            int is_self_recursive_call = 0;
            if (ret && ret->type == AST_FUNCTION_CALL && ret->value &&
                fn_being_analyzed &&
                strcmp(ret->value, fn_being_analyzed) == 0) {
                is_self_recursive_call = 1;
            }
            if (is_self_recursive_call) {
                /* Optimistic heap-classification for self-recursive
                 * returns. The function's return-ness can't be
                 * resolved during its own analysis (cycle break),
                 * but a self-recursive `return f(...)` propagates
                 * whatever shape `f` returns — which IS the
                 * function's shape. Marking heap here is sound
                 * because the uniform-heap shim's cold path
                 * malloc-duplicates literal returns, so over-
                 * classifying costs at most one copy per literal-
                 * return path (avn-bench shape doesn't have one).
                 * Under-classifying (the alternative) causes UAF
                 * when the base case `return param` returns a
                 * buffer that the recursive wrap is about to
                 * free — see the walk_join trace in the v0.149
                 * lucky-UAF write-up. */
                is_heap = 1;
            } else if (catch_scope_has(cs, ret)) {
                /* `return e` inside `catch e`: the binding may own a
                 * heap-built reason (#2333). */
                is_heap = 1;
            } else if (return_expr_is_heap(gen, ret, fn_body_root, fn_being_analyzed)) {
                /* Heap evidence for the return expression. Bare
                 * identifiers resolve STRUCTURALLY against the
                 * analysed function's own body (declaration-from-heap
                 * or destructure-from-heap-position), never through
                 * `gen->heap_string_vars`: the memoised walk can be
                 * triggered from a caller's site during ANY function's
                 * emission, where the tracker table belongs to the
                 * wrong function (#1311). Covers the accumulator
                 * shape (`body = ""; ...; return body`, avn's
                 * `rebuild_dir` O(N²) leak) and the destructure shape
                 * (`v, e = g(...); return v`). */
                is_heap = 1;
            }
        }
        if (is_heap) *any_heap = 1;
        else         *any_non_heap = 1;
        return;
    }
    // Don't descend into nested function/lambda definitions.
    if (node->type == AST_FUNCTION_DEFINITION ||
        node->type == AST_BUILDER_FUNCTION ||
        node->type == AST_CLOSURE) {
        return;
    }
    int pushed = 0;
    if (node->type == AST_CATCH_CLAUSE && node->value && cs->count < CATCH_SCOPE_MAX) {
        cs->names[cs->count++] = node->value;
        pushed = 1;
    }
    for (int i = 0; i < node->child_count; i++) {
        walk_returns_for_heap_check_in(gen, classifier_child(gen, node, i),
                                       fn_being_analyzed, fn_body_root,
                                       any_heap, any_non_heap, cs);
    }
    if (pushed) cs->count--;
}

int function_def_returns_heap_string(CodeGenerator* gen, ASTNode* fn_def) {
    if (!fn_def ||
        (fn_def->type != AST_FUNCTION_DEFINITION &&
         fn_def->type != AST_BUILDER_FUNCTION)) {
        return 0;
    }
    // Memoised result on the annotation field. Three values:
    //   "heap_yes"     — at least one return yields a heap string;
    //                    caller owns the result (uniform-heap shim
    //                    guarantees the literal branch, if any, is
    //                    freshly malloc'd at the return site).
    //   "heap_no"      — every return yields a non-heap value;
    //                    caller does not free.
    //   "heap_pending" — currently being analysed (cycle break).
    if (fn_def->annotation) {
        if (strcmp(fn_def->annotation, "heap_yes") == 0)     return 1;
        if (strcmp(fn_def->annotation, "heap_no") == 0)      return 0;
        if (strcmp(fn_def->annotation, "heap_pending") == 0) return 0;
        // Some other annotation (e.g. "c_callback:..."). Don't clobber
        // — analyse afresh, but skip caching to preserve the original
        // annotation for downstream codegen.
    }
    int memoise = (fn_def->annotation == NULL);
    if (memoise) fn_def->annotation = strdup("heap_pending");

    ASTNode* body = NULL;
    for (int i = 0; i < fn_def->child_count; i++) {
        ASTNode* c = fn_def->children[i];
        if (c && c->type == AST_BLOCK) { body = c; break; }
    }
    int any_heap = 0;
    int any_non_heap = 0;
    if (body) walk_returns_for_heap_check(gen, body, fn_def->value, body,
                                          &any_heap, &any_non_heap);
    int result = any_heap ? 1 : 0;

    if (memoise) {
        free(fn_def->annotation);
        fn_def->annotation = strdup(result ? "heap_yes" : "heap_no");
    }
    return result;
}

/* Emit conditional `free()` for every return-escape-marked heap
 * string var that is NOT the variable this return statement is
 * about to return. Used at every AST_RETURN_STATEMENT codegen site
 * (the function-exit defer-free pre-pass is suppressed for these
 * names — see push_heap_string_exit_free_defers / the OR-fold
 * walker) so a function that has TWO return paths where only one
 * returns the tracked local doesn't leak the local on the other
 * path. The avn-bench `find_decorated(prefix, n)` shape:
 *
 *   while i < n {
 *       candidate = string.concat(prefix, "_match")   // _heap_candidate=1
 *       if i == 5 { return candidate }                // owned by caller
 *       i = i + 1
 *   }
 *   return string.concat(prefix, "_fallback")        // <- pre-fix leak:
 *                                                    //    last candidate buffer
 *
 * Without this drain, the after-loop return leaves the last loop
 * iteration's `candidate` buffer unreferenced. The reassign-wrapper
 * inside the loop frees iter-by-iter; the loop's natural exit
 * leaves the final buffer with no consumer.
 *
 * The return-statement codegen drains EVERY return-escape var
 * other than the one bound to this return's bare-identifier expr;
 * if the return expression is a non-identifier (concat result,
 * interp, user-fn call), every return-escape var is drained
 * because none of them owns the returned value. */
static void emit_return_escape_drains_for_unreturned(CodeGenerator* gen,
                                                     ASTNode* return_expr) {
    if (!gen) return;
    const char* preserve = NULL;
    if (return_expr && return_expr->type == AST_IDENTIFIER && return_expr->value) {
        preserve = return_expr->value;
    }
    for (int i = 0; i < gen->return_escaped_string_var_count; i++) {
        const char* name = gen->return_escaped_string_vars[i];
        if (!name) continue;
        if (preserve && strcmp(name, preserve) == 0) continue;
        print_indent(gen);
        fprintf(gen->output,
                "if (_heap_%s) { aether_heap_str_free(%s); %s = NULL; _heap_%s = 0; }\n",
                name, name, name, name);
    }
}

/* The multi-value form of the drain above, for `return e0, e1, ...`.
 * Every return-escape var that no position of THIS return hands over as
 * a bare identifier is freed: `m = string.concat(...); if c { return n, m }
 * ... return 0, ""` otherwise leaked `m` on the second path (the
 * function-exit free is suppressed for `m` because the first path returns
 * it). Emitted after the tuple value is built, so a position that merely
 * reads a drained var (`return string.length(m), ""`) reads it first.
 * Returns how many drains it emitted.
 *
 * A var that also escaped into a container (call argument, field, capture)
 * is left alone: its buffer may be owned elsewhere, so a missed free is
 * the safe side. The single-value drain predates that check and keeps its
 * own contract. */
static int count_tuple_return_drains(CodeGenerator* gen, ASTNode* stmt) {
    int n = 0;
    for (int i = 0; gen && i < gen->return_escaped_string_var_count; i++) {
        const char* name = gen->return_escaped_string_vars[i];
        if (!name || is_escaped_string_var(gen, name)) continue;
        int returned = 0;
        for (int j = 0; j < stmt->child_count && !returned; j++) {
            ASTNode* c = stmt->children[j];
            returned = c && c->type == AST_IDENTIFIER && c->value &&
                       strcmp(c->value, name) == 0;
        }
        if (!returned) n++;
    }
    return n;
}

static void emit_tuple_return_escape_drains(CodeGenerator* gen, ASTNode* stmt) {
    for (int i = 0; gen && i < gen->return_escaped_string_var_count; i++) {
        const char* name = gen->return_escaped_string_vars[i];
        if (!name || is_escaped_string_var(gen, name)) continue;
        int returned = 0;
        for (int j = 0; j < stmt->child_count && !returned; j++) {
            ASTNode* c = stmt->children[j];
            returned = c && c->type == AST_IDENTIFIER && c->value &&
                       strcmp(c->value, name) == 0;
        }
        if (returned) continue;
        print_indent(gen);
        fprintf(gen->output,
                "if (_heap_%s) { aether_heap_str_free(%s); %s = NULL; _heap_%s = 0; }\n",
                name, name, name, name);
    }
}

/* Struct-field heap-string assignment helper (#465).
 *
 * For an assignment `<var>.<field> = <rhs>` where `<var>` is a
 * struct local whose definition has `<field>: string`, emit:
 *
 *     { const char* _tmp_old = <var>.<field>;
 *       <var>.<field> = <rhs>;
 *       if (<var>._heap_<field>) free((void*)_tmp_old);
 *       <var>._heap_<field> = <rhs_is_heap>; }
 *
 * Returns 1 if the wrap was emitted (caller skips the bare
 * assignment), 0 if the LHS shape isn't one of the recognised
 * struct-field patterns (caller falls through to the existing
 * bare-assignment emission). */
/* #1866: is `name` a POINTER-TO-STRUCT PARAMETER of the function being
 * emitted? A setter takes the box as a parameter, so the field assignment
 * inside it is written through a parameter rather than a local, and the
 * ownership wrapper below used to skip it: the tracker was never set and the
 * box's destructor believed it owned nothing. That left no correct way to
 * write a destructor at all, since freeing the field by hand double-frees
 * when the assignment was written on a local and leaks when it went through a
 * setter.
 *
 * Locals stay excluded, so a raw `malloc(...) as *T` local keeps the #790
 * treatment. That guard is narrower than it looks either way: the generated
 * destructor reads the same `_heap_<field>` tracker unconditionally at free
 * time, so a non-zeroed box is already unsound there regardless of what this
 * assignment site does. heap.new zero-inits, which is the documented way to
 * make one. */
static int is_ptr_struct_param(CodeGenerator* gen, const char* name) {
    if (!gen || !gen->current_function || !name) return 0;
    ASTNode* fn = gen->current_function;
    for (int i = 0; i < fn->child_count; i++) {
        ASTNode* p = fn->children[i];
        /* Parameters come from parse_pattern, so a plain `name: T` is a
         * pattern-variable node rather than a dedicated parameter node. The
         * body is the last child and is not a parameter. */
        if (!p || !p->value) continue;
        if (p->type != AST_PATTERN_VARIABLE && p->type != AST_IDENTIFIER) continue;
        if (strcmp(p->value, name) != 0) continue;
        Type* t = p->node_type;
        return t && t->kind == TYPE_PTR && t->element_type &&
               t->element_type->kind == TYPE_STRUCT &&
               t->element_type->struct_name != NULL;
    }
    return 0;
}

/* #2369: zero-initialised box provenance.
 *
 * Releasing a string field's previous value reads the box's `_heap_<field>`
 * tracker, and that read is only sound on a box whose trackers are known to
 * be initialised (#1873): heap.new(T) calloc's the box, and every Aether store
 * after that keeps the trackers truthful, while `malloc(n) as *T` leaves them
 * garbage. #790 could only see that for a local bound to heap.new in the same
 * function. A box returned by a constructor, held in another struct's pointer
 * field, or passed through a `ptr` and cast back was of unknown origin, so the
 * store never released the old value and every replaced string leaked.
 *
 * zb_expr answers "does this expression always yield a heap.new(T) box, or
 * null?" by following the value back to where it was made:
 *   - heap.new(T), and null / none (a store through null faults anyway);
 *   - a cast (`x as *T`, `x as ptr`) of such a value. The cast does not
 *     touch the memory, but a box of another struct has its trackers at other
 *     offsets, so the struct must be the same T;
 *   - a call to an Aether function every `return` of which is such a value;
 *   - a local every binding of which, anywhere in its function, is such a
 *     value (flow-insensitive, so a loop that rebinds it later still counts);
 *   - a struct field every store into which, anywhere in the program (struct
 *     literals included), is such a value.
 * Everything else, a parameter, a C extern's result, a list element, a
 * global, is of unknown origin and answers no, which keeps the #1873
 * behaviour for it. "No" is always the safe answer, so a cycle or the depth
 * limit resolves to it. */
#define ZB_MAX_DEPTH 24
#define ZB_IN_PROGRESS ((void*)1)
#define ZB_YES ((void*)2)
#define ZB_NO ((void*)3)

static int zb_expr(CodeGenerator* gen, ASTNode* e, ASTNode* ctx,
                   const char* sname, int depth);

static const char* zb_struct_of(Type* t) {
    if (!t) return NULL;
    if (t->kind == TYPE_STRUCT) return t->struct_name;
    if (t->kind == TYPE_PTR && t->element_type &&
        t->element_type->kind == TYPE_STRUCT) return t->element_type->struct_name;
    return NULL;
}

static int zb_is_name(ASTNode* n, const char* name) {
    return n && n->type == AST_IDENTIFIER && n->value && strcmp(n->value, name) == 0;
}

static int zb_mentions(ASTNode* n, const char* name) {
    if (!n) return 0;
    if (n->value && strcmp(n->value, name) == 0) return 1;
    for (int i = 0; i < n->child_count; i++) {
        if (zb_mentions(n->children[i], name)) return 1;
    }
    return 0;
}

/* A node whose `value` names something other than a binding of a local:
 * a use, a field, a callee, a type. Any other node spelling the name (a
 * pattern variable, a closure parameter, a catch binding) binds it in a way
 * the walk below does not follow, so the local is of unknown origin. */
static int zb_value_not_a_binding(ASTNode* n) {
    switch (n->type) {
        case AST_IDENTIFIER: case AST_MEMBER_ACCESS: case AST_OPTIONAL_CHAIN:
        case AST_FUNCTION_CALL: case AST_LITERAL: case AST_NAMED_ARG:
        case AST_HEAP_NEW: case AST_STRUCT_LITERAL: case AST_PTR_AS_STRUCT_CAST:
        case AST_SIZEOF: case AST_OFFSETOF:
            return 1;
        default:
            return 0;
    }
}

/* Is every binding of local `name` under `n` a zeroed `sname` box? Sets
 * `*found` when it sees one, so a name never bound here (a global, a capture
 * the closure does not bind) is not mistaken for a local. */
static int zb_bindings(CodeGenerator* gen, ASTNode* n, ASTNode* ctx, const char* name,
                       const char* sname, int depth, int* found) {
    if (!n) return 1;
    switch (n->type) {
        case AST_VARIABLE_DECLARATION:
            if (n->value && strcmp(n->value, name) == 0) {
                *found = 1;
                /* No initialiser: a tuple-destructure target or a bare typed
                 * declaration, bound to something this walk cannot see. */
                if (n->child_count == 0 || !n->children[0]) return 0;
                if (!zb_expr(gen, n->children[0], ctx, sname, depth + 1)) return 0;
            }
            break;
        case AST_ASSIGNMENT:
        case AST_BINARY_EXPRESSION:
            if (n->child_count >= 2 && zb_is_name(n->children[0], name) &&
                (n->type == AST_ASSIGNMENT || (n->value && strcmp(n->value, "=") == 0))) {
                *found = 1;
                if (!zb_expr(gen, n->children[1], ctx, sname, depth + 1)) return 0;
            }
            break;
        case AST_COMPOUND_ASSIGNMENT:
            if (n->child_count >= 1 && zb_is_name(n->children[0], name)) return 0;
            break;
        case AST_UNARY_EXPRESSION:
            /* `&b` can be written through; `b++` rebinds it. */
            if (n->child_count >= 1 && zb_is_name(n->children[0], name) && n->value &&
                (strcmp(n->value, "&") == 0 || strcmp(n->value, "++") == 0 ||
                 strcmp(n->value, "--") == 0)) return 0;
            break;
        default:
            if (n->value && strcmp(n->value, name) == 0 && !zb_value_not_a_binding(n))
                return 0;
            break;
    }
    for (int i = 0; i < n->child_count; i++) {
        if (!zb_bindings(gen, n->children[i], ctx, name, sname, depth, found)) return 0;
    }
    return 1;
}

static int zb_memo_get(CodeGenerator* gen, const char* key, int* answer) {
    void* v = strmap_get(&gen->zeroed_box_memo, key);
    if (!v) return 0;
    *answer = (v == ZB_YES);
    return 1;
}

static int zb_contains(ASTNode* n, ASTNode* target) {
    if (!n) return 0;
    if (n == target) return 1;
    for (int i = 0; i < n->child_count; i++) {
        if (zb_contains(n->children[i], target)) return 1;
    }
    return 0;
}

/* The function, main or builder whose body holds `closure`, or NULL (a
 * closure in an actor handler or a global initialiser). */
static ASTNode* zb_enclosing_function(CodeGenerator* gen, ASTNode* closure) {
    if (!gen->program) return NULL;
    char key[64];
    snprintf(key, sizeof(key), "P|%p", (void*)closure);
    void* v = strmap_get(&gen->zeroed_box_memo, key);
    if (v) return v == ZB_NO ? NULL : (ASTNode*)v;
    ASTNode* found = NULL;
    for (int i = 0; !found && i < gen->program->child_count; i++) {
        ASTNode* d = gen->program->children[i];
        if (d && (d->type == AST_FUNCTION_DEFINITION || d->type == AST_MAIN_FUNCTION ||
                  d->type == AST_BUILDER_FUNCTION) && zb_contains(d, closure))
            found = d;
    }
    strmap_put(&gen->zeroed_box_memo, key, found ? (void*)found : ZB_NO);
    return found;
}

static int zb_param_holds_boxes(CodeGenerator* gen, ASTNode* fn, const char* name,
                                const char* sname);

static int zb_local(CodeGenerator* gen, ASTNode* ctx, const char* name,
                    const char* sname, int depth) {
    if (!ctx || !name) return 0;
    if (is_module_global_var(gen, name)) return 0;
    /* Inside a closure the name is either the closure's own or a capture,
     * and a closure's write to a capture is a write to the captured
     * variable (#2458). Both are bindings somewhere in the enclosing
     * function's body, closures included, so that body is the scope to
     * walk; a closure parameter of the name anywhere in it is refused by
     * the walk, which keeps a shadowing parameter from passing as the
     * captured box. */
    if (ctx->type == AST_CLOSURE) ctx = zb_enclosing_function(gen, ctx);
    if (!ctx) return 0;
    ASTNode* body;
    int is_param = 0;
    if (ctx->type == AST_FUNCTION_DEFINITION || ctx->type == AST_MAIN_FUNCTION ||
        ctx->type == AST_BUILDER_FUNCTION) {
        if (ctx->child_count == 0) return 0;
        /* A parameter: its value is whatever the callers pass, a box only
         * where every call passes one (zb_param_holds_boxes), and then
         * whatever the body rebinds it to. */
        for (int i = 0; i < ctx->child_count - 1; i++) {
            if (zb_mentions(ctx->children[i], name)) {
                if (!zb_param_holds_boxes(gen, ctx, name, sname)) return 0;
                is_param = 1;
                break;
            }
        }
        body = ctx->children[ctx->child_count - 1];
    } else {
        return 0;
    }
    /* The map copies its keys, so each is built whole and freed here. */
    char* key = heap_strf("L|%p|%s|%s", (void*)ctx, name, sname);
    int answer;
    if (zb_memo_get(gen, key, &answer)) { free(key); return answer; }
    strmap_put(&gen->zeroed_box_memo, key, ZB_IN_PROGRESS);
    int found = 0;
    answer = zb_bindings(gen, body, ctx, name, sname, depth, &found) && (found || is_param);
    strmap_put(&gen->zeroed_box_memo, key, answer ? ZB_YES : ZB_NO);
    free(key);
    return answer;
}

static int zb_returns(CodeGenerator* gen, ASTNode* n, ASTNode* fn, const char* sname,
                      int depth, int* saw) {
    if (!n) return 1;
    if (n->type == AST_CLOSURE) return 1;   /* its returns are its own */
    if (n->type == AST_RETURN_STATEMENT) {
        *saw = 1;
        return n->child_count > 0 && zb_expr(gen, n->children[0], fn, sname, depth + 1);
    }
    for (int i = 0; i < n->child_count; i++) {
        if (!zb_returns(gen, n->children[i], fn, sname, depth, saw)) return 0;
    }
    return 1;
}

/* Does anything in the program bind `name` as a variable: a local, a
 * parameter, a closure parameter, a pattern, a global? A call spelled with
 * such a name may invoke that variable's closure rather than the top-level
 * function of the same name, so the function's returns say nothing about it. */
static int zb_binds_name(ASTNode* n, const char* name) {
    if (!n) return 0;
    switch (n->type) {
        case AST_FUNCTION_DEFINITION: case AST_BUILDER_FUNCTION:
            for (int i = 0; i + 1 < n->child_count; i++) {
                if (zb_mentions(n->children[i], name)) return 1;
            }
            break;
        case AST_ASSIGNMENT: case AST_BINARY_EXPRESSION:
            if (n->child_count >= 2 && zb_is_name(n->children[0], name) &&
                (n->type == AST_ASSIGNMENT || (n->value && strcmp(n->value, "=") == 0)))
                return 1;
            break;
        case AST_IDENTIFIER: case AST_MEMBER_ACCESS: case AST_OPTIONAL_CHAIN:
        case AST_FUNCTION_CALL: case AST_LITERAL: case AST_NAMED_ARG:
            break;
        default:
            /* Declarations, closure parameters, patterns, state. */
            if (n->value && strcmp(n->value, name) == 0) return 1;
            break;
    }
    for (int i = 0; i < n->child_count; i++) {
        if (zb_binds_name(n->children[i], name)) return 1;
    }
    return 0;
}

static int zb_call(CodeGenerator* gen, const char* name, const char* sname, int depth) {
    if (!name || !gen->program) return 0;
    char* key = heap_strf("R|%s|%s", name, sname);
    int answer;
    if (zb_memo_get(gen, key, &answer)) { free(key); return answer; }
    char* bkey = heap_strf("B|%s", name);
    int bound;
    if (!zb_memo_get(gen, bkey, &bound)) {
        bound = is_module_global_var(gen, name);
        for (int i = 0; !bound && i < gen->program->child_count; i++) {
            ASTNode* d = gen->program->children[i];
            /* The definitions themselves name it; look inside them. */
            if (d && (d->type == AST_FUNCTION_DEFINITION || d->type == AST_EXTERN_FUNCTION ||
                      d->type == AST_BUILDER_FUNCTION) &&
                d->value && strcmp(d->value, name) == 0) {
                for (int j = 0; !bound && j < d->child_count; j++)
                    bound = zb_binds_name(d->children[j], name);
                continue;
            }
            bound = zb_binds_name(d, name);
        }
        strmap_put(&gen->zeroed_box_memo, bkey, bound ? ZB_YES : ZB_NO);
    }
    free(bkey);
    if (bound) {
        strmap_put(&gen->zeroed_box_memo, key, ZB_NO);
        free(key);
        return 0;
    }
    strmap_put(&gen->zeroed_box_memo, key, ZB_IN_PROGRESS);
    /* Every definition of the name must qualify (a guarded function has one
     * per clause); an extern or anything else of that name does not. */
    int defs = 0;
    answer = 1;
    for (int i = 0; answer && i < gen->program->child_count; i++) {
        ASTNode* d = gen->program->children[i];
        if (!d || !d->value || strcmp(d->value, name) != 0) continue;
        if (d->type != AST_FUNCTION_DEFINITION || d->child_count == 0) { answer = 0; break; }
        int saw = 0;
        answer = zb_returns(gen, d->children[d->child_count - 1], d, sname, depth, &saw) && saw;
        defs++;
    }
    answer = answer && defs > 0;
    strmap_put(&gen->zeroed_box_memo, key, answer ? ZB_YES : ZB_NO);
    free(key);
    return answer;
}

/* Does `lhs` name field `field` of struct `owner`? 1 yes, 0 no, -1 when it
 * names a field of that name on an object whose type is not known. */
static int zb_names_field(ASTNode* lhs, const char* owner, const char* field) {
    if (!lhs || !lhs->value || strcmp(lhs->value, field) != 0) return 0;
    if (lhs->type == AST_OPTIONAL_CHAIN) return -1;
    if (lhs->type != AST_MEMBER_ACCESS) return 0;
    const char* s = (lhs->child_count > 0 && lhs->children[0])
                    ? zb_struct_of(lhs->children[0]->node_type) : NULL;
    if (!s) return -1;
    return strcmp(s, owner) == 0;
}

static int zb_field_stores(CodeGenerator* gen, ASTNode* n, ASTNode* ctx, const char* owner,
                           const char* field, const char* sname, int depth, int* saw) {
    if (!n) return 1;
    /* A closure keeps its enclosing function as the scope (see zb_local). */
    if (n->type == AST_FUNCTION_DEFINITION || n->type == AST_MAIN_FUNCTION ||
        n->type == AST_BUILDER_FUNCTION) {
        ctx = n;
    } else if (n->type == AST_ACTOR_DEFINITION) {
        ctx = NULL;
    }
    if ((n->type == AST_ASSIGNMENT ||
         (n->type == AST_BINARY_EXPRESSION && n->value && strcmp(n->value, "=") == 0)) &&
        n->child_count >= 2) {
        int hit = zb_names_field(n->children[0], owner, field);
        if (hit < 0) return 0;
        if (hit && !zb_expr(gen, n->children[1], ctx, sname, depth + 1)) return 0;
        if (hit) *saw = 1;
    }
    if ((n->type == AST_COMPOUND_ASSIGNMENT ||
         (n->type == AST_UNARY_EXPRESSION && n->value && strcmp(n->value, "&") == 0)) &&
        n->child_count >= 1 && zb_names_field(n->children[0], owner, field) != 0) {
        return 0;
    }
    if (n->type == AST_STRUCT_LITERAL) {
        const char* lit = zb_struct_of(n->node_type);
        for (int i = 0; i < n->child_count; i++) {
            ASTNode* fi = n->children[i];
            if (!fi || fi->type != AST_ASSIGNMENT || fi->child_count < 1 || !fi->value ||
                strcmp(fi->value, field) != 0) continue;
            if (!lit && !(n->value && strcmp(n->value, owner) == 0)) return 0;
            if (lit && strcmp(lit, owner) != 0) continue;
            if (!zb_expr(gen, fi->children[0], ctx, sname, depth + 1)) return 0;
            *saw = 1;
        }
    }
    for (int i = 0; i < n->child_count; i++) {
        if (!zb_field_stores(gen, n->children[i], ctx, owner, field, sname, depth, saw))
            return 0;
    }
    return 1;
}

static int zb_field(CodeGenerator* gen, const char* owner, const char* field,
                    const char* sname, int depth) {
    if (!gen->program) return 0;
    char* key = heap_strf("F|%s|%s|%s", owner, field, sname);
    int answer;
    if (zb_memo_get(gen, key, &answer)) { free(key); return answer; }
    strmap_put(&gen->zeroed_box_memo, key, ZB_IN_PROGRESS);
    /* A field no Aether code stores into is filled by C, raw memory writes
     * or nothing at all: of unknown origin, not vacuously a box. */
    int saw = 0;
    answer = zb_field_stores(gen, gen->program, NULL, owner, field, sname, depth, &saw) && saw;
    strmap_put(&gen->zeroed_box_memo, key, answer ? ZB_YES : ZB_NO);
    free(key);
    return answer;
}

static int zb_expr(CodeGenerator* gen, ASTNode* e, ASTNode* ctx,
                   const char* sname, int depth) {
    if (!e || !sname || depth > ZB_MAX_DEPTH) return 0;
    switch (e->type) {
        case AST_NULL_LITERAL:
        case AST_NONE_LITERAL:
            return 1;
        case AST_HEAP_NEW: {
            const char* made = NULL;
            if (e->node_type && e->node_type->kind == TYPE_PTR)
                made = zb_struct_of(e->node_type);
            if (!made) made = e->value;
            return made && strcmp(made, sname) == 0;
        }
        case AST_PTR_AS_STRUCT_CAST:
            if (!e->value || strcmp(e->value, sname) != 0) return 0;
            return e->child_count > 0 && zb_expr(gen, e->children[0], ctx, sname, depth + 1);
        case AST_VALUE_CAST:
            if (!e->node_type || e->node_type->kind != TYPE_PTR) return 0;
            return e->child_count > 0 && zb_expr(gen, e->children[0], ctx, sname, depth + 1);
        case AST_IDENTIFIER:
            return zb_local(gen, ctx, e->value, sname, depth + 1);
        case AST_FUNCTION_CALL:
            return zb_call(gen, e->value, sname, depth + 1);
        case AST_MEMBER_ACCESS: {
            if (!e->value || e->child_count < 1 || !e->children[0]) return 0;
            const char* owner = zb_struct_of(e->children[0]->node_type);
            return owner && zb_field(gen, owner, e->value, sname, depth + 1);
        }
        default:
            return 0;
    }
}

/* #2369: is the struct pointer `obj`, about to be stored through, known to
 * be a heap.new box of its own struct type, so its trackers can be read? */
/* Which pointer-to-struct parameters only ever hold a heap.new box (#2369).
 *
 * A parameter holds what its callers pass, so it holds only boxes when every
 * call passes one: a box made there, or a parameter of the calling function
 * that in turn holds only boxes. That needs every call to be visible, so a
 * function qualifies only when the program calls it directly and in no
 * other way:
 *   - it has internal linkage (fn_has_internal_linkage: an imported
 *     module's function, or a file-local `name_`). A top-level function of
 *     the entry file is an external symbol C may call (#703), from an
 *     `--extra` file or a plugin, with a struct it allocated itself;
 *   - not in a library or header build (C calls those through stubs);
 *   - one definition taking plain parameters, not a builder (its `_ctx` is
 *     injected, shifting every argument), never given a trailing block;
 *   - its name used nowhere but as a callee: a value, a variable, a field, a
 *     literal or an annotation spelling it, or an extern of that name in
 *     any module, could reach a call this walk does not see;
 *   - every call passing one argument per parameter (a named argument is
 *     not followed; a default is filled in before codegen).
 * A function nothing calls stays unknown too.
 *
 * Parameters that pass themselves on form a graph, recursion included. Each
 * starts as "only boxes"; a call passing anything else marks its parameter
 * unknown, and unknown spreads along the graph until nothing changes. What
 * is left holds only boxes, since every value a parameter can hold comes in
 * through some call from outside the cycle it sits on. The arguments are
 * judged by zb_expr while every parameter answers "unknown", the safe
 * answer; the memo those answers filled is dropped afterwards, so later
 * questions see the parameters' own. */
typedef struct {
    const char* fn;     /* the function's name */
    int idx;            /* the parameter's position */
    const char* sname;  /* the struct it points to */
    int unknown;        /* some call may pass something other than a box */
    int* feeds;         /* the parameters it is passed on to */
    int feed_count;
    int feed_cap;
} ZbParam;

typedef struct {
    StrMap at;          /* "fn|idx" -> index into params, plus 1 */
    ZbParam* params;
    int count;
    int cap;
} ZbParams;

typedef struct {
    ASTNode* call;
    ASTNode* ctx;       /* the innermost closure around the call, else fn */
    ASTNode* fn;        /* the top-level function, main or builder, or NULL */
    int next;           /* the next call of the same callee, or -1 */
} ZbSite;

typedef struct {
    StrMap mentions;    /* every name spelled other than as a callee */
    StrMap first;       /* callee -> index of its last-seen call, plus 1 */
    ZbSite* sites;
    int site_count;
    int site_cap;
    const char** notes; /* annotations, which may name a function */
    int note_count;
    int note_cap;
} ZbScan;

/* A name as `codegen_normalise_callee` would spell it, without interning
 * it: every identifier and literal of the program passes through here. */
static void zb_scan_mention_normalised(ZbScan* sc, const char* name) {
    size_t n = strlen(name);
    char* tmp = (char*)aether_xrealloc(NULL, n + 1);
    for (size_t i = 0; i <= n; i++) tmp[i] = name[i] == '.' ? '_' : name[i];
    strmap_put(&sc->mentions, tmp, ZB_YES);
    free(tmp);
}

void zb_params_free(CodeGenerator* gen) {
    ZbParams* zp = (ZbParams*)gen->zb_params;
    if (!zp) return;
    for (int i = 0; i < zp->count; i++) free(zp->params[i].feeds);
    free(zp->params);
    strmap_free(&zp->at);
    free(zp);
    gen->zb_params = NULL;
}

static void zb_scan_mention(ZbScan* sc, const char* name) {
    if (!name || !name[0]) return;
    strmap_put(&sc->mentions, name, ZB_YES);
    if (strchr(name, '.')) zb_scan_mention_normalised(sc, name);
}

/* `a.b.c` spelled by a member-access chain over an identifier, or NULL. */
static char* zb_dotted_path(ASTNode* n) {
    if (!n || !n->value) return NULL;
    if (n->type == AST_IDENTIFIER) return strdup(n->value);
    if (n->type != AST_MEMBER_ACCESS || n->child_count != 1) return NULL;
    char* base = zb_dotted_path(n->children[0]);
    if (!base) return NULL;
    char* path = heap_strf("%s.%s", base, n->value);
    free(base);
    return path;
}

static void zb_scan_note(ZbScan* sc, const char* note) {
    if (!note) return;
    if (sc->note_count == sc->note_cap) {
        sc->note_cap = sc->note_cap ? sc->note_cap * 2 : 64;
        sc->notes = aether_xrealloc(sc->notes, sizeof(*sc->notes) * sc->note_cap);
    }
    sc->notes[sc->note_count++] = note;
}

static void zb_scan(ZbScan* sc, ASTNode* n, ASTNode* fn, ASTNode* ctx) {
    if (!n) return;
    zb_scan_note(sc, n->annotation);
    if (n->type == AST_FUNCTION_CALL && n->value) {
        const char* callee = codegen_normalise_callee(n->value);
        if (sc->site_count == sc->site_cap) {
            sc->site_cap = sc->site_cap ? sc->site_cap * 2 : 256;
            sc->sites = aether_xrealloc(sc->sites, sizeof(*sc->sites) * sc->site_cap);
        }
        void* last = strmap_get(&sc->first, callee);
        ZbSite* s = &sc->sites[sc->site_count];
        s->call = n;
        s->ctx = ctx;
        s->fn = fn;
        s->next = last ? (int)(intptr_t)last - 1 : -1;
        strmap_put(&sc->first, callee, (void*)(intptr_t)(sc->site_count + 1));
        sc->site_count++;
    } else if (n->value) {
        zb_scan_mention(sc, n->value);
        if (n->type == AST_MEMBER_ACCESS) {
            char* path = zb_dotted_path(n);
            zb_scan_mention(sc, path);
            free(path);
        }
    }
    ASTNode* inner = n->type == AST_CLOSURE ? n : ctx;
    for (int i = 0; i < n->child_count; i++) zb_scan(sc, n->children[i], fn, inner);
}

static int zb_is_param_node(ASTNode* c) {
    switch (c->type) {
        case AST_VARIABLE_DECLARATION: case AST_PATTERN_VARIABLE: case AST_PATTERN_LITERAL:
        case AST_PATTERN_STRUCT: case AST_PATTERN_LIST: case AST_PATTERN_CONS:
            return 1;
        default:
            return 0;
    }
}

/* The parameter count of a function whose parameters are all plain names,
 * or -1 (a pattern parameter is matched, not bound by position). */
static int zb_plain_param_count(ASTNode* fn) {
    int k = 0;
    for (int i = 0; i + 1 < fn->child_count; i++) {
        ASTNode* c = fn->children[i];
        if (!c || !zb_is_param_node(c)) continue;
        if (c->type != AST_VARIABLE_DECLARATION && c->type != AST_PATTERN_VARIABLE) return -1;
        if (!c->value) return -1;
        k++;
    }
    return k;
}

static ASTNode* zb_param_node(ASTNode* fn, int pos) {
    int k = 0;
    for (int i = 0; i + 1 < fn->child_count; i++) {
        ASTNode* c = fn->children[i];
        if (!c || !zb_is_param_node(c)) continue;
        if (k++ == pos) return c;
    }
    return NULL;
}

static int zb_param_pos(ASTNode* fn, const char* name) {
    int k = 0;
    for (int i = 0; i + 1 < fn->child_count; i++) {
        ASTNode* c = fn->children[i];
        if (!c || !zb_is_param_node(c)) continue;
        if (c->value && strcmp(c->value, name) == 0) return k;
        k++;
    }
    return -1;
}

static ZbParam* zb_param_find(ZbParams* zp, const char* fn, int idx) {
    char* key = heap_strf("%s|%d", fn, idx);
    void* v = strmap_get(&zp->at, key);
    free(key);
    return v ? &zp->params[(int)(intptr_t)v - 1] : NULL;
}

static int zb_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_';
}

/* Does annotation `note` spell `name` as a whole identifier? */
static int zb_note_names(const char* note, const char* name) {
    size_t n = strlen(name);
    for (const char* at = strstr(note, name); at; at = strstr(at + 1, name)) {
        if ((at == note || !zb_ident_char(at[-1])) && !zb_ident_char(at[n])) return 1;
    }
    return 0;
}

/* Does function definition `fn` qualify (see above), given the scan? The
 * cheap tests first: most functions fail one of them. */
static int zb_fn_qualifies(CodeGenerator* gen, ZbScan* sc, ASTNode* fn) {
    if (!fn->value || !fn_has_internal_linkage(fn) || is_builder_func_reg(gen, fn->value))
        return 0;
    int n = zb_plain_param_count(fn);
    void* first = strmap_get(&sc->first, fn->value);
    if (n < 1 || !first) return 0;
    ASTNode* p0 = zb_param_node(fn, 0);
    if (!p0 || (p0->value && strcmp(p0->value, "_ctx") == 0)) return 0;
    const DefClauses* dc = program_index_clauses(gen->program, fn->value);
    if (!dc || dc->count != 1 || dc->nodes[0] != fn) return 0;
    if (strmap_has(&sc->mentions, fn->value)) return 0;
    for (int s = (int)(intptr_t)first - 1; s >= 0; s = sc->sites[s].next) {
        ASTNode* call = sc->sites[s].call;
        if (call->child_count != n) return 0;
        for (int a = 0; a < call->child_count; a++) {
            ASTNode* arg = call->children[a];
            if (!arg || arg->type == AST_NAMED_ARG ||
                (arg->type == AST_CLOSURE && arg->value && strcmp(arg->value, "trailing") == 0))
                return 0;
        }
    }
    for (int i = 0; i < sc->note_count; i++) {
        if (zb_note_names(sc->notes[i], fn->value)) return 0;
    }
    return 1;
}

/* Does `fn` take a pointer-to-struct parameter, the only kind asked about? */
static int zb_fn_takes_box(ASTNode* fn) {
    int n = zb_plain_param_count(fn);
    for (int k = 0; k < n; k++) {
        ASTNode* p = zb_param_node(fn, k);
        if (p && p->node_type && p->node_type->kind == TYPE_PTR && zb_struct_of(p->node_type))
            return 1;
    }
    return 0;
}

static void zb_params_build(CodeGenerator* gen) {
    ZbParams* zp = aether_xrealloc(NULL, sizeof(ZbParams));
    memset(zp, 0, sizeof(*zp));
    strmap_init(&zp->at);
    gen->zb_params = zp;
    /* A library or header build is called from C, which this walk cannot
     * see: every parameter stays of unknown origin. */
    if (!gen->program || gen->emit_lib || gen->emit_header || gen->csrc_header_file ||
        gen->csrc_catalog_file) {
        gen->zb_params_state = 2;
        return;
    }

    ZbScan sc;
    memset(&sc, 0, sizeof(sc));
    strmap_init(&sc.mentions);
    strmap_init(&sc.first);
    for (int i = 0; i < gen->program->child_count; i++) {
        ASTNode* d = gen->program->children[i];
        if (!d) continue;
        if (d->type == AST_FUNCTION_DEFINITION || d->type == AST_MAIN_FUNCTION ||
            d->type == AST_BUILDER_FUNCTION) {
            /* The definition spells its own name; everything in it counts. */
            zb_scan_note(&sc, d->annotation);
            for (int j = 0; j < d->child_count; j++) zb_scan(&sc, d->children[j], d, d);
        } else {
            zb_scan(&sc, d, NULL, NULL);
        }
    }
    /* Externs stay in their modules' trees: one spelling a function's
     * symbol (`@extern("name")` or the same name) would call it from
     * where the walk cannot see. */
    for (int m = 0; global_module_registry && m < global_module_registry->module_count; m++) {
        AetherModule* mod = global_module_registry->modules[m];
        for (int i = 0; mod && mod->ast && i < mod->ast->child_count; i++) {
            ASTNode* d = mod->ast->children[i];
            if (!d || d->type != AST_EXTERN_FUNCTION) continue;
            zb_scan_mention(&sc, d->value);
            zb_scan_note(&sc, d->annotation);
        }
    }

    /* The candidates: each pointer-to-struct parameter of a function that
     * qualifies; any other function's parameters are left out, which is
     * "unknown" to every question about them. */
    for (int i = 0; i < gen->program->child_count; i++) {
        ASTNode* d = gen->program->children[i];
        if (!d || d->type != AST_FUNCTION_DEFINITION || !zb_fn_takes_box(d) ||
            !zb_fn_qualifies(gen, &sc, d)) continue;
        int n = zb_plain_param_count(d);
        for (int k = 0; k < n; k++) {
            ASTNode* p = zb_param_node(d, k);
            if (!p || !p->node_type || p->node_type->kind != TYPE_PTR) continue;
            const char* sname = zb_struct_of(p->node_type);
            if (!sname) continue;
            if (zp->count == zp->cap) {
                zp->cap = zp->cap ? zp->cap * 2 : 64;
                zp->params = aether_xrealloc(zp->params, sizeof(*zp->params) * zp->cap);
            }
            ZbParam* zq = &zp->params[zp->count];
            memset(zq, 0, sizeof(*zq));
            zq->fn = d->value;
            zq->idx = k;
            zq->sname = sname;
            char* key = heap_strf("%s|%d", d->value, k);
            strmap_put(&zp->at, key, (void*)(intptr_t)(zp->count + 1));
            free(key);
            zp->count++;
        }
    }

    /* What each call passes. A parameter of the calling function that its
     * body leaves alone (zb_bindings: never bound, written through `&`,
     * stepped, or shadowed by a closure parameter or pattern) is an edge;
     * anything else is judged as it stands. */
    gen->zb_params_state = 1;
    /* Whether a caller leaves a parameter alone, asked once per caller and
     * name: a function passing its parameter on many times walked its body
     * for every call. */
    StrMap untouched;
    strmap_init(&untouched);
    for (int q = 0; q < zp->count; q++) {
        void* first = strmap_get(&sc.first, zp->params[q].fn);
        for (int s = first ? (int)(intptr_t)first - 1 : -1; s >= 0; s = sc.sites[s].next) {
            ZbSite* site = &sc.sites[s];
            ASTNode* arg = site->call->children[zp->params[q].idx];
            ASTNode* caller = site->fn;
            int pos = -1;
            if (arg->type == AST_IDENTIFIER && arg->value && caller &&
                caller->type == AST_FUNCTION_DEFINITION && caller->child_count > 0)
                pos = zb_param_pos(caller, arg->value);
            int bound = 0;
            if (pos >= 0) {
                char* ukey = heap_strf("%p|%s|%s", (void*)caller, arg->value, zp->params[q].sname);
                void* known = strmap_get(&untouched, ukey);
                if (known) {
                    bound = known == ZB_NO;
                } else {
                    if (!zb_bindings(gen, caller->children[caller->child_count - 1], caller,
                                     arg->value, zp->params[q].sname, 0, &bound))
                        bound = 1;
                    strmap_put(&untouched, ukey, bound ? ZB_NO : ZB_YES);
                }
                free(ukey);
            }
            if (pos >= 0 && !bound) {
                ZbParam* src = zb_param_find(zp, caller->value, pos);
                if (!src || strcmp(src->sname, zp->params[q].sname) != 0) {
                    zp->params[q].unknown = 1;
                    break;
                }
                if (src->feed_count == src->feed_cap) {
                    src->feed_cap = src->feed_cap ? src->feed_cap * 2 : 4;
                    src->feeds = aether_xrealloc(src->feeds, sizeof(int) * src->feed_cap);
                }
                src->feeds[src->feed_count++] = q;
            } else if (!zb_expr(gen, arg, site->ctx, zp->params[q].sname, 0)) {
                zp->params[q].unknown = 1;
                break;
            }
        }
    }

    /* Unknown spreads to every parameter it is passed on to. */
    int* work = aether_xrealloc(NULL, sizeof(int) * (zp->count + 1));
    int top = 0;
    for (int q = 0; q < zp->count; q++) {
        if (zp->params[q].unknown) work[top++] = q;
    }
    while (top > 0) {
        ZbParam* p = &zp->params[work[--top]];
        for (int f = 0; f < p->feed_count; f++) {
            ZbParam* t = &zp->params[p->feeds[f]];
            if (!t->unknown) {
                t->unknown = 1;
                work[top++] = p->feeds[f];
            }
        }
    }
    free(work);

    free(sc.sites);
    free(sc.notes);
    strmap_free(&sc.mentions);
    strmap_free(&sc.first);
    strmap_free(&untouched);
    /* Answers given while every parameter was unknown are dropped, except
     * which function holds a closure ("P|"), which no parameter changes. */
    StrMap kept;
    strmap_init(&kept);
    for (int i = 0; i < strmap_count(&gen->zeroed_box_memo); i++) {
        const char* k = strmap_key_at(&gen->zeroed_box_memo, i);
        if (k && k[0] == 'P' && k[1] == '|')
            strmap_put(&kept, k, strmap_value_at(&gen->zeroed_box_memo, i));
    }
    strmap_free(&gen->zeroed_box_memo);
    gen->zeroed_box_memo = kept;
    gen->zb_params_state = 2;
}

/* Does parameter `name` of `fn` hold only `sname` boxes on entry? */
static int zb_param_holds_boxes(CodeGenerator* gen, ASTNode* fn, const char* name,
                                const char* sname) {
    if (gen->zb_params_state != 2 || !gen->zb_params) return 0;
    if (fn->type != AST_FUNCTION_DEFINITION || !fn->value) return 0;
    int k = zb_param_pos(fn, name);
    if (k < 0) return 0;
    ZbParam* p = zb_param_find((ZbParams*)gen->zb_params, fn->value, k);
    return p && !p->unknown && strcmp(p->sname, sname) == 0;
}

static int box_trackers_are_initialised(CodeGenerator* gen, ASTNode* obj) {
    if (!gen || !obj || !obj->node_type || obj->node_type->kind != TYPE_PTR) return 0;
    const char* sname = zb_struct_of(obj->node_type);
    if (!sname) return 0;
    if (gen->zb_params_state == 0) zb_params_build(gen);
    return zb_expr(gen, obj, gen->current_function, sname, 0);
}

/* #1879: emit a NESTED-path field assignment (`o.inner.name = ...`).
 *
 * The identifier-only path below cannot serve this: it splices `obj->value`
 * into the generated C as a bare name, and here the object is itself a member
 * access. The result was a plain store with no `_heap_<field>` tracker, so the
 * inner struct's destructor believed it owned nothing and the string leaked --
 * while the identical write spelled on the inner pointer released correctly.
 * Ownership followed how the assignment was SPELLED rather than the type.
 *
 * The object expression is bound to a temporary first: it is emitted three
 * times (store, tracker, and the field read) and re-evaluating it would run
 * any side effects more than once.
 *
 * The previous value is freed only when the inner box is known to be a
 * heap.new box (#2369, box_trackers_are_initialised): every store into that
 * pointer field, program-wide, put one there. Otherwise it may have come from
 * `malloc(n) as *T`, whose tracker is garbage (#1873), and reading it frees a
 * garbage pointer; the store then only sets the tracker, which is safe
 * regardless and lets the destructor reclaim the value. */
static int emit_field_tracker_from_rhs(CodeGenerator* gen, ASTNode* rhs,
                                       const char* tracker_lvalue);

/* #2461: a field store whose value is a view (another struct's field, an
 * `if` over owned values) takes it with emit_string_take; `own` names the
 * flag the field's tracker is set from, or is empty for a classic store. */
static void field_store_take_flag(CodeGenerator* gen, ASTNode* rhs,
                                  char* own, size_t n) {
    own[0] = '\0';
    if (string_take_is_view(gen, rhs)) string_take_new_flag(own, n);
}

static void emit_field_store_value(CodeGenerator* gen, ASTNode* rhs,
                                   const char* own) {
    if (own[0]) emit_string_take(gen, rhs, own, NULL);
    else generate_expression(gen, rhs);
}

/* Set the field's tracker after the store: from the take's flag, else
 * from the source var's runtime ownership (a bare heap-var identifier),
 * else from the static classification. */
static void emit_field_store_tracker(CodeGenerator* gen, ASTNode* rhs,
                                     const char* own, const char* tracker_lv,
                                     int rhs_is_heap) {
    if (own[0]) {
        fprintf(gen->output, " %s = %s;", tracker_lv, own);
    } else if (!emit_field_tracker_from_rhs(gen, rhs, tracker_lv)) {
        fprintf(gen->output, " %s = %d;", tracker_lv, rhs_is_heap ? 1 : 0);
    }
}

/* #2497: may the trackers of the value struct `obj` (a field held by value,
 * `o.inner`) be read to release the previous value? Its trackers are its
 * holder's: a local struct value's are (as for `v.name = ...`); one reached
 * through a pointer field is as trustworthy as that box. */
static int value_path_trackers_are_initialised(CodeGenerator* gen, ASTNode* obj) {
    while (obj && obj->type == AST_MEMBER_ACCESS && obj->child_count == 1 &&
           obj->children[0] && obj->children[0]->node_type) {
        ASTNode* holder = obj->children[0];
        if (holder->node_type->kind == TYPE_PTR) return box_trackers_are_initialised(gen, holder);
        if (holder->node_type->kind != TYPE_STRUCT) return 0;
        obj = holder;
    }
    return obj && obj->type == AST_IDENTIFIER && obj->node_type &&
           obj->node_type->kind == TYPE_STRUCT;
}

static int emit_nested_field_heap_assign(CodeGenerator* gen, ASTNode* lhs,
                                         ASTNode* rhs, ASTNode* obj) {
    Type* obj_type = obj->node_type;
    if (!obj_type) return 0;
    /* A struct POINTER, or (#2497) a struct held by value in another one
     * (`o.inner.name = ...`). The latter used to be a plain store that left
     * `o.inner._heap_name` as it was; now that replacing or destroying `o`
     * releases `o.inner`'s strings, a stale tracker would free a literal. */
    int by_value = obj_type->kind == TYPE_STRUCT && obj_type->struct_name;
    const char* sname = by_value ? obj_type->struct_name
                        : (obj_type->kind == TYPE_PTR && obj_type->element_type &&
                           obj_type->element_type->kind == TYPE_STRUCT)
                          ? obj_type->element_type->struct_name : NULL;
    if (!sname) return 0;
    /* A header-defined struct has no `_heap_<field>` trackers: its fields
     * are the C header's, and the store is a plain one (see
     * emit_struct_field_heap_assign). */
    if (aether_is_c_import_struct(sname)) return 0;
    if (!gen->program) return 0;

    ASTNode* sdef = find_struct_definition_by_name(gen->program, sname);
    if (!sdef) return 0;
    ASTNode* matching_field = NULL;
    for (int fi = 0; fi < sdef->child_count; fi++) {
        ASTNode* f = sdef->children[fi];
        if (f && f->type == AST_STRUCT_FIELD &&
            f->value && strcmp(f->value, lhs->value) == 0) {
            matching_field = f;
            break;
        }
    }
    if (!matching_field || !matching_field->node_type ||
        matching_field->node_type->kind != TYPE_STRING) return 0;
    if (!struct_owns_heap_strings(gen, sdef)) return 0;

    int rhs_is_heap = is_heap_string_expr(gen, rhs);
    /* Unique per emission: these blocks nest (an argument to the RHS may
     * itself be a nested-path assignment), and a fixed name would shadow. */
    static int nested_tgt_seq = 0;
    char tgt[32];
    snprintf(tgt, sizeof(tgt), "_ae_ntgt%d", nested_tgt_seq++);

    const char* tracker_lv = cg_internf("%s->_heap_%s", tgt, lhs->value);
    char own[32];
    field_store_take_flag(gen, rhs, own, sizeof(own));
    int release_old = by_value ? value_path_trackers_are_initialised(gen, obj)
                               : box_trackers_are_initialised(gen, obj);
    print_indent(gen);
    fprintf(gen->output, "{ %s* %s = %s(", sname, tgt, by_value ? "&" : "");
    generate_expression(gen, obj);
    fprintf(gen->output, ");");
    if (own[0]) fprintf(gen->output, " int %s = 0;", own);
    if (release_old) {
        fprintf(gen->output, " const char* _tmp_old = %s->%s;", tgt, lhs->value);
    }
    fprintf(gen->output, " %s->%s = ", tgt, lhs->value);
    emit_field_store_value(gen, rhs, own);
    fprintf(gen->output, ";");
    if (release_old) {
        fprintf(gen->output, " if (%s) aether_heap_str_free(_tmp_old);", tracker_lv);
    }
    /* Move the source var's runtime ownership when the RHS is a heap-var
     * identifier (it may hold a borrow); otherwise the static classification. */
    emit_field_store_tracker(gen, rhs, own, tracker_lv, rhs_is_heap);
    fprintf(gen->output, " }\n");
    return 1;
}

/* When a heap-string struct field is assigned from a bare heap-tracked-var
 * identifier, the field's `_heap_<field>` tracker must take that variable's
 * RUNTIME ownership, not a static 1. A local classified as a heap-string var
 * can still hold a BORROWED value at runtime (e.g. it was assigned from a
 * function that returns a borrowed/literal pass-through, leaving `_heap_<var>
 * == 0`). Hard-coding the field tracker to 1 then makes the struct destructor
 * free a pointer the program never owned — a literal (free of rodata) or a
 * value still owned elsewhere (double free). This is the field-store analogue
 * of the `_heap_dest = _heap_src` ownership move used for `dest = src` aliases.
 *
 * Writes `<field-tracker-lvalue> = _heap_<var>;` then disowns the source
 * (`_heap_<var> = 0;`) so the freeing duty transfers to the field and the
 * source is not also freed at scope exit. Returns 1 if it handled the RHS
 * (a bare heap-var identifier); 0 to fall back to the static rhs_is_heap. */
static int emit_field_tracker_from_rhs(CodeGenerator* gen, ASTNode* rhs,
                                       const char* tracker_lvalue) {
    if (!gen || !rhs || rhs->type != AST_IDENTIFIER || !rhs->value) return 0;
    if (!is_heap_string_var(gen, rhs->value)) return 0;
    fprintf(gen->output, " %s = _heap_%s; _heap_%s = 0;",
            tracker_lvalue, rhs->value, rhs->value);
    return 1;
}

/* A `string` field of a header-defined struct (`extern struct ... @c_import`)
 * is the header's `const char*`, read by C. An Aether string may be a wrapped
 * AetherString (interpolation, substring and the like build one), whose
 * header would reach C in place of the characters, so the store takes the
 * payload with aether_string_data, as a call to a C extern does. Any spelling
 * of the object: a value (`v.f`), a pointer (`p.f`), a nested path
 * (`t.inner.f`). The field borrows (see the escape walk); nothing is freed. */
static int emit_c_import_string_field_store(CodeGenerator* gen, ASTNode* lhs, ASTNode* rhs) {
    if (lhs->type != AST_MEMBER_ACCESS || lhs->child_count != 1 || !lhs->children[0]) return 0;
    if (!lhs->node_type || lhs->node_type->kind != TYPE_STRING) return 0;
    Type* ot = lhs->children[0]->node_type;
    const char* sname = NULL;
    if (ot && ot->kind == TYPE_STRUCT) sname = ot->struct_name;
    else if (ot && ot->kind == TYPE_PTR && ot->element_type &&
             ot->element_type->kind == TYPE_STRUCT) sname = ot->element_type->struct_name;
    if (!sname || !aether_is_c_import_struct(sname)) return 0;
    print_indent(gen);
    gen->generating_lvalue = 1;
    generate_expression(gen, lhs);
    gen->generating_lvalue = 0;
    /* NULL stays NULL: aether_string_data maps it to "", and a C field told
     * apart from an empty string by being NULL must stay so. */
    fprintf(gen->output, " = ({ const void* _ae_cs = (const void*)(");
    generate_expression(gen, rhs);
    fprintf(gen->output, "); _ae_cs ? aether_string_data(_ae_cs) : (const char*)0; });\n");
    return 1;
}

/* #2497: `o.inner = v` where `inner` is a struct held by value that owns
 * strings. The field is part of `o`, released with it, so it takes `v`
 * (emit_struct_take) and replaces what it held. Through a pointer whose
 * trackers cannot be trusted (#1873) the old value is not read: the store
 * only takes `v`. */
static int emit_struct_valued_field_store(CodeGenerator* gen, ASTNode* lhs, ASTNode* rhs) {
    const char* fs = struct_owning_strings(gen, lhs->node_type);
    if (!fs) return 0;
    ASTNode* holder = lhs->children[0];
    Type* ht = holder->node_type;
    int trusted;
    if (ht && ht->kind == TYPE_STRUCT) {
        trusted = value_path_trackers_are_initialised(gen, lhs);
    } else if (ht && ht->kind == TYPE_PTR) {
        trusted = box_trackers_are_initialised(gen, holder);
    } else {
        return 0;
    }
    print_indent(gen);
    if (trusted) {
        fprintf(gen->output, "%s_replace(&(", fs);
        generate_expression(gen, lhs);
        fprintf(gen->output, "), ");
        emit_struct_take(gen, rhs, fs, NULL);
        fprintf(gen->output, ");\n");
    } else {
        generate_expression(gen, lhs);
        fprintf(gen->output, " = ");
        emit_struct_take(gen, rhs, fs, NULL);
        fprintf(gen->output, ";\n");
    }
    return 1;
}

/* #2525: `o.cb = v` where `cb` is a closure field. The field holds a
 * reference of its own: the store takes `v` (emit_closure_take) and gives
 * back the reference to the value it held, when the holder's fields can be
 * read (the same trust as a string field's tracker: a local struct value,
 * or a box every store into which initialised; not `malloc(n) as *T`, whose
 * env slot is garbage). A header-defined struct's fields are C's: a plain
 * store. */
static int emit_closure_field_store(CodeGenerator* gen, ASTNode* lhs, ASTNode* rhs) {
    if (!lhs->node_type || lhs->node_type->kind != TYPE_FUNCTION || lhs->node_type->is_fnptr) return 0;
    ASTNode* holder = lhs->children[0];
    Type* ht = holder->node_type;
    const char* sname = NULL;
    int trusted;
    if (ht && ht->kind == TYPE_STRUCT) {
        sname = ht->struct_name;
        trusted = value_path_trackers_are_initialised(gen, lhs);
    } else if (ht && ht->kind == TYPE_PTR && ht->element_type &&
               ht->element_type->kind == TYPE_STRUCT) {
        sname = ht->element_type->struct_name;
        trusted = box_trackers_are_initialised(gen, holder);
    } else {
        return 0;
    }
    if (!sname || aether_is_c_import_struct(sname)) return 0;
    print_indent(gen);
    fprintf(gen->output, "{ _AeClosure _ae_cv = ");
    emit_closure_take(gen, rhs);
    fprintf(gen->output, "; ");
    if (trusted) {
        fprintf(gen->output, "void* _ae_old = (");
        generate_expression(gen, lhs);
        fprintf(gen->output, ").env; ");
    }
    generate_expression(gen, lhs);
    fprintf(gen->output, " = _ae_cv;");
    if (trusted) fprintf(gen->output, " _aether_closure_env_release(_ae_old);");
    fprintf(gen->output, " }\n");
    return 1;
}

/* #2525: `o.slots[i] = v` where `slots` is a fixed-size array field of
 * structs that own strings or closures: each element is a value of its own,
 * released with `o`, so the store takes `v` and replaces what the element
 * held, as `o.inner = v` does (emit_struct_valued_field_store). Only where
 * the holder's fields can be read (a local struct value, a trusted box). */
/* #2528: does the local array `name` own its elements (declared from a
 * literal of structs that own strings or closures, with a scope-exit
 * destroy pending)? */
static int struct_array_local_owns(CodeGenerator* gen, const char* name) {
    size_t n = strlen(name);
    for (int i = 0; i < gen->defer_count; i++) {
        ASTNode* d = gen->defer_stack[i];
        const char* a = d ? d->annotation : NULL;
        if (a && strncmp(a, "struct_array_destroy:", 21) == 0 &&
            strncmp(a + 21, name, n) == 0 && a[21 + n] == ':') return 1;
    }
    return 0;
}

/* #2528: `v` as the value an owned `string[N]` element takes: a fresh heap
 * string is adopted, anything else (a literal, a local, a field) copied, so
 * every element is the array's own, as a string array cell's are (#2474). */
void emit_owned_string_element(CodeGenerator* gen, ASTNode* v) {
    int fresh = v && (v->type == AST_FUNCTION_CALL || v->type == AST_STRING_INTERP ||
                      v->type == AST_OR_ELSE) && is_heap_string_expr(gen, v);
    if (fresh) {
        generate_expression(gen, v);
        return;
    }
    fprintf(gen->output, "aether_uniform_heap_str((const char*)(");
    generate_expression(gen, v);
    fprintf(gen->output, "), 0)");
}

void emit_owned_array_literal(CodeGenerator* gen, ASTNode* lit, int kind) {
    fprintf(gen->output, "{");
    for (int i = 0; i < lit->child_count; i++) {
        if (i > 0) fprintf(gen->output, ", ");
        if (kind == 1) emit_owned_string_element(gen, lit->children[i]);
        else emit_closure_take(gen, lit->children[i]);
    }
    fprintf(gen->output, "}");
}

/* #2525, #2528: an element store into an array that owns its elements:
 * `o.slots[i] = v` on a fixed-size array field of structs that own strings
 * or closures, `o.names[i] = v` / `o.cbs[i] = v` on a `string[N]` / `fn[N]`
 * field, or `arr[i] = v` on a local array of such structs. The element is a
 * value of its own, released with its holder, so the store takes `v` and
 * replaces what the element held, as `o.inner = v` does
 * (emit_struct_valued_field_store). Only where the holder's slots can be
 * read (a local struct value or owning local array, a trusted box). */
static int emit_struct_element_store(CodeGenerator* gen, ASTNode* lhs, ASTNode* rhs) {
    if (!lhs || lhs->type != AST_ARRAY_ACCESS || lhs->child_count < 2 || !lhs->children[0]) return 0;
    ASTNode* arr = lhs->children[0];
    if (!arr->node_type || arr->node_type->kind != TYPE_ARRAY || arr->node_type->array_size <= 0) return 0;
    Type* et = lhs->node_type ? lhs->node_type : arr->node_type->element_type;
    const char* es = struct_owning_strings(gen, et);
    int kind = es ? 3 : (et && et->kind == TYPE_STRING) ? 1
                      : (et && et->kind == TYPE_FUNCTION && !et->is_fnptr) ? 2 : 0;
    if (!kind) return 0;
    int trusted = 0;
    if (arr->type == AST_MEMBER_ACCESS) {
        ASTNode* holder = arr->children[0];
        Type* ht = holder ? holder->node_type : NULL;
        const char* sname = NULL;
        if (ht && ht->kind == TYPE_STRUCT) {
            sname = ht->struct_name;
            trusted = value_path_trackers_are_initialised(gen, arr);
        } else if (ht && ht->kind == TYPE_PTR && ht->element_type &&
                   ht->element_type->kind == TYPE_STRUCT) {
            sname = ht->element_type->struct_name;
            trusted = box_trackers_are_initialised(gen, holder);
        }
        if (!sname || aether_is_c_import_struct(sname)) return 0;
        /* A `string[N]` / `fn[N]` field of a struct that does not own its
         * elements is a plain store. */
        if (kind != 3) {
            ASTNode* sdef = find_struct_definition_by_name(gen->program, sname);
            ASTNode* fdef = NULL;
            for (int i = 0; sdef && i < sdef->child_count; i++) {
                ASTNode* f = sdef->children[i];
                if (f && f->type == AST_STRUCT_FIELD && f->value && arr->value &&
                    strcmp(f->value, arr->value) == 0) { fdef = f; break; }
            }
            if (!fdef || struct_field_owned_array(fdef, NULL) != kind) return 0;
        }
    } else if (arr->type == AST_IDENTIFIER && arr->value && kind == 3) {
        if (!struct_array_local_owns(gen, arr->value)) return 0;
        trusted = 1;
    } else if (arr->type == AST_IDENTIFIER && arr->value && is_actor_state_var(gen, arr->value)) {
        /* #2528: a `string[N]` / `fn[N]` state field owns its elements
         * (destroy_state releases them); the store reads `self->f[i]`. */
        int sk = 0;
        for (int a = 0; gen->program && gen->current_actor && a < gen->program->child_count; a++) {
            ASTNode* actor = gen->program->children[a];
            if (!actor || actor->type != AST_ACTOR_DEFINITION || !actor->value ||
                strcmp(actor->value, gen->current_actor) != 0) continue;
            for (int i = 0; i < actor->child_count; i++) {
                ASTNode* sd = actor->children[i];
                if (sd && sd->type == AST_STATE_DECLARATION && sd->value &&
                    strcmp(sd->value, arr->value) == 0) { sk = state_array_owned(sd, NULL); break; }
            }
        }
        if (sk != kind) return 0;
        trusted = 1;
    } else {
        return 0;
    }
    if (!trusted) return 0;
    print_indent(gen);
    if (kind == 3) {
        fprintf(gen->output, "%s_replace(&(", es);
        generate_expression(gen, lhs);
        fprintf(gen->output, "), ");
        emit_struct_take(gen, rhs, es, NULL);
        fprintf(gen->output, ");\n");
    } else if (kind == 1) {
        fprintf(gen->output, "{ const char* _ae_old = ");
        generate_expression(gen, lhs);
        fprintf(gen->output, "; ");
        generate_expression(gen, lhs);
        fprintf(gen->output, " = ");
        emit_owned_string_element(gen, rhs);
        fprintf(gen->output, "; if (_ae_old) aether_heap_str_free(_ae_old); }\n");
    } else {
        fprintf(gen->output, "{ _AeClosure _ae_cv = ");
        emit_closure_take(gen, rhs);
        fprintf(gen->output, "; void* _ae_old = (");
        generate_expression(gen, lhs);
        fprintf(gen->output, ").env; ");
        generate_expression(gen, lhs);
        fprintf(gen->output, " = _ae_cv; _aether_closure_env_release(_ae_old); }\n");
    }
    return 1;
}

/* Does `lhs = local` go through emit_struct_field_heap_assign's tracked
 * store, which moves a bare heap-tracked local's ownership into the field
 * (emit_field_tracker_from_rhs: the field's tracker takes the local's, the
 * local's is cleared)? A string field of an Aether struct, reached from a
 * named struct value or struct pointer: the shapes that function handles
 * itself, mirrored here for the escape walk. A header-defined struct's field
 * borrows instead, and a nested path is not claimed. */
static int field_store_takes_local(CodeGenerator* gen, ASTNode* lhs) {
    if (!gen || !gen->program || !lhs || lhs->type != AST_MEMBER_ACCESS || !lhs->value ||
        lhs->child_count != 1 || !lhs->children[0]) return 0;
    ASTNode* obj = lhs->children[0];
    if (obj->type != AST_IDENTIFIER || !obj->value || !obj->node_type) return 0;
    Type* t = obj->node_type;
    const char* sname = NULL;
    if (t->kind == TYPE_STRUCT) {
        sname = t->struct_name;
    } else if (t->kind == TYPE_PTR && t->element_type &&
               t->element_type->kind == TYPE_STRUCT) {
        sname = t->element_type->struct_name;
    }
    if (!sname || aether_is_c_import_struct(sname)) return 0;
    ASTNode* sdef = find_struct_definition_by_name(gen->program, sname);
    for (int i = 0; sdef && i < sdef->child_count; i++) {
        ASTNode* f = sdef->children[i];
        if (f && f->type == AST_STRUCT_FIELD && f->value && strcmp(f->value, lhs->value) == 0)
            return f->node_type && f->node_type->kind == TYPE_STRING;
    }
    return 0;
}

static int emit_struct_field_heap_assign(CodeGenerator* gen, ASTNode* lhs, ASTNode* rhs) {
    if (!gen || !lhs || !rhs) return 0;
    if (emit_struct_element_store(gen, lhs, rhs)) return 1;
    if (lhs->type != AST_MEMBER_ACCESS || !lhs->value) return 0;
    if (lhs->child_count != 1 || !lhs->children[0]) return 0;
    if (emit_c_import_string_field_store(gen, lhs, rhs)) return 1;
    if (emit_struct_valued_field_store(gen, lhs, rhs)) return 1;
    if (emit_closure_field_store(gen, lhs, rhs)) return 1;
    /* #1879: a nested path (`o.inner.name`) has a MEMBER_ACCESS object rather
     * than a bare identifier. Handle it separately -- the code below splices
     * the object in as a name. */
    if (lhs->children[0]->type == AST_MEMBER_ACCESS) {
        return emit_nested_field_heap_assign(gen, lhs, rhs, lhs->children[0]);
    }
    if (lhs->children[0]->type != AST_IDENTIFIER || !lhs->children[0]->value) return 0;
    ASTNode* obj = lhs->children[0];
    Type* obj_type = obj->node_type;
    if (!obj_type) return 0;
    /* Accept a value struct (`v.field = ...`, accessor ".") and a heap-boxed
     * struct pointer from heap.new (`p.field = ...`, accessor "->"). The
     * pointer case (#790) lets a heap.new'd struct own its string fields: the
     * box adopts the heap string and sets its `_heap_<field>` tracker, exactly
     * as a value struct does, so the generated `<Name>_heap_free` / scope-exit
     * destructor reclaims it. */
    const char* acc;
    const char* struct_name;
    /* Whether `_heap_<field>` is known zero-initialised, and so safe to READ
     * in order to release the previous value (#1873). A value struct and a
     * heap.new box both qualify; a pointer parameter does not. */
    int tracker_is_trustworthy = 1;
    if (obj_type->kind == TYPE_STRUCT && obj_type->struct_name) {
        acc = ".";
        struct_name = obj_type->struct_name;
    } else if (obj_type->kind == TYPE_PTR && obj_type->element_type &&
               obj_type->element_type->kind == TYPE_STRUCT &&
               obj_type->element_type->struct_name) {
        /* #1879: ANY struct pointer, not only a heap.new local or a pointer
         * parameter. A local ALIAS (`p = o.inner; p.name = ...`) is neither,
         * so it used to fall through to a bare store and leak -- the same
         * "ownership depends on how you spell it" defect as the nested path,
         * reached by binding the inner pointer to a name first.
         *
         * Widening is safe because the trustworthiness check below, not this
         * guard, is what decides whether the previous value may be FREED.
         * Claiming ownership is sound for any struct pointer; reading a
         * possibly-garbage tracker is not, and that stays gated. */
        /* #1873: a PARAMETER is not a promise that the box was zeroed — the
         * caller may have handed us `malloc(n) as *T`, whose `_heap_<field>`
         * tracker is garbage. Reading it to decide whether to free the
         * previous value then frees a garbage pointer. Only a box we can
         * SEE was made by heap.new carries that guarantee, so remember which
         * branch we are on and suppress just the free for the other. #2369:
         * "see" follows the pointer back through calls, casts, locals and
         * struct fields (box_trackers_are_initialised), not only a local
         * bound to heap.new in this function. */
        /* Not is_heap_box_var: it records that some binding of the name is
         * a heap.new box, and another binding (another arm, another
         * branch) may be `malloc(n) as *T`. box_trackers_are_initialised
         * asks it of every binding. */
        tracker_is_trustworthy = box_trackers_are_initialised(gen, obj);
        /* Only a heap.new(T) box has zero-initialised `_heap_<field>`
         * trackers, so only there is reading/freeing the previous field
         * value safe. A raw `malloc(...) as *T` has garbage trackers — its
         * field stays a bare store (#790 regression guard). */
        acc = "->";
        struct_name = obj_type->element_type->struct_name;
    } else {
        return 0;
    }
    /* `extern struct ... @c_import`: the layout is the C header's, which has
     * no `_heap_<field>` companions, so there is nothing to track ownership
     * in. The field BORROWS the string, as any C API that stores a `char*`
     * does: a plain store, and the string stays owned where it was. */
    if (aether_is_c_import_struct(struct_name)) return 0;
    if (!gen->program) return 0;
    ASTNode* sdef = find_struct_definition_by_name(gen->program, struct_name);
    if (!sdef) return 0;
    ASTNode* matching_field = NULL;
    for (int fi = 0; fi < sdef->child_count; fi++) {
        ASTNode* f = sdef->children[fi];
        if (f && f->type == AST_STRUCT_FIELD &&
            f->value && strcmp(f->value, lhs->value) == 0) {
            matching_field = f;
            break;
        }
    }
    if (!matching_field || !matching_field->node_type ||
        matching_field->node_type->kind != TYPE_STRING) return 0;

    int rhs_is_heap = is_heap_string_expr(gen, rhs);
    /* A variable a closure writes lives in a shared cell (#2458): the name
     * is the cell pointer, and the struct is `(*name)`, as every other use
     * of it spells it (the AST_IDENTIFIER emission). */
    /* An actor's state is a field of `self`, spelled as an identifier
     * expression spells it (`self->kept.name`); bare, the store named an
     * undeclared local. */
    const char* objs = is_promoted_capture(gen, obj->value)
                       ? cg_internf("(*%s)", obj->value)
                       : is_actor_state_var(gen, obj->value)
                       ? cg_internf("%s->%s", gen->state_self_alias ? gen->state_self_alias : "self",
                                    obj->value)
                       : obj->value;
    const char* tracker_lv = cg_internf("%s%s_heap_%s", objs, acc, lhs->value);
    char own[32];
    field_store_take_flag(gen, rhs, own, sizeof(own));
    print_indent(gen);
    if (!tracker_is_trustworthy) {
        /* #1873: store and SET the tracker (so the destructor still reclaims
         * this value — #1866's leak fix is preserved), but do not read the
         * pre-existing tracker to free the old value. On a hand-malloc'd box
         * that read is uninitialised memory, and acting on it frees a garbage
         * pointer. Not freeing here can leak a previous value on a box that
         * was genuinely zeroed; that is strictly better than a segfault, and
         * the caller can use heap.new to get the releasing behaviour. */
        fprintf(gen->output, "{");
        if (own[0]) fprintf(gen->output, " int %s = 0;", own);
        fprintf(gen->output, " %s%s%s = ", objs, acc, lhs->value);
        emit_field_store_value(gen, rhs, own);
        fprintf(gen->output, ";");
        /* Move the source var's runtime ownership when the RHS is a heap-var
         * identifier (it may hold a borrow); otherwise the static class. */
        emit_field_store_tracker(gen, rhs, own, tracker_lv, rhs_is_heap);
        fprintf(gen->output, " }\n");
        return 1;
    }
    fprintf(gen->output, "{ const char* _tmp_old = %s%s%s;", objs, acc, lhs->value);
    if (own[0]) fprintf(gen->output, " int %s = 0;", own);
    fprintf(gen->output, " %s%s%s = ", objs, acc, lhs->value);
    emit_field_store_value(gen, rhs, own);
    fprintf(gen->output, "; if (%s) aether_heap_str_free(_tmp_old);", tracker_lv);
    emit_field_store_tracker(gen, rhs, own, tracker_lv, rhs_is_heap);
    fprintf(gen->output, " }\n");
    return 1;
}

/* datastar#4: is this initialiser a builder call carrying a trailing block?
 *
 * Such a call is lowered TWICE by design: the declaration emits it so the
 * variable has a value, and the builder handler further down re-emits it with
 * the filled config and reassigns. The ordinary declaration path already
 * suppresses its half for exactly this reason (`defer_with_trailing`, which
 * emits ` = 0`), but the heap-STRING reassignment path did not -- so a builder
 * declaring a `string` return ran its body twice, once with a NULL config.
 *
 * Silent, and side-effecting: the assigned value is correct because the second
 * write wins, so only a builder that DOES something reveals it. The datastar
 * port hit it as every SSE patch being streamed twice, once without its
 * selector.
 *
 * `err = sse.patch_elements(html) { ... }` is the natural shape for any
 * builder reporting an error Go-style, so this is on a common path. */
static int init_is_builder_with_trailing(CodeGenerator* gen, ASTNode* init) {
    if (!gen || !init) return 0;
    if (init->type != AST_FUNCTION_CALL || !init->value) return 0;
    if (!is_builder_func_reg(gen, init->value)) return 0;
    for (int i = 0; i < init->child_count; i++) {
        ASTNode* a = init->children[i];
        if (a && a->type == AST_CLOSURE && a->value &&
            strcmp(a->value, "trailing") == 0) return 1;
    }
    return 0;
}

/* Struct-reassignment helper (#465). For `<var> = <value>` where `<var>`
 * is a struct local with heap-string fields, emit
 * `<Struct>_replace(&<var>, <value>);`: the previous struct's owned strings
 * are reclaimed, except one the new value takes over (`r = Rec { name:
 * r.name }`), which moves to it. Returns 1 if it emitted the assignment;
 * 0 leaves the caller to emit the bare one. */
static int emit_struct_replace_assign(CodeGenerator* gen, const char* var_name,
                                      Type* var_type, ASTNode* value) {
    if (!gen || !var_name || !var_type || !value) return 0;
    if (var_type->kind != TYPE_STRUCT || !var_type->struct_name) return 0;
    if (!gen->program) return 0;
    ASTNode* sdef = find_struct_definition_by_name(gen->program, var_type->struct_name);
    if (!sdef || !struct_owns_heap_strings(gen, sdef)) return 0;
    fprintf(gen->output, "%s_replace(&%s, ", var_type->struct_name, var_name);
    /* #2497: a struct another owner keeps is copied or moved in. */
    emit_struct_take(gen, value, var_type->struct_name, var_name);
    fprintf(gen->output, ");\n");
    return 1;
}

/* Should the return statement at this site route its value through
 * the uniform-heap shim? True when: the enclosing function is not
 * main, the return is single-value, the function is classifier-
 * classified heap-returning, and the return expression's static type
 * is `string` (TYPE_STRING) or implied to be so by the function's
 * declared return type. The shim only makes sense for string returns
 * — wrapping a non-string would emit a type-mismatched call. */
static int should_uniform_heap_return(CodeGenerator* gen, ASTNode* stmt) {
    if (!gen || !stmt) return 0;
    if (gen->in_main_function) return 0;
    if (stmt->child_count != 1) return 0;
    ASTNode* ret = stmt->children[0];
    if (!ret) return 0;
    /* Skip the wrap for AST_PRINT_STATEMENT-as-return — that path
     * never propagates a value to the caller, only side-effects. */
    if (ret->type == AST_PRINT_STATEMENT) return 0;
    /* A string-returning closure hands every result over owned (#2054):
     * its caller reaches it through a value and cannot ask which return
     * sites are heap the way it can for a named function. */
    if (gen->in_string_closure) return 1;
    if (!gen->current_function) return 0;
    if (!function_def_returns_heap_string(gen, gen->current_function)) return 0;
    /* Static type gate. Prefer the function's declared return type
     * (the C compiler's actual constraint). Fall back to the
     * expression's stamped type if the function lacks a typed
     * return slot. */
    Type* ret_type = gen->current_func_return_type
        ? gen->current_func_return_type
        : ret->node_type;
    if (!ret_type || ret_type->kind != TYPE_STRING) return 0;
    return 1;
}

/* Emit a return expression wrapped in `aether_uniform_heap_str`. The
 * helper (emitted once in the codegen prologue) guarantees the caller
 * receives a malloc-owned pointer regardless of which branch produced
 * the value: heap inputs are returned as-is (fast path), literal /
 * static inputs are malloc-duplicated.
 *
 * The static flag is resolved at compile time wherever possible:
 *   - 1  when the expression is provably heap (string.concat, string
 *        interpolation, heap-returning user fn / extern, …)
 *   - `_heap_<name>` when the expression is a bare identifier of a
 *        heap-tracked local (the only runtime case)
 *   - 0  otherwise (literals, fields, plain identifiers)
 *
 * Returns 1 if the wrap was emitted, 0 if the expression should be
 * emitted raw (the caller is responsible for raw emission when this
 * helper declines, e.g. for tuple / void returns).
 *
 * Invariant: only call when the enclosing function is classifier-
 * classified heap-returning. For non-heap functions, the raw return
 * still works because the caller doesn't free. */
static int emit_uniform_heap_return_expr(CodeGenerator* gen, ASTNode* expr) {
    if (!expr) return 0;
    /* Tuple returns flow through their own per-position channel
     * (Type.tuple_heap_flags + the AST_TUPLE_DESTRUCTURE handler).
     * Multi-value returns reach the caller's RHS site element by
     * element, not as a single pointer, so the uniform-heap shim
     * is the wrong shape there — the caller is responsible. */
    if (expr->type == AST_IF_EXPRESSION && string_take_is_view(gen, expr)) {
        /* #2461: the arm that runs is taken as a binding would take it (a
         * local moved out of this scope, a field read copied, a fresh
         * value adopted); the shim copies only what is still borrowed. */
        emit_string_take_owned(gen, expr, 0);
        return 1;
    }
    fprintf(gen->output, "aether_uniform_heap_str(");
    /* Cast to `const char*` so the helper's signature matches even
     * when the expression's static type is something C considers
     * incompatible (e.g. `void*` from `_aether_interp`). The shim
     * returns `const char*`; the C compiler accepts the assignment
     * back into the function's declared return type via implicit
     * pointer-conversion rules. */
    fprintf(gen->output, "(const char*)(");
    generate_expression(gen, expr);
    fprintf(gen->output, "), ");
    /* Static-flag resolution. */
    if (expr->type == AST_IDENTIFIER && expr->value &&
        is_heap_string_var(gen, expr->value)) {
        fprintf(gen->output, "_heap_%s", expr->value);
    } else if (is_heap_string_expr(gen, expr)) {
        fprintf(gen->output, "1");
    } else {
        fprintf(gen->output, "0");
    }
    fprintf(gen->output, ")");
    return 1;
}

// Per-position structural escape analysis for tuple-returning user
// functions (issue #420). Returns 1 iff *some* `return e0, e1, ...`
// in `fn_def`'s body has the `position`-th return expression
// classified as a heap string (an OR-fold across return sites) AND
// no whole-tuple-passthrough return vetoes it (see below). Returns 0
// for a missing position / non-tuple return / no returns at all
// (conservative — a `void`-falling-off function can't leak via tuple
// destructure since there's no value to destructure).
//
// OR-fold + uniform-heap wrap (the zlib/cryptography/lzf decode
// shape): these functions `return owned, n, ""` on success and
// `return "", 0, "err"` on the error paths — a string position
// that is heap on some return sites and a borrowed literal on
// others. A strict AND-fold classified such a position non-heap,
// so the caller never freed and the success-path allocation
// leaked at every call. The OR-fold classifies it heap; the
// matching `emit_tuple_return_position` then routes EVERY return
// path's value at that position through `aether_uniform_heap_str`,
// so the literal branches are malloc-duplicated and the caller can
// free uniformly. Same contract the single-value path already runs
// via `should_uniform_heap_return` / `emit_uniform_heap_return_expr`.
//
// The veto: a single-child return — `return g(...)` where `g` is
// tuple-typed — is a whole-tuple passthrough. `emit_tuple_return_
// position` only rewrites the `return e0, e1, ...` form, so a
// passthrough position cannot be wrapped; it is freeable only if `g`
// itself guarantees it (`function_def_returns_heap_at(g, position)`).
// If `g` doesn't, the position is hard-vetoed to non-heap regardless
// of what the wrappable returns do — otherwise the caller is told to
// free a borrowed literal `g` returned. The cost is a (rare) missed
// leak on the wrappable branches, never a free of non-heap memory.
//
// Memoisation: a comma-separated bit string in `fn_def->annotation`
// of the form `"heap_positions:1,0,1"` where the integer count
// matches the function's tuple_count. Mirrors the single-value
// `"heap_yes"` / `"heap_no"` sentinels used by
// function_def_returns_heap_string. Set to `"heap_pending"`
// during analysis to break cycles in mutually-recursive tuple-
// returning functions; the cycle case conservatively returns 0
// (no allocation classification → no auto-free → leak, but no
// crash).
//
// The cache is consulted by the AST_TUPLE_DESTRUCTURE codegen
// path to decide whether to emit `_heap_<lhs> = 1;` at the
// destructure site, and by `emit_tuple_return_position` to decide
// whether to wrap the return value.
static void walk_returns_for_heap_at_in(CodeGenerator* gen, ASTNode* node,
                                        int position, ASTNode* fn_body_root, const char* fn_name,
                                        int* found, int* any_heap, int* vetoed,
                                        CatchScope* cs);

static void walk_returns_for_heap_at(CodeGenerator* gen, ASTNode* node,
                                     int position, ASTNode* fn_body_root, const char* fn_name,
                                     int* found, int* any_heap, int* vetoed) {
    CatchScope cs = { {0}, 0 };
    walk_returns_for_heap_at_in(gen, node, position, fn_body_root, fn_name,
                                found, any_heap, vetoed, &cs);
}

static void walk_returns_for_heap_at_in(CodeGenerator* gen, ASTNode* node,
                                        int position, ASTNode* fn_body_root, const char* fn_name,
                                        int* found, int* any_heap, int* vetoed,
                                        CatchScope* cs) {
    if (!node || *vetoed) return;
    if (node->type == AST_RETURN_STATEMENT) {
        *found = 1;
        /* Single-child return in a tuple-returning function is a
         * whole-tuple passthrough — `return g(...)` where `g` is
         * tuple-typed. F hands `g`'s tuple straight to its caller and
         * cannot wrap an individual position: `emit_tuple_return_
         * position` only rewrites the `return e0, e1, ...` form, never
         * this one. So position `p` reaches F's caller freeable only
         * if `g` already guarantees it. If `g` doesn't — or can't be
         * resolved — F must NOT be heap-classified at `p`, else the
         * caller is told to free a value (a borrowed literal `g`
         * returned there) that was never malloc'd. That is a hard
         * veto, not just "no evidence": one such return poisons the
         * whole position regardless of what the wrappable returns do.
         * Surfaced by `hash_file` returning `"", rerr` on one branch
         * and `return cryptography.sha256_hex(...)` on the other —
         * sha256_hex's error slot is a literal, so the passthrough
         * fed a `""` to a caller told to free it (`free(): invalid
         * pointer`). */
        if (node->child_count == 1) {
            ASTNode* child = node->children[0];
            ASTNode* callee = NULL;
            ASTNode* ext_callee = NULL;
            if (child && child->type == AST_FUNCTION_CALL && child->value &&
                gen && gen->program) {
                const char* fn = codegen_normalise_callee(child->value);
                callee = find_function_definition_by_name(gen->program, fn);
                /* When the passthrough target is an extern (no user
                 * fn def), consult its `@heap` tuple-position flags
                 * directly instead of vetoing — same channel the
                 * AST_TUPLE_DESTRUCTURE handler reads when the callee
                 * is named directly at the destructure site. Without
                 * this, every `-> { return some_extern(...) }` wrapper
                 * silently dropped its callee's heap classification,
                 * so the user-facing wrapper looked non-heap at every
                 * destructure site. Surfaced by fs.read_binary (whose
                 * extern, fs_read_binary_tuple, is `(string @heap,
                 * int, string)`) leaking its bytes buffer in the avn
                 * port — tuple-destructure-heap-classification.md. */
                if (!callee) {
                    ext_callee = find_extern_declaration_by_name(
                        gen->program, fn);
                }
            }
            /* `T!` auto-wrap, NOT a tuple passthrough: a single-child
             * `return <expr>` whose expr is a plain (non-tuple) value is
             * the result auto-wrap `(<expr>, "")` — position 0 is exactly
             * <expr>, and emit_tuple_return_position DOES wrap it. So if
             * <expr> is a heap-string producer, position 0 is heap; no veto.
             * This is the shape a fallible `-> T!` uses when it returns a
             * bare heap value (`return bytes.finish(...)` /
             * `return string.concat(...)`), as opposed to the genuine
             * whole-tuple passthrough (`return g(...)` where g is
             * tuple-typed) the veto below guards. Distinguish by the
             * child's own type: a non-tuple child is an auto-wrap. */
            int child_is_tuple =
                child && child->node_type &&
                child->node_type->kind == TYPE_TUPLE;
            if (position == 0 && !child_is_tuple &&
                (catch_scope_has(cs, child) ||
                 return_expr_is_heap(gen, child, fn_body_root, fn_name))) {
                *any_heap = 1;
                return;
            }
            if (callee && function_def_returns_heap_at(gen, callee, position)) {
                *any_heap = 1;
            } else if (ext_callee && ext_callee->node_type &&
                       ext_callee->node_type->kind == TYPE_TUPLE &&
                       position >= 0 &&
                       position < ext_callee->node_type->tuple_count &&
                       ext_callee->node_type->tuple_heap_flags &&
                       ext_callee->node_type->tuple_heap_flags[position]) {
                *any_heap = 1;
            } else if (position == 0 && !child_is_tuple) {
                /* Auto-wrap of a non-heap value at position 0 (e.g.
                 * `return ""` / `return borrowed`): not heap, but NOT a
                 * veto either — it is a legitimate non-heap success value,
                 * not an unfreeable passthrough. Leave found set, any_heap
                 * unchanged, no veto. */
            } else {
                *vetoed = 1;
            }
            return;
        }
        // A tuple `return a, b, c` is represented as a return statement
        // with `child_count` matching the tuple arity (children are the
        // per-position expressions). Out-of-range = "this return doesn't
        // produce a value at `position`" → contributes no heap evidence.
        if (position < 0 || position >= node->child_count) return;
        ASTNode* pos_expr = node->children[position];
        /* Heap evidence for the position expression. Bare identifiers
         * resolve structurally against the analysed function's own
         * body (`return_expr_is_heap`): declaration-from-heap covers
         * the accumulator-into-tuple shape (`owned = string_new_with_
         * length(...); return owned, n, ""`, zlib/cryptography/lzf
         * decode results), destructure-from-heap-position covers the
         * error-forwarding shape (`v, err = g(...); return "", err`,
         * the asn1 chain in #1311). Never through
         * `gen->heap_string_vars`, which belongs to whichever function
         * happens to be emitting when the memo is first computed. */
        /* `return x, e` inside `catch e`: the binding may own a
         * heap-built reason (#2333). */
        if (catch_scope_has(cs, pos_expr) ||
            return_expr_is_heap(gen, pos_expr, fn_body_root, fn_name)) {
            *any_heap = 1;
        }
        return;
    }
    if (node->type == AST_FUNCTION_DEFINITION ||
        node->type == AST_BUILDER_FUNCTION ||
        node->type == AST_CLOSURE) {
        return;
    }
    int pushed = 0;
    if (node->type == AST_CATCH_CLAUSE && node->value && cs->count < CATCH_SCOPE_MAX) {
        cs->names[cs->count++] = node->value;
        pushed = 1;
    }
    for (int i = 0; i < node->child_count && !*vetoed; i++) {
        walk_returns_for_heap_at_in(gen, classifier_child(gen, node, i), position,
                                    fn_body_root, fn_name, found, any_heap, vetoed, cs);
    }
    if (pushed) cs->count--;
}

static int parse_heap_positions_annotation(const char* ann, int position) {
    /* Parses `"heap_positions:1,0,1"`. Returns the integer at
     * `position`, or -1 if the string is malformed or position is
     * out of range. */
    if (!ann) return -1;
    const char* prefix = "heap_positions:";
    size_t plen = strlen(prefix);
    if (strncmp(ann, prefix, plen) != 0) return -1;
    const char* p = ann + plen;
    int idx = 0;
    while (*p) {
        int digit;
        if (*p == '0') digit = 0;
        else if (*p == '1') digit = 1;
        else return -1;
        if (idx == position) return digit;
        p++;
        idx++;
        if (*p == ',') p++;
        else if (*p == '\0') break;
        else return -1;
    }
    return -1;  /* position out of range */
}

static int function_def_returns_heap_at(CodeGenerator* gen, ASTNode* fn_def,
                                         int position) {
    if (!fn_def ||
        (fn_def->type != AST_FUNCTION_DEFINITION &&
         fn_def->type != AST_BUILDER_FUNCTION)) {
        return 0;
    }
    if (position < 0) return 0;
    /* Refuse to analyse non-tuple returns at non-zero position. */
    if (!fn_def->node_type ||
        fn_def->node_type->kind != TYPE_TUPLE ||
        position >= fn_def->node_type->tuple_count) {
        return 0;
    }
    /* Memo hit on a pre-parsed positions string. */
    int cached = parse_heap_positions_annotation(fn_def->annotation, position);
    if (cached >= 0) return cached;
    /* Currently analysing (cycle break) — conservative no-heap. Same
     * shape as the single-value analyzer's "heap_pending" sentinel. */
    if (fn_def->annotation &&
        strcmp(fn_def->annotation, "heap_pending") == 0) {
        return 0;
    }
    /* Some unrelated annotation (e.g. "c_callback:...", "heap_yes"
     * for a single-value function that's somehow being asked at
     * position 0) — analyse without clobbering. */
    int memoise = (fn_def->annotation == NULL);
    if (memoise) fn_def->annotation = strdup("heap_pending");

    int tuple_count = fn_def->node_type->tuple_count;
    int* per_pos = (int*)calloc((size_t)tuple_count, sizeof(int));

    ASTNode* body = NULL;
    for (int i = 0; i < fn_def->child_count; i++) {
        ASTNode* c = fn_def->children[i];
        if (c && c->type == AST_BLOCK) { body = c; break; }
    }
    if (body) {
        for (int p = 0; p < tuple_count; p++) {
            int found = 0, any_heap = 0, vetoed = 0;
            walk_returns_for_heap_at(gen, body, p, body, fn_def->value,
                                     &found, &any_heap, &vetoed);
            /* Heap at `p` iff some return makes it heap (OR-fold) AND
             * no whole-tuple-passthrough return yields an unwrappable
             * non-heap value there (veto). The veto wins — see the
             * walker comment. */
            per_pos[p] = (found && any_heap && !vetoed) ? 1 : 0;
        }
    }

    int result = per_pos[position];

    if (memoise) {
        /* Build "heap_positions:1,0,1\0" — at most 2*tuple_count + 16. */
        size_t cap = (size_t)tuple_count * 2u + 32u;
        char* buf = (char*)malloc(cap);
        size_t off = (size_t)snprintf(buf, cap, "heap_positions:");
        for (int p = 0; p < tuple_count; p++) {
            off += (size_t)snprintf(buf + off, cap - off, "%s%d",
                                    p ? "," : "", per_pos[p]);
        }
        free(fn_def->annotation);
        fn_def->annotation = buf;
    }
    free(per_pos);
    return result;
}

/* Is tuple `position` of `fallible`'s result classified heap by
 * `function_def_returns_heap_at`? Shared by the `or` lowering's
 * error-slot free (position = last) and its value-slot uniform-heap
 * boxing (position 0). True only when the fallible is a call to a
 * resolvable user function — for such a function
 * emit_tuple_return_position wrapped EVERY return at that position in
 * `aether_uniform_heap_str`, so the slot is uniformly malloc-owned.
 * Callees with no resolvable definition (externs, unknowns) are
 * conservatively non-heap, matching the `v, e = f()` destructure
 * default. */
static int or_fallible_slot_is_heap(CodeGenerator* gen, ASTNode* fallible,
                                    int position) {
    if (!fallible || fallible->type != AST_FUNCTION_CALL || !fallible->value ||
        !gen || !gen->program) {
        return 0;
    }
    Type* tup = fallible->node_type;
    if (!tup || tup->kind != TYPE_TUPLE || tup->tuple_count < 2) return 0;
    if (position < 0 || position >= tup->tuple_count) return 0;
    const char* fn = codegen_normalise_callee(fallible->value);
    ASTNode* callee = find_function_definition_by_name(gen->program, fn);
    if (!callee) return 0;
    return function_def_returns_heap_at(gen, callee, position);
}

/* Is the error (last) slot of `fallible` a heap-owned string that the
 * `or` lowering must release when it discards it? When true,
 * emit_tuple_return_position wrapped every return (including the `""`
 * success sentinel) in `aether_uniform_heap_str`, so the slot is always
 * malloc-owned; when false (e.g. `string.to_long` returning the raw
 * literal "invalid long") the slot is a `.rodata` literal that must NOT
 * be freed. Same gate the `v, e = f()` destructure site uses for
 * `_heap_e`, keeping the two forms consistent. */
int or_fallible_error_slot_is_heap(CodeGenerator* gen, ASTNode* fallible) {
    Type* tup = fallible ? fallible->node_type : NULL;
    if (!tup || tup->kind != TYPE_TUPLE || tup->tuple_count < 2) return 0;
    return or_fallible_slot_is_heap(gen, fallible, tup->tuple_count - 1);
}

/* Is the VALUE (position 0) slot of `fallible` a heap-owned string? Used
 * by the `or` lowering to decide uniform-heap boxing of the result: when
 * the success value is heap, the `or`-expression is classified heap (so
 * the caller's `_heap_<lhs>` tracker frees it at scope exit) and the
 * handler's non-heap default is boxed via `aether_uniform_heap_str` so
 * BOTH paths yield a uniformly malloc-owned pointer. */
int or_fallible_value_slot_is_heap(CodeGenerator* gen, ASTNode* fallible) {
    return or_fallible_slot_is_heap(gen, fallible, 0);
}

/* Emit one position of a multi-value `return e0, e1, ...`. When the
 * enclosing function classifies tuple position `j` as a heap-string
 * position (`function_def_returns_heap_at`), route that position's
 * value through `aether_uniform_heap_str` so EVERY return path hands
 * the caller a malloc-owned pointer it can free uniformly: a heap
 * value passes through (fast path), a literal / borrowed value is
 * malloc-duplicated. Without this, a tuple-returning function with
 * a mixed string position — heap on some return sites, a borrowed
 * literal on others, the zlib/cryptography/lzf decode shape
 * (`return owned, n, ""` vs `return "", 0, "err"`) — either leaked
 * the heap branch (caller told not to free) or, post-OR-fold, would
 * free a string literal on the error branch. The wrap closes both.
 * Non-heap positions and non-string positions emit raw.
 * See string-new-with-length-heap-annotation.md. */
/* #2501: is `call` a `call(f, ...)` whose `f` is known to be one closure
 * literal of this program (codegen's closure_var_map), whose tuple string
 * slots are handed over owned? A function value or an unknown closure may
 * return borrowed slots, which stay the caller's to leave alone. */
static int call_targets_closure_literal(CodeGenerator* gen, ASTNode* call) {
    if (!call || call->type != AST_FUNCTION_CALL || !call->value ||
        strcmp(call->value, "call") != 0 || call->child_count < 1) return 0;
    ASTNode* f = call->children[0];
    if (!f) return 0;
    if (f->type == AST_CLOSURE) return 1;
    if (f->type != AST_IDENTIFIER || !f->value) return 0;
    return closure_var_id(gen, gen->closure_var_scope, f->value) >= 0;
}

static void emit_tuple_return_position(CodeGenerator* gen, ASTNode* expr,
                                       int j) {
    int pos_heap = 0;
    if (gen && gen->current_function && !gen->in_main_function) {
        Type* rt = gen->current_func_return_type;
        if (rt && rt->kind == TYPE_TUPLE && j >= 0 && j < rt->tuple_count &&
            rt->tuple_types[j] && rt->tuple_types[j]->kind == TYPE_STRING) {
            /* #2501: a closure hands every string slot over owned, as a
             * string closure does its result (#2054): its caller reaches it
             * through a value and cannot ask which returns are heap. */
            pos_heap = gen->current_function->type == AST_CLOSURE ||
                       function_def_returns_heap_at(gen, gen->current_function, j);
        }
    }
    if (pos_heap) {
        emit_uniform_heap_return_expr(gen, expr);
    } else {
        generate_expression(gen, expr);
    }
}

// Recursive: collect every variable name that may need a heap-string
// tracker — i.e. every variable that appears as the LHS of an
// AST_VARIABLE_DECLARATION (in Aether, "decl" covers both first-
// assignment and reassignment) where the RHS could yield a string.
//
// "Could yield a string" is intentionally conservative:
//   - The RHS is a heap-string-expr (string_concat, interp, or a
//     user-defined `-> string` function) → definitely needs tracking.
//   - The variable's type-annotated TYPE_STRING → tracking is cheap
//     defence (one int per string var); makes follow-up reassignments
//     to heap RHS in a different scope correct.
//
// Walking is purely structural: every nested block, every loop body,
// every if-then / if-else, every match arm. The hoist must see all
// of them so a name first-assigned at depth-3 and reassigned at
// depth-1 still has a function-scope tracker.
//
// Issue #405 — the architectural fix that unblocks the string-leak
// bug from bug_repo.md. Without this pre-pass, `_heap_<name>` was
// declared at the C scope where the variable was first seen, which
// went out of scope when control left that block. Cross-block
// reassignment of a string variable then either failed to compile
// (`'_heap_x' undeclared`) or silently leaked the old value.
static void collect_heap_string_var_names(CodeGenerator* gen, ASTNode* node,
                                          const char** names,
                                          int* count, int cap) {
    if (!node || *count >= cap) return;

    if (node->type == AST_VARIABLE_DECLARATION && node->value &&
        strcmp(node->value, "_") != 0 &&
        !is_module_global_var(gen, node->value)) {
        /* #701/#744: a module-level string `var` is a file-scope
         * `static const char*`, not a function local. It must NOT be
         * collected as a heap-tracked local — doing so would (a) hoist
         * a shadowing `const char* <name> = NULL;` over the global,
         * (b) route the write through the reassignment wrapper into
         * that shadow (so cross-function reads never see the new
         * value), and (c) push a function-exit defer-free that frees
         * the process-lifetime global (a UAF/double-free across
         * calls). Skipping the name here leaves is_var_declared false,
         * so the AST_VARIABLE_DECLARATION emitter routes the write to
         * the static (is_module_global_var branch) exactly like the
         * scalar case, and the global is never freed — the latest
         * value persists for the process lifetime, mirroring the
         * scalar/#701 model. This matches how hoist_loop_vars and the
         * if-branch hoists already skip module globals by name. */
        /* An optional-typed LHS (`b: string? = <string>`) is an
         * `ae_opt_string`, NOT a bare `const char*`. Without this guard
         * the initializer-type check below grabs it (the RHS is
         * TYPE_STRING), hoists a `const char* b`, and the optional
         * decl-site then assigns an `ae_opt_string` struct into that
         * `char*` slot — a C type error. Optional-of-string locals are
         * owned exclusively by the opt-str registry
         * (collect_opt_str_var_names); skip them here. */
        if (node->node_type && node->node_type->kind == TYPE_OPTIONAL) {
            for (int i = 0; i < node->child_count; i++)
                collect_heap_string_var_names(gen, node->children[i], names, count, cap);
            return;
        }
        // Decide whether this declaration's LHS deserves a tracker.
        // Bare `_` is a per-use discard, never a tracked variable —
        // skipped here so a string-typed `_` destructure slot doesn't
        // get hoisted as a `const char* _` the codegen then reuses.
        int needs_tracker = 0;
        if (node->child_count > 0 && is_heap_string_expr(gen, node->children[0])) {
            needs_tracker = 1;
        }
        // Type-annotated string variable (covers `s: string = ""`).
        if (!needs_tracker && node->node_type &&
            node->node_type->kind == TYPE_STRING) {
            needs_tracker = 1;
        }
        // Initializer-typed string (covers `s = ""` where the
        // typechecker stamped TYPE_STRING on the RHS).
        if (!needs_tracker && node->child_count > 0 && node->children[0] &&
            node->children[0]->node_type &&
            node->children[0]->node_type->kind == TYPE_STRING) {
            needs_tracker = 1;
        }
        if (needs_tracker) {
            int already = 0;
            for (int i = 0; i < *count; i++) {
                if (strcmp(names[i], node->value) == 0) { already = 1; break; }
            }
            if (!already && *count < cap) {
                names[(*count)++] = node->value;
            }
        }
    }

    for (int i = 0; i < node->child_count; i++) {
        collect_heap_string_var_names(gen, node->children[i], names, count, cap);
    }
}

// Emit `int _heap_<name> = 0; (void)_heap_<name>;` at function-entry
// scope for every string variable in `body`, AND additionally hoist
// the C-level `const char* <name> = NULL;` declaration to the same
// scope. Caller invokes this after parameters are declared and
// before the body is generated.
//
// After this runs, `is_heap_string_var(gen, name)` and
// `is_var_declared(gen, name)` both return true for every collected
// non-special name, so the per-stmt codegen routes ALL assignments
// (including the original "first" assignment) through the
// reassignment path at line 1839+ and emits the wrapper. The
// wrapper reads `_tmp_old` from the function-scoped slot — never
// from a freshly-declared per-block stack slot.
//
// History: the original 0.135.0 fix (#405) hoisted only the tracker
// (the `int _heap_<name>`). The C-level variable declaration stayed
// at its original first-use point, which `hoist_loop_vars` /
// `hoist_if_branch_vars` then promoted to the loop-enclosing or if-
// referenced-outside C scope — but NOT to function scope. When the
// same Aether name was first-assigned in two sibling C-blocks (e.g.
// `if (...) { ... name = ... }` followed by another `if (...)
// { ... name = ... }`), the codegen emitted two separate
// uninitialised C variables sharing one function-scoped tracker.
// The wrapper at the second block's first assignment read
// `_tmp_old = name` from the freshly-declared (uninitialised) stack
// slot, evaluated `if (_heap_name)` against the tracker (= 1 from
// the first block's last iteration), and called `free()` on stack
// garbage — glibc abort. The fix here makes the architectural
// intent self-consistent: tracker AND the variable it tracks both
// at function scope, lock-step.
//
// Skipped names (tracker is still hoisted; only the C var hoist is
// skipped):
//   - already-declared (function parameters, vars hoisted by
//     hoist_if_branch_vars before this pass) — would emit a
//     duplicate declaration.
//   - actor state vars — accessed via `self->name`; a local
//     `const char* name = NULL` would be unused (-Wunused-variable
//     under -Werror) and shadow nothing useful.
//   - env captures — closure body accesses via `_env->name`.
//   - promoted captures — declared as `int* name = malloc(...)` by
//     the closure-promotion path; declaring `const char*` here
//     would create a conflicting pre-decl.
/* #2528: is `name` a `string` state field of the actor being emitted, whose
 * tracker is a field of the actor struct? */
static int actor_state_string_tracked(CodeGenerator* gen, const char* name) {
    if (!gen->current_actor || !gen->program || !is_actor_state_var(gen, name)) return 0;
    for (int a = 0; a < gen->program->child_count; a++) {
        ASTNode* actor = gen->program->children[a];
        if (!actor || actor->type != AST_ACTOR_DEFINITION || !actor->value ||
            strcmp(actor->value, gen->current_actor) != 0) continue;
        for (int i = 0; i < actor->child_count; i++) {
            ASTNode* sd = actor->children[i];
            if (sd && sd->type == AST_STATE_DECLARATION && sd->value &&
                strcmp(sd->value, name) == 0) return state_field_owns_string(sd);
        }
    }
    return 0;
}

void hoist_heap_string_trackers(CodeGenerator* gen, ASTNode* body) {
    if (!body || !gen) return;
    const char* names[256];  // 256 string vars per fn is generous
    int count = 0;
    collect_heap_string_var_names(gen, body, names, &count, 256);
    for (int i = 0; i < count; i++) {
        const char* name = names[i];
        /* Tracker hoist (existing) — applies to ALL collected names
         * including state vars / env caps / promoted caps, because
         * the wrapper sites for those still reference _heap_<name>.
         * #2528: a string state field's tracker is the actor's own
         * (`self->_heap_<name>`), so the handler aliases the name to it
         * (undefined again where the handler ends, codegen_actor.c): the
         * handler-local `int _heap_<name> = 0` forgot between messages
         * what the state owned. */
        if (!is_heap_string_var(gen, name)) {
            /* #2528: a string state field's tracker is the actor's own,
             * `self->_heap_<name>`, spelt at the state store sites; the
             * name is not a heap-string local here, so a take of the
             * field's value copies it (the state keeps its own). A handler
             * local of that name cannot exist (an assignment to the name
             * is a state store). */
            if (actor_state_string_tracked(gen, name)) continue;
            print_indent(gen);
            fprintf(gen->output,
                    "int _heap_%s = 0; (void)_heap_%s;\n",
                    name, name);
            mark_heap_string_var(gen, name);
        }

        /* C-variable hoist (the second half of the architectural fix
         * — see the function comment above). Skip names that aren't
         * simple function-local C vars. */
        if (is_var_declared(gen, name)) continue;
        if (gen->current_actor) {
            int is_state = 0;
            for (int s = 0; s < gen->state_var_count; s++) {
                if (gen->actor_state_vars[s] &&
                    strcmp(gen->actor_state_vars[s], name) == 0) {
                    is_state = 1; break;
                }
            }
            if (is_state) continue;
        }
        int is_env_cap = 0;
        for (int e = 0; e < gen->current_env_capture_count; e++) {
            if (gen->current_env_captures[e] &&
                strcmp(gen->current_env_captures[e], name) == 0) {
                is_env_cap = 1; break;
            }
        }
        if (is_env_cap) continue;
        if (is_promoted_capture(gen, name)) continue;

        print_indent(gen);
        fprintf(gen->output, "const char* %s = NULL;\n", name);
        /* #2124: hoisted as a string; a binding of another kind anywhere
         * in the function is checked against that. */
        Type* st = create_type(TYPE_STRING);
        mark_var_declared_typed(gen, name, st);
        free_type(st);
    }
}

/* Collect `*StringSeq`-typed local declaration names (parallel to
 * collect_heap_string_var_names). A declaration is seq-typed if its LHS
 * or initializer carries a *StringSeq type. */
static void collect_seq_var_names(CodeGenerator* gen, ASTNode* node,
                                  const char** names, int* count, int cap) {
    if (!node || *count >= cap) return;
    if (node->type == AST_VARIABLE_DECLARATION && node->value &&
        strcmp(node->value, "_") != 0) {
        int is_seq = (node->node_type && is_string_seq_ptr_type(node->node_type));
        if (!is_seq && node->child_count > 0 && node->children[0] &&
            node->children[0]->node_type &&
            is_string_seq_ptr_type(node->children[0]->node_type)) {
            is_seq = 1;
        }
        if (is_seq) {
            int already = 0;
            for (int i = 0; i < *count; i++)
                if (strcmp(names[i], node->value) == 0) { already = 1; break; }
            if (!already && *count < cap) names[(*count)++] = node->value;
        }
    }
    for (int i = 0; i < node->child_count; i++)
        collect_seq_var_names(gen, node->children[i], names, count, cap);
}

/* Hoist `int _seqheap_<name> = 0;` + the function-scope `StringSeq*
 * <name> = NULL;` declaration for every *StringSeq local, mirroring
 * hoist_heap_string_trackers. Function-scope hoisting means the
 * scope-exit defer-free can always reference the slot, and every
 * assignment (including the first) routes through the reassignment
 * wrapper that maintains the ownership flag. */
void hoist_seq_trackers(CodeGenerator* gen, ASTNode* body) {
    if (!body || !gen) return;
    const char* names[256];
    int count = 0;
    collect_seq_var_names(gen, body, names, &count, 256);
    for (int i = 0; i < count; i++) {
        const char* name = names[i];
        if (!is_seq_var(gen, name)) {
            print_indent(gen);
            fprintf(gen->output,
                    "int _seqheap_%s = 0; (void)_seqheap_%s;\n", name, name);
            mark_seq_var(gen, name);
        }
        if (is_var_declared(gen, name)) continue;
        if (gen->current_actor) {
            int is_state = 0;
            for (int s = 0; s < gen->state_var_count; s++)
                if (gen->actor_state_vars[s] &&
                    strcmp(gen->actor_state_vars[s], name) == 0) { is_state = 1; break; }
            if (is_state) continue;
        }
        int is_env_cap = 0;
        for (int e = 0; e < gen->current_env_capture_count; e++)
            if (gen->current_env_captures[e] &&
                strcmp(gen->current_env_captures[e], name) == 0) { is_env_cap = 1; break; }
        if (is_env_cap) continue;
        if (is_promoted_capture(gen, name)) continue;
        print_indent(gen);
        fprintf(gen->output, "StringSeq* %s = NULL;\n", name);
        mark_var_declared(gen, name);
    }
}

/* True for a `string?` type: an optional wrapping a bare string. The
 * heap-ownership tracking only applies to string payloads (scalar
 * optionals like `int?` carry no allocation). */
static int is_opt_string_type(Type* t) {
    return t && t->kind == TYPE_OPTIONAL &&
           t->element_type && t->element_type->kind == TYPE_STRING;
}

/* Does the RHS of a `string? = <rhs>` assignment produce an optional
 * whose `.val` is a freshly-owned heap buffer this slot must free?
 *
 * Two provenance cases, mirroring emit_optional_coerced:
 *   - WRAP (a bare string value coerced into the optional): owned iff
 *     the value is a heap producer (is_heap_string_expr).
 *   - PASSTHROUGH (the RHS is already a `string?`): owned only when it
 *     is a function call returning `string?` — the callee transfers
 *     ownership of `.val` to us (its return-escape suppression means it
 *     will NOT free the buffer). An identifier passthrough (`o = other`)
 *     is an alias, NOT an ownership transfer: treating it as owned would
 *     double-free when both locals' exit-frees fire, so it stays
 *     unowned and the source local frees it. Other optional-yielding
 *     shapes (`??`, `?.`, match-expr) may borrow, so also unowned —
 *     conservative: never double-frees, at worst leaks a rare case. */
static int is_heap_opt_string_rhs(CodeGenerator* gen, ASTNode* rhs) {
    if (!rhs) return 0;
    if (rhs->type == AST_NONE_LITERAL) return 0;
    /* A bare identifier RHS — `b = a` where `a` names a heap-string or
     * another opt-str local — is an ALIAS, not an ownership transfer:
     * the source local already owns the buffer and will free it. Taking
     * ownership here too would double-free. Leave `b` unowned; the
     * source's tracker reclaims the buffer. (True ownership arrives only
     * via a fresh producer — a call or a heap-string expression that
     * isn't itself a tracked slot.) */
    if (rhs->type == AST_IDENTIFIER && rhs->value &&
        (is_heap_string_var(gen, rhs->value) || is_opt_str_var(gen, rhs->value))) {
        return 0;
    }
    if (rhs->node_type && rhs->node_type->kind == TYPE_OPTIONAL) {
        return rhs->type == AST_FUNCTION_CALL;
    }
    return is_heap_string_expr(gen, rhs);
}

/* Collect `string?` local declaration names (parallel to
 * collect_seq_var_names). Keyed on the LHS or initializer carrying a
 * `string?` type. */
static void collect_opt_str_var_names(CodeGenerator* gen, ASTNode* node,
                                      const char** names, int* count, int cap) {
    if (!node || *count >= cap) return;
    if (node->type == AST_VARIABLE_DECLARATION && node->value &&
        strcmp(node->value, "_") != 0 &&
        !is_module_global_var(gen, node->value)) {
        int is_opt = is_opt_string_type(node->node_type);
        if (!is_opt && node->child_count > 0 && node->children[0] &&
            is_opt_string_type(node->children[0]->node_type)) {
            is_opt = 1;
        }
        if (is_opt) {
            int already = 0;
            for (int i = 0; i < *count; i++)
                if (strcmp(names[i], node->value) == 0) { already = 1; break; }
            if (!already && *count < cap) names[(*count)++] = node->value;
        }
    }
    for (int i = 0; i < node->child_count; i++)
        collect_opt_str_var_names(gen, node->children[i], names, count, cap);
}

/* Hoist `int _heapopt_<name> = 0;` + the function-scope `ae_opt_string
 * <name> = (ae_opt_string){0};` declaration for every `string?` local,
 * mirroring hoist_seq_trackers. Function-scope hoisting lets the
 * scope-exit defer-free always reference the slot, and routes every
 * assignment (including the first) through the reassignment path so the
 * ownership flag is maintained and the prior `.val` is freed. */
void hoist_opt_str_trackers(CodeGenerator* gen, ASTNode* body) {
    if (!body || !gen) return;
    const char* names[256];
    int count = 0;
    collect_opt_str_var_names(gen, body, names, &count, 256);
    for (int i = 0; i < count; i++) {
        const char* name = names[i];
        if (!is_opt_str_var(gen, name)) {
            print_indent(gen);
            fprintf(gen->output,
                    "int _heapopt_%s = 0; (void)_heapopt_%s;\n", name, name);
            mark_opt_str_var(gen, name);
        }
        if (is_var_declared(gen, name)) continue;
        /* Actor state vars (accessed via self->name), env captures and
         * promoted captures don't live as a plain function-scope
         * `ae_opt_string` — skip the C-var hoist for them, exactly as
         * hoist_seq_trackers / hoist_heap_string_trackers do. */
        if (gen->current_actor) {
            int is_state = 0;
            for (int s = 0; s < gen->state_var_count; s++)
                if (gen->actor_state_vars[s] &&
                    strcmp(gen->actor_state_vars[s], name) == 0) { is_state = 1; break; }
            if (is_state) continue;
        }
        int is_env_cap = 0;
        for (int e = 0; e < gen->current_env_capture_count; e++)
            if (gen->current_env_captures[e] &&
                strcmp(gen->current_env_captures[e], name) == 0) { is_env_cap = 1; break; }
        if (is_env_cap) continue;
        if (is_promoted_capture(gen, name)) continue;
        print_indent(gen);
        fprintf(gen->output, "ae_opt_string %s = (ae_opt_string){0};\n", name);
        mark_var_declared(gen, name);
    }
}

// =====================================================================
// Escape analysis for heap-string variables
//
// Pre-pass that walks a function body and marks heap-tracked string
// variables as "escaped" when their value is passed somewhere the
// recipient may store the pointer raw — most commonly a function-call
// argument that isn't the RHS of `V = ...` (where V is the LHS), or a
// closure capture. The wrapper at codegen_stmt.c:1611 then skips its
// `free(_tmp_old)` for escaped vars: freeing a value that has been
// adopted by `map.put`/`list.add`/an actor message/etc. would dangle
// the stored copy and produce a use-after-free.
//
// The "consumed transiently" exception covers the canonical bug_repo
// pattern from #405:
//
//     while i < N {
//         s = my_concat(s, "x")    // s on RHS, but LHS is also s
//         i = i + 1
//     }
//
// Here the call only reads the old `s` to build the new one — the
// recipient (my_concat) returns a fresh value and the result replaces
// `s`. The old `s` is genuinely unreachable after the call. So we
// don't mark `s` escaped on the strength of its appearance inside its
// own assignment's RHS — only on appearances outside that exception.
//
// Conservative everywhere else: any other call argument, any closure
// capture, any non-RHS use is treated as "may have stored the
// pointer". That makes the analysis alias-safe: heap-tracked vars
// either get freed correctly (no escape → wrapper fires) or leak for
// the function's lifetime (escape → wrapper skipped). Strictly
// better than the pre-pass UAF.
//
// Soundness boundary: this catches function-call arguments (which is
// where 90%+ of the alias bugs live: map.put, list.add, actor
// `send`, struct/message field init via fn-call wrappers). It does
// not yet catch direct struct-field writes (`s.field = x`) or array
// element writes (`a[i] = x`); those land as AST_ASSIGNMENT (LHS-as-
// expr shape) rather than AST_FUNCTION_CALL, and the rare cases
// they cover would also leak rather than UAF if added. Worth a
// follow-up if a downstream surfaces one.
// =====================================================================

// Walks `node` looking for AST_FUNCTION_CALL or AST_METHOD_CALL whose
// arguments include identifiers that name a heap-tracked string var.
// `consumed_lhs`, when non-NULL, names the variable whose own
// assignment RHS we're inside — that LHS is exempted from the escape
// mark for the duration of this subwalk (the bug_repo "consumed
// transiently" exception above).
static void escape_walk(CodeGenerator* gen, ASTNode* node,
                         const char* consumed_lhs);

/* Look up the parameter type-kind for the n-th argument of a callee
 * named `func_name` (in either dotted source-form or underscored
 * extern-form). Returns the param's TypeKind, or TYPE_UNKNOWN if the
 * callee can't be resolved. */
TypeKind lookup_callee_param_kind(CodeGenerator* gen,
                                          const char* func_name,
                                          int param_idx) {
    if (!gen || !func_name || param_idx < 0) return TYPE_UNKNOWN;
    const char* fn = codegen_normalise_callee(func_name);
    /* Externs first — registered with param-kind table. */
    TypeKind k = lookup_extern_param_kind(gen, fn, param_idx);
    if (k != TYPE_UNKNOWN) return k;
    /* Fall back to user-defined fn lookup via the program AST. */
    if (gen->program) {
        ASTNode* fn_def = find_function_definition_by_name(gen->program, fn);
        if (fn_def && param_idx < fn_def->child_count) {
            ASTNode* param = fn_def->children[param_idx];
            if (param && param->node_type) {
                return param->node_type->kind;
            }
        }
    }
    return TYPE_UNKNOWN;
}

/* Decide whether passing a heap-string variable as an argument to
 * `func_name` at position `param_idx` should be treated as an escape.
 *
 * The heuristic: storage usually happens through a `ptr` parameter
 * (opaque pointer — the callee can stash it anywhere). Other typed
 * parameters (`string`, `int`, `bool`, structs, etc.) are typically
 * read-and-consume — `string.length`, `string.equals`, `print`,
 * comparison ops. Treating those as escape would re-create the leak
 * the wrapper is meant to fix (#405's bug_repo loop has
 * `string.length(s)` outside the loop, which would otherwise mark
 * `s` escaped and skip the wrapper inside the loop).
 *
 * Conservative when the callee can't be resolved (TYPE_UNKNOWN): we
 * assume escape rather than not, because mis-marking as non-escape
 * costs a UAF (worse than the leak from over-marking). The common
 * case — known stdlib + user fns visible in the program — resolves
 * cleanly. */
int call_arg_escapes(TypeKind param_kind) {
    switch (param_kind) {
        case TYPE_STRING:
        case TYPE_INT:
        case TYPE_INT64:
        case TYPE_UINT64:
        case TYPE_UINT32:
        case TYPE_UINT16:
        case TYPE_UINT8:
        case TYPE_DURATION:
        case TYPE_BYTE:
        case TYPE_FLOAT:
        case TYPE_LONGDOUBLE:
        case TYPE_BOOL:
        case TYPE_VOID:
            return 0;
        case TYPE_PTR:
        case TYPE_UNKNOWN:
        case TYPE_WILDCARD:
        default:
            return 1;
    }
}

/* Interprocedural escape: does parameter `param_idx` of user function
 * `func_name` flow into an escaping sink within that function's body —
 * i.e. is it passed (as a bare identifier) to a call-argument position
 * that itself escapes (a `ptr`/unknown param, a `@retain` extern param,
 * or, recursively, another user wrapper whose param escapes), or
 * returned?
 *
 * This is the missing edge behind the map-value use-after-free
 * (heap-string-map-value-use-after-free-multi-tu.md): a storing wrapper
 * `store(m, v: string) { map.put(m, k, v) }` has a `string`-typed value
 * parameter, which `call_arg_escapes` treats as read-only. Without this
 * check the caller's heap local — passed as `v` — is never marked
 * escaped, so it's freed at the caller's scope exit while the map still
 * holds the pointer. Looking through the callee's body sees `v` reach
 * `map.put`'s `ptr` value parameter and reports the escape, so the
 * caller keeps the buffer alive.
 *
 * Conservative on recursion overflow (returns escape — over-marking is
 * a leak, under-marking is a UAF). Handles only direct param→sink flow
 * and param-return; aliasing the param through an intermediate local is
 * not tracked (rare, and the safe direction would only be a leak). */
static int param_escapes_in_subtree(CodeGenerator* gen, ASTNode* node,
                                     const char* pname, int depth,
                                     int return_is_escape);
/* #2499: set while a copy-on-keep query (closure_string_param_kept,
 * callee_string_param_kept) asks whether a `string` parameter's REFERENCE
 * outlives the call. A capture by a nested closure takes its own reference,
 * and an alias into a local keeps it only if that local does, so neither
 * is a keep by itself there. */
static int g_capture_holds_own_ref = 0;
/* The body the query walks (the function's or closure's own, switched by
 * each nested callee walk), where an alias's later uses are found, and the
 * closure whose captures are outer variables rather than locals. */
static ASTNode* g_keep_body = NULL;
static ASTNode* g_keep_closure = NULL;
/* The named function whose body a copy-on-keep query walks, where a
 * pointer's provenance is looked up (box_trackers_are_initialised). */
static ASTNode* g_keep_fn = NULL;
static int value_directly_carries_param(ASTNode* node, const char* pname);
static int handback_leaf_is(CodeGenerator* gen, ASTNode* expr, const char* name, int depth);
static int is_nonstoring_builtin(const char* fn);
static int is_consuming_free(CodeGenerator* gen, const char* fn);

/* In a copy-on-keep query, is `name` a local of the walked body, so that
 * assigning the parameter to it keeps the reference only if `name` keeps
 * it? A module global, or (in a closure) a captured outer variable, holds
 * it past the call. */
static int keep_alias_is_local(CodeGenerator* gen, const char* name) {
    if (!name || !g_keep_body || is_module_global_var(gen, name)) return 0;
    if (g_keep_closure) {
        for (int ci = 0; ci < gen->closure_count; ci++) {
            if (gen->closures[ci].closure_node != g_keep_closure) continue;
            for (int k = 0; k < gen->closures[ci].capture_count; k++) {
                if (gen->closures[ci].captures[k] &&
                    strcmp(gen->closures[ci].captures[k], name) == 0) return 0;
            }
        }
    }
    return 1;
}

/* Shared resolver: find user-fn `func_name`'s param-name + body block.
 * Returns 1 and fills out_pname and out_body on success; 0 otherwise. */
/* #2528: set while the walk below asks about a closure-typed parameter. A
 * holder that keeps a closure takes a reference of its own (#2525: a struct
 * or message field, a global, an actor's state), so storing the parameter
 * there keeps nothing of the caller's; for a string, the same store takes
 * the pointer. */
static int g_escape_param_is_closure = 0;

static int resolve_callee_param_body(CodeGenerator* gen, const char* func_name,
                                     int param_idx, const char** out_pname,
                                     ASTNode** out_body) {
    if (!gen || !gen->program || !func_name || param_idx < 0) return 0;
    const char* fn = codegen_normalise_callee(func_name);
    ASTNode* fn_def = find_function_definition_by_name(gen->program, fn);
    if (!fn_def || param_idx >= fn_def->child_count) return 0;
    ASTNode* param = fn_def->children[param_idx];
    if (!param || !param->value ||
        (param->type != AST_VARIABLE_DECLARATION &&
         param->type != AST_PATTERN_VARIABLE)) {
        return 0;
    }
    g_escape_param_is_closure = param->node_type && param->node_type->kind == TYPE_FUNCTION &&
                                !param->node_type->is_fnptr;
    ASTNode* body = NULL;
    for (int i = fn_def->child_count - 1; i >= 0; i--) {
        if (fn_def->children[i] && fn_def->children[i]->type == AST_BLOCK) {
            body = fn_def->children[i];
            break;
        }
    }
    if (!body) return 0;
    *out_pname = param->value;
    *out_body = body;
    return 1;
}

int callee_param_escapes_via_body(CodeGenerator* gen, const char* func_name,
                                  int param_idx, int depth) {
    if (depth > 8) return 1;  /* recursion / mutual-recursion guard */
    const char* pname; ASTNode* body;
    int saved_cl = g_escape_param_is_closure;   /* #2528: per walk */
    if (!resolve_callee_param_body(gen, func_name, param_idx, &pname, &body)) {
        g_escape_param_is_closure = saved_cl;
        return 0;
    }
    /* Used by the arg-drain / escape-pre-pass gates: a `return pname`
     * IS an escape (the value flows out to the caller). */
    ASTNode* saved_body = g_keep_body;
    ASTNode* saved_closure = g_keep_closure;
    g_keep_body = body;
    g_keep_closure = NULL;
    int r = param_escapes_in_subtree(gen, body, pname, depth, /*return_is_escape=*/1);
    g_keep_body = saved_body;
    g_keep_closure = saved_closure;
    g_escape_param_is_closure = saved_cl;
    return r;
}

/* #2499 copy-on-keep, for a named function called through its closure
 * adapter (emit_bare_fn_adapters): does it keep its `string` parameter
 * `param_idx` past the call, so the adapter must hand it a reference of its
 * own? The same question closure_string_param_kept asks of a closure
 * literal: a capture or an alias into a local that keeps nothing is no
 * keep. A return counts only when `return_is_keep`. */
static int callee_string_param_kept_at(CodeGenerator* gen, const char* func_name, int param_idx,
                                       int return_is_keep, int depth);

/* The ownership answers about a callee's `string` parameter are properties
 * of the callee's body alone, so each is computed once per program and
 * remembered (gen->callee_memo). Computed afresh (the depth bound restarts
 * at each callee), and asked again while it is being computed (a function
 * reaching itself), a query gets the answer the depth bound used to give:
 * the one that only ever keeps a caller's argument alive longer.
 * Unremembered, every walk re-walked every callee at every call site, a
 * cost that multiplied with nesting. */
enum { CALLEE_Q_HANDBACK, CALLEE_Q_KEEPS, CALLEE_Q_CAPTURES, CALLEE_Q_KEPT, CALLEE_Q_KEPT_RET };

typedef struct {
    ASTNode* fn;
    int key;     /* param_idx * 8 + query */
    int state;   /* 0 empty, 1 being computed, 2 no, 3 yes */
} CalleeMemo;

static size_t callee_memo_hash(const ASTNode* fn, int key, int cap) {
    uint64_t h = (uint64_t)(uintptr_t)fn >> 4;
    h = h * 0x9E3779B97F4A7C15ull + (uint64_t)(unsigned)key;
    return (size_t)(h ^ (h >> 29)) & (size_t)(cap - 1);
}

/* The entry for (fn, key), inserted empty if absent; NULL on OOM. */
static CalleeMemo* callee_memo_entry(CodeGenerator* gen, ASTNode* fn, int key) {
    if (gen->callee_memo_count * 2 >= gen->callee_memo_cap) {
        int cap = gen->callee_memo_cap ? gen->callee_memo_cap * 2 : 256;
        CalleeMemo* grown = (CalleeMemo*)calloc((size_t)cap, sizeof(CalleeMemo));
        if (!grown) return NULL;
        CalleeMemo* old = (CalleeMemo*)gen->callee_memo;
        for (int i = 0; i < gen->callee_memo_cap; i++) {
            if (!old[i].fn) continue;
            size_t j = callee_memo_hash(old[i].fn, old[i].key, cap);
            while (grown[j].fn) j = (j + 1) & (size_t)(cap - 1);
            grown[j] = old[i];
        }
        free(old);
        gen->callee_memo = grown;
        gen->callee_memo_cap = cap;
    }
    CalleeMemo* t = (CalleeMemo*)gen->callee_memo;
    size_t j = callee_memo_hash(fn, key, gen->callee_memo_cap);
    while (t[j].fn && !(t[j].fn == fn && t[j].key == key))
        j = (j + 1) & (size_t)(gen->callee_memo_cap - 1);
    if (!t[j].fn) {
        t[j].fn = fn;
        t[j].key = key;
        t[j].state = 0;
        gen->callee_memo_count++;
    }
    return &t[j];
}

static ASTNode* callee_memo_fn(CodeGenerator* gen, const char* func_name) {
    if (!gen || !gen->program || !func_name) return NULL;
    return find_function_definition_by_name(gen->program, codegen_normalise_callee(func_name));
}

/* 1 with the answer in *out when it is known, or `pending` while it is
 * being computed; 0 when the caller is to compute it (now marked) and
 * hand it to callee_memo_end; -1 when the table cannot grow, and the
 * caller computes it unremembered within its own depth bound. */
static int callee_memo_begin(CodeGenerator* gen, ASTNode* fn, int idx, int query,
                             int pending, int* out) {
    CalleeMemo* m = callee_memo_entry(gen, fn, idx * 8 + query);
    if (!m) return -1;
    if (m->state == 1) { *out = pending; return 1; }
    if (m->state) { *out = m->state == 3; return 1; }
    m->state = 1;
    return 0;
}

static void callee_memo_end(CodeGenerator* gen, ASTNode* fn, int idx, int query, int r) {
    CalleeMemo* m = callee_memo_entry(gen, fn, idx * 8 + query);
    if (m) m->state = r ? 3 : 2;
}

/* compute_closure_args_borrowed asks under a hypothesis it may drop. */
static void callee_memo_reset(CodeGenerator* gen) {
    if (gen->callee_memo)
        memset(gen->callee_memo, 0, sizeof(CalleeMemo) * (size_t)gen->callee_memo_cap);
    gen->callee_memo_count = 0;
}

int callee_string_param_kept(CodeGenerator* gen, const char* func_name, int param_idx,
                             int return_is_keep) {
    return callee_string_param_kept_at(gen, func_name, param_idx, return_is_keep, 0);
}

static int callee_string_param_kept_at_walk(CodeGenerator* gen, const char* func_name,
                                            int param_idx, int return_is_keep, int depth);

static int callee_string_param_kept_at(CodeGenerator* gen, const char* func_name, int param_idx,
                                       int return_is_keep, int depth) {
    int query = return_is_keep ? CALLEE_Q_KEPT_RET : CALLEE_Q_KEPT;
    ASTNode* fn = callee_memo_fn(gen, func_name);
    int r;
    int known = fn ? callee_memo_begin(gen, fn, param_idx, query, 1, &r) : -1;
    if (known == 1) return r;
    r = callee_string_param_kept_at_walk(gen, func_name, param_idx, return_is_keep,
                                         known == 0 ? 0 : depth);
    if (known == 0) callee_memo_end(gen, fn, param_idx, query, r);
    return r;
}

static int callee_string_param_kept_at_walk(CodeGenerator* gen, const char* func_name,
                                            int param_idx, int return_is_keep, int depth) {
    /* Mutual recursion between callees: past the bound the parameter
     * counts as kept, which only ever keeps a caller's argument alive
     * longer (the walk through callee_keeps_string_arg restarts here). */
    if (depth > 8) return 1;
    const char* pname; ASTNode* body;
    int saved_cl = g_escape_param_is_closure;   /* #2528: per walk */
    if (!resolve_callee_param_body(gen, func_name, param_idx, &pname, &body)) {
        g_escape_param_is_closure = saved_cl;
        return 1;
    }
    int saved_flag = g_capture_holds_own_ref;
    ASTNode* saved_body = g_keep_body;
    ASTNode* saved_closure = g_keep_closure;
    g_capture_holds_own_ref = 1;
    g_keep_body = body;
    g_keep_closure = NULL;
    int kept = param_escapes_in_subtree(gen, body, pname, depth, return_is_keep);
    g_capture_holds_own_ref = saved_flag;
    g_keep_body = saved_body;
    g_keep_closure = saved_closure;
    g_escape_param_is_closure = saved_cl;
    return kept;
}

/* The `string` parameter `param_idx` of `func_name`, or NULL. */
static ASTNode* callee_string_param_node(CodeGenerator* gen, const char* func_name, int param_idx) {
    if (!gen || !gen->program || !func_name || param_idx < 0) return NULL;
    const char* fn = codegen_normalise_callee(func_name);
    ASTNode* fn_def = find_function_definition_by_name(gen->program, fn);
    if (!fn_def || param_idx >= fn_def->child_count) return NULL;
    ASTNode* param = fn_def->children[param_idx];
    if (!param || !param->value || !param->node_type || param->node_type->kind != TYPE_STRING ||
        (param->type != AST_VARIABLE_DECLARATION && param->type != AST_PATTERN_VARIABLE)) return NULL;
    return param;
}

int callee_param_is_string(CodeGenerator* gen, const char* func_name, int param_idx) {
    return callee_string_param_node(gen, func_name, param_idx) != NULL;
}

/* Copy-on-keep for a named function, as for a closure (#2499): a `string`
 * parameter the body keeps past the call (a container store, a struct
 * field, a cell, a global, a call that keeps it) is the function's own
 * reference, taken on entry (generate_function) and from there on a
 * heap-tracked local that the keep moves or copies and the exit frees. A
 * return of it hands that reference to the caller (return_expr_is_heap
 * counts it as owned). The caller then borrows whatever it passed.
 * Without this the container borrowed the caller's string and the caller
 * was told to keep it alive for ever (a leak per call), or, through a
 * wrapper, the closure's own reference was stored raw and given back by
 * nobody. A promoted parameter's cell takes its own reference already
 * (emit_promoted_param_cell). `depth` bounds the walk through nested
 * callees, as every body walk here is bounded; past the bound the answer
 * is "does not capture", which only ever keeps a caller's argument alive
 * longer. */
static int param_opaque_sink(CodeGenerator* gen, ASTNode* node, const char* pname, int depth);
static int param_consumed(CodeGenerator* gen, ASTNode* node, const char* pname, int depth);

static int callee_string_param_captures_at_walk(CodeGenerator* gen, const char* func_name,
                                                int param_idx, int depth);

static int callee_string_param_captures_at(CodeGenerator* gen, const char* func_name,
                                           int param_idx, int depth) {
    ASTNode* fn = callee_memo_fn(gen, func_name);
    int r;
    int known = fn ? callee_memo_begin(gen, fn, param_idx, CALLEE_Q_CAPTURES, 0, &r) : -1;
    if (known == 1) return r;
    r = callee_string_param_captures_at_walk(gen, func_name, param_idx, known == 0 ? 0 : depth);
    if (known == 0) callee_memo_end(gen, fn, param_idx, CALLEE_Q_CAPTURES, r);
    return r;
}

static int callee_string_param_captures_at_walk(CodeGenerator* gen, const char* func_name,
                                                int param_idx, int depth) {
    if (depth > 8) return 0;
    ASTNode* param = callee_string_param_node(gen, func_name, param_idx);
    if (!param) return 0;
    const char* fn = codegen_normalise_callee(func_name);
    ASTNode* fn_def = find_function_definition_by_name(gen->program, fn);
    char** promoted = NULL;
    int promoted_count = 0;
    get_promoted_names_for_func(gen, fn_def->value, &promoted, &promoted_count);
    for (int k = 0; k < promoted_count; k++) {
        if (promoted[k] && strcmp(promoted[k], param->value) == 0) return 0;
    }
    const char* pname; ASTNode* body;
    int saved_cl = g_escape_param_is_closure;
    if (!resolve_callee_param_body(gen, func_name, param_idx, &pname, &body)) {
        g_escape_param_is_closure = saved_cl;
        return 0;
    }
    int saved_flag = g_capture_holds_own_ref;
    ASTNode* saved_body = g_keep_body;
    ASTNode* saved_closure = g_keep_closure;
    ASTNode* saved_fn = g_keep_fn;
    g_capture_holds_own_ref = 1;
    g_keep_body = body;
    g_keep_closure = NULL;
    g_keep_fn = fn_def;
    int kept = param_escapes_in_subtree(gen, body, pname, depth, /*return_is_escape=*/0);
    g_capture_holds_own_ref = saved_flag;
    g_keep_body = saved_body;
    g_keep_closure = saved_closure;
    g_escape_param_is_closure = saved_cl;
    /* Every keep must be a slot this compiler tracks (a container, a
     * struct field, a cell, a global, a callee that captures in turn): a
     * reference handed to an extern's `ptr` parameter, a `@retain`
     * parameter or a callee without a body has no releaser, so the
     * function keeps borrowing and its caller keeps the old rule. A
     * function that frees its parameter takes the caller's reference
     * (param_consumed), so it takes none of its own either. */
    int captures = 0;
    if (kept) {
        ASTNode* walked_fn = g_keep_fn;
        g_keep_fn = fn_def;
        captures = !param_opaque_sink(gen, body, pname, depth) &&
                   !param_consumed(gen, body, pname, depth);
        g_keep_fn = walked_fn;
    }
    g_keep_fn = saved_fn;
    return captures;
}

int callee_string_param_captures(CodeGenerator* gen, const char* func_name, int param_idx) {
    return callee_string_param_captures_at(gen, func_name, param_idx, 0);
}

/* Is `pname` passed, as a bare argument anywhere under `node`, to a sink
 * the compiler cannot release behind: an extern parameter that keeps it
 * (`ptr`, unknown, `@retain`), a callee without a visible body, or a
 * callee whose own parameter reaches such a sink (through its `string`
 * parameter that does not capture, or a parameter of another kind that
 * escapes in its body)? Read-only externs, consuming frees, `@noescape`
 * parameters and closure calls under the borrowed convention are not
 * sinks; a nested closure takes a reference of its own. */
/* Is `node` a store of the parameter into a field of a struct reached
 * through a pointer the compiler cannot prove is a heap.new box (#2369,
 * box_trackers_are_initialised): memory from `malloc(n) as *T`, a C pointer,
 * a parameter? Nothing destroys such a struct with its fields (it is freed
 * with free(), its strings borrowed), so a copy stored there has no releaser
 * and leaked: test_self_ref_struct's `e.msg = msg` into `malloc(64) as
 * *ErrChain`. A struct held by value, or in a heap.new box, is destroyed with
 * its fields, so a store there stays a tracked keep. */
static int param_store_through_raw_pointer(CodeGenerator* gen, ASTNode* node,
                                           const char* pname) {
    int is_store = (node->type == AST_ASSIGNMENT && node->child_count >= 2) ||
                   (node->type == AST_BINARY_EXPRESSION && node->value &&
                    strcmp(node->value, "=") == 0 && node->child_count >= 2);
    if (!is_store) return 0;
    ASTNode* lhs = node->children[0];
    if (!lhs || lhs->type != AST_MEMBER_ACCESS || lhs->child_count < 1) return 0;
    if (!value_directly_carries_param(node->children[1], pname) &&
        !handback_leaf_is(gen, node->children[1], pname, 1)) return 0;
    ASTNode* obj = lhs->children[0];
    if (!obj || !obj->node_type || obj->node_type->kind != TYPE_PTR) return 0;
    ASTNode* saved_fn = gen->current_function;
    if (g_keep_fn) gen->current_function = g_keep_fn;
    int boxed = box_trackers_are_initialised(gen, obj);
    gen->current_function = saved_fn;
    return !boxed;
}

static int param_opaque_sink(CodeGenerator* gen, ASTNode* node, const char* pname, int depth) {
    if (!node) return 0;
    if (depth > 8) return 1;
    if (param_store_through_raw_pointer(gen, node, pname)) return 1;
    if (node->type == AST_CLOSURE && !(node->value && strcmp(node->value, "trailing") == 0)) return 0;
    /* A module-level `var` never frees what it holds (process lifetime,
     * readable from any thread), so a store of the parameter into one has
     * no releaser either: the global borrows, as before. A call that only
     * hands the parameter back stores it the same way. */
    if (node->type == AST_VARIABLE_DECLARATION && node->value && node->child_count > 0 &&
        node->children[0] &&
        ((node->children[0]->type == AST_IDENTIFIER && node->children[0]->value &&
          strcmp(node->children[0]->value, pname) == 0) ||
         handback_leaf_is(gen, node->children[0], pname, depth + 1)) &&
        is_module_global_var(gen, node->value)) return 1;
    if (node->type == AST_FUNCTION_CALL && node->value) {
        const char* fn = codegen_normalise_callee(node->value);
        int is_call = strcmp(node->value, "call") == 0;
        int first_arg = is_call ? 1 : 0;
        for (int i = first_arg; i < node->child_count; i++) {
            ASTNode* a = node->children[i];
            if (!a || a->type != AST_IDENTIFIER || !a->value || strcmp(a->value, pname) != 0) continue;
            if (is_call) {
                if (!gen->closure_args_borrowed) return 1;
                continue;
            }
            if (is_nonstoring_builtin(fn) || is_consuming_free(gen, fn)) continue;
            if (is_noescape_extern_param(gen, fn, i)) continue;
            /* A list add, list set or map put of the parameter: the slot
             * the owning rewrite takes, a tracked keep. */
            if (container_store_slot(gen, node) == i) continue;
            if (is_retain_extern_param(gen, fn, i)) return 1;
            if (callee_has_visible_body(gen, node->value)) {
                if (callee_param_is_string(gen, node->value, i)) {
                    if (callee_string_param_captures_at(gen, node->value, i, depth + 1)) continue;
                    const char* cp; ASTNode* cb;
                    int saved_cl = g_escape_param_is_closure;
                    int r = resolve_callee_param_body(gen, node->value, i, &cp, &cb)
                            ? param_opaque_sink(gen, cb, cp, depth + 1) : 1;
                    g_escape_param_is_closure = saved_cl;
                    if (r) return 1;
                } else if (callee_param_escapes_via_body(gen, node->value, i, depth + 1)) {
                    return 1;
                }
                continue;
            }
            if (call_arg_escapes(lookup_callee_param_kind(gen, node->value, i))) return 1;
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        if (param_opaque_sink(gen, node->children[i], pname, depth)) return 1;
    }
    return 0;
}

/* Is `pname` handed, as a bare argument anywhere under `node`, to a free
 * (is_consuming_free), directly or through a callee that frees its own
 * parameter in turn? Such a function consumes the reference its caller
 * passed: `_free_cstr(s: string) { string_free(s) }` is how a caller that
 * holds a string it does not track (a struct field, a string from C) gives
 * it back. Copy-on-keep taking a reference of its own there freed the copy
 * and left the caller's string to nobody: std.jsonpath lost 96 bytes per
 * parse. A nested closure's frees are its own. */
static int param_consumed(CodeGenerator* gen, ASTNode* node, const char* pname, int depth) {
    if (!node || depth > 8) return 0;
    if (node->type == AST_CLOSURE && !(node->value && strcmp(node->value, "trailing") == 0)) return 0;
    if (node->type == AST_FUNCTION_CALL && node->value && strcmp(node->value, "call") != 0) {
        const char* fn = codegen_normalise_callee(node->value);
        for (int i = 0; i < node->child_count; i++) {
            ASTNode* a = node->children[i];
            if (!a || a->type != AST_IDENTIFIER || !a->value || strcmp(a->value, pname) != 0) continue;
            if (is_consuming_free(gen, fn)) return 1;
            if (callee_has_visible_body(gen, node->value) &&
                callee_param_is_string(gen, node->value, i)) {
                const char* cp; ASTNode* cb;
                int saved_cl = g_escape_param_is_closure;
                int r = resolve_callee_param_body(gen, node->value, i, &cp, &cb) &&
                        param_consumed(gen, cb, cp, depth + 1);
                g_escape_param_is_closure = saved_cl;
                if (r) return 1;
            }
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        if (param_consumed(gen, node->children[i], pname, depth)) return 1;
    }
    return 0;
}

/* The caller-side rule for a `string` argument to a function with a
 * visible body: does the caller's own pointer live on past the call? Not
 * when the callee captures the parameter (it holds a reference of its
 * own; a returned one is that reference, handed over owned). Otherwise
 * the keep walk decides, the one a closure's copy-on-keep uses: a store
 * into a sink the callee cannot release behind keeps it, a capture by a
 * nested closure does not (the env takes its own reference), and a return
 * keeps it unless the callee's string result is uniform-heap, which hands
 * back a copy. Shared by the escape walk, the argument drain and the keep
 * walk of an enclosing body, so the three never disagree. */
static int callee_keeps_string_arg_walk(CodeGenerator* gen, const char* func_name,
                                        int param_idx, int depth);

int callee_keeps_string_arg(CodeGenerator* gen, const char* func_name, int param_idx, int depth) {
    ASTNode* fn = callee_memo_fn(gen, func_name);
    int r;
    int known = fn ? callee_memo_begin(gen, fn, param_idx, CALLEE_Q_KEEPS, 1, &r) : -1;
    if (known == 1) return r;
    r = callee_keeps_string_arg_walk(gen, func_name, param_idx, known == 0 ? 0 : depth);
    if (known == 0) callee_memo_end(gen, fn, param_idx, CALLEE_Q_KEEPS, r);
    return r;
}

static int callee_keeps_string_arg_walk(CodeGenerator* gen, const char* func_name,
                                        int param_idx, int depth) {
    if (callee_string_param_captures_at(gen, func_name, param_idx, depth)) return 0;
    const char* fn = codegen_normalise_callee(func_name);
    ASTNode* fn_def = gen->program ? find_function_definition_by_name(gen->program, fn) : NULL;
    int copies = fn_def && function_def_returns_heap_string(gen, fn_def);
    return callee_string_param_kept_at(gen, func_name, param_idx, !copies, depth);
}

/* `return <param>` in `fn_name`: owned when the parameter is the function's
 * own reference (callee_string_param_captures). */
static int returned_param_is_captured(CodeGenerator* gen, const char* name, const char* fn_name) {
    if (!gen || !gen->program || !name || !fn_name) return 0;
    ASTNode* fn_def = find_function_definition_by_name(gen->program, fn_name);
    for (int i = 0; fn_def && i < fn_def->child_count; i++) {
        ASTNode* c = fn_def->children[i];
        if (c && (c->type == AST_PATTERN_VARIABLE || c->type == AST_VARIABLE_DECLARATION) &&
            c->value && strcmp(c->value, name) == 0) {
            return callee_string_param_captures_at(gen, fn_name, i, 0);
        }
    }
    return 0;
}

/* Does the callee's param STORE-escape (anything except being directly
 * returned)? Used by the call-site identity-drain to distinguish a param
 * that is merely return-passed-through (identity-drainable) from one a
 * container/@retain/struct-field/closure owns (must NOT be freed). */
int callee_param_store_escapes_via_body(CodeGenerator* gen, const char* func_name,
                                        int param_idx) {
    const char* pname; ASTNode* body;
    int saved_cl = g_escape_param_is_closure;   /* #2528: per walk */
    if (!resolve_callee_param_body(gen, func_name, param_idx, &pname, &body)) {
        g_escape_param_is_closure = saved_cl;
        return 1;  /* unresolved → conservatively assume it stores */
    }
    int r = param_escapes_in_subtree(gen, body, pname, 0, /*return_is_escape=*/0);
    g_escape_param_is_closure = saved_cl;
    return r;
}

int closure_param_escapes_via_body(CodeGenerator* gen, ASTNode* closure, int param_idx,
                                   int return_is_escape) {
    if (!gen || !closure || closure->type != AST_CLOSURE || param_idx < 0) return 1;
    const char* pname = NULL;
    ASTNode* body = NULL;
    ASTNode* pnode = NULL;
    int seen = 0;
    for (int i = 0; i < closure->child_count; i++) {
        ASTNode* c = closure->children[i];
        if (!c) continue;
        if (c->type == AST_CLOSURE_PARAM && seen++ == param_idx) { pname = c->value; pnode = c; }
        if (c->type == AST_BLOCK) body = c;
    }
    if (!pname || !body) return 1;
    ASTNode* saved_body = g_keep_body;
    ASTNode* saved_closure = g_keep_closure;
    int saved_cl = g_escape_param_is_closure;   /* #2528: per walk */
    g_keep_body = body;
    g_keep_closure = closure;
    g_escape_param_is_closure = pnode->node_type && pnode->node_type->kind == TYPE_FUNCTION &&
                                !pnode->node_type->is_fnptr;
    int r = param_escapes_in_subtree(gen, body, pname, 0, return_is_escape);
    g_keep_body = saved_body;
    g_keep_closure = saved_closure;
    g_escape_param_is_closure = saved_cl;
    return r;
}

/* #2499: can a value of type `t` be a string the caller owns? A closure
 * call through an erased `fn` checks no argument types, so a string can
 * reach a `ptr` (or untyped-pointer) parameter as well as a `string` one. */
static int param_may_hold_caller_string(const Type* t) {
    if (!t) return 0;   /* an untyped closure parameter is an int */
    return t->kind == TYPE_STRING || t->kind == TYPE_PTR ||
           t->kind == TYPE_UNKNOWN || t->kind == TYPE_WILDCARD ||
           t->kind == TYPE_OPTIONAL;
}

static ASTNode* last_block_child_of(ASTNode* node) {
    for (int i = node ? node->child_count - 1 : -1; i >= 0; i--) {
        if (node->children[i] && node->children[i]->type == AST_BLOCK) return node->children[i];
    }
    return NULL;
}

/* The first value-carrying return under `node`, not looking into a nested
 * (non-trailing) closure. */
static ASTNode* closure_first_value_return(ASTNode* node) {
    if (!node) return NULL;
    if (node->type == AST_CLOSURE && !(node->value && strcmp(node->value, "trailing") == 0)) {
        return NULL;
    }
    if (node->type == AST_RETURN_STATEMENT && node->child_count > 0 && node->children[0] &&
        node->children[0]->type != AST_PRINT_STATEMENT) return node;
    for (int i = 0; i < node->child_count; i++) {
        ASTNode* r = closure_first_value_return(node->children[i]);
        if (r) return r;
    }
    return NULL;
}

/* #2499: does every value return of a closure body hand back a copy of a
 * string it returns? A string closure (one return is a string) wraps each
 * single-value return in aether_uniform_heap_str, which copies a string the
 * closure does not own; a tuple closure (#2501) wraps each string slot the
 * same way, a slot being a string slot when the first return's value there
 * is a string. `first` is that first return. */
static int closure_returns_copy_walk(ASTNode* node, ASTNode* first, int* saw_string) {
    if (!node) return 1;
    if (node->type == AST_CLOSURE && !(node->value && strcmp(node->value, "trailing") == 0)) {
        return 1;   /* a nested closure's returns are its own */
    }
    if (node->type == AST_RETURN_STATEMENT && node->child_count > 0) {
        if ((node->child_count > 1) != (first->child_count > 1)) return 0;
        if (node->child_count > 1) {
            for (int j = 0; j < node->child_count; j++) {
                ASTNode* c = node->children[j];
                if (!(c && c->node_type && c->node_type->kind == TYPE_STRING)) continue;
                ASTNode* slot = j < first->child_count ? first->children[j] : NULL;
                if (!(slot && slot->node_type && slot->node_type->kind == TYPE_STRING)) return 0;
            }
            *saw_string = 1;
        } else if (node->children[0] && node->children[0]->node_type &&
                   node->children[0]->node_type->kind == TYPE_STRING) {
            *saw_string = 1;
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        if (!closure_returns_copy_walk(node->children[i], first, saw_string)) return 0;
    }
    return 1;
}

static int closure_returns_copy(ASTNode* body, int* saw_string) {
    ASTNode* first = closure_first_value_return(body);
    if (!first) return 1;   /* returns nothing */
    return closure_returns_copy_walk(body, first, saw_string);
}

/* #2499: a closure-typed (`fn`, not a raw C function pointer) value. */
static int is_closure_type(const Type* t) {
    return t && t->kind == TYPE_FUNCTION && !t->is_fnptr;
}

/* Does `t` hold a closure in a struct (or through a pointer to one)? */
static int type_reaches_closure_field(CodeGenerator* gen, const Type* t) {
    while (t && (t->kind == TYPE_PTR || t->kind == TYPE_ARRAY) && t->element_type) t = t->element_type;
    if (!t || t->kind != TYPE_STRUCT || !t->struct_name) return 0;
    ASTNode* sdef = find_struct_definition_by_name(gen->program, t->struct_name);
    for (int i = 0; sdef && i < sdef->child_count; i++) {
        ASTNode* f = sdef->children[i];
        if (f && f->type == AST_STRUCT_FIELD && is_closure_type(f->node_type)) return 1;
    }
    return 0;
}

/* Is any AST node under `n` a way for a closure made outside this program
 * into it: `unbox_closure(p)` (the ptr may be anyone's box), or a view of a
 * raw pointer as a struct that has a closure field (`p as *Rec`)? */
static int subtree_imports_closure(CodeGenerator* gen, ASTNode* n) {
    if (!n) return 0;
    if (n->type == AST_FUNCTION_CALL && n->value && strcmp(n->value, "unbox_closure") == 0) return 1;
    /* A `ptr` argument at an `fn` parameter is unboxed at the call
     * (generate_expression's argument loop), the same recovery. The
     * argument may sit one position later in the C call when a builder
     * context is injected first, so both positions count. */
    if (n->type == AST_FUNCTION_CALL && n->value && strcmp(n->value, "call") != 0) {
        ASTNode* fdef = find_function_definition_by_name(gen->program, n->value);
        int shift = fdef && fdef->child_count > 0 && fdef->children[0] &&
                    fdef->children[0]->value && strcmp(fdef->children[0]->value, "_ctx") == 0;
        for (int i = 0; i < n->child_count; i++) {
            ASTNode* a = n->children[i];
            if (!a || !a->node_type || a->node_type->kind != TYPE_PTR) continue;
            if (lookup_callee_param_kind(gen, n->value, i) == TYPE_FUNCTION ||
                (shift && lookup_callee_param_kind(gen, n->value, i + 1) == TYPE_FUNCTION)) return 1;
        }
    }
    if (n->type == AST_PTR_AS_STRUCT_CAST && type_reaches_closure_field(gen, n->node_type)) return 1;
    for (int i = 0; i < n->child_count; i++) {
        if (subtree_imports_closure(gen, n->children[i])) return 1;
    }
    return 0;
}

/* #2499: can the program call a closure whose body it does not have? Each
 * way one gets in:
 *   - a library build: its exported functions are called from outside,
 *     with whatever closures the host passes;
 *   - an extern that returns a closure, or that returns or takes a struct
 *     with a closure field (C fills the field);
 *   - a C-laid-out (`extern`) struct with a closure field;
 *   - a @c_callback function with a closure parameter: C calls it with a
 *     closure of its own making;
 *   - a `ptr` converted to a closure, by `unbox_closure` or implicitly at a
 *     `fn` slot (typecheck_ptr_to_closure_seen): the box may be one a host
 *     bridge or another library made;
 *   - a raw pointer viewed as a struct with a closure field. */
static int closure_from_outside(CodeGenerator* gen) {
    if (gen->emit_lib || typecheck_ptr_to_closure_seen()) return 1;
    ASTNode* prog = gen->program;
    for (int i = 0; i < prog->child_count; i++) {
        ASTNode* top = prog->children[i];
        if (top && top->type == AST_EXPORT_STATEMENT && top->child_count > 0) top = top->children[0];
        if (!top) continue;
        if (top->type == AST_EXTERN_FUNCTION) {
            if (is_closure_type(top->node_type) || type_reaches_closure_field(gen, top->node_type)) return 1;
            for (int k = 0; k < top->child_count; k++) {
                ASTNode* p = top->children[k];
                if (p && type_reaches_closure_field(gen, p->node_type)) return 1;
            }
        }
        if (top->type == AST_STRUCT_DEFINITION && top->annotation &&
            strncmp(top->annotation, "extern", 6) == 0) {
            for (int k = 0; k < top->child_count; k++) {
                ASTNode* f = top->children[k];
                if (f && f->type == AST_STRUCT_FIELD && is_closure_type(f->node_type)) return 1;
            }
        }
        if ((top->type == AST_FUNCTION_DEFINITION || top->type == AST_BUILDER_FUNCTION) &&
            is_c_callback(top)) {
            for (int k = 0; k < top->child_count; k++) {
                ASTNode* p = top->children[k];
                if (p && (p->type == AST_PATTERN_VARIABLE || p->type == AST_VARIABLE_DECLARATION) &&
                    is_closure_type(p->node_type)) return 1;
            }
        }
    }
    return subtree_imports_closure(gen, prog);
}

/* #2499: may a caller free the owned string it passes to a closure call
 * once the call returns, whatever closure it calls? A named call decides
 * that from the callee's body (callee_param_escapes_via_body); a call
 * through an `fn` parameter has no body to read, so the answer has to hold
 * for every closure the program can call: every closure literal, and every
 * function used as a closure value (the bare-fn adapters). It does when
 * none of them keeps a parameter that can hold such a string as it is:
 *   - a `string` parameter is never kept as it is: a closure that keeps one
 *     takes its own reference on entry (closure_string_param_kept), and
 *     the adapter of a function value that keeps one passes it a reference
 *     of its own;
 *   - a `ptr` (or unknown) parameter is kept by a store (a list or map, a
 *     struct field, another variable or cell), a nested call that keeps it,
 *     a capture (a ptr is captured as it is) or a return, and nothing can
 *     copy what a pointer points at, so one such closure anywhere turns the
 *     convention off.
 * The walk assumes the answer it is checking for calls between closures
 * (an argument passed on to `call(g, ...)` is borrowed), which is sound by
 * induction: if no closure keeps one, none keeps one by passing it on.
 * A closure whose body this walk did not see can keep anything, so the
 * answer is no whenever one can be called (closure_from_outside). */
void compute_closure_args_borrowed(CodeGenerator* gen) {
    if (!gen) return;
    gen->closure_args_borrowed = 0;
    if (!gen->program || closure_from_outside(gen)) return;
    gen->closure_args_borrowed = 1;   /* the hypothesis the walks use */
    int ok = 1;
    for (int ci = 0; ci < gen->closure_count && ok; ci++) {
        ASTNode* lit = gen->closures[ci].closure_node;
        int pi = 0;
        for (int k = 0; lit && k < lit->child_count && ok; k++) {
            ASTNode* p = lit->children[k];
            if (!p || p->type != AST_CLOSURE_PARAM) continue;
            /* A `string` one the closure keeps is its own copy
             * (closure_string_param_kept); a pointer cannot be copied. */
            if (param_may_hold_caller_string(p->node_type) &&
                p->node_type->kind != TYPE_STRING &&
                closure_param_escapes_via_body(gen, lit, pi, 1)) ok = 0;
            pi++;
        }
    }
    for (int a = 0; a < gen->bare_fn_adapter_count && ok; a++) {
        const char* name = gen->bare_fn_adapter_names[a];
        ASTNode* fdef = find_function_definition_by_name(gen->program, name);
        if (!fdef) { ok = 0; break; }
        int pi = 0;
        for (int k = 0; k < fdef->child_count && ok; k++) {
            ASTNode* p = fdef->children[k];
            if (!p || (p->type != AST_PATTERN_VARIABLE && p->type != AST_VARIABLE_DECLARATION)) continue;
            /* The adapter hands a kept `string` its own reference
             * (emit_bare_fn_adapters). */
            if (param_may_hold_caller_string(p->node_type) &&
                p->node_type->kind != TYPE_STRING &&
                callee_param_escapes_via_body(gen, name, pi, 0)) ok = 0;
            pi++;
        }
    }
    gen->closure_args_borrowed = ok;
    callee_memo_reset(gen);
}

/* #2499 copy-on-keep: does the closure literal `closure` keep its `string`
 * parameter `param_idx` past the call, so it must take a reference of its
 * own when it is entered? Every way of keeping counts (a list, map or set
 * store, a struct field, a cell or outer variable it is assigned to, a
 * local that keeps it, a nested call that keeps it, a multi-value return)
 * except two that already take their own reference: a capture by a nested
 * closure, and the return of a string closure, which is a copy. A local it
 * is assigned to and that keeps nothing is no keep either. With the
 * reference taken, the caller's own one is its to free after the call,
 * whatever the closure does. */
int closure_string_param_kept(CodeGenerator* gen, ASTNode* closure, int param_idx) {
    int saw_string = 0;
    int returns_copy = closure_returns_copy(last_block_child_of(closure), &saw_string) &&
                       saw_string;
    g_capture_holds_own_ref = 1;
    int kept = closure_param_escapes_via_body(gen, closure, param_idx, !returns_copy);
    g_capture_holds_own_ref = 0;
    return kept;
}

/* Does the user function `func_name` declare a `-> string` return? Only
 * then is the call-site identity-drain meaningful (it compares the call
 * result pointer against the passed temp). */
int callee_returns_string(CodeGenerator* gen, const char* func_name) {
    if (!gen || !gen->program || !func_name) return 0;
    const char* fn = codegen_normalise_callee(func_name);
    ASTNode* fn_def = find_function_definition_by_name(gen->program, fn);
    if (!fn_def) return 0;
    return fn_def->node_type && fn_def->node_type->kind == TYPE_STRING;
}

/* True when `func_name` resolves to a user function with a visible body
 * block in the merged program AST. Only then is the body-walk
 * (callee_param_escapes_via_body) authoritative: a proven non-escape can
 * safely override the conservative call_arg_escapes heuristic. Externs
 * and unknown callees have no body and stay conservative. */
int callee_has_visible_body(CodeGenerator* gen, const char* func_name) {
    if (!gen || !gen->program || !func_name) return 0;
    const char* fn = codegen_normalise_callee(func_name);
    ASTNode* fn_def = find_function_definition_by_name(gen->program, fn);
    if (!fn_def) return 0;
    for (int i = fn_def->child_count - 1; i >= 0; i--) {
        if (fn_def->children[i] && fn_def->children[i]->type == AST_BLOCK) {
            return 1;
        }
    }
    return 0;
}

/* Does `pname` appear as an identifier anywhere in this subtree? Used to
 * detect storage sinks (assignment RHS, aggregate elements, closure
 * captures) that retain the parameter's pointer past the call. Erring
 * toward "mentioned" is sound: a false positive only withholds a drain
 * (a leak), never frees a live pointer (a UAF). */
static int subtree_mentions_param(ASTNode* node, const char* pname) {
    if (!node) return 0;
    if (node->type == AST_IDENTIFIER && node->value &&
        strcmp(node->value, pname) == 0) return 1;
    for (int i = 0; i < node->child_count; i++) {
        if (subtree_mentions_param(node->children[i], pname)) return 1;
    }
    return 0;
}

/* Does evaluating `node` yield a value that IS, or directly aggregates,
 * the parameter's pointer? This is the "pointer-carrying" test for store
 * sinks: a bare reference (`pname`) or an array/struct literal element
 * (`[pname]`, `{f: pname}`) carries the pointer, so storing/returning it
 * retains the param. A nested CALL does NOT directly carry — `f(pname)`
 * yields f's result, a distinct value; whether THAT retains the param is
 * decided precisely by the call-rule in param_escapes_in_subtree (which
 * sees through read-only accessors). Keeping this test narrow is what
 * lets `ok = file_delete_raw(path)` NOT count as an escape while
 * `y = path` still does. */
static int value_directly_carries_param(ASTNode* node, const char* pname) {
    if (!node) return 0;
    if (node->type == AST_IDENTIFIER && node->value &&
        strcmp(node->value, pname) == 0) return 1;
    if (node->type == AST_ARRAY_LITERAL || node->type == AST_FIELD_INIT) {
        for (int i = 0; i < node->child_count; i++) {
            if (value_directly_carries_param(node->children[i], pname)) return 1;
        }
    }
    /* A struct literal's fields are AST_ASSIGNMENT nodes holding just the
     * value. `return Rec { name: s }` carries `s` as much as `[s]` does; it
     * was missed, so the caller freed an argument the returned struct still
     * pointed at (and #2499's convention relies on this walk). */
    if (node->type == AST_STRUCT_LITERAL) {
        for (int i = 0; i < node->child_count; i++) {
            ASTNode* f = node->children[i];
            if (!f) continue;
            if (f->type == AST_ASSIGNMENT || f->type == AST_FIELD_INIT) {
                for (int j = 0; j < f->child_count; j++) {
                    ASTNode* v = f->children[j];
                    /* #2528: a closure field retains the closure it is
                     * given (#2525): the parameter itself stays the
                     * caller's. */
                    if (g_escape_param_is_closure && v && v->type == AST_IDENTIFIER &&
                        v->value && strcmp(v->value, pname) == 0) continue;
                    if (value_directly_carries_param(v, pname)) return 1;
                }
            } else if (value_directly_carries_param(f, pname)) {
                return 1;
            }
        }
    }
    return 0;
}

/* #2528: is `node` a store that retains a closure parameter for its holder
 * (so the parameter does not escape through it): `o.f = p` on a struct
 * field, a message field init, a write to a global or an actor's state? */
static int closure_param_store_retains(CodeGenerator* gen, ASTNode* node, const char* pname) {
    if (!g_escape_param_is_closure || !node) return 0;
    ASTNode* lhs = NULL; ASTNode* rhs = NULL;
    if ((node->type == AST_ASSIGNMENT ||
         (node->type == AST_BINARY_EXPRESSION && node->value && strcmp(node->value, "=") == 0)) &&
        node->child_count >= 2) {
        lhs = node->children[0]; rhs = node->children[1];
        if (!lhs || lhs->type != AST_MEMBER_ACCESS) return 0;
    } else if (node->type == AST_VARIABLE_DECLARATION && node->value && node->child_count > 0) {
        if (!is_module_global_var(gen, node->value) && !is_actor_state_var(gen, node->value)) return 0;
        rhs = node->children[0];
    } else if (node->type == AST_FIELD_INIT && node->child_count > 0) {
        rhs = node->children[0];
    } else {
        return 0;
    }
    return rhs && rhs->type == AST_IDENTIFIER && rhs->value && strcmp(rhs->value, pname) == 0;
}

/* #2499: does `call` keep its `string` argument `i` only by handing it
 * back? Its callee returns the parameter as it is (no copy: the string
 * result is not uniform-heap) and stores it nowhere, so the call's value is
 * the argument itself, and the argument goes wherever that value goes. */
/* Is the value of `call` known to be something other than a string: its own
 * type, or its callee's result, is (a distinct type over string is a string
 * here)? An unknown type is no answer. */
static int call_result_known_not_string(CodeGenerator* gen, ASTNode* call) {
    Type* t = call->node_type;
    if (t && t->kind != TYPE_STRING && t->kind != TYPE_UNKNOWN) return 1;
    const char* fn = codegen_normalise_callee(call->value);
    ASTNode* def = gen->program ? find_function_definition_by_name(gen->program, fn) : NULL;
    Type* rt = def ? def->node_type : NULL;
    return rt && rt->kind != TYPE_STRING && rt->kind != TYPE_UNKNOWN;
}

static int handback_param(CodeGenerator* gen, ASTNode* call, int i, int depth) {
    if (!call || call->type != AST_FUNCTION_CALL || !call->value ||
        strcmp(call->value, "call") == 0) return 0;
    /* Only a call whose value is a string can be its string argument. A
     * callee that keeps the parameter in what it returns (a struct holding
     * it, #2584) keeps it like any store. Told apart by the two questions
     * below alone, such a call was taken for a hand-back, and the local it
     * was bound to was typed as the string, so the C did not compile. */
    if (call_result_known_not_string(gen, call)) return 0;
    if (!callee_has_visible_body(gen, call->value) ||
        !callee_param_is_string(gen, call->value, i)) return 0;
    /* Asked again while it is being computed (a callee reaching itself), the
     * answer is no: the call is then judged as a keep of its own, as it was
     * before hand-backs were told apart. */
    ASTNode* fn = callee_memo_fn(gen, call->value);
    int r;
    int known = fn ? callee_memo_begin(gen, fn, i, CALLEE_Q_HANDBACK, 0, &r) : -1;
    if (known == 1) return r;
    int d = known == 0 ? 0 : depth;
    r = callee_keeps_string_arg(gen, call->value, i, d) &&
        !callee_string_param_kept_at(gen, call->value, i, 0, d);
    if (known == 0) callee_memo_end(gen, fn, i, CALLEE_Q_HANDBACK, r);
    return r;
}

/* The variable whose pointer `expr` evaluates to through calls that only
 * hand it back (handback_param): `s` for `temp_prefix(s)` or
 * `pick(trim_none(s))`, NULL for anything else. A call that names the
 * variable in another argument as well, or hands back two, is no chain.
 * Returns the variable's identifier node. */
static ASTNode* handback_leaf_node(CodeGenerator* gen, ASTNode* expr, int depth) {
    if (!expr || depth > 8) return NULL;
    if (expr->type == AST_IDENTIFIER) return expr->value ? expr : NULL;
    if (expr->type != AST_FUNCTION_CALL) return NULL;
    ASTNode* leaf = NULL;
    int at = -1;
    for (int i = 0; i < expr->child_count; i++) {
        ASTNode* l = handback_leaf_node(gen, expr->children[i], depth + 1);
        if (!l || !handback_param(gen, expr, i, depth + 1)) continue;
        if (leaf) return NULL;
        leaf = l;
        at = i;
    }
    for (int i = 0; leaf && i < expr->child_count; i++) {
        if (i != at && subtree_mentions_param(expr->children[i], leaf->value)) return NULL;
    }
    return leaf;
}

static const char* handback_leaf(CodeGenerator* gen, ASTNode* expr, int depth) {
    ASTNode* leaf = handback_leaf_node(gen, expr, depth);
    return leaf ? leaf->value : NULL;
}

static int handback_leaf_is(CodeGenerator* gen, ASTNode* expr, const char* name, int depth) {
    const char* leaf = expr && expr->type == AST_FUNCTION_CALL ? handback_leaf(gen, expr, depth) : NULL;
    return leaf && strcmp(leaf, name) == 0;
}

/* #2548: for the take (emit_string_take), the heap-tracked local a call
 * only hands back, or NULL. A closure's shared cell or env slot frees its
 * string itself, so its value is never moved out. */
static ASTNode* handback_take_leaf(CodeGenerator* gen, ASTNode* e) {
    if (!e || e->type != AST_FUNCTION_CALL) return NULL;
    ASTNode* leaf = handback_leaf_node(gen, e, 0);
    if (!leaf || !is_heap_string_var(gen, leaf->value) ||
        is_promoted_capture(gen, leaf->value) || is_env_capture_name(gen, leaf->value)) return NULL;
    return leaf;
}

/* In a keep walk: is passing the parameter as argument `i` of the named
 * call `node` a keep? */
static int call_position_keeps_param(CodeGenerator* gen, ASTNode* node, int i, int depth) {
    const char* fn = codegen_normalise_callee(node->value);
    /* Read-only accessor (byte/length view, print, free): provably does not
     * retain the pointer, so the param does not escape via THIS call. If the
     * accessor's RETURN value (a view into the param) is later stored, the
     * assignment / aggregate / return sinks catch that mention separately,
     * so this is sound. */
    if (is_nonstoring_builtin(fn)) return 0;
    if (is_consuming_free(gen, fn)) return 1;  /* this function frees it; the caller must not */
    /* #2523: the extern's declaration says the argument is used only during
     * the call, neither stored nor freed, so a wrapper that forwards its
     * parameter there (`fs.walk` into `fs_walk_raw`) keeps nothing either. */
    if (is_noescape_extern_param(gen, fn, i)) return 0;
    if (is_retain_extern_param(gen, fn, i)) return 1;
    if (callee_has_visible_body(gen, node->value)) {
        /* As call_arg_position_escapes decides it: a visible body is
         * authoritative, so a parameter passed on to a function that only
         * calls it (`it(cb) { it_impl(cb) }`) is not kept. Deciding by the
         * parameter's kind first made every `fn` or `ptr` parameter passed
         * on a keep, and the env of a callback handed to such a wrapper was
         * never drained. A `string` argument is kept only as
         * callee_keeps_string_arg says. */
        if (callee_param_is_string(gen, node->value, i))
            return callee_keeps_string_arg(gen, node->value, i, depth + 1);
        return callee_param_escapes_via_body(gen, node->value, i, depth + 1);
    }
    if (call_arg_escapes(lookup_callee_param_kind(gen, node->value, i))) return 1;
    return callee_param_escapes_via_body(gen, node->value, i, depth + 1);
}

static int param_escapes_in_subtree(CodeGenerator* gen, ASTNode* node,
                                    const char* pname, int depth,
                                    int return_is_escape) {
    if (!node) return 0;
    /* Storage sinks: the parameter's pointer is retained beyond the call
     * only when it flows, AS A POINTER, into a binding, a container
     * element, a struct field, a return, or a closure capture. Each is an
     * escape; missing one would let the arg-drain free a still-referenced
     * buffer (UAF), so the body-walk catches them all before it can be
     * trusted as authoritative over the conservative call_arg_escapes
     * heuristic. The carry-test is deliberately narrow (direct ref /
     * aggregate element) so that merely READING the param via a nested
     * call — `f(pname)` whose result is what's stored — is left to the
     * precise call-rule below rather than blanket-marked as an escape. */
    if (return_is_escape && node->type == AST_RETURN_STATEMENT) {
        for (int i = 0; i < node->child_count; i++) {
            if (value_directly_carries_param(node->children[i], pname)) return 1;
        }
    }
    /* #2499: in a copy-on-keep query a return is no keep (the caller is
     * handed the reference), and neither is returning a call that only
     * hands the parameter back (`greet(n) { return shout(n) }`): its value
     * is the parameter, so it is that same return. Counted as a keep, greet
     * took a copy and returned it to a caller handed a borrowed result. */
    if (!return_is_escape && node->type == AST_RETURN_STATEMENT) {
        for (int i = 0; i < node->child_count; i++) {
            ASTNode* c = node->children[i];
            if (handback_leaf_is(gen, c, pname, depth + 1)) continue;
            /* The parameter inside what is returned (a field of a struct
             * literal, an element of an array one) is kept, as in any
             * struct field: the value returned holds the function's own
             * reference, and the caller gets that value, not the parameter.
             * Only a bare `return pname` hands the reference itself over.
             * Missed, `cursor(t) -> Cursor { return Cursor { text: t } }`
             * took no reference of its own, while its caller, seeing the
             * argument kept, no longer freed it: nobody did (#2584). */
            if (c && c->type != AST_IDENTIFIER && value_directly_carries_param(c, pname)) return 1;
            if (param_escapes_in_subtree(gen, c, pname, depth, return_is_escape)) return 1;
        }
        return 0;
    }
    if (closure_param_store_retains(gen, node, pname)) return 0;   /* #2528 */
    if (node->type == AST_ASSIGNMENT && node->child_count >= 2) {
        /* `x = pname`, `obj.field = pname`, `arr[i] = pname` escape the
         * pointer into ANOTHER location. Reassigning the param's own slot
         * (`pname = ...`, including the no-op `pname = pname`) does NOT —
         * the value is overwritten in place, never aliased elsewhere — so
         * skip when the LHS is the param itself. */
        ASTNode* lhs = node->children[0];
        int lhs_is_self = lhs && lhs->type == AST_IDENTIFIER && lhs->value &&
                          strcmp(lhs->value, pname) == 0;
        if (!lhs_is_self && value_directly_carries_param(node->children[1], pname)) return 1;
    }
    if (node->type == AST_BINARY_EXPRESSION && node->value &&
        strcmp(node->value, "=") == 0 && node->child_count >= 2) {
        ASTNode* lhs = node->children[0];
        int lhs_is_self = lhs && lhs->type == AST_IDENTIFIER && lhs->value &&
                          strcmp(lhs->value, pname) == 0;
        if (!lhs_is_self && value_directly_carries_param(node->children[1], pname)) return 1;
    }
    if (node->type == AST_VARIABLE_DECLARATION) {
        /* `y = pname` aliases the pointer into a DIFFERENT local y → escape.
         * But the parser also models a bare param REASSIGNMENT as a decl
         * node whose `value` IS the param name (e.g. the no-op-free shim's
         * `p = p`, or `p = concat(p, x)`): that overwrites the param's own
         * slot, never aliasing the pointer elsewhere, so it is not an
         * escape. Skip when the declared name is the param itself — the
         * same lhs-is-self exclusion the AST_ASSIGNMENT sink applies. */
        int decl_is_self = node->value && strcmp(node->value, pname) == 0;
        if (!decl_is_self) {
            int followed = -1;
            for (int i = 0; i < node->child_count; i++) {
                /* A call that only hands the parameter back
                 * (`t = temp_prefix(p)`) aliases it as the bare name does. */
                int handback = 0;
                if (!value_directly_carries_param(node->children[i], pname)) {
                    handback = g_capture_holds_own_ref &&
                               handback_leaf_is(gen, node->children[i], pname, depth + 1);
                    if (!handback) continue;
                }
                /* A copy-on-keep query follows an alias into a local: the
                 * reference outlives the call only if the local keeps it
                 * (#2499). Every use of the local's name in the body counts,
                 * whatever value it holds by then, which errs toward a keep. */
                if (g_capture_holds_own_ref && node->value && keep_alias_is_local(gen, node->value)) {
                    if (depth >= 8 ||
                        param_escapes_in_subtree(gen, g_keep_body, node->value, depth + 1,
                                                 return_is_escape)) return 1;
                    if (handback) followed = i;
                    continue;
                }
                return 1;
            }
            /* The hand-back names the parameter only along its chain: walked
             * again, it would count as a keep of its own. */
            if (followed >= 0) {
                for (int i = 0; i < node->child_count; i++) {
                    if (i != followed &&
                        param_escapes_in_subtree(gen, node->children[i], pname, depth,
                                                 return_is_escape)) return 1;
                }
                return 0;
            }
        }
    }
    if (node->type == AST_CLOSURE && subtree_mentions_param(node, pname)) {
        /* #2499: a string captured by a nested closure is held by the
         * closure's env through a reference of its own (aether_str_capture;
         * a promoted cell takes one too, emit_promoted_param_cell), so the
         * capture keeps nothing the caller frees. Only the closure-argument
         * convention check asks for that answer, and only for a string. A
         * trailing block is inlined, not captured: it is walked like the
         * rest of the body. */
        int trailing = node->value && strcmp(node->value, "trailing") == 0;
        if (!g_capture_holds_own_ref) return 1;  /* closure capture may outlive the call */
        if (!trailing) return 0;
    }
    /* #2499: an argument to a closure call is borrowed when every closure
     * in the program is known not to keep one (compute_closure_args_
     * borrowed), so passing the parameter on keeps nothing. */
    if (node->type == AST_FUNCTION_CALL && node->value &&
        strcmp(node->value, "call") == 0 && gen->closure_args_borrowed) {
        for (int i = 0; i < node->child_count; i++) {
            if (param_escapes_in_subtree(gen, node->children[i], pname, depth,
                                         return_is_escape)) return 1;
        }
        return 0;
    }
    if (node->type == AST_FUNCTION_CALL && node->value) {
        /* An INDIRECT call `cb(...)` lowers to value=="call" with child[0]
         * the CALLEE (the closure/fn being invoked) and children[1..] the
         * arguments. Invoking a parameter reads cb.fn/cb.env and runs it —
         * it neither stores nor returns cb, so the callee slot is NOT an
         * escape. Only the actual arguments can escape. For a NAMED call
         * (value == function name) every child is an argument. */
        int first_arg = (strcmp(node->value, "call") == 0) ? 1 : 0;
        /* #2518: a closure stored into a list or a map is retained by it,
         * which keeps nothing of the caller's. */
        ASTNode* list_kept = closure_container_store_value(gen, node);
        for (int i = first_arg; i < node->child_count; i++) {
            ASTNode* a = node->children[i];
            if (!a) continue;
            if (a != list_kept && a->type == AST_IDENTIFIER && a->value &&
                strcmp(a->value, pname) == 0) {
                if (call_position_keeps_param(gen, node, i, depth)) return 1;
                continue;
            }
            /* #2499: a call that only hands the parameter back passes it on
             * to this position (`fs_make_temp_file_raw(d, temp_prefix(p))`),
             * so the position decides, not the hand-back: counted as a
             * keep, p was copied and the copy, marked escaped by the same
             * call, was never freed. */
            if (a != list_kept && first_arg == 0 && handback_leaf_is(gen, a, pname, depth + 1)) {
                if (call_position_keeps_param(gen, node, i, depth)) return 1;
                continue;
            }
            if (param_escapes_in_subtree(gen, a, pname, depth, return_is_escape)) return 1;
        }
        for (int i = 0; i < first_arg && i < node->child_count; i++) {
            if (param_escapes_in_subtree(gen, node->children[i], pname, depth, return_is_escape)) return 1;
        }
        return 0;
    }
    for (int i = 0; i < node->child_count; i++) {
        if (param_escapes_in_subtree(gen, node->children[i], pname, depth, return_is_escape)) return 1;
    }
    return 0;
}

/* Built-in callees that take a string argument but provably never
 * RETAIN the pointer beyond the call, so the argument does not escape:
 *   - the print family (print / println / print_char) writes the bytes
 *     to stdout/stderr and returns;
 *   - the frees are NOT in this list; see is_consuming_free below. They do
 *     not store the pointer, but they do end its life, and the two questions
 *     this predicate answers need opposite answers for them.
 * Their parameter has no registered type, so lookup_callee_param_kind
 * returns TYPE_UNKNOWN and the conservative call_arg_escapes() would
 * mark the argument escaped. That false escape suppresses the
 * reassignment-free wrapper and the function-exit defer-free, leaking
 * every prior value of a variable printed in a loop (string_leak_loop's
 * `println(result)`) and — for `release` — withholding the `_heap_X`
 * tracker the release lowering needs to actually free the value. This
 * list is sound in the only direction that matters: we assert
 * non-retention, so we never withhold a free a recipient still depends
 * on (a UAF) — we only restore a free the heuristic wrongly withheld.
 * The release lowering (codegen_expr.c) is itself flag-guarded, so the
 * restored defer-free and the explicit release never double-free. */
/* The frees. They neither store nor return the pointer, but they DO end its
 * life, and that means the two questions this file asks need opposite answers.
 *
 * At the caller's own call site (`string.free(local)`) the argument must stay
 * tracked: the lowering needs `_heap_local` to know which physical shape it is
 * freeing, and it clears the flag afterwards so scope exit cannot free it
 * twice. Treating the call as an escape there withholds the tracker and the
 * value leaks, which is what `string.free` on an interpolated string did.
 *
 * Inside a callee's body, walking whether a PARAMETER escapes, the same call
 * means the opposite: this function consumes what it was handed, so the caller
 * must not also free it. `name_free(s: string) { string.free(s) }` is the
 * shape, and answering "does not escape" there makes every caller of such a
 * helper double-free. */
const char* consuming_free_symbol(CodeGenerator* gen, const char* fn) {
    if (!fn) return NULL;
    if (strcmp(fn, "release") == 0) return "release";
    if (strcmp(fn, "string_release") == 0) return "string_release";
    if (strcmp(fn, "string_free") == 0) return "string_free";
    /* `@extern("string_free") _release_x(s: string)` (std.jsonpath) is the
     * same free under another name; seen by its name only, the free went
     * unrecognised, and the local it freed was freed again at exit. */
    if (!gen || !gen->program) return NULL;
    ProgramIndex* ix = program_index(gen->program);
    ASTNode* ext = ix ? (ASTNode*)strmap_get(&ix->externs, fn) : NULL;
    const char* sym = ext ? extern_c_symbol(ext) : NULL;
    if (sym && strcmp(sym, "string_free") == 0) return "string_free";
    if (sym && strcmp(sym, "string_release") == 0) return "string_release";
    return NULL;
}

static int is_consuming_free(CodeGenerator* gen, const char* fn) {
    return consuming_free_symbol(gen, fn) != NULL;
}

static int is_nonstoring_builtin(const char* fn) {
    if (!fn) return 0;
    return strcmp(fn, "print") == 0 ||
           strcmp(fn, "println") == 0 ||
           strcmp(fn, "print_char") == 0 ||
           /* Pure read-only views into a string's bytes/length. These
            * return a non-owning view (or a scalar) and never stash the
            * argument pointer, so passing a heap string to one is not an
            * escape. Stdlib wrappers funnel params through these before
            * handing the raw bytes to a synchronous C extern
            * (`file_delete_raw(aether_string_data(path))`), which had
            * falsely marked `path` escaped and leaked every interpolated
            * argument. If the VIEW is stored, the assignment/return sinks
            * in param_escapes_in_subtree catch it independently. */
           strcmp(fn, "aether_string_data") == 0 ||
           strcmp(fn, "aether_string_len") == 0 ||
           strcmp(fn, "aether_string_length") == 0 ||
           strcmp(fn, "_aether_safe_str") == 0 ||
           /* std.string.bytes: a BORROWED byte[] view of the string's own
            * storage (#2301). It does not retain the argument, and a slice
            * does not extend its owner's lifetime (the documented slice
            * contract, and bytes()'s own), so a NAMED heap string passed to
            * it keeps its scope-exit free. Without this, routing a scope-
            * owned string through `string.bytes(s)` to a write withheld the
            * free and leaked it on every call.
            *
            * Deliberately NOT listed: aether_string_raw_ptr, the accessor in
            * bytes()'s body. The call-argument drain decides whether a
            * TEMPORARY can be freed straight after the call by walking the
            * callee's body; with the accessor listed, that walk stops seeing
            * `s` flow into the returned view, and `string.bytes(f())` frees
            * f()'s result while the view still points into it. Unlisted,
            * a temporary is left alone (it leaks, which is safe). */
           strcmp(fn, "string_bytes") == 0 ||
           /* string_seq_join walks the spine read-only and copies the
            * bytes into a fresh buffer; it retains neither the seq nor
            * the separator. Without this, a `s = seq_cons(x, s)`
            * accumulator that is later joined has its whole loop
            * marked escaped, and every intermediate spine ref leaks. */
           strcmp(fn, "string_seq_join") == 0 ||
           strcmp(fn, "string_join") == 0 ||
           /* std.bytes readers that yield a scalar. They cannot retain the
            * pointer they are given, and since #1380 they reject anything that
            * is not an AetherBytes outright. Without these, a heap string
            * passed to bytes.length/get had its scope-exit free suppressed and
            * leaked unless the caller added an explicit release(). */
           strcmp(fn, "aether_bytes_length") == 0 ||
           strcmp(fn, "aether_bytes_capacity") == 0 ||
           strcmp(fn, "aether_bytes_get") == 0 ||
           strcmp(fn, "aether_bytes_get_le16") == 0 ||
           strcmp(fn, "aether_bytes_get_le32") == 0 ||
           strcmp(fn, "aether_bytes_get_le64") == 0 ||
           strcmp(fn, "aether_bytes_get_be16") == 0 ||
           strcmp(fn, "aether_bytes_get_be32") == 0 ||
           strcmp(fn, "aether_bytes_get_be64") == 0 ||
           /* Copies the source bytes into the buffer; retains neither. */
           strcmp(fn, "aether_bytes_copy_from_string") == 0;
}

/* Does argument position `arg_idx` of `call` escape — i.e. might the
 * callee store the pointer beyond the call? Shared by the heap-string
 * and `string?` escape walks so both apply identical precision (a
 * read-only `string`/`string?` param does NOT escape, which is what
 * keeps `string.length(s)` and `sink(opt)` from leaking; a `ptr`/
 * unknown/`@retain` param does). Encapsulates the type-kind check, the
 * non-storing-builtin allowlist, the visible-body interprocedural walk,
 * and the extern-retain annotation. */
static int call_arg_position_escapes(CodeGenerator* gen, ASTNode* call,
                                     int arg_idx) {
    if (!call) return 1;
    const char* fn = call->value
        ? codegen_normalise_callee(call->value)
        : NULL;
    if (fn && (is_nonstoring_builtin(fn) || is_consuming_free(gen, fn))) return 0;
    /* #2499: under the closure-argument convention a closure call borrows
     * its arguments (the callee slot, 0, is invoked, not stored). */
    if (fn && strcmp(fn, "call") == 0 && gen->closure_args_borrowed) return 0;
    if (fn && is_retain_extern_param(gen, fn, arg_idx)) return 1;
    if (callee_has_visible_body(gen, call->value)) {
        /* Visible body → the body-walk is authoritative (sees through
         * read-only accessors, ignores self-assignment `p = p`). A `string`
         * argument escapes only as callee_keeps_string_arg says: a callee
         * that keeps it holds a reference of its own, and one whose string
         * result is uniform-heap hands back a copy of a parameter it
         * returns (`return p` goes through aether_uniform_heap_str with
         * the parameter's tracker 0), so only a borrowing callee that
         * returns the parameter as it is keeps the caller's pointer alive.
         * Counting every return made the caller keep a local passed to
         * `_query_key_escape(key)` for ever (the leak in url.parse_query). */
        if (callee_param_is_string(gen, call->value, arg_idx)) {
            return callee_keeps_string_arg(gen, call->value, arg_idx, 0);
        }
        if (is_heap_string_expr(gen, call)) {
            return callee_param_store_escapes_via_body(gen, call->value, arg_idx);
        }
        return callee_param_escapes_via_body(gen, call->value, arg_idx, 0);
    }
    /* No visible body (extern / unknown): a `string`-typed param looks
     * read-only to call_arg_escapes, but a storing wrapper lets it
     * escape — keep both the kind check and the (no-body → 0) body
     * walk so unknown callees stay conservatively escaped. */
    TypeKind k = lookup_callee_param_kind(gen, call->value, arg_idx);
    return call_arg_escapes(k) ||
           callee_param_escapes_via_body(gen, call->value, arg_idx, 0);
}

static void escape_inspect_arg(CodeGenerator* gen, ASTNode* call, int i,
                               const char* consumed_lhs);
static void escape_walk_chain_rest(CodeGenerator* gen, ASTNode* expr, const char* leaf,
                                   const char* consumed_lhs);

/* #2548: the calls that only hand a heap-tracked local back
 * (handback_take_leaf) and that an owning slot takes as the local itself
 * (emit_string_take): the value of a binding, of a container store or of a
 * struct literal's field, or an `if` / `match` arm of one. The take moves
 * the local on its last use and copies it otherwise, exactly as it takes a
 * bare local there, so the local does not escape through the call: an
 * escaped local was never freed when another arm ran, or when the take
 * copied. Registered by the node that takes the value, consulted where the
 * walk meets the call; a stack, popped by the registering node. */
#define TAKEN_HANDBACK_MAX 64
static ASTNode* g_taken_handbacks[TAKEN_HANDBACK_MAX];
static int g_taken_handback_count = 0;

static void register_taken_handbacks(CodeGenerator* gen, ASTNode* e) {
    if (!e) return;
    if (e->type == AST_IF_EXPRESSION && e->child_count >= 3) {
        register_taken_handbacks(gen, e->children[1]);
        register_taken_handbacks(gen, e->children[2]);
        return;
    }
    if (e->type == AST_MATCH_STATEMENT) {
        for (int i = 1; i < e->child_count; i++) {
            ASTNode* arm = e->children[i];
            if (arm && arm->type == AST_MATCH_ARM && arm->child_count >= 2)
                register_taken_handbacks(gen, match_arm_value(arm->children[1]));
        }
        return;
    }
    if (g_taken_handback_count < TAKEN_HANDBACK_MAX && handback_take_leaf(gen, e))
        g_taken_handbacks[g_taken_handback_count++] = e;
}

static int is_taken_handback(ASTNode* e) {
    for (int i = g_taken_handback_count - 1; i >= 0; i--) {
        if (g_taken_handbacks[i] == e) return 1;
    }
    return 0;
}

/* The arguments of the hand-back chain `expr` other than the one carrying
 * `leaf` on (handback_leaf), each inspected as an argument of its call. */
static void escape_walk_chain_rest(CodeGenerator* gen, ASTNode* expr, const char* leaf,
                                   const char* consumed_lhs) {
    if (!expr || expr->type != AST_FUNCTION_CALL) return;
    for (int i = 0; i < expr->child_count; i++) {
        if (subtree_mentions_param(expr->children[i], leaf)) {
            escape_walk_chain_rest(gen, expr->children[i], leaf, consumed_lhs);
        } else {
            escape_inspect_arg(gen, expr, i, consumed_lhs);
        }
    }
}

static void escape_inspect_arg(CodeGenerator* gen, ASTNode* call, int i,
                               const char* consumed_lhs) {
    ASTNode* arg = call->children[i];
    if (!arg) return;
    if (arg->type == AST_IDENTIFIER && arg->value) {
        /* Bare identifier in argument position. See
         * call_arg_position_escapes for the escape rationale
         * (read-only string/scalar params don't escape; ptr/
         * unknown/@retain do). */
        if (is_heap_string_var(gen, arg->value) &&
            (consumed_lhs == NULL ||
             strcmp(arg->value, consumed_lhs) != 0)) {
            if (call_arg_position_escapes(gen, call, i)) {
                mark_escaped_string_var(gen, arg->value);
            }
        }
        return;
    }
    /* #2499: a call that only hands a variable back passes it on to this
     * position (`fs_make_temp_file_raw(d, temp_prefix(p))`), which decides
     * whether it escapes, as handback_leaf_is does in the keep walk. */
    const char* leaf = arg->type == AST_FUNCTION_CALL ? handback_leaf(gen, arg, 0) : NULL;
    if (leaf && is_heap_string_var(gen, leaf) && !call_arg_position_escapes(gen, call, i)) {
        escape_walk_chain_rest(gen, arg, leaf, consumed_lhs);
        return;
    }
    /* Non-identifier arg (literal, nested call, etc.): still recurse to
     * find any identifiers buried inside. */
    escape_walk(gen, arg, consumed_lhs);
}

static void escape_inspect_call_args(CodeGenerator* gen, ASTNode* call,
                                      const char* consumed_lhs) {
    if (!call) return;
    /* Children of an AST_FUNCTION_CALL are the arg expressions. Each
     * is itself walked recursively in case it nests further calls. */
    /* A list add or map put takes a heap-tracked local (moved on its
     * last use, copied otherwise), so the local keeps its own frees. */
    ASTNode* taken = string_container_store_value(gen, call);
    for (int i = 0; i < call->child_count; i++) {
        if (call->children[i] && call->children[i] != taken)
            escape_inspect_arg(gen, call, i, consumed_lhs);
    }
}

/* Where the escape walk is: inside how many closure bodies, and which node
 * is a statement of the block it last entered. A field store takes a local
 * (field_store_takes_local) only where it is emitted as a statement of the
 * function being generated (an assignment arm of a match is parsed as a
 * block holding one, #2575); a closure's body is emitted in its own context,
 * where the local is a capture. */
static int g_escape_closure_depth = 0;
/* Of those, how many are trailing blocks: emitted inline in the caller's
 * context, where a store moves the local itself (and a builder's block may
 * be emitted twice), so no store in one is exempt. */
static int g_escape_trailing_depth = 0;
static const ASTNode* g_escape_block_stmt = NULL;

static void escape_walk(CodeGenerator* gen, ASTNode* node,
                         const char* consumed_lhs) {
    if (!node) return;

    /* Closure body — every captured outer heap-string var escapes
     * conservatively. Closures may outlive the enclosing scope (stored
     * in actor state, queued in scheduler, returned from the function),
     * so we cannot reason locally about whether the closure stores the
     * captured pointer. Walk the closure body looking for identifiers
     * that name heap-tracked vars; mark each. */
    if (node->type == AST_CLOSURE) {
        /* The "consumed_lhs" exception does not apply inside a closure
         * — even if the closure happens to be the RHS of `V = closure
         * { ... uses V ... }`, the closure may run later, after V has
         * been reassigned, with V already freed. Walk the closure body
         * unconditionally. */
        int trailing = node->value && strcmp(node->value, "trailing") == 0;
        g_escape_closure_depth++;
        g_escape_trailing_depth += trailing;
        for (int i = 0; i < node->child_count; i++) {
            escape_walk(gen, node->children[i], NULL);
        }
        g_escape_trailing_depth -= trailing;
        g_escape_closure_depth--;
        return;
    }

    /* Identifier referenced bare in a non-call, non-RHS context (e.g.
     * a struct-field expr, an array index expr, a return value). The
     * caller's recursion handles call-arg-identifier specifically; here
     * we skip — bare identifier reads (not stores) don't escape. */

    /* Function call — inspect args. Method calls (`receiver.method(args)`)
     * are also AST_FUNCTION_CALL nodes (with the dotted callee in
     * `value`); the receiver appears as a regular argument among the
     * children. */
    if (node->type == AST_FUNCTION_CALL) {
        if (is_taken_handback(node)) {
            ASTNode* hb = handback_take_leaf(gen, node);
            if (hb && (!consumed_lhs || strcmp(hb->value, consumed_lhs) != 0)) {
                escape_walk_chain_rest(gen, node, hb->value, consumed_lhs);
                return;
            }
        }
        int saved_taken = g_taken_handback_count;
        int slot = container_store_slot(gen, node);
        if (slot >= 0 && slot < node->child_count) register_taken_handbacks(gen, node->children[slot]);
        escape_inspect_call_args(gen, node, consumed_lhs);
        g_taken_handback_count = saved_taken;
        return;
    }

    /* Variable assignment / declaration: `V = <expr>`. The RHS is
     * walked with `consumed_lhs = V` so a `V = f(V, ...)` pattern
     * doesn't mark V escaped. Other LHS values inside the RHS still
     * follow normal rules. */
    if (node->type == AST_VARIABLE_DECLARATION && node->value &&
        node->child_count > 0) {
        const char* lhs = node->value;
        /* When `lhs` is an env-backed capture of the current closure,
         * `V = <expr>` writes `_env->V`, which outlives this closure's
         * activation. So a heap-string assigned to it escapes, exactly
         * like the non-local `s.field = expr` case below: mark it, or the
         * closure-exit defer-free reclaims the buffer while the env still
         * points at it. A promoted capture's cell is not such a slot: it
         * takes a value of its own (the store moves the local's buffer or
         * copies it, #2514), so the local keeps its own exit free. */
        int lhs_is_capture = 0;
        for (int e = 0; e < gen->current_env_capture_count; e++) {
            if (gen->current_env_captures[e] &&
                strcmp(gen->current_env_captures[e], lhs) == 0) {
                lhs_is_capture = 1; break;
            }
        }
        if (lhs_is_capture) {
            ASTNode* rhs = node->children[node->child_count - 1];
            if (rhs && rhs->type == AST_IDENTIFIER && rhs->value &&
                is_heap_string_var(gen, rhs->value)) {
                mark_escaped_string_var(gen, rhs->value);
            }
            ASTNode* hb = handback_take_leaf(gen, rhs);
            if (hb) mark_escaped_string_var(gen, hb->value);
        }
        /* #2548: a string slot takes its value (emit_string_take), so a
         * call there that only hands a local back takes the local as
         * `V = local` does. */
        int saved_taken = g_taken_handback_count;
        if (!lhs_is_capture &&
            (is_heap_string_var(gen, lhs) || is_module_global_var(gen, lhs) ||
             is_actor_state_var(gen, lhs))) {
            register_taken_handbacks(gen, node->children[node->child_count - 1]);
        }
        for (int i = 0; i < node->child_count; i++) {
            escape_walk(gen, node->children[i], lhs);
        }
        g_taken_handback_count = saved_taken;
        return;
    }

    /* Return statement — the returned value escapes. If it's a bare
     * identifier naming a heap-tracked var, mark it return-escaped.
     * This is a SEPARATE channel from container-escape (call-arg,
     * struct-field, closure-capture): the reassign-wrapper-free
     * fires for return-only-escaped vars (no recipient stash means
     * the wrapper can safely reclaim the old buffer at each loop
     * iteration), while the function-exit defer-free is still
     * suppressed (otherwise the return value would dangle). See
     * `return_escaped_string_vars` in codegen.h for the contract. */
    if (node->type == AST_RETURN_STATEMENT) {
        for (int i = 0; i < node->child_count; i++) {
            ASTNode* c = node->children[i];
            if (c && c->type == AST_IDENTIFIER && c->value &&
                is_heap_string_var(gen, c->value)) {
                mark_return_escaped_string_var(gen, c->value);
            } else {
                escape_walk(gen, c, consumed_lhs);
            }
        }
        return;
    }

    /* Non-trivial assignment — `s.field = expr` (LHS is an
     * AST_MEMBER_ACCESS) or `arr[i] = expr` (LHS is an
     * AST_ARRAY_ACCESS). The parser uses AST_BINARY_EXPRESSION with
     * value "=" for these; bare-local reassignment uses
     * AST_VARIABLE_DECLARATION instead, which is handled above. The
     * write target outlives the current activation record (a struct
     * instance, an array element passed in by ptr, an actor's state,
     * etc.), so any heap-tracked variable assigned in the RHS must
     * be marked escaped — otherwise the function-exit defer-free
     * reclaims the buffer while the struct field is still pointing
     * at it (cross-function setter/getter dangle). Closes the
     * field-write half of the escape-edge gap that landed in the
     * #420 follow-up alongside call-arg, return, and closure
     * capture. */
    if (node->type == AST_BINARY_EXPRESSION && node->value &&
        strcmp(node->value, "=") == 0 && node->child_count == 2) {
        ASTNode* lhs = node->children[0];
        ASTNode* rhs = node->children[1];
        if (lhs && lhs->type != AST_IDENTIFIER) {
            /* #2474: an element of a string array a closure writes takes a
             * buffer of its own (emit_cell_string_element_store moves the
             * local's or copies it), so the local keeps its own exit free.
             * An Aether struct's string field moves the local's ownership
             * into the field (field_store_takes_local), so the local keeps
             * its exit free too, a no-op once moved: escaped, a local stored
             * on one path leaked on every other (std.jsonpath's `save_err =
             * p.err`, restored into p.err only when a speculative parse
             * failed, leaked its copy on every success). */
            /* In the function's own blocks the store moves the local into
             * the field. In a closure's body (emitted as a function of its
             * own) the local is a capture the env or a cell holds, and the
             * store copies it (is_captured_string, #2574), so it keeps
             * nothing of the local either. */
            int takes = node == g_escape_block_stmt && g_escape_trailing_depth == 0 &&
                        rhs && rhs->type == AST_IDENTIFIER && rhs->value &&
                        (g_escape_closure_depth > 0 ||
                         (!is_promoted_capture(gen, rhs->value) &&
                          !is_env_capture_name(gen, rhs->value))) &&
                        field_store_takes_local(gen, lhs);
            if (rhs && rhs->type == AST_IDENTIFIER && rhs->value &&
                is_heap_string_var(gen, rhs->value) &&
                !is_cell_string_element(gen, lhs) && !takes) {
                mark_escaped_string_var(gen, rhs->value);
            }
            /* Walk both sides for any nested calls / closures whose
             * inner identifiers also need the regular escape
             * treatment. consumed_lhs cleared because the bare-LHS
             * exception (`V = f(V, …)`) doesn't apply once the LHS
             * is a non-trivial location. */
            escape_walk(gen, lhs, NULL);
            escape_walk(gen, rhs, NULL);
            return;
        }
    }

    /* A literal of a header-defined struct (`extern struct ... @c_import`):
     * its string fields BORROW (no `_heap_<field>` to move ownership into),
     * and the value may outlive this activation (returned, stored, handed
     * to C). A heap-tracked local named in a field therefore escapes, as it
     * does when stored with `s.field = v`: kept alive rather than freed at
     * exit under the struct that still points at it. */
    if (node->type == AST_STRUCT_LITERAL && node->value &&
        aether_is_c_import_struct(node->value)) {
        for (int i = 0; i < node->child_count; i++) {
            ASTNode* fi = node->children[i];
            if (fi && fi->type == AST_ASSIGNMENT && fi->child_count > 0 &&
                fi->children[0] && fi->children[0]->type == AST_IDENTIFIER &&
                fi->children[0]->value &&
                is_heap_string_var(gen, fi->children[0]->value)) {
                mark_escaped_string_var(gen, fi->children[0]->value);
            }
        }
    }

    /* Don't descend into nested function definitions — their own pass
     * handles them. */
    if (node->type == AST_FUNCTION_DEFINITION ||
        node->type == AST_BUILDER_FUNCTION) {
        return;
    }

    /* #2548: an Aether struct literal takes each string field's value. */
    int saved_taken = g_taken_handback_count;
    if (node->type == AST_STRUCT_LITERAL && !(node->value && aether_is_c_import_struct(node->value))) {
        for (int i = 0; i < node->child_count; i++) {
            ASTNode* fi = node->children[i];
            if (fi && (fi->type == AST_ASSIGNMENT || fi->type == AST_FIELD_INIT) && fi->child_count > 0)
                register_taken_handbacks(gen, fi->children[0]);
        }
    }

    /* Default: recurse with the same consumed_lhs context. */
    for (int i = 0; i < node->child_count; i++) {
        /* A block's statement, or the expression an expression statement
         * holds (`o.f = s` is parsed as one around the `=`). */
        if (node->type == AST_BLOCK) {
            ASTNode* st = node->children[i];
            g_escape_block_stmt = st && st->type == AST_EXPRESSION_STATEMENT &&
                                  st->child_count > 0 ? st->children[0] : st;
        }
        escape_walk(gen, node->children[i], consumed_lhs);
    }
    g_taken_handback_count = saved_taken;
}

void mark_escaped_heap_string_vars(CodeGenerator* gen, ASTNode* body) {
    if (!gen || !body) return;
    escape_walk(gen, body, NULL);
}

/* Emit a builder block's body as its own C scope.
 *
 * CRITICAL: the `{ ... }` braces must be matched by a defer scope. Cleanup
 * queued inside the block (a heap string, a closure-capture cell) names a C
 * variable declared between those braces; emitting it at the enclosing
 * function's exit instead puts the free where the name is out of scope, and
 * the generated C does not compile. */
void emit_trailing_block_body(CodeGenerator* gen, ASTNode* body) {
    if (!gen || !body) return;
    int saved_var_count = gen->declared_var_count;
    print_indent(gen);
    fprintf(gen->output, "{\n");
    gen->indent_level++;
    enter_scope(gen);
    /* Every caller pushed this block's builder context just before calling
     * here. Its pop is the scope's first defer, so it runs after the
     * block's own defers on every exit: the fall-through end below, a
     * return, a break/continue out of the block (see try_emit_builder_ctx_pop
     * in codegen.c). Callers do not emit the pop themselves. */
    ASTNode* ctx_pop = create_ast_node(AST_EXPRESSION_STATEMENT, NULL,
                                       body->line, body->column);
    if (ctx_pop) {
        if (ctx_pop->annotation) free(ctx_pop->annotation);
        ctx_pop->annotation = strdup("builder_ctx_pop");
        codegen_own_node(gen, ctx_pop);
        push_defer(gen, ctx_pop);
    }
    gen->in_trailing_block++;
    for (int si = 0; si < body->child_count; si++) {
        generate_statement(gen, body->children[si]);
    }
    gen->in_trailing_block--;
    exit_scope(gen);
    gen->indent_level--;
    print_indent(gen);
    fprintf(gen->output, "}\n");
    truncate_declared_vars(gen, saved_var_count);
}

/* The one closure-argument shape codegen can prove is dead after the call:
 * a non-trailing closure passed to a parameter that provably neither stores
 * nor returns it. Returns that closure node, or NULL when no such proof
 * exists. Two callers depend on the same verdict: the env-drain that frees
 * the closure's heap env after the call, and the capture-box escape walk
 * that frees the cells that env points at.
 *
 * Transient capturing-closure argument: a closure passed to a parameter that
 * neither stores nor returns it (callback pattern, run(cb){ cb() }) is dead
 * after the call, so its heap env must be freed or it leaks. Find the first
 * non-trailing closure arg (trailing blocks are inlined at the call site, not
 * passed by value) and check whether its parameter provably does not escape.
 *
 * Soundness gate (closure-env-freed-when-passed-to-extern): the drain is only
 * safe with *proof* the callee neither stores nor returns the closure. The
 * `callee_param_escapes_via_body` walk requires a visible body to be
 * authoritative; for an extern callee the walk silently defaults to "does not
 * escape", which is exactly wrong for the common extern-callback-registry
 * pattern (the C side keeps the boxed closure and invokes it later). Treat
 * unknown-body callees as escaping, the fail-safe direction (leak >> UAF),
 * unless the extern's declaration marks the parameter `@noescape`: used only
 * during the call, so the argument is dead after it (#2523). */
ASTNode* transient_closure_arg(CodeGenerator* gen, ASTNode* call) {
    if (!gen || !call || call->type != AST_FUNCTION_CALL || !call->value) return NULL;

    /* A closure literal, or a closure a call hands over that nobody else
     * holds (#2506: `run(make_counter())`): either way the argument is the
     * only reference, so it is dead once the call returns. */
    ASTNode* cclos = NULL; int cclos_idx = -1;
    for (int ai = 0; ai < call->child_count; ai++) {
        ASTNode* a = call->children[ai];
        if (a && ((a->type == AST_CLOSURE &&
                   !(a->value && strcmp(a->value, "trailing") == 0)) ||
                  call_returns_owned_closure(gen, a))) {
            cclos = a; cclos_idx = ai; break;
        }
    }
    if (!cclos || cclos_idx < 0) return NULL;
    /* #2518: a container that stores the closure retains it; the argument's own
     * reference is dead after the call. */
    if (closure_container_store_value(gen, call) == cclos) return cclos;
    /* #2523: an extern's `@noescape` parameter is its declaration's word
     * that nothing is kept. An extern injects no `_ctx`, so the argument's
     * index is the parameter's. */
    if (is_noescape_extern_param(gen, call->value, cclos_idx)) return cclos;

    /* Map AST arg index -> function-def param index. When the callee is a
     * `_ctx: ptr` builder and the user omitted `_ctx`, codegen auto-injects
     * `_aether_ctx_get()` at position 0; the AST args are then shifted left by
     * one relative to the callee's declared params. Without this shift the
     * escape walk checks the wrong param (e.g. the label, which provably does
     * not escape) and the drain fires under a false-non-escape verdict -> UAF.
     *
     * Look the callee up by its normalised (dot->underscore) name, which is how
     * cross-module functions land in the merged program AST. Without
     * normalisation an imported `aether_ui.btn` call site looks up
     * "aether_ui.btn" but the merged AST has node value "aether_ui_btn", the
     * lookup misses, the `_ctx`-injection shift is skipped, and the escape walk
     * checks the wrong param (label, read-only -> false non-escape -> UAF). */
    int param_idx = cclos_idx;
    const char* fn = codegen_normalise_callee(call->value);
    ASTNode* fdef = find_function_definition_by_name(gen->program, fn);
    if (fdef) {
        int declared_params = 0;
        for (int pi = 0; pi < fdef->child_count; pi++) {
            ASTNode* p = fdef->children[pi];
            if (!p) continue;
            if (p->type == AST_GUARD_CLAUSE) continue;
            if (p->type == AST_BLOCK) continue;
            declared_params++;
        }
        int user_args = 0;
        for (int ai = 0; ai < call->child_count; ai++) {
            ASTNode* a = call->children[ai];
            if (a && a->type == AST_CLOSURE && a->value &&
                strcmp(a->value, "trailing") == 0) continue;
            user_args++;
        }
        if (user_args == declared_params - 1 && declared_params > 0) {
            ASTNode* p0 = fdef->children[0];
            if (p0 && p0->value && strcmp(p0->value, "_ctx") == 0) {
                param_idx = cclos_idx + 1;
            }
        }
    }

    if (callee_has_visible_body(gen, call->value) &&
        callee_param_escapes_via_body(gen, call->value, param_idx, 0) == 0) {
        return cclos;
    }
    return NULL;
}

/* ------------------------------------------------------------------
 * Env of a closure bound to a local (#2480, #2494)
 *
 * `g = || { ... }` mallocs g's env, and the env holds a reference to every
 * promoted cell, captured string and captured closure it uses. The value's
 * owner holds one reference to the env, and every env that captures `g`
 * takes one of its own (#2494), so the declaring scope can release its
 * reference at scope exit provided no copy of the value that does NOT hold
 * a reference outlives the scope. Every use of `g` makes such a copy except
 * these, which is all the walk below accepts:
 *
 *   - invoking it: `g(...)`, `call(g, ...)`;
 *   - passing it to a user function whose parameter is itself only used these
 *     ways (every clause is walked the same way; an extern, a named argument
 *     or a callee that cannot be resolved keeps the value);
 *   - capturing it in a closure, which retains the env, provided the closure
 *     body uses it only these ways too.
 *
 * Any other mention (a return, an alias, a store into a struct, list, map,
 * message, global or actor state, an operand) keeps the env, as does a
 * binding that is not a fresh closure: a closure literal, or the result of a
 * function that returns one it alone holds (function_returns_owned_closure).
 * A tuple slot or another variable may be a value someone else holds.
 * Keeping is a leak at worst; a wrong free is a use after free, so every
 * doubt keeps.
 * ------------------------------------------------------------------ */

#define ENV_SCAN_MAX_DEPTH 8
#define ENV_SCAN_MAX_CLEARS 16

typedef struct {
    const char* name;      /* the local (or, in param_mode, the parameter) */
    ASTNode* root;         /* body of the C function the name belongs to */
    ASTNode* owner;        /* the function / closure whose body root is */
    ASTNode* decl;         /* the binding being declared; the walk must meet it */
    int param_mode;
    int allow_return;      /* `return name` hands the reference to the caller */
    int depth;
    int escapes;
    int bindings;
    int saw_decl;
    int heap_env;          /* some bound closure captures, so its env is malloc'd */
    int cid;               /* -2 none bound yet, -1 several closures, else the one */
    /* #2506: a use that hands the value on is not fatal when it is a whole
     * simple statement: the local stops owning its value right before that
     * statement, and a later fresh binding owns again. */
    int track_clears;
    ASTNode* cur_stmt;     /* the statement of the enclosing block being walked */
    int cur_stmt_nested;   /* cur_stmt belongs to a nested closure's body */
    ASTNode* clears[ENV_SCAN_MAX_CLEARS];
    int clear_count;
    /* #2519: hand-offs inside a nested closure's body, one per mention:
     * the body retains the env for the new holder right before that
     * statement (a `retain` entry of register_env_own_clear), since the
     * local's flag is out of its reach. */
    ASTNode* retain_stmts[ENV_SCAN_MAX_CLEARS];
    ASTNode* retain_refs[ENV_SCAN_MAX_CLEARS];
    int retain_count;
} EnvScan;

static void env_scan_walk(CodeGenerator* gen, EnvScan* s, ASTNode* node,
                          ASTNode* parent, int nested);
static int returns_owned_closure(CodeGenerator* gen, EnvScan* s, const char* callee,
                                 ASTNode* call);
static int call_hands_over_closure(CodeGenerator* gen, EnvScan* s, ASTNode* call);

static int env_scan_is_real_closure(ASTNode* n) {
    return n && n->type == AST_CLOSURE &&
           !(n->value && strcmp(n->value, "trailing") == 0);
}

/* Does `node` use `name` at all? Unlike subtree_mentions_param this counts an
 * invocation `name(...)` too, which is how a closure body captures a closure. */
static int env_scan_mentions(ASTNode* node, const char* name) {
    if (!node) return 0;
    if (node->value && node->type != AST_LITERAL && node->type != AST_CLOSURE &&
        strcmp(node->value, name) == 0) return 1;
    for (int i = 0; i < node->child_count; i++) {
        if (env_scan_mentions(node->children[i], name)) return 1;
    }
    return 0;
}

static int env_scan_capture_count(CodeGenerator* gen, ASTNode* closure) {
    for (int ci = 0; ci < gen->closure_count; ci++) {
        if (gen->closures[ci].closure_node == closure) return gen->closures[ci].capture_count;
    }
    return 0;
}

/* Does anything in `node` bind `name` (a local, a parameter, a pattern)? */
static int env_scan_binds(ASTNode* node, const char* name) {
    if (!node) return 0;
    if ((node->type == AST_VARIABLE_DECLARATION || node->type == AST_PATTERN_VARIABLE ||
         node->type == AST_CLOSURE_PARAM) && node->value && strcmp(node->value, name) == 0) {
        return 1;
    }
    for (int i = 0; i < node->child_count; i++) {
        if (env_scan_binds(node->children[i], name)) return 1;
    }
    return 0;
}

/* Is the callee name of `call` a variable of the scanned function rather than
 * the top-level function of that name? Then nothing is known about it. */
static int env_scan_callee_is_variable(CodeGenerator* gen, EnvScan* s, const char* callee) {
    if (env_scan_binds(s->root, callee)) return 1;
    if (s->owner) {
        for (int i = 0; i < s->owner->child_count; i++) {
            ASTNode* p = s->owner->children[i];
            if (p && p != s->root && p->type != AST_BLOCK && env_scan_binds(p, callee)) return 1;
        }
    }
    if (s->param_mode || s->allow_return) return 0;
    if (is_var_declared(gen, callee)) return 1;
    /* A closure body also sees its captures. */
    for (int ci = 0; ci < gen->closure_count; ci++) {
        if (gen->closures[ci].closure_node != s->owner) continue;
        for (int k = 0; k < gen->closures[ci].capture_count; k++) {
            if (gen->closures[ci].captures[k] &&
                strcmp(gen->closures[ci].captures[k], callee) == 0) return 1;
        }
    }
    return 0;
}

/* The body block of a function definition, NULL without one. */
static ASTNode* env_scan_fn_body(ASTNode* fdef) {
    for (int i = fdef->child_count - 1; i >= 0; i--) {
        if (fdef->children[i] && fdef->children[i]->type == AST_BLOCK) return fdef->children[i];
    }
    return NULL;
}

/* Is `rhs`, bound to s->name, a closure value nobody else holds: a closure
 * literal, or a call of a function that returns one it alone holds? Either
 * way it must not read the name, whose previous value it would capture or
 * be handed. */
/* #2525: a closure read out of a field or an element (`x = h.cb`), which a
 * binding retains, so the local holds a reference of its own. */
static int actor_state_string_tracked(CodeGenerator* gen, const char* name);

static int closure_view_binding(ASTNode* rhs) {
    return rhs && (rhs->type == AST_MEMBER_ACCESS || rhs->type == AST_ARRAY_ACCESS) &&
           rhs->node_type && rhs->node_type->kind == TYPE_FUNCTION && !rhs->node_type->is_fnptr;
}

/* #2528: a closure an ask brings back (`x = k ? Get {}`): the reply handed
 * the asker a reference of its own, so the binding owns it like a literal. */
static int closure_ask_binding(ASTNode* rhs) {
    return rhs && rhs->type == AST_SEND_ASK && rhs->node_type &&
           rhs->node_type->kind == TYPE_FUNCTION && !rhs->node_type->is_fnptr;
}

static int env_scan_fresh_binding(CodeGenerator* gen, EnvScan* s, ASTNode* rhs) {
    if (!rhs || env_scan_mentions(rhs, s->name)) return 0;
    if (env_scan_is_real_closure(rhs)) return 1;
    if (closure_view_binding(rhs) || closure_ask_binding(rhs)) return 1;
    if (rhs->type != AST_FUNCTION_CALL || !rhs->value) return 0;
    for (int i = 0; i < rhs->child_count; i++) {
        /* A builder's trailing block re-emits the call with its config. */
        if (rhs->children[i] && rhs->children[i]->type == AST_CLOSURE &&
            !env_scan_is_real_closure(rhs->children[i])) return 0;
    }
    return call_hands_over_closure(gen, s, rhs);
}

/* Callees whose parameter scan is in progress: a recursive call passing the
 * parameter on does not by itself keep it. */
static ASTNode* g_env_scan_active[ENV_SCAN_MAX_DEPTH + 2];
static int g_env_scan_active_param[ENV_SCAN_MAX_DEPTH + 2];
static int g_env_scan_active_count = 0;

/* May the callee of `call` keep the value passed as `arg` (a direct child)?
 * Only a user function whose matching parameter the walk proves is used
 * without being copied answers no. */
static int env_scan_param_keeps(CodeGenerator* gen, EnvScan* s, ASTNode* call, ASTNode* arg) {
    if (s->depth >= ENV_SCAN_MAX_DEPTH || !gen->program || !call->value ||
        strcmp(call->value, "call") == 0) return 1;
    int pos = -1, user_args = 0;
    for (int i = 0; i < call->child_count; i++) {
        ASTNode* a = call->children[i];
        if (!a) continue;
        if (a->type == AST_NAMED_ARG) return 1;
        if (a->type == AST_CLOSURE && a->value && strcmp(a->value, "trailing") == 0) continue;
        if (a == arg) pos = user_args;
        user_args++;
    }
    if (pos < 0) return 1;
    if (env_scan_callee_is_variable(gen, s, call->value)) return 1;
    /* #2523: an extern's `@noescape` parameter is used only during the
     * call, so the local keeps its reference and releases it at scope end,
     * as after an Aether callee that keeps nothing. */
    if (is_noescape_extern_param(gen, call->value, pos)) return 0;
    const char* fn = codegen_normalise_callee(call->value);
    const DefClauses* dc = program_index_clauses(gen->program, fn);
    if (!dc || dc->count == 0) return 1;
    for (int c = 0; c < dc->count; c++) {
        ASTNode* fdef = dc->nodes[c];
        if (!fdef) return 1;
        ASTNode* params[64];
        int declared = 0;
        ASTNode* body = NULL;
        for (int i = 0; i < fdef->child_count; i++) {
            ASTNode* p = fdef->children[i];
            if (!p) continue;
            if (p->type == AST_BLOCK) { body = p; continue; }
            if (p->type == AST_GUARD_CLAUSE || p->type == AST_REQUIRES_CLAUSE ||
                p->type == AST_ENSURES_CLAUSE) continue;
            if (declared >= 64) return 1;
            params[declared++] = p;
        }
        /* The `_ctx` a builder callee takes is injected, not written. */
        int pidx = pos;
        if (user_args == declared - 1 && declared > 0 && params[0]->value &&
            strcmp(params[0]->value, "_ctx") == 0) {
            pidx = pos + 1;
        } else if (user_args > declared) {
            return 1;
        }
        ASTNode* param = params[pidx];
        if (!body || !param->value ||
            (param->type != AST_PATTERN_VARIABLE && param->type != AST_VARIABLE_DECLARATION)) {
            return 1;
        }
        int active = 0;
        for (int a = 0; a < g_env_scan_active_count; a++) {
            if (g_env_scan_active[a] == fdef && g_env_scan_active_param[a] == pidx) { active = 1; break; }
        }
        if (active) continue;
        EnvScan ps;
        memset(&ps, 0, sizeof(ps));
        ps.name = param->value;
        ps.root = body;
        ps.owner = fdef;
        ps.param_mode = 1;
        ps.depth = s->depth + 1;
        ps.cid = -2;
        g_env_scan_active[g_env_scan_active_count] = fdef;
        g_env_scan_active_param[g_env_scan_active_count] = pidx;
        g_env_scan_active_count++;
        env_scan_walk(gen, &ps, body, NULL, 0);
        g_env_scan_active_count--;
        if (ps.escapes) return 1;
    }
    return 0;
}

/* Functions whose return contract is being decided: a call back into one
 * on the way is not proven to return an owned closure. */
static ASTNode* g_owned_ret_active[ENV_SCAN_MAX_DEPTH + 2];
static int g_owned_ret_active_count = 0;

/* The parameter `fdef` returns, when every return hands back the same
 * parameter (`keep(cb) -> fn { return cb }`): its index, -1 when no return
 * does, -2 when returns disagree. The callee hands the caller's argument
 * back, so the call's result is the caller's own exactly when that argument
 * was a closure nobody else holds (returns_owned_closure checks the call). */
static int g_returns_param = -1;

/* The index of parameter `name` of `fdef` (a function definition's pattern
 * variables, a closure's AST_CLOSURE_PARAMs), or -1. */
static int owned_return_param_index(ASTNode* fdef, const char* name) {
    int idx = 0;
    for (int i = 0; fdef && i < fdef->child_count; i++) {
        ASTNode* p = fdef->children[i];
        if (!p) continue;
        if (p->type == AST_BLOCK) break;
        if (p->type == AST_GUARD_CLAUSE || p->type == AST_REQUIRES_CLAUSE ||
            p->type == AST_ENSURES_CLAUSE) continue;
        if (p->type != AST_PATTERN_VARIABLE && p->type != AST_VARIABLE_DECLARATION &&
            p->type != AST_CLOSURE_PARAM) continue;
        if (p->value && strcmp(p->value, name) == 0) return idx;
        idx++;
    }
    return -1;
}

/* Every `return` of `fdef` (a closure body's returns are its own) must hand
 * back a closure value only the caller will hold. */
static int returns_owned_in(CodeGenerator* gen, ASTNode* fdef, ASTNode* body,
                            ASTNode* node, int depth, int* returns) {
    if (!node) return 1;
    if (env_scan_is_real_closure(node)) return 1;
    if (node->type == AST_RETURN_STATEMENT) {
        (*returns)++;
        if (node->child_count != 1 || !node->children[0]) return 0;
        ASTNode* e = node->children[0];
        if (env_scan_is_real_closure(e)) return 1;   /* fresh: nobody else holds it */
        /* #2525: a field or element read is returned retained by a named
         * function's return emission (its declared `fn` result is what the
         * emission keys on; a closure body's return does not), so the
         * caller holds a reference of its own. */
        if ((e->type == AST_MEMBER_ACCESS || e->type == AST_ARRAY_ACCESS) &&
            fdef && fdef->type != AST_CLOSURE) return 1;
        EnvScan s;
        memset(&s, 0, sizeof(s));
        s.root = body;
        s.owner = fdef;
        s.allow_return = 1;
        s.depth = depth;
        s.cid = -2;
        if (e->type == AST_FUNCTION_CALL && e->value) {
            s.name = "";
            return call_hands_over_closure(gen, &s, e);
        }
        if (e->type != AST_IDENTIFIER || !e->value) return 0;
        /* A parameter returned is the caller's argument handed back: the
         * result is the caller's own when that argument was (the call
         * site decides, through g_returns_param), provided the body keeps
         * no copy of it (a store into a holder retains its own, #2525). */
        int pidx = owned_return_param_index(fdef, e->value);
        if (pidx >= 0) {
            s.name = e->value;
            s.param_mode = 1;
            env_scan_walk(gen, &s, body, NULL, 0);
            if (s.escapes) return 0;
            if (g_returns_param == -1 || g_returns_param == pidx) {
                g_returns_param = pidx;
                return 1;
            }
            g_returns_param = -2;
            return 0;
        }
        /* A local bound only to fresh closures whose every other use leaves
         * no copy: its reference goes to the caller. */
        s.name = e->value;
        env_scan_walk(gen, &s, body, NULL, 0);
        return !s.escapes && s.bindings > 0;
    }
    for (int i = 0; i < node->child_count; i++) {
        if (!returns_owned_in(gen, fdef, body, node->children[i], depth, returns)) return 0;
    }
    return 1;
}

/* #2494: does user function `callee` return a closure that only its caller
 * holds? Then the caller owns the value's reference, and a local it binds
 * the call to is freed by the caller's scope like a closure literal is.
 * Every clause must return only fresh closures. */
/* Is the argument the callee hands back (parameter `pidx` of a callee whose
 * every return is that parameter) a closure nobody else holds at `call`: a
 * closure literal, or a call that hands one over? `first_arg` is 1 for the
 * `call(f, ...)` form. A builder's injected `_ctx` shifts the arguments, so
 * builders are left alone. */
static int handed_back_arg_is_fresh(CodeGenerator* gen, EnvScan* s, ASTNode* fdef,
                                    ASTNode* call, int pidx, int first_arg) {
    if (!call || pidx < 0) return 0;
    if (fdef && fdef->child_count > 0 && fdef->children[0] && fdef->children[0]->value &&
        strcmp(fdef->children[0]->value, "_ctx") == 0) return 0;
    int ai = first_arg + pidx;
    if (ai >= call->child_count) return 0;
    ASTNode* arg = call->children[ai];
    if (env_scan_is_real_closure(arg)) return 1;
    return arg && arg->type == AST_FUNCTION_CALL && call_hands_over_closure(gen, s, arg);
}

static int returns_owned_closure(CodeGenerator* gen, EnvScan* s, const char* callee,
                                 ASTNode* call) {
    if (!gen->program || !callee || s->depth >= ENV_SCAN_MAX_DEPTH) return 0;
    const char* fn = codegen_normalise_callee(callee);
    const DefClauses* dc = program_index_clauses(gen->program, fn);
    if (!dc || dc->count == 0) return 0;
    int handed = -1;   /* the parameter every clause hands back, if any */
    ASTNode* first_def = NULL;
    for (int c = 0; c < dc->count; c++) {
        ASTNode* fdef = dc->nodes[c];
        if (!fdef) return 0;
        if (!first_def) first_def = fdef;
        /* The result must be a closure value, not a box in a `ptr`. */
        Type* rt = fdef->node_type;
        if (!rt || rt->kind != TYPE_FUNCTION || rt->is_fnptr) return 0;
        for (int a = 0; a < g_owned_ret_active_count; a++) {
            if (g_owned_ret_active[a] == fdef) return 0;
        }
        ASTNode* body = env_scan_fn_body(fdef);
        if (!body) return 0;
        int returns = 0;
        int saved_param = g_returns_param;   /* nested walks have their own */
        g_returns_param = -1;
        g_owned_ret_active[g_owned_ret_active_count++] = fdef;
        int ok = returns_owned_in(gen, fdef, body, body, s->depth + 1, &returns);
        g_owned_ret_active_count--;
        int param = g_returns_param;
        g_returns_param = saved_param;
        if (!ok || returns == 0 || param == -2) return 0;
        if (c == 0) handed = param;
        else if (handed != param) return 0;
    }
    /* A handed-back parameter is the caller's own only when the argument
     * was. */
    if (handed >= 0) return handed_back_arg_is_fresh(gen, s, first_def, call, handed, 0);
    return 1;
}

/* The closure literal local `name` of s->root is bound to: its only binding
 * in the function (none in a nested closure body), else NULL. */
static ASTNode* env_scan_sole_literal(ASTNode* node, const char* name, int* count) {
    if (!node) return NULL;
    ASTNode* found = NULL;
    if (node->type == AST_VARIABLE_DECLARATION && node->value && strcmp(node->value, name) == 0) {
        (*count)++;
        if (node->child_count > 0 && env_scan_is_real_closure(node->children[0])) {
            found = node->children[0];
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        ASTNode* f = env_scan_sole_literal(node->children[i], name, count);
        if (f) found = f;
    }
    return found;
}

/* #2494, #2506: is `call` a call whose result is a closure only the caller
 * holds? A named function under the contract (returns_owned_closure), or a
 * local bound once to a closure literal whose returns all hand over such a
 * closure (`keep = |s| { ...; return leaf }; held = keep(x)`). */
static int call_hands_over_closure(CodeGenerator* gen, EnvScan* s, ASTNode* call) {
    if (!call || call->type != AST_FUNCTION_CALL || !call->value) return 0;
    /* A call through a closure local is lowered to `call(f, ...)` by the
     * time codegen sees it. */
    const char* callee = call->value;
    if (strcmp(callee, "call") == 0) {
        ASTNode* f = call->child_count > 0 ? call->children[0] : NULL;
        if (!f || f->type != AST_IDENTIFIER || !f->value) return 0;
        callee = f->value;
        if (!env_scan_callee_is_variable(gen, s, callee)) return 0;
    } else if (!env_scan_callee_is_variable(gen, s, callee)) {
        return returns_owned_closure(gen, s, callee, call);
    }
    if (s->depth >= ENV_SCAN_MAX_DEPTH || !s->root) return 0;
    /* The result must be a closure value, not a box in a `ptr`. */
    Type* rt = call->node_type;
    if (!rt || rt->kind != TYPE_FUNCTION || rt->is_fnptr) return 0;
    /* A parameter of that name would be the caller's closure until the
     * binding replaced it. */
    if (s->owner) {
        for (int i = 0; i < s->owner->child_count; i++) {
            ASTNode* p = s->owner->children[i];
            if (p && p != s->root && p->type != AST_BLOCK && env_scan_binds(p, callee)) return 0;
        }
    }
    int count = 0;
    ASTNode* lit = env_scan_sole_literal(s->root, callee, &count);
    if (!lit || count != 1) return 0;
    for (int a = 0; a < g_owned_ret_active_count; a++) {
        if (g_owned_ret_active[a] == lit) return 0;
    }
    ASTNode* body = env_scan_fn_body(lit);
    if (!body) return 0;
    int returns = 0;
    g_owned_ret_active[g_owned_ret_active_count++] = lit;
    int saved_param = g_returns_param;   /* nested walks have their own */
    g_returns_param = -1;
    int ok = returns_owned_in(gen, lit, body, body, s->depth + 1, &returns);
    int param = g_returns_param;
    g_returns_param = saved_param;
    g_owned_ret_active_count--;
    if (!ok || returns == 0 || param == -2) return 0;
    /* A handed-back parameter of the closure: the argument follows the
     * callee slot in `call(f, ...)`. */
    if (param >= 0) return handed_back_arg_is_fresh(gen, s, NULL, call, param, 1);
    return 1;
}

/* Does `n` bind `name` anywhere below it (not counting `n` itself)? */
static int env_scan_binds_below(ASTNode* n, const char* name) {
    for (int i = 0; n && i < n->child_count; i++) {
        if (env_scan_binds(n->children[i], name)) return 1;
    }
    return 0;
}

/* The value of s->name is handed on (or replaced by one nobody vouches for)
 * inside statement s->cur_stmt of the scanned function's own body. The
 * local stops owning it: `_envown_<name> = 0` is emitted right before that
 * statement runs, which is sound when the local holds the same value from
 * there to the hand-off. That is so when nothing inside the statement binds
 * the name: a simple statement, a condition or match subject, a loop whose
 * body does not rebind it (#2507), a statement with a trailing block that
 * does not rebind it. A `defer` hands off when it runs, so its deferred
 * statement is the point (each emission of it clears). Anything that binds
 * the name around the hand-off keeps the env for good.
 *
 * A hand-off inside a nested closure body (`g = || { keep(f) }`, #2519)
 * happens in another C function, which cannot reach the flag; there the
 * value is the env's own copy, held through the env's reference, and the
 * hand-off gives the new holder a reference of its own: the body retains
 * the env right before the statement, once per handed-off mention
 * (`mention` is that mention). The local then still owns its reference,
 * and the env its own, so neither frees early. That needs the statement
 * to be the body's own (a closure without a block is the enclosing
 * statement's), and the walk visits each mention once. */
static void env_scan_escape(EnvScan* s, int nested, ASTNode* mention) {
    ASTNode* st = s->cur_stmt;
    if (st && st->type == AST_DEFER_STATEMENT) st = st->child_count > 0 ? st->children[0] : NULL;
    if (s->track_clears && st && !env_scan_binds_below(st, s->name)) {
        if (nested) {
            if (s->cur_stmt_nested && mention && s->retain_count < ENV_SCAN_MAX_CLEARS) {
                s->retain_stmts[s->retain_count] = st;
                s->retain_refs[s->retain_count] = mention;
                s->retain_count++;
                return;
            }
        } else {
            for (int i = 0; i < s->clear_count; i++) {
                if (s->clears[i] == st) return;
            }
            if (s->clear_count < ENV_SCAN_MAX_CLEARS) {
                s->clears[s->clear_count++] = st;
                return;
            }
        }
    }
    s->escapes = 1;
}

/* #2525: is `node` (a mention of the scanned local) the value a store
 * retains for its holder: a struct field store `o.f = name`, a struct
 * literal's or a message's field init, or a write to a global or an actor's
 * state? The holder then has a reference of its own, and the local keeps
 * releasing its own. */
static int env_scan_store_retains(CodeGenerator* gen, ASTNode* parent, ASTNode* node) {
    switch (parent->type) {
        case AST_ASSIGNMENT:
            if (parent->child_count == 1 && parent->children[0] == node && parent->value) {
                return 1;   /* a struct literal's `.f = name` */
            }
            return parent->child_count >= 2 && parent->children[1] == node &&
                   parent->children[0] && parent->children[0]->type == AST_MEMBER_ACCESS;
        case AST_BINARY_EXPRESSION:
            return parent->value && strcmp(parent->value, "=") == 0 &&
                   parent->child_count >= 2 && parent->children[1] == node &&
                   parent->children[0] && parent->children[0]->type == AST_MEMBER_ACCESS;
        case AST_FIELD_INIT:
            return parent->child_count > 0 && parent->children[0] == node;
        case AST_VARIABLE_DECLARATION:
            return parent->child_count > 0 && parent->children[0] == node && parent->value &&
                   (is_module_global_var(gen, parent->value) ||
                    is_actor_state_var(gen, parent->value));
        default:
            return 0;
    }
}

static void env_scan_walk(CodeGenerator* gen, EnvScan* s, ASTNode* node,
                          ASTNode* parent, int nested) {
    if (!node || s->escapes) return;
    const char* name = s->name;
    int named = node->value && strcmp(node->value, name) == 0;
    if (node->type == AST_BLOCK) {
        ASTNode* saved = s->cur_stmt;
        int saved_nested = s->cur_stmt_nested;
        s->cur_stmt_nested = nested;
        for (int i = 0; i < node->child_count && !s->escapes; i++) {
            s->cur_stmt = node->children[i];
            env_scan_walk(gen, s, node->children[i], node, nested);
        }
        s->cur_stmt = saved;
        s->cur_stmt_nested = saved_nested;
        return;
    }
    switch (node->type) {
        case AST_VARIABLE_DECLARATION:
            if (named) {
                ASTNode* rhs = node->child_count > 0 ? node->children[0] : NULL;
                if (s->param_mode) {
                    /* Rebinding the parameter overwrites the caller's copy;
                     * later uses are walked as if they were still it. */
                    env_scan_walk(gen, s, rhs, node, nested);
                    return;
                }
                /* A binding in a nested closure body is that closure's own
                 * local; a tuple slot has no initializer; anything but a fresh
                 * closure may be a value someone else holds. */
                if (nested) {
                    s->escapes = 1;
                    return;
                }
                if (!env_scan_fresh_binding(gen, s, rhs)) {
                    env_scan_escape(s, nested, NULL);
                    env_scan_walk(gen, s, rhs, node, nested);
                    return;
                }
                s->bindings++;
                if (node == s->decl) s->saw_decl = 1;
                int cid = -1;
                if (env_scan_is_real_closure(rhs)) {
                    if (env_scan_capture_count(gen, rhs) > 0) s->heap_env = 1;
                    if (rhs->value) cid = atoi(rhs->value);
                } else {
                    s->heap_env = 1;   /* a call's closure: which one is not known */
                }
                s->cid = (s->cid == -2 || s->cid == cid) ? cid : -1;
                return;
            }
            break;
        case AST_IDENTIFIER:
            if (named) {
                /* #2518: a container that stores the closure takes a reference
                 * of its own, so the store hands nothing over. */
                if (parent && closure_container_store_value(gen, parent) == node) return;
                /* #2525: so does a struct field, a message field, a global
                 * or an actor's state (emit_closure_take retains a view). */
                if (parent && env_scan_store_retains(gen, parent, node)) return;
                if (parent && parent->type == AST_FUNCTION_CALL && parent->value) {
                    if (strcmp(parent->value, "call") == 0) {
                        if (parent->children[0] == node) return;   /* invoked */
                    } else if (!env_scan_param_keeps(gen, s, parent, node)) {
                        return;
                    }
                }
                if (s->allow_return && !nested && parent &&
                    parent->type == AST_RETURN_STATEMENT && parent->child_count == 1) {
                    return;   /* the reference goes to the caller */
                }
                env_scan_escape(s, nested, node);
            }
            return;
        case AST_FUNCTION_CALL: {
            /* `g(...)` invokes it; only the arguments are walked. A
             * container store of the name, bare or through
             * `box_closure(name)`, retains it (#2518): the other arguments
             * are walked, the stored value is no hand-off. */
            ASTNode* cv = closure_container_store_value(gen, node);
            if (cv && cv->type == AST_IDENTIFIER && cv->value && strcmp(cv->value, name) == 0) {
                for (int i = 0; i < node->child_count && !s->escapes; i++) {
                    ASTNode* c = node->children[i];
                    if (!c || c == cv) continue;
                    if (c->type == AST_FUNCTION_CALL && c->value &&
                        strcmp(c->value, "box_closure") == 0 &&
                        c->child_count == 1 && c->children[0] == cv) continue;
                    env_scan_walk(gen, s, c, node, nested);
                }
                return;
            }
            break;
        }
        case AST_CLOSURE:
            if (!env_scan_is_real_closure(node)) break;   /* a trailing block runs inline */
            /* A closure capturing the name takes its own reference to the
             * env (#2494), so wherever the closure goes, the capture is no
             * copy to keep the env for. Its body is walked: what it does
             * with the name must leave no copy either. */
            if (!env_scan_mentions(node, name)) return;
            for (int i = 0; i < node->child_count; i++) {
                env_scan_walk(gen, s, node->children[i], node, 1);
            }
            return;
        case AST_LITERAL:
            return;
        default:
            /* A parameter, pattern, field or other binding spelled the same. */
            if (named) {
                s->escapes = 1;
                return;
            }
            break;
    }
    for (int i = 0; i < node->child_count; i++) {
        env_scan_walk(gen, s, node->children[i], node, nested);
    }
}

/* #2506: is `call` a call whose result is a closure only the caller holds
 * (returns_owned_closure), in the function being generated? */
int call_returns_owned_closure(CodeGenerator* gen, ASTNode* call) {
    if (!call || call->type != AST_FUNCTION_CALL || !call->value ||
        !gen->hoist_scope_body) return 0;
    for (int i = 0; i < call->child_count; i++) {
        if (call->children[i] && call->children[i]->type == AST_CLOSURE &&
            !env_scan_is_real_closure(call->children[i])) return 0;
    }
    EnvScan s;
    memset(&s, 0, sizeof(s));
    s.name = "";
    s.root = gen->hoist_scope_body;
    s.owner = gen->current_function;
    s.cid = -2;
    return call_hands_over_closure(gen, &s, call);
}

/* The env carrier's annotation: "closure_env_free:<cid>:<own>:<name>", see
 * try_emit_closure_env_free. Index on the defer stack of `name`'s, or -1. */
static int closure_env_carrier_index(CodeGenerator* gen, const char* name) {
    const char* prefix = "closure_env_free:";
    size_t plen = strlen(prefix);
    for (int i = gen->defer_count - 1; i >= 0; i--) {
        ASTNode* d = gen->defer_stack[i];
        if (!d || !d->annotation || strncmp(d->annotation, prefix, plen) != 0) continue;
        const char* sep = strchr(d->annotation + plen, ':');
        sep = sep ? strchr(sep + 1, ':') : NULL;
        if (sep && strcmp(sep + 1, name) == 0) return i;
    }
    return -1;
}

/* The closure id the carrier at `idx` frees with, -1 for the generic release. */
static int closure_env_carrier_cid(CodeGenerator* gen, int idx) {
    return atoi(gen->defer_stack[idx]->annotation + strlen("closure_env_free:"));
}

/* Does the local behind the carrier at `idx` track whether it owns its value
 * (`_envown_<name>`, #2506)? */
static int closure_env_carrier_owned_flag(CodeGenerator* gen, int idx) {
    const char* sep = strchr(gen->defer_stack[idx]->annotation + strlen("closure_env_free:"), ':');
    return sep && sep[1] == '1';
}

/* #2506: statements right before which a local stops owning its closure
 * value (`_envown_<name> = 0;`), registered by claim_closure_local_env and
 * emitted by generate_statement. A statement is emitted once, in the
 * function whose scan found it. */
/* A `retain` entry (#2519) is a hand-off inside a nested closure's body:
 * `_aether_closure_env_retain(<mention>.env);` goes before the statement,
 * one per handed-off mention, so the new holder has a reference of its own
 * while the env keeps the one its capture took. The mention is emitted as
 * an expression so it is spelled as the closure body spells the capture. */
typedef struct { ASTNode* stmt; char* name; ASTNode* retain; } EnvOwnClear;
static EnvOwnClear* g_env_own_clears = NULL;
static int g_env_own_clear_count = 0;
static int g_env_own_clear_cap = 0;

static void register_env_own_clear(ASTNode* stmt, const char* name, ASTNode* retain) {
    /* One retain per mention: a scan that meets the same mention again
     * must not add a second reference. */
    for (int i = 0; retain && i < g_env_own_clear_count; i++) {
        if (g_env_own_clears[i].retain == retain) return;
    }
    if (g_env_own_clear_count >= g_env_own_clear_cap) {
        g_env_own_clear_cap = g_env_own_clear_cap ? g_env_own_clear_cap * 2 : 8;
        g_env_own_clears = aether_xrealloc(g_env_own_clears,
                                           g_env_own_clear_cap * sizeof(EnvOwnClear));
    }
    g_env_own_clears[g_env_own_clear_count].stmt = stmt;
    g_env_own_clears[g_env_own_clear_count].name = strdup(name);
    g_env_own_clears[g_env_own_clear_count].retain = retain;
    g_env_own_clear_count++;
}

static void emit_env_own_clears(CodeGenerator* gen, ASTNode* stmt) {
    for (int i = 0; i < g_env_own_clear_count; i++) {
        if (g_env_own_clears[i].stmt != stmt) continue;
        print_indent(gen);
        if (g_env_own_clears[i].retain) {
            fprintf(gen->output, "_aether_closure_env_retain((");
            generate_expression(gen, g_env_own_clears[i].retain);
            fprintf(gen->output, ").env);\n");
        } else {
            fprintf(gen->output, "_envown_%s = 0;\n", g_env_own_clears[i].name);
        }
    }
}

/* #2480: called where the C declaration of local `name` is emitted (its
 * first binding or the hoist that lifts it out of a loop or branch), with
 * `binding`, one of its bindings. Pushes the scope-exit release of its env
 * when the walk proves no copy without a reference outlives the scope; a
 * rebinding then releases the env it replaces (see the reassignment path).
 * A closure that captured the local holds its own reference (#2494), so it
 * may outlive the scope, and the local may be rebound under it. */
static void claim_closure_local_env(CodeGenerator* gen, const char* name,
                                    ASTNode* binding, int at_binding) {
    if (!gen || !name || !binding || gen->scope_depth <= 0 || !gen->hoist_scope_body) return;
    if (binding->type != AST_VARIABLE_DECLARATION || binding->child_count < 1) return;
    ASTNode* rhs = binding->children[0];
    if (!rhs || (!env_scan_is_real_closure(rhs) && rhs->type != AST_FUNCTION_CALL &&
                 !closure_view_binding(rhs) && !closure_ask_binding(rhs))) return;
    /* Only an `_AeClosure` local has an env to release; a closure coerced
     * into a `ptr` slot is a box with an owner of its own. */
    Type* vt = binding->node_type;
    if (vt && vt->kind != TYPE_UNKNOWN && !(vt->kind == TYPE_FUNCTION && !vt->is_fnptr)) return;
    if (rhs->type == AST_FUNCTION_CALL) {
        /* Cheap gate before the walk: only a call typed as a closure (a
         * user function declared to return one, or a closure local) can
         * hand one over. */
        if (!rhs->value || !gen->program) return;
        const char* fn = codegen_normalise_callee(rhs->value);
        const DefClauses* dc = program_index_clauses(gen->program, fn);
        Type* rt = (dc && dc->count > 0 && dc->nodes[0]) ? dc->nodes[0]->node_type
                                                         : rhs->node_type;
        if (!rt || rt->kind != TYPE_FUNCTION || rt->is_fnptr) return;
    }
    if (is_promoted_capture(gen, name) || is_module_global_var(gen, name) ||
        is_actor_state_var(gen, name)) return;
    if (closure_env_carrier_index(gen, name) >= 0) return;
    EnvScan s;
    memset(&s, 0, sizeof(s));
    s.name = name;
    s.root = gen->hoist_scope_body;
    s.owner = gen->current_function;
    s.decl = binding;
    s.cid = -2;
    /* A local a `try` body writes is volatile: a flag beside it would not
     * survive the longjmp, so its hand-offs stay fatal. */
    const char* vq = try_volatile_qual_for(gen, name);
    s.track_clears = !(vq && vq[0]);
    env_scan_walk(gen, &s, s.root, NULL, 0);
    if (s.escapes || !s.saw_decl || !s.heap_env) return;
    int owned_flag = s.clear_count > 0;
    if (owned_flag) {
        /* #2506: the value is handed on at some statements; the local owns
         * what a fresh binding gave it until the next such statement. */
        print_indent(gen);
        fprintf(gen->output, "int _envown_%s = %d; (void)_envown_%s;\n",
                name, at_binding ? 1 : 0, name);
        for (int i = 0; i < s.clear_count; i++) register_env_own_clear(s.clears[i], name, NULL);
    }
    for (int i = 0; i < s.retain_count; i++) {
        register_env_own_clear(s.retain_stmts[i], name, s.retain_refs[i]);   /* #2519 */
    }
    ASTNode* carrier = create_ast_node(AST_EXPRESSION_STATEMENT, NULL,
                                       binding->line, binding->column);
    if (!carrier) return;
    if (carrier->annotation) free(carrier->annotation);
    carrier->annotation = heap_strf("closure_env_free:%d:%d:%s", s.cid < 0 ? -1 : s.cid,
                                    owned_flag, name);
    codegen_own_node(gen, carrier);
    push_defer(gen, carrier);
}

/* ------------------------------------------------------------------
 * Closure-capture cell lifetime (#2019)
 *
 * A variable a closure mutates is promoted to a heap cell (`T* n = ...`)
 * that the closure's env points at. The cell is shared between the
 * declaring scope and every env built from it, and neither side knows
 * how long the other lives: a callback passed to fs.walk is gone when the
 * call returns, a handler handed to a widget or a timer fires long after
 * the scope has ended, and a closure that is returned outlives its whole
 * function.
 *
 * So the cell is reference-counted. The scope holds one reference from
 * declaration to exit; each env takes one when it is built and gives it
 * back in its generated destructor; whoever releases last frees. That
 * makes the scope-exit release below unconditional — there is no escape
 * analysis to get wrong in either direction, no cell freed under a live
 * callback and no cell leaked because a walk could not see through a
 * tuple destructure or an extern that owns the closure.
 *
 * The three places that declare a cell (a first assignment, a tuple
 * destructure slot, a promoted parameter) all come through here so the
 * shape is written once.
 * ------------------------------------------------------------------ */
/* #2474: a fixed-size array's C type spells as `E[N]` (get_c_type). Its
 * length N, with the element's C type in `elem`; 0 when `c_type` is not an
 * array. */
int promoted_cell_array_len(const char* c_type, const char** elem) {
    size_t len = c_type ? strlen(c_type) : 0;
    const char* br = len ? strrchr(c_type, '[') : NULL;
    if (!br || c_type[len - 1] != ']') return 0;
    int count = atoi(br + 1);
    if (count <= 0) return 0;
    *elem = cg_intern_n(c_type, (size_t)(br - c_type));
    return count;
}

/* #2474: the C spelling of a pointer to a promoted cell holding `c_type`,
 * declaring `name`, or the bare type for a cast when `name` is NULL. A cell
 * of a scalar, a string or a struct is `T* name`. An array's `E[N]` is no
 * declarator, so its cell is a pointer to the whole array, `E (*name)[N]`:
 * then `(*name)`, the spelling every use of a promoted name gets (the
 * AST_IDENTIFIER emission), is the array itself, and indexes, decays to a
 * pointer, passes as a slice and has the array's `sizeof`, as the array
 * does. Interned (#2539): a 600-byte buffer cut a long type or name. */
const char* promoted_cell_pointer(const char* c_type, const char* name) {
    const char* elem;
    int count = promoted_cell_array_len(c_type, &elem);
    if (count > 0) return cg_internf("%s (*%s)[%d]", elem, name ? name : "", count);
    return cg_internf("%s*%s%s", c_type, name ? " " : "", name ? name : "");
}

/* The value a string cell, or a string element of an array cell (#2474),
 * takes. The cell owns what it holds (its last release frees it, and a
 * store frees the value it replaces), so it takes a buffer of its own
 * (emit_string_take_owned: a fresh value adopted, a heap-tracked local
 * moved or copied, a view or a borrow copied). A borrowed string stored as
 * it stood was released by the cell as well as by its owner: a parameter
 * a closure captured was released by the closure's env and by the cell
 * (#2514). A literal is stored as it is: the cell frees only refcounted
 * strings. */
static void emit_cell_element_value(CodeGenerator* gen, int str_elem, ASTNode* v) {
    if (str_elem && !(v->type == AST_LITERAL && v->node_type &&
                      v->node_type->kind == TYPE_STRING)) {
        emit_string_take_owned(gen, v, 1);
    } else {
        generate_expression(gen, v);
    }
}

/* `name = value` into the cell of the promoted capture `name`, of type `t`:
 * the one shape every store into a cell takes (a reassignment, a match
 * arm). A string cell owns its string and a struct cell its struct's
 * strings (#2458), so each takes a value of its own and gives up the one
 * it held (#2514). A whole fixed-size array is stored by
 * emit_cell_array_store. */
static void emit_cell_store(CodeGenerator* gen, const char* name, Type* t, ASTNode* value) {
    const char* ct = t ? get_c_type(t) : NULL;
    const char* sname = struct_owning_strings(gen, t);
    if (ct && strcmp(ct, "const char*") == 0) {
        fprintf(gen->output, "_aether_str_cell_set(%s, ", name);
        emit_cell_element_value(gen, 1, value);
        fprintf(gen->output, ");\n");
    } else if (sname) {
        /* #2497: a struct owned elsewhere is copied in. */
        fprintf(gen->output, "%s_replace(%s, ", sname, name);
        emit_struct_take(gen, value, sname, NULL);
        fprintf(gen->output, ");\n");
    } else {
        fprintf(gen->output, "*%s = ", name);
        generate_expression(gen, value);
        fprintf(gen->output, ";\n");
    }
}

/* #2474: is `e` an element of a promoted string array (`arr[i]` where a
 * closure writes `arr`)? The cell owns the element and frees it when it is
 * replaced, so a slot that keeps it takes a copy, as it does a struct's
 * string field (is_owned_string_field_read). */
static int is_cell_string_element(CodeGenerator* gen, ASTNode* e) {
    if (!e || e->type != AST_ARRAY_ACCESS || e->child_count < 2) return 0;
    ASTNode* base = e->children[0];
    if (!base || base->type != AST_IDENTIFIER || !base->value ||
        !is_promoted_capture(gen, base->value)) return 0;
    Type* t = base->node_type;
    return t && type_is_sized_array(t) && t->element_type &&
           t->element_type->kind == TYPE_STRING;
}

/* #2474: `arr[i] = v` into a promoted string array. The cell owns its
 * elements, so the store frees the one it replaces and takes a buffer of
 * its own, as a store into a string cell does. */
static int emit_cell_string_element_store(CodeGenerator* gen, ASTNode* lhs, ASTNode* rhs) {
    if (!rhs || !is_cell_string_element(gen, lhs)) return 0;
    fprintf(gen->output, "_aether_str_cell_set(&");
    generate_expression(gen, lhs);
    fprintf(gen->output, ", ");
    emit_cell_element_value(gen, 1, rhs);
    fprintf(gen->output, ");\n");
    return 1;
}

/* #2474: `arr = [...]` where `arr` is a promoted fixed-size array. An array
 * does not assign, so the literal's elements are stored into the cell and
 * the ones past the literal's zeroed, as #2289 does for a plain array. The
 * literal is evaluated whole, in order, into a temporary first, so an
 * element that reads the array (`arr = [arr[1], arr[0]]`) reads the value
 * from before the store. A string array's cell frees each element it
 * replaces. Returns 0 for any other shape. */
static int emit_cell_array_store(CodeGenerator* gen, ASTNode* stmt) {
    if (!stmt || stmt->child_count < 1 || !stmt->value) return 0;
    Type* at = declared_var_type(gen, stmt->value);
    if (!at || !type_is_sized_array(at) || at->array_size <= 0) return 0;
    ASTNode* lit = stmt->children[0];
    if (lit && lit->type == AST_SLICE_FROM_ARRAY && lit->child_count > 0)
        lit = lit->children[0];
    if (lit && lit->type != AST_ARRAY_LITERAL && is_sized_array_param(lit->node_type)) {
        /* #2516: another array's elements, copied in. A string array's
         * cell takes a copy of each, as it does a stored element. */
        const char* elem = get_c_type(at->element_type);
        if (strcmp(elem, "const char*") == 0) {
            fprintf(gen->output, "{ const char* const* _ae_src = (const char* const*)(");
            generate_expression(gen, lit);
            fprintf(gen->output,
                    "); for (int _ae_k = 0; _ae_k < %d; _ae_k++) _aether_str_cell_set(&(*%s)[_ae_k], aether_uniform_heap_str(_ae_src[_ae_k], 0)); }\n",
                    at->array_size, stmt->value);
        } else {
            fprintf(gen->output, "memmove(*%s, ", stmt->value);
            generate_expression(gen, lit);
            fprintf(gen->output, ", sizeof(*%s));\n", stmt->value);
        }
        return 1;
    }
    if (!lit || lit->type != AST_ARRAY_LITERAL) return 0;
    if (lit->child_count > at->array_size) {
        char msg[300];
        snprintf(msg, sizeof(msg),
                 "array literal of %d elements assigned to '%s', which holds %d: an array keeps the size of its first binding",
                 lit->child_count, stmt->value, at->array_size);
        AetherError e = { stmt->source_file, NULL, stmt->line, stmt->column, msg,
                          NULL, NULL, AETHER_ERR_TYPE_MISMATCH };
        aether_error_report(&e);
        return 1;
    }
    const char* elem = get_c_type(at->element_type);
    int str_elem = strcmp(elem, "const char*") == 0;
    fprintf(gen->output, "{ %s _ae_cell_arr[%d] = {0};", elem, at->array_size);
    for (int k = 0; k < lit->child_count; k++) {
        fprintf(gen->output, " _ae_cell_arr[%d] = ", k);
        emit_cell_element_value(gen, str_elem, lit->children[k]);
        fprintf(gen->output, ";");
    }
    if (str_elem) {
        fprintf(gen->output,
                " for (int _ae_k = 0; _ae_k < %d; _ae_k++) _aether_str_cell_set(&(*%s)[_ae_k], _ae_cell_arr[_ae_k]); }\n",
                at->array_size, stmt->value);
    } else {
        fprintf(gen->output, " memcpy(*%s, _ae_cell_arr, sizeof(_ae_cell_arr)); }\n",
                stmt->value);
    }
    return 1;
}

/* #2516: a fixed-size array parameter (`xs: int[3]`). A fixed-size array
 * is a value, as a struct is: binding one (`a = b`, or passing it) copies
 * its elements. C passes an array as a pointer to its first element, so the
 * signature takes `E _param_xs[N]` and the body starts by copying the
 * caller's elements into an array of its own, `E xs[N]`. A parameter a
 * closure writes is seeded into its cell instead (emit_promoted_param_cell).
 * The signature used to spell `int[3] xs`, which is not a C declarator. */
int is_sized_array_param(Type* t) {
    return type_is_sized_array(t) && t->array_size > 0;
}

void emit_sized_array_param_declarator(CodeGenerator* gen, Type* t, const char* name) {
    fprintf(gen->output, "%s _param_%s[%d]", get_c_type(t->element_type), name, t->array_size);
}

void emit_sized_array_param_copy(CodeGenerator* gen, Type* t, const char* name) {
    fprintf(gen->output, "%s %s[%d]; memcpy(%s, _param_%s, sizeof(%s));\n",
            get_c_type(t->element_type), name, t->array_size, name, name, name);
}

/* The function that gives back one reference to a promoted cell of C type
 * `c_type`; the last holder frees the cell. A string cell owns its heap
 * string and a struct cell the struct's owned string fields, so those free
 * the value first (`<Name>_cell_release`, generate_struct_definition); an
 * int / ptr cell is freed as it is. The scope exit and each closure env's
 * destructor both release through here, so they agree. */
const char* promoted_cell_release_fn(CodeGenerator* gen, const char* c_type) {
    if (c_type && strcmp(c_type, "const char*") == 0) return "_aether_cell_release_str";
    /* #2474: a string array cell owns every string element, as a string
     * cell owns its one (the macro reads the length off the cell's type). */
    const char* elem;
    if (promoted_cell_array_len(c_type, &elem) > 0) {
        return strcmp(elem, "const char*") == 0 ? "_aether_cell_release_strs"
                                                : "_aether_cell_release";
    }
    ASTNode* sdef = (c_type && gen->program && !aether_is_c_import_struct(c_type))
                        ? find_struct_definition_by_name(gen->program, c_type)
                        : NULL;
    if (sdef && struct_owns_heap_strings(gen, sdef)) return cg_internf("%s_cell_release", c_type);
    return "_aether_cell_release";
}

void emit_promoted_cell_declaration(CodeGenerator* gen, const char* name,
                                    const char* c_type, Type* var_type,
                                    ASTNode* init_expr, const char* init_text,
                                    int line, int column) {
    if (!c_type || c_type[0] == 0) c_type = "int";
    /* Interned: the caller's spelling may be a buffer the initialiser's own
     * emission below reuses before the release reads the type again. */
    c_type = cg_intern(c_type);
    const char* cell_decl = promoted_cell_pointer(c_type, name);
    const char* cell_type = promoted_cell_pointer(c_type, NULL);
    const char* elem = "";
    fprintf(gen->output, "%s = (%s)_aether_cell_new(sizeof(%s));",
            cell_decl, cell_type, c_type);
    int arr_len = promoted_cell_array_len(c_type, &elem);
    /* No initialiser is the hoisted shape (#2024): the cell is declared
     * ahead of the loop or branch that first assigns it, and the
     * allocation is zero-filled, so the value is defined until then. */
    if (arr_len > 0) {
        /* #2474: an array does not assign. The cell starts zeroed, as the
         * plain array's `= {0}` does, and an array literal's elements are
         * stored into it in order. */
        ASTNode* lit = init_expr;
        if (lit && lit->type == AST_SLICE_FROM_ARRAY && lit->child_count > 0)
            lit = lit->children[0];
        if (!init_expr && init_text) {
            /* #2516: seeded from an array (a parameter's elements). A
             * string cell owns its elements, so it takes a reference to
             * each, as a string parameter's cell does. */
            if (strcmp(elem, "const char*") == 0)
                fprintf(gen->output, " for (int _ae_k = 0; _ae_k < %d; _ae_k++) (*%s)[_ae_k] = aether_str_capture((%s)[_ae_k]);",
                        arr_len, name, init_text);
            else
                fprintf(gen->output, " memcpy(*%s, %s, sizeof(*%s));", name, init_text, name);
        } else if (lit && lit->type == AST_ARRAY_LITERAL && lit->child_count > arr_len) {
            char msg[300];
            snprintf(msg, sizeof(msg),
                     "array literal of %d elements initialises '%s', which holds %d",
                     lit->child_count, name, arr_len);
            AetherError e = { lit->source_file, NULL, line, column, msg,
                              NULL, NULL, AETHER_ERR_TYPE_MISMATCH };
            aether_error_report(&e);
        } else if (lit && lit->type == AST_ARRAY_LITERAL) {
            int str_elem = strcmp(elem, "const char*") == 0;
            for (int k = 0; k < lit->child_count; k++) {
                fprintf(gen->output, " (*%s)[%d] = ", name, k);
                emit_cell_element_value(gen, str_elem, lit->children[k]);
                fprintf(gen->output, ";");
            }
        }
    } else if (init_expr || init_text) {
        fprintf(gen->output, " *%s = ", name);
        if (init_expr && strcmp(c_type, "const char*") == 0) {
            /* #2461, #2514: a string cell frees what it holds. */
            emit_cell_element_value(gen, 1, init_expr);
        } else if (init_expr) {
            generate_expression(gen, init_expr);
        } else {
            fprintf(gen->output, "%s", init_text);
        }
        fprintf(gen->output, ";");
    }
    fprintf(gen->output, "\n");
    /* #2474: an array cell records its type, which a later whole-array
     * store reads its length and element type from (as #2289's does). */
    if (arr_len > 0 && var_type && type_is_sized_array(var_type))
        mark_var_declared_typed(gen, name, var_type);
    else
        mark_var_declared(gen, name);
    const char* release_fn = promoted_cell_release_fn(gen, c_type);
    ASTNode* release_call = create_ast_node(AST_FUNCTION_CALL, release_fn,
                                            line, column);
    ASTNode* arg = create_ast_node(AST_IDENTIFIER, name, line, column);
    /* The release takes the cell pointer itself, not `*name`: the
     * annotation tells the AST_IDENTIFIER emission not to dereference. */
    if (arg->annotation) free(arg->annotation);
    arg->annotation = strdup("raw_promoted");
    add_child(release_call, arg);
    ASTNode* expr_stmt = create_ast_node(AST_EXPRESSION_STATEMENT, NULL, line, column);
    add_child(expr_stmt, release_call);
    /* Built here, never attached to the program AST: owned by the generator
     * so free_code_generator reaches it (#1667). */
    codegen_own_node(gen, expr_stmt);
    push_defer(gen, expr_stmt);
}

/* A string cell owns the refcounted string it holds: overwriting it or the
 * last release gives that reference back (_aether_str_cell_set /
 * _aether_cell_release_str). A parameter's string is borrowed from the
 * caller, so seeding the cell with it as it stands made the first write in
 * a closure, or the scope exit, free the caller's string (#2463). The cell
 * takes a reference of its own. */
void emit_promoted_param_cell(CodeGenerator* gen, const char* name,
                              const char* c_type, const char* param_cname,
                              int line, int column) {
    const char* init = param_cname;
    if (c_type && strcmp(c_type, "const char*") == 0) {
        /* aether_str_capture: a refcounted string is retained and a plain
         * buffer copied, so the cell never holds the caller's pointer
         * (#2499 relies on it: a caller may free a plain heap argument
         * after a closure call). */
        init = cg_internf("aether_str_capture(%s)", param_cname);
    }
    emit_promoted_cell_declaration(gen, name, c_type, NULL, NULL, init, line, column);
}

/* Push function-exit defer-free statements for every hoisted
 * heap-string variable that's not escaped (issue #420 follow-up).
 *
 * The wrapper at codegen_stmt.c (single-value path) and at the
 * AST_TUPLE_DESTRUCTURE handler (tuple path) frees the PREVIOUS
 * value on every reassignment — but a variable that's assigned
 * once and never reassigned still has a live heap allocation
 * when the function exits. Without a function-exit free, that
 * final allocation leaks per-call.
 *
 * This pre-pass runs AFTER mark_escaped_heap_string_vars (so
 * we know which vars escape via call-args, closure-capture, or
 * return-statement) and BEFORE body codegen. For every hoisted
 * heap-string var that is NOT escaped, push a synthetic defer
 * onto the function-scope defer stack. The defer carries an
 * annotation `"heap_string_exit_free:<name>"` so emit_defers_*
 * can recognise it and emit the wrapped form:
 *
 *     if (_heap_<name>) { free((void*)<name>); <name> = NULL; _heap_<name> = 0; }
 *
 * (the `<name> = NULL; _heap_<name> = 0;` after the free is
 * defensive — a defer can fire from emit_all_defers at a
 * return site, after which control returns to the caller; the
 * function-local C var is dead either way, but resetting keeps
 * a re-emitted scope-exit free idempotent if some other path
 * also fires.)
 *
 * Escape gate: if a var is marked escaped, we skip the defer.
 * The escape walker handles three cases:
 *   - return <name>;     — the value flows out, caller owns it
 *   - f(name) where f's matching param is `ptr` — likely stored
 *   - closure body captures the name — closure may outlive scope
 * In each case the function should NOT free; mark_escaped already
 * skips the on-reassignment free, and we mirror that here.
 *
 * Cost: one AST node + one defer-stack slot per non-escaped
 * heap-string var per function. Emission cost: one inline
 * conditional + free per defer at function exit / each return.
 * For functions that don't allocate heap-strings, no defers are
 * pushed; cost is zero. */
void push_heap_string_exit_free_defers(CodeGenerator* gen, ASTNode* body) {
    if (!gen || !body) return;
    if (gen->heap_string_var_count <= 0) return;
    for (int i = 0; i < gen->heap_string_var_count; i++) {
        const char* name = gen->heap_string_vars[i];
        if (!name) continue;
        /* Suppress defer-free for BOTH escape channels. Container-
         * escape (call-arg, struct-field, closure capture): recipient
         * may have stashed the pointer — freeing would dangle their
         * copy. Return-escape: the function's return value is the
         * variable's buffer — freeing here would dangle the caller's
         * pointer. The reassign-wrapper-free is still active for
         * return-escape vars (the in-loop accumulator pattern), so
         * intermediate buffers ARE reclaimed; only the final one
         * survives to the return. */
        if (is_escaped_string_var(gen, name)) continue;
        if (is_return_escaped_string_var(gen, name)) continue;
        /* Skip closure-env vars and promoted captures — they have
         * their own defer-free shapes via the existing closure /
         * promoted-cell paths (see claim_closure_local_env and
         * emit_promoted_cell_declaration below). Adding a heap-
         * string-exit defer on top would emit a free on a name
         * that doesn't live as a `const char*` at function
         * scope. */
        int is_env_cap = 0;
        for (int e = 0; e < gen->current_env_capture_count; e++) {
            if (gen->current_env_captures[e] &&
                strcmp(gen->current_env_captures[e], name) == 0) {
                is_env_cap = 1; break;
            }
        }
        if (is_env_cap) continue;
        if (is_promoted_capture(gen, name)) continue;
        /* Build the defer carrier: an AST_EXPRESSION_STATEMENT
         * whose annotation encodes `heap_string_exit_free:<name>`.
         * The body is empty — emit_defers_for_scope and
         * emit_all_defers pick the annotation up and
         * emit the conditional-free directly without descending
         * into the body. */
        ASTNode* carrier = create_ast_node(AST_EXPRESSION_STATEMENT, NULL,
                                            body->line, body->column);
        if (carrier) {
            if (carrier->annotation) free(carrier->annotation);
            carrier->annotation = heap_strf("heap_string_exit_free:%s", name);
            codegen_own_node(gen, carrier);
            push_defer(gen, carrier);
        }
        /* #2499: a parameter captured on entry owns its copy from the first
         * statement on. Journal it now that its exit free is armed, as a
         * local is journaled where it is assigned (emit_unwind_track_local),
         * or a panic unwinding through the body leaks the copy. No body
         * statement has been emitted yet. */
        if (is_captured_string_param(gen, name)) {
            print_indent(gen);
            fprintf(gen->output, "aether_unwind_track_str_if(%s, _heap_%s);\n", name, name);
        }
    }
}

/* Escape pre-pass for *StringSeq locals. A seq var escapes (and so its
 * scope-exit free is suppressed) only when ownership leaves the
 * function: it is `return`ed, captured by a closure, or passed to a
 * NON-seq function that may store it raw (list.add, map.put, an actor
 * message field, a struct constructor). Passing a seq to any
 * `string_seq_*` op is NOT an escape — those retain (cons, concat,
 * retain) or read (length, head, is_empty) or consume-and-clear
 * (seq_free, handled by the explicit-free flag-clear), so the caller
 * keeps its own ref. The RHS-of-own-assignment exception (`deep =
 * cons(x, deep)`) keeps the accumulator pattern freeable. */
static void seq_escape_walk(CodeGenerator* gen, ASTNode* node,
                            const char* consumed_lhs) {
    if (!node) return;
    if (node->type == AST_RETURN_STATEMENT) {
        for (int i = 0; i < node->child_count; i++) {
            ASTNode* c = node->children[i];
            if (c && c->type == AST_IDENTIFIER && c->value &&
                is_seq_var(gen, c->value)) {
                mark_escaped_seq_var(gen, c->value);
            } else {
                seq_escape_walk(gen, c, consumed_lhs);
            }
        }
        return;
    }
    if (node->type == AST_VARIABLE_DECLARATION && node->value &&
        node->child_count > 0) {
        for (int i = 0; i < node->child_count; i++)
            seq_escape_walk(gen, node->children[i], node->value);
        return;
    }
    if (node->type == AST_CLOSURE) {
        for (int i = 0; i < node->child_count; i++)
            seq_escape_walk(gen, node->children[i], NULL);
        return;
    }
    if (node->type == AST_FUNCTION_CALL && node->value) {
        const char* fn = codegen_normalise_callee(node->value);
        /* `string.join(seq, sep)` normalises to `string_join`, not the
         * `string_seq_` prefix, but it is a pure read of the spine like
         * every other seq op — without it here, a `s = seq_cons(x, s)`
         * accumulator that is later joined is marked escaped and every
         * intermediate spine ref leaks. */
        int callee_is_seq_op = fn && (strncmp(fn, "string_seq_", 11) == 0 ||
                                      strcmp(fn, "string_join") == 0);
        for (int i = 0; i < node->child_count; i++) {
            ASTNode* a = node->children[i];
            if (a && a->type == AST_IDENTIFIER && a->value &&
                is_seq_var(gen, a->value)) {
                if (consumed_lhs && strcmp(a->value, consumed_lhs) == 0) continue;
                if (callee_is_seq_op) continue;
                mark_escaped_seq_var(gen, a->value);
            } else {
                seq_escape_walk(gen, a, consumed_lhs);
            }
        }
        return;
    }
    if (node->type == AST_FUNCTION_DEFINITION ||
        node->type == AST_BUILDER_FUNCTION) {
        return;
    }
    for (int i = 0; i < node->child_count; i++)
        seq_escape_walk(gen, node->children[i], consumed_lhs);
}

void mark_escaped_seq_vars(CodeGenerator* gen, ASTNode* body) {
    if (!gen || !body) return;
    seq_escape_walk(gen, body, NULL);
}

/* Scope-exit defer-free for non-escaped *StringSeq locals (parallel to
 * push_heap_string_exit_free_defers). The carrier annotation
 * `seq_exit_free:<name>` is recognised by try_emit_seq_exit_free. */
void push_seq_exit_free_defers(CodeGenerator* gen, ASTNode* body) {
    if (!gen || !body) return;
    if (gen->seq_var_count <= 0) return;
    for (int i = 0; i < gen->seq_var_count; i++) {
        const char* name = gen->seq_vars[i];
        if (!name) continue;
        if (is_escaped_seq_var(gen, name)) continue;
        int is_env_cap = 0;
        for (int e = 0; e < gen->current_env_capture_count; e++) {
            if (gen->current_env_captures[e] &&
                strcmp(gen->current_env_captures[e], name) == 0) {
                is_env_cap = 1; break;
            }
        }
        if (is_env_cap) continue;
        if (is_promoted_capture(gen, name)) continue;
        ASTNode* carrier = create_ast_node(AST_EXPRESSION_STATEMENT, NULL,
                                            body->line, body->column);
        if (carrier) {
            if (carrier->annotation) free(carrier->annotation);
            carrier->annotation = heap_strf("seq_exit_free:%s", name);
            codegen_own_node(gen, carrier);
            push_defer(gen, carrier);
        }
    }
}

/* Escape pre-pass for `string?` locals. Unlike the two-channel
 * heap-string analysis, a `string?` uses a SINGLE escape set: any
 * ownership departure — `return`, capture by a closure, a raw store
 * into a struct field / array element, or being passed to any function
 * (which may stash the `.val` pointer) — suppresses the scope-exit
 * free entirely. This is conservative (an in-loop-reassigned optional
 * that also escapes keeps its intermediate buffers to scope exit
 * rather than freeing them at each reassign), but it can never
 * double-free a buffer the recipient still holds. The RHS-of-own-
 * assignment exception (`o = f(o)`) keeps a self-referential rebind
 * from marking itself escaped. */
static void opt_str_escape_walk(CodeGenerator* gen, ASTNode* node,
                                const char* consumed_lhs) {
    if (!node) return;
    if (node->type == AST_RETURN_STATEMENT) {
        for (int i = 0; i < node->child_count; i++) {
            ASTNode* c = node->children[i];
            if (c && c->type == AST_IDENTIFIER && c->value &&
                is_opt_str_var(gen, c->value)) {
                mark_escaped_opt_str_var(gen, c->value);
            } else {
                opt_str_escape_walk(gen, c, consumed_lhs);
            }
        }
        return;
    }
    if (node->type == AST_VARIABLE_DECLARATION && node->value &&
        node->child_count > 0) {
        for (int i = 0; i < node->child_count; i++)
            opt_str_escape_walk(gen, node->children[i], node->value);
        return;
    }
    /* Non-trivial store `s.field = opt` / `arr[i] = opt`: the write
     * target outlives this activation, so a `string?` on the RHS
     * escapes. */
    if (node->type == AST_BINARY_EXPRESSION && node->value &&
        strcmp(node->value, "=") == 0 && node->child_count == 2) {
        ASTNode* lhs = node->children[0];
        ASTNode* rhs = node->children[1];
        if (lhs && lhs->type != AST_IDENTIFIER) {
            if (rhs && rhs->type == AST_IDENTIFIER && rhs->value &&
                is_opt_str_var(gen, rhs->value)) {
                mark_escaped_opt_str_var(gen, rhs->value);
            }
            opt_str_escape_walk(gen, lhs, NULL);
            opt_str_escape_walk(gen, rhs, NULL);
            return;
        }
    }
    if (node->type == AST_CLOSURE) {
        for (int i = 0; i < node->child_count; i++)
            opt_str_escape_walk(gen, node->children[i], NULL);
        return;
    }
    if (node->type == AST_FUNCTION_CALL) {
        for (int i = 0; i < node->child_count; i++) {
            ASTNode* a = node->children[i];
            if (a && a->type == AST_IDENTIFIER && a->value &&
                is_opt_str_var(gen, a->value)) {
                if (consumed_lhs && strcmp(a->value, consumed_lhs) == 0) continue;
                /* Same parameter-aware precision as the heap-string walk:
                 * a `string?` passed by value to a read-only callee (its
                 * param only read, like `sink(x: string?)`) does NOT
                 * escape — the recipient sees a copy of the `{has,val}`
                 * struct and, unless it stores `.val`, our exit-free may
                 * still reclaim it. Only a genuine storing sink escapes. */
                if (call_arg_position_escapes(gen, node, i)) {
                    mark_escaped_opt_str_var(gen, a->value);
                }
            } else {
                opt_str_escape_walk(gen, a, consumed_lhs);
            }
        }
        return;
    }
    if (node->type == AST_FUNCTION_DEFINITION ||
        node->type == AST_BUILDER_FUNCTION) {
        return;
    }
    for (int i = 0; i < node->child_count; i++)
        opt_str_escape_walk(gen, node->children[i], consumed_lhs);
}

void mark_escaped_opt_str_vars(CodeGenerator* gen, ASTNode* body) {
    if (!gen || !body) return;
    opt_str_escape_walk(gen, body, NULL);
}

/* Scope-exit defer-free for non-escaped `string?` locals (parallel to
 * push_seq_exit_free_defers). The carrier annotation
 * `opt_str_exit_free:<name>` is recognised by try_emit_opt_str_exit_free. */
void push_opt_str_exit_free_defers(CodeGenerator* gen, ASTNode* body) {
    if (!gen || !body) return;
    if (gen->opt_str_var_count <= 0) return;
    for (int i = 0; i < gen->opt_str_var_count; i++) {
        const char* name = gen->opt_str_vars[i];
        if (!name) continue;
        if (is_escaped_opt_str_var(gen, name)) continue;
        int is_env_cap = 0;
        for (int e = 0; e < gen->current_env_capture_count; e++) {
            if (gen->current_env_captures[e] &&
                strcmp(gen->current_env_captures[e], name) == 0) {
                is_env_cap = 1; break;
            }
        }
        if (is_env_cap) continue;
        if (is_promoted_capture(gen, name)) continue;
        ASTNode* carrier = create_ast_node(AST_EXPRESSION_STATEMENT, NULL,
                                            body->line, body->column);
        if (carrier) {
            if (carrier->annotation) free(carrier->annotation);
            carrier->annotation = heap_strf("opt_str_exit_free:%s", name);
            codegen_own_node(gen, carrier);
            push_defer(gen, carrier);
        }
    }
}

// Collect the names of top-level AST_VARIABLE_DECLARATION nodes in a
// block. Used by the if/else hoist below to find variables that are
// first-assigned in BOTH branches — those need to be visible after the
// `if`, so we declare them at the outer scope before opening the if.
//
// Pulls only direct children (not nested blocks) since a name introduced
// inside a deeper `while` of the then-branch should NOT escape to the
// post-if scope.
// When both arms of an if/else first-assign the same variable name,
// hoist a single declaration to the enclosing scope so the post-block
// code can read it. Without this, both arms emit a C-local declaration
// and the variable goes out of scope at the closing `}`. See
// docs/notes/compiler_notes_from_vcr_port.md item #2 for the original
// repro and rationale.
//
// Names that appear in only one arm are deliberately NOT hoisted —
// using such a name after the if would be undefined behavior at the
// Aether level, and the existing scope-restore in AST_IF_STATEMENT
// keeps that locality. Names already declared before the if are also
// skipped (they're already in scope).
/* Emit a hoisted C local declaration `<type> <name>;` that is correct
 * for ARRAY types too. get_c_type(TYPE_ARRAY) returns "T[N]", which is
 * a valid type spelling only in the postfix-declarator position
 * (`T name[N]`), NOT as a prefix (`T[N] name;` is invalid C). The
 * loop/branch var hoisters below pre-declare a var as `<c_type> <name>;`
 * — fine for scalars/pointers, but for a `byte[8]`-style fixed array it
 * produced `unsigned char[8] name;`. Split the array case out so it
 * emits `elem name[N];`. (Hit repeatedly by the Redis port's per-loop
 * scratch buffers; the first-statement-in-block decl path already does
 * this correctly, this fixes the not-first / hoisted path.) */
/* The zero initializer for a hoisted local. A hoisted variable is
 * declared at function scope and assigned inside the branch or loop
 * body that binds it; on a path that skips the body its value is
 * indeterminate, and reading an indeterminate value is undefined — not
 * a garbage value, undefined: gcc's interprocedural constant
 * propagation may treat it as whatever the other call sites pass (a
 * literal 0), so a parameter that "genuinely varies" arrives as a
 * constant, with no sanitizer able to say so (#2128 is the shape of that
 * report). Initialized, the variable is 0/NULL/{0} on such a path and
 * every read is defined. Spelled per kind rather than as a universal
 * `{0}`, which clang flags on scalars (-Wbraced-scalar-init). */
static const char* hoisted_zero_init(Type* t, const char* c_type) {
    if (t) {
        switch (t->kind) {
            case TYPE_INT: case TYPE_INT64: case TYPE_UINT64: case TYPE_UINT32:
            case TYPE_UINT16: case TYPE_UINT8: case TYPE_BYTE: case TYPE_BOOL:
            case TYPE_FLOAT: case TYPE_FLOAT32: case TYPE_LONGDOUBLE:
            case TYPE_DURATION: case TYPE_ENUM:
            case TYPE_BITSET: case TYPE_BITSTRUCT:   /* backing integers */
                return " = 0";
            case TYPE_ISOLATED:
                return hoisted_zero_init(t->element_type, c_type);
            case TYPE_F32X4: case TYPE_F64X2: case TYPE_I32X4: case TYPE_I64X2:
            case TYPE_I16X8: case TYPE_F32X8: case TYPE_I32X8:
                return " = {0}";   /* #2146: a vector, not a scalar 0 */
            case TYPE_PTR: case TYPE_STRING: case TYPE_ACTOR_REF:
                return " = NULL";
            case TYPE_FUNCTION:
                return t->is_fnptr ? " = NULL" : " = {0}";
            default:
                break;
        }
    }
    size_t n = c_type ? strlen(c_type) : 0;
    if (n && c_type[n - 1] == '*') return " = NULL";
    return " = {0}";
}

/* #2124: a local first bound inside a branch or loop body is hoisted to
 * function scope, one variable for every binding of the name in the
 * function. Its declared type is the numeric join of those bindings
 * (`int` here, `long` there: `int64_t`), so none of them narrows and the
 * type matches what the early inference pass records for a use after
 * the branches. Non-numeric kinds keep `seed` (a clash is reported at
 * the binding). Nested closures are their own functions and are not
 * walked. Returns a fresh Type, or NULL when nothing widened `seed`. */
static Type* sibling_join_type(CodeGenerator* gen, const char* name, Type* seed) {
    if (!gen->hoist_scope_body) return NULL;
    return hoist_join_type(gen->hoist_scope_body, name, seed);
}

/* #2124: the kinds a number local holds; a pointer cannot flow into one
 * (nor one into a pointer) whatever is_type_compatible says for the C
 * interop cases. */
static int rebind_is_number(TypeKind k) {
    return k == TYPE_INT || k == TYPE_INT64 || k == TYPE_UINT64 || k == TYPE_UINT32 ||
           k == TYPE_UINT16 || k == TYPE_UINT8 || k == TYPE_BYTE || k == TYPE_BOOL ||
           k == TYPE_FLOAT || k == TYPE_FLOAT32 || k == TYPE_LONGDOUBLE;
}

static void emit_hoisted_local_decl(CodeGenerator* gen, Type* var_type,
                                     const char* name) {
    if (var_type && var_type->kind == TYPE_ARRAY && var_type->array_size > 0) {
        const char* elem = get_c_type(var_type->element_type);
        fprintf(gen->output, "%s %s[%d] = {0};\n", elem, name, var_type->array_size);
        return;
    }
    const char* c_type = get_c_type(var_type);
    fprintf(gen->output, "%s %s%s;\n", c_type, name, hoisted_zero_init(var_type, c_type));
}

static void hoist_if_else_common_vars(CodeGenerator* gen,
                                       ASTNode* then_body,
                                       ASTNode* else_body) {
    if (!then_body || !else_body) return;
    const char* then_names[HOIST_MAX_NAMES];
    int then_count = hoist_direct_decl_names(then_body, then_names, 0, HOIST_MAX_NAMES);
    const char* else_names[HOIST_MAX_NAMES];
    int else_count = hoist_direct_decl_names(else_body, else_names, 0, HOIST_MAX_NAMES);

    for (int i = 0; i < then_count; i++) {
        const char* n = then_names[i];
        // Must appear in else_names too.
        int in_else = 0;
        for (int j = 0; j < else_count; j++) {
            if (strcmp(n, else_names[j]) == 0) { in_else = 1; break; }
        }
        if (!in_else) continue;

        // Skip if already declared at outer scope.
        if (is_var_declared(gen, n)) continue;
        /* #744: never shadow a module-level `var` global with a hoisted
         * local — the write is routed to the file-scope static by the
         * variable-declaration emitter. */
        if (is_module_global_var(gen, n)) continue;
        /* #2505: nor an actor's state field, which `n` writes in a handler. */
        if (is_actor_state_var(gen, n)) continue;

        // Recover a usable type from either branch's initializer.
        ASTNode* decl = hoist_find_decl(then_body, n);
        Type* var_type = decl ? decl->node_type : NULL;
        if ((!var_type || var_type->kind == TYPE_VOID
             || var_type->kind == TYPE_UNKNOWN)
            && decl && decl->child_count > 0
            && decl->children[0] && decl->children[0]->node_type) {
            var_type = decl->children[0]->node_type;
        }
        if (!var_type || var_type->kind == TYPE_VOID
            || var_type->kind == TYPE_UNKNOWN) {
            decl = hoist_find_decl(else_body, n);
            if (decl && decl->child_count > 0
                && decl->children[0] && decl->children[0]->node_type) {
                var_type = decl->children[0]->node_type;
            }
        }
        Type* joined = sibling_join_type(gen, n, var_type);
        if (joined) var_type = joined;
        print_indent(gen);
        /* #2024: a mutated capture is hoisted as its cell, not as a plain
         * value -- see hoist_loop_vars. */
        if (is_promoted_capture(gen, n)) {
            emit_promoted_cell_declaration(gen, n, get_c_type(var_type), var_type, NULL, NULL,
                                           decl ? decl->line : then_body->line,
                                           decl ? decl->column : then_body->column);
            if (joined) free_type(joined);
            continue;
        }
        mark_var_declared_typed(gen, n, var_type);
        emit_hoisted_local_decl(gen, var_type, n);
        claim_closure_local_env(gen, n, decl, 0);   /* #2480 */
        if (joined) free_type(joined);
    }
}

// Pre-declare variables from a while/for loop body so they're visible
// at function scope in the generated C. Without this, variables first
// assigned inside a while block are C-block-scoped and invisible to
// subsequent while blocks in the same function.
/* One declaration hoist_loop_vars declares before its while (the order
 * and the set come from hoist_loop_decls, shared with the typechecker). */
static void hoist_loop_var(ASTNode* child, void* user) {
    CodeGenerator* gen = (CodeGenerator*)user;
    /* #744: don't hoist a module-level `var` global as a loop-
     * scoped local — it would shadow the file-scope static. */
    /* #2505: nor an actor's state field: `kept = kept + 1` in a handler's
     * loop writes self->kept, and a hoisted local of that name shadowed it
     * for the loop's closed form. */
    if (!is_var_declared(gen, child->value) &&
        !is_module_global_var(gen, child->value) &&
        !is_actor_state_var(gen, child->value)) {
        // Determine type
        Type* var_type = child->node_type;
        if ((!var_type || var_type->kind == TYPE_VOID || var_type->kind == TYPE_UNKNOWN)
            && child->child_count > 0 && child->children[0] && child->children[0]->node_type) {
            var_type = child->children[0]->node_type;
        }
        Type* joined = sibling_join_type(gen, child->value, var_type);
        if (joined) var_type = joined;
        const char* c_type = get_c_type(var_type);
        print_indent(gen);
        /* #2024: a variable a closure mutates lives in a heap
         * cell, not a plain value, and every later write to it
         * is `*name = ...`. Hoisting it as `T name;` declared
         * the wrong thing and the first assignment in the body
         * then dereferenced an int. The cell is hoisted instead
         * -- same scope as any other hoisted loop variable, so
         * one cell across iterations, and its release is queued
         * at this scope's exit like a first assignment would. */
        if (is_promoted_capture(gen, child->value)) {
            emit_promoted_cell_declaration(gen, child->value, c_type, var_type, NULL, NULL,
                                           child->line, child->column);
            if (joined) free_type(joined);
            return;
        }
        mark_var_declared_typed(gen, child->value, var_type);
        /* Zero-initialize struct hoists so the first-iteration
         * struct-destroy call (#465) sees zero `_heap_<field>`
         * trackers instead of stack-uninitialised garbage.
         * Without this, the first `b = Box { ... }` inside the
         * loop body runs `Box_destroy(&b)` on uninit memory
         * and may free a garbage pointer. The {0} initialiser
         * is C99-portable and a no-op for non-struct types
         * either (the C compiler does the right thing). */
        if (var_type && var_type->kind == TYPE_STRUCT) {
            fprintf(gen->output, "%s %s = {0};\n", c_type, child->value);
            /* Push the function-exit struct-destroy defer
             * here too — the in-loop reassignment path
             * doesn't run the first-declaration codegen
             * that normally pushes the defer (the var is
             * already-declared via this hoist). Without
             * this, the final loop-iteration's heap fields
             * never get reclaimed at function exit. */
            if (var_type->struct_name && gen->program) {
                ASTNode* sdef = find_struct_definition_by_name(
                    gen->program, var_type->struct_name);
                if (sdef && struct_owns_heap_strings(gen, sdef)) {
                    ASTNode* carrier = create_ast_node(
                        AST_EXPRESSION_STATEMENT, NULL,
                        child->line, child->column);
                    if (carrier) {
                        if (carrier->annotation) free(carrier->annotation);
                        carrier->annotation = heap_strf("struct_destroy:%s:%s",
                                                        child->value, var_type->struct_name);
                        codegen_own_node(gen, carrier);
                        push_defer(gen, carrier);
                    }
                }
            }
        } else {
            emit_hoisted_local_decl(gen, var_type, child->value);
            /* #2480: the hoisted local is the loop's one C variable, so the
             * env free is queued here, outside the loop; each iteration's
             * rebinding frees the env it replaces. */
            claim_closure_local_env(gen, child->value, child, 0);
        }
        if (joined) free_type(joined);
    }
}

static void hoist_loop_vars(CodeGenerator* gen, ASTNode* body) {
    hoist_loop_decls(body, hoist_loop_var, gen);
}

/* ---- #2378: threaded dispatch for interpreter loops -----------------------
 *
 *     while true {
 *         <head statements>
 *         switch <selector> { case ...: { ... continue } ... }
 *         <tail statements>
 *     }
 *
 * is an interpreter's dispatch loop. Where the C compiler has labels-as-values
 * (GCC, Clang, MinGW, Emscripten), each arm gets a C label and a static table
 * maps a selector value to it, and a `continue` that targets the loop runs the
 * head again and jumps straight to the next arm (`goto *table[sel]`) instead of
 * going round the loop. Each arm then ends in its own indirect branch, which
 * predicts far better than the one shared jump a `switch` compiles to, and the
 * loop head drops out of the hot path. Actor message dispatch already lowers
 * this way (codegen_actor.c).
 *
 * Correctness never depends on the table. The loop and the `switch` are
 * emitted exactly as before; the labels and the table are added under
 * AE_TD_GUARD, and a selector with no table entry jumps back to the `switch`,
 * which handles it as it always did. Without labels-as-values (MSVC), or with
 * AETHER_NO_THREADED_DISPATCH defined, a `continue` is the plain `continue;` of
 * today's output. An arm that falls off the end of the `switch` also goes round
 * the loop as before, so a loop that only partly matches the shape is still
 * correct, just less threaded.
 *
 * The tail runs only when an arm breaks or falls off, or nothing matches, as
 * in the plain loop, and is emitted once, so it is unrestricted (interpreters
 * put their slow-path exit and unknown-opcode error there).
 *
 * The shape is recognised conservatively: the head is re-emitted at every
 * `continue` site, so it may only assign scalar values (no strings, closures or
 * interpolation, whose ownership tracking is per-site); the selector must be an
 * integer; and every case value must be an integer known at compile time,
 * between 0 and AE_TD_MAX_TABLE, since it is a table index. Anything else
 * lowers exactly as today. */
static int find_labeled_loop_level(CodeGenerator* gen, const char* name);

#define AE_TD_MAX_TABLE 4096
#define AE_TD_GUARD "#if (defined(__GNUC__) || defined(__clang__)) && !defined(AETHER_NO_THREADED_DISPATCH)"

static int td_type_is_int(Type* t) {
    if (!t) return 0;
    switch (t->kind) {
        case TYPE_INT: case TYPE_INT64: case TYPE_UINT64: case TYPE_UINT32:
        case TYPE_UINT16: case TYPE_UINT8: case TYPE_BYTE: case TYPE_ENUM:
            return 1;
        default:
            return 0;
    }
}

/* No closure, interpolation, value-producing block, or string anywhere in e. */
static int td_tree_is_plain(ASTNode* e) {
    if (!e) return 1;
    switch (e->type) {
        case AST_CLOSURE: case AST_STRING_INTERP: case AST_IF_EXPRESSION:
        case AST_MATCH_STATEMENT: case AST_BLOCK: case AST_DEFER_STATEMENT:
        case AST_TRY_STATEMENT:
            return 0;
        default:
            break;
    }
    if (e->node_type && e->node_type->kind == TYPE_STRING) return 0;
    for (int i = 0; i < e->child_count; i++) {
        if (!td_tree_is_plain(e->children[i])) return 0;
    }
    return 1;
}

/* The case values of one arm, appended to vals/arms. 0 when one is not a
 * compile-time integer in [0, AE_TD_MAX_TABLE). */
static int td_case_values(CodeGenerator* gen, ASTNode* sel, int arm,
                          int64_t* vals, int* arms, int* n, int cap) {
    if (!sel) return 0;
    if (sel->type == AST_MATCH_ALT) {
        for (int a = 0; a < sel->child_count; a++) {
            if (!td_case_values(gen, sel->children[a], arm, vals, arms, n, cap)) return 0;
        }
        return 1;
    }
    int64_t v;
    if (!contract_eval_int64(sel, gen->program, &v)) return 0;
    if (v < 0 || v >= AE_TD_MAX_TABLE || *n >= cap) return 0;
    vals[*n] = v;
    arms[*n] = arm;
    (*n)++;
    return 1;
}

/* 1 when some `continue` in `n` targets the loop: an unlabeled one not
 * inside a nested loop, or one naming `label` at any depth. Closures are not
 * searched: a continue cannot leave one. */
static int td_has_continue(ASTNode* n, const char* label, int nested) {
    if (!n) return 0;
    if (n->type == AST_CLOSURE) return 0;
    if (n->type == AST_CONTINUE_STATEMENT) {
        if (n->value) return label && strcmp(n->value, label) == 0;
        return !nested;
    }
    int inner = nested;
    if (n->type == AST_WHILE_LOOP || n->type == AST_FOR_LOOP) inner = 1;
    for (int i = 0; i < n->child_count; i++) {
        if (td_has_continue(n->children[i], label, inner)) return 1;
    }
    return 0;
}

/* 1 while the loop head is being emitted (the loop's own pass, or a copy at
 * a continue site). A `continue` in the head then stays a plain `continue;`
 * instead of expanding into another copy of the head, which would recurse;
 * inside a copy it still leaves the switch for the loop, so it is correct. */
static int g_td_in_head = 0;

/* A head statement can be copied to every continue site: no closures,
 * interpolation, strings, defers, try, nested loops or switches, and no
 * `break` (in a copy, which sits inside the switch, it would leave the switch
 * instead of the loop). `if` statements and `continue` are fine. */
static int td_head_ok(ASTNode* n) {
    if (!n) return 1;
    switch (n->type) {
        case AST_CLOSURE: case AST_STRING_INTERP: case AST_IF_EXPRESSION:
        case AST_MATCH_STATEMENT: case AST_DEFER_STATEMENT: case AST_TRY_STATEMENT:
        case AST_WHILE_LOOP: case AST_FOR_LOOP: case AST_SWITCH_STATEMENT:
        case AST_BREAK_STATEMENT:
            return 0;
        default:
            break;
    }
    if (n->node_type && n->node_type->kind == TYPE_STRING) return 0;
    for (int i = 0; i < n->child_count; i++) {
        if (!td_head_ok(n->children[i])) return 0;
    }
    return 1;
}

/* Index of the loop body's dispatch switch: its first top-level `switch`, or
 * -1. Statements before it are the head; statements after it are the tail,
 * which runs only when an arm breaks or falls off, or no arm matches, exactly
 * as in the plain loop, so it is emitted once and is not restricted. */
static int td_switch_index(ASTNode* body) {
    if (!body || body->type != AST_BLOCK) return -1;
    for (int i = 0; i < body->child_count; i++) {
        if (body->children[i] && body->children[i]->type == AST_SWITCH_STATEMENT) return i;
    }
    return -1;
}

/* AETHER_EXPLAIN_THREADED=1: say why a loop that looks like a dispatch loop
 * (`while <always true> { ...; switch ... }`) was not threaded. */
static void td_explain(ASTNode* loop, const char* why) {
    static int on = -1;
    if (on < 0) {
        const char* v = getenv("AETHER_EXPLAIN_THREADED");
        on = (v && *v && strcmp(v, "0") != 0) ? 1 : 0;
    }
    if (!on || !loop || loop->child_count < 2 || !loop->children[1]) return;
    /* Neither the while nor the switch node carries a position; the
     * switch's selector expression does. */
    ASTNode* body = loop->children[1];
    int si = td_switch_index(body);
    ASTNode* at = si >= 0 ? body->children[si] : loop;
    if (at->child_count > 0 && at->children[0] && at->children[0]->line > 0) at = at->children[0];
    fprintf(stderr, "%s:%d: note: dispatch loop not threaded: %s\n",
            at->source_file ? at->source_file : "?", at->line, why);
}

/* The loop's switch when `loop` has the threaded shape, else NULL. */
static ASTNode* td_loop_switch(CodeGenerator* gen, ASTNode* loop) {
    if (!loop || loop->child_count < 2) return NULL;
    ASTNode* cond = loop->children[0];
    ASTNode* body = loop->children[1];
    int si = td_switch_index(body);
    if (si < 0) return NULL;
    ASTNode* sw = body->children[si];
    if (sw->child_count < 2) return NULL;
    /* An always-true condition: `true`, `1`, `1 == 1`, a true const. */
    ContractEnv env;
    memset(&env, 0, sizeof env);
    env.program = gen->program;
    if (!cond || contract_eval_predicate(cond, &env) != CONTRACT_TRUE) return NULL;
    /* From here the loop has the shape; anything that stops it is worth saying. */
    ASTNode* sel = sw->children[0];
    if (!sel || !td_type_is_int(sel->node_type)) {
        td_explain(loop, "the switch selector is not an integer");
        return NULL;
    }
    if (!td_tree_is_plain(sel)) {
        td_explain(loop, "the switch selector is not a plain expression");
        return NULL;
    }
    for (int i = 0; i < si; i++) {
        if (!td_head_ok(body->children[i])) {
            td_explain(loop, "a statement before the switch cannot be repeated at each continue "
                             "(it has a string, closure, defer, try, nested loop, switch or break)");
            return NULL;
        }
    }
    int64_t vals[512];
    int arms[512];
    int n = 0;
    for (int i = 1; i < sw->child_count; i++) {
        ASTNode* c = sw->children[i];
        if (!c || c->type != AST_CASE_STATEMENT) return NULL;
        if (c->value && strcmp(c->value, "default") == 0) continue;
        if (c->child_count < 1 || selector_has_range(c->children[0])) {
            td_explain(loop, "a case is a range");
            return NULL;
        }
        if (!td_case_values(gen, c->children[0], i, vals, arms, &n, 512)) {
            td_explain(loop, "a case value is not an integer constant in 0..4095 known at compile time");
            return NULL;
        }
    }
    if (n == 0) return NULL;
    /* Threading pays off only through a `continue` that targets this loop,
     * and without one the table and labels would go unreferenced. */
    for (int i = 1; i < sw->child_count; i++) {
        if (td_has_continue(sw->children[i], loop->value, 0)) return sw;
    }
    td_explain(loop, "no arm continues the loop");
    return NULL;
}

/* The static table: one entry per case value, pointing at its arm's label. */
static void td_emit_table(CodeGenerator* gen, ASTNode* sw, int id) {
    int64_t vals[512];
    int arms[512];
    int n = 0;
    int64_t max = 0;
    for (int i = 1; i < sw->child_count; i++) {
        ASTNode* c = sw->children[i];
        if (c->value && strcmp(c->value, "default") == 0) continue;
        td_case_values(gen, c->children[0], i, vals, arms, &n, 512);
    }
    for (int k = 0; k < n; k++) {
        if (vals[k] > max) max = vals[k];
    }
    print_line(gen, AE_TD_GUARD);
    print_line(gen, "int64_t _ae_td_sel_%d = 0;", id);
    print_indent(gen);
    fprintf(gen->output, "static void* const _ae_td_tbl_%d[%lld] = {", id, (long long)(max + 1));
    for (int k = 0; k < n; k++) {
        fprintf(gen->output, "%s[%lld] = &&_ae_td_%d_arm%d", k ? ", " : " ",
                (long long)vals[k], id, arms[k]);
    }
    fprintf(gen->output, " };\n");
    print_line(gen, "#endif");
}

/* What a `continue` targeting threaded loop `level` becomes: the loop-head
 * checks, the head statements, the selector, then a jump to the next arm (or
 * back to the switch when the value has no entry). `fallback` is the line the
 * continue lowers to without labels-as-values. */
static void td_emit_dispatch(CodeGenerator* gen, int level, const char* fallback) {
    ASTNode* loop = gen->loop_td_node[level];
    int id = gen->loop_td_id[level];
    ASTNode* body = loop->children[1];
    int si = td_switch_index(body);
    ASTNode* sw = body->children[si];
    print_line(gen, AE_TD_GUARD);
    print_line(gen, "{");
    indent(gen);
    if (gen->preempt_loops) {
        print_line(gen, "if (--_aether_reductions <= 0) { _aether_reductions = 10000; sched_yield(); }");
    }
    if (gen->emit_lib) {
        print_line(gen, "if (aether_caps_armed && aether_caps_deadline_tripped()) { __aether_abort_call(); goto _ae_td_exit_%d; }", id);
    }
    g_td_in_head++;
    for (int i = 0; i < si; i++) {
        generate_statement(gen, body->children[i]);
    }
    g_td_in_head--;
    print_indent(gen);
    fprintf(gen->output, "_ae_td_sel_%d = (int64_t)(", id);
    generate_expression(gen, sw->children[0]);
    fprintf(gen->output, ");\n");
    print_line(gen, "if ((uint64_t)_ae_td_sel_%d < (uint64_t)(sizeof(_ae_td_tbl_%d) / sizeof(_ae_td_tbl_%d[0])) && _ae_td_tbl_%d[_ae_td_sel_%d]) goto *_ae_td_tbl_%d[_ae_td_sel_%d];",
               id, id, id, id, id, id, id);
    print_line(gen, "goto _ae_td_sw_%d;", id);
    unindent(gen);
    print_line(gen, "}");
    print_line(gen, "#else");
    print_line(gen, "%s", fallback);
    print_line(gen, "#endif");
}

/* The threaded loop a `continue` targets, as its loop level, or -1. */
static int td_continue_target(CodeGenerator* gen, ASTNode* stmt) {
    if (g_td_in_head) return -1;
    int lvl;
    if (stmt->value) {
        lvl = find_labeled_loop_level(gen, stmt->value);
    } else {
        lvl = gen->loop_nest_depth - 1;
    }
    if (lvl < 0 || lvl >= AETHER_MAX_LOOP_NEST) return -1;
    return gen->loop_td_id[lvl] ? lvl : -1;
}

// Pre-hoist variables first-declared inside if-statement branches at
// the enclosing function-body scope, when:
//   (a) the variable is referenced *outside* (after) the if-block, and
//   (b) the existing hoist_if_else_common_vars hasn't already handled
//       it (which only fires when both branches declare the variable
//       and they have a common else).
//
// Without this, a sequence like
//
//     if cond1 { x = ... }
//     if cond2 { x = ... }
//     return x
//
// emits C where each branch C-scopes `x` inside its own `{ ... }`,
// and the function-scope `return x` can't see it. Closes #278.
//
// This is over-hoisting: any variable first-written inside any if
// gets a function-scope declaration. Harmless in C (just a tentative
// definition); the inner branches' `Type x = expr` becomes an
// assignment to the outer-scope `x`. The codegen's existing
// is_var_declared check skips re-declaration in the inner branch.

// ============================================================
// Issue #348 — Eiffel-style `requires` / `ensures` contracts.
// ============================================================
//
// The parser attaches each clause as an AST_REQUIRES_CLAUSE or
// AST_ENSURES_CLAUSE child of AST_FUNCTION_DEFINITION; the predicate
// expression is the clause node's single child. Codegen lowers each
// to an `if (!(<expr>)) aether_panic(...)` shaped check at the right
// scope:
//
//   `requires`  → emitted at function entry, after parameters are
//                  declared and before any user code runs.
//                  Parameters are in scope.
//
//   `ensures`   → emitted before every `return <expr>;` site,
//                  wrapped in a C block scope `{ <T> result = <expr>;
//                  ... return result; }` so the predicate's `result`
//                  identifier resolves to a fresh local that holds
//                  the about-to-be-returned value. Each return site
//                  gets its own copy of every check; partial-return
//                  paths through if/else / match all stay correct.
//
// `--no-contracts` (CodeGenerator::no_contracts) skips emission
// entirely — the per-call cost goes to zero, mirroring C's
// `-DNDEBUG` for assert.
//
// Diagnostic message format:
//
//   precondition violation: <predicate-text> in <fn-name>
//   postcondition violation: <predicate-text> in <fn-name>
//
// `<predicate-text>` comes from a small reverse-printer
// (`fprint_expr_text`) that round-trips the AST back to source-like
// form. It's intentionally simple — covers identifiers, literals,
// binary/unary ops, member access, function calls — so the panic
// message names the specific failed predicate even when a function
// has multiple clauses. Anything the round-tripper doesn't handle
// falls through to the literal string `"<expr>"`, which is still
// disambiguated by the surrounding "<predicate-text> in <fn-name>"
// line+column info from the panic stack trace (issue #347).
// Emit one `if (!(<predicate>)) aether_panic("<role> violation: <text>
// in <fn>");` block for a single clause. If the predicate is
// provably constant-true at compile time, skip emission entirely
// (zero per-call cost — analog of `static_assert` for the trivial
// case). A constant-false predicate falls through to runtime
// emission so the panic surface still names the failed clause; the
// runtime trip is observable to the test suite without aetherc
// having to refuse the build.
static void emit_contract_check(CodeGenerator* gen,
                                ASTNode* clause,
                                const char* role,
                                const char* fn_name) {
    if (!clause || clause->child_count == 0) return;
    ASTNode* predicate = clause->children[0];
    /* Const-fold through the shared evaluator (contract folding — see
     * docs/contract-folding.md). Definition-site env: no parameter bindings,
     * but the program handle so `const` names and enum members resolve —
     * `requires cap > MIN_CAP` now elides when both sides are constants,
     * which the older literal-only folder here could not do.
     *
     * Only TRUE elides. A decidably-FALSE predicate was already rejected by
     * the typechecker before codegen ran; if one ever reaches here anyway
     * (UNKNOWN to the typechecker but false at run time), the runtime check
     * below still fires, so the belt keeps its braces. */
    ContractEnv cenv = {{0}, {0}, 0, gen->program};
    if (contract_eval_predicate(predicate, &cenv) == CONTRACT_TRUE) {
        /* Trivially-true predicate. Drop the runtime check — the
         * generated C should be byte-for-byte identical to a
         * function written without the clause. Emit a comment for
         * the curious reader inspecting the .c output. */
        print_indent(gen);
        fprintf(gen->output, "/* %s elided (always-true): ", role);
        char buf[1024];
        ContractStr s = { buf, sizeof(buf), 0 };
        contract_sprint_expr(&s, predicate);
        contract_str_terminate(&s);
        for (const char* p = buf; *p; p++) {
            /* Defensively split any star-slash sequence so the
             * predicate text can't accidentally terminate the
             * surrounding C comment. */
            if (p[0] == '*' && p[1] == '/') { fputs("* /", gen->output); p++; }
            else fputc(*p, gen->output);
        }
        fprintf(gen->output, " */\n");
        return;
    }
    print_indent(gen);
    fprintf(gen->output, "if (!(");
    generate_expression(gen, predicate);
    /* #1378: a stable, greppable category token leads the message, so CI and
       downstream triage can match on the category instead of prose that each
       site words differently. The human detail follows the colon. */
    fprintf(gen->output, ")) aether_panic(\"%s_violation: ", role);
    /* Re-render the predicate text into the C string literal. We
     * escape backslash and double-quote; everything else passes
     * through (Aether-source-level printable ASCII is safe in C
     * literals). */
    char buf[1024];
    ContractStr s = { buf, sizeof(buf), 0 };
    contract_sprint_expr(&s, predicate);
    contract_str_terminate(&s);
    for (const char* p = buf; *p; p++) {
        if (*p == '\\' || *p == '"') fputc('\\', gen->output);
        fputc(*p, gen->output);
    }
    fprintf(gen->output, " in %s\");\n", fn_name ? fn_name : "<fn>");
}

void emit_contract_preconditions(CodeGenerator* gen, ASTNode* func) {
    if (!gen || !func || gen->no_contracts) return;
    const char* fn_name = func->value ? func->value : "<fn>";
    for (int i = 0; i < func->child_count; i++) {
        ASTNode* c = func->children[i];
        if (c && c->type == AST_REQUIRES_CLAUSE) {
            emit_contract_check(gen, c, "precondition", fn_name);
        }
    }
}

// Emit `ensures` checks before a return. Caller has already opened a
// fresh `{` scope and emitted `<T> result = <expr>;` so `result` is
// in scope as a C local. Returns 1 if any check was emitted.
int emit_contract_postconditions(CodeGenerator* gen, ASTNode* func) {
    if (!gen || !func || gen->no_contracts) return 0;
    const char* fn_name = func->value ? func->value : "<fn>";
    int emitted = 0;
    for (int i = 0; i < func->child_count; i++) {
        ASTNode* c = func->children[i];
        if (c && c->type == AST_ENSURES_CLAUSE) {
            emit_contract_check(gen, c, "postcondition", fn_name);
            emitted = 1;
        }
    }
    return emitted;
}

// Returns 1 iff `func` has at least one AST_ENSURES_CLAUSE child.
// Used by the AST_RETURN_STATEMENT codegen to decide whether to
// route through the result-local + post-check wrapper.
static int function_has_ensures(ASTNode* func) {
    if (!func) return 0;
    for (int i = 0; i < func->child_count; i++) {
        if (func->children[i] &&
            func->children[i]->type == AST_ENSURES_CLAUSE) {
            return 1;
        }
    }
    return 0;
}

void hoist_if_branch_vars(CodeGenerator* gen, ASTNode* body) {
    if (!body) return;
    /* Which names: hoist_if_branch_candidates (shared with the typechecker,
     * which must accept exactly the reads this makes compile). */
    const char* names[HOIST_MAX_NAMES];
    int count = hoist_if_branch_candidates(body, names, HOIST_MAX_NAMES);
    for (int n = 0; n < count; n++) {
        const char* name = names[n];
        if (is_var_declared(gen, name)) continue;
        /* #744: a module-level `var` global first assigned inside an
         * if-branch must NOT be hoisted as a fresh function local —
         * that local shadows the file-scope `static`, so every write
         * lands in the local and the global keeps its initializer
         * forever (a silent miscompile; regression in #701). The
         * assignment is already routed to the global by the
         * AST_VARIABLE_DECLARATION emitter (is_module_global_var), so
         * skip it here exactly as that path does. */
        if (is_module_global_var(gen, name)) continue;
        if (is_actor_state_var(gen, name)) continue;   /* #2505: self->name */
        ASTNode* first_decl = hoist_if_branch_first_decl(body, name);
        if (!first_decl) continue;
        Type* var_type = first_decl->node_type;
        if ((!var_type || var_type->kind == TYPE_VOID || var_type->kind == TYPE_UNKNOWN)
            && first_decl->child_count > 0 && first_decl->children[0]
            && first_decl->children[0]->node_type) {
            var_type = first_decl->children[0]->node_type;
        }
        Type* joined = sibling_join_type(gen, name, var_type);
        if (joined) var_type = joined;
        const char* c_type = get_c_type(var_type);
        print_indent(gen);
        /* #2024: same as hoist_loop_vars -- a mutated capture is hoisted
         * as its cell, not as a plain value. */
        if (is_promoted_capture(gen, name)) {
            emit_promoted_cell_declaration(gen, name, c_type, var_type, NULL, NULL,
                                           first_decl->line, first_decl->column);
            if (joined) free_type(joined);
            continue;
        }
        fprintf(gen->output, "%s %s%s;\n", c_type, name, hoisted_zero_init(var_type, c_type));
        mark_var_declared_typed(gen, name, var_type);
        claim_closure_local_env(gen, name, first_decl, 0);   /* #2480 */
        if (joined) free_type(joined);
    }
}


/* #752: a struct local returned (directly or as a tuple element) hands
 * ownership of its heap-string fields to the caller. Mark it so the
 * function-exit <Struct>_destroy defer is suppressed (try_emit_struct_
 * destroy) — otherwise the fields are freed at callee exit while the
 * returned shallow copy still points at them. Marking a non-struct
 * identifier is harmless (no struct_destroy defer is keyed to it). */
static void mark_returned_struct_escaped(CodeGenerator* gen, ASTNode* expr) {
    if (!expr || expr->type != AST_IDENTIFIER || !expr->value || !gen->program) return;
    Type* t = expr->node_type;
    if (!t || t->kind != TYPE_STRUCT || !t->struct_name) return;
    ASTNode* sdef = find_struct_definition_by_name(gen->program, t->struct_name);
    if (sdef && struct_owns_heap_strings(gen, sdef)) {
        mark_return_escaped_struct_var(gen, expr->value);
    }
}

/* A struct type whose values own heap strings: it has `_heap_<field>`
 * trackers and a `<Name>_destroy`. NULL otherwise. */
const char* struct_owning_strings(CodeGenerator* gen, Type* t) {
    if (!gen || !gen->program || !t || t->kind != TYPE_STRUCT || !t->struct_name) return NULL;
    ASTNode* sdef = find_struct_definition_by_name(gen->program, t->struct_name);
    return (sdef && struct_owns_heap_strings(gen, sdef)) ? t->struct_name : NULL;
}

/* The calls in an expression statement whose value is a struct, owning
 * strings, returned by value: temporaries the statement destroys once done
 * (see the expression-statement path). Not into a closure, whose body is
 * its own function. */
void collect_stmt_struct_temps(CodeGenerator* gen, ASTNode* e,
                               ASTNode*** nodes, int* count, int* cap) {
    if (!e || e->type == AST_CLOSURE) return;
    if (e->type == AST_FUNCTION_CALL && e->node_type &&
        e->node_type->kind == TYPE_STRUCT && struct_owning_strings(gen, e->node_type)) {
        if (*count == *cap) {
            *cap = *cap ? *cap * 2 : 4;
            *nodes = (ASTNode**)aether_xrealloc(*nodes, sizeof(ASTNode*) * (size_t)*cap);
        }
        (*nodes)[(*count)++] = e;
    }
    for (int i = 0; i < e->child_count; i++)
        collect_stmt_struct_temps(gen, e->children[i], nodes, count, cap);
}

/* `<lvalue>._heap_<f> = 0;` for every string field of `struct_name`: the
 * value at `lvalue` stops owning its strings, which stay with (or move to)
 * whoever else holds them. */
/* #2525: a closure field has no tracker apart from its value: the env is
 * the reference. Moving the struct out (`retain_closures` 0) clears it so
 * the old holder releases nothing; a parameter, a copy of the caller's
 * value that stays callable (`retain_closures` 1), takes a reference of its
 * own instead, released by its scope-exit destroy or the store that
 * replaces the field, never the caller's. */
static void emit_struct_disown_fields(CodeGenerator* gen, ASTNode* sdef,
                                      const char* lvalue, int depth, int retain_closures) {
    for (int i = 0; sdef && depth < 32 && i < sdef->child_count; i++) {
        ASTNode* f = sdef->children[i];
        if (f && f->type == AST_STRUCT_FIELD && f->node_type &&
            f->node_type->kind == TYPE_STRING) {
            fprintf(gen->output, "%s._heap_%s = 0; ", lvalue, f->value);
        }
        if (struct_field_is_closure(f)) {
            if (retain_closures) {
                fprintf(gen->output, "_aether_closure_env_retain(%s.%s.env); ", lvalue, f->value);
            } else {
                fprintf(gen->output, "%s.%s.env = (void*)0; ", lvalue, f->value);
            }
        }
        /* #2528: an owned array's elements move with the value, or a
         * parameter takes copies (strings) or references (closures) of its
         * own. */
        int oalen = 0;
        int oak = struct_field_owned_array(f, &oalen);
        if (oak == 1) {
            if (retain_closures) {
                fprintf(gen->output, "for (int _ai%d = 0; _ai%d < %d; _ai%d++) if (%s.%s[_ai%d]) %s.%s[_ai%d] = aether_uniform_heap_str(%s.%s[_ai%d], 0); ",
                        depth, depth, oalen, depth, lvalue, f->value, depth, lvalue, f->value, depth, lvalue, f->value, depth);
            } else {
                fprintf(gen->output, "for (int _ai%d = 0; _ai%d < %d; _ai%d++) %s.%s[_ai%d] = (const char*)0; ",
                        depth, depth, oalen, depth, lvalue, f->value, depth);
            }
        } else if (oak == 2) {
            fprintf(gen->output, "for (int _ai%d = 0; _ai%d < %d; _ai%d++) ", depth, depth, oalen, depth);
            if (retain_closures) {
                fprintf(gen->output, "_aether_closure_env_retain(%s.%s[_ai%d].env); ", lvalue, f->value, depth);
            } else {
                fprintf(gen->output, "%s.%s[_ai%d].env = (void*)0; ", lvalue, f->value, depth);
            }
        }
        /* #2497: a struct field held by value owns its own strings; #2525:
         * so does each element of a fixed-size array of them. */
        int alen = 0;
        ASTNode* inner = owning_struct_field_def_n(gen, f, &alen);
        if (inner) {
            if (alen > 0) {
                fprintf(gen->output, "for (int _ai%d = 0; _ai%d < %d; _ai%d++) { ",
                        depth, depth, alen, depth);
                const char* sub = cg_internf("%s.%s[_ai%d]", lvalue, f->value, depth);
                emit_struct_disown_fields(gen, inner, sub, depth + 1, retain_closures);
                fprintf(gen->output, "} ");
            } else {
                const char* sub = cg_internf("%s.%s", lvalue, f->value);
                emit_struct_disown_fields(gen, inner, sub, depth + 1, retain_closures);
            }
        }
    }
}

void emit_struct_disown(CodeGenerator* gen, const char* struct_name, const char* lvalue,
                        int retain_closures) {
    ASTNode* sdef = gen->program ? find_struct_definition_by_name(gen->program, struct_name) : NULL;
    if (!sdef) return;
    print_indent(gen);
    emit_struct_disown_fields(gen, sdef, lvalue, 0, retain_closures);
    fprintf(gen->output, "\n");
}

/* #2582: the counterpart of emit_struct_disown_fields for a struct parameter
 * its body keeps as a whole value (struct_param_kept). Each string field
 * becomes the parameter's own: a reference taken on a counted string, a copy
 * of anything else (aether_str_capture), with its tracker set. A closure
 * field and an owned array's elements are taken as a borrowing parameter
 * takes them. The parameter then owns all it holds, as a local does:
 * returned, moved or stored, it hands over strings of its own, and its exit
 * destroys what it did not hand over. The caller keeps its argument either
 * way, as for a `string` parameter a function keeps (#2499). */
static void emit_struct_capture_fields(CodeGenerator* gen, ASTNode* sdef,
                                       const char* lvalue, int depth) {
    for (int i = 0; sdef && depth < 32 && i < sdef->child_count; i++) {
        ASTNode* f = sdef->children[i];
        if (f && f->type == AST_STRUCT_FIELD && f->node_type &&
            f->node_type->kind == TYPE_STRING) {
            fprintf(gen->output, "%s.%s = aether_str_capture(%s.%s); %s._heap_%s = %s.%s != 0; ",
                    lvalue, f->value, lvalue, f->value, lvalue, f->value, lvalue, f->value);
        }
        if (struct_field_is_closure(f)) {
            fprintf(gen->output, "_aether_closure_env_retain(%s.%s.env); ", lvalue, f->value);
        }
        int oalen = 0;
        int oak = struct_field_owned_array(f, &oalen);
        if (oak == 1) {
            fprintf(gen->output, "for (int _ai%d = 0; _ai%d < %d; _ai%d++) if (%s.%s[_ai%d]) %s.%s[_ai%d] = aether_uniform_heap_str(%s.%s[_ai%d], 0); ",
                    depth, depth, oalen, depth, lvalue, f->value, depth, lvalue, f->value, depth, lvalue, f->value, depth);
        } else if (oak == 2) {
            fprintf(gen->output, "for (int _ai%d = 0; _ai%d < %d; _ai%d++) _aether_closure_env_retain(%s.%s[_ai%d].env); ",
                    depth, depth, oalen, depth, lvalue, f->value, depth);
        }
        int alen = 0;
        ASTNode* inner = owning_struct_field_def_n(gen, f, &alen);
        if (inner) {
            if (alen > 0) {
                fprintf(gen->output, "for (int _ai%d = 0; _ai%d < %d; _ai%d++) { ",
                        depth, depth, alen, depth);
                emit_struct_capture_fields(gen, inner,
                                           cg_internf("%s.%s[_ai%d]", lvalue, f->value, depth),
                                           depth + 1);
                fprintf(gen->output, "} ");
            } else {
                emit_struct_capture_fields(gen, inner, cg_internf("%s.%s", lvalue, f->value),
                                           depth + 1);
            }
        }
    }
}

void emit_struct_capture(CodeGenerator* gen, const char* struct_name, const char* lvalue) {
    ASTNode* sdef = gen->program ? find_struct_definition_by_name(gen->program, struct_name) : NULL;
    if (!sdef) return;
    print_indent(gen);
    emit_struct_capture_fields(gen, sdef, lvalue, 0);
    fprintf(gen->output, "\n");
}

/* #2582: is `n` an access chain (fields, elements) rooted at identifier
 * `pname`? */
static int access_chain_rooted_at(ASTNode* n, const char* pname) {
    while (n && (n->type == AST_MEMBER_ACCESS || n->type == AST_ARRAY_ACCESS) &&
           n->child_count >= 1)
        n = n->children[0];
    return n && n->type == AST_IDENTIFIER && n->value && strcmp(n->value, pname) == 0;
}

/* #2582: does a value of type `t` own strings: a struct that does, or an
 * array of such structs or of strings? */
static int type_owns_strings(CodeGenerator* gen, Type* t) {
    if (!t) return 0;
    if (struct_owning_strings(gen, t)) return 1;
    return t->kind == TYPE_ARRAY && t->element_type &&
           (t->element_type->kind == TYPE_STRING ||
            struct_owning_strings(gen, t->element_type));
}

/* #2582: can a value of type `t` point into memory an argument owns: a
 * string, a pointer, an array or a tuple holding one? */
static int type_can_view(Type* t) {
    if (!t) return 1;
    if (t->kind == TYPE_STRING || t->kind == TYPE_PTR || t->kind == TYPE_ARRAY) return 1;
    if (t->kind == TYPE_TUPLE) {
        for (int i = 0; i < t->tuple_count; i++)
            if (type_can_view(t->tuple_types[i])) return 1;
    }
    return 0;
}

static int mentions_any_param(ASTNode* fn_def, ASTNode* e) {
    for (int i = 0; i < fn_def->child_count; i++) {
        ASTNode* prm = fn_def->children[i];
        if (prm && prm->value &&
            (prm->type == AST_PATTERN_VARIABLE || prm->type == AST_VARIABLE_DECLARATION) &&
            subtree_mentions_param(e, prm->value)) return 1;
    }
    return 0;
}

/* Does a `return` under `n` give, where the result can point into memory
 * (type_can_view, per tuple position), a value that mentions a parameter
 * of `fn_def`? A nested closure's returns are its own. */
static int returns_view_params_walk(ASTNode* fn_def, ASTNode* n, Type* rt) {
    if (!n || n->type == AST_CLOSURE) return 0;
    if (n->type == AST_RETURN_STATEMENT) {
        for (int j = 0; j < n->child_count; j++) {
            Type* pt = (rt && rt->kind == TYPE_TUPLE && n->child_count > 1 && j < rt->tuple_count)
                       ? rt->tuple_types[j] : rt;
            if (type_can_view(pt) && mentions_any_param(fn_def, n->children[j])) return 1;
        }
        return 0;
    }
    for (int i = 0; i < n->child_count; i++)
        if (returns_view_params_walk(fn_def, n->children[i], rt)) return 1;
    return 0;
}

/* #2582: may call `call`'s result be, or point into, one of its arguments as
 * it came? Then an argument is not the statement's to destroy while the
 * result lives.
 *
 * A scalar result holds nothing, and neither does a fresh heap string
 * (`string.concat`, a function whose every return is one). A function or
 * closure the compiler emits returns a struct of its own (a struct
 * parameter it keeps takes references on entry, struct_param_kept); any
 * other result it gives can be an argument only through a `return` whose
 * value mentions a parameter where the result can point into memory, as
 * `first(s, n) { return s }` hands back the caller's pointer, which may
 * point into the argument's struct (`first(make_item(w).name, 1)`). A
 * fallible `int!` returning a literal error is no view. Anything a C
 * function returns could be its argument. */
static int call_may_hand_back_args(CodeGenerator* gen, ASTNode* call) {
    if (!call || call->type != AST_FUNCTION_CALL) return 0;
    Type* rt = call->node_type;
    if (rt) {
        switch (rt->kind) {
            case TYPE_INT: case TYPE_INT64: case TYPE_UINT64: case TYPE_UINT32:
            case TYPE_UINT16: case TYPE_UINT8: case TYPE_DURATION: case TYPE_FLOAT:
            case TYPE_LONGDOUBLE: case TYPE_FLOAT32: case TYPE_BOOL: case TYPE_BYTE:
            case TYPE_VOID:
                return 0;
            default:
                break;
        }
    }
    if (rt && rt->kind == TYPE_STRING && is_heap_string_expr(gen, call) &&
        !handback_take_leaf(gen, call)) return 0;
    const char* fn = call->value ? codegen_normalise_callee(call->value) : NULL;
    if (!fn) return 1;
    if (strcmp(fn, "call") == 0) {
        /* A closure follows the convention the compiler emits it by: a
         * struct or string it returns is its own (a uniform-heap string
         * return, #2499). One it did not see could return anything. */
        if (!gen->closure_args_borrowed) return 1;
        return !(rt && (rt->kind == TYPE_STRUCT || rt->kind == TYPE_STRING));
    }
    if (is_nonstoring_builtin(fn)) return 0;
    if (!callee_has_visible_body(gen, call->value)) return 1;
    if (rt && rt->kind == TYPE_STRUCT) return 0;
    ASTNode* fn_def = find_function_definition_by_name(gen->program, fn);
    if (!fn_def) return 1;
    return returns_view_params_walk(fn_def, fn_def, rt);
}

/* #2619: is a struct a call returns, owning strings, somewhere in `e`? */
static int holds_struct_call(CodeGenerator* gen, ASTNode* e) {
    if (!e || e->type == AST_CLOSURE) return 0;
    if (e->type == AST_FUNCTION_CALL && e->node_type && e->node_type->kind == TYPE_STRUCT &&
        struct_owning_strings(gen, e->node_type)) return 1;
    for (int i = 0; i < e->child_count; i++)
        if (holds_struct_call(gen, e->children[i])) return 1;
    return 0;
}

/* #2619: a `string` call that may hand back an argument as it came
 * (call_may_hand_back_args) where an argument holds a struct another call
 * returns, which its statement destroys as a temporary. Its result is
 * copied where it is made (generate_expression), so it never points into
 * the temporary and the temporary can go with the statement. Before, the
 * argument was kept alive for good: `b = first(make_item(w).name, 1)`
 * leaked the struct on every call. */
static int g_view_check = 0;
int call_returns_view_of_temp(CodeGenerator* gen, ASTNode* call) {
    if (g_view_check || !call || call->type != AST_FUNCTION_CALL || !call->node_type ||
        call->node_type->kind != TYPE_STRING) return 0;
    int any = 0;
    for (int i = 0; i < call->child_count && !any; i++)
        any = holds_struct_call(gen, call->children[i]);
    if (!any) return 0;
    g_view_check = 1;   /* the question below asks is_heap_string_expr */
    int r = call_may_hand_back_args(gen, call);
    g_view_check = 0;
    return r;
}

/* #2582: does a body keep its struct parameter `pname` as a whole value?
 * Any use of the bare name counts but these: the object of an access
 * (`p.name`, `p.items[0].tag`, `p.n = 1`) whose value owns no strings, and
 * an argument of a call that hands back nothing of its arguments
 * (call_may_hand_back_args): such a callee borrows it, and keeps what it
 * keeps by its own entry. Returned (directly, in an `if` or `match` arm, a
 * tuple), aliased (`q = p`), stored, captured by a closure, or read as a
 * field or element that owns strings (`p.inner`, `p.items[0]`), the
 * parameter could reach the caller or outlive the call as a view of the
 * caller's argument, whose strings the caller frees. Conservative: a keep
 * the walk cannot rule out costs a copy of each string field on entry,
 * never a free under someone else. */
static int struct_param_kept_walk(CodeGenerator* gen, ASTNode* n, ASTNode* parent,
                                  const char* pname, int depth) {
    if (!n || depth > 512) return n != NULL;
    int is_access_object = parent && parent->child_count >= 1 && parent->children[0] == n &&
                           (parent->type == AST_MEMBER_ACCESS || parent->type == AST_ARRAY_ACCESS);
    if (n->type == AST_IDENTIFIER && n->value && strcmp(n->value, pname) == 0) {
        if (is_access_object) return 0;   /* judged at the outermost access */
        if (parent && parent->type == AST_FUNCTION_CALL && !call_may_hand_back_args(gen, parent)) {
            const char* fn = parent->value ? codegen_normalise_callee(parent->value) : NULL;
            /* A closure call's slot 0 is the closure invoked, not an argument. */
            if (!(fn && strcmp(fn, "call") == 0 && parent->child_count > 0 &&
                  parent->children[0] == n)) return 0;
        }
        return 1;
    }
    if ((n->type == AST_MEMBER_ACCESS || n->type == AST_ARRAY_ACCESS) && !is_access_object &&
        access_chain_rooted_at(n, pname) && type_owns_strings(gen, n->node_type)) {
        /* A part of the parameter used as a value. Stored into
         * (`p.inner = v`), it is replaced, not kept. */
        int stored = parent && parent->child_count >= 1 && parent->children[0] == n &&
                     (parent->type == AST_ASSIGNMENT ||
                      (parent->type == AST_BINARY_EXPRESSION && parent->value &&
                       strcmp(parent->value, "=") == 0));
        if (!stored) return 1;
    }
    for (int i = 0; i < n->child_count; i++) {
        if (struct_param_kept_walk(gen, n->children[i], n, pname, depth + 1)) return 1;
    }
    return 0;
}

int struct_param_kept(CodeGenerator* gen, ASTNode* body, const char* pname) {
    if (!body || !pname) return 0;
    return struct_param_kept_walk(gen, body, NULL, pname, 0);
}

/* #2582: what a statement's value holds of the struct-returning calls in it.
 * TEMP_FREE: the statement keeps nothing of `e` past its own evaluation (an
 * argument consumed by its call, the object of a read the slot copies): a
 * struct a call returns there is a temporary. TEMP_KEPT: `e` is what the
 * statement keeps (the bound or returned value, an `if` arm or `or` default
 * that becomes it, a struct literal's field, an array element); the slot
 * that takes it copies a view (a field read), so the object of such a read
 * is still a temporary. TEMP_RAW: `e` flows as it is into a kept value
 * (an argument a call may hand back), so neither it nor the struct a read
 * of it reaches into is a temporary. */
enum { TEMP_FREE = 0, TEMP_KEPT = 1, TEMP_RAW = 2 };

static void add_value_temp(ASTNode* e, ASTNode*** nodes, int* count, int* cap) {
    if (*count == *cap) {
        *cap = *cap ? *cap * 2 : 4;
        *nodes = (ASTNode**)aether_xrealloc(*nodes, sizeof(ASTNode*) * (size_t)*cap);
    }
    (*nodes)[(*count)++] = e;
}

/* #2582: collects the struct-returning calls in `e` that are temporaries of
 * the statement, given how much of `e` it keeps. A block, a `match` or a
 * closure holds statements of its own, with temporaries of their own. */
static void collect_value_struct_temps(CodeGenerator* gen, ASTNode* e, int kept,
                                       ASTNode*** nodes, int* count, int* cap) {
    if (!e || e->type == AST_CLOSURE || e->type == AST_BLOCK ||
        e->type == AST_MATCH_STATEMENT) return;
    if (kept != TEMP_FREE) {
        switch (e->type) {
            case AST_IF_EXPRESSION:
                for (int i = 0; i < e->child_count; i++)
                    collect_value_struct_temps(gen, e->children[i], i > 0 ? kept : TEMP_FREE,
                                               nodes, count, cap);
                return;
            case AST_OR_ELSE: case AST_NULL_COALESCE: case AST_VALUE_CAST:
                for (int i = 0; i < e->child_count; i++)
                    collect_value_struct_temps(gen, e->children[i], kept, nodes, count, cap);
                return;
            case AST_STRUCT_LITERAL: case AST_ARRAY_LITERAL:
                for (int i = 0; i < e->child_count; i++) {
                    ASTNode* c = e->children[i];
                    /* A struct literal's field is an AST_ASSIGNMENT named by
                     * the field, holding its value. */
                    if (c && e->type == AST_STRUCT_LITERAL && c->type == AST_ASSIGNMENT) {
                        for (int k = 0; k < c->child_count; k++)
                            collect_value_struct_temps(gen, c->children[k], kept, nodes, count, cap);
                    } else {
                        collect_value_struct_temps(gen, c, kept, nodes, count, cap);
                    }
                }
                return;
            case AST_FUNCTION_CALL: {
                int raw = call_may_hand_back_args(gen, e) && !call_returns_view_of_temp(gen, e);
                for (int i = 0; i < e->child_count; i++)
                    collect_value_struct_temps(gen, e->children[i], raw ? TEMP_RAW : TEMP_FREE,
                                               nodes, count, cap);
                return;
            }
            case AST_MEMBER_ACCESS: case AST_ARRAY_ACCESS:
                if (kept == TEMP_RAW) {
                    for (int i = 0; i < e->child_count; i++)
                        collect_value_struct_temps(gen, e->children[i], i == 0 ? TEMP_RAW : TEMP_FREE,
                                                   nodes, count, cap);
                    return;
                }
                break;
            default:
                break;
        }
    }
    if (e->type == AST_FUNCTION_CALL && e->node_type &&
        e->node_type->kind == TYPE_STRUCT && struct_owning_strings(gen, e->node_type))
        add_value_temp(e, nodes, count, cap);
    for (int i = 0; i < e->child_count; i++)
        collect_value_struct_temps(gen, e->children[i], TEMP_FREE, nodes, count, cap);
}

/* A call with a trailing block (a builder) is emitted twice by the
 * declaration paths, so a temporary in it would be assigned twice. */
static int has_trailing_block_call(ASTNode* e) {
    if (!e || e->type == AST_CLOSURE) return 0;
    if (e->type == AST_FUNCTION_CALL) {
        for (int i = 0; i < e->child_count; i++) {
            ASTNode* c = e->children[i];
            if (c && c->type == AST_CLOSURE && c->value && strcmp(c->value, "trailing") == 0)
                return 1;
        }
    }
    for (int i = 0; i < e->child_count; i++)
        if (has_trailing_block_call(e->children[i])) return 1;
    return 0;
}

/* #2582: the temporaries of a declaration's initializer, an assignment's
 * value or a return's values. The expression statement collects its own
 * (collect_stmt_struct_temps). A return in main() leaves through
 * main_exit, a return lowered from a `match` returns from inside its arms
 * (emit_match_result_value has their temporaries), and a return of a call
 * with no value runs the defers before the call, so those are left as they
 * were. */
static int statement_value_struct_temps(CodeGenerator* gen, ASTNode* stmt,
                                        ASTNode*** nodes, int* count, int* cap) {
    switch (stmt->type) {
        case AST_VARIABLE_DECLARATION:
            if (stmt->child_count < 1 || has_trailing_block_call(stmt->children[0])) return 0;
            collect_value_struct_temps(gen, stmt->children[0], TEMP_KEPT, nodes, count, cap);
            break;
        case AST_ASSIGNMENT:
            if (stmt->child_count < 2 || has_trailing_block_call(stmt->children[1])) return 0;
            collect_value_struct_temps(gen, stmt->children[1], TEMP_KEPT, nodes, count, cap);
            break;
        case AST_TUPLE_DESTRUCTURE: {
            /* `a, b = f(...)`: the last child is the value. */
            ASTNode* v = stmt->child_count >= 2 ? stmt->children[stmt->child_count - 1] : NULL;
            if (!v || has_trailing_block_call(v)) return 0;
            collect_value_struct_temps(gen, v, TEMP_KEPT, nodes, count, cap);
            break;
        }
        case AST_RETURN_STATEMENT:
            if (gen->in_main_function) return 0;
            for (int i = 0; i < stmt->child_count; i++) {
                ASTNode* v = stmt->children[i];
                if (!v || v->type == AST_MATCH_STATEMENT || !v->node_type ||
                    v->node_type->kind == TYPE_VOID || has_trailing_block_call(v)) return 0;
            }
            for (int i = 0; i < stmt->child_count; i++)
                collect_value_struct_temps(gen, stmt->children[i], TEMP_KEPT, nodes, count, cap);
            break;
        default:
            return 0;
    }
    return *count;
}

typedef struct {
    ASTNode** nodes;
    const char** names;
    ASTNode** carriers;
    int count;
    int mapped;
    ASTNode** outer_nodes;
    const char** outer_names;
    int outer_count;
} ValueTemps;

/* #2582: declares each temporary in `nodes` zeroed on a line of its own
 * where code is being emitted, so one a short circuit skips destroys
 * nothing, puts its destroy on the defer stack (a `return` or `break` from
 * inside the value, as from an `or` handler, destroys it on the way out),
 * and maps the calls to their temporaries for the call emitter. Takes
 * ownership of `nodes`. */
static void temps_open(CodeGenerator* gen, ASTNode** nodes, int count, int line, int column,
                       ValueTemps* vt) {
    static int seq = 0;
    memset(vt, 0, sizeof(*vt));
    vt->nodes = nodes;
    vt->count = count;
    vt->names = (const char**)aether_xrealloc(NULL, sizeof(char*) * (size_t)count);
    vt->carriers = (ASTNode**)aether_xrealloc(NULL, sizeof(ASTNode*) * (size_t)count);
    for (int i = 0; i < count; i++) {
        vt->names[i] = cg_internf("_ae_vtmp%d", seq++);
        print_indent(gen);
        fprintf(gen->output, "%s %s = {0};\n", get_c_type(nodes[i]->node_type), vt->names[i]);
        int before = gen->defer_count;
        push_struct_destroy_defer(gen, vt->names[i], nodes[i]->node_type, line, column);
        vt->carriers[i] = gen->defer_count > before ? gen->defer_stack[before] : NULL;
    }
    stmt_struct_temps_get(&vt->outer_nodes, &vt->outer_names, &vt->outer_count);
    stmt_struct_temps_set(vt->nodes, vt->names, vt->count);
    vt->mapped = 1;
}

/* The calls are emitted: later code emitted inside the temporaries' life
 * (a match's arms) maps calls of its own. */
static void temps_unmap(ValueTemps* vt) {
    if (!vt->mapped) return;
    stmt_struct_temps_set(vt->outer_nodes, vt->outer_names, vt->outer_count);
    vt->mapped = 0;
}

/* Each temporary leaves the defer stack and, unless `destroy` is 0 (a
 * return, whose exit already destroyed it with the other defers), is
 * destroyed in place. */
static void temps_close(CodeGenerator* gen, ValueTemps* vt, int destroy) {
    temps_unmap(vt);
    for (int i = vt->count - 1; i >= 0; i--) {
        for (int d = gen->defer_count - 1; vt->carriers[i] && d >= 0; d--) {
            if (gen->defer_stack[d] == vt->carriers[i]) {
                gen->defer_stack[d] = NULL;
                if (d == gen->defer_count - 1) gen->defer_count--;
                break;
            }
        }
        if (destroy) {
            print_indent(gen);
            fprintf(gen->output, "%s_destroy(&%s);\n",
                    struct_owning_strings(gen, vt->nodes[i]->node_type), vt->names[i]);
        }
    }
    while (gen->defer_count > 0 && !gen->defer_stack[gen->defer_count - 1] &&
           (gen->scope_depth <= 0 ||
            gen->defer_count > gen->scope_defer_start[gen->scope_depth - 1]))
        gen->defer_count--;
    free(vt->nodes);
    free(vt->names);
    free(vt->carriers);
    vt->nodes = NULL;
    vt->count = 0;
}

/* #2582: the temporaries of `e`, given how much of it the code around keeps,
 * opened where code is being emitted. 0 when there are none. */
static int value_temps_open_expr(CodeGenerator* gen, ASTNode* e, int kept, ValueTemps* vt) {
    ASTNode** nodes = NULL;
    int count = 0, cap = 0;
    collect_value_struct_temps(gen, e, kept, &nodes, &count, &cap);
    if (count == 0) {
        free(nodes);
        memset(vt, 0, sizeof(*vt));
        return 0;
    }
    temps_open(gen, nodes, count, e->line, e->column, vt);
    return 1;
}

/* #2582: a declaration's, an assignment's, a tuple destructure's or a
 * return's temporaries, opened just ahead of the statement. */
static int value_temps_open(CodeGenerator* gen, ASTNode* stmt, ValueTemps* vt) {
    memset(vt, 0, sizeof(*vt));
    if (stmt->type != AST_VARIABLE_DECLARATION && stmt->type != AST_ASSIGNMENT &&
        stmt->type != AST_TUPLE_DESTRUCTURE && stmt->type != AST_RETURN_STATEMENT) return 0;
    ASTNode** nodes = NULL;
    int count = 0, cap = 0;
    if (statement_value_struct_temps(gen, stmt, &nodes, &count, &cap) == 0) {
        free(nodes);
        return 0;
    }
    temps_open(gen, nodes, count, stmt->line, stmt->column, vt);
    return 1;
}

static void value_temps_close(CodeGenerator* gen, ASTNode* stmt, ValueTemps* vt) {
    temps_close(gen, vt, stmt->type != AST_RETURN_STATEMENT);
}

/* #2582: an `if` or loop condition. Its value is a bool, which holds nothing
 * of a struct a call in it returns, so each such struct is destroyed as
 * soon as the condition is evaluated, every time it is: the condition
 * becomes a statement expression holding the temporaries. */
static void emit_condition(CodeGenerator* gen, ASTNode* cond) {
    ASTNode** nodes = NULL;
    int count = 0, cap = 0;
    collect_value_struct_temps(gen, cond, TEMP_FREE, &nodes, &count, &cap);
    gen->in_condition = 1;
    if (count == 0) {
        free(nodes);
        generate_expression(gen, cond);
        gen->in_condition = 0;
        return;
    }
    static int seq = 0;
    int id = seq++;
    const char** names = (const char**)aether_xrealloc(NULL, sizeof(char*) * (size_t)count);
    fprintf(gen->output, "({ ");
    for (int i = 0; i < count; i++) {
        names[i] = cg_internf("_ae_ctmp%d_%d", id, i);
        fprintf(gen->output, "%s %s = {0}; ", get_c_type(nodes[i]->node_type), names[i]);
    }
    ASTNode** outer_nodes;
    const char** outer_names;
    int outer_count;
    stmt_struct_temps_get(&outer_nodes, &outer_names, &outer_count);
    stmt_struct_temps_set(nodes, names, count);
    fprintf(gen->output, "int _ae_cv%d = !!(", id);
    generate_expression(gen, cond);
    fprintf(gen->output, "); ");
    stmt_struct_temps_set(outer_nodes, outer_names, outer_count);
    for (int i = 0; i < count; i++) {
        fprintf(gen->output, "%s_destroy(&%s); ",
                struct_owning_strings(gen, nodes[i]->node_type), names[i]);
    }
    fprintf(gen->output, "_ae_cv%d; })", id);
    gen->in_condition = 0;
    free(nodes);
    free(names);
}

/* #2497: the struct counterpart of emit_string_take. A struct that owns
 * heap strings (directly or in a struct field held by value) is stored into
 * an owning slot (a local, a field, a match result, a return value) by
 * taking it: a fresh value (a struct literal, a call) is adopted; a struct
 * a local owns is moved out of it on its last use (its trackers cleared, so
 * its scope exit releases nothing); anything else that only views a struct
 * owned elsewhere (a parameter, a variable still in use, a field, an
 * element) is copied with `<Name>_dup`. Storing the view as it was made two
 * owners of every string: `b = a` freed `a`'s strings twice. */
int struct_take_shape(ASTNode* e) {
    return e && (e->type == AST_IDENTIFIER || e->type == AST_MEMBER_ACCESS ||
                 e->type == AST_ARRAY_ACCESS || e->type == AST_IF_EXPRESSION);
}

/* Does local `name` own its struct value: is its scope-exit destroy
 * pending? Parameters, pattern bindings and other copies do not. */
static int struct_local_owns(CodeGenerator* gen, const char* name) {
    size_t n = strlen(name);
    for (int i = 0; i < gen->defer_count; i++) {
        ASTNode* d = gen->defer_stack[i];
        const char* a = d ? d->annotation : NULL;
        if (a && strncmp(a, "struct_destroy:", 15) == 0 &&
            strncmp(a + 15, name, n) == 0 && a[15 + n] == ':') return 1;
    }
    return 0;
}

void emit_struct_take(CodeGenerator* gen, ASTNode* e, const char* sname,
                      const char* target) {
    if (e && e->type == AST_IF_EXPRESSION && e->child_count >= 3) {
        fprintf(gen->output, "((");
        generate_expression(gen, e->children[0]);
        fprintf(gen->output, ") ? ");
        emit_struct_take(gen, e->children[1], sname, target);
        fprintf(gen->output, " : ");
        emit_struct_take(gen, e->children[2], sname, target);
        fprintf(gen->output, ")");
        return;
    }
    if (!struct_take_shape(e) ||
        (e->type == AST_IDENTIFIER && target && e->value && strcmp(e->value, target) == 0)) {
        generate_expression(gen, e);
        return;
    }
    if (e->type == AST_IDENTIFIER && e->value && struct_local_owns(gen, e->value) &&
        !is_promoted_capture(gen, e->value) && !is_env_capture_name(gen, e->value) &&
        !alias_source_must_copy(gen, e->value)) {
        ASTNode* sdef = find_struct_definition_by_name(gen->program, sname);
        fprintf(gen->output, "({ %s _ae_mv = %s; ", sname, e->value);
        emit_struct_disown_fields(gen, sdef, e->value, 0, 0);
        fprintf(gen->output, "_ae_mv; })");
        return;
    }
    fprintf(gen->output, "%s_dup(", sname);
    generate_expression(gen, e);
    fprintf(gen->output, ")");
}

/* #2525: is `e` a closure value nobody else holds: a closure literal, or a
 * call that hands one over (call_returns_owned_closure)? An `if` over two
 * such is one too. */
static int closure_value_is_fresh(CodeGenerator* gen, ASTNode* e) {
    if (!e) return 0;
    if (env_scan_is_real_closure(e)) return 1;
    /* #2528: a reply's closure was taken for the asker. */
    if (e->type == AST_SEND_ASK) return 1;
    if (e->type == AST_FUNCTION_CALL) return call_returns_owned_closure(gen, e);
    if (e->type == AST_IF_EXPRESSION && e->child_count >= 3) {
        return closure_value_is_fresh(gen, e->children[1]) &&
               closure_value_is_fresh(gen, e->children[2]);
    }
    return 0;
}

/* #2525: the closure counterpart of emit_string_take, for a slot that holds
 * a reference of its own (a struct or message field, a global, an actor's
 * state). A fresh value's reference is adopted; a view of a closure held
 * elsewhere (a local, a parameter, a field, an element, a call returning a
 * borrowed one) is retained, and its holder keeps releasing its own. A bare
 * named function has no env: nothing to retain. An `if` over values takes
 * each branch on its own. */
void emit_closure_take(CodeGenerator* gen, ASTNode* e) {
    if (e && e->type == AST_IF_EXPRESSION && e->child_count >= 3 &&
        !closure_value_is_fresh(gen, e)) {
        fprintf(gen->output, "((");
        generate_expression(gen, e->children[0]);
        fprintf(gen->output, ") ? ");
        emit_closure_take(gen, e->children[1]);
        fprintf(gen->output, " : ");
        emit_closure_take(gen, e->children[2]);
        fprintf(gen->output, ")");
        return;
    }
    int bare_fn = e && e->type == AST_IDENTIFIER && e->value && gen->program &&
                  !is_var_declared(gen, e->value) &&
                  find_function_definition_by_name(gen->program, e->value);
    if (closure_value_is_fresh(gen, e) || bare_fn) {
        generate_expression(gen, e);
        return;
    }
    fprintf(gen->output, "_aether_closure_retain(");
    generate_expression(gen, e);
    fprintf(gen->output, ")");
}

/* A promoted struct returned by name hands its owned strings to the caller
 * with the returned copy (#752). The cell is still released at the scope
 * exit, and a closure env may hold it longer: it must not free them too. */
static void disown_returned_promoted_struct(CodeGenerator* gen, ASTNode* expr) {
    if (!expr || expr->type != AST_IDENTIFIER || !expr->value ||
        !is_promoted_capture(gen, expr->value)) return;
    const char* sname = struct_owning_strings(gen, expr->node_type);
    if (!sname) return;
    const char* lv = cg_internf("(*%s)", expr->value);
    /* The cell keeps its closure references and the returned copy takes
     * ones of its own (#2525): both are released by their holders. */
    emit_struct_disown(gen, sname, lv, 1);
}

/* #752 (caller side): a struct local that RECEIVES ownership of a
 * returned struct — a tuple-unpack target, or a local initialised from a
 * struct-returning call — owns that struct's heap-string fields and must
 * free them at scope exit. Push the same `<Struct>_destroy` defer the
 * struct-literal declaration path uses. The callee already transferred
 * ownership (its own destroy was suppressed via mark_return_escaped_
 * struct_var), so this is the single owner; no double-free. Gated on the
 * struct actually having heap-string fields (else the defer is a no-op
 * we skip emitting). */
void push_struct_destroy_defer(CodeGenerator* gen, const char* var_name,
                                      Type* struct_type, int line, int col) {
    if (!gen->program || !var_name || !struct_type ||
        struct_type->kind != TYPE_STRUCT || !struct_type->struct_name) return;
    ASTNode* sdef = find_struct_definition_by_name(gen->program, struct_type->struct_name);
    if (!sdef || !struct_owns_heap_strings(gen, sdef)) return;
    ASTNode* carrier = create_ast_node(AST_EXPRESSION_STATEMENT, NULL, line, col);
    if (carrier) {
        if (carrier->annotation) free(carrier->annotation);
        carrier->annotation = heap_strf("struct_destroy:%s:%s",
                                        var_name, struct_type->struct_name);
        codegen_own_node(gen, carrier);
        push_defer(gen, carrier);
    }
}

// #893: innermost enclosing loop whose source label matches `name`, or -1.
// Searches the loop-nest stack from the inside out, so a reused label binds
// to the nearest loop (the standard rule).
static int find_labeled_loop_level(CodeGenerator* gen, const char* name) {
    if (!name) return -1;
    for (int d = gen->loop_nest_depth - 1; d >= 0; d--) {
        if (gen->loop_label[d] && strcmp(gen->loop_label[d], name) == 0) return d;
    }
    return -1;
}

// #340: emit `value` coerced to the optional type `target`. A bare value of
// the inner type is implicitly wrapped (`{.has=1, .val=value}`); `none`
// becomes `{0}`; an expression already of optional type passes through. Used
// wherever a value flows into a `T?` slot — var-decl init, assignment, return,
// call argument.
void emit_optional_coerced(CodeGenerator* gen, ASTNode* value, Type* target) {
    if (!value || !target || target->kind != TYPE_OPTIONAL) {
        if (value) generate_expression(gen, value);
        return;
    }
    const char* tc = get_c_type(target);
    if (value->type == AST_NONE_LITERAL) {
        fprintf(gen->output, "(%s){0}", tc);
        return;
    }
    if (value->node_type && value->node_type->kind == TYPE_OPTIONAL) {
        generate_expression(gen, value);   // already an optional
        return;
    }
    fprintf(gen->output, "(%s){ .has = 1, .val = ", tc);
    generate_expression(gen, value);
    fprintf(gen->output, " }");
}

static void emit_match_result_value(CodeGenerator* gen, ASTNode* result);

// #2459: a match arm's block body is its own scope, the way an `if` / `switch`
// arm's block is (AST_BLOCK): its defers run when the arm ends, only when that
// arm was taken, and the names it declares do not leak into the next arm. The
// caller has already opened the arm's C braces.
//
// #2496: when the match is an expression, the block yields its final value
// (match_arm_value) to the match's result, inside the block's scope so its
// locals are still live and before its defers run, as a return's value is
// taken before a function's. The block's other statements are statements:
// a `match` among them is not the outer match's value, so the result
// variable is out of reach while they are emitted.
static void emit_match_arm_block(CodeGenerator* gen, ASTNode* block) {
    int saved_var_count = gen->declared_var_count;
    const char* result_var = gen->match_result_var;
    const char* result_own = gen->match_result_own;
    const char* result_struct = gen->match_result_struct;
    ASTNode* value = result_var ? match_arm_value(block) : NULL;
    int stmts = block->child_count - (value ? 1 : 0);
    gen->match_result_var = NULL;
    gen->match_result_own = NULL;
    gen->match_result_struct = NULL;
    enter_scope(gen);
    for (int j = 0; j < stmts; j++) {
        generate_statement(gen, block->children[j]);
    }
    gen->match_result_var = result_var;
    gen->match_result_own = result_own;
    gen->match_result_struct = result_struct;
    if (value && value->type == AST_MATCH_STATEMENT) {
        /* A nested match yields for this one: its arms assign the same
         * result. */
        generate_statement(gen, value);
    } else if (value) {
        emit_match_result_value(gen, value);
    }
    gen->match_result_var = NULL;
    gen->match_result_own = NULL;
    gen->match_result_struct = NULL;
    exit_scope(gen);
    gen->match_result_var = result_var;
    gen->match_result_own = result_own;
    gen->match_result_struct = result_struct;
    truncate_declared_vars(gen, saved_var_count);
}

// A match arm's value: assigned to the match's result variable when the
// match is an expression, else evaluated as a statement. #2461: an owning
// string result takes the value (emit_string_take) and records in the
// match's flag whether it owns it.
static void emit_match_result_value_now(CodeGenerator* gen, ASTNode* result);

/* #2582: an arm's value, with the temporaries of the structs calls in it
 * return, destroyed once the match's result has taken what it keeps. */
static void emit_match_result_value(CodeGenerator* gen, ASTNode* result) {
    ValueTemps vt;
    int has = value_temps_open_expr(gen, result,
                                    gen->match_result_var ? TEMP_KEPT : TEMP_FREE, &vt);
    emit_match_result_value_now(gen, result);
    if (has) temps_close(gen, &vt, 1);
}

static void emit_match_result_value_now(CodeGenerator* gen, ASTNode* result) {
    print_indent(gen);
    if (gen->match_result_var && gen->match_result_cell) {
        /* #2514: a closure's variable takes the arm's value in its cell. */
        emit_cell_store(gen, gen->match_result_var, gen->match_result_cell, result);
        return;
    }
    if (gen->match_result_var && gen->match_result_struct) {
        /* #2497: an owning struct result takes the arm's struct. */
        if (gen->match_result_replace) {
            fprintf(gen->output, "%s_replace(&%s, ", gen->match_result_struct,
                    gen->match_result_var);
        } else {
            fprintf(gen->output, "%s = ", gen->match_result_var);
        }
        emit_struct_take(gen, result, gen->match_result_struct, gen->match_result_var);
        fprintf(gen->output, gen->match_result_replace ? ");\n" : ";\n");
        return;
    }
    if (gen->match_result_var && gen->match_result_own) {
        fprintf(gen->output, "%s = ", gen->match_result_var);
        emit_string_take(gen, result, gen->match_result_own, gen->match_result_var);
        fprintf(gen->output, ";\n");
        return;
    }
    if (gen->match_result_var) {
        fprintf(gen->output, "%s = ", gen->match_result_var);
    }
    generate_expression(gen, result);
    fprintf(gen->output, ";\n");
}

// #340: emit a match arm's result body — mirrors the generic match dispatch
// (block / statement / expression), including match-as-expression's
// `match_result_var` assignment so `let r = match m { ... }` works.
static void emit_opt_match_arm(CodeGenerator* gen, ASTNode* result) {
    if (!result) return;
    if (result->type == AST_BLOCK) {
        emit_match_arm_block(gen, result);
    } else if (result->type == AST_PRINT_STATEMENT ||
               result->type == AST_RETURN_STATEMENT ||
               result->type == AST_VARIABLE_DECLARATION) {
        generate_statement(gen, result);
    } else {
        emit_match_result_value(gen, result);
    }
}

// Emit a function's return value. #340: when the return type is `T?`,
// coerce a bare value / `none` into the optional (`return 5` / `return none`).
// Otherwise use the uniform-heap-return wrap or the plain expression.
static void emit_return_value(CodeGenerator* gen, ASTNode* stmt) {
    if (!stmt || stmt->child_count == 0) return;
    if (gen->current_func_return_type &&
        gen->current_func_return_type->kind == TYPE_OPTIONAL &&
        needs_optional_coerce(stmt->children[0], gen->current_func_return_type)) {
        emit_optional_coerced(gen, stmt->children[0], gen->current_func_return_type);
        return;
    }
    // #914: `return Circle {...}` from a `-> Shape` function wraps the variant.
    if (gen->current_func_return_type &&
        gen->current_func_return_type->kind == TYPE_SUM &&
        needs_sum_coerce(stmt->children[0], gen->current_func_return_type)) {
        emit_sum_coerced(gen, stmt->children[0], gen->current_func_return_type);
        return;
    }
    // #913: `return value` from a `T!` function wraps the success value into
    // the `(value, "")` result tuple. An explicit `return value, "err"` is a
    // 2-value return handled by the multi-return path and never reaches here;
    // a value that is already the result tuple (forwarding another result)
    // passes through unchanged.
    if (gen->current_func_return_type &&
        gen->current_func_return_type->is_result) {
        ASTNode* v = stmt->children[0];
        if (!(v->node_type && v->node_type->kind == TYPE_TUPLE)) {
            fprintf(gen->output, "(%s){ ._0 = ",
                    get_c_type(gen->current_func_return_type));
            /* When position 0 of this `T!` is classified heap (some other
             * return yields a heap value there), EVERY value-slot return —
             * including this single-value auto-wrap of a literal `""` — must
             * be uniform-heap-wrapped, exactly as the multi-value
             * `emit_tuple_return_position` does. Otherwise a bare `return
             * ""` hands the caller a `.rodata` literal that its `_heap_`
             * tracker (set because the position is heap) frees at scope
             * exit → invalid free. Gate on the same classifier the caller's
             * destructure uses, and only for a string value slot. */
            Type* v0 = gen->current_func_return_type->tuple_types
                       ? gen->current_func_return_type->tuple_types[0] : NULL;
            int wrap_v0 = v0 && v0->kind == TYPE_STRING &&
                          gen->current_function &&
                          function_def_returns_heap_at(gen, gen->current_function, 0);
            if (wrap_v0 && string_take_is_view(gen, v)) {
                /* #2497: an `if` or field read is taken as a return takes
                 * it: a fresh arm adopted, not copied and leaked. */
                emit_string_take_owned(gen, v, 0);
            } else if (wrap_v0) {
                fprintf(gen->output, "aether_uniform_heap_str((const char*)(");
                generate_expression(gen, v);
                fprintf(gen->output, "), %d)",
                        is_heap_string_expr(gen, v) ? 1 : 0);
            } else {
                generate_expression(gen, v);
            }
            fprintf(gen->output, ", ._1 = \"\" }");
            return;
        }
    }
    /* #2497: a struct field or element returned is still owned by its
     * holder, which may be released at this function's exit; the caller
     * adopts what it gets, so it gets a copy. */
    {
        ASTNode* v = stmt->children[0];
        /* A closure body has no declared return type here: its value's
         * type says. */
        Type* srt = gen->current_func_return_type ? gen->current_func_return_type
                                                   : (v ? v->node_type : NULL);
        const char* rs = struct_owning_strings(gen, srt);
        /* An `if` is taken arm by arm (#2582): an arm naming a local or a
         * parameter that owns its strings moves them out, or copies them,
         * so the exit's destroy of it does not free what is returned. A
         * struct a closure captured is its environment's, which frees it
         * when the closure goes: returned, the caller gets a copy. */
        int captured = v && v->type == AST_IDENTIFIER && v->value &&
                       (is_env_capture_name(gen, v->value) ||
                        is_alias_capture_name(gen, v->value));
        if (rs && v && (v->type == AST_MEMBER_ACCESS || v->type == AST_ARRAY_ACCESS ||
                        v->type == AST_IF_EXPRESSION || captured)) {
            emit_struct_take(gen, v, rs, NULL);
            return;
        }
        /* #2525: a closure read out of a field or an element is held by
         * its holder, which may be destroyed at this exit; the caller gets
         * a reference of its own (returns_owned_in counts the return as
         * handing one over). */
        Type* rt = gen->current_func_return_type;
        if (rt && rt->kind == TYPE_FUNCTION && !rt->is_fnptr && v &&
            (v->type == AST_MEMBER_ACCESS || v->type == AST_ARRAY_ACCESS)) {
            fprintf(gen->output, "_aether_closure_retain(");
            generate_expression(gen, v);
            fprintf(gen->output, ")");
            return;
        }
    }
    if (should_uniform_heap_return(gen, stmt)) {
        emit_uniform_heap_return_expr(gen, stmt->children[0]);
    } else if (gen->current_func_return_type &&
               emit_int_ptr_bridged(gen, stmt->children[0],
                                    gen->current_func_return_type->kind)) {
        /* #2218: int <-> ptr return, cast emitted. */
    } else {
        generate_expression(gen, stmt->children[0]);
    }
}

// #340: does `value` need optional coercion to flow into `target`? True when
// target is `T?` and value isn't already that optional (a bare T, or `none`).
int needs_optional_coerce(ASTNode* value, Type* target) {
    if (!value || !target || target->kind != TYPE_OPTIONAL) return 0;
    if (value->type == AST_NONE_LITERAL) return 1;
    if (value->node_type && value->node_type->kind == TYPE_OPTIONAL) return 0;
    return 1;
}

// #914: does `value` need sum coercion to flow into `target`? True when target
// is a sum and value is one of its variant structs (not already the sum).
int needs_sum_coerce(ASTNode* value, Type* target) {
    if (!value || !target || target->kind != TYPE_SUM) return 0;
    Type* vt = value->node_type;
    if (!vt || vt->kind != TYPE_STRUCT || !vt->struct_name) return 0;
    for (int i = 0; i < target->tuple_count; i++) {
        Type* var = target->tuple_types[i];
        if (var && var->struct_name &&
            strcmp(var->struct_name, vt->struct_name) == 0) return 1;
    }
    return 0;
}

// #914: wrap a variant struct value into its sum:
//   (Shape){ .tag = Shape__Circle, .data.Circle_ = <value> }
// If `value` isn't a coercible variant, emit it bare.
void emit_sum_coerced(CodeGenerator* gen, ASTNode* value, Type* target) {
    if (!needs_sum_coerce(value, target)) {
        if (value) generate_expression(gen, value);
        return;
    }
    const char* sname = target->struct_name;
    const char* variant = value->node_type->struct_name;
    fprintf(gen->output, "(%s){ .tag = %s__%s, .data.%s_ = ",
            sname, sname, variant, variant);
    generate_expression(gen, value);
    fprintf(gen->output, " }");
}

// #340: optional-chain assignment `recv?.field = rhs` — a no-op when the
// optional is `none`, else writes the field. Returns 1 if it emitted the
// store (so the caller skips its normal assignment path), 0 otherwise.
// `&(recv)` evaluates the receiver once and works for any lvalue receiver
// (variable / array element / struct field); the `->val` access uses
// `->`/`.` per whether the wrapped type is a pointer-to-struct or a value
// struct. Both assignment shapes the parser can produce route here:
// AST_ASSIGNMENT and AST_EXPRESSION_STATEMENT > AST_BINARY_EXPRESSION(`=`).
static int emit_optional_chain_assign(CodeGenerator* gen, ASTNode* lhs, ASTNode* rhs) {
    if (!lhs || lhs->type != AST_OPTIONAL_CHAIN || !lhs->value ||
        lhs->child_count == 0)
        return 0;
    ASTNode* recv = lhs->children[0];
    Type* ot = recv ? recv->node_type : NULL;
    if (!ot || ot->kind != TYPE_OPTIONAL) return 0;
    Type* inner = ot->element_type;
    const char* acc = (inner && inner->kind == TYPE_PTR) ? "->" : ".";
    static int oca_counter = 0;
    int id = oca_counter++;
    fprintf(gen->output, "{ %s* _oca%d = &(", get_c_type(ot), id);
    generate_expression(gen, recv);
    fprintf(gen->output, "); if (_oca%d->has) _oca%d->val%s%s = ",
            id, id, acc, lhs->value);
    generate_expression(gen, rhs);
    fprintf(gen->output, "; }\n");
    return 1;
}

/* #1860: does every `return` in this function hand back a heap.new(T) box?
 * Box provenance was per-function, so `b = build_box()` lost the fact that the
 * value is a zero-initialised box and the field-assign wrapper was skipped:
 * `b.name = ...` became a bare store that never set `_heap_<field>`, so the
 * box's destructor believed it owned nothing and every owned string field
 * leaked. Requiring ALL returns to be heap.new keeps the #790 guard exactly as
 * strict: a function that can also hand back a raw `malloc as *T` (garbage
 * trackers) does not qualify. */
static int returns_only_heap_new(ASTNode* node, int* saw_return) {
    if (!node) return 1;
    if (node->type == AST_RETURN_STATEMENT) {
        *saw_return = 1;
        if (node->child_count == 0 || !node->children[0]) return 0;
        return node->children[0]->type == AST_HEAP_NEW;
    }
    for (int i = 0; i < node->child_count; i++) {
        if (!returns_only_heap_new(node->children[i], saw_return)) return 0;
    }
    return 1;
}

static int call_yields_heap_box(CodeGenerator* gen, ASTNode* init) {
    if (!gen || !gen->program || !init) return 0;
    if (init->type != AST_FUNCTION_CALL || !init->value) return 0;
    for (int i = 0; i < gen->program->child_count; i++) {
        ASTNode* fn = gen->program->children[i];
        if (!fn || fn->type != AST_FUNCTION_DEFINITION) continue;
        if (!fn->value || strcmp(fn->value, init->value) != 0) continue;
        if (fn->child_count == 0) return 0;
        ASTNode* body = fn->children[fn->child_count - 1];
        int saw_return = 0;
        int ok = returns_only_heap_new(body, &saw_return);
        return ok && saw_return;
    }
    return 0;
}

/* #2220 — observable struct models.
 *
 * A statement that stores into a field (`m.f = v`, `p.f = v`, `a.b.c = v`,
 * and the compound forms the parser desugars to them) is followed by one
 * `aether_observe_notify(<object>)` per observable object whose bytes the
 * store changed, innermost first:
 *   - a value-typed base (`m` : T, or `a.b` : T) is notified by address
 *     (`&m`), and the walk continues outward because the enclosing value's
 *     bytes changed too;
 *   - a pointer-typed base (`p` : *T) is notified by the pointer itself, and
 *     the walk stops: the holder of the pointer did not change.
 * The hook runs after the statement was emitted whichever assignment path
 * emitted it (heap-string tracking, @c_struct overlays, trailing-block
 * builders), which is why it lives at the generate_statement boundary rather
 * than inside each path. */
int struct_is_observable(CodeGenerator* gen, const char* struct_name) {
    if (!gen || !struct_name) return 0;
    ASTNode* sd = find_struct_definition_by_name(gen->program, struct_name);
    return sd && annotation_has_marker(sd->annotation, "observable");
}

/* The field-store LHS of `stmt`, or NULL when the statement is not one. */
static ASTNode* field_store_lhs(ASTNode* stmt) {
    if (!stmt) return NULL;
    ASTNode* lhs = NULL;
    if (stmt->type == AST_ASSIGNMENT && stmt->child_count >= 2) {
        lhs = stmt->children[0];
    } else if (stmt->type == AST_EXPRESSION_STATEMENT && stmt->child_count > 0) {
        ASTNode* inner = stmt->children[0];
        if (inner && inner->type == AST_BINARY_EXPRESSION && inner->value &&
            strcmp(inner->value, "=") == 0 && inner->child_count == 2) {
            lhs = inner->children[0];
        }
    }
    return (lhs && lhs->type == AST_MEMBER_ACCESS && lhs->child_count >= 1) ? lhs : NULL;
}

/* The index of field `field` in struct `struct_name`, in declaration order
 * (0 for the first), or -1 (any field) when it is not one of its fields. */
static int observable_field_index(CodeGenerator* gen, const char* struct_name,
                                  const char* field) {
    ASTNode* sd = find_struct_definition_by_name(gen->program, struct_name);
    if (!sd || !field) return -1;
    int idx = 0;
    for (int i = 0; i < sd->child_count; i++) {
        ASTNode* f = sd->children[i];
        if (!f || f->type != AST_STRUCT_FIELD) continue;
        if (f->value && strcmp(f->value, field) == 0) return idx;
        idx++;
    }
    return -1;
}

/* Notify `base`'s observers that its field `field` (an index) changed
 * (#2299: the field, so a replicator or undo log knows which). */
static void emit_observe_notify(CodeGenerator* gen, ASTNode* base, int by_address, int field) {
    print_indent(gen);
    fprintf(gen->output, "aether_observe_notify_field(%s(", by_address ? "&" : "");
    int saved = gen->generating_lvalue;
    gen->generating_lvalue = 1;
    generate_expression(gen, base);
    gen->generating_lvalue = saved;
    fprintf(gen->output, "), %d);\n", field);
}

static void emit_observable_store_notify(CodeGenerator* gen, ASTNode* stmt) {
    ASTNode* node = field_store_lhs(gen ? stmt : NULL);
    /* At each level `node` is `base.field`: the field of `base` that holds
     * what changed, so an outer object hears which of its own fields did. */
    while (node && node->type == AST_MEMBER_ACCESS && node->child_count >= 1) {
        ASTNode* base = node->children[0];
        Type* bt = base ? base->node_type : NULL;
        if (bt && bt->kind == TYPE_STRUCT) {
            if (struct_is_observable(gen, bt->struct_name))
                emit_observe_notify(gen, base, 1,
                                    observable_field_index(gen, bt->struct_name, node->value));
            node = base;   /* the enclosing value changed too */
            continue;
        }
        if (bt && bt->kind == TYPE_PTR && bt->element_type &&
            bt->element_type->kind == TYPE_STRUCT) {
            const char* sn = bt->element_type->struct_name;
            if (struct_is_observable(gen, sn))
                emit_observe_notify(gen, base, 0, observable_field_index(gen, sn, node->value));
        }
        break;
    }
}

static void generate_statement_body(CodeGenerator* gen, ASTNode* stmt);

/* The call whose trailing block this statement runs itself: a declaration's
 * initializer, an assignment's right side, an expression statement's call.
 * generate_expression lowers a trailing call anywhere else (a return value,
 * an argument) on its own; this tells it which one not to. */
static ASTNode* stmt_trailing_call(CodeGenerator* gen, ASTNode* stmt) {
    ASTNode* call = NULL;
    if (stmt->type == AST_VARIABLE_DECLARATION && stmt->child_count > 0) {
        call = stmt->children[0];
    } else if (stmt->type == AST_ASSIGNMENT && stmt->child_count >= 2) {
        call = stmt->children[1];
    } else if (stmt->type == AST_EXPRESSION_STATEMENT && stmt->child_count > 0) {
        call = stmt->children[0];
        if (call && call->type == AST_BINARY_EXPRESSION && call->value &&
            strcmp(call->value, "=") == 0 && call->child_count == 2) {
            call = call->children[1];
        }
    }
    if (!call || call->type != AST_FUNCTION_CALL) return NULL;
    return trailing_dsl_block(gen, call) ? call : NULL;
}

void generate_statement(CodeGenerator* gen, ASTNode* stmt) {
    if (!stmt) return;
    if (g_env_own_clear_count) emit_env_own_clears(gen, stmt);   /* #2506 */
    ASTNode* saved_trailing = gen->trailing_stmt_call;
    gen->trailing_stmt_call = stmt_trailing_call(gen, stmt);
    ValueTemps vt;
    int has_vt = value_temps_open(gen, stmt, &vt);   /* #2582 */
    int escaped_mark = gen->return_escaped_struct_var_count;
    int order_depth = order_prelude_depth();   /* #2478 */
    generate_statement_body(gen, stmt);
    order_prelude_end(order_depth);
    if (has_vt) value_temps_close(gen, stmt, &vt);
    /* #752 marks a struct local a return hands over, so that return's exit
     * does not destroy it. The mark is that return's alone (#2582): kept for
     * the rest of the function, it skipped the destroy on every later exit,
     * so a struct returned on one path leaked on the others. */
    if (stmt->type == AST_RETURN_STATEMENT) {
        while (gen->return_escaped_struct_var_count > escaped_mark) {
            gen->return_escaped_struct_var_count--;
            free(gen->return_escaped_struct_vars[gen->return_escaped_struct_var_count]);
        }
    }
    gen->trailing_stmt_call = saved_trailing;
    emit_observable_store_notify(gen, stmt);
}

static void generate_statement_body(CodeGenerator* gen, ASTNode* stmt) {
    if (!stmt) return;

    codegen_note_diag_pos(stmt);

    // Emit `#line N "src.ae"` so gcc errors, gdb breakpoints, and
    // gcov reports reference the .ae source the user wrote, not the
    // mid-file position of the merged .c output. Dedup'd inside
    // codegen_maybe_emit_line — back-to-back statements on the same
    // source line emit one directive, not two.
    codegen_maybe_emit_line(gen, stmt);

    switch (stmt->type) {
        case AST_CONST_DECLARATION: {
            // Local constant: const <type> <name> = <value>;
            // or const arr[] = [v1, v2, ...];
            if (stmt->value && stmt->child_count > 0) {
                mark_var_declared(gen, stmt->value);
                if (stmt->annotation && strcmp(stmt->annotation, "array_const") == 0) {
                    // static const T NAME[] = {v1, v2, ...};
                    Type* elem_type = (stmt->node_type && stmt->node_type->element_type)
                                      ? stmt->node_type->element_type : NULL;
                    const char* ctype = elem_type ? const_array_elem_c_type(elem_type) : "const char*";
                    fprintf(gen->output, "static const %s %s[] = ", ctype, stmt->value);
                    generate_expression(gen, stmt->children[0]);
                    fprintf(gen->output, ";\n");
                } else {
                    Type* var_type = stmt->node_type;
                    if ((!var_type || var_type->kind == TYPE_VOID || var_type->kind == TYPE_UNKNOWN)
                        && stmt->children[0] && stmt->children[0]->node_type) {
                        var_type = stmt->children[0]->node_type;
                    }
                    // STRING already emits "const char*", skip extra const qualifier
                    if (var_type && var_type->kind == TYPE_STRING) {
                        generate_type(gen, var_type);
                    } else {
                        fprintf(gen->output, "const ");
                        generate_type(gen, var_type);
                    }
                    fprintf(gen->output, " %s = ", stmt->value);
                    generate_expression(gen, stmt->children[0]);
                    fprintf(gen->output, ";\n");
                }
            }
            break;
        }
        case AST_TUPLE_DESTRUCTURE: {
            // a, b = func() — last child is RHS, others are variable declarations
            if (stmt->child_count < 2) break;
            int var_count = stmt->child_count - 1;
            ASTNode* rhs = stmt->children[var_count];

            // Infer tuple type from RHS
            Type* rhs_type = rhs->node_type;
            if (rhs_type && rhs_type->kind == TYPE_TUPLE) {
                ensure_tuple_typedef(gen, rhs_type);
            }

            // Generate: _tuple_X_Y _tmp = func();
            const char* tuple_type_name = rhs_type ? get_c_type(rhs_type) : "_tuple_unknown";
            static int tuple_tmp_counter = 0;
            int tmp_id = tuple_tmp_counter++;
            print_indent(gen);
            fprintf(gen->output, "%s _tup%d = ", tuple_type_name, tmp_id);
            generate_expression(gen, rhs);
            fprintf(gen->output, ";\n");

            /* Per-position heap-ness lookup for the destructure
             * (issue #420). For each tuple position `j`, decide
             * whether the source value at that position is a fresh
             * heap allocation that the destructured LHS now owns.
             * Computed once up-front so the per-LHS loop can route
             * correctly:
             *
             *   - User-defined tuple-returning fn: walk return-sites
             *     via `function_def_returns_heap_at`, AND-fold per
             *     position. Memoised on the fn's annotation slot.
             *   - Extern or any other RHS: read the per-position
             *     `tuple_heap_flags[j]` populated by the parser
             *     when an `@heap` annotation is in scope. NULL
             *     flags ⇒ all 0 (borrow) — preserves the silent
             *     pre-#420 behaviour for unannotated externs.
             *
             * Heap classification is only meaningful for TYPE_STRING
             * positions; non-string positions keep their plain
             * assignment shape regardless of the flag. */
            ASTNode* callee_def = NULL;
            if (rhs && rhs->type == AST_FUNCTION_CALL && rhs->value) {
                /* Source-level callees land in the AST in dotted form
                 * (`"json.get_string"`) but the merged user-fn lives in
                 * the program AST under the underscored namespace-
                 * prefixed name (`"json_get_string"`). Without
                 * normalisation the lookup misses every cross-module
                 * callee, the per-position structural analyzer never
                 * runs, and the destructure wrapper falls back to
                 * `_heap_<lhs> = 0` even when the callee was uniform-
                 * heap-classifiable. Same dot-normalisation pattern
                 * `is_heap_string_expr` already uses on its hardcoded
                 * fast-path lookups. */
                const char* fn = codegen_normalise_callee(rhs->value);
                callee_def = find_function_definition_by_name(gen->program, fn);
            }

            // Generate: type a = _tmp._0; type b = _tmp._1; ...
            for (int j = 0; j < var_count; j++) {
                ASTNode* var = stmt->children[j];

                /* Per-position heap classification. Defaults to 0
                 * for any case the analyzer can't classify. */
                int pos_is_string = (rhs_type && rhs_type->kind == TYPE_TUPLE &&
                                     j < rhs_type->tuple_count &&
                                     rhs_type->tuple_types[j] &&
                                     rhs_type->tuple_types[j]->kind == TYPE_STRING);
                int pos_is_heap = 0;
                if (pos_is_string) {
                    if (callee_def) {
                        pos_is_heap = function_def_returns_heap_at(gen, callee_def, j);
                    } else if (rhs_type && rhs_type->tuple_heap_flags) {
                        pos_is_heap = rhs_type->tuple_heap_flags[j];
                    } else if (call_targets_closure_literal(gen, rhs)) {
                        /* #2501: a closure's string slots are owned
                         * (emit_tuple_return_position). */
                        pos_is_heap = 1;
                    }
                }

                /* `_` discard slot. If the position is a heap value,
                 * the destructure target has no name to free against
                 * — emit an immediate `free` so the heap allocation
                 * doesn't leak across the destructure. Non-heap
                 * positions stay no-ops. */
                if (var->value && strcmp(var->value, "_") == 0) {
                    if (pos_is_heap) {
                        print_indent(gen);
                        fprintf(gen->output,
                                "if (_tup%d._%d) aether_heap_str_free(_tup%d._%d);\n",
                                tmp_id, j, tmp_id, j);
                    }
                    continue;
                }

                // Prefer tuple element type over var's node_type (may be UNKNOWN)
                const char* var_type;
                if (rhs_type && rhs_type->kind == TYPE_TUPLE && j < rhs_type->tuple_count &&
                    rhs_type->tuple_types[j]->kind != TYPE_UNKNOWN) {
                    var_type = get_c_type(rhs_type->tuple_types[j]);
                } else {
                    var_type = get_c_type(var->node_type);
                }
                print_indent(gen);
                // Promoted-capture aware destructure: same routing as the
                // AST_VARIABLE_DECLARATION single-name path. At first use
                // declare the heap cell + defer free; on reassignment write
                // through the cell. Without this, a closure-body destructure
                // of a captured name miscompiled as `name = _tup._N` against
                // a `T**` slot — produced a -Wincompatible-pointer-types
                // warning and a runtime segfault on the next deref of the
                // captured slot (closure-shadow-tuple-destructure, svn-aether
                // porter Round 238/239). The matching is_assigned_to fix in
                // codegen_expr.c teaches the promotion analysis to see
                // tuple-destructure targets as writes.
                if (is_promoted_capture(gen, var->value)) {
                    /* #2514: a string cell owns what it holds, so a slot the
                     * call did not hand over is copied in. */
                    char slot[96];
                    if (pos_is_string)
                        snprintf(slot, sizeof(slot), "aether_uniform_heap_str(_tup%d._%d, %d)",
                                 tmp_id, j, pos_is_heap);
                    else
                        snprintf(slot, sizeof(slot), "_tup%d._%d", tmp_id, j);
                    if (!is_var_declared(gen, var->value)) {
                        emit_promoted_cell_declaration(gen, var->value, var_type, NULL, NULL, slot,
                                                       stmt->line, stmt->column);
                    } else if (pos_is_string) {
                        fprintf(gen->output, "_aether_str_cell_set(%s, %s);\n", var->value, slot);
                    } else {
                        fprintf(gen->output, "*%s = %s;\n", var->value, slot);
                    }
                    continue;
                }
                if (is_var_declared(gen, var->value)) {
                    int destruct_is_env_cap = 0;
                    for (int ec = 0; ec < gen->current_env_capture_count; ec++) {
                        if (gen->current_env_captures[ec] &&
                            strcmp(gen->current_env_captures[ec], var->value) == 0) {
                            destruct_is_env_cap = 1;
                            break;
                        }
                    }
                    if (destruct_is_env_cap) {
                        fprintf(gen->output, "_env->%s = _tup%d._%d;\n", var->value, tmp_id, j);
                        continue;
                    }
                    /* String-typed LHS with a hoisted heap tracker —
                     * route through the wrapper so heap-allocated
                     * values from the destructure don't leak on
                     * later reassignments. Mirrors the AST_VARIABLE_
                     * DECLARATION reassignment shape at lines
                     * 2087-2094. Issue #420.
                     *
                     * Escape gate: if the LHS has been passed to
                     * something that may have stored its pointer
                     * (map.put value, list.add, struct field write,
                     * actor message field, closure capture), the
                     * `free(_tmp_old)` would dangle the stored
                     * copy — emit a plain assignment instead.
                     * Strictly leaks the previous value; strictly
                     * better than UAF.
                     *
                     * The wrapper fires for BOTH first-destructure
                     * and re-destructure of a hoisted var, because
                     * the hoist initialised `<lhs> = NULL` and
                     * `_heap_<lhs> = 0`, so on first use the free
                     * is a no-op and the tracker simply moves to
                     * its true value. */
                    int lhs_is_tracked = (var->value &&
                                          is_heap_string_var(gen, var->value));
                    /* Wrapper-emission gate. Fire whenever the LHS is
                     * heap-string-tracked, regardless of whether the
                     * destructure POSITION's type is string. The
                     * tracker carries the LHS's previous-value-heapness
                     * across the destructure; if the previous value was
                     * a heap string (e.g. `pkg_include = "${name}*"`)
                     * and the destructure now reassigns it to a non-
                     * string position (e.g. `pkg_include, _ = map.get(...)`
                     * where map.get returns `(ptr, string)` and
                     * position 0 is TYPE_PTR), the wrapper must still
                     * free the previous heap-string value and clear
                     * the flag — otherwise the function-exit defer-
                     * free reads the now-stale flag (=1) and runs
                     * free() against the new non-owned pointer.
                     *
                     * For non-string positions the new heap flag is
                     * always 0 (borrow assumption — the destructured
                     * value is an opaque pointer from the heap-string-
                     * tracker's perspective, never the source's
                     * fresh-malloc'd char*). */
                    if (lhs_is_tracked) {
                        int escaped = is_escaped_string_var(gen, var->value);
                        int new_heap = pos_is_string ? pos_is_heap : 0;
                        if (escaped) {
                            /* The old value is not freed (a recipient may
                             * still hold it), but the tracker still records
                             * whether the NEW value is owned: a struct field
                             * store moves this flag into the field's tracker,
                             * and a stale 0 there left a destructured @heap
                             * string owned by nobody (#2366). The escaped
                             * var's own scope-exit free stays suppressed, as
                             * on the declaration path. */
                            fprintf(gen->output, "%s = _tup%d._%d; _heap_%s = %d;\n",
                                    var->value, tmp_id, j, var->value, new_heap);
                        } else {
                            fprintf(gen->output,
                                "{ const char* _tmp_old = %s; "
                                "%s = _tup%d._%d; "
                                "if (_heap_%s) aether_heap_str_free(_tmp_old); "
                                "_heap_%s = %d;",
                                var->value,
                                var->value, tmp_id, j,
                                var->value,
                                var->value, new_heap);
                            emit_unwind_track_local(gen, var->value);
                            fprintf(gen->output, " }\n");
                        }
                        continue;
                    }
                    fprintf(gen->output, "%s = _tup%d._%d;\n", var->value, tmp_id, j);
                } else {
                    mark_var_declared(gen, var->value);
                    fprintf(gen->output, "%s %s = _tup%d._%d;\n", var_type, var->value, tmp_id, j);
                    /* If the LHS is a hoisted heap-string tracker
                     * (rare for first-decl since the hoist also
                     * marks the var declared, but kept defensively
                     * for the lazy-promote case) and the source
                     * position is heap, set the tracker. */
                    if (pos_is_string && var->value &&
                        is_heap_string_var(gen, var->value) && pos_is_heap) {
                        print_indent(gen);
                        fprintf(gen->output, "_heap_%s = 1;", var->value);
                        emit_unwind_track_local(gen, var->value);
                        fprintf(gen->output, "\n");
                    }
                    /* #752: a struct-typed tuple position transfers
                     * ownership of its heap-string fields to this LHS —
                     * free them at scope exit. */
                    if (rhs_type && rhs_type->kind == TYPE_TUPLE &&
                        j < rhs_type->tuple_count) {
                        push_struct_destroy_defer(gen, var->value,
                                                  rhs_type->tuple_types[j],
                                                  stmt->line, stmt->column);
                    }
                }
            }
            break;
        }

        case AST_VARIABLE_DECLARATION: {
            /* #790: track heap.new(T) box provenance. A var bound to
             * `heap.new(T)` is a zero-initialised box whose string fields can
             * be owned; rebinding it to anything else drops that status. */
            if (stmt->value && strcmp(stmt->value, "_") != 0 &&
                stmt->child_count > 0 && stmt->children[0]) {
                if (stmt->children[0]->type == AST_HEAP_NEW ||
                    call_yields_heap_box(gen, stmt->children[0]))
                    mark_heap_box_var(gen, stmt->value);
                else
                    unmark_heap_box_var(gen, stmt->value);
            }

            // #340: optional-typed local (`x: T? = ...`). Emit the
            // declaration / re-bind with implicit `T -> T?` coercion of the
            // initializer (a bare value wraps, `none` zero-inits, an optional
            // passes through). A declaration with no initializer defaults to
            // `none`. Handled here because the generic declarator paths below
            // don't understand the `ae_opt_<T>` tagged-struct wrap.
            if (stmt->value && strcmp(stmt->value, "_") != 0 &&
                stmt->node_type && stmt->node_type->kind == TYPE_OPTIONAL) {
                /* `string?` heap-ownership: the local is registered by
                 * hoist_opt_str_trackers, so is_var_declared is already
                 * true here and this always takes the assign-only branch.
                 * On reassignment the PREVIOUS `.val` (if this slot owns
                 * a heap buffer) must be freed before the overwrite, and
                 * the `_heapopt_<name>` flag re-set from the RHS's
                 * heap-ness — exactly the bare-string reassignment
                 * wrapper, adapted to the `{has,val}` struct. */
                int is_opt_str = is_opt_str_var(gen, stmt->value);
                int rhs_is_heap = is_opt_str && stmt->child_count > 0 &&
                                  stmt->children[0] &&
                                  is_heap_opt_string_rhs(gen, stmt->children[0]);
                print_indent(gen);
                if (is_opt_str) {
                    /* Free the prior owned buffer, then assign the new
                     * value, then update the ownership flag. A single
                     * temp captures the old struct so the RHS (which may
                     * read the same slot, e.g. `o = o ?? x`) evaluates
                     * against the un-freed value. */
                    fprintf(gen->output, "{ ae_opt_string _opt_old_%s = %s; ",
                            stmt->value, stmt->value);
                    fprintf(gen->output, "%s = ", stmt->value);
                    if (stmt->child_count > 0 && stmt->children[0]) {
                        emit_optional_coerced(gen, stmt->children[0], stmt->node_type);
                    } else {
                        fprintf(gen->output, "(%s){0}", get_c_type(stmt->node_type));
                    }
                    fprintf(gen->output,
                            "; if (_heapopt_%s && _opt_old_%s.has) aether_heap_str_free((void*)_opt_old_%s.val);",
                            stmt->value, stmt->value, stmt->value);
                    fprintf(gen->output, " _heapopt_%s = %d; }\n",
                            stmt->value, rhs_is_heap ? 1 : 0);
                    break;
                }
                if (!is_var_declared(gen, stmt->value)) {
                    fprintf(gen->output, "%s %s", get_c_type(stmt->node_type), stmt->value);
                    mark_var_declared(gen, stmt->value);
                } else {
                    fprintf(gen->output, "%s", stmt->value);
                }
                fprintf(gen->output, " = ");
                if (stmt->child_count > 0 && stmt->children[0]) {
                    emit_optional_coerced(gen, stmt->children[0], stmt->node_type);
                } else {
                    fprintf(gen->output, "(%s){0}", get_c_type(stmt->node_type));
                }
                fprintf(gen->output, ";\n");
                break;
            }

            // #914: sum-typed local (`s: Shape = Circle {...}`). Emit the
            // declaration / re-bind, wrapping a variant struct initializer into
            // the tagged union (a value already of the sum type passes through).
            if (stmt->value && strcmp(stmt->value, "_") != 0 &&
                stmt->node_type && stmt->node_type->kind == TYPE_SUM) {
                print_indent(gen);
                if (!is_var_declared(gen, stmt->value)) {
                    fprintf(gen->output, "%s %s", get_c_type(stmt->node_type), stmt->value);
                    mark_var_declared(gen, stmt->value);
                } else {
                    fprintf(gen->output, "%s", stmt->value);
                }
                fprintf(gen->output, " = ");
                if (stmt->child_count > 0 && stmt->children[0]) {
                    emit_sum_coerced(gen, stmt->children[0], stmt->node_type);
                } else {
                    fprintf(gen->output, "(%s){0}", get_c_type(stmt->node_type));
                }
                fprintf(gen->output, ";\n");
                break;
            }
            /* Bare `_` is a per-use discard, not a real variable.
             * `_ = <expr>` evaluates the RHS for its side effects and
             * throws the value away — no declaration, no type, no
             * shared lvalue. Without this, `_` was one C variable
             * whose type was fixed by its first use, so a function
             * discarding (say) a string at one site and an int at
             * another emitted conflicting `_` declarations and failed
             * the C compile (aeb-ae-help-and-toolchain-feedback.md
             * #4). A freshly-allocated heap string handed to `_` is
             * freed so the discard doesn't leak; everything else is a
             * plain `(void)` cast. A match RHS falls through to the
             * normal match-as-expression path. */
            if (stmt->value && strcmp(stmt->value, "_") == 0 &&
                stmt->child_count > 0 && stmt->children[0] &&
                stmt->children[0]->type != AST_MATCH_STATEMENT) {
                ASTNode* drhs = stmt->children[0];
                int fresh_heap =
                    (drhs->type == AST_FUNCTION_CALL ||
                     drhs->type == AST_STRING_INTERP) &&
                    is_heap_string_expr(gen, drhs);
                print_indent(gen);
                if (fresh_heap) {
                    fprintf(gen->output, "aether_heap_str_free((void*)(");
                    generate_expression(gen, drhs);
                    fprintf(gen->output, "));\n");
                } else if (call_returns_owned_closure(gen, drhs)) {
                    /* #2507: a handed-over closure thrown away. */
                    fprintf(gen->output, "_aether_closure_env_release((");
                    generate_expression(gen, drhs);
                    fprintf(gen->output, ").env);\n");
                } else {
                    fprintf(gen->output, "(void)(");
                    generate_expression(gen, drhs);
                    fprintf(gen->output, ");\n");
                }
                break;
            }
            // Register fn-pointer locals so call-site codegen can emit
            // the matching C function-pointer cast.  Two sources:
            //   - explicit annotation:  `fp: fn(int, int) -> int = ...`
            //   - inferred-from-cast:   `fp = expr as fn(int, int) -> int`
            // In both cases, the AST node carries TYPE_FUNCTION with
            // is_fnptr=1 by typecheck time (parser sets it on `as fn`
            // casts; the var-decl node inherits from the initializer
            // via the typechecker's RHS inference pass).
            if (stmt->value) {
                Type* fnptr_sig = NULL;
                if (stmt->node_type && stmt->node_type->kind == TYPE_FUNCTION &&
                    stmt->node_type->is_fnptr) {
                    fnptr_sig = stmt->node_type;
                } else if (stmt->child_count > 0 && stmt->children[0] &&
                           stmt->children[0]->type == AST_PTR_AS_FN_CAST &&
                           stmt->children[0]->node_type &&
                           stmt->children[0]->node_type->kind == TYPE_FUNCTION &&
                           stmt->children[0]->node_type->is_fnptr) {
                    fnptr_sig = stmt->children[0]->node_type;
                }
                if (fnptr_sig) {
                    register_fnptr_local(gen, stmt->value, fnptr_sig);
                }
            }

            // Check if this is a state variable assignment in an actor
            int is_state_var = 0;
            if (gen->current_actor && stmt->value) {
                for (int i = 0; i < gen->state_var_count; i++) {
                    if (strcmp(stmt->value, gen->actor_state_vars[i]) == 0) {
                        is_state_var = 1;
                        break;
                    }
                }
            }
            
            if (is_state_var) {
                /* Generate as assignment to self->field. #2528: a string
                 * state field's tracker is the actor's own (`self->_heap_<f>`,
                 * released by destroy_state); any other state name keeps the
                 * handler-local tracker the hoist declared. */
                const char* trk = actor_state_string_tracked(gen, stmt->value)
                                  ? cg_internf("self->_heap_%s", stmt->value)
                                  : cg_internf("_heap_%s", stmt->value);
                if (stmt->child_count > 0 &&
                    string_take_is_view(gen, stmt->children[0])) {
                    /* #2461: state outlives the handler, so it takes a view
                     * (a field of a message or local struct, an `if` over
                     * locals) as a value of its own, the take's flag saying
                     * whether it owns it. As below, an escaped value's old
                     * buffer is left to whoever it escaped to. */
                    char own[32];
                    string_take_new_flag(own, sizeof(own));
                    fprintf(gen->output, "{ const char* _tmp_old = self->%s; int %s = 0; self->%s = ",
                            stmt->value, own, stmt->value);
                    emit_string_take(gen, stmt->children[0], own, stmt->value);
                    fprintf(gen->output, ";");
                    if (!is_escaped_string_var(gen, stmt->value)) {
                        fprintf(gen->output, " if (%s) aether_heap_str_free(_tmp_old);", trk);
                    }
                    fprintf(gen->output, " %s = %s; (void)_tmp_old; }\n", trk, own);
                } else if (stmt->child_count > 0 && is_heap_string_expr(gen, stmt->children[0])) {
                    /* Skip the free if the var has escaped (passed to
                     * a function that may have stored the pointer):
                     * freeing now would dangle the stored copy. Leak
                     * instead — strictly better than a UAF. See
                     * mark_escaped_heap_string_vars. */
                    if (is_escaped_string_var(gen, stmt->value)) {
                        fprintf(gen->output, "self->%s = ", stmt->value);
                        generate_expression(gen, stmt->children[0]);
                        fprintf(gen->output, ";\n");
                    } else {
                        fprintf(gen->output, "{ const char* _tmp_old = self->%s; ", stmt->value);
                        fprintf(gen->output, "self->%s = ", stmt->value);
                        generate_expression(gen, stmt->children[0]);
                        fprintf(gen->output, "; if (%s) aether_heap_str_free(_tmp_old);", trk);
                        fprintf(gen->output, " %s = 1; }\n", trk);
                    }
                } else if (stmt->child_count > 0 && stmt->children[0] &&
                           stmt->children[0]->type == AST_IDENTIFIER &&
                           stmt->children[0]->node_type &&
                           stmt->children[0]->node_type->kind == TYPE_STRING &&
                           !is_heap_string_var(gen, stmt->children[0]->value)) {
                    /* Retaining a BORROWED string into actor state — the
                     * classic case is a message pattern field (`SetN(in_n)
                     * -> { n = in_n }`). The field is owned by the message
                     * envelope and freed by `<Msg>_release_fields` right
                     * after this handler returns, so storing the raw pointer
                     * into `self->field` dangles and a LATER message reads
                     * freed bytes (the aeo actor-state-string corruption).
                     * Copy into an owned AetherString — the same idiom the
                     * message SEND site uses for string fields — and free any
                     * prior owned copy. Marked via `_heap_<field>` so a
                     * subsequent retain frees the previous one. */
                    fprintf(gen->output, "{ const char* _tmp_old = self->%s; ", stmt->value);
                    fprintf(gen->output, "const char* _src = (const char*)(");
                    generate_expression(gen, stmt->children[0]);
                    fprintf(gen->output, "); self->%s = _src ? (const char*)string_new_with_length(aether_string_data(_src), (int)aether_string_length(_src)) : _src;",
                            stmt->value);
                    fprintf(gen->output, " if (%s) aether_heap_str_free(_tmp_old);", trk);
                    fprintf(gen->output, " %s = 1; }\n", trk);
                } else if (stmt->child_count > 0 && stmt->children[0] &&
                           stmt->children[0]->node_type &&
                           stmt->children[0]->node_type->kind == TYPE_FUNCTION &&
                           !stmt->children[0]->node_type->is_fnptr) {
                    /* #2525: state holds a reference of its own to a closure
                     * (a message field's or a local's is retained, a fresh
                     * one adopted) and gives back the one it held. */
                    fprintf(gen->output, "{ void* _ae_old = self->%s.env; self->%s = ",
                            stmt->value, stmt->value);
                    emit_closure_take(gen, stmt->children[0]);
                    fprintf(gen->output, "; _aether_closure_env_release(_ae_old); }\n");
                } else if (stmt->child_count > 0 && stmt->children[0] &&
                           struct_owning_strings(gen, stmt->children[0]->node_type)) {
                    /* A struct in state that owns strings or closures is
                     * replaced as a local one is (#465, #2497): the old
                     * value's are released and the new value is taken, a
                     * copy of one another owner keeps. A plain store leaked
                     * the old value's strings and shared a local struct's,
                     * which the handler's exit then freed. */
                    const char* sname = struct_owning_strings(gen, stmt->children[0]->node_type);
                    fprintf(gen->output, "%s_replace(&self->%s, ", sname, stmt->value);
                    emit_struct_take(gen, stmt->children[0], sname, stmt->value);
                    fprintf(gen->output, ");\n");
                } else {
                    fprintf(gen->output, "self->%s", stmt->value);
                    if (stmt->child_count > 0) {
                        fprintf(gen->output, " = ");
                        generate_expression(gen, stmt->children[0]);
                    }
                    fprintf(gen->output, ";\n");
                }
            } else {
                // Match-as-expression: x = match val { ... }
                if (stmt->child_count > 0 && stmt->children[0] &&
                    stmt->children[0]->type == AST_MATCH_STATEMENT) {
                    /* #2514: a variable a closure writes lives in its cell,
                     * declared here, empty, when the match is its first
                     * binding; each arm stores into the cell. */
                    Type* cell_type = NULL;
                    if (is_promoted_capture(gen, stmt->value)) {
                        cell_type = (stmt->node_type && stmt->node_type->kind != TYPE_UNKNOWN)
                                    ? stmt->node_type : match_value_type(stmt->children[0]);
                        if (!is_var_declared(gen, stmt->value)) {
                            print_indent(gen);
                            emit_promoted_cell_declaration(gen, stmt->value,
                                get_c_type(cell_type), cell_type, NULL, NULL,
                                stmt->line, stmt->column);
                        }
                    }
                    if (!is_var_declared(gen, stmt->value)) {
                        mark_var_declared(gen, stmt->value);
                        // Infer type from first match arm result
                        const char* c_type = get_c_type(stmt->node_type);
                        ASTNode* match_node = stmt->children[0];
                        if (!stmt->node_type || stmt->node_type->kind == TYPE_UNKNOWN) {
                            Type* arm_type = match_value_type(match_node);
                            if (arm_type) c_type = get_c_type(arm_type);
                        }
                        /* #2497: a struct that owns strings starts empty
                         * (its trackers read by the replace each arm does)
                         * and is released at scope exit, as a struct a
                         * literal or call initialises is. */
                        Type* mt = (stmt->node_type && stmt->node_type->kind != TYPE_UNKNOWN)
                                   ? stmt->node_type : match_value_type(match_node);
                        const char* msname = struct_owning_strings(gen, mt);
                        print_indent(gen);
                        fprintf(gen->output, msname ? "%s %s = {0};\n" : "%s %s;\n",
                                c_type, stmt->value);
                        if (msname) {
                            push_struct_destroy_defer(gen, stmt->value, mt,
                                                      stmt->line, stmt->column);
                        }
                        /* #2461: a string local a match binds owns what its
                         * arms hand it, so it needs the tracker the hoist
                         * gives every other string local. */
                        if (c_type && strcmp(c_type, "const char*") == 0 &&
                            !is_heap_string_var(gen, stmt->value) &&
                            !is_promoted_capture(gen, stmt->value)) {
                            print_line(gen, "int _heap_%s = 0; (void)_heap_%s;",
                                       stmt->value, stmt->value);
                            mark_heap_string_var(gen, stmt->value);
                        }
                    }
                    /* #2461: a string local takes each arm's value as it
                     * would take it on its own (a field read copied, a
                     * heap-tracked local moved or copied, a fresh value
                     * adopted) and frees the value it held before, like
                     * the reassignment wrapper. The arms used to store a
                     * bare pointer and leave `_heap_<name>` alone: a borrow
                     * of a buffer its owner then freed, the old value
                     * leaked, and a stale flag that freed a literal. The
                     * flag stays -1 when no value arm ran (a block or
                     * statement arm leaves the local as it was). */
                    const char* saved_mvar = gen->match_result_var;
                    const char* saved_mown = gen->match_result_own;
                    const char* saved_mstruct = gen->match_result_struct;
                    int saved_mreplace = gen->match_result_replace;
                    Type* saved_mcell = gen->match_result_cell;
                    /* #2497: a local that owns a string-owning struct takes
                     * each arm's struct and releases the one it held. */
                    Type* bound = declared_var_type(gen, stmt->value);
                    if (!bound || bound->kind != TYPE_STRUCT) bound = stmt->node_type;
                    if (!bound || bound->kind != TYPE_STRUCT)
                        bound = match_value_type(stmt->children[0]);
                    const char* bound_struct =
                        (!is_promoted_capture(gen, stmt->value) &&
                         !is_env_capture_name(gen, stmt->value))
                        ? struct_owning_strings(gen, bound) : NULL;
                    char own[32] = "";
                    int owning = is_heap_string_var(gen, stmt->value) &&
                                 !is_promoted_capture(gen, stmt->value) &&
                                 !is_env_capture_name(gen, stmt->value);
                    if (owning) {
                        string_take_new_flag(own, sizeof(own));
                        print_indent(gen);
                        fprintf(gen->output, "{ const char* _tmp_old_%s = %s; int %s = -1;\n",
                                own, stmt->value, own);
                    }
                    // Generate match with result assignment
                    gen->match_result_var = stmt->value;
                    gen->match_result_own = owning ? own : NULL;
                    gen->match_result_struct = bound_struct;
                    gen->match_result_replace = 1;
                    gen->match_result_cell = cell_type;
                    generate_statement(gen, stmt->children[0]);
                    gen->match_result_var = saved_mvar;
                    gen->match_result_own = saved_mown;
                    gen->match_result_struct = saved_mstruct;
                    gen->match_result_replace = saved_mreplace;
                    gen->match_result_cell = saved_mcell;
                    if (owning) {
                        print_indent(gen);
                        fprintf(gen->output, "if (%s >= 0) {", own);
                        if (!is_escaped_string_var(gen, stmt->value)) {
                            fprintf(gen->output, " if (_heap_%s) aether_heap_str_free(_tmp_old_%s);",
                                    stmt->value, own);
                        }
                        fprintf(gen->output, " _heap_%s = %s;", stmt->value, own);
                        emit_unwind_track_local(gen, stmt->value);
                        fprintf(gen->output, " } (void)_tmp_old_%s; }\n", own);
                    }
                    break;
                }

                // Route 1: promoted captures are heap-allocated cells. In an
                // outer function body, the FIRST assignment declares
                // `int* name = malloc(...); *name = <init>;` and queues a
                // defer for free(). Subsequent writes emit `*name = <expr>;`.
                // In a closure body, the name is never newly declared (it's
                // aliased from _env->name in the prologue), so all writes
                // are dereferences.
                if (is_promoted_capture(gen, stmt->value)) {
                    if (!is_var_declared(gen, stmt->value)) {
                        // First occurrence in this scope — declaration:
                        // allocate the shared cell, initialise it, and
                        // defer the scope's release (#2019).
                        emit_promoted_cell_declaration(gen, stmt->value,
                            get_c_type(stmt->node_type), stmt->node_type,
                            stmt->child_count > 0 ? stmt->children[0] : NULL, "0",
                            stmt->line, stmt->column);
                    } else if (emit_cell_array_store(gen, stmt)) {
                        /* #2474: a whole fixed-size array, stored into its cell. */
                    } else if (stmt->child_count > 0) {
                        // Reassignment: write through the pointer. A string
                        // cell OWNS its value, so it frees the superseded
                        // string as it stores the new one (else a per-
                        // iteration running-max / accumulator leaks every
                        // prior value).
                        emit_cell_store(gen, stmt->value, stmt->node_type, stmt->children[0]);
                    } else {
                        fprintf(gen->output, "*%s;\n", stmt->value);
                    }
                    break;
                }

                // If we're in a closure body and this name is a mutated capture,
                // route the write through _env-> so mutations persist on the env
                // struct rather than dying with a stack-local alias.
                // NOTE: with Route 1, this path is bypassed for promoted names
                // (handled above). It remains as a fallback for the pre-Route-1
                // env-cap mechanism.
                int is_env_cap = 0;
                for (int ec = 0; ec < gen->current_env_capture_count; ec++) {
                    if (gen->current_env_captures[ec] &&
                        strcmp(gen->current_env_captures[ec], stmt->value) == 0) {
                        is_env_cap = 1;
                        break;
                    }
                }
                if (is_env_cap) {
                    fprintf(gen->output, "_env->%s", stmt->value);
                    if (stmt->child_count > 0) {
                        fprintf(gen->output, " = ");
                        generate_expression(gen, stmt->children[0]);
                    }
                    fprintf(gen->output, ";\n");
                    break;
                }

                // #701: a `name = expr` whose name is a module-level `var`
                // global is a WRITE to that file-scope static, not a new
                // local — the global is in scope in every same-module
                // function (the issue's required semantics: reads and writes
                // are plain identifier access). Reads already resolve to the
                // bare identifier, so only the write side needs steering. The
                // guard on !is_var_declared lets a same-named parameter or an
                // earlier in-function local legitimately shadow the global.
                if (stmt->value &&
                    !is_var_declared(gen, stmt->value) &&
                    is_module_global_var(gen, stmt->value)) {
                    print_indent(gen);
                    ASTNode* grhs = stmt->child_count > 0 ? stmt->children[0] : NULL;
                    if (grhs && (string_take_is_view(gen, grhs) ||
                                 (grhs->type == AST_IDENTIFIER && grhs->value &&
                                  is_heap_string_var(gen, grhs->value)))) {
                        /* #2461: a global outlives the function, so it takes
                         * a view (a field of a local struct, an `if` over
                         * locals) as a value of its own, and a heap-tracked
                         * local is moved on its last use or copied, so the
                         * local's exit free cannot take the global's value
                         * away; like every heap value a global holds, it is
                         * never freed. */
                        char own[32];
                        string_take_new_flag(own, sizeof(own));
                        fprintf(gen->output, "{ int %s = 0; %s = ", own, stmt->value);
                        emit_string_take(gen, stmt->children[0], own, NULL);
                        fprintf(gen->output, "; (void)%s; }\n", own);
                        break;
                    }
                    if (stmt->child_count > 0 && stmt->children[0] &&
                        stmt->children[0]->node_type &&
                        stmt->children[0]->node_type->kind == TYPE_FUNCTION &&
                        !stmt->children[0]->node_type->is_fnptr) {
                        /* #2525: a global holds a reference of its own to a
                         * closure and gives back the one it held when
                         * rebound; the last one lives as long as the program. */
                        fprintf(gen->output, "{ void* _ae_old = %s.env; %s = ",
                                stmt->value, stmt->value);
                        emit_closure_take(gen, stmt->children[0]);
                        fprintf(gen->output, "; _aether_closure_env_release(_ae_old); }\n");
                        break;
                    }
                    /* A struct that owns strings is taken as a string is
                     * above: a local or parameter moved on its last use or
                     * copied, a view copied, a fresh one adopted. Stored as
                     * it was, the global shared strings its source freed at
                     * its exit. */
                    const char* gsname = grhs ? struct_owning_strings(gen, grhs->node_type) : NULL;
                    if (gsname) {
                        fprintf(gen->output, "%s = ", stmt->value);
                        emit_struct_take(gen, grhs, gsname, NULL);
                        fprintf(gen->output, ";\n");
                        break;
                    }
                    fprintf(gen->output, "%s", stmt->value);
                    if (stmt->child_count > 0) {
                        fprintf(gen->output, " = ");
                        generate_expression(gen, stmt->children[0]);
                    }
                    fprintf(gen->output, ";\n");
                    break;
                }

                // Check if this is a reassignment (Python-style)
                if (is_var_declared(gen, stmt->value)) {
                    /* #2124: a hoisted local is one C variable for the
                     * whole function. A bare re-bind here — in a sibling
                     * branch or loop body the typechecker could not see
                     * as the same variable — with a value that cannot
                     * flow into the hoisted type used to be a C error
                     * ("assignment to 'const char *' from 'int'"); say
                     * it in the language's terms. The numeric and
                     * pointer conversions the language permits pass. */
                    Type* hoisted = declared_var_type(gen, stmt->value);
                    Type* here = stmt->node_type;
                    if ((!here || here->kind == TYPE_UNKNOWN) && stmt->child_count > 0 &&
                        stmt->children[0]) here = stmt->children[0]->node_type;
                    if (stmt->type_inferred && hoisted && here &&
                        hoisted->kind != TYPE_UNKNOWN && here->kind != TYPE_UNKNOWN &&
                        /* Same kind is the same C variable whatever the
                         * nominal wrapper (a distinct string into a
                         * string-hoisted local; the typechecker owns the
                         * nominal rules). Optionals have their own re-bind
                         * rules there too. */
                        here->kind != hoisted->kind &&
                        here->kind != TYPE_OPTIONAL && hoisted->kind != TYPE_OPTIONAL &&
                        !((here->kind == TYPE_PTR && hoisted->kind == TYPE_STRING) ||
                          (here->kind == TYPE_STRING && hoisted->kind == TYPE_PTR)) &&
                        (!is_type_compatible(here, hoisted) ||
                         (here->kind == TYPE_PTR && rebind_is_number(hoisted->kind)) ||
                         (rebind_is_number(here->kind) && hoisted->kind == TYPE_PTR))) {
                        char msg[400];
                        snprintf(msg, sizeof(msg),
                                 "cannot bind '%s' as %s: it is bound as %s in another branch or "
                                 "loop body of this function, and a local first bound inside a "
                                 "branch or loop body is one variable for the whole function. Use "
                                 "a new name for the %s value, or convert it to %s",
                                 stmt->value, type_to_string(here), type_to_string(hoisted),
                                 type_to_string(here), type_to_string(hoisted));
                        AetherError e = { stmt->source_file, NULL, stmt->line, stmt->column, msg,
                                          NULL, NULL, AETHER_ERR_TYPE_MISMATCH };
                        aether_error_report(&e);
                    }
                    /* Self-assignment peephole. `p = p` is a no-op at
                     * the semantic level; the heap-tracker wrapper
                     * below would otherwise (a) consult
                     * is_heap_string_expr on the RHS, which now
                     * recognises bare-identifier-of-tracked-local as
                     * heap=1, (b) emit `_heap_<p> = 1` at wrapper
                     * exit, and (c) cause the function-exit defer-
                     * free to free the buffer the caller still owns
                     * (the parameter was a borrow, not an owned
                     * value). avn's `noop_free(p: string) { p = p }`
                     * shim hit this — every call double-freed the
                     * caller's heap-allocated string. See
                     * path-b-self-assign-param-doublefree.md filing.
                     *
                     * Skipping emission entirely is sound: `p = p`
                     * has no observable effect. C doesn't even need
                     * the statement (it'd compile to a no-op
                     * anyway), and the heap-tracker stays in its
                     * pre-assignment state — which is the truth
                     * because the assignment didn't change anything.
                     *
                     * The guard fires before any other wrapper-
                     * emission branch, so it covers every code path
                     * downstream (string-tracked, escaped, non-
                     * string, etc.) uniformly. */
                    if (stmt->child_count > 0 && stmt->children[0] &&
                        stmt->children[0]->type == AST_IDENTIFIER &&
                        stmt->children[0]->value && stmt->value &&
                        strcmp(stmt->children[0]->value, stmt->value) == 0) {
                        break;
                    }
                    /* #2289: an array literal bound to a fixed-size array
                     * that is already declared -- hoisted out of the loop
                     * or branch it is written in, or bound earlier -- is
                     * stored element by element, in order: C accepts
                     * `{...}` only in a declaration. Elements past the
                     * literal's are zeroed, as the declaration's
                     * initializer zeroes them, so the array holds exactly
                     * the new value. */
                    if (stmt->child_count > 0 && stmt->children[0] &&
                        stmt->children[0]->type == AST_ARRAY_LITERAL &&
                        hoisted && hoisted->kind == TYPE_ARRAY && hoisted->array_size > 0) {
                        ASTNode* lit = stmt->children[0];
                        if (lit->child_count > hoisted->array_size) {
                            char msg[300];
                            snprintf(msg, sizeof(msg),
                                     "array literal of %d elements assigned to '%s', which holds %d: an array keeps the size of its first binding",
                                     lit->child_count, stmt->value, hoisted->array_size);
                            AetherError e = { stmt->source_file, NULL, stmt->line, stmt->column, msg,
                                              NULL, NULL, AETHER_ERR_TYPE_MISMATCH };
                            aether_error_report(&e);
                            break;
                        }
                        /* #2478: the literal is evaluated before it is
                         * stored, in source order: an element that reads
                         * the array sees it from before the stores. */
                        order_prelude_begin(gen, lit->children, lit->child_count, stmt->value);
                        for (int ei = 0; ei < lit->child_count; ei++) {
                            if (ei > 0) print_indent(gen);
                            fprintf(gen->output, "%s[%d] = ", stmt->value, ei);
                            generate_expression(gen, lit->children[ei]);
                            fprintf(gen->output, ";\n");
                        }
                        if (lit->child_count < hoisted->array_size) {
                            if (lit->child_count > 0) print_indent(gen);
                            fprintf(gen->output, "memset(&%s[%d], 0, sizeof(%s[0]) * %d);\n",
                                    stmt->value, lit->child_count, stmt->value,
                                    hoisted->array_size - lit->child_count);
                        }
                        break;
                    }
                    /* #2516: another fixed-size array bound to this one
                     * copies its elements (the typechecker holds both to
                     * one length); C does not assign arrays. memmove: the
                     * source may be the array itself. */
                    if (stmt->child_count > 0 && stmt->children[0] &&
                        hoisted && is_sized_array_param(hoisted) &&
                        is_sized_array_param(stmt->children[0]->node_type)) {
                        /* #2528: an owning local array replaces each element
                         * with a copy of the source's (a self-copy keeps
                         * every element). */
                        const char* owning_elem = struct_owning_strings(gen, hoisted->element_type);
                        if (owning_elem && struct_array_local_owns(gen, stmt->value)) {
                            fprintf(gen->output, "{ %s* _ae_src = ", owning_elem);
                            generate_expression(gen, stmt->children[0]);
                            fprintf(gen->output, "; for (int _ae_k = 0; _ae_k < %d; _ae_k++) %s_replace(&%s[_ae_k], %s_dup(_ae_src[_ae_k])); }\n",
                                    hoisted->array_size, owning_elem, stmt->value, owning_elem);
                            break;
                        }
                        fprintf(gen->output, "memmove(%s, ", stmt->value);
                        generate_expression(gen, stmt->children[0]);
                        fprintf(gen->output, ", sizeof(%s));\n", stmt->value);
                        break;
                    }
                    // Already declared - generate assignment only.
                    //
                    // For string-tracked variables (issue #405) the
                    // assignment must go through the heap-aware
                    // wrapper for *every* string→string transition so
                    // the tracker stays in lock-step with the actual
                    // pointer's heap-ness. Four transitions, all
                    // handled by one shape:
                    //   heap → heap  : free old, _heap=1
                    //   heap → lit   : free old, _heap=0
                    //   lit  → heap  : no free (_heap was 0), _heap=1
                    //   lit  → lit   : no free, _heap=0
                    // The wrapper does this uniformly via `if (_heap_X)
                    // free(_tmp_old); _heap_X = <init_heap>`. Without
                    // this, heap→lit would leave _heap stale and a
                    // later free could attempt to release a literal.
                    int rhs_is_heap = (stmt->child_count > 0 &&
                                       is_heap_string_expr(gen, stmt->children[0]));
                    int var_is_string = is_heap_string_var(gen, stmt->value);
                    /* Escape gate (mark_escaped_heap_string_vars): if
                     * the var's value has been passed to a function
                     * that may have stored the pointer (`map.put`
                     * value, `list.add`, struct field write via fn,
                     * actor message field, closure capture), the
                     * `free(_tmp_old)` here would dangle the stored
                     * copy. Conservative: emit a plain assignment
                     * instead, leaving the previous heap value alive
                     * for the recipient — the variable's lifetime
                     * leak is strictly better than a UAF. See
                     * mark_escaped_heap_string_vars in this file for
                     * the analysis. */
                    int var_escaped = is_escaped_string_var(gen, stmt->value);
                    /* *StringSeq reassignment. The slot owns a refcounted
                     * spine; free the prior ref (string_seq_free is a
                     * decrement, so this is safe even when the spine is
                     * shared) before overwriting. A bare-identifier alias
                     * of another owned seq takes an INDEPENDENT ref via
                     * string_seq_retain so both slots free safely. The
                     * `_seqheap_<name>` flag records whether the slot
                     * currently owns a ref; it is suppressed (free skipped)
                     * for escaped seqs (returned / raw-stored) exactly like
                     * the heap-string escape gate. */
                    int var_is_seq = is_seq_var(gen, stmt->value);
                    /* datastar#4: a builder call with a trailing block is
                     * re-emitted with the filled config by the builder handler
                     * below, which assigns the real value. Emitting it here as
                     * well runs the builder's BODY twice -- once with a NULL
                     * config -- which is silent whenever the body has side
                     * effects. The plain declaration path already suppresses
                     * its half (`defer_with_trailing`); these ownership-aware
                     * paths did not. */
                    int builder_defers = init_is_builder_with_trailing(
                        gen, stmt->child_count > 0 ? stmt->children[0] : NULL);
                    if (builder_defers) {
                        /* declared already; the builder handler assigns. */
                    } else if (var_is_seq && stmt->child_count > 0) {
                        ASTNode* srhs = stmt->children[0];
                        int srhs_owning = is_seq_owning_expr(gen, srhs);
                        int srhs_alias = (srhs->type == AST_IDENTIFIER &&
                                          srhs->value &&
                                          is_seq_var(gen, srhs->value) &&
                                          strcmp(srhs->value, stmt->value) != 0);
                        int seq_escaped = is_escaped_seq_var(gen, stmt->value);
                        fprintf(gen->output, "{ StringSeq* _tmp_seq = %s; %s = ",
                                stmt->value, stmt->value);
                        if (srhs_alias) {
                            fprintf(gen->output, "string_seq_retain(");
                            generate_expression(gen, srhs);
                            fprintf(gen->output, ")");
                        } else {
                            generate_expression(gen, srhs);
                        }
                        if (!seq_escaped) {
                            fprintf(gen->output,
                                    "; if (_seqheap_%s) string_seq_free(_tmp_seq);",
                                    stmt->value);
                        } else {
                            fprintf(gen->output, ";");
                        }
                        fprintf(gen->output, " _seqheap_%s = %d; }\n",
                                stmt->value,
                                (srhs_owning || srhs_alias) ? 1 : 0);
                    } else if (var_is_string && stmt->child_count > 0 &&
                               string_take_is_view(gen, stmt->children[0])) {
                        /* #2461: a field read or an `if` over owned values
                         * is taken (copied / moved / adopted per arm), not
                         * borrowed from a buffer its owner may free. */
                        emit_string_take_rebind(gen, stmt->value, stmt->children[0]);
                    } else if (var_is_string && stmt->child_count > 0 && var_escaped) {
                        /* Escaped-LHS bare assignment. Wrapper-free is
                         * suppressed because the var's old value is
                         * potentially stored by a recipient (list_add,
                         * map_put, struct field, etc.) — freeing now
                         * would dangle the stored copy.
                         *
                         * Companion to the 0.147 alias-ownership-
                         * transfer fix in the non-escaped branch: when
                         * RHS is a bare identifier referring to a heap-
                         * tracked local, we must ALSO clear the source's
                         * heap flag here. Otherwise, in the canonical
                         * "alias then reassign source" pattern —
                         *
                         *   line = rest                   // line escaped
                         *   rest = ""                     // wrapper frees buf
                         *   list_add(out, line)           // line dangles
                         *
                         * — the source's wrapper would free the buffer
                         * the escape recipient (list, map, struct, …)
                         * still references. The non-escaped path
                         * transfers the flag from source to dest; the
                         * escaped path can't (dest's flag is meaningless
                         * because its defer-free is suppressed) but it
                         * still needs to clear the source's flag for
                         * the same reason. Net effect: the buffer stays
                         * alive in the recipient and no wrapper-free
                         * fires against it. */
                        ASTNode* rhs_for_escape = stmt->children[0];
                        int rhs_is_alias_to_heap_var =
                            (rhs_for_escape &&
                             rhs_for_escape->type == AST_IDENTIFIER &&
                             rhs_for_escape->value &&
                             is_heap_string_var(gen, rhs_for_escape->value) &&
                             strcmp(rhs_for_escape->value, stmt->value) != 0);
                        if (rhs_is_alias_to_heap_var) {
                            /* Always-transfer ownership flag on escaped-LHS
                             * alias. The flag IS the ownership token; with
                             * this transfer:
                             *
                             *  - Container-escape case (list.add / map.put /
                             *    struct field): LHS's defer-free is
                             *    suppressed by the escape mark, so the
                             *    transferred _heap_<lhs> = 1 sits unused.
                             *    Buffer stays alive in the recipient.
                             *    Source's later wrapper-free no-ops
                             *    (_heap_<rhs> = 0).
                             *
                             *  - Return-escape case (LHS is the function's
                             *    return value): LHS's defer-free is also
                             *    suppressed, so the buffer survives the
                             *    function exit. The return-statement
                             *    codegen below emits a runtime-uniform-heap
                             *    shim that ensures the returned buffer is
                             *    always heap-allocated; combined with the
                             *    classifier extension in
                             *    walk_returns_for_heap_check, the caller's
                             *    wrapper sets _heap_<result> = 1 and
                             *    correctly frees the buffer on its own
                             *    defer-free / next reassignment. */
                            fprintf(gen->output,
                                "{ %s = ", stmt->value);
                            generate_expression(gen, stmt->children[0]);
                            fprintf(gen->output,
                                "; _heap_%s = _heap_%s; _heap_%s = 0;",
                                stmt->value,
                                rhs_for_escape->value,
                                rhs_for_escape->value);
                            emit_unwind_track_local(gen, stmt->value);
                            fprintf(gen->output, " }\n");
                        } else {
                            /* Record the value's heapness even though this
                             * var's own frees are escape-suppressed. The
                             * tracker used to be left at its stale value here
                             * on the reasoning that nothing would read it: the
                             * defer-free and the reassignment wrapper are both
                             * suppressed for an escaped var, so the flag was
                             * dead weight. It is not dead any more. A struct
                             * field store now MOVES this flag into the field's
                             * own tracker to decide whether the destructor
                             * frees, so a stale 0 on a genuinely owned value
                             * (`s = strbuilder.finish(b)` then `n.text = s`)
                             * tells the destructor to leave it alone and the
                             * buffer leaks. The flag is the ownership token;
                             * it has to be true whether or not this scope is
                             * the one that acts on it. */
                            fprintf(gen->output, "{ %s = ", stmt->value);
                            generate_expression(gen, stmt->children[0]);
                            fprintf(gen->output, "; _heap_%s = %d; }\n",
                                    stmt->value, rhs_is_heap ? 1 : 0);
                        }
                    } else if (var_is_string && stmt->child_count > 0) {
                        // Defensive: if the hoist somehow missed this
                        // name (e.g. promoted via a path the pre-pass
                        // doesn't walk), declare the tracker now.
                        // Should be unreachable post-#405; kept as
                        // belt-and-braces.
                        if (!is_heap_string_var(gen, stmt->value)) {
                            fprintf(gen->output, "int _heap_%s = 0; (void)_heap_%s; ",
                                    stmt->value, stmt->value);
                            mark_heap_string_var(gen, stmt->value);
                        }
                        /* Identifier-alias ownership transfer. When
                         * the RHS is a bare identifier referring to a
                         * heap-tracked local, this assignment aliases
                         * the source's buffer. The classifier returns
                         * `_heap_lhs = 0` for this shape because
                         * is_heap_string_expr only recognises function
                         * calls and string interpolation — it can't
                         * tell whether a bare identifier currently
                         * holds a heap value. Pre-fix consequence: the
                         * alias dropped its tracker, the source kept
                         * its tracker, and the source's next
                         * reassignment-wrapper freed the buffer the
                         * alias still pointed at (silent UAF).
                         *
                         * Fix: move the heap flag from source to dest.
                         * The flag IS the ownership token — there is
                         * only one buffer; only one slot should hold
                         * the freeing duty. After transfer, the alias
                         * is responsible for the buffer's lifetime and
                         * the source's later reassignment frees nothing
                         * because its flag is now 0.
                         *
                         * Self-assignment guard: `a = a` would free
                         * its own buffer and then keep the flag set —
                         * a pre-existing bug the fix doesn't make
                         * worse; we just don't go through the transfer
                         * path so it falls back to the same buggy
                         * shape as before. (Real-world code doesn't
                         * write `a = a`; a separate audit would close
                         * that as a no-op skip.) */
                        ASTNode* rhs = stmt->children[0];
                        int rhs_is_alias_to_heap_var =
                            (rhs && rhs->type == AST_IDENTIFIER && rhs->value &&
                             is_heap_string_var(gen, rhs->value) &&
                             strcmp(rhs->value, stmt->value) != 0);
                        /* Live-source guard (180-regression.md): if the
                         * aliased source is read again later, take a
                         * defensive copy instead of moving its buffer —
                         * otherwise this slot's next free dangles the
                         * source. */
                        int rhs_alias_copy = (rhs_is_alias_to_heap_var &&
                                              alias_source_must_copy(gen, rhs->value));
                        fprintf(gen->output, "{ const char* _tmp_old = %s; ", stmt->value);
                        fprintf(gen->output, "%s = ", stmt->value);
                        if (rhs_alias_copy) {
                            fprintf(gen->output, "aether_uniform_heap_str(");
                            generate_expression(gen, stmt->children[0]);
                            fprintf(gen->output, ", 0)");
                        } else {
                            generate_expression(gen, stmt->children[0]);
                        }
                        fprintf(gen->output, "; if (_heap_%s) aether_heap_str_free(_tmp_old);",
                                stmt->value);
                        if (rhs_alias_copy) {
                            fprintf(gen->output, " _heap_%s = 1;", stmt->value);
                        } else if (rhs_is_alias_to_heap_var) {
                            fprintf(gen->output,
                                    " _heap_%s = _heap_%s; _heap_%s = 0;",
                                    stmt->value, rhs->value, rhs->value);
                        } else {
                            fprintf(gen->output, " _heap_%s = %d;",
                                    stmt->value, rhs_is_heap ? 1 : 0);
                        }
                        emit_unwind_track_local(gen, stmt->value);
                        fprintf(gen->output, " }\n");
                    } else if (rhs_is_heap && var_escaped) {
                        /* Non-string-typed escaped var reassigned to
                         * a heap string. Same gate — skip the free. */
                        fprintf(gen->output, "%s = ", stmt->value);
                        generate_expression(gen, stmt->children[0]);
                        fprintf(gen->output, ";\n");
                    } else if (rhs_is_heap) {
                        // Non-string-typed variable being reassigned
                        // to a heap string. Rare (type-inference
                        // edge cases). Lazy-init the tracker and use
                        // the wrapper.
                        if (!is_heap_string_var(gen, stmt->value)) {
                            fprintf(gen->output, "int _heap_%s = 0; (void)_heap_%s; ",
                                    stmt->value, stmt->value);
                            mark_heap_string_var(gen, stmt->value);
                        }
                        fprintf(gen->output, "{ const char* _tmp_old = %s; ", stmt->value);
                        fprintf(gen->output, "%s = ", stmt->value);
                        generate_expression(gen, stmt->children[0]);
                        fprintf(gen->output, "; if (_heap_%s) aether_heap_str_free(_tmp_old);",
                                stmt->value);
                        fprintf(gen->output, " _heap_%s = 1;", stmt->value);
                        emit_unwind_track_local(gen, stmt->value);
                        fprintf(gen->output, " }\n");
                    } else if (stmt->child_count > 0 && stmt->children[0] &&
                               (env_scan_is_real_closure(stmt->children[0]) ||
                                call_returns_owned_closure(gen, stmt->children[0]) ||
                                closure_view_binding(stmt->children[0]) ||
                                closure_ask_binding(stmt->children[0])) &&
                               closure_env_carrier_index(gen, stmt->value) >= 0) {
                        /* #2480: a local whose env this scope frees holds one
                         * env at a time. Rebinding it (a loop body's closure,
                         * hoisted out of the loop) frees the env it replaces,
                         * once the new value is built; the carrier frees the
                         * last one at scope exit. No copy of the old value
                         * without a reference of its own is live:
                         * claim_closure_local_env only owns a local bound to
                         * fresh closures (a literal, or an owned result,
                         * #2494) whose every use is a call, a non-keeping
                         * argument or a capture, which retains. */
                        int cidx = closure_env_carrier_index(gen, stmt->value);
                        int ccid = closure_env_carrier_cid(gen, cidx);
                        int flagged = closure_env_carrier_owned_flag(gen, cidx);
                        fprintf(gen->output, "{ void* _ae_old_env = %s.env; ", stmt->value);
                        if (flagged) {
                            /* #2506: the old value is released only if the
                             * local still owned it; the new one it owns. */
                            fprintf(gen->output, "if (!_envown_%s) _ae_old_env = NULL; ",
                                    stmt->value);
                        }
                        fprintf(gen->output, "%s = ", stmt->value);
                        emit_closure_take(gen, stmt->children[0]);   /* #2525: a view is retained */
                        if (flagged) fprintf(gen->output, "; _envown_%s = 1", stmt->value);
                        if (ccid >= 0) {
                            fprintf(gen->output, "; _closure_env_%d_free(_ae_old_env); }\n", ccid);
                        } else {
                            fprintf(gen->output, "; _aether_closure_env_release(_ae_old_env); }\n");
                        }
                    } else {
                        // Plain non-string assignment.
                        /* Struct-reassignment heap cleanup (#465).
                         * `b = Box { ... }` overwrites the struct
                         * wholesale; without an explicit destroy of
                         * the previous instance, every heap-string
                         * field's old buffer leaks. Emit
                         * `<Struct>_replace(&<var>, <value>)`, which
                         * reclaims the prior heap fields the new value
                         * does not take over. The struct's literal initializer
                         * (codegen_expr.c AST_STRUCT_LITERAL) sets
                         * the new `_heap_<field>` bits so the next
                         * destroy at function exit fires correctly. */
                        Type* vtype = stmt->node_type;
                        if ((!vtype || vtype->kind != TYPE_STRUCT) &&
                            stmt->child_count > 0 && stmt->children[0]) {
                            vtype = stmt->children[0]->node_type;
                        }
                        if (stmt->child_count > 0 &&
                            emit_struct_replace_assign(gen, stmt->value, vtype,
                                                       stmt->children[0])) {
                            /* emitted */
                        } else if (stmt->child_count > 0) {
                            fprintf(gen->output, "%s = ", stmt->value);
                            generate_expression(gen, stmt->children[0]);
                            fprintf(gen->output, ";\n");
                        } else {
                            /* A re-declaration with no initializer, of a name
                             * something already declared: `byte[8] scratch`
                             * inside a loop, whose declaration the array hoist
                             * lifted to function scope. There is nothing left
                             * to assign, and emitting the bare name left
                             * `scratch;` in the output, a dead statement that
                             * is -Wunused-value in the user's own build. */
                            fprintf(gen->output, "\n");
                        }
                    }
                    // Handle trailing blocks on reassignment (same as first declaration)
                    if (stmt->child_count > 0 && stmt->children[0] &&
                        stmt->children[0]->type == AST_FUNCTION_CALL) {
                        ASTNode* reinit_call = stmt->children[0];
                        int reinit_is_builder = reinit_call->value &&
                            is_builder_func_reg(gen, reinit_call->value);
                        int reinit_has_trailing = 0;
                        for (int tc = 0; tc < reinit_call->child_count; tc++) {
                            if (reinit_call->children[tc] && reinit_call->children[tc]->type == AST_CLOSURE &&
                                reinit_call->children[tc]->value &&
                                strcmp(reinit_call->children[tc]->value, "trailing") == 0) {
                                reinit_has_trailing = 1;
                                break;
                            }
                        }
                        if (reinit_has_trailing && reinit_is_builder) {
                            // BUILDER PATTERN for reassignment
                            for (int tc = 0; tc < reinit_call->child_count; tc++) {
                                ASTNode* trailing = reinit_call->children[tc];
                                if (trailing && trailing->type == AST_CLOSURE &&
                                    trailing->value && strcmp(trailing->value, "trailing") == 0) {
                                    for (int bi = 0; bi < trailing->child_count; bi++) {
                                        if (trailing->children[bi] && trailing->children[bi]->type == AST_BLOCK) {
                                            print_indent(gen);
                                            fprintf(gen->output, "{\n");
                                            gen->indent_level++;
                                            print_indent(gen);
                                            /* (void*)(intptr_t) bridges int-returning factories. */
                                            fprintf(gen->output, "void* _bcfg = (void*)(intptr_t)%s();\n",
                                                    get_builder_factory(gen, reinit_call->value));
                                            print_indent(gen);
                                            fprintf(gen->output, "_aether_ctx_push(_bcfg);\n");
                                            emit_trailing_block_body(gen, trailing->children[bi]);
                                            print_indent(gen);
                                            const char* c_rfn = codegen_normalise_callee(
                                                safe_c_name(reinit_call->value));
                                            fprintf(gen->output, "%s = %s(", safe_c_name(stmt->value), c_rfn);
                                            int rarg = 0;
                                            for (int ai = 0; ai < reinit_call->child_count; ai++) {
                                                ASTNode* arg = reinit_call->children[ai];
                                                if (arg && arg->type == AST_CLOSURE &&
                                                    arg->value && strcmp(arg->value, "trailing") == 0) continue;
                                                if (rarg > 0) fprintf(gen->output, ", ");
                                                generate_expression(gen, arg);
                                                rarg++;
                                            }
                                            if (rarg > 0) fprintf(gen->output, ", ");
                                            fprintf(gen->output, "_bcfg);\n");
                                            gen->indent_level--;
                                            print_indent(gen);
                                            fprintf(gen->output, "}\n");
                                            break;
                                        }
                                    }
                                    break;
                                }
                            }
                        } else if (reinit_has_trailing) {
                            // REGULAR PATTERN: push reassigned value as context, run block
                            for (int tc = 0; tc < reinit_call->child_count; tc++) {
                                ASTNode* trailing = reinit_call->children[tc];
                                if (trailing && trailing->type == AST_CLOSURE &&
                                    trailing->value && strcmp(trailing->value, "trailing") == 0) {
                                    for (int bi = 0; bi < trailing->child_count; bi++) {
                                        if (trailing->children[bi] && trailing->children[bi]->type == AST_BLOCK) {
                                            print_indent(gen);
                                            fprintf(gen->output, "_aether_ctx_push((void*)(intptr_t)%s);\n",
                                                    safe_c_name(stmt->value));
                                            emit_trailing_block_body(gen, trailing->children[bi]);
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else if (stmt->child_count > 0 &&
                           string_take_is_view(gen, stmt->children[0])) {
                    /* #2461: first binding of a string local (one the
                     * function-entry hoist did not declare) to a view: declare
                     * it and its tracker, then take the value as the
                     * reassignment path does. */
                    Type* st = create_type(TYPE_STRING);
                    print_indent(gen);
                    fprintf(gen->output, "%sconst char* %s = NULL;",
                            try_volatile_qual_for(gen, stmt->value), stmt->value);
                    mark_var_declared_typed(gen, stmt->value, st);
                    free_type(st);
                    if (!is_heap_string_var(gen, stmt->value)) {
                        fprintf(gen->output, " int _heap_%s = 0; (void)_heap_%s;",
                                stmt->value, stmt->value);
                        mark_heap_string_var(gen, stmt->value);
                    }
                    fprintf(gen->output, "\n");
                    print_indent(gen);
                    emit_string_take_rebind(gen, stmt->value, stmt->children[0]);
                } else {
                    // First declaration - generate type + variable
                    /* #2289: a fixed-size array records its type, so a
                     * later binding of an array literal to the same name
                     * knows how many elements the C array holds. */
                    {
                        Type* arr_decl = NULL;
                        int arr_owned = 0;
                        if (stmt->node_type && stmt->node_type->kind == TYPE_ARRAY) {
                            if (stmt->node_type->array_size > 0) arr_decl = stmt->node_type;
                        } else if (stmt->child_count > 0 && stmt->children[0] &&
                                   stmt->children[0]->type == AST_ARRAY_LITERAL &&
                                   stmt->children[0]->child_count > 0) {
                            /* The `int name[N]` declaration below, for an
                             * initializer the type system left untyped. */
                            arr_decl = create_array_type(create_type(TYPE_INT),
                                                         stmt->children[0]->child_count);
                            arr_owned = 1;
                        }
                        mark_var_declared_typed(gen, stmt->value, arr_decl);
                        if (arr_owned) free_type(arr_decl);
                    }

                    // Detect if initializer is an array literal (type system may not tag empty arrays)
                    /* #1286: a literal into a `T[]` binding arrives wrapped
                     * in the typechecker's slice coercion; look through it. */
                    ASTNode* init_lit = (stmt->child_count > 0) ? stmt->children[0] : NULL;
                    if (init_lit && init_lit->type == AST_SLICE_FROM_ARRAY &&
                        init_lit->child_count > 0 &&
                        init_lit->children[0]->type == AST_ARRAY_LITERAL)
                        init_lit = init_lit->children[0];
                    int is_array_init = (init_lit && init_lit->type == AST_ARRAY_LITERAL);
                    /* #2478: an initializer list leaves its elements' order
                     * open; the ones that need it run first, in order. */
                    if (is_array_init)
                        order_prelude_begin(gen, init_lit->children, init_lit->child_count, NULL);
                    /* #1286: >= 0 when a `T[]` local takes an array literal
                     * (the literal's length; the slice views a hidden array). */
                    int slice_lit_len = -1;

                    // Handle array types specially (C syntax: int name[size])
                    /* Issue #501 follow-up: volatile prefix for
                     * try-clobbered locals.  Same `vq` value used
                     * across each branch below so we only consult
                     * the set once. */
                    const char* vq = try_volatile_qual_for(gen, stmt->value);
                    /* #2497: set when this local takes a string-owning struct
                     * that another owner keeps (emit_struct_take). */
                    const char* decl_struct_take = NULL;
                    if (stmt->node_type && stmt->node_type->kind == TYPE_ARRAY) {
                        const char* elem_type = get_c_type(stmt->node_type->element_type);
                        if (stmt->node_type->array_size > 0) {
                            fprintf(gen->output, "%s%s %s[%d]", vq, elem_type,
                                    stmt->value, stmt->node_type->array_size);
                            /* #2528: a local array of structs that own
                             * strings or closures owns its elements: each
                             * is destroyed at scope exit, an element store
                             * replaces (emit_struct_element_store). */
                            const char* owning_elem = struct_owning_strings(gen, stmt->node_type->element_type);
                            if (owning_elem && is_array_init && !is_promoted_capture(gen, stmt->value)) {
                                ASTNode* carrier = create_ast_node(AST_EXPRESSION_STATEMENT, NULL,
                                                                   stmt->line, stmt->column);
                                if (carrier) {
                                    if (carrier->annotation) free(carrier->annotation);
                                    carrier->annotation = heap_strf(
                                        "struct_array_destroy:%s:%s:%d", stmt->value,
                                        owning_elem, stmt->node_type->array_size);
                                    codegen_own_node(gen, carrier);
                                    push_defer(gen, carrier);
                                }
                            }
                        } else {
                            /* #1286: a `T[]` local is a slice. A literal
                             * initializer lands in a hidden fixed array the
                             * slice then views. */
                            if (is_array_init && init_lit->child_count > 0) {
                                slice_lit_len = init_lit->child_count;
                                fprintf(gen->output, "%s _ae_slit_%s[%d] = ",
                                        elem_type, stmt->value, slice_lit_len);
                                generate_expression(gen, init_lit);
                                fprintf(gen->output, ";\n");
                                print_indent(gen);
                            } else if (is_array_init) {
                                slice_lit_len = 0;
                            }
                            fprintf(gen->output, "%sAetherSlice %s", vq, stmt->value);
                        }
                    } else if (is_array_init) {
                        // Type system missed array type but initializer is array literal
                        int arr_size = stmt->children[0]->child_count;
                        if (arr_size > 0) {
                            fprintf(gen->output, "%sint %s[%d]", vq, stmt->value, arr_size);
                        } else {
                            // Empty array [] - use NULL pointer
                            fprintf(gen->output, "%sint* %s", vq, stmt->value);
                        }
                    } else if (stmt->child_count > 0 && stmt->children[0] &&
                               (stmt->children[0]->type == AST_MESSAGE_CONSTRUCTOR ||
                                stmt->children[0]->type == AST_STRUCT_LITERAL) &&
                               stmt->children[0]->value) {
                        // Message/struct constructor — use the constructor name as type.
                        // A header-defined struct is spelled `struct Name`, as
                        // get_c_type spells it: the header need not typedef the tag
                        // (a union `union Name`, #2561).
                        int c_tagged = stmt->children[0]->type == AST_STRUCT_LITERAL &&
                                       aether_is_c_import_struct(stmt->children[0]->value);
                        fprintf(gen->output, "%s%s%s%s %s", vq,
                                c_tagged ? aether_c_tag(stmt->children[0]->value) : "",
                                c_tagged ? " " : "",
                                stmt->children[0]->value, stmt->value);
                        /* Struct-field heap-string ownership (#465).
                         * Push a function-exit defer that calls the
                         * auto-emitted <Struct>_destroy(&<var>) to
                         * free any heap-owned string fields. The
                         * destroy function is no-op for structs
                         * without heap-string fields, but we only
                         * push the defer when the field set actually
                         * needs cleanup (skips the no-op call). */
                        if (stmt->children[0]->type == AST_STRUCT_LITERAL && gen->program) {
                            ASTNode* sdef = find_struct_definition_by_name(
                                gen->program, stmt->children[0]->value);
                            if (sdef && struct_owns_heap_strings(gen, sdef)) {
                                ASTNode* carrier = create_ast_node(
                                    AST_EXPRESSION_STATEMENT, NULL,
                                    stmt->line, stmt->column);
                                if (carrier) {
                                    if (carrier->annotation) free(carrier->annotation);
                                    carrier->annotation = heap_strf(
                                        "struct_destroy:%s:%s",
                                        stmt->value, stmt->children[0]->value);
                                    codegen_own_node(gen, carrier);
                                    push_defer(gen, carrier);
                                }
                            }
                        }
                    } else {
                        // Determine the best type for this variable
                        Type* var_type = stmt->node_type;

                        // If type is void/unknown, try to get it from the initializer
                        if ((!var_type || var_type->kind == TYPE_VOID || var_type->kind == TYPE_UNKNOWN)
                            && stmt->child_count > 0 && stmt->children[0]) {
                            ASTNode* init = stmt->children[0];
                            // Check initializer's own node_type
                            if (init->node_type && init->node_type->kind != TYPE_VOID
                                && init->node_type->kind != TYPE_UNKNOWN) {
                                var_type = init->node_type;
                            }
                            // For function calls, look up the function's return type
                            else if (init->type == AST_FUNCTION_CALL && init->value) {
                                for (int fi = 0; fi < gen->program->child_count; fi++) {
                                    ASTNode* fn = gen->program->children[fi];
                                    if (fn && (fn->type == AST_FUNCTION_DEFINITION || fn->type == AST_BUILDER_FUNCTION)
                                        && fn->value && strcmp(fn->value, init->value) == 0) {
                                        if (fn->node_type && fn->node_type->kind != TYPE_VOID
                                            && fn->node_type->kind != TYPE_UNKNOWN) {
                                            var_type = fn->node_type;
                                        } else if (has_return_value(fn)) {
                                            // Same heuristic as generate_function_definition:
                                            // function has return-with-value but type is void → int
                                            static Type int_type = { .kind = TYPE_INT };
                                            var_type = &int_type;
                                        }
                                        break;
                                    }
                                }
                            }
                        }

                        /* Issue #501 follow-up: `vq` from above
                         * carries "volatile " when this local is
                         * modified inside a try body of the current
                         * function and we're emitting the decl at
                         * outer scope.  Without this, the C optimizer
                         * may keep the local in a register that the
                         * panic's siglongjmp clobbers and the catch
                         * handler reads the stale pre-try value.
                         * C99 7.13.2.1. */
                        fprintf(gen->output, "%s", vq);
                        generate_type(gen, var_type);
                        fprintf(gen->output, " %s", stmt->value);
                        /* #752 (caller side): a struct-with-heap-fields
                         * received from a struct-returning CALL transfers
                         * ownership to this local — free its fields at
                         * scope exit. Gated on a function-call initializer
                         * (an owned, freshly-returned struct); a plain
                         * alias (`o2 = o`) is not a call and gets no
                         * defer, so there's no double-free. */
                        if (stmt->child_count > 0 && stmt->children[0] &&
                            stmt->children[0]->type == AST_FUNCTION_CALL) {
                            push_struct_destroy_defer(gen, stmt->value, var_type,
                                                      stmt->line, stmt->column);
                        }
                        /* #2497: `o2 = o`, `x = o.inner`, `e = arr[i]`: the
                         * local used to alias a struct its owner frees, while
                         * a later replace of it freed that owner's strings
                         * too. It now takes the struct (moved out of a local
                         * on its last use, copied otherwise) and owns it. */
                        if (stmt->child_count > 0 &&
                            struct_take_shape(stmt->children[0]) &&
                            struct_owning_strings(gen, var_type)) {
                            decl_struct_take = struct_owning_strings(gen, var_type);
                            push_struct_destroy_defer(gen, stmt->value, var_type,
                                                      stmt->line, stmt->column);
                        }
                    }

                    if (stmt->child_count > 0) {
                        // Check if this is a builder function with trailing block —
                        // if so, just declare the variable; the builder handler assigns later
                        int defer_with_trailing = 0;
                        if (stmt->children[0] && stmt->children[0]->type == AST_FUNCTION_CALL &&
                            stmt->children[0]->value && is_builder_func_reg(gen, stmt->children[0]->value)) {
                            for (int dtc = 0; dtc < stmt->children[0]->child_count; dtc++) {
                                ASTNode* dtarg = stmt->children[0]->children[dtc];
                                if (dtarg && dtarg->type == AST_CLOSURE &&
                                    dtarg->value && strcmp(dtarg->value, "trailing") == 0) {
                                    defer_with_trailing = 1;
                                    break;
                                }
                            }
                        }
                        if (defer_with_trailing) {
                            // Just declare — defer trailing block handler will assign
                            fprintf(gen->output, " = 0");
                        } else if (is_array_init && stmt->children[0]->child_count == 0) {
                            // Empty array literal gets NULL, not {}
                            fprintf(gen->output, " = NULL");
                        } else {
                            /* Live-source alias copy (180-regression.md):
                             * `dest = src` where src is a still-live heap-
                             * tracked local must NOT steal src's buffer.
                             * Take a binary-safe defensive copy so dest
                             * owns an independent buffer and src is left
                             * intact for its later reads. The matching
                             * heap-flag block below sets `_heap_dest = 1`
                             * and leaves `_heap_src` untouched. */
                            ASTNode* di = stmt->children[0];
                            int copy_alias = (di && di->type == AST_IDENTIFIER &&
                                              di->value &&
                                              is_heap_string_var(gen, di->value) &&
                                              strcmp(di->value, stmt->value) != 0 &&
                                              alias_source_must_copy(gen, di->value));
                            fprintf(gen->output, " = ");
                            if (slice_lit_len == 0) {
                                fprintf(gen->output, "aether_slice_make((void*)0, 0)");
                            } else if (slice_lit_len > 0) {
                                fprintf(gen->output, "aether_slice_make(_ae_slit_%s, %d)",
                                        stmt->value, slice_lit_len);
                            } else if (copy_alias) {
                                fprintf(gen->output, "aether_uniform_heap_str(");
                                generate_expression(gen, di);
                                fprintf(gen->output, ", 0)");
                            } else if (decl_struct_take) {
                                emit_struct_take(gen, stmt->children[0],
                                                 decl_struct_take, stmt->value);
                            } else if (closure_view_binding(stmt->children[0]) &&
                                       !is_promoted_capture(gen, stmt->value) &&
                                       !is_module_global_var(gen, stmt->value) &&
                                       !is_actor_state_var(gen, stmt->value)) {
                                /* #2525: `x = h.cb` holds a reference of its
                                 * own, released by this scope, so `h` may
                                 * be destroyed first. */
                                emit_closure_take(gen, stmt->children[0]);
                            } else {
                                generate_expression(gen, stmt->children[0]);
                            }
                        }
                    }

                    fprintf(gen->output, ";\n");
                    // Emit heap-ownership flag for string variables.
                    // This flag is checked at reassignment to avoid freeing
                    // string literals; it's set to 1 after the first heap
                    // string assignment (string_concat, string_substring, etc.).
                    {
                        Type* vt = stmt->node_type;
                        if ((!vt || vt->kind == TYPE_UNKNOWN || vt->kind == TYPE_VOID)
                            && stmt->child_count > 0 && stmt->children[0]
                            && stmt->children[0]->node_type) {
                            vt = stmt->children[0]->node_type;
                        }
                        int is_string_var = (vt && vt->kind == TYPE_STRING);
                        // Also detect string by initializer: literal string or string function
                        if (!is_string_var && stmt->child_count > 0 && stmt->children[0]) {
                            ASTNode* init = stmt->children[0];
                            if (init->type == AST_LITERAL && init->value &&
                                init->node_type && init->node_type->kind == TYPE_STRING) {
                                is_string_var = 1;
                            }
                            if (is_heap_string_expr(gen, init)) {
                                is_string_var = 1;
                            }
                        }
                        if (is_string_var) {
                            int init_heap = (stmt->child_count > 0 &&
                                             is_heap_string_expr(gen, stmt->children[0]));
                            /* Alias-ownership transfer at first declaration.
                             * `sorted = new_sorted` (bare-id RHS to a heap-
                             * tracked local) MUST move the ownership flag,
                             * not duplicate it. Otherwise both slots claim
                             * the same buffer and one of them double-frees
                             * (function-exit defer for the source, caller's
                             * reassign-wrapper for the destination). The
                             * reassignment branch at codegen_stmt.c:2647
                             * has the same logic; this is the mirror for
                             * the first-declaration shape. */
                            ASTNode* alias_init = (stmt->child_count > 0) ? stmt->children[0] : NULL;
                            int init_is_alias_to_heap_var =
                                (alias_init &&
                                 alias_init->type == AST_IDENTIFIER &&
                                 alias_init->value &&
                                 is_heap_string_var(gen, alias_init->value) &&
                                 strcmp(alias_init->value, stmt->value) != 0);
                            print_indent(gen);
                            // Issue #405: the function-entry hoist
                            // (hoist_heap_string_trackers, called from
                            // generate_function_definition before this
                            // statement runs) may have already declared
                            // `int _heap_<name> = 0;`. Re-declaring it
                            // here would be a duplicate-definition C
                            // error. Detect via is_heap_string_var and
                            // emit assignment-only when already hoisted.
                            /* Live-source guard (180-regression.md): when
                             * the source is read again after this alias,
                             * a move would free its buffer out from under
                             * it on the alias's next reassignment. The
                             * `dest = aether_uniform_heap_str(src, 0)`
                             * rewrite above already replaced the bare
                             * alias with a defensive copy, so here the
                             * dest simply owns its own copy and the source
                             * keeps its flag untouched. */
                            int alias_copied = (init_is_alias_to_heap_var &&
                                                alias_source_must_copy(gen, alias_init->value));
                            if (is_heap_string_var(gen, stmt->value)) {
                                if (alias_copied) {
                                    fprintf(gen->output, "_heap_%s = 1;", stmt->value);
                                } else if (init_is_alias_to_heap_var) {
                                    fprintf(gen->output,
                                            "_heap_%s = _heap_%s; _heap_%s = 0;",
                                            stmt->value, alias_init->value, alias_init->value);
                                } else {
                                    fprintf(gen->output, "_heap_%s = %d;",
                                            stmt->value, init_heap ? 1 : 0);
                                }
                            } else {
                                if (alias_copied) {
                                    fprintf(gen->output,
                                            "int _heap_%s = 1; (void)_heap_%s;",
                                            stmt->value, stmt->value);
                                } else if (init_is_alias_to_heap_var) {
                                    fprintf(gen->output,
                                            "int _heap_%s = _heap_%s; (void)_heap_%s; _heap_%s = 0;",
                                            stmt->value, alias_init->value,
                                            stmt->value, alias_init->value);
                                } else {
                                    fprintf(gen->output, "int _heap_%s = %d; (void)_heap_%s;",
                                            stmt->value, init_heap ? 1 : 0, stmt->value);
                                }
                                mark_heap_string_var(gen, stmt->value);
                            }
                            emit_unwind_track_local(gen, stmt->value);
                            fprintf(gen->output, "\n");
                        }
                    }
                    // Record variable→closure mapping for closure invocation.
                    // If the variable was previously bound to a different
                    // closure (e.g. reassigned from |a,b|->a+b to |a,b|->a*b),
                    // mark the entry as ambiguous (closure_id = -1) so
                    // call() falls back to generic function-pointer dispatch
                    // through .fn — which always reflects the currently-stored
                    // closure, not whichever one was first assigned.
                    if (stmt->child_count > 0 && stmt->children[0] &&
                        stmt->children[0]->type == AST_CLOSURE &&
                        stmt->children[0]->value && stmt->value) {
                        closure_var_bind(gen, gen->closure_var_scope, stmt->value,
                                         atoi(stmt->children[0]->value));

                        /* Free the env at scope exit when the value stays
                         * here (#2480). The free used to be pushed only when
                         * closure_var_map had no entry for the name yet, but
                         * discover_closures seeds that map for every closure
                         * binding before any statement is emitted, so the
                         * free was never pushed and every call leaked the
                         * env and the cells it holds. */
                        claim_closure_local_env(gen, stmt->value, stmt, 1);
                    } else if (stmt->child_count > 0 && stmt->children[0] &&
                               stmt->children[0]->type == AST_FUNCTION_CALL && stmt->value) {
                        /* #2494: the result of a function that returns a
                         * closure only its caller holds is freed here too. */
                        claim_closure_local_env(gen, stmt->value, stmt, 1);
                    } else if (stmt->child_count > 0 &&
                               (closure_view_binding(stmt->children[0]) ||
                                closure_ask_binding(stmt->children[0])) &&
                               stmt->value) {
                        /* #2525: a retained field or element read, likewise;
                         * #2528: a reply's closure too. */
                        claim_closure_local_env(gen, stmt->value, stmt, 1);
                    }
                    // Suppress unused-variable warning for arrays used with list
                    // pattern matching — the paired _len variable may be the only
                    // one used when patterns only check size ([], [_], wildcard).
                    if (is_array_init || (stmt->node_type && stmt->node_type->kind == TYPE_ARRAY)) {
                        print_line(gen, "(void)%s;", stmt->value);
                    }

                    // Handle trailing blocks on function calls used as initializers
                    // e.g., root = make_container("root") { ... }
                    if (stmt->child_count > 0 && stmt->children[0] &&
                        stmt->children[0]->type == AST_FUNCTION_CALL) {
                        ASTNode* init_call = stmt->children[0];
                        int init_is_defer = init_call->value &&
                            is_builder_func_reg(gen, init_call->value);

                        if (init_is_defer) {
                            // DEFER PATTERN for assignment: block first, then call
                            // The variable was already declared with func(args, (void*)0)
                            // We need to redo it: create config, run block, reassign with config
                            for (int tc = 0; tc < init_call->child_count; tc++) {
                                ASTNode* trailing = init_call->children[tc];
                                if (trailing && trailing->type == AST_CLOSURE &&
                                    trailing->value && strcmp(trailing->value, "trailing") == 0) {
                                    for (int bi = 0; bi < trailing->child_count; bi++) {
                                        if (trailing->children[bi] &&
                                            trailing->children[bi]->type == AST_BLOCK) {
                                            // Open block scope for _bcfg
                                            print_indent(gen);
                                            fprintf(gen->output, "{\n");
                                            gen->indent_level++;
                                            print_indent(gen);
                                            /* (void*)(intptr_t) bridges int-returning factories. */
                                            fprintf(gen->output, "void* _bcfg = (void*)(intptr_t)%s();\n",
                                                    get_builder_factory(gen, init_call->value));
                                            print_indent(gen);
                                            fprintf(gen->output, "_aether_ctx_push(_bcfg);\n");
                                            // Run trailing block
                                            emit_trailing_block_body(gen, trailing->children[bi]);
                                            // Reassign variable with defer config
                                            print_indent(gen);
                                            const char* c_dfn = codegen_normalise_callee(
                                                safe_c_name(init_call->value));
                                            fprintf(gen->output, "%s = %s(",
                                                    safe_c_name(stmt->value), c_dfn);
                                            int darg = 0;
                                            for (int ai = 0; ai < init_call->child_count; ai++) {
                                                ASTNode* arg = init_call->children[ai];
                                                if (arg && arg->type == AST_CLOSURE &&
                                                    arg->value && strcmp(arg->value, "trailing") == 0) {
                                                    continue;
                                                }
                                                if (darg > 0) fprintf(gen->output, ", ");
                                                generate_expression(gen, arg);
                                                darg++;
                                            }
                                            if (darg > 0) fprintf(gen->output, ", ");
                                            fprintf(gen->output, "_bcfg);\n");
                                            gen->indent_level--;
                                            print_indent(gen);
                                            fprintf(gen->output, "}\n");
                                            break;
                                        }
                                    }
                                    break;
                                }
                            }
                        } else {
                            // REGULAR PATTERN: function already called, push result as context
                            for (int tc = 0; tc < init_call->child_count; tc++) {
                                ASTNode* trailing = init_call->children[tc];
                                if (trailing && trailing->type == AST_CLOSURE &&
                                    trailing->value && strcmp(trailing->value, "trailing") == 0) {
                                    for (int bi = 0; bi < trailing->child_count; bi++) {
                                        if (trailing->children[bi] &&
                                            trailing->children[bi]->type == AST_BLOCK) {
                                            // Push the variable's value as builder context
                                            print_indent(gen);
                                            fprintf(gen->output, "_aether_ctx_push((void*)(intptr_t)%s);\n",
                                                    safe_c_name(stmt->value));
                                            emit_trailing_block_body(gen, trailing->children[bi]);
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            break;
        }

        case AST_ASSIGNMENT:
            if (stmt->child_count >= 2) {
                ASTNode* lhs = stmt->children[0];
                ASTNode* rhs = stmt->children[1];

                /* #340: optional-chain assignment `recv?.field = rhs`. */
                if (emit_optional_chain_assign(gen, lhs, rhs)) break;

                /* #790: a whole-variable reassignment updates heap.new box
                 * provenance (a member-access LHS like `box.field = ...` does
                 * not — it is a field store, handled below). */
                if (lhs && lhs->type == AST_IDENTIFIER && lhs->value) {
                    /* #1860: a call whose every return is heap.new(T) hands
                     * back that same zero-initialised box, so provenance has
                     * to survive the call. */
                    if (rhs && (rhs->type == AST_HEAP_NEW ||
                                call_yields_heap_box(gen, rhs)))
                        mark_heap_box_var(gen, lhs->value);
                    else
                        unmark_heap_box_var(gen, lhs->value);
                }

                // Check if RHS is a function call with a trailing block
                int assign_has_trailing = 0;
                if (rhs && rhs->type == AST_FUNCTION_CALL) {
                    for (int tc = 0; tc < rhs->child_count; tc++) {
                        if (rhs->children[tc] && rhs->children[tc]->type == AST_CLOSURE &&
                            rhs->children[tc]->value &&
                            strcmp(rhs->children[tc]->value, "trailing") == 0) {
                            assign_has_trailing = 1;
                            break;
                        }
                    }
                }

                /* Struct-field heap-string ownership (#465). Helper
                 * handles both AST_ASSIGNMENT and AST_EXPRESSION_
                 * STATEMENT-wrapping-BINARY-`=` shapes (the parser
                 * lands at different node types depending on whether
                 * the LHS is a struct field vs. an array element vs.
                 * a bare local — keep both paths routed through the
                 * same wrapper). */
                if (emit_struct_field_heap_assign(gen, lhs, rhs)) break;
                if (emit_cell_string_element_store(gen, lhs, rhs)) break;   /* #2474 */

                /* #891 @c_struct overlay write (incl. nested `s.a.b = v`):
                 * lowers to a width-correct aether_mem_set_<width> at the
                 * cumulative offset (no C `->field =`). */
                if (lhs && lhs->type == AST_MEMBER_ACCESS &&
                    aether_c_struct_overlay_lhs(lhs)) {
                    const char* cpath = NULL;
                    ASTNode* root = aether_c_struct_chain(lhs, &cpath);
                    const char* sname = root->node_type->element_type->struct_name;
                    long off = 0; const char* width = NULL;
                    if (aether_c_struct_resolve(sname, cpath, &off, &width) && width) {
                        fprintf(gen->output, "aether_mem_set_%s((void*)(", width);
                        generate_expression(gen, root);
                        fprintf(gen->output, "), %ld, ", off);
                        generate_expression(gen, rhs);
                        fprintf(gen->output, ");\n");
                        break;
                    }
                }

                /* #1132 bitstruct field write: `b.f = v` lowers to a
                 * read-modify-write on the backing word —
                 *   b = (b & ~(mask << lo)) | ((v & mask) << lo);
                 * The RHS is masked before shifting, so an out-of-range value
                 * truncates to the field rather than corrupting its neighbours.
                 * No C bitfield is involved, so the layout is exact. */
                if (lhs && lhs->type == AST_MEMBER_ACCESS && lhs->value) {
                    const char* bname = aether_bitstruct_base_name(lhs);
                    if (bname) {
                        int lo = 0, hi = 0, is_bool = 0;
                        const char* backing = NULL;
                        if (aether_bitstruct_resolve(bname, lhs->value, &lo, &hi,
                                                     &is_bool, &backing)) {
                            unsigned long long mask = aether_bitstruct_mask(lo, hi);
                            ASTNode* base = lhs->children[0];
                            print_indent(gen);
                            generate_expression(gen, base);
                            fprintf(gen->output, " = (%s)((", backing ? backing : "unsigned char");
                            generate_expression(gen, base);
                            fprintf(gen->output, " & ~(0x%llxULL << %d)) | ((((unsigned long long)(",
                                    mask, lo);
                            generate_expression(gen, rhs);
                            fprintf(gen->output, ")) & 0x%llxULL) << %d));\n", mask, lo);
                            break;
                        }
                    }
                }

                // Generate the assignment itself
                gen->generating_lvalue = 1;
                generate_expression(gen, lhs);
                gen->generating_lvalue = 0;
                fprintf(gen->output, " = ");
                generate_expression(gen, rhs);
                fprintf(gen->output, ";\n");

                // Handle trailing blocks on the RHS function call
                // Same logic as VAR_DECLARATION trailing block handler
                if (assign_has_trailing && rhs->type == AST_FUNCTION_CALL) {
                    int assign_is_builder = rhs->value &&
                        is_builder_func_reg(gen, rhs->value);

                    if (assign_is_builder) {
                        // BUILDER PATTERN: block first, then call
                        for (int tc = 0; tc < rhs->child_count; tc++) {
                            ASTNode* trailing = rhs->children[tc];
                            if (trailing && trailing->type == AST_CLOSURE &&
                                trailing->value && strcmp(trailing->value, "trailing") == 0) {
                                for (int bi = 0; bi < trailing->child_count; bi++) {
                                    if (trailing->children[bi] &&
                                        trailing->children[bi]->type == AST_BLOCK) {
                                        print_indent(gen);
                                        fprintf(gen->output, "{\n");
                                        gen->indent_level++;
                                        print_indent(gen);
                                        /* Cast the factory's scalar return into void* to keep the ctx
                                             * slot universally void*-shaped. Without the cast, an
                                             * int-returning factory (e.g. aether-ui's
                                             * `_surface_window_factory() -> int`) emits
                                             * `void* _bcfg = int_fn();` — a bare int→pointer
                                             * conversion that GCC 14+/MinGW64 reject under
                                             * default -Werror=int-conversion. The (intptr_t)
                                             * intermediate is a no-op for ptr factories. See
                                             * builder-ctx-handle-void-ptr-int-conversion.md. */
                                            fprintf(gen->output, "void* _bcfg = (void*)(intptr_t)%s();\n",
                                                get_builder_factory(gen, rhs->value));
                                        print_indent(gen);
                                        fprintf(gen->output, "_aether_ctx_push(_bcfg);\n");
                                        emit_trailing_block_body(gen, trailing->children[bi]);
                                        // Reassign with config
                                        print_indent(gen);
                                        gen->generating_lvalue = 1;
                                        generate_expression(gen, lhs);
                                        gen->generating_lvalue = 0;
                                        const char* c_fn = codegen_normalise_callee(
                                            safe_c_name(rhs->value));
                                        fprintf(gen->output, " = %s(", c_fn);
                                        int darg = 0;
                                        for (int ai = 0; ai < rhs->child_count; ai++) {
                                            ASTNode* arg = rhs->children[ai];
                                            if (arg && arg->type == AST_CLOSURE &&
                                                arg->value && strcmp(arg->value, "trailing") == 0) {
                                                continue;
                                            }
                                            if (darg > 0) fprintf(gen->output, ", ");
                                            generate_expression(gen, arg);
                                            darg++;
                                        }
                                        if (darg > 0) fprintf(gen->output, ", ");
                                        fprintf(gen->output, "_bcfg);\n");
                                        gen->indent_level--;
                                        print_indent(gen);
                                        fprintf(gen->output, "}\n");
                                        break;
                                    }
                                }
                                break;
                            }
                        }
                    } else {
                        // REGULAR PATTERN: push assigned value as context, run block
                        for (int tc = 0; tc < rhs->child_count; tc++) {
                            ASTNode* trailing = rhs->children[tc];
                            if (trailing && trailing->type == AST_CLOSURE &&
                                trailing->value && strcmp(trailing->value, "trailing") == 0) {
                                for (int bi = 0; bi < trailing->child_count; bi++) {
                                    if (trailing->children[bi] &&
                                        trailing->children[bi]->type == AST_BLOCK) {
                                        // Push the variable's value as builder context
                                        print_indent(gen);
                                        // For simple identifiers, use the variable name directly
                                        fprintf(gen->output, "_aether_ctx_push((void*)(intptr_t)");
                                        gen->generating_lvalue = 1;
                                        generate_expression(gen, lhs);
                                        gen->generating_lvalue = 0;
                                        fprintf(gen->output, ");\n");
                                        emit_trailing_block_body(gen, trailing->children[bi]);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            break;

        case AST_COMPOUND_ASSIGNMENT: {
            // node->value = variable name, children[0] = operator literal, children[1] = RHS
            if (stmt->child_count >= 2 && stmt->value && stmt->children[0] && stmt->children[0]->value) {
                const char* op = stmt->children[0]->value;  // "+=", "-=", etc.

                // Check if this is a state variable in an actor
                int is_state_var = 0;
                if (gen->current_actor && stmt->value) {
                    for (int i = 0; i < gen->state_var_count; i++) {
                        if (strcmp(stmt->value, gen->actor_state_vars[i]) == 0) {
                            is_state_var = 1;
                            break;
                        }
                    }
                }

                /* #2428: `v op= x` on an eight-lane value is `v = helper(v, x)`
                 * (the halves form is a struct; see codegen_expr.c). The
                 * typechecker records the target's type on the node for this. */
                TypeKind tk8 = stmt->node_type ? stmt->node_type->kind : TYPE_UNKNOWN;
                const char* fn8 = NULL;
                if ((tk8 == TYPE_F32X8 || tk8 == TYPE_I32X8) && op[0] && op[1] == '=') {
                    switch (op[0]) {
                        case '+': fn8 = "add"; break;
                        case '-': fn8 = "sub"; break;
                        case '*': fn8 = "mul"; break;
                        case '/': fn8 = "div"; break;
                        case '%': fn8 = "mod"; break;
                        default: break;
                    }
                }
                /* The place written. A variable a closure captures by
                 * reference lives in a shared cell, and its C name is the
                 * cell's POINTER (`int* x`), in the closure body and in the
                 * declaring function alike: plain assignment writes `*x`.
                 * `x += 1` was emitted as is, which is pointer arithmetic on
                 * the cell, so the value never changed and a later `*x`
                 * read the wrong memory, with no diagnostic. */
                const char* target = stmt->value;
                if (is_state_var)
                    target = cg_internf("self->%s", stmt->value);
                else if (is_promoted_capture(gen, stmt->value))
                    target = cg_internf("(*%s)", stmt->value);
                if (fn8) {
                    const char* pfx = tk8 == TYPE_F32X8 ? "f32x8" : "i32x8";
                    Type* rt = stmt->children[1]->node_type;
                    fprintf(gen->output, "%s = _ae_%s_%s(%s, ", target, pfx, fn8, target);
                    if (rt && rt->kind == tk8) {
                        generate_expression(gen, stmt->children[1]);
                    } else {
                        fprintf(gen->output, "_ae_%s_splat((%s)(", pfx, tk8 == TYPE_F32X8 ? "float" : "int");
                        generate_expression(gen, stmt->children[1]);
                        fprintf(gen->output, "))");
                    }
                    fprintf(gen->output, ");\n");
                    break;
                }
                fprintf(gen->output, "%s %s ", target, op);
                generate_expression(gen, stmt->children[1]);
                fprintf(gen->output, ";\n");
            }
            break;
        }

        case AST_IF_STATEMENT:
            // Hoist any variable that's first-assigned in BOTH branches to
            // the outer scope before opening the if. Without this, the
            // C-side declarations stay block-local and disappear at the
            // closing `}`, even though Aether semantics expect them to
            // survive the merge. See docs/notes/compiler_notes_from_vcr_port.md
            // item #2.
            if (stmt->child_count > 2) {
                hoist_if_else_common_vars(gen, stmt->children[1], stmt->children[2]);
            }

            fprintf(gen->output, "if (");
            if (stmt->child_count > 0) emit_condition(gen, stmt->children[0]);   /* #2582 */
            fprintf(gen->output, ") {\n");

            {
                // Save declared_var_count before if-body.  Variables declared
                // inside if/else blocks live in separate C scopes and must not
                // leak to sibling statements (fixes Issue #2: sibling if blocks
                // re-using the same variable name).
                int saved_var_count = gen->declared_var_count;

                indent(gen);
                if (stmt->child_count > 1) {
                    generate_statement(gen, stmt->children[1]);
                }
                unindent(gen);

                if (stmt->child_count > 2) {
                    // Restore: else-branch sees only pre-if declarations.
                    truncate_declared_vars(gen, saved_var_count);

                    /* Anchor the `} else {` line. Without a directive here
                     * it inherits the then-branch's drifting count and lands
                     * on the line of the ELSE BODY, so gcov charges the
                     * branch-decision counter to a statement that never ran
                     * and an untaken branch reports as covered. */
                    codegen_maybe_emit_line(gen, stmt->children[2]);
                    print_line(gen, "} else {");
                    indent(gen);
                    generate_statement(gen, stmt->children[2]);
                    unindent(gen);
                }

                // Restore after entire if/else: variables declared inside
                // if/else blocks do not leak to subsequent sibling statements.
                truncate_declared_vars(gen, saved_var_count);
            }

            print_line(gen, "}");
            break;
            
        case AST_FOR_LOOP:
            fprintf(gen->output, "for (");
            if (stmt->child_count > 0 && stmt->children[0]) {
                ASTNode* init = stmt->children[0];
                if (init->type == AST_VARIABLE_DECLARATION) {
                    generate_type(gen, init->node_type);
                    fprintf(gen->output, " %s", init->value);
                    if (init->child_count > 0) {
                        fprintf(gen->output, " = ");
                        generate_expression(gen, init->children[0]);
                    }
                } else {
                    generate_expression(gen, init);
                }
            }
            fprintf(gen->output, "; ");
            if (stmt->child_count > 1 && stmt->children[1]) {
                generate_expression(gen, stmt->children[1]); // condition
            }
            // Note: If no condition, C for loop becomes infinite (for (;;))
            fprintf(gen->output, "; ");
            if (stmt->child_count > 2 && stmt->children[2]) {
                generate_expression(gen, stmt->children[2]); // increment
            }
            fprintf(gen->output, ") {\n");
            
            indent(gen);
            if (gen->preempt_loops) {
                print_line(gen, "if (--_aether_reductions <= 0) { _aether_reductions = 10000; sched_yield(); }");
            }
            // Issue #343 codegen tripwire: under --emit=lib, emit a
            // deadline check at every loop head. The check is one
            // TLS read + one atomic load + branch; clock_gettime
            // only when the deadline is armed. Zero cost on
            // --emit=exe builds (the if (gen->emit_lib) gate elides
            // the print entirely).
            if (gen->emit_lib) {
                print_line(gen, "if (aether_caps_armed && aether_caps_deadline_tripped()) { __aether_abort_call(); break; }");
            }
            /* Issue #501: snapshot try_frame_depth at loop entry.
             * #893: record the loop's label / containing scope / C-label id. */
            int for_lbl_idx = -1;
            if (gen->loop_nest_depth < AETHER_MAX_LOOP_NEST) {
                for_lbl_idx = gen->loop_nest_depth;
                gen->loop_try_base[for_lbl_idx] = gen->try_frame_depth;
                gen->loop_label[for_lbl_idx] = stmt->value;
                gen->loop_label_scope[for_lbl_idx] = gen->scope_depth;
                gen->loop_label_id[for_lbl_idx] = gen->next_loop_label_id++;
                gen->loop_label_break_used[for_lbl_idx] = 0;
                gen->loop_label_continue_used[for_lbl_idx] = 0;
                gen->loop_nest_depth++;
            }
            if (stmt->child_count > 3 && stmt->children[3]) {
                // Body is always a statement (could be a block or single statement)
                generate_statement(gen, stmt->children[3]); // body
            }
            if (gen->loop_nest_depth > 0) gen->loop_nest_depth--;
            /* #893: labeled-continue target — end of the body, so falling
             * through runs the C `for`'s increment then re-tests the condition. */
            if (for_lbl_idx >= 0 && gen->loop_label[for_lbl_idx] &&
                gen->loop_label_continue_used[for_lbl_idx]) {
                print_line(gen, "__ae_cont_%d: ;", gen->loop_label_id[for_lbl_idx]);
            }
            unindent(gen);

            print_line(gen, "}");
            /* #893: labeled-break target — after the loop. */
            if (for_lbl_idx >= 0 && gen->loop_label[for_lbl_idx] &&
                gen->loop_label_break_used[for_lbl_idx]) {
                print_line(gen, "__ae_brk_%d: ;", gen->loop_label_id[for_lbl_idx]);
            }
            break;

        case AST_WHILE_LOOP: {
            // OPTIMIZATION: Try to collapse arithmetic series loops into O(1) expressions.
            // Only attempt when not inside actors and no sends (sends need batch treatment).
            int has_sends = contains_send_expression(stmt);
            if (!has_sends && try_emit_series_collapse(gen, stmt)) {
                break;  // collapsed — done
            }

            // Batch optimization: only in main() (not inside actors)
            // Uses queue_enqueue_batch to reduce atomics from N to num_cores
            if (has_sends && gen->current_actor == NULL) {
                print_line(gen, "scheduler_send_batch_start();");
                gen->in_main_loop = 1;
            }

            // Hoist variable declarations from loop body to function scope
            // so they're visible to subsequent while blocks
            if (stmt->child_count > 1) {
                hoist_loop_vars(gen, stmt->children[1]);
            }

            /* #2378: an interpreter dispatch loop gets a threaded-dispatch
             * table (see td_loop_switch). Not under batched sends, whose loop
             * shape is managed separately. */
            ASTNode* td_sw = NULL;
            int td_id = 0;
            if (!has_sends && gen->loop_nest_depth < AETHER_MAX_LOOP_NEST) {
                td_sw = td_loop_switch(gen, stmt);
                if (td_sw) {
                    td_id = ++gen->next_td_id;
                    td_emit_table(gen, td_sw, td_id);
                }
            }

            if (td_id) print_indent(gen);   /* the table ended on a fresh line */
            fprintf(gen->output, "while (");
            if (stmt->child_count > 0) emit_condition(gen, stmt->children[0]);   /* #2582 */
            fprintf(gen->output, ") {\n");

            indent(gen);
            // Cooperative preemption: yield to OS at loop back-edges
            if (gen->preempt_loops) {
                print_line(gen, "if (--_aether_reductions <= 0) { _aether_reductions = 10000; sched_yield(); }");
            }
            // Issue #343 codegen tripwire — see AST_FOR_LOOP comment.
            if (gen->emit_lib) {
                print_line(gen, "if (aether_caps_armed && aether_caps_deadline_tripped()) { __aether_abort_call(); break; }");
            }
            /* Issue #501: snapshot try_frame_depth at loop entry so
             * `break` / `continue` inside the body drains only
             * frames pushed *inside* the loop, not outer-scope tries.
             * #893: record the loop's label / containing scope / a fresh
             * C-label id in the same slot. */
            int while_lbl_idx = -1;
            if (gen->loop_nest_depth < AETHER_MAX_LOOP_NEST) {
                while_lbl_idx = gen->loop_nest_depth;
                gen->loop_try_base[while_lbl_idx] = gen->try_frame_depth;
                gen->loop_label[while_lbl_idx] = stmt->value;
                gen->loop_label_scope[while_lbl_idx] = gen->scope_depth;
                gen->loop_label_id[while_lbl_idx] = gen->next_loop_label_id++;
                gen->loop_label_break_used[while_lbl_idx] = 0;
                gen->loop_label_continue_used[while_lbl_idx] = 0;
                gen->loop_td_id[while_lbl_idx] = td_id;
                gen->loop_td_node[while_lbl_idx] = td_id ? stmt : NULL;
                gen->loop_nest_depth++;
            }
            ASTNode* saved_td_switch = gen->td_switch;
            int saved_td_id = gen->td_id;
            int saved_in_head = g_td_in_head;
            if (td_id) {
                gen->td_switch = td_sw;
                gen->td_id = td_id;
                g_td_in_head = 1;   /* cleared when the dispatch switch starts */
            }
            if (stmt->child_count > 1) {
                generate_statement(gen, stmt->children[1]);
            }
            gen->td_switch = saved_td_switch;
            gen->td_id = saved_td_id;
            g_td_in_head = saved_in_head;
            /* The slot is reused by the next loop at this depth, whatever
             * kind it is; a stale id would thread that loop's continues. */
            if (while_lbl_idx >= 0) {
                gen->loop_td_id[while_lbl_idx] = 0;
                gen->loop_td_node[while_lbl_idx] = NULL;
            }
            if (gen->loop_nest_depth > 0) gen->loop_nest_depth--;
            /* #893: labeled-continue target — end of the loop body, after the
             * body's scope-exit defers, so the loop re-tests the condition. */
            if (while_lbl_idx >= 0 && gen->loop_label[while_lbl_idx] &&
                gen->loop_label_continue_used[while_lbl_idx]) {
                print_line(gen, "__ae_cont_%d: ;", gen->loop_label_id[while_lbl_idx]);
            }
            unindent(gen);

            print_line(gen, "}");

            /* #893: labeled-break target — after the loop. */
            if (while_lbl_idx >= 0 && gen->loop_label[while_lbl_idx] &&
                gen->loop_label_break_used[while_lbl_idx]) {
                print_line(gen, "__ae_brk_%d: ;", gen->loop_label_id[while_lbl_idx]);
            }
            /* #2378: where a threaded continue lands when the --emit=lib
             * deadline trips (the loop head's own check uses `break`). */
            if (td_id && gen->emit_lib) {
                print_line(gen, AE_TD_GUARD);
                print_line(gen, "_ae_td_exit_%d: ;", td_id);
                print_line(gen, "#endif");
            }

            if (has_sends && gen->current_actor == NULL) {
                print_line(gen, "scheduler_send_batch_flush();");
                gen->in_main_loop = 0;
            }
            break;
        }
            
        case AST_MATCH_STATEMENT:
            // Generate match as a series of if-else statements
            // match (x) { 1 -> a, 2 -> b, _ -> c }
            // becomes: { T _match_val = x; if (_match_val == 1) { a; } else if ... }
            // Using a temp variable avoids re-evaluating the match expression per arm.
            if (stmt->child_count > 0) {
                ASTNode* match_expr = stmt->children[0];

                // #340: optional `match` — `match m { none -> A  some(v) -> B }`
                // lowers to a presence test on the tagged struct. Handled
                // before the generic numeric / list / seq match lowering.
                if (match_expr->node_type && match_expr->node_type->kind == TYPE_OPTIONAL) {
                    Type* inner = match_expr->node_type->element_type;
                    const char* inner_c = inner ? get_c_type(inner) : "int";
                    const char* oc = get_c_type(match_expr->node_type);
                    static int om_counter = 0;
                    int id = om_counter++;
                    ASTNode *none_body = NULL, *some_body = NULL, *wild_body = NULL;
                    const char* some_bind = NULL;
                    for (int i = 1; i < stmt->child_count; i++) {
                        ASTNode* arm = stmt->children[i];
                        if (!arm || arm->type != AST_MATCH_ARM || arm->child_count < 2) continue;
                        ASTNode* pat = arm->children[0];
                        ASTNode* body = arm->children[1];
                        if (pat->type == AST_NONE_LITERAL) none_body = body;
                        else if (pat->type == AST_PATTERN_VARIABLE && pat->annotation &&
                                 strcmp(pat->annotation, "some_pattern") == 0) {
                            some_body = body; some_bind = pat->value;
                        } else if (pat->node_type && pat->node_type->kind == TYPE_WILDCARD) {
                            wild_body = body;
                        } else if (pat->type == AST_IDENTIFIER && pat->value &&
                                   strcmp(pat->value, "_") == 0) {
                            wild_body = body;
                        }
                    }
                    print_line(gen, "{");
                    indent(gen);
                    print_indent(gen);
                    fprintf(gen->output, "%s _om%d = ", oc, id);
                    generate_expression(gen, match_expr);
                    fprintf(gen->output, ";\n");
                    print_indent(gen);
                    fprintf(gen->output, "if (!_om%d.has) {\n", id);
                    indent(gen);
                    emit_opt_match_arm(gen, none_body ? none_body : wild_body);
                    unindent(gen);
                    print_line(gen, "} else {");
                    indent(gen);
                    if (some_bind) {
                        print_indent(gen);
                        fprintf(gen->output, "%s %s = _om%d.val;\n", inner_c, some_bind, id);
                    }
                    emit_opt_match_arm(gen, some_body ? some_body : wild_body);
                    unindent(gen);
                    print_line(gen, "}");
                    unindent(gen);
                    print_line(gen, "}");
                    break;
                }

                // #914 sum `match` — switch on the tag enum, narrowing the
                // matched value to each variant struct inside its case so
                // `s.field` reads the right union member. Handled before the
                // generic numeric / list / seq match lowering.
                if (match_expr->node_type && match_expr->node_type->kind == TYPE_SUM) {
                    Type* st = match_expr->node_type;
                    const char* sname = st->struct_name ? st->struct_name : "_sum";
                    static int sm_counter = 0;
                    int id = sm_counter++;
                    // The scrutinee is narrowed in each arm only when it is a
                    // plain variable (so `s.field` has a name to bind to).
                    const char* scrut = (match_expr->type == AST_IDENTIFIER)
                                        ? match_expr->value : NULL;
                    print_line(gen, "{");
                    indent(gen);
                    print_indent(gen);
                    fprintf(gen->output, "%s _sm%d = ", sname, id);
                    generate_expression(gen, match_expr);
                    fprintf(gen->output, ";\n");
                    print_indent(gen);
                    fprintf(gen->output, "switch (_sm%d.tag) {\n", id);
                    indent(gen);
                    for (int i = 1; i < stmt->child_count; i++) {
                        ASTNode* arm = stmt->children[i];
                        if (!arm || arm->type != AST_MATCH_ARM || arm->child_count < 2) continue;
                        ASTNode* pat = arm->children[0];
                        ASTNode* body = arm->children[1];
                        int is_wild =
                            (pat->node_type && pat->node_type->kind == TYPE_WILDCARD) ||
                            (pat->value && strcmp(pat->value, "_") == 0);
                        // A bare variant pattern (`Circle ->`) arrives as an
                        // AST_IDENTIFIER (parse_expression); accept that and
                        // the AST_PATTERN_VARIABLE form.
                        const char* variant = NULL;
                        if (!is_wild && pat->value &&
                            (pat->type == AST_IDENTIFIER ||
                             pat->type == AST_PATTERN_VARIABLE))
                            variant = pat->value;
                        print_indent(gen);
                        if (is_wild) {
                            fprintf(gen->output, "default: {\n");
                        } else if (variant) {
                            fprintf(gen->output, "case %s__%s: {\n", sname, variant);
                        } else {
                            continue;   // unrecognised arm — typechecker flagged it
                        }
                        indent(gen);
                        if (variant && scrut) {
                            print_indent(gen);
                            fprintf(gen->output, "%s %s = _sm%d.data.%s_; (void)%s;\n",
                                    variant, scrut, id, variant, scrut);
                        }
                        emit_opt_match_arm(gen, body);
                        print_line(gen, "break;");
                        unindent(gen);
                        print_line(gen, "}");
                    }
                    unindent(gen);
                    print_line(gen, "}");
                    unindent(gen);
                    print_line(gen, "}");
                    break;
                }

                // Check if any arm uses list patterns
                int uses_list_patterns = has_list_patterns(stmt);
                /* When the matched expression is *StringSeq, the
                 * "length variable" is actually a pointer to the
                 * cons cell — we walk it via NULL-checks and
                 * head/tail dereferences instead of array
                 * slicing. The flag below toggles between the
                 * two lowerings end-to-end. See
                 * std/collections/aether_stringseq.h for the
                 * cell layout. */
                int is_seq_match = uses_list_patterns &&
                    is_string_seq_ptr_type(match_expr->node_type);
                char len_name[64] = "_match_len";
                ValueTemps subject_temps;
                int has_subject_temps = 0;

                // Wrap match in a block and store the match expression in a temp
                // to avoid evaluating it multiple times (could have side effects).
                print_line(gen, "{");
                indent(gen);

                // If using list patterns, generate length variable for conditions
                if (is_seq_match) {
                    snprintf(len_name, sizeof(len_name), "_match_seq");
                    print_indent(gen);
                    fprintf(gen->output, "StringSeq* %s = ", len_name);
                    generate_expression(gen, match_expr);
                    fprintf(gen->output, ";\n");
                } else if (uses_list_patterns && type_is_slice(match_expr->node_type)) {
                    /* #1286: a slice carries its own length. */
                    print_indent(gen);
                    fprintf(gen->output, "int %s = (int)aether_slice_len(", len_name);
                    generate_expression(gen, match_expr);
                    fprintf(gen->output, ");\n");
                } else if (uses_list_patterns) {
                    print_indent(gen);
                    fprintf(gen->output, "int %s = ", len_name);
                    generate_expression(gen, match_expr);
                    fprintf(gen->output, "_len;\n");
                } else {
                    // Emit temp variable for the match expression value
                    Type* mexpr_type = match_expr->node_type;
                    const char* match_c_type = "int";
                    if (mexpr_type) {
                        if (mexpr_type->kind == TYPE_STRING || mexpr_type->kind == TYPE_PTR)
                            match_c_type = "const char*";
                        else if (mexpr_type->kind == TYPE_FLOAT)
                            match_c_type = "double";
                        else if (mexpr_type->kind == TYPE_FLOAT32)
                            match_c_type = "float";
                        else if (mexpr_type->kind == TYPE_LONGDOUBLE)
                            match_c_type = "long double";
                        else if (mexpr_type->kind == TYPE_INT64)
                            match_c_type = "int64_t";
                        else if (mexpr_type->kind == TYPE_BOOL)
                            match_c_type = "bool";
                    }
                    /* #2582: a struct a call in the subject returns lives
                     * until the match is done, since the subject (a string
                     * field of it, say) is read by every arm. */
                    has_subject_temps = value_temps_open_expr(gen, match_expr, TEMP_KEPT,
                                                              &subject_temps);
                    print_indent(gen);
                    fprintf(gen->output, "%s _match_val = ", match_c_type);
                    generate_expression(gen, match_expr);
                    fprintf(gen->output, ";\n");
                    if (has_subject_temps) temps_unmap(&subject_temps);
                }

                for (int i = 1; i < stmt->child_count; i++) {
                    ASTNode* match_arm = stmt->children[i];
                    if (!match_arm || match_arm->type != AST_MATCH_ARM || match_arm->child_count < 2) continue;

                    ASTNode* pattern = match_arm->children[0];
                    ASTNode* result = match_arm->children[1];

                    // Check if wildcard pattern
                    int is_wildcard = (pattern->type == AST_LITERAL &&
                                      pattern->value &&
                                      strcmp(pattern->value, "_") == 0) ||
                                     (pattern->node_type &&
                                      pattern->node_type->kind == TYPE_WILDCARD);

                    // Check if list pattern
                    int is_list_pattern = (pattern->type == AST_PATTERN_LIST ||
                                          pattern->type == AST_PATTERN_CONS);

                    if (is_wildcard) {
                        // else clause
                        if (i > 1) {
                            print_indent(gen);
                            fprintf(gen->output, "else {\n");
                        } else {
                            print_indent(gen);
                            fprintf(gen->output, "{\n");
                        }
                    } else if (is_list_pattern) {
                        // List pattern clause
                        if (i > 1) {
                            print_indent(gen);
                            fprintf(gen->output, "else if (");
                        } else {
                            print_indent(gen);
                            fprintf(gen->output, "if (");
                        }
                        generate_list_pattern_condition(gen, pattern, len_name, is_seq_match);
                        fprintf(gen->output, ") {\n");
                    } else {
                        // Regular literal/expression pattern
                        if (i > 1) {
                            print_indent(gen);
                            fprintf(gen->output, "else if (");
                        } else {
                            print_indent(gen);
                            fprintf(gen->output, "if (");
                        }
                        // Use _match_val (temp) instead of re-evaluating
                        // match_expr. emit_selector_condition (#1047) handles a
                        // single value, an inclusive/half-open range, or a
                        // comma-list. For strings it uses string_equals (NULL
                        // -safe, magic-aware: _match_val may be a magic
                        // AetherString, so a raw strcmp would compare the struct
                        // header; the NULL guard makes a NULL scrutinee match no
                        // pattern).
                        Type* mexpr_type = match_expr->node_type;
                        int mexpr_is_string = mexpr_type && mexpr_type->kind == TYPE_STRING;
                        emit_selector_condition(gen, pattern, "_match_val", mexpr_is_string);
                        fprintf(gen->output, ") {\n");
                    }

                    indent(gen);

                    // Generate list pattern bindings if needed
                    if (is_list_pattern) {
                        generate_list_pattern_bindings(gen, pattern, match_expr, len_name, result, is_seq_match);
                    }

                    if (result->type == AST_BLOCK) {
                        // Already a block, generate its statements
                        emit_match_arm_block(gen, result);
                    } else if (result->type == AST_PRINT_STATEMENT
                            || result->type == AST_RETURN_STATEMENT
                            || result->type == AST_VARIABLE_DECLARATION) {
                        // Statement-level node (e.g. print, return)
                        generate_statement(gen, result);
                    } else {
                        // Single expression — assign to result var or emit as statement
                        emit_match_result_value(gen, result);
                    }
                    unindent(gen);
                    print_line(gen, "}");
                }

                if (has_subject_temps) temps_close(gen, &subject_temps, 1);
                // Close the match scoping block
                unindent(gen);
                print_line(gen, "}");
            }
            break;

        case AST_SWITCH_STATEMENT: {
            // #1047: a C `switch` can't express a ranged case (`case 1..=5:`).
            // Aether's switch has no fall-through (each case auto-breaks), so it
            // is semantically identical to an if-else chain. When any case is
            // ranged, lower the whole switch to an if-chain (comparing a temp
            // scrutinee via emit_selector_condition); plain / comma-list-only
            // switches keep the C `switch` for readable output.
            int needs_chain = 0;
            for (int i = 1; i < stmt->child_count; i++) {
                ASTNode* c = stmt->children[i];
                if (c && c->type == AST_CASE_STATEMENT && c->child_count > 0 &&
                    !(c->value && strcmp(c->value, "default") == 0) &&
                    selector_has_range(c->children[0])) { needs_chain = 1; break; }
            }

            if (!needs_chain && stmt == gen->td_switch && gen->td_id && stmt->child_count > 0) {
                /* #2378: the dispatch switch of a threaded loop. The selector
                 * goes through _ae_td_sel_N so a continue whose value has no
                 * table entry can jump back to the switch (_ae_td_sw_N)
                 * without re-running the head; each arm gets a label (see
                 * AST_CASE_STATEMENT). Without labels-as-values this is the
                 * plain switch of today. */
                int id = gen->td_id;
                gen->td_switch = NULL;   /* a nested switch is not the dispatch */
                g_td_in_head = 0;        /* the head is done; arms dispatch */
                fprintf(gen->output, "\n");
                print_line(gen, AE_TD_GUARD);
                print_indent(gen);
                fprintf(gen->output, "_ae_td_sel_%d = (int64_t)(", id);
                generate_expression(gen, stmt->children[0]);
                fprintf(gen->output, ");\n");
                print_line(gen, "_ae_td_sw_%d: ;", id);
                print_line(gen, "switch (_ae_td_sel_%d) {", id);
                print_line(gen, "#else");
                print_indent(gen);
                fprintf(gen->output, "switch (");
                generate_expression(gen, stmt->children[0]);
                fprintf(gen->output, ") {\n");
                print_line(gen, "#endif");
                indent(gen);
                for (int i = 1; i < stmt->child_count; i++) {
                    gen->td_arm = i;
                    generate_statement(gen, stmt->children[i]);
                }
                gen->td_arm = -1;
                unindent(gen);
                print_line(gen, "}");
                break;
            }

            if (!needs_chain) {
                int saved_td_arm = gen->td_arm;
                gen->td_arm = -1;
                fprintf(gen->output, "switch (");
                if (stmt->child_count > 0) generate_expression(gen, stmt->children[0]);
                fprintf(gen->output, ") {\n");
                indent(gen);
                for (int i = 1; i < stmt->child_count; i++)
                    generate_statement(gen, stmt->children[i]);
                unindent(gen);
                print_line(gen, "}");
                gen->td_arm = saved_td_arm;
                break;
            }

            // Ranged switch -> if-else chain.
            ASTNode* sexpr = stmt->child_count > 0 ? stmt->children[0] : NULL;
            int is_str = sexpr && sexpr->node_type && sexpr->node_type->kind == TYPE_STRING;
            print_line(gen, "{");
            indent(gen);
            print_indent(gen);
            fprintf(gen->output, "%s _switch_val = ", scrutinee_c_type(sexpr));
            if (sexpr) generate_expression(gen, sexpr);
            else fprintf(gen->output, "0");
            fprintf(gen->output, ";\n");

            ASTNode* default_case = NULL;
            int emitted = 0;
            for (int i = 1; i < stmt->child_count; i++) {
                ASTNode* c = stmt->children[i];
                if (!c || c->type != AST_CASE_STATEMENT) continue;
                int is_default = (c->value && strcmp(c->value, "default") == 0);
                if (is_default) { default_case = c; continue; }
                print_indent(gen);
                fprintf(gen->output, emitted ? "else if (" : "if (");
                emit_selector_condition(gen, c->children[0], "_switch_val", is_str);
                fprintf(gen->output, ") {\n");
                indent(gen);
                for (int j = 1; j < c->child_count; j++) generate_statement(gen, c->children[j]);
                unindent(gen);
                print_line(gen, "}");
                emitted = 1;
            }
            if (default_case) {
                print_indent(gen);
                fprintf(gen->output, emitted ? "else {\n" : "{\n");
                indent(gen);
                for (int j = 0; j < default_case->child_count; j++)
                    generate_statement(gen, default_case->children[j]);
                unindent(gen);
                print_line(gen, "}");
            }
            unindent(gen);
            print_line(gen, "}");
            break;
        }
            
        case AST_CASE_STATEMENT: {
            int is_default = (stmt->value && strcmp(stmt->value, "default") == 0);
            if (is_default) {
                print_line(gen, "default:");
            } else {
                // #1047: a comma-list case (`case 7, 8, 9:`) lowers to one C
                // `case` label per value, sharing the body (no fall-through in
                // between, Aether auto-breaks after the shared body). A ranged
                // case never reaches here (AST_SWITCH_STATEMENT diverts a switch
                // with any range to an if-chain).
                ASTNode* sel = stmt->child_count > 0 ? stmt->children[0] : NULL;
                if (sel && sel->type == AST_MATCH_ALT) {
                    for (int a = 0; a < sel->child_count; a++) {
                        fprintf(gen->output, "case ");
                        generate_expression(gen, sel->children[a]);
                        fprintf(gen->output, ":\n");
                    }
                } else {
                    fprintf(gen->output, "case ");
                    if (sel) generate_expression(gen, sel);
                    fprintf(gen->output, ":\n");
                }
            }

            /* #2378: an arm of a threaded loop's dispatch switch gets the
             * label its table entries point at. Consumed here, so a switch
             * nested in the arm's body is never labelled. */
            if (!is_default && gen->td_arm >= 0 && gen->td_id) {
                print_line(gen, AE_TD_GUARD);
                print_line(gen, "_ae_td_%d_arm%d: ;", gen->td_id, gen->td_arm);
                print_line(gen, "#endif");
            }
            gen->td_arm = -1;

            indent(gen);
            // Generate all statements in the case block (skip first child which is the case value)
            int start_idx = is_default ? 0 : 1;
            for (int i = start_idx; i < stmt->child_count; i++) {
                generate_statement(gen, stmt->children[i]);
            }
            // Auto-insert `break` unless the case ends with its own
            // control-flow exit. Aether's `switch` has no
            // fallthrough — that's the surprising-default-everyone-
            // gets-wrong-once C inherited; we don't carry it forward.
            // (Empty `case 7: case 8: ...` chaining isn't supported
            // via this path — each case currently must have a body.
            // If we add empty-fallthrough later it'll be an explicit
            // syntax, not the silent default.)
            int needs_break = 1;
            int total = stmt->child_count;
            int last = total - 1;
            if (last >= start_idx) {
                ASTNode* tail_stmt = stmt->children[last];
                if (tail_stmt) {
                    ASTNodeType t = tail_stmt->type;
                    if (t == AST_RETURN_STATEMENT ||
                        t == AST_BREAK_STATEMENT ||
                        t == AST_CONTINUE_STATEMENT) {
                        needs_break = 0;
                    }
                }
            }
            if (needs_break) {
                print_line(gen, "break;");
            }
            unindent(gen);
            break;
        }
            
        case AST_RETURN_STATEMENT: {
            // #1054: `return match x { ... }`, a match in return position, is a
            // value-producing expression, not a statement. Lower it via the same
            // result-variable mechanism the working `v = match x { ... }` form
            // uses: declare a temp of the return type, run the match with each
            // arm assigning the temp, then re-dispatch as `return <temp>` so all
            // the return machinery (contracts, defers, escape drains) applies
            // uniformly. Without this, a bare match in return position emitted
            // `return;` (void) followed by an orphaned match whose arm bodies
            // were dead expression-statements, so the function returned garbage.
            if (stmt->child_count == 1 && stmt->children[0] &&
                stmt->children[0]->type == AST_MATCH_STATEMENT) {
                ASTNode* m = stmt->children[0];
                // Return type: the function's declared type, else infer from the
                // first arm's result (mirrors the `v = match` path above).
                Type* rt = gen->current_func_return_type;
                const char* ct = (rt && rt->kind != TYPE_VOID && rt->kind != TYPE_UNKNOWN)
                                 ? get_c_type(rt) : NULL;
                if (!ct && match_value_type(m)) ct = get_c_type(match_value_type(m));
                if (!ct) ct = "int";
                static int ret_match_ctr = 0;
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "_ret_match%d", ret_match_ctr++);
                print_indent(gen);
                fprintf(gen->output, "%s %s;\n", ct, tmp);
                /* #2461: a heap-returning function hands its caller an owned
                 * string, so the arms take their values (a local of this
                 * function moved out, a field read copied) and the temp's
                 * tracker tells the uniform-heap return shim below which
                 * the caller already owns. Returning the bare arm value
                 * handed back a pointer this function's scope exit freed. */
                char own[48] = "";
                if (strcmp(ct, "const char*") == 0 && gen->current_function &&
                    function_def_returns_heap_string(gen, gen->current_function)) {
                    snprintf(own, sizeof(own), "_heap_%s", tmp);
                    print_line(gen, "int %s = 0; (void)%s;", own, own);
                    mark_heap_string_var(gen, tmp);
                }
                const char* saved = gen->match_result_var;
                const char* saved_own = gen->match_result_own;
                const char* saved_struct = gen->match_result_struct;
                int saved_replace = gen->match_result_replace;
                Type* saved_cell = gen->match_result_cell;
                gen->match_result_cell = NULL;
                gen->match_result_var = tmp;
                gen->match_result_own = own[0] ? own : NULL;
                /* #2497: the caller adopts a returned struct, so each arm hands
                 * over one this function no longer owns. */
                gen->match_result_struct = struct_owning_strings(gen, rt);
                gen->match_result_replace = 0;
                generate_statement(gen, m);
                gen->match_result_var = saved;
                gen->match_result_own = saved_own;
                gen->match_result_struct = saved_struct;
                gen->match_result_replace = saved_replace;
                gen->match_result_cell = saved_cell;
                // Re-dispatch as `return <tmp>` to reuse all return machinery.
                ASTNode* rid = create_ast_node(AST_IDENTIFIER, tmp, stmt->line, stmt->column);
                rid->node_type = rt ? clone_type(rt)
                                    : (m->node_type ? clone_type(m->node_type) : NULL);
                ASTNode* rstmt = create_ast_node(AST_RETURN_STATEMENT, NULL,
                                                 stmt->line, stmt->column);
                add_child(rstmt, rid);
                generate_statement(gen, rstmt);
                free_ast_node(rstmt);
                break;
            }
            // Issue #348 — postcondition checks. When the enclosing
            // function has any `ensures` clauses AND we're emitting
            // a single-value, non-main return AND --no-contracts is
            // off, route through a fresh C scope: assign the return
            // expression to a local `result`, run the checks, then
            // `return result`. Each return site gets its own copy of
            // every check; the C scope hides any outer `result`.
            //
            // Skip when the function is `main` (the existing
            // main_exit goto chain is fine — main has no callers
            // expecting postconditions) or when the return is
            // multi-value (tuple semantics for `result` aren't yet
            // defined; multi-value contracts are an out-of-scope
            // follow-up).
            if (!gen->in_main_function &&
                stmt->child_count == 1 &&
                gen->current_function &&
                function_has_ensures(gen->current_function) &&
                !gen->no_contracts) {
                Type* ret_type = stmt->children[0]->node_type;
                const char* ret_c_type =
                    (ret_type && ret_type->kind != TYPE_VOID && ret_type->kind != TYPE_UNKNOWN)
                    ? get_c_type(ret_type)
                    : (gen->current_func_return_type &&
                       gen->current_func_return_type->kind != TYPE_VOID &&
                       gen->current_func_return_type->kind != TYPE_UNKNOWN)
                        ? get_c_type(gen->current_func_return_type)
                        : "int";
                print_indent(gen);
                fprintf(gen->output, "{\n");
                gen->indent_level++;
                print_indent(gen);
                fprintf(gen->output, "%s result = ", ret_c_type);
                emit_return_value(gen, stmt);   // #340: coerces into `T?`
                fprintf(gen->output, ";\n");
                /* Drain return-escape heap-string vars that aren't
                 * the one being returned. See
                 * emit_return_escape_drains_for_unreturned. */
                emit_return_escape_drains_for_unreturned(gen, stmt->children[0]);
                mark_returned_struct_escaped(gen, stmt->children[0]);
                disown_returned_promoted_struct(gen, stmt->children[0]);
                emit_contract_postconditions(gen, gen->current_function);
                /* Drain function-level defers BEFORE returning so
                 * cleanup happens between the postcondition check
                 * and the return — same ordering as the regular
                 * defer-aware return path further down. */
                if (gen->defer_count > 0) {
                    emit_all_defers(gen);
                }
                /* Issue #501: drain in-flight try frames before
                 * returning, so a `return` inside a try body doesn't
                 * leak the panic frame. */
                emit_try_pops_for_nonlocal_exit(gen);
                print_indent(gen);
                fprintf(gen->output, "return result;\n");
                gen->indent_level--;
                print_indent(gen);
                fprintf(gen->output, "}\n");
                break;
            }
            // In main(), all returns go through main_exit so scheduler_wait() always runs
            if (gen->in_main_function) {
                if (gen->defer_count > 0) {
                    emit_all_defers(gen);
                }
                /* Issue #501: drain in-flight try frames before
                 * the goto so a `return` inside a try body in
                 * main() doesn't leak the panic frame.  goto and
                 * return are both non-local exits as far as the
                 * try-frame stack is concerned. */
                emit_try_pops_for_nonlocal_exit(gen);
                print_indent(gen);
                if (stmt->child_count > 0 && stmt->children[0] &&
                    stmt->children[0]->type != AST_PRINT_STATEMENT) {
                    fprintf(gen->output, "main_exit_ret = ");
                    generate_expression(gen, stmt->children[0]);
                    fprintf(gen->output, "; goto main_exit;\n");
                } else {
                    if (stmt->child_count > 0 && stmt->children[0] &&
                        stmt->children[0]->type == AST_PRINT_STATEMENT) {
                        generate_statement(gen, stmt->children[0]);
                        print_indent(gen);
                    }
                    print_line(gen, "goto main_exit;");
                }
                break;
            }
            // Emit ALL defers before return (unwind entire function)
            if (gen->defer_count > 0) {
                // Multi-value return + defer: build a _builder_ret typed
                // as the function's tuple return so the existing defer-
                // unwind machinery still applies. Without this branch,
                // we'd save children[0]'s type alone and the C compiler
                // would reject `return _builder_ret;` against the tuple-
                // typed function. Issue #254. Mirrors the no-defer
                // multi-value path below at the "return (_tuple_X_Y){...}"
                // line — same tuple-literal shape, just stuffed into
                // _builder_ret first.
                if (stmt->child_count > 1) {
                    /* Each defer-unwinding return declares its own
                     * `_builder_ret`, so it gets its own C scope: two
                     * returns in one Aether block (`return a` then an
                     * unreachable `return b`, as a converted build file
                     * had) otherwise redeclare it in the same C scope
                     * and clang/gcc reject the redefinition. */
                    print_line(gen, "{");
                    gen->indent_level++;
                    print_indent(gen);
                    Type* tuple = NULL;
                    int owned = 0;
                    if (gen->current_func_return_type &&
                        gen->current_func_return_type->kind == TYPE_TUPLE) {
                        tuple = gen->current_func_return_type;
                    } else {
                        tuple = create_type(TYPE_TUPLE);
                        tuple->tuple_count = stmt->child_count;
                        tuple->tuple_types = malloc(stmt->child_count * sizeof(Type*));
                        for (int j = 0; j < stmt->child_count; j++) {
                            tuple->tuple_types[j] = stmt->children[j]->node_type
                                ? clone_type(stmt->children[j]->node_type)
                                : create_type(TYPE_INT);
                        }
                        owned = 1;
                    }
                    ensure_tuple_typedef(gen, tuple);
                    const char* tname = get_c_type(tuple);
                    /* #2478: the positions are evaluated left to right. */
                    order_prelude_begin(gen, stmt->children, stmt->child_count, NULL);
                    fprintf(gen->output, "%s _builder_ret = (%s){", tname, tname);
                    for (int j = 0; j < stmt->child_count; j++) {
                        if (j > 0) fprintf(gen->output, ", ");
                        emit_tuple_return_position(gen, stmt->children[j], j);
                    }
                    fprintf(gen->output, "};\n");
                    if (owned) free_type(tuple);
                    emit_tuple_return_escape_drains(gen, stmt);
                    /* #752: any struct element of the returned tuple
                     * escapes — suppress its exit-time `<Struct>_destroy`
                     * so its heap-string fields aren't freed under the
                     * caller (the caller that receives the unpacked struct
                     * gets its own destroy defer — see #762's two-sided
                     * return-escape contract). This supersedes the earlier
                     * #759 flag-zeroing approach, which only neutered the
                     * callee's destroy and left the caller-side leak. */
                    for (int j = 0; j < stmt->child_count; j++) {
                        mark_returned_struct_escaped(gen, stmt->children[j]);
                        disown_returned_promoted_struct(gen, stmt->children[j]);
                    }
                    // Multi-value returns can't be returning a closure
                    // (closures aren't tuples), so the closure-of-captures
                    // protection logic the single-value path runs is
                    // unnecessary here — drain the defers and emit the
                    // return.
                    /* #1140: this is a `(value, err)` return, so `defer try` /
                     * `defer catch` fire here — conditionally on the error slot.
                     * `_builder_ret` is already constructed above, so the guard
                     * can test it directly, using the same "non-NULL and
                     * non-empty" convention the rest of the compiler uses for
                     * "this result carries an error". A literal `""` in the
                     * error position would let us decide statically, but the
                     * runtime test costs a predictable compare on the return
                     * path and keeps this to one code path. */
                    {
                        char errslot[32];
                        snprintf(errslot, sizeof(errslot), "_builder_ret._%d",
                                 stmt->child_count - 1);
                        DeferExit prev_exit = gen->defer_exit;
                        const char* prev_slot = gen->defer_err_slot;
                        gen->defer_exit = DEFER_EXIT_RUNTIME;
                        gen->defer_err_slot = errslot;
                        emit_all_defers(gen);
                        gen->defer_exit = prev_exit;
                        gen->defer_err_slot = prev_slot;
                    }
                    /* Issue #501: drain try frames. */
                    emit_try_pops_for_nonlocal_exit(gen);
                    print_line(gen, "return _builder_ret;");
                    gen->indent_level--;
                    print_line(gen, "}");
                    break;
                }
                // For return with value, save to temp first
                if (stmt->child_count > 0 && stmt->children[0] &&
                    stmt->children[0]->type != AST_PRINT_STATEMENT) {
                    /* Own C scope per return, as in the multi-value
                     * branch above: a second return in the same block
                     * would otherwise redeclare `_builder_ret`. */
                    print_line(gen, "{");
                    gen->indent_level++;
                    print_indent(gen);
                    /* Pick the C type for `_builder_ret`. Order:
                     *   1. The function's declared return type
                     *      (`gen->current_func_return_type`) — this
                     *      is what the C compiler will check the
                     *      `return _builder_ret;` against, so using
                     *      anything else risks a type-mismatch
                     *      compile error.
                     *   2. The expression's stamped node_type — used
                     *      when the function doesn't declare a
                     *      return type (Aether's type inference
                     *      stamps the expression).
                     *   3. `int` — last-resort fallback so we always
                     *      emit something compilable.
                     * Without (1), bare-identifier returns whose
                     * node_type isn't filled in (a common gap) fell
                     * through to `int`, which the C compiler then
                     * rejected against a `const char*`-returning
                     * function. Surfaced when the function-exit
                     * defer-free pre-pass started pushing defers
                     * for previously-leaking heap-string locals,
                     * which forced this branch to take effect for
                     * functions that previously skipped it. */
                    Type* fn_ret = gen->current_func_return_type;
                    Type* ret_type = (fn_ret &&
                                      fn_ret->kind != TYPE_VOID &&
                                      fn_ret->kind != TYPE_UNKNOWN)
                                     ? fn_ret
                                     : stmt->children[0]->node_type;
                    const char* ret_c_type = (ret_type && ret_type->kind != TYPE_VOID && ret_type->kind != TYPE_UNKNOWN)
                                             ? get_c_type(ret_type) : "int";
                    fprintf(gen->output, "%s _builder_ret = ", ret_c_type);
                    /* Uniform-heap return-escape contract: heap-returning
                     * functions route every return through
                     * aether_uniform_heap_str so the caller can free the
                     * result regardless of which branch produced it.
                     * Heap inputs pass through (fast path); literal
                     * inputs are malloc-duplicated. See codegen.c
                     * prologue and emit_uniform_heap_return_expr. */
                    emit_return_value(gen, stmt);   // #340: coerces into `T?`
                    fprintf(gen->output, ";\n");
                    /* Drain unreturned return-escape vars (the
                     * after-loop-return case where one return path's
                     * tracked local isn't this path's return value).
                     * See emit_return_escape_drains_for_unreturned. */
                    emit_return_escape_drains_for_unreturned(gen, stmt->children[0]);
                    /* Every env-free here is sound to run: a closure local
                     * the value returns has none (claim_closure_local_env
                     * counts a return as an escape), and a closure that the
                     * returned one captured is kept by the reference the
                     * capture took (#2494). Holding those frees back, as
                     * this return used to, only leaked the captured env. */
                    /* #752: a directly-returned struct escapes — suppress
                     * its exit-time destroy (heap-string fields now owned
                     * by the caller). */
                    mark_returned_struct_escaped(gen, stmt->children[0]);
                    disown_returned_promoted_struct(gen, stmt->children[0]);
                    /* #1140: a single-value `return v` is a SUCCESS exit — in a
                     * `T!` function it is wrapped as `{._0 = v, ._1 = ""}`, and
                     * in an ordinary function there is no error channel at all.
                     * Either way `defer try` fires and `defer catch` does not,
                     * and it is known statically, so no runtime guard. */
                    {
                        DeferExit prev_exit = gen->defer_exit;
                        gen->defer_exit = DEFER_EXIT_SUCCESS;
                        emit_all_defers(gen);
                        gen->defer_exit = prev_exit;
                    }
                    /* Issue #501: drain try frames. */
                    emit_try_pops_for_nonlocal_exit(gen);
                    print_line(gen, "return _builder_ret;");
                    gen->indent_level--;
                    print_line(gen, "}");
                } else if (stmt->child_count > 0 && stmt->children[0] &&
                           stmt->children[0]->type == AST_PRINT_STATEMENT) {
                    emit_all_defers(gen);
                    generate_statement(gen, stmt->children[0]);
                    /* Issue #501: drain try frames. */
                    emit_try_pops_for_nonlocal_exit(gen);
                    print_line(gen, "return;");
                } else {
                    emit_all_defers(gen);
                    /* Issue #501: drain try frames. */
                    emit_try_pops_for_nonlocal_exit(gen);
                    print_line(gen, "return;");
                }
            } else {
                // No defers - original behavior
                if (stmt->child_count > 0 && stmt->children[0] &&
                    stmt->children[0]->type == AST_PRINT_STATEMENT) {
                    generate_statement(gen, stmt->children[0]);
                    /* Issue #501: drain try frames. */
                    emit_try_pops_for_nonlocal_exit(gen);
                    print_line(gen, "return;");
                } else if (stmt->child_count > 1) {
                    // Multi-value return: return a, b → return (_tuple_X_Y){a, b}
                    /* Issue #501: drain try frames first; the
                     * print_indent below is for the `return ...`
                     * line, which lands after the drained pops. */
                    emit_try_pops_for_nonlocal_exit(gen);
                    print_indent(gen);
                    // Use the function's known return type if it's a tuple
                    // (avoids UNKNOWN types from unresolved identifiers)
                    Type* tuple = NULL;
                    int owned = 0;
                    if (gen->current_func_return_type &&
                        gen->current_func_return_type->kind == TYPE_TUPLE) {
                        tuple = gen->current_func_return_type;
                    } else {
                        // Fallback: build from expression types
                        tuple = create_type(TYPE_TUPLE);
                        tuple->tuple_count = stmt->child_count;
                        tuple->tuple_types = malloc(stmt->child_count * sizeof(Type*));
                        for (int j = 0; j < stmt->child_count; j++) {
                            tuple->tuple_types[j] = stmt->children[j]->node_type
                                ? clone_type(stmt->children[j]->node_type)
                                : create_type(TYPE_INT);
                        }
                        owned = 1;
                    }
                    ensure_tuple_typedef(gen, tuple);
                    const char* tname = get_c_type(tuple);
                    /* Unreturned return-escape vars are drained after the
                     * value is built (see emit_tuple_return_escape_drains),
                     * so that shape goes through a local; the common shape
                     * keeps the bare `return (T){...};`. */
                    int drains = count_tuple_return_drains(gen, stmt);
                    /* #2478: the positions are evaluated left to right. */
                    order_prelude_begin(gen, stmt->children, stmt->child_count, NULL);
                    if (drains > 0) fprintf(gen->output, "{ %s _no_defer_ret = (%s){", tname, tname);
                    else fprintf(gen->output, "return (%s){", tname);
                    for (int j = 0; j < stmt->child_count; j++) {
                        if (j > 0) fprintf(gen->output, ", ");
                        emit_tuple_return_position(gen, stmt->children[j], j);
                    }
                    fprintf(gen->output, "};\n");
                    if (drains > 0) {
                        gen->indent_level++;
                        emit_tuple_return_escape_drains(gen, stmt);
                        print_line(gen, "return _no_defer_ret;");
                        gen->indent_level--;
                        print_line(gen, "}");
                    }
                    if (owned) free_type(tuple);
                } else {
                    /* No-defer single-value path. To drain unreturned
                     * return-escape vars BEFORE the return executes
                     * (the drain must observe `_heap_<name>` before
                     * the C `return` statement consumes the function
                     * frame), we route through a `_no_defer_ret` C
                     * local on the heap-returning + drain-needed
                     * shape only. Functions without return-escape
                     * vars keep the bare `return <expr>;` shape so
                     * the common path is unchanged. */
                    int route_through_local =
                        gen->return_escaped_string_var_count > 0 &&
                        should_uniform_heap_return(gen, stmt);
                    if (route_through_local) {
                        Type* fn_ret = gen->current_func_return_type;
                        const char* ret_c_type = (fn_ret &&
                                                  fn_ret->kind != TYPE_VOID &&
                                                  fn_ret->kind != TYPE_UNKNOWN)
                                                 ? get_c_type(fn_ret) : "const char*";
                        print_indent(gen);
                        fprintf(gen->output, "%s _no_defer_ret = ", ret_c_type);
                        emit_uniform_heap_return_expr(gen, stmt->children[0]);
                        fprintf(gen->output, ";\n");
                        emit_return_escape_drains_for_unreturned(gen, stmt->children[0]);
                        /* Issue #501: drain try frames. */
                        emit_try_pops_for_nonlocal_exit(gen);
                        print_line(gen, "return _no_defer_ret;");
                    } else {
                        /* Issue #501: drain try frames before the
                         * print_indent for the `return <expr>;` line. */
                        emit_try_pops_for_nonlocal_exit(gen);
                        print_indent(gen);
                        fprintf(gen->output, "return");
                        if (stmt->child_count > 0) {
                            fprintf(gen->output, " ");
                            /* Uniform-heap return-escape contract on
                             * the no-defer single-value path. Mirrors
                             * the defer-aware path's wrap so the
                             * contract holds even for functions with
                             * no defers (the avn-bench shape with a
                             * single escape-marked heap-string local).
                             * #340: emit_return_value also coerces a
                             * bare value / `none` into a `T?` return. */
                            emit_return_value(gen, stmt);
                        }
                        fprintf(gen->output, ";\n");
                    }
                }
            }
            break;
        }

        case AST_BREAK_STATEMENT: {
            int lvl = stmt->value ? find_labeled_loop_level(gen, stmt->value) : -1;
            if (stmt->value && lvl >= 0) {
                /* #893: labeled break — unwind every scope nested inside the
                 * target loop (run their defers), drain the try frames pushed
                 * since that loop was entered, then jump past the loop. */
                emit_defers_through_scope(gen, gen->loop_label_scope[lvl]);
                int drop = gen->try_frame_depth - gen->loop_try_base[lvl];
                for (int i = 0; i < drop; i++) {
                    print_indent(gen);
                    fprintf(gen->output, "aether_try_pop();\n");
                }
                gen->loop_label_break_used[lvl] = 1;
                print_line(gen, "goto __ae_brk_%d;", gen->loop_label_id[lvl]);
            } else {
                /* Unwind every scope nested inside the innermost loop,
                 * not just the current one: a `break` inside an `if` (or
                 * a trailing block) inside the loop body leaves those
                 * scopes too, and their defers -- a trailing block's
                 * builder-context pop among them -- must run. Same rule
                 * as the labeled form above. */
                if (gen->loop_nest_depth > 0)
                    emit_defers_through_scope(gen, gen->loop_label_scope[gen->loop_nest_depth - 1]);
                else
                    emit_defers_for_scope(gen);
                /* Issue #501: drain try frames pushed inside the current
                 * loop body so `break` from inside a try { } in a loop
                 * doesn't leak the panic frame. */
                emit_try_pops_for_break_continue(gen);
                print_line(gen, "break;");
            }
            break;
        }

        case AST_CONTINUE_STATEMENT: {
            int lvl = stmt->value ? find_labeled_loop_level(gen, stmt->value) : -1;
            if (stmt->value && lvl >= 0) {
                /* #893: labeled continue — unwind nested scopes and inner try
                 * frames, then jump to the end of the target loop's body (where
                 * the loop re-tests / increments). */
                emit_defers_through_scope(gen, gen->loop_label_scope[lvl]);
                int drop = gen->try_frame_depth - gen->loop_try_base[lvl];
                for (int i = 0; i < drop; i++) {
                    print_indent(gen);
                    fprintf(gen->output, "aether_try_pop();\n");
                }
                gen->loop_label_continue_used[lvl] = 1;
                if (gen->loop_td_id[lvl] && !g_td_in_head) {
                    /* #2378: into a threaded loop — dispatch directly. */
                    char fallback[64];
                    snprintf(fallback, sizeof fallback, "goto __ae_cont_%d;", gen->loop_label_id[lvl]);
                    td_emit_dispatch(gen, lvl, fallback);
                } else {
                    print_line(gen, "goto __ae_cont_%d;", gen->loop_label_id[lvl]);
                }
            } else {
                /* Unwind every scope nested inside the innermost loop,
                 * not just the current one: a `continue` inside an `if` (or
                 * a trailing block) inside the loop body leaves those
                 * scopes too, and their defers -- a trailing block's
                 * builder-context pop among them -- must run. Same rule
                 * as the labeled form above. */
                if (gen->loop_nest_depth > 0)
                    emit_defers_through_scope(gen, gen->loop_label_scope[gen->loop_nest_depth - 1]);
                else
                    emit_defers_for_scope(gen);
                /* Issue #501: drain inside-loop try frames. */
                emit_try_pops_for_break_continue(gen);
                int td_lvl = td_continue_target(gen, stmt);
                if (td_lvl >= 0) {
                    /* #2378: run the head and jump to the next arm. */
                    td_emit_dispatch(gen, td_lvl, "continue;");
                } else {
                    print_line(gen, "continue;");
                }
            }
            break;
        }

        case AST_DEFER_STATEMENT:
            // Push deferred statement to stack - will be executed at scope exit.
            // #1140: `value` carries the qualifier ("try" / "catch"; NULL for a
            // plain `defer`), set by the parser. The stack holds the deferred
            // STATEMENT, so the mode travels alongside in gen->defer_mode[].
            if (stmt->child_count > 0) {
                DeferMode mode = DEFER_ALWAYS;
                if (stmt->value) {
                    if (strcmp(stmt->value, "try") == 0)        mode = DEFER_TRY;
                    else if (strcmp(stmt->value, "catch") == 0) mode = DEFER_CATCH;
                }
                push_defer_mode(gen, stmt->children[0], mode);
            }
            break;

        case AST_TRY_STATEMENT: {
            // try { body } catch name { handler }
            // Emit:
            //   { AetherJmpFrame* _af = aether_try_push();
            //     if (sigsetjmp(_af->buf, 1) == 0) {
            //         body
            //         aether_try_pop();
            //     } else {
            //         const char* NAME = _af->reason ? _af->reason : "panic";
            //         aether_try_pop();
            //         handler
            //     }
            //   }
            //
            // Each try site gets a uniquely-named frame variable so nested
            // try blocks don't shadow each other at the C level.
            //
            // Issue #501: a `return` (or any non-fall-through exit) from
            // inside the body skips the body's `aether_try_pop()` and
            // leaks a panic frame.  We bump gen->try_frame_depth while
            // generating the body so every AST_RETURN_STATEMENT codegen
            // inside emits a draining `aether_try_pop()` first.  The
            // catch handler runs with the depth back at caller's value
            // because the runtime pop is emitted before the handler
            // body — a return inside catch has no live frame to drain.
            if (stmt->child_count != 2) break;
            ASTNode* body = stmt->children[0];
            ASTNode* catch_clause = stmt->children[1];
            if (!body || !catch_clause || catch_clause->type != AST_CATCH_CLAUSE ||
                !catch_clause->value || catch_clause->child_count < 1) break;

            static int s_try_counter = 0;
            int uid = ++s_try_counter;

            print_line(gen, "{");
            indent(gen);
            /* A panic that unwinds out of an enforced block (or out of a
             * trusted call inside one) skips the code that pops the sandbox
             * or restores its depth. Catching it puts the depth back to what
             * it was here, so a catch inside the block stays sandboxed and
             * one outside it is no longer held to the block's grants. */
            if (gen->uses_sandbox) {
                print_line(gen, "int _aether_try_sbx_%d = _aether_sandbox_depth;", uid);
            }
            print_line(gen, "AetherJmpFrame* _aether_try_%d = aether_try_push();", uid);
            print_line(gen, "if (AETHER_SIGSETJMP(_aether_try_%d->buf, 1) == 0) {", uid);
            indent(gen);
            gen->try_frame_depth++;
            // Body runs inside the if; it already emits its own { } via AST_BLOCK.
            generate_statement(gen, body);
            gen->try_frame_depth--;
            print_line(gen, "aether_try_pop();");
            unindent(gen);
            print_line(gen, "} else {");
            indent(gen);
            if (gen->uses_sandbox) {
                print_line(gen, "_aether_sandbox_depth = _aether_try_sbx_%d;", uid);
            }
            print_line(gen, "const char* %s = _aether_try_%d->reason ? _aether_try_%d->reason : \"panic\";",
                      catch_clause->value, uid, uid);
            if (catch_binding_can_own(gen, catch_clause->value)) {
                /* #2333: a heap-built panic message (aether_panic_owned)
                 * belongs to the catcher. The binding becomes a heap-
                 * tracked string local for the handler's scope: its flag
                 * is the frame's ownership, so aliasing, reassignment,
                 * escape and scope exit follow the rules every other
                 * owned string local does. */
                emit_owned_catch_handler(gen, catch_clause, uid);
            } else {
                print_line(gen, "aether_try_pop();");
                generate_statement(gen, catch_clause->children[0]);
            }
            print_line(gen, "(void)%s;", catch_clause->value);
            unindent(gen);
            print_line(gen, "}");
            unindent(gen);
            print_line(gen, "}");
            break;
        }

        case AST_PANIC_STATEMENT: {
            // panic(reason_expr);  → capture backtrace at the call site,
            // then aether_panic(reason). Capturing into TLS before the
            // noreturn call is what gives the runtime stack-trace path
            // (issue #347) the user's caller frames — calling backtrace()
            // from inside aether_panic alone loses them under -O2 because
            // tail-call + noreturn collapses the caller's frame.
            if (stmt->child_count < 1) break;
            print_indent(gen);
            fprintf(gen->output, "aether_panic_capture_stack();\n");
            emit_panic_call(gen, stmt->children[0]);
            break;
        }
            
        case AST_EXPRESSION_STATEMENT:
            if (stmt->child_count > 0) {
                ASTNode* inner = stmt->children[0];

                /* Struct-field heap-string ownership (#465). Many
                 * parsers land `<var>.<field> = <rhs>` as
                 * AST_EXPRESSION_STATEMENT > AST_BINARY_EXPRESSION
                 * (op="=") rather than AST_ASSIGNMENT. Same wrapper
                 * applies — route through the helper before any
                 * other expression handling. */
                if (inner && inner->type == AST_BINARY_EXPRESSION &&
                    inner->value && strcmp(inner->value, "=") == 0 &&
                    inner->child_count == 2) {
                    /* #340: optional-chain assignment `recv?.field = rhs`. */
                    if (emit_optional_chain_assign(gen, inner->children[0],
                                                   inner->children[1])) {
                        break;
                    }
                    if (emit_struct_field_heap_assign(gen, inner->children[0],
                                                       inner->children[1])) {
                        break;
                    }
                    if (emit_cell_string_element_store(gen, inner->children[0],
                                                       inner->children[1])) {
                        break;   /* #2474 */
                    }
                    /* `obj.field = builder(args) { ... }`: the right side
                     * carries a trailing block. Emitted as a plain binary
                     * `=` the block was never visited — the whole body was
                     * silently dropped (aether-ui: every widget built inside
                     * `st.pane = vstack() { ... }` went missing). The
                     * AST_ASSIGNMENT case runs the block (builder or
                     * regular pattern), so route this shape through it. */
                    ASTNode* arhs = inner->children[1];
                    int rhs_trailing = 0;
                    if (arhs && arhs->type == AST_FUNCTION_CALL) {
                        for (int tc = 0; tc < arhs->child_count; tc++) {
                            if (arhs->children[tc] && arhs->children[tc]->type == AST_CLOSURE &&
                                arhs->children[tc]->value &&
                                strcmp(arhs->children[tc]->value, "trailing") == 0) {
                                rhs_trailing = 1;
                                break;
                            }
                        }
                    }
                    if (rhs_trailing) {
                        ASTNode as_assign = *inner;
                        as_assign.type = AST_ASSIGNMENT;
                        print_indent(gen);
                        /* The body, not the wrapper: the enclosing
                         * generate_statement runs the observable-store
                         * hook once for this statement (#2220). */
                        generate_statement_body(gen, &as_assign);
                        break;
                    }
                }

                // Check if this function call has a trailing block
                int has_trailing = 0;
                if (inner && inner->type == AST_FUNCTION_CALL) {
                    for (int tc = 0; tc < inner->child_count; tc++) {
                        if (inner->children[tc] && inner->children[tc]->type == AST_CLOSURE &&
                            inner->children[tc]->value &&
                            strcmp(inner->children[tc]->value, "trailing") == 0) {
                            has_trailing = 1;
                            break;
                        }
                    }
                }

                // Check if this is a builder function call with trailing block
                int is_builder_call = has_trailing && inner->value &&
                    is_builder_func_reg(gen, inner->value);

                if (has_trailing && is_builder_call) {
                    // BUILDER PATTERN: block configures first, then function executes
                    // Wrap in block scope so _bcfg doesn't collide with other builder calls
                    print_indent(gen);
                    fprintf(gen->output, "{\n");
                    gen->indent_level++;

                    // 1. Create config object and push as context
                    print_indent(gen);
                    /* Cast the factory's scalar return into void* to keep the ctx
                                             * slot universally void*-shaped. Without the cast, an
                                             * int-returning factory (e.g. aether-ui's
                                             * `_surface_window_factory() -> int`) emits
                                             * `void* _bcfg = int_fn();` — a bare int→pointer
                                             * conversion that GCC 14+/MinGW64 reject under
                                             * default -Werror=int-conversion. The (intptr_t)
                                             * intermediate is a no-op for ptr factories. See
                                             * builder-ctx-handle-void-ptr-int-conversion.md. */
                                            fprintf(gen->output, "void* _bcfg = (void*)(intptr_t)%s();\n",
                            get_builder_factory(gen, inner->value));
                    print_indent(gen);
                    fprintf(gen->output, "_aether_ctx_push(_bcfg);\n");

                    // 2. Run trailing block (fills config via builder functions)
                    for (int tc = 0; tc < inner->child_count; tc++) {
                        ASTNode* trailing = inner->children[tc];
                        if (trailing && trailing->type == AST_CLOSURE &&
                            trailing->value && strcmp(trailing->value, "trailing") == 0) {
                            for (int bi = 0; bi < trailing->child_count; bi++) {
                                if (trailing->children[bi] &&
                                    trailing->children[bi]->type == AST_BLOCK) {
                                    emit_trailing_block_body(gen, trailing->children[bi]);
                                    break;
                                }
                            }
                        }
                    }

                    // 3. Context popped by the block's own scope exit
                    //    (see emit_trailing_block_body).

                    // 4. Call function with config as extra last arg
                    print_indent(gen);
                    const char* c_builder_name = codegen_normalise_callee(
                        safe_c_name(inner->value));
                    fprintf(gen->output, "%s(", c_builder_name);
                    int arg_printed = 0;
                    for (int ai = 0; ai < inner->child_count; ai++) {
                        ASTNode* arg = inner->children[ai];
                        if (arg && arg->type == AST_CLOSURE &&
                            arg->value && strcmp(arg->value, "trailing") == 0) {
                            continue; // skip trailing block
                        }
                        if (arg_printed > 0) fprintf(gen->output, ", ");
                        generate_expression(gen, arg);
                        arg_printed++;
                    }
                    if (arg_printed > 0) fprintf(gen->output, ", ");
                    fprintf(gen->output, "_bcfg);\n");

                    gen->indent_level--;
                    print_indent(gen);
                    fprintf(gen->output, "}\n");

                } else if (has_trailing) {
                    // REGULAR PATTERN: function runs first, block decorates
                    // Check if function returns void (no return value to capture)
                    int returns_void = 1;
                    if (inner->node_type && inner->node_type->kind != TYPE_VOID &&
                        inner->node_type->kind != TYPE_UNKNOWN) {
                        returns_void = 0;
                    }
                    // Also check if function has return statements
                    if (inner->value) {
                        for (int fi = 0; fi < gen->program->child_count; fi++) {
                            ASTNode* fdef = gen->program->children[fi];
                            if (fdef && (fdef->type == AST_FUNCTION_DEFINITION || fdef->type == AST_BUILDER_FUNCTION) &&
                                fdef->value && strcmp(fdef->value, inner->value) == 0) {
                                if (has_return_value(fdef)) returns_void = 0;
                                break;
                            }
                        }
                    }

                    if (!returns_void) {
                        // Capture return value and push as context
                        print_indent(gen);
                        fprintf(gen->output, "_aether_ctx_push((void*)(intptr_t)");
                        generate_expression(gen, inner);
                        fprintf(gen->output, ");\n");
                    } else {
                        // Void function — just call it, push NULL context
                        generate_expression(gen, inner);
                        fprintf(gen->output, ";\n");
                        print_indent(gen);
                        fprintf(gen->output, "_aether_ctx_push((void*)0);\n");
                    }
                } else {
                    /* `exit(code)` is noreturn — libc exit() terminates the
                     * process immediately, so any function-exit defer-frees
                     * emitted AFTER this call (at the function's natural end)
                     * never run, leaking every live heap local. Emit all
                     * pending defers FIRST so the heap is reclaimed before the
                     * process ends. The defers are flag-guarded (`if(_heap_x)`)
                     * and emit_all_defers is non-destructive, so other (non-
                     * exit) paths still get their own scope-exit frees. This
                     * is what made tests ending in `exit(0)` leak all their
                     * string/seq locals. */
                    if (inner && inner->type == AST_FUNCTION_CALL && inner->value &&
                        strcmp(inner->value, "exit") == 0) {
                        emit_all_defers(gen);
                    }
                    /* A bare call-statement discards the call's value, so
                     * heap-returning inline args must still be drained even
                     * when the callee's declared return type is VOID or
                     * unknown (e.g. `check(label, string.from_long(n), ...)`).
                     * Signal that to the call codegen; the flag is captured
                     * and cleared at the call entry, and reset here too so
                     * it can never leak past this statement. */
                    if (inner && inner->type == AST_FUNCTION_CALL) {
                        gen->discard_call_node = inner;
                    }
                    /* Transient capturing-closure argument: a closure passed
                     * to a parameter that neither stores nor returns it
                     * (callback pattern, run(cb){ cb() }) is dead after the
                     * call, so its heap env must be freed or it leaks. Find
                     * the first non-trailing closure arg (trailing blocks are
                     * inlined below, not passed by value) and, when its
                     * parameter provably does not escape, emit the env-
                     * draining call form.
                     *
                     * Soundness gate (closure-env-freed-when-passed-to-extern):
                     * the env-drain is only safe when we have *proof* the
                     * callee neither stores nor returns the closure. The
                     * `callee_param_escapes_via_body` walk requires a visible
                     * body to be authoritative; for an extern callee the
                     * walk silently defaults to "does not escape", which is
                     * exactly wrong for the common extern-callback-registry
                     * pattern (the C side keeps the boxed closure and
                     * invokes it later). Treat unknown-body callees as
                     * escaping, the fail-safe direction (leak over UAF),
                     * unless the extern declares the parameter `@noescape`
                     * (#2523). */
                    ASTNode* cclos = transient_closure_arg(gen, inner);
                    if (inner && call_returns_owned_closure(gen, inner)) {
                        /* #2507: a closure a call hands over, discarded:
                         * nothing else holds it. */
                        gen->discard_call_node = NULL;
                        fprintf(gen->output, "{ _AeClosure _ae_dc = ");
                        generate_expression(gen, inner);
                        fprintf(gen->output, "; _aether_closure_env_release(_ae_dc.env); }\n");
                    } else if (cclos) {
                        emit_closure_env_drained_call(gen, inner, cclos);
                        fprintf(gen->output, "\n");
                    } else {
                        /* A statement whose value is thrown away is
                         * -Wunused-value in the user's own build: a
                         * line-leading `- b`, a bare name, an `x ? y` ask
                         * whose reply is ignored. The cast says what the
                         * statement means (evaluate, keep nothing) and costs
                         * nothing at runtime.
                         *
                         * An allow-list, not a deny-list: some statements
                         * lower to something that is not an expression at all
                         * (a bare array literal emits a braced initializer,
                         * and `(void)({2, 99})` is a statement-expression
                         * containing a brace list, which does not compile).
                         * Calls are left out too: discarding a call's result
                         * is ordinary and warns nowhere. */
                        /* A bare array-literal statement (`[1 + 1, 99]` on
                         * its own line) has no value and no C spelling: the
                         * literal lowers to a braced initializer, and
                         * `{2, 99};` is not a statement any C compiler
                         * accepts. It used to be emitted anyway, so a program
                         * the parser accepts produced C that did not build.
                         * Emit the elements as discarded expressions instead:
                         * anything with a side effect still runs, in order. */
                        if (inner && inner->type == AST_ARRAY_LITERAL) {
                            if (inner->child_count == 0) {
                                fprintf(gen->output, "(void)0;\n");
                            } else {
                                for (int ei = 0; ei < inner->child_count; ei++) {
                                    fprintf(gen->output, "(void)(");
                                    generate_expression(gen, inner->children[ei]);
                                    fprintf(gen->output, ");%s",
                                            ei + 1 == inner->child_count ? "\n" : " ");
                                }
                            }
                            gen->discard_call_node = NULL;
                            break;
                        }
                        /* A thrown-away string the statement owns (a
                         * heap-returning call, an interpolation, an ask
                         * answered with a string): freed here, as `_ = e`
                         * frees it. Discarded bare, it leaked one buffer
                         * per statement. */
                        if (inner && (inner->type == AST_FUNCTION_CALL ||
                                      inner->type == AST_STRING_INTERP ||
                                      inner->type == AST_SEND_ASK) &&
                            is_heap_string_expr(gen, inner)) {
                            /* The value is used (freed), not discarded: an
                             * argument drain keeps the result. */
                            gen->discard_call_node = NULL;
                            fprintf(gen->output, "aether_heap_str_free((void*)(");
                            generate_expression(gen, inner);
                            fprintf(gen->output, "));\n");
                            break;
                        }
                        /* A struct a call returns by value, owning strings,
                         * that nothing in this statement keeps: the result
                         * thrown away, or the argument of another call (a
                         * fluent chain, `expect_str(s).to_equal(t)`). The
                         * callee's parameter only borrows it (a struct
                         * parameter disowns its copy on entry, and a store
                         * of it copies), so nothing holds its strings past
                         * the statement, and nothing used to free them: the
                         * statement now keeps each in a temporary and
                         * destroys it once done, as a local's scope exit
                         * would. Zeroed first, so one a short circuit
                         * skipped destroys nothing. */
                        ASTNode** st_nodes = NULL;
                        int st_count = 0, st_cap = 0;
                        collect_stmt_struct_temps(gen, inner, &st_nodes, &st_count, &st_cap);
                        const char** st_names = NULL;
                        ASTNode** st_outer_nodes = NULL;
                        const char** st_outer_names = NULL;
                        int st_outer_count = 0;
                        stmt_struct_temps_get(&st_outer_nodes, &st_outer_names, &st_outer_count);
                        if (st_count > 0) {
                            st_names = (const char**)aether_xrealloc(NULL, sizeof(char*) * (size_t)st_count);
                            static int st_seq = 0;
                            fprintf(gen->output, "{ ");
                            for (int ti = 0; ti < st_count; ti++) {
                                st_names[ti] = cg_internf("_ae_stmp%d", st_seq++);
                                fprintf(gen->output, "%s %s = {0}; ",
                                        get_c_type(st_nodes[ti]->node_type), st_names[ti]);
                            }
                            stmt_struct_temps_set(st_nodes, st_names, st_count);
                        }
                        int discards_value = inner && (
                            inner->type == AST_IDENTIFIER ||
                            inner->type == AST_LITERAL ||
                            inner->type == AST_UNARY_EXPRESSION ||
                            inner->type == AST_SEND_ASK ||
                            (inner->type == AST_BINARY_EXPRESSION &&
                             !(inner->value && strcmp(inner->value, "=") == 0)));
                        if (discards_value) fprintf(gen->output, "(void)(");
                        generate_expression(gen, inner);
                        if (discards_value) fprintf(gen->output, ")");
                        fprintf(gen->output, ";");
                        if (st_count > 0) {
                            stmt_struct_temps_set(st_outer_nodes, st_outer_names, st_outer_count);
                            for (int ti = 0; ti < st_count; ti++) {
                                fprintf(gen->output, " %s_destroy(&%s);",
                                        struct_owning_strings(gen, st_nodes[ti]->node_type),
                                        st_names[ti]);
                            }
                            fprintf(gen->output, " }");
                            free(st_nodes);
                            free(st_names);
                        }
                        fprintf(gen->output, "\n");
                    }
                    gen->discard_call_node = NULL;
                }

                // Trailing blocks for non-defer: emit closure body as inline statements after the call
                if (inner && inner->type == AST_FUNCTION_CALL && !is_builder_call) {
                    for (int tc = 0; tc < inner->child_count; tc++) {
                        ASTNode* trailing = inner->children[tc];
                        if (trailing && trailing->type == AST_CLOSURE &&
                            trailing->value && strcmp(trailing->value, "trailing") == 0) {
                            for (int bi = 0; bi < trailing->child_count; bi++) {
                                if (trailing->children[bi] &&
                                    trailing->children[bi]->type == AST_BLOCK) {
                                    emit_trailing_block_body(gen, trailing->children[bi]);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            break;
            
        case AST_PRINT_STATEMENT:
            // Generate printf call with all arguments
            if (stmt->child_count > 0) {
                ASTNode* first_arg = stmt->children[0];

                // Interpolated string: delegate directly to expression codegen (emits printf(...)),
                // then flush like every other print below.
                if (stmt->child_count == 1 && first_arg->type == AST_STRING_INTERP) {
                    gen->interp_as_printf = 1;
                    generate_expression(gen, first_arg);
                    gen->interp_as_printf = 0;
                    fprintf(gen->output, ";\n");
                    fprintf(gen->output, "fflush(stdout);\n");
                    break;
                }

                // Check if we have a single typed argument (not a string literal)
                if (stmt->child_count == 1 && first_arg->node_type &&
                    !(first_arg->type == AST_LITERAL && first_arg->node_type->kind == TYPE_STRING)) {

                    Type* arg_type = first_arg->node_type;

                    // Generate printf with appropriate format string based on type
                    if (arg_type->kind == TYPE_INT) {
                        // Narrow to (int) to match %d: a single-scalar message
                        // field rides the intptr_t payload slot, so a TYPE_INT
                        // value can be stored wider. Genuine ints only reach
                        // here (ptr/actor-ref use %s), so nothing is truncated.
                        fprintf(gen->output, "printf(\"%%d\", (int)");
                        generate_expression(gen, first_arg);
                        fprintf(gen->output, ");\n");
                    } else if (arg_type->kind == TYPE_FLOAT || arg_type->kind == TYPE_FLOAT32) {
                        fprintf(gen->output, "printf(\"%%f\", ");
                        generate_expression(gen, first_arg);
                        fprintf(gen->output, ");\n");
                    } else if (arg_type->kind == TYPE_LONGDOUBLE) {
                        fprintf(gen->output, "printf(\"%%Lf\", ");
                        generate_expression(gen, first_arg);
                        fprintf(gen->output, ");\n");
                    } else if (arg_type->kind == TYPE_STRING) {
                        /* A heap-producing CALL in bare argument position
                         * (`print(string.concat(a, b))`) is a temporary
                         * nothing else owns: print-and-free via the owned
                         * helper or it leaks per call. Bare identifiers
                         * stay unwrapped, their scope-exit defer owns the
                         * free (wrapping them would double-free). The
                         * interpolation path already handles its own
                         * temporaries; this closes the direct-arg form. */
                        if (first_arg->type == AST_FUNCTION_CALL &&
                            is_heap_string_expr(gen, first_arg)) {
                            fprintf(gen->output, "_aether_print_owned(");
                            generate_expression(gen, first_arg);
                            fprintf(gen->output, ");\n");
                        } else {
                            // NULL-safe, and by length (#2521)
                            fprintf(gen->output, "_aether_print_str(");
                            generate_expression(gen, first_arg);
                            fprintf(gen->output, ");\n");
                        }
                    } else if (arg_type->kind == TYPE_BOOL) {
                        fprintf(gen->output, "printf(\"%%s\", ");
                        generate_expression(gen, first_arg);
                        fprintf(gen->output, " ? \"true\" : \"false\");\n");
                    } else if (arg_type->kind == TYPE_INT64) {
                        fprintf(gen->output, "printf(\"%%lld\", (long long)");
                        generate_expression(gen, first_arg);
                        fprintf(gen->output, ");\n");
                    } else if (arg_type->kind == TYPE_UINT32) {
                        /* Through %d a uint32 past 2^31 prints negative;
                         * uint32_t is unsigned int on every target, so %u
                         * is its conversion. */
                        fprintf(gen->output, "printf(\"%%u\", ");
                        generate_expression(gen, first_arg);
                        fprintf(gen->output, ");\n");
                    } else if (arg_type->kind == TYPE_DURATION) {
                        fprintf(gen->output, "printf(\"%%s\", _aether_duration_repr(");
                        generate_expression(gen, first_arg);
                        fprintf(gen->output, "));\n");
                    } else if (arg_type->kind == TYPE_PTR) {
                        // NULL-safe, and by length (#2521)
                        fprintf(gen->output, "_aether_print_str(");
                        generate_expression(gen, first_arg);
                        fprintf(gen->output, ");\n");
                    } else {
                        // Unknown type - default to %d
                        fprintf(gen->output, "printf(\"%%d\", ");
                        generate_expression(gen, first_arg);
                        fprintf(gen->output, ");\n");
                    }
                } else if (stmt->child_count == 1) {
                    // String literal - print directly
                    ASTNode* arg = stmt->children[0];
                    if (arg->type == AST_LITERAL && arg->node_type && arg->node_type->kind == TYPE_STRING) {
                        emit_print_literal_format(gen, arg);
                        fprintf(gen->output, ";\n");
                    } else {
                        // Unknown type - default to %d
                        fprintf(gen->output, "printf(\"%%d\", ");
                        generate_expression(gen, arg);
                        fprintf(gen->output, ");\n");
                    }
                } else {
                    // Multiple arguments - first is format string
                    // Auto-fix format specifiers based on argument types to prevent
                    // undefined behavior (e.g. print("Test: %s", 201) would crash)
                    ASTNode* fmt_arg = stmt->children[0];
                    if (fmt_arg->type == AST_LITERAL && fmt_arg->node_type &&
                        fmt_arg->node_type->kind == TYPE_STRING && fmt_arg->value) {
                        // Parse format string and replace specifiers with type-correct ones
                        const char* fmt = fmt_arg->value;
                        fprintf(gen->output, "printf(\"");
                        int arg_idx = 1;  // index into stmt->children for arguments
                        for (int fi = 0; fmt[fi]; fi++) {
                            if (fmt[fi] == '%' && fmt[fi + 1]) {
                                fi++;
                                // Skip flags, width, precision
                                while (fmt[fi] == '-' || fmt[fi] == '+' || fmt[fi] == ' ' ||
                                       fmt[fi] == '#' || fmt[fi] == '0') fi++;
                                while (fmt[fi] >= '0' && fmt[fi] <= '9') fi++;
                                if (fmt[fi] == '.') {
                                    fi++;
                                    while (fmt[fi] >= '0' && fmt[fi] <= '9') fi++;
                                }
                                if (fmt[fi] == '%') {
                                    // Literal %%
                                    fprintf(gen->output, "%%%%");
                                } else if (arg_idx < stmt->child_count) {
                                    // Replace with type-correct specifier
                                    ASTNode* arg = stmt->children[arg_idx];
                                    Type* atype = arg->node_type;
                                    if (atype && atype->kind == TYPE_LONGDOUBLE) {
                                        fprintf(gen->output, "%%Lf");
                                    } else if (atype && (atype->kind == TYPE_FLOAT || atype->kind == TYPE_FLOAT32)) {
                                        fprintf(gen->output, "%%f");
                                    } else if (atype && atype->kind == TYPE_INT64) {
                                        fprintf(gen->output, "%%lld");
                                    } else if (atype && atype->kind == TYPE_UINT32) {
                                        fprintf(gen->output, "%%u");
                                    } else if (atype && atype->kind == TYPE_DURATION) {
                                        fprintf(gen->output, "%%s");
                                    } else if (atype && (atype->kind == TYPE_STRING || atype->kind == TYPE_PTR)) {
                                        fprintf(gen->output, "%%s");
                                    } else if (atype && atype->kind == TYPE_BOOL) {
                                        fprintf(gen->output, "%%s");
                                    } else {
                                        fprintf(gen->output, "%%d");
                                    }
                                    arg_idx++;
                                } else {
                                    // More specifiers than args — keep original
                                    fprintf(gen->output, "%%%c", fmt[fi]);
                                }
                            } else {
                                // Re-escape special characters for C string output
                                switch (fmt[fi]) {
                                    case '\n': fprintf(gen->output, "\\n");  break;
                                    case '\t': fprintf(gen->output, "\\t");  break;
                                    case '\r': fprintf(gen->output, "\\r");  break;
                                    case '\0': fprintf(gen->output, "\\0");  break;
                                    case '\\': fprintf(gen->output, "\\\\"); break;
                                    case '"':  fprintf(gen->output, "\\\""); break;
                                    default:   fprintf(gen->output, "%c", fmt[fi]); break;
                                }
                            }
                        }
                        fprintf(gen->output, "\", ");
                        // Emit arguments with type-safe wrappers
                        for (int i = 1; i < stmt->child_count; i++) {
                            if (i > 1) fprintf(gen->output, ", ");
                            ASTNode* arg = stmt->children[i];
                            Type* atype = arg->node_type;
                            if (atype && atype->kind == TYPE_INT64) {
                                fprintf(gen->output, "(long long)");
                                generate_expression(gen, arg);
                            } else if (atype && atype->kind == TYPE_DURATION) {
                                fprintf(gen->output, "_aether_duration_repr(");
                                generate_expression(gen, arg);
                                fprintf(gen->output, ")");
                            } else if (atype && atype->kind == TYPE_BOOL) {
                                generate_expression(gen, arg);
                                fprintf(gen->output, " ? \"true\" : \"false\"");
                            } else if (atype && (atype->kind == TYPE_STRING || atype->kind == TYPE_PTR)) {
                                fprintf(gen->output, "_aether_safe_str(");
                                generate_expression(gen, arg);
                                fprintf(gen->output, ")");
                            } else if (atype && atype->kind == TYPE_INT) {
                                // Narrow to (int) to match the %d chosen above:
                                // a single-scalar message field rides the
                                // intptr_t payload slot, so a TYPE_INT value can
                                // be stored wider. Genuine ints only.
                                fprintf(gen->output, "(int)");
                                generate_expression(gen, arg);
                            } else {
                                generate_expression(gen, arg);
                            }
                        }
                        fprintf(gen->output, ");\n");
                    } else {
                        // Non-literal format string — use %s to prevent format injection
                        fprintf(gen->output, "printf(\"%%s\", ");
                        generate_expression(gen, stmt->children[0]);
                        fprintf(gen->output, ");\n");
                    }
                }
                // Flush stdout so partial-line output appears immediately
                // (without this, print(".") in a loop won't show until \n)
                fprintf(gen->output, "fflush(stdout);\n");
            }
            break;

        case AST_SEND_STATEMENT:
        case AST_SPAWN_ACTOR_STATEMENT:
            // Unreachable in a program that type-checks: generic send() /
            // spawn_actor() are rejected during type checking with a diagnostic
            // pointing at `actor ! Message { ... }` and `spawn(ActorType())`.
            // Kept as a defensive marker so a future path that reaches codegen
            // with one of these nodes fails the C compile loudly rather than
            // silently emitting nothing.
            fprintf(gen->output,
                    "#error internal: generic send()/spawn_actor() reached codegen\n");
            break;

        case AST_BLOCK: {
            // Save declared_var_count before the block. Variables declared
            // inside the block live in its C `{ ... }` scope and must not
            // leak to sibling statements that follow — otherwise a sibling
            // bare-block writing the same name is codegen'd as a
            // reassignment (no type on LHS) even though C scope already
            // closed the earlier declaration. This mirrors what the
            // AST_IF_STATEMENT path does at the `if`/`else` branch boundaries.
            int saved_var_count = gen->declared_var_count;
            print_line(gen, "{");
            indent(gen);
            enter_scope(gen);  // Track defer scope
            for (int i = 0; i < stmt->child_count; i++) {
                generate_statement(gen, stmt->children[i]);
            }
            exit_scope(gen);  // Emit defers and pop scope
            unindent(gen);
            print_line(gen, "}");
            truncate_declared_vars(gen, saved_var_count);
            break;
        }
        
        case AST_REPLY_STATEMENT:
            if (stmt->child_count > 0) {
                ASTNode* reply_expr = stmt->children[0];

                if (reply_expr->type == AST_MESSAGE_CONSTRUCTOR && reply_expr->value) {
                    MessageDef* msg_def = lookup_message(gen->message_registry, reply_expr->value);
                    if (msg_def) {
                        print_indent(gen);
                        // Construct the reply message (validates fields at compile time)
                        fprintf(gen->output, "{ %s _reply = { ._message_id = %d",
                                reply_expr->value, msg_def->message_id);

                        for (int i = 0; i < reply_expr->child_count; i++) {
                            ASTNode* field_init = reply_expr->children[i];
                            if (field_init && field_init->type == AST_FIELD_INIT) {
                                fprintf(gen->output, ", .%s = ", field_init->value);
                                if (field_init->child_count > 0) {
                                    MessageFieldDef* fdef = find_msg_field(msg_def, field_init->value);
                                    emit_message_field_init(gen, fdef, field_init->children[0]);
                                }
                            }
                        }
                        fprintf(gen->output, " }; ");

                        /* Deep-copy heap-string reply-message fields
                         * (#466). The reply message crosses an actor
                         * boundary back to the asker; without deep-
                         * copy the asker's reply-extraction reads
                         * the handler's heap-string after the
                         * handler's defer-free has run. */
                        for (MessageFieldDef* f = msg_def->fields; f; f = f->next) {
                            if (f->type_kind == TYPE_STRING) {
                                const char* lv = cg_internf("_reply.%s", f->name);
                                emit_message_string_copy(gen, lv,
                                    message_field_init_expr(reply_expr, f->name));
                            }
                        }

                        /* Send reply back to the waiting asker via the
                         * scheduler reply slot. #2528: a reply that owns
                         * strings or closures names its release, for the
                         * case nobody takes it. */
                        int reply_owns = 0;
                        for (MessageFieldDef* f = msg_def->fields; f; f = f->next) {
                            if (f->type_kind == TYPE_STRING ||
                                (f->type_kind == TYPE_FUNCTION && f->c_type &&
                                 strcmp(f->c_type, "_AeClosure") == 0)) { reply_owns = 1; break; }
                        }
                        if (reply_owns) {
                            fprintf(gen->output, "scheduler_reply_owned((ActorBase*)self, &_reply, sizeof(%s), (void (*)(void*))%s_release_fields); }\n",
                                    reply_expr->value, reply_expr->value);
                        } else {
                            fprintf(gen->output, "scheduler_reply((ActorBase*)self, &_reply, sizeof(%s)); }\n",
                                    reply_expr->value);
                        }
                    } else {
                        fprintf(stderr,
                                "aetherc: line %d: reply references unknown message type '%s'\n",
                                stmt->line, reply_expr->value);
                        exit(1);
                    }
                } else {
                    /* Expression reply (#1324): `reply count`. Deliver a
                     * typed copy through the same scheduler_reply slot the
                     * message form uses; the asker derefs the buffer as
                     * this type (see the AST_SEND_ASK scalar branch, which
                     * MUST stay in sync with this switch). */
                    TypeKind k = (reply_expr->node_type)
                        ? reply_expr->node_type->kind : TYPE_INT;
                    print_indent(gen);
                    if (k == TYPE_FUNCTION && !reply_expr->node_type->is_fnptr) {
                        /* #2528: the asker's binding owns the reply's
                         * closure, so a view is retained and a fresh one
                         * handed over. */
                        fprintf(gen->output, "{ _AeClosure _reply_val = ");
                        emit_closure_take(gen, reply_expr);
                        fprintf(gen->output,
                                "; scheduler_reply_owned((ActorBase*)self, &_reply_val, "
                                "sizeof(_reply_val), _aether_release_closure_buf); }\n");
                    } else if (k == TYPE_STRING) {
                        /* Deep-copy so the asker's read outlives the
                         * handler's defer-free, same contract as
                         * message-field string replies (#466). */
                        fprintf(gen->output, "{ const char* _reply_val = ");
                        generate_expression(gen, reply_expr);
                        fprintf(gen->output, "; ");
                        emit_message_string_copy(gen, "_reply_val", reply_expr);
                        /* #2528: the copy is released if nobody takes it. */
                        fprintf(gen->output,
                                "scheduler_reply_owned((ActorBase*)self, &_reply_val, "
                                "sizeof(const char*), _aether_release_string_buf); }\n");
                    } else {
                        const char* c_type = "int";
                        switch (k) {
                            case TYPE_FLOAT:      c_type = "double"; break;
                            case TYPE_LONGDOUBLE: c_type = "long double"; break;
                            case TYPE_BOOL:       c_type = "int"; break;
                            case TYPE_INT64:      c_type = "int64_t"; break;
                            case TYPE_UINT64:     c_type = "uint64_t"; break;
                            case TYPE_DURATION:   c_type = "int64_t"; break;
                            case TYPE_PTR:        c_type = "void*"; break;
                            default:              c_type = "int"; break;
                        }
                        fprintf(gen->output, "{ %s _reply_val = (%s)(", c_type, c_type);
                        generate_expression(gen, reply_expr);
                        fprintf(gen->output,
                                "); scheduler_reply((ActorBase*)self, &_reply_val, "
                                "sizeof(_reply_val)); }\n");
                    }
                }
            }
            break;
            
        default:
            for (int i = 0; i < stmt->child_count; i++) {
                generate_statement(gen, stmt->children[i]);
            }
            break;
    }
}

// =====================================================================
// Ownership diagnosis (--diagnose=ownership)
//
// The dot-normalisation fix in this same release flipped a class of
// latent leaks into latent UAFs in downstream code that aliased a
// heap-string across an ownership-transfer boundary (e.g. handing
// the string to map.put then reassigning the local). This pass walks
// the program after parse + typecheck and prints the same heap/non-
// heap verdicts the wrapper terminator at codegen_stmt.c:1611-1631
// would emit — without running codegen. The goal is to surface
// "this variable is now heap-tracked, the wrapper will free its
// previous value at the next reassignment" so a porter can audit
// whether the previous value is aliased anywhere before the crash
// hits at runtime.
// =====================================================================

static void diag_walk_assignments(CodeGenerator* gen, ASTNode* node,
                                   FILE* out, int* found_any) {
    if (!node) return;
    /* Don't descend into nested function/closure definitions — they
     * get their own pass at the top level. */
    if (node->type == AST_FUNCTION_DEFINITION ||
        node->type == AST_BUILDER_FUNCTION ||
        node->type == AST_CLOSURE) {
        return;
    }
    if (node->type == AST_VARIABLE_DECLARATION && node->value &&
        node->child_count > 0) {
        ASTNode* rhs = node->children[0];
        int rhs_heap = is_heap_string_expr(gen, rhs);
        int lhs_string =
            (node->node_type && node->node_type->kind == TYPE_STRING) ||
            rhs_heap;
        if (lhs_string) {
            const char* shape = "non-heap RHS";
            if (rhs->type == AST_STRING_INTERP) {
                shape = "string interpolation → HEAP";
            } else if (rhs->type == AST_FUNCTION_CALL && rhs->value) {
                shape = rhs_heap ? "heap-returning fn → HEAP"
                                 : "fn call (not heap-classified)";
            } else if (rhs->type == AST_LITERAL) {
                shape = "literal";
            } else if (rhs->type == AST_IDENTIFIER) {
                shape = "borrow from another variable";
            }
            int escaped = is_escaped_string_var(gen, node->value);
            fprintf(out,
                    "    line %4d: %-20s = ...   _heap_%s = %d   [%s]%s\n",
                    node->line,
                    node->value, node->value, rhs_heap ? 1 : 0, shape,
                    escaped ? "  ESCAPED, wrapper skips free" : "");
            *found_any = 1;
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        diag_walk_assignments(gen, node->children[i], out, found_any);
    }
}

void codegen_diagnose_ownership(ASTNode* program, FILE* out) {
    if (!program || !out) return;

    /* Build a minimal CodeGenerator. The predicates below read
     * `gen->program` (for the user-fn structural-escape lookup) and
     * `gen->extern_registry` (for the type-based escape param lookup
     * `lookup_callee_param_kind` does to keep `string.length(s)` from
     * over-marking `s` as escaped). The rest of the struct stays
     * zeroed. */
    CodeGenerator gen;
    memset(&gen, 0, sizeof(gen));
    gen.program = program;
    /* The verdicts are codegen's, on the tree codegen reads. */
    erase_string_retype_casts(program);
    /* Populate the extern registry so type-based escape analysis can
     * resolve `string.length`-style param kinds — without this every
     * call falls into the TYPE_UNKNOWN branch of call_arg_escapes,
     * which over-marks vars as escaped (because the conservative
     * answer is "may store"). Mirrors the registration generate_program
     * does at the top of normal codegen — both direct extern children
     * AND externs reachable via `import` statements (the std.string,
     * std.map, … pulled in by the program live in module ASTs, not
     * the program's own children). */
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* ch = program->children[i];
        if (!ch) continue;
        if (ch->type == AST_EXTERN_FUNCTION && ch->value) {
            register_extern_func(&gen, ch);
        } else if (ch->type == AST_IMPORT_STATEMENT && ch->value) {
            AetherModule* mod_entry = module_find(ch->value);
            ASTNode* mod_ast = mod_entry ? mod_entry->ast : NULL;
            if (mod_ast) {
                for (int j = 0; j < mod_ast->child_count; j++) {
                    ASTNode* decl = mod_ast->children[j];
                    if (decl && decl->type == AST_EXTERN_FUNCTION &&
                        decl->value) {
                        register_extern_func(&gen, decl);
                    }
                }
            }
        }
    }

    fprintf(out, "=== aether ownership diagnosis ===\n");
    fprintf(out, "(prints the heap/non-heap verdicts codegen will\n"
                 " use at the wrapper terminator in\n"
                 " codegen_stmt.c:1611-1631)\n\n");

    /* Pass 1 — string-returning user functions, with HEAP verdict. */
    fprintf(out, "[1] string-returning user functions\n");
    int sr_count = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (!c) continue;
        if (c->type != AST_FUNCTION_DEFINITION &&
            c->type != AST_BUILDER_FUNCTION) continue;
        /* For function defs, `node_type` holds the return type
         * directly (not a TYPE_FUNCTION wrapper) — see codegen_func.c
         * where `func->node_type` is read straight as the return type. */
        if (!c->node_type || c->node_type->kind != TYPE_STRING) continue;
        int heap = function_def_returns_heap_string(&gen, c);
        fprintf(out, "  %-30s line %4d   %s\n",
                c->value ? c->value : "(anonymous)",
                c->line,
                heap ? "HEAP, every return path heap-classified"
                     : "NOT HEAP, ≥ 1 return literal/borrowed/unclassified");
        sr_count++;
    }
    if (sr_count == 0) {
        fprintf(out, "  (none)\n");
    }

    /* Pass 2 — heap-tracked variable assignments, by function.
     *
     * Per-function we replay the same prelude codegen runs:
     * collect_heap_string_var_names → mark_heap_string_var (the
     * non-emitting half of hoist_heap_string_trackers) → then
     * mark_escaped_heap_string_vars. This populates the gen-side
     * registries that diag_walk_assignments queries
     * (is_heap_string_var, is_escaped_string_var) so the printed
     * verdicts match what the codegen would actually emit for the
     * same program. State is cleared between functions so a name
     * shadowed across fns doesn't carry over. */
    fprintf(out,
            "\n[2] string-typed variable assignments\n"
            "    (the codegen wrapper at line 1611-1631 emits\n"
            "     `if (_heap_<lhs>) free(_tmp_old); _heap_<lhs> = N`\n"
            "     after each assignment, with N as shown, except\n"
            "     where the var is marked ESCAPED, in which case the\n"
            "     wrapper emits a plain assignment instead, leaving\n"
            "     the previous heap value alive for the function's\n"
            "     lifetime so a stored alias stays valid)\n\n");
    int total = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (!c) continue;
        if (c->type != AST_FUNCTION_DEFINITION &&
            c->type != AST_BUILDER_FUNCTION &&
            c->type != AST_MAIN_FUNCTION) continue;

        /* Prelude — replicate what generate_function_definition runs
         * before the body, minus the emit. */
        clear_heap_string_vars(&gen);
        clear_escaped_string_vars(&gen);
        const char* heap_names[256];
        int heap_count = 0;
        for (int j = 0; j < c->child_count; j++) {
            collect_heap_string_var_names(&gen, c->children[j],
                                          heap_names, &heap_count, 256);
        }
        for (int n = 0; n < heap_count; n++) {
            if (!is_heap_string_var(&gen, heap_names[n])) {
                mark_heap_string_var(&gen, heap_names[n]);
            }
        }
        for (int j = 0; j < c->child_count; j++) {
            mark_escaped_heap_string_vars(&gen, c->children[j]);
        }

        fprintf(out, "  %s (line %d):\n",
                c->value ? c->value : "(anonymous)", c->line);
        int found = 0;
        for (int j = 0; j < c->child_count; j++) {
            diag_walk_assignments(&gen, c->children[j], out, &found);
        }
        if (!found) {
            fprintf(out, "    (no string-typed assignments)\n");
        }
        total++;
    }
    /* Final cleanup so the temporary registries don't outlive the
     * stack-allocated `gen`. */
    clear_heap_string_vars(&gen);
    clear_escaped_string_vars(&gen);
    if (total == 0) {
        fprintf(out, "  (no functions in program)\n");
    }

    fprintf(out,
            "\nUAF triage: any line above with `_heap_<lhs> = 1` AND\n"
            "no ESCAPED tag will have the wrapper free `<lhs>`'s\n"
            "previous value at the next reassignment. Lines tagged\n"
            "ESCAPED already had their wrapper-free skipped by the\n"
            "type-based escape analysis (the var was passed to a `ptr`\n"
            "parameter, captured by a closure, or returned from the\n"
            "function, all of which let the recipient store the\n"
            "pointer past the next reassignment); those leak the value\n"
            "across the function's lifetime in exchange for alias\n"
            "safety. If you see an ESCAPED-tagged line that you expected\n"
            "to free, check whether the recipient really retains the\n"
            "pointer, and if not, the conservative analysis is leaking\n"
            "more than necessary (file an issue with a repro).\n"
            "\n=== end diagnosis ===\n");
    code_generator_release(&gen);
}

/* Handing an owned string field to a call that takes the reference.
 *
 * A function that frees its `string` parameter takes the reference its
 * caller passed (param_consumed): a local handed to one counts as escaped,
 * so its scope exit frees nothing. A struct's string field handed to one
 * kept its `_heap_<field>` tracker set, so whatever freed the field next
 * freed the same buffer again: the store that replaces it, `p.name =
 * set_owned(p.name, v)` with `set_owned` freeing its first argument (the
 * shape #2369 suggested while stores through a parameter leaked), and the
 * struct's destructor after `string.free(p.name)`. The field now gives the
 * reference up as the call takes it. */
int callee_consumes_string_arg(CodeGenerator* gen, const char* func_name, int idx) {
    if (!gen || !func_name || idx < 0) return 0;
    if (is_consuming_free(gen, codegen_normalise_callee(func_name))) return 1;
    if (!callee_has_visible_body(gen, func_name) || !callee_param_is_string(gen, func_name, idx))
        return 0;
    const char* cp;
    ASTNode* cb;
    int saved_cl = g_escape_param_is_closure;
    int r = resolve_callee_param_body(gen, func_name, idx, &cp, &cb) &&
            param_consumed(gen, cb, cp, 1);
    g_escape_param_is_closure = saved_cl;
    return r;
}

/* An owned field read whose struct can be reached again to clear its
 * tracker: through a pointer, or a struct value that is a variable or a
 * field of one reached through a variable or a pointer (an lvalue; a call's
 * struct result has no address). */
int field_read_can_hand_off(ASTNode* e) {
    if (!is_owned_string_field_read(e)) return 0;
    ASTNode* obj = e->children[0];
    while (obj && obj->node_type && obj->node_type->kind != TYPE_PTR) {
        if (obj->type == AST_IDENTIFIER) return 1;
        /* A call's struct result the statement keeps in a temporary
         * (collect_stmt_struct_temps) and destroys: reached through it. */
        if (obj->type == AST_FUNCTION_CALL) return stmt_struct_temp_of(obj) != NULL;
        if (obj->type != AST_MEMBER_ACCESS || obj->child_count != 1) return 0;
        obj = obj->children[0];
    }
    return obj && obj->node_type != NULL;
}

/* `_ae_fo`, the address of the struct whose field is read. A path rooted at
 * a call held in a statement temporary is evaluated into the temporary
 * first and then addressed through it: `(temp = call()).inner` has no
 * address. */
static void emit_field_owner(CodeGenerator* gen, ASTNode* obj) {
    if (obj->node_type->kind == TYPE_PTR) {
        fprintf(gen->output, "__auto_type _ae_fo = ");
        generate_expression(gen, obj);
        fprintf(gen->output, "; ");
        return;
    }
    ASTNode* root = obj;
    while (root->type == AST_MEMBER_ACCESS && root->child_count == 1 &&
           root->children[0]->node_type && root->children[0]->node_type->kind != TYPE_PTR)
        root = root->children[0];
    const char* temp = root->type == AST_FUNCTION_CALL ? stmt_struct_temp_of(root) : NULL;
    if (!temp) {
        fprintf(gen->output, "__auto_type _ae_fo = &(");
        generate_expression(gen, obj);
        fprintf(gen->output, "); ");
        return;
    }
    fprintf(gen->output, "(void)");
    generate_expression(gen, root);
    fprintf(gen->output, "; __auto_type _ae_fo = &%s", temp);
    /* The member names from the root out to `obj`. */
    int depth = 0;
    for (ASTNode* n = obj; n != root; n = n->children[0]) depth++;
    for (int d = depth; d > 0; d--) {
        ASTNode* n = obj;
        for (int k = 1; k < d; k++) n = n->children[0];
        fprintf(gen->output, ".%s", n->value);
    }
    fprintf(gen->output, "; ");
}

/* The field's value, handed over. The frees a callee can take a reference
 * with (is_consuming_free) are the runtime's, which release a counted
 * AetherString and leave a plain malloc'd buffer alone, so only a counted
 * one leaves the field: the field keeps a plain buffer, and frees it. */
void emit_string_field_handoff(CodeGenerator* gen, ASTNode* e) {
    fprintf(gen->output, "({ ");
    emit_field_owner(gen, e->children[0]);
    fprintf(gen->output, "const char* _ae_fv = _ae_fo->%s; "
                         "if (aether_str_is_counted(_ae_fv)) _ae_fo->_heap_%s = 0; _ae_fv; })",
            e->value, e->value);
}

/* `string.free(s.f)` / `string.release(s.f)` / `release(s.f)`: what the
 * field owns goes through the shape-aware free, as for a tracked local,
 * wherever its tracker can be read (a struct value or heap.new box the
 * compiler can trace: #1873), and the field is left empty, owning nothing.
 * Elsewhere the runtime call frees a counted AetherString, which then
 * leaves the field; a plain buffer it cannot free stays the field's own,
 * as before, for the struct's destructor. */
void emit_string_field_free(CodeGenerator* gen, ASTNode* e, const char* runtime_free) {
    ASTNode* obj = e->children[0];
    int trusted = obj->node_type->kind == TYPE_PTR
        ? box_trackers_are_initialised(gen, obj)
        : value_path_trackers_are_initialised(gen, obj);
    const char* f = e->value;
    fprintf(gen->output, "({ ");
    emit_field_owner(gen, obj);
    if (trusted) {
        fprintf(gen->output,
                "if (_ae_fo->_heap_%s) aether_heap_str_free((void*)_ae_fo->%s); "
                "else %s(_ae_fo->%s); _ae_fo->%s = NULL; _ae_fo->_heap_%s = 0; })",
                f, f, runtime_free, f, f, f);
    } else {
        fprintf(gen->output,
                "const char* _ae_fv = _ae_fo->%s; int _ae_fc = aether_str_is_counted(_ae_fv); "
                "%s(_ae_fv); if (_ae_fc) { _ae_fo->%s = NULL; _ae_fo->_heap_%s = 0; } })",
                f, runtime_free, f, f);
    }
}
