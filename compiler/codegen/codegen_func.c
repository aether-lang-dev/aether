#include "codegen_internal.h"

// True when this function definition is annotated `@c_callback`.
// The annotation (#235) marks a function as having a stable, externally-
// visible C symbol so it can be passed across module boundaries to C
// externs that take function pointers (HTTP route handlers, signal
// handlers, qsort comparators, libcurl callbacks).
int is_c_callback(ASTNode* func) {
    return func && func->annotation &&
           strncmp(func->annotation, "c_callback:", 11) == 0;
}

/* Imported functions are private to each translation unit so linkers that
   reject duplicate symbols (macOS ld64) do not see one copy per importer.
   Trailing-underscore names are the file-local convention (#279).
   @c_callback overrides both: that symbol must stay externally addressable.

   Top-level functions keep external linkage. Making them static would also
   have solved the libaether collisions in #1366, but it breaks the #703
   guarantee that an Aether-owned definition matches a force-included C
   header, so collisions are resolved by renaming instead (see
   rename_extern_colliding_functions). */
int fn_has_internal_linkage(ASTNode* func) {
    if (!func || is_c_callback(func)) return 0;
    if (func->is_imported) return 1;
    if (func->value) {
        size_t n = strlen(func->value);
        if (n > 0 && func->value[n - 1] == '_') return 1;   /* #279 */
    }
    return 0;
}

/* #2123: a small, loop-free, non-recursive function with internal
 * linkage is emitted `static inline`. Every Aether function reached C as
 * plain `static`, and at -O2 gcc's budget for a function not declared
 * inline (max-inline-insns-auto, 15 insns) left the small value-returning
 * maths of a physics engine — a 3x3 product, a quaternion rotation, a
 * 3x3 solve — out of line: 1.6x on a joint-grid solve against the same
 * functions marked inline. `inline` on a static function is a hint that
 * raises the budget to max-inline-insns-single (200); the function keeps
 * its address and its semantics. Only internal-linkage functions
 * qualify: `inline` on an external C99 function changes what the
 * translation unit must provide. */
static int fn_body_is_small_leaf(ASTNode* n, const char* self, int* budget) {
    if (!n) return 1;
    if (n->type == AST_WHILE_LOOP || n->type == AST_FOR_LOOP || n->type == AST_CLOSURE ||
        n->type == AST_MATCH_STATEMENT) return 0;
    if (n->type == AST_FUNCTION_CALL && n->value && self && strcmp(n->value, self) == 0) return 0;
    if (--*budget < 0) return 0;
    for (int i = 0; i < n->child_count; i++) {
        if (!fn_body_is_small_leaf(n->children[i], self, budget)) return 0;
    }
    return 1;
}

static int fn_body_inlinable(ASTNode* func) {
    if (!func || func->child_count == 0) return 0;
    ASTNode* body = func->children[func->child_count - 1];
    if (!body || body->type != AST_BLOCK) return 0;
    int budget = 160;   /* AST nodes — a 3x3 matrix product is ~120 */
    return fn_body_is_small_leaf(body, func->value, &budget);
}

int fn_is_inline_candidate(ASTNode* func) {
    if (!func || !fn_has_internal_linkage(func) || is_c_callback(func)) return 0;
    return fn_body_inlinable(func);
}

// Returns the C symbol bound by a `@c_callback` annotation.
//   `@c_callback("name") foo(...)` — uses "name" verbatim.
//   `@c_callback foo(...)`         — falls back to func->value, which
//                                    after the module merger is the
//                                    namespace-prefixed form (e.g.
//                                    `vcr_dispatch`). That's the safe
//                                    default — no symbol collisions
//                                    between modules — and callers who
//                                    want an unmangled symbol opt in
//                                    explicitly with the parenthesised
//                                    form.
// Returns NULL when the function is not @c_callback-annotated.
const char* c_callback_symbol(ASTNode* func) {
    if (!is_c_callback(func)) return NULL;
    const char* tag = func->annotation + 11;
    return (tag && tag[0]) ? tag : (func->value ? func->value : NULL);
}

/* #2664: the @c_callback definition behind the function `fn`: `fn` itself
 * when annotated, or, for a function written as several clauses, the first
 * annotated clause, which binds the symbol of the whole set (its
 * dispatcher's); NULL when none is. */
ASTNode* fn_c_callback_def(CodeGenerator* gen, ASTNode* fn) {
    const DefClauses* dc = gen ? fn_def_clause_set(gen->program, fn) : NULL;
    if (!dc) return is_c_callback(fn) ? fn : NULL;
    for (int c = 0; c < dc->count; c++)
        if (is_c_callback(dc->nodes[c])) return dc->nodes[c];
    return NULL;
}

// Look up a top-level @c_callback function by its current AST value
// (post-merge: the prefixed `<ns>_<name>` form for imported callbacks;
// the bare name for in-file ones) and return the C symbol it's bound
// to. Returns NULL when no such callback exists, so the caller can
// fall through to the default identifier-emission path.
//
// #2007: through the program index. This is asked for every identifier
// emitted, and a scan of the top level each time made codegen quadratic
// in the number of functions.
const char* lookup_c_callback_symbol(CodeGenerator* gen, const char* name) {
    if (!gen || !gen->program || !name) return NULL;
    ProgramIndex* ix = program_index(gen->program);
    return ix ? strmap_get(&ix->c_callbacks, name) : NULL;
}

// The C symbol bound by `@extern("c_symbol")`, interned (whole however
// long, #2539), or NULL when `ext` was not declared via `@extern`
// (annotation begins with "c_symbol:"). The stored annotation is
// `c_symbol:NAME` for a plain `@extern` and `c_symbol:NAME;varargs`
// when the `@extern` declaration also carries a trailing `...`; the
// `;` delimiter never occurs inside a C identifier, so only NAME is
// taken regardless.
const char* extern_c_symbol(const ASTNode* ext) {
    if (!ext || !ext->annotation) return NULL;
    if (strncmp(ext->annotation, "c_symbol:", 9) != 0) return NULL;
    const char* s = ext->annotation + 9;
    const char* semi = strchr(s, ';');
    size_t n = semi ? (size_t)(semi - s) : strlen(s);
    return cg_intern_n(s, n);
}

// Is `ext` a variadic extern? True for the bare
// `extern foo(fmt: string, ...)` form and for the
// `@extern("sym") foo(fmt: string, ...)` form (annotation
// "c_symbol:NAME;varargs"). Delimiter-aware, so a C symbol that merely
// contains the substring "varargs" can never be misread as variadic.
static int extern_is_varargs(const ASTNode* ext) {
    if (!ext) return 0;
    return annotation_has_marker(ext->annotation, "varargs");
}

// `@c_import` (#1239): the prototype belongs to a C header, so emit none.
static int extern_is_c_import(const ASTNode* ext) {
    if (!ext) return 0;
    return annotation_has_marker(ext->annotation, "c_import");
}

// Register an extern function's parameter types for call-site cast emission.
// Called whenever generate_extern_declaration() processes a function.
void register_extern_func(CodeGenerator* gen, ASTNode* ext) {
    if (!ext || !ext->value) return;

    // Grow registry if needed
    if (gen->extern_registry_count >= gen->extern_registry_capacity) {
        int new_cap = gen->extern_registry_capacity * 2 + 8;
        void* new_reg = realloc(gen->extern_registry,
            (size_t)new_cap * sizeof(*gen->extern_registry));
        if (!new_reg) return;  // OOM: skip registration
        gen->extern_registry = new_reg;
        gen->extern_registry_capacity = new_cap;
    }

    int idx = gen->extern_registry_count++;
    gen->extern_registry[idx].name = strdup(ext->value);
    gen->extern_registry[idx].c_name = NULL;
    // @extern("c_symbol") rebinds the call-site emission to a chosen
    // C symbol while keeping the Aether-side name in the namespace.
    {
        const char* c_sym = extern_c_symbol(ext);
        if (c_sym) {
            gen->extern_registry[idx].c_name = strdup(c_sym);
        }
    }
    gen->extern_registry[idx].param_count = ext->child_count;
    gen->extern_registry[idx].ret_type = ext->node_type;
    gen->extern_registry[idx].params = NULL;
    gen->extern_registry[idx].param_full = NULL;
    gen->extern_registry[idx].params_aether = NULL;
    gen->extern_registry[idx].params_retain = NULL;
    gen->extern_registry[idx].params_noescape = NULL;

    if (ext->child_count > 0) {
        gen->extern_registry[idx].params = malloc(ext->child_count * sizeof(TypeKind));
        /* Param annotations are stored as a comma-separated set on
         * `param->annotation` so multiple stack on one slot
         * (`@aether @retain string`). Test for an individual tag
         * via substring match. The lazy-allocation pattern below
         * keeps the per-extern parallel arrays NULL in the common
         * (no-annotation) case. */
        int any_aether = 0, any_retain = 0, any_noescape = 0;
        for (int i = 0; i < ext->child_count; i++) {
            ASTNode* param = ext->children[i];
            if (!param || !param->annotation) continue;
            if (strstr(param->annotation, "aether_param")) any_aether = 1;
            if (strstr(param->annotation, "retain_param")) any_retain = 1;
            if (strstr(param->annotation, "noescape_param")) any_noescape = 1;
        }
        if (any_aether) {
            gen->extern_registry[idx].params_aether =
                calloc(ext->child_count, sizeof(int));
        }
        if (any_retain) {
            gen->extern_registry[idx].params_retain =
                calloc(ext->child_count, sizeof(int));
        }
        if (any_noescape) {
            gen->extern_registry[idx].params_noescape =
                calloc(ext->child_count, sizeof(int));
        }
        gen->extern_registry[idx].param_full =
            calloc(ext->child_count, sizeof(Type*));
        for (int i = 0; i < ext->child_count; i++) {
            ASTNode* param = ext->children[i];
            if (param && param->node_type) {
                gen->extern_registry[idx].params[i] = param->node_type->kind;
                if (gen->extern_registry[idx].param_full) {
                    gen->extern_registry[idx].param_full[i] = param->node_type;
                }
            } else {
                gen->extern_registry[idx].params[i] = TYPE_UNKNOWN;
            }
            if (param && param->annotation) {
                if (gen->extern_registry[idx].params_aether &&
                    strstr(param->annotation, "aether_param")) {
                    gen->extern_registry[idx].params_aether[i] = 1;
                }
                if (gen->extern_registry[idx].params_retain &&
                    strstr(param->annotation, "retain_param")) {
                    gen->extern_registry[idx].params_retain[i] = 1;
                }
                if (gen->extern_registry[idx].params_noescape &&
                    strstr(param->annotation, "noescape_param")) {
                    gen->extern_registry[idx].params_noescape[i] = 1;
                }
            }
        }
    }
}

// Check if a function name is registered as a builder function. The
// registry holds module-qualified names with dots as underscores.
int is_builder_func_reg(CodeGenerator* gen, const char* func_name) {
    if (!gen || !func_name) return 0;
    const char* normalized = codegen_normalise_callee(func_name);
    for (int i = 0; i < gen->builder_func_reg_count; i++) {
        if (gen->builder_funcs_reg[i].name && strcmp(gen->builder_funcs_reg[i].name, normalized) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Find a user-fn definition by name in the program AST. Helper for
 * the bare-fn-adapter discovery pre-pass. */
static ASTNode* find_user_function_by_name(CodeGenerator* gen, const char* name) {
    if (!gen || !gen->program || !name) return NULL;
    ASTNode* direct = find_function_definition_by_name(gen->program, name);
    if (direct) return direct;
    /* #940: a qualified call `mod.fn(...)` keeps its dotted name on the AST,
     * but the merged definition is `<namespace>_fn` — where the namespace is
     * the LAST segment of the module path (module_get_namespace's rule:
     * `std.foo.bar.fn` → namespace `bar` → `bar_fn`). Reconstruct that
     * merged form and retry, so cross-module callees resolve here. Without
     * it the bare-fn-arg discovery below never finds the callee, the
     * fn-typed param is never inspected, and the env-ignoring adapter for a
     * bare function passed across the import boundary is never emitted
     * (`_aether_bare_adapter_<name>' undeclared` in the caller's TU). */
    const char* last_dot = strrchr(name, '.');
    if (last_dot && last_dot != name) {
        const char* fn = last_dot + 1;
        /* Namespace = last path segment before the function name. */
        const char* ns_start = name;
        for (const char* p = name; p < last_dot; p++) {
            if (*p == '.') ns_start = p + 1;
        }
        size_t ns_len = (size_t)(last_dot - ns_start);
        if (ns_len > 0) {
            const char* merged = cg_internf("%.*s_%s", (int)ns_len, ns_start, fn);
            for (int i = 0; i < gen->program->child_count; i++) {
                ASTNode* c = gen->program->children[i];
                if (c && (c->type == AST_FUNCTION_DEFINITION ||
                          c->type == AST_BUILDER_FUNCTION) &&
                    c->value && strcmp(c->value, merged) == 0) {
                    return c;
                }
            }
        }
    }
    return NULL;
}

/* Pre-walk the AST registering bare-fn adapters for every coercion site
 * where a bare named function is wrapped as `(_AeClosure){.fn=name,
 * .env=NULL}`. This mirrors the registration that happens lazily at
 * the wrap-site codegen — doing it eagerly lets emit_bare_fn_adapters
 * run before main is emitted, so adapter forward declarations are
 * visible to user code that calls through them.
 *
 * The mirror is conservative: we register any bare-fn identifier
 * appearing as a call argument where the callee has a `fn`-typed
 * param, as the RHS of an `=` assignment to a `ptr`-typed
 * struct field, or in an `_aether_box_closure(...)` wrap position
 * (which the call-site arg coercion also handles). False positives
 * are harmless — they just emit an unused adapter the C compiler
 * inlines / discards. */
static void discover_bare_fn_adapters_walk(CodeGenerator* gen, ASTNode* node) {
    if (!gen || !node) return;
    /* AST_FUNCTION_CALL: register any bare-fn arg whose callee param
     * is `fn`-typed. */
    if (node->type == AST_FUNCTION_CALL && node->value && gen->program) {
        /* box_closure is a builtin, so it never resolves as a user
         * function below; register its bare-fn argument here or the
         * adapter the wrap site emits is never declared. */
        if (strcmp(node->value, "box_closure") == 0 && node->child_count == 1) {
            ASTNode* arg = node->children[0];
            if (arg && arg->type == AST_IDENTIFIER && arg->value &&
                find_user_function_by_name(gen, arg->value)) {
                register_bare_fn_adapter(gen, arg->value);
            }
        }
        ASTNode* callee = find_user_function_by_name(gen, node->value);
        if (callee) {
            int pi = 0;
            for (int j = 0; j < callee->child_count; j++) {
                ASTNode* p = callee->children[j];
                if (!p || p->type == AST_GUARD_CLAUSE || p->type == AST_BLOCK) continue;
                /* Map param index to call-arg index. Skip _ctx
                 * auto-injection — the user's child index 0 maps to
                 * the callee's first non-_ctx param. */
                if (p->node_type && p->node_type->kind == TYPE_FUNCTION &&
                    !p->node_type->is_fnptr &&
                    pi < node->child_count) {
                    ASTNode* arg = node->children[pi];
                    if (arg && arg->type == AST_IDENTIFIER && arg->value) {
                        if (find_user_function_by_name(gen, arg->value)) {
                            register_bare_fn_adapter(gen, arg->value);
                        }
                    }
                }
                pi++;
            }
        }
    }
    /* #2055: an identifier the typechecker marked as a function value. */
    if (node->type == AST_IDENTIFIER && node->value && node->annotation &&
        strcmp(node->annotation, "fn_value") == 0 &&
        find_user_function_by_name(gen, node->value)) {
        register_bare_fn_adapter(gen, node->value);
    }
    /* AST_BINARY_EXPRESSION with op="=": register bare-fn RHS into a
     * ptr-typed LHS struct field. Conservative: also register for any
     * RHS that's a bare named function regardless of LHS — false
     * positives just emit unused adapters. */
    if (node->type == AST_BINARY_EXPRESSION && node->value &&
        strcmp(node->value, "=") == 0 && node->child_count == 2) {
        ASTNode* lhs = node->children[0];
        ASTNode* rhs = node->children[1];
        if (lhs && rhs && rhs->type == AST_IDENTIFIER && rhs->value &&
            lhs->node_type && lhs->node_type->kind == TYPE_PTR) {
            if (find_user_function_by_name(gen, rhs->value)) {
                register_bare_fn_adapter(gen, rhs->value);
            }
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        discover_bare_fn_adapters_walk(gen, node->children[i]);
    }
}

void discover_bare_fn_adapters(CodeGenerator* gen) {
    if (!gen || !gen->program) return;
    discover_bare_fn_adapters_walk(gen, gen->program);
}

/* Register `bare_fn_name` as needing an env-ignoring adapter emitted at
 * file finalisation. Returns 1 if newly added, 0 if it was already
 * registered. Idempotent / deduping — multiple wrap sites for the same
 * bare fn share one adapter. */
int register_bare_fn_adapter(CodeGenerator* gen, const char* bare_fn_name) {
    if (!gen || !bare_fn_name) return 0;
    for (int i = 0; i < gen->bare_fn_adapter_count; i++) {
        if (strcmp(gen->bare_fn_adapter_names[i], bare_fn_name) == 0) return 0;
    }
    if (gen->bare_fn_adapter_count >= gen->bare_fn_adapter_capacity) {
        int new_cap = gen->bare_fn_adapter_capacity ? gen->bare_fn_adapter_capacity * 2 : 8;
        char** nn = realloc(gen->bare_fn_adapter_names, sizeof(char*) * new_cap);
        if (!nn) return 0;
        gen->bare_fn_adapter_names = nn;
        gen->bare_fn_adapter_capacity = new_cap;
    }
    gen->bare_fn_adapter_names[gen->bare_fn_adapter_count++] = strdup(bare_fn_name);
    return 1;
}

/* Resolve a registered bare-fn name to its definition and emit the adapter
 * signature `static R _aether_bare_adapter_<name>(void* _env, P0 _a0, ...)`.
 * Returns the resolved fdef (and fills params/param_count/ret_c) so the
 * caller can either terminate with `;` (forward decl) or `{ ... }` (body).
 * Returns NULL when the bare fn can't be resolved (caller skips it). */
static ASTNode* emit_bare_fn_adapter_signature(CodeGenerator* gen,
                                               const char* fname,
                                               ASTNode** params,
                                               int* param_count,
                                               const char** ret_c_out) {
    ASTNode* fdef = NULL;
    for (int j = 0; j < gen->program->child_count; j++) {
        ASTNode* c = gen->program->children[j];
        if (c && (c->type == AST_FUNCTION_DEFINITION ||
                  c->type == AST_BUILDER_FUNCTION) &&
            c->value && strcmp(c->value, fname) == 0) {
            fdef = c;
            break;
        }
    }
    if (!fdef) return NULL;
    /* The parameters and result as the function's prototype spells them,
     * a clause set's decided over its clauses (#2644, #2645). */
    int pc = 0;
    for (int k = 0; k < fn_param_count(fdef) && pc < 16; k++) params[pc++] = fn_param_at(fdef, k);
    *param_count = pc;
    Type* rt = fn_result_type(gen, fdef);
    const char* ret_c = rt ? get_c_type(rt) : "void";
    *ret_c_out = ret_c;
    fprintf(gen->output, "static %s _aether_bare_adapter_%s(void* _env",
            ret_c, fname);
    for (int k = 0; k < pc; k++) {
        Type* pt = fn_param_type_at(gen, fdef, k);
        fprintf(gen->output, ", %s _a%d", pt ? get_c_type(pt) : "int", k);
    }
    fprintf(gen->output, ")");
    return fdef;
}

/* #2586: named functions used as values (a bare name that is not a call),
 * found by a walk over the whole program before anything is classified or
 * emitted.
 *
 * A function pointer typed `fn(...) -> string` may point at an Aether
 * function or at C, and a call through it cannot tell which, nor which
 * return of an Aether function ran. Since 0.792 a `string` function
 * returning a field returns a copy of its own, so taking every result as
 * borrowed leaked one string per call (ae3d's component getters, called as
 * `setter(dst, getter(src))`), while taking every result as owned frees a
 * string C keeps (strerror's static buffer). So the call decides at run
 * time: a function used as a value that hands over owned strings marks the
 * one it returns (aether_fnptr_give, at its uniform-heap returns), and the
 * call takes its result as owned when it is the marked pointer, copying
 * anything else (aether_fnptr_take). A C function never marks, whatever
 * way its pointer arrived (cast from a raw `ptr`, an extern's result, a
 * field of C memory, an extern named as a value), and a function handed to
 * C is the function itself.
 *
 * The arguments follow the closure convention, slot by slot: a call through
 * such a pointer frees an owned string argument after the call, and a
 * parameter passed on through one is not kept by passing it, when no
 * function used as a value keeps what it receives at that slot (a named
 * function keeps a `string` parameter only through a reference of its own,
 * unless it stores it where nothing releases it; a `ptr` one, which a
 * string can reach, is kept by any escape). A parameter returned as it came
 * is no keep there: the call copies an unmarked result before it frees its
 * arguments. A C function behind the pointer takes its `string` arguments
 * as an extern's are taken, borrowed. */
static char** g_fnval_names = NULL;
static int g_fnval_count = 0;
static int g_fnval_cap = 0;
static unsigned long long g_fnptr_args_borrowed = 0;   /* bit k: slot k borrows */

static void fnval_add(const char* name) {
    for (int i = 0; i < g_fnval_count; i++)
        if (strcmp(g_fnval_names[i], name) == 0) return;
    if (g_fnval_count == g_fnval_cap) {
        g_fnval_cap = g_fnval_cap ? g_fnval_cap * 2 : 16;
        g_fnval_names = (char**)aether_xrealloc(g_fnval_names, sizeof(char*) * (size_t)g_fnval_cap);
    }
    g_fnval_names[g_fnval_count++] = strdup(name);
}

/* The parameters in scope during the walk: a function's or a closure's
 * parameter shadows a function of its name in that body (std.mem's
 * `steady_growth(round: fn, ...)` names no user function `round`). */
#define FNVAL_SHADOW_MAX 256
static const char* g_fnval_shadow[FNVAL_SHADOW_MAX];
static int g_fnval_shadow_count = 0;

static int fnval_shadowed(const char* name) {
    for (int i = g_fnval_shadow_count - 1; i >= 0; i--)
        if (strcmp(g_fnval_shadow[i], name) == 0) return 1;
    return 0;
}

/* A name in value position that names a function, and no parameter in
 * scope. A local spelt like a function is still taken: whether it is in
 * scope at the use is the checker's to say, and taking a function that is
 * not a value only costs it a mark and the argument slots it keeps. */
static void discover_fn_values_walk(CodeGenerator* gen, ASTNode* n) {
    if (!n) return;
    if (n->type == AST_IDENTIFIER && n->value && !fnval_shadowed(n->value)) {
        ASTNode* fd = find_function_definition_by_name(gen->program, n->value);
        if (fd && fd->type == AST_FUNCTION_DEFINITION) fnval_add(n->value);
    }
    int pushed = 0;
    if (n->type == AST_FUNCTION_DEFINITION || n->type == AST_BUILDER_FUNCTION ||
        n->type == AST_CLOSURE) {
        for (int i = 0; i < n->child_count; i++) {
            ASTNode* p = n->children[i];
            if (p && p->value && g_fnval_shadow_count < FNVAL_SHADOW_MAX &&
                (p->type == AST_PATTERN_VARIABLE || p->type == AST_CLOSURE_PARAM ||
                 (p->type == AST_VARIABLE_DECLARATION && n->type != AST_CLOSURE))) {
                g_fnval_shadow[g_fnval_shadow_count++] = p->value;
                pushed++;
            }
        }
    }
    for (int i = 0; i < n->child_count; i++) discover_fn_values_walk(gen, n->children[i]);
    g_fnval_shadow_count -= pushed;
}

void discover_fn_values(CodeGenerator* gen) {
    for (int i = 0; i < g_fnval_count; i++) free(g_fnval_names[i]);
    g_fnval_count = 0;
    /* Until compute_fnptr_args_borrowed runs, an argument through a fn
     * pointer escapes (the closure analyses before it see that). */
    g_fnptr_args_borrowed = 0;
    g_fnval_shadow_count = 0;
    if (!gen || !gen->program) return;
    discover_fn_values_walk(gen, gen->program);
}

/* Is `name` a function used as a typed fn-pointer value (discover_fn_values)? */
int fn_value_name(const char* name) {
    if (!name) return 0;
    for (int i = 0; i < g_fnval_count; i++)
        if (strcmp(g_fnval_names[i], name) == 0) return 1;
    return 0;
}

/* Parameter `k` of some clause of `name` (one, or each of a set written as
 * clauses, #2627): its node, or NULL when it is a pattern that binds no
 * name (a literal compared, never kept). */
static ASTNode* fnval_clause_param(CodeGenerator* gen, const char* name, int clause, int k) {
    const DefClauses* dc = program_index_clauses(gen->program, name);
    ASTNode* fd = dc && clause < dc->count ? dc->nodes[clause] : NULL;
    if (!fd || k >= fd->child_count) return NULL;
    ASTNode* p = fd->children[k];
    return p && (p->type == AST_PATTERN_VARIABLE || p->type == AST_VARIABLE_DECLARATION) &&
           p->value ? p : NULL;
}

/* Does the function `name`, called through a pointer, keep what it receives
 * at parameter `k`, under the current slot answer? */
static int fnval_keeps_slot(CodeGenerator* gen, const char* name, int k) {
    const DefClauses* dc = program_index_clauses(gen->program, name);
    int n = dc ? dc->count : 0;
    int any_string = 0, any_pointer = 0;
    for (int c = 0; c < n; c++) {
        ASTNode* p = fnval_clause_param(gen, name, c, k);
        if (!p || !p->node_type) continue;
        if (p->node_type->kind == TYPE_STRING) any_string = 1;
        else if (param_may_hold_caller_string(p->node_type)) any_pointer = 1;
    }
    /* A `string` parameter: kept unless the function takes a reference of
     * its own; a return of it keeps nothing through a pointer. The walks
     * read every clause (#2627), each deciding copy-on-keep for itself
     * (#2644). */
    if (any_string && callee_string_param_kept_as_given(gen, name, k, 0)) return 1;
    return any_pointer && callee_param_escapes_via_body(gen, name, k, 0);
}

/* The walks assume the answer for calls between fn pointers (an argument
 * passed on through one at a borrowing slot is not kept), so it is iterated
 * to a fixed point: start from every slot borrowing, drop each slot some
 * function keeps at under the current answer, and ask again until nothing
 * drops. Then no function keeps at a slot left borrowing, by induction, as
 * for closures (compute_closure_args_borrowed). A slot is a parameter's
 * position, a literal pattern's included, as the C signature counts them;
 * a list pattern's two C parameters end the analysis there (every later
 * slot escapes). */
void compute_fnptr_args_borrowed(CodeGenerator* gen) {
    g_fnptr_args_borrowed = 0;
    if (!gen || !gen->program || g_fnval_count == 0) return;
    unsigned long long mask = ~0ULL;
    for (;;) {
        g_fnptr_args_borrowed = mask;
        callee_memo_clear(gen);
        unsigned long long next = mask;
        for (int i = 0; i < g_fnval_count; i++) {
            const char* name = g_fnval_names[i];
            ASTNode* fd = find_function_definition_by_name(gen->program, name);
            for (int k = 0; fd && k < fd->child_count && k < 64; k++) {
                ASTNode* p = fd->children[k];
                if (!p || p->type == AST_GUARD_CLAUSE || p->type == AST_BLOCK ||
                    p->type == AST_REQUIRES_CLAUSE || p->type == AST_ENSURES_CLAUSE) continue;
                if (p->type == AST_PATTERN_LIST || p->type == AST_PATTERN_CONS) {
                    next &= (k == 0) ? 0ULL : ((1ULL << k) - 1ULL);
                    break;
                }
                unsigned long long bit = 1ULL << k;
                if ((next & bit) && fnval_keeps_slot(gen, name, k)) next &= ~bit;
            }
        }
        if (next == mask) break;
        mask = next;
    }
    g_fnptr_args_borrowed = mask;
    callee_memo_clear(gen);
}

/* Does an argument at `slot` of a call through a typed fn pointer stay the
 * caller's (compute_fnptr_args_borrowed)? */
int fnptr_arg_borrowed(int slot) {
    return slot >= 0 && slot < 64 && ((g_fnptr_args_borrowed >> slot) & 1ULL);
}

/* #943: emit FORWARD DECLARATIONS for every registered bare-fn adapter.
 * Closure bodies (emitted before emit_bare_fn_adapters) may reference an
 * adapter, so its prototype must be in scope by then — otherwise the closure
 * function sees `_aether_bare_adapter_<name>' undeclared'. Emitted right
 * before the closure definitions; the full bodies still come later. */
void emit_bare_fn_adapter_decls(CodeGenerator* gen) {
    if (!gen || gen->bare_fn_adapter_count == 0 || !gen->program) return;
    print_line(gen, "// Bare-fn adapter forward declarations (closures may call them)");
    for (int i = 0; i < gen->bare_fn_adapter_count; i++) {
        ASTNode* params[16];
        int param_count = 0;
        const char* ret_c = "void";
        if (emit_bare_fn_adapter_signature(gen, gen->bare_fn_adapter_names[i],
                                           params, &param_count, &ret_c)) {
            fprintf(gen->output, ";\n");
        }
    }
}

/* Emit `_aether_bare_adapter_<name>(void* env, args) -> R` for every
 * registered bare-fn name. The adapter ignores env and forwards to the
 * bare fn with its real C signature. Looks up each bare fn in the
 * program AST to derive its param + return type list; for fns that
 * can't be resolved (shouldn't happen — registration only fires from
 * the bare-fn coercion path which already confirms the def exists),
 * a no-op stub is emitted so the codegen isn't blocked. Must be called
 * AFTER all user fn forward decls and bodies — the adapter calls into
 * the bare fn by its real C name. */
void emit_bare_fn_adapters(CodeGenerator* gen) {
    if (!gen || gen->bare_fn_adapter_count == 0) return;
    if (!gen->program) return;
    print_line(gen, "// Bare-fn → fn-typed-slot env-ignoring adapters (ASK 3)");
    for (int i = 0; i < gen->bare_fn_adapter_count; i++) {
        const char* fname = gen->bare_fn_adapter_names[i];
        ASTNode* params[16];
        int param_count = 0;
        const char* ret_c = "void";
        ASTNode* fdef = emit_bare_fn_adapter_signature(gen, fname, params,
                                                       &param_count, &ret_c);
        if (!fdef) continue;  /* Shouldn't happen; registration gate
                               * already confirmed existence. */
        Type* rt = fn_result_type(gen, fdef);
        /* A string result is handed over owned, as a closure's is (#2054):
         * the caller of a fn value cannot tell which return sites of the
         * function behind it are heap, so it frees every string result.
         * Wrapped the way the function's own callers see it -- passed
         * through when the function returns owned strings, copied when it
         * returns a literal or a borrow. */
        int owned_string = rt && rt->kind == TYPE_STRING;
        fprintf(gen->output, " {\n    (void)_env;\n    ");
        if (rt) fprintf(gen->output, "return ");
        if (owned_string) fprintf(gen->output, "aether_uniform_heap_str((const char*)(");
        /* A @c_callback function is its bound symbol (#2664). */
        ASTNode* cb_def = fn_c_callback_def(gen, fdef);
        fprintf(gen->output, "%s(", cb_def ? c_callback_symbol(cb_def) : safe_c_name(fname));
        /* #2499: a closure call borrows its arguments. A `string` the
         * function keeps is its own reference, taken by its body (each
         * clause's, #2644) on entry (fn_def_string_param_captures), so the
         * adapter passes the caller's argument as it is. */
        for (int k = 0; k < param_count; k++) {
            if (k > 0) fprintf(gen->output, ", ");
            fprintf(gen->output, "_a%d", k);
        }
        fprintf(gen->output, ")");
        if (owned_string) {
            fprintf(gen->output, "), %d)", function_def_returns_heap_string(gen, fdef) ? 1 : 0);
        }
        fprintf(gen->output, ";\n}\n");
    }
}

// Get the factory function for a builder function (default: "map_new").
const char* get_builder_factory(CodeGenerator* gen, const char* func_name) {
    if (!gen || !func_name) return "map_new";
    const char* normalized = codegen_normalise_callee(func_name);
    for (int i = 0; i < gen->builder_func_reg_count; i++) {
        if (gen->builder_funcs_reg[i].name && strcmp(gen->builder_funcs_reg[i].name, normalized) == 0) {
            return gen->builder_funcs_reg[i].factory ? gen->builder_funcs_reg[i].factory : "map_new";
        }
    }
    return "map_new";
}

// Look up a registry entry by name, trying both the exact form and
// the dot-normalised form. Module-qualified call sites store the
// function name with a dot (e.g. "http.response_set_body_n"), but
// externs are registered under the raw extern name (e.g.
// "http_response_set_body_n"). Without the second pass, a call
// reaching an extern via `import std.http` is not recognised as an
// extern call, and the codegen skips behaviours gated on
// is_extern_func — most importantly the #297 auto-unwrap of
// `string`-typed args. Inline `extern` declarations in the same
// .ae file matched fine because the call site's name and the
// registered name are identical.
static int find_extern_registry_index(CodeGenerator* gen, const char* func_name) {
    if (!gen || !func_name) return -1;
    for (int i = 0; i < gen->extern_registry_count; i++) {
        if (gen->extern_registry[i].name && strcmp(gen->extern_registry[i].name, func_name) == 0) {
            return i;
        }
    }
    /* Try the dot-normalised form: "ns.fn" → "ns_fn", whole however
     * long the name (#2539). */
    if (strchr(func_name, '.')) {
        const char* norm = codegen_normalise_callee(func_name);
        for (int i = 0; i < gen->extern_registry_count; i++) {
            if (gen->extern_registry[i].name &&
                strcmp(gen->extern_registry[i].name, norm) == 0) {
                return i;
            }
        }
    }
    return -1;
}

// Check if a function name is registered as an extern function.
int is_extern_func(CodeGenerator* gen, const char* func_name) {
    return find_extern_registry_index(gen, func_name) >= 0;
}

// If `func_name` was declared via @extern("c_symbol"), return the C
// symbol bound to it. Otherwise returns `func_name` unchanged. Used
// at call sites: codegen translates the Aether-side name (which lives
// in the module namespace) into the actual C symbol the linker wants.
// See #234.
const char* lookup_extern_c_name(CodeGenerator* gen, const char* func_name) {
    if (!gen || !func_name) return func_name;
    int idx = find_extern_registry_index(gen, func_name);
    if (idx >= 0 && gen->extern_registry[idx].c_name) {
        return gen->extern_registry[idx].c_name;
    }
    return func_name;
}

// Look up the expected TypeKind for the nth parameter of an extern function.
// Returns TYPE_UNKNOWN if the function or parameter is not found.
TypeKind lookup_extern_param_kind(CodeGenerator* gen, const char* func_name, int param_idx) {
    int idx = find_extern_registry_index(gen, func_name);
    if (idx < 0) return TYPE_UNKNOWN;
    if (param_idx < gen->extern_registry[idx].param_count && gen->extern_registry[idx].params) {
        return gen->extern_registry[idx].params[param_idx];
    }
    return TYPE_UNKNOWN;
}

// Full Type* sibling of the above (#1033) — NULL when unregistered or
// the param carried no node_type. Borrowed from the extern's AST.
Type* lookup_extern_param_type(CodeGenerator* gen, const char* func_name, int param_idx) {
    int idx = find_extern_registry_index(gen, func_name);
    if (idx < 0) return NULL;
    if (param_idx >= 0 && param_idx < gen->extern_registry[idx].param_count &&
        gen->extern_registry[idx].param_full) {
        return gen->extern_registry[idx].param_full[param_idx];
    }
    return NULL;
}

Type* lookup_extern_return_type(CodeGenerator* gen, const char* func_name) {
    int idx = find_extern_registry_index(gen, func_name);
    if (idx < 0) return NULL;
    return gen->extern_registry[idx].ret_type;
}

// Returns 1 if the nth parameter of `func_name` was declared with the
// `@aether` annotation (`name: @aether string`), 0 otherwise. Used by
// call-site codegen to suppress the aether_string_data() unwrap on
// that arg slot — receiver is Aether-emitted C and dispatches on the
// AetherString magic via str_len; passing the unwrapped data pointer
// would strlen-truncate binary content at embedded NULs. See #351.
int is_aether_extern_param(CodeGenerator* gen, const char* func_name, int param_idx) {
    int idx = find_extern_registry_index(gen, func_name);
    if (idx < 0) return 0;
    if (!gen->extern_registry[idx].params_aether) return 0;
    if (param_idx < 0 || param_idx >= gen->extern_registry[idx].param_count) return 0;
    return gen->extern_registry[idx].params_aether[param_idx];
}

/* Returns 1 if extern `func_name`'s parameter at `param_idx` was
 * declared with `@retain` — meaning the function stores or
 * retains the pointer beyond the call. The escape walker uses
 * this to mark a heap-string arg as escaped so the function-
 * exit defer-free skips the free (would otherwise UAF the
 * stored copy). See codegen_func.c::register_extern_func and
 * the parser's @retain handling. Returns 0 if the function
 * isn't a registered extern, has no annotation, or the index
 * is out of range. */
int is_retain_extern_param(CodeGenerator* gen, const char* func_name, int param_idx) {
    int idx = find_extern_registry_index(gen, func_name);
    if (idx < 0) return 0;
    if (!gen->extern_registry[idx].params_retain) return 0;
    if (param_idx < 0 || param_idx >= gen->extern_registry[idx].param_count) return 0;
    return gen->extern_registry[idx].params_retain[param_idx];
}

/* #2523: was extern `func_name`'s parameter at `param_idx` declared
 * `@noescape`, so the function uses the argument only during the call and
 * neither stores nor frees it? A closure passed there is the caller's to
 * release once the call returns, and a `ptr` slot takes a box on the
 * caller's stack. 0 for an unregistered extern, no annotation or an index
 * out of range: the conservative answer, under which the callee keeps it. */
int is_noescape_extern_param(CodeGenerator* gen, const char* func_name, int param_idx) {
    int idx = find_extern_registry_index(gen, func_name);
    if (idx < 0) return 0;
    if (!gen->extern_registry[idx].params_noescape) return 0;
    if (param_idx < 0 || param_idx >= gen->extern_registry[idx].param_count) return 0;
    return gen->extern_registry[idx].params_noescape[param_idx];
}

// Check if an AST subtree contains a return statement with a value.
// Stops at AST_CLOSURE boundaries: a nested lambda's `return` belongs
// to that lambda, not to the enclosing function/closure. Without this
// stop, `|| { inner = |x| { return x*2 }; call(inner, 3) }` would have
// its outer closure mis-typed as returning-int (picking up inner's
// `return`) when the outer actually has no return statement of its own.
int has_return_value(ASTNode* node) {
    if (!node) return 0;
    if (node->type == AST_CLOSURE) return 0;
    if (node->type == AST_RETURN_STATEMENT && node->child_count > 0 && node->children[0]) {
        // Print statements don't count as "return values" - they're void
        if (node->children[0]->type == AST_PRINT_STATEMENT) {
            return 0;
        }
        return 1;
    }
    for (int i = 0; i < node->child_count; i++) {
        if (has_return_value(node->children[i])) return 1;
    }
    return 0;
}

// Names that are unconditionally declared by the C standard library
// (or POSIX headers we already include via the prelude) and whose
// signatures we don't want to redeclare from the user's `extern`. The
// generated C already pulls in <stdlib.h>/<stdio.h>/<string.h>/<unistd.h>,
// so a redeclaration of `sleep` / `exit` / `printf` / etc. with the
// Aether-translated prototype would conflict whenever the libc and
// Aether parameter shapes differ (e.g. unsigned vs int sleep, varargs
// vs typed printf). The user's `extern foo(...)` is still useful — it
// registers the function for type-aware call-site emission — but we
// skip writing the C forward declaration. Issue #233.
static int extern_name_is_libc_conflict(const char* name) {
    if (!name) return 0;
    static const char* libc_names[] = {
        /* Process / signal */
        "sleep", "usleep", "exit", "_exit", "abort", "atexit",
        "fork", "wait", "waitpid", "execv", "execvp", "execve",
        "getpid", "getppid", "getuid", "geteuid", "getgid", "getegid",
        "signal", "kill", "raise",
        /* Standard I/O */
        "printf", "fprintf", "sprintf", "snprintf",
        "vprintf", "vfprintf", "vsprintf", "vsnprintf",
        "puts", "fputs", "gets", "fgets",
        "getc", "putc", "getchar", "putchar", "ungetc",
        "fopen", "fclose", "fflush", "freopen",
        "fread", "fwrite", "fseek", "ftell", "rewind",
        "feof", "ferror", "fileno", "perror", "clearerr",
        /* Memory */
        "malloc", "calloc", "realloc", "free",
        /* String — note: strdup/strndup added; size_t-arg variants
           like strstr/strchr also clash on prototype shape. */
        "strlen", "strcmp", "strncmp", "strcpy", "strncpy",
        "strcat", "strncat", "strdup", "strndup",
        "strstr", "strchr", "strrchr", "strpbrk",
        "strspn", "strcspn", "strtok", "strtok_r",
        "strcasecmp", "strncasecmp",
        "memcpy", "memset", "memmove", "memcmp", "memchr",
        /* Number conversion */
        "atoi", "atol", "atoll", "atof",
        "strtol", "strtoul", "strtoll", "strtoull",
        "strtod", "strtof",
        "abs", "labs", "llabs",
        /* Sort / search — fn-pointer params clash with Aether's
           `ptr` lowering to `void*`. */
        "qsort", "qsort_r", "bsearch",
        /* Environment */
        "getenv", "setenv", "putenv", "unsetenv",
        /* POSIX file ops */
        "open", "close", "read", "write", "lseek",
        "dup", "dup2", "pipe",
        "access", "isatty", "unlink", "rename", "mkdir", "rmdir",
        "stat", "fstat", "lstat",
        /* Time */
        "time", "clock", "clock_gettime", "gettimeofday",
        "localtime", "gmtime", "mktime", "strftime",
        NULL
    };
    for (int i = 0; libc_names[i]; i++) {
        if (strcmp(name, libc_names[i]) == 0) return 1;
    }
    return 0;
}

// Generate extern C function declaration
// extern printf(format: string) -> int  =>  extern int printf(const char*);
void generate_extern_declaration(CodeGenerator* gen, ASTNode* ext) {
    if (!ext || ext->type != AST_EXTERN_FUNCTION) return;

    // Register parameter types for call-site type-aware casting
    register_extern_func(gen, ext);

    // Skip the C forward declaration for libc-conflicting names — the
    // libc headers we include in the prelude already declare them with
    // the canonical prototype. Without this skip, e.g. `extern sleep(ms:
    // int)` produces `void sleep(int);` which conflicts with libc's
    // `unsigned int sleep(unsigned int)` and breaks compilation.
    // The function's name is still registered above, so call-site code
    // generation uses the correct cast-to-libc-type emission.
    // @extern("c_symbol") aether_name(...) — the Aether-side name
    // (ext->value) lives in the module namespace, but the C forward
    // declaration and every call site use the annotated C symbol.
    // Closes #234.
    const char* c_name = ext->value;
    const char* c_sym = extern_c_symbol(ext);
    if (c_sym) {
        c_name = c_sym;
    }

    if (extern_name_is_libc_conflict(c_name)) {
        fprintf(gen->output,
                "// Extern C function: %s (libc-provided, declaration skipped)\n",
                c_name);
        return;
    }

    /* `@c_import` (#1239): a C header owns this prototype. Emitting our own
     * would put a second declaration in the TU whose spelling is only
     * ABI-compatible, not identical (int vs uint8_t, long vs size_t, void*
     * vs a typed pointer), the source of the LTO type-mismatch warnings.
     * Emitting nothing makes disagreement impossible, and the C compiler
     * still checks every call site against the header. Also the only shape
     * that is safe for a `static inline` helper (#1241), which has no
     * linkable symbol for a non-static prototype to refer to. The name is
     * registered above, so call-site casts are unaffected. */
    if (extern_is_c_import(ext)) {
        fprintf(gen->output,
                "// Extern C function: %s (@c_import, header owns the prototype)\n",
                c_name);
        return;
    }

    fprintf(gen->output, "// Extern C function: %s\n", c_name);

    // Generate return type (map Aether types to C types)
    if (ext->node_type && ext->node_type->kind != TYPE_VOID) {
        /* A C ABI alias / qualified type (`c_alias` set: `size_t`,
         * `const void*`, `char*`, ...) emits its exact C spelling so
         * the prototype matches the system header byte-for-byte. */
        if (ext->node_type->c_alias) {
            fprintf(gen->output, "%s", ext->node_type->c_alias);
        } else
        switch (ext->node_type->kind) {
            case TYPE_STRING:
                fprintf(gen->output, "const char*");
                break;
            case TYPE_FLOAT:
                fprintf(gen->output, "double");  // C uses double by default
                break;
            case TYPE_LONGDOUBLE:
                fprintf(gen->output, "long double");
                break;
            case TYPE_PTR:
                /* `*StructName` typed pointer (PR #307): TYPE_PTR with a
                 * TYPE_STRUCT element. Emit `StructName*` so the extern
                 * declaration matches the runtime header's signature
                 * exactly — otherwise downstream C compilers see
                 * conflicting prototypes (`void* foo(void*)` here vs
                 * `StructName* foo(StructName*)` in the runtime .h)
                 * and refuse to link. Bare `ptr` (no element type) stays
                 * `void*`. */
                if (ext->node_type->element_type &&
                    ext->node_type->element_type->kind == TYPE_STRUCT &&
                    ext->node_type->element_type->struct_name) {
                    const char* sname = ext->node_type->element_type->struct_name;
                    if (aether_is_c_import_struct(sname)) {
                        /* `@c_import` structs have no aetherc-emitted
                         * typedef; some headers (<time.h> `struct tm`)
                         * don't ship one either.  `struct Name*` is the
                         * portable form. */
                        fprintf(gen->output, "%s %s*", aether_c_tag(sname), sname);
                    } else {
                        fprintf(gen->output, "%s*", sname);
                    }
                } else {
                    fprintf(gen->output, "void*");
                }
                break;
            case TYPE_BOOL:
                fprintf(gen->output, "int");
                break;
            case TYPE_TUPLE:
                // `-> (T1, T2, ...)` — the C function returns a struct
                // by value with the matching `_tuple_T1_T2` shape. The
                // typedef was synthesised in codegen.c's pre-scan so
                // it's already in scope here. Issue #271.
                fprintf(gen->output, "%s", get_c_type(ext->node_type));
                break;
            case TYPE_ARRAY:
                /* #1286: a C function hands back a bare `T*`; the call
                 * site wraps it into an (unbounded) slice view. */
                if (type_is_slice(ext->node_type) && ext->node_type->element_type)
                    fprintf(gen->output, "%s*", get_c_type(ext->node_type->element_type));
                else
                    generate_type(gen, ext->node_type);
                break;
            default:
                generate_type(gen, ext->node_type);
                break;
        }
    } else {
        fprintf(gen->output, "void");
    }

    fprintf(gen->output, " %s(", c_name);  // No mangling: extern refers to actual C symbol

    // Generate parameters
    int first_param = 1;
    for (int i = 0; i < ext->child_count; i++) {
        ASTNode* param = ext->children[i];
        if (param->type == AST_IDENTIFIER) {
            if (!first_param) fprintf(gen->output, ", ");
            first_param = 0;

            // Map Aether types to C types
            if (param->node_type) {
                if (param->node_type->c_alias) {
                    /* C ABI alias / qualified type — exact C spelling. */
                    fprintf(gen->output, "%s", param->node_type->c_alias);
                } else
                switch (param->node_type->kind) {
                    case TYPE_STRING:
                        fprintf(gen->output, "const char*");
                        break;
                    case TYPE_FLOAT:
                        fprintf(gen->output, "double");
                        break;
                    case TYPE_LONGDOUBLE:
                        fprintf(gen->output, "long double");
                        break;
                    case TYPE_PTR:
                        /* See the matching note on the return-type switch
                         * above — typed `*StructName` parameters must
                         * emit `StructName*`, not `void*`. */
                        if (param->node_type->element_type &&
                            param->node_type->element_type->kind == TYPE_STRUCT &&
                            param->node_type->element_type->struct_name) {
                            const char* sname = param->node_type->element_type->struct_name;
                            if (aether_is_c_import_struct(sname)) {
                                fprintf(gen->output, "%s %s*", aether_c_tag(sname), sname);
                            } else {
                                fprintf(gen->output, "%s*", sname);
                            }
                        } else {
                            fprintf(gen->output, "void*");
                        }
                        break;
                    case TYPE_BOOL:
                        fprintf(gen->output, "int");
                        break;
                    case TYPE_TUPLE:
                        /* `v: (T1, T2, ...)` — by-value C struct parameter
                         * with the matching `_tuple_T1_T2` shape (#1033),
                         * the parameter-position mirror of the #271 tuple
                         * return. The typedef is synthesized in codegen.c's
                         * pre-scan, so it's already in scope here. */
                        fprintf(gen->output, "%s", get_c_type(param->node_type));
                        break;
                    case TYPE_ARRAY:
                        /* #1286: C sees `T*`; the call site passes `.ptr`. */
                        if (type_is_slice(param->node_type) && param->node_type->element_type)
                            fprintf(gen->output, "%s*", get_c_type(param->node_type->element_type));
                        else
                            generate_type(gen, param->node_type);
                        break;
                    default:
                        generate_type(gen, param->node_type);
                        break;
                }
            } else {
                fprintf(gen->output, "int");
            }
        }
    }
    int is_varargs = extern_is_varargs(ext);
    if (first_param && !is_varargs) {
        fprintf(gen->output, "void");
    }

    // Variadic externs: append `, ...` to the C prototype.  The
    // varargs flag is set by the parser when the user writes
    // `extern foo(fmt: string, ...)` (annotation "varargs") or
    // `@extern("sym") foo(fmt: string, ...)` (annotation
    // "c_symbol:NAME;varargs").  Standalone `(...)` (no named params)
    // emits as `(...)`.
    if (is_varargs) {
        if (first_param) {
            fprintf(gen->output, "...");
        } else {
            fprintf(gen->output, ", ...");
        }
    }

    fprintf(gen->output, ");\n\n");
}

// Scan AST for multi-return statements and merge their tuple types
void merge_return_tuple_types(ASTNode* node, Type* merged) {
    if (!node || !merged) return;
    if (node->type == AST_RETURN_STATEMENT && node->child_count > 1 &&
        node->child_count == merged->tuple_count) {
        for (int i = 0; i < node->child_count; i++) {
            ASTNode* val = node->children[i];
            if (merged->tuple_types[i]->kind == TYPE_UNKNOWN && val->node_type &&
                val->node_type->kind != TYPE_UNKNOWN) {
                free_type(merged->tuple_types[i]);
                merged->tuple_types[i] = clone_type(val->node_type);
            }
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        merge_return_tuple_types(node->children[i], merged);
    }
}

// Propagate a function's merged tuple return type to all call sites
// and to tuple destructuring variables
void propagate_tuple_type_to_calls(ASTNode* node, const char* func_name, Type* type) {
    if (!node) return;
    if (node->type == AST_FUNCTION_CALL && node->value &&
        strcmp(node->value, func_name) == 0) {
        if (node->node_type) free_type(node->node_type);
        node->node_type = clone_type(type);
    }
    // Also update tuple destructure variables whose RHS is this function
    if (node->type == AST_TUPLE_DESTRUCTURE && node->child_count >= 2) {
        ASTNode* rhs = node->children[node->child_count - 1];
        if (rhs && rhs->type == AST_FUNCTION_CALL && rhs->value &&
            strcmp(rhs->value, func_name) == 0) {
            int var_count = node->child_count - 1;
            for (int i = 0; i < var_count && i < type->tuple_count; i++) {
                ASTNode* var = node->children[i];
                if (var && (!var->node_type || var->node_type->kind == TYPE_UNKNOWN)) {
                    if (var->node_type) free_type(var->node_type);
                    var->node_type = clone_type(type->tuple_types[i]);
                }
            }
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        propagate_tuple_type_to_calls(node->children[i], func_name, type);
    }
}

/* Inline bodies for the std.mem scalar accessors (#1733).
 *
 * Returns 1 if `func` is one of them and a body was emitted; 0 otherwise.
 *
 * Each entry is copied VERBATIM from std/mem/aether_mem.c. The null returns
 * are deliberately asymmetric and must stay that way:
 *   - get_byte / get_byte_sz -> -1;  every other getter -> 0 / 0.0;
 *     every setter -> 0.
 * The integer getters DEREFERENCE (inheriting the platform alignment
 * requirement, exactly as today) while the float accessors go through
 * memcpy (unaligned-safe, exactly as today). Copying that distinction is
 * what makes "same as the extern" true rather than approximately true.
 *
 * get_ptr/set_ptr were first excluded as never hot; a ported VM showed
 * otherwise, and they are now lowered through memcpy (see their entries).
 *
 * The bodies reference the wrapper's own parameter names (p / i / offset /
 * value / bits), which are the names in std/mem/module.ae and are what this
 * emitter prints for the signature. Differential integration tests pin
 * this coupling, including the unchecked companions (#2379). */
typedef struct { const char* name; const char* body; } MemAccessorBody;

static const MemAccessorBody MEM_ACCESSOR_BODIES[] = {
    { "mem_get_byte",
      "    if (!p) return -1;\n"
      "    return (int)((unsigned char*)p)[i];\n" },
    { "mem_set_byte",
      "    if (!p) return 0;\n"
      "    ((unsigned char*)p)[i] = (unsigned char)value;\n"
      "    return 1;\n" },
    { "mem_get_byte_sz",
      "    if (!p) return -1;\n"
      "    return (int)((unsigned char*)p)[i];\n" },
    { "mem_set_byte_sz",
      "    if (!p) return 0;\n"
      "    ((unsigned char*)p)[i] = (unsigned char)value;\n"
      "    return 1;\n" },
    { "mem_get_int",
      "    if (!p) return 0;\n"
      "    return *(int32_t*)((char*)p + offset);\n" },
    { "mem_set_int",
      "    if (!p) return 0;\n"
      "    *(int32_t*)((char*)p + offset) = (int32_t)value;\n"
      "    return 1;\n" },
    { "mem_get_long",
      "    if (!p) return 0;\n"
      "    return *(int64_t*)((char*)p + offset);\n" },
    { "mem_set_long",
      "    if (!p) return 0;\n"
      "    *(int64_t*)((char*)p + offset) = value;\n"
      "    return 1;\n" },
    { "mem_get_float32",
      "    if (!p) return 0.0;\n"
      "    float _v;\n"
      "    __builtin_memcpy(&_v, (char*)p + offset, sizeof(_v));\n"
      "    return (double)_v;\n" },
    { "mem_set_float32",
      "    if (!p) return 0;\n"
      "    float _v = (float)value;\n"
      "    __builtin_memcpy((char*)p + offset, &_v, sizeof(_v));\n"
      "    return 1;\n" },
    { "mem_get_float64",
      "    if (!p) return 0.0;\n"
      "    double _v;\n"
      "    __builtin_memcpy(&_v, (char*)p + offset, sizeof(_v));\n"
      "    return _v;\n" },
    { "mem_set_float64",
      "    if (!p) return 0;\n"
      "    __builtin_memcpy((char*)p + offset, &value, sizeof(value));\n"
      "    return 1;\n" },
    /* The rest were added when a profile of mquickjs-ae (a C engine ported
     * to Aether, whose VM reads its heap through std.mem) put about 30% of
     * its run time in calls to aether_mem_long_to_ptr / ptr_to_long alone,
     * with get_ptr and the narrow widths close behind. */
    { "mem_ptr_to_long",
      "    return (int64_t)(uintptr_t)p;\n" },
    { "mem_long_to_ptr",
      "    return (void*)(uintptr_t)addr;\n" },
    /* get_ptr/set_ptr go through memcpy rather than the extern's typed
     * `*(void**)` deref: the same result on the aligned slots their contract
     * requires, but alias-safe once inlined. Inlined typed pointer accesses
     * next to int64 accesses of the same heap slots let clang reorder them
     * (type-based alias analysis) and crashed mquickjs-ae's Octane run. */
    { "mem_get_ptr",
      "    if (!p) return NULL;\n"
      "    void* _v;\n"
      "    __builtin_memcpy(&_v, (char*)p + offset, sizeof(_v));\n"
      "    return _v;\n" },
    { "mem_set_ptr",
      "    if (!p) return 0;\n"
      "    __builtin_memcpy((char*)p + offset, &value, sizeof(value));\n"
      "    return 1;\n" },
    { "mem_get_int8",
      "    if (!p) return 0;\n"
      "    return (int)((int8_t*)p)[offset];\n" },
    { "mem_set_int8",
      "    if (!p) return 0;\n"
      "    ((int8_t*)p)[offset] = (int8_t)(value & 0xff);\n"
      "    return 1;\n" },
    { "mem_get_uint8",
      "    if (!p) return 0;\n"
      "    return (int)((uint8_t*)p)[offset];\n" },
    { "mem_set_uint8",
      "    if (!p) return 0;\n"
      "    ((uint8_t*)p)[offset] = (uint8_t)(value & 0xff);\n"
      "    return 1;\n" },
    { "mem_get_int16",
      "    if (!p) return 0;\n"
      "    int16_t _v;\n"
      "    __builtin_memcpy(&_v, (char*)p + offset, sizeof(_v));\n"
      "    return (int)_v;\n" },
    { "mem_set_int16",
      "    if (!p) return 0;\n"
      "    int16_t _v = (int16_t)(value & 0xffff);\n"
      "    __builtin_memcpy((char*)p + offset, &_v, sizeof(_v));\n"
      "    return 1;\n" },
    { "mem_get_uint16",
      "    if (!p) return 0;\n"
      "    uint16_t _v;\n"
      "    __builtin_memcpy(&_v, (char*)p + offset, sizeof(_v));\n"
      "    return (int)_v;\n" },
    { "mem_set_uint16",
      "    if (!p) return 0;\n"
      "    uint16_t _v = (uint16_t)(value & 0xffff);\n"
      "    __builtin_memcpy((char*)p + offset, &_v, sizeof(_v));\n"
      "    return 1;\n" },
    { "mem_get_uint32",
      "    if (!p) return 0;\n"
      "    uint32_t _v;\n"
      "    __builtin_memcpy(&_v, (char*)p + offset, sizeof(_v));\n"
      "    return (int64_t)_v;\n" },
    { "mem_set_uint32",
      "    if (!p) return 0;\n"
      "    uint32_t _v = (uint32_t)(value & 0xFFFFFFFF);\n"
      "    __builtin_memcpy((char*)p + offset, &_v, sizeof(_v));\n"
      "    return 1;\n" },
    { "mem_bits_of_float",
      "    int64_t _b; __builtin_memcpy(&_b, &value, sizeof(_b)); return _b;\n" },
    { "mem_float_from_bits",
      "    double _d; __builtin_memcpy(&_d, &bits, sizeof(_d)); return _d;\n" },
    { "mem_clz32",
      "    return __builtin_clz((unsigned int)value);\n" },
    { "mem_clz64",
      "    return __builtin_clzll((unsigned long long)value);\n" },
};

static int emit_mem_accessor_body(CodeGenerator* gen, ASTNode* func) {
    if (!func || !func->value) return 0;
    if (!fn_has_internal_linkage(func)) return 0;
    const char* cname = safe_c_name(func->value);
    if (!cname) return 0;

    for (size_t k = 0;
         k < sizeof(MEM_ACCESSOR_BODIES) / sizeof(MEM_ACCESSOR_BODIES[0]);
         k++) {
        const char* name = MEM_ACCESSOR_BODIES[k].name;
        const char* body = MEM_ACCESSOR_BODIES[k].body;
        if (strcmp(cname, name) != 0) {
            /* Only scalar loads/stores have unchecked companions. Reuse
             * the exact checked body after its leading null guard, keeping
             * widths, conversions and alias-safe memcpy paths in sync. */
            size_t n = strlen(name);
            if (strncmp(body, "    if (!p) return ", 19) != 0 ||
                strncmp(cname, name, n) != 0 ||
                strcmp(cname + n, "_unchecked") != 0) continue;
            body = strchr(body, '\n') + 1;
        }

        /* The canned bodies name the wrapper's parameters directly (p / i /
         * offset / value), which are the names in std/mem/module.ae and the
         * ones this emitter prints for the signature. That coupling is
         * pinned by tests/integration/mem_inline_accessors, which compares
         * every lowered accessor against its extern for equal results --
         * including the null paths -- so a rename in std.mem fails loudly
         * there rather than silently emitting C that will not compile. */
        fputs(body, gen->output);
        return 1;
    }
    return 0;
}

/* #2645: the type a function returns in C, decided once for every place
 * that spells it: the prototype, the definition and, for a function written
 * as several clauses, the dispatcher and each clause function. Its declared
 * type, else int when a body returns a value (the legacy default), else
 * void (#354). A clause set is decided over every clause: the first
 * declared type among them, else int when any clause returns a value. The
 * prototype read the first clause and the definition all of them, so a set
 * whose first clause returned nothing was declared void and defined int. */
Type* fn_result_type(CodeGenerator* gen, ASTNode* fn) {
    static Type int_type = { .kind = TYPE_INT };
    if (!fn) return NULL;
    const DefClauses* dc = gen ? fn_def_clause_set(gen->program, fn) : NULL;
    int n = dc ? dc->count : 1;
    int returns_value = 0;
    for (int c = 0; c < n; c++) {
        ASTNode* clause = dc ? dc->nodes[c] : fn;
        Type* t = clause->node_type;
        if (t && t->kind != TYPE_VOID && t->kind != TYPE_UNKNOWN) return t;
        if (!returns_value) returns_value = has_return_value(clause);
    }
    return returns_value ? &int_type : NULL;
}

void emit_fn_result_c_type(CodeGenerator* gen, ASTNode* fn) {
    Type* t = fn_result_type(gen, fn);
    if (t) generate_type(gen, t);
    else fprintf(gen->output, "void");
}

/* A child of a function definition that is one of its parameters, in the
 * order the C signature takes them: a variable, a literal or struct pattern
 * (one C parameter each), a list pattern (a pointer and a length). Guards
 * and contracts sit among them and are not. */
static int is_param_node(const ASTNode* c) {
    return c && (c->type == AST_PATTERN_VARIABLE || c->type == AST_VARIABLE_DECLARATION ||
                 c->type == AST_PATTERN_LITERAL || c->type == AST_PATTERN_STRUCT ||
                 c->type == AST_PATTERN_LIST || c->type == AST_PATTERN_CONS);
}

static int is_list_param_node(const ASTNode* c) {
    return c && (c->type == AST_PATTERN_LIST || c->type == AST_PATTERN_CONS);
}

int fn_param_count(ASTNode* fn) {
    int n = 0;
    for (int i = 0; fn && i < fn->child_count; i++)
        if (is_param_node(fn->children[i])) n++;
    return n;
}

/* Parameter position `pos` of `fn`, or NULL. */
ASTNode* fn_param_at(ASTNode* fn, int pos) {
    for (int i = 0, p = 0; fn && i < fn->child_count; i++) {
        if (!is_param_node(fn->children[i])) continue;
        if (p++ == pos) return fn->children[i];
    }
    return NULL;
}

/* #2644: the type at parameter position `pos` of the clause set `fn` is one
 * of (of `fn` alone for a single definition), which the prototype, the
 * dispatcher and each clause function's pattern parameters share: the
 * first clause binding a variable of a known type there gives it, else the
 * first literal or struct pattern with a type. A wildcard has none. */
Type* fn_param_type_at(CodeGenerator* gen, ASTNode* fn, int pos) {
    const DefClauses* dc = gen ? fn_def_clause_set(gen->program, fn) : NULL;
    int n = dc ? dc->count : 1;
    Type* pattern_type = NULL;
    for (int c = 0; c < n; c++) {
        ASTNode* p = fn_param_at(dc ? dc->nodes[c] : fn, pos);
        if (!p || !p->node_type || p->node_type->kind == TYPE_UNKNOWN ||
            p->node_type->kind == TYPE_WILDCARD) continue;
        if (p->type == AST_PATTERN_VARIABLE || p->type == AST_VARIABLE_DECLARATION)
            return p->node_type;
        if (!pattern_type) pattern_type = p->node_type;
    }
    if (pattern_type) return pattern_type;
    ASTNode* own = fn_param_at(fn, pos);
    return own ? own->node_type : NULL;
}

/* The element C type of list parameter position `pos`: the first clause
 * whose pattern there binds an element of a known type gives it, else int. */
const char* fn_list_param_elem_ctype(CodeGenerator* gen, ASTNode* fn, int pos) {
    const DefClauses* dc = gen ? fn_def_clause_set(gen->program, fn) : NULL;
    int n = dc ? dc->count : 1;
    for (int c = 0; c < n; c++) {
        ASTNode* p = fn_param_at(dc ? dc->nodes[c] : fn, pos);
        if (!is_list_param_node(p) || p->child_count == 0 || !p->children[0]) continue;
        Type* et = p->children[0]->node_type;
        if (et && et->kind != TYPE_UNKNOWN) return get_c_type(et);
    }
    return "int";
}

/* The value a function returns when no pattern, guard or clause matches,
 * of the type it returns (fn_result_type). A string its caller owns is a
 * copy of "" the caller may free, not the literal (#2627); so is each owned
 * string of a tuple, whose other positions are zero, as a struct's or an
 * optional's fields are (#2646: a tuple's default was a bare `0`). */
static void emit_no_match_value(CodeGenerator* gen, ASTNode* func) {
    Type* t = fn_result_type(gen, func);
    if (!t) return;
    if (t->kind == TYPE_STRING && function_def_returns_heap_string(gen, func)) {
        /* Marked like its other returns when used as a fn value (#2586). */
        if (fn_value_name(func->value))
            fprintf(gen->output, "aether_fnptr_give(aether_uniform_heap_str(\"\", 0))");
        else
            fprintf(gen->output, "aether_uniform_heap_str(\"\", 0)");
        return;
    }
    if (t->kind == TYPE_TUPLE) {
        fprintf(gen->output, "(%s){", get_c_type(t));
        int any = 0;
        for (int p = 0; p < t->tuple_count; p++) {
            Type* et = t->tuple_types ? t->tuple_types[p] : NULL;
            if (!et || et->kind != TYPE_STRING) continue;
            fprintf(gen->output, "%s ._%d = %s", any ? "," : "", p,
                    function_def_returns_heap_at(gen, func, p)
                        ? "aether_uniform_heap_str(\"\", 0)" : "\"\"");
            any = 1;
        }
        fprintf(gen->output, "%s}", any ? " " : "0");
        return;
    }
    if (t->kind == TYPE_STRUCT || t->kind == TYPE_OPTIONAL) {
        fprintf(gen->output, "(%s){0}", get_c_type(t));
        return;
    }
    generate_default_return_value(gen, t);
}

/* Emit `func`. A single definition is the C function of its name and tests
 * its own literal patterns and guard on entry. With `clause_cname`, `func`
 * is one clause of a set (#2644), emitted as the static C function of that
 * name with the whole setup a single function gets: the dispatcher
 * (generate_combined_function) has already matched its patterns and guard,
 * which are not tested again, and its return type is the set's. */
static void emit_function(CodeGenerator* gen, ASTNode* func, const char* clause_cname) {
    if (!func || (func->type != AST_FUNCTION_DEFINITION && func->type != AST_BUILDER_FUNCTION)) return;
    int as_clause = clause_cname != NULL;

    // Emit a `#line` directive at the function's definition line so
    // codegen sees a clean reset every time it crosses into a new
    // function — important when the merged program intermixes
    // user-written and module-imported functions in arbitrary order.
    codegen_maybe_emit_line(gen, func);
    codegen_note_diag_pos(func);
    codegen_note_diag_func(func->value);

    // If function returns a tuple with UNKNOWN elements, scan all returns and merge
    if (func->node_type && func->node_type->kind == TYPE_TUPLE) {
        merge_return_tuple_types(func, func->node_type);
    }
    // Determine return type:
    //   - unannotated + has `return <value>` → int (legacy default).
    //   - unannotated + no return-with-value → void. Without this,
    //     the unresolved-type fallback emitted `int` and the C
    //     compiler warned `non-void function does not return a value`
    //     (issue #354). Functions like `wait_for_next_round` whose
    //     bodies are pure side-effect get a clean void signature.
    // A clause returns what its set does (fn_result_type, #2645).
    Type* result = fn_result_type(gen, func);
    // Multi-value return: ensure the `_tuple_T1_T2` typedef is in
    // scope before the signature references it. The return-statement
    // path also calls this when emitting `return (_tuple_X){a, b};`,
    // but the signature is rendered first (#285).
    if (result && result->kind == TYPE_TUPLE) {
        ensure_tuple_typedef(gen, result);
    }

    // Track current function's return type for multi-return codegen
    gen->current_func_return_type = (as_clause && result) ? result : func->node_type;
    // Track the function's AST node so AST_RETURN_STATEMENT codegen
    // can find any `ensures` clauses attached to it (issue #348).
    // A clause is its own node, classified with its set (#2627).
    ASTNode* prev_current_function = gen->current_function;
    gen->current_function = func;
    /* #2513: the function's variables, as discover_closures_scoped named
     * its scope (a clause its own, #2644). */
    const char* scope = fn_scope_name(gen->program, func);
    const char* prev_closure_var_scope = gen->closure_var_scope;
    gen->closure_var_scope = scope;

    // Functions cloned from imported modules are emitted with the C
    // `static` storage class so each translation unit gets a private copy.
    // Without this, linking multiple .o files that all import the same
    // SDK module produces duplicate symbol errors on linkers that don't
    // support GNU's --allow-multiple-definition (notably macOS ld64).
    //
    // Exception: `@c_callback` (#235) demands an externally-visible
    // symbol — the whole point of the annotation is that the function
    // can be referenced as a function pointer from across the linkage
    // boundary, so it must NOT be static even when imported.
    // Trailing-underscore convention `foo_` marks a function as
    // file-local — the same convention emit_lib_alias_stubs honours
    // by skipping the aether_<name> alias. Emit as `static` so two
    // .ae files in the same namespace bundle / [[bin]] can each
    // declare their own `record_start_` / `helper_` without the
    // generated C colliding at link time. Closes #279.
    // A clause function is static whatever its set's linkage: only the
    // set's dispatcher calls it.
    if (as_clause) {
        fprintf(gen->output, fn_body_inlinable(func) ? "static inline AETHER_MAYBE_UNUSED "
                                                     : "static AETHER_MAYBE_UNUSED ");
    } else if (fn_has_internal_linkage(func)) {
        fprintf(gen->output, fn_is_inline_candidate(func) ? "static inline AETHER_MAYBE_UNUSED "
                                                          : "static AETHER_MAYBE_UNUSED ");
    } else if (is_c_callback(func)) {
        // A @c_callback keeps an external, verbatim C symbol so the C side can
        // bind it by name — it CANNOT be static. But when a module carrying one
        // (e.g. std.cryptography.tls13_client's aether_pure_tls_client_* bridge)
        // is pulled into more than one translation unit — two TUs that both
        // transitively import std.http.client — each emits the same external
        // definition and they collide at link ("multiple definition of
        // aether_pure_tls_client_send"). Emit the definition weak so duplicate
        // copies dedupe at link while the symbol stays externally addressable.
        // (#2041-adjacent: asks/pure-tls-client-defined-non-static-in-every-tu.)
        fprintf(gen->output, "AETHER_WEAK_DEF ");
    }

    if (result) generate_type(gen, result);
    else fprintf(gen->output, "void");
    // For @c_callback, emit the chosen C symbol verbatim (no namespace
    // mangling); the symbol is what other translation units reach for
    // when they take the address of this function.
    const char* cb_sym = c_callback_symbol(func);
    fprintf(gen->output, " %s(", as_clause ? clause_cname
                                           : (cb_sym ? cb_sym : safe_c_name(func->value)));

    // Generate parameters - handle pattern matching. A parameter's C name
    // carries its position: `_pattern_<pos>` for a literal or struct
    // pattern, `_list_<pos>` / `_len_<pos>` for a list pattern.
    int param_count = 0;
    ASTNode* body = NULL;
    // Track the C name of the last emitted named parameter — needed as
    // the second argument to va_start() if this function is variadic.
    const char* last_param_cname = "";
    char** promoted = NULL;
    int promoted_count = 0;
    get_promoted_names_for_func(gen, scope, &promoted, &promoted_count);

    for (int i = 0; i < func->child_count; i++) {
        ASTNode* child = func->children[i];

        if (child->type == AST_GUARD_CLAUSE) {
            // has_guards = 1;  // Reserved for future optimization
            continue;
        }

        if (child->type == AST_BLOCK) {
            body = child;
            continue;
        }

        // Handle different parameter pattern types
        if (child->type == AST_PATTERN_VARIABLE ||
            child->type == AST_VARIABLE_DECLARATION) {
            if (param_count > 0) fprintf(gen->output, ", ");
            /* #750: a `fn(...)->R` parameter must emit the typed C
             * function-pointer declarator `R (*name)(T1,T2)` — the name
             * sits inside the `(*...)`, so the plain type+name path below
             * can't express it. */
            if (is_fnptr_type(child->node_type)) {
                emit_fnptr_decl(gen, child->node_type, child->value);
                last_param_cname = child->value ? child->value : "";
                param_count++;
                continue;
            }
            /* #2516: `E _param_xs[N]`; the body copies it (or seeds a cell). */
            if (is_sized_array_param(child->node_type) && child->value) {
                emit_sized_array_param_declarator(gen, child->node_type, child->value);
                last_param_cname = cg_internf("_param_%s", child->value);
                param_count++;
                continue;
            }
            /* An unannotated clause parameter takes its position's type
             * across the set, which the dispatcher passes (#2644). */
            generate_type(gen, (as_clause && (!child->node_type ||
                                              child->node_type->kind == TYPE_UNKNOWN))
                                   ? fn_param_type_at(gen, func, param_count)
                                   : child->node_type);
            // If this parameter is a Route 1 promoted name in this function,
            // emit it as `_param_<name>` so the body's heap cell can use
            // the short name. Body prologue below does
            // `T* name = malloc(...); *name = _param_name;`.
            int is_promoted = 0;
            for (int pp = 0; pp < promoted_count; pp++) {
                if (promoted[pp] && child->value &&
                    strcmp(promoted[pp], child->value) == 0) {
                    is_promoted = 1;
                    break;
                }
            }
            if (is_promoted) {
                fprintf(gen->output, " _param_%s", child->value);
                last_param_cname = cg_internf("_param_%s", child->value);
            } else {
                fprintf(gen->output, " %s", child->value);
                last_param_cname = child->value ? child->value : "";
            }
            param_count++;
        } else if (child->type == AST_PATTERN_LITERAL) {
            // Pattern literal becomes regular parameter, of its position's
            // type across a clause set (a wildcard has none of its own).
            if (param_count > 0) fprintf(gen->output, ", ");
            generate_type(gen, as_clause ? fn_param_type_at(gen, func, param_count)
                                         : child->node_type);
            fprintf(gen->output, " _pattern_%d", param_count);
            last_param_cname = cg_internf("_pattern_%d", param_count);
            param_count++;
        } else if (child->type == AST_PATTERN_STRUCT) {
            if (param_count > 0) fprintf(gen->output, ", ");
            fprintf(gen->output, "%s _pattern_%d", child->value, param_count);
            last_param_cname = cg_internf("_pattern_%d", param_count);
            param_count++;
        } else if (child->type == AST_PATTERN_LIST || child->type == AST_PATTERN_CONS) {
            // List pattern becomes array pointer, its element type the one
            // the prototype gives the position.
            if (param_count > 0) fprintf(gen->output, ", ");
            fprintf(gen->output, "%s* _list_%d, int _len_%d",
                    fn_list_param_elem_ctype(gen, func, param_count), param_count, param_count);
            // has_list_patterns = 1;  // Reserved for future optimization
            param_count++;
        }
    }

    // Builder functions get hidden void* _builder as last parameter
    if (func->type == AST_BUILDER_FUNCTION) {
        if (param_count > 0) fprintf(gen->output, ", ");
        fprintf(gen->output, "void* _builder");
        param_count++;
    }
    // C-variadic function (Aether `f(..., ...)`): trailing `...`. C
    // requires at least one named parameter before it.
    int is_variadic = (annotation_has_marker(func->annotation, "varargs")
                       && last_param_cname[0] != '\0');
    if (is_variadic) {
        if (param_count > 0) fprintf(gen->output, ", ");
        fprintf(gen->output, "...");
        param_count++;
    }
    if (param_count == 0) {
        fprintf(gen->output, "void");
    }

    fprintf(gen->output, ") {\n");

    /* std.mem scalar accessors: emit the body inline instead of the extern
     * call (#1733).
     *
     * These wrappers are already emitted `static` into the consumer's own
     * translation unit, so gcc can inline THEM — but each body is a call to
     * aether_mem_* in libaether.a, which no -O level can inline across the
     * static-library boundary (short of LTO, which the toolchain does not
     * do). In a per-pixel loop that call IS the cost: measured on a
     * 640x480x4 composite loop, 60 frames took 215 ms with 12 accessor
     * calls in the inner loop, and 29 ms with the same bodies inlined --
     * 7.4x, with the null contract fully preserved.
     *
     * Semantics are copied verbatim from std/mem/aether_mem.c, INCLUDING
     * the null returns, which are asymmetric: reads return -1, writes
     * return 0. The extern symbols stay in the runtime for existing
     * binaries and FFI consumers; this changes only what this wrapper's
     * body compiles to.
     *
     * The table also covers ptr_to_long / long_to_ptr, get_ptr / set_ptr and
     * the int8/uint8/int16/uint16/uint32 widths (see MEM_ACCESSOR_BODIES). */
    if (!as_clause && emit_mem_accessor_body(gen, func)) {
        fprintf(gen->output, "}\n\n");
        gen->current_function = prev_current_function;
        gen->closure_var_scope = prev_closure_var_scope;
        return;
    }

    indent(gen);
    // Variadic prologue: declare the hidden va_list and prime it from
    // the last named parameter. va_start()/va_arg()/va_end() in the
    // body operate on `&__ae_va`.
    if (is_variadic) {
        fprintf(gen->output, "    va_list __ae_va; va_start(__ae_va, %s);\n",
                last_param_cname);
    }
    clear_declared_vars(gen);  // Reset for each function
    clear_fnptr_locals(gen);   // #2130: fn-typed parameters/locals are this function's
    clear_heap_string_vars(gen);
    clear_captured_string_params(gen);
    clear_seq_vars(gen);
    clear_opt_str_vars(gen);
    clear_escaped_string_vars(gen);
    clear_try_clobbered_vars(gen);  /* Issue #501 follow-up — per-fn set */

    // Mark function parameters as declared so they aren't re-declared
    // (e.g., by hoist_loop_vars when a parameter is reassigned in a loop).
    for (int i = 0; i < func->child_count; i++) {
        ASTNode* child = func->children[i];
        if (!child) continue;
        if ((child->type == AST_PATTERN_VARIABLE || child->type == AST_VARIABLE_DECLARATION)
            && child->value) {
            /* #2516: an array parameter records its type, which a whole-
             * array store reads its length and element type from. */
            if (is_sized_array_param(child->node_type))
                mark_var_declared_typed(gen, child->value, child->node_type);
            else
                mark_var_declared(gen, child->value);
            /* #750: register a `fn(...)->R` parameter in the fn-ptr
             * registry so a call through it (`cb(a,b)`) lowers via the
             * same typed indirect-call path as fn-ptr locals
             * (codegen_expr.c), not a bare `cb(a,b)` against a void*. */
            if (is_fnptr_type(child->node_type)) {
                register_fnptr_local(gen, child->value, child->node_type);
            }
        }
    }

    // Reset defer state for new function and enter function scope
    gen->defer_count = 0;
    gen->scope_depth = 0;
    enter_scope(gen);

    // Route 1: for any parameter whose name is in this function's
    // promoted set, the signature was emitted as `_param_<name>`. Emit a
    // heap cell `T* name = malloc(...); *name = _param_name;` and push a
    // defer so the rest of the body can use the short name through the
    // promotion-aware access path. Must happen AFTER enter_scope so the
    // defer lands in this function's scope, not the caller's.
    for (int i = 0; i < func->child_count; i++) {
        ASTNode* child = func->children[i];
        if (!child) continue;
        if ((child->type == AST_PATTERN_VARIABLE || child->type == AST_VARIABLE_DECLARATION)
            && child->value) {
            int is_promoted = 0;
            for (int pp = 0; pp < promoted_count; pp++) {
                if (promoted[pp] &&
                    strcmp(promoted[pp], child->value) == 0) {
                    is_promoted = 1;
                    break;
                }
            }
            if (is_promoted) {
                const char* c_type = "int";
                if (child->node_type && child->node_type->kind != TYPE_UNKNOWN) {
                    c_type = get_c_type(child->node_type);
                }
                const char* param_cname = cg_internf("_param_%s", child->value);
                print_indent(gen);
                emit_promoted_param_cell(gen, child->value, c_type, param_cname,
                                         child->line, child->column);
            } else if (is_sized_array_param(child->node_type)) {
                print_indent(gen);
                emit_sized_array_param_copy(gen, child->node_type, child->value, body);   /* #2516 */
            } else if (fn_def_string_param_captures(gen, func, i)) {
                /* Copy-on-keep (#2499), as a closure does on entry: a
                 * `string` parameter this body keeps becomes a reference of
                 * its own (a refcounted string retained, a plain buffer
                 * copied) and a heap-tracked local from here on, so a store
                 * moves or copies it, a return hands it over, and the exit
                 * frees what is left. The caller borrows its own argument.
                 * A clause decides for itself (#2644); its callers ask every
                 * clause (callee_keeps_string_arg). */
                print_indent(gen);
                fprintf(gen->output, "%s = aether_str_capture(%s); int _heap_%s = 1; (void)_heap_%s;\n",
                        child->value, child->value, child->value, child->value);
                mark_heap_string_var(gen, child->value);
                mark_captured_string_param(gen, child->value);
            }
            /* A struct parameter is a copy of the caller's value, strings
             * included, and the caller still owns those strings: the copy
             * borrows them. Its trackers are the caller's, so a field store
             * or a reassignment here freed the caller's string, and a
             * promoted cell's release freed it again. Clear them: the
             * callee then owns, and frees at its exit, only the strings it
             * stores itself, unless it returns the struct (#752). A closure
             * field is retained instead (#2525): the copy stays callable
             * and holds a reference of its own. */
            /* #2582: one the body keeps as a whole value (returns,
             * aliases, stores, captures) takes references of its own
             * instead, so what it hands over is its own, and its caller can
             * destroy the argument after the statement. */
            const char* owning = struct_owning_strings(gen, child->node_type);
            if (owning && child->type == AST_PATTERN_VARIABLE) {
                const char* lv = is_promoted ? cg_internf("(*%s)", child->value)
                                             : child->value;
                ASTNode* fbody = NULL;
                for (int b = func->child_count - 1; b >= 0 && !fbody; b--) {
                    if (func->children[b] && func->children[b]->type == AST_BLOCK)
                        fbody = func->children[b];
                }
                if (struct_param_kept(gen, fbody, child->value))
                    emit_struct_capture(gen, owning, lv);
                else
                    emit_struct_disown(gen, owning, lv, 1);
                if (!is_promoted) {
                    push_struct_destroy_defer(gen, child->value, child->node_type,
                                              child->line, child->column);
                }
            }
        }
    }

    // Generate pattern matching checks. A clause's were made by its
    // dispatcher; its pattern parameters are then only bound or unused.
    int pos = 0;

    for (int i = 0; i < func->child_count; i++) {
        ASTNode* child = func->children[i];
        if (!is_param_node(child) && child->type != AST_GUARD_CLAUSE) continue;

        if (child->type == AST_PATTERN_LITERAL && as_clause) {
            print_line(gen, "(void)_pattern_%d;", pos);
        } else if (child->type == AST_PATTERN_LITERAL &&
            strcmp(child->value, "_") != 0) {
            // Generate pattern match check
            print_indent(gen);
            fprintf(gen->output, "if (_pattern_%d != %s) return ",
                    pos, child->value);
            emit_no_match_value(gen, func);
            fprintf(gen->output, ";\n");
        }

        // Generate list pattern checks
        if (child->type == AST_PATTERN_LIST) {
            if (!as_clause) {
                // Empty or fixed-size list check
                print_indent(gen);
                fprintf(gen->output, "if (_len_%d != %d) return ", pos, child->child_count);
                emit_no_match_value(gen, func);
                fprintf(gen->output, ";\n");
            }
            // Bind pattern variables to list elements
            for (int j = 0; j < child->child_count; j++) {
                ASTNode* elem = child->children[j];
                if (elem->type == AST_PATTERN_VARIABLE) {
                    print_indent(gen);
                    const char* etype = "int";
                    if (elem->node_type && elem->node_type->kind != TYPE_UNKNOWN)
                        etype = get_c_type(elem->node_type);
                    fprintf(gen->output, "%s %s = _list_%d[%d];\n",
                            etype, elem->value, pos, j);
                }
            }
        } else if (child->type == AST_PATTERN_CONS) {
            if (!as_clause) {
                // [H|T] pattern - check non-empty
                print_indent(gen);
                fprintf(gen->output, "if (_len_%d < 1) return ", pos);
                emit_no_match_value(gen, func);
                fprintf(gen->output, ";\n");
            }

            // Bind head and tail
            if (child->child_count >= 1 && child->children[0]->type == AST_PATTERN_VARIABLE) {
                print_indent(gen);
                const char* htype = "int";
                if (child->children[0]->node_type && child->children[0]->node_type->kind != TYPE_UNKNOWN)
                    htype = get_c_type(child->children[0]->node_type);
                fprintf(gen->output, "%s %s = _list_%d[0];\n",
                        htype, child->children[0]->value, pos);
            }
            if (child->child_count >= 2 && child->children[1]->type == AST_PATTERN_VARIABLE) {
                print_indent(gen);
                const char* ttype = "int";
                if (child->children[1]->node_type && child->children[1]->node_type->kind != TYPE_UNKNOWN)
                    ttype = get_c_type(child->children[1]->node_type);
                fprintf(gen->output, "%s* %s = &_list_%d[1];\n",
                        ttype, child->children[1]->value, pos);
                print_indent(gen);
                fprintf(gen->output, "int %s_len = _len_%d - 1;\n",
                        child->children[1]->value, pos);
            }
        }

        if (is_param_node(child)) pos++;

        // Generate guard clause check
        if (!as_clause && child->type == AST_GUARD_CLAUSE && child->child_count > 0) {
            print_indent(gen);
            fprintf(gen->output, "if (!(");
            generate_expression(gen, child->children[0]);
            fprintf(gen->output, ")) return ");
            emit_no_match_value(gen, func);
            fprintf(gen->output, ";\n");
        }
    }

    // Publish this function's promoted-captures set so var decls malloc
    // heap cells and reads/writes dereference. (Route 1.)
    char** prev_promoted = gen->current_promoted_captures;
    int prev_promoted_count = gen->current_promoted_capture_count;
    gen->current_promoted_captures = promoted;
    gen->current_promoted_capture_count = promoted_count;

    // Issue #348 — emit `requires` precondition checks at function
    // entry. Each AST_REQUIRES_CLAUSE child of the function carries
    // a single boolean-expression child; codegen emits
    //   if (!(<expr>)) aether_panic("precondition violation: <expr> in <fn>");
    // immediately after parameters are declared and before any
    // user code runs. Skipped entirely when --no-contracts is set.
    // A clause's are its own: they run when its dispatcher picks it.
    if (!gen->no_contracts) {
        emit_contract_preconditions(gen, func);
    }

    // Generate body
    if (body) {
        // Pre-hoist variables first-declared inside if-statement
        // branches whose use escapes the if-block. Without this, the
        // generated C scopes them too tightly and post-block reads
        // fail to compile. See #278.
        if (body->type == AST_BLOCK) {
            gen->hoist_scope_body = body;
            hoist_if_branch_vars(gen, body);
            /* Issue #501 follow-up: mark vars modified inside any
             * try body in this function so AST_VARIABLE_DECLARATION
             * codegen knows which outer-scope locals need a
             * `volatile` C type qualifier.  Must run before the
             * body is generated, so the per-function set is
             * populated by the time each decl-site lookup happens. */
            mark_try_clobbered_vars(gen, body);
            // Pre-hoist `_heap_<name>` companions for every string
            // variable in the body so the tracker is visible across
            // every nesting depth — closes the architectural blocker
            // from issue #405. The first-decl codegen path becomes
            // an assignment-only after this; cross-block reassignment
            // resolves to the function-scope tracker instead of an
            // undeclared local. See codegen_stmt.c::
            // hoist_heap_string_trackers for the full rationale.
            hoist_heap_string_trackers(gen, body);
            // *StringSeq locals: hoist their _seqheap flags + function-
            // scope decl, mark return/raw-store escapes, and push the
            // refcount-decrement scope-exit free (parallel to the
            // heap-string passes immediately around this).
            hoist_seq_trackers(gen, body);
            // `string?` locals: hoist their _heapopt flags + function-
            // scope decl, mark escapes, and push the scope-exit free of
            // the owned `.val` buffer (parallel to the seq passes).
            hoist_opt_str_trackers(gen, body);
            // Mark heap-string vars that escape via call argument or
            // closure capture. The wrapper at codegen_stmt.c:1611
            // skips its `free(_tmp_old)` for escaped vars to avoid
            // dangling pointers held by `map.put`/`list.add`/actor
            // message fields/etc. Conservative — alias-safe at the
            // cost of leaking the value over the function's lifetime.
            mark_escaped_heap_string_vars(gen, body);
            mark_escaped_seq_vars(gen, body);
            mark_escaped_opt_str_vars(gen, body);
            // Push a function-exit defer-free for every non-escaped
            // hoisted heap-string var (#420 follow-up). The
            // wrapper-on-reassignment frees the previous value on
            // every assignment; this defer closes the single-call
            // leak shape (variable assigned once, never reassigned,
            // function exits with a live heap allocation). Drained
            // by exit_scope at function end and emit_all_defers at
            // every explicit return.
            push_heap_string_exit_free_defers(gen, body);
            push_seq_exit_free_defers(gen, body);
            push_opt_str_exit_free_defers(gen, body);
        }
        // If body is a block, it handles its own scope
        // If not a block, we still need to generate the statements
        if (body->type == AST_BLOCK) {
            // Block will handle inner scope, but we need to generate contents
            // without the extra braces since we're already in function body
            for (int i = 0; i < body->child_count; i++) {
                generate_statement(gen, body->children[i]);
            }
        } else {
            generate_statement(gen, body);
        }
    }

    // Emit function-level defers at implicit return (end of function)
    exit_scope(gen);

    /* #2645: a clause that returns nothing, in a set that returns a value,
     * gives its caller the default when it ends. */
    if (as_clause && result && !has_return_value(func)) {
        print_indent(gen);
        fprintf(gen->output, "return ");
        emit_no_match_value(gen, func);
        fprintf(gen->output, ";\n");
    }

    gen->current_promoted_captures = prev_promoted;
    gen->current_promoted_capture_count = prev_promoted_count;
    gen->current_function = prev_current_function;
    gen->closure_var_scope = prev_closure_var_scope;

    unindent(gen);
    print_line(gen, "}");
    print_line(gen, "");
}

void generate_function_definition(CodeGenerator* gen, ASTNode* func) {
    emit_function(gen, func, NULL);
}

/* Does `expr` name `name`, as a value or as the function it calls? */
static int expr_mentions(ASTNode* expr, const char* name) {
    if (!expr || !name) return 0;
    if ((expr->type == AST_IDENTIFIER || expr->type == AST_FUNCTION_CALL) &&
        expr->value && strcmp(expr->value, name) == 0) return 1;
    for (int i = 0; i < expr->child_count; i++)
        if (expr_mentions(expr->children[i], name)) return 1;
    return 0;
}

/* Declare `name` of type `t` as the dispatcher's parameter `src`, for a
 * guard to read. A fixed-size array arrives as a pointer to its first
 * element. */
static void emit_guard_binding(CodeGenerator* gen, Type* t, const char* name, const char* src) {
    print_indent(gen);
    if (is_fnptr_type(t)) {
        emit_fnptr_decl(gen, t, name);
    } else if (is_sized_array_param(t)) {
        fprintf(gen->output, "%s* %s", get_c_type(t->element_type), name);
    } else {
        fprintf(gen->output, "%s %s", (t && t->kind != TYPE_UNKNOWN) ? get_c_type(t) : "int", name);
    }
    fprintf(gen->output, " = %s; (void)%s;\n", src, name);
    mark_var_declared(gen, name);
}

/* A guard is an expression of its clause, read in the dispatcher with
 * none of the clause function's per-function state: the names it binds
 * are plain copies of the parameters, borrowed, with no cells. */
static void begin_dispatch_scope(CodeGenerator* gen, ASTNode* clause) {
    gen->current_function = clause;
    clear_declared_vars(gen);
    clear_fnptr_locals(gen);
    clear_heap_string_vars(gen);
    clear_captured_string_params(gen);
    clear_seq_vars(gen);
    clear_opt_str_vars(gen);
    clear_escaped_string_vars(gen);
    clear_try_clobbered_vars(gen);
}

/* The dispatcher's call of clause `clause`, as a statement that returns
 * what the clause function returns (or returns after it, for a set that
 * returns nothing). */
static void emit_clause_call(CodeGenerator* gen, ASTNode* clause, int returns_value) {
    fprintf(gen->output, returns_value ? "return %s(" : "{ %s(", clause_c_name(gen->program, clause));
    int npos = fn_param_count(clause);
    for (int p = 0; p < npos; p++) {
        if (p > 0) fprintf(gen->output, ", ");
        if (is_list_param_node(fn_param_at(clause, p)))
            fprintf(gen->output, "_list%d, _len%d", p, p);
        else
            fprintf(gen->output, "_arg%d", p);
    }
    if (clause->type == AST_BUILDER_FUNCTION)
        fprintf(gen->output, "%s_builder", npos > 0 ? ", " : "");
    fprintf(gen->output, returns_value ? ");\n" : "); return; }\n");
}

/* The dispatcher's test of clause `clause`: its literal and list patterns
 * against the parameters, then its guard over the names the guard reads,
 * bound from the parameters. Each is evaluated once, here; the clause
 * function does not test them again. Returns 1 when the clause matches
 * every call (no pattern, no guard), after which no clause is reached. */
static int emit_clause_dispatch(CodeGenerator* gen, ASTNode* clause, int returns_value) {
    ASTNode* guard = NULL;
    for (int i = 0; i < clause->child_count; i++) {
        ASTNode* c = clause->children[i];
        if (c && c->type == AST_GUARD_CLAUSE && c->child_count > 0) guard = c->children[0];
    }
    int npos = fn_param_count(clause);
    int has_cond = 0;
    for (int p = 0; p < npos; p++) {
        ASTNode* c = fn_param_at(clause, p);
        if (is_list_param_node(c) ||
            (c->type == AST_PATTERN_LITERAL && strcmp(c->value, "_") != 0)) has_cond = 1;
    }
    print_indent(gen);
    if (!has_cond && !guard) {
        emit_clause_call(gen, clause, returns_value);
        return 1;
    }
    if (has_cond) {
        fprintf(gen->output, "if (");
        int first_cond = 1;
        for (int p = 0; p < npos; p++) {
            ASTNode* c = fn_param_at(clause, p);
            if (c->type == AST_PATTERN_LITERAL && strcmp(c->value, "_") != 0) {
                if (!first_cond) fprintf(gen->output, " && ");
                if (c->node_type && c->node_type->kind == TYPE_STRING) {
                    /* #2467: a string pattern compares by content, NULL-safe, as
                     * a `match` string arm does (emit_selector_condition): the
                     * argument is a pointer and may be a magic AetherString. */
                    fprintf(gen->output, "(_arg%d && string_equals(_arg%d, ", p, p);
                    emit_string_literal_node(gen, c);   /* all its bytes (#2520) */
                    fprintf(gen->output, "))");
                } else {
                    fprintf(gen->output, "_arg%d == %s", p, c->value);
                }
                first_cond = 0;
            } else if (c->type == AST_PATTERN_LIST) {
                if (!first_cond) fprintf(gen->output, " && ");
                fprintf(gen->output, "_len%d == %d", p, c->child_count);
                first_cond = 0;
            } else if (c->type == AST_PATTERN_CONS) {
                if (!first_cond) fprintf(gen->output, " && ");
                fprintf(gen->output, "_len%d >= 1", p);
                first_cond = 0;
            }
        }
        fprintf(gen->output, ") {\n");
    } else {
        fprintf(gen->output, "{\n");
    }
    indent(gen);
    if (guard) {
        /* The guard reads the clause's parameters by name: bind the ones it
         * names. It was emitted with each name replaced by its parameter,
         * which reached only a guard made of names, operators and literals:
         * a call in it (`string.length(name) > 3`) read an undeclared name. */
        begin_dispatch_scope(gen, clause);
        gen->closure_var_scope = fn_scope_name(gen->program, clause);
        for (int p = 0; p < npos; p++) {
            ASTNode* c = fn_param_at(clause, p);
            if ((c->type == AST_PATTERN_VARIABLE || c->type == AST_VARIABLE_DECLARATION) &&
                c->value && expr_mentions(guard, c->value)) {
                emit_guard_binding(gen, fn_param_type_at(gen, clause, p), c->value,
                                   cg_internf("_arg%d", p));
                if (is_fnptr_type(c->node_type)) register_fnptr_local(gen, c->value, c->node_type);
            } else if (c->type == AST_PATTERN_LIST) {
                for (int j = 0; j < c->child_count; j++) {
                    ASTNode* e = c->children[j];
                    if (e && e->type == AST_PATTERN_VARIABLE && e->value && expr_mentions(guard, e->value))
                        emit_guard_binding(gen, e->node_type, e->value, cg_internf("_list%d[%d]", p, j));
                }
            } else if (c->type == AST_PATTERN_CONS) {
                ASTNode* h = c->child_count >= 1 ? c->children[0] : NULL;
                ASTNode* t = c->child_count >= 2 ? c->children[1] : NULL;
                if (h && h->type == AST_PATTERN_VARIABLE && h->value && expr_mentions(guard, h->value))
                    emit_guard_binding(gen, h->node_type, h->value, cg_internf("_list%d[0]", p));
                if (t && t->type == AST_PATTERN_VARIABLE && t->value && expr_mentions(guard, t->value)) {
                    print_line(gen, "%s* %s = &_list%d[1]; int %s_len = _len%d - 1; (void)%s; (void)%s_len;",
                               fn_list_param_elem_ctype(gen, clause, p), t->value, p, t->value, p,
                               t->value, t->value);
                    mark_var_declared(gen, t->value);
                }
            }
        }
        print_indent(gen);
        fprintf(gen->output, "if (");
        generate_expression(gen, guard);
        fprintf(gen->output, ") ");
    } else {
        print_indent(gen);
    }
    emit_clause_call(gen, clause, returns_value);
    unindent(gen);
    print_line(gen, "}");
    return 0;
}

/* #2644: a function written as several clauses (`f(0) -> ...`, `f(n) when
 * n > 0 -> ...`) is a set of real functions. Each clause is emitted as a
 * static C function of its own (clause_c_name) through the single-function
 * path, so it gets everything a single function's body gets: parameter
 * promotion for a closure that mutates one, copy-on-keep, struct and
 * fixed-size array parameters, trackers, defers, contracts and the
 * uniform-heap returns of its set's ownership. The set's name is the
 * dispatcher: it tests each clause's patterns and guard in order over its
 * `_argN` parameters and returns what the first matching clause's function
 * returns, passed through as it came (a clause function marks a string
 * handed through a fn pointer itself, #2586), or the no-match default of
 * the set's type. The clause bodies used to be inlined into one C function,
 * where the parts of a single body's setup each needed redoing for clauses,
 * and most were not. */
void generate_combined_function(CodeGenerator* gen, ASTNode** clauses, int clause_count) {
    if (clause_count == 0) return;
    ASTNode* first = clauses[0];
    for (int i = 0; i < clause_count; i++)
        emit_function(gen, clauses[i], clause_c_name(gen->program, clauses[i]));

    codegen_maybe_emit_line(gen, first);
    codegen_note_diag_pos(first);
    codegen_note_diag_func(first->value);
    Type* result = fn_result_type(gen, first);

    // Imported clause sets get the same `static` storage class as a single
    // function, and a @c_callback one the same weak external symbol (see
    // emit_function), the symbol its first annotated clause binds (#2664);
    // the prototype agrees (generate_program).
    ASTNode* cb_def = fn_c_callback_def(gen, first);
    if (!cb_def && fn_has_internal_linkage(first)) {
        fprintf(gen->output, fn_is_inline_candidate(first) ? "static inline AETHER_MAYBE_UNUSED "
                                                           : "static AETHER_MAYBE_UNUSED ");
    } else if (cb_def) {
        fprintf(gen->output, "AETHER_WEAK_DEF ");
    }
    emit_fn_result_c_type(gen, first);
    const char* cb_sym = cb_def ? c_callback_symbol(cb_def) : NULL;
    fprintf(gen->output, " %s(", cb_sym ? cb_sym : safe_c_name(first->value));
    int npos = fn_param_count(first);
    for (int p = 0; p < npos; p++) {
        if (p > 0) fprintf(gen->output, ", ");
        if (is_list_param_node(fn_param_at(first, p))) {
            fprintf(gen->output, "%s* _list%d, int _len%d",
                    fn_list_param_elem_ctype(gen, first, p), p, p);
            continue;
        }
        Type* t = fn_param_type_at(gen, first, p);
        if (is_fnptr_type(t)) {
            emit_fnptr_decl(gen, t, cg_internf("_arg%d", p));
        } else if (is_sized_array_param(t)) {
            /* #2516: a fixed-size array is passed as its first element's
             * address; the clause function copies it. */
            fprintf(gen->output, "%s _arg%d[%d]", get_c_type(t->element_type), p, t->array_size);
        } else {
            generate_type(gen, t);
            fprintf(gen->output, " _arg%d", p);
        }
    }
    if (first->type == AST_BUILDER_FUNCTION)
        fprintf(gen->output, "%svoid* _builder", npos > 0 ? ", " : "");
    else if (npos == 0)
        fprintf(gen->output, "void");
    fprintf(gen->output, ") {\n");
    indent(gen);

    /* A guard is emitted as an expression of its clause (emit_clause_dispatch),
     * with no promoted cells: the dispatcher's bindings are plain copies. */
    Type* prev_return_type = gen->current_func_return_type;
    ASTNode* prev_current_function = gen->current_function;
    const char* prev_closure_var_scope = gen->closure_var_scope;
    char** prev_promoted = gen->current_promoted_captures;
    int prev_promoted_count = gen->current_promoted_capture_count;
    gen->current_func_return_type = result;
    gen->current_promoted_captures = NULL;
    gen->current_promoted_capture_count = 0;
    gen->defer_count = 0;
    gen->scope_depth = 0;
    enter_scope(gen);

    int caught_all = 0;
    for (int i = 0; i < clause_count && !caught_all; i++)
        caught_all = emit_clause_dispatch(gen, clauses[i], result != NULL);

    exit_scope(gen);
    if (!caught_all && result) {
        print_indent(gen);
        fprintf(gen->output, "return ");
        emit_no_match_value(gen, first);
        fprintf(gen->output, ";\n");
    }

    gen->current_func_return_type = prev_return_type;
    gen->current_function = prev_current_function;
    gen->closure_var_scope = prev_closure_var_scope;
    gen->current_promoted_captures = prev_promoted;
    gen->current_promoted_capture_count = prev_promoted_count;
    unindent(gen);
    print_line(gen, "}");
    print_line(gen, "");
}

/* Emit one field of an extern struct body.
 *
 * Dispatches on field->type:
 *  - AST_STRUCT_FIELD         — leaf: emits `T name;` (handles array,
 *                               bitfield, plain type cases as before).
 *  - AST_STRUCT_FIELD_UNION   — emits `union { ... } name;` with the
 *                               compound's children as its members
 *                               (recursive).
 *  - AST_STRUCT_FIELD_NESTED  — same but `struct { ... } name;`.
 *
 * The `is_last_in_parent` flag is forwarded only for the flexible-array
 * heuristic on the top-level extern struct; nested fields are never
 * the trailing slot of the outer struct from this function's perspective,
 * so the caller passes 0 for them.
 */
/* #749: emit a function-pointer struct field as `R (*name)(T1, T2)`.
 * The field name sits inside the `(*...)` declarator, so the plain
 * `<type> name` shape can't express it (get_c_type collapses an fn-ptr
 * type to "void*"). Mirrors the typed indirect-call cast in
 * codegen_expr.c so a field, the C layout, and a call through it agree.
 * Local to this TU (the fn-ptr-parameter feature has a sibling exported
 * helper; keep this one file-static to avoid a cross-PR symbol clash). */
static void emit_fnptr_struct_field(CodeGenerator* gen, Type* sig, const char* name) {
    /* #2651: the shared spelling, nested fn-pointer parameters included. */
    fputs(fnptr_c_spelling(sig, name), gen->output);
}

static void generate_extern_struct_field(CodeGenerator* gen, ASTNode* field,
                                         int is_extern, int is_last_in_parent) {
    if (!field) return;
    if (field->type == AST_STRUCT_FIELD_UNION ||
        field->type == AST_STRUCT_FIELD_NESTED) {
        print_indent(gen);
        fprintf(gen->output, "%s {\n",
                field->type == AST_STRUCT_FIELD_UNION ? "union" : "struct");
        indent(gen);
        for (int i = 0; i < field->child_count; i++) {
            generate_extern_struct_field(gen, field->children[i], is_extern, 0);
        }
        unindent(gen);
        print_indent(gen);
        fprintf(gen->output, "} %s;\n", field->value);
        return;
    }
    if (field->type != AST_STRUCT_FIELD) return;

    print_indent(gen);
    if (field->node_type && field->node_type->kind == TYPE_ARRAY) {
        const char* element_type = get_c_type(field->node_type->element_type);
        if (field->node_type->array_size > 0) {
            fprintf(gen->output, "%s %s[%d];\n", element_type, field->value, field->node_type->array_size);
        } else if (is_extern && is_last_in_parent) {
            fprintf(gen->output, "%s %s[];\n", element_type, field->value);
        } else if (is_extern) {
            fprintf(gen->output, "%s* %s;\n", element_type, field->value);
        } else {
            /* #1286: an Aether struct's `T[]` field is a slice. */
            fprintf(gen->output, "AetherSlice %s;\n", field->value);
        }
    } else if (field->bit_width > 0) {
        generate_type(gen, field->node_type);
        fprintf(gen->output, " %s : %d;\n", field->value, field->bit_width);
    } else if (field->node_type && field->node_type->kind == TYPE_FUNCTION &&
               field->node_type->is_fnptr) {
        /* #749: typed function-pointer field (a `dictType`-style vtable
         * member) — emit `R (*name)(T1,T2)`, not `void* name`. */
        emit_fnptr_struct_field(gen, field->node_type, field->value);
        fprintf(gen->output, ";\n");
    } else {
        generate_type(gen, field->node_type);
        fprintf(gen->output, " %s;\n", field->value);
    }
}

void generate_struct_definition(CodeGenerator* gen, ASTNode* struct_def) {
    if (!struct_def || struct_def->type != AST_STRUCT_DEFINITION) return;

    /* `extern type Name` — an opaque, header-defined C type.  Its
     * complete emission is the incomplete forward typedef
     * `typedef struct Name Name;` that the forward-typedef hoist pass
     * already emits for every struct definition.  Emitting nothing
     * more here is deliberate: an opaque type has no body, `Name*` is
     * a usable pointer, and field access stays a compile error.
     * Matches a C header's own `typedef struct Name Name;` exactly.
     * See redis-porting-language-gaps.md "P0: Typed And Qualified C
     * Pointers". */
    if (struct_def->annotation &&
        strcmp(struct_def->annotation, "extern_opaque") == 0) {
        return;
    }

    /* `extern struct ... @c_import` — the struct's layout is imported
     * from a C header, not emitted by Aether.  Emit nothing: no body,
     * no typedef.  The included header is the sole source of truth
     * for size, layout and padding; field access still typechecks
     * because the AST_STRUCT_DEFINITION node carries the declared
     * fields.  See redis-porting-language-gaps.md "P0: Header-Defined
     * C Struct Interop". */
    if (aether_is_c_import_annotation(struct_def->annotation)) {
        return;
    }

    /* `extern struct` (annotation="extern") gets two opt-in C
     * spellings that don't apply to regular Aether structs:
     *  - bit-width annotations on integer fields emit C bitfields
     *  - a trailing array field with no explicit size emits a C
     *    flexible-array `T name[];` instead of the pointer-shaped
     *    `T* name;` an Aether-managed dynamic array would use. */
    int is_extern = struct_def->annotation &&
                    (strcmp(struct_def->annotation, "extern") == 0 ||
                     strcmp(struct_def->annotation, "extern_packed") == 0);

    /* #747: `@packed` emits the C body with __attribute__((packed)) so
     * the layout has no inter-field padding — the sdshdr8/16/32/64 shape
     * a port needs to overlay a packed C struct. The attribute goes
     * between `struct` and the tag (GCC/Clang spelling) so it applies to
     * the type. */
    int is_packed = struct_def->annotation &&
                    strcmp(struct_def->annotation, "extern_packed") == 0;

    // Generate C struct
    if (is_packed) {
        print_line(gen, "typedef struct __attribute__((packed)) %s {", struct_def->value);
    } else {
        print_line(gen, "typedef struct %s {", struct_def->value);
    }
    indent(gen);

    /* Heap-string ownership tracking for struct fields (#465).
     *
     * For each `string`-typed field, we emit a sibling
     * `int _heap_<fieldname>` companion tracker so the matching
     * reassign-wrapper at field-write sites can `free` the
     * previous value when (and only when) it was heap-allocated.
     * The struct's auto-emitted `<Name>_destroy()` (below) reads
     * the same trackers to free heap fields at scope exit.
     *
     * The hidden trackers grow the struct by 4 bytes per string
     * field. For pure-Aether structs this is acceptable; for
     * structs that cross an FFI boundary, callers that
     * hand-declare the struct in C won't have the trackers — they
     * get the same field-only layout they already had (the trackers
     * sit after the declared fields, so up-to-and-including the
     * last user-declared field the offsets match). Strict-ABI
     * structs that need binary stability can opt out by declaring
     * fields as `ptr` instead of `string` (no tracker emitted). */
    int has_string_field = 0;
    int first_string_idx = -1;
    /* #1879-regression: for a pure-Aether struct, place each `_heap_<field>`
     * tracker IMMEDIATELY AFTER its string field rather than appending all
     * trackers at the end. Trailing placement kept the NAMED fields' offsets
     * stable, but it moved the trackers to a different offset in two structs
     * that share a named-field prefix — the deliberate punning idiom of
     * allocating a wide struct and writing it through a narrow prefix type
     * (`heap.new(SignalOptions)` written via `*CommonOptions`). A tracker
     * store through the narrow view then landed on a real data field of the
     * wide object: silent corruption. Inline placement makes the narrow
     * struct a true MEMORY prefix of the wide one — its tracker sits at the
     * same offset in both — so the pun is sound again.
     *
     * Extern structs keep the trailing layout: a hand-declared C counterpart
     * has no trackers, and trailing placement keeps the declared fields at
     * their C offsets up to the last one. An extern struct that needs strict
     * binary layout still opts out entirely by declaring `ptr` not `string`
     * (no tracker emitted at all). */
    int inline_trackers = !is_extern;
    for (int i = 0; i < struct_def->child_count; i++) {
        ASTNode* field = struct_def->children[i];
        int is_last = (i == struct_def->child_count - 1);
        if (field->type == AST_STRUCT_FIELD ||
            field->type == AST_STRUCT_FIELD_UNION ||
            field->type == AST_STRUCT_FIELD_NESTED) {
            generate_extern_struct_field(gen, field, is_extern, is_last);

            if (field->type == AST_STRUCT_FIELD &&
                field->node_type && field->node_type->kind == TYPE_STRING) {
                has_string_field = 1;
                if (first_string_idx < 0) first_string_idx = i;
                if (inline_trackers) {
                    print_indent(gen);
                    fprintf(gen->output, "int _heap_%s;\n", field->value);
                }
            }
        }
    }

    /* Extern structs still append trackers after all declared fields, to keep
     * the declared-field offsets matching a hand-written C struct. */
    if (has_string_field && !inline_trackers) {
        for (int i = 0; i < struct_def->child_count; i++) {
            ASTNode* field = struct_def->children[i];
            if (field->type == AST_STRUCT_FIELD &&
                field->node_type && field->node_type->kind == TYPE_STRING) {
                print_indent(gen);
                fprintf(gen->output, "int _heap_%s;\n", field->value);
            }
        }
    }

    unindent(gen);
    print_line(gen, "} %s;", struct_def->value);

    /* Auto-emit a destructor `<Name>_destroy(<Name>* s)` that
     * walks every heap-string field and frees the buffer when the
     * matching tracker is set. Called from the scope-exit defer
     * for local struct variables (codegen_stmt.c pushes the
     * defer at struct-literal initialization sites). Idempotent —
     * each free zeroes the tracker so a second call no-ops.
     *
     * #2497: a field that is itself a string-owning struct, held by
     * value, is part of this value: its strings are released with it
     * (through its own destroy / replace). Without that, `o.inner.name`
     * leaked whenever `o` was replaced or went out of scope, and a struct
     * whose only strings were nested got no destructor at all. */
    if (struct_owns_heap_strings(gen, struct_def)) {
        print_line(gen, "static inline void %s_destroy(%s* s) {",
                   struct_def->value, struct_def->value);
        indent(gen);
        print_line(gen, "if (!s) return;");
        for (int i = 0; i < struct_def->child_count; i++) {
            ASTNode* field = struct_def->children[i];
            if (field->type == AST_STRUCT_FIELD &&
                field->node_type && field->node_type->kind == TYPE_STRING) {
                print_line(gen, "if (s->_heap_%s) { aether_heap_str_free(s->%s); s->%s = (const char*)0; s->_heap_%s = 0; }",
                           field->value, field->value, field->value, field->value);
            }
            /* #2525: the field's reference to the closure's env goes back;
             * cleared so a second destroy releases nothing. */
            if (struct_field_is_closure(field)) {
                print_line(gen, "if (s->%s.env) { _aether_closure_env_release(s->%s.env); s->%s.env = (void*)0; }",
                           field->value, field->value, field->value);
            }
            /* #2528: every element of an owned array goes with the struct. */
            int oalen = 0;
            int oak = struct_field_owned_array(field, &oalen);
            if (oak == 1) {
                print_line(gen, "for (int _ai = 0; _ai < %d; _ai++) if (s->%s[_ai]) { aether_heap_str_free(s->%s[_ai]); s->%s[_ai] = (const char*)0; }",
                           oalen, field->value, field->value, field->value);
            } else if (oak == 2) {
                print_line(gen, "for (int _ai = 0; _ai < %d; _ai++) if (s->%s[_ai].env) { _aether_closure_env_release(s->%s[_ai].env); s->%s[_ai].env = (void*)0; }",
                           oalen, field->value, field->value, field->value);
            }
            int alen = 0;
            ASTNode* inner = owning_struct_field_def_n(gen, field, &alen);
            if (inner && alen > 0) {
                /* #2525: each element of a fixed-size array field is a
                 * value of its own, released with the struct. */
                print_line(gen, "for (int _ai = 0; _ai < %d; _ai++) %s_destroy(&s->%s[_ai]);",
                           alen, inner->value, field->value);
            } else if (inner) {
                print_line(gen, "%s_destroy(&s->%s);", inner->value, field->value);
            }
        }
        unindent(gen);
        print_line(gen, "}");

        /* #790: typed free for a heap.new'd box that owns string fields —
         * release every owned field (via the destructor), then free the box
         * itself. heap.free(p) routes here when p's struct has heap fields;
         * the POD path stays a plain free(p). NULL-safe. */
        print_line(gen, "static inline void %s_heap_free(%s* s) {",
                   struct_def->value, struct_def->value);
        indent(gen);
        print_line(gen, "if (!s) return;");
        print_line(gen, "%s_destroy(s);", struct_def->value);
        print_line(gen, "free(s);");
        unindent(gen);
        print_line(gen, "}");

        /* `<Name>_replace(dst, src)`: overwrite a struct that owns string
         * fields with a new value. Each string the old value owns is freed,
         * unless the new value holds that same string (`r = Rec { name:
         * r.name }`, or `r = pass_through(r)`): then it is kept, and moves
         * to the new value unless the new value already owns it. Freeing
         * first and assigning after (#465) left such a field pointing at
         * freed memory. One owner per string: a string the new value holds
         * in two fields moves to the first. */
        print_line(gen, "static inline void %s_replace(%s* dst, %s src) {",
                   struct_def->value, struct_def->value, struct_def->value);
        indent(gen);
        for (int i = 0; i < struct_def->child_count; i++) {
            ASTNode* f = struct_def->children[i];
            if (!(f->type == AST_STRUCT_FIELD && f->node_type &&
                  f->node_type->kind == TYPE_STRING)) continue;
            print_line(gen, "if (dst->_heap_%s) {", f->value);
            indent(gen);
            /* held: the new value holds the string; owned: one of its
             * fields already owns it (a struct that came back through a
             * call, say). Held and not owned: the first holder takes it.
             * Not held: nothing else refers to it, so it is freed. */
            print_indent(gen);
            fprintf(gen->output, "int held = 0, owned = 0;");
            for (int j = 0; j < struct_def->child_count; j++) {
                ASTNode* g = struct_def->children[j];
                if (!(g->type == AST_STRUCT_FIELD && g->node_type &&
                      g->node_type->kind == TYPE_STRING)) continue;
                fprintf(gen->output, " if (src.%s == dst->%s) { held = 1; owned |= src._heap_%s; }",
                        g->value, f->value, g->value);
            }
            fprintf(gen->output, "\n");
            print_line(gen, "if (!held) aether_heap_str_free(dst->%s);", f->value);
            print_indent(gen);
            fprintf(gen->output, "else if (!owned) {");
            int first = 1;
            for (int j = 0; j < struct_def->child_count; j++) {
                ASTNode* g = struct_def->children[j];
                if (!(g->type == AST_STRUCT_FIELD && g->node_type &&
                      g->node_type->kind == TYPE_STRING)) continue;
                fprintf(gen->output, " %sif (src.%s == dst->%s) src._heap_%s = 1;",
                        first ? "" : "else ", g->value, f->value, g->value);
                first = 0;
            }
            fprintf(gen->output, " }\n");
            unindent(gen);
            print_line(gen, "}");
        }
        /* #2497: a nested owning struct is replaced by its own rule, and the
         * ownership it settles on is what the new outer value carries. */
        for (int i = 0; i < struct_def->child_count; i++) {
            ASTNode* f = struct_def->children[i];
            int alen = 0;
            ASTNode* inner = owning_struct_field_def_n(gen, f, &alen);
            if (!inner) continue;
            if (alen > 0) {
                print_line(gen, "for (int _ai = 0; _ai < %d; _ai++) { %s_replace(&dst->%s[_ai], src.%s[_ai]); src.%s[_ai] = dst->%s[_ai]; }",
                           alen, inner->value, f->value, f->value, f->value, f->value);
            } else {
                print_line(gen, "%s_replace(&dst->%s, src.%s); src.%s = dst->%s;",
                           inner->value, f->value, f->value, f->value, f->value);
            }
        }
        /* #2525: the new value carries a reference of its own per closure
         * field (a fresh closure's, or one the take retained), so the old
         * value's goes back whether or not both name the same env. */
        for (int i = 0; i < struct_def->child_count; i++) {
            ASTNode* f = struct_def->children[i];
            if (!struct_field_is_closure(f)) continue;
            print_line(gen, "_aether_closure_env_release(dst->%s.env);", f->value);
        }
        /* #2528: an owned array's old elements go, except a string the new
         * value holds at the same slot (`r = Rec { names: r.names }`), which
         * stays its own. */
        for (int i = 0; i < struct_def->child_count; i++) {
            ASTNode* f = struct_def->children[i];
            int oalen = 0;
            int oak = struct_field_owned_array(f, &oalen);
            if (oak == 1) {
                print_line(gen, "for (int _ai = 0; _ai < %d; _ai++) if (dst->%s[_ai] && dst->%s[_ai] != src.%s[_ai]) aether_heap_str_free(dst->%s[_ai]);",
                           oalen, f->value, f->value, f->value, f->value);
            } else if (oak == 2) {
                print_line(gen, "for (int _ai = 0; _ai < %d; _ai++) _aether_closure_env_release(dst->%s[_ai].env);",
                           oalen, f->value);
            }
        }
        print_line(gen, "*dst = src;");
        unindent(gen);
        print_line(gen, "}");

        /* #2497: `<Name>_dup(src)`: a value of its own, for a slot that takes
         * a struct another owner keeps (a variable still in use, a field, an
         * element). Every string src owns is copied; one it only borrows
         * stays borrowed, as it was in src. */
        print_line(gen, "static inline %s %s_dup(%s src) {",
                   struct_def->value, struct_def->value, struct_def->value);
        indent(gen);
        for (int i = 0; i < struct_def->child_count; i++) {
            ASTNode* f = struct_def->children[i];
            if (f->type == AST_STRUCT_FIELD && f->node_type &&
                f->node_type->kind == TYPE_STRING) {
                print_line(gen, "if (src._heap_%s) src.%s = aether_uniform_heap_str(src.%s, 0);",
                           f->value, f->value, f->value);
            }
            /* #2525: the copy holds a reference of its own to the env. */
            if (struct_field_is_closure(f)) {
                print_line(gen, "_aether_closure_env_retain(src.%s.env);", f->value);
            }
            /* #2528: an owned array's elements are copied or retained. */
            int oalen = 0;
            int oak = struct_field_owned_array(f, &oalen);
            if (oak == 1) {
                print_line(gen, "for (int _ai = 0; _ai < %d; _ai++) if (src.%s[_ai]) src.%s[_ai] = aether_uniform_heap_str(src.%s[_ai], 0);",
                           oalen, f->value, f->value, f->value);
            } else if (oak == 2) {
                print_line(gen, "for (int _ai = 0; _ai < %d; _ai++) _aether_closure_env_retain(src.%s[_ai].env);",
                           oalen, f->value);
            }
            int alen = 0;
            ASTNode* inner = owning_struct_field_def_n(gen, f, &alen);
            if (inner && alen > 0) {
                print_line(gen, "for (int _ai = 0; _ai < %d; _ai++) src.%s[_ai] = %s_dup(src.%s[_ai]);",
                           alen, f->value, inner->value, f->value);
            } else if (inner) {
                print_line(gen, "src.%s = %s_dup(src.%s);", f->value, inner->value, f->value);
            }
        }
        print_line(gen, "return src;");
        unindent(gen);
        print_line(gen, "}");

        /* #2458: the release for a shared cell holding one of these (a
         * variable a closure writes; emit_promoted_cell_declaration): the
         * last holder releases the owned fields, then the cell, as a scope
         * exit runs `<Name>_destroy` on a local. */
        print_line(gen, "static inline void %s_cell_release(void* cell) {",
                   struct_def->value);
        indent(gen);
        print_line(gen, "if (!cell) return;");
        print_line(gen, "_AeCellHeader* h = (_AeCellHeader*)cell - 1;");
        print_line(gen, "if (_aether_cell_last(h)) { %s_destroy((%s*)cell); free(h); }",
                   struct_def->value, struct_def->value);
        unindent(gen);
        print_line(gen, "}");
    }
    print_line(gen, "");
}

/* Predicate: does `struct_def` have any string-typed field that
 * needs heap-ownership tracking? Used by the codegen_stmt.c
 * struct-local-declaration site to decide whether to push the
 * function-exit destructor defer. */
/* #2497: the definition of `field`'s struct type when the field holds that
 * struct by value and it owns heap strings; NULL otherwise (a string, a
 * pointer, a struct with nothing to release). */
static int struct_owns_heap_strings_at(CodeGenerator* gen, ASTNode* struct_def, int depth);

/* The struct type a field holds by value, directly or (#2525) as the
 * element of a fixed-size array (`slots: Holder[2]`, whose length goes to
 * `*len`; 0 for a direct field); NULL for anything else. */
static Type* field_value_struct_type(ASTNode* field, int* len) {
    if (!field || field->type != AST_STRUCT_FIELD || !field->node_type) return NULL;
    Type* t = field->node_type;
    *len = 0;
    if (t->kind == TYPE_ARRAY && t->array_size > 0 && t->element_type) {
        *len = t->array_size;
        t = t->element_type;
    }
    if (t->kind != TYPE_STRUCT || !t->struct_name || aether_is_c_import_struct(t->struct_name)) return NULL;
    return t;
}

ASTNode* owning_struct_field_def_n(CodeGenerator* gen, ASTNode* field, int* len) {
    int n = 0;
    Type* t = gen && gen->program ? field_value_struct_type(field, &n) : NULL;
    if (!t) return NULL;
    ASTNode* def = find_struct_definition_by_name(gen->program, t->struct_name);
    if (!def || !struct_owns_heap_strings_at(gen, def, 1)) return NULL;
    if (len) *len = n;
    return def;
}

ASTNode* owning_struct_field_def(CodeGenerator* gen, ASTNode* field) {
    int n = 0;
    ASTNode* def = owning_struct_field_def_n(gen, field, &n);
    return n == 0 ? def : NULL;
}

static int struct_owns_heap_strings_at(CodeGenerator* gen, ASTNode* struct_def, int depth) {
    if (struct_has_heap_string_field(struct_def)) return 1;
    /* A struct cannot hold itself by value; the bound only stops a
     * malformed program from recursing forever. */
    if (!struct_def || struct_def->type != AST_STRUCT_DEFINITION || depth > 32) return 0;
    if (aether_is_c_import_annotation(struct_def->annotation)) return 0;
    for (int i = 0; i < struct_def->child_count; i++) {
        int n = 0;
        Type* t = (gen && gen->program) ? field_value_struct_type(struct_def->children[i], &n) : NULL;
        if (!t) continue;
        ASTNode* def = find_struct_definition_by_name(gen->program, t->struct_name);
        if (def && def != struct_def &&
            struct_owns_heap_strings_at(gen, def, depth + 1)) return 1;
    }
    return 0;
}

int struct_owns_heap_strings(CodeGenerator* gen, ASTNode* struct_def) {
    return struct_owns_heap_strings_at(gen, struct_def, 0);
}

int struct_field_is_closure(ASTNode* field) {
    return field && field->type == AST_STRUCT_FIELD && field->node_type &&
           field->node_type->kind == TYPE_FUNCTION && !field->node_type->is_fnptr;
}

/* #2528: a fixed-size array field whose elements the struct owns: 1 for
 * `string[N]` (every element a heap copy of its own, as a string array cell
 * holds them, #2474), 2 for `fn[N]` (a reference per element); 0 otherwise.
 * The length goes to `*len`. */
int struct_field_owned_array(ASTNode* field, int* len) {
    if (!field || field->type != AST_STRUCT_FIELD || !field->node_type) return 0;
    Type* t = field->node_type;
    if (t->kind != TYPE_ARRAY || t->array_size <= 0 || !t->element_type) return 0;
    Type* e = t->element_type;
    if (len) *len = t->array_size;
    if (e->kind == TYPE_STRING) return 1;
    if (e->kind == TYPE_FUNCTION && !e->is_fnptr) return 2;
    return 0;
}

/* A string field, or (#2525) a closure field, whose env the struct holds a
 * reference to: either makes the struct an owner with a destructor. */
int struct_has_heap_string_field(ASTNode* struct_def) {
    if (!struct_def || struct_def->type != AST_STRUCT_DEFINITION) return 0;
    /* A header-defined struct's string fields borrow; nothing tracks them,
     * and no `<Name>_destroy` / `<Name>_heap_free` is emitted for it. */
    if (aether_is_c_import_annotation(struct_def->annotation)) return 0;
    for (int i = 0; i < struct_def->child_count; i++) {
        ASTNode* field = struct_def->children[i];
        if (field && field->type == AST_STRUCT_FIELD &&
            field->node_type && field->node_type->kind == TYPE_STRING) {
            return 1;
        }
        if (struct_field_is_closure(field)) return 1;
        if (struct_field_owned_array(field, NULL)) return 1;   /* #2528 */
    }
    return 0;
}

/* Lookup helper: find an AST_STRUCT_DEFINITION by struct name in
 * the program AST. Returns NULL if not found. Used by the codegen
 * site to decide whether a local-struct declaration needs the
 * destructor-defer push. */
ASTNode* find_struct_definition_by_name(ASTNode* program, const char* name) {
    if (!program || !name) return NULL;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_STRUCT_DEFINITION &&
            c->value && strcmp(c->value, name) == 0) {
            return c;
        }
    }
    return NULL;
}
