#include "optimizer.h"
#include "../aether_error.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <errno.h>

OptimizationStats global_opt_stats = {0, 0, 0, 0, 0};

void reset_optimization_stats() {
    global_opt_stats.constants_folded = 0;
    global_opt_stats.dead_code_removed = 0;
    global_opt_stats.tail_calls_detected = 0;
    global_opt_stats.series_loops_collapsed = 0;
    global_opt_stats.linear_loops_collapsed = 0;
}

void print_optimization_stats() {
    printf("Optimization Statistics:\n");
    printf("  Constants folded: %d\n", global_opt_stats.constants_folded);
    printf("  Dead code removed: %d\n", global_opt_stats.dead_code_removed);
    printf("  Tail calls detected: %d (optimized by C backend at -O2)\n", global_opt_stats.tail_calls_detected);
    printf("  Series loops collapsed: %d\n", global_opt_stats.series_loops_collapsed);
    printf("  Linear loops collapsed: %d\n", global_opt_stats.linear_loops_collapsed);
}

// Helper: check if node is a numeric literal constant (not string/bool)
static int is_constant(ASTNode* node) {
    if (!node || node->type != AST_LITERAL || !node->value) return 0;
    // Exclude booleans and strings from numeric constant folding
    if (node->node_type) {
        if (node->node_type->kind == TYPE_STRING) return 0;
        if (node->node_type->kind == TYPE_BOOL) return 0;
    }
    // Also check by value for untyped literals
    if (strcmp(node->value, "true") == 0 || strcmp(node->value, "false") == 0) return 0;
    return 1;
}

// Helper: get numeric value from literal
static double get_constant_value(ASTNode* node) {
    if (!node || !node->value) return 0.0;
    // Handle boolean literals: "true" = 1.0, "false" = 0.0
    if (strcmp(node->value, "true") == 0) return 1.0;
    if (strcmp(node->value, "false") == 0) return 0.0;
    return atof(node->value);
}

// Helper: create a literal node with a numeric value
static ASTNode* create_numeric_literal(double value, int is_int, int line, int column) {
    char buffer[64];
    if (is_int && value == (double)(long long)value) {
        snprintf(buffer, sizeof(buffer), "%lld", (long long)value);
    } else {
        snprintf(buffer, sizeof(buffer), "%.10g", value);
    }
    ASTNode* node = create_ast_node(AST_LITERAL, buffer, line, column);
    node->node_type = create_type(is_int ? TYPE_INT : TYPE_FLOAT);
    return node;
}

// Constant folding for binary expressions
static ASTNode* fold_binary_expression(ASTNode* node) {
    if (!node || node->type != AST_BINARY_EXPRESSION) return node;
    if (node->child_count < 2) return node;
    
    ASTNode* left = node->children[0];
    ASTNode* right = node->children[1];
    
    // Recursively fold children first
    left = optimize_constant_folding(left);
    right = optimize_constant_folding(right);
    node->children[0] = left;
    node->children[1] = right;
    
    // If both operands are constants, fold the expression
    if (is_constant(left) && is_constant(right)) {
        double left_val = get_constant_value(left);
        double right_val = get_constant_value(right);
        double result = 0.0;
        int can_fold = 1;
        
        const char* op = node->value;
        if (strcmp(op, "+") == 0) {
            result = left_val + right_val;
        } else if (strcmp(op, "-") == 0) {
            result = left_val - right_val;
        } else if (strcmp(op, "*") == 0) {
            result = left_val * right_val;
        } else if (strcmp(op, "/") == 0) {
            if (right_val != 0.0) {
                result = left_val / right_val;
            } else {
                can_fold = 0; // Division by zero, can't fold
            }
        } else if (strcmp(op, "%") == 0) {
            if (right_val != 0.0) {
                result = fmod(left_val, right_val);
            } else {
                can_fold = 0;
            }
        } else {
            can_fold = 0; // Unknown operator
        }
        
        if (can_fold) {
            global_opt_stats.constants_folded++;
            // Preserve integer type if both operands are integers
            int both_int = (left->node_type && left->node_type->kind == TYPE_INT) &&
                           (right->node_type && right->node_type->kind == TYPE_INT);
            // When both operands are int, use C integer division semantics (truncate)
            // so that 10/3 = 3, not 3.333... — avoids %d format mismatch warning
            if (both_int) {
                long long exact = (long long)result;
                /* The fold runs in double, which represents values far
                 * beyond 32 bits exactly, but the emitted C computes the
                 * same expression in `int`. Left alone, the two disagree:
                 * `(250000000 - 200000000) * 50 / 225000000` folded to 11
                 * while the identical expression over int VARIABLES
                 * evaluated to -7 at runtime. That gap is what let the
                 * benchmark runner's CV overflow hide during development
                 * (the literal form looked right).
                 *
                 * Wrap to the value the runtime actually produces, so
                 * constant folding is semantics-preserving, and warn:
                 * an int expression whose true value does not fit is a
                 * bug essentially every time, and here the compiler can
                 * prove it. Widen a term to `long` to keep the value. */
                int wrapped = (int)exact;
                if ((long long)wrapped != exact) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "int constant expression overflows 32 bits: the exact value is "
                             "%lld, which wraps to %d at runtime. Widen a term to `long` "
                             "(e.g. `long x = a` then `x * b`) to keep the full value.",
                             exact, wrapped);
                    AetherError w = {NULL, NULL, node->line, node->column, msg,
                                     NULL, NULL, AETHER_WARN_CONST_OVERFLOW};
                    aether_warning_report(&w);
                }
                result = (double)wrapped;
            }
            ASTNode* folded = create_numeric_literal(result, both_int, node->line, node->column);
            
            // Free old node (but not the original structure, return new one)
            free(node->value);
            free_type(node->node_type);
            free(node->children);
            free(node);
            
            return folded;
        }
    }
    
    return node;
}

/* ---------------------------------------------------------------------------
 * Compile-time constant evaluation, phase-1 (issue #482)
 *
 * A HARD-WHITELISTED folder — emphatically NOT a general interpreter. A
 * general compile-time evaluator could synthesize std.fs / std.net calls
 * that slip past the `--emit=lib` capability gate, so only the explicit
 * whitelist below is ever folded. Everything else is left untouched here
 * and rejected upstream by `is_const_expression` in the typechecker.
 *
 * The whitelist (mirrors the typechecker's `is_const_expr_call`):
 *   string.from_int(<int const>)    -> decimal literal string
 *   string.from_long(<int const>)   -> decimal literal string
 *   string.from_float(<num const>)  -> %g literal string
 *   string.concat(<str const>, <str const>) -> concatenated literal string
 *
 * Arithmetic (+ - * / %) on numeric literals is folded by
 * fold_binary_expression above and predates this phase.
 * ------------------------------------------------------------------------- */

/* True when `name` is the dotted source spelling OR the underscored
 * C-symbol spelling of one of the whitelisted pure conversions. The
 * parser stores dotted callees (`string.from_int`); imported/merged
 * callees can arrive underscored (`string_from_int`), so accept both. */
static int is_whitelisted_string_call(const char* name, const char* dotted) {
    if (!name) return 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "string.%s", dotted);
    if (strcmp(name, buf) == 0) return 1;
    snprintf(buf, sizeof(buf), "string_%s", dotted);
    if (strcmp(name, buf) == 0) return 1;
    return 0;
}

// Helper: create a string literal node (value carries the raw bytes; the
// codegen adds the surrounding quotes + escapes on emit).
static ASTNode* create_string_literal(const char* value, int line, int column) {
    ASTNode* node = create_ast_node(AST_LITERAL, value, line, column);
    if (node) node->node_type = create_type(TYPE_STRING);
    return node;
}

// True when `node` is a string literal we can read the bytes of.
static int is_string_literal(ASTNode* node) {
    return node && node->type == AST_LITERAL && node->value &&
           node->node_type && node->node_type->kind == TYPE_STRING;
}

/* Fold a whitelisted pure-conversion call on constant arguments into a
 * string literal. Returns the folded literal (and frees `node`) on
 * success, or `node` unchanged when the call is not in the whitelist /
 * its arguments are not constants / the value would not be representable.
 *
 * Width discipline: from_int folds in 32-bit, from_long in 64-bit. An
 * out-of-range argument for the chosen width is NOT folded — it is left
 * as the runtime call so behaviour is unchanged (no silently-wrong
 * literal). */
static ASTNode* fold_const_string_call(ASTNode* node) {
    if (!node || node->type != AST_FUNCTION_CALL || !node->value) return node;

    // string.from_int / string.from_long — one numeric-literal argument.
    int is_from_int  = is_whitelisted_string_call(node->value, "from_int");
    int is_from_long = is_whitelisted_string_call(node->value, "from_long");
    int is_from_flt  = is_whitelisted_string_call(node->value, "from_float");

    if ((is_from_int || is_from_long || is_from_flt) && node->child_count == 1) {
        ASTNode* arg = node->children[0];
        if (!is_constant(arg) || !arg->value) return node;  // non-literal arg: leave runtime call
        char buf[64];
        if (is_from_flt) {
            double v = get_constant_value(arg);
            snprintf(buf, sizeof(buf), "%g", v);
        } else {
            /* Parse the integer literal text directly (not through the
             * lossy double in get_constant_value) so 64-bit values past
             * 2^53 fold exactly. strtoll handles decimal and 0x; 0b / 0o
             * are uncommon as conversion args, so a literal we cannot
             * parse exactly is left as the runtime call. */
            errno = 0;
            char* end = NULL;
            const char* s = arg->value;
            long long v;
            if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
                v = strtoll(s + 2, &end, 2);
            } else if (s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) {
                v = strtoll(s + 2, &end, 8);
            } else {
                v = strtoll(s, &end, 0);  // decimal / 0x
            }
            if (errno != 0 || !end || *end != '\0') return node;  // unparseable / overflow
            if (is_from_int) {
                // 32-bit width: an out-of-range value would print a
                // different string than the runtime int truncation, so
                // leave it as the runtime call rather than fold wrongly.
                if (v < -2147483647LL - 1 || v > 2147483647LL) return node;
                snprintf(buf, sizeof(buf), "%d", (int)v);
            } else {
                snprintf(buf, sizeof(buf), "%lld", v);
            }
        }
        ASTNode* folded = create_string_literal(buf, node->line, node->column);
        if (!folded) return node;
        global_opt_stats.constants_folded++;
        free_ast_node(node);
        return folded;
    }

    // string.concat(a, b) — two string-literal arguments fold to one literal.
    if (is_whitelisted_string_call(node->value, "concat") && node->child_count == 2) {
        ASTNode* a = node->children[0];
        ASTNode* b = node->children[1];
        if (!is_string_literal(a) || !is_string_literal(b)) return node;
        size_t la = strlen(a->value), lb = strlen(b->value);
        char* joined = (char*)malloc(la + lb + 1);
        if (!joined) return node;
        memcpy(joined, a->value, la);
        memcpy(joined + la, b->value, lb);
        joined[la + lb] = '\0';
        ASTNode* folded = create_string_literal(joined, node->line, node->column);
        free(joined);
        if (!folded) return node;
        global_opt_stats.constants_folded++;
        free_ast_node(node);
        return folded;
    }

    return node;
}

// Constant folding main function
ASTNode* optimize_constant_folding(ASTNode* node) {
    if (!node) return NULL;

    // Handle binary expressions
    if (node->type == AST_BINARY_EXPRESSION) {
        return fold_binary_expression(node);
    }

    // Recursively optimize children FIRST so nested constants (and
    // string-literal arguments produced by an inner fold) are available
    // before we try to fold this node.
    for (int i = 0; i < node->child_count; i++) {
        node->children[i] = optimize_constant_folding(node->children[i]);
    }

    // Whitelisted pure-conversion calls fold to a string literal once
    // their arguments are themselves constants.
    if (node->type == AST_FUNCTION_CALL) {
        return fold_const_string_call(node);
    }

    /* A `const` whose RHS folded to a string literal must have its
     * declared type updated to `string`: the parser inferred the type
     * from the original conversion call (which returns `ptr`), but the
     * folded RHS is now a literal string. The `#define`/local emit keys
     * off this type for the heap-string vs. literal decision. */
    if (node->type == AST_CONST_DECLARATION && node->child_count > 0 &&
        is_string_literal(node->children[0])) {
        int is_array = node->annotation &&
                       strcmp(node->annotation, "array_const") == 0;
        if (!is_array &&
            (!node->node_type || node->node_type->kind != TYPE_STRING)) {
            if (node->node_type) free_type(node->node_type);
            node->node_type = create_type(TYPE_STRING);
        }
    }

    return node;
}

// Helper: check if condition is a compile-time known constant (numeric or boolean)
static int is_constant_condition(ASTNode* node, int* is_truthy) {
    if (!node || !node->value) return 0;
    if (node->type == AST_LITERAL) {
        // Boolean literals
        if (strcmp(node->value, "true") == 0) { *is_truthy = 1; return 1; }
        if (strcmp(node->value, "false") == 0) { *is_truthy = 0; return 1; }
        // Numeric literals (exclude strings)
        if (node->node_type && node->node_type->kind == TYPE_STRING) return 0;
        double val = atof(node->value);
        *is_truthy = (val != 0.0);
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Compile-time `when` / static-if resolution (issue #483)
//
// A `when COND { ... } else { ... }` is evaluated entirely at compile time:
// only the selected arm survives into type-checking and codegen. The dead
// arm parses but is pruned here, BEFORE typecheck runs (called from aetherc.c
// between parse and typecheck), so a platform-specific extern in the dead arm
// is never resolved, type-checked, or emitted.
//
// `target.os` / `target.arch` are compiler-provided compile-time string
// constants for the build target. They are sourced from the SAME C
// preprocessor macros the runtime's os.platform() uses (std/os/aether_os.c
// os_platform_raw), so the canonical names match across the toolchain:
// os ∈ {"windows","darwin","linux","freebsd","openbsd","netbsd",
// "dragonfly","solaris","wasm","unknown"}; arch ∈ {"x86_64","aarch64",
// "x86","arm","riscv64","ppc64","wasm","unknown"}. The target is the host
// (aetherc emits C compiled by the host toolchain; there is no cross-target
// flag today), exactly as os.platform() and select() already assume.
// ---------------------------------------------------------------------------

static const char* target_os_string(void) {
#if defined(_WIN32) || defined(_WIN64)
    return "windows";
#elif defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#elif defined(__FreeBSD__)
    return "freebsd";
#elif defined(__OpenBSD__)
    return "openbsd";
#elif defined(__NetBSD__)
    return "netbsd";
#elif defined(__DragonFly__)
    return "dragonfly";
#elif defined(__sun) && (defined(__SVR4) || defined(__svr4__))
    return "solaris";
#elif defined(__EMSCRIPTEN__) || defined(__wasi__) || defined(__wasm__)
    return "wasm";
#else
    return "unknown";
#endif
}

static const char* target_arch_string(void) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__arm64__)
    return "aarch64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#elif defined(__riscv) && (__riscv_xlen == 64)
    return "riscv64";
#elif defined(__powerpc64__) || defined(__ppc64__)
    return "ppc64";
#elif defined(__EMSCRIPTEN__) || defined(__wasi__) || defined(__wasm__)
    return "wasm";
#else
    return "unknown";
#endif
}

// If `node` is the compiler-provided `target.os` / `target.arch` member
// access, return its compile-time string value; otherwise NULL.
static const char* when_target_field_value(ASTNode* node) {
    if (!node || node->type != AST_MEMBER_ACCESS || !node->value) return NULL;
    if (node->child_count < 1 || !node->children[0]) return NULL;
    ASTNode* base = node->children[0];
    if (base->type != AST_IDENTIFIER || !base->value) return NULL;
    if (strcmp(base->value, "target") != 0) return NULL;
    if (strcmp(node->value, "os") == 0)   return target_os_string();
    if (strcmp(node->value, "arch") == 0) return target_arch_string();
    return NULL;
}

// Result of evaluating a `when` condition at compile time.
typedef enum {
    WHEN_EVAL_ERROR = 0,   // not a recognised compile-time constant
    WHEN_EVAL_FALSE,
    WHEN_EVAL_TRUE
} WhenEvalResult;

// Evaluate a `when` condition expression to a compile-time boolean.
// Supported forms (composed freely with `&&`, `||`, `!`, and parens):
//   - target.os == "<str>" / target.os != "<str>"
//   - target.arch == "<str>" / target.arch != "<str>"
//   - "<str>" == "<str>" (and !=) between any two constant strings
//   - boolean literals `true` / `false`
//   - a numeric literal (nonzero is true) — lets `when 1 {}` work
// Returns WHEN_EVAL_ERROR for anything not reducible to a constant bool;
// the caller turns that into a clear compile error rather than guessing.
static WhenEvalResult when_eval_condition(ASTNode* cond) {
    if (!cond) return WHEN_EVAL_ERROR;

    // Parenthesised / passthrough wrappers: a unary `!` is the only
    // operator we special-case; otherwise fall through to literal/binary.
    if (cond->type == AST_UNARY_EXPRESSION && cond->value &&
        strcmp(cond->value, "!") == 0 && cond->child_count >= 1) {
        WhenEvalResult inner = when_eval_condition(cond->children[0]);
        if (inner == WHEN_EVAL_ERROR) return WHEN_EVAL_ERROR;
        return inner == WHEN_EVAL_TRUE ? WHEN_EVAL_FALSE : WHEN_EVAL_TRUE;
    }

    if (cond->type == AST_LITERAL && cond->value) {
        if (strcmp(cond->value, "true") == 0) return WHEN_EVAL_TRUE;
        if (strcmp(cond->value, "false") == 0) return WHEN_EVAL_FALSE;
        // A string literal alone is not a boolean condition.
        if (cond->node_type && cond->node_type->kind == TYPE_STRING)
            return WHEN_EVAL_ERROR;
        return atof(cond->value) != 0.0 ? WHEN_EVAL_TRUE : WHEN_EVAL_FALSE;
    }

    if (cond->type == AST_BINARY_EXPRESSION && cond->value &&
        cond->child_count >= 2) {
        ASTNode* l = cond->children[0];
        ASTNode* r = cond->children[1];

        // Logical connectives compose sub-conditions.
        if (strcmp(cond->value, "&&") == 0) {
            WhenEvalResult a = when_eval_condition(l);
            WhenEvalResult b = when_eval_condition(r);
            if (a == WHEN_EVAL_ERROR || b == WHEN_EVAL_ERROR) return WHEN_EVAL_ERROR;
            return (a == WHEN_EVAL_TRUE && b == WHEN_EVAL_TRUE)
                       ? WHEN_EVAL_TRUE : WHEN_EVAL_FALSE;
        }
        if (strcmp(cond->value, "||") == 0) {
            WhenEvalResult a = when_eval_condition(l);
            WhenEvalResult b = when_eval_condition(r);
            if (a == WHEN_EVAL_ERROR || b == WHEN_EVAL_ERROR) return WHEN_EVAL_ERROR;
            return (a == WHEN_EVAL_TRUE || b == WHEN_EVAL_TRUE)
                       ? WHEN_EVAL_TRUE : WHEN_EVAL_FALSE;
        }

        // String equality/inequality. Each side is either `target.os`/
        // `target.arch` or a string literal; both must resolve to a string.
        int is_eq = strcmp(cond->value, "==") == 0;
        int is_ne = strcmp(cond->value, "!=") == 0;
        if (is_eq || is_ne) {
            const char* ls = when_target_field_value(l);
            if (!ls && l->type == AST_LITERAL && l->node_type &&
                l->node_type->kind == TYPE_STRING) ls = l->value;
            const char* rs = when_target_field_value(r);
            if (!rs && r->type == AST_LITERAL && r->node_type &&
                r->node_type->kind == TYPE_STRING) rs = r->value;
            if (ls && rs) {
                int equal = strcmp(ls, rs) == 0;
                int truth = is_eq ? equal : !equal;
                return truth ? WHEN_EVAL_TRUE : WHEN_EVAL_FALSE;
            }
            // Fall back to numeric/bool equality on two constant literals.
            int l_truthy = 0, r_truthy = 0;
            if (is_constant_condition(l, &l_truthy) &&
                is_constant_condition(r, &r_truthy)) {
                int equal = (l_truthy == r_truthy);
                int truth = is_eq ? equal : !equal;
                return truth ? WHEN_EVAL_TRUE : WHEN_EVAL_FALSE;
            }
            return WHEN_EVAL_ERROR;
        }
    }

    return WHEN_EVAL_ERROR;
}

// Tracks whether any `when` failed to resolve, so the driver can fail the
// build with a non-zero status instead of silently dropping code.
static int g_when_error_count = 0;

// Replace the `when` child at parent->children[idx] with the children of its
// selected arm (an AST_BLOCK), spliced in place. Splicing the arm's children
// (rather than nesting the AST_BLOCK) keeps top-level externs/imports at top
// level and avoids introducing a spurious C scope around a statement arm's
// locals — the arm braces are a compile-time grouping, not a runtime block.
// Frees the `when` node and the discarded arm. Returns the number of nodes
// that now occupy the slot (the arm's child count), so the caller can
// continue scanning from the right place.
static int splice_when_selected_arm(ASTNode* parent, int idx, ASTNode* selected_arm) {
    ASTNode* when_node = parent->children[idx];

    // Detach the selected arm so freeing the `when` node doesn't free it.
    int n = selected_arm ? selected_arm->child_count : 0;
    ASTNode** spliced = NULL;
    if (n > 0) {
        spliced = (ASTNode**)malloc(sizeof(ASTNode*) * (size_t)n);
        for (int i = 0; i < n; i++) {
            spliced[i] = selected_arm->children[i];
            selected_arm->children[i] = NULL;  // transfer ownership
        }
        selected_arm->child_count = 0;
    }

    // Free the entire `when` subtree (condition + both arms; the selected
    // arm's children were transferred out above, so they survive).
    free_ast_node(when_node);

    // Rebuild parent->children with the spliced nodes in place of slot idx.
    int old_count = parent->child_count;
    int new_count = old_count - 1 + n;
    ASTNode** rebuilt = (ASTNode**)malloc(sizeof(ASTNode*) * (size_t)(new_count > 0 ? new_count : 1));
    int w = 0;
    for (int i = 0; i < old_count; i++) {
        if (i == idx) {
            for (int j = 0; j < n; j++) rebuilt[w++] = spliced[j];
        } else {
            rebuilt[w++] = parent->children[i];
        }
    }
    free(parent->children);
    free(spliced);
    parent->children = rebuilt;
    parent->child_capacity = 0;
    parent->child_count = new_count;
    return n;
}

// Recursively resolve every `when` statement in the subtree rooted at `node`,
// scanning the children of each node so a `when` can be replaced in place by
// its selected arm's contents. Runs before typecheck; the unselected arm is
// freed and never seen by later phases.
static void resolve_when_in_children(ASTNode* node) {
    if (!node) return;
    for (int i = 0; i < node->child_count; i++) {
        ASTNode* child = node->children[i];
        if (child && child->type == AST_WHEN_STATEMENT && child->child_count >= 2) {
            ASTNode* cond = child->children[0];
            WhenEvalResult r = when_eval_condition(cond);
            if (r == WHEN_EVAL_ERROR) {
                g_when_error_count++;
                fprintf(stderr,
                    "Error: `when` condition at line %d is not a compile-time "
                    "constant.\n"
                    "       Supported forms: target.os == \"<os>\", "
                    "target.os != \"<os>\", target.arch == \"<arch>\",\n"
                    "       a bool literal, and combinations with && / || / !.\n",
                    child->line);
                // Leave the node in place but neutralise it so we don't
                // descend into a half-built arm; it won't survive anyway
                // because the build fails. Skip it.
                continue;
            }
            ASTNode* selected = NULL;
            if (r == WHEN_EVAL_TRUE) {
                selected = child->children[1];
            } else if (child->child_count >= 3) {
                selected = child->children[2];
            }
            // Resolve any nested `when`s inside the selected arm first, so
            // the spliced-in nodes are already fully resolved.
            resolve_when_in_children(selected);
            int spliced = splice_when_selected_arm(node, i, selected);
            // Re-examine the slot we just filled (the first spliced node may
            // itself be a `when` from an `else when` chain wrapper).
            i += spliced - 1;
            if (i < -1) i = -1;
            continue;
        }
        resolve_when_in_children(child);
    }
}

// Public entry: resolve all compile-time `when` statements in `program`.
// Returns 0 on success, non-zero if any `when` condition was not a
// compile-time constant (the driver then aborts the build).
int resolve_when_statements(ASTNode* program) {
    g_when_error_count = 0;
    resolve_when_in_children(program);
    return g_when_error_count;
}

// Dead code elimination
ASTNode* optimize_dead_code(ASTNode* node) {
    if (!node) return NULL;

    // If statement with constant condition
    if (node->type == AST_IF_STATEMENT && node->child_count >= 2) {
        ASTNode* condition = node->children[0];
        int truthy = 0;
        if (is_constant_condition(condition, &truthy)) {
            if (truthy) {
                // Condition is always true, replace with then-branch
                global_opt_stats.dead_code_removed++;
                ASTNode* then_branch = node->children[1];
                
                // Detach then-branch from parent
                for (int i = 1; i < node->child_count - 1; i++) {
                    node->children[i] = node->children[i + 1];
                }
                node->child_count--;
                
                // Free the original if node and return then-branch
                free(node->value);
                free_type(node->node_type);
                free(node->children);
                free(node);
                
                return optimize_dead_code(then_branch);
            } else if (node->child_count >= 3) {
                // Condition is always false, replace with else-branch
                global_opt_stats.dead_code_removed++;
                ASTNode* else_branch = node->children[2];
                
                // Similar detachment and cleanup
                free(node->value);
                free_type(node->node_type);
                free(node->children);
                free(node);
                
                return optimize_dead_code(else_branch);
            } else {
                // No else branch, remove entire if
                global_opt_stats.dead_code_removed++;
                free_ast_node(node);
                return NULL;
            }
        }
    }
    
    // While loop with constant false condition
    if (node->type == AST_WHILE_LOOP && node->child_count >= 1) {
        ASTNode* condition = node->children[0];
        int while_truthy = 0;
        if (is_constant_condition(condition, &while_truthy)) {
            if (!while_truthy) {
                // Loop never executes
                global_opt_stats.dead_code_removed++;
                free_ast_node(node);
                return NULL;
            }
        }
    }
    
    // Recursively optimize children
    int new_count = 0;
    for (int i = 0; i < node->child_count; i++) {
        ASTNode* optimized = optimize_dead_code(node->children[i]);
        if (optimized) {
            node->children[new_count++] = optimized;
        }
    }
    node->child_count = new_count;
    
    return node;
}

// Tail call optimization helper: check if call is in tail position
static int is_tail_call(ASTNode* function, ASTNode* call) {
    if (!call || call->type != AST_FUNCTION_CALL) return 0;
    if (!call->value || !function->value) return 0;
    
    // Check if calling the same function
    return strcmp(call->value, function->value) == 0;
}

// Tail call optimization
ASTNode* optimize_tail_calls(ASTNode* node) {
    if (!node) return NULL;
    
    // Look for function definitions
    if (node->type == AST_FUNCTION_DEFINITION && node->child_count > 0) {
        // Find return statements in the function body
        ASTNode* body = NULL;
        for (int i = 0; i < node->child_count; i++) {
            if (node->children[i] && node->children[i]->type == AST_BLOCK) {
                body = node->children[i];
                break;
            }
        }
        
        if (body && body->child_count > 0) {
            // Check last statement for tail call
            ASTNode* last_stmt = body->children[body->child_count - 1];
            if (!last_stmt) goto recurse_children;

            if (last_stmt->type == AST_RETURN_STATEMENT &&
                last_stmt->child_count > 0 &&
                is_tail_call(node, last_stmt->children[0])) {
                
                global_opt_stats.tail_calls_detected++;
                // GCC/Clang optimize tail calls into loops at -O2.
                // ae build uses -O2, so no source-level transformation needed.
            }
        }
    }
    
recurse_children:
    // Recursively optimize children
    for (int i = 0; i < node->child_count; i++) {
        if (node->children[i]) {
            node->children[i] = optimize_tail_calls(node->children[i]);
        }
    }

    return node;
}

// Apply all optimizations
ASTNode* optimize_ast(ASTNode* node) {
    if (!node) return NULL;
    
    reset_optimization_stats();
    
    // Apply optimizations in order
    node = optimize_constant_folding(node);
    node = optimize_dead_code(node);
    node = optimize_tail_calls(node);
    
    return node;
}

