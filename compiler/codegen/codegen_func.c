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

// Look up a top-level @c_callback function by its current AST value
// (post-merge: the prefixed `<ns>_<name>` form for imported callbacks;
// the bare name for in-file ones) and return the C symbol it's bound
// to. Returns NULL when no such callback exists, so the caller can
// fall through to the default identifier-emission path.
const char* lookup_c_callback_symbol(CodeGenerator* gen, const char* name) {
    if (!gen || !gen->program || !name) return NULL;
    for (int i = 0; i < gen->program->child_count; i++) {
        ASTNode* top = gen->program->children[i];
        if (!top || !top->value) continue;
        if (top->type != AST_FUNCTION_DEFINITION &&
            top->type != AST_BUILDER_FUNCTION) continue;
        if (!is_c_callback(top)) continue;
        if (strcmp(top->value, name) == 0) {
            return c_callback_symbol(top);
        }
    }
    return NULL;
}

// Extract the C symbol bound by `@extern("c_symbol")` into `buf`.
// Returns 1 if `ext` was declared via `@extern` (annotation begins
// with "c_symbol:"), 0 otherwise. The stored annotation is
// `c_symbol:NAME` for a plain `@extern` and `c_symbol:NAME;varargs`
// when the `@extern` declaration also carries a trailing `...`; the
// `;` delimiter never occurs inside a C identifier, so only NAME is
// copied out regardless.
static int extern_c_symbol(const ASTNode* ext, char* buf, size_t bufsz) {
    if (!ext || !ext->annotation || bufsz == 0) return 0;
    if (strncmp(ext->annotation, "c_symbol:", 9) != 0) return 0;
    const char* s = ext->annotation + 9;
    const char* semi = strchr(s, ';');
    size_t n = semi ? (size_t)(semi - s) : strlen(s);
    if (n >= bufsz) n = bufsz - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';
    return 1;
}

// Is `ext` a variadic extern? True both for the bare
// `extern foo(fmt: string, ...)` form (annotation == "varargs") and
// the `@extern("sym") foo(fmt: string, ...)` form (annotation
// "c_symbol:NAME;varargs"). The `;varargs` suffix is matched with a
// leading delimiter so a C symbol that merely contains the substring
// "varargs" can never be misread as variadic.
static int extern_is_varargs(const ASTNode* ext) {
    if (!ext || !ext->annotation) return 0;
    return strcmp(ext->annotation, "varargs") == 0 ||
           strstr(ext->annotation, ";varargs") != NULL;
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
        char c_sym[256];
        if (extern_c_symbol(ext, c_sym, sizeof(c_sym))) {
            gen->extern_registry[idx].c_name = strdup(c_sym);
        }
    }
    gen->extern_registry[idx].param_count = ext->child_count;
    gen->extern_registry[idx].params = NULL;
    gen->extern_registry[idx].param_full = NULL;
    gen->extern_registry[idx].params_aether = NULL;
    gen->extern_registry[idx].params_retain = NULL;

    if (ext->child_count > 0) {
        gen->extern_registry[idx].params = malloc(ext->child_count * sizeof(TypeKind));
        /* Param annotations are stored as a comma-separated set on
         * `param->annotation` so multiple stack on one slot
         * (`@aether @retain string`). Test for an individual tag
         * via substring match. The lazy-allocation pattern below
         * keeps the per-extern parallel arrays NULL in the common
         * (no-annotation) case. */
        int any_aether = 0, any_retain = 0;
        for (int i = 0; i < ext->child_count; i++) {
            ASTNode* param = ext->children[i];
            if (!param || !param->annotation) continue;
            if (strstr(param->annotation, "aether_param")) any_aether = 1;
            if (strstr(param->annotation, "retain_param")) any_retain = 1;
        }
        if (any_aether) {
            gen->extern_registry[idx].params_aether =
                calloc(ext->child_count, sizeof(int));
        }
        if (any_retain) {
            gen->extern_registry[idx].params_retain =
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
            }
        }
    }
}

// Normalize a function name by replacing dots with underscores (for module-qualified calls).
// Writes into a caller-provided buffer.
static void normalize_func_name(const char* name, char* buf, int buf_size) {
    strncpy(buf, name, buf_size - 1);
    buf[buf_size - 1] = '\0';
    for (char* p = buf; *p; p++) {
        if (*p == '.') *p = '_';
    }
}

// Check if a function name is registered as a builder function.
int is_builder_func_reg(CodeGenerator* gen, const char* func_name) {
    if (!gen || !func_name) return 0;
    char normalized[256];
    normalize_func_name(func_name, normalized, sizeof(normalized));
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
    for (int i = 0; i < gen->program->child_count; i++) {
        ASTNode* c = gen->program->children[i];
        if (c && (c->type == AST_FUNCTION_DEFINITION ||
                  c->type == AST_BUILDER_FUNCTION) &&
            c->value && strcmp(c->value, name) == 0) {
            return c;
        }
    }
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
        char merged[512];
        size_t ns_len = (size_t)(last_dot - ns_start);
        if (ns_len > 0 && ns_len + 1 + strlen(fn) + 1 <= sizeof(merged)) {
            memcpy(merged, ns_start, ns_len);
            merged[ns_len] = '_';
            strcpy(merged + ns_len + 1, fn);
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
    int pc = 0;
    for (int k = 0; k < fdef->child_count && pc < 16; k++) {
        ASTNode* p = fdef->children[k];
        if (!p) continue;
        if (p->type == AST_GUARD_CLAUSE || p->type == AST_BLOCK) continue;
        params[pc++] = p;
    }
    *param_count = pc;
    Type* rt = fdef->node_type;
    const char* ret_c = (rt && rt->kind != TYPE_UNKNOWN) ? get_c_type(rt) : "void";
    *ret_c_out = ret_c;
    fprintf(gen->output, "static %s _aether_bare_adapter_%s(void* _env",
            ret_c, fname);
    for (int k = 0; k < pc; k++) {
        ASTNode* p = params[k];
        const char* pt = p->node_type ? get_c_type(p->node_type) : "int";
        fprintf(gen->output, ", %s _a%d", pt, k);
    }
    fprintf(gen->output, ")");
    return fdef;
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
        Type* rt = fdef->node_type;
        fprintf(gen->output, " {\n    (void)_env;\n    ");
        if (rt && rt->kind != TYPE_VOID && rt->kind != TYPE_UNKNOWN) {
            fprintf(gen->output, "return ");
        }
        fprintf(gen->output, "%s(", fname);
        for (int k = 0; k < param_count; k++) {
            if (k > 0) fprintf(gen->output, ", ");
            fprintf(gen->output, "_a%d", k);
        }
        fprintf(gen->output, ");\n}\n");
    }
}

// Get the factory function for a builder function (default: "map_new").
const char* get_builder_factory(CodeGenerator* gen, const char* func_name) {
    if (!gen || !func_name) return "map_new";
    char normalized[256];
    normalize_func_name(func_name, normalized, sizeof(normalized));
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
    /* Try the dot-normalised form: "ns.fn" → "ns_fn". Bounded
     * to a small stack buffer; truncation just means the
     * lookup misses, which preserves current behaviour. */
    if (strchr(func_name, '.')) {
        char buf[256];
        size_t n = strlen(func_name);
        if (n < sizeof(buf)) {
            memcpy(buf, func_name, n);
            buf[n] = '\0';
            for (char* p = buf; *p; p++) {
                if (*p == '.') *p = '_';
            }
            for (int i = 0; i < gen->extern_registry_count; i++) {
                if (gen->extern_registry[i].name &&
                    strcmp(gen->extern_registry[i].name, buf) == 0) {
                    return i;
                }
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
    char c_sym_buf[256];
    if (extern_c_symbol(ext, c_sym_buf, sizeof(c_sym_buf))) {
        c_name = c_sym_buf;
    }

    if (extern_name_is_libc_conflict(c_name)) {
        fprintf(gen->output,
                "// Extern C function: %s (libc-provided, declaration skipped)\n",
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
                        fprintf(gen->output, "struct %s*", sname);
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
                                fprintf(gen->output, "struct %s*", sname);
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

void generate_function_definition(CodeGenerator* gen, ASTNode* func) {
    if (!func || (func->type != AST_FUNCTION_DEFINITION && func->type != AST_BUILDER_FUNCTION)) return;

    // Emit a `#line` directive at the function's definition line so
    // codegen sees a clean reset every time it crosses into a new
    // function — important when the merged program intermixes
    // user-written and module-imported functions in arbitrary order.
    codegen_maybe_emit_line(gen, func);

    // If function returns a tuple with UNKNOWN elements, scan all returns and merge
    if (func->node_type && func->node_type->kind == TYPE_TUPLE) {
        merge_return_tuple_types(func, func->node_type);
    }

    // Track current function's return type for multi-return codegen
    gen->current_func_return_type = func->node_type;
    // Track the function's AST node so AST_RETURN_STATEMENT codegen
    // can find any `ensures` clauses attached to it (issue #348).
    ASTNode* prev_current_function = gen->current_function;
    gen->current_function = func;

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
    if (fn_has_internal_linkage(func)) {
        fprintf(gen->output, "static ");
    }

    // Determine return type:
    //   - unannotated + has `return <value>` → int (legacy default).
    //   - unannotated + no return-with-value → void. Without this,
    //     the unresolved-type fallback emitted `int` and the C
    //     compiler warned `non-void function does not return a value`
    //     (issue #354). Functions like `wait_for_next_round` whose
    //     bodies are pure side-effect get a clean void signature.
    Type* ret_type = func->node_type;
    int ret_unannotated = (!ret_type
                           || ret_type->kind == TYPE_VOID
                           || ret_type->kind == TYPE_UNKNOWN);
    if (ret_unannotated && has_return_value(func)) {
        fprintf(gen->output, "int");
    } else if (ret_unannotated) {
        fprintf(gen->output, "void");
    } else {
        // Multi-value return — ensure the `_tuple_T1_T2` typedef is in
        // scope before the signature references it. The return-statement
        // path also calls this when emitting `return (_tuple_X){a, b};`,
        // but the signature is rendered first (#285).
        if (ret_type && ret_type->kind == TYPE_TUPLE) {
            ensure_tuple_typedef(gen, ret_type);
        }
        generate_type(gen, ret_type);
    }
    // For @c_callback, emit the chosen C symbol verbatim (no namespace
    // mangling); the symbol is what other translation units reach for
    // when they take the address of this function.
    const char* cb_sym = c_callback_symbol(func);
    fprintf(gen->output, " %s(", cb_sym ? cb_sym : safe_c_name(func->value));

    // Generate parameters - handle pattern matching
    int param_count = 0;
    ASTNode* body = NULL;
    // Track the C name of the last emitted named parameter — needed as
    // the second argument to va_start() if this function is variadic.
    char last_param_cname[256];
    last_param_cname[0] = '\0';

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
                snprintf(last_param_cname, sizeof(last_param_cname), "%s", child->value);
                param_count++;
                continue;
            }
            generate_type(gen, child->node_type);
            // If this parameter is a Route 1 promoted name in this function,
            // emit it as `_param_<name>` so the body's heap cell can use
            // the short name. Body prologue below does
            // `T* name = malloc(...); *name = _param_name;`.
            char** promoted = NULL;
            int promoted_count = 0;
            get_promoted_names_for_func(gen, func->value, &promoted, &promoted_count);
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
                snprintf(last_param_cname, sizeof(last_param_cname), "_param_%s", child->value);
            } else {
                fprintf(gen->output, " %s", child->value);
                snprintf(last_param_cname, sizeof(last_param_cname), "%s", child->value);
            }
            param_count++;
        } else if (child->type == AST_PATTERN_LITERAL) {
            // Pattern literal becomes regular parameter
            if (param_count > 0) fprintf(gen->output, ", ");
            generate_type(gen, child->node_type);
            fprintf(gen->output, " _pattern_%d", param_count);
            snprintf(last_param_cname, sizeof(last_param_cname), "_pattern_%d", param_count);
            param_count++;
        } else if (child->type == AST_PATTERN_STRUCT) {
            if (param_count > 0) fprintf(gen->output, ", ");
            fprintf(gen->output, "%s _pattern_%d", child->value, param_count);
            snprintf(last_param_cname, sizeof(last_param_cname), "_pattern_%d", param_count);
            param_count++;
        } else if (child->type == AST_PATTERN_LIST || child->type == AST_PATTERN_CONS) {
            // List pattern becomes array pointer
            if (param_count > 0) fprintf(gen->output, ", ");
            // Determine element type from first child's node_type if available
            const char* elem_ctype = "int";
            if (child->child_count > 0 && child->children[0]->node_type &&
                child->children[0]->node_type->kind != TYPE_UNKNOWN) {
                elem_ctype = get_c_type(child->children[0]->node_type);
            }
            fprintf(gen->output, "%s* _list_%d, int _len_%d", elem_ctype, param_count, param_count);
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
    int is_variadic = (func->annotation && strcmp(func->annotation, "varargs") == 0
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
    indent(gen);
    // Variadic prologue: declare the hidden va_list and prime it from
    // the last named parameter. va_start()/va_arg()/va_end() in the
    // body operate on `&__ae_va`.
    if (is_variadic) {
        fprintf(gen->output, "    va_list __ae_va; va_start(__ae_va, %s);\n",
                last_param_cname);
    }
    clear_declared_vars(gen);  // Reset for each function
    clear_heap_string_vars(gen);
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
    char** promoted_here = NULL;
    int promoted_here_count = 0;
    get_promoted_names_for_func(gen, func->value, &promoted_here, &promoted_here_count);
    for (int i = 0; i < func->child_count; i++) {
        ASTNode* child = func->children[i];
        if (!child) continue;
        if ((child->type == AST_PATTERN_VARIABLE || child->type == AST_VARIABLE_DECLARATION)
            && child->value) {
            int is_promoted = 0;
            for (int pp = 0; pp < promoted_here_count; pp++) {
                if (promoted_here[pp] &&
                    strcmp(promoted_here[pp], child->value) == 0) {
                    is_promoted = 1;
                    break;
                }
            }
            if (is_promoted) {
                const char* c_type = "int";
                if (child->node_type && child->node_type->kind != TYPE_UNKNOWN) {
                    c_type = get_c_type(child->node_type);
                }
                print_indent(gen);
                fprintf(gen->output, "%s* %s = malloc(sizeof(%s)); *%s = _param_%s;\n",
                        c_type, child->value, c_type, child->value, child->value);
                // Defer free(name) at function exit.
                ASTNode* free_call = create_ast_node(AST_FUNCTION_CALL, "free",
                    child->line, child->column);
                ASTNode* arg = create_ast_node(AST_IDENTIFIER, child->value,
                    child->line, child->column);
                arg->annotation = strdup("raw_promoted");
                add_child(free_call, arg);
                ASTNode* expr_stmt = create_ast_node(AST_EXPRESSION_STATEMENT, NULL,
                    child->line, child->column);
                add_child(expr_stmt, free_call);
                push_defer(gen, expr_stmt);
            }
        }
    }
    
    // Generate pattern matching checks
    int pattern_idx = 0;
    int list_idx = 0;
    
    for (int i = 0; i < func->child_count; i++) {
        ASTNode* child = func->children[i];
        
        if (child->type == AST_PATTERN_LITERAL &&
            strcmp(child->value, "_") != 0) {
            // Generate pattern match check
            print_indent(gen);
            fprintf(gen->output, "if (_pattern_%d != %s) return ",
                    pattern_idx, child->value);
            generate_default_return_value(gen, func->node_type);
            fprintf(gen->output, ";\n");
        }
        
        // Generate list pattern checks
        if (child->type == AST_PATTERN_LIST) {
            if (strcmp(child->value, "[]") == 0 && child->child_count == 0) {
                // Empty list check
                print_indent(gen);
                fprintf(gen->output, "if (_len_%d != 0) return ", list_idx);
                generate_default_return_value(gen, func->node_type);
                fprintf(gen->output, ";\n");
            } else {
                // Fixed-size list check
                print_indent(gen);
                fprintf(gen->output, "if (_len_%d != %d) return ",
                        list_idx, child->child_count);
                generate_default_return_value(gen, func->node_type);
                fprintf(gen->output, ";\n");
                
                // Bind pattern variables to list elements
                for (int j = 0; j < child->child_count; j++) {
                    ASTNode* elem = child->children[j];
                    if (elem->type == AST_PATTERN_VARIABLE) {
                        print_indent(gen);
                        const char* etype = "int";
                        if (elem->node_type && elem->node_type->kind != TYPE_UNKNOWN)
                            etype = get_c_type(elem->node_type);
                        fprintf(gen->output, "%s %s = _list_%d[%d];\n",
                                etype, elem->value, list_idx, j);
                    }
                }
            }
            list_idx++;
        } else if (child->type == AST_PATTERN_CONS) {
            // [H|T] pattern - check non-empty
            print_indent(gen);
            fprintf(gen->output, "if (_len_%d < 1) return ", list_idx);
            generate_default_return_value(gen, func->node_type);
            fprintf(gen->output, ";\n");
            
            // Bind head and tail
            if (child->child_count >= 1 && child->children[0]->type == AST_PATTERN_VARIABLE) {
                print_indent(gen);
                const char* htype = "int";
                if (child->children[0]->node_type && child->children[0]->node_type->kind != TYPE_UNKNOWN)
                    htype = get_c_type(child->children[0]->node_type);
                fprintf(gen->output, "%s %s = _list_%d[0];\n",
                        htype, child->children[0]->value, list_idx);
            }
            if (child->child_count >= 2 && child->children[1]->type == AST_PATTERN_VARIABLE) {
                print_indent(gen);
                const char* ttype = "int";
                if (child->children[1]->node_type && child->children[1]->node_type->kind != TYPE_UNKNOWN)
                    ttype = get_c_type(child->children[1]->node_type);
                fprintf(gen->output, "%s* %s = &_list_%d[1];\n",
                        ttype, child->children[1]->value, list_idx);
                print_indent(gen);
                fprintf(gen->output, "int %s_len = _len_%d - 1;\n",
                        child->children[1]->value, list_idx);
            }
            list_idx++;
        }
        
        if (child->type == AST_PATTERN_LITERAL ||
            child->type == AST_PATTERN_VARIABLE ||
            child->type == AST_PATTERN_STRUCT ||
            child->type == AST_VARIABLE_DECLARATION) {
            pattern_idx++;
        }
        
        // Generate guard clause check
        if (child->type == AST_GUARD_CLAUSE && child->child_count > 0) {
            print_indent(gen);
            fprintf(gen->output, "if (!(");
            generate_expression(gen, child->children[0]);
            fprintf(gen->output, ")) return ");
            generate_default_return_value(gen, func->node_type);
            fprintf(gen->output, ";\n");
        }
    }
    
    // Publish this function's promoted-captures set so var decls malloc
    // heap cells and reads/writes dereference. (Route 1.)
    char** prev_promoted = gen->current_promoted_captures;
    int prev_promoted_count = gen->current_promoted_capture_count;
    get_promoted_names_for_func(gen, func->value,
        &gen->current_promoted_captures, &gen->current_promoted_capture_count);

    // Issue #348 — emit `requires` precondition checks at function
    // entry. Each AST_REQUIRES_CLAUSE child of the function carries
    // a single boolean-expression child; codegen emits
    //   if (!(<expr>)) aether_panic("precondition violation: <expr> in <fn>");
    // immediately after parameters are declared and before any
    // user code runs. Skipped entirely when --no-contracts is set.
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

    gen->current_promoted_captures = prev_promoted;
    gen->current_promoted_capture_count = prev_promoted_count;
    gen->current_function = prev_current_function;

    unindent(gen);
    print_line(gen, "}");
    print_line(gen, "");
}

// Structure to hold pattern variable to parameter mapping
typedef struct {
    const char* var_name;
    int arg_index;
} PatternVarMapping;

// Forward declaration for expression generation with substitution
static void generate_expression_with_subst(CodeGenerator* gen, ASTNode* expr,
                                           PatternVarMapping* mappings, int mapping_count);

// Helper for expression substitution with optional parentheses
static void generate_expression_with_subst_inner(CodeGenerator* gen, ASTNode* expr,
                                                  PatternVarMapping* mappings, int mapping_count,
                                                  int add_parens);

// Generate an expression, substituting pattern variables with _argN
static void generate_expression_with_subst(CodeGenerator* gen, ASTNode* expr,
                                           PatternVarMapping* mappings, int mapping_count) {
    // Top-level: no extra parentheses needed (if statement provides them)
    generate_expression_with_subst_inner(gen, expr, mappings, mapping_count, 0);
}

static void generate_expression_with_subst_inner(CodeGenerator* gen, ASTNode* expr,
                                                  PatternVarMapping* mappings, int mapping_count,
                                                  int add_parens) {
    if (!expr) return;

    // Check if this is an identifier that should be substituted
    if (expr->type == AST_IDENTIFIER && expr->value) {
        for (int i = 0; i < mapping_count; i++) {
            if (strcmp(mappings[i].var_name, expr->value) == 0) {
                fprintf(gen->output, "_arg%d", mappings[i].arg_index);
                return;
            }
        }
        // Not in mapping, output as-is
        fprintf(gen->output, "%s", expr->value);
        return;
    }

    // For binary operations
    if (expr->type == AST_BINARY_EXPRESSION) {
        if (add_parens) fprintf(gen->output, "(");
        generate_expression_with_subst_inner(gen, expr->children[0], mappings, mapping_count, 1);
        fprintf(gen->output, " %s ", get_c_operator(expr->value));
        generate_expression_with_subst_inner(gen, expr->children[1], mappings, mapping_count, 1);
        if (add_parens) fprintf(gen->output, ")");
        return;
    }

    // For unary operations
    if (expr->type == AST_UNARY_EXPRESSION) {
        if (add_parens) fprintf(gen->output, "(");
        fprintf(gen->output, "%s", get_c_operator(expr->value));
        if (expr->child_count > 0) {
            generate_expression_with_subst_inner(gen, expr->children[0], mappings, mapping_count, 1);
        }
        if (add_parens) fprintf(gen->output, ")");
        return;
    }

    // For literals, just output value
    if (expr->type == AST_LITERAL) {
        if (expr->node_type && expr->node_type->kind == TYPE_STRING) {
            fprintf(gen->output, "\"%s\"", expr->value);
        } else {
            fprintf(gen->output, "%s", expr->value);
        }
        return;
    }

    // Fallback: use normal expression generation
    generate_expression(gen, expr);
}

// Generate a single clause's pattern match condition and body
// Returns 1 if this clause has a pattern/guard that needs checking, 0 if it's a catch-all
static int generate_clause_condition(CodeGenerator* gen, ASTNode* func, int is_first) {
    int has_condition = 0;
    int param_idx = 0;

    // First pass: check if this clause has any conditions (literals or guards)
    for (int i = 0; i < func->child_count; i++) {
        ASTNode* child = func->children[i];
        if (child->type == AST_PATTERN_LITERAL && strcmp(child->value, "_") != 0) {
            has_condition = 1;
            break;
        }
        if (child->type == AST_GUARD_CLAUSE) {
            has_condition = 1;
            break;
        }
        if (child->type == AST_PATTERN_LIST || child->type == AST_PATTERN_CONS) {
            has_condition = 1;
            break;
        }
    }

    if (!has_condition) {
        // Catch-all clause (e.g., `fib(n) -> fib(n-1) + fib(n-2)` with no
        // guard). Two things must happen that the previous version missed:
        //   1. When there are preceding conditional clauses, this clause
        //      must be wrapped in `else { ... }` so the prior conditions
        //      don't fall through into it. Without the else, `fib(0)` and
        //      `fib(1)` matched their literals AND also executed the
        //      recursive n-branch body.
        //   2. Pattern-variable parameters must be bound: `int n = _arg0;`
        //      before the body runs. Previously the binding happened only
        //      on the conditional path, leaving `n` undeclared in the
        //      catch-all body — GCC rejected the generated C.
        if (!is_first) {
            print_indent(gen);
            fprintf(gen->output, "} else {\n");
            indent(gen);
        }

        // Bind pattern variables to their corresponding _argN positions.
        int param_idx_bind = 0;
        for (int i = 0; i < func->child_count; i++) {
            ASTNode* child = func->children[i];
            if (child->type == AST_GUARD_CLAUSE || child->type == AST_BLOCK) continue;
            if (child->type == AST_PATTERN_VARIABLE && child->value) {
                print_indent(gen);
                generate_type(gen, child->node_type);
                fprintf(gen->output, " %s = _arg%d;\n", child->value, param_idx_bind);
                print_line(gen, "(void)%s;", child->value);
            }
            if (child->type == AST_PATTERN_LITERAL ||
                child->type == AST_PATTERN_VARIABLE ||
                child->type == AST_PATTERN_STRUCT ||
                child->type == AST_VARIABLE_DECLARATION) {
                param_idx_bind++;
            }
        }

        // Emit the body. Leave the `else { ... }` block open: the outer
        // generate_combined_function emits exactly one chain-closing `}`
        // at the end when any prior clause had conditions, which will
        // close our else block cleanly.
        for (int i = 0; i < func->child_count; i++) {
            ASTNode* child = func->children[i];
            if (child->type == AST_BLOCK) {
                generate_statement(gen, child);
                break;
            }
        }

        if (!is_first) {
            // Indent-level bookkeeping: we called indent(gen) when opening
            // the else, so unindent now so the closing `}` that the outer
            // function emits sits at the right column.
            unindent(gen);
        }
        return 0;
    }

    // Build pattern variable to _argN mapping first
    PatternVarMapping mappings[32];  // Max 32 parameters
    int mapping_count = 0;
    param_idx = 0;

    for (int i = 0; i < func->child_count; i++) {
        ASTNode* child = func->children[i];
        if (child->type == AST_GUARD_CLAUSE || child->type == AST_BLOCK) continue;

        if (child->type == AST_PATTERN_VARIABLE && child->value && mapping_count < 32) {
            mappings[mapping_count].var_name = child->value;
            mappings[mapping_count].arg_index = param_idx;
            mapping_count++;
        }

        if (child->type == AST_PATTERN_LITERAL ||
            child->type == AST_PATTERN_VARIABLE ||
            child->type == AST_PATTERN_STRUCT ||
            child->type == AST_VARIABLE_DECLARATION) {
            param_idx++;
        }
    }

    // Generate condition
    print_indent(gen);
    if (is_first) {
        fprintf(gen->output, "if (");
    } else {
        fprintf(gen->output, "} else if (");
    }

    int first_cond = 1;
    param_idx = 0;
    int list_idx = 0;
    ASTNode* guard = NULL;

    for (int i = 0; i < func->child_count; i++) {
        ASTNode* child = func->children[i];

        if (child->type == AST_GUARD_CLAUSE) {
            guard = child;
            continue;
        }

        if (child->type == AST_BLOCK) continue;

        if (child->type == AST_PATTERN_LITERAL && strcmp(child->value, "_") != 0) {
            if (!first_cond) fprintf(gen->output, " && ");
            fprintf(gen->output, "_arg%d == %s", param_idx, child->value);
            first_cond = 0;
        }

        if (child->type == AST_PATTERN_LIST) {
            if (strcmp(child->value, "[]") == 0 && child->child_count == 0) {
                if (!first_cond) fprintf(gen->output, " && ");
                fprintf(gen->output, "_len%d == 0", list_idx);
                first_cond = 0;
            } else {
                if (!first_cond) fprintf(gen->output, " && ");
                fprintf(gen->output, "_len%d == %d", list_idx, child->child_count);
                first_cond = 0;
            }
            list_idx++;
        } else if (child->type == AST_PATTERN_CONS) {
            if (!first_cond) fprintf(gen->output, " && ");
            fprintf(gen->output, "_len%d >= 1", list_idx);
            first_cond = 0;
            list_idx++;
        }

        if (child->type == AST_PATTERN_LITERAL ||
            child->type == AST_PATTERN_VARIABLE ||
            child->type == AST_PATTERN_STRUCT ||
            child->type == AST_VARIABLE_DECLARATION) {
            param_idx++;
        }
    }

    // Add guard condition with variable substitution
    if (guard && guard->child_count > 0) {
        if (!first_cond) fprintf(gen->output, " && ");
        generate_expression_with_subst(gen, guard->children[0], mappings, mapping_count);
    }

    fprintf(gen->output, ") {\n");
    indent(gen);

    // Bind pattern variables
    param_idx = 0;
    list_idx = 0;
    for (int i = 0; i < func->child_count; i++) {
        ASTNode* child = func->children[i];

        if (child->type == AST_PATTERN_VARIABLE) {
            print_indent(gen);
            generate_type(gen, child->node_type);
            fprintf(gen->output, " %s = _arg%d;\n", child->value, param_idx);
            // Suppress unused-variable warning when pattern binds but body ignores
            print_line(gen, "(void)%s;", child->value);
        }

        if (child->type == AST_PATTERN_LIST && child->child_count > 0) {
            for (int j = 0; j < child->child_count; j++) {
                ASTNode* elem = child->children[j];
                if (elem->type == AST_PATTERN_VARIABLE) {
                    print_indent(gen);
                    fprintf(gen->output, "int %s = _list%d[%d];\n",
                            elem->value, list_idx, j);
                    print_line(gen, "(void)%s;", elem->value);
                }
            }
            list_idx++;
        } else if (child->type == AST_PATTERN_CONS) {
            if (child->child_count >= 1 && child->children[0]->type == AST_PATTERN_VARIABLE) {
                print_indent(gen);
                fprintf(gen->output, "int %s = _list%d[0];\n",
                        child->children[0]->value, list_idx);
                print_line(gen, "(void)%s;", child->children[0]->value);
            }
            if (child->child_count >= 2 && child->children[1]->type == AST_PATTERN_VARIABLE) {
                print_indent(gen);
                fprintf(gen->output, "int* %s = &_list%d[1];\n",
                        child->children[1]->value, list_idx);
                print_indent(gen);
                fprintf(gen->output, "int %s_len = _len%d - 1;\n",
                        child->children[1]->value, list_idx);
                print_line(gen, "(void)%s; (void)%s_len;",
                           child->children[1]->value, child->children[1]->value);
            }
            list_idx++;
        }

        if (child->type == AST_PATTERN_LITERAL ||
            child->type == AST_PATTERN_VARIABLE ||
            child->type == AST_PATTERN_STRUCT ||
            child->type == AST_VARIABLE_DECLARATION) {
            param_idx++;
        }
    }

    // Generate body
    for (int i = 0; i < func->child_count; i++) {
        ASTNode* child = func->children[i];
        if (child->type == AST_BLOCK) {
            generate_statement(gen, child);
            break;
        }
    }

    unindent(gen);
    return 1;
}

// Generate a combined function from multiple pattern-matching clauses
void generate_combined_function(CodeGenerator* gen, ASTNode** clauses, int clause_count) {
    if (clause_count == 0) return;

    ASTNode* first = clauses[0];

    // Determine return type from first clause
    Type* ret_type = first->node_type;
    int has_return = has_return_value(first);

    // Check all clauses for return value
    for (int i = 1; i < clause_count && !has_return; i++) {
        if (has_return_value(clauses[i])) {
            has_return = 1;
        }
    }

    // Imported clauses get the same `static` storage class — see
    // generate_function_definition for the full rationale. @c_callback
    // overrides this so the symbol is reachable from other TUs (#235).
    // Trailing-underscore private helpers (#279) also get `static`.
    if (fn_has_internal_linkage(first)) {
        fprintf(gen->output, "static ");
    }

    if ((!ret_type || ret_type->kind == TYPE_VOID || ret_type->kind == TYPE_UNKNOWN) && has_return) {
        fprintf(gen->output, "int");
    } else {
        generate_type(gen, ret_type);
    }
    const char* cb_sym = c_callback_symbol(first);
    fprintf(gen->output, " %s(", cb_sym ? cb_sym : safe_c_name(first->value));

    // Generate unified parameter list using _argN naming
    // Count parameters from first clause
    int param_count = 0;
    int list_count = 0;

    for (int i = 0; i < first->child_count; i++) {
        ASTNode* child = first->children[i];
        if (child->type == AST_GUARD_CLAUSE || child->type == AST_BLOCK) continue;

        if (child->type == AST_PATTERN_LIST || child->type == AST_PATTERN_CONS) {
            if (param_count > 0 || list_count > 0) fprintf(gen->output, ", ");
            fprintf(gen->output, "int* _list%d, int _len%d", list_count, list_count);
            list_count++;
        } else if (child->type == AST_PATTERN_LITERAL ||
                   child->type == AST_PATTERN_VARIABLE ||
                   child->type == AST_PATTERN_STRUCT ||
                   child->type == AST_VARIABLE_DECLARATION) {
            if (param_count > 0 || list_count > 0) fprintf(gen->output, ", ");
            generate_type(gen, child->node_type);
            fprintf(gen->output, " _arg%d", param_count);
            param_count++;
        }
    }

    fprintf(gen->output, ") {\n");
    indent(gen);
    clear_declared_vars(gen);
    clear_heap_string_vars(gen);
    clear_seq_vars(gen);
    clear_opt_str_vars(gen);
    clear_try_clobbered_vars(gen);  /* Issue #501 follow-up — per-fn set */

    // Generate each clause as an if/else-if branch
    int is_first = 1;
    int had_catchall = 0;

    for (int i = 0; i < clause_count; i++) {
        int had_condition = generate_clause_condition(gen, clauses[i], is_first);
        if (had_condition) {
            is_first = 0;
        } else {
            had_catchall = 1;
        }
    }

    // Close last if block if we had conditions
    if (!is_first) {
        print_indent(gen);
        fprintf(gen->output, "}\n");
    }

    // Add fallback return if no catch-all and function returns value
    if (!had_catchall && has_return) {
        print_indent(gen);
        fprintf(gen->output, "return ");
        // If ret_type was void/unknown but we're returning int, use 0
        if (!ret_type || ret_type->kind == TYPE_VOID || ret_type->kind == TYPE_UNKNOWN) {
            fprintf(gen->output, "0");
        } else {
            generate_default_return_value(gen, ret_type);
        }
        fprintf(gen->output, ";\n");
    }

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
    const char* ret_c = (sig && sig->return_type) ? get_c_type(sig->return_type) : "void";
    fprintf(gen->output, "%s (*%s)(", ret_c, name ? name : "");
    if (sig && sig->param_count > 0) {
        for (int i = 0; i < sig->param_count; i++) {
            if (i > 0) fprintf(gen->output, ", ");
            fprintf(gen->output, "%s", get_c_type(sig->param_types[i]));
        }
    } else {
        fprintf(gen->output, "void");
    }
    fprintf(gen->output, ")");
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
        } else {
            fprintf(gen->output, "%s* %s;\n", element_type, field->value);
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
    if (struct_def->annotation &&
        strcmp(struct_def->annotation, "extern_c_import") == 0) {
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
            }
        }
    }

    /* Append the heap-tracker fields AFTER all user-declared fields
     * so the struct's user-visible layout (offsets of the named
     * fields) stays stable. */
    if (has_string_field) {
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
     * each free zeroes the tracker so a second call no-ops. */
    if (has_string_field) {
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
    }
    print_line(gen, "");
}

/* Predicate: does `struct_def` have any string-typed field that
 * needs heap-ownership tracking? Used by the codegen_stmt.c
 * struct-local-declaration site to decide whether to push the
 * function-exit destructor defer. */
int struct_has_heap_string_field(ASTNode* struct_def) {
    if (!struct_def || struct_def->type != AST_STRUCT_DEFINITION) return 0;
    for (int i = 0; i < struct_def->child_count; i++) {
        ASTNode* field = struct_def->children[i];
        if (field && field->type == AST_STRUCT_FIELD &&
            field->node_type && field->node_type->kind == TYPE_STRING) {
            return 1;
        }
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
