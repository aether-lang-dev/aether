#include "codegen_internal.h"
#include "optimizer.h"
#include "../aether_module.h"
#include "../analysis/contract_eval.h"

// Is `name` the variable name of a known closure? If yes, also returns the
// closure id via *out_id. Used by return-site Bug B protection.
static int lookup_closure_var(CodeGenerator* gen, const char* name, int* out_id) {
    if (!gen || !name) return 0;
    for (int i = 0; i < gen->closure_var_count; i++) {
        if (gen->closure_var_map[i].var_name &&
            strcmp(gen->closure_var_map[i].var_name, name) == 0) {
            if (out_id) *out_id = gen->closure_var_map[i].closure_id;
            return 1;
        }
    }
    return 0;
}

// Append `name` to the protected-closures list if it isn't already there.
static void add_protected_name(char*** names, int* count, int* cap, const char* name) {
    if (!name) return;
    for (int i = 0; i < *count; i++) {
        if ((*names)[i] && strcmp((*names)[i], name) == 0) return;
    }
    if (*count >= *cap) {
        *cap = *cap ? *cap * 2 : 4;
        *names = aether_xrealloc(*names, *cap * sizeof(char*));
    }
    (*names)[(*count)++] = strdup(name);
}

// Walk `expr` and collect the names of any closure variables that appear.
// Accepts bare identifiers (`return bump`), box_closure wrappers
// (`return box_closure(bump)`), and nested calls. Then transitively expands:
// if `bump` captures `digit` and `digit` is also a closure variable, `digit`'s
// env must be protected too.
static void collect_returned_closures(CodeGenerator* gen, ASTNode* expr,
                                      char*** names, int* count, int* cap) {
    if (!expr) return;
    if (expr->type == AST_IDENTIFIER && expr->value) {
        int cid;
        if (lookup_closure_var(gen, expr->value, &cid)) {
            add_protected_name(names, count, cap, expr->value);
            // Transitive: any capture of this closure that is itself a
            // closure variable must also be protected. Likewise, any
            // capture of this closure that is a Route 1 promoted cell
            // must have its free suppressed — the cell's pointer is
            // inside the returned closure's env.
            for (int ci = 0; ci < gen->closure_count; ci++) {
                if (gen->closures[ci].id != cid) continue;
                const char* pfn = gen->closures[ci].parent_func;
                char** promoted = NULL;
                int promoted_count = 0;
                get_promoted_names_for_func(gen, pfn, &promoted, &promoted_count);
                for (int k = 0; k < gen->closures[ci].capture_count; k++) {
                    const char* cap_name = gen->closures[ci].captures[k];
                    if (!cap_name) continue;
                    if (lookup_closure_var(gen, cap_name, NULL)) {
                        add_protected_name(names, count, cap, cap_name);
                    }
                    for (int pp = 0; pp < promoted_count; pp++) {
                        if (promoted[pp] && strcmp(promoted[pp], cap_name) == 0) {
                            add_protected_name(names, count, cap, cap_name);
                            break;
                        }
                    }
                }
                break;
            }
        }
    }
    // Inline closure literal in the return expression (e.g.
    // `return || { count = count + 1; return count }`). Protect any
    // promoted captures this closure carries — the pointer lives inside
    // the returned closure's env and must not be freed before the
    // caller uses it.
    if (expr->type == AST_CLOSURE && expr->value) {
        int cid = atoi(expr->value);
        if (cid >= 0) {
            for (int ci = 0; ci < gen->closure_count; ci++) {
                if (gen->closures[ci].id != cid) continue;
                const char* pfn = gen->closures[ci].parent_func;
                char** promoted = NULL;
                int promoted_count = 0;
                get_promoted_names_for_func(gen, pfn, &promoted, &promoted_count);
                for (int k = 0; k < gen->closures[ci].capture_count; k++) {
                    const char* cap_name = gen->closures[ci].captures[k];
                    if (!cap_name) continue;
                    for (int pp = 0; pp < promoted_count; pp++) {
                        if (promoted[pp] && strcmp(promoted[pp], cap_name) == 0) {
                            add_protected_name(names, count, cap, cap_name);
                            break;
                        }
                    }
                }
                break;
            }
        }
    }
    for (int i = 0; i < expr->child_count; i++) {
        collect_returned_closures(gen, expr->children[i], names, count, cap);
    }
}

// ============================================================================
// ARITHMETIC SERIES LOOP COLLAPSE
//
// Detects while loops of the form:
//   while counter < bound {
//       acc1 = acc1 + invariant_expr1    // any number of accumulators
//       acc2 = acc2 + invariant_expr2
//       counter = counter + step         // must be a positive literal step
//   }
//
// And replaces them with closed-form O(1) expressions:
//   acc1 = acc1 + invariant_expr1 * (bound - counter);
//   acc2 = acc2 + invariant_expr2 * (bound - counter);
//   counter = bound;
//
// Works for any starting value of counter and any bound expression (even
// runtime variables) — the formula (bound - counter) computes remaining
// iterations correctly regardless of initial state.
//
// Also handles "counter <= bound" (adds one extra iteration).
// Also handles step != 1 via division.
// ============================================================================

#define MAX_SERIES_ACCUMULATORS 16

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
// copy.
static int alias_source_must_copy(CodeGenerator* gen, const char* src_name) {
    if (!gen || !gen->current_function || !src_name) return 1;
    return count_var_identifier_uses(gen->current_function, src_name) > 1;
}

// Returns 1 if the expression has any side effects (function calls, sends).
static int expr_has_side_effects(ASTNode* node) {
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
        if (expr_has_side_effects(node->children[i])) return 1;
    }
    return 0;
}

// Try to detect and emit a collapsed arithmetic series loop.
// Returns 1 if the loop was collapsed and emitted; 0 otherwise (caller emits normally).
static int try_emit_series_collapse(CodeGenerator* gen, ASTNode* while_node) {
    if (!while_node || while_node->child_count < 2) return 0;

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

    // Bound must not have side effects
    if (expr_has_side_effects(cond_right)) return 0;

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
    const char* acc_vars[MAX_SERIES_ACCUMULATORS];
    ASTNode*    acc_addends[MAX_SERIES_ACCUMULATORS];
    int         acc_is_linear[MAX_SERIES_ACCUMULATORS];   // 1 = addend is counter (linear sum)
    double      acc_linear_scale[MAX_SERIES_ACCUMULATORS]; // scale for counter*C pattern
    int         acc_count        = 0;
    int         found_counter    = 0;
    double      counter_step     = 1.0;

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

        ASTNode* addend = left_is_self ? rhs_right : rhs_left;

        // Track this target for bound-mutation check later
        if (stmt_target_count < MAX_SERIES_ACCUMULATORS + 1)
            stmt_targets[stmt_target_count++] = target;

        if (strcmp(target, counter_var) == 0) {
            // Counter increment: must be a positive literal step
            if (addend->type != AST_LITERAL || !addend->value) return 0;
            counter_step = atof(addend->value);
            if (counter_step <= 0.0) return 0;
            found_counter = 1;
        } else {
            // Accumulator: addend is either loop-invariant (constant series)
            // or the counter variable itself / counter*C (linear sum: Σ i = n*(n-1)/2).
            if (acc_count >= MAX_SERIES_ACCUMULATORS) return 0;

            int addend_is_counter = 0;
            double linear_scale = 1.0;

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
                    mr && mr->type == AST_LITERAL && mr->value) {
                    addend_is_counter = 1;
                    linear_scale = atof(mr->value);
                } else if (mr && mr->type == AST_IDENTIFIER && mr->value &&
                           strcmp(mr->value, counter_var) == 0 &&
                           ml && ml->type == AST_LITERAL && ml->value) {
                    addend_is_counter = 1;
                    linear_scale = atof(ml->value);
                }
            }

            if (addend_is_counter) {
                acc_vars[acc_count]          = target;
                acc_addends[acc_count]       = addend;
                acc_is_linear[acc_count]     = 1;
                acc_linear_scale[acc_count]  = linear_scale;
            } else {
                // Regular invariant addend: must not reference counter
                if (expr_references_var(addend, counter_var)) return 0;
                if (expr_has_side_effects(addend)) return 0;
                acc_vars[acc_count]          = target;
                acc_addends[acc_count]       = addend;
                acc_is_linear[acc_count]     = 0;
                acc_linear_scale[acc_count]  = 0.0;
            }
            acc_count++;
        }
    }

    if (!found_counter) return 0;

    // Linear sums require step = 1 (the triangular formula doesn't generalize cleanly to other steps).
    for (int i = 0; i < acc_count; i++) {
        if (acc_is_linear[i] && counter_step != 1.0) return 0;
    }

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

    // 4. Emit collapsed form, wrapped in a guard matching the original condition.
    // The guard is needed so that when counter >= bound (loop would not execute
    // at all), the accumulators are left unchanged — without it, the formula
    // (bound - counter) is zero or negative and could corrupt the accumulator.
    print_indent(gen);
    fprintf(gen->output, "if ((%s) %s (", counter_var, is_lte ? "<=" : "<");
    generate_expression(gen, cond_right);
    fprintf(gen->output, ")) {\n");
    indent(gen);

    // Emit each accumulator update.
    // Constant addend: acc = acc + addend * trip_count
    // Linear addend:   acc = acc + scale * (bound*(bound±1)/2 - counter*(counter-1)/2)
    int emitted_linear = 0;
    for (int i = 0; i < acc_count; i++) {
        print_indent(gen);
        if (acc_is_linear[i]) {
            // Triangular-number closed form:
            //   Σ(j = counter .. bound-1) j  =  bound*(bound-1)/2 - counter*(counter-1)/2
            //   Σ(j = counter .. bound)   j  =  bound*(bound+1)/2 - counter*(counter-1)/2
            if (acc_linear_scale[i] != 1.0) {
                fprintf(gen->output, "%s = %s + %g * (", acc_vars[i], acc_vars[i], acc_linear_scale[i]);
            } else {
                fprintf(gen->output, "%s = %s + (", acc_vars[i], acc_vars[i]);
            }
            // Cast to int64_t to prevent overflow for large N.
            // e.g., N=100000: N*(N-1)/2 = 4999950000 which exceeds int32 max.
            fprintf(gen->output, "(int64_t)(");
            generate_expression(gen, cond_right);
            if (is_lte) {
                fprintf(gen->output, ") * ((int64_t)(");
                generate_expression(gen, cond_right);
                fprintf(gen->output, ") + 1)");
            } else {
                fprintf(gen->output, ") * ((int64_t)(");
                generate_expression(gen, cond_right);
                fprintf(gen->output, ") - 1)");
            }
            fprintf(gen->output, " / 2 - (int64_t)%s * ((int64_t)%s - 1) / 2);\n", counter_var, counter_var);
            emitted_linear = 1;
        } else {
            // Constant addend: multiply by trip count (int64 to prevent overflow)
            fprintf(gen->output, "%s = %s + (int64_t)(", acc_vars[i], acc_vars[i]);
            generate_expression(gen, acc_addends[i]);
            fprintf(gen->output, ") * (");
            if (counter_step == 1.0) {
                fprintf(gen->output, "(int64_t)(");
                generate_expression(gen, cond_right);
                fprintf(gen->output, ") - %s", counter_var);
            } else {
                fprintf(gen->output, "((int64_t)(");
                generate_expression(gen, cond_right);
                fprintf(gen->output, ") - %s) / %g", counter_var, counter_step);
            }
            if (is_lte) {
                fprintf(gen->output, " + 1");
            }
            fprintf(gen->output, ");\n");
        }
    }

    // counter = bound (or bound + step for <=)
    print_indent(gen);
    fprintf(gen->output, "%s = (", counter_var);
    generate_expression(gen, cond_right);
    if (is_lte) {
        fprintf(gen->output, ") + %g;\n", counter_step);
    } else {
        fprintf(gen->output, ");\n");
    }

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
        generate_expression(gen, match_expr);
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
                print_line(gen, "int* %s = &_match_arr[1];", tail->value);
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
static int function_def_returns_heap_string(CodeGenerator* gen, ASTNode* fn_def);

// Linear scan over program-root children matching by `value`. Mirror
// of count_function_clauses in codegen.c (kept module-local here to
// avoid an internal-header churn). Returns the first match — for
// pattern-matched multi-clause functions, the first clause's body is
// representative for return-type purposes.
//
// Cost note: O(K) per call where K is the number of top-level fn
// definitions. Called once per user-fn-call-site during codegen, so
// total codegen-time cost is O(call_sites × K). For typical programs
// (K < 200) this is well under a millisecond. If the count grows,
// promote to a hash via gen->fn_def_lookup; the static here is the
// O(1)-amortised refactor seam.
static ASTNode* find_function_definition_by_name(ASTNode* program,
                                                 const char* name) {
    if (!program || !name) return NULL;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && (c->type == AST_FUNCTION_DEFINITION ||
                  c->type == AST_BUILDER_FUNCTION) &&
            c->value && strcmp(c->value, name) == 0) {
            return c;
        }
    }
    return NULL;
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
    char fn_norm[256];
    const char* fn = codegen_normalise_callee(expr->value, fn_norm, sizeof(fn_norm));
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

    // String interpolation (non-printf mode) allocates via _aether_interp.
    if (expr->type == AST_STRING_INTERP) {
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
        char fn_norm[256];
        const char* fn = codegen_normalise_callee(expr->value, fn_norm, sizeof(fn_norm));
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
             * path; io_read_file_raw returns the file contents). They are
             * single-value `-> string` externs, so they can't carry the
             * tuple-only `@heap` annotation — classify them here. Verified
             * owned (never a borrowed/literal pointer): the leak each
             * produced was exactly the caller never freeing this result. */
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
             * call sites like `s = http.request_body(req)` get
             * `_heap_s = 1` and the reassignment-wrapper free fires
             * on the next assignment to `s`.
             *
             * Tuple-returning externs use the parallel
             * `Type.tuple_heap_flags` channel consumed at the
             * destructure site in codegen_stmt.c:2018-2019; this
             * extern_returns_heap_string branch is the single-value
             * complement, the only path that closes leaks on externs
             * like http_request_body, fs.read_to_string, … whose
             * return shape has no tuple to hang per-position flags
             * on. There is no user-side workaround:
             * `string_concat(extern_call(), "")` copies but never
             * frees the underlying buffer. See the further-bug-fix5
             * filing for the rationale. */
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

int body_assigns_var_from_heap(CodeGenerator* gen, ASTNode* node,
                               const char* var_name) {
    if (!node || !var_name) return 0;
    if (node->type == AST_FUNCTION_DEFINITION ||
        node->type == AST_BUILDER_FUNCTION ||
        node->type == AST_CLOSURE) {
        return 0;
    }
    if (node->type == AST_VARIABLE_DECLARATION && node->value &&
        strcmp(node->value, var_name) == 0 &&
        node->child_count > 0 && node->children[0] &&
        is_heap_string_expr(gen, node->children[0])) {
        return 1;
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
                    char fn_norm[256];
                    const char* fn = codegen_normalise_callee(rhs->value,
                                                              fn_norm,
                                                              sizeof(fn_norm));
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
        if (body_assigns_var_from_heap(gen, node->children[i], var_name)) return 1;
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
                char fn_norm[256];
                const char* fn = codegen_normalise_callee(rhs->value, fn_norm,
                                                          sizeof(fn_norm));
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
        if (body_tuple_destructure_binds_heap(gen, node->children[i], var_name)) {
            return 1;
        }
    }
    return 0;
}

/* Heap evidence for a return-site expression inside the classifiers.
 * Bare identifiers MUST resolve structurally against the analysed
 * function's own body (see the INVARIANT above): is_heap_string_expr
 * consults the currently-emitting function's tracker table for
 * identifiers, which is the wrong function whenever classification is
 * triggered from a caller's destructure site. */
static int return_expr_is_heap(CodeGenerator* gen, ASTNode* expr,
                               ASTNode* fn_body_root) {
    if (!expr) return 0;
    if (expr->type == AST_IDENTIFIER) {
        return expr->value && fn_body_root &&
               (body_assigns_var_from_heap(gen, fn_body_root, expr->value) ||
                body_tuple_destructure_binds_heap(gen, fn_body_root,
                                                  expr->value));
    }
    return is_heap_string_expr(gen, expr);
}

static void walk_returns_for_heap_check(CodeGenerator* gen, ASTNode* node,
                                         const char* fn_being_analyzed,
                                         ASTNode* fn_body_root,
                                         int* any_heap, int* any_non_heap) {
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
            } else if (return_expr_is_heap(gen, ret, fn_body_root)) {
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
    for (int i = 0; i < node->child_count; i++) {
        walk_returns_for_heap_check(gen, node->children[i],
                                    fn_being_analyzed, fn_body_root,
                                    any_heap, any_non_heap);
    }
}

static int function_def_returns_heap_string(CodeGenerator* gen, ASTNode* fn_def) {
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
static int emit_struct_field_heap_assign(CodeGenerator* gen, ASTNode* lhs, ASTNode* rhs) {
    if (!gen || !lhs || !rhs) return 0;
    if (lhs->type != AST_MEMBER_ACCESS || !lhs->value) return 0;
    if (lhs->child_count != 1 || !lhs->children[0]) return 0;
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
    if (obj_type->kind == TYPE_STRUCT && obj_type->struct_name) {
        acc = ".";
        struct_name = obj_type->struct_name;
    } else if (obj_type->kind == TYPE_PTR && obj_type->element_type &&
               obj_type->element_type->kind == TYPE_STRUCT &&
               obj_type->element_type->struct_name &&
               is_heap_box_var(gen, obj->value)) {
        /* Only a heap.new(T) box has zero-initialised `_heap_<field>`
         * trackers, so only there is reading/freeing the previous field
         * value safe. A raw `malloc(...) as *T` has garbage trackers — its
         * field stays a bare store (#790 regression guard). */
        acc = "->";
        struct_name = obj_type->element_type->struct_name;
    } else {
        return 0;
    }
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
    print_indent(gen);
    fprintf(gen->output,
            "{ const char* _tmp_old = %s%s%s; %s%s%s = ",
            obj->value, acc, lhs->value,
            obj->value, acc, lhs->value);
    generate_expression(gen, rhs);
    fprintf(gen->output,
            "; if (%s%s_heap_%s) aether_heap_str_free(_tmp_old); "
            "%s%s_heap_%s = %d; }\n",
            obj->value, acc, lhs->value,
            obj->value, acc, lhs->value,
            rhs_is_heap ? 1 : 0);
    return 1;
}

/* Struct-reassignment helper (#465). For `<var> = <struct_literal>`
 * where `<var>` is a struct local with heap-string fields, emit
 * `<Struct>_destroy(&<var>)` BEFORE the bare assignment so the
 * previous struct's heap fields are reclaimed. Returns 1 if a
 * destroy call was emitted; the caller still proceeds with the
 * bare assignment emission afterward. */
static int emit_struct_destroy_before_reassign(CodeGenerator* gen,
                                                const char* var_name,
                                                Type* var_type) {
    if (!gen || !var_name || !var_type) return 0;
    if (var_type->kind != TYPE_STRUCT || !var_type->struct_name) return 0;
    if (!gen->program) return 0;
    ASTNode* sdef = find_struct_definition_by_name(gen->program, var_type->struct_name);
    if (!sdef || !struct_has_heap_string_field(sdef)) return 0;
    print_indent(gen);
    fprintf(gen->output, "%s_destroy(&%s);\n", var_type->struct_name, var_name);
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
static void walk_returns_for_heap_at(CodeGenerator* gen, ASTNode* node,
                                     int position, ASTNode* fn_body_root,
                                     int* found, int* any_heap, int* vetoed) {
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
                char fn_norm[256];
                const char* fn = codegen_normalise_callee(child->value,
                                                          fn_norm,
                                                          sizeof(fn_norm));
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
                return_expr_is_heap(gen, child, fn_body_root)) {
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
        if (return_expr_is_heap(gen, pos_expr, fn_body_root)) {
            *any_heap = 1;
        }
        return;
    }
    if (node->type == AST_FUNCTION_DEFINITION ||
        node->type == AST_BUILDER_FUNCTION ||
        node->type == AST_CLOSURE) {
        return;
    }
    for (int i = 0; i < node->child_count && !*vetoed; i++) {
        walk_returns_for_heap_at(gen, node->children[i], position,
                                 fn_body_root, found, any_heap, vetoed);
    }
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
            walk_returns_for_heap_at(gen, body, p, body,
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
    char fn_norm[256];
    const char* fn = codegen_normalise_callee(fallible->value, fn_norm,
                                              sizeof(fn_norm));
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
static void emit_tuple_return_position(CodeGenerator* gen, ASTNode* expr,
                                       int j) {
    int pos_heap = 0;
    if (gen && gen->current_function && !gen->in_main_function) {
        Type* rt = gen->current_func_return_type;
        if (rt && rt->kind == TYPE_TUPLE && j >= 0 && j < rt->tuple_count &&
            rt->tuple_types[j] && rt->tuple_types[j]->kind == TYPE_STRING) {
            pos_heap = function_def_returns_heap_at(gen,
                                                    gen->current_function, j);
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
void hoist_heap_string_trackers(CodeGenerator* gen, ASTNode* body) {
    if (!body || !gen) return;
    const char* names[256];  // 256 string vars per fn is generous
    int count = 0;
    collect_heap_string_var_names(gen, body, names, &count, 256);
    for (int i = 0; i < count; i++) {
        const char* name = names[i];
        /* Tracker hoist (existing) — applies to ALL collected names
         * including state vars / env caps / promoted caps, because
         * the wrapper sites for those still reference _heap_<name>. */
        if (!is_heap_string_var(gen, name)) {
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
        mark_var_declared(gen, name);
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
    char fn_norm[256];
    const char* fn = codegen_normalise_callee(func_name, fn_norm, sizeof(fn_norm));
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
static int is_nonstoring_builtin(const char* fn);
static int is_consuming_free(const char* fn);

/* Shared resolver: find user-fn `func_name`'s param-name + body block.
 * Returns 1 and fills out_pname and out_body on success; 0 otherwise. */
static int resolve_callee_param_body(CodeGenerator* gen, const char* func_name,
                                     int param_idx, const char** out_pname,
                                     ASTNode** out_body) {
    if (!gen || !gen->program || !func_name || param_idx < 0) return 0;
    char fn_norm[256];
    const char* fn = codegen_normalise_callee(func_name, fn_norm, sizeof(fn_norm));
    ASTNode* fn_def = find_function_definition_by_name(gen->program, fn);
    if (!fn_def || param_idx >= fn_def->child_count) return 0;
    ASTNode* param = fn_def->children[param_idx];
    if (!param || !param->value ||
        (param->type != AST_VARIABLE_DECLARATION &&
         param->type != AST_PATTERN_VARIABLE)) {
        return 0;
    }
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
    if (!resolve_callee_param_body(gen, func_name, param_idx, &pname, &body)) return 0;
    /* Used by the arg-drain / escape-pre-pass gates: a `return pname`
     * IS an escape (the value flows out to the caller). */
    return param_escapes_in_subtree(gen, body, pname, depth, /*return_is_escape=*/1);
}

/* Does the callee's param STORE-escape (anything except being directly
 * returned)? Used by the call-site identity-drain to distinguish a param
 * that is merely return-passed-through (identity-drainable) from one a
 * container/@retain/struct-field/closure owns (must NOT be freed). */
int callee_param_store_escapes_via_body(CodeGenerator* gen, const char* func_name,
                                        int param_idx) {
    const char* pname; ASTNode* body;
    if (!resolve_callee_param_body(gen, func_name, param_idx, &pname, &body)) {
        return 1;  /* unresolved → conservatively assume it stores */
    }
    return param_escapes_in_subtree(gen, body, pname, 0, /*return_is_escape=*/0);
}

/* Does the user function `func_name` declare a `-> string` return? Only
 * then is the call-site identity-drain meaningful (it compares the call
 * result pointer against the passed temp). */
int callee_returns_string(CodeGenerator* gen, const char* func_name) {
    if (!gen || !gen->program || !func_name) return 0;
    char fn_norm[256];
    const char* fn = codegen_normalise_callee(func_name, fn_norm, sizeof(fn_norm));
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
    char fn_norm[256];
    const char* fn = codegen_normalise_callee(func_name, fn_norm, sizeof(fn_norm));
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
    return 0;
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
            for (int i = 0; i < node->child_count; i++) {
                if (value_directly_carries_param(node->children[i], pname)) return 1;
            }
        }
    }
    if (node->type == AST_CLOSURE && subtree_mentions_param(node, pname)) {
        return 1;  /* closure capture may outlive the call */
    }
    if (node->type == AST_FUNCTION_CALL && node->value) {
        /* An INDIRECT call `cb(...)` lowers to value=="call" with child[0]
         * the CALLEE (the closure/fn being invoked) and children[1..] the
         * arguments. Invoking a parameter reads cb.fn/cb.env and runs it —
         * it neither stores nor returns cb, so the callee slot is NOT an
         * escape. Only the actual arguments can escape. For a NAMED call
         * (value == function name) every child is an argument. */
        int first_arg = (strcmp(node->value, "call") == 0) ? 1 : 0;
        for (int i = first_arg; i < node->child_count; i++) {
            ASTNode* a = node->children[i];
            if (a && a->type == AST_IDENTIFIER && a->value &&
                strcmp(a->value, pname) == 0) {
                char fn_norm[256];
                const char* fn = codegen_normalise_callee(node->value, fn_norm, sizeof(fn_norm));
                /* Read-only accessor (byte/length view, print, free):
                 * provably does not retain the pointer, so the param does
                 * not escape via THIS call. If the accessor's RETURN value
                 * (a view into the param) is later stored, the assignment
                 * / aggregate / return sinks above catch that mention
                 * separately — so this is sound. */
                if (is_nonstoring_builtin(fn)) {
                    /* not an escape via this call */
                } else if (is_consuming_free(fn)) {
                    return 1;  /* this function frees it; the caller must not */
                } else if (is_retain_extern_param(gen, fn, i)) {
                    return 1;
                } else if (call_arg_escapes(lookup_callee_param_kind(gen, node->value, i))) {
                    return 1;
                } else if (callee_param_escapes_via_body(gen, node->value, i, depth + 1)) {
                    return 1;
                }
            }
        }
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
static int is_consuming_free(const char* fn) {
    if (!fn) return 0;
    return strcmp(fn, "release") == 0 ||
           strcmp(fn, "string_release") == 0 ||
           strcmp(fn, "string_free") == 0;
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
    char fn_norm[256];
    const char* fn = call->value
        ? codegen_normalise_callee(call->value, fn_norm, sizeof(fn_norm))
        : NULL;
    if (fn && (is_nonstoring_builtin(fn) || is_consuming_free(fn))) return 0;
    if (fn && is_retain_extern_param(gen, fn, arg_idx)) return 1;
    if (callee_has_visible_body(gen, call->value)) {
        /* Visible body → the body-walk is authoritative (sees through
         * read-only accessors, ignores self-assignment `p = p`). */
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

static void escape_inspect_call_args(CodeGenerator* gen, ASTNode* call,
                                      const char* consumed_lhs) {
    if (!call) return;
    /* Children of an AST_FUNCTION_CALL are the arg expressions. Each
     * is itself walked recursively in case it nests further calls. */
    for (int i = 0; i < call->child_count; i++) {
        ASTNode* arg = call->children[i];
        if (!arg) continue;
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
        } else {
            /* Non-identifier arg (literal, nested call, etc.) —
             * still recurse to find any identifiers buried inside. */
            escape_walk(gen, arg, consumed_lhs);
        }
    }
}

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
        for (int i = 0; i < node->child_count; i++) {
            escape_walk(gen, node->children[i], NULL);
        }
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
        escape_inspect_call_args(gen, node, consumed_lhs);
        return;
    }

    /* Variable assignment / declaration: `V = <expr>`. The RHS is
     * walked with `consumed_lhs = V` so a `V = f(V, ...)` pattern
     * doesn't mark V escaped. Other LHS values inside the RHS still
     * follow normal rules. */
    if (node->type == AST_VARIABLE_DECLARATION && node->value &&
        node->child_count > 0) {
        const char* lhs = node->value;
        for (int i = 0; i < node->child_count; i++) {
            escape_walk(gen, node->children[i], lhs);
        }
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
            if (rhs && rhs->type == AST_IDENTIFIER && rhs->value &&
                is_heap_string_var(gen, rhs->value)) {
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

    /* Don't descend into nested function definitions — their own pass
     * handles them. */
    if (node->type == AST_FUNCTION_DEFINITION ||
        node->type == AST_BUILDER_FUNCTION) {
        return;
    }

    /* Default: recurse with the same consumed_lhs context. */
    for (int i = 0; i < node->child_count; i++) {
        escape_walk(gen, node->children[i], consumed_lhs);
    }
}

void mark_escaped_heap_string_vars(CodeGenerator* gen, ASTNode* body) {
    if (!gen || !body) return;
    escape_walk(gen, body, NULL);
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
         * promoted-cell paths (see is_env_free_for /
         * is_promoted_free_for in codegen.c). Adding a heap-
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
         * emit_all_defers_protected pick the annotation up and
         * emit the conditional-free directly without descending
         * into the body. */
        char annot[300];
        snprintf(annot, sizeof(annot), "heap_string_exit_free:%s", name);
        ASTNode* carrier = create_ast_node(AST_EXPRESSION_STATEMENT, NULL,
                                            body->line, body->column);
        if (carrier) {
            if (carrier->annotation) free(carrier->annotation);
            carrier->annotation = strdup(annot);
            push_defer(gen, carrier);
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
        char fn_norm[256];
        const char* fn = codegen_normalise_callee(node->value, fn_norm, sizeof(fn_norm));
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
        char annot[300];
        snprintf(annot, sizeof(annot), "seq_exit_free:%s", name);
        ASTNode* carrier = create_ast_node(AST_EXPRESSION_STATEMENT, NULL,
                                            body->line, body->column);
        if (carrier) {
            if (carrier->annotation) free(carrier->annotation);
            carrier->annotation = strdup(annot);
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
        char annot[300];
        snprintf(annot, sizeof(annot), "opt_str_exit_free:%s", name);
        ASTNode* carrier = create_ast_node(AST_EXPRESSION_STATEMENT, NULL,
                                            body->line, body->column);
        if (carrier) {
            if (carrier->annotation) free(carrier->annotation);
            carrier->annotation = strdup(annot);
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
static void collect_branch_decl_names(ASTNode* body,
                                       const char** names, int* count, int cap) {
    if (!body) return;
    for (int i = 0; i < body->child_count; i++) {
        ASTNode* child = body->children[i];
        if (!child) continue;
        if (child->type == AST_VARIABLE_DECLARATION && child->value
            && *count < cap) {
            // Dedup so a branch like `x = 1; x = 2` only registers once.
            int already = 0;
            for (int j = 0; j < *count; j++) {
                if (strcmp(names[j], child->value) == 0) { already = 1; break; }
            }
            if (!already) names[(*count)++] = child->value;
        }
    }
}

// Find the AST_VARIABLE_DECLARATION node for `name` inside a block,
// returning the first match (so type inference can use its initializer).
static ASTNode* find_branch_decl(ASTNode* body, const char* name) {
    if (!body || !name) return NULL;
    for (int i = 0; i < body->child_count; i++) {
        ASTNode* child = body->children[i];
        if (!child) continue;
        if (child->type == AST_VARIABLE_DECLARATION && child->value
            && strcmp(child->value, name) == 0) {
            return child;
        }
    }
    return NULL;
}

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
static void emit_hoisted_local_decl(CodeGenerator* gen, Type* var_type,
                                     const char* name) {
    if (var_type && var_type->kind == TYPE_ARRAY && var_type->array_size > 0) {
        const char* elem = get_c_type(var_type->element_type);
        fprintf(gen->output, "%s %s[%d];\n", elem, name, var_type->array_size);
        return;
    }
    fprintf(gen->output, "%s %s;\n", get_c_type(var_type), name);
}

static void hoist_if_else_common_vars(CodeGenerator* gen,
                                       ASTNode* then_body,
                                       ASTNode* else_body) {
    if (!then_body || !else_body) return;
    const char* then_names[64];
    int then_count = 0;
    collect_branch_decl_names(then_body, then_names, &then_count, 64);
    const char* else_names[64];
    int else_count = 0;
    collect_branch_decl_names(else_body, else_names, &else_count, 64);

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
        mark_var_declared(gen, n);

        // Recover a usable type from either branch's initializer.
        ASTNode* decl = find_branch_decl(then_body, n);
        Type* var_type = decl ? decl->node_type : NULL;
        if ((!var_type || var_type->kind == TYPE_VOID
             || var_type->kind == TYPE_UNKNOWN)
            && decl && decl->child_count > 0
            && decl->children[0] && decl->children[0]->node_type) {
            var_type = decl->children[0]->node_type;
        }
        if (!var_type || var_type->kind == TYPE_VOID
            || var_type->kind == TYPE_UNKNOWN) {
            decl = find_branch_decl(else_body, n);
            if (decl && decl->child_count > 0
                && decl->children[0] && decl->children[0]->node_type) {
                var_type = decl->children[0]->node_type;
            }
        }
        print_indent(gen);
        emit_hoisted_local_decl(gen, var_type, n);
    }
}

// Pre-declare variables from a while/for loop body so they're visible
// at function scope in the generated C. Without this, variables first
// assigned inside a while block are C-block-scoped and invisible to
// subsequent while blocks in the same function.
static void hoist_loop_vars(CodeGenerator* gen, ASTNode* body) {
    if (!body) return;
    for (int i = 0; i < body->child_count; i++) {
        ASTNode* child = body->children[i];
        if (!child) continue;
        if (child->type == AST_VARIABLE_DECLARATION && child->value) {
            /* #744: don't hoist a module-level `var` global as a loop-
             * scoped local — it would shadow the file-scope static. */
            if (!is_var_declared(gen, child->value) &&
                !is_module_global_var(gen, child->value)) {
                mark_var_declared(gen, child->value);
                // Determine type
                Type* var_type = child->node_type;
                if ((!var_type || var_type->kind == TYPE_VOID || var_type->kind == TYPE_UNKNOWN)
                    && child->child_count > 0 && child->children[0] && child->children[0]->node_type) {
                    var_type = child->children[0]->node_type;
                }
                const char* c_type = get_c_type(var_type);
                print_indent(gen);
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
                        if (sdef && struct_has_heap_string_field(sdef)) {
                            char annot[300];
                            snprintf(annot, sizeof(annot),
                                     "struct_destroy:%s:%s",
                                     child->value, var_type->struct_name);
                            ASTNode* carrier = create_ast_node(
                                AST_EXPRESSION_STATEMENT, NULL,
                                child->line, child->column);
                            if (carrier) {
                                if (carrier->annotation) free(carrier->annotation);
                                carrier->annotation = strdup(annot);
                                push_defer(gen, carrier);
                            }
                        }
                    }
                } else {
                    emit_hoisted_local_decl(gen, var_type, child->value);
                }
            }
        }
        // Recurse into nested blocks (e.g., if inside while)
        if (child->type == AST_IF_STATEMENT || child->type == AST_WHILE_LOOP ||
            child->type == AST_FOR_LOOP) {
            for (int j = 0; j < child->child_count; j++) {
                hoist_loop_vars(gen, child->children[j]);
            }
        }
    }
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
static void collect_if_branch_vars(ASTNode* body, const char** out, int* count, int max);

static int has_identifier_ref(ASTNode* node, const char* name) {
    if (!node || !name) return 0;
    if (node->type == AST_IDENTIFIER && node->value &&
        strcmp(node->value, name) == 0) return 1;
    /* Don't treat a fresh declaration as a "ref" — only post-decl
     * uses count. But we don't know declaration order from a single
     * subtree, so treat any AST_IDENTIFIER as a use. The hoist is
     * over-eager but safe. */
    for (int i = 0; i < node->child_count; i++) {
        if (has_identifier_ref(node->children[i], name)) return 1;
    }
    return 0;
}

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
    /* First: collect names that appear as top-level declarations in
     * the function body (outside any if). These already get a
     * function-scope declaration via the regular generate_statement
     * path AND its companion `_heap_<name>` tracker. Hoisting them
     * here would emit a duplicate declaration AND skip the heap
     * tracker — see the test_string_late_heap_reassign repro that
     * exercises variant 2 (`line = ""` then if/else reassignment). */
    const char* top_level_decls[64];
    int top_count = 0;
    for (int i = 0; i < body->child_count && top_count < 64; i++) {
        ASTNode* child = body->children[i];
        if (!child) continue;
        if (child->type == AST_VARIABLE_DECLARATION && child->value) {
            top_level_decls[top_count++] = child->value;
        }
    }

    /* Walk top-level statements collecting names first-declared
     * inside any if-branch. */
    const char* names[64];
    int count = 0;
    for (int i = 0; i < body->child_count; i++) {
        ASTNode* child = body->children[i];
        if (!child || child->type != AST_IF_STATEMENT) continue;
        /* Walk both then- and else- branches (children[1], [2] when
         * present). children[0] is the condition. */
        for (int j = 1; j < child->child_count && j < 3; j++) {
            collect_if_branch_vars(child->children[j], names, &count, 64);
        }
    }
    /* Filter out names already declared at top level. */
    int kept = 0;
    for (int n = 0; n < count; n++) {
        int dup = 0;
        for (int k = 0; k < top_count; k++) {
            if (strcmp(names[n], top_level_decls[k]) == 0) { dup = 1; break; }
        }
        if (!dup) names[kept++] = names[n];
    }
    count = kept;
    /* For each candidate, only hoist if it's referenced outside any
     * if-block in the function body (i.e. in a top-level statement
     * that isn't an AST_IF_STATEMENT, or as the controlling condition
     * of an if). Otherwise the existing C-local scoping was correct. */
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
        int referenced_outside = 0;
        for (int i = 0; i < body->child_count; i++) {
            ASTNode* child = body->children[i];
            if (!child) continue;
            if (child->type == AST_IF_STATEMENT) {
                /* The condition (child[0]) counts as outside-the-branch. */
                if (child->child_count > 0 &&
                    has_identifier_ref(child->children[0], name)) {
                    referenced_outside = 1;
                    break;
                }
                continue;
            }
            if (has_identifier_ref(child, name)) {
                referenced_outside = 1;
                break;
            }
        }
        if (!referenced_outside) continue;
        /* Hoist: find the first declaration in any branch to recover
         * the type, then emit a function-scope declaration. */
        ASTNode* first_decl = NULL;
        for (int i = 0; i < body->child_count && !first_decl; i++) {
            ASTNode* child = body->children[i];
            if (!child || child->type != AST_IF_STATEMENT) continue;
            for (int j = 1; j < child->child_count && j < 3 && !first_decl; j++) {
                first_decl = find_branch_decl(child->children[j], name);
            }
        }
        if (!first_decl) continue;
        Type* var_type = first_decl->node_type;
        if ((!var_type || var_type->kind == TYPE_VOID || var_type->kind == TYPE_UNKNOWN)
            && first_decl->child_count > 0 && first_decl->children[0]
            && first_decl->children[0]->node_type) {
            var_type = first_decl->children[0]->node_type;
        }
        const char* c_type = get_c_type(var_type);
        print_indent(gen);
        fprintf(gen->output, "%s %s;\n", c_type, name);
        mark_var_declared(gen, name);
    }
}

static void collect_if_branch_vars(ASTNode* body, const char** out, int* count, int max) {
    if (!body || !out || !count) return;
    for (int i = 0; i < body->child_count && *count < max; i++) {
        ASTNode* child = body->children[i];
        if (!child) continue;
        if (child->type == AST_VARIABLE_DECLARATION && child->value) {
            int dup = 0;
            for (int k = 0; k < *count; k++) {
                if (strcmp(out[k], child->value) == 0) { dup = 1; break; }
            }
            if (!dup) out[(*count)++] = child->value;
        }
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
    if (sdef && struct_has_heap_string_field(sdef)) {
        mark_return_escaped_struct_var(gen, expr->value);
    }
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
static void push_struct_destroy_defer(CodeGenerator* gen, const char* var_name,
                                      Type* struct_type, int line, int col) {
    if (!gen->program || !var_name || !struct_type ||
        struct_type->kind != TYPE_STRUCT || !struct_type->struct_name) return;
    ASTNode* sdef = find_struct_definition_by_name(gen->program, struct_type->struct_name);
    if (!sdef || !struct_has_heap_string_field(sdef)) return;
    char annot[300];
    snprintf(annot, sizeof(annot), "struct_destroy:%s:%s",
             var_name, struct_type->struct_name);
    ASTNode* carrier = create_ast_node(AST_EXPRESSION_STATEMENT, NULL, line, col);
    if (carrier) {
        if (carrier->annotation) free(carrier->annotation);
        carrier->annotation = strdup(annot);
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

// #340: emit a match arm's result body — mirrors the generic match dispatch
// (block / statement / expression), including match-as-expression's
// `match_result_var` assignment so `let r = match m { ... }` works.
static void emit_opt_match_arm(CodeGenerator* gen, ASTNode* result) {
    if (!result) return;
    if (result->type == AST_BLOCK) {
        for (int j = 0; j < result->child_count; j++) {
            generate_statement(gen, result->children[j]);
        }
    } else if (result->type == AST_PRINT_STATEMENT ||
               result->type == AST_RETURN_STATEMENT ||
               result->type == AST_VARIABLE_DECLARATION) {
        generate_statement(gen, result);
    } else {
        print_indent(gen);
        if (gen->match_result_var) {
            fprintf(gen->output, "%s = ", gen->match_result_var);
        }
        generate_expression(gen, result);
        fprintf(gen->output, ";\n");
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
            if (wrap_v0) {
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
    if (should_uniform_heap_return(gen, stmt)) {
        emit_uniform_heap_return_expr(gen, stmt->children[0]);
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

void generate_statement(CodeGenerator* gen, ASTNode* stmt) {
    if (!stmt) return;

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
                char fn_norm[256];
                const char* fn = codegen_normalise_callee(rhs->value,
                                                          fn_norm,
                                                          sizeof(fn_norm));
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
                    if (!is_var_declared(gen, var->value)) {
                        const char* c_type = var_type && var_type[0] ? var_type : "int";
                        fprintf(gen->output,
                                "%s* %s = malloc(sizeof(%s)); *%s = _tup%d._%d;\n",
                                c_type, var->value, c_type, var->value, tmp_id, j);
                        mark_var_declared(gen, var->value);
                        ASTNode* free_call = create_ast_node(AST_FUNCTION_CALL, "free",
                            stmt->line, stmt->column);
                        ASTNode* arg = create_ast_node(AST_IDENTIFIER, var->value,
                            stmt->line, stmt->column);
                        if (arg->annotation) free(arg->annotation);
                        arg->annotation = strdup("raw_promoted");
                        add_child(free_call, arg);
                        ASTNode* expr_stmt = create_ast_node(AST_EXPRESSION_STATEMENT, NULL,
                            stmt->line, stmt->column);
                        add_child(expr_stmt, free_call);
                        push_defer(gen, expr_stmt);
                    } else {
                        fprintf(gen->output, "*%s = _tup%d._%d;\n", var->value, tmp_id, j);
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
                            fprintf(gen->output, "%s = _tup%d._%d;\n",
                                    var->value, tmp_id, j);
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
                if (stmt->children[0]->type == AST_HEAP_NEW)
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
                // Generate as assignment to self->field
                if (stmt->child_count > 0 && is_heap_string_expr(gen, stmt->children[0])) {
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
                        fprintf(gen->output, "; if (_heap_%s) aether_heap_str_free(_tmp_old);",
                                stmt->value);
                        fprintf(gen->output, " _heap_%s = 1; }\n", stmt->value);
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
                    fprintf(gen->output, " if (_heap_%s) aether_heap_str_free(_tmp_old);", stmt->value);
                    fprintf(gen->output, " _heap_%s = 1; }\n", stmt->value);
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
                    if (!is_var_declared(gen, stmt->value)) {
                        mark_var_declared(gen, stmt->value);
                        // Infer type from first match arm result
                        const char* c_type = get_c_type(stmt->node_type);
                        ASTNode* match_node = stmt->children[0];
                        if ((!stmt->node_type || stmt->node_type->kind == TYPE_UNKNOWN) &&
                            match_node->child_count >= 2) {
                            ASTNode* first_arm = match_node->children[1];
                            if (first_arm && first_arm->child_count >= 2 && first_arm->children[1]) {
                                Type* arm_type = first_arm->children[1]->node_type;
                                if (arm_type) c_type = get_c_type(arm_type);
                            }
                        }
                        print_indent(gen);
                        fprintf(gen->output, "%s %s;\n", c_type, stmt->value);
                    }
                    // Generate match with result assignment
                    gen->match_result_var = stmt->value;
                    generate_statement(gen, stmt->children[0]);
                    gen->match_result_var = NULL;
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
                        // allocate, initialise, defer the free.
                        const char* c_type = get_c_type(stmt->node_type);
                        if (!c_type || c_type[0] == 0) c_type = "int";
                        fprintf(gen->output, "%s* %s = malloc(sizeof(%s)); *%s = ",
                                c_type, stmt->value, c_type, stmt->value);
                        if (stmt->child_count > 0) {
                            generate_expression(gen, stmt->children[0]);
                        } else {
                            fprintf(gen->output, "0");
                        }
                        fprintf(gen->output, ";\n");
                        mark_var_declared(gen, stmt->value);
                        // Defer free(name) at scope exit.
                        ASTNode* free_call = create_ast_node(AST_FUNCTION_CALL, "free",
                            stmt->line, stmt->column);
                        ASTNode* arg = create_ast_node(AST_IDENTIFIER, stmt->value,
                            stmt->line, stmt->column);
                        // Mark so the AST_IDENTIFIER emission doesn't dereference it
                        // (free takes the pointer itself, not `*name`).
                        if (arg->annotation) free(arg->annotation);
                        arg->annotation = strdup("raw_promoted");
                        add_child(free_call, arg);
                        ASTNode* expr_stmt = create_ast_node(AST_EXPRESSION_STATEMENT, NULL,
                            stmt->line, stmt->column);
                        add_child(expr_stmt, free_call);
                        push_defer(gen, expr_stmt);
                    } else {
                        // Reassignment: write through the pointer.
                        fprintf(gen->output, "*%s", stmt->value);
                        if (stmt->child_count > 0) {
                            fprintf(gen->output, " = ");
                            generate_expression(gen, stmt->children[0]);
                        }
                        fprintf(gen->output, ";\n");
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
                    if (var_is_seq && stmt->child_count > 0) {
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
                            fprintf(gen->output, "%s = ", stmt->value);
                            generate_expression(gen, stmt->children[0]);
                            fprintf(gen->output, ";\n");
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
                    } else {
                        // Plain non-string assignment.
                        /* Struct-reassignment heap cleanup (#465).
                         * `b = Box { ... }` overwrites the struct
                         * wholesale; without an explicit destroy of
                         * the previous instance, every heap-string
                         * field's old buffer leaks. Emit
                         * `<Struct>_destroy(&<var>)` before the bare
                         * assignment so any prior heap fields are
                         * reclaimed. The struct's literal initializer
                         * (codegen_expr.c AST_STRUCT_LITERAL) sets
                         * the new `_heap_<field>` bits so the next
                         * destroy at function exit fires correctly. */
                        Type* vtype = stmt->node_type;
                        if ((!vtype || vtype->kind != TYPE_STRUCT) &&
                            stmt->child_count > 0 && stmt->children[0]) {
                            vtype = stmt->children[0]->node_type;
                        }
                        emit_struct_destroy_before_reassign(gen, stmt->value, vtype);
                        fprintf(gen->output, "%s", stmt->value);
                        if (stmt->child_count > 0) {
                            fprintf(gen->output, " = ");
                            generate_expression(gen, stmt->children[0]);
                        }
                        fprintf(gen->output, ";\n");
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
                                            print_indent(gen);
                                            fprintf(gen->output, "{\n");
                                            gen->indent_level++;
                                            gen->in_trailing_block++;
                                            ASTNode* body = trailing->children[bi];
                                            for (int si = 0; si < body->child_count; si++) {
                                                generate_statement(gen, body->children[si]);
                                            }
                                            gen->in_trailing_block--;
                                            gen->indent_level--;
                                            print_indent(gen);
                                            fprintf(gen->output, "}\n");
                                            print_indent(gen);
                                            fprintf(gen->output, "_aether_ctx_pop();\n");
                                            print_indent(gen);
                                            char c_rfn[256];
                                            strncpy(c_rfn, safe_c_name(reinit_call->value), sizeof(c_rfn) - 1);
                                            c_rfn[sizeof(c_rfn) - 1] = '\0';
                                            for (char* p = c_rfn; *p; p++) { if (*p == '.') *p = '_'; }
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
                                            print_indent(gen);
                                            fprintf(gen->output, "{\n");
                                            gen->indent_level++;
                                            gen->in_trailing_block++;
                                            ASTNode* body = trailing->children[bi];
                                            for (int si = 0; si < body->child_count; si++) {
                                                generate_statement(gen, body->children[si]);
                                            }
                                            gen->in_trailing_block--;
                                            gen->indent_level--;
                                            print_indent(gen);
                                            fprintf(gen->output, "}\n");
                                            print_indent(gen);
                                            fprintf(gen->output, "_aether_ctx_pop();\n");
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else {
                    // First declaration - generate type + variable
                    mark_var_declared(gen, stmt->value);

                    // Detect if initializer is an array literal (type system may not tag empty arrays)
                    int is_array_init = (stmt->child_count > 0 &&
                                         stmt->children[0] &&
                                         stmt->children[0]->type == AST_ARRAY_LITERAL);

                    // Handle array types specially (C syntax: int name[size])
                    /* Issue #501 follow-up: volatile prefix for
                     * try-clobbered locals.  Same `vq` value used
                     * across each branch below so we only consult
                     * the set once. */
                    const char* vq = try_volatile_qual_for(gen, stmt->value);
                    if (stmt->node_type && stmt->node_type->kind == TYPE_ARRAY) {
                        const char* elem_type = get_c_type(stmt->node_type->element_type);
                        if (stmt->node_type->array_size > 0) {
                            fprintf(gen->output, "%s%s %s[%d]", vq, elem_type,
                                    stmt->value, stmt->node_type->array_size);
                        } else {
                            // Dynamic/empty array - use pointer
                            fprintf(gen->output, "%s%s* %s", vq, elem_type, stmt->value);
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
                        // Message/struct constructor — use the constructor name as type
                        fprintf(gen->output, "%s%s %s",
                                vq, stmt->children[0]->value, stmt->value);
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
                            if (sdef && struct_has_heap_string_field(sdef)) {
                                char annot[300];
                                snprintf(annot, sizeof(annot),
                                         "struct_destroy:%s:%s",
                                         stmt->value, stmt->children[0]->value);
                                ASTNode* carrier = create_ast_node(
                                    AST_EXPRESSION_STATEMENT, NULL,
                                    stmt->line, stmt->column);
                                if (carrier) {
                                    if (carrier->annotation) free(carrier->annotation);
                                    carrier->annotation = strdup(annot);
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
                            if (copy_alias) {
                                fprintf(gen->output, "aether_uniform_heap_str(");
                                generate_expression(gen, di);
                                fprintf(gen->output, ", 0)");
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
                        int cid = atoi(stmt->children[0]->value);
                        int existing_idx = -1;
                        for (int ci = 0; ci < gen->closure_var_count; ci++) {
                            if (gen->closure_var_map[ci].var_name &&
                                strcmp(gen->closure_var_map[ci].var_name, stmt->value) == 0) {
                                existing_idx = ci;
                                break;
                            }
                        }
                        int is_first_assignment = (existing_idx < 0);
                        if (existing_idx >= 0) {
                            if (gen->closure_var_map[existing_idx].closure_id != cid) {
                                gen->closure_var_map[existing_idx].closure_id = -1;
                            }
                        } else {
                            if (gen->closure_var_count >= gen->closure_var_capacity) {
                                gen->closure_var_capacity = gen->closure_var_capacity ? gen->closure_var_capacity * 2 : 16;
                                gen->closure_var_map = aether_xrealloc(gen->closure_var_map,
                                    gen->closure_var_capacity * sizeof(gen->closure_var_map[0]));
                            }
                            gen->closure_var_map[gen->closure_var_count].var_name = strdup(stmt->value);
                            gen->closure_var_map[gen->closure_var_count].closure_id = cid;
                            gen->closure_var_count++;
                        }

                        // Emit deferred free for heap-allocated closure envs
                        // only on the FIRST assignment — reassignment replaces
                        // the env pointer in the variable, and the existing
                        // defer will free whatever env is live at scope exit.
                        // Pushing a second defer on reassignment would
                        // double-free when the scope unwinds.
                        if (is_first_assignment) {
                            for (int ci = 0; ci < gen->closure_count; ci++) {
                                if (gen->closures[ci].id == cid && gen->closures[ci].capture_count > 0) {
                                    /* #1398: member-aware teardown so the env
                                       releases what its captures own. */
                                    char envfree[64];
                                    snprintf(envfree, sizeof(envfree),
                                             "_closure_env_%d_free", cid);
                                    ASTNode* free_call = create_ast_node(AST_FUNCTION_CALL, envfree,
                                        stmt->line, stmt->column);
                                    char env_access[256];
                                    snprintf(env_access, sizeof(env_access), "%s.env", safe_c_name(stmt->value));
                                    ASTNode* env_arg = create_ast_node(AST_IDENTIFIER, env_access,
                                        stmt->line, stmt->column);
                                    add_child(free_call, env_arg);
                                    // Wrap in expression statement so generate_statement handles it
                                    ASTNode* expr_stmt = create_ast_node(AST_EXPRESSION_STATEMENT, NULL,
                                        stmt->line, stmt->column);
                                    add_child(expr_stmt, free_call);
                                    push_defer(gen, expr_stmt);
                                    break;
                                }
                            }
                        }
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
                                            print_indent(gen);
                                            fprintf(gen->output, "{\n");
                                            gen->indent_level++;
                                            gen->in_trailing_block++;
                                            ASTNode* body = trailing->children[bi];
                                            for (int si = 0; si < body->child_count; si++) {
                                                generate_statement(gen, body->children[si]);
                                            }
                                            gen->in_trailing_block--;
                                            gen->indent_level--;
                                            print_indent(gen);
                                            fprintf(gen->output, "}\n");
                                            print_indent(gen);
                                            fprintf(gen->output, "_aether_ctx_pop();\n");
                                            // Reassign variable with defer config
                                            print_indent(gen);
                                            char c_dfn[256];
                                            strncpy(c_dfn, safe_c_name(init_call->value), sizeof(c_dfn) - 1);
                                            c_dfn[sizeof(c_dfn) - 1] = '\0';
                                            for (char* p = c_dfn; *p; p++) { if (*p == '.') *p = '_'; }
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
                                            print_indent(gen);
                                            fprintf(gen->output, "{\n");
                                            gen->indent_level++;
                                            gen->in_trailing_block++;
                                            ASTNode* body = trailing->children[bi];
                                            for (int si = 0; si < body->child_count; si++) {
                                                generate_statement(gen, body->children[si]);
                                            }
                                            gen->in_trailing_block--;
                                            gen->indent_level--;
                                            print_indent(gen);
                                            fprintf(gen->output, "}\n");
                                            print_indent(gen);
                                            fprintf(gen->output, "_aether_ctx_pop();\n");
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
                    if (rhs && rhs->type == AST_HEAP_NEW)
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

                /* #891 @c_struct overlay write (incl. nested `s.a.b = v`):
                 * lowers to a width-correct aether_mem_set_<width> at the
                 * cumulative offset (no C `->field =`). */
                if (lhs && lhs->type == AST_MEMBER_ACCESS &&
                    aether_c_struct_overlay_lhs(lhs)) {
                    char cpath[256];
                    ASTNode* root = aether_c_struct_chain(lhs, cpath, sizeof(cpath));
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
                                        print_indent(gen);
                                        fprintf(gen->output, "{\n");
                                        gen->indent_level++;
                                        gen->in_trailing_block++;
                                        ASTNode* body = trailing->children[bi];
                                        for (int si = 0; si < body->child_count; si++) {
                                            generate_statement(gen, body->children[si]);
                                        }
                                        gen->in_trailing_block--;
                                        gen->indent_level--;
                                        print_indent(gen);
                                        fprintf(gen->output, "}\n");
                                        print_indent(gen);
                                        fprintf(gen->output, "_aether_ctx_pop();\n");
                                        // Reassign with config
                                        print_indent(gen);
                                        gen->generating_lvalue = 1;
                                        generate_expression(gen, lhs);
                                        gen->generating_lvalue = 0;
                                        char c_fn[256];
                                        strncpy(c_fn, safe_c_name(rhs->value), sizeof(c_fn) - 1);
                                        c_fn[sizeof(c_fn) - 1] = '\0';
                                        for (char* p = c_fn; *p; p++) { if (*p == '.') *p = '_'; }
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
                                        print_indent(gen);
                                        fprintf(gen->output, "{\n");
                                        gen->indent_level++;
                                        gen->in_trailing_block++;
                                        ASTNode* body = trailing->children[bi];
                                        for (int si = 0; si < body->child_count; si++) {
                                            generate_statement(gen, body->children[si]);
                                        }
                                        gen->in_trailing_block--;
                                        gen->indent_level--;
                                        print_indent(gen);
                                        fprintf(gen->output, "}\n");
                                        print_indent(gen);
                                        fprintf(gen->output, "_aether_ctx_pop();\n");
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

                if (is_state_var) {
                    fprintf(gen->output, "self->%s %s ", stmt->value, op);
                } else {
                    fprintf(gen->output, "%s %s ", stmt->value, op);
                }
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
            if (stmt->child_count > 0) {
                gen->in_condition = 1;
                generate_expression(gen, stmt->children[0]);
                gen->in_condition = 0;
            }
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
                print_line(gen, "if (aether_caps_deadline_tripped()) { __aether_abort_call(); break; }");
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

            fprintf(gen->output, "while (");
            if (stmt->child_count > 0) {
                gen->in_condition = 1;
                generate_expression(gen, stmt->children[0]);
                gen->in_condition = 0;
            }
            fprintf(gen->output, ") {\n");

            indent(gen);
            // Cooperative preemption: yield to OS at loop back-edges
            if (gen->preempt_loops) {
                print_line(gen, "if (--_aether_reductions <= 0) { _aether_reductions = 10000; sched_yield(); }");
            }
            // Issue #343 codegen tripwire — see AST_FOR_LOOP comment.
            if (gen->emit_lib) {
                print_line(gen, "if (aether_caps_deadline_tripped()) { __aether_abort_call(); break; }");
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
                gen->loop_nest_depth++;
            }
            if (stmt->child_count > 1) {
                generate_statement(gen, stmt->children[1]);
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
                    char inner_c[256];
                    snprintf(inner_c, sizeof(inner_c), "%s", inner ? get_c_type(inner) : "int");
                    char oc[256];
                    snprintf(oc, sizeof(oc), "%s", get_c_type(match_expr->node_type));
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
                        else if (mexpr_type->kind == TYPE_LONGDOUBLE)
                            match_c_type = "long double";
                        else if (mexpr_type->kind == TYPE_INT64)
                            match_c_type = "int64_t";
                        else if (mexpr_type->kind == TYPE_BOOL)
                            match_c_type = "bool";
                    }
                    print_indent(gen);
                    fprintf(gen->output, "%s _match_val = ", match_c_type);
                    generate_expression(gen, match_expr);
                    fprintf(gen->output, ";\n");
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
                        for (int j = 0; j < result->child_count; j++) {
                            generate_statement(gen, result->children[j]);
                        }
                    } else if (result->type == AST_PRINT_STATEMENT
                            || result->type == AST_RETURN_STATEMENT
                            || result->type == AST_VARIABLE_DECLARATION) {
                        // Statement-level node (e.g. print, return)
                        generate_statement(gen, result);
                    } else {
                        // Single expression — assign to result var or emit as statement
                        print_indent(gen);
                        if (gen->match_result_var) {
                            fprintf(gen->output, "%s = ", gen->match_result_var);
                        }
                        generate_expression(gen, result);
                        fprintf(gen->output, ";\n");
                    }
                    unindent(gen);
                    print_line(gen, "}");
                }

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

            if (!needs_chain) {
                fprintf(gen->output, "switch (");
                if (stmt->child_count > 0) generate_expression(gen, stmt->children[0]);
                fprintf(gen->output, ") {\n");
                indent(gen);
                for (int i = 1; i < stmt->child_count; i++)
                    generate_statement(gen, stmt->children[i]);
                unindent(gen);
                print_line(gen, "}");
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
                if (!ct && m->child_count >= 2) {
                    ASTNode* first_arm = m->children[1];
                    if (first_arm && first_arm->child_count >= 2 &&
                        first_arm->children[1] && first_arm->children[1]->node_type)
                        ct = get_c_type(first_arm->children[1]->node_type);
                }
                if (!ct) ct = "int";
                static int ret_match_ctr = 0;
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "_ret_match%d", ret_match_ctr++);
                print_indent(gen);
                fprintf(gen->output, "%s %s;\n", ct, tmp);
                const char* saved = gen->match_result_var;
                gen->match_result_var = tmp;
                generate_statement(gen, m);
                gen->match_result_var = saved;
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
                    fprintf(gen->output, "%s _builder_ret = (%s){", tname, tname);
                    for (int j = 0; j < stmt->child_count; j++) {
                        if (j > 0) fprintf(gen->output, ", ");
                        emit_tuple_return_position(gen, stmt->children[j], j);
                    }
                    fprintf(gen->output, "};\n");
                    if (owned) free_type(tuple);
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
                    break;
                }
                // For return with value, save to temp first
                if (stmt->child_count > 0 && stmt->children[0] &&
                    stmt->children[0]->type != AST_PRINT_STATEMENT) {
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
                    // Bug B suppression: any closure whose env is still live
                    // through the returned value (directly or transitively via
                    // another closure capturing it) must not have its env-free
                    // defer run — the caller now owns the env.
                    char** protected_names = NULL;
                    int protected_count = 0, protected_cap = 0;
                    collect_returned_closures(gen, stmt->children[0],
                                              &protected_names, &protected_count, &protected_cap);
                    // Transitive closure-of-captures fixpoint: if bump
                    // escapes and bump captures digit, digit's captures
                    // must also be protected. collect_returned_closures
                    // only handled the first hop; iterate until stable.
                    int scan_idx = 0;
                    while (scan_idx < protected_count) {
                        int start_count = protected_count;
                        for (int i = scan_idx; i < start_count; i++) {
                            const char* name = protected_names[i];
                            if (!name) continue;
                            int cid;
                            if (!lookup_closure_var(gen, name, &cid)) continue;
                            for (int ci = 0; ci < gen->closure_count; ci++) {
                                if (gen->closures[ci].id != cid) continue;
                                const char* pfn = gen->closures[ci].parent_func;
                                char** promoted = NULL;
                                int promoted_count = 0;
                                get_promoted_names_for_func(gen, pfn, &promoted, &promoted_count);
                                for (int k = 0; k < gen->closures[ci].capture_count; k++) {
                                    const char* cap_name = gen->closures[ci].captures[k];
                                    if (!cap_name) continue;
                                    if (lookup_closure_var(gen, cap_name, NULL)) {
                                        add_protected_name(&protected_names, &protected_count,
                                                           &protected_cap, cap_name);
                                    }
                                    for (int pp = 0; pp < promoted_count; pp++) {
                                        if (promoted[pp] && strcmp(promoted[pp], cap_name) == 0) {
                                            add_protected_name(&protected_names, &protected_count,
                                                               &protected_cap, cap_name);
                                            break;
                                        }
                                    }
                                }
                                break;
                            }
                        }
                        scan_idx = start_count;
                    }
                    /* #752: a directly-returned struct escapes — suppress
                     * its exit-time destroy (heap-string fields now owned
                     * by the caller). */
                    mark_returned_struct_escaped(gen, stmt->children[0]);
                    /* #1140: a single-value `return v` is a SUCCESS exit — in a
                     * `T!` function it is wrapped as `{._0 = v, ._1 = ""}`, and
                     * in an ordinary function there is no error channel at all.
                     * Either way `defer try` fires and `defer catch` does not,
                     * and it is known statically, so no runtime guard. */
                    {
                        DeferExit prev_exit = gen->defer_exit;
                        gen->defer_exit = DEFER_EXIT_SUCCESS;
                        emit_all_defers_protected(gen, protected_names, protected_count);
                        gen->defer_exit = prev_exit;
                    }
                    for (int p = 0; p < protected_count; p++) free(protected_names[p]);
                    free(protected_names);
                    /* Issue #501: drain try frames. */
                    emit_try_pops_for_nonlocal_exit(gen);
                    print_line(gen, "return _builder_ret;");
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
                    fprintf(gen->output, "return (%s){", tname);
                    for (int j = 0; j < stmt->child_count; j++) {
                        if (j > 0) fprintf(gen->output, ", ");
                        emit_tuple_return_position(gen, stmt->children[j], j);
                    }
                    fprintf(gen->output, "};\n");
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
                // Emit defers for current scope before break
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
                print_line(gen, "goto __ae_cont_%d;", gen->loop_label_id[lvl]);
            } else {
                // Emit defers for current scope before continue
                emit_defers_for_scope(gen);
                /* Issue #501: drain inside-loop try frames. */
                emit_try_pops_for_break_continue(gen);
                print_line(gen, "continue;");
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
            print_line(gen, "const char* %s = _aether_try_%d->reason ? _aether_try_%d->reason : \"panic\";",
                      catch_clause->value, uid, uid);
            print_line(gen, "aether_try_pop();");
            generate_statement(gen, catch_clause->children[0]);
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
            print_indent(gen);
            fprintf(gen->output, "aether_panic(");
            generate_expression(gen, stmt->children[0]);
            fprintf(gen->output, ");\n");
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
                                    print_indent(gen);
                                    fprintf(gen->output, "{\n");
                                    gen->indent_level++;
                                    gen->in_trailing_block++;
                                    ASTNode* body = trailing->children[bi];
                                    for (int si = 0; si < body->child_count; si++) {
                                        generate_statement(gen, body->children[si]);
                                    }
                                    gen->in_trailing_block--;
                                    gen->indent_level--;
                                    print_indent(gen);
                                    fprintf(gen->output, "}\n");
                                    break;
                                }
                            }
                        }
                    }

                    // 3. Pop context
                    print_indent(gen);
                    fprintf(gen->output, "_aether_ctx_pop();\n");

                    // 4. Call function with config as extra last arg
                    print_indent(gen);
                    char c_builder_name[256];
                    strncpy(c_builder_name, safe_c_name(inner->value), sizeof(c_builder_name) - 1);
                    c_builder_name[sizeof(c_builder_name) - 1] = '\0';
                    for (char* p = c_builder_name; *p; p++) { if (*p == '.') *p = '_'; }
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
                        gen->discard_call_value = 1;
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
                     * escaping — fail-safe direction (leak ≫ UAF). When
                     * the future `@retains` annotation lands, opt-in
                     * non-escaping externs can re-enable the drain. */
                    ASTNode* cclos = NULL; int cclos_idx = -1;
                    if (inner && inner->type == AST_FUNCTION_CALL && inner->value) {
                        for (int ai = 0; ai < inner->child_count; ai++) {
                            ASTNode* a = inner->children[ai];
                            if (a && a->type == AST_CLOSURE &&
                                !(a->value && strcmp(a->value, "trailing") == 0)) {
                                cclos = a; cclos_idx = ai; break;
                            }
                        }
                    }
                    /* Map AST arg index → function-def param index. When the
                     * callee is a `_ctx: ptr` builder and the user omitted
                     * `_ctx`, codegen auto-injects `_aether_ctx_get()` at
                     * position 0; the AST args are then shifted left by one
                     * relative to the callee's declared params. Without this
                     * shift the escape walk checks the wrong param (e.g. the
                     * label, which provably doesn't escape) and the drain
                     * fires under a false-non-escape verdict → UAF. */
                    int param_idx = cclos_idx;
                    if (cclos && cclos_idx >= 0 && inner && inner->value) {
                        /* Look up the callee by its normalised (dot→underscore)
                         * name — that's how cross-module functions land in the
                         * merged program AST. Without normalisation an imported
                         * `aether_ui.btn` call site looks up "aether_ui.btn"
                         * but the merged AST has node value "aether_ui_btn",
                         * the lookup misses, the `_ctx`-injection shift is
                         * skipped, and the escape walk checks the wrong param
                         * (label, read-only → false non-escape → UAF). */
                        char fn_norm[256];
                        const char* fn = codegen_normalise_callee(inner->value, fn_norm, sizeof(fn_norm));
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
                            for (int ai = 0; ai < inner->child_count; ai++) {
                                ASTNode* a = inner->children[ai];
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
                    }
                    if (cclos && cclos_idx >= 0 &&
                        callee_has_visible_body(gen, inner->value) &&
                        callee_param_escapes_via_body(gen, inner->value,
                                                      param_idx, 0) == 0) {
                        emit_closure_env_drained_call(gen, inner, cclos);
                        fprintf(gen->output, "\n");
                    } else {
                        generate_expression(gen, inner);
                        fprintf(gen->output, ";\n");
                    }
                    gen->discard_call_value = 0;
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
                                    print_indent(gen);
                                    fprintf(gen->output, "{\n");
                                    gen->indent_level++;
                                    gen->in_trailing_block++;
                                    ASTNode* body = trailing->children[bi];
                                    for (int si = 0; si < body->child_count; si++) {
                                        generate_statement(gen, body->children[si]);
                                    }
                                    gen->in_trailing_block--;
                                    gen->indent_level--;
                                    print_indent(gen);
                                    fprintf(gen->output, "}\n");

                                    // Pop the builder context
                                    if (has_trailing) {
                                        print_indent(gen);
                                        fprintf(gen->output, "_aether_ctx_pop();\n");
                                    }
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

                // Interpolated string: delegate directly to expression codegen (emits printf(...))
                if (stmt->child_count == 1 && first_arg->type == AST_STRING_INTERP) {
                    gen->interp_as_printf = 1;
                    generate_expression(gen, first_arg);
                    gen->interp_as_printf = 0;
                    fprintf(gen->output, ";\n");
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
                    } else if (arg_type->kind == TYPE_FLOAT) {
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
                            // NULL-safe via helper (no double-evaluation)
                            fprintf(gen->output, "printf(\"%%s\", _aether_safe_str(");
                            generate_expression(gen, first_arg);
                            fprintf(gen->output, "));\n");
                        }
                    } else if (arg_type->kind == TYPE_BOOL) {
                        fprintf(gen->output, "printf(\"%%s\", ");
                        generate_expression(gen, first_arg);
                        fprintf(gen->output, " ? \"true\" : \"false\");\n");
                    } else if (arg_type->kind == TYPE_INT64) {
                        fprintf(gen->output, "printf(\"%%lld\", (long long)");
                        generate_expression(gen, first_arg);
                        fprintf(gen->output, ");\n");
                    } else if (arg_type->kind == TYPE_DURATION) {
                        fprintf(gen->output, "printf(\"%%s\", _aether_duration_repr(");
                        generate_expression(gen, first_arg);
                        fprintf(gen->output, "));\n");
                    } else if (arg_type->kind == TYPE_PTR) {
                        // NULL-safe via helper (no double-evaluation)
                        fprintf(gen->output, "printf(\"%%s\", _aether_safe_str(");
                        generate_expression(gen, first_arg);
                        fprintf(gen->output, "));\n");
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
                        fprintf(gen->output, "printf(");
                        generate_expression(gen, arg);
                        fprintf(gen->output, ");\n");
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
                                    } else if (atype && atype->kind == TYPE_FLOAT) {
                                        fprintf(gen->output, "%%f");
                                    } else if (atype && atype->kind == TYPE_INT64) {
                                        fprintf(gen->output, "%%lld");
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
                                fprintf(gen->output,
                                        "if (_reply.%s) { "
                                        "size_t _ml = aether_string_length(_reply.%s); "
                                        "_reply.%s = (const char*)string_new_with_length("
                                        "aether_string_data(_reply.%s), (int)_ml); "
                                        "} ",
                                        f->name, f->name, f->name, f->name);
                            }
                        }

                        // Send reply back to the waiting asker via the scheduler reply slot.
                        fprintf(gen->output, "scheduler_reply((ActorBase*)self, &_reply, sizeof(%s)); }\n",
                                reply_expr->value);
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
                    if (k == TYPE_STRING) {
                        /* Deep-copy so the asker's read outlives the
                         * handler's defer-free, same contract as
                         * message-field string replies (#466). */
                        fprintf(gen->output, "{ const char* _reply_val = ");
                        generate_expression(gen, reply_expr);
                        fprintf(gen->output,
                                "; if (_reply_val) { "
                                "size_t _ml = aether_string_length(_reply_val); "
                                "_reply_val = (const char*)string_new_with_length("
                                "aether_string_data(_reply_val), (int)_ml); } "
                                "scheduler_reply((ActorBase*)self, &_reply_val, "
                                "sizeof(const char*)); }\n");
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
}
