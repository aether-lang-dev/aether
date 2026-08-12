#include "codegen_internal.h"
#include "../aether_error.h"

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

/* A bare reference to a top-level Aether function, as opposed to a closure or
 * an fn-typed variable. Its address is a real C symbol, which is the only thing
 * a C function-pointer field can hold. */
static ASTNode* bare_top_level_fn(CodeGenerator* gen, ASTNode* node) {
    if (!gen || !gen->program || !node ||
        node->type != AST_IDENTIFIER || !node->value) return NULL;
    for (int i = 0; i < gen->program->child_count; i++) {
        ASTNode* pc = gen->program->children[i];
        if (pc && (pc->type == AST_FUNCTION_DEFINITION ||
                   pc->type == AST_BUILDER_FUNCTION) &&
            pc->value && strcmp(pc->value, node->value) == 0) return pc;
    }
    return NULL;
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
        if (!bigger) { free(name); return; }
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
       string captures own. Falls back to free() only when the closure is not
       in the registry (no captures, hence nothing owned). */
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
        fprintf(gen->output, "; if (%s.env) free((void*)%s.env); }", nm, nm);
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
 *
 * Writes the translated form into `out` (size `out_size`). Returns
 * `out` on translation, or the original `value` if no translation
 * is needed. The previous lexer eagerly decimalised these forms,
 * so this path is new — pre-cherry-pick code never had to worry
 * about it because the literal had been collapsed by the time it
 * reached codegen.
 */
static const char* translate_integer_literal(const char* value, char* out, size_t out_size) {
    if (!value || value[0] != '0' || !value[1]) return value;
    char p = value[1];
    if (p == 'x' || p == 'X') return value;
    if (p == 'o' || p == 'O') {
        /* 0o777 → 0777. A bare '0' is also valid C octal for zero. */
        if (snprintf(out, out_size, "0%s", value + 2) >= (int)out_size) return value;
        return out;
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
        if (snprintf(out, out_size, "0x%llx%s", acc, suffix) >= (int)out_size) return value;
        return out;
    }
    return value;
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
static void collect_identifiers(ASTNode* node, char*** names, int* count, int* cap) {
    if (!node) return;
    if (node->type == AST_IDENTIFIER && node->value) {
        // Check if already in list
        for (int i = 0; i < *count; i++) {
            if (strcmp((*names)[i], node->value) == 0) return;
        }
        if (*count >= *cap) {
            *cap = *cap ? *cap * 2 : 16;
            *names = aether_xrealloc(*names, *cap * sizeof(char*));
        }
        (*names)[(*count)++] = strdup(node->value);
    }
    for (int i = 0; i < node->child_count; i++) {
        collect_identifiers(node->children[i], names, count, cap);
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

// Check if a name is declared as a fresh local inside a block. A statement
// `x = expr` is a fresh-local declaration when `x` was not previously read
// or written in this block and when `expr` does not itself read `x`
// (i.e., `x = x + 1` is a reassignment, not a fresh declaration).
// Only top-level statements of the block are considered — nested blocks
// (if/for/while bodies) have their own scopes.
static int is_local_var(ASTNode* block, const char* name) {
    if (!block || !name) return 0;
    for (int i = 0; i < block->child_count; i++) {
        ASTNode* s = block->children[i];
        if (!s) continue;
        if ((s->type == AST_VARIABLE_DECLARATION || s->type == AST_CONST_DECLARATION) &&
            s->value && strcmp(s->value, name) == 0) {
            // If the initializer reads `name`, this is a reassignment of a
            // captured value, not a fresh local. Otherwise a genuine local.
            int init_reads_self = 0;
            for (int c = 0; c < s->child_count; c++) {
                if (subtree_reads(s->children[c], name)) {
                    init_reads_self = 1;
                    break;
                }
            }
            if (!init_reads_self) return 1;
        }
    }
    return 0;
}

// Find an AST_RECEIVE_ARM anywhere in the program whose synthetic name
// (format `__recv_arm_<pointer>`) matches `func_name`. Returns NULL if
// not found. Used so that actor message handlers — which are effectively
// mini-functions for closure-promotion purposes — can be looked up the
// same way as top-level functions.
static ASTNode* find_receive_arm_by_name(ASTNode* node, const char* func_name) {
    if (!node || !func_name) return NULL;
    if (node->type == AST_RECEIVE_ARM) {
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
// `target`, and write target's ENCLOSING scope name into `out`. Returns 1 on
// success. The scope name uses the same vocabulary as `parent_func`
// everywhere else: a function name, "main", `__recv_arm_<ptr>`, or
// `__closure_<ptr>`.
static int find_enclosing_scope_name(ASTNode* node, const char* scope,
                                     ASTNode* target, char* out, size_t n) {
    if (!node) return 0;
    char here[64];
    const char* child_scope = scope;
    if (node->type == AST_FUNCTION_DEFINITION || node->type == AST_BUILDER_FUNCTION) {
        child_scope = node->value ? node->value : scope;
    } else if (node->type == AST_MAIN_FUNCTION) {
        child_scope = "main";
    } else if (node->type == AST_RECEIVE_ARM) {
        snprintf(here, sizeof(here), "__recv_arm_%p", (void*)node);
        child_scope = here;
    } else if (is_hoisted_closure(node)) {
        closure_scope_name(node, here, sizeof(here));
        child_scope = here;
    }
    for (int i = 0; i < node->child_count; i++) {
        ASTNode* c = node->children[i];
        if (c == target) {
            if (!child_scope) return 0;
            snprintf(out, n, "%s", child_scope);
            return 1;
        }
        if (find_enclosing_scope_name(c, child_scope, target, out, n)) return 1;
    }
    return 0;
}

// Forward declaration — subtree_declares is defined below but used here
// to recurse through trailing-block closures while stopping at real
// closures.
static int subtree_declares(ASTNode* node, const char* var_name);

// Does a block declare `var_name` at its top-level, treating trailing-
// block closures as transparent (their contents inline at the call site)
// but if/for/while blocks and real closures as opaque? Used by
// is_top_level_decl_in_function so that a declaration inside a trailing
// block (e.g. `root = grid() { c = 42 }`) is recognised as living in
// the enclosing function's scope, while a declaration inside
// `if cond { v = ... }` correctly stays block-local.
static int scope_declares_at_top_level(ASTNode* block, const char* var_name) {
    if (!block) return 0;
    for (int k = 0; k < block->child_count; k++) {
        ASTNode* s = block->children[k];
        if (!s) continue;
        if ((s->type == AST_VARIABLE_DECLARATION ||
             s->type == AST_CONST_DECLARATION) &&
            s->value && strcmp(s->value, var_name) == 0) {
            return 1;
        }
        // Var decls whose initializer is a function call with a trailing
        // block: `root = grid() { ... }`. The trailing block's body is
        // part of the enclosing function's scope. Look into it.
        // Same for bare function-call expression statements with trailing
        // blocks.
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
        if (call) {
            for (int ci = 0; ci < call->child_count; ci++) {
                ASTNode* arg = call->children[ci];
                if (arg && arg->type == AST_CLOSURE &&
                    arg->value && strcmp(arg->value, "trailing") == 0) {
                    // Trailing block's body is this closure's (last) AST_BLOCK child.
                    for (int bi = arg->child_count - 1; bi >= 0; bi--) {
                        if (arg->children[bi] && arg->children[bi]->type == AST_BLOCK) {
                            if (scope_declares_at_top_level(arg->children[bi], var_name)) {
                                return 1;
                            }
                            break;
                        }
                    }
                }
            }
        }
    }
    return 0;
}

// Is `var_name` declared at the TOP level of the given function's body
// block (or as a parameter) — i.e. in the function's "own" lexical scope,
// not inside a nested if/for/while block? This is the scope that closures
// capture from in languages like JavaScript and Ruby. Names declared
// inside nested blocks share names only by coincidence and are not
// capture targets.
static int is_top_level_decl_in_function(ASTNode* program, const char* func_name, const char* var_name) {
    if (!program || !func_name || !var_name) return 0;
    // Hoisted closures are scopes too: `__closure_<ptr>`. Their params and
    // top-level body decls are the enclosing scope of any closure nested
    // inside them. A name the outer closure itself captures is also live in
    // its C frame (the `T name = _env->name;` prologue alias), so chain up to
    // the outer closure's own scope for anything not found locally.
    if (strncmp(func_name, "__closure_", 10) == 0) {
        ASTNode* c = find_closure_by_name(program, func_name);
        if (!c) return 0;
        if (is_closure_param(c, var_name)) return 1;
        if (scope_declares_at_top_level(last_block_child(c), var_name)) return 1;
        char outer[64];
        if (find_enclosing_scope_name(program, NULL, c, outer, sizeof(outer))) {
            return is_top_level_decl_in_function(program, outer, var_name);
        }
        return 0;
    }
    // Actor receive arms use synthetic function names `__recv_arm_<ptr>`.
    if (strncmp(func_name, "__recv_arm_", 11) == 0) {
        ASTNode* arm = find_receive_arm_by_name(program, func_name);
        if (!arm) return 0;
        // Arm body is children[1] (children[0] is the pattern).
        if (arm->child_count < 2) return 0;
        ASTNode* body = arm->children[1];
        if (!body || body->type != AST_BLOCK) return 0;
        for (int k = 0; k < body->child_count; k++) {
            ASTNode* s = body->children[k];
            if (s && (s->type == AST_VARIABLE_DECLARATION ||
                      s->type == AST_CONST_DECLARATION) &&
                s->value && strcmp(s->value, var_name) == 0) {
                return 1;
            }
        }
        return 0;
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
        // Parameters count as top-level declarations.
        if (top->type != AST_MAIN_FUNCTION) {
            for (int j = 0; j < top->child_count; j++) {
                ASTNode* p = top->children[j];
                if (p && p->type == AST_PATTERN_VARIABLE && p->value &&
                    strcmp(p->value, var_name) == 0) {
                    return 1;
                }
            }
        }
        // Only TOP-LEVEL statements of the body block count — not nested
        // if/for/while bodies. But trailing-block closures ARE top-level
        // for scoping purposes: they inline at the call site, so a
        // declaration inside a trailing block binds in the enclosing
        // function's scope. Walk through trailing blocks only, not
        // through if/for/while blocks or real closures.
        for (int j = 0; j < top->child_count; j++) {
            ASTNode* body = top->children[j];
            if (!body || body->type != AST_BLOCK) continue;
            if (scope_declares_at_top_level(body, var_name)) return 1;
        }
        return 0;
    }
    return 0;
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
        char outer[64];
        if (find_enclosing_scope_name(program, NULL, c, outer, sizeof(outer))) {
            return is_declared_in_function(program, outer, var_name);
        }
        return 0;
    }
    if (strncmp(func_name, "__recv_arm_", 11) == 0) {
        ASTNode* arm = find_receive_arm_by_name(program, func_name);
        if (!arm || arm->child_count < 2) return 0;
        ASTNode* body = arm->children[1];
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

// Return 1 if any AST_VARIABLE_DECLARATION node under `node` assigns to
// `name` (i.e., appears as its `value`). Used by closure codegen to detect
// which captures are mutated inside the body — those captures cannot use
// the read-only alias prologue and must route writes through _env->.
static int is_assigned_to(ASTNode* node, const char* name) {
    if (!node) return 0;
    if (node->type == AST_VARIABLE_DECLARATION && node->value &&
        strcmp(node->value, name) == 0) {
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
    if (node->type == AST_RECEIVE_ARM) {
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
            if (!is_closure_param(node, all_ids[i]) &&
                !is_builtin_name(all_ids[i]) &&
                !is_local_var(body, all_ids[i])) {
                if (enclosing_func) {
                    is_cap = is_declared_in_function(gen->program, enclosing_func, all_ids[i]);
                } else {
                    is_cap = 1;
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
                // Skip closure params / builtins.
                if (is_closure_param(node, writes[i]) || is_builtin_name(writes[i])) {
                    free(writes[i]);
                    continue;
                }
                // Promote only if the name is declared at the enclosing
                // function's top level (or is one of its parameters).
                if (is_top_level_decl_in_function(gen->program, enclosing_func, writes[i])) {
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
            ASTNode* target_fn = NULL;
            for (int i = 0; i < gen->program->child_count; i++) {
                ASTNode* top = gen->program->children[i];
                if (top && (top->type == AST_FUNCTION_DEFINITION ||
                            top->type == AST_BUILDER_FUNCTION) &&
                    top->value && strcmp(top->value, rhs->value) == 0) {
                    target_fn = top;
                    break;
                }
            }
            if (target_fn) {
                for (int i = 0; i < target_fn->child_count; i++) {
                    ASTNode* body = target_fn->children[i];
                    if (!body || body->type != AST_BLOCK) continue;
                    ASTNode* ret_expr = find_first_return_expr(body);
                    if (ret_expr && ret_expr->type == AST_IDENTIFIER && ret_expr->value) {
                        for (int ci = 0; ci < gen->closure_var_count; ci++) {
                            if (gen->closure_var_map[ci].var_name &&
                                strcmp(gen->closure_var_map[ci].var_name, ret_expr->value) == 0) {
                                cid_to_bind = gen->closure_var_map[ci].closure_id;
                                break;
                            }
                        }
                    }
                    break;
                }
            }
        }
        if (cid_to_bind >= 0) {
            int existing_idx = -1;
            for (int ci = 0; ci < gen->closure_var_count; ci++) {
                if (gen->closure_var_map[ci].var_name &&
                    strcmp(gen->closure_var_map[ci].var_name, node->value) == 0) {
                    existing_idx = ci;
                    break;
                }
            }
            if (existing_idx < 0) {
                if (gen->closure_var_count >= gen->closure_var_capacity) {
                    gen->closure_var_capacity = gen->closure_var_capacity ? gen->closure_var_capacity * 2 : 16;
                    gen->closure_var_map = aether_xrealloc(gen->closure_var_map,
                        gen->closure_var_capacity * sizeof(gen->closure_var_map[0]));
                }
                gen->closure_var_map[gen->closure_var_count].var_name = strdup(node->value);
                gen->closure_var_map[gen->closure_var_count].closure_id = cid_to_bind;
                gen->closure_var_count++;
            } else if (gen->closure_var_map[existing_idx].closure_id != cid_to_bind) {
                // Variable was previously bound to a different closure
                // (either via declaration or via an earlier reassignment).
                // The variable's dynamic identity is no longer a single
                // closure — mark ambiguous so call() falls back to generic
                // function-pointer dispatch through .fn.
                gen->closure_var_map[existing_idx].closure_id = -1;
            }
        }
    }
}

// Resolve call(<closure_var>) to the concrete return type, or NULL.
static Type* resolve_call_type(CodeGenerator* gen, ASTNode* call_expr) {
    if (!call_expr || call_expr->type != AST_FUNCTION_CALL ||
        !call_expr->value || strcmp(call_expr->value, "call") != 0 ||
        call_expr->child_count < 1 || !call_expr->children[0] ||
        call_expr->children[0]->type != AST_IDENTIFIER ||
        !call_expr->children[0]->value) return NULL;
    const char* callee = call_expr->children[0]->value;
    int callee_id = -1;
    for (int ci = 0; ci < gen->closure_var_count; ci++) {
        if (gen->closure_var_map[ci].var_name &&
            strcmp(gen->closure_var_map[ci].var_name, callee) == 0) {
            callee_id = gen->closure_var_map[ci].closure_id;
            break;
        }
    }
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
                                           Type* fn_ret);

static void propagate_call_return_types(CodeGenerator* gen, ASTNode* node) {
    propagate_call_return_types_in(gen, node, NULL);
}

/* `fn_ret` is the declared return type of the function/closure whose body we
 * are inside, or NULL at the top level. It resolves the one case
 * resolve_call_type cannot: `call(f, ...)` where `f` is a `fn` PARAMETER, so
 * there is no closure body to read a type from. In `-> ptr f(...) { return
 * call(f, v) }` the context supplies it. Without this the global `call`
 * symbol's TYPE_INT default reaches codegen, which casts the closure to an
 * int-returning function pointer and truncates a returned pointer. */
static void propagate_call_return_types_in(CodeGenerator* gen, ASTNode* node,
                                           Type* fn_ret) {
    if (!node) return;
    if (node->type == AST_FUNCTION_DEFINITION ||
        node->type == AST_BUILDER_FUNCTION) {
        Type* inner = node->node_type;
        for (int i = 0; i < node->child_count; i++) {
            propagate_call_return_types_in(gen, node->children[i], inner);
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
        for (int i = 0; i < node->child_count; i++) {
            propagate_call_return_types_in(gen, node->children[i], NULL);
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
            !resolve_call_type(gen, r)) {
            r->node_type = clone_type(fn_ret);
        }
    }
    Type* resolved = resolve_call_type(gen, node);
    if (resolved && (!node->node_type || node->node_type->kind != resolved->kind)) {
        node->node_type = clone_type(resolved);
    }
    // Back-propagate into variable declarations whose initializer is a
    // call(<closure_var>) — otherwise the var is declared `int` based on
    // the typechecker's stale default and later casts or format-string
    // selection go wrong.
    if (node->type == AST_VARIABLE_DECLARATION && node->child_count > 0) {
        Type* init_resolved = resolve_call_type(gen, node->children[0]);
        if (init_resolved && (!node->node_type || node->node_type->kind == TYPE_INT)) {
            node->node_type = clone_type(init_resolved);
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        propagate_call_return_types_in(gen, node->children[i], fn_ret);
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
    char scope[64];
    snprintf(scope, sizeof(scope), "%s", start_scope);
    for (;;) {
        add_promoted_name(gen, scope, cap);
        if (strncmp(scope, "__closure_", 10) != 0) return;  // reached a real function
        ASTNode* c = find_closure_by_name(gen->program, scope);
        if (!c) return;
        // The scope that declares the name owns the cell — stop there.
        if (is_closure_param(c, cap)) return;
        if (subtree_declares(last_block_child(c), cap)) return;
        // Otherwise the name is this closure's own capture: keep climbing.
        if (!find_enclosing_scope_name(gen->program, NULL, c, scope, sizeof(scope))) return;
    }
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
            if (is_assigned_to(body, cap)) {
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
            if (!is_assigned_to(body, state_names[n])) continue;
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

// Find the C type of a declaration of `var_name` anywhere in `node`'s subtree,
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
static const char* decl_c_type_in_scope(ASTNode* node, const char* var_name) {
    if (!node) return NULL;
    if (is_hoisted_closure(node)) return NULL;
    if ((node->type == AST_VARIABLE_DECLARATION || node->type == AST_CONST_DECLARATION) &&
        node->value && strcmp(node->value, var_name) == 0) {
        if (node->node_type && node->node_type->kind != TYPE_UNKNOWN) {
            return get_c_type(node->node_type);
        }
        if (node->child_count > 0 && node->children[0] &&
            node->children[0]->node_type &&
            node->children[0]->node_type->kind != TYPE_UNKNOWN) {
            return get_c_type(node->children[0]->node_type);
        }
        // Declaration found but untyped — keep looking; a later re-declaration
        // or assignment of the same name may carry the resolved type.
    }
    for (int i = 0; i < node->child_count; i++) {
        const char* t = decl_c_type_in_scope(node->children[i], var_name);
        if (t) return t;
    }
    return NULL;
}

// Search a single function node for `var_name` as either a parameter
// (AST_PATTERN_VARIABLE directly under the function) or a local variable
// declaration inside the function body. Returns the C type or NULL.
static const char* lookup_in_function(ASTNode* func, const char* var_name) {
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
                return get_c_type(p->node_type);
            }
        }
    }
    // Locals: walk the body block(s), nested blocks included.
    for (int j = 0; j < func->child_count; j++) {
        ASTNode* body = func->children[j];
        if (!body || body->type != AST_BLOCK) continue;
        const char* t = decl_c_type_in_scope(body, var_name);
        if (t) return t;
    }
    return NULL;
}

// Look up a variable's C type. If `parent_func` is non-NULL, prefer the
// parameters and locals of that function — this is the closure's lexical
// parent and the only correct place to resolve its captures. Fall back to a
// program-wide search for backward compatibility with call sites that don't
// yet pass a parent.
static const char* lookup_var_c_type(CodeGenerator* gen, const char* var_name, const char* parent_func) {
    if (!gen->program || !var_name) return "int";
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
                    return get_c_type(p->node_type);
                }
            }
            const char* t = decl_c_type_in_scope(last_block_child(c), var_name);
            if (t) return t;
            char outer[64];
            if (find_enclosing_scope_name(gen->program, NULL, c, outer, sizeof(outer))) {
                return lookup_var_c_type(gen, var_name, outer);
            }
        }
        return "int";
    }
    // Parent-function-first lookup
    if (parent_func) {
        for (int i = 0; i < gen->program->child_count; i++) {
            ASTNode* top = gen->program->children[i];
            if (!top) continue;
            int matches = 0;
            if (strcmp(parent_func, "main") == 0 && top->type == AST_MAIN_FUNCTION) {
                matches = 1;
            } else if ((top->type == AST_FUNCTION_DEFINITION || top->type == AST_BUILDER_FUNCTION) &&
                       top->value && strcmp(top->value, parent_func) == 0) {
                matches = 1;
            }
            if (matches) {
                const char* t = lookup_in_function(top, var_name);
                if (t) return t;
                break; // don't scan other functions — captured names resolve lexically
            }
        }
    }
    // Fallback: program-wide search (kept for safety when parent_func is NULL
    // or when the var was declared at an unexpected location).
    for (int i = 0; i < gen->program->child_count; i++) {
        ASTNode* top = gen->program->children[i];
        if (!top) continue;
        if (top->type == AST_FUNCTION_DEFINITION || top->type == AST_BUILDER_FUNCTION || top->type == AST_MAIN_FUNCTION) {
            const char* t = lookup_in_function(top, var_name);
            if (t) return t;
        }
    }
    return "int"; // fallback
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
    const char* ret_type = "int";
    ASTNode* ret_expr = find_first_return_expr(body_check);
    int resolved = 0;
    if (ret_expr && ret_expr->type == AST_FUNCTION_CALL && ret_expr->value &&
        strcmp(ret_expr->value, "call") == 0 &&
        ret_expr->child_count >= 1 &&
        ret_expr->children[0] &&
        ret_expr->children[0]->type == AST_IDENTIFIER &&
        ret_expr->children[0]->value) {
        const char* callee = ret_expr->children[0]->value;
        for (int cvi = 0; cvi < gen->closure_var_count; cvi++) {
            if (gen->closure_var_map[cvi].var_name &&
                strcmp(gen->closure_var_map[cvi].var_name, callee) == 0) {
                int callee_id = gen->closure_var_map[cvi].closure_id;
                for (int cj = 0; cj < gen->closure_count; cj++) {
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
                    if (callee_ret) {
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
                break;
            }
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
    return ret_type;
}

// True when a captured variable is a READ-ONLY string capture — the case
// whose env store must go through aether_str_capture() so the env owns a
// reference (asks/closure-captured-heap-string-dangles.md: the enclosing
// scope releases its own reference on loop-carried reassignment and at
// scope exit, so a borrowed pointer dangles by the time a stored closure
// fires). Promoted (assigned-to) captures share a heap cell — the env
// field is `ctype*` and must NOT be routed through the retain.
static int capture_is_retained_string(CodeGenerator* gen, const char* name,
                                      const char* parent_func) {
    char** promoted = NULL;
    int promoted_count = 0;
    get_promoted_names_for_func(gen, parent_func, &promoted, &promoted_count);
    for (int p = 0; p < promoted_count; p++) {
        if (promoted[p] && strcmp(promoted[p], name) == 0) return 0;
    }
    const char* ctype = lookup_var_c_type(gen, name, parent_func);
    return ctype && (strcmp(ctype, "const char*") == 0 ||
                     strcmp(ctype, "char*") == 0);
}

// Emit just the signature (no trailing `;` or `{`) of a closure function.
// Caller appends `;\n` for forward decls or ` {\n` for bodies.
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
            fprintf(gen->output, ", %s %s", ptype, safe_c_name(p->value));
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
    char** parent_promoted = NULL;
    int parent_promoted_count = 0;
    get_promoted_names_for_func(gen, parent_func, &parent_promoted, &parent_promoted_count);
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
    if (cap_count == 0) {
        fprintf(gen->output, "    int _dummy;\n");
    } else {
        for (int i = 0; i < cap_count; i++) {
            const char* ctype = lookup_var_c_type(gen, captures[i], parent_func);
            int is_promoted = 0;
            for (int p = 0; p < parent_promoted_count; p++) {
                if (parent_promoted[p] && strcmp(parent_promoted[p], captures[i]) == 0) {
                    is_promoted = 1;
                    break;
                }
            }
            if (is_promoted) {
                fprintf(gen->output, "    %s* %s;\n", ctype, captures[i]);
            } else {
                fprintf(gen->output, "    %s %s;\n", ctype, captures[i]);
            }
        }
    }
    fprintf(gen->output, "} _closure_env_%d;\n\n", id);

    /* #1398: the env owns a reference per retained-string capture, so teardown
       has to be member-aware. Promoted captures share a heap cell rather than
       owning a reference and are skipped. */
    fprintf(gen->output, "static void _closure_env_%d_free(void* _p) {\n", id);
    fprintf(gen->output, "    if (!_p) return;\n");
    {
        int released = 0;
        for (int i = 0; i < cap_count; i++) {
            int is_promoted = 0;
            for (int p2 = 0; p2 < parent_promoted_count; p2++) {
                if (parent_promoted[p2] && strcmp(parent_promoted[p2], captures[i]) == 0) {
                    is_promoted = 1;
                    break;
                }
            }
            if (is_promoted) continue;
            if (!capture_is_retained_string(gen, captures[i], parent_func)) continue;
            if (!released) {
                fprintf(gen->output, "    _closure_env_%d* _e = (_closure_env_%d*)_p;\n", id, id);
                released = 1;
            }
            fprintf(gen->output, "    aether_string_release_captured(_e->%s);\n", captures[i]);
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
void emit_closure_definitions(CodeGenerator* gen) {
    // Pass 1: forward declarations.
    for (int ci = 0; ci < gen->closure_count; ci++) {
        emit_closure_env_typedef(gen, ci);
        const char* ret_type = resolve_closure_return_type(gen, ci);
        emit_closure_signature(gen, ci, ret_type);
        fprintf(gen->output, ";\n");
    }
    if (gen->closure_count > 0) fprintf(gen->output, "\n");

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
        emit_closure_signature(gen, ci, ret_type);
        fprintf(gen->output, " {\n");

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
            if (is_promoted_for_parent) {
                // Pointer alias: body reads/writes dereference through the
                // AST_IDENTIFIER emit path when the name is in
                // current_promoted_captures.
                fprintf(gen->output, "    %s* %s = _env->%s;\n",
                        ctype, captures[i], captures[i]);
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
            int prev_declared_count = gen->declared_var_count;
            gen->declared_vars = NULL;
            gen->declared_var_count = 0;
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
            /* Track the closure as the current function so
             * body-structural queries (current_fn_body_block /
             * body_assigns_var_from_heap) resolve a value identifier
             * against the closure's own body, not the enclosing
             * function's. */
            ASTNode* prev_current_function = gen->current_function;
            gen->current_function = closure;
            // Publish env-backed captures so generate_statement routes writes
            // through _env-> instead of a local alias.
            char** prev_env = gen->current_env_captures;
            int prev_env_count = gen->current_env_capture_count;
            gen->current_env_captures = env_captures;
            gen->current_env_capture_count = env_capture_count;
            // Publish promoted names visible to this closure body so
            // reads/writes dereference through the pointer alias we just
            // emitted above. EXCLUDE names that are closure parameters of
            // this closure — those are regular-typed values (int, string,
            // etc.), not pointers, and dereferencing them would be wrong.
            //
            // Two sources, and they behave differently:
            //  - the PARENT scope's promoted names: they arrive as `T*` env
            //    slots with a `T* name = _env->name;` prologue alias, and are
            //    pre-marked declared below;
            //  - this closure's OWN promoted names (recorded against its
            //    `__closure_<ptr>` scope because a closure nested inside it
            //    writes one of its locals): these are its own locals, so the
            //    ordinary declaration path must mint the heap cell. They go
            //    into the promoted set — reads/writes dereference — but are
            //    NOT pre-marked declared and get no prologue alias.
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
                    body_promoted[body_promoted_count++] = parent_promoted[p];
                }
                for (int p = 0; p < own_promoted_count; p++) {
                    if (!own_promoted[p]) continue;
                    if (is_closure_param(closure, own_promoted[p])) continue;
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
            // The prologue alias `T* name = _env->name;` is the declaration.
            for (int p = 0; p < parent_promoted_count; p++) {
                if (parent_promoted[p]) mark_var_declared(gen, parent_promoted[p]);
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
            if (gen->declared_vars) {
                for (int i = 0; i < gen->declared_var_count; i++) free(gen->declared_vars[i]);
                free(gen->declared_vars);
            }
            gen->declared_vars = prev_declared;
            gen->declared_var_count = prev_declared_count;
            // Free this closure body's heap-string set and restore the
            // enclosing scope's (see the matching reset above).
            clear_heap_string_vars(gen);
    clear_seq_vars(gen);
            gen->heap_string_vars = prev_heap;
            gen->heap_string_var_count = prev_heap_count;
            gen->current_function = prev_current_function;
            gen->in_trailing_block--;
            gen->indent_level = 0;
        }

        free(env_captures);

        fprintf(gen->output, "}\n\n");

        // Emit MSVC-compatible closure constructor function (avoids statement expressions)
        if (cap_count > 0) {
            fprintf(gen->output, "#if !AETHER_GCC_COMPAT\n");
            fprintf(gen->output, "static _AeClosure _aether_make_closure_%d(", id);
            for (int i = 0; i < cap_count; i++) {
                if (i > 0) fprintf(gen->output, ", ");
                const char* ctype = lookup_var_c_type(gen, captures[i], parent_func);
                fprintf(gen->output, "%s %s", ctype, captures[i]);
            }
            fprintf(gen->output, ") {\n");
            fprintf(gen->output, "    _closure_env_%d* _e = malloc(sizeof(_closure_env_%d));\n", id, id);
            fprintf(gen->output, "    _e->_dtor = _closure_env_%d_free;\n", id);
            for (int i = 0; i < cap_count; i++) {
                if (capture_is_retained_string(gen, captures[i], parent_func)) {
                    /* env owns a reference — see aether_str_capture preamble */
                    const char* ctype = lookup_var_c_type(gen, captures[i], parent_func);
                    fprintf(gen->output, "    _e->%s = (%s)aether_str_capture(%s);\n",
                            captures[i], ctype, captures[i]);
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

void emit_message_field_init(CodeGenerator* gen, MessageFieldDef* fdef, ASTNode* rhs) {
    // If this field was hoisted by emit_message_array_hoists, emit the
    // hoisted variable name instead of inlining the compound literal.
    // (Requires the pre-walk to have populated the map for this field.)
    if (rhs && rhs->type == AST_ARRAY_LITERAL && fdef && fdef->element_c_type) {
        int id = lookup_msg_arr_id(fdef->name);
        if (id >= 0) {
            fprintf(gen->output, "_aether_arr_%d", id);
            return;
        }
        // Fall-through: no hoist set up (e.g. reply statement). Use a
        // compound literal — still wrong for cross-thread sends, but
        // fine for synchronous ask/reply where the sender stays alive.
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

void generate_expression(CodeGenerator* gen, ASTNode* expr) {
    if (!expr) return;

    codegen_note_diag_pos(expr);

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
                fprintf(gen->output, "\"");
                const char* str = expr->value;
                while (*str) {
                    unsigned char ch = (unsigned char)*str;
                    switch (*str) {
                        case '\n': fprintf(gen->output, "\\n"); break;
                        case '\t': fprintf(gen->output, "\\t"); break;
                        case '\r': fprintf(gen->output, "\\r"); break;
                        case '\\': fprintf(gen->output, "\\\\"); break;
                        case '"': fprintf(gen->output, "\\\""); break;
                        default:
                            if (ch < 0x20 || ch == 0x7F) {
                                /* Zero-padded OCTAL, never \x: a C hex
                                 * escape has no length limit, so
                                 * "\x01a" re-lexes as byte 0x1A (silent
                                 * corruption when the next char is a
                                 * hex digit). \001 is exactly three
                                 * digits and cannot munch. */
                                fprintf(gen->output, "\\%03o", ch);
                            } else if (ch >= 0x80) {
                                /* Bytes above ASCII: emit a VALID UTF-8
                                 * sequence raw so human text stays
                                 * readable in the generated C; escape
                                 * anything else (decoded \x binary,
                                 * e.g. CBOR/MsgPack test vectors) so
                                 * the output stays valid text and every
                                 * downstream tool (grep/awk/editors)
                                 * treats it uniformly on all platforms. */
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
                fprintf(gen->output, "\"");
            } else if (expr->node_type && expr->node_type->kind == TYPE_DURATION) {
                fprintf(gen->output, "%lldLL", parse_duration_literal_ns(expr->value));
            } else {
                /* Numeric literals: translate 0o / 0b prefixes that C
                 * doesn't accept. Decimal / 0x pass through unchanged. */
                char buf[64];
                fprintf(gen->output, "%s",
                        translate_integer_literal(expr->value, buf, sizeof(buf)));
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
            const char* struct_c = "void";
            if (expr->node_type && expr->node_type->kind == TYPE_PTR &&
                expr->node_type->element_type) {
                struct_c = get_c_type(expr->node_type->element_type);
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
            char rc[256];
            snprintf(rc, sizeof(rc), "%s", get_c_type(rt));
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
                /* #891 @c_struct overlay read (handles nested chains
                 * `s.a.b.c`): flatten to the overlay-pointer root + dotted
                 * field path, then emit aether_mem_get_<width> at the
                 * cumulative offset. Width is DERIVED from the field type. */
                if (expr->value) {
                    char cpath[256];
                    ASTNode* root = aether_c_struct_chain(expr, cpath, sizeof(cpath));
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
                if (child->node_type && child->node_type->kind == TYPE_ACTOR_REF && expr->value) {
                    size_t name_len = strlen(expr->value);
                    int is_ref_field = (name_len > 4 && strcmp(expr->value + name_len - 4, "_ref") == 0);

                    if (!gen->current_actor && !gen->generating_lvalue && !is_ref_field) {
                        needs_atomic = 1;
                    }
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

        case AST_PTR_AS_ARRAY_CAST:
            /* `expr as T[]` — emit `((T*)(expr))`.  The result is a
             * typed C pointer that an enclosing AST_ARRAY_ACCESS lowers
             * to `((T*)(expr))[i]` (C scales by sizeof(T) automatically).
             * No allocation, no bounds check — same systems-programming
             * escape hatch as `as *StructName`, just at a buffer-element
             * granularity instead of a struct-header granularity. */
            if (expr->child_count > 0 && expr->node_type &&
                expr->node_type->kind == TYPE_ARRAY &&
                expr->node_type->element_type) {
                const char* elem = get_c_type(expr->node_type->element_type);
                fprintf(gen->output, "((%s*)(", elem);
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
                        // comparison). For string element types, dispatch through
                        // `_aether_safe_str` + `strcmp`, exactly as the ordinary
                        // string-comparison path below does.
                        Type* elem = L->node_type ? L->node_type->element_type : NULL;
                        int str_val = elem && elem->kind == TYPE_STRING;
                        fprintf(gen->output, "(({ %s _l = ", get_c_type(L->node_type));
                        generate_expression(gen, L);
                        fprintf(gen->output, "; %s _r = ", get_c_type(R->node_type));
                        generate_expression(gen, R);
                        if (str_val) {
                            fprintf(gen->output,
                                "; %s(_l.has == _r.has && (!_l.has || "
                                "strcmp(_aether_safe_str(_l.val), _aether_safe_str(_r.val)) == 0)); }))",
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

                // String comparison: emit strcmp instead of pointer ==.
                // Applies to ==, !=, <, >, <=, >= when:
                //   - both sides are strings, OR
                //   - one side is a string literal / string-typed value
                //     and the other is a `ptr`-typed value that may
                //     carry an AetherString header (string.from_int,
                //     fs.read_binary, string_concat_wrapped, …).
                // NOT when either side is a null literal — that's a
                // null check, not a string compare.
                //
                // The ptr-vs-string case fixes #267: an Aether comparison
                // like `string.from_int(42) != "42"` would otherwise
                // emit a bare pointer compare and always evaluate true.
                // Routing through _aether_safe_str + strcmp dispatches
                // on the magic header so wrapped strings are read by
                // their payload bytes.
                int is_string_cmp = 0;
                if (expr->value && (strcmp(expr->value, "==") == 0 || strcmp(expr->value, "!=") == 0
                    || strcmp(expr->value, "<") == 0 || strcmp(expr->value, ">") == 0
                    || strcmp(expr->value, "<=") == 0 || strcmp(expr->value, ">=") == 0)) {
                    Type* lhs_type = expr->children[0]->node_type;
                    Type* rhs_type = expr->children[1]->node_type;
                    ASTNode* rhs = expr->children[1];
                    ASTNode* lhs_node = expr->children[0];
                    int rhs_is_null = (rhs->type == AST_LITERAL && rhs->value && strcmp(rhs->value, "0") == 0)
                                   || (rhs->type == AST_IDENTIFIER && rhs->value && strcmp(rhs->value, "NULL") == 0);
                    int lhs_is_null = (lhs_node->type == AST_LITERAL && lhs_node->value && strcmp(lhs_node->value, "0") == 0)
                                   || (lhs_node->type == AST_IDENTIFIER && lhs_node->value && strcmp(lhs_node->value, "NULL") == 0);
                    int lhs_is_string = (lhs_type && lhs_type->kind == TYPE_STRING);
                    int rhs_is_string = (rhs_type && rhs_type->kind == TYPE_STRING);
                    int lhs_is_ptr_t  = (lhs_type && lhs_type->kind == TYPE_PTR);
                    int rhs_is_ptr_t  = (rhs_type && rhs_type->kind == TYPE_PTR);
                    if (!rhs_is_null && !lhs_is_null) {
                        if (lhs_is_string && rhs_is_string) {
                            is_string_cmp = 1;
                        } else if ((lhs_is_string && rhs_is_ptr_t) ||
                                   (lhs_is_ptr_t && rhs_is_string)) {
                            // ptr vs string-literal / string-typed value:
                            // assume the ptr is an AetherString-bearing
                            // payload and dispatch via _aether_safe_str.
                            // Pure ptr-vs-ptr opaque-handle comparisons
                            // still go through bare pointer eq (handled
                            // by the else-branch below).
                            is_string_cmp = 1;
                        }
                    }
                }

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
                        fprintf(gen->output,
                                "); int _se_v = (strcmp(_aether_safe_str(_se_l), "
                                "_aether_safe_str(_se_r)) %s 0); ",
                                get_c_operator(expr->value));
                        if (drain_l) fprintf(gen->output, "aether_heap_str_free(_se_l); ");
                        if (drain_r) fprintf(gen->output, "aether_heap_str_free(_se_r); ");
                        fprintf(gen->output, "_se_v; })");
                        if (!skip_parens) fprintf(gen->output, ")");
                    } else {
                        if (!skip_parens) fprintf(gen->output, "(");
                        fprintf(gen->output, "strcmp(_aether_safe_str(");
                        generate_expression(gen, expr->children[0]);
                        fprintf(gen->output, "), _aether_safe_str(");
                        generate_expression(gen, expr->children[1]);
                        fprintf(gen->output, ")) %s 0", get_c_operator(expr->value));
                        if (!skip_parens) fprintf(gen->output, ")");
                    }
                } else if (is_assignment && expr->children[0] &&
                           expr->children[0]->type == AST_MEMBER_ACCESS &&
                           aether_c_struct_overlay_lhs(expr->children[0])) {
                    /* #891 @c_struct overlay write (incl. nested `s.a.b = v`):
                     * flatten to overlay-pointer root + dotted path, emit
                     * aether_mem_set_<width> at the cumulative offset. The
                     * parser lands a member-access store as a binary-`=`. */
                    char cpath[256];
                    ASTNode* root = aether_c_struct_chain(expr->children[0], cpath, sizeof(cpath));
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
                    if (duration_ratio) fprintf(gen->output, "(double)");
                    if (ptr_int_cmp && lhs_is_ptr) fprintf(gen->output, "(intptr_t)");
                    if (wide_cast && AE_IS_NARROW_INT(ltype)) fprintf(gen->output, "%s", wide_cast);
                    generate_expression(gen, expr->children[0]);
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
                if (arg && arg->node_type && arg->node_type->kind == TYPE_PTR &&
                    arg->node_type->element_type &&
                    arg->node_type->element_type->kind == TYPE_STRUCT &&
                    arg->node_type->element_type->struct_name && gen->program) {
                    ASTNode* sdef = find_struct_definition_by_name(
                        gen->program, arg->node_type->element_type->struct_name);
                    if (sdef && struct_has_heap_string_field(sdef)) {
                        typed_free = arg->node_type->element_type->struct_name;
                    }
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
             * so no cast is needed. */
            if (expr->annotation && expr->value &&
                strncmp(expr->annotation, "fnfield_", 8) == 0) {
                int is_ptr = strcmp(expr->annotation, "fnfield_ptr") == 0;
                const char* dot = strrchr(expr->value, '.');
                if (dot) {
                    char recv[200];
                    size_t rlen = (size_t)(dot - expr->value);
                    if (rlen >= sizeof(recv)) rlen = sizeof(recv) - 1;
                    memcpy(recv, expr->value, rlen);
                    recv[rlen] = '\0';
                    char recv_c[200], field_c[200];
                    /* Keyword mangling only (safe_value_name): the receiver
                     * is a local and the field is a struct member, neither is
                     * a linker symbol, so the libc-collision rename in
                     * safe_c_name must not apply. It renamed a field spelled
                     * `read` to `ae_read` here while the struct definition
                     * kept `read`, so the emitted C referenced a member that
                     * does not exist (#1251). */
                    snprintf(recv_c, sizeof(recv_c), "%s", safe_value_name(recv));
                    snprintf(field_c, sizeof(field_c), "%s", safe_value_name(dot + 1));
                    fprintf(gen->output, "(%s%s%s)(",
                            recv_c, is_ptr ? "->" : ".", field_c);
                    for (int i = 0; i < expr->child_count; i++) {
                        if (i > 0) fprintf(gen->output, ", ");
                        generate_expression(gen, expr->children[i]);
                    }
                    fprintf(gen->output, ")");
                    break;
                }
            }
            if (expr->value) {
                const char* func_name = expr->value;
                /* Dotted source callees (`string.seq_free`) normalised to
                 * the underscored C/registry form for handlers that match
                 * stdlib functions by name. */
                char func_name_norm_buf[256];
                const char* func_name_norm =
                    codegen_normalise_callee(func_name, func_name_norm_buf,
                                             sizeof(func_name_norm_buf));
                /* Capture + clear the discarded-value flag at the top of
                 * the call codegen so it governs THIS call only and never
                 * leaks into nested argument calls (whose values ARE
                 * consumed). When set, the arg-temp drain below treats the
                 * parent as void-yielding so heap inline args still free.
                 * See discard_call_value in codegen.h. */
                int ad_call_discarded = gen->discard_call_value;
                gen->discard_call_value = 0;

                if (strcmp(func_name, "make") == 0 && expr->node_type && expr->node_type->kind == TYPE_ARRAY) {
                    fprintf(gen->output, "(%s)malloc(", get_c_type(expr->node_type));
                    if (expr->child_count > 0) {
                        fprintf(gen->output, "(");
                        generate_expression(gen, expr->children[0]);
                        fprintf(gen->output, ") * sizeof(%s)", get_c_type(expr->node_type->element_type));
                    }
                    fprintf(gen->output, ")");
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
                        } else if (arg_type->kind == TYPE_FLOAT) {
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
                                // Runtime string — could be NULL
                                fprintf(gen->output, "printf(\"%%s\", _aether_safe_str(");
                                generate_expression(gen, arg);
                                fprintf(gen->output, "))");
                            }
                        } else if (arg_type->kind == TYPE_PTR) {
                            // Runtime pointer — could be NULL
                            fprintf(gen->output, "printf(\"%%s\", _aether_safe_str(");
                            generate_expression(gen, arg);
                            fprintf(gen->output, "))");
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
                            fprintf(gen->output, "printf(");
                            generate_expression(gen, a);
                            fprintf(gen->output, ")");
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
                                    else if (atype && atype->kind == TYPE_FLOAT) fprintf(gen->output, "%%f");
                                    else if (atype && atype->kind == TYPE_INT64) fprintf(gen->output, "%%lld");
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
                    // println(x) = print(x) then putchar('\n')
                    // Special case: println("...${expr}...") — generate interp then add \n
                    if (expr->child_count == 1 && expr->children[0]->type == AST_STRING_INTERP) {
                        gen->interp_as_printf = 1;
                        generate_expression(gen, expr->children[0]);
                        gen->interp_as_printf = 0;
                        fprintf(gen->output, "; putchar('\\n')");
                    } else
                    if (expr->child_count == 1 && expr->children[0]->node_type) {
                        ASTNode* arg = expr->children[0];
                        Type* arg_type = arg->node_type;
                        if (arg_type->kind == TYPE_INT) {
                            fprintf(gen->output, "printf(\"%%d\\n\", ");
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
                        } else if (arg_type->kind == TYPE_FLOAT) {
                            fprintf(gen->output, "printf(\"%%f\\n\", ");
                            generate_expression(gen, arg);
                            fprintf(gen->output, ")");
                        } else if (arg_type->kind == TYPE_STRING) {
                            if (arg->type == AST_LITERAL) {
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
                                // Runtime string — could be NULL
                                fprintf(gen->output, "printf(\"%%s\\n\", _aether_safe_str(");
                                generate_expression(gen, arg);
                                fprintf(gen->output, "))");
                            }
                        } else if (arg_type->kind == TYPE_PTR) {
                            // Runtime pointer — could be NULL
                            fprintf(gen->output, "printf(\"%%s\\n\", _aether_safe_str(");
                            generate_expression(gen, arg);
                            fprintf(gen->output, "))");
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
                                    else if (atype && atype->kind == TYPE_FLOAT) fprintf(gen->output, "%%f");
                                    else if (atype && atype->kind == TYPE_INT64) fprintf(gen->output, "%%lld");
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
                else if (strcmp(func_name, "atoi") == 0 && expr->child_count == 1) {
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
                    generate_expression(gen, expr->children[0]);
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
                                "({ if (_heap_%s) { aether_heap_str_free((void*)%s); "
                                "%s = NULL; _heap_%s = 0; } })",
                                arg->value, arg->value, arg->value, arg->value);
                        } else {
                            fprintf(gen->output, "string_release(");
                            generate_expression(gen, arg);
                            fprintf(gen->output, ")");
                        }
                    } else {
                        fprintf(stderr,
                            "error: release() at line %d: only `string` is supported today.\n"
                            "  For other heap types, call the typed release function:\n"
                            "    *StringSeq -> string.string_seq_free\n"
                            "    *Map       -> hashmap.free\n",
                            expr->line);
                        fprintf(gen->output, "0 /* release() type error, see stderr */");
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
                          strcmp(func_name, "string.release") == 0) &&
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
                        "({ if (_heap_%s) { aether_heap_str_free((void*)%s); "
                        "%s = NULL; _heap_%s = 0; } })",
                        arg->value, arg->value, arg->value, arg->value);
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
                          strcmp(func_name, "string.free") == 0) &&
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
                    fprintf(gen->output, "(int)(*(intptr_t*)");
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
                        }
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
                    fprintf(gen->output, "_aether_ctx_push((void*)(intptr_t)");
                    generate_expression(gen, expr->children[0]);
                    fprintf(gen->output, ")");
                }
                else if (strcmp(func_name, "sandbox_pop") == 0 && expr->child_count == 0) {
                    fprintf(gen->output, "_aether_ctx_pop()");
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
                    if (closure_arg && closure_arg->type == AST_IDENTIFIER && closure_arg->value) {
                        if (!is_promoted_capture(gen, closure_arg->value)) {
                            for (int ci = 0; ci < gen->closure_var_count; ci++) {
                                if (strcmp(gen->closure_var_map[ci].var_name, closure_arg->value) == 0) {
                                    int cid = gen->closure_var_map[ci].closure_id;
                                    if (cid >= 0) found_id = cid;
                                    break;
                                }
                            }
                        }
                    }

                    if (found_id >= 0) {
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
                        Type* ret_t = NULL;
                        if (expr->node_type && expr->node_type->kind != TYPE_VOID &&
                            expr->node_type->kind != TYPE_UNKNOWN) {
                            ret_t = expr->node_type;
                        } else if (closure_sig && closure_sig->return_type &&
                                   closure_sig->return_type->kind != TYPE_UNKNOWN) {
                            ret_t = closure_sig->return_type;
                        } else if (gen->current_func_return_type &&
                                   gen->current_func_return_type->kind != TYPE_VOID &&
                                   gen->current_func_return_type->kind != TYPE_UNKNOWN) {
                            /* Last resort: the call is a `return
                             * cb(...)` statement inside a typed
                             * fn, and the enclosing fn's return
                             * type dictates the cast's return slot.
                             * Imperfect if the call result is
                             * discarded or used in a non-return
                             * position, but those cases already
                             * have `expr->node_type` set correctly. */
                            ret_t = gen->current_func_return_type;
                        }
                        const char* ret = ret_t ? get_c_type(ret_t) : "int";
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
                    }
                }
                else {
                    /* Typed fn-pointer local call: `fp(a, b)` where
                     * `fp` was declared as `fn(T1, T2, ...) -> R` (or
                     * initialised from an `expr as fn(...)` cast).
                     * Emit a typed C function-pointer cast around the
                     * stored void* so the C compiler sees the correct
                     * signature.  The cast and call are inlined here;
                     * no per-signature shim is needed. */
                    Type* fnptr_sig = lookup_fnptr_local(gen, func_name);
                    if (fnptr_sig && fnptr_sig->kind == TYPE_FUNCTION &&
                        fnptr_sig->is_fnptr) {
                        const char* ret_c = fnptr_sig->return_type
                            ? get_c_type(fnptr_sig->return_type) : "void";
                        fprintf(gen->output, "((%s(*)(", ret_c);
                        for (int pi = 0; pi < fnptr_sig->param_count; pi++) {
                            if (pi > 0) fprintf(gen->output, ", ");
                            fprintf(gen->output, "%s",
                                get_c_type(fnptr_sig->param_types[pi]));
                        }
                        if (fnptr_sig->param_count == 0) {
                            fprintf(gen->output, "void");
                        }
                        fprintf(gen->output, "))(%s))(", safe_c_name(func_name));
                        for (int i = 0; i < expr->child_count; i++) {
                            if (i > 0) fprintf(gen->output, ", ");
                            generate_expression(gen, expr->children[i]);
                        }
                        fprintf(gen->output, ")");
                        break;
                    }

                    char c_func_name[256];
                    // Don't mangle extern functions — they refer to real C symbols.
                    // For @extern("c_symbol") aether_name(...), translate the
                    // Aether-side name to its bound C symbol. See #234.
                    const char* mangled = is_extern_func(gen, func_name)
                        ? lookup_extern_c_name(gen, func_name)
                        : safe_c_name(func_name);
                    strncpy(c_func_name, mangled, sizeof(c_func_name) - 1);
                    c_func_name[sizeof(c_func_name) - 1] = '\0';
                    for (char* p = c_func_name; *p; p++) {
                        if (*p == '.') *p = '_';
                    }
                    /* #1383: substituting '_' for the dot assumes the C symbol is
                       `<module>_<name>`. It is not when the export already carries
                       the module (`intarr.intarr_new_raw`) or carries none
                       (`os.aether_args_count`). Prefer the declared extern. */
                    {
                        const char* qdot = strchr(func_name, '.');
                        /* `<module>_<name>` may be an Aether wrapper rather than an
                           extern; redirecting past it would call the raw extern and
                           skip the wrapper's ownership handling. */
                        int qknown = is_extern_func(gen, c_func_name);
                        if (!qknown && gen->program) {
                            for (int qk = 0; qk < gen->program->child_count; qk++) {
                                ASTNode* qd = gen->program->children[qk];
                                if (qd && (qd->type == AST_FUNCTION_DEFINITION ||
                                           qd->type == AST_BUILDER_FUNCTION) &&
                                    qd->value && strcmp(qd->value, c_func_name) == 0) {
                                    qknown = 1;
                                    break;
                                }
                            }
                        }
                        if (qdot && qdot[1] && !qknown) {
                            const char* after = qdot + 1;
                            if (is_extern_func(gen, after)) {
                                const char* real = lookup_extern_c_name(gen, after);
                                if (real) {
                                    strncpy(c_func_name, real, sizeof(c_func_name) - 1);
                                    c_func_name[sizeof(c_func_name) - 1] = '\0';
                                }
                            }
                        }
                    }

                    // spawn_ActorName(preferred_core) — pass core hint or -1
                    if (strncmp(func_name, "spawn_", 6) == 0 &&
                        strcmp(func_name, "spawn_sandboxed") != 0) {
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
                    if (is_list_shape || is_map_shape) {
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
                            if (val && val->type == AST_IDENTIFIER && val->value) {
                                val_is_heap = body_assigns_var_from_heap(
                                    gen, current_fn_body_block(gen), val->value);
                            } else {
                                val_is_heap = val && is_heap_string_expr(gen, val);
                            }
                            /* A closure value (`fn`-typed, not a raw fn-ptr)
                             * stored into a container is heap-boxed by the
                             * arg coercion (_aether_box_closure — mirror of
                             * the condition at the call-arg fn->ptr path).
                             * The box is a fresh malloc the container should
                             * own, or it leaks (the list holds an opaque ptr
                             * with no owner). Route to the owning add so
                             * list_free reclaims the box. A bare fn -> ptr
                             * (is_fnptr) is a code address, not heap — it
                             * stays on the raw path. */
                            int val_is_closure =
                                val && val->node_type &&
                                val->node_type->kind == TYPE_FUNCTION &&
                                !val->node_type->is_fnptr;
                            if (val_is_heap) {
                                if (is_wrapper) {
                                    fprintf(gen->output, "(");
                                }
                                if (is_list_shape) {
                                    fprintf(gen->output, "list_add_string_adopted(");
                                    generate_expression(gen, expr->children[0]);
                                    fprintf(gen->output, ", (void*)");
                                    generate_expression(gen, val);
                                    fprintf(gen->output, ")");
                                    if (is_wrapper) {
                                        fprintf(gen->output, " ? \"\" : \"list.add failed\")");
                                    }
                                } else {
                                    fprintf(gen->output, "map_put_string_adopted(");
                                    generate_expression(gen, expr->children[0]);
                                    /* Key may now be a magic AetherString
                                     * (string ops return magic); route the
                                     * payload bytes through aether_string_data
                                     * so the map hashes/compares the content,
                                     * not the struct header. Safe for plain
                                     * char-pointer / literal keys too
                                     * (str_data returns them unchanged). */
                                    fprintf(gen->output, ", aether_string_data((const void*)");
                                    generate_expression(gen, expr->children[1]);
                                    fprintf(gen->output, "), (void*)");
                                    generate_expression(gen, val);
                                    fprintf(gen->output, ")");
                                    if (is_wrapper) {
                                        fprintf(gen->output, " ? \"\" : \"map.put failed\")");
                                    }
                                }
                                break;
                            }
                            /* NOTE: a heap string reaching the container only
                             * through a `string` PARAMETER is deliberately
                             * NOT adopted here. The magic header proves the
                             * value is a heap AetherString, but NOT that the
                             * caller transferred ownership — a borrowed magic
                             * string (owned by another container/scope) would
                             * be double-freed if the container also released
                             * it at free time. Representation != ownership.
                             * Such a value stays on the raw path (the
                             * container does not own it); the residual leak
                             * when the caller DID transfer ownership is the
                             * safe side of that trade (see
                             * docs/memory-management.md, leaks_known.txt). */
                            /* Closure value -> the container owns the heap
                             * box. list_add_string_adopted just stores the
                             * pointer and sets owned_flags[i]=1 (no string
                             * semantics); list_free's owned path frees the
                             * non-magic box via libc free. (map values are a
                             * `ptr` slot — closures stored in maps are rare
                             * and keep the existing raw path.) */
                            if (val_is_closure && is_list_shape) {
                                if (is_wrapper) fprintf(gen->output, "(");
                                /* list_add_closure_owned tags the slot as an
                                 * owned closure box (owned_flags == 2) so
                                 * list_free reclaims the box AND its captured
                                 * env — not just the box. */
                                fprintf(gen->output, "list_add_closure_owned(");
                                generate_expression(gen, expr->children[0]);
                                fprintf(gen->output, ", (void*)_aether_box_closure(");
                                generate_expression(gen, val);
                                fprintf(gen->output, "))");
                                if (is_wrapper) fprintf(gen->output, " ? \"\" : \"list.add failed\")");
                                break;
                            }
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
                    int ad_saved_count = g_arg_drain_count;
                    int ad_arg_idx[16];
                    int ad_identity[16] = {0};  /* 1 = identity-guarded release (return-escape-only param) */
                    int ad_arg_count = 0;
                    Type* ad_ret_type = expr->node_type;
                    int ad_have_value =
                        (ad_ret_type &&
                         ad_ret_type->kind != TYPE_VOID &&
                         ad_ret_type->kind != TYPE_UNKNOWN);
                    /* A VOID parent (e.g. an assert-style helper
                     * `check(label, string.from_long(n), want)`) yields
                     * no value, but its heap-returning inline args still
                     * leak. Drain them via a statement-expression that
                     * yields void: `({ T t=...; call(...); free(t); })`
                     * — valid wherever a void call appears (it is always
                     * a statement, so `({...});` is well-formed C).
                     * TYPE_UNKNOWN stays excluded UNLESS the call's value
                     * is being discarded at the statement level
                     * (ad_call_discarded) — there the wrap yields void and
                     * is provably safe regardless of the declared type. */
                    int ad_is_void =
                        (ad_ret_type && ad_ret_type->kind == TYPE_VOID) ||
                        ad_call_discarded;
                    if (ad_have_value || ad_is_void) {
                        for (int ai = 0; ai < expr->child_count && ad_arg_count < 16; ai++) {
                            ASTNode* arg = expr->children[ai];
                            if (!arg) continue;
                            /* Arg-temp wrapping fires on heap-classified
                             * subexpressions. Originally scoped to
                             * AST_FUNCTION_CALL only (see commit
                             * 39446f9), extended here to AST_STRING_INTERP
                             * — `_aether_interp(...)` allocates a heap
                             * buffer the caller is expected to own.
                             * Passing one inline as an argument leaked
                             * the buffer because no temp captured the
                             * pointer and no free fired after the outer
                             * call returned. Observed as ~34 byte/call
                             * leak on `outer(string_returning_call("seed-${i}"))`
                             * shape; 100k iterations grew RSS by 3.3 MB. */
                            /* AST_OR_ELSE included: `f(g() or { … })`
                             * yields a uniformly-heap string (the `or`
                             * lowering boxes both paths) with no consumer,
                             * so it leaks in argument position exactly like
                             * a bare heap-returning call. is_heap_string_expr
                             * classifies it, so drain it the same way. */
                            if (arg->type != AST_FUNCTION_CALL &&
                                arg->type != AST_STRING_INTERP &&
                                arg->type != AST_OR_ELSE) continue;
                            if (arg_drain_lookup(arg)) continue;
                            if (!is_heap_string_expr(gen, arg)) continue;
                            /* @retain parameter: callee stores the
                             * pointer (list_add, map_put's key,
                             * etc.). The heap value's lifetime is
                             * now the recipient's responsibility —
                             * freeing here would dangle the stored
                             * copy. Same gate the escape walker uses
                             * at codegen_stmt.c:1339-1346. */
                            if (is_retain_extern_param(gen, func_name, ai)) continue;
                            /* Escape decision for the arg's heap pointer.
                             * When the callee has a VISIBLE BODY the
                             * body-walk is authoritative: it now detects
                             * every storage sink (return, assignment RHS,
                             * aggregate element, struct field, closure
                             * capture, escaping nested call-arg), so a
                             * "does not escape" verdict is a proof, and a
                             * ptr-typed param that is only read (e.g. an
                             * assert helper's `got: ptr` compared via
                             * string.equals) can be safely drained.
                             *
                             * Without a visible body (extern / unknown)
                             * we fall back to the conservative
                             * call_arg_escapes heuristic — same gate the
                             * escape walker uses; a storage-shaped param
                             * is assumed to stash the pointer.
                             *
                             * Soundness: we only ADD a drain where
                             * non-escape is proven; anything unprovable
                             * stays "escapes". False-escape = leak (safe);
                             * false-non-escape = UAF (never introduced). */
                            int ad_id_guard = 0;  /* identity-guarded release for this arg */
                            if (callee_has_visible_body(gen, func_name)) {
                                if (callee_param_escapes_via_body(gen, func_name, ai, 0)) {
                                    /* The param escapes. If it ONLY return-escapes
                                     * (the callee passes the value through / may
                                     * return it) and does NOT store-escape, and the
                                     * callee returns a string, we can still reclaim
                                     * this FRESH temp at the call site with a pointer-
                                     * identity guard: free it iff the call did not
                                     * return it (r != t). This is the sound fix for
                                     * the recursive accumulator (walk_join(t,
                                     * concat(acc,sep,h))) — the intermediate concat
                                     * is freed when consumed, preserved when returned.
                                     * Only FRESH heap-expr args reach here (the
                                     * AST_FUNCTION_CALL/INTERP gate above), never a
                                     * borrowed var, so this can never free a value
                                     * the caller still holds.
                                     *
                                     * Otherwise (store-escape → a container/@retain/
                                     * field owns it; or non-string / value-less call
                                     * → no result pointer to compare) leave it. */
                                    if (!ad_have_value ||
                                        callee_param_store_escapes_via_body(gen, func_name, ai) ||
                                        !callee_returns_string(gen, func_name)) {
                                        continue;
                                    }
                                    ad_id_guard = 1;
                                }
                            } else {
                                TypeKind param_kind = lookup_callee_param_kind(gen, func_name, ai);
                                if (call_arg_escapes(param_kind)) continue;
                                if (callee_param_escapes_via_body(gen, func_name, ai, 0)) continue;
                            }
                            ad_identity[ad_arg_count] = ad_id_guard;
                            ad_arg_idx[ad_arg_count++] = ai;
                        }
                    }
                    char* ad_names[16] = {0};
                    if (ad_arg_count > 0) {
                        fprintf(gen->output, "({ ");
                        /* Pre-mint all temp names BEFORE recursing
                         * into generate_expression — the recursion
                         * may itself open inner wraps and mint
                         * their own temps, advancing
                         * g_arg_drain_counter. Reserving names up
                         * front keeps the outer and inner names
                         * distinct. */
                        for (int h = 0; h < ad_arg_count; h++) {
                            ad_names[h] = arg_drain_mint_name();
                        }
                        /* Emit each temp's decl. The arg's
                         * generate_expression may register inner
                         * substitutions on the registry stack —
                         * those are bound to inner-arg nodes and
                         * won't collide with our outer names because
                         * the counter advanced. */
                        for (int h = 0; h < ad_arg_count; h++) {
                            ASTNode* arg = expr->children[ad_arg_idx[h]];
                            fprintf(gen->output, "const char* %s = (const char*)(", ad_names[h]);
                            generate_expression(gen, arg);
                            fprintf(gen->output, "); ");
                        }
                        /* Now bind the outer-arg → outer-temp
                         * substitutions. The bind transfers
                         * ownership of the name string; subsequent
                         * arg_drain_truncate frees it. */
                        for (int h = 0; h < ad_arg_count; h++) {
                            ASTNode* arg = expr->children[ad_arg_idx[h]];
                            arg_drain_bind(arg, ad_names[h]);
                            ad_names[h] = NULL;
                        }
                        if (ad_have_value) {
                            const char* ad_ret_ct = get_c_type(ad_ret_type);
                            fprintf(gen->output, "%s _ad_r = ", ad_ret_ct);
                        }
                        /* void parent: emit the call bare (no _ad_r); the
                         * statement-expression yields void via the final
                         * free below. */
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
                        char bf_normalized[256];
                        codegen_normalise_callee(func_name, bf_normalized, sizeof(bf_normalized));
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
                            for (int fi = 0; fi < gen->program->child_count; fi++) {
                                ASTNode* fdef = gen->program->children[fi];
                                if (fdef && (fdef->type == AST_FUNCTION_DEFINITION || fdef->type == AST_BUILDER_FUNCTION) &&
                                    fdef->value && strcmp(fdef->value, func_name) == 0) {
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
                                    break;
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
                            for (int fi = 0; fi < gen->program->child_count; fi++) {
                                ASTNode* fdef = gen->program->children[fi];
                                if (fdef && (fdef->type == AST_FUNCTION_DEFINITION || fdef->type == AST_BUILDER_FUNCTION) &&
                                    fdef->value &&
                                    (strcmp(fdef->value, func_name) == 0 ||
                                     strcmp(fdef->value, c_func_name) == 0)) {
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
                                    break;
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
                             * type-check accept, hard fail at C compile. */
                            fprintf(gen->output, "_aether_box_closure(");
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
                            int arg_is_bare_fn = 0;
                            if (arg->type == AST_IDENTIFIER && arg->value && gen->program) {
                                for (int pi = 0; pi < gen->program->child_count; pi++) {
                                    ASTNode* pc = gen->program->children[pi];
                                    if (pc && (pc->type == AST_FUNCTION_DEFINITION ||
                                               pc->type == AST_BUILDER_FUNCTION) &&
                                        pc->value && strcmp(pc->value, arg->value) == 0) {
                                        arg_is_bare_fn = 1;
                                        break;
                                    }
                                }
                            }
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
                        } else if (expected == TYPE_PTR && arg->node_type &&
                            (arg->node_type->kind == TYPE_INT || arg->node_type->kind == TYPE_BOOL)) {
                            fprintf(gen->output, "(void*)(intptr_t)(");
                            generate_expression(gen, arg);
                            fprintf(gen->output, ")");
                        } else if ((expected == TYPE_INT || expected == TYPE_BOOL) &&
                                   arg->node_type && arg->node_type->kind == TYPE_PTR) {
                            /* Inverse of the ptr-cast above: bridges the injected
                             * `_builder` (typed `ptr`, lowered to `void*`) and ctx
                             * values when a callee parameter is declared `int`.
                             * Without this, the body of a `builder ... with factory`
                             * function passing `_builder` to an int-handle extern
                             * emits a bare void*→int conversion that GCC 14+/MinGW64
                             * reject under default -Werror=int-conversion. See
                             * builder-ctx-handle-void-ptr-int-conversion.md. */
                            fprintf(gen->output, "(int)(intptr_t)(");
                            generate_expression(gen, arg);
                            fprintf(gen->output, ")");
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
                    /* Close the argument-temp lifetime wrap, if any
                     * was opened. Emits the per-temp free, the
                     * statement-expression's yield value, and the
                     * closing brace. */
                    if (ad_arg_count > 0) {
                        fprintf(gen->output, "; ");
                        for (int h = 0; h < ad_arg_count; h++) {
                            /* Look up the temp name we registered.
                             * Names are stable across the wrap's
                             * scope. */
                            ASTNode* arg = expr->children[ad_arg_idx[h]];
                            const char* nm = arg_drain_lookup(arg);
                            if (nm) {
                                if (ad_identity[h]) {
                                    /* Return-escape-only param: free the fresh temp
                                     * ONLY if the call did not return it (string_release
                                     * is magic-guarded; the temp is always a magic
                                     * string-op result here, never a literal). */
                                    fprintf(gen->output,
                                            "if ((const char*)_ad_r != %s) string_release(%s); ",
                                            nm, nm);
                                } else {
                                    fprintf(gen->output, "aether_heap_str_free(%s); ", nm);
                                }
                            }
                        }
                        if (ad_have_value) {
                            fprintf(gen->output, "_ad_r; })");
                        } else {
                            /* void parent: the trailing free is the last
                             * statement, so the ({...}) yields void. */
                            fprintf(gen->output, "})");
                        }
                        arg_drain_truncate(ad_saved_count);
                    }
                }
            }
            break;

        case AST_STRING_INTERP: {
            // Children alternate: AST_LITERAL (string) and expression nodes.
            // Two modes:
            //   1. interp_as_printf: emit printf() directly (used by print/println)
            //   2. default: emit snprintf+malloc → returns (void*) heap string (TYPE_PTR)

            // Helper macro: emit the format string for both modes
            #define EMIT_INTERP_FMT() do { \
                for (int i = 0; i < expr->child_count; i++) { \
                    ASTNode* ch = expr->children[i]; \
                    if (ch->type == AST_LITERAL && ch->node_type && ch->node_type->kind == TYPE_STRING) { \
                        const char* s = ch->value ? ch->value : ""; \
                        for (; *s; s++) { \
                            switch (*s) { \
                                case '\n': fprintf(gen->output, "\\n");   break; \
                                case '\t': fprintf(gen->output, "\\t");   break; \
                                case '\r': fprintf(gen->output, "\\r");   break; \
                                case '"':  fprintf(gen->output, "\\\"");  break; \
                                case '%':  fprintf(gen->output, "%%%%");  break; \
                                case '\\': { \
                                    char esc = *(s+1); \
                                    if (esc == 'n')       { fprintf(gen->output, "\\n");   s++; } \
                                    else if (esc == 't')  { fprintf(gen->output, "\\t");   s++; } \
                                    else if (esc == 'r')  { fprintf(gen->output, "\\r");   s++; } \
                                    else if (esc == '\\') { fprintf(gen->output, "\\\\");  s++; } \
                                    else if (esc == '"')  { fprintf(gen->output, "\\\"");  s++; } \
                                    else if (esc == 'x') { \
                                        s += 2; \
                                        int hval = 0, hd = 0; \
                                        while (hd < 2 && ((*s >= '0' && *s <= '9') || \
                                               (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F'))) { \
                                            char hc = *s; \
                                            hval = hval * 16 + (hc >= 'a' ? hc-'a'+10 : hc >= 'A' ? hc-'A'+10 : hc-'0'); \
                                            s++; hd++; \
                                        } \
                                        s--; \
                                        if (hd > 0) fprintf(gen->output, "\\%03o", hval & 0xFF); \
                                        else        fprintf(gen->output, "\\\\x"); \
                                    } else if (esc >= '0' && esc <= '7') { \
                                        s++; \
                                        int oval = esc - '0', od = 1; \
                                        while (od < 3 && *(s+1) >= '0' && *(s+1) <= '7') { \
                                            s++; oval = oval * 8 + (*s - '0'); od++; \
                                        } \
                                        fprintf(gen->output, "\\%03o", oval & 0xFF); \
                                    } else { \
                                        fprintf(gen->output, "\\\\"); \
                                    } \
                                    break; \
                                } \
                                default: \
                                    if ((unsigned char)*s < 0x20 || *s == 0x7F) \
                                        fprintf(gen->output, "\\%03o", (unsigned char)*s); \
                                    else \
                                        fputc(*s, gen->output); \
                                    break; \
                            } \
                        } \
                    } else { \
                        TypeKind tk = (ch->node_type) ? ch->node_type->kind : TYPE_UNKNOWN; \
                        switch (tk) { \
                            case TYPE_INT:    fprintf(gen->output, "%%d");  break; \
                            case TYPE_INT64:  fprintf(gen->output, "%%lld"); break; \
                            case TYPE_UINT64: fprintf(gen->output, "%%llu"); break; \
                            case TYPE_DURATION: fprintf(gen->output, "%%s"); break; \
                            case TYPE_FLOAT:  fprintf(gen->output, "%%g");  break; \
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
                    if (ch->type == AST_LITERAL && ch->node_type && ch->node_type->kind == TYPE_STRING) \
                        continue; \
                    fprintf(gen->output, ", "); \
                    TypeKind tk = ch->node_type ? ch->node_type->kind : TYPE_UNKNOWN; \
                    if (tk == TYPE_BOOL) { \
                        generate_expression(gen, ch); \
                        fprintf(gen->output, " ? \"true\" : \"false\""); \
                    } else if (tk == TYPE_STRING || tk == TYPE_PTR) { \
                        fprintf(gen->output, "_aether_safe_str("); \
                        generate_expression(gen, ch); \
                        fprintf(gen->output, ")"); \
                    } else if (tk == TYPE_INT64) { \
                        fprintf(gen->output, "(long long)"); \
                        generate_expression(gen, ch); \
                    } else if (tk == TYPE_INT) { \
                        /* Aether int is C `int` and %d expects one, but a \
                         * TYPE_INT value can be stored wider: a single-scalar \
                         * message field rides the intptr_t payload slot. Narrow \
                         * to (int) to match %d, as INT64 casts to (long long). \
                         * Only genuine TYPE_INT values reach here (actor-ref / \
                         * ptr fields are TYPE_PTR and print via %s), so no \
                         * pointer is ever truncated. */ \
                        fprintf(gen->output, "(int)"); \
                        generate_expression(gen, ch); \
                    } else if (tk == TYPE_UINT64) { \
                        fprintf(gen->output, "(unsigned long long)"); \
                        generate_expression(gen, ch); \
                    } else if (tk == TYPE_DURATION) { \
                        fprintf(gen->output, "_aether_duration_repr("); \
                        generate_expression(gen, ch); \
                        fprintf(gen->output, ")"); \
                    } else { \
                        generate_expression(gen, ch); \
                    } \
                } \
            } while(0)

            /* A heap-producing CALL segment ("[${string.join(parts, ",")}]")
             * is a temporary nothing owns: bind it to a drained temp so it
             * is freed after the printf / _aether_interp consumes it, or it
             * leaks once per interpolation. Identifiers are never drained
             * (their owner frees them); only calls the classifier proves
             * heap-producing qualify. Same registry-substitution pattern
             * as the closure-env and call-argument drains above. */
            int it_saved = g_arg_drain_count;
            int it_drain_count = 0;
            for (int di = 0; di < expr->child_count; di++) {
                ASTNode* ch = expr->children[di];
                if (!ch || ch->type != AST_FUNCTION_CALL) continue;
                TypeKind tk = ch->node_type ? ch->node_type->kind : TYPE_UNKNOWN;
                if (tk != TYPE_STRING && tk != TYPE_PTR) continue;
                if (!is_heap_string_expr(gen, ch)) continue;
                it_drain_count++;
            }
            if (it_drain_count > 0) {
                fprintf(gen->output, gen->interp_as_printf ? "{ " : "({ ");
                for (int di = 0; di < expr->child_count; di++) {
                    ASTNode* ch = expr->children[di];
                    if (!ch || ch->type != AST_FUNCTION_CALL) continue;
                    TypeKind tk = ch->node_type ? ch->node_type->kind : TYPE_UNKNOWN;
                    if (tk != TYPE_STRING && tk != TYPE_PTR) continue;
                    if (!is_heap_string_expr(gen, ch)) continue;
                    char* nm = arg_drain_mint_name();
                    fprintf(gen->output, "const char* %s = (const char*)(", nm);
                    generate_expression(gen, ch);
                    fprintf(gen->output, "); ");
                    arg_drain_bind(ch, nm);
                }
            }
            if (gen->interp_as_printf) {
                // Mode 1: direct printf (for print/println)
                fprintf(gen->output, "printf(\"");
                EMIT_INTERP_FMT();
                fprintf(gen->output, "\"");
                EMIT_INTERP_ARGS();
                fprintf(gen->output, ")");
            } else {
                // Mode 2: heap-allocated C string — always use portable helper function
                if (it_drain_count > 0) {
                    fprintf(gen->output, "const char* _it_r = ");
                }
                fprintf(gen->output, "_aether_interp(\"");
                EMIT_INTERP_FMT();
                fprintf(gen->output, "\"");
                EMIT_INTERP_ARGS();
                fprintf(gen->output, ")");
            }
            if (it_drain_count > 0) {
                fprintf(gen->output, "; ");
                for (int di = g_arg_drain_count - it_drain_count;
                     di < g_arg_drain_count; di++) {
                    fprintf(gen->output, "aether_heap_str_free(%s); ",
                            g_arg_drain_subs[di].name);
                }
                if (gen->interp_as_printf) {
                    fprintf(gen->output, "}");
                } else {
                    fprintf(gen->output, "_it_r; })");
                }
                arg_drain_truncate(it_saved);
            }

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
                generate_expression(gen, expr->children[i]);
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
            for (int i = 0; i < expr->child_count; i++) {
                ASTNode* fi = expr->children[i];
                if (fi && fi->type == AST_ASSIGNMENT && fi->child_count > 0 &&
                    fi->children[0]->type == AST_IDENTIFIER &&
                    fi->children[0]->value &&
                    is_heap_string_var(gen, fi->children[0]->value)) {
                    any_moved = 1; break;
                }
            }
            /* When a variable is moved into a field, build the struct into a
             * temp inside a statement-expression, clear the moved-from vars'
             * heap flags, then yield the temp. Otherwise emit the literal
             * directly. */
            if (any_moved) fprintf(gen->output, "({ %s _ae_slit = ", expr->value);
            fprintf(gen->output, "(%s){", expr->value);
            int emitted = 0;
            for (int i = 0; i < expr->child_count; i++) {
                ASTNode* field_init = expr->children[i];
                if (field_init && field_init->type == AST_ASSIGNMENT) {
                    if (emitted > 0) fprintf(gen->output, ", ");
                    fprintf(gen->output, ".%s = ", field_init->value);
                    if (field_init->child_count > 0) {
                        generate_expression(gen, field_init->children[0]);
                    }
                    emitted++;
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
                    if (field_init->child_count > 0 &&
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
            if (any_moved) {
                /* #911: disown each moved-from variable so its function-exit
                 * `if (_heap_<v>)` free is a no-op (ownership now in the
                 * struct). Then yield the built struct. */
                fprintf(gen->output, ";");
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
            break;
        }
        
        case AST_ARRAY_ACCESS:
            if (expr->child_count >= 2) {
                /* #1380: a string may be a char* or an AetherString*; index the payload. */
                ASTNode* base = expr->children[0];
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
                                fprintf(gen->output, ", _imsg, current_core_id); }");
                            } else {
                                fprintf(gen->output, "ActorBase* _send_target = ");
                                emit_send_target(gen, target, "ActorBase*");
                                fprintf(gen->output, "; ");
                                fprintf(gen->output, "if (current_core_id >= 0 && current_core_id == _send_target->assigned_core) { ");
                                fprintf(gen->output, "scheduler_send_local(_send_target, _imsg); } else { ");
                                fprintf(gen->output, "scheduler_send_remote(_send_target, _imsg, current_core_id); } }");
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
                                        fprintf(gen->output,
                                                "if (_msg.%s) { "
                                                "size_t _ml = aether_string_length(_msg.%s); "
                                                "_msg.%s = (const char*)string_new_with_length("
                                                "aether_string_data(_msg.%s), (int)_ml); "
                                                "} ",
                                                f->name, f->name, f->name, f->name);
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

                        int timeout_ms = 5000;
                        if (expr->child_count >= 3 && expr->children[2] &&
                            expr->children[2]->value) {
                            timeout_ms = atoi(expr->children[2]->value);
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
                                fprintf(gen->output,
                                        "if (_msg.%s) { "
                                        "size_t _ml = aether_string_length(_msg.%s); "
                                        "_msg.%s = (const char*)string_new_with_length("
                                        "aether_string_data(_msg.%s), (int)_ml); "
                                        "} ",
                                        f->name, f->name, f->name, f->name);
                            }
                        }

                        fprintf(gen->output, "void* _ask_r = scheduler_ask_message(");
                        emit_send_target(gen, target, "ActorBase*");
                        fprintf(gen->output, ", &_msg, sizeof(%s), %d); ", message->value, timeout_ms);

                        if (reply_msg_name && reply_field) {
                            const char* c_type = "int";
                            const char* c_zero = "0";
                            switch (reply_field_type) {
                                case TYPE_FLOAT:   c_type = "double"; c_zero = "0.0"; break;
                                case TYPE_LONGDOUBLE: c_type = "long double"; c_zero = "0.0L"; break;
                                case TYPE_BOOL:    c_type = "int";    c_zero = "0";   break;
                                case TYPE_STRING:  c_type = "const char*"; c_zero = "NULL"; break;
                                case TYPE_INT64:   c_type = "int64_t"; c_zero = "0";  break;
                                case TYPE_UINT64:  c_type = "uint64_t"; c_zero = "0"; break;
                                case TYPE_DURATION: c_type = "int64_t"; c_zero = "0"; break;
                                case TYPE_PTR:     c_type = "void*";  c_zero = "NULL"; break;
                                default:           c_type = "int";    c_zero = "0";   break;
                            }
                            fprintf(gen->output, "%s _ask_val = _ask_r ? ((%s*)_ask_r)->%s : %s; ",
                                    c_type, reply_msg_name, reply_field, c_zero);
                            fprintf(gen->output, "free(_ask_r); _ask_val; })");
                        } else if (reply_scalar_kind != TYPE_UNKNOWN) {
                            /* Expression reply (#1324): the handler sent a
                             * typed copy; deref the buffer as that type.
                             * Keep this switch in sync with the
                             * AST_REPLY_STATEMENT scalar emission. */
                            const char* c_type = "int";
                            const char* c_zero = "0";
                            switch (reply_scalar_kind) {
                                case TYPE_FLOAT:      c_type = "double"; c_zero = "0.0"; break;
                                case TYPE_LONGDOUBLE: c_type = "long double"; c_zero = "0.0L"; break;
                                case TYPE_BOOL:       c_type = "int"; c_zero = "0"; break;
                                case TYPE_INT64:      c_type = "int64_t"; c_zero = "0"; break;
                                case TYPE_UINT64:     c_type = "uint64_t"; c_zero = "0"; break;
                                case TYPE_DURATION:   c_type = "int64_t"; c_zero = "0"; break;
                                case TYPE_PTR:        c_type = "void*"; c_zero = "NULL"; break;
                                case TYPE_STRING:     c_type = "const char*"; c_zero = "NULL"; break;
                                default:              c_type = "int"; c_zero = "0"; break;
                            }
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
                        // MSVC: use _aether_ask helper + compound literal
                        gen->ask_temp_counter++;
                        fprintf(gen->output, "_aether_ask_helper(");
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
                            fprintf(gen->output, "offsetof(%s, %s), sizeof(", reply_msg_name, reply_field);
                            // Emit the field type size based on reply_field_type
                            switch (reply_field_type) {
                                case TYPE_FLOAT:   fprintf(gen->output, "double"); break;
                                case TYPE_LONGDOUBLE: fprintf(gen->output, "long double"); break;
                                case TYPE_INT64:   fprintf(gen->output, "int64_t"); break;
                                case TYPE_UINT64:  fprintf(gen->output, "uint64_t"); break;
                                case TYPE_DURATION: fprintf(gen->output, "int64_t"); break;
                                case TYPE_PTR:     fprintf(gen->output, "void*"); break;
                                case TYPE_STRING:  fprintf(gen->output, "const char*"); break;
                                default:           fprintf(gen->output, "int"); break;
                            }
                            fprintf(gen->output, "))");
                        } else if (reply_scalar_kind != TYPE_UNKNOWN) {
                            fprintf(gen->output, "0, sizeof(");
                            switch (reply_scalar_kind) {
                                case TYPE_FLOAT:      fprintf(gen->output, "double"); break;
                                case TYPE_LONGDOUBLE: fprintf(gen->output, "long double"); break;
                                case TYPE_INT64:      fprintf(gen->output, "int64_t"); break;
                                case TYPE_UINT64:     fprintf(gen->output, "uint64_t"); break;
                                case TYPE_DURATION:   fprintf(gen->output, "int64_t"); break;
                                case TYPE_PTR:        fprintf(gen->output, "void*"); break;
                                case TYPE_STRING:     fprintf(gen->output, "const char*"); break;
                                default:              fprintf(gen->output, "int"); break;
                            }
                            fprintf(gen->output, "))");
                        } else {
                            fprintf(gen->output, "0, sizeof(int))");
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
                fprintf(gen->output, "({ _closure_env_%d* _e = malloc(sizeof(_closure_env_%d)); _e->_dtor = _closure_env_%d_free; ", id, id, id);
                for (int i = 0; i < cap_count; i++) {
                    if (capture_is_retained_string(gen, captures[i], cl_parent_func)) {
                        /* env owns a reference — see aether_str_capture preamble */
                        const char* ctype = lookup_var_c_type(gen, captures[i], cl_parent_func);
                        fprintf(gen->output, "_e->%s = (%s)aether_str_capture(%s); ",
                                captures[i], ctype, captures[i]);
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
