#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "typechecker.h"
#include "type_inference.h"
#include "actor_reply.h"

/* Defined with typecheck_program below; infer_type needs it far earlier. */
static ASTNode* aether_typecheck_program_node(void);
#include "../parser/lexer.h"
#include "../parser/parser.h"
#include "../aether_error.h"
#include "../aether_module.h"
#include "contract_eval.h"

// #1062: the canonical C-type-name renderer (codegen.c). The tuple-extern
// argument check keys on the same C spelling codegen names `_tuple_*` typedefs
// by, so an aliased scalar (int vs int8_t) or a differing pointer pointee is
// rejected at type-check instead of deferring to an opaque C compile error.
extern const char* get_c_type(Type* type);

static int error_count = 0;
static int warning_count = 0;
// #891: query the @c_struct overlay name set (defined ~typecheck_program).
static int is_c_struct_name(const char* name);
// #891: declared Type of a @c_struct field (dotted for nested). Defined below.
static Type* c_struct_field_decl_type(const char* sname, const char* field);
// #1044: enum-name registry queries (defined ~typecheck_program). Used by
// infer_type's member-access resolution above their definitions.
static int is_enum_type_name(const char* name);
// #1132: bitstruct definition/field lookup (defined ~typecheck_program). Used by
// infer_type's member-access resolution above their definitions, same as the
// enum queries above.
static ASTNode* g_bitstruct_program;
// #1366: program handle for the const-fold shadow check below.
static ASTNode* g_constfold_program;
static ASTNode* bitstruct_field_lookup(ASTNode* program, const char* bname,
                                       const char* field);
static int enum_has_member(const char* ename, const char* member);

// Get the last component of a module path for namespace
// "mypackage.utils" -> "utils"
static const char* get_namespace_from_path(const char* module_path) {
    const char* last_dot = strrchr(module_path, '.');
    if (last_dot) {
        return last_dot + 1;
    }
    return module_path;
}

// Symbol table functions
SymbolTable* create_symbol_table(SymbolTable* parent) {
    SymbolTable* table = malloc(sizeof(SymbolTable));
    table->symbols = NULL;
    table->parent = parent;
    table->hidden_names = NULL;
    table->seal_whitelist = NULL;
    table->is_sealed = 0;
    // Inherit merged-body flag so nested scopes (loops, blocks, closures
    // inside a merged function) keep the relaxed namespace visibility.
    table->inside_merged_body = parent ? parent->inside_merged_body : 0;
    // dsl_receiver does NOT inherit. It is a per-trailing-closure-scope
    // marker that typecheck_function_call stamps on the immediate
    // closure body. Nested closures inside that body get their own
    // scope without a receiver unless they are themselves trailing
    // closures of another member-access call. Lookup walks the parent
    // chain when the local fallback misses, so an outer DSL block's
    // receiver still wins over the file scope for nested-but-non-DSL
    // identifiers — see lookup_symbol below.
    table->dsl_receiver = NULL;
    return table;
}

static void free_name_list(NameNode* head) {
    while (head) {
        NameNode* next = head->next;
        if (head->name) free(head->name);
        free(head);
        head = next;
    }
}

void free_symbol_table(SymbolTable* table) {
    if (!table) return;

    Symbol* current = table->symbols;
    while (current) {
        Symbol* next = current->next;
        if (current->name) free(current->name);
        if (current->type) free_type(current->type);
        if (current->alias_target) free(current->alias_target);
        free(current);
        current = next;
    }

    free_name_list(table->hidden_names);
    free_name_list(table->seal_whitelist);
    if (table->dsl_receiver) free(table->dsl_receiver);

    free(table);
}

// --- hide / seal directive helpers ---

static int name_list_contains(NameNode* head, const char* name) {
    for (NameNode* n = head; n; n = n->next) {
        if (strcmp(n->name, name) == 0) return 1;
    }
    return 0;
}

void scope_hide_name(SymbolTable* table, const char* name) {
    if (!table || !name) return;
    if (name_list_contains(table->hidden_names, name)) return;
    NameNode* n = malloc(sizeof(NameNode));
    n->name = strdup(name);
    n->next = table->hidden_names;
    table->hidden_names = n;
}

void scope_seal_except(SymbolTable* table, const char* name) {
    if (!table || !name) return;
    table->is_sealed = 1;
    if (name_list_contains(table->seal_whitelist, name)) return;
    NameNode* n = malloc(sizeof(NameNode));
    n->name = strdup(name);
    n->next = table->seal_whitelist;
    table->seal_whitelist = n;
}

int scope_name_is_hidden(SymbolTable* table, const char* name) {
    if (!table || !name) return 0;
    return name_list_contains(table->hidden_names, name);
}

int scope_name_in_whitelist(SymbolTable* table, const char* name) {
    if (!table || !name) return 0;
    return name_list_contains(table->seal_whitelist, name);
}

// Returns 1 if `name` was blocked by a `hide` or `seal except` directive
// somewhere in the scope chain AND a real binding for it exists farther
// up (above the blocking scope). Used to give a better error message than
// "undefined variable" when the user actually meant to reach a hidden one.
static int name_blocked_by_hide(SymbolTable* table, const char* name) {
    if (!table || !name) return 0;
    SymbolTable* t = table;
    while (t) {
        int blocked_here = scope_name_is_hidden(t, name) ||
                           (t->is_sealed && !scope_name_in_whitelist(t, name));
        if (blocked_here) {
            // Walk upward from the parent looking for a real binding.
            for (SymbolTable* check = t->parent; check; check = check->parent) {
                if (lookup_symbol_local(check, name)) return 1;
            }
            return 0;
        }
        t = t->parent;
    }
    return 0;
}

void add_symbol(SymbolTable* table, const char* name, Type* type, int is_actor, int is_function, int is_state) {
    Symbol* symbol = malloc(sizeof(Symbol));
    symbol->name = strdup(name);
    symbol->type = type;
    symbol->is_actor = is_actor;
    symbol->is_function = is_function;
    symbol->is_state = is_state;
    symbol->is_module_alias = 0;
    symbol->alias_target = NULL;
    symbol->node = NULL;  // Initialize to NULL
    symbol->type_inferred = 0;
    symbol->width_explicit = 0;
    symbol->next = table->symbols;
    table->symbols = symbol;
}

Symbol* lookup_symbol(SymbolTable* table, const char* name) {
    if (!table || !name) return NULL;

    // Local bindings always win — `hide` and `seal except` only block
    // resolution that would walk OUT of this scope into a parent.
    Symbol* symbol = lookup_symbol_local(table, name);
    if (symbol) return symbol;

    // Issue #333 DSL block receiver fallback: when this scope is the
    // body of a `receiver.method(args) { ... }` trailing closure,
    // `dsl_receiver` names the receiver namespace or struct type.
    // Try `<dsl_receiver>_<name>` — that's how Aether codegen names
    // namespace and struct-method helpers (e.g. `bash_script`,
    // `Builder_configure`). The walk uses lookup_symbol_local at
    // each level to avoid re-entering this DSL fallback at parent
    // scopes (lookup_symbol is the recursion vector; lookup_symbol_local
    // is not).
    //
    // Nested DSL blocks: inner block's dsl_receiver is checked first.
    // If the rewrite misses and the parent chain is then walked, the
    // outer block's dsl_receiver is checked when the recursion lands
    // on its scope — natural outer→inner shadowing without explicit
    // stack management.
    if (table->dsl_receiver && table->dsl_receiver[0] && name[0]) {
        /* Bounded snprintf via precision spec — caps each component
         * at 250 bytes regardless of strlen, which keeps GCC's
         * format-truncation analyzer happy on -Werror builds.
         * Identifiers above 250 bytes don't occur in practice
         * (longest stdlib symbol is <40 chars). */
        char rewritten[512];
        snprintf(rewritten, sizeof(rewritten), "%.250s_%.250s",
                 table->dsl_receiver, name);
        for (SymbolTable* t = table; t; t = t->parent) {
            Symbol* s = lookup_symbol_local(t, rewritten);
            if (s) return s;
        }
    }

    // Crossing the scope boundary upward: enforce hide / seal directives.
    // - `hide foo` blocks any name in the hidden_names list.
    // - `seal except a, b` blocks anything that isn't in the whitelist.
    // Either way, return NULL so the caller sees an undefined identifier.
    if (scope_name_is_hidden(table, name)) {
        return NULL;
    }
    if (table->is_sealed && !scope_name_in_whitelist(table, name)) {
        return NULL;
    }

    if (table->parent) {
        return lookup_symbol(table->parent, name);
    }

    return NULL;
}

Symbol* lookup_symbol_local(SymbolTable* table, const char* name) {
    Symbol* current = table->symbols;
    while (current) {
        if (strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

// Module alias functions
void add_module_alias(SymbolTable* table, const char* alias, const char* module_name) {
    Symbol* symbol = malloc(sizeof(Symbol));
    symbol->name = strdup(alias);
    symbol->type = NULL;  // Modules don't have types
    symbol->is_actor = 0;
    symbol->is_function = 0;
    symbol->is_state = 0;
    symbol->is_module_alias = 1;
    symbol->alias_target = strdup(module_name);
    symbol->node = NULL;
    symbol->next = table->symbols;
    table->symbols = symbol;
}

Symbol* resolve_module_alias(SymbolTable* table, const char* name) {
    Symbol* symbol = lookup_symbol(table, name);
    if (symbol && symbol->is_module_alias) {
        return symbol;
    }
    return NULL;
}

// Track imported namespaces for qualified function calls.
//
// Two parallel sets exist (issue #243 sealed-scope follow-up):
//
//   imported_namespaces[]      — every namespace registered during
//     orchestration, including ones the user did not explicitly import
//     but that were pulled in transitively by module_merge_into_program's
//     BFS pass. Cloned function bodies of merged modules need this set
//     to resolve their internal qualified calls (e.g. a cloned
//     `client_post_json` calling `json.stringify(...)` even though the
//     user only wrote `import std.http.client`).
//
//   user_explicit_namespaces[] — only namespaces the user explicitly
//     wrote `import` for. Synthetic AST_IMPORT_STATEMENT nodes that
//     module_merge_into_program injects (annotated "synthetic") are
//     skipped. User-code qualified calls resolve against this stricter
//     set so a user can't accidentally call into a transitively-pulled-
//     in module they never asked for.
static char* imported_namespaces[64];
static int namespace_count = 0;
static char* user_explicit_namespaces[64];
static int user_explicit_namespace_count = 0;

// Import alias table: maps short names to dotted qualified names
// for selective imports (e.g. "release" -> "build.release")
#define MAX_IMPORT_ALIASES 512
static struct {
    char short_name[128];
    char qualified_name[256];
} import_aliases[MAX_IMPORT_ALIASES];
static int import_alias_count = 0;

static void add_import_alias(const char* short_name, const char* qualified) {
    if (import_alias_count < MAX_IMPORT_ALIASES) {
        snprintf(import_aliases[import_alias_count].short_name, sizeof(import_aliases[0].short_name), "%s", short_name);
        snprintf(import_aliases[import_alias_count].qualified_name, sizeof(import_aliases[0].qualified_name), "%s", qualified);
        import_alias_count++;
    }
}

static const char* find_import_alias(const char* name) {
    for (int i = 0; i < import_alias_count; i++) {
        if (strcmp(import_aliases[i].short_name, name) == 0) {
            return import_aliases[i].qualified_name;
        }
    }
    return NULL;
}

void register_namespace(const char* ns) {
    if (namespace_count < 64) {
        // Check if already registered
        for (int i = 0; i < namespace_count; i++) {
            if (strcmp(imported_namespaces[i], ns) == 0) return;
        }
        imported_namespaces[namespace_count++] = strdup(ns);
    }
}

// Issue #243 sealed-scope follow-up. Records that the user *explicitly*
// wrote `import <module>` for the given namespace leaf. Called from the
// AST_IMPORT_STATEMENT visitor only when the import is not flagged
// synthetic (`annotation == "synthetic"`) — which means the import came
// from the user's source rather than from the BFS transitive-merge
// pass. Used by user-code qualified-call resolution to reject
// `lib_b.shout()` when the user only wrote `import lib_a`.
static void register_user_explicit_namespace(const char* ns) {
    if (user_explicit_namespace_count < 64) {
        for (int i = 0; i < user_explicit_namespace_count; i++) {
            if (strcmp(user_explicit_namespaces[i], ns) == 0) return;
        }
        user_explicit_namespaces[user_explicit_namespace_count++] = strdup(ns);
    }
}

static int is_user_explicit_namespace(const char* name) {
    for (int i = 0; i < user_explicit_namespace_count; i++) {
        if (strcmp(user_explicit_namespaces[i], name) == 0) return 1;
    }
    return 0;
}

// Forward decl — defined below alongside the global registry it gates.
int is_imported_namespace(const char* name);

// Gate for qualified-call resolution. A qualified call `mod.fn()` is
// allowed if either:
//   - The caller is inside a merged-module body (typechecker propagates
//     SymbolTable::inside_merged_body from the cloned function decl);
//     in that case ANY transitively-merged namespace is fair game,
//     because cloned bodies need to call into their original module's
//     transitive deps to compile.
//   - The caller is user code (inside_merged_body == 0); in that case
//     only namespaces the user explicitly imported are visible. This
//     closes the encapsulation hole left after the round-1 BFS-merge
//     fix for issue #243.
//
// `table` may be NULL during early symbol-table population; treat NULL
// as user-context (the strict path) — early registration paths don't
// resolve qualified user calls, so this is safe.
int is_visible_namespace(const char* name, SymbolTable* table) {
    /* Single channel: the SymbolTable's inside_merged_body flag.
     * Both walkers (the typechecker — which creates per-function
     * child tables — and the type-inference pass — which walks
     * against the global symbol table directly) flip this flag
     * transiently while inside a `is_imported` function body, then
     * restore it on exit. Save/restore is the standard scope-
     * stack pattern; no global mutable state required. */
    if (table && table->inside_merged_body) {
        return is_imported_namespace(name);
    }
    return is_user_explicit_namespace(name);
}

// #878: a module's qualified `X.fn()` surface is available whenever the
// namespace is imported in ANY form (bare, selective, or qualified) — like
// Java's fully-qualified name, which is always legal regardless of imports.
// A selective import (`import std.math (sqrt)`) is purely ADDITIVE: it adds
// the bare-name binding (`sqrt(...)`) on top of the always-available qualified
// surface (`math.sqrt(...)`, `math.pow(...)`). It no longer restricts the
// qualified form. Externs are still always added to the symbol table so merged
// stdlib wrappers resolve their own module's symbols. Export visibility
// (`is_export_blocked`) and hide/seal still gate qualified access; only the
// per-import-form selective filter is gone. (Previously a per-module filter
// rejected `math.pow` under `import std.math (sqrt)`; that whole machinery is
// removed.)

/* Note: the (selective-import + local-def-with-same-name) shadow
 * check that issue #436 facet A targeted lives at the orchestration
 * layer instead — see check_selective_import_shadow in
 * compiler/aether_module.c. That position catches the collision
 * BEFORE the merger renames the local def, so the diagnostic can
 * cite the original source location and the original import path
 * without reconstructing them post-merge. */

// Check if a symbol is blocked by export visibility.
// Returns 1 if blocked (module has exports and symbol isn't one), 0 if allowed.
static int is_export_blocked(const char* namespace, const char* symbol) {
    if (!global_module_registry) return 0;
    AetherModule* mod = module_find(namespace);
    return (mod && mod->export_count > 0 && !module_is_exported(mod, symbol));
}

/* Find a registered module by exact name, or by the last dot-component
 * of its name. Std modules register under their full path ("std.os")
 * while qualified call sites carry the leaf ("os.getenv"), so a plain
 * module_find(prefix) misses them. First leaf match wins; the caller's
 * exports gate keeps an ambiguous leaf from resolving anything the
 * matched module doesn't explicitly export. (#1035) */
static AetherModule* module_find_by_name_or_leaf(const char* name) {
    if (!global_module_registry || !name) return NULL;
    AetherModule* m = module_find(name);
    if (m) return m;
    for (int i = 0; i < global_module_registry->module_count; i++) {
        AetherModule* cand = global_module_registry->modules[i];
        if (!cand || !cand->name) continue;
        const char* last_dot = strrchr(cand->name, '.');
        if (last_dot && strcmp(last_dot + 1, name) == 0) return cand;
    }
    return NULL;
}

int is_imported_namespace(const char* name) {
    for (int i = 0; i < namespace_count; i++) {
        if (strcmp(imported_namespaces[i], name) == 0) return 1;
    }
    return 0;
}

Symbol* lookup_qualified_symbol(SymbolTable* table, const char* qualified_name) {
    if (!table || !qualified_name) return NULL;
    // Split qualified name on '.'
    char* name_copy = strdup(qualified_name);
    char* dot = strchr(name_copy, '.');

    if (dot) {
        *dot = '\0';
        const char* prefix = name_copy;
        const char* suffix = dot + 1;

        // Enforce hide / seal on the prefix before any namespace resolution.
        // `hide http` must block both bare `http` AND `http.get(url)`.
        if (scope_name_is_hidden(table, prefix) ||
            (table->is_sealed && !scope_name_in_whitelist(table, prefix))) {
            free(name_copy);
            return NULL;
        }

        // Check if prefix is a module alias
        Symbol* alias_sym = resolve_module_alias(table, prefix);
        if (alias_sym && alias_sym->alias_target) {
            // Reconstruct with actual module name
            char resolved_name[512];
            snprintf(resolved_name, sizeof(resolved_name), "%s.%s",
                    alias_sym->alias_target, suffix);
            free(name_copy);
            return lookup_symbol(table, resolved_name);
        }

        // Check if prefix is a namespace visible from this scope.
        // Issue #243: user code can only see namespaces it explicitly
        // imported; merged-body code can see all transitively-merged
        // namespaces. is_visible_namespace picks the right set based
        // on the table's inside_merged_body flag.
        // Convert string.new -> string_new
        if (is_visible_namespace(prefix, table)) {
            // Enforce export visibility
            if (is_export_blocked(prefix, suffix)) {
                free(name_copy);
                return NULL;
            }
            // #878: no selective-import gate here — the qualified `X.fn()`
            // surface is available whenever the namespace is visible, in any
            // import form. (export visibility + hide/seal above still apply.)
            char c_func_name[512];
            snprintf(c_func_name, sizeof(c_func_name), "%s_%s", prefix, suffix);
            Symbol* sym = lookup_symbol(table, c_func_name);
            /* #924 re-export: `hub.fn()` where hub re-exports an imported
             * `fn`. No local `hub_fn` symbol exists; redirect to the
             * defining module's `<origin>_fn`. */
            if (!sym && global_module_registry) {
                AetherModule* hub = module_find(prefix);
                AetherModule* origin = module_resolve_reexport(hub, suffix);
                if (origin && origin->name) {
                    char c_origin[512];
                    snprintf(c_origin, sizeof(c_origin), "%s_%s",
                             origin->name, suffix);
                    sym = lookup_symbol(table, c_origin);
                }
            }
            /* #1035: exports that don't carry the module-name prefix —
             * raw externs like std.os's `aether_args_count` — have no
             * `<prefix>_<suffix>` symbol; their C name IS the bare
             * export name. If the module explicitly exports `suffix`,
             * resolve the bare symbol so the documented qualified form
             * (`os.aether_args_count()`) works. Gated on a positive
             * exports-list hit, so `anything.foo` can never reach an
             * unrelated global `foo`. */
            if (!sym && global_module_registry) {
                AetherModule* mod = module_find_by_name_or_leaf(prefix);
                if (mod && mod->export_count > 0 &&
                    module_is_exported(mod, suffix)) {
                    sym = lookup_symbol(table, suffix);
                }
            }
            free(name_copy);
            return sym;
        }
    }

    free(name_copy);
    return lookup_symbol(table, qualified_name);
}

void type_error(const char* message, int line, int column) {
    AetherErrorCode code = AETHER_ERR_TYPE_MISMATCH;
    if (strstr(message, "not exported")) code = AETHER_ERR_NOT_EXPORTED;
    else if (strstr(message, "is hidden in this scope") ||
             strstr(message, "it is hidden in this scope")) code = AETHER_ERR_HIDDEN_NAME;
    else if (strstr(message, "Undefined variable")) code = AETHER_ERR_UNDEFINED_VAR;
    else if (strstr(message, "Undefined function") || strstr(message, "Unknown function"))
        code = AETHER_ERR_UNDEFINED_FUNC;
    else if (strstr(message, "Undefined type") || strstr(message, "Unknown type"))
        code = AETHER_ERR_UNDEFINED_TYPE;
    else if (strstr(message, "Redefinition") || strstr(message, "redefinition"))
        code = AETHER_ERR_REDEFINITION;
    aether_error_with_code(message, line, column, code);
    error_count++;
}

void type_warning(const char* message, int line, int column);

/* ---- string-interpolation operand check -------------------------------
 *
 * `"${expr}"` used to accept ANY type: the typechecker walked the operands
 * and discarded the result, so codegen emitted a `%s`-shaped read of whatever
 * bits arrived. Three kinds produced garbage instead of a diagnostic:
 *
 *   ${zero_arg_fn}  -> the function's ADDRESS read as a string. The bytes are
 *                      an x86 prologue ("UH\x89\xe5..."), and this was found
 *                      in the wild only when such a string reached /bin/sh --
 *                      i.e. it is memory disclosure, not a formatting wart.
 *   ${struct_value} -> silently printed the first field.
 *   ${bytes_handle} -> read the handle as a char*, printing nothing.
 *
 * The check is a WHITELIST of renderable kinds, not a blocklist of bad ones:
 * a blocklist lets the next type added to the language silently join the
 * garbage list, which is how this shipped in the first place.
 *
 * TYPE_TUPLE is admitted because the type is often simply WRONG rather than
 * the value being un-renderable. `x = f()` where f returns `(int, string)`
 * binds the FIRST element -- verified: it prints 42, not a tuple -- but the
 * slot keeps the tuple type. Rejecting it flagged a working example in
 * std/tcp/README.md. Fixing that inference is a separate change; until then a
 * tuple operand is far more likely to be a correctly-bound scalar than a real
 * attempt to render a tuple.
 *
 * TYPE_PTR is admitted despite being the `${bytes_handle}` case above, and
 * that is a deliberate scope decision rather than an oversight. Interpolating
 * a `ptr` is an established idiom here: 21 sites across std/, contrib/,
 * examples/ and tests/ do it -- including std/spec and every std.map consumer
 * -- because map/pqueue/schema return `ptr` for values that really are
 * `char*`, and those render correctly today. Rejecting them would break
 * working code, and there is no sanctioned replacement to migrate to (a
 * `ptr`-to-string binding does not currently compile). Narrowing this is its
 * own change: it needs a conversion for callers to move to, and a sweep of
 * those 21 sites. Filed rather than smuggled in behind a bug fix.
 */
static int interp_kind_is_renderable(TypeKind k) {
    switch (k) {
        case TYPE_INT: case TYPE_INT64: case TYPE_UINT64:
        case TYPE_UINT32: case TYPE_UINT16: case TYPE_UINT8:
        case TYPE_BYTE: case TYPE_BOOL:
        case TYPE_FLOAT: case TYPE_FLOAT32: case TYPE_LONGDOUBLE:
        case TYPE_DURATION:
        case TYPE_STRING:
        case TYPE_ENUM:      /* integer-backed; renders as its numeric value */
        case TYPE_UNKNOWN:   /* inference already failed and reported */
        case TYPE_PTR:       /* see below -- deliberately admitted */
        case TYPE_TUPLE:     /* see below -- deliberately admitted */
            return 1;
        default:
            return 0;
    }
}

/* Reject an un-renderable operand, naming the fix. The message matters more
 * than the rejection: each of these has an obvious intended spelling, and
 * saying it turns a silent wrong answer into a one-line correction.
 *
 * fb_line/fb_col are the enclosing interpolation's position. Prefer them: an
 * identifier inside `${...}` carries the position of the DECLARATION it
 * resolves to, so reporting the child's own line sent the reader to where the
 * function was defined rather than to the string that misuses it. */
static void check_interp_operand(ASTNode* child, SymbolTable* table,
                                 int fb_line, int fb_col) {
    if (!child) return;
    if (child->type == AST_LITERAL) return;   /* the literal text segments */

    int line = fb_line > 0 ? fb_line : child->line;
    int col  = fb_line > 0 ? fb_col  : child->column;

    /* A bare reference to a function. Caught by NAME before consulting the
     * inferred type, because a bare function identifier infers as its RETURN
     * type -- so `${cmd}` looks like a perfectly good string here -- and
     * "did you mean cmd()?" is the whole value of the diagnostic. */
    if (child->type == AST_IDENTIFIER && child->value) {
        Symbol* sym = lookup_symbol(table, child->value);
        if (sym && sym->is_function) {
            char msg[320];
            snprintf(msg, sizeof(msg),
                "cannot interpolate '%s': it is a function, so `${%s}` renders "
                "its ADDRESS, not a value. Did you mean `${%s()}`?",
                child->value, child->value, child->value);
            type_error(msg, line, col);
            return;
        }
    }

    /* Stay quiet once something else has already failed. A cascading
     * diagnostic on a broken parse or a failed inference is worse than
     * silence: tests/integration/tuple_through_fn/bare_fn.ae is a deliberate
     * negative fixture whose whole point is that a bare `fn` parameter gets
     * ONE targeted hint and no follow-on noise, and without this guard the
     * destructured string `e2` there was reported as a function value --
     * true of its (bogus) inferred type, misleading about the code. */
    if (error_count > 0) return;

    Type* t = child->node_type;
    if (!t || interp_kind_is_renderable(t->kind)) return;

    char msg[320];
    switch (t->kind) {
        case TYPE_STRUCT:
            snprintf(msg, sizeof(msg),
                "cannot interpolate a struct%s%s: interpolate a field instead, "
                "e.g. `${value.field}`",
                t->struct_name ? " " : "", t->struct_name ? t->struct_name : "");
            break;
        case TYPE_ARRAY:
            snprintf(msg, sizeof(msg),
                "cannot interpolate an array: interpolate an element, "
                "e.g. `${a[0]}`");
            break;
        case TYPE_FUNCTION:
            snprintf(msg, sizeof(msg),
                "cannot interpolate a function value: `${f}` renders its "
                "address. Call it (`${f()}`) to interpolate its result");
            break;
        case TYPE_VOID:
            snprintf(msg, sizeof(msg),
                "cannot interpolate a void value: this expression returns "
                "nothing to render");
            break;
        default:
            snprintf(msg, sizeof(msg),
                "cannot interpolate this value: its type has no string "
                "rendering");
            break;
    }
    type_error(msg, line, col);
}


/* #1140 — can this function signal failure to its caller? True for Aether's
 * Go-style `(value, err)` convention (a tuple whose last element is the error
 * string) and hence for `T!`, which is represented as exactly that tuple with
 * `is_result` set. Everything else has no error channel, so a `defer catch`
 * there can never fire. */
static int function_can_fail(Type* ret) {
    if (!ret) return 0;
    if (ret->kind != TYPE_TUPLE || ret->tuple_count < 2) return 0;
    Type* last = ret->tuple_types ? ret->tuple_types[ret->tuple_count - 1] : NULL;
    return last && last->kind == TYPE_STRING;
}

/* Phase 2 of error-unification: a `T!` result used as a bare expression
 * statement — dropped on the floor — is a compile error; the fallible
 * outcome must be consumed. `expr` is the direct child of an
 * AST_EXPRESSION_STATEMENT (or a raw call used as a statement). If its
 * inferred type is a `T!` (`is_result` set), emit the "unconsumed result"
 * diagnostic and return 1.
 *
 * Enforcement keys on `is_result`, NOT on the structural
 * `function_can_fail` shape: a raw `(value, string)` stdlib tuple that has
 * not migrated to `T!` is deliberately NOT flagged (gradualism is
 * structural — a signature opts in the moment it becomes `T!`). Every
 * legitimate consumer nests the call as a CHILD of a non-expression-
 * statement node — destructure/assign bind it, `return`/argument pass it,
 * and `!`/`or` unwrap it to the success type with `is_result` cleared — so
 * none of them reach here as the statement's own expression. */
Type* infer_type(ASTNode* expr, SymbolTable* table);   /* defined below */
static void set_node_type(ASTNode* node, Type* fresh);
static Type* resolve_fnptr_struct_field(SymbolTable* table, const char* recv_name,
                                        const char* field_name, int* out_is_ptr);
static int check_unconsumed_result(ASTNode* expr, SymbolTable* table) {
    if (!expr) return 0;
    Type* t = infer_type(expr, table);
    int flagged = 0;
    if (t && t->is_result) {
        aether_error_with_suggestion(
            "fallible result must be consumed, not discarded",
            expr->line, expr->column,
            "bind it (`v, e = f(...)`), propagate with `!`, handle with "
            "`f(...) or { ... }`, or discard explicitly with `_, _ = f(...)`");
        error_count++;
        flagged = 1;
    }
    if (t) free_type(t);
    return flagged;
}

/* Walk a function body for `defer try` / `defer catch`, without descending into
 * nested closures or function definitions (which have their own return type and
 * are checked separately). */
static void warn_conditional_defers_walk(ASTNode* node, int can_fail) {
    if (!node) return;
    if (node->type == AST_CLOSURE || node->type == AST_FUNCTION_DEFINITION) return;
    if (node->type == AST_DEFER_STATEMENT && node->value && !can_fail) {
        char msg[220];
        if (strcmp(node->value, "catch") == 0) {
            snprintf(msg, sizeof(msg),
                     "`defer catch` in a function that cannot fail: it will never run. "
                     "Give the function an error result (`-> (T, string)` or `-> T!`), "
                     "or use a plain `defer`.");
            type_warning(msg, node->line, node->column);
        } else if (strcmp(node->value, "try") == 0) {
            snprintf(msg, sizeof(msg),
                     "`defer try` in a function that cannot fail: it always runs, so it "
                     "is the same as a plain `defer`. Drop the `try`, or give the "
                     "function an error result (`-> (T, string)` or `-> T!`).");
            type_warning(msg, node->line, node->column);
        }
    }
    for (int i = 0; i < node->child_count; i++)
        warn_conditional_defers_walk(node->children[i], can_fail);
}

static void warn_conditional_defers_in_infallible(ASTNode* func, Type* ret) {
    if (!func || function_can_fail(ret)) return;
    for (int i = 0; i < func->child_count; i++)
        warn_conditional_defers_walk(func->children[i], 0);
}

void type_warning(const char* message, int line, int column) {
    AetherError w = {
        .filename = NULL, .source_code = NULL,
        .line = line, .column = column,
        .message = message, .suggestion = NULL,
        .context = NULL, .code = AETHER_ERR_NONE
    };
    aether_warning_report(&w);
    warning_count++;
}

// Return a human-readable type name (static buffer — for error messages only)
static const char* type_name(Type* t) {
    if (!t) return "unknown";
    /* C ABI alias — report the spelling the user wrote (size_t, ...). */
    if (t->c_alias) return t->c_alias;
    switch (t->kind) {
        case TYPE_INT:      return "int";
        case TYPE_INT64:    return "long";
        case TYPE_UINT64:   return "uint64";
        case TYPE_UINT32:   return "uint32";
        case TYPE_UINT16:   return "uint16";
        case TYPE_UINT8:    return "uint8";
        case TYPE_DURATION: return "Duration";
        case TYPE_FLOAT:    return "float";
        case TYPE_LONGDOUBLE: return "longdouble";
        case TYPE_BOOL:     return "bool";
        case TYPE_BYTE:     return "byte";
        case TYPE_STRING:   return "string";
        case TYPE_VOID:     return "void";
        case TYPE_PTR:      return "ptr";
        case TYPE_ACTOR_REF: return "actor_ref";
        case TYPE_MESSAGE:  return "message";
        case TYPE_ARRAY:    return "array";
        case TYPE_STRUCT:   return t->struct_name ? t->struct_name : "struct";
        case TYPE_FUNCTION:  return "closure";
        case TYPE_TUPLE:    return "tuple";
        case TYPE_OPTIONAL: return "optional";   // #340
        /* Named nominal types report their own name — "expected uint8_t, got B"
         * is actionable where "got unknown" is not. */
        case TYPE_ENUM:     return t->struct_name ? t->struct_name : "enum";
        case TYPE_BITSTRUCT: return t->struct_name ? t->struct_name : "bitstruct";
        case TYPE_BITSET:   return "bit_set";
        case TYPE_UNKNOWN:  return "unknown";
        default:            return "unknown";
    }
}

static int is_integer_scalar(TypeKind kind) {
    return kind == TYPE_INT || kind == TYPE_INT64 || kind == TYPE_UINT64 ||
           kind == TYPE_UINT32 || kind == TYPE_UINT16 || kind == TYPE_UINT8;
}

static int is_numeric_scalar(TypeKind kind) {
    return kind == TYPE_INT || kind == TYPE_INT64 || kind == TYPE_UINT64 ||
           kind == TYPE_UINT32 || kind == TYPE_UINT16 || kind == TYPE_UINT8 ||
           kind == TYPE_FLOAT || kind == TYPE_LONGDOUBLE;
}

static TypeKind wider_integer_kind(TypeKind a, TypeKind b) {
    if (a == TYPE_UINT64 || b == TYPE_UINT64) return TYPE_UINT64;
    if (a == TYPE_INT64 || b == TYPE_INT64) return TYPE_INT64;
    /* uint32/16/8 all fit int's value range for arithmetic. */
    return TYPE_INT;
}

/* #697: downward integer-width propagation.
 *
 * Type inference is bottom-up, so a sub-expression like `byte << 24`
 * (int << int) is typed 32-bit `int` even when its result is immediately
 * combined into a 64-bit value (`u | (byte << 24)`). C then evaluates the
 * shift in 32-bit — overflowing the sign bit and sign-extending into the
 * high 32 bits of the uint64 — silently corrupting binary-protocol codecs.
 *
 * When a binary integer op resolves to a 64-bit type, walk its operands
 * and re-type any narrower-int arithmetic operand whose VALUE is computed
 * (a nested arith/bitwise/left-shift binary expression) to the same 64-bit
 * kind, so codegen widens it before the operation. Leaf operands
 * (variables, literals, calls) keep their own type and are cast at the use
 * site by codegen. `>>` is intentionally NOT widened: promoting a signed
 * right-shift to unsigned would turn an arithmetic shift into a logical
 * one and change the value of negative operands. Division/modulo are
 * likewise excluded (their 32-bit result is well-defined and widening
 * could only change a value the source already committed to). */
static void propagate_int_width_64(ASTNode* node, TypeKind wide) {
    if (!node || node->type != AST_BINARY_EXPRESSION || !node->node_type) return;
    /* Only widen nodes currently typed as 32-bit int — leave anything
     * already 64-bit (or non-integer) alone. */
    if (node->node_type->kind != TYPE_INT) return;
    if (!node->value) return;
    const char* op = node->value;
    int width_sensitive =
        strcmp(op, "<<") == 0 || strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
        strcmp(op, "*") == 0  || strcmp(op, "|") == 0 || strcmp(op, "&") == 0 ||
        strcmp(op, "^") == 0;
    if (!width_sensitive) return;
    node->node_type->kind = wide;
    if (node->child_count >= 2) {
        propagate_int_width_64(node->children[0], wide);
        propagate_int_width_64(node->children[1], wide);
    }
}

// Count the number of formal parameters of a function definition node
// Skips _ctx parameters (auto-injected by builder context)
static int count_function_params(ASTNode* func) {
    if (!func || func->child_count == 0) return 0;
    int count = 0;
    // Last child is the function body; everything before it may be params or a guard
    for (int i = 0; i < func->child_count - 1; i++) {
        ASTNode* child = func->children[i];
        if (child->type == AST_VARIABLE_DECLARATION ||
            child->type == AST_PATTERN_VARIABLE ||
            child->type == AST_PATTERN_LITERAL) {
            count++;
        }
        // AST_GUARD_CLAUSE is skipped (not a parameter)
    }
    return count;
}

// Returns 1 if the parameter has a default expression attached
// (Phase A2.1 — default function arguments). Default expressions are
// stored as the first child of an AST_PATTERN_VARIABLE / AST_VARIABLE_
// DECLARATION node by the parser, with annotation="has_default" so
// they are distinguishable from struct/list-pattern children.
static int param_has_default(ASTNode* param) {
    return param && param->annotation &&
           strcmp(param->annotation, "has_default") == 0 &&
           param->child_count > 0 && param->children[0] != NULL;
}

// Phase A2.2 (issue #265 close): rewrite source-location intrinsic
// AST nodes inside a cloned default expression to reflect the
// caller's location instead of the function-definition's. Called on
// the clone before it is appended to the call's child list.
//
//   __LINE__  — codegen emits `expr->line`. Overwrite with the
//               call site's line so the caller's site is captured.
//   __FILE__  — codegen reads `gen->source_file` (per-TU global), not
//               `expr->line`, so no per-node rewrite needed; the
//               value is naturally the file holding the call.
//   __func__  — codegen emits the literal C99 `__func__` keyword,
//               which the C compiler resolves to the enclosing C
//               function. Since codegen mirrors Aether function
//               names, this is the calling Aether function's name —
//               exactly the caller-site semantics we want. No
//               rewrite needed.
static void rewrite_caller_site_intrinsics(ASTNode* node, int call_line, int call_column) {
    if (!node) return;
    if (node->type == AST_IDENTIFIER && node->value &&
        strcmp(node->value, "__LINE__") == 0) {
        node->line = call_line;
        node->column = call_column;
    }
    for (int i = 0; i < node->child_count; i++) {
        rewrite_caller_site_intrinsics(node->children[i], call_line, call_column);
    }
}

// Counts required (non-defaulted) parameters. Defaults trail required
// (Python rule: once a default appears, every subsequent parameter
// must also have one). The rule is enforced separately at function-
// declaration time; this helper just counts.
static int count_required_params(ASTNode* func) {
    if (!func || func->child_count == 0) return 0;
    int count = 0;
    for (int i = 0; i < func->child_count - 1; i++) {
        ASTNode* child = func->children[i];
        if (child->type == AST_GUARD_CLAUSE) continue;
        if (child->type != AST_VARIABLE_DECLARATION &&
            child->type != AST_PATTERN_VARIABLE &&
            child->type != AST_PATTERN_LITERAL) continue;
        if (param_has_default(child)) {
            return count;  // first defaulted param ends the required prefix
        }
        count++;
    }
    return count;
}

// Returns 1 if the function's first parameter is _ctx: ptr. Such functions
// can be called either with _ctx passed explicitly (got == expected) or with
// _ctx auto-injected by the builder-DSL runtime (got == expected - 1).
/* Does this function's LAST declared parameter have type `fn`?
 *
 * Such a function wants a real (hoisted, capturing) closure — `callback { }`
 * or `|x| { }` — not a bare trailing block. A bare `{ }` parses as a DSL
 * block, which inlines at the call site and is never hoisted, so codegen
 * emits a reference to a `_closure_fn_N` it never generated and the C
 * compiler fails with `'_closure_fn_0' undeclared`. Detecting it here turns
 * that into a diagnostic that names the fix. */
static int last_param_is_fn(ASTNode* func) {
    if (!func || func->child_count < 2) return 0;
    ASTNode* last_param = NULL;
    for (int i = 0; i < func->child_count - 1; i++) {
        ASTNode* child = func->children[i];
        if (child && (child->type == AST_VARIABLE_DECLARATION ||
                      child->type == AST_PATTERN_VARIABLE ||
                      child->type == AST_PATTERN_LITERAL)) {
            last_param = child;
        }
    }
    return last_param && last_param->node_type &&
           last_param->node_type->kind == TYPE_FUNCTION;
}

static int has_ctx_first_param(ASTNode* func) {
    if (!func || func->child_count == 0) return 0;
    for (int i = 0; i < func->child_count - 1; i++) {
        ASTNode* child = func->children[i];
        if (child->type == AST_VARIABLE_DECLARATION ||
            child->type == AST_PATTERN_VARIABLE ||
            child->type == AST_PATTERN_LITERAL) {
            return child->value && strcmp(child->value, "_ctx") == 0 &&
                   child->node_type && child->node_type->kind == TYPE_PTR;
        }
    }
    return 0;
}

/* Returns 1 if `init` is an integer literal whose value is outside
 * 0..255, which would silently truncate when assigned to a `byte`-
 * typed slot. Returns 0 if it's in range, not a literal, or not an
 * integer literal at all (those cases are accepted; runtime
 * truncation matches how other narrowings behave).
 *
 * Catches the obvious typo `b: byte = 256` at compile time via a
 * literal-range check + runtime truncate for non-literal int. */
static int byte_assignment_literal_out_of_range(ASTNode* init) {
    if (!init || init->type != AST_LITERAL || !init->value) return 0;
    if (!init->node_type) return 0;
    /* Only int / int64 literals get the range check. Float / string /
     * bool literals fail the type-compatibility check earlier and
     * never reach here. */
    if (init->node_type->kind != TYPE_INT && init->node_type->kind != TYPE_INT64) {
        return 0;
    }
    /* Parse the literal text. Aether accepts decimal, hex (0x), octal
     * (0o), and binary (0b) integer literals. strtoll handles decimal
     * and 0x out of the box; octal/binary need a lighter touch but
     * the values for byte-range bounds (0..255) are small enough that
     * any of these representations is fine to parse. */
    const char* s = init->value;
    long long v = 0;
    if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
        v = strtoll(s + 2, NULL, 2);
    } else if (s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) {
        v = strtoll(s + 2, NULL, 8);
    } else {
        v = strtoll(s, NULL, 0);  /* handles decimal and 0x / 0X */
    }
    return (v < 0 || v > 255) ? 1 : 0;
}

/* Is this expression a compile-time constant — i.e. valid as the
 * RHS of `const X = ...`?
 *
 * Aether's `const` is substitution-at-each-use: the compiler inlines
 * the RHS expression at every reference rather than allocating
 * storage for the value. That works for `const PI = 3.14` (each use
 * inlines a float literal) but is silently wrong for
 * `const G = make_thing()` — every reference to G would re-call
 * make_thing(), allocating fresh state each time. The previous
 * behaviour was to accept it and produce wrong-but-runs code; per
 * Nico's design call we now reject these at compile time.
 *
 * Allowed RHS forms:
 *   - Literal (int / float / bool / string / null)
 *   - Identifier referring to another const
 *   - Unary expression on a const-expression operand
 *   - Binary expression where both operands are const-expressions
 *   - Interpolated string ("${X}") where every interpolated value
 *     is itself a const-expression — useful for building
 *     concatenated literal-ish strings at the const layer
 *   - Array literal where every element is a const-expression
 *     (lowered to a `static const` C array, not #define-inlined)
 *   - A HARD-WHITELISTED pure-conversion call on const arguments —
 *     `string.from_int` / `string.from_long` / `string.from_float` /
 *     `string.concat` (see is_const_expr_call). The optimizer folds
 *     these to a literal post-typecheck (issue #482).
 *
 * Disallowed:
 *   - Any function call NOT on the whitelist (the headline trap Nico
 *     flagged — an arbitrary call is inlined per use, re-evaluating
 *     side effects; and a general compile-time evaluator could
 *     synthesize std.fs / std.net calls past the --emit=lib
 *     capability gate, so the folder is whitelist-only by design)
 *   - Member access (could be a namespaced call; treat as non-const)
 *   - Struct literals (would allocate fresh per use)
 *   - Anything else
 *
 * `table` may be NULL — when called from the global pre-pass, the
 * const-symbol's binding existence in the table is what we'd check;
 * we treat unknown identifiers as non-const to err on the safe
 * side. The check is robust to NULL.
 */

static int is_const_expression(ASTNode* expr, SymbolTable* table);

/* The hard whitelist of pure conversions that may appear in a const
 * initializer. Kept byte-for-byte in sync with the optimizer's
 * is_whitelisted_string_call (compiler/codegen/optimizer.c) — that pass
 * folds exactly these to literals after typecheck. Accepts both the
 * dotted source spelling (`string.from_int`) and the underscored
 * C-symbol spelling (`string_from_int`) a merged/imported callee can
 * carry. NOTHING outside this list is admissible. */
/* #1366: a top-level function the program itself defines under this name
   shadows the stdlib primitive, so folding the call to a literal would run the
   standard library's semantics instead of the user's body and silently produce
   the wrong answer. */
static int user_defines_top_level_fn(const char* name) {
    if (!name || !g_constfold_program) return 0;
    for (int i = 0; i < g_constfold_program->child_count; i++) {
        ASTNode* child = g_constfold_program->children[i];
        if (!child || child->is_imported) continue;
        if ((child->type == AST_FUNCTION_DEFINITION ||
             child->type == AST_BUILDER_FUNCTION) &&
            child->value && strcmp(child->value, name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_const_expr_call(ASTNode* expr) {
    if (!expr || expr->type != AST_FUNCTION_CALL || !expr->value) return 0;
    const char* name = expr->value;
    if (user_defines_top_level_fn(name)) return 0;
    static const char* const whitelist[] = {
        "from_int", "from_long", "from_float", "concat"
    };
    for (size_t i = 0; i < sizeof(whitelist) / sizeof(whitelist[0]); i++) {
        char dotted[64], under[64];
        snprintf(dotted, sizeof(dotted), "string.%s", whitelist[i]);
        snprintf(under,  sizeof(under),  "string_%s", whitelist[i]);
        if (strcmp(name, dotted) == 0 || strcmp(name, under) == 0) {
            /* Arguments must themselves be const-expressions — guards
             * `string.from_int(rand())` and the like. */
            for (int a = 0; a < expr->child_count; a++) {
                if (!is_const_expression(expr->children[a], NULL)) return 0;
            }
            return 1;
        }
    }
    return 0;
}

static int is_const_expression(ASTNode* expr, SymbolTable* table) {
    if (!expr) return 0;
    switch (expr->type) {
        case AST_LITERAL:
        case AST_NULL_LITERAL:
            return 1;
        case AST_IDENTIFIER: {
            /* Allow only if the named symbol is itself a const.
             * The symbol's `is_const` flag is set when the
             * registration path stored it under a const declaration.
             * We don't have a strict bit today, so fall back to
             * "the binding exists in the table" — this is permissive
             * (a non-const identifier slips through) but it doesn't
             * regress against the current behaviour, and the actual
             * trap (`const G = make_thing()`) is gated by the
             * AST_FUNCTION_CALL case below. */
            if (!table || !expr->value) return 1;
            (void)lookup_symbol;  /* avoid unused-when-NULL noise */
            return 1;
        }
        case AST_UNARY_EXPRESSION:
            return expr->child_count > 0 &&
                   is_const_expression(expr->children[0], table);
        case AST_BINARY_EXPRESSION:
            return expr->child_count >= 2 &&
                   is_const_expression(expr->children[0], table) &&
                   is_const_expression(expr->children[1], table);
        case AST_STRING_INTERP: {
            /* Each interp child must itself be a const-expression
             * (the literal-text children are AST_LITERALs which
             * pass; the substituted-value children must too). */
            for (int i = 0; i < expr->child_count; i++) {
                if (!is_const_expression(expr->children[i], table)) return 0;
            }
            return 1;
        }
        case AST_SIZEOF:
        case AST_OFFSETOF:
            /* #879: `sizeof(T)` / `offsetof(T, f)` lower to C compile-time
             * constants (`((int)sizeof(struct T))` / `((int)offsetof(...))`)
             * — no evaluation, allocation, or side effect. They are valid
             * const initializers (the E0200 "function calls re-evaluate"
             * rationale doesn't apply); they merely share call syntax. Lets a
             * port centralise struct sizes/offsets as named `const`s instead
             * of a hand-maintained, drift-prone offset table. */
            return 1;
        case AST_FUNCTION_CALL:
            /* Only the hard whitelist of pure conversions (folded to
             * literals by the optimizer post-typecheck). Every other
             * call stays rejected. */
            return is_const_expr_call(expr);
        case AST_MEMBER_ACCESS:
        case AST_STRUCT_LITERAL:
            return 0;
        case AST_ARRAY_LITERAL:
            /* A bare array literal is NOT a scalar const — it cannot be
             * #define-inlined. It is admissible only in the const-array
             * form (`const A[] = [...]` / `const A: T[N] = [...]`), which
             * carries the "array_const" annotation and is gated at the
             * declaration site (where each element is checked with
             * is_const_array_element). */
            return 0;
        default:
            return 0;
    }
}

/* Element check for a const array literal. Each element must be a
 * compile-time constant (literal, const identifier, folded const
 * expression, or a whitelisted pure-conversion call). Used by the
 * array_const branch of the const-declaration typecheck. */
static int is_const_array_element(ASTNode* elem, SymbolTable* table) {
    return is_const_expression(elem, table);
}

/* Defined below, beside typecheck_function_call — used by the
 * variable-declaration arm, which appears earlier in this file. */
static int call_yields_no_value(ASTNode* call, SymbolTable* table);

// Type compatibility functions
int is_type_compatible(Type* from, Type* to) {
    if (!from || !to) return 0;
    
    // Unknown types match anything (for inference)
    if (from->kind == TYPE_UNKNOWN || to->kind == TYPE_UNKNOWN) return 1;

    // #480 distinct types: a distinct type converts to/from any other type
    // ONLY via an explicit `as` cast — never implicitly, even when the base
    // `kind` matches. So if either side is distinct, they are compatible only
    // when they are the SAME distinct type. (Checked before the kind-based
    // numeric rules below, which would otherwise let `distinct int` flow into
    // `int`.)
    if (from->distinct_name || to->distinct_name) {
        return from->distinct_name && to->distinct_name &&
               strcmp(from->distinct_name, to->distinct_name) == 0;
    }

    // #479 Isolated[T] is nominal and move-only: it never implicitly converts
    // to or from a bare T. If either side is Isolated, both must be Isolated
    // with compatible wrapped types (the bare T is obtained only via consume()).
    if (from->kind == TYPE_ISOLATED || to->kind == TYPE_ISOLATED) {
        return from->kind == TYPE_ISOLATED && to->kind == TYPE_ISOLATED &&
               is_type_compatible(from->element_type, to->element_type);
    }

    // #1044 enums are integer-backed. An enum is compatible only with the SAME
    // enum (nominal, via types_equal) or with an integer scalar (so `x: int =
    // Color.Red` and `code == Errno.NotFound` typecheck). Two different enums,
    // or an enum and a non-integer, are incompatible.
    if (from->kind == TYPE_ENUM || to->kind == TYPE_ENUM) {
        if (from->kind == TYPE_ENUM && to->kind == TYPE_ENUM)
            return types_equal(from, to);
        TypeKind other = (from->kind == TYPE_ENUM) ? to->kind : from->kind;
        return is_integer_scalar(other);
    }

    // #1046 bit_set is strictly nominal: never an int, never another enum's
    // set. If either side is a bit_set, both must be bit_sets over the same
    // enum (types_equal compares the element enums nominally).
    if (from->kind == TYPE_BITSET || to->kind == TYPE_BITSET) {
        return from->kind == TYPE_BITSET && to->kind == TYPE_BITSET &&
               types_equal(from, to);
    }

    // #1132 bitstruct is strictly nominal, like bit_set and unlike enum: it never
    // implicitly converts to or from its backing integer, and two bitstructs over
    // the same backing type are still different types. Crossing the boundary is an
    // explicit `as` (`f as uint8_t` to get the raw word, `w as Flags` to wrap one).
    // Implicit conversion would defeat the purpose — the value is a packed layout,
    // not a number you can do arithmetic on.
    if (from->kind == TYPE_BITSTRUCT || to->kind == TYPE_BITSTRUCT) {
        return from->kind == TYPE_BITSTRUCT && to->kind == TYPE_BITSTRUCT &&
               types_equal(from, to);
    }

    // Exact match
    if (types_equal(from, to)) return 1;

    // Numeric conversions
    if (from->kind == TYPE_INT && to->kind == TYPE_FLOAT) return 1;
    if (from->kind == TYPE_FLOAT && to->kind == TYPE_INT) return 1;
    // int promotes to long/uint64 without loss
    if (from->kind == TYPE_INT && to->kind == TYPE_INT64) return 1;
    if (from->kind == TYPE_INT && to->kind == TYPE_UINT64) return 1;
    if (from->kind == TYPE_INT64 && to->kind == TYPE_INT) return 1;
    if (from->kind == TYPE_UINT64 && to->kind == TYPE_INT) return 1;
    if (from->kind == TYPE_UINT64 && to->kind == TYPE_INT64) return 1;
    if (from->kind == TYPE_INT64 && to->kind == TYPE_UINT64) return 1;
    // uint32/uint16/uint8 — the underlying kinds of the narrow C ABI
    // aliases (uint32_t, ...). They interconvert freely with every
    // integer scalar; the C compiler does the actual narrowing.
    {
        int from_small = (from->kind == TYPE_UINT32 ||
                          from->kind == TYPE_UINT16 ||
                          from->kind == TYPE_UINT8);
        int to_small   = (to->kind == TYPE_UINT32 ||
                          to->kind == TYPE_UINT16 ||
                          to->kind == TYPE_UINT8);
        if (from_small && is_integer_scalar(to->kind)) return 1;
        if (to_small && is_integer_scalar(from->kind)) return 1;
    }
    // long <-> float compatibility
    if (from->kind == TYPE_INT64 && to->kind == TYPE_FLOAT) return 1;
    if (from->kind == TYPE_FLOAT && to->kind == TYPE_INT64) return 1;
    // #749: longdouble interconverts with int / int64 / uint64 / float.
    // It is the widest C float; the C compiler performs the conversion.
    if (from->kind == TYPE_LONGDOUBLE &&
        (to->kind == TYPE_INT || to->kind == TYPE_INT64 ||
         to->kind == TYPE_UINT64 || to->kind == TYPE_FLOAT)) return 1;
    if (to->kind == TYPE_LONGDOUBLE &&
        (from->kind == TYPE_INT || from->kind == TYPE_INT64 ||
         from->kind == TYPE_UINT64 || from->kind == TYPE_FLOAT)) return 1;

    // Array compatibility
    if (from->kind == TYPE_ARRAY && to->kind == TYPE_ARRAY) {
        return is_type_compatible(from->element_type, to->element_type);
    }

    // #892: array-to-pointer decay (C semantics). A fixed-size array is
    // assignable to a pointer — `ptr p = byte_buf`, a `ptr`-typed argument,
    // or comparison against a pointer. This already worked at call sites; the
    // rule makes it uniform so a stack-buffer-with-heap-fallback binding
    // (`ids = static_ids; ... ids = heap`) type-checks.
    if (from->kind == TYPE_ARRAY && to->kind == TYPE_PTR) return 1;

    // Actor reference compatibility
    // Bare actor_ref (no type parameter) is compatible with any actor_ref
    if (from->kind == TYPE_ACTOR_REF && to->kind == TYPE_ACTOR_REF) {
        if (!from->element_type || !to->element_type) return 1;
        return is_type_compatible(from->element_type, to->element_type);
    }

    // Actor refs stored in int/ptr state fields (common wiring pattern: state ref = 0)
    if (from->kind == TYPE_ACTOR_REF &&
        (to->kind == TYPE_INT || to->kind == TYPE_INT64 || to->kind == TYPE_PTR)) return 1;
    if (to->kind == TYPE_ACTOR_REF &&
        (from->kind == TYPE_INT || from->kind == TYPE_INT64 || from->kind == TYPE_PTR)) return 1;

    // int ↔ ptr compatibility (e.g. x = 0 then x = ptr_func(), or passing 0 to ptr param)
    if (from->kind == TYPE_INT && to->kind == TYPE_PTR) return 1;
    if (from->kind == TYPE_PTR && to->kind == TYPE_INT) return 1;

    /* fn ↔ ptr compatibility (the Option B1 boxing shape). A closure
     * stored as ptr (struct field, list/map element) and recovered
     * back to a `fn` slot — codegen wraps the cross-direction
     * transitions in _aether_box_closure / _aether_unbox_closure
     * (compiler/codegen/codegen_expr.c at the AST_FUNCTION_CALL
     * arg-emit loop and at struct-field assignment).
     *
     * Without this rule, `h.cb = c` where `h.cb: ptr` and `c: fn`
     * fails at typecheck with E0200 — exactly the gap the aether-ui
     * CVG port hit in `grammar_element.ae` for 17 callback fields
     * (`onClick`, `bindFill`, `whenPredicate`, etc.).  Raw-C-fn-
     * pointer form (TYPE_FUNCTION with is_fnptr=1, storage = void*)
     * has always been ptr-compatible by virtue of the `as fn`
     * cast contract; this extends the same compatibility to the
     * `_AeClosure`-shaped form (is_fnptr=0). */
    if (from->kind == TYPE_FUNCTION && to->kind == TYPE_PTR) return 1;
    if (from->kind == TYPE_PTR && to->kind == TYPE_FUNCTION) return 1;

    // byte → int / int64 / float: safe widenings.
    // Reverse direction (int → byte) is intentionally NOT here — it's
    // gated at the assignment site so out-of-range integer literals
    // produce a compile-time error rather than silent truncation.
    // See `check_byte_assignment_literal_range` in the assignment path.
    if (from->kind == TYPE_BYTE &&
        (to->kind == TYPE_INT || to->kind == TYPE_INT64 ||
         to->kind == TYPE_UINT64 || to->kind == TYPE_FLOAT)) return 1;
    // int → byte allowed for non-literal int (runtime truncate is the
    // contract, matching how other narrowings behave). Literal-range
    // check happens elsewhere.
    if (from->kind == TYPE_INT && to->kind == TYPE_BYTE) return 1;
    if (from->kind == TYPE_INT64 && to->kind == TYPE_BYTE) return 1;

    // #914 sum types. A variant struct value implicitly WRAPS into the sum:
    // `from` is TYPE_STRUCT{V} and `to` is a TYPE_SUM whose variant set
    // contains V. (Sum -> same-sum is the exact-match case above.) There is no
    // implicit UNWRAP (sum -> a variant) — the user must `match`.
    if (to->kind == TYPE_SUM && from->kind == TYPE_STRUCT && from->struct_name) {
        for (int i = 0; i < to->tuple_count; i++) {
            Type* vt = to->tuple_types[i];
            if (vt && vt->struct_name &&
                strcmp(vt->struct_name, from->struct_name) == 0) return 1;
        }
        return 0;
    }

    // #340 optionals. Assigning INTO a `T?`:
    //   - `none` (optional with unknown inner) fits any optional;
    //   - `U?` fits `T?` when inner U is compatible with inner T;
    //   - a bare `U` implicitly WRAPS into `T?` when U is compatible with T.
    // There is no implicit UNWRAP (`T?` -> `T`): the user must write `!` or
    // `??`, so no rule for `from->kind == TYPE_OPTIONAL && to non-optional`.
    if (to->kind == TYPE_OPTIONAL) {
        if (from->kind == TYPE_OPTIONAL) {
            if (!from->element_type || from->element_type->kind == TYPE_UNKNOWN) return 1;
            return is_type_compatible(from->element_type, to->element_type);
        }
        return is_type_compatible(from, to->element_type);
    }

    return 0;
}

/* #1240: does `rhs` name a top-level function whose signature matches the
 * function-pointer type `target`?
 *
 * The whole point of a typed callback field is that the slot rejects the wrong
 * function, so arity and every parameter are compared, not just "it is some
 * function". An UNKNOWN on either side is a match, the same latitude the UFCS
 * receiver check allows, since inference may still refine it. Only bare
 * top-level functions qualify: a closure carries an environment pointer that a
 * C function pointer has nowhere to put. */
static int fn_name_matches_signature(SymbolTable* table, ASTNode* rhs, Type* target) {
    if (!table || !rhs || !target || target->kind != TYPE_FUNCTION) return 0;
    if (rhs->type != AST_IDENTIFIER || !rhs->value) return 0;

    Symbol* fs = lookup_symbol(table, rhs->value);
    if (!fs || !fs->is_function || !fs->node) return 0;
    if (fs->node->type != AST_FUNCTION_DEFINITION &&
        fs->node->type != AST_BUILDER_FUNCTION) return 0;

    /* Return type: the function symbol's own type IS its return type. */
    Type* want_ret = target->return_type;
    if (want_ret && fs->type &&
        want_ret->kind != TYPE_UNKNOWN && fs->type->kind != TYPE_UNKNOWN &&
        !is_type_compatible(fs->type, want_ret)) return 0;

    int declared = 0;
    for (int i = 0; i < fs->node->child_count; i++) {
        ASTNode* p = fs->node->children[i];
        if (!p) continue;
        if (p->type != AST_VARIABLE_DECLARATION &&
            p->type != AST_PATTERN_VARIABLE &&
            p->type != AST_PATTERN_LITERAL) continue;
        if (declared < target->param_count) {
            Type* want = target->param_types ? target->param_types[declared] : NULL;
            if (want && p->node_type &&
                want->kind != TYPE_UNKNOWN && p->node_type->kind != TYPE_UNKNOWN &&
                !is_type_compatible(p->node_type, want)) return 0;
        }
        declared++;
    }
    return declared == target->param_count;
}

int is_assignable(Type* from, Type* to) {
    return is_type_compatible(from, to);
}

int is_callable(Type* type) {
    if (!type) return 0;
    switch (type->kind) {
        case TYPE_INT:
        case TYPE_INT64:
        case TYPE_UINT64:
        case TYPE_DURATION:
        case TYPE_FLOAT:
        case TYPE_LONGDOUBLE:
        case TYPE_BOOL:
        case TYPE_STRING:
        case TYPE_VOID:
        case TYPE_ARRAY:
        case TYPE_WILDCARD:
        case TYPE_PTR:
            return 0;
        default:
            return 1;
    }
}

// #340: resolve a struct field's declared type by struct name + field name.
// Returns a fresh clone, or NULL if the struct/field is unknown.
static Type* optional_struct_field_type(SymbolTable* table, const char* struct_name,
                                        const char* field) {
    if (!struct_name || !field) return NULL;
    Symbol* s = lookup_symbol(table, struct_name);
    if (s && s->node) {
        for (int i = 0; i < s->node->child_count; i++) {
            ASTNode* f = s->node->children[i];
            if (f && f->value && strcmp(f->value, field) == 0) {
                if (f->node_type && f->node_type->kind != TYPE_UNKNOWN)
                    return clone_type(f->node_type);
                return NULL;
            }
        }
    }
    return NULL;
}

// #1048 field injection: `expr` is a member access `base.field` on struct
// `struct_def` that has no DIRECT field `field`. If a `using`-embedded field's
// struct declares it, rewrite `base.field` into `(base.embed).field` (so both
// typing and codegen route through the embedded struct) and return the field's
// type. Returns NULL when no `using` field provides it. Idempotent: once
// rewritten, the base is the embedded struct and the field resolves directly.
static Type* try_resolve_using_field(SymbolTable* table, ASTNode* struct_def,
                                     ASTNode* expr) {
    if (!table || !struct_def || !expr || !expr->value || expr->child_count < 1) return NULL;
    for (int fi = 0; fi < struct_def->child_count; fi++) {
        ASTNode* uf = struct_def->children[fi];
        if (!uf || uf->type != AST_STRUCT_FIELD || !uf->value ||
            !uf->annotation || strcmp(uf->annotation, "using") != 0) continue;
        if (!uf->node_type || uf->node_type->kind != TYPE_STRUCT ||
            !uf->node_type->struct_name) continue;
        Symbol* inner_sym = lookup_symbol(table, uf->node_type->struct_name);
        if (!inner_sym || !inner_sym->node) continue;
        ASTNode* inner_def = inner_sym->node;
        for (int ij = 0; ij < inner_def->child_count; ij++) {
            ASTNode* inf = inner_def->children[ij];
            if (!inf || !inf->value || strcmp(inf->value, expr->value) != 0) continue;
            ASTNode* mid = create_ast_node(AST_MEMBER_ACCESS, uf->value,
                                           expr->line, expr->column);
            add_child(mid, expr->children[0]);
            set_node_type(mid, clone_type(uf->node_type));
            expr->children[0] = mid;
            Type* ft = (inf->node_type && inf->node_type->kind != TYPE_UNKNOWN)
                       ? clone_type(inf->node_type) : create_type(TYPE_UNKNOWN);
            if (expr->node_type) free_type(expr->node_type);
            expr->node_type = clone_type(ft);
            return ft;
        }
    }
    return NULL;
}

// Type inference functions
Type* infer_type(ASTNode* expr, SymbolTable* table) {
    if (!expr) return NULL;

    switch (expr->type) {
        case AST_LITERAL:
            return clone_type(expr->node_type);

        case AST_NONE_LITERAL:
            // `none` — optional with as-yet-unknown inner; context pins the
            // concrete T (assignment / `??` / `==`). If already pinned, use it.
            if (expr->node_type && expr->node_type->kind == TYPE_OPTIONAL)
                return clone_type(expr->node_type);
            return create_optional_type(create_type(TYPE_UNKNOWN));

        case AST_SEND_ASK: {
            /* `actor ? Request {}` yields whatever the handler replies. The
             * reply message's first field, or the type of a bare `reply
             * <expr>`, resolved from the actor that answers this request.
             * Unresolvable (no such handler, no reply, a reply whose own
             * type is unknown) stays TYPE_UNKNOWN, which is what it was for
             * every ask before this. */
            if (expr->child_count >= 2 && expr->children[1] &&
                expr->children[1]->value) {
                Type* reply = aether_reply_type(aether_typecheck_program_node(),
                                                expr->children[1]->value);
                if (reply) {
                    /* Stamp the node too. Codegen reads node_type to decide
                     * whether the value the asker receives is an owned heap
                     * string it must reclaim; a type that exists only as
                     * this return value would leave every string reply
                     * leaking one deep copy per ask. */
                    if (!expr->node_type || expr->node_type->kind == TYPE_UNKNOWN) {
                        if (expr->node_type) free_type(expr->node_type);
                        expr->node_type = clone_type(reply);
                    }
                    return clone_type(reply);
                }
            }
            return create_type(TYPE_UNKNOWN);
        }

        case AST_NULL_COALESCE: {
            // `x ?? d` yields the value type:
            //   - optional `T?`     -> inner T
            //   - result `T!` / (value, err) tuple -> the value slot (#e-u P1.3:
            //     `??` on a fallible is the same value-or-default as `f() or {d}`)
            //   - already non-opt   -> T
            if (expr->child_count < 2) return create_type(TYPE_UNKNOWN);
            Type* o = infer_type(expr->children[0], table);
            Type* r;
            if (o && o->kind == TYPE_OPTIONAL && o->element_type) r = clone_type(o->element_type);
            else if (o && o->kind == TYPE_TUPLE && o->tuple_count >= 2 && o->tuple_types[0])
                r = clone_type(o->tuple_types[0]);
            else if (o) r = clone_type(o);
            else r = infer_type(expr->children[1], table);
            if (o) free_type(o);
            return r;
        }

        case AST_OPTIONAL_CHAIN: {
            // `opt?.field` -> fieldT? (none-propagating).
            if (expr->child_count == 0 || !expr->value)
                return create_optional_type(create_type(TYPE_UNKNOWN));
            Type* o = infer_type(expr->children[0], table);
            Type* inner = (o && o->kind == TYPE_OPTIONAL) ? o->element_type : o;
            Type* field_t = NULL;
            if (inner && inner->kind == TYPE_STRUCT) {
                field_t = optional_struct_field_type(table, inner->struct_name, expr->value);
            } else if (inner && inner->kind == TYPE_PTR && inner->element_type &&
                       inner->element_type->kind == TYPE_STRUCT) {
                field_t = optional_struct_field_type(table, inner->element_type->struct_name, expr->value);
            }
            Type* result = create_optional_type(field_t ? field_t : create_type(TYPE_UNKNOWN));
            if (o) free_type(o);
            return result;
        }

        case AST_TUPLE_UNWRAP: {
            /* `expr!` is polymorphic on the operand type:
             *   - optional `T?`  -> inner `T` (#340 force-unwrap; panics at
             *     runtime on `none`).
             *   - tuple (value, err) -> the FIRST slot type (unwrap-or-trap).
             * A non-optional, non-tuple operand is a type error reported in
             * typecheck_expression. */
            if (expr->child_count == 0 || !expr->children[0])
                return create_type(TYPE_UNKNOWN);
            Type* operand = infer_type(expr->children[0], table);
            Type* result = create_type(TYPE_UNKNOWN);
            if (operand && operand->kind == TYPE_OPTIONAL && operand->element_type) {
                free_type(result);
                result = clone_type(operand->element_type);
            } else if (operand && operand->kind == TYPE_TUPLE &&
                operand->tuple_count >= 1 && operand->tuple_types[0]) {
                free_type(result);
                result = clone_type(operand->tuple_types[0]);
            }
            if (operand) free_type(operand);
            return result;
        }

        case AST_OR_ELSE: {
            // #913: `fallible or handler` yields the fallible's success value
            // type (the first slot of its (value, err) tuple).
            if (expr->child_count < 1 || !expr->children[0])
                return create_type(TYPE_UNKNOWN);
            Type* operand = infer_type(expr->children[0], table);
            Type* result = create_type(TYPE_UNKNOWN);
            if (operand && operand->kind == TYPE_TUPLE &&
                operand->tuple_count >= 1 && operand->tuple_types[0]) {
                free_type(result);
                result = clone_type(operand->tuple_types[0]);
            }
            if (operand) free_type(operand);
            return result;
        }

        case AST_NULL_LITERAL:
            return create_type(TYPE_PTR);

        case AST_SIZEOF:
        case AST_OFFSETOF:
            // Compile-time layout builtins; both lower to a C int.
            return create_type(TYPE_INT);

        case AST_BITSET_LITERAL:   // #1046 `bit_set[E]{...}`; the parser set the
            return expr->node_type ? clone_type(expr->node_type)  // TYPE_BITSET.
                                   : create_type(TYPE_UNKNOWN);

        case AST_BITSET_CARD:      // #1046 `card(s)`; cardinality is an int.
            return create_type(TYPE_INT);

        case AST_VA_START:
            // Opaque va_list cookie, modelled as a ptr.
            return create_type(TYPE_PTR);

        case AST_VA_ARG:
            // Yields whatever C type the call requested (parser set it).
            return expr->node_type ? clone_type(expr->node_type)
                                   : create_type(TYPE_PTR);

        case AST_VA_END:
            return create_type(TYPE_VOID);

        case AST_HEAP_NEW: {
            /* heap.new(T) — yields `*T` (TYPE_PTR with element_type =
             * TYPE_STRUCT{name}). Same result shape as `as *T`. The
             * POD-only gate and struct-existence error live in
             * typecheck_expression; here we just hand back the type. */
            if (!expr->value) return create_type(TYPE_UNKNOWN);
            Type* inner = create_type(TYPE_STRUCT);
            inner->struct_name = strdup(expr->value);
            Type* result = create_type(TYPE_PTR);
            result->element_type = inner;
            return result;
        }

        case AST_PTR_AS_STRUCT_CAST: {
            /* `expr as *StructName` — view the operand as a pointer to
             * StructName. Operand must be ptr-typed. Result is
             * TYPE_PTR with element_type = TYPE_STRUCT{name}, mirroring
             * how TYPE_ACTOR_REF carries its underlying struct. The
             * spelled type at the source level is `*StructName`. */
            if (!expr->value) return create_type(TYPE_UNKNOWN);
            if (expr->child_count == 0 || !expr->children[0])
                return create_type(TYPE_UNKNOWN);
            Type* operand = infer_type(expr->children[0], table);
            if (operand) {
                /* Aether already coerces int<->ptr in places, so we
                 * accept ptr OR int64 OR int as the operand. anything
                 * else is a type error (a struct value isn't a ptr;
                 * neither is a string). */
                int operand_ok = operand->kind == TYPE_PTR ||
                                 operand->kind == TYPE_INT ||
                                 operand->kind == TYPE_INT64 ||
                                 operand->kind == TYPE_UNKNOWN;
                free_type(operand);
                if (!operand_ok) {
                    type_error("`as *T` cast operand must be a `ptr` value",
                               expr->line, expr->column);
                    return create_type(TYPE_UNKNOWN);
                }
            }
            /* Validate the named struct exists — or is a #891 @c_struct
             * overlay (no struct symbol; it's a pure-offset lens). */
            if (!is_c_struct_name(expr->value)) {
                Symbol* struct_sym = lookup_symbol(table, expr->value);
                if (!struct_sym || !struct_sym->type ||
                    struct_sym->type->kind != TYPE_STRUCT) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                        "`as *%s`, '%s' is not a struct type", expr->value, expr->value);
                    type_error(msg, expr->line, expr->column);
                    return create_type(TYPE_UNKNOWN);
                }
            }
            Type* inner = create_type(TYPE_STRUCT);
            inner->struct_name = strdup(expr->value);
            Type* result = create_type(TYPE_PTR);
            result->element_type = inner;
            return result;
        }

        case AST_VALUE_CAST:
            /* `expr as T` (scalar / distinct target, #480) — the result IS the
             * target type the parser stashed (distinct-resolved by then). */
            return expr->node_type ? clone_type(expr->node_type)
                                   : create_type(TYPE_UNKNOWN);

        case AST_PTR_AS_ARRAY_CAST: {
            /* `expr as T[]` — view the operand as a typed C array.
             * Operand must be ptr-typed. Result type is the TYPE_ARRAY
             * the parser stashed on expr->node_type. AST_ARRAY_ACCESS
             * on the result emits `((T*)(expr))[i]` which scales by
             * sizeof(T) via C semantics. */
            if (expr->child_count == 0 || !expr->children[0])
                return create_type(TYPE_UNKNOWN);
            Type* operand = infer_type(expr->children[0], table);
            if (operand) {
                int operand_ok = operand->kind == TYPE_PTR ||
                                 operand->kind == TYPE_INT ||
                                 operand->kind == TYPE_INT64 ||
                                 operand->kind == TYPE_UNKNOWN;
                free_type(operand);
                if (!operand_ok) {
                    type_error("`as T[]` cast operand must be a `ptr` value",
                               expr->line, expr->column);
                    return create_type(TYPE_UNKNOWN);
                }
            }
            if (expr->node_type) return clone_type(expr->node_type);
            return create_type(TYPE_UNKNOWN);
        }

        case AST_PTR_AS_FN_CAST: {
            /* `expr as fn(T1, T2, ...) -> R` — view the operand as a
             * typed C function pointer. Operand must be ptr-typed
             * (or int/int64, since Aether already coerces those to
             * ptr in places). Result type is the TYPE_FUNCTION that
             * the parser populated on expr->node_type. */
            if (expr->child_count == 0 || !expr->children[0])
                return create_type(TYPE_UNKNOWN);
            Type* operand = infer_type(expr->children[0], table);
            if (operand) {
                int operand_ok = operand->kind == TYPE_PTR ||
                                 operand->kind == TYPE_INT ||
                                 operand->kind == TYPE_INT64 ||
                                 operand->kind == TYPE_UINT64 ||
                                 operand->kind == TYPE_UNKNOWN;
                free_type(operand);
                if (!operand_ok) {
                    type_error("`as fn(...)` cast operand must be a `ptr` value",
                               expr->line, expr->column);
                    return create_type(TYPE_UNKNOWN);
                }
            }
            /* The parser stashed the TYPE_FUNCTION (with signature)
             * on the node — return it (copy preserved). */
            if (expr->node_type) {
                /* Don't free — caller may inspect; return a borrow
                 * via a shallow clone to keep ownership clean. */
                Type* dup = create_type(TYPE_FUNCTION);
                dup->is_fnptr = 1;  /* cast → raw fn-pointer */
                dup->param_count = expr->node_type->param_count;
                if (dup->param_count > 0) {
                    dup->param_types = malloc((size_t)dup->param_count * sizeof(Type*));
                    for (int i = 0; i < dup->param_count; i++) {
                        Type* src = expr->node_type->param_types[i];
                        Type* d = create_type(src->kind);
                        d->struct_name = src->struct_name ? strdup(src->struct_name) : NULL;
                        d->element_type = src->element_type;  /* shallow */
                        dup->param_types[i] = d;
                    }
                }
                if (expr->node_type->return_type) {
                    Type* src = expr->node_type->return_type;
                    Type* d = create_type(src->kind);
                    d->struct_name = src->struct_name ? strdup(src->struct_name) : NULL;
                    d->element_type = src->element_type;
                    dup->return_type = d;
                }
                return dup;
            }
            Type* fb = create_type(TYPE_FUNCTION);
            fb->is_fnptr = 1;
            return fb;
        }

        case AST_IF_EXPRESSION:
            // Type is the type of the then-branch expression
            if (expr->child_count >= 2) {
                return infer_type(expr->children[1], table);
            }
            return create_type(TYPE_UNKNOWN);

        case AST_STRING_INTERP:
            return create_type(TYPE_STRING);

        case AST_MATCH_STATEMENT:
            // Return type of first arm's result expression
            if (expr->node_type && expr->node_type->kind != TYPE_UNKNOWN) {
                return clone_type(expr->node_type);
            }
            if (expr->child_count >= 2) {
                ASTNode* first_arm = expr->children[1];
                if (first_arm && first_arm->child_count >= 2) {
                    return infer_type(first_arm->children[1], table);
                }
            }
            return create_type(TYPE_UNKNOWN);

        case AST_ARRAY_LITERAL:
            // Return the inferred array type
            return expr->node_type ? clone_type(expr->node_type) : create_type(TYPE_UNKNOWN);
            
        case AST_IDENTIFIER: {
            Symbol* symbol = lookup_symbol(table, expr->value);
            return (symbol && symbol->type) ? clone_type(symbol->type) : create_type(TYPE_UNKNOWN);
        }
        
        case AST_BINARY_EXPRESSION:
            return infer_binary_type(expr->children[0], expr->children[1],
                                   get_token_type_from_string(expr->value));

        case AST_UNARY_EXPRESSION:
            return infer_unary_type(expr->children[0], 
                                  get_token_type_from_string(expr->value));
            
        case AST_FUNCTION_CALL: {
            /* select(...) yields whatever its branches yield. Nothing inferred
             * it before, so it fell to unresolved and codegen defaulted to int:
             * a select over strings assigned a pointer into an int slot and
             * printed a number. Branches must agree, and disagreement is worth
             * saying out loud, since one branch is only compiled per platform
             * and the mismatch would otherwise surface on someone else's OS. */
            if (expr->value && strcmp(expr->value, "select") == 0 &&
                expr->child_count >= 1) {
                Type* chosen = NULL;
                const char* chosen_key = NULL;
                for (int i = 0; i < expr->child_count; i++) {
                    ASTNode* arg = expr->children[i];
                    if (!arg || arg->type != AST_NAMED_ARG || arg->child_count < 1) continue;
                    Type* t = infer_type(arg->children[0], table);
                    if (!t) continue;
                    if (t->kind == TYPE_UNKNOWN) { free_type(t); continue; }
                    if (!chosen) {
                        chosen = t;
                        chosen_key = arg->value;
                        continue;
                    }
                    if (t->kind != chosen->kind) {
                        char msg[220];
                        snprintf(msg, sizeof(msg),
                                 "select() branches must all yield the same type: "
                                 "'%s' is %s but '%s' is %s",
                                 chosen_key ? chosen_key : "?", type_to_string(chosen),
                                 arg->value ? arg->value : "?", type_to_string(t));
                        type_error(msg, arg->line, arg->column);
                    }
                    free_type(t);
                }
                if (chosen) {
                    if (!expr->node_type || expr->node_type->kind == TYPE_UNKNOWN) {
                        if (expr->node_type) free_type(expr->node_type);
                        expr->node_type = clone_type(chosen);
                    }
                    return chosen;
                }
            }

            /* #479 Isolated[T] builtins, resolved polymorphically here rather
             * than from a fixed symbol type. isolate(x) : Isolated[typeof x];
             * consume(iso) : the wrapped T (when iso is Isolated[T]). */
            if (expr->value && expr->child_count == 1) {
                if (strcmp(expr->value, "isolate") == 0) {
                    Type* inner = infer_type(expr->children[0], table);
                    Type* iso = create_type(TYPE_ISOLATED);
                    iso->element_type = inner ? inner : create_type(TYPE_UNKNOWN);
                    return iso;
                }
                if (strcmp(expr->value, "consume") == 0) {
                    Type* arg = infer_type(expr->children[0], table);
                    if (arg && arg->kind == TYPE_ISOLATED && arg->element_type) {
                        Type* inner = clone_type(arg->element_type);
                        free_type(arg);
                        return inner;
                    }
                    if (arg) free_type(arg);
                    return create_type(TYPE_UNKNOWN);
                }
            }
            Symbol* symbol = lookup_qualified_symbol(table, expr->value);
            if (symbol && symbol->is_function && symbol->type
                && symbol->type->kind != TYPE_VOID
                && symbol->type->kind != TYPE_UNKNOWN) {
                return clone_type(symbol->type);
            }
            /* Calling an fn-typed local: `fp(a, b)` where `fp` is a
             * variable typed `fn(T1, T2, ...) -> R` (typically from
             * `... as fn(...) -> R`).  Return R as the call's type. */
            if (symbol && !symbol->is_function && symbol->type &&
                symbol->type->kind == TYPE_FUNCTION &&
                symbol->type->return_type) {
                return clone_type(symbol->type->return_type);
            }
            /* #749 dispatch through a struct's function-pointer field,
             * `recv.field(args)`. typecheck_call resolves and stamps this,
             * but a declaration reads its initializer through infer_type,
             * so without the same resolution here `n = t.read(...)` bound a
             * `long`-returning call to an `int` and every later read of `n`
             * truncated (#1643). Same shape and same single-level receiver
             * rule as the call-site dispatch. */
            if (expr->value && (!symbol || !symbol->is_function)) {
                const char* dot = strrchr(expr->value, '.');
                size_t rlen = dot ? (size_t)(dot - expr->value) : 0;
                if (dot && rlen > 0 && rlen < 200 &&
                    !memchr(expr->value, '.', rlen)) {
                    char recv[200];
                    memcpy(recv, expr->value, rlen);
                    recv[rlen] = '\0';
                    Type* fsig = resolve_fnptr_struct_field(table, recv, dot + 1, NULL);
                    if (fsig && fsig->return_type) {
                        return clone_type(fsig->return_type);
                    }
                }
            }
            return create_type(TYPE_UNKNOWN);
        }
        
        case AST_ACTOR_REF:
            return create_type(TYPE_ACTOR_REF);
            
        case AST_STRUCT_LITERAL:
            // Return the struct type from node_type (set during type inference)
            return expr->node_type ? clone_type(expr->node_type) : create_type(TYPE_UNKNOWN);
            
        case AST_ARRAY_ACCESS:
            // Return the element type from array access (set during type inference)
            return expr->node_type ? clone_type(expr->node_type) : create_type(TYPE_UNKNOWN);

        case AST_MEMBER_ACCESS: {
            // (#1044 enum member access `Enum.Member` is rewritten to the bare
            // constant identifier `Enum_Member` up front by resolve_enum_types,
            // so it never reaches here as a member access.)
            // Enforce export visibility before resolving. Use the
            // strict per-scope `is_visible_namespace` check (issue #243
            // sealed scopes) so user code that did not import a
            // transitively-pulled-in module gets a clear "not visible"
            // error rather than the looser "not exported" message.
            if (expr->child_count > 0 && expr->children[0] &&
                expr->children[0]->type == AST_IDENTIFIER && expr->children[0]->value &&
                is_visible_namespace(expr->children[0]->value, table) && expr->value &&
                is_export_blocked(expr->children[0]->value, expr->value)) {
                char msg[256];
                snprintf(msg, sizeof(msg), "'%s' is not exported from module '%s'",
                         expr->value, expr->children[0]->value);
                type_error(msg, expr->line, expr->column);
                return create_type(TYPE_UNKNOWN);
            }
            // If node_type already set, use it
            if (expr->node_type && expr->node_type->kind != TYPE_UNKNOWN)
                return clone_type(expr->node_type);
            // #1132 bitstruct field read `b.flag` — the field's declared type.
            // An unknown field name is a hard error here; a bitstruct's fields
            // are a closed, declared set, so a typo should not silently become
            // an UNKNOWN that leaks downstream.
            if (expr->child_count > 0 && expr->children[0] && expr->value) {
                Type* bt = infer_type(expr->children[0], table);
                if (bt && bt->kind == TYPE_BITSTRUCT && bt->struct_name) {
                    ASTNode* f = bitstruct_field_lookup(g_bitstruct_program,
                                                        bt->struct_name, expr->value);
                    if (!f) {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "'%s' is not a field of bitstruct '%s'",
                                 expr->value, bt->struct_name);
                        type_error(msg, expr->line, expr->column);
                        free_type(bt);
                        return create_type(TYPE_UNKNOWN);
                    }
                    free_type(bt);
                    return f->node_type ? clone_type(f->node_type)
                                        : create_type(TYPE_UNKNOWN);
                }
                if (bt) free_type(bt);
            }
            // Namespace-qualified constant access: mymath.PI_APPROX -> mymath_PI_APPROX
            if (expr->child_count > 0 && expr->children[0] &&
                expr->children[0]->type == AST_IDENTIFIER && expr->children[0]->value &&
                is_visible_namespace(expr->children[0]->value, table) && expr->value) {
                char qualified[512];
                snprintf(qualified, sizeof(qualified), "%s_%s",
                         expr->children[0]->value, expr->value);
                Symbol* sym = lookup_symbol(table, qualified);
                if (sym && sym->type) {
                    // Rewrite node in-place for codegen
                    expr->type = AST_IDENTIFIER;
                    free(expr->value);
                    expr->value = strdup(qualified);
                    set_node_type(expr, clone_type(sym->type));
                    return clone_type(sym->type);
                }
            }
            // Look up the struct/actor type and find the field type
            if (expr->child_count > 0 && expr->children[0]) {
                Type* base_type = infer_type(expr->children[0], table);
                /* #891 @c_struct overlay field access. base is a
                 * `*OverlayName` ptr; resolve the field's declared type so the
                 * node carries the right type (string interpolation picks
                 * %lld for a uint64 field instead of defaulting to %d). A
                 * field that is itself an overlay yields `*ThatOverlay` so a
                 * nested chain (`s.last_id.ms`) keeps resolving. */
                if (base_type && base_type->kind == TYPE_PTR &&
                    base_type->element_type &&
                    base_type->element_type->kind == TYPE_STRUCT &&
                    base_type->element_type->struct_name &&
                    is_c_struct_name(base_type->element_type->struct_name) &&
                    expr->value) {
                    Type* ft = c_struct_field_decl_type(base_type->element_type->struct_name,
                                                   expr->value);
                    if (ft) {
                        Type* result;
                        if (ft->kind == TYPE_STRUCT && ft->struct_name &&
                            is_c_struct_name(ft->struct_name)) {
                            /* nested overlay field → pointer-to-overlay */
                            result = create_type(TYPE_PTR);
                            result->element_type = ft;  /* adopt */
                        } else {
                            result = ft;
                        }
                        if (expr->node_type) free_type(expr->node_type);
                        expr->node_type = clone_type(result);
                        free_type(base_type);
                        return result;
                    }
                }
                if (base_type && base_type->kind == TYPE_DURATION) {
                    Type* out = NULL;
                    if (expr->value && strcmp(expr->value, "ns") == 0) {
                        out = create_type(TYPE_INT64);
                    } else if (expr->value &&
                               (strcmp(expr->value, "us") == 0 || strcmp(expr->value, "ms") == 0 ||
                                strcmp(expr->value, "s") == 0 || strcmp(expr->value, "m") == 0 ||
                                strcmp(expr->value, "h") == 0 || strcmp(expr->value, "d") == 0)) {
                        out = create_type(TYPE_FLOAT);
                    }
                    free_type(base_type);
                    if (out) return out;
                    return create_type(TYPE_UNKNOWN);
                }
                // Struct field lookup
                if (base_type && base_type->kind == TYPE_STRUCT && base_type->struct_name) {
                    Symbol* struct_sym = lookup_symbol(table, base_type->struct_name);
                    if (struct_sym && struct_sym->node) {
                        ASTNode* struct_def = struct_sym->node;
                        int direct = 0;
                        for (int fi = 0; fi < struct_def->child_count; fi++) {
                            ASTNode* field = struct_def->children[fi];
                            if (field && field->value && strcmp(field->value, expr->value) == 0) {
                                direct = 1;
                                if (field->node_type && field->node_type->kind != TYPE_UNKNOWN)
                                    return clone_type(field->node_type);
                                break;
                            }
                        }
                        // #1048 field injection: no direct field, so try the
                        // `using`-embedded sub-structs (rewrites base.x ->
                        // (base.embed).x and returns the field's type).
                        if (!direct) {
                            Type* uft = try_resolve_using_field(table, struct_def, expr);
                            if (uft) { free_type(base_type); return uft; }
                        }
                    }
                }
                // Pointer-to-struct field lookup: `*StructName` field access
                // resolves through the underlying struct's field list, same
                // as a value-struct. Only the codegen access mode differs
                // (`->field` vs `.field`).
                if (base_type && base_type->kind == TYPE_PTR && base_type->element_type &&
                    base_type->element_type->kind == TYPE_STRUCT &&
                    base_type->element_type->struct_name) {
                    Symbol* struct_sym = lookup_symbol(table, base_type->element_type->struct_name);
                    if (struct_sym && struct_sym->node) {
                        ASTNode* struct_def = struct_sym->node;
                        for (int fi = 0; fi < struct_def->child_count; fi++) {
                            ASTNode* field = struct_def->children[fi];
                            if (field && field->value && strcmp(field->value, expr->value) == 0) {
                                if (field->node_type && field->node_type->kind != TYPE_UNKNOWN)
                                    return clone_type(field->node_type);
                                break;
                            }
                        }
                    }
                }
                // Actor ref field lookup — look up state declarations in the actor definition
                if (base_type && base_type->kind == TYPE_ACTOR_REF && base_type->element_type &&
                    base_type->element_type->kind == TYPE_STRUCT && base_type->element_type->struct_name) {
                    Symbol* actor_sym = lookup_symbol(table, base_type->element_type->struct_name);
                    if (actor_sym && actor_sym->node) {
                        ASTNode* actor_def = actor_sym->node;
                        for (int fi = 0; fi < actor_def->child_count; fi++) {
                            ASTNode* field = actor_def->children[fi];
                            if (field && field->type == AST_STATE_DECLARATION &&
                                field->value && strcmp(field->value, expr->value) == 0) {
                                if (field->node_type && field->node_type->kind != TYPE_UNKNOWN)
                                    return clone_type(field->node_type);
                                break;
                            }
                        }
                    }
                }
            }
            return create_type(TYPE_UNKNOWN);
        }

        default:
            return create_type(TYPE_UNKNOWN);
    }
}

Type* infer_binary_type(ASTNode* left, ASTNode* right, AeTokenType operator) {
    Type* left_type = left ? left->node_type : NULL;
    Type* right_type = right ? right->node_type : NULL;

    // #1046 bit_set operators. A bit_set is nominal (never an int), so any
    // operator touching one is resolved here before the generic numeric/
    // comparison rules; a mismatch returns TYPE_UNKNOWN (a clean type error).
    //   member in set   -> bool   (left enum member, right same-enum bit_set)
    //   set + set        -> set    (union)
    //   set - set        -> set    (difference)
    //   set <= / >= set  -> bool   (subset / superset)
    //   set == / != set  -> bool   (equality)
    if (operator == TOKEN_IN) {
        if (left_type && right_type && right_type->kind == TYPE_BITSET &&
            left_type->kind == TYPE_ENUM && right_type->element_type &&
            types_equal(left_type, right_type->element_type)) {
            return create_type(TYPE_BOOL);
        }
        return create_type(TYPE_UNKNOWN);
    }
    if ((left_type && left_type->kind == TYPE_BITSET) ||
        (right_type && right_type->kind == TYPE_BITSET)) {
        int both_same = left_type && right_type &&
                        left_type->kind == TYPE_BITSET &&
                        right_type->kind == TYPE_BITSET &&
                        types_equal(left_type, right_type);
        if (operator == TOKEN_PLUS || operator == TOKEN_MINUS)
            return both_same ? clone_type(left_type) : create_type(TYPE_UNKNOWN);
        if (operator == TOKEN_LESS_EQUAL || operator == TOKEN_GREATER_EQUAL ||
            operator == TOKEN_EQUALS || operator == TOKEN_NOT_EQUALS)
            return both_same ? create_type(TYPE_BOOL) : create_type(TYPE_UNKNOWN);
        return create_type(TYPE_UNKNOWN);   // any other op on a bit_set is invalid
    }

    // Duration comparisons are only valid against another Duration.
    if (operator == TOKEN_EQUALS || operator == TOKEN_NOT_EQUALS ||
        operator == TOKEN_LESS || operator == TOKEN_LESS_EQUAL ||
        operator == TOKEN_GREATER || operator == TOKEN_GREATER_EQUAL) {
        if (left_type && right_type &&
            (left_type->kind == TYPE_DURATION || right_type->kind == TYPE_DURATION)) {
            if (left_type->kind == TYPE_DURATION && right_type->kind == TYPE_DURATION) {
                return create_type(TYPE_BOOL);
            }
            return create_type(TYPE_UNKNOWN);
        }
    }

    // Comparison and logical operators always produce bool, even with unknown operands
    switch (operator) {
        case TOKEN_EQUALS:
        case TOKEN_NOT_EQUALS:
        case TOKEN_LESS:
        case TOKEN_LESS_EQUAL:
        case TOKEN_GREATER:
        case TOKEN_GREATER_EQUAL:
        case TOKEN_AND:
        case TOKEN_OR:
            return create_type(TYPE_BOOL);
        default:
            break;
    }

    if (!left_type || !right_type) return create_type(TYPE_UNKNOWN);

    switch (operator) {
        case TOKEN_PLUS:
        case TOKEN_MINUS:
        case TOKEN_MULTIPLY:
        case TOKEN_DIVIDE:
        case TOKEN_MODULO:
            // Numeric operations
            if (left_type->kind == TYPE_UNKNOWN || right_type->kind == TYPE_UNKNOWN) {
                // If either type is unknown (e.g., unresolved parameter), allow it
                return create_type(TYPE_UNKNOWN);
            }
            if ((operator == TOKEN_PLUS || operator == TOKEN_MINUS) &&
                left_type->kind == TYPE_DURATION && right_type->kind == TYPE_DURATION) {
                return create_type(TYPE_DURATION);
            }
            if (operator == TOKEN_MULTIPLY &&
                ((left_type->kind == TYPE_DURATION && is_numeric_scalar(right_type->kind)) ||
                 (right_type->kind == TYPE_DURATION && is_numeric_scalar(left_type->kind)))) {
                return create_type(TYPE_DURATION);
            }
            if (operator == TOKEN_DIVIDE &&
                left_type->kind == TYPE_DURATION && is_numeric_scalar(right_type->kind)) {
                return create_type(TYPE_DURATION);
            }
            if (operator == TOKEN_DIVIDE &&
                left_type->kind == TYPE_DURATION && right_type->kind == TYPE_DURATION) {
                return create_type(TYPE_FLOAT);
            }
            if (left_type->kind == TYPE_DURATION || right_type->kind == TYPE_DURATION) {
                return create_type(TYPE_UNKNOWN);
            }
            // #749: longdouble is the widest numeric — wins over float/int.
            if (left_type->kind == TYPE_LONGDOUBLE || right_type->kind == TYPE_LONGDOUBLE) {
                return create_type(TYPE_LONGDOUBLE);
            }
            if (left_type->kind == TYPE_FLOAT || right_type->kind == TYPE_FLOAT) {
                return create_type(TYPE_FLOAT);
            }
            // byte arithmetic: byte op byte → byte; mixed byte/int → int
            // (the wider type wins, matching how int op int64 → int64).
            // This keeps `b1 + b2` typed as byte for tag-byte / NaN-boxing
            // patterns while letting `b + 1` widen for general arithmetic.
            if (left_type->kind == TYPE_BYTE && right_type->kind == TYPE_BYTE) {
                return create_type(TYPE_BYTE);
            }
            if ((left_type->kind == TYPE_BYTE || right_type->kind == TYPE_BYTE) &&
                (is_integer_scalar(left_type->kind) || is_integer_scalar(right_type->kind))) {
                /* Pick the wider int side */
                return create_type(wider_integer_kind(left_type->kind, right_type->kind));
            }
            if (left_type->kind == TYPE_INT && right_type->kind == TYPE_INT) {
                return create_type(TYPE_INT);
            }
            // ptr arithmetic. For + and -, result is ptr (real
            // pointer offset arithmetic — used by C interop like the
            // mquickjs port). For * / %, fall back to int (legacy
            // "ptr-as-int" use case where ptr boxed an integer
            // value). ptr - ptr → int64 (ptrdiff_t-style). The old
            // unconditional `→ int` rule silently truncated 64-bit
            // pointers when stored in inferred-type locals.
            if (operator == TOKEN_PLUS || operator == TOKEN_MINUS) {
                if ((left_type->kind == TYPE_PTR && (right_type->kind == TYPE_INT || right_type->kind == TYPE_INT64)) ||
                    ((left_type->kind == TYPE_INT || left_type->kind == TYPE_INT64) && right_type->kind == TYPE_PTR)) {
                    return create_type(TYPE_PTR);
                }
                if (left_type->kind == TYPE_PTR && right_type->kind == TYPE_PTR &&
                    operator == TOKEN_MINUS) {
                    return create_type(TYPE_INT64);
                }
                /* Array + int → ptr (and the int + array commutative
                 * form).  Used when an `extern struct` flex-array
                 * field is treated as a byte buffer with offset
                 * arithmetic: `(p as *JSString).buf + idx` is the
                 * idiomatic way to materialise a pointer-to-element
                 * for handoff to a memcpy-style C extern.  In C the
                 * array decays to a pointer at expression level, so
                 * this rule matches the C semantics. */
                if ((left_type->kind == TYPE_ARRAY && (right_type->kind == TYPE_INT || right_type->kind == TYPE_INT64)) ||
                    ((left_type->kind == TYPE_INT || left_type->kind == TYPE_INT64) && right_type->kind == TYPE_ARRAY)) {
                    return create_type(TYPE_PTR);
                }
            }
            if ((left_type->kind == TYPE_PTR && right_type->kind == TYPE_INT) ||
                (left_type->kind == TYPE_INT && right_type->kind == TYPE_PTR) ||
                (left_type->kind == TYPE_PTR && right_type->kind == TYPE_PTR)) {
                return create_type(TYPE_INT);
            }
            // Promote integer operations to the wider integer type.
            if (is_integer_scalar(left_type->kind) && is_integer_scalar(right_type->kind)) {
                return create_type(wider_integer_kind(left_type->kind, right_type->kind));
            }
            if (left_type->kind == TYPE_STRING && right_type->kind == TYPE_STRING) {
                return create_type(TYPE_STRING);
            }
            break;
            
        case TOKEN_AMPERSAND:
        case TOKEN_PIPE:
        case TOKEN_CARET:
        case TOKEN_LSHIFT:
        case TOKEN_RSHIFT:
            // Bitwise operations: integer operands, result matches wider type
            if (left_type->kind == TYPE_UNKNOWN || right_type->kind == TYPE_UNKNOWN) {
                return create_type(TYPE_UNKNOWN);
            }
            // byte op byte → byte. NaN-boxing / packed-tag code does
            // `tag & 0x07`, `flags | 0x80`, etc. — keeping the result
            // typed `byte` lets the value flow back into a `byte` field
            // without an explicit narrowing.
            if (left_type->kind == TYPE_BYTE && right_type->kind == TYPE_BYTE) {
                return create_type(TYPE_BYTE);
            }
            if ((left_type->kind == TYPE_BYTE || right_type->kind == TYPE_BYTE) &&
                (is_integer_scalar(left_type->kind) || is_integer_scalar(right_type->kind))) {
                return create_type(wider_integer_kind(left_type->kind, right_type->kind));
            }
            if (left_type->kind == TYPE_INT && right_type->kind == TYPE_INT) {
                return create_type(TYPE_INT);
            }
            if (is_integer_scalar(left_type->kind) && is_integer_scalar(right_type->kind)) {
                return create_type(wider_integer_kind(left_type->kind, right_type->kind));
            }
            break;

        case TOKEN_ASSIGN:
            return clone_type(right_type);

        default:
            break;
    }
    
    return create_type(TYPE_UNKNOWN);
}

Type* infer_unary_type(ASTNode* operand, AeTokenType operator) {
    Type* operand_type = operand ? operand->node_type : NULL;
    if (!operand_type) return create_type(TYPE_UNKNOWN);
    
    switch (operator) {
        case TOKEN_NOT:
            return create_type(TYPE_BOOL);
            
        case TOKEN_TILDE:
            return clone_type(operand_type); // Bitwise NOT: same integer type

        case TOKEN_MINUS:
        case TOKEN_INCREMENT:
        case TOKEN_DECREMENT:
            return clone_type(operand_type); // Same type as operand

        case TOKEN_AMPERSAND: {
            /* #890: address-of `&lvalue` yields a pointer to the operand's
             * type — `&x` where `x: T` is `*T` (TYPE_PTR with element_type T).
             * Carries the pointee type for `as *T` round-trips and deref, and
             * is assignable to a bare `ptr` parameter (the C `&field`
             * out-param case the issue targets). */
            Type* p = create_type(TYPE_PTR);
            p->element_type = clone_type(operand_type);
            return p;
        }

        default:
            return create_type(TYPE_UNKNOWN);
    }
}

AeTokenType get_token_type_from_string(const char* str) {
    if (!str) return TOKEN_ERROR;
    
    if (strcmp(str, "+") == 0) return TOKEN_PLUS;
    if (strcmp(str, "-") == 0) return TOKEN_MINUS;
    if (strcmp(str, "*") == 0) return TOKEN_MULTIPLY;
    if (strcmp(str, "/") == 0) return TOKEN_DIVIDE;
    if (strcmp(str, "%") == 0) return TOKEN_MODULO;
    if (strcmp(str, "==") == 0) return TOKEN_EQUALS;
    if (strcmp(str, "!=") == 0) return TOKEN_NOT_EQUALS;
    if (strcmp(str, "<") == 0) return TOKEN_LESS;
    if (strcmp(str, "<=") == 0) return TOKEN_LESS_EQUAL;
    if (strcmp(str, ">") == 0) return TOKEN_GREATER;
    if (strcmp(str, ">=") == 0) return TOKEN_GREATER_EQUAL;
    if (strcmp(str, "&&") == 0) return TOKEN_AND;
    if (strcmp(str, "||") == 0) return TOKEN_OR;
    if (strcmp(str, "=") == 0) return TOKEN_ASSIGN;
    if (strcmp(str, "!") == 0) return TOKEN_NOT;
    if (strcmp(str, "++") == 0) return TOKEN_INCREMENT;
    if (strcmp(str, "--") == 0) return TOKEN_DECREMENT;
    if (strcmp(str, "&") == 0) return TOKEN_AMPERSAND;
    if (strcmp(str, "|") == 0) return TOKEN_PIPE;
    if (strcmp(str, "^") == 0) return TOKEN_CARET;
    if (strcmp(str, "~") == 0) return TOKEN_TILDE;
    if (strcmp(str, "<<") == 0) return TOKEN_LSHIFT;
    if (strcmp(str, ">>") == 0) return TOKEN_RSHIFT;
    if (strcmp(str, "in") == 0) return TOKEN_IN;  // #1046 bit_set membership

    return TOKEN_ERROR;
}

// --- Unused variable analysis ---

#define MAX_TRACKED_VARS 256

typedef struct {
    const char* name;
    int line;
    int col;
    int used;
} TrackedVar;

// Collect all AST_IDENTIFIER references in a subtree (excluding declarations)
static void collect_references(ASTNode* node, TrackedVar* vars, int var_count) {
    if (!node) return;

    // An identifier in expression position is a reference
    if (node->type == AST_IDENTIFIER && node->value) {
        for (int i = 0; i < var_count; i++) {
            if (strcmp(vars[i].name, node->value) == 0) {
                vars[i].used = 1;
            }
        }
    }

    // A call through a local is a use of that local: `g(p)` where `g` was
    // assigned via `f as fn(ptr) -> int` parses as AST_FUNCTION_CALL with
    // the callee name in node->value (no AST_IDENTIFIER child), so match
    // the call name against tracked variables too.
    if (node->type == AST_FUNCTION_CALL && node->value) {
        for (int i = 0; i < var_count; i++) {
            if (strcmp(vars[i].name, node->value) == 0) {
                vars[i].used = 1;
            }
        }
    }

    // Match statements with list patterns implicitly reference <expr>_len variables
    // (the codegen generates: int _match_len = <expr>_len;)
    if (node->type == AST_MATCH_STATEMENT && node->child_count > 0) {
        ASTNode* match_expr = node->children[0];
        if (match_expr && match_expr->type == AST_IDENTIFIER && match_expr->value) {
            char len_name[256];
            snprintf(len_name, sizeof(len_name), "%s_len", match_expr->value);
            for (int i = 0; i < var_count; i++) {
                if (strcmp(vars[i].name, len_name) == 0) {
                    vars[i].used = 1;
                }
            }
        }
    }

    // For variable declarations, the RHS is a reference but the name itself is not
    if (node->type == AST_VARIABLE_DECLARATION) {
        // Only walk children (RHS expression), not the declaration name
        for (int i = 0; i < node->child_count; i++) {
            collect_references(node->children[i], vars, var_count);
        }
        return;
    }

    for (int i = 0; i < node->child_count; i++) {
        collect_references(node->children[i], vars, var_count);
    }
}

/* Is `name` a module-level `var` global? Such a name, when it appears
 * as the LHS of a bare `name = expr` inside a function, parses as an
 * AST_VARIABLE_DECLARATION but is NOT a new local — it is a WRITE to
 * the file-scope `static` (#701). Treating it as a local declaration
 * makes the unused-variable pass flag pure-setter functions
 * (`set_x(v) { x = v }`) with a spurious "unused variable 'x'": the
 * write-only assignment never reads the name, and collect_references
 * deliberately skips a declaration's LHS. The scalar case escaped
 * notice only because its setters read-modify-write (`x = x + 1`).
 * Caller passes the module-global name set so these writes are not
 * mistaken for unused local declarations. */
static int is_module_global_name(const char** global_names, int global_count,
                                 const char* name) {
    if (!name || !global_names) return 0;
    for (int i = 0; i < global_count; i++) {
        if (global_names[i] && strcmp(global_names[i], name) == 0) return 1;
    }
    return 0;
}

// Collect variable declarations from a block (non-recursive into nested functions)
static int collect_declarations(ASTNode* node, TrackedVar* vars, int var_count,
                                const char** global_names, int global_count) {
    if (!node || var_count >= MAX_TRACKED_VARS) return var_count;

    if (node->type == AST_VARIABLE_DECLARATION && node->value) {
        // Skip _ prefixed names (intentional discard) and writes to a
        // module-level `var` global (a store to the file-scope static,
        // not a local declaration).
        if (node->value[0] != '_' &&
            !is_module_global_name(global_names, global_count, node->value)) {
            vars[var_count].name = node->value;
            vars[var_count].line = node->line;
            vars[var_count].col = node->column;
            vars[var_count].used = 0;
            var_count++;
        }
    }

    // Don't recurse into nested function definitions or actor definitions
    if (node->type == AST_FUNCTION_DEFINITION || node->type == AST_BUILDER_FUNCTION || node->type == AST_ACTOR_DEFINITION) {
        return var_count;
    }

    for (int i = 0; i < node->child_count; i++) {
        var_count = collect_declarations(node->children[i], vars, var_count,
                                         global_names, global_count);
    }
    return var_count;
}

static void check_unused_variables(ASTNode* body, const char** global_names,
                                   int global_count) {
    if (!body) return;

    TrackedVar vars[MAX_TRACKED_VARS];
    int var_count = 0;

    // Collect declarations
    for (int i = 0; i < body->child_count; i++) {
        var_count = collect_declarations(body->children[i], vars, var_count,
                                         global_names, global_count);
    }

    if (var_count == 0) return;

    // Collect references
    for (int i = 0; i < body->child_count; i++) {
        collect_references(body->children[i], vars, var_count);
    }

    // Warn about unused
    for (int i = 0; i < var_count; i++) {
        if (!vars[i].used) {
            char msg[256];
            snprintf(msg, sizeof(msg), "unused variable '%s'", vars[i].name);
            AetherError warn = {
                .filename = NULL,
                .source_code = NULL,
                .line = vars[i].line,
                .column = vars[i].col,
                .message = msg,
                .suggestion = "prefix with '_' to suppress this warning",
                .context = NULL,
                .code = AETHER_WARN_UNUSED_VAR
            };
            aether_warning_report(&warn);
            warning_count++;
        }
    }
}

// --- Unreachable code analysis ---

// Check if a statement is a return or exit() call
static int is_terminating(ASTNode* node) {
    if (!node) return 0;
    if (node->type == AST_RETURN_STATEMENT) return 1;
    // Unwrap expression statement to check inner call
    if (node->type == AST_EXPRESSION_STATEMENT && node->child_count > 0) {
        return is_terminating(node->children[0]);
    }
    if (node->type == AST_FUNCTION_CALL && node->value &&
        strcmp(node->value, "exit") == 0) return 1;
    // if/else where BOTH branches terminate
    if (node->type == AST_IF_STATEMENT && node->child_count >= 3) {
        ASTNode* then_branch = node->children[1];
        ASTNode* else_branch = node->children[2];
        // Check last statement of each branch
        if (then_branch && else_branch) {
            int then_terminates = 0;
            int else_terminates = 0;
            if (then_branch->child_count > 0)
                then_terminates = is_terminating(then_branch->children[then_branch->child_count - 1]);
            else
                then_terminates = is_terminating(then_branch);
            if (else_branch->child_count > 0)
                else_terminates = is_terminating(else_branch->children[else_branch->child_count - 1]);
            else
                else_terminates = is_terminating(else_branch);
            return then_terminates && else_terminates;
        }
    }
    return 0;
}

static void check_unreachable_code(ASTNode* body) {
    if (!body) return;

    for (int i = 0; i < body->child_count; i++) {
        ASTNode* stmt = body->children[i];
        if (is_terminating(stmt) && i + 1 < body->child_count) {
            // Next statement is unreachable
            ASTNode* unreachable = body->children[i + 1];
            if (unreachable) {
                char msg[256];
                snprintf(msg, sizeof(msg), "unreachable code after %s",
                         stmt->type == AST_RETURN_STATEMENT ? "return" :
                         (stmt->type == AST_FUNCTION_CALL ? "exit()" : "terminating block"));
                AetherError warn = {
                    .filename = NULL,
                    .source_code = NULL,
                    .line = unreachable->line,
                    .column = unreachable->column,
                    .message = msg,
                    .suggestion = "remove unreachable code or restructure control flow",
                    .context = NULL,
                    .code = AETHER_WARN_UNREACHABLE
                };
                aether_warning_report(&warn);
                warning_count++;
            }
            break;  // Only warn once per block
        }

        // Recurse into blocks (if/else bodies, while bodies, etc.)
        if (stmt->type == AST_IF_STATEMENT) {
            for (int j = 1; j < stmt->child_count; j++) {
                check_unreachable_code(stmt->children[j]);
            }
        } else if (stmt->type == AST_WHILE_LOOP && stmt->child_count > 1) {
            check_unreachable_code(stmt->children[1]);
        }
    }
}

// #521: reject escapes of `@scoped` bindings (defined below, used in the
// unused-variable/unreachable third pass).
static void scoped_check_bindings(ASTNode* node, ASTNode* root);
// #479: enforce Isolated[T] move-only linearity over a function body.
static void iso_check_function(ASTNode* fn, ASTNode* body);
// #481: validate `@pure`/`@no_fs`/`@no_net`/`@no_os` effect tags.
static void check_effect_tags(ASTNode* program);
// #522: fold `__pure(fn)` queries to compile-time bool constants.
/* Replace a node's type, releasing the one it already had.
 *
 * Inference runs before this walk and leaves most expressions already typed,
 * so a plain `expr->node_type = clone_type(...)` orphans that first type: four
 * of them per trivial program, which is what kept the typechecker out of the
 * LeakSanitizer job (#1575). The new type is always freshly created or cloned
 * at these sites, so freeing first cannot free what is being assigned. */
static void set_node_type(ASTNode* node, Type* fresh) {
    if (!node) { if (fresh) free_type(fresh); return; }
    if (node->node_type == fresh) return;
    if (node->node_type) free_type(node->node_type);
    node->node_type = fresh;
}

static void resolve_purity_queries(ASTNode* node, ASTNode* program,
                                   const char** globals, int nglobals);
// #480: resolve `type X = distinct Y` placeholders into distinct Types.
static void resolve_distinct_types(ASTNode* program);
// #914: resolve `type Name = A | B | C` references into TYPE_SUM.
static void resolve_sum_types(ASTNode* program);
static void sum_apply(Type* t, ASTNode* def);   // fill TYPE_SUM variant Types
// #1044: resolve `enum Name { ... }` references (TYPE_STRUCT -> TYPE_ENUM).
static void resolve_enum_types(ASTNode* program);
static void resolve_bitstruct_types(ASTNode* program);

// #891: typecheck-time registry of @c_struct overlay names. Codegen has its
// own field-level registry; the typechecker only needs the NAME set so an
// `expr as *Name` cast and member access against it validate.
#define AETHER_MAX_C_STRUCTS 256
static const char* g_c_struct_names[AETHER_MAX_C_STRUCTS];
static int g_c_struct_name_count = 0;
static ASTNode* g_c_struct_program = NULL;   /* for field-type lookup (#891) */

static void collect_c_struct_names(ASTNode* program) {
    g_c_struct_name_count = 0;
    g_c_struct_program = program;
    if (!program) return;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_C_STRUCT_DEF && c->value &&
            g_c_struct_name_count < AETHER_MAX_C_STRUCTS)
            g_c_struct_names[g_c_struct_name_count++] = c->value;
    }
}

// #1044 first-class enums. Name registry + program handle so `EnumName.Member`
// member access and `x: EnumName` annotations resolve without a symbol-table
// round-trip (mirrors the @c_struct registry above).
#define AETHER_MAX_ENUMS 256
static const char* g_enum_names[AETHER_MAX_ENUMS];
static int g_enum_name_count = 0;
static ASTNode* g_enum_program = NULL;

// #1044 implicit enum selector: the return type of the function currently being
// typechecked, so `return Blue` can resolve the bare member against it. Set and
// restored around each function body (save/restore handles nesting). Borrowed
// pointer, never freed here.
static Type* g_tc_return_type = NULL;

static int is_enum_type_name(const char* name) {
    if (!name) return 0;
    for (int i = 0; i < g_enum_name_count; i++)
        if (strcmp(g_enum_names[i], name) == 0) return 1;
    return 0;
}

// #1132 bitstruct: program handle so a bitstruct name in type position, and a
// field access on a bitstruct value, both resolve without a symbol-table
// round-trip (same shape as the enum registry above).
static ASTNode* g_bitstruct_program;   /* forward-declared at the top of the file */

// The AST_BITSTRUCT_DEFINITION named `name`, or NULL.
static ASTNode* bitstruct_def_lookup(ASTNode* program, const char* name) {
    if (!program || !name) return NULL;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_BITSTRUCT_DEFINITION && c->value &&
            strcmp(c->value, name) == 0)
            return c;
    }
    return NULL;
}

// The AST_BITSTRUCT_FIELD named `field` on bitstruct `bname`, or NULL.
static ASTNode* bitstruct_field_lookup(ASTNode* program, const char* bname,
                                       const char* field) {
    ASTNode* def = bitstruct_def_lookup(program, bname);
    if (!def || !field) return NULL;
    for (int i = 0; i < def->child_count; i++) {
        ASTNode* f = def->children[i];
        if (f && f->type == AST_BITSTRUCT_FIELD && f->value &&
            strcmp(f->value, field) == 0)
            return f;
    }
    return NULL;
}

/* Contract folding, definition-site tier (docs/contract-folding.md): a
 * `requires` / `where` / `ensures` predicate that is decidably FALSE with no
 * arguments substituted can never be satisfied by ANY call — the realistic
 * way to write one is a refactor that stales a constant (`requires cap >
 * MIN_CAP` after a `const MIN_CAP` edit makes it unsatisfiable). Erroring at
 * the definition beats panicking at the first runtime call.
 *
 * This deliberately reverses codegen's earlier "constant-false falls through
 * to a runtime panic" stance, which was right when folding's only job was
 * check ELISION (a perf feature shouldn't change what compiles) and is wrong
 * for a diagnostic. UNKNOWN — anything touching a parameter, a call, or a
 * non-const name — stays a runtime check and says nothing. */
static void contract_check_clauses_at_definition(ASTNode* func) {
    if (!func) return;
    ContractEnv env = {{0}, {0}, 0, g_enum_program};
    for (int i = 0; i < func->child_count; i++) {
        ASTNode* cl = func->children[i];
        if (!cl || cl->child_count == 0) continue;
        int is_req = (cl->type == AST_REQUIRES_CLAUSE);
        int is_ens = (cl->type == AST_ENSURES_CLAUSE);
        if (!is_req && !is_ens) continue;
        if (contract_eval_predicate(cl->children[0], &env) != CONTRACT_FALSE)
            continue;
        char ptxt[512];
        ContractStr ps = { ptxt, sizeof(ptxt), 0 };
        contract_sprint_expr(&ps, cl->children[0]);
        contract_str_terminate(&ps);
        char emsg[768];
        snprintf(emsg, sizeof(emsg),
                 is_req
                     ? "`requires` predicate is always false, no call to '%s' "
                       "can ever satisfy it: %s"
                     : "`ensures` predicate is always false, '%s' can never "
                       "return legally: %s",
                 func->value ? func->value : "<fn>", ptxt);
        type_error(emsg, cl->line, cl->column);
    }
}

// The AST_ENUM_DEFINITION for `name`, or NULL.
static ASTNode* enum_def_lookup(const char* name) {
    if (!name || !g_enum_program) return NULL;
    for (int i = 0; i < g_enum_program->child_count; i++) {
        ASTNode* c = g_enum_program->children[i];
        if (c && c->type == AST_ENUM_DEFINITION && c->value &&
            strcmp(c->value, name) == 0) return c;
    }
    return NULL;
}

// Does enum `ename` declare a member named `member`?
static int enum_has_member(const char* ename, const char* member) {
    ASTNode* def = enum_def_lookup(ename);
    if (!def || !member) return 0;
    for (int i = 0; i < def->child_count; i++) {
        ASTNode* m = def->children[i];
        if (m && m->type == AST_ENUM_MEMBER && m->value &&
            strcmp(m->value, member) == 0) return 1;
    }
    return 0;
}

// #1044 implicit enum selector. When a bare identifier `Blue` appears where an
// enum type `Color` is expected (a function argument, a typed initializer, a
// return value, or the other side of an enum comparison) and `Blue` is a member
// of that enum, lower it in place to the enum constant `Color_Blue` with a
// TYPE_ENUM node_type, matching exactly the node resolve_enum_types produces for
// the qualified `Color.Blue`. That constant is a registered global symbol, so
// ordinary resolution then succeeds. Returns 1 if the node was rewritten.
// Guards keep it non-breaking: only a bare AST_IDENTIFIER that is not already an
// enum value and whose name does NOT resolve to a real binding is coerced, so an
// actual variable of that name always wins.
static int coerce_bare_enum_member(ASTNode* expr, Type* expected, SymbolTable* table) {
    if (!expr || expr->type != AST_IDENTIFIER || !expr->value) return 0;
    if (!expected || expected->kind != TYPE_ENUM || !expected->struct_name) return 0;
    if (expr->node_type && expr->node_type->kind == TYPE_ENUM) return 0;
    if (!enum_has_member(expected->struct_name, expr->value)) return 0;
    if (table && lookup_symbol(table, expr->value)) return 0;  // a real binding wins
    char qualified[512];
    snprintf(qualified, sizeof(qualified), "%s_%s", expected->struct_name, expr->value);
    free(expr->value);
    expr->value = strdup(qualified);
    if (expr->node_type) free_type(expr->node_type);
    Type* et = create_type(TYPE_ENUM);
    et->struct_name = strdup(expected->struct_name);
    expr->node_type = et;
    return 1;
}

// #1044 enum-indexed arrays: the C array slot count for `[E]T`, i.e. the max
// member value + 1 (values follow the same implicit `previous + 1` / explicit
// integer-literal rule as codegen's emit_enum_typedef). Returns 0 if the enum
// is unknown. A non-literal explicit value (`A = 1 << 2`) is not evaluated here;
// enum-indexed arrays require integer-literal member values, so those size to
// their literal text via the same path C uses.
static int enum_slot_count(const char* ename) {
    ASTNode* def = enum_def_lookup(ename);
    if (!def) return 0;
    int cur = 0, maxv = -1;
    for (int i = 0; i < def->child_count; i++) {
        ASTNode* m = def->children[i];
        if (!m || m->type != AST_ENUM_MEMBER) continue;
        if (m->child_count > 0 && m->children[0] && m->children[0]->value)
            cur = atoi(m->children[0]->value);
        if (cur > maxv) maxv = cur;
        cur++;
    }
    return maxv + 1;
}

static int is_c_struct_name(const char* name) {
    if (!name) return 0;
    for (int i = 0; i < g_c_struct_name_count; i++)
        if (strcmp(g_c_struct_names[i], name) == 0) return 1;
    return 0;
}

/* #891: find the AST_C_STRUCT_DEF for `name`. */
static ASTNode* c_struct_def_node(const char* name) {
    if (!name || !g_c_struct_program) return NULL;
    for (int i = 0; i < g_c_struct_program->child_count; i++) {
        ASTNode* c = g_c_struct_program->children[i];
        if (c && c->type == AST_C_STRUCT_DEF && c->value && strcmp(c->value, name) == 0)
            return c;
    }
    return NULL;
}

/* #891: declared Type of `struct.field` (field may be a dotted chain for
 * nested overlays). Returns a cloned Type (caller frees), or NULL if unknown.
 * Lets member-access typing set the right node_type so string interpolation
 * picks %lld for a uint64 field instead of defaulting to %d. */
static Type* c_struct_field_decl_type(const char* sname, const char* field) {
    ASTNode* def = c_struct_def_node(sname);
    if (!def || !field) return NULL;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", field);
    char* seg = buf;
    while (seg && *seg) {
        char* dot = strchr(seg, '.');
        if (dot) *dot = '\0';
        ASTNode* fnode = NULL;
        for (int i = 0; i < def->child_count; i++) {
            ASTNode* f = def->children[i];
            if (f && f->type == AST_STRUCT_FIELD && f->value && strcmp(f->value, seg) == 0) {
                fnode = f; break;
            }
        }
        if (!fnode || !fnode->node_type) return NULL;
        if (dot) {
            /* descend: this field must name another overlay */
            if (fnode->node_type->kind != TYPE_STRUCT || !fnode->node_type->struct_name)
                return NULL;
            def = c_struct_def_node(fnode->node_type->struct_name);
            if (!def) return NULL;
            seg = dot + 1;
        } else {
            return clone_type(fnode->node_type);
        }
    }
    return NULL;
}

// Type checking functions
/* The program being checked, so infer_type can resolve an ask's reply type
 * from the actor that answers it. infer_type takes only an expression and a
 * symbol table, and threading a program through every one of its call sites
 * would touch far more than the one case that needs it. */
static ASTNode* g_typecheck_program = NULL;

static ASTNode* aether_typecheck_program_node(void) { return g_typecheck_program; }

int typecheck_program(ASTNode* program) {
    if (!program || program->type != AST_PROGRAM) return 0;
    g_typecheck_program = program;

    error_count = 0;
    warning_count = 0;
    namespace_count = 0;  // Reset imported namespaces
    user_explicit_namespace_count = 0;  // Reset user-explicit namespaces (issue #243)
    // (#878: per-module selective-import filter removed — nothing to reset)

    // #480: resolve `type X = distinct Y` placeholders into distinct Types
    // across the whole AST before any type-checking or inference runs.
    resolve_distinct_types(program);

    // #914: resolve `type Name = A | B | C` references — rewrite bare
    // TYPE_STRUCT{Name} use-sites into the real TYPE_SUM. Runs after distinct
    // resolution (a name is one or the other) and before any type-checking.
    resolve_sum_types(program);

    // #1044: resolve `enum Name { ... }`: rewrite bare TYPE_STRUCT{Name}
    // annotations into TYPE_ENUM and populate the enum-name registry, before
    // any type-checking or member-access resolution runs.
    resolve_enum_types(program);

    // #1132: resolve `bitstruct Name : uint8_t { ... }`: rewrite bare
    // TYPE_STRUCT{Name} annotations into TYPE_BITSTRUCT carrying the backing
    // integer. After the enum pass, since a bitstruct field may be enum-typed.
    resolve_bitstruct_types(program);

    // #891: collect @c_struct overlay names so `expr as *Name` casts and
    // member access typecheck against them (they have no struct symbol —
    // they're a pure-offset lens, not a declared struct type).
    collect_c_struct_names(program);

    // #522: fold `__pure(fn)` queries to bool constants before any expression
    // is type-checked. Needs the merged call graph (all function defs), which
    // is present in `program` by now; module globals make a write impure.
    {
        const char* pg[MAX_TRACKED_VARS];
        int npg = 0;
        for (int i = 0; i < program->child_count; i++) {
            ASTNode* c = program->children[i];
            if (c && c->type == AST_EXPORT_STATEMENT && c->child_count > 0) c = c->children[0];
            if (c && c->type == AST_CONST_DECLARATION && c->value && c->annotation &&
                strcmp(c->annotation, "global_var") == 0 && npg < MAX_TRACKED_VARS)
                pg[npg++] = c->value;
        }
        resolve_purity_queries(program, program, pg, npg);
    }

    SymbolTable* global_table = create_symbol_table(NULL);
    
    // Add builtin functions
    // Signature: add_symbol(table, name, type, is_actor, is_function, is_state)
    Type* typeof_type = create_type(TYPE_STRING);
    add_symbol(global_table, "typeof", typeof_type, 0, 1, 0);

    Type* is_type_type = create_type(TYPE_BOOL);
    add_symbol(global_table, "is_type", is_type_type, 0, 1, 0);

    Type* convert_type_type = create_type(TYPE_UNKNOWN);  // Returns any type
    add_symbol(global_table, "convert_type", convert_type_type, 0, 1, 0);

    // Scheduler/concurrency builtins
    Type* wait_idle_type = create_type(TYPE_VOID);
    add_symbol(global_table, "wait_for_idle", wait_idle_type, 0, 1, 0);

    Type* sleep_type = create_type(TYPE_VOID);
    add_symbol(global_table, "sleep", sleep_type, 0, 1, 0);

    // Environment variable builtins
    Type* getenv_type = create_type(TYPE_STRING);  // Returns string (or null)
    add_symbol(global_table, "getenv", getenv_type, 0, 1, 0);

    Type* atoi_type = create_type(TYPE_INT);  // Returns int
    add_symbol(global_table, "atoi", atoi_type, 0, 1, 0);

    // Timing builtin — returns nanoseconds as int64 (int32 overflows after ~2.1 seconds)
    Type* clock_ns_type = create_type(TYPE_INT64);
    add_symbol(global_table, "clock_ns", clock_ns_type, 0, 1, 0);

    // Source-location intrinsics (#265). At codegen time these expand
    // to the AST node's line, source-file path, and enclosing C
    // function name, useful for assertions, panic messages, and log
    // formatters. They capture the location where they are written, so
    // callers that want the call site pass them explicitly:
    // `my_log(msg, __LINE__, __FILE__, __func__)`.
    add_symbol(global_table, "__LINE__", create_type(TYPE_INT),    0, 1, 0);
    add_symbol(global_table, "__FILE__", create_type(TYPE_STRING), 0, 1, 0);
    add_symbol(global_table, "__func__", create_type(TYPE_STRING), 0, 1, 0);

    // Output builtins
    Type* println_type = create_type(TYPE_VOID);
    add_symbol(global_table, "println", println_type, 0, 1, 0);
    Type* print_char_type = create_type(TYPE_VOID);
    add_symbol(global_table, "print_char", print_char_type, 0, 1, 0);

    // Platform selection builtin
    Type* select_type = create_type(TYPE_UNKNOWN);
    add_symbol(global_table, "select", select_type, 0, 1, 0);

    // Process control builtins
    Type* exit_type = create_type(TYPE_VOID);
    add_symbol(global_table, "exit", exit_type, 0, 1, 0);

    // Memory builtins
    Type* free_builtin_type = create_type(TYPE_VOID);
    add_symbol(global_table, "free", free_builtin_type, 0, 1, 0);

    // release(X) — explicit refcount-release sugar. Codegen restricts
    // the argument type to `string` (other heap types call their typed
    // release function). Pairs with `defer` to undo allocations made
    // by stdlib functions returning ownership: `defer release(body)`
    // after `body, err = http.get(url)`.
    Type* release_type = create_type(TYPE_VOID);
    add_symbol(global_table, "release", release_type, 0, 1, 0);

    // #479 Isolated[T] builtins. isolate(x) wraps x in a move-only Isolated[T];
    // consume(iso) unwraps it back to T. Both are polymorphic, so the result
    // type is resolved in infer_type (AST_FUNCTION_CALL); these placeholders
    // exist only so call-site validation treats them as known builtins.
    add_symbol(global_table, "isolate", create_type(TYPE_UNKNOWN), 0, 1, 0);
    add_symbol(global_table, "consume", create_type(TYPE_UNKNOWN), 0, 1, 0);

    // Array/collection builtins
    Type* make_type = create_type(TYPE_PTR);  // returns allocated memory
    add_symbol(global_table, "make", make_type, 0, 1, 0);

    // Closure/iteration builtins
    Type* each_type = create_type(TYPE_VOID);
    add_symbol(global_table, "each", each_type, 0, 1, 0);
    Type* call_type = create_type(TYPE_INT);  // return type depends on closure
    add_symbol(global_table, "call", call_type, 0, 1, 0);
    Type* read_char_type = create_type(TYPE_INT);
    add_symbol(global_table, "read_char", read_char_type, 0, 1, 0);
    Type* char_at_type = create_type(TYPE_INT);
    add_symbol(global_table, "char_at", char_at_type, 0, 1, 0);
    Type* box_closure_type = create_type(TYPE_PTR);
    add_symbol(global_table, "box_closure", box_closure_type, 0, 1, 0);
    Type* unbox_closure_type = create_type(TYPE_FUNCTION);
    add_symbol(global_table, "unbox_closure", unbox_closure_type, 0, 1, 0);
    Type* ref_type = create_type(TYPE_PTR);
    add_symbol(global_table, "ref", ref_type, 0, 1, 0);
    /* Both cells hold an intptr_t; typing the read `int` truncated every
     * value above 2^31 with no diagnostic. */
    Type* ref_get_type = create_type(TYPE_INT64);
    add_symbol(global_table, "ref_get", ref_get_type, 0, 1, 0);
    Type* ref_set_type = create_type(TYPE_VOID);
    add_symbol(global_table, "ref_set", ref_set_type, 0, 1, 0);
    Type* ref_free_type = create_type(TYPE_VOID);
    add_symbol(global_table, "ref_free", ref_free_type, 0, 1, 0);
    // Lazy evaluation builtins
    Type* lazy_type = create_type(TYPE_PTR);
    add_symbol(global_table, "lazy", lazy_type, 0, 1, 0);
    Type* force_type = create_type(TYPE_INT64);
    add_symbol(global_table, "force", force_type, 0, 1, 0);
    Type* thunk_free_type = create_type(TYPE_VOID);
    add_symbol(global_table, "thunk_free", thunk_free_type, 0, 1, 0);
    Type* str_eq_type = create_type(TYPE_INT);
    add_symbol(global_table, "str_eq", str_eq_type, 0, 1, 0);
    Type* raw_mode_type = create_type(TYPE_VOID);
    add_symbol(global_table, "raw_mode", raw_mode_type, 0, 1, 0);
    Type* cooked_mode_type = create_type(TYPE_VOID);
    add_symbol(global_table, "cooked_mode", cooked_mode_type, 0, 1, 0);
    Type* spawn_sandboxed_type = create_type(TYPE_INT);
    add_symbol(global_table, "spawn_sandboxed", spawn_sandboxed_type, 0, 1, 0);
    // num_cores: runtime global (extern int num_cores in multicore_scheduler.h)
    Type* num_cores_type = create_type(TYPE_INT);
    add_symbol(global_table, "num_cores", num_cores_type, 0, 0, 0);
    Type* aether_push_type = create_type(TYPE_VOID);
    add_symbol(global_table, "sandbox_push", aether_push_type, 0, 1, 0);
    Type* aether_pop_type = create_type(TYPE_VOID);
    add_symbol(global_table, "sandbox_pop", aether_pop_type, 0, 1, 0);
    Type* sandbox_install_type = create_type(TYPE_VOID);
    add_symbol(global_table, "sandbox_install", sandbox_install_type, 0, 1, 0);
    Type* sandbox_uninstall_type = create_type(TYPE_VOID);
    add_symbol(global_table, "sandbox_uninstall", sandbox_uninstall_type, 0, 1, 0);
    Type* builder_ctx_type = create_type(TYPE_PTR);
    add_symbol(global_table, "builder_context", builder_ctx_type, 0, 1, 0);
    Type* builder_depth_type = create_type(TYPE_INT);
    add_symbol(global_table, "builder_depth", builder_depth_type, 0, 1, 0);

    // First pass: collect all declarations
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];

        switch (child->type) {
            case AST_ACTOR_DEFINITION: {
                // Create actor struct type
                Type* actor_type = create_type(TYPE_STRUCT);
                actor_type->struct_name = strdup(child->value);
                add_symbol(global_table, child->value, actor_type, 1, 0, 0);
                // Store AST node so state field types can be looked up
                Symbol* actor_sym_node = lookup_symbol(global_table, child->value);
                if (actor_sym_node) actor_sym_node->node = child;
                
                // Add generated spawn_ActorName() function - returns pointer to actor
                // Use TYPE_ACTOR_REF to represent pointer type
                char spawn_name[256];
                snprintf(spawn_name, sizeof(spawn_name), "spawn_%s", child->value);
                Type* spawn_return_type = create_type(TYPE_ACTOR_REF);
                spawn_return_type->element_type = clone_type(actor_type);
                add_symbol(global_table, spawn_name, spawn_return_type, 0, 1, 0);
                
                // Add generated send_ActorName() function - returns void
                char send_name[256];
                snprintf(send_name, sizeof(send_name), "send_%s", child->value);
                Type* send_type = create_type(TYPE_VOID);
                add_symbol(global_table, send_name, send_type, 0, 1, 0);
                
                // Add generated ActorName_step() function - returns void
                char step_name[256];
                snprintf(step_name, sizeof(step_name), "%s_step", child->value);
                Type* step_type = create_type(TYPE_VOID);
                add_symbol(global_table, step_name, step_type, 0, 1, 0);
                break;
            }
            case AST_BUILDER_FUNCTION:
            case AST_FUNCTION_DEFINITION: {
                // Reject two top-level definitions that mangle to the
                // same C symbol (<module>_<name>). A `builder` and a
                // plain function sharing a name both emit that symbol;
                // without this check one silently wins and every call
                // site dispatches to the winner — clean compile, clean
                // link, wrong function body (no diagnostic). The check
                // runs on the post-merge program, so it catches the
                // cross-module case too. See
                // docs/notes/builder-function-name-collision-silent-dispatch.md.
                // Only a builder-vs-plain-function clash is a true
                // collision. Two plain functions sharing a name is the
                // legal multi-clause / pattern-matching form (Erlang-
                // style: several `fib(...)` clauses merged into one
                // dispatcher), so function-vs-function must NOT fire.
                if (child->value) {
                    Symbol* prior = lookup_symbol(global_table, child->value);
                    if (prior && prior->node &&
                        (prior->node->type == AST_FUNCTION_DEFINITION ||
                         prior->node->type == AST_BUILDER_FUNCTION) &&
                        prior->node->type != child->type) {
                        const char* this_kind =
                            (child->type == AST_BUILDER_FUNCTION) ? "builder" : "function";
                        const char* prior_kind =
                            (prior->node->type == AST_BUILDER_FUNCTION) ? "builder" : "function";
                        char msg[320];
                        snprintf(msg, sizeof(msg),
                            "duplicate definition of '%s': a %s and a %s "
                            "cannot share a name (both emit the same C symbol); "
                            "previous definition at line %d",
                            child->value, prior_kind, this_kind,
                            prior->node->line);
                        type_error(msg, child->line, child->column);
                    }
                }
                add_symbol(global_table, child->value, clone_type(child->node_type), 0, 1, 0);
                // Store AST node so arity can be verified at call sites
                Symbol* func_sym = lookup_symbol(global_table, child->value);
                if (func_sym) func_sym->node = child;
                break;
            }
            case AST_EXPORT_STATEMENT: {
                // `export X(...) { ... }` wraps the function definition
                // in AST_EXPORT_STATEMENT. Unwrap and register the inner
                // function so same-file callers can resolve it via the
                // ordinary lexical-scope path. The export modifier is a
                // visibility annotation for cross-module qualified
                // calls; it shouldn't gate intra-module bare calls.
                // Closes #287.
                if (child->child_count > 0) {
                    ASTNode* inner = child->children[0];
                    if (inner && (inner->type == AST_FUNCTION_DEFINITION ||
                                  inner->type == AST_BUILDER_FUNCTION) &&
                        inner->value) {
                        add_symbol(global_table, inner->value,
                                   clone_type(inner->node_type), 0, 1, 0);
                        Symbol* sym = lookup_symbol(global_table, inner->value);
                        if (sym) sym->node = inner;
                    }
                }
                break;
            }
            case AST_EXTERN_FUNCTION: {
                // Register extern C function in symbol table. Wire the
                // AST node into the symbol too so call-site type
                // validation (typecheck_function_call's Duration check
                // and the existing extern-arg type-check block) can
                // walk the declared parameters. Without this, both
                // checks silently no-op on externs because they gate
                // on `symbol->node`.
                add_symbol(global_table, child->value, clone_type(child->node_type), 0, 1, 0);
                Symbol* extern_sym = lookup_symbol(global_table, child->value);
                if (extern_sym) {
                    extern_sym->node = child;
                }
                break;
            }
            case AST_STRUCT_DEFINITION: {
                Type* struct_type = create_type(TYPE_STRUCT);
                struct_type->struct_name = strdup(child->value);
                add_symbol(global_table, child->value, struct_type, 0, 0, 0);
                // Store AST node in symbol for later field type updates
                Symbol* struct_sym = lookup_symbol(global_table, child->value);
                if (struct_sym) {
                    struct_sym->node = child;
                }
                break;
            }
            case AST_SUM_TYPE_DEF: {
                // #914: register the sum name as a TYPE_SUM symbol (variants in
                // tuple_types[]) and stash the def node, so type lookups and the
                // codegen typedef pass can resolve `Name` to its variant set.
                Type* sum_type = create_sum_type(child->value);
                sum_apply(sum_type, child);   // fill variant Types from children
                add_symbol(global_table, child->value, sum_type, 0, 0, 0);
                Symbol* sum_sym = lookup_symbol(global_table, child->value);
                if (sum_sym) sum_sym->node = child;
                break;
            }
            case AST_ENUM_DEFINITION: {
                // #1044: register the enum name (TYPE_ENUM) and each member's C
                // constant `EnumName_Member` as a global symbol of that enum
                // type, so a `EnumName.Member` access (rewritten to the bare
                // identifier by resolve_enum_types) resolves, and `x: EnumName`
                // annotations look up the type.
                Type* etype = create_type(TYPE_ENUM);
                etype->struct_name = strdup(child->value);
                add_symbol(global_table, child->value, etype, 0, 0, 0);
                for (int mi = 0; mi < child->child_count; mi++) {
                    ASTNode* m = child->children[mi];
                    if (!m || m->type != AST_ENUM_MEMBER || !m->value) continue;
                    char qname[512];
                    snprintf(qname, sizeof(qname), "%s_%s", child->value, m->value);
                    Type* mt = create_type(TYPE_ENUM);
                    mt->struct_name = strdup(child->value);
                    add_symbol(global_table, qname, mt, 0, 0, 0);
                }
                break;
            }
            case AST_BITSTRUCT_DEFINITION: {
                // #1132: register the bitstruct name so `x: Flags` annotations
                // resolve. The definition node is stashed on the symbol (as the
                // sum-type case does above) so field lookups can find the bit
                // ranges without re-scanning the program.
                Type* bt = create_bitstruct_type(child->value,
                                                 clone_type(child->node_type));
                add_symbol(global_table, child->value, bt, 0, 0, 0);
                Symbol* bs = lookup_symbol(global_table, child->value);
                if (bs) bs->node = child;
                break;
            }
            case AST_FAULT_DEFINITION: {
                /* #error-unification P3: each fault member is a string-valued
                 * global whose content is its qualified name. Register each
                 * member's identifier as a TYPE_STRING symbol so `fs.NotFound`
                 * (rewritten to the bare member identifier by the module-merge
                 * namespace pass, exactly like a `const`) resolves, and so
                 * `err == fs.NotFound` typechecks as string == string. The
                 * member's C symbol / string content is emitted by codegen. */
                for (int mi = 0; mi < child->child_count; mi++) {
                    ASTNode* m = child->children[mi];
                    if (!m || !m->value) continue;
                    add_symbol(global_table, m->value,
                               create_type(TYPE_STRING), 0, 0, 0);
                }
                break;
            }
            case AST_MESSAGE_DEFINITION: {
                // Register message type so receive patterns can look up field types
                Type* msg_type = create_type(TYPE_MESSAGE);
                add_symbol(global_table, child->value, msg_type, 0, 0, 0);
                Symbol* msg_sym = lookup_symbol(global_table, child->value);
                if (msg_sym) {
                    msg_sym->node = child;
                }
                break;
            }
            case AST_CONST_DECLARATION: {
                // Register constant in symbol table
                Type* ctype = child->node_type ? clone_type(child->node_type) : create_type(TYPE_UNKNOWN);
                // Infer type from the value expression if unknown
                if (ctype->kind == TYPE_UNKNOWN && child->child_count > 0 && child->children[0]->node_type) {
                    free_type(ctype);
                    ctype = clone_type(child->children[0]->node_type);
                }
                /* #1857: the initializer's node_type is only filled in by the
                 * second pass, so a const whose type has to come from its
                 * value was registered UNKNOWN here and stayed that way. Any
                 * function binding it to a local then inferred UNKNOWN too and
                 * codegen warned "unresolved type, defaulting to int" —
                 * spuriously, since the default happened to be right. Infer it
                 * from the expression directly so registration does not depend
                 * on which pass has run. */
                if (ctype->kind == TYPE_UNKNOWN && child->child_count > 0) {
                    Type* inferred = infer_type(child->children[0], global_table);
                    if (inferred) {
                        if (inferred->kind != TYPE_UNKNOWN) {
                            free_type(ctype);
                            ctype = inferred;
                        } else {
                            free_type(inferred);
                        }
                    }
                }
                if (getenv("AE_DEBUG_CONST")) fprintf(stderr, "[const] name=%s kind=%d children=%d child0type=%d child0nt=%d\n", child->value, ctype ? ctype->kind : -1, child->child_count, child->child_count>0?child->children[0]->type:-1, (child->child_count>0 && child->children[0]->node_type)?child->children[0]->node_type->kind:-1);
                add_symbol(global_table, child->value, ctype, 0, 0, 0);
                /* #929: a module-scope `var x = 0` is a global_var whose type
                 * was inferred 32-bit int from a bare initializer. Carry the
                 * parser's `type_inferred` marker onto the symbol (mirroring
                 * the local AST_VARIABLE_DECLARATION path) so the #698
                 * silent-narrowing guard fires on a later 64-bit assignment to
                 * it — without this, `cell = os.now_monotonic_ns()` truncated
                 * silently for a global where the same code errors for a local. */
                if (child->type_inferred && ctype &&
                    (ctype->kind == TYPE_INT)) {
                    Symbol* gs = lookup_symbol_local(global_table, child->value);
                    if (gs) gs->type_inferred = 1;
                }
                break;
            }
            case AST_MAIN_FUNCTION:
                // Main function doesn't need to be in symbol table
                break;
            case AST_IMPORT_STATEMENT: {
                // Process import and register alias if present
                const char* module_path = child->value;

                // Check if this import has an alias. Module aliases are
                // AST_IDENTIFIER children annotated "module_alias" by the
                // parser, distinguishing them from selective-import
                // symbols, which are also AST_IDENTIFIER children but
                // carry the symbol name to expose unqualified.
                if (child->child_count > 0) {
                    ASTNode* last_child = child->children[child->child_count - 1];
                    if (last_child && last_child->type == AST_IDENTIFIER &&
                        last_child->annotation &&
                        strcmp(last_child->annotation, "module_alias") == 0) {
                        const char* alias = last_child->value;
                        add_module_alias(global_table, alias, module_path);
                    }
                }

                // Handle stdlib imports: import std.X (or std.X.Y, std.X.Y.Z, ...)
                if (strncmp(module_path, "std.", 4) == 0) {
                    // For selective-import filter purposes we still want
                    // the substring after `std.` ("fs", "http.client", ...).
                    const char* module_name = module_path + 4;

                    // The qualified-call namespace prefix is the LEAF
                    // component (the bit after the last dot), not the
                    // whole sub-path. For `std.http.client` callers
                    // write `client.foo(...)`, not `http.client.foo(...)`.
                    // This matches what the orchestrator's merger uses
                    // when it prefixes wrapper function names — see
                    // module_get_namespace() in aether_module.c.
                    const char* last_dot = strrchr(module_name, '.');
                    const char* ns_leaf  = last_dot ? last_dot + 1 : module_name;

                    // Register namespace for qualified calls (e.g., string.new)
                    register_namespace(ns_leaf);
                    // User-explicit registration (issue #243 sealed scopes):
                    // skip if this import was synthesized by
                    // module_merge_into_program's BFS transitive-merge
                    // pass. Synthetic imports keep the namespace
                    // resolvable for cloned merged-body callers but
                    // hide it from user code that didn't ask for it.
                    if (!child->annotation ||
                        strcmp(child->annotation, "synthetic") != 0) {
                        register_user_explicit_namespace(ns_leaf);
                    }

                    // #878: nothing to record here — a selective import no
                    // longer restricts the qualified `X.fn()` surface, so
                    // there is no per-module allow list to build. The bare-name
                    // bindings a selective import adds are wired by the merger,
                    // not here.

                    // Look up cached module from orchestrator
                    AetherModule* mod = module_find(module_path);
                    ASTNode* mod_ast = mod ? mod->ast : NULL;
                    if (mod_ast) {
                        // Extract extern declarations from the module.
                        //
                        // Externs are always registered regardless of import
                        // form. Merged Aether-native stdlib wrappers (like
                        // `http.get` calling `http_get_raw` internally) need
                        // every extern from their own module visible in the
                        // global symbol table to compile. (#878: the qualified
                        // `X.fn()` surface is available on any import form, so
                        // there is no selective filter to apply here.)
                        for (int j = 0; j < mod_ast->child_count; j++) {
                            ASTNode* decl = mod_ast->children[j];
                            if (decl->type == AST_EXTERN_FUNCTION && decl->value) {
                                if (!lookup_symbol_local(global_table, decl->value)) {
                                    add_symbol(global_table, decl->value,
                                               clone_type(decl->node_type), 0, 1, 0);
                                    /* #952: wire the extern's AST node into its
                                     * symbol so call-site arity + arg-type checks
                                     * apply to imported externs too — exactly as
                                     * the entry-file extern registration does. */
                                    Symbol* es = lookup_symbol_local(global_table, decl->value);
                                    if (es) es->node = decl;
                                }
                            }
                        }
                        // NOTE: do NOT free mod_ast — registry owns it
                    }
                } else {
                    // Handle local package imports: import mypackage.utils
                    const char* namespace = get_namespace_from_path(module_path);
                    register_namespace(namespace);
                    // Same synthetic-skip gate as the std.* path above
                    // (issue #243 sealed scopes).
                    if (!child->annotation ||
                        strcmp(child->annotation, "synthetic") != 0) {
                        register_user_explicit_namespace(namespace);
                    }

                    // Look up cached module from orchestrator
                    AetherModule* mod = module_find(module_path);
                    ASTNode* mod_ast = mod ? mod->ast : NULL;
                    if (mod_ast) {
                        for (int j = 0; j < mod_ast->child_count; j++) {
                            ASTNode* decl = mod_ast->children[j];
                            if (decl->type == AST_EXTERN_FUNCTION && decl->value) {
                                // Always import externs regardless of import
                                // form. Externs are C bindings that merged
                                // Aether functions may call internally —
                                // filtering them out breaks transitive
                                // references. (#878: no selective filter at
                                // qualified-call sites anymore.)
                                if (!lookup_symbol_local(global_table, decl->value)) {
                                    add_symbol(global_table, decl->value,
                                               clone_type(decl->node_type), 0, 1, 0);
                                    /* #952: wire the extern's AST node into its
                                     * symbol so call-site arity + arg-type checks
                                     * apply to imported externs too — exactly as
                                     * the entry-file extern registration does. */
                                    Symbol* es = lookup_symbol_local(global_table, decl->value);
                                    if (es) es->node = decl;
                                }
                            }
                            // AST_FUNCTION_DEFINITION handled by module_merge_into_program()
                        }
                        // NOTE: do NOT free mod_ast — registry owns it
                    }
                }
                break;
            }
            default:
                break;
        }
    }
    
    // Register unqualified short names for selective imports.
    // At this point all merged function definitions are in the symbol table,
    // so we can look up their types to register the short aliases.
    //
    // Two forms register short aliases here:
    //   1. Selective:  import mod (a, b)        — children are AST_IDENTIFIER
    //   2. Glob:       import mod (*)           — annotation == "glob_import"
    //
    // The glob form synthesizes the same per-name registration by walking
    // every symbol in global_table whose name starts with the module's
    // namespace prefix (`<ns>_`) and registering the trailing short name
    // as an alias. Names with a leading underscore in the short part are
    // treated as private and skipped. Issue #171 (P1).
    import_alias_count = 0;  // Reset for fresh compilation
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if (child->type != AST_IMPORT_STATEMENT || !child->value) continue;

        int is_glob = (child->annotation
                       && strcmp(child->annotation, "glob_import") == 0);

        // Selective imports need at least one identifier child to do anything;
        // glob imports drive their own loop off the symbol table.
        if (!is_glob && child->child_count == 0) continue;
        if (!is_glob) {
            ASTNode* first = child->children[0];
            if (!first || first->type != AST_IDENTIFIER) continue;
        }

        const char* module_path = child->value;
        const char* ns;
        if (strncmp(module_path, "std.", 4) == 0) {
            ns = module_path + 4;
        } else {
            ns = get_namespace_from_path(module_path);
        }

        // Build the iteration source for the inner loop. For selective
        // imports, this is the AST_IDENTIFIER children. For glob imports,
        // we materialize a temporary list of short-name strings by
        // scanning global_table for every "<ns>_*" symbol.
        const char** glob_names = NULL;
        int glob_count = 0;
        int glob_cap = 0;
        if (is_glob) {
            size_t ns_len = strlen(ns);
            for (Symbol* sym = global_table->symbols; sym; sym = sym->next) {
                if (!sym->name) continue;
                if (strncmp(sym->name, ns, ns_len) != 0) continue;
                if (sym->name[ns_len] != '_') continue;
                const char* tail = sym->name + ns_len + 1;
                if (!*tail || *tail == '_') continue;  // private / malformed
                if (glob_count >= glob_cap) {
                    glob_cap = glob_cap == 0 ? 16 : glob_cap * 2;
                    glob_names = (const char**)aether_xrealloc(
                        glob_names, sizeof(const char*) * glob_cap);
                }
                glob_names[glob_count++] = tail;
            }
        }

        int loop_end = is_glob ? glob_count : child->child_count;
        for (int k = 0; k < loop_end; k++) {
            const char* short_name;
            const char* local_name;
            if (is_glob) {
                short_name = glob_names[k];
                local_name = short_name;
            } else {
                ASTNode* sel = child->children[k];
                if (!sel || sel->type != AST_IDENTIFIER) continue;
                short_name = sel->value;
                local_name = short_name;
                /* Per-symbol alias (`import M (path as vgpath)`): the
                 * node's value is the exported name; the LOCAL binding
                 * the program sees is the alias. Resolution below maps
                 * alias -> ns.exported, so a bare `vgpath(...)` rewrites
                 * to the same qualified call `path` would have. */
                if (sel->annotation &&
                    strncmp(sel->annotation, "select_alias:", 13) == 0) {
                    local_name = sel->annotation + 13;
                }
            }

            // Build the full C name: namespace_shortname
            char full_name[256];
            snprintf(full_name, sizeof(full_name), "%s_%s", ns, short_name);

            Symbol* full_sym = lookup_symbol(global_table, full_name);
            if (full_sym) {
                Symbol* existing_short = lookup_symbol_local(global_table, local_name);
                if (!existing_short || !existing_short->is_function) {
                    // Register or override: either no existing symbol, or existing
                    // is not a function (e.g. C's sqrt from math.h)
                    if (existing_short) {
                        // Update existing symbol in place
                        if (existing_short->type) free_type(existing_short->type);
                        existing_short->type = full_sym->type ? clone_type(full_sym->type) : create_type(TYPE_UNKNOWN);
                        existing_short->is_function = full_sym->is_function;
                        existing_short->node = full_sym->node;
                    } else {
                        add_symbol(global_table, local_name,
                                   full_sym->type ? clone_type(full_sym->type) : create_type(TYPE_UNKNOWN),
                                   0, full_sym->is_function, 0);
                        Symbol* short_sym = lookup_symbol(global_table, local_name);
                        if (short_sym && full_sym->node) {
                            short_sym->node = full_sym->node;
                        }
                    }
                }

                // Store alias for AST rewriting: "release" -> "build.release"
                // (or, per-symbol aliased, "vgpath" -> "vg.path").
                // Only register when the prefixed symbol exists — otherwise
                // we'd rewrite calls to externs (which keep their bare
                // names) into nonexistent `ns_extern` forms.
                char dotted[256];
                snprintf(dotted, sizeof(dotted), "%s.%s", ns, short_name);
                add_import_alias(local_name, dotted);
            }
        }
        free((void*)glob_names);
    }

    // NEW: Run type inference before type checking
    if (!infer_all_types(program, global_table)) {
        free_symbol_table(global_table);
        // Clean up namespace strings to avoid leaks on re-runs
        for (int ns = 0; ns < namespace_count; ns++) free(imported_namespaces[ns]);
        namespace_count = 0;
        for (int ns = 0; ns < user_explicit_namespace_count; ns++) free(user_explicit_namespaces[ns]);
        user_explicit_namespace_count = 0;
        return 0;
    }
    
    // Update symbol table with inferred types
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if ((child->type == AST_FUNCTION_DEFINITION || child->type == AST_BUILDER_FUNCTION) && child->value && child->node_type) {
            Symbol* func_sym = lookup_symbol(global_table, child->value);
            if (func_sym) {
                if (func_sym->type) free_type(func_sym->type);
                func_sym->type = clone_type(child->node_type);
            }
        }
    }

    // Re-sync import-alias short symbols from their now-inferred full
    // symbols. The short aliases (registered above for selective AND
    // glob imports) clone the full symbol's type BEFORE `infer_all_types`
    // runs, so a wrapper whose return type is inferred — e.g. an
    // `fs.list_dir` whose `-> { ... return list, "" }` body yields a
    // `(ptr, string)` tuple — leaves the short alias `list_dir` stuck on
    // the pre-inference placeholder (int/unknown). A bare `list, err =
    // list_dir(...)` call then resolves through the stale short symbol,
    // stamps the call node's return type as `int`, and codegen emits
    // `int _tup0 = fs_list_dir(...)` — a C type error. Selective imports
    // happened to dodge this when the call rewrote-and-re-resolved to the
    // full symbol, but the glob path did not; refreshing every alias from
    // the canonical full symbol makes both forms carry the wrappers' real
    // tuple return types. (fbs-core ask #1.)
    for (int a = 0; a < import_alias_count; a++) {
        const char* short_name = import_aliases[a].short_name;
        const char* dotted = import_aliases[a].qualified_name;
        // Build the underscored full C name from the dotted alias
        // ("fs.list_dir" -> "fs_list_dir").
        char full_name[256];
        snprintf(full_name, sizeof(full_name), "%s", dotted);
        for (char* p = full_name; *p; p++) { if (*p == '.') *p = '_'; }

        Symbol* full_sym = lookup_symbol(global_table, full_name);
        Symbol* short_sym = lookup_symbol_local(global_table, short_name);
        if (full_sym && short_sym && short_sym->is_function && full_sym->type) {
            if (short_sym->type) free_type(short_sym->type);
            short_sym->type = clone_type(full_sym->type);
            short_sym->node = full_sym->node;
        }
    }

    // Second pass: type check all nodes
    for (int i = 0; i < program->child_count; i++) {
        typecheck_node(program->children[i], global_table);
    }

    // Collect module-level `var` global names (#701). A bare
    // `name = expr` inside a function whose `name` is one of these is a
    // write to the file-scope static, not a local declaration — the
    // unused-variable pass must not flag write-only setters for them.
    const char* global_var_names[MAX_TRACKED_VARS];
    int global_var_count = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if (child && child->type == AST_EXPORT_STATEMENT && child->child_count > 0) {
            child = child->children[0];  // unwrap `export var ...`
        }
        if (child && child->type == AST_CONST_DECLARATION && child->value &&
            child->annotation && strcmp(child->annotation, "global_var") == 0 &&
            global_var_count < MAX_TRACKED_VARS) {
            global_var_names[global_var_count++] = child->value;
        }
    }

    // Third pass: unused variable + unreachable code analysis
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if ((child->type == AST_FUNCTION_DEFINITION || child->type == AST_BUILDER_FUNCTION) && child->child_count > 0) {
            ASTNode* body = child->children[child->child_count - 1];
            check_unused_variables(body, global_var_names, global_var_count);
            check_unreachable_code(body);
            scoped_check_bindings(body, body);  // #521
            iso_check_function(child, body);    // #479 Isolated[T] move check
        } else if (child->type == AST_MAIN_FUNCTION && child->child_count > 0) {
            // main() has a BLOCK child containing the actual statements
            ASTNode* main_body = child->children[0];
            check_unused_variables(main_body, global_var_names, global_var_count);
            check_unreachable_code(main_body);
            scoped_check_bindings(main_body, main_body);  // #521
            iso_check_function(child, main_body);  // #479 Isolated[T] move check
        }
    }

    // #481: validate effect tags over the whole-program call graph.
    check_effect_tags(program);

    free_symbol_table(global_table);

    // Report errors and warnings
    if (error_count > 0) {
        fprintf(stderr, "Type checking failed with %d error(s)\n", error_count);
        return 0;  // Block compilation on errors
    }
    
    if (warning_count > 0) {
        fprintf(stderr, "Type checking completed with %d warning(s)\n", warning_count);
    }
    
    // Clean up namespace strings
    for (int ns = 0; ns < namespace_count; ns++) free(imported_namespaces[ns]);
    namespace_count = 0;
    for (int ns = 0; ns < user_explicit_namespace_count; ns++) free(user_explicit_namespaces[ns]);
    user_explicit_namespace_count = 0;

    return 1;
}

int typecheck_node(ASTNode* node, SymbolTable* table) {
    if (!node) return 0;
    
    switch (node->type) {
        case AST_ACTOR_DEFINITION:
            return typecheck_actor_definition(node, table);
        case AST_BUILDER_FUNCTION:
        case AST_FUNCTION_DEFINITION:
            return typecheck_function_definition(node, table);
        case AST_EXPORT_STATEMENT:
            // Type-check the inner declaration. The wrapper is purely a
            // visibility annotation; any same-file caller went through
            // the unwrap-and-register path in the first pass (#287).
            if (node->child_count > 0) {
                return typecheck_node(node->children[0], table);
            }
            return 1;
        case AST_EXTERN_FUNCTION:
            // Extern functions have no body to check - just a declaration
            return 1;
        case AST_C_STRUCT_DEF:
            // #891 @c_struct overlay: a declaration of (field, type, offset)
            // tuples — no body, no statements to check. The name set is
            // collected (collect_c_struct_names) and the fields registered at
            // codegen for width/offset resolution. Nothing to typecheck here.
            return 1;
        case AST_STRUCT_DEFINITION:
            return typecheck_struct_definition(node, table);
        case AST_SUM_TYPE_DEF: {
            // #914: variant references were rewritten in resolve_sum_types;
            // validate here that every variant names a real struct (and not
            // another sum — nested sums aren't supported in v1). Nothing else
            // to check per-node.
            for (int i = 0; i < node->child_count; i++) {
                ASTNode* v = node->children[i];
                if (!v || v->type != AST_IDENTIFIER || !v->value) continue;
                Symbol* vs = lookup_symbol(table, v->value);
                if (!vs || !vs->type || vs->type->kind != TYPE_STRUCT) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                        "sum type '%s': variant '%s' must name a struct type",
                        node->value ? node->value : "?", v->value);
                    type_error(msg, v->line, v->column);
                }
            }
            return 1;
        }
        case AST_MAIN_FUNCTION:
            return typecheck_statement(node, table);
        default:
            return typecheck_statement(node, table);
    }
}

// Look up the type of a specific field in a message definition
static Type* lookup_message_field_type(SymbolTable* table, const char* message_name, const char* field_name) {
    Symbol* msg_sym = lookup_symbol(table, message_name);
    if (!msg_sym || !msg_sym->node || msg_sym->node->type != AST_MESSAGE_DEFINITION) {
        return NULL;
    }
    ASTNode* msg_def = msg_sym->node;
    for (int i = 0; i < msg_def->child_count; i++) {
        ASTNode* field = msg_def->children[i];
        if (field->type == AST_MESSAGE_FIELD && field->value && strcmp(field->value, field_name) == 0) {
            return field->node_type ? clone_type(field->node_type) : NULL;
        }
    }
    return NULL;
}

// Validate that message constructor field values match declared field types
static void typecheck_message_constructor(ASTNode* constructor, SymbolTable* table) {
    if (!constructor || constructor->type != AST_MESSAGE_CONSTRUCTOR || !constructor->value) return;
    const char* msg_name = constructor->value;
    Symbol* msg_sym = lookup_symbol(table, msg_name);
    if (!msg_sym || !msg_sym->node || msg_sym->node->type != AST_MESSAGE_DEFINITION) return;

    for (int i = 0; i < constructor->child_count; i++) {
        ASTNode* field_init = constructor->children[i];
        if (!field_init || field_init->type != AST_FIELD_INIT || !field_init->value) continue;
        if (field_init->child_count == 0) continue;

        Type* declared = lookup_message_field_type(table, msg_name, field_init->value);
        if (!declared) continue;

        ASTNode* value_expr = field_init->children[0];
        typecheck_expression(value_expr, table);
        Type* actual = infer_type(value_expr, table);

        /* Cons-cell context: when the declared field type is
         * `*StringSeq` and the RHS is an array literal, accept the
         * assignment and stamp the literal's node_type to *StringSeq.
         * The codegen branch in emit_message_field_init / AST_ARRAY_LITERAL
         * picks up the stamped type and emits a cons chain instead of
         * a static C array initialiser. Same disambiguation rule the
         * variable-decl path uses, lifted to message-field context.
         * See codegen_expr.c emit_message_field_init for the
         * matching codegen branch. */
        int seq_literal_match =
            value_expr && value_expr->type == AST_ARRAY_LITERAL &&
            is_string_seq_ptr_type(declared);

        if (seq_literal_match) {
            if (value_expr->node_type) free_type(value_expr->node_type);
            value_expr->node_type = clone_type(declared);
        } else if (actual && actual->kind != TYPE_UNKNOWN &&
            declared->kind != TYPE_UNKNOWN &&
            !is_type_compatible(actual, declared)) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "Type mismatch in field '%s' of message '%s': expected %s, got %s",
                     field_init->value, msg_name, type_name(declared), type_name(actual));
            type_error(buf, field_init->line, field_init->column);
        }
        free_type(actual);
        free_type(declared);
    }
}

int typecheck_actor_definition(ASTNode* actor, SymbolTable* table) {
    if (!actor || actor->type != AST_ACTOR_DEFINITION) return 0;
    
    SymbolTable* actor_table = create_symbol_table(table);
    
    // Type check actor body
    for (int i = 0; i < actor->child_count; i++) {
        ASTNode* child = actor->children[i];
        
        if (child->type == AST_STATE_DECLARATION) {
            if ((!child->node_type || child->node_type->kind == TYPE_UNKNOWN)
                && child->child_count > 0 && child->children[0]) {
                ASTNode* init = child->children[0];
                if (init->type == AST_FUNCTION_CALL && init->value) {
                    Symbol* fn = lookup_qualified_symbol(actor_table, init->value);
                    if (fn && fn->type) {
                        set_node_type(child, clone_type(fn->type));
                    }
                }
            }
            add_symbol(actor_table, child->value, clone_type(child->node_type), 0, 0, 1);
        } else if (child->type == AST_RECEIVE_STATEMENT) {
            // Handle receive statement
            SymbolTable* receive_table = create_symbol_table(actor_table);

            // V1 syntax: receive(msg) { ... } has child->value set
            // V2 syntax: receive { Pattern -> ... } has child->value = NULL
            if (child->value) {
                Type* msg_type = create_type(TYPE_MESSAGE);
                add_symbol(receive_table, child->value, msg_type, 0, 0, 0);
            }

            // Type check the receive body/arms
            for (int j = 0; j < child->child_count; j++) {
                ASTNode* arm = child->children[j];

                // For V2 receive arms, extract pattern variables
                if (arm->type == AST_RECEIVE_ARM && arm->child_count >= 2) {
                    ASTNode* pattern = arm->children[0];
                    ASTNode* arm_body = arm->children[1];

                    // Add pattern variables to scope
                    if (pattern->type == AST_MESSAGE_PATTERN) {
                        for (int k = 0; k < pattern->child_count; k++) {
                            ASTNode* field = pattern->children[k];
                            if (field->type == AST_PATTERN_FIELD) {
                                // Look up actual field type from message definition
                                Type* field_type = lookup_message_field_type(table, pattern->value, field->value);
                                if (!field_type) {
                                    field_type = create_type(TYPE_UNKNOWN);
                                }
                                // Use pattern variable name if present (field: var), else field name
                                const char* var_name = field->value;
                                if (field->child_count > 0 && field->children[0] &&
                                    field->children[0]->type == AST_PATTERN_VARIABLE && field->children[0]->value) {
                                    var_name = field->children[0]->value;
                                }
                                add_symbol(receive_table, var_name, field_type, 0, 0, 0);
                            }
                        }
                    }

                    // Type check arm body
                    typecheck_statement(arm_body, receive_table);
                } else if (arm->type == AST_TIMEOUT_ARM && arm->child_count >= 2) {
                    // Timeout arm: after N -> { body }
                    typecheck_expression(arm->children[0], receive_table);  // timeout expr
                    typecheck_statement(arm->children[1], receive_table);   // body
                } else {
                    typecheck_statement(arm, receive_table);
                }
            }

            free_symbol_table(receive_table);
            continue;
        }
        
        typecheck_node(child, actor_table);
    }
    
    free_symbol_table(actor_table);
    return 1;
}

/* #521 @scoped bindings — escape rejection.
 *
 * A binding annotated `@scoped` must not outlive the lexical block that
 * introduced it. The check rejects the value escaping via: a return; an
 * assignment / declaration that copies it into another binding or field; an
 * element of an aggregate (array / struct / message) literal; a closure
 * capture; or a container-insert builtin. Only a scalar *derived* from it may
 * escape — `return buf.checksum()` carries the call RESULT, not the binding,
 * so it is allowed. This is opt-in escape analysis, not a borrow checker:
 * one annotation makes the non-escape a checked invariant. */

/* Direct carry: does `node` hand `name`'s value out as-is — a bare reference,
 * or an element of an aggregate literal? A call ON `name` (`name.f()`,
 * `f(name)`) is NOT a direct carry; its result is a fresh value. */
static int scoped_carries(ASTNode* node, const char* name) {
    if (!node) return 0;
    if (node->type == AST_IDENTIFIER && node->value &&
        strcmp(node->value, name) == 0) return 1;
    if (node->type == AST_ARRAY_LITERAL || node->type == AST_STRUCT_LITERAL ||
        node->type == AST_MESSAGE_CONSTRUCTOR || node->type == AST_FIELD_INIT) {
        for (int i = 0; i < node->child_count; i++)
            if (scoped_carries(node->children[i], name)) return 1;
    }
    return 0;
}

/* Does the subtree reference `name` as an identifier (a closure-capture test)? */
static int scoped_mentions(ASTNode* node, const char* name) {
    if (!node) return 0;
    if (node->type == AST_IDENTIFIER && node->value &&
        strcmp(node->value, name) == 0) return 1;
    for (int i = 0; i < node->child_count; i++)
        if (scoped_mentions(node->children[i], name)) return 1;
    return 0;
}

/* Curated container-insert builtins: handing a @scoped value to one of these
 * stores it beyond the block. Same curation posture as codegen's
 * is_nonstoring_builtin allowlist. Names are the as-written qualified
 * spellings (the AST stores callees in dotted form). */
static int scoped_store_call(const char* fn) {
    if (!fn) return 0;
    static const char* sinks[] = {
        "list.add", "list.push", "list.set", "map.put", "map.set",
        "set.add", "seq.cons", NULL };
    for (int i = 0; sinks[i]; i++)
        if (strcmp(fn, sinks[i]) == 0) return 1;
    return 0;
}

/* Reason string if `node` escapes `name` AT THIS NODE, else NULL. */
static const char* scoped_escape_reason(ASTNode* node, const char* name) {
    if (!node) return NULL;
    if (node->type == AST_RETURN_STATEMENT) {
        for (int i = 0; i < node->child_count; i++)
            if (scoped_carries(node->children[i], name))
                return "it is returned from the function";
    }
    if ((node->type == AST_ASSIGNMENT ||
         (node->type == AST_BINARY_EXPRESSION && node->value &&
          strcmp(node->value, "=") == 0)) && node->child_count >= 2) {
        ASTNode* lhs = node->children[0];
        int lhs_is_self = lhs && lhs->type == AST_IDENTIFIER && lhs->value &&
                          strcmp(lhs->value, name) == 0;
        if (!lhs_is_self && scoped_carries(node->children[1], name))
            return "it is stored into another binding or field";
    }
    if (node->type == AST_VARIABLE_DECLARATION) {
        int decl_is_self = node->value && strcmp(node->value, name) == 0;
        if (!decl_is_self)
            for (int i = 0; i < node->child_count; i++)
                if (scoped_carries(node->children[i], name))
                    return "it is aliased into another binding";
    }
    if (node->type == AST_CLOSURE && scoped_mentions(node, name))
        return "it is captured by a closure that can outlive the block";
    if (node->type == AST_FUNCTION_CALL && node->value &&
        scoped_store_call(node->value)) {
        int first = strcmp(node->value, "call") == 0 ? 1 : 0;
        for (int i = first; i < node->child_count; i++) {
            ASTNode* a = node->children[i];
            if (a && a->type == AST_IDENTIFIER && a->value &&
                strcmp(a->value, name) == 0)
                return "it is inserted into a container";
        }
    }
    return NULL;
}

/* Walk `node`, reporting the first escape of `name`. Does not descend into a
 * closure (handled at the capture site) or a nested function/actor scope. */
static int scoped_check_subtree(ASTNode* node, const char* name) {
    if (!node) return 0;
    const char* reason = scoped_escape_reason(node, name);
    if (reason) {
        char msg[320];
        snprintf(msg, sizeof(msg),
            "`@scoped` binding '%s' escapes its block: %s. A @scoped value "
            "must not outlive the block that introduced it, return a scalar "
            "derived from it (e.g. `%s.len()`), or drop the `@scoped`.",
            name, reason, name);
        type_error(msg, node->line, node->column);
        return 1;
    }
    if (node->type == AST_CLOSURE || node->type == AST_FUNCTION_DEFINITION ||
        node->type == AST_BUILDER_FUNCTION || node->type == AST_ACTOR_DEFINITION)
        return 0;
    int found = 0;
    for (int i = 0; i < node->child_count; i++)
        found |= scoped_check_subtree(node->children[i], name);
    return found;
}

/* For each `@scoped` binding declared in `body`, reject any escape. */
static void scoped_check_bindings(ASTNode* node, ASTNode* root) {
    if (!node) return;
    if (node->type == AST_VARIABLE_DECLARATION && node->value &&
        node->annotation && strstr(node->annotation, "scoped")) {
        scoped_check_subtree(root, node->value);
    }
    if (node->type == AST_FUNCTION_DEFINITION ||
        node->type == AST_BUILDER_FUNCTION ||
        node->type == AST_ACTOR_DEFINITION) return;
    for (int i = 0; i < node->child_count; i++)
        scoped_check_bindings(node->children[i], root);
}

/* ---- #479 Isolated[T] move (linearity) analysis -------------------------
 *
 * An Isolated[T] value is move-only: every use consumes it, so using it twice
 * (or after send/consume) is a compile error. isolate(x) also consumes its
 * source local x. This is a forward pass over a function body: `moved` is the
 * set of names consumed on the current path; `iso` is the set of names known
 * to be Isolated-typed (a bare reference to one is itself a consuming move).
 * Control flow is handled soundly: if/else analyzes each branch on a copy of
 * the incoming set and joins by union (skipping a branch that diverges via
 * return/break/continue); a loop body is analyzed twice so a consume that
 * would repeat across iterations is caught, while one rebound each iteration
 * is not. It descends into nested scopes' own analysis, not the parent's. */
#define ISO_SET_MAX 128
typedef struct { const char* names[ISO_SET_MAX]; int count; } IsoSet;

static int iso_has(const IsoSet* s, const char* n) {
    if (!n) return 0;
    for (int i = 0; i < s->count; i++)
        if (s->names[i] && strcmp(s->names[i], n) == 0) return 1;
    return 0;
}
static void iso_put(IsoSet* s, const char* n) {
    if (!n || iso_has(s, n)) return;
    if (s->count < ISO_SET_MAX) s->names[s->count++] = n;
}
static void iso_del(IsoSet* s, const char* n) {
    if (!n) return;
    for (int i = 0; i < s->count; i++)
        if (s->names[i] && strcmp(s->names[i], n) == 0) {
            s->names[i] = s->names[--s->count];
            return;
        }
}
static void iso_merge(IsoSet* dst, const IsoSet* src) {
    for (int i = 0; i < src->count; i++) iso_put(dst, src->names[i]);
}

/* Is this declaration an Isolated binding (annotated type, or init = isolate())? */
static int iso_decl_is_isolated(ASTNode* decl) {
    if (decl->node_type && decl->node_type->kind == TYPE_ISOLATED) return 1;
    if (decl->child_count > 0 && decl->children[0] &&
        decl->children[0]->type == AST_FUNCTION_CALL &&
        decl->children[0]->value &&
        strcmp(decl->children[0]->value, "isolate") == 0) return 1;
    return 0;
}

/* Is a value of this type move-worthy, i.e. does isolate()ing it transfer an
 * ownership that the source must then relinquish? Heap / reference / aggregate
 * types are (a *StringSeq, struct, string, ptr, message...). Copyable scalars
 * (int/float/bool/byte/duration) are not: isolate(counter) must not kill the
 * counter, so the source-consume is skipped for them. Unknown/NULL types are
 * treated as not-move-worthy so an un-inferred source is never falsely moved. */
static int iso_type_is_moveworthy(Type* t) {
    if (!t) return 0;
    switch (t->kind) {
        case TYPE_STRING: case TYPE_PTR: case TYPE_STRUCT: case TYPE_ARRAY:
        case TYPE_MESSAGE: case TYPE_ACTOR_REF: case TYPE_TUPLE: case TYPE_SUM:
        case TYPE_OPTIONAL: case TYPE_ISOLATED: case TYPE_FUNCTION:
            return 1;
        default:
            return 0;
    }
}

/* Does this statement definitely transfer control (no fall-through)? */
static int iso_diverges(ASTNode* n) {
    if (!n) return 0;
    if (n->type == AST_RETURN_STATEMENT || n->type == AST_BREAK_STATEMENT ||
        n->type == AST_CONTINUE_STATEMENT) return 1;
    if (n->type == AST_BLOCK && n->child_count > 0)
        return iso_diverges(n->children[n->child_count - 1]);
    if (n->type == AST_IF_STATEMENT && n->child_count > 2)
        return iso_diverges(n->children[1]) && iso_diverges(n->children[2]);
    return 0;
}

static void iso_check(ASTNode* node, IsoSet* moved, IsoSet* iso) {
    if (!node) return;
    switch (node->type) {
        /* Nested scopes get their own analysis; don't leak this frame in. */
        case AST_FUNCTION_DEFINITION:
        case AST_BUILDER_FUNCTION:
        case AST_ACTOR_DEFINITION:
        case AST_CLOSURE:
            return;

        case AST_IDENTIFIER: {
            const char* nm = node->value;
            if (!nm) return;
            if (iso_has(moved, nm)) {
                char msg[320];
                snprintf(msg, sizeof(msg),
                    "use of moved value '%s': an Isolated[T] value is consumed "
                    "by its single use (isolate / consume / send) and cannot be "
                    "used again. Re-create it with isolate(...) to send another.",
                    nm);
                type_error(msg, node->line, node->column);
                return;
            }
            if (iso_has(iso, nm)) iso_put(moved, nm); /* this reference consumes it */
            return;
        }

        case AST_VARIABLE_DECLARATION: {
            for (int i = 0; i < node->child_count; i++)
                iso_check(node->children[i], moved, iso);   /* init (RHS) first */
            if (node->value) {
                iso_del(moved, node->value);                 /* fresh binding revives */
                if (iso_decl_is_isolated(node)) iso_put(iso, node->value);
                else iso_del(iso, node->value);
            }
            return;
        }

        case AST_ASSIGNMENT: {
            ASTNode* lhs = node->child_count > 0 ? node->children[0] : NULL;
            if (node->child_count > 1) iso_check(node->children[1], moved, iso); /* RHS */
            if (lhs && lhs->type == AST_IDENTIFIER && lhs->value) {
                iso_del(moved, lhs->value);   /* plain rebind revives the name */
            } else {
                iso_check(lhs, moved, iso);   /* member/index lhs: base is a read */
            }
            return;
        }

        case AST_IF_STATEMENT: {
            if (node->child_count > 0) iso_check(node->children[0], moved, iso); /* cond */
            IsoSet then_s = *moved;
            if (node->child_count > 1) iso_check(node->children[1], &then_s, iso);
            IsoSet else_s = *moved;
            if (node->child_count > 2) iso_check(node->children[2], &else_s, iso);
            int then_div = node->child_count > 1 && iso_diverges(node->children[1]);
            int else_div = node->child_count > 2 && iso_diverges(node->children[2]);
            if (then_div && else_div) return;   /* code after is unreachable */
            IsoSet joined; joined.count = 0;
            if (!then_div) iso_merge(&joined, &then_s);
            if (!else_div) iso_merge(&joined, &else_s);
            *moved = joined;
            return;
        }

        case AST_WHILE_LOOP:
        case AST_FOR_LOOP: {
            int body_idx = node->child_count - 1;
            for (int i = 0; i < body_idx; i++)          /* init/cond/incr once */
                iso_check(node->children[i], moved, iso);
            if (body_idx >= 0) {                          /* body twice */
                iso_check(node->children[body_idx], moved, iso);
                iso_check(node->children[body_idx], moved, iso);
            }
            return;
        }

        default:
            /* isolate(x): check the argument (a read), then consume the source
             * local so a later use of x is a use-after-move. */
            if (node->type == AST_FUNCTION_CALL && node->value &&
                strcmp(node->value, "isolate") == 0 && node->child_count == 1) {
                iso_check(node->children[0], moved, iso);
                ASTNode* a = node->children[0];
                if (a && a->type == AST_IDENTIFIER && a->value &&
                    iso_type_is_moveworthy(a->node_type))
                    iso_put(moved, a->value);
                return;
            }
            for (int i = 0; i < node->child_count; i++)
                iso_check(node->children[i], moved, iso);
            return;
    }
}

/* Entry point: register Isolated-typed parameters, then walk the body. */
static void iso_check_function(ASTNode* fn, ASTNode* body) {
    if (!body) return;
    IsoSet moved; moved.count = 0;
    IsoSet iso;   iso.count = 0;
    if (fn) {
        for (int i = 0; i < fn->child_count; i++) {
            ASTNode* p = fn->children[i];
            if (!p || p == body) { if (p == body) break; else continue; }
            if ((p->type == AST_VARIABLE_DECLARATION || p->type == AST_PATTERN_VARIABLE) &&
                p->value && p->node_type && p->node_type->kind == TYPE_ISOLATED)
                iso_put(&iso, p->value);
        }
    }
    iso_check(body, &moved, &iso);
}

/* #481 effect tags — capability of a call's NAMESPACE (the as-written prefix,
 * e.g. `file.read` → fs). std.fs re-exports the file/dir/path namespaces,
 * std.net the tcp/http namespaces, std.os the os namespace — mirrors the
 * `--with=` capability gate (aetherc.c inspect_module_capability), keyed on
 * the call site rather than the import path. NULL = capability-free (pure). */
static const char* effect_call_capability(const char* ns) {
    if (!ns) return NULL;
    if (!strcmp(ns, "fs") || !strcmp(ns, "file") || !strcmp(ns, "dir") ||
        !strcmp(ns, "path")) return "fs";
    if (!strcmp(ns, "net") || !strcmp(ns, "tcp") || !strcmp(ns, "http"))
        return "net";
    if (!strcmp(ns, "os")) return "os";
    return NULL;
}

static ASTNode* effect_find_func(ASTNode* program, const char* name) {
    if (!program || !name) return NULL;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if ((c->type == AST_FUNCTION_DEFINITION || c->type == AST_BUILDER_FUNCTION) &&
            c->value && strcmp(c->value, name) == 0)
            return c;
    }
    return NULL;
}

/* Walk `node`; return the first forbidden capability reached (recursing into
 * user-function callees), writing the offending `ns.fn` spelling to `outcall`.
 * `forbid` is the comma list of forbidden caps; `visited` guards recursion. */
static const char* effect_scan(ASTNode* program, ASTNode* node, const char* forbid,
                               const char** visited, int* nvisited,
                               char* outcall, size_t outsz, int depth) {
    if (!node || depth > 128) return NULL;
    if (node->type == AST_FUNCTION_CALL && node->value) {
        const char* dot = strchr(node->value, '.');
        if (dot) {
            char ns[64];
            size_t l = (size_t)(dot - node->value);
            if (l < sizeof(ns)) {
                memcpy(ns, node->value, l);
                ns[l] = '\0';
                const char* cap = effect_call_capability(ns);
                if (cap && strstr(forbid, cap)) {
                    snprintf(outcall, outsz, "%s", node->value);
                    return cap;
                }
            }
        } else {
            int seen = 0;
            for (int i = 0; i < *nvisited; i++)
                if (strcmp(visited[i], node->value) == 0) { seen = 1; break; }
            if (!seen && *nvisited < 512) {
                ASTNode* def = effect_find_func(program, node->value);
                if (def && def->child_count > 0) {
                    visited[(*nvisited)++] = node->value;
                    ASTNode* body = def->children[def->child_count - 1];
                    const char* c = effect_scan(program, body, forbid, visited,
                                                nvisited, outcall, outsz, depth + 1);
                    if (c) return c;
                }
            }
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        const char* c = effect_scan(program, node->children[i], forbid, visited,
                                    nvisited, outcall, outsz, depth);
        if (c) return c;
    }
    return NULL;
}

/* For each function carrying an `effect:<caps>` annotation, reject any
 * transitive use of a forbidden capability. A raw `extern` call is not
 * classifiable, so — like the `--with=` gate — it is not flagged here. */
static void check_effect_tags(ASTNode* program) {
    if (!program) return;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* fn = program->children[i];
        if ((fn->type != AST_FUNCTION_DEFINITION && fn->type != AST_BUILDER_FUNCTION) ||
            !fn->annotation) continue;
        const char* eff = strstr(fn->annotation, "effect:");
        if (!eff) continue;
        const char* forbid = eff + 7;  /* "fs,net,os" or a subset */
        if (fn->child_count == 0) continue;
        ASTNode* body = fn->children[fn->child_count - 1];
        const char* visited[512];
        int nv = 0;
        if (fn->value) visited[nv++] = fn->value;  /* no self-recursion loop */
        char offending[160];
        offending[0] = '\0';
        const char* cap = effect_scan(program, body, forbid, visited, &nv,
                                      offending, sizeof(offending), 0);
        if (cap) {
            char msg[360];
            snprintf(msg, sizeof(msg),
                "function '%s' is declared `@no_%s` (or `@pure`) but reaches a "
                "`%s` operation through `%s`. Remove that call path, or drop the "
                "effect tag.", fn->value ? fn->value : "?", cap, cap, offending);
            type_error(msg, fn->line, fn->column);
        }
    }
}

/* #522 purity inference. A function is PURE when it (transitively) reaches no
 * capability stdlib call (fs/net/os — time/exec live under os) and mutates no
 * caller-visible state: a parameter's pointee (`p.field = …`, `p[i] = …`) or a
 * module global. Conservative — a function the analysis can't see the body of
 * (extern, unresolved) is treated as impure; a raw `extern` call inside a body
 * is opaque and left unflagged (same boundary as the effect-tag / `--with=`
 * gates). The result is exposed by the compile-time `__pure(fn)` builtin. */

static int purity_is_param(ASTNode* fn, const char* name) {
    if (!fn || !name) return 0;
    for (int i = 0; i < fn->child_count - 1; i++) {  /* last child is the body */
        ASTNode* p = fn->children[i];
        if (p && (p->type == AST_VARIABLE_DECLARATION || p->type == AST_PATTERN_VARIABLE) &&
            p->value && strcmp(p->value, name) == 0) return 1;
    }
    return 0;
}

/* Root identifier of an lvalue: x, x.f, x[i], x.f[i].g → "x". */
static const char* purity_lvalue_root(ASTNode* n) {
    while (n) {
        if (n->type == AST_IDENTIFIER) return n->value;
        if ((n->type == AST_MEMBER_ACCESS || n->type == AST_ARRAY_ACCESS) &&
            n->child_count > 0) { n = n->children[0]; continue; }
        return NULL;
    }
    return NULL;
}

static int purity_scan(ASTNode* program, ASTNode* fn, ASTNode* node,
                       const char** globals, int nglobals,
                       const char** visited, int* nv, int depth) {
    if (!node || depth > 128) return 0;
    if (node->type == AST_FUNCTION_CALL && node->value) {
        const char* dot = strchr(node->value, '.');
        if (dot) {
            char ns[64];
            size_t l = (size_t)(dot - node->value);
            if (l < sizeof(ns)) {
                memcpy(ns, node->value, l);
                ns[l] = '\0';
                if (effect_call_capability(ns)) return 1;  /* fs/net/os → impure */
            }
        } else {
            int seen = 0;
            for (int i = 0; i < *nv; i++)
                if (strcmp(visited[i], node->value) == 0) { seen = 1; break; }
            if (!seen && *nv < 512) {
                ASTNode* def = effect_find_func(program, node->value);
                if (def && def->child_count > 0) {
                    visited[(*nv)++] = node->value;
                    if (purity_scan(program, def, def->children[def->child_count - 1],
                                    globals, nglobals, visited, nv, depth + 1)) return 1;
                }
            }
        }
    }
    if ((node->type == AST_ASSIGNMENT ||
         (node->type == AST_BINARY_EXPRESSION && node->value &&
          strcmp(node->value, "=") == 0)) && node->child_count >= 1) {
        ASTNode* lhs = node->children[0];
        const char* root = purity_lvalue_root(lhs);
        if (root) {
            int is_field = lhs && (lhs->type == AST_MEMBER_ACCESS ||
                                   lhs->type == AST_ARRAY_ACCESS);
            if (is_field && purity_is_param(fn, root)) return 1;  /* mutate a param's pointee */
            for (int g = 0; g < nglobals; g++)
                if (strcmp(globals[g], root) == 0) return 1;       /* write a module global */
        }
    }
    for (int i = 0; i < node->child_count; i++)
        if (purity_scan(program, fn, node->children[i], globals, nglobals, visited, nv, depth))
            return 1;
    return 0;
}

static int func_is_pure(ASTNode* program, const char* fnname,
                        const char** globals, int nglobals) {
    ASTNode* fn = effect_find_func(program, fnname);
    if (!fn || fn->child_count == 0) return 0;  /* unknown / extern → conservatively impure */
    const char* visited[512];
    int nv = 0;
    visited[nv++] = fnname;
    ASTNode* body = fn->children[fn->child_count - 1];
    return !purity_scan(program, fn, body, globals, nglobals, visited, &nv, 0);
}

/* #889: derived per-function effect reachability, exposed via --emit=effects.
 *
 * Accumulates, over a whole-program call-graph walk from one function body, the
 * set of capabilities (fs/net/os) transitively reached and whether the walk
 * reaches a raw `extern` (the fail-closed signal: an extern is unclassifiable,
 * so a consumer must treat it as reaching anything). Mirrors effect_scan's
 * walk, but COLLECTS all caps rather than stopping at the first forbidden one,
 * and additionally flags extern reachability. */
typedef struct {
    int reach_fs, reach_net, reach_os;  /* capability bits */
    int reach_extern;                   /* hit a raw extern callee */
} EffectSet;

static int effect_is_extern(ASTNode* program, const char* name) {
    if (!program || !name) return 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_EXTERN_FUNCTION && c->value &&
            strcmp(c->value, name) == 0)
            return 1;
    }
    return 0;
}

static void effect_collect(ASTNode* program, ASTNode* node, EffectSet* set,
                           const char** visited, int* nvisited, int depth) {
    if (!node || depth > 128) return;
    if (node->type == AST_FUNCTION_CALL && node->value) {
        const char* dot = strchr(node->value, '.');
        if (dot) {
            char ns[64];
            size_t l = (size_t)(dot - node->value);
            if (l < sizeof(ns)) {
                memcpy(ns, node->value, l);
                ns[l] = '\0';
                const char* cap = effect_call_capability(ns);
                if (cap) {
                    if (!strcmp(cap, "fs"))  set->reach_fs = 1;
                    else if (!strcmp(cap, "net")) set->reach_net = 1;
                    else if (!strcmp(cap, "os"))  set->reach_os = 1;
                } else {
                    /* Not a capability namespace — a call into an imported
                       user module. After module merge the callee lives under
                       its underscore-merged name (`osmod.do_exec` →
                       `osmod_do_exec`), so follow that edge to keep the
                       reachability whole-program across import boundaries. */
                    char merged[256];
                    snprintf(merged, sizeof(merged), "%.*s_%s",
                             (int)l, node->value, dot + 1);
                    int seen = 0;
                    for (int i = 0; i < *nvisited; i++)
                        if (strcmp(visited[i], merged) == 0) { seen = 1; break; }
                    if (!seen && *nvisited < 512) {
                        ASTNode* def = effect_find_func(program, merged);
                        if (def && def->child_count > 0) {
                            /* visited[] holds borrowed pointers; this name is
                               stack-local, so search by value above is fine,
                               but we must store a stable pointer. Use the
                               def's own name (lives in the AST). */
                            visited[(*nvisited)++] = def->value ? def->value : merged;
                            effect_collect(program, def->children[def->child_count - 1],
                                           set, visited, nvisited, depth + 1);
                        }
                    }
                }
            }
        } else {
            int seen = 0;
            for (int i = 0; i < *nvisited; i++)
                if (strcmp(visited[i], node->value) == 0) { seen = 1; break; }
            if (!seen && *nvisited < 512) {
                ASTNode* def = effect_find_func(program, node->value);
                if (def && def->child_count > 0) {
                    visited[(*nvisited)++] = node->value;
                    effect_collect(program, def->children[def->child_count - 1],
                                   set, visited, nvisited, depth + 1);
                } else if (effect_is_extern(program, node->value)) {
                    set->reach_extern = 1;  /* fail-closed: unclassifiable */
                }
            }
        }
    }
    for (int i = 0; i < node->child_count; i++)
        effect_collect(program, node->children[i], set, visited, nvisited, depth);
}

void emit_effects_json(FILE* out, ASTNode* program) {
    if (!out) return;
    /* Collect module globals once (for the purity test, same basis as #522:
       a global is a top-level CONST_DECLARATION annotated "global_var",
       possibly wrapped in an EXPORT_STATEMENT). */
    const char* globals[256];
    int nglobals = 0;
    if (program) {
        for (int i = 0; i < program->child_count && nglobals < 256; i++) {
            ASTNode* c = program->children[i];
            if (c && c->type == AST_EXPORT_STATEMENT && c->child_count > 0) c = c->children[0];
            if (c && c->type == AST_CONST_DECLARATION && c->value && c->annotation &&
                strcmp(c->annotation, "global_var") == 0)
                globals[nglobals++] = c->value;
        }
    }
    fprintf(out, "{\n");
    int first = 1;
    if (program) {
        for (int i = 0; i < program->child_count; i++) {
            ASTNode* fn = program->children[i];
            if (!fn || (fn->type != AST_FUNCTION_DEFINITION &&
                        fn->type != AST_BUILDER_FUNCTION) || !fn->value)
                continue;
            EffectSet set = {0, 0, 0, 0};
            const char* visited[512];
            int nv = 0;
            visited[nv++] = fn->value;
            if (fn->child_count > 0)
                effect_collect(program, fn->children[fn->child_count - 1],
                               &set, visited, &nv, 0);
            int rfs  = set.reach_fs  || set.reach_extern;
            int rnet = set.reach_net || set.reach_extern;
            int ros  = set.reach_os  || set.reach_extern;
            /* Pure iff: the #522 engine says so (covers caller-visible
               mutation), AND no capability/extern is reached. The second
               clause keeps `pure` consistent with `reaches`/`extern` even
               where func_is_pure's own walk has a cross-module blind spot
               that effect_collect resolves. */
            int pure = func_is_pure(program, fn->value, globals, nglobals) &&
                       !rfs && !rnet && !ros && !set.reach_extern;
            fprintf(out, "%s  \"%s\": { \"pure\": %s, \"extern\": %s, "
                         "\"reaches\": [",
                    first ? "" : ",\n", fn->value,
                    pure ? "true" : "false",
                    set.reach_extern ? "true" : "false");
            int rfirst = 1;
            if (rfs)  { fprintf(out, "%s\"fs\"",  rfirst ? "" : ", "); rfirst = 0; }
            if (rnet) { fprintf(out, "%s\"net\"", rfirst ? "" : ", "); rfirst = 0; }
            if (ros)  { fprintf(out, "%s\"os\"",  rfirst ? "" : ", "); rfirst = 0; }
            fprintf(out, "] }");
            first = 0;
        }
    }
    fprintf(out, "\n}\n");
}

/* #480 distinct types — resolution pass. The parser leaves a reference to a
 * distinct type as a TYPE_STRUCT{struct_name=Name} placeholder (the same shape
 * any named type gets). This pass collects every `type Name = distinct Base`
 * definition and rewrites those placeholders, anywhere in the AST, into the
 * distinct Type: machine `kind` from Base (so codegen/get_c_type emit Base's
 * C type — zero cost) plus `distinct_name = Name` (so the type system keeps it
 * nominally separate). */
#define AETHER_MAX_DISTINCT 256
typedef struct { const char* name; Type* base; } DistinctDef;

static void distinct_rewrite_type(Type* t, DistinctDef* defs, int ndefs, int depth) {
    if (!t || depth > 64) return;
    if (t->kind == TYPE_STRUCT && t->struct_name && !t->distinct_name) {
        for (int i = 0; i < ndefs; i++) {
            if (strcmp(t->struct_name, defs[i].name) == 0) {
                Type* base = defs[i].base;
                char* nm = strdup(t->struct_name);
                free(t->struct_name);
                t->struct_name = NULL;
                t->kind = base->kind;
                if (base->c_alias) t->c_alias = strdup(base->c_alias);
                if (base->element_type) t->element_type = clone_type(base->element_type);
                t->distinct_name = nm;
                break;
            }
        }
    }
    distinct_rewrite_type(t->element_type, defs, ndefs, depth + 1);
    distinct_rewrite_type(t->return_type, defs, ndefs, depth + 1);
    for (int i = 0; i < t->tuple_count; i++)
        if (t->tuple_types) distinct_rewrite_type(t->tuple_types[i], defs, ndefs, depth + 1);
    for (int i = 0; i < t->param_count; i++)
        if (t->param_types) distinct_rewrite_type(t->param_types[i], defs, ndefs, depth + 1);
}

static void distinct_rewrite_ast(ASTNode* n, DistinctDef* defs, int ndefs) {
    if (!n) return;
    if (n->node_type) distinct_rewrite_type(n->node_type, defs, ndefs, 0);
    for (int i = 0; i < n->child_count; i++)
        distinct_rewrite_ast(n->children[i], defs, ndefs);
}

static void resolve_distinct_types(ASTNode* program) {
    if (!program) return;
    DistinctDef defs[AETHER_MAX_DISTINCT];
    int ndefs = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_DISTINCT_TYPE_DEF && c->value && c->node_type &&
            ndefs < AETHER_MAX_DISTINCT) {
            defs[ndefs].name = c->value;
            defs[ndefs].base = c->node_type;
            ndefs++;
        }
    }
    if (ndefs == 0) return;
    distinct_rewrite_ast(program, defs, ndefs);
}

// #914 sum/variant types. `type Name = A | B | C` parses to AST_SUM_TYPE_DEF
// (value = Name, each child an AST_IDENTIFIER naming a variant struct). A use
// site `s: Name` is parsed by parse_type as a bare TYPE_STRUCT{Name}; this
// pass rewrites every such reference to the real TYPE_SUM (carrying the
// variant Types in tuple_types[]), exactly as resolve_distinct_types does for
// distinct aliases.
#define AETHER_MAX_SUMS 256
typedef struct { const char* name; ASTNode* def; } SumDef;

// Convert an in-place TYPE_STRUCT{sumName} into the TYPE_SUM, filling its
// variant Types (each a TYPE_STRUCT{variantName}) from the def's children.
static void sum_apply(Type* t, ASTNode* def) {
    t->kind = TYPE_SUM;                 // struct_name already holds the sum name
    int nv = 0;
    for (int i = 0; i < def->child_count; i++)
        if (def->children[i] && def->children[i]->type == AST_IDENTIFIER) nv++;
    t->tuple_types = nv ? (Type**)malloc(nv * sizeof(Type*)) : NULL;
    t->tuple_count = 0;
    for (int i = 0; i < def->child_count; i++) {
        ASTNode* v = def->children[i];
        if (v && v->type == AST_IDENTIFIER && v->value) {
            Type* vt = create_type(TYPE_STRUCT);
            vt->struct_name = strdup(v->value);
            t->tuple_types[t->tuple_count++] = vt;
        }
    }
}

static void sum_rewrite_type(Type* t, SumDef* defs, int ndefs, int depth) {
    if (!t || depth > 64) return;
    if (t->kind == TYPE_STRUCT && t->struct_name && !t->distinct_name) {
        for (int i = 0; i < ndefs; i++) {
            if (strcmp(t->struct_name, defs[i].name) == 0) {
                sum_apply(t, defs[i].def);
                break;
            }
        }
    }
    sum_rewrite_type(t->element_type, defs, ndefs, depth + 1);
    sum_rewrite_type(t->return_type, defs, ndefs, depth + 1);
    for (int i = 0; i < t->tuple_count; i++)
        if (t->tuple_types) sum_rewrite_type(t->tuple_types[i], defs, ndefs, depth + 1);
    for (int i = 0; i < t->param_count; i++)
        if (t->param_types) sum_rewrite_type(t->param_types[i], defs, ndefs, depth + 1);
}

static void sum_rewrite_ast(ASTNode* n, SumDef* defs, int ndefs) {
    if (!n) return;
    if (n->node_type) sum_rewrite_type(n->node_type, defs, ndefs, 0);
    for (int i = 0; i < n->child_count; i++)
        sum_rewrite_ast(n->children[i], defs, ndefs);
}

static void resolve_sum_types(ASTNode* program) {
    if (!program) return;
    SumDef defs[AETHER_MAX_SUMS];
    int ndefs = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_SUM_TYPE_DEF && c->value && ndefs < AETHER_MAX_SUMS) {
            defs[ndefs].name = c->value;
            defs[ndefs].def = c;
            ndefs++;
        }
    }
    if (ndefs == 0) return;
    sum_rewrite_ast(program, defs, ndefs);
}

// #1044: rewrite bare `TYPE_STRUCT{EnumName}` type annotations into TYPE_ENUM
// (mirrors the distinct/sum resolvers), and populate the enum-name registry.
static void enum_rewrite_type(Type* t, const char** names, int n, int depth) {
    if (!t || depth > 64) return;
    if (t->kind == TYPE_STRUCT && t->struct_name && !t->distinct_name) {
        for (int i = 0; i < n; i++)
            if (strcmp(t->struct_name, names[i]) == 0) { t->kind = TYPE_ENUM; break; }
    }
    // #1044 enum-indexed array `[E]T`: now that the enum is known, size the
    // array to its member range (the parser left array_size = -1).
    if (t->kind == TYPE_ARRAY && t->index_enum_name && t->array_size < 0)
        t->array_size = enum_slot_count(t->index_enum_name);
    enum_rewrite_type(t->element_type, names, n, depth + 1);
    enum_rewrite_type(t->return_type, names, n, depth + 1);
    for (int i = 0; i < t->tuple_count; i++)
        if (t->tuple_types) enum_rewrite_type(t->tuple_types[i], names, n, depth + 1);
    for (int i = 0; i < t->param_count; i++)
        if (t->param_types) enum_rewrite_type(t->param_types[i], names, n, depth + 1);
}

static void enum_rewrite_ast(ASTNode* nd, const char** names, int n) {
    if (!nd) return;
    if (nd->node_type) enum_rewrite_type(nd->node_type, names, n, 0);
    for (int i = 0; i < nd->child_count; i++)
        enum_rewrite_ast(nd->children[i], names, n);
}

// #1044: rewrite `EnumName.Member` member-access nodes into a plain identifier
// naming the C enum constant `EnumName_Member`, with the enum's type. Done up
// front (in the resolve pass) so the identifier is in place before the
// undefined-variable / member-access checks run. An unknown member is a
// compile error here.
static void enum_rewrite_member_access(ASTNode* nd) {
    if (!nd) return;
    if (nd->type == AST_MEMBER_ACCESS && nd->child_count > 0 && nd->children[0] &&
        nd->children[0]->type == AST_IDENTIFIER && nd->children[0]->value &&
        nd->value && is_enum_type_name(nd->children[0]->value)) {
        const char* ename = nd->children[0]->value;
        if (!enum_has_member(ename, nd->value)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "'%s' is not a member of enum '%s'",
                     nd->value, ename);
            type_error(msg, nd->line, nd->column);
        } else {
            char qualified[512];
            snprintf(qualified, sizeof(qualified), "%s_%s", ename, nd->value);
            Type* et = create_type(TYPE_ENUM);
            et->struct_name = strdup(ename);
            free(nd->value);
            nd->value = strdup(qualified);
            /* Drop the base-identifier child; the node is now a leaf identifier. */
            free_ast_node(nd->children[0]);
            free(nd->children);
            nd->children = NULL;
    nd->child_capacity = 0;
            nd->child_count = 0;
            nd->type = AST_IDENTIFIER;
            if (nd->node_type) free_type(nd->node_type);
            nd->node_type = et;
            return;   /* leaf now; nothing below to rewrite */
        }
    }
    for (int i = 0; i < nd->child_count; i++)
        enum_rewrite_member_access(nd->children[i]);
}

static void resolve_enum_types(ASTNode* program) {
    g_enum_name_count = 0;
    g_enum_program = program;
    g_constfold_program = program;
    if (!program) return;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_ENUM_DEFINITION && c->value &&
            g_enum_name_count < AETHER_MAX_ENUMS)
            g_enum_names[g_enum_name_count++] = c->value;
    }
    if (g_enum_name_count == 0) return;
    enum_rewrite_ast(program, g_enum_names, g_enum_name_count);        /* type annotations */
    enum_rewrite_member_access(program);                              /* EnumName.Member */
}

/* #1132 — rewrite `TYPE_STRUCT{B}` into `TYPE_BITSTRUCT{B}` for every declared
 * bitstruct B, carrying the backing integer along as element_type so codegen can
 * emit the storage without another lookup. Mirrors enum_rewrite_type: the parser
 * cannot tell a bitstruct name from a struct name (an unknown identifier in type
 * position always becomes TYPE_STRUCT), so the fix-up happens here. */
static void bitstruct_rewrite_type(Type* t, ASTNode* program, int depth) {
    if (!t || depth > 64) return;
    if (t->kind == TYPE_STRUCT && t->struct_name && !t->distinct_name) {
        ASTNode* def = bitstruct_def_lookup(program, t->struct_name);
        if (def) {
            t->kind = TYPE_BITSTRUCT;
            if (!t->element_type && def->node_type)
                t->element_type = clone_type(def->node_type);
        }
    }
    bitstruct_rewrite_type(t->element_type, program, depth + 1);
    bitstruct_rewrite_type(t->return_type, program, depth + 1);
    for (int i = 0; i < t->tuple_count; i++)
        if (t->tuple_types) bitstruct_rewrite_type(t->tuple_types[i], program, depth + 1);
    for (int i = 0; i < t->param_count; i++)
        if (t->param_types) bitstruct_rewrite_type(t->param_types[i], program, depth + 1);
}

static void bitstruct_rewrite_ast(ASTNode* nd, ASTNode* program) {
    if (!nd) return;
    /* Don't rewrite the definition's own backing-type annotation — it is the
     * uint8_t/uint16_t/... storage type, not a reference to the bitstruct. */
    if (nd->type != AST_BITSTRUCT_DEFINITION && nd->node_type)
        bitstruct_rewrite_type(nd->node_type, program, 0);
    for (int i = 0; i < nd->child_count; i++)
        bitstruct_rewrite_ast(nd->children[i], program);
}

static void resolve_bitstruct_types(ASTNode* program) {
    if (!program) return;
    g_bitstruct_program = program;
    int any = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_BITSTRUCT_DEFINITION) { any = 1; break; }
    }
    if (!any) return;
    bitstruct_rewrite_ast(program, program);
}

/* Fold every `__pure(fn)` node in the tree to a `true`/`false` bool literal. */
static void resolve_purity_queries(ASTNode* node, ASTNode* program,
                                   const char** globals, int nglobals) {
    if (!node) return;
    if (node->type == AST_PURITY_QUERY && node->value) {
        int pure = func_is_pure(program, node->value, globals, nglobals);
        node->type = AST_LITERAL;
        free(node->value);
        node->value = strdup(pure ? "true" : "false");
        if (node->node_type) free_type(node->node_type);
        node->node_type = create_type(TYPE_BOOL);
        return;
    }
    for (int i = 0; i < node->child_count; i++)
        resolve_purity_queries(node->children[i], program, globals, nglobals);
}

int typecheck_function_definition(ASTNode* func, SymbolTable* table) {
    if (!func || (func->type != AST_FUNCTION_DEFINITION && func->type != AST_BUILDER_FUNCTION)) return 0;

    SymbolTable* func_table = create_symbol_table(table);

    // Issue #243 sealed scopes: cloned function bodies from
    // module_merge_into_program's BFS transitive-merge pass need
    // relaxed qualified-call resolution so they can reach into other
    // transitively-merged namespaces. The flag propagates from
    // parent in create_symbol_table, so nested scopes inside this
    // body inherit it; on function exit we just free func_table.
    if (func->is_imported) {
        func_table->inside_merged_body = 1;
    }

    // Add parameters to function's symbol table
    for (int i = 0; i < func->child_count - 1; i++) { // Last child is body
        ASTNode* param = func->children[i];
        if (param->type == AST_VARIABLE_DECLARATION || param->type == AST_PATTERN_VARIABLE) {
            Type* param_type = param->node_type ? clone_type(param->node_type) : create_type(TYPE_UNKNOWN);
            add_symbol(func_table, param->value, param_type, 0, 0, 0);
        }
    }

    // Builder functions get implicit _builder: ptr parameter
    if (func->type == AST_BUILDER_FUNCTION) {
        add_symbol(func_table, "_builder", create_type(TYPE_PTR), 0, 0, 0);
    }

    // Type check function body. Track the return type so `return Blue` can
    // resolve a bare enum member (save/restore keeps nested functions correct).
    ASTNode* body = func->children[func->child_count - 1];
    Type* saved_ret = g_tc_return_type;
    g_tc_return_type = func->node_type;
    typecheck_statement(body, func_table);
    g_tc_return_type = saved_ret;

    /* #1140: `defer try` / `defer catch` only mean something in a function that
     * can actually fail. In a function with no error channel a `defer catch`
     * would never fire and a `defer try` is just a plain `defer` — either way
     * the author has written something that does not do what it says, so say so
     * rather than silently compiling it. */
    warn_conditional_defers_in_infallible(func, func->node_type);

    /* Contract folding, definition-site tier: reject clauses that are
     * decidably false with no arguments substituted. */
    contract_check_clauses_at_definition(func);

    free_symbol_table(func_table);
    return 1;
}

int typecheck_struct_definition(ASTNode* struct_def, SymbolTable* table) {
    (void)table;  // Unused for now
    if (!struct_def || struct_def->type != AST_STRUCT_DEFINITION) return 0;
    
    // Type check all fields
    for (int i = 0; i < struct_def->child_count; i++) {
        ASTNode* field = struct_def->children[i];

        // Compound fields (union { ... } / nested struct { ... }) skip the
        // leaf-type-required check — they carry no `node_type`; their
        // children are independently typed (and walked at access time
        // via base_type->compound_node).
        if (field->type == AST_STRUCT_FIELD_UNION ||
            field->type == AST_STRUCT_FIELD_NESTED) {
            continue;
        }

        if (field->type != AST_STRUCT_FIELD) {
            type_error("Invalid struct field", field->line, field->column);
            return 0;
        }

        // Verify field type is valid
        if (!field->node_type) {
            char error_msg[256];
            snprintf(error_msg, sizeof(error_msg),
                    "Struct field '%s' has no type", field->value);
            type_error(error_msg, field->line, field->column);
            return 0;
        }
        
        // Check for duplicate field names
        for (int j = 0; j < i; j++) {
            ASTNode* other_field = struct_def->children[j];
            if (strcmp(field->value, other_field->value) == 0) {
                char error_msg[256];
                snprintf(error_msg, sizeof(error_msg), 
                        "Duplicate field name '%s' in struct '%s'", 
                        field->value, struct_def->value);
                type_error(error_msg, field->line, field->column);
                return 0;
            }
        }
    }
    
    return 1;
}

/* #893: enclosing loop labels, for validating `break label` / `continue
 * label`. Pushed (one slot per loop, NULL for an unlabeled loop) while a
 * loop body is type-checked. Type-checking is single-threaded, so a file-
 * static stack is sufficient. */
#define TC_MAX_LOOP_LABELS 256
static const char* tc_loop_labels[TC_MAX_LOOP_LABELS];
static int tc_loop_label_depth = 0;

static void tc_push_loop_label(const char* label) {
    if (tc_loop_label_depth < TC_MAX_LOOP_LABELS) {
        tc_loop_labels[tc_loop_label_depth] = label;
    }
    tc_loop_label_depth++;   /* always increment so pop stays balanced */
}
static void tc_pop_loop_label(void) {
    if (tc_loop_label_depth > 0) tc_loop_label_depth--;
}
static int tc_loop_label_in_scope(const char* label) {
    if (!label) return 0;
    int n = tc_loop_label_depth < TC_MAX_LOOP_LABELS
          ? tc_loop_label_depth : TC_MAX_LOOP_LABELS;
    for (int i = 0; i < n; i++) {
        if (tc_loop_labels[i] && strcmp(tc_loop_labels[i], label) == 0) return 1;
    }
    return 0;
}

// #1068 flow-sensitive optional narrowing.
//
// If a condition is a none-check on an optional variable, the guarded branch can
// treat that variable as its inner type `T` (used without the `!` force-unwrap)
// and the runtime none-check is elided (presence is proven by the guard). This
// is a pure compile-time analysis with zero runtime cost.
//
// `optional_narrow_target` recognizes `x != none` / `none != x` (narrow the THEN
// branch) and `x == none` / `none == x` (narrow the ELSE branch). It returns the
// optional variable's identifier node (borrowed) and sets *narrow_then; else
// NULL. The condition is already typechecked, so the identifier carries its
// TYPE_OPTIONAL node_type.
static ASTNode* optional_narrow_target(ASTNode* cond, int* narrow_then) {
    if (!cond || cond->type != AST_BINARY_EXPRESSION || !cond->value ||
        cond->child_count < 2) return NULL;
    int is_ne = strcmp(cond->value, "!=") == 0;
    int is_eq = strcmp(cond->value, "==") == 0;
    if (!is_ne && !is_eq) return NULL;
    ASTNode* a = cond->children[0];
    ASTNode* b = cond->children[1];
    ASTNode* var = NULL;
    if (a && a->type == AST_IDENTIFIER && b && b->type == AST_NONE_LITERAL) var = a;
    else if (b && b->type == AST_IDENTIFIER && a && a->type == AST_NONE_LITERAL) var = b;
    if (!var || !var->value) return NULL;
    if (!var->node_type || var->node_type->kind != TYPE_OPTIONAL ||
        !var->node_type->element_type ||
        var->node_type->element_type->kind == TYPE_UNKNOWN) return NULL;
    *narrow_then = is_ne ? 1 : 0;   // `!= none` -> then present; `== none` -> else
    return var;
}

// Would narrowing `name` to its inner type be UNSAFE anywhere in subtree `n`?
// Narrowing changes `name` from `T?` to `T` inside the branch, so it must be
// refused (branch typechecked unchanged) when the branch:
//   - rebinds `name` (assignment / re-declaration), it could become `none`;
//   - uses `name` in a none-comparison (`name == none` / `name != none`);
//   - uses `name` as the operand of an optional-only operator: `!` (unwrap),
//     `??` (null-coalesce), or `?.` (optional chain).
// Each of those needs the optional form of `name`, which narrowing removes.
// Conservative but sound (and the innermost none-check still narrows its own
// block via recursion). Does not descend into nested function/actor bodies.
static int narrow_unsafe(ASTNode* n, const char* name) {
    if (!n || !name) return 0;
    if (n->type == AST_FUNCTION_DEFINITION || n->type == AST_ACTOR_DEFINITION) return 0;
    // Rebind.
    if ((n->type == AST_ASSIGNMENT || n->type == AST_VARIABLE_DECLARATION) &&
        n->value && strcmp(n->value, name) == 0) return 1;
    // `name` as the operand of an optional-only operator (`!` / `??` / `?.`).
    if ((n->type == AST_TUPLE_UNWRAP || n->type == AST_NULL_COALESCE ||
         n->type == AST_OPTIONAL_CHAIN) && n->child_count >= 1) {
        ASTNode* c = n->children[0];
        if (c && c->type == AST_IDENTIFIER && c->value && strcmp(c->value, name) == 0)
            return 1;
    }
    // `name == none` / `name != none` (either operand order).
    if (n->type == AST_BINARY_EXPRESSION && n->value &&
        (strcmp(n->value, "==") == 0 || strcmp(n->value, "!=") == 0) &&
        n->child_count >= 2) {
        ASTNode* a = n->children[0], *b = n->children[1];
        int a_var = a && a->type == AST_IDENTIFIER && a->value && strcmp(a->value, name) == 0;
        int b_var = b && b->type == AST_IDENTIFIER && b->value && strcmp(b->value, name) == 0;
        int a_none = a && a->type == AST_NONE_LITERAL;
        int b_none = b && b->type == AST_NONE_LITERAL;
        if ((a_var && b_none) || (b_var && a_none)) return 1;
    }
    for (int i = 0; i < n->child_count; i++)
        if (narrow_unsafe(n->children[i], name)) return 1;
    return 0;
}

// Mark every read of `name` in `n` as a narrowed-optional access, so codegen
// emits `name.val` (the present inner value) rather than the `ae_opt` struct.
// Only reached when `name` is not rebound in the branch, so every occurrence is
// a read of the proven-present optional. Does not cross a nested function/actor
// boundary.
static void mark_optional_narrowed(ASTNode* n, const char* name, Type* inner) {
    if (!n || !name) return;
    if (n->type == AST_FUNCTION_DEFINITION || n->type == AST_ACTOR_DEFINITION) return;
    if (n->type == AST_IDENTIFIER && n->value && strcmp(n->value, name) == 0) {
        if (n->annotation) free(n->annotation);
        n->annotation = strdup("__opt_narrowed");
        // Pin the node's type to the inner T so any pass that reads node_type
        // (e.g. the var-declaration type-join for `y = x`) sees the narrowed
        // type, not the optional. Codegen uses the annotation to emit `x.val`.
        if (inner) {
            if (n->node_type) free_type(n->node_type);
            n->node_type = clone_type(inner);
        }
    }
    for (int i = 0; i < n->child_count; i++)
        mark_optional_narrowed(n->children[i], name, inner);
}

/* #1377 match-arm reachability.
 *
 * Arms are tried in source order, so an arm that can never be reached is dead
 * code the compiler accepted silently: a mis-ordered `_`, or two arms that were
 * meant to differ and collapsed to the same case through a typo.
 *
 * Reported only where the shadowing is certain. A bare identifier arm can be a
 * binding that matches anything rather than a named case, so those are never
 * compared; a warning on correct code would be worse than none. */
static int match_arm_is_wildcard(ASTNode* p) {
    if (!p) return 0;
    if (p->node_type && p->node_type->kind == TYPE_WILDCARD) return 1;
    return p->value && strcmp(p->value, "_") == 0;
}

/* Writes a comparable key for the pattern shapes whose identity is
 * unambiguous. Returns 0 for everything else, which is then left alone. */
static int match_arm_key(ASTNode* p, char* out, size_t cap) {
    if (!p || match_arm_is_wildcard(p)) return 0;
    if (p->type == AST_LITERAL && p->value) {
        snprintf(out, cap, "lit:%s", p->value);
        return 1;
    }
    if (p->type == AST_MEMBER_ACCESS && p->value &&
        p->child_count > 0 && p->children[0] && p->children[0]->value) {
        snprintf(out, cap, "mem:%s.%s", p->children[0]->value, p->value);
        return 1;
    }
    return 0;
}

static void warn_unreachable_arm(ASTNode* pattern, const char* msg,
                                 const char* suggestion) {
    AetherError w = {
        .filename = NULL, .source_code = NULL,
        .line = pattern->line, .column = pattern->column,
        .message = msg, .suggestion = suggestion,
        .context = NULL, .code = AETHER_WARN_UNREACHABLE_ARM
    };
    aether_warning_report(&w);
    warning_count++;
}

static void check_match_arm_reachability(ASTNode* stmt) {
    if (!stmt) return;
    enum { MATCH_KEYS_MAX = 64 };
    char keys[MATCH_KEYS_MAX][160];
    int  key_line[MATCH_KEYS_MAX];
    int  nkeys = 0;
    int  wildcard_line = -1;

    for (int i = 1; i < stmt->child_count; i++) {
        ASTNode* arm = stmt->children[i];
        if (!arm || arm->type != AST_MATCH_ARM || arm->child_count < 1) continue;
        ASTNode* pattern = arm->children[0];
        if (!pattern) continue;

        if (wildcard_line >= 0) {
            char msg[240];
            snprintf(msg, sizeof(msg),
                     "unreachable match arm: the `_` arm on line %d already "
                     "matches every remaining case", wildcard_line);
            warn_unreachable_arm(pattern, msg,
                                 "move this arm above the `_`, or remove it");
            continue;
        }
        if (match_arm_is_wildcard(pattern)) {
            wildcard_line = pattern->line;
            continue;
        }

        char key[160];
        if (!match_arm_key(pattern, key, sizeof(key))) continue;
        int dup_line = -1;
        for (int k = 0; k < nkeys; k++) {
            if (strcmp(keys[k], key) == 0) { dup_line = key_line[k]; break; }
        }
        if (dup_line >= 0) {
            char msg[240];
            snprintf(msg, sizeof(msg),
                     "unreachable match arm: this case is already handled on "
                     "line %d", dup_line);
            warn_unreachable_arm(pattern, msg,
                                 "remove the duplicate, or change it to the case you meant");
        } else if (nkeys < MATCH_KEYS_MAX) {
            snprintf(keys[nkeys], sizeof(keys[nkeys]), "%s", key);
            key_line[nkeys] = pattern->line;
            nkeys++;
        }
    }
}

/* #1778 diagnostic helper: is `rhs` a call to a function that has a BARE `fn`
 * parameter (a TYPE_FUNCTION with no return-type signature)? That is the shape
 * that erases a tuple return — `via_fn(f: fn, ...) -> { return f(...) }` types
 * the inner call `void` because a bare `fn` carries no signature, so `via_fn`'s
 * inferred return is void and the destructure at the call site fails. Used only
 * to steer the error message toward the signatured-`fn` fix; conservative (a
 * false negative just yields the generic hint, never a wrong one). */
static int rhs_returns_void_via_bare_fn(ASTNode* rhs, SymbolTable* table) {
    if (!rhs || rhs->type != AST_FUNCTION_CALL || !rhs->value) return 0;
    Symbol* callee = lookup_symbol(table, rhs->value);
    if (!callee || !callee->is_function || !callee->node) return 0;
    ASTNode* def = callee->node;
    /* Params are the def's children except the trailing body; a bare `fn` is a
     * param whose type is TYPE_FUNCTION with no populated return_type. */
    for (int i = 0; i < def->child_count - 1; i++) {
        ASTNode* p = def->children[i];
        if (p && p->node_type && p->node_type->kind == TYPE_FUNCTION &&
            p->node_type->return_type == NULL) {
            return 1;
        }
    }
    return 0;
}

int typecheck_statement(ASTNode* stmt, SymbolTable* table) {
    if (!stmt) return 0;

    switch (stmt->type) {
        case AST_TUPLE_DESTRUCTURE: {
            // a, b = func() — last child is RHS, others are variable declarations
            if (stmt->child_count < 2) {
                type_error("Invalid tuple destructuring", stmt->line, stmt->column);
                return 0;
            }
            int var_count = stmt->child_count - 1;
            ASTNode* rhs = stmt->children[var_count];  // Last child is RHS

            // Typecheck the RHS
            typecheck_expression(rhs, table);
            Type* rhs_type = infer_type(rhs, table);

            // Verify RHS is a tuple with matching element count
            if (!rhs_type || rhs_type->kind != TYPE_TUPLE) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "cannot destructure, '%s' returns '%s', not a tuple",
                    rhs->value ? rhs->value : "expression",
                    type_to_string(rhs_type));
                /* #1778: a tuple-returning function passed through a BARE `fn`
                 * param loses its return type — a bare `fn` carries no
                 * signature, so a call through it is typed `void`, and the
                 * failure surfaces here at the destructure rather than at the
                 * fn boundary that erased it. Detect that shape (the callee's
                 * return came from calling a bare-fn parameter) and point at the
                 * real fix: give the `fn` a signature. */
                const char* hint =
                    "use single assignment instead, or ensure the function "
                    "returns multiple values";
                if (rhs_returns_void_via_bare_fn(rhs, table)) {
                    hint =
                        "a value passed through a bare `fn` parameter loses its "
                        "return type (a bare `fn` has no signature), so the call "
                        "is typed `void`. Give the parameter a signature so the "
                        "tuple survives, e.g. `f: fn(int, int) -> (int, string)`.";
                }
                aether_error_with_suggestion(msg, stmt->line, stmt->column, hint);
                error_count++;
                /* Still bind the target names (with an unknown/void type) so a
                 * later use of `y`/`e2` does not cascade into a misleading
                 * "Undefined variable" reported at a bogus location (#1778's
                 * secondary observation: those follow-ons pointed at line 1). */
                for (int j = 0; j < var_count; j++) {
                    ASTNode* var = stmt->children[j];
                    if (var->value && strcmp(var->value, "_") != 0) {
                        add_symbol(table, var->value,
                                   rhs_type ? clone_type(rhs_type)
                                            : create_type(TYPE_VOID),
                                   0, 0, 0);
                    }
                }
                if (rhs_type) free_type(rhs_type);
                return 0;
            }

            if (rhs_type->tuple_count != var_count) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "tuple destructuring count mismatch, %d variables, but expression returns %d values",
                    var_count, rhs_type->tuple_count);
                aether_error_with_suggestion(msg, stmt->line, stmt->column,
                    "match the number of variables to the number of returned values");
                error_count++;
                free_type(rhs_type);
                return 0;
            }

            // Assign types to each variable and add to symbol table
            for (int j = 0; j < var_count; j++) {
                ASTNode* var = stmt->children[j];
                if (var->node_type) free_type(var->node_type);
                var->node_type = clone_type(rhs_type->tuple_types[j]);
                // Don't register _ (discard) in symbol table
                if (var->value && strcmp(var->value, "_") != 0) {
                    add_symbol(table, var->value, clone_type(var->node_type), 0, 0, 0);
                }
            }

            free_type(rhs_type);
            return 1;
        }

        case AST_CONST_DECLARATION:
        case AST_VARIABLE_DECLARATION: {
            // Forbid declaring a name that's been hidden in this scope —
            // shadowing a hidden binding would re-introduce position-sensitive
            // semantics. If you want a fresh name, pick a different one.
            if (stmt->value && scope_name_is_hidden(table, stmt->value)) {
                char error_msg[256];
                snprintf(error_msg, sizeof(error_msg),
                         "cannot declare '%s', it is hidden in this scope by `hide`",
                         stmt->value);
                type_error(error_msg, stmt->line, stmt->column);
                return 0;
            }
            if (stmt->child_count > 0) {
                // Has initializer
                ASTNode* init = stmt->children[0];

                /* Reading the value of a call that yields none. The callee
                 * lowers to C `void`, so this reads the return register:
                 * stack residue, different every run, and silent today. See
                 * call_yields_no_value for the full shape. */
                if (call_yields_no_value(init, table)) {
                    char vmsg[320];
                    snprintf(vmsg, sizeof(vmsg),
                             "'%s' returns no value, so there is nothing to "
                             "assign here. Give it a return type (e.g. "
                             "`%s(...): int`) and a `return`, or call it as a "
                             "statement on its own line",
                             init->value ? init->value : "function",
                             init->value ? init->value : "f");
                    type_error(vmsg, init->line, init->column);
                    return 0;
                }
                // #1044 implicit enum selector: resolve a bare enum member on
                // the RHS. For a typed declaration `c: Color = Blue` the expected
                // type is the annotation (stmt->node_type); for a bare-local
                // reassignment `d = Blue` (also an AST_VARIABLE_DECLARATION with
                // no annotation) it is the existing variable's type.
                {
                    Type* expected_enum = stmt->node_type;
                    if (!(expected_enum && expected_enum->kind == TYPE_ENUM) && stmt->value) {
                        Symbol* ex = lookup_symbol(table, stmt->value);
                        if (ex && ex->type && ex->type->kind == TYPE_ENUM)
                            expected_enum = ex->type;
                    }
                    coerce_bare_enum_member(init, expected_enum, table);
                }
                // Match-as-expression: typecheck as statement, then use its type
                if (init->type == AST_MATCH_STATEMENT) {
                    typecheck_statement(init, table);
                } else {
                    typecheck_expression(init, table);
                }
                Type* init_type = infer_type(init, table);

                /* `const` is substitution-at-each-use: the compiler
                 * inlines the RHS expression at every reference. That
                 * works for literals but is silently wrong for
                 * `const G = make_thing()` — every reference re-calls
                 * the function, allocating fresh state. Per Nico's
                 * design call: reject non-constant RHS expressions at
                 * compile time rather than running silently-wrong
                 * code. See `is_const_expression` above for the
                 * allowed/disallowed list. */
                if (stmt->type == AST_CONST_DECLARATION &&
                    !is_const_expression(init, table)) {
                    /* Allow array literals for const arr[] = [...] declarations.
                     * These emit static const arrays, not #define-inlined forms.
                     * Each element must itself be a compile-time constant — a
                     * non-const element (e.g. `[foo(), 2]`) is rejected so the
                     * static initializer stays well-formed. */
                    int is_array_const = (stmt->annotation && strcmp(stmt->annotation, "array_const") == 0 &&
                                          init->type == AST_ARRAY_LITERAL);
                    if (is_array_const) {
                        for (int ei = 0; ei < init->child_count; ei++) {
                            if (!is_const_array_element(init->children[ei], table)) {
                                is_array_const = 0;
                                break;
                            }
                        }
                    }
                    if (!is_array_const) {
                        char msg[512];
                        if (stmt->annotation && strcmp(stmt->annotation, "global_var") == 0) {
                            /* #701: a module-level `var` lowers to a C file-scope
                             * `static`, whose initializer C requires to be a
                             * constant expression. Initialize to a constant and
                             * assign the computed value from a function at
                             * startup. */
                            snprintf(msg, sizeof(msg),
                                "module-level `var` initializer must be a compile-time constant "
                                "expression, it lowers to a file-scope `static`, which C requires "
                                "to have a constant initializer. Initialize it to a constant here "
                                "and assign the computed value from a function at startup instead.");
                        } else {
                            snprintf(msg, sizeof(msg),
                                "const initializer must be a compile-time constant expression, "
                                "function calls are inlined at each use site, which would re-evaluate "
                                "the call (and re-allocate / re-side-effect) on every reference. "
                                "Use a regular assignment in main() (or std.config / std.actors for "
                                "process-global state) instead.");
                        }
                        type_error(msg, stmt->line, stmt->column);
                        free_type(init_type);
                        return 0;
                    }
                }

                /* Python-style "redeclaration" (`b = 999` after a prior
                 * `byte b = 0`) parses as a fresh AST_VARIABLE_DECLARATION
                 * with stmt->node_type == TYPE_UNKNOWN. The variable's
                 * actual type lives in the symbol table. When the existing
                 * binding is byte-typed, run the literal-range check
                 * before stmt->node_type gets overwritten with the int
                 * init type below. */
                Symbol* existing = stmt->value ? lookup_symbol(table, stmt->value) : NULL;
                if (existing && existing->type && existing->type->kind == TYPE_BYTE &&
                    byte_assignment_literal_out_of_range(init)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                        "byte literal out of range: %s does not fit in 0..255",
                        init->value ? init->value : "?");
                    type_error(msg, stmt->line, stmt->column);
                    free_type(init_type);
                    return 0;
                }

                /* #869: a bare re-bind (`v = <expr>`, no type annotation) of
                 * a local that was declared with an EXPLICIT integer type
                 * must NOT re-infer/narrow that type. In Aether `name = expr`
                 * with no annotation parses as a fresh AST_VARIABLE_DECLARATION
                 * (the Python-style re-bind noted above), and the path below
                 * would otherwise stamp the binding with the initializer's
                 * type — so `uint64 v = 0; v = x - 48` silently re-typed v to
                 * 32-bit int, discarding the explicit width, and the next
                 * 64-bit assignment (`v = v + d`) then tripped the #698 guard
                 * with a "type was inferred as 32-bit int" message about a
                 * variable that is explicitly `uint64`. The explicit
                 * declaration is authoritative: keep v's declared type and
                 * treat the re-bind as an assignment (an int RHS widens; a
                 * narrower RHS is the same truncation the explicit type already
                 * permits). Scoped to the explicitly-typed case — a re-bind of
                 * an *inferred* int local still triggers #698 below. */
                if (stmt->type == AST_VARIABLE_DECLARATION &&
                    stmt->type_inferred &&
                    existing && existing->type && existing->width_explicit &&
                    is_integer_scalar(existing->type->kind) &&
                    init_type && is_integer_scalar(init_type->kind)) {
                    if (!is_assignable(init_type, existing->type)) {
                        char emsg[256];
                        snprintf(emsg, sizeof(emsg),
                            "Type mismatch in assignment to '%s': expected %s, got %s",
                            stmt->value ? stmt->value : "?",
                            type_name(existing->type), type_name(init_type));
                        type_error(emsg, stmt->line, stmt->column);
                        free_type(init_type);
                        return 0;
                    }
                    if (stmt->node_type) free_type(stmt->node_type);
                    stmt->node_type = clone_type(existing->type);
                    /* Compute the RHS in 64 bits when the declared type is
                     * 64-bit, so an int-typed sub-expression doesn't truncate
                     * mid-evaluation (#697). */
                    if (existing->type->kind == TYPE_INT64 ||
                        existing->type->kind == TYPE_UINT64) {
                        propagate_int_width_64(init, existing->type->kind);
                    }
                    free_type(init_type);
                    return 1;
                }

                /* #698: silent-narrowing guard. Re-binding a local whose
                 * type was INFERRED as 32-bit int (a bare `x = expr`, not an
                 * explicit annotation) with a 64-bit value truncates
                 * silently — is_assignable permits int64->int and codegen
                 * keeps the int storage. Because the type was inferred, not
                 * chosen, this is almost certainly unintended (the aedis
                 * lpStringEqualsInt64 bug: `parsed = 0` then `parsed =
                 * <int64>`, so the comparison never matched). Diagnose with
                 * the load-bearing fix; an explicit annotation clears the
                 * inferred flag and silences it. */
                if (existing && existing->type_inferred &&
                    existing->type && existing->type->kind == TYPE_INT &&
                    init_type && (init_type->kind == TYPE_INT64 ||
                                  init_type->kind == TYPE_UINT64)) {
                    const char* nm = stmt->value ? stmt->value : "x";
                    char msg[420];
                    snprintf(msg, sizeof(msg),
                        "narrowing assignment to '%s': its type was inferred "
                        "as 32-bit int from its initializer, but a 64-bit "
                        "value is assigned here and would truncate. Annotate "
                        "the declaration to keep 64 bits (e.g. `long %s = ...` "
                        "or `uint64 %s = ...`), or write `int %s = ...` to "
                        "make the narrowing explicit.", nm, nm, nm, nm);
                    type_error(msg, stmt->line, stmt->column);
                    free_type(init_type);
                    return 0;
                }

                /* Context-sensitive literal: `[a, b, c]` against a
                 * `*StringSeq`-typed variable means "build a cons chain"
                 * rather than "build a static C array". Stamp the
                 * literal's node_type so the codegen branch in
                 * codegen_expr.c picks the cons-chain emitter. The
                 * empty literal `[]` carries through as a *StringSeq
                 * NULL the same way. See std/collections/aether_stringseq.h
                 * for the cell layout. */
                if (init->type == AST_ARRAY_LITERAL &&
                    is_string_seq_ptr_type(stmt->node_type)) {
                    if (init->node_type) free_type(init->node_type);
                    init->node_type = clone_type(stmt->node_type);
                    free_type(init_type);
                    add_symbol(table, stmt->value, clone_type(stmt->node_type), 0, 0, 0);
                    return 1;
                }

                /* #745: a typed module-level const array
                 * (`const T[N] = [literals]`, annotation "array_const")
                 * lets the porter pin the C element type — narrowing the
                 * int literals to uint8/uint16/uint32 is the explicit,
                 * compile-time-constant intent (same posture as
                 * `byte b = 5`), not an is_assignable mismatch. Permit an
                 * integer-element array literal to initialise an integer-
                 * element typed const array. */
                int array_const_int_narrow = 0;
                if (stmt->annotation && strcmp(stmt->annotation, "array_const") == 0 &&
                    init_type && init_type->kind == TYPE_ARRAY && init_type->element_type &&
                    stmt->node_type && stmt->node_type->kind == TYPE_ARRAY &&
                    stmt->node_type->element_type) {
                    TypeKind ie = init_type->element_type->kind;
                    TypeKind de = stmt->node_type->element_type->kind;
                    int ie_int = (ie == TYPE_INT || ie == TYPE_INT64 || ie == TYPE_UINT64 ||
                                  ie == TYPE_UINT32 || ie == TYPE_UINT16 || ie == TYPE_UINT8 ||
                                  ie == TYPE_BYTE);
                    int de_int = (de == TYPE_INT || de == TYPE_INT64 || de == TYPE_UINT64 ||
                                  de == TYPE_UINT32 || de == TYPE_UINT16 || de == TYPE_UINT8 ||
                                  de == TYPE_BYTE);
                    array_const_int_narrow = ie_int && de_int;
                }

                // If variable has no explicit type (TYPE_UNKNOWN), use initializer's type
                if (!stmt->node_type || stmt->node_type->kind == TYPE_UNKNOWN) {
                    if (stmt->node_type) free_type(stmt->node_type);
                    /* #892: a NAMED array used as an initializer decays to a
                     * pointer, matching C's array-to-pointer decay. So
                     * `ids = static_ids` (static_ids: byte[N]) infers `ids`
                     * as a plain `ptr`, not an array type — keeping a later
                     * `ids = heap` / `ids = null` legal (the stack-buffer-
                     * with-heap-fallback C idiom). An array LITERAL
                     * initializer (`x = [1,2,3]`) is NOT an identifier, so it
                     * still binds a real array; only a named-array lvalue
                     * decays. */
                    if (init_type && init_type->kind == TYPE_ARRAY &&
                        init && init->type == AST_IDENTIFIER) {
                        stmt->node_type = create_type(TYPE_PTR);
                    } else {
                        stmt->node_type = clone_type(init_type);
                    }
                } else if (!array_const_int_narrow && !is_assignable(init_type, stmt->node_type)) {
                    // Has explicit type but initializer doesn't match
                    free_type(init_type);
                    type_error("Type mismatch in variable initialization", stmt->line, stmt->column);
                    return 0;
                }
                // #1044 enum-indexed array `[E]T`: the index type must be a real
                // enum, and a literal initializer supplies one value per member,
                // in declaration order (an empty `[]` zero-initialises every
                // slot).
                if (stmt->node_type && stmt->node_type->kind == TYPE_ARRAY &&
                    stmt->node_type->index_enum_name) {
                    const char* en = stmt->node_type->index_enum_name;
                    if (!is_enum_type_name(en)) {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                            "enum-indexed array `[%s]...` requires `%s` to be an enum",
                            en, en);
                        type_error(msg, stmt->line, stmt->column);
                        return 0;
                    }
                    if (init && init->type == AST_ARRAY_LITERAL &&
                        init->child_count > 0 &&
                        init->child_count != stmt->node_type->array_size) {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                            "enum-indexed array `[%s]...` needs %d values (one per "
                            "member, in order), got %d",
                            en, stmt->node_type->array_size, init->child_count);
                        type_error(msg, stmt->line, stmt->column);
                        return 0;
                    }
                }
                /* #340: re-binding an already-declared optional local
                 * (`z = 5` / `z = none` after `let z: int? = ...`) adopts the
                 * existing optional type, so codegen wraps the value (or zeros
                 * `none`) into the right `ae_opt_<T>` instead of stamping the
                 * binding with the bare value type. */
                if (existing && existing->type && existing->type->kind == TYPE_OPTIONAL &&
                    (!stmt->node_type || stmt->node_type->kind != TYPE_OPTIONAL)) {
                    if (stmt->node_type) free_type(stmt->node_type);
                    stmt->node_type = clone_type(existing->type);
                }
                /* #340: a `none` initializer needs the declared optional type
                 * pinned onto it (codegen emits the matching `ae_opt_<T>`).
                 * `let m = none` with no annotation can't know T — require one. */
                if (init && init->type == AST_NONE_LITERAL) {
                    if (stmt->node_type && stmt->node_type->kind == TYPE_OPTIONAL &&
                        stmt->node_type->element_type &&
                        stmt->node_type->element_type->kind != TYPE_UNKNOWN) {
                        if (init->node_type) free_type(init->node_type);
                        init->node_type = clone_type(stmt->node_type);
                    } else if (existing && existing->type &&
                               existing->type->kind == TYPE_OPTIONAL &&
                               existing->type->element_type &&
                               existing->type->element_type->kind != TYPE_UNKNOWN) {
                        // Python-style re-bind of an already-declared optional
                        // (`z = none` after `let z: int? = ...`): adopt the
                        // existing type so codegen emits the right `ae_opt_<T>`.
                        if (stmt->node_type) free_type(stmt->node_type);
                        stmt->node_type = clone_type(existing->type);
                        if (init->node_type) free_type(init->node_type);
                        init->node_type = clone_type(existing->type);
                    } else {
                        type_error("cannot infer optional type from `none` alone, "
                                   "annotate the binding (e.g. `x: int? = none`)",
                                   stmt->line, stmt->column);
                    }
                }
                /* #697: declared 64-bit but the initializer is a 32-bit-int
                 * arithmetic expression (e.g. `uint64 x = byte << 24`) —
                 * widen it so the computation happens in 64 bits, not C int. */
                if (stmt->node_type &&
                    (stmt->node_type->kind == TYPE_INT64 ||
                     stmt->node_type->kind == TYPE_UINT64)) {
                    propagate_int_width_64(init, stmt->node_type->kind);
                }
                /* `byte b = <int literal>` — fresh declaration with explicit
                 * `byte` type. Reject out-of-range literals at compile time
                 * (literal-range check). Non-literal int is accepted (runtime
                 * truncation, matching int64→int etc.). */
                if (stmt->node_type && stmt->node_type->kind == TYPE_BYTE &&
                    byte_assignment_literal_out_of_range(init)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                        "byte literal out of range: %s does not fit in 0..255",
                        init->value ? init->value : "?");
                    type_error(msg, stmt->line, stmt->column);
                    free_type(init_type);
                    return 0;
                }
                free_type(init_type);
            }

            // Add to symbol table. Bare `_` is a per-use discard — it
            // is never registered as a symbol, so each `_ = <expr>`
            // stays an independent throwaway with no type unified
            // across occurrences (aeb-ae-help-and-toolchain-feedback.md
            // #4). Mirrors the existing `_`-skip in the tuple-
            // destructure path above.
            if (stmt->value && strcmp(stmt->value, "_") != 0) {
                add_symbol(table, stmt->value, clone_type(stmt->node_type), 0, 0, 0);
                /* #698: carry the parser's inferred-type marker onto the
                 * binding, but only for a 32-bit int (the sole narrowing
                 * target). A later 64-bit re-bind then triggers the guard
                 * above / in AST_ASSIGNMENT. */
                if (stmt->type_inferred && stmt->node_type &&
                    stmt->node_type->kind == TYPE_INT) {
                    Symbol* s = lookup_symbol_local(table, stmt->value);
                    if (s) s->type_inferred = 1;
                }
                /* #869: record an EXPLICIT integer-type annotation
                 * (`uint64 v = 0`, `long n = ...`) so a later bare re-bind
                 * preserves the declared width. Distinct from type_inferred:
                 * the width-inference pass mutates `type`/`type_inferred` of an
                 * inferred local, but never sets this flag, so it cannot make
                 * an inferred local masquerade as explicitly-typed. */
                if (!stmt->type_inferred && stmt->node_type &&
                    is_integer_scalar(stmt->node_type->kind)) {
                    Symbol* s = lookup_symbol_local(table, stmt->value);
                    if (s) s->width_explicit = 1;
                }
            }
            return 1;
        }

        case AST_ASSIGNMENT: {
            if (stmt->child_count >= 2) {
                ASTNode* left = stmt->children[0];
                ASTNode* right = stmt->children[1];
                
                Symbol* symbol = lookup_symbol(table, left->value);
                if (!symbol) {
                    char error_msg[256];
                    if (left->value && name_blocked_by_hide(table, left->value)) {
                        snprintf(error_msg, sizeof(error_msg),
                                 "'%s' is hidden in this scope by `hide` or `seal except`",
                                 left->value);
                    } else {
                        snprintf(error_msg, sizeof(error_msg), "Undefined variable '%s'", left->value ? left->value : "?");
                    }
                    type_error(error_msg, left->line, left->column);
                    return 0;
                }

                // #1044 implicit enum selector: `d = Blue` where `d` has an enum
                // type resolves the bare member against that enum.
                coerce_bare_enum_member(right, symbol->type, table);

                Type* right_type = infer_type(right, table);
                if (!is_assignable(right_type, symbol->type)) {
                    char error_msg[256];
                    snprintf(error_msg, sizeof(error_msg),
                             "Type mismatch in assignment to '%s': expected %s, got %s",
                             left->value ? left->value : "?",
                             type_name(symbol->type), type_name(right_type));
                    free_type(right_type);
                    type_error(error_msg, stmt->line, stmt->column);
                    return 0;
                }
                /* #698: silent-narrowing guard (assignment path) — twin of
                 * the AST_VARIABLE_DECLARATION guard, for assignment forms
                 * that take this path. */
                if (symbol->type_inferred &&
                    symbol->type && symbol->type->kind == TYPE_INT &&
                    right_type && (right_type->kind == TYPE_INT64 ||
                                   right_type->kind == TYPE_UINT64)) {
                    const char* nm = left->value ? left->value : "x";
                    char msg[420];
                    snprintf(msg, sizeof(msg),
                        "narrowing assignment to '%s': its type was inferred "
                        "as 32-bit int from its initializer, but a 64-bit "
                        "value is assigned here and would truncate. Annotate "
                        "the declaration to keep 64 bits (e.g. `long %s = ...` "
                        "or `uint64 %s = ...`), or write `int %s = ...` to "
                        "make the narrowing explicit.", nm, nm, nm, nm);
                    type_error(msg, stmt->line, stmt->column);
                    free_type(right_type);
                    return 0;
                }
                /* `b = <int literal>` where b: byte — same range check as the
                 * declaration path. */
                if (symbol->type && symbol->type->kind == TYPE_BYTE &&
                    byte_assignment_literal_out_of_range(right)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                        "byte literal out of range: %s does not fit in 0..255",
                        right->value ? right->value : "?");
                    type_error(msg, stmt->line, stmt->column);
                    free_type(right_type);
                    return 0;
                }
                free_type(right_type);
            }
            return 1;
        }

        case AST_COMPOUND_ASSIGNMENT: {
            // node->value = variable name, children[0] = operator, children[1] = RHS
            if (stmt->child_count >= 2) {
                Symbol* symbol = lookup_symbol(table, stmt->value);
                if (!symbol) {
                    char error_msg[256];
                    if (stmt->value && name_blocked_by_hide(table, stmt->value)) {
                        snprintf(error_msg, sizeof(error_msg),
                                 "'%s' is hidden in this scope by `hide` or `seal except`",
                                 stmt->value);
                    } else {
                        snprintf(error_msg, sizeof(error_msg), "Undefined variable '%s'", stmt->value ? stmt->value : "?");
                    }
                    type_error(error_msg, stmt->line, stmt->column);
                    return 0;
                }
                ASTNode* rhs = stmt->children[1];
                typecheck_expression(rhs, table);
                { Type* _t = infer_type(rhs, table); free_type(_t); }
                if (stmt->node_type && stmt->node_type->kind == TYPE_UNKNOWN && symbol->type) {
                    free_type(stmt->node_type);
                    stmt->node_type = clone_type(symbol->type);
                }
            }
            return 1;
        }

        case AST_IF_STATEMENT: {
            ASTNode* narrow_var = NULL;
            int narrow_then = 1;
            if (stmt->child_count >= 1) {
                ASTNode* condition = stmt->children[0];
                typecheck_expression(condition, table);
                Type* cond_type = infer_type(condition, table);

                if (cond_type && cond_type->kind != TYPE_BOOL) {
                    free_type(cond_type);
                    type_error("If condition must be boolean", condition->line, condition->column);
                    return 0;
                }
                free_type(cond_type);
                // #1068 detect an optional none-check to narrow one branch.
                narrow_var = optional_narrow_target(condition, &narrow_then);
            }

            // Type check then (child 1) and else (child 2) branches. The branch
            // that the guard proves present narrows the optional variable to its
            // inner type `T` in a child scope (so `x.field` / `foo(x)` typecheck
            // without `!`), unless the variable is rebound in that branch.
            for (int i = 1; i < stmt->child_count; i++) {
                ASTNode* branch = stmt->children[i];
                int is_then = (i == 1);
                int narrow_this = narrow_var && branch &&
                                  ((is_then && narrow_then) || (!is_then && !narrow_then)) &&
                                  !narrow_unsafe(branch, narrow_var->value);
                if (narrow_this) {
                    Type* inner = narrow_var->node_type->element_type;
                    SymbolTable* nt = create_symbol_table(table);
                    add_symbol(nt, narrow_var->value, clone_type(inner), 0, 0, 0);
                    mark_optional_narrowed(branch, narrow_var->value, inner);
                    typecheck_statement(branch, nt);
                    free_symbol_table(nt);
                } else {
                    typecheck_statement(branch, table);
                }
            }
            return 1;
        }
        
        case AST_FOR_LOOP: {
            SymbolTable* loop_table = create_symbol_table(table);
            
            // Type check init (child 0)
            if (stmt->child_count > 0 && stmt->children[0]) {
                typecheck_statement(stmt->children[0], loop_table);
            }
            
            // Type check condition (child 1)
            if (stmt->child_count > 1 && stmt->children[1]) {
                typecheck_expression(stmt->children[1], loop_table);
                Type* cond_type = infer_type(stmt->children[1], loop_table);
                if (cond_type && cond_type->kind != TYPE_BOOL) {
                    free_type(cond_type);
                    type_error("For loop condition must be boolean", stmt->line, stmt->column);
                    free_symbol_table(loop_table);
                    return 0;
                }
                free_type(cond_type);
            }
            
            // Type check increment (child 2)
            if (stmt->child_count > 2 && stmt->children[2]) {
                typecheck_expression(stmt->children[2], loop_table);
            }
            
            // Type check body (child 3) — #893: with this loop's label in
            // scope for break/continue validation.
            if (stmt->child_count > 3 && stmt->children[3]) {
                tc_push_loop_label(stmt->value);
                typecheck_statement(stmt->children[3], loop_table);
                tc_pop_loop_label();
            }

            free_symbol_table(loop_table);
            return 1;
        }
        
        case AST_WHILE_LOOP: {
            if (stmt->child_count >= 1) {
                ASTNode* condition = stmt->children[0];
                typecheck_expression(condition, table);
                Type* cond_type = infer_type(condition, table);

                if (cond_type && cond_type->kind != TYPE_BOOL) {
                    free_type(cond_type);
                    type_error("Loop condition must be boolean", condition->line, condition->column);
                    return 0;
                }
                free_type(cond_type);
            }

            // Type check loop body (#893: with this loop's label in scope so
            // a `break`/`continue <label>` inside it validates).
            tc_push_loop_label(stmt->value);
            for (int i = 1; i < stmt->child_count; i++) {
                typecheck_statement(stmt->children[i], table);
            }
            tc_pop_loop_label();
            return 1;
        }

        case AST_BREAK_STATEMENT:
        case AST_CONTINUE_STATEMENT:
            /* #893: a labeled break/continue must name an enclosing loop. */
            if (stmt->value && !tc_loop_label_in_scope(stmt->value)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "no enclosing loop labeled '%s' for `%s %s`",
                    stmt->value,
                    stmt->type == AST_BREAK_STATEMENT ? "break" : "continue",
                    stmt->value);
                type_error(msg, stmt->line, stmt->column);
                return 0;
            }
            return 1;

        case AST_BLOCK: {
            SymbolTable* block_table = create_symbol_table(table);

            // PRE-PASS: collect hide / seal directives BEFORE processing any
            // other statements, so they're scope-level (position within the
            // block doesn't matter) and apply to every other statement here.
            for (int i = 0; i < stmt->child_count; i++) {
                ASTNode* child = stmt->children[i];
                if (!child) continue;
                if (child->type == AST_HIDE_DIRECTIVE) {
                    for (int j = 0; j < child->child_count; j++) {
                        ASTNode* id = child->children[j];
                        if (id && id->value) scope_hide_name(block_table, id->value);
                    }
                } else if (child->type == AST_SEAL_DIRECTIVE) {
                    block_table->is_sealed = 1;
                    for (int j = 0; j < child->child_count; j++) {
                        ASTNode* id = child->children[j];
                        if (id && id->value) scope_seal_except(block_table, id->value);
                    }
                }
            }

            for (int i = 0; i < stmt->child_count; i++) {
                ASTNode* child = stmt->children[i];
                // Skip the directive nodes themselves — they were already
                // processed in the pre-pass above. Walking them as statements
                // would just be a no-op, but we keep the block tidy.
                if (child && (child->type == AST_HIDE_DIRECTIVE ||
                              child->type == AST_SEAL_DIRECTIVE)) {
                    continue;
                }
                typecheck_statement(child, block_table);
            }

            free_symbol_table(block_table);
            return 1;
        }
        
        case AST_EXPRESSION_STATEMENT: {
            if (stmt->child_count > 0) {
                typecheck_expression(stmt->children[0], table);
                /* Phase 2: a `T!` result dropped as a bare statement is
                 * an error — it must be consumed. Only fires on a raw
                 * result expression; every consumer nests the call and
                 * clears/binds the result-ness. */
                check_unconsumed_result(stmt->children[0], table);
            }
            return 1;
        }

        // Bridge cases: when these node types appear in a "statement"
        // recursion (e.g. as the value of a return statement, or as
        // the RHS of an `if x = call()` shape that walks via the
        // catch-all default), we want them routed through the
        // expression typechecker so node_type lands on member-access
        // / interp children. Without these, the default branch's
        // `typecheck_node → typecheck_statement` recursion fell back
        // to default again at AST_STRING_INTERP / AST_MEMBER_ACCESS
        // (no cases here pre-fix) and never called typecheck_expression
        // — string interp's TypeKind switch then defaulted to `%d`
        // for string fields read through a `*Self` chain. Section C.1
        // of fresh-aether-requests.
        case AST_STRING_INTERP:
        case AST_MEMBER_ACCESS:
            return typecheck_expression(stmt, table);

        case AST_FUNCTION_CALL:
            // Function call used as a statement (e.g. println(...), user_fn(...))
            //
            // NB: this case is ALSO reached when the statement walker
            // recurses into a sub-expression that happens to be a call —
            // e.g. `return f() or { … }` routes the `or`-expr through
            // typecheck_node → the default statement branch → typecheck_node
            // on the or-expr's fallible child, which lands here. So this is
            // NOT a reliable "bare statement" position, and the Phase 2
            // unconsumed-result check must NOT run here (it would false-fire
            // on the consumed fallible inside an `or`/return). The genuine
            // bare-drop `f()` always arrives wrapped in AST_EXPRESSION_
            // STATEMENT (the parser wraps every statement-position
            // expression), where the check does run.
            return typecheck_function_call(stmt, table);

        case AST_TRY_STATEMENT: {
            // children: [0] body block, [1] AST_CATCH_CLAUSE (value = name, child[0] = handler)
            if (stmt->child_count != 2) {
                type_error("malformed try/catch", stmt->line, stmt->column);
                return 0;
            }
            ASTNode* body = stmt->children[0];
            ASTNode* catch_clause = stmt->children[1];

            // Body runs in its own scope (already handled by AST_BLOCK typecheck).
            typecheck_statement(body, table);

            // Catch binds `name` to a string (panic reason) and typechecks
            // the handler in a scope where that name is visible.
            if (!catch_clause || catch_clause->type != AST_CATCH_CLAUSE ||
                !catch_clause->value || catch_clause->child_count < 1) {
                type_error("malformed catch clause", stmt->line, stmt->column);
                return 0;
            }

            SymbolTable* catch_table = create_symbol_table(table);
            Type* str_type = create_type(TYPE_STRING);
            add_symbol(catch_table, catch_clause->value, str_type, 0, 0, 0);
            typecheck_statement(catch_clause->children[0], catch_table);
            free_symbol_table(catch_table);
            return 1;
        }

        case AST_PANIC_STATEMENT: {
            // panic(reason) — reason must typecheck and evaluate to a string.
            if (stmt->child_count < 1) {
                type_error("panic() requires a reason argument", stmt->line, stmt->column);
                return 0;
            }
            ASTNode* reason = stmt->children[0];
            typecheck_expression(reason, table);
            Type* rt = infer_type(reason, table);
            if (rt && rt->kind != TYPE_STRING && rt->kind != TYPE_UNKNOWN) {
                type_error("panic() reason must be a string", reason->line, reason->column);
                free_type(rt);
                return 0;
            }
            if (rt) free_type(rt);
            return 1;
        }

        case AST_PRINT_STATEMENT: {
            for (int i = 0; i < stmt->child_count; i++) {
                typecheck_expression(stmt->children[i], table);
            }
            if (stmt->child_count >= 2 &&
                stmt->children[0]->type == AST_LITERAL &&
                stmt->children[0]->node_type &&
                stmt->children[0]->node_type->kind == TYPE_STRING &&
                stmt->children[0]->value) {
                const char* fmt = stmt->children[0]->value;
                int arg_idx = 1;
                for (int fi = 0; fmt[fi]; fi++) {
                    if (fmt[fi] != '%' || !fmt[fi + 1]) continue;
                    fi++;
                    while (fmt[fi] == '-' || fmt[fi] == '+' || fmt[fi] == ' ' ||
                           fmt[fi] == '#' || fmt[fi] == '0') fi++;
                    while (fmt[fi] >= '0' && fmt[fi] <= '9') fi++;
                    if (fmt[fi] == '.') { fi++; while (fmt[fi] >= '0' && fmt[fi] <= '9') fi++; }
                    if (fmt[fi] == '%') continue;
                    if (arg_idx >= stmt->child_count) break;
                    Type* atype = infer_type(stmt->children[arg_idx], table);
                    TypeKind ak = atype ? atype->kind : TYPE_UNKNOWN;
                    char spec = fmt[fi];
                    int mismatch = 0;
                    if ((spec == 's') && ak != TYPE_STRING && ak != TYPE_PTR) mismatch = 1;
                    if ((spec == 'd' || spec == 'i') && ak != TYPE_INT && ak != TYPE_INT64 && ak != TYPE_BOOL) mismatch = 1;
                    if ((spec == 'f' || spec == 'g' || spec == 'e') && ak != TYPE_FLOAT && ak != TYPE_LONGDOUBLE) mismatch = 1;
                    if (mismatch) {
                        char wbuf[256];
                        snprintf(wbuf, sizeof(wbuf),
                            "Format specifier '%%%c' does not match argument type '%s' (auto-corrected)",
                            spec, type_name(atype));
                        type_warning(wbuf, stmt->children[arg_idx]->line, stmt->children[arg_idx]->column);
                    }
                    if (atype) free_type(atype);
                    arg_idx++;
                }
            }
            return 1;
        }
        
        case AST_SEND_STATEMENT: {
            // Generic `send(actor, message)` was an early actor-messaging form
            // whose codegen path was never completed; the fire-and-forget
            // operator `actor ! Message { ... }` supersedes it. Reject it here
            // with a pointer to the supported syntax instead of letting it fall
            // through to a broken C emission. (The `send` keyword stays reserved
            // so it still cannot be used as an ordinary identifier.) The move
            // checker's third pass still runs, so an Isolated[T] used twice via
            // send is reported with its own diagnostic as well.
            int line = stmt->line, col = stmt->column;
            if (stmt->child_count > 0 && stmt->children[0]) {
                line = stmt->children[0]->line;
                col = stmt->children[0]->column;
            }
            type_error("`send(actor, message)` is not supported; use `actor ! Message { ... }`",
                       line, col);
            return 0;
        }

        case AST_SEND_FIRE_FORGET: {
            // actor ! MessageType { fields... }
            if (stmt->child_count >= 2) {
                ASTNode* actor_ref = stmt->children[0];
                ASTNode* message = stmt->children[1];

                // Validate actor reference type
                typecheck_expression(actor_ref, table);
                Type* actor_type = infer_type(actor_ref, table);
                if (actor_type && actor_type->kind != TYPE_ACTOR_REF && actor_type->kind != TYPE_UNKNOWN) {
                    char error_msg[256];
                    snprintf(error_msg, sizeof(error_msg),
                             "Cannot send to '%s': expected an actor reference",
                             actor_ref->value ? actor_ref->value : "expression");
                    free_type(actor_type);
                    type_error(error_msg, actor_ref->line, actor_ref->column);
                    return 0;
                }
                free_type(actor_type);

                // Validate that the message type is a registered message definition
                if (message->type == AST_MESSAGE_CONSTRUCTOR && message->value) {
                    Symbol* msg_sym = lookup_symbol(table, message->value);
                    if (!msg_sym || !msg_sym->type || msg_sym->type->kind != TYPE_MESSAGE) {
                        char error_msg[256];
                        snprintf(error_msg, sizeof(error_msg),
                                 "Undefined message type '%s'", message->value);
                        type_error(error_msg, message->line, message->column);
                        return 0;
                    }
                }

                // Validate field value types match declared field types
                typecheck_message_constructor(message, table);
            }
            return 1;
        }

        case AST_SPAWN_ACTOR_STATEMENT: {
            // `spawn_actor(Type)` was an early form whose codegen was never
            // completed; `spawn(ActorType())` supersedes it. Reject with a
            // pointer to the supported syntax.
            int line = stmt->line, col = stmt->column;
            if (stmt->child_count > 0 && stmt->children[0]) {
                line = stmt->children[0]->line;
                col = stmt->children[0]->column;
            }
            type_error("`spawn_actor(Type)` is not supported; use `spawn(ActorType())`",
                       line, col);
            return 0;
        }
        
        case AST_MATCH_STATEMENT: {
            // Type check the match expression
            Type* match_expr_type = NULL;
            Type* element_type = NULL;
            /* Whether the match expression is a `*StringSeq`. When it
             * is, head bindings are typed `string` and tail bindings
             * are typed `*StringSeq` (so recursive walks compose) —
             * not the array-element + array-of-element shapes used
             * for the legacy int-array path. See codegen_stmt.c
             * `is_string_seq_type` for the matching codegen
             * dispatch. */
            int is_seq_match = 0;
            if (stmt->child_count > 0) {
                typecheck_expression(stmt->children[0], table);
                match_expr_type = stmt->children[0]->node_type;
                // Extract element type if matching on an array
                if (match_expr_type && match_expr_type->kind == TYPE_ARRAY && match_expr_type->element_type) {
                    element_type = match_expr_type->element_type;
                } else if (is_string_seq_ptr_type(match_expr_type)) {
                    is_seq_match = 1;
                    element_type = create_type(TYPE_STRING);
                }
            }
            // Default to int if we couldn't determine the element type
            if (!element_type) {
                element_type = create_type(TYPE_INT);
            }

            // #914 sum `match` bookkeeping: track variant coverage (for the
            // exhaustiveness check) and the scrutinee variable name (for
            // per-arm narrowing — `match s { Circle -> { s.r } }` narrows `s`
            // to Circle inside the Circle arm). Narrowing applies only when
            // the scrutinee is a plain variable.
            int sum_match = (match_expr_type && match_expr_type->kind == TYPE_SUM);
            const char* scrutinee_name =
                (sum_match && stmt->children[0] &&
                 stmt->children[0]->type == AST_IDENTIFIER)
                    ? stmt->children[0]->value : NULL;
            int sum_nvar = sum_match ? match_expr_type->tuple_count : 0;
            int sum_covered[64];
            for (int z = 0; z < 64; z++) sum_covered[z] = 0;
            int sum_wildcard = 0;
            int sum_wildcard_line = -1;

            // #1044 enum `match` bookkeeping (mirrors the sum path): resolve
            // bare-name arms (`Red ->`) to the enum constant and track member
            // coverage for the exhaustiveness check. Only engages when the
            // scrutinee is a TYPE_ENUM, so every other match shape is untouched.
            int enum_match = (match_expr_type && match_expr_type->kind == TYPE_ENUM);
            const char* enum_name = enum_match ? match_expr_type->struct_name : NULL;
            ASTNode* enum_def = enum_name ? enum_def_lookup(enum_name) : NULL;
            int enum_covered[64];
            for (int z = 0; z < 64; z++) enum_covered[z] = 0;
            int enum_wildcard = 0;
            int enum_wildcard_line = -1;

            check_match_arm_reachability(stmt);   /* #1377 */

            // Type check each match arm
            for (int i = 1; i < stmt->child_count; i++) {
                ASTNode* arm = stmt->children[i];
                if (!arm || arm->type != AST_MATCH_ARM || arm->child_count < 2) continue;

                ASTNode* pattern = arm->children[0];
                ASTNode* body = arm->children[1];

                // Create a new scope for pattern variables
                SymbolTable* arm_table = create_symbol_table(table);

                // Register pattern variables from list patterns using the actual element type
                if (pattern->type == AST_PATTERN_LIST) {
                    for (int j = 0; j < pattern->child_count; j++) {
                        ASTNode* elem = pattern->children[j];
                        if (elem && elem->type == AST_PATTERN_VARIABLE && elem->value) {
                            add_symbol(arm_table, elem->value, clone_type(element_type), 0, 0, 0);
                        }
                    }
                } else if (pattern->type == AST_PATTERN_CONS) {
                    // [h|t] - register head and tail with proper types
                    if (pattern->child_count >= 1) {
                        ASTNode* head = pattern->children[0];
                        if (head && head->type == AST_PATTERN_VARIABLE && head->value) {
                            add_symbol(arm_table, head->value, clone_type(element_type), 0, 0, 0);
                        }
                    }
                    if (pattern->child_count >= 2) {
                        ASTNode* tail = pattern->children[1];
                        if (tail && tail->type == AST_PATTERN_VARIABLE && tail->value) {
                            Type* tail_type;
                            if (is_seq_match) {
                                /* tail in `[h|t]` against a `*StringSeq`
                                 * is itself a `*StringSeq`, not an
                                 * array-of-element. This is what makes
                                 * recursive walks compose: walk(tail)
                                 * passes a properly-typed seq into the
                                 * recursive call. */
                                tail_type = make_string_seq_ptr_type();
                            } else {
                                tail_type = create_type(TYPE_ARRAY);
                                tail_type->element_type = clone_type(element_type);
                            }
                            add_symbol(arm_table, tail->value, tail_type, 0, 0, 0);
                        }
                    }
                }

                // #340: optional `match` — bind `some(v)` to the unwrapped
                // value (typed as the optional's inner T), and pin a `none`
                // arm's type to the matched optional so codegen compares the
                // right shape.
                if (match_expr_type && match_expr_type->kind == TYPE_OPTIONAL) {
                    if (pattern->type == AST_PATTERN_VARIABLE && pattern->annotation &&
                        strcmp(pattern->annotation, "some_pattern") == 0 && pattern->value) {
                        Type* inner = match_expr_type->element_type
                                    ? clone_type(match_expr_type->element_type)
                                    : create_type(TYPE_UNKNOWN);
                        add_symbol(arm_table, pattern->value, inner, 0, 0, 0);
                    } else if (pattern->type == AST_NONE_LITERAL && !pattern->node_type) {
                        pattern->node_type = clone_type(match_expr_type);
                    }
                }

                // #914 sum `match`: a bare variant-name pattern selects that
                // variant and narrows the scrutinee to the variant struct
                // inside the arm. `_` is the catch-all. Validate the name and
                // record coverage for the post-loop exhaustiveness check.
                if (sum_match && pattern) {
                    // A bare variant pattern (`Circle ->`) is parsed by
                    // parse_match_case via parse_expression, so it arrives as
                    // an AST_IDENTIFIER; `_` arrives as a TYPE_WILDCARD literal.
                    int is_wild = (pattern->node_type &&
                                   pattern->node_type->kind == TYPE_WILDCARD) ||
                                  (pattern->value && strcmp(pattern->value, "_") == 0);
                    if (is_wild) {
                        sum_wildcard = 1;
                        if (sum_wildcard_line < 0) sum_wildcard_line = pattern->line;
                    } else if ((pattern->type == AST_IDENTIFIER ||
                                pattern->type == AST_PATTERN_VARIABLE) && pattern->value) {
                        int vidx = -1;
                        for (int vi = 0; vi < sum_nvar; vi++) {
                            Type* vt = match_expr_type->tuple_types[vi];
                            if (vt && vt->struct_name &&
                                strcmp(vt->struct_name, pattern->value) == 0) { vidx = vi; break; }
                        }
                        if (vidx < 0) {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                "'%s' is not a variant of sum type '%s'",
                                pattern->value,
                                match_expr_type->struct_name ? match_expr_type->struct_name : "?");
                            type_error(msg, pattern->line, pattern->column);
                        } else {
                            if (vidx < 64) sum_covered[vidx] = 1;
                            // Narrow the scrutinee variable to the variant
                            // struct so `s.field` typechecks inside the arm.
                            if (scrutinee_name) {
                                add_symbol(arm_table, scrutinee_name,
                                           clone_type(match_expr_type->tuple_types[vidx]),
                                           0, 0, 0);
                            }
                        }
                    }
                }

                // #1044 enum `match`: an arm pattern is an enum member, written
                // qualified (`Color.Red`, already lowered to the constant
                // `Color_Red` by resolve_enum_types) or bare (`Red`). Resolve a
                // bare member to the constant here (so codegen emits
                // `_match_val == Color_Red`), reject non-members with a clear
                // error, and record coverage for the exhaustiveness check. `_`
                // is the catch-all.
                if (enum_match && enum_def && pattern) {
                    int is_wild = (pattern->node_type &&
                                   pattern->node_type->kind == TYPE_WILDCARD) ||
                                  (pattern->value && strcmp(pattern->value, "_") == 0);
                    if (is_wild) {
                        enum_wildcard = 1;
                        if (enum_wildcard_line < 0) enum_wildcard_line = pattern->line;
                    } else if ((pattern->type == AST_IDENTIFIER ||
                                pattern->type == AST_PATTERN_VARIABLE) && pattern->value) {
                        size_t elen = strlen(enum_name);
                        const char* mem = NULL;
                        int bare = 0;
                        if (enum_has_member(enum_name, pattern->value)) {
                            mem = pattern->value;                 // bare `Member`
                            bare = 1;
                        } else if (strncmp(pattern->value, enum_name, elen) == 0 &&
                                   pattern->value[elen] == '_' &&
                                   enum_has_member(enum_name, pattern->value + elen + 1)) {
                            mem = pattern->value + elen + 1;      // lowered `Enum_Member`
                        }
                        if (!mem) {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                "'%s' is not a member of enum '%s'",
                                pattern->value, enum_name ? enum_name : "?");
                            type_error(msg, pattern->line, pattern->column);
                        } else {
                            for (int mi = 0; mi < enum_def->child_count && mi < 64; mi++) {
                                ASTNode* mn = enum_def->children[mi];
                                if (mn && mn->type == AST_ENUM_MEMBER && mn->value &&
                                    strcmp(mn->value, mem) == 0) { enum_covered[mi] = 1; break; }
                            }
                            if (bare) {
                                // Lower the bare member to the enum constant so
                                // codegen compares against `Enum_Member`.
                                char qualified[512];
                                snprintf(qualified, sizeof(qualified), "%s_%s",
                                         enum_name, mem);
                                free(pattern->value);
                                pattern->value = strdup(qualified);
                                if (pattern->node_type) free_type(pattern->node_type);
                                pattern->node_type = clone_type(match_expr_type);
                            }
                        }
                    }
                }

                // Type check the arm body in the new scope. A match-as-
                // expression arm has an EXPRESSION body (an identifier,
                // literal, call, interpolation, ...); typecheck_statement only
                // recurses statement nodes, so a bare-expression body would
                // keep whatever node_type it carried from an earlier pass —
                // which, for an optional `some(v) -> v`, resolves `v` against
                // an unrelated same-named binding instead of the arm's. Route
                // expression bodies through typecheck_expression against
                // arm_table so the pattern bindings resolve correctly; only
                // genuine statement bodies (block / return / print / decl /
                // expression-statement) go through typecheck_statement.
                if (body && body->type != AST_BLOCK &&
                    body->type != AST_EXPRESSION_STATEMENT &&
                    body->type != AST_RETURN_STATEMENT &&
                    body->type != AST_PRINT_STATEMENT &&
                    body->type != AST_VARIABLE_DECLARATION &&
                    body->type != AST_ASSIGNMENT) {
                    typecheck_expression(body, arm_table);
                } else {
                    typecheck_statement(body, arm_table);
                }

                // Propagate arm result type to the match node (for match-as-expression)
                if (!stmt->node_type || stmt->node_type->kind == TYPE_UNKNOWN) {
                    if (body->node_type && body->node_type->kind != TYPE_UNKNOWN) {
                        set_node_type(stmt, clone_type(body->node_type));
                    }
                }

                free_symbol_table(arm_table);
            }

            /* #1377: the opposite of the exhaustiveness check below. When
             * every variant or member is already handled, the `_` arm cannot
             * run. Reported for sums and enums only, where the set of cases
             * is closed and known. */
            if (sum_match && sum_wildcard && sum_nvar > 0 && sum_nvar <= 64) {
                int all = 1;
                for (int vi = 0; vi < sum_nvar; vi++) {
                    if (!sum_covered[vi]) { all = 0; break; }
                }
                if (all && sum_wildcard_line >= 0) {
                    char msg[240];
                    snprintf(msg, sizeof(msg),
                             "unreachable match arm: every variant of sum type "
                             "'%s' is already handled, so the `_` arm cannot run",
                             match_expr_type->struct_name ? match_expr_type->struct_name : "?");
                    AetherError w = {
                        .filename = NULL, .source_code = NULL,
                        .line = sum_wildcard_line, .column = 0,
                        .message = msg,
                        .suggestion = "remove the `_` arm; adding a variant later will then be a compile error rather than silently falling through",
                        .context = NULL, .code = AETHER_WARN_UNREACHABLE_ARM
                    };
                    aether_warning_report(&w);
                    warning_count++;
                }
            }
            if (enum_match && enum_def && enum_wildcard) {
                int nmem = 0, all = 1;
                for (int mi = 0; mi < enum_def->child_count && mi < 64; mi++) {
                    ASTNode* mn = enum_def->children[mi];
                    if (!mn || mn->type != AST_ENUM_MEMBER) continue;
                    nmem++;
                    if (!enum_covered[mi]) { all = 0; break; }
                }
                if (all && nmem > 0 && enum_wildcard_line >= 0) {
                    char msg[240];
                    snprintf(msg, sizeof(msg),
                             "unreachable match arm: every member of enum '%s' "
                             "is already handled, so the `_` arm cannot run",
                             enum_name ? enum_name : "?");
                    AetherError w = {
                        .filename = NULL, .source_code = NULL,
                        .line = enum_wildcard_line, .column = 0,
                        .message = msg,
                        .suggestion = "remove the `_` arm; adding a member later will then be a compile error rather than silently falling through",
                        .context = NULL, .code = AETHER_WARN_UNREACHABLE_ARM
                    };
                    aether_warning_report(&w);
                    warning_count++;
                }
            }

            // #914 exhaustiveness: every variant of the sum must be handled,
            // unless a `_` wildcard arm catches the rest. Missing a variant is
            // a compile error — the payoff that makes adding a variant safe.
            if (sum_match && !sum_wildcard) {
                char missing[256]; int mp = 0, nmiss = 0;
                for (int vi = 0; vi < sum_nvar && vi < 64; vi++) {
                    if (sum_covered[vi]) continue;
                    Type* vt = match_expr_type->tuple_types[vi];
                    const char* vn = (vt && vt->struct_name) ? vt->struct_name : "?";
                    if (nmiss > 0 && mp < (int)sizeof(missing))
                        mp += snprintf(missing + mp, sizeof(missing) - mp, ", ");
                    if (mp < (int)sizeof(missing))
                        mp += snprintf(missing + mp, sizeof(missing) - mp, "%s", vn);
                    nmiss++;
                }
                if (nmiss > 0) {
                    char msg[360];
                    snprintf(msg, sizeof(msg),
                        "non-exhaustive match on sum type '%s': missing variant%s "
                        "%s. Add an arm for each, or a `_` wildcard.",
                        match_expr_type->struct_name ? match_expr_type->struct_name : "?",
                        nmiss > 1 ? "s" : "", missing);
                    type_error(msg, stmt->line, stmt->column);
                }
            }

            // #1044 enum exhaustiveness: every member must be handled unless a
            // `_` catches the rest (mirrors the sum-type check). This turns a
            // non-exhaustive enum match, which otherwise falls through and
            // yields an uninitialized result, into a compile error.
            if (enum_match && enum_def && !enum_wildcard) {
                char missing[256]; int mp = 0, nmiss = 0;
                for (int mi = 0; mi < enum_def->child_count && mi < 64; mi++) {
                    ASTNode* mn = enum_def->children[mi];
                    if (!mn || mn->type != AST_ENUM_MEMBER || !mn->value) continue;
                    if (enum_covered[mi]) continue;
                    if (nmiss > 0 && mp < (int)sizeof(missing))
                        mp += snprintf(missing + mp, sizeof(missing) - mp, ", ");
                    if (mp < (int)sizeof(missing))
                        mp += snprintf(missing + mp, sizeof(missing) - mp, "%s", mn->value);
                    nmiss++;
                }
                if (nmiss > 0) {
                    char msg[360];
                    snprintf(msg, sizeof(msg),
                        "non-exhaustive match on enum '%s': missing member%s "
                        "%s. Add an arm for each, or a `_` wildcard.",
                        enum_name ? enum_name : "?",
                        nmiss > 1 ? "s" : "", missing);
                    type_error(msg, stmt->line, stmt->column);
                }
            }
            return 1;
        }

        case AST_RETURN_STATEMENT:
            // #1044 implicit enum selector: `return Blue` in a function whose
            // return type is an enum resolves the bare member against it. Then
            // typecheck children exactly as the default case does.
            for (int i = 0; i < stmt->child_count; i++) {
                coerce_bare_enum_member(stmt->children[i], g_tc_return_type, table);
                typecheck_node(stmt->children[i], table);
            }
            return 1;

        default:
            // Type check all children
            for (int i = 0; i < stmt->child_count; i++) {
                typecheck_node(stmt->children[i], table);
            }
            return 1;
    }
}

int typecheck_expression(ASTNode* expr, SymbolTable* table) {
    if (!expr) return 0;
    
    switch (expr->type) {
        case AST_BINARY_EXPRESSION:
            return typecheck_binary_expression(expr, table);
            
        case AST_UNARY_EXPRESSION: {
            if (expr->child_count > 0) {
                typecheck_expression(expr->children[0], table);
                expr->node_type = infer_unary_type(expr->children[0],
                                                 get_token_type_from_string(expr->value));
            }
            return 1;
        }

        case AST_NONE_LITERAL:   // #340
            if (!expr->node_type)
                expr->node_type = create_optional_type(create_type(TYPE_UNKNOWN));
            return 1;

        case AST_NULL_COALESCE: {   // #340  opt ?? default -> T
            if (expr->child_count < 2) return 0;
            typecheck_expression(expr->children[0], table);
            typecheck_expression(expr->children[1], table);
            Type* o = infer_type(expr->children[0], table);
            /* `??` accepts EITHER an optional `T?` (value-or-default over
             * presence) OR a fallible `T!` / `(value, err)` tuple (value-or-
             * default over failure — #error-unification P1.3, the same
             * value-yielding lowering as `f() or { default }`). The value
             * type is the optional's element or the tuple's first slot. */
            int is_opt = o && o->kind == TYPE_OPTIONAL;
            int is_result = o && o->kind == TYPE_TUPLE && o->tuple_count >= 2 &&
                            o->tuple_types[0];
            if (!is_opt && !is_result) {
                type_error("`??` requires an optional `T?` or a fallible `T!` "
                           "on its left",
                           expr->line, expr->column);
                if (o) free_type(o);
                if (expr->node_type) free_type(expr->node_type);
                expr->node_type = create_type(TYPE_UNKNOWN);
                return 0;
            }
            Type* value_t = is_opt
                ? (o->element_type ? clone_type(o->element_type)
                                   : create_type(TYPE_UNKNOWN))
                : clone_type(o->tuple_types[0]);
            Type* d = infer_type(expr->children[1], table);
            if (d && value_t->kind != TYPE_UNKNOWN && d->kind != TYPE_UNKNOWN &&
                !is_assignable(d, value_t)) {
                char msg[200];
                snprintf(msg, sizeof(msg),
                    "`??` default type %s is not assignable to the value type %s",
                    type_name(d), type_name(value_t));
                type_error(msg, expr->line, expr->column);
            }
            if (d) free_type(d);
            if (expr->node_type) free_type(expr->node_type);
            expr->node_type = value_t;   // transfer ownership
            /* #error-unification P1.3: `result ?? default` IS `result or
             * { default }` — same value-or-default-over-failure lowering.
             * Rewrite the node to AST_OR_ELSE (a bare-expression handler) so
             * codegen reuses the single, ownership-correct `or` lowering
             * (error-slot free, uniform-heap boxing, heap classification, arg
             * drain) with no duplicated logic. The optional `??` keeps its own
             * codegen (AST_NULL_COALESCE). Left unchanged for is_opt. */
            if (is_result) {
                expr->type = AST_OR_ELSE;
            }
            free_type(o);
            return 1;
        }

        case AST_OPTIONAL_CHAIN: {   // #340  opt?.field -> fieldT?
            if (expr->child_count == 0) return 0;
            typecheck_expression(expr->children[0], table);
            Type* o = infer_type(expr->children[0], table);
            Type* inner = (o && o->kind == TYPE_OPTIONAL) ? o->element_type : NULL;
            const char* sname = NULL;
            if (inner && inner->kind == TYPE_STRUCT) sname = inner->struct_name;
            else if (inner && inner->kind == TYPE_PTR && inner->element_type &&
                     inner->element_type->kind == TYPE_STRUCT)
                sname = inner->element_type->struct_name;
            if (!o || o->kind != TYPE_OPTIONAL || !sname) {
                type_error("optional chaining `?.` requires an optional struct operand",
                           expr->line, expr->column);
                if (o) free_type(o);
                if (expr->node_type) free_type(expr->node_type);
                expr->node_type = create_optional_type(create_type(TYPE_UNKNOWN));
                return 0;
            }
            Type* ft = optional_struct_field_type(table, sname, expr->value);
            if (expr->node_type) free_type(expr->node_type);
            expr->node_type = create_optional_type(ft ? ft : create_type(TYPE_UNKNOWN));
            free_type(o);
            return 1;
        }

        case AST_OR_ELSE: {
            // #913: `fallible or handler`. The LHS must be a fallible
            // `(value, err)` / `T!` expression. A block handler runs when the
            // error slot is non-empty (with `err` bound) and yields a value or
            // exits; a bare expression is a default value. The whole
            // expression's type is the success value type.
            if (expr->child_count < 2) return 0;
            typecheck_expression(expr->children[0], table);
            Type* op = infer_type(expr->children[0], table);
            if (!op || op->kind != TYPE_TUPLE || op->tuple_count < 2) {
                type_error("`or` requires a fallible `(value, err)` / `T!` "
                           "expression on its left", expr->line, expr->column);
                if (op) free_type(op);
                set_node_type(expr, create_type(TYPE_UNKNOWN));
                return 0;
            }
            ASTNode* handler = expr->children[1];
            if (handler && handler->type == AST_BLOCK) {
                SymbolTable* h = create_symbol_table(table);
                add_symbol(h, "err", create_type(TYPE_STRING), 0, 0, 0);
                typecheck_statement(handler, h);
                /* The whole `or` expression yields a value, so a block handler
                 * must either produce one or never fall through. Its LAST
                 * statement must therefore be a value expression (assignable to
                 * the success type — codegen assigns it to the result slot) or
                 * an exit (return / panic / break / continue). Anything else —
                 * an empty block, a trailing println, a trailing if — would
                 * fall through with the result UNINITIALIZED, which is exactly
                 * the miscompile this check closes. Conservative on purpose:
                 * an if/else whose branches both return is rejected too; end
                 * the block with the return or the value instead. */
                ASTNode* lastst = handler->child_count > 0
                                      ? handler->children[handler->child_count - 1]
                                      : NULL;
                int ends_ok = 0;
                if (lastst) {
                    switch (lastst->type) {
                        case AST_RETURN_STATEMENT:
                        case AST_PANIC_STATEMENT:
                        case AST_BREAK_STATEMENT:
                        case AST_CONTINUE_STATEMENT:
                            ends_ok = 1;
                            break;
                        case AST_EXPRESSION_STATEMENT:
                            if (lastst->child_count > 0 && lastst->children[0]) {
                                Type* vt = infer_type(lastst->children[0], h);
                                if (vt && vt->kind != TYPE_UNKNOWN &&
                                    op->tuple_types[0] &&
                                    !is_type_compatible(vt, op->tuple_types[0])) {
                                    char emsg[256];
                                    snprintf(emsg, sizeof(emsg),
                                        "`or { }` handler yields %s where the "
                                        "expression's value type is %s",
                                        type_name(vt), type_name(op->tuple_types[0]));
                                    type_error(emsg, lastst->line, lastst->column);
                                }
                                if (vt) free_type(vt);
                                ends_ok = 1;
                            }
                            break;
                        default:
                            break;
                    }
                }
                if (!ends_ok) {
                    /* Block nodes often carry no position; fall back to the
                     * `or` expression's own so the error points at real code. */
                    int el = handler->line ? handler->line : expr->line;
                    int ec = handler->line ? handler->column : expr->column;
                    type_error("`or { }` handler must end with a value for the "
                               "expression to yield, or exit via return / panic / "
                               "break / continue, otherwise the result would be "
                               "uninitialized on the error path", el, ec);
                }
                free_symbol_table(h);
            } else if (handler) {
                typecheck_expression(handler, table);
            }
            if (expr->node_type) free_type(expr->node_type);
            expr->node_type = op->tuple_types[0] ? clone_type(op->tuple_types[0])
                                                  : create_type(TYPE_UNKNOWN);
            free_type(op);
            return 1;
        }

        case AST_TUPLE_UNWRAP: {
            /* `expr!` — unwrap-or-trap. The operand must return a tuple
             * whose LAST slot is the (string) error; the unwrap yields
             * the first slot and panics at runtime if the error slot is
             * non-empty. Typecheck the operand, then require a tuple of
             * at least 2 elements with a string final slot — the
             * canonical `(value, err)` shape this sugar targets. */
            if (expr->child_count == 0) return 0;
            typecheck_expression(expr->children[0], table);
            Type* op = infer_type(expr->children[0], table);
            /* #340: optional force-unwrap `opt!` -> inner `T` (panics at
             * runtime on `none`). Polymorphic with the tuple unwrap below. */
            if (op && op->kind == TYPE_OPTIONAL) {
                if (expr->node_type) free_type(expr->node_type);
                expr->node_type = op->element_type ? clone_type(op->element_type)
                                                   : create_type(TYPE_UNKNOWN);
                free_type(op);
                return 1;
            }
            if (!op || op->kind != TYPE_TUPLE) {
                type_error("`!` requires an optional `T?` or a tuple "
                           "`(value, err)` operand",
                           expr->line, expr->column);
                if (op) free_type(op);
                expr->node_type = create_type(TYPE_UNKNOWN);
                return 0;
            }
            if (op->tuple_count < 2) {
                type_error("`!` unwrap requires a `(value, err)` tuple "
                           "with at least two slots",
                           expr->line, expr->column);
                free_type(op);
                set_node_type(expr, create_type(TYPE_UNKNOWN));
                return 0;
            }
            Type* last = op->tuple_types[op->tuple_count - 1];
            if (!last || last->kind != TYPE_STRING) {
                type_error("`!` unwrap requires the final tuple slot to be "
                           "the `string` error of a `(value, err)` result",
                           expr->line, expr->column);
                free_type(op);
                set_node_type(expr, create_type(TYPE_UNKNOWN));
                return 0;
            }
            set_node_type(expr, clone_type(op->tuple_types[0]));
            free_type(op);
            return 1;
        }

        case AST_HEAP_NEW: {
            /* heap.new(T) — zero-init heap allocation of a POD struct.
             * Validate: (a) T names a struct; (b) it is POD — no `string`
             * (or other heap-managed) field. The string gate mirrors the
             * existing destructor predicate `struct_has_heap_string_field`:
             * a struct with a heap field needs an ownership model before
             * it can be boxed (issue #564), so reject it at compile time
             * with a clear message rather than minting a UAF/leak footgun. */
            if (!expr->value) {
                type_error("heap.new requires a struct type name",
                           expr->line, expr->column);
                set_node_type(expr, create_type(TYPE_UNKNOWN));
                return 0;
            }
            Symbol* struct_sym = lookup_symbol(table, expr->value);
            if (!struct_sym || !struct_sym->type ||
                struct_sym->type->kind != TYPE_STRUCT) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "heap.new(%s), '%s' is not a struct type",
                    expr->value, expr->value);
                type_error(msg, expr->line, expr->column);
                set_node_type(expr, create_type(TYPE_UNKNOWN));
                return 0;
            }
            /* #790: a struct with `string` fields IS allowed. The box owns
             * its string fields under the same ownership model value structs
             * already use: each string field carries a hidden `_heap_<field>`
             * tracker (codegen_func.c), a field store adopts a heap string and
             * sets the tracker, and `heap.free(p)` routes through the generated
             * `<Name>_heap_free` (release every owned field, then free the box).
             * calloc in heap.new zero-inits the trackers, so an unset field is
             * never mistaken for owned. No POD gate to apply. */
            /* Result type: *T (TYPE_PTR -> TYPE_STRUCT{name}). */
            if (expr->node_type) free_type(expr->node_type);
            Type* inner = create_type(TYPE_STRUCT);
            inner->struct_name = strdup(expr->value);
            expr->node_type = create_type(TYPE_PTR);
            expr->node_type->element_type = inner;
            return 1;
        }

        case AST_FUNCTION_CALL:
            return typecheck_function_call(expr, table);
            
        case AST_IDENTIFIER: {
            Symbol* symbol = lookup_symbol(table, expr->value);
            if (!symbol) {
                char error_msg[256];
                if (expr->value && name_blocked_by_hide(table, expr->value)) {
                    snprintf(error_msg, sizeof(error_msg),
                             "'%s' is hidden in this scope by `hide` or `seal except`",
                             expr->value);
                } else {
                    snprintf(error_msg, sizeof(error_msg), "Undefined variable '%s'", expr->value ? expr->value : "?");
                }
                type_error(error_msg, expr->line, expr->column);
                return 0;
            }
            set_node_type(expr, symbol->type ? clone_type(symbol->type)
                                             : create_type(TYPE_UNKNOWN));
            return 1;
        }

        case AST_LITERAL:
            // Literals are already typed
            return 1;

        case AST_SIZEOF:
        case AST_OFFSETOF:
            // Layout builtins. The child of OFFSETOF is the field name,
            // which is NOT an expression to resolve — do not recurse
            // (the default case would treat it as an undefined variable).
            // Field/type validity is the C compiler's job (offsetof on a
            // bad field is a hard C error), matching the trust-the-author
            // posture of the `as *Struct` cast.
            set_node_type(expr, create_type(TYPE_INT));
            return 1;

        case AST_BITSET_LITERAL: {
            // #1046 `bit_set[E]{ E.A, E.B }`. The element type must resolve to an
            // enum (resolve_enum_types rewrites TYPE_STRUCT{E} -> TYPE_ENUM), and
            // every member must belong to that same enum. The members were
            // normalized to `E.Member` at parse time and rewritten to the enum
            // constant `E_Member` (node_type TYPE_ENUM{E}) by enum resolution.
            Type* bs = expr->node_type;
            Type* elem = bs ? bs->element_type : NULL;
            if (!elem || elem->kind != TYPE_ENUM) {
                type_error("bit_set element type must be an enum, e.g. bit_set[Color]",
                           expr->line, expr->column);
                return 0;
            }
            for (int i = 0; i < expr->child_count; i++) {
                ASTNode* m = expr->children[i];
                // Members were resolved to enum constants (node_type TYPE_ENUM)
                // by resolve_enum_types; just validate they name this enum.
                if (!m->node_type || m->node_type->kind != TYPE_ENUM ||
                    !types_equal(m->node_type, elem)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                        "bit_set member must be a value of enum '%s'",
                        elem->struct_name ? elem->struct_name : "?");
                    type_error(msg, m->line, m->column);
                    return 0;
                }
            }
            return 1;
        }

        case AST_BITSET_CARD: {
            // #1046 `card(s)`: s must be a bit_set; the count is an int.
            if (expr->child_count < 1) return 0;
            typecheck_expression(expr->children[0], table);
            Type* at = infer_type(expr->children[0], table);
            if (!at || at->kind != TYPE_BITSET) {
                type_error("card() requires a bit_set argument", expr->line, expr->column);
                if (at) free_type(at);
                set_node_type(expr, create_type(TYPE_INT));
                return 0;
            }
            free_type(at);
            set_node_type(expr, create_type(TYPE_INT));
            return 1;
        }

        case AST_VA_START:
            // No children; opaque va_list cookie (ptr).
            set_node_type(expr, create_type(TYPE_PTR));
            return 1;

        case AST_VA_ARG:
            // children[0] is the cookie expr — typecheck it; the result
            // type was fixed by the parser from the requested C type.
            if (expr->child_count > 0) {
                typecheck_expression(expr->children[0], table);
            }
            if (!expr->node_type) expr->node_type = create_type(TYPE_PTR);
            return 1;

        case AST_VA_END:
            if (expr->child_count > 0) {
                typecheck_expression(expr->children[0], table);
            }
            set_node_type(expr, create_type(TYPE_VOID));
            return 1;

        case AST_IF_EXPRESSION:
            // Typecheck all children (condition, then-expr, else-expr)
            for (int i = 0; i < expr->child_count; i++) {
                typecheck_expression(expr->children[i], table);
            }
            if (expr->child_count >= 2) {
                Type* then_type = infer_type(expr->children[1], table);
                if (!expr->node_type || expr->node_type->kind == TYPE_UNKNOWN) {
                    if (expr->node_type) free_type(expr->node_type);
                    expr->node_type = clone_type(then_type);
                }
                free_type(then_type);
            }
            return 1;

        case AST_NULL_LITERAL:
            // null is always TYPE_PTR
            if (!expr->node_type) expr->node_type = create_type(TYPE_PTR);
            return 1;

        case AST_PTR_AS_STRUCT_CAST:
            /* Walk the operand, then set our own node_type via the
             * shared inference path so codegen sees the
             * TYPE_PTR{element=TYPE_STRUCT} on this node and emits
             * `->field` for downstream member access. */
            if (expr->child_count > 0) {
                typecheck_expression(expr->children[0], table);
            }
            if (expr->node_type) free_type(expr->node_type);
            expr->node_type = infer_type(expr, table);
            return 1;

        case AST_PTR_AS_ARRAY_CAST:
            /* Walk the operand; keep the parser-populated node_type
             * (TYPE_ARRAY with element_type) so codegen emits the
             * correct C element-pointer cast `((T*)(expr))`. */
            if (expr->child_count > 0) {
                typecheck_expression(expr->children[0], table);
            }
            return 1;

        case AST_VALUE_CAST: {
            /* `expr as T` (#480) — a zero-cost nominal (un)wrap or numeric
             * conversion. Validate it's a sensible value cast: the operand and
             * target share a machine `kind` (distinct (un)wrap) or are both
             * numeric (a numeric conversion). The target carries the result
             * type (distinct-resolved). */
            if (expr->child_count > 0) typecheck_expression(expr->children[0], table);
            Type* operand = expr->child_count > 0 ? infer_type(expr->children[0], table) : NULL;
            if (operand && expr->node_type) {
                int same = (operand->kind == expr->node_type->kind);
                int numeric = is_numeric_scalar(operand->kind) &&
                              is_numeric_scalar(expr->node_type->kind);
                /* #1132: a bitstruct never converts IMPLICITLY (is_type_compatible
                 * keeps it strictly nominal), but `as` is exactly how you cross the
                 * boundary on purpose: `w as Flags` to wrap a raw word, `f as
                 * uint8_t` to get it back. Both directions are zero-cost — the
                 * bitstruct IS the integer at the C level. Two different bitstructs
                 * do NOT convert into each other, even over the same backing type;
                 * that would silently reinterpret one layout as another. */
                int bs_cast = 0;
                if (operand->kind == TYPE_BITSTRUCT && expr->node_type->kind != TYPE_BITSTRUCT)
                    bs_cast = is_integer_scalar(expr->node_type->kind);
                else if (expr->node_type->kind == TYPE_BITSTRUCT && operand->kind != TYPE_BITSTRUCT)
                    bs_cast = is_integer_scalar(operand->kind);
                if (!same && !numeric && !bs_cast) {
                    char msg[220];
                    snprintf(msg, sizeof(msg),
                        "cannot cast %s to %s with `as`: a value cast converts "
                        "between a distinct type and its base, or between numeric "
                        "types", type_name(operand), type_name(expr->node_type));
                    type_error(msg, expr->line, expr->column);
                }
            }
            if (operand) free_type(operand);
            return 1;
        }

        case AST_PTR_AS_FN_CAST:
            /* Walk the operand; keep the parser-populated node_type
             * (TYPE_FUNCTION with signature) so the call-site codegen
             * can read the param/return types off it. */
            if (expr->child_count > 0) {
                typecheck_expression(expr->children[0], table);
            }
            /* Don't replace expr->node_type — the parser set it from
             * the `fn(...)` source spelling and it carries the exact
             * Type* tree we need for codegen. */
            return 1;

        case AST_ARRAY_LITERAL:
            // Type check all array elements
            for (int i = 0; i < expr->child_count; i++) {
                typecheck_expression(expr->children[i], table);
            }
            return 1;

        case AST_TUPLE_LITERAL: {
            /* #1033: `(a, b, ...)` — element expressions typecheck here;
             * the extern-call argument check validates shape (element
             * count, scalar element kinds) against the tuple-typed
             * parameter and stamps expr->node_type with the param's
             * tuple type. Anywhere else the literal is left untyped and
             * the consuming context reports its own mismatch. */
            for (int i = 0; i < expr->child_count; i++) {
                typecheck_expression(expr->children[i], table);
            }
            return 1;
        }

        case AST_NAMED_ARG:
            // Named argument: type check the value expression
            if (expr->child_count > 0) {
                typecheck_expression(expr->children[0], table);
                expr->node_type = expr->children[0]->node_type
                    ? clone_type(expr->children[0]->node_type)
                    : create_type(TYPE_UNKNOWN);
            }
            return 1;

        case AST_STRING_INTERP:
            /* Type check each sub-expression, then check it is a KIND that
             * can actually be rendered -- see check_interp_operand.
             *
             * NB a diagnostic from here prints more than once. That is
             * pre-existing and general (an unrelated `as *Missing` error
             * repeats identically), not something this check introduces. */
            for (int i = 0; i < expr->child_count; i++) {
                typecheck_expression(expr->children[i], table);
                check_interp_operand(expr->children[i], table,
                                     expr->line, expr->column);
            }
            set_node_type(expr, create_type(TYPE_STRING));
            return 1;

        case AST_CLOSURE: {
            // Create a child scope for the closure's parameters
            SymbolTable* closure_scope = create_symbol_table(table);

            // Issue #333: when typecheck_function_call stamped a
            // `dsl_recv:<name>` annotation on this closure (because
            // the closure is the trailing block of a member-access
            // call), seed the closure scope's dsl_receiver. lookup_symbol
            // uses it to resolve unqualified calls inside the body
            // through the `<receiver>_<name>` rewrite.
            if (expr->annotation &&
                strncmp(expr->annotation, "dsl_recv:", 9) == 0) {
                const char* recv = expr->annotation + 9;
                if (*recv) {
                    closure_scope->dsl_receiver = strdup(recv);
                }
            }

            // Register closure parameters in the child scope
            for (int i = 0; i < expr->child_count; i++) {
                ASTNode* child = expr->children[i];
                if (child && child->type == AST_CLOSURE_PARAM && child->value) {
                    Type* ptype = child->node_type ? clone_type(child->node_type)
                                                   : create_type(TYPE_INT); // default to int
                    add_symbol(closure_scope, child->value, ptype, 0, 0, 0);
                }
            }

            /* Type-check the closure body via `typecheck_statement`.
             *
             * A closure body is a sequence of statements, so it must be
             * checked the same way a function body is — through
             * `typecheck_statement`, which registers declarations
             * (plain `AST_VARIABLE_DECLARATION` and `AST_TUPLE_
             * DESTRUCTURE` alike) into the scope it is given, and
             * recurses correctly into `if` / `while` / `for` / nested
             * blocks with their own sub-scopes.
             *
             * The body was previously walked with `typecheck_
             * expression`, which has no statement cases — it fell to
             * the generic child-walk and registered nothing, so every
             * declared name inside the closure was `E0300 Undefined
             * variable` at its later uses. Earlier point fixes patched
             * one statement kind at a time at the body's top level
             * (the `AST_TUPLE_DESTRUCTURE` special-case from
             * tuple-destructure-in-closure-scope.md); a plain `x = e`
             * nested inside an `if` within the closure still slipped
             * through. Routing the whole body through
             * `typecheck_statement` closes the class — see
             * new_string_len_something.md §4. */
            for (int i = 0; i < expr->child_count; i++) {
                ASTNode* child = expr->children[i];
                if (child && child->type == AST_BLOCK) {
                    for (int j = 0; j < child->child_count; j++) {
                        typecheck_statement(child->children[j], closure_scope);
                    }
                }
            }

            free_symbol_table(closure_scope);

            if (!expr->node_type) {
                expr->node_type = create_type(TYPE_FUNCTION);
            }
            return 1;
        }

        case AST_ARRAY_ACCESS:
            // Type check array access — validate index is integer and
            // propagate the element type onto this node so downstream
            // consumers (print format specifiers, string interpolation,
            // further expressions) know what type an access yields. Without
            // this, arr[i] has no node_type and defaults to %d even when
            // arr is string[], producing wrong format specifiers and UB.
            if (expr->child_count >= 2) {
                typecheck_expression(expr->children[0], table);
                typecheck_expression(expr->children[1], table);
                Type* arr_type = expr->children[0]->node_type;
                Type* idx_type = infer_type(expr->children[1], table);
                if (arr_type && arr_type->kind == TYPE_ARRAY && arr_type->index_enum_name) {
                    // Index must be a value of the array's index enum, not a raw int.
                    if (idx_type && idx_type->kind != TYPE_UNKNOWN &&
                        !(idx_type->kind == TYPE_ENUM && idx_type->struct_name &&
                          strcmp(idx_type->struct_name, arr_type->index_enum_name) == 0)) {
                        char error_msg[256];
                        snprintf(error_msg, sizeof(error_msg),
                                 "index into `[%s]...` must be a %s value, got %s",
                                 arr_type->index_enum_name, arr_type->index_enum_name,
                                 type_name(idx_type));
                        type_error(error_msg, expr->line, expr->column);
                    }
                } else if (idx_type && idx_type->kind != TYPE_INT &&
                           idx_type->kind != TYPE_INT64 &&
                           idx_type->kind != TYPE_UNKNOWN &&
                           idx_type->kind != TYPE_ENUM) {
                    // Ordinary array: an integer index (an enum is int-backed).
                    char error_msg[256];
                    snprintf(error_msg, sizeof(error_msg),
                             "Array index must be an integer, got %s",
                             type_name(idx_type));
                    type_error(error_msg, expr->line, expr->column);
                }
                if (idx_type) free_type(idx_type);

                // Propagate element type from the array expression.
                if (arr_type && arr_type->kind == TYPE_ARRAY && arr_type->element_type &&
                    (!expr->node_type || expr->node_type->kind == TYPE_UNKNOWN)) {
                    if (expr->node_type) free_type(expr->node_type);
                    expr->node_type = clone_type(arr_type->element_type);
                }
            }
            return 1;

        case AST_STRUCT_LITERAL:
            // Type check struct literal field initializers
            for (int i = 0; i < expr->child_count; i++) {
                ASTNode* field_init = expr->children[i];
                if (field_init && field_init->type == AST_ASSIGNMENT && field_init->child_count > 0) {
                    typecheck_expression(field_init->children[0], table);
                }
            }
            // Struct literal type is already set during type inference
            return 1;
            
        case AST_MEMBER_ACCESS: {
            /* #1132: a bitstruct's fields are a closed, declared set, so a name
             * that isn't one of them is a typo, not an open question. Reject it
             * here (this walk always runs) rather than letting codegen fall back
             * to emitting a literal 0 with an "unknown field" comment. */
            if (expr->child_count > 0 && expr->children[0] && expr->value) {
                Type* bt = infer_type(expr->children[0], table);
                if (bt && bt->kind == TYPE_BITSTRUCT && bt->struct_name &&
                    !bitstruct_field_lookup(g_bitstruct_program, bt->struct_name,
                                            expr->value)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "'%s' is not a field of bitstruct '%s'",
                             expr->value, bt->struct_name);
                    type_error(msg, expr->line, expr->column);
                    free_type(bt);
                    return 0;
                }
                if (bt) free_type(bt);
            }
            // Namespace-qualified constant access: mymath.PI_APPROX -> mymath_PI_APPROX
            // Rewrite AST to AST_IDENTIFIER so codegen emits the C variable name directly.
            // Issue #243: gate on the strict per-scope visibility check
            // so user code can't reach into transitively-merged consts.
            //
            // A same-named in-scope VALUE (local / param / global) shadows the
            // namespace: if `flags` is a bound variable, `flags.field` is a value
            // member access, never a module-const reference. Without this guard a
            // module named `flags` whose own (merged) body has a local `flags`
            // has its `flags.field` mis-resolved as `flags_field` const lookup ->
            // spurious "module 'flags' has no export 'field'". Skip the namespace
            // branch when the base resolves to a value symbol so struct-field
            // resolution below takes precedence.
            if (expr->child_count > 0 && expr->children[0] &&
                expr->children[0]->type == AST_IDENTIFIER && expr->children[0]->value &&
                !lookup_symbol(table, expr->children[0]->value) &&
                is_visible_namespace(expr->children[0]->value, table) && expr->value) {
                // Enforce export visibility for constants
                if (is_export_blocked(expr->children[0]->value, expr->value)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "'%s' is not exported from module '%s'",
                             expr->value, expr->children[0]->value);
                    type_error(msg, expr->line, expr->column);
                    return 0;
                }
                char qualified[512];
                snprintf(qualified, sizeof(qualified), "%s_%s",
                         expr->children[0]->value, expr->value);
                Symbol* sym = lookup_symbol(table, qualified);
                /* #924 re-export: `hub.X` where hub lists X in `exports` but
                 * imports it from another module. The local `hub_X` symbol
                 * doesn't exist; redirect to the defining module's symbol
                 * `<origin>_X`. */
                if ((!sym || !sym->type) && global_module_registry) {
                    AetherModule* hub = module_find(expr->children[0]->value);
                    AetherModule* origin = module_resolve_reexport(hub, expr->value);
                    if (origin && origin->name) {
                        snprintf(qualified, sizeof(qualified), "%s_%s",
                                 origin->name, expr->value);
                        sym = lookup_symbol(table, qualified);
                    }
                }
                if (sym && sym->type) {
                    // Rewrite node in-place
                    expr->type = AST_IDENTIFIER;
                    free(expr->value);
                    expr->value = strdup(qualified);
                    set_node_type(expr, clone_type(sym->type));
                    return 1;
                }
                // The prefix IS a visible imported namespace, but `prefix.member`
                // resolves to no value/const. Without this, the node falls
                // through to base-expression handling, where `prefix` can't
                // resolve as a value and the error is the misleading
                // `Undefined variable '<prefix>'` — it points at the module,
                // not the member, which sends you down the wrong path (the
                // module clearly IS imported; its functions resolve). This
                // most often bites a `.so`-backed import (`foo.SOME_CONST`
                // against a library whose ABI omits that name). A bare module
                // reference (`println(foo)`) legitimately has no member, so
                // only fire when this is an actual `prefix.member` access.
                // (Non-exported *functions* are reported separately and
                // already name `prefix.member` — this covers the value path.)
                {
                    char msg[320];
                    snprintf(msg, sizeof(msg),
                             "module '%s' has no export '%s' "
                             "(not part of the module's API / library ABI)",
                             expr->children[0]->value, expr->value);
                    type_error(msg, expr->line, expr->column);
                    return 0;
                }
            }
            // Type check member access (e.g., msg.type, struct.field)
            if (expr->child_count > 0) {
                ASTNode* base = expr->children[0];
                typecheck_expression(base, table);

                Type* base_type = infer_type(base, table);

                if (base_type && base_type->kind == TYPE_DURATION) {
                    if (expr->value && strcmp(expr->value, "ns") == 0) {
                        set_node_type(expr, create_type(TYPE_INT64));
                    } else if (expr->value &&
                               (strcmp(expr->value, "us") == 0 || strcmp(expr->value, "ms") == 0 ||
                                strcmp(expr->value, "s") == 0 || strcmp(expr->value, "m") == 0 ||
                                strcmp(expr->value, "h") == 0 || strcmp(expr->value, "d") == 0)) {
                        set_node_type(expr, create_type(TYPE_FLOAT));
                    } else {
                        char error_msg[256];
                        snprintf(error_msg, sizeof(error_msg),
                                 "Type 'Duration' has no field '%s'",
                                 expr->value ? expr->value : "?");
                        free_type(base_type);
                        type_error(error_msg, expr->line, expr->column);
                        return 0;
                    }
                    free_type(base_type);
                    return 1;
                }

                // Reject member access on primitive types — catch the error in Aether, not C
                if (base_type && (base_type->kind == TYPE_INT || base_type->kind == TYPE_FLOAT ||
                                  base_type->kind == TYPE_BOOL || base_type->kind == TYPE_STRING)) {
                    char error_msg[256];
                    snprintf(error_msg, sizeof(error_msg),
                             "Type '%s' has no field '%s'",
                             type_name(base_type), expr->value ? expr->value : "?");
                    free_type(base_type);
                    type_error(error_msg, expr->line, expr->column);
                    return 0;
                }

                // Handle Message type member access
                if (base_type && base_type->kind == TYPE_MESSAGE) {
                    if (strcmp(expr->value, "type") == 0 ||
                        strcmp(expr->value, "sender_id") == 0 ||
                        strcmp(expr->value, "payload_int") == 0) {
                        set_node_type(expr, create_type(TYPE_INT));
                    } else if (strcmp(expr->value, "payload_ptr") == 0) {
                        set_node_type(expr, create_type(TYPE_VOID));
                    }
                }
                // Handle actor ref member access — look up state field type from actor definition
                else if (base_type && base_type->kind == TYPE_ACTOR_REF && base_type->element_type &&
                         base_type->element_type->kind == TYPE_STRUCT && base_type->element_type->struct_name) {
                    Symbol* actor_sym2 = lookup_symbol(table, base_type->element_type->struct_name);
                    if (actor_sym2 && actor_sym2->node) {
                        ASTNode* actor_def2 = actor_sym2->node;
                        for (int fi = 0; fi < actor_def2->child_count; fi++) {
                            ASTNode* field = actor_def2->children[fi];
                            if (field && field->type == AST_STATE_DECLARATION &&
                                field->value && strcmp(field->value, expr->value) == 0) {
                                if (field->node_type && field->node_type->kind != TYPE_UNKNOWN) {
                                    set_node_type(expr, clone_type(field->node_type));
                                }
                                break;
                            }
                        }
                    }
                    // Fallback to general inference
                    if (!expr->node_type || expr->node_type->kind == TYPE_UNKNOWN) {
                        expr->node_type = infer_type(expr, table);
                    }
                }
                // Handle struct member access — look up field type from definition.
                // For `extern struct` types whose fields include union/nested
                // compounds, we need to walk children with awareness of the
                // compound shape — see lookup_extern_field_type in this file.
                else if (base_type && base_type->kind == TYPE_STRUCT && base_type->struct_name) {
                    // Anonymous compound — base_type.compound_node points
                    // directly at the AST_STRUCT_FIELD_UNION / _NESTED node
                    // produced from a previous member access. The compound's
                    // children ARE the lookup list.
                    ASTNode* children_owner = NULL;
                    if (base_type->compound_node) {
                        children_owner = base_type->compound_node;
                    } else {
                        Symbol* struct_sym = lookup_symbol(table, base_type->struct_name);
                        if (struct_sym && struct_sym->node) children_owner = struct_sym->node;
                    }
                    if (children_owner) {
                        int found = 0;
                        for (int fi = 0; fi < children_owner->child_count; fi++) {
                            ASTNode* field = children_owner->children[fi];
                            if (!field || !field->value) continue;
                            if (strcmp(field->value, expr->value) != 0) continue;
                            if (field->type == AST_STRUCT_FIELD_UNION ||
                                field->type == AST_STRUCT_FIELD_NESTED) {
                                // Compound child: synthesize a TYPE_STRUCT
                                // whose compound_node points at THIS field
                                // so the next access can walk its children.
                                Type* ct = create_type(TYPE_STRUCT);
                                ct->struct_name = strdup("__AeCompound");
                                ct->compound_node = field;
                                expr->node_type = ct;
                            } else if (field->node_type && field->node_type->kind != TYPE_UNKNOWN) {
                                set_node_type(expr, clone_type(field->node_type));
                            }
                            found = 1;
                            break;
                        }
                        // #1048: no direct field, so try a `using`-embedded one.
                        if (!found) {
                            Type* uft = try_resolve_using_field(table, children_owner, expr);
                            if (uft) { free_type(uft); found = 1; }
                        }
                        if (!found) {
                            char error_msg[256];
                            snprintf(error_msg, sizeof(error_msg),
                                     "Struct '%s' has no field '%s'",
                                     base_type->struct_name, expr->value ? expr->value : "?");
                            free_type(base_type);
                            type_error(error_msg, expr->line, expr->column);
                            return 0;
                        }
                    }
                    // Fallback to general inference
                    if (!expr->node_type || expr->node_type->kind == TYPE_UNKNOWN) {
                        expr->node_type = infer_type(expr, table);
                    }
                }
                // Pointer-to-struct member access: `e.field` where e: *Foo
                // Look through the TYPE_PTR to the underlying struct's
                // field list, identical to TYPE_STRUCT above. Without
                // this branch the field's declared type (e.g. `string`)
                // never landed on the AST_MEMBER_ACCESS node, leaving
                // string-interpolation codegen guessing — which printed
                // string fields as `%d` (the int default in the
                // interp's TypeKind switch) rather than `%s`. Issue
                // surfaces on self-referential structs walked through
                // a `*Self` chain. Section C.1 of fresh-aether-requests.
                else if (base_type && base_type->kind == TYPE_PTR &&
                         base_type->element_type &&
                         base_type->element_type->kind == TYPE_STRUCT &&
                         base_type->element_type->struct_name) {
                    Symbol* struct_sym = lookup_symbol(table, base_type->element_type->struct_name);
                    if (struct_sym && struct_sym->node) {
                        ASTNode* struct_def = struct_sym->node;
                        int found = 0;
                        for (int fi = 0; fi < struct_def->child_count; fi++) {
                            ASTNode* field = struct_def->children[fi];
                            if (!field || !field->value) continue;
                            if (strcmp(field->value, expr->value) != 0) continue;
                            if (field->type == AST_STRUCT_FIELD_UNION ||
                                field->type == AST_STRUCT_FIELD_NESTED) {
                                Type* ct = create_type(TYPE_STRUCT);
                                ct->struct_name = strdup("__AeCompound");
                                ct->compound_node = field;
                                expr->node_type = ct;
                            } else if (field->node_type && field->node_type->kind != TYPE_UNKNOWN) {
                                set_node_type(expr, clone_type(field->node_type));
                            }
                            found = 1;
                            break;
                        }
                        // #1048: no direct field, so try a `using`-embedded one.
                        if (!found) {
                            Type* uft = try_resolve_using_field(table, struct_def, expr);
                            if (uft) { free_type(uft); found = 1; }
                        }
                        if (!found) {
                            char error_msg[256];
                            snprintf(error_msg, sizeof(error_msg),
                                     "Struct '%s' has no field '%s'",
                                     base_type->element_type->struct_name,
                                     expr->value ? expr->value : "?");
                            free_type(base_type);
                            type_error(error_msg, expr->line, expr->column);
                            return 0;
                        }
                    }
                    if (!expr->node_type || expr->node_type->kind == TYPE_UNKNOWN) {
                        expr->node_type = infer_type(expr, table);
                    }
                }
                free_type(base_type);
            }
            return 1;
        }
            
        case AST_SEND_FIRE_FORGET: {
            // actor ! MessageType { fields... }  — validate both operands
            if (expr->child_count >= 2) {
                ASTNode* actor_ref = expr->children[0];
                ASTNode* message   = expr->children[1];

                typecheck_expression(actor_ref, table);

                // Validate that the message type is a registered message definition
                if (message->type == AST_MESSAGE_CONSTRUCTOR && message->value) {
                    Symbol* msg_sym = lookup_symbol(table, message->value);
                    if (!msg_sym || !msg_sym->type || msg_sym->type->kind != TYPE_MESSAGE) {
                        char error_msg[256];
                        snprintf(error_msg, sizeof(error_msg),
                                 "Undefined message type '%s'", message->value);
                        type_error(error_msg, message->line, message->column);
                        return 0;
                    }
                }

                // Validate field value types match declared field types
                typecheck_message_constructor(message, table);
            }
            set_node_type(expr, create_type(TYPE_VOID));
            return 1;
        }

        default:
            // Type check all children
            for (int i = 0; i < expr->child_count; i++) {
                typecheck_expression(expr->children[i], table);
            }
            return 1;
    }
}

int typecheck_binary_expression(ASTNode* expr, SymbolTable* table) {
    if (!expr || expr->type != AST_BINARY_EXPRESSION || expr->child_count < 2) return 0;
    
    ASTNode* left = expr->children[0];
    ASTNode* right = expr->children[1];

    // #1044 implicit enum selector across an enum operator (`c == Blue`,
    // `Blue == c`, `c = Blue`). When one side is a bare identifier that resolves
    // to no symbol and the other side is an enum value, coerce the bare side to
    // that enum's member constant before either side is typechecked, so the bare
    // member is not first reported as an undefined variable. The already-checked
    // side is re-checked below (idempotent).
    {
        int l_bare = (left->type == AST_IDENTIFIER && left->value &&
                      !lookup_symbol(table, left->value));
        int r_bare = (right->type == AST_IDENTIFIER && right->value &&
                      !lookup_symbol(table, right->value));
        if (l_bare && !r_bare) {
            typecheck_expression(right, table);
            Type* rt = infer_type(right, table);
            coerce_bare_enum_member(left, rt, table);
            if (rt) free_type(rt);
        } else if (r_bare && !l_bare) {
            typecheck_expression(left, table);
            Type* lt = infer_type(left, table);
            coerce_bare_enum_member(right, lt, table);
            if (lt) free_type(lt);
        }
    }

    typecheck_expression(left, table);
    typecheck_expression(right, table);

    Type* left_type = infer_type(left, table);
    Type* right_type = infer_type(right, table);

    AeTokenType operator = get_token_type_from_string(expr->value);

    // #340: equality with `none` / between optionals — `m == none`,
    // `none == m`, `a? == b?` all yield bool. Pin a bare `none` operand to the
    // other side's concrete optional type so codegen emits the right compare.
    if (operator == TOKEN_EQUALS || operator == TOKEN_NOT_EQUALS) {
        int l_opt  = left_type  && left_type->kind  == TYPE_OPTIONAL;
        int r_opt  = right_type && right_type->kind == TYPE_OPTIONAL;
        int l_none = left->type  == AST_NONE_LITERAL;
        int r_none = right->type == AST_NONE_LITERAL;
        if (l_opt || r_opt || l_none || r_none) {
            if (r_none && l_opt && (!right->node_type ||
                right->node_type->kind != TYPE_OPTIONAL || !right->node_type->element_type)) {
                if (right->node_type) free_type(right->node_type);
                right->node_type = clone_type(left_type);
            }
            if (l_none && r_opt && (!left->node_type ||
                left->node_type->kind != TYPE_OPTIONAL || !left->node_type->element_type)) {
                if (left->node_type) free_type(left->node_type);
                left->node_type = clone_type(right_type);
            }
            free_type(left_type);
            free_type(right_type);
            set_node_type(expr, create_type(TYPE_BOOL));
            return 1;
        }
    }

    // Reject `string + string` at typecheck rather than emitting
    // invalid C (`(const char*) + (const char*)` is a pointer-arith
    // error, not a useful diagnostic to anyone). Aether doesn't
    // overload `+` for strings; the idiomatic ways to join strings
    // are interpolation `"${a}${b}"` (literal-time) or
    // `string.concat(a, b)` / `string.format(fmt, args)` (runtime).
    // Closes #276.
    if (operator == TOKEN_PLUS &&
        left_type && left_type->kind == TYPE_STRING &&
        right_type && right_type->kind == TYPE_STRING) {
        free_type(left_type);
        free_type(right_type);
        type_error("'+' is not defined for strings, use \"${a}${b}\" interpolation or string.concat(a, b)",
                   expr->line, expr->column);
        return 0;
    }

    if (operator == TOKEN_ASSIGN) {
        if (!is_assignable(right_type, left_type)) {
            /* #1240: `table.callback = my_fn` where the field is declared
             * `fn(T...) -> R`. A bare function name infers its RETURN type
             * (that is what a function symbol carries), so the plain
             * compatibility test compares `int` against `fn(ptr) -> int` and
             * rejects the one value a C function-pointer field can actually
             * hold. Accept it only when the signature matches, so the vtable
             * slot stays typed rather than merely pointer-shaped, and stamp the
             * RHS with the field's type so codegen emits the raw symbol instead
             * of boxing it into an _AeClosure. */
            if (left_type && left_type->kind == TYPE_FUNCTION &&
                fn_name_matches_signature(table, right, left_type)) {
                if (right->node_type) free_type(right->node_type);
                right->node_type = clone_type(left_type);
                free_type(right_type);
                right_type = clone_type(left_type);
            } else {
                free_type(left_type);
                free_type(right_type);
                type_error("Type mismatch in assignment", expr->line, expr->column);
                return 0;
            }
        }
        /* `b = <int literal>` where b: byte — same range check as the
         * declaration / AST_ASSIGNMENT paths. The plain `=` operator
         * parses as AST_BINARY_EXPRESSION, so we need the check here
         * too. */
        if (left_type && left_type->kind == TYPE_BYTE &&
            byte_assignment_literal_out_of_range(right)) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                "byte literal out of range: %s does not fit in 0..255",
                right->value ? right->value : "?");
            type_error(msg, expr->line, expr->column);
            free_type(left_type);
            free_type(right_type);
            return 0;
        }
        set_node_type(expr, clone_type(left_type));
    } else {
        Type* result_type = infer_binary_type(left, right, operator);
        if (result_type->kind == TYPE_UNKNOWN &&
            left_type && left_type->kind != TYPE_UNKNOWN &&
            right_type && right_type->kind != TYPE_UNKNOWN) {
            // Only error if both types are known but incompatible
            free_type(left_type);
            free_type(right_type);
            free_type(result_type);
            type_error("Invalid operation for given types", expr->line, expr->column);
            return 0;
        }
        set_node_type(expr, result_type);
        /* #697: this op is 64-bit — widen any 32-bit-int arithmetic
         * operands so the computation happens in 64 bits, not in C int. */
        if (result_type && (result_type->kind == TYPE_INT64 ||
                            result_type->kind == TYPE_UINT64)) {
            propagate_int_width_64(left, result_type->kind);
            propagate_int_width_64(right, result_type->kind);
        }
    }

    free_type(left_type);
    free_type(right_type);
    return 1;
}

/* #749: resolve `<recv>.<field>` where `recv` is a local struct or
 * pointer-to-struct whose `field` is a function-pointer member. Returns
 * the field's TYPE_FUNCTION (is_fnptr) Type* (borrowed — do not free) or
 * NULL. *out_is_ptr is set to 1 when the receiver is a pointer-to-struct
 * (codegen then emits `->field` rather than `.field`). */
static Type* resolve_fnptr_struct_field(SymbolTable* table, const char* recv_name,
                                        const char* field_name, int* out_is_ptr) {
    Symbol* rs = lookup_symbol(table, recv_name);
    if (!rs || !rs->type) return NULL;
    Type* st = rs->type;
    const char* sname = NULL;
    int is_ptr = 0;
    if (st->kind == TYPE_STRUCT && st->struct_name) {
        sname = st->struct_name;
    } else if (st->kind == TYPE_PTR && st->element_type &&
               st->element_type->kind == TYPE_STRUCT && st->element_type->struct_name) {
        sname = st->element_type->struct_name;
        is_ptr = 1;
    } else {
        return NULL;
    }
    Symbol* ss = lookup_symbol(table, sname);
    if (!ss || !ss->node) return NULL;
    for (int fi = 0; fi < ss->node->child_count; fi++) {
        ASTNode* f = ss->node->children[fi];
        if (f && f->value && strcmp(f->value, field_name) == 0) {
            if (f->node_type && f->node_type->kind == TYPE_FUNCTION &&
                f->node_type->is_fnptr) {
                if (out_is_ptr) *out_is_ptr = is_ptr;
                return f->node_type;
            }
            return NULL;
        }
    }
    return NULL;
}

/* #928 UFCS support: the first declared parameter node of a user
 * function (the slot a `recv.method(...)` receiver would fill). Returns
 * NULL for builtins/externs with no AST body, or a zero-param function.
 * Mirrors the param-walk in count_function_params (last child = body;
 * guard clauses skipped). */
static ASTNode* ufcs_first_param(ASTNode* func) {
    if (!func || func->child_count == 0) return NULL;
    for (int i = 0; i < func->child_count - 1; i++) {
        ASTNode* child = func->children[i];
        if (child->type == AST_VARIABLE_DECLARATION ||
            child->type == AST_PATTERN_VARIABLE ||
            child->type == AST_PATTERN_LITERAL) {
            return child;
        }
    }
    return NULL;
}

/* #928/#934 UFCS: does `func`'s first parameter type unify with the receiver
 * type `rtype`? Used to pick the UFCS target among same-file and imported
 * candidates. An UNKNOWN on either side is treated as a match (inference may
 * refine it) — the same latitude the original same-file check used. */
static int ufcs_first_param_matches(ASTNode* func, Type* rtype) {
    ASTNode* p0 = ufcs_first_param(func);
    if (!p0 || !rtype || !p0->node_type) return 0;
    return types_equal(rtype, p0->node_type) ||
           rtype->kind == TYPE_UNKNOWN ||
           p0->node_type->kind == TYPE_UNKNOWN;
}

/* #928 method-call-on-value (UFCS): rewrite `recv.method(args)` into the
 * free-function call `method(recv, args)` when `method` is a free function
 * whose FIRST parameter type unifies with typeof(recv). Strictly a
 * LAST-RESORT fallback — it only runs after module-qualified resolution,
 * struct-field, and fnptr-field dispatch have all declined, so nothing
 * that compiles today changes meaning (UFCS only fires on what currently
 * errors with "Undefined function").
 *
 * Two entry shapes reach here:
 *   (a) parser tagged the call `ufcs` and already put the receiver subtree
 *       at children[0] (the receiver was NOT a bare identifier, e.g. a
 *       call result `expect(5).to_eq(...)`). call->value is the bare
 *       method name.
 *   (b) call->value is the dotted name `recv.method` where `recv` is a
 *       single identifier (a local value), and children[] are the plain
 *       args — the legacy member-call shape that previously errored.
 *
 * On success the call node is left in canonical `method(recv, args...)`
 * form (value = method, children = [recv, args...]) and 1 is returned so
 * the caller continues into the normal arity/type-check path. Returns 0
 * when no UFCS match applies (caller then emits its Undefined-function
 * error). */
static int try_ufcs_rewrite(ASTNode* call, SymbolTable* table) {
    if (!call || !call->value) return 0;

    int parser_tagged = (call->annotation && strcmp(call->annotation, "ufcs") == 0);

    ASTNode* recv = NULL;       /* receiver expression (owned by call) */
    char* method = NULL;        /* bare method name (heap; freed before return) */
    int recv_is_child0 = 0;     /* receiver already sits at children[0] */

    if (parser_tagged) {
        if (call->child_count < 1) return 0;
        recv = call->children[0];
        recv_is_child0 = 1;
        method = strdup(call->value);
    } else {
        /* shape (b): split a single-identifier dotted name. Bail if the
         * receiver part itself contains a dot (that's a module path, not a
         * value receiver) — those are handled by qualified resolution. */
        char* dot = strrchr(call->value, '.');
        if (!dot || dot == call->value) return 0;
        size_t rlen = (size_t)(dot - call->value);
        if (memchr(call->value, '.', rlen)) return 0;  /* multi-dot → not UFCS */
        char rname[200];
        if (rlen >= sizeof(rname)) return 0;
        memcpy(rname, call->value, rlen);
        rname[rlen] = '\0';
        /* The receiver must be a known value (local/global), not a module
         * or type name. If it doesn't resolve as a value symbol, this isn't
         * UFCS — let the normal Undefined-function error stand. */
        Symbol* rsym = lookup_symbol(table, rname);
        if (!rsym || rsym->is_function) return 0;
        recv = create_ast_node(AST_IDENTIFIER, rname, call->line, call->column);
        method = strdup(dot + 1);
    }

    if (!method) { if (!recv_is_child0) free_ast_node(recv); return 0; }

    /* Receiver type, needed for first-parameter matching below. */
    typecheck_expression(recv, table);
    Type* rtype = infer_type(recv, table);

    /* Resolve `method` to a free function whose first param matches the
     * receiver. Two tiers, in order:
     *   (1) a same-file (bare) free function, and
     *   (2) #934: a function exported by an imported module — the case that
     *       makes library-provided fluent surfaces work. We honour the same
     *       visibility as a normal qualified `mod.method(recv)` call: walk
     *       the visible namespaces and accept the first whose `mod.method`
     *       resolves to a function with a matching first parameter. On an
     *       imported match the call is rewritten to the QUALIFIED form
     *       `mod.method` so normal resolution + codegen emit `mod_method`. */
    char* resolved = NULL;  /* heap; the name to put on the call node */

    Symbol* fsym = lookup_symbol(table, method);
    if (fsym && fsym->is_function && fsym->node &&
        ufcs_first_param_matches(fsym->node, rtype)) {
        resolved = strdup(method);            /* same-file bare call */
    } else if (global_module_registry) {
        for (int mi = 0; mi < global_module_registry->module_count && !resolved; mi++) {
            AetherModule* m = global_module_registry->modules[mi];
            if (!m || !m->name) continue;
            const char* ns = m->name;
            if (!is_visible_namespace(ns, table)) continue;
            if (!module_is_exported(m, method)) continue;
            char qualified[512];
            snprintf(qualified, sizeof(qualified), "%s.%s", ns, method);
            Symbol* qs = lookup_qualified_symbol(table, qualified);
            if (qs && qs->is_function && qs->node &&
                ufcs_first_param_matches(qs->node, rtype)) {
                resolved = strdup(qualified);  /* imported qualified call */
            }
        }
    }

    if (rtype) free_type(rtype);
    free(method);

    if (!resolved) {
        if (!recv_is_child0) free_ast_node(recv);
        return 0;
    }

    /* Splice into `<resolved>(recv, args...)`. For shape (a) the receiver is
     * already children[0], so only the call name changes. For shape (b)
     * prepend the synthesized receiver. */
    if (!recv_is_child0) {
        int old = call->child_count;
        ASTNode** nc = malloc(sizeof(ASTNode*) * (old + 1));
        nc[0] = recv;
        for (int i = 0; i < old; i++) nc[i + 1] = call->children[i];
        if (call->children) free(call->children);
        call->children = nc;
    call->child_capacity = 0;
        call->child_count = old + 1;
    }
    free(call->value);
    call->value = resolved;          /* transfer ownership */
    if (call->annotation) { free(call->annotation); call->annotation = NULL; }
    return 1;
}

/* Mirrors codegen's has_return_value (codegen_func.c:586) — deliberately a
 * copy rather than a cross-layer include, so the typechecker does not depend
 * on a codegen header. The two must agree: if they diverge, this diagnostic
 * either fires on a function that codegen emits as int, or misses one it
 * emits as void. tests/regression/test_void_value_read.ae pins the agreement
 * from the outside, by asserting the diagnostic on exactly the shapes codegen
 * makes void. */
static int tc_has_return_value(ASTNode* node) {
    if (!node) return 0;
    if (node->type == AST_CLOSURE) return 0;
    if (node->type == AST_RETURN_STATEMENT && node->child_count > 0 && node->children[0]) {
        if (node->children[0]->type == AST_PRINT_STATEMENT) return 0;
        return 1;
    }
    for (int i = 0; i < node->child_count; i++) {
        if (tc_has_return_value(node->children[i])) return 1;
    }
    return 0;
}

/* Does calling `call` yield no value?
 *
 * A function with no declared return type and no `return <value>` anywhere in
 * its body lowers to C `void` (codegen_func.c, the ret_unannotated branch —
 * deliberate since #354). Reading such a call's "result" reads whatever the
 * ABI's return register happens to hold: stack residue, different on every
 * run, and silently so.
 *
 * The shape that surfaced this (#1745-adjacent, reported by the aeb line):
 *
 *     builder voidprog(ctx: ptr) { }              // no ': int'
 *     node(s: ptr) -> int { voidprog(null) { } }  // rc = -843144544
 *
 * A build orchestrator read that as a node's exit status, so a failing node
 * could exit falsely green. It compiled without a word of warning, and the
 * same-translation-unit case is not caught by the C compiler either, because
 * Aether emits the callee as `void` and the caller simply reads the register.
 *
 * Returns 1 when `call` names a user-defined function that lowers to void.
 * Externs are excluded: their declared type is the only truth available, and
 * an `extern f() -> int` may well front a real int-returning C function. */
static int call_yields_no_value(ASTNode* call, SymbolTable* table) {
    if (!call || call->type != AST_FUNCTION_CALL || !call->value) return 0;
    Symbol* sym = lookup_symbol(table, call->value);
    if (!sym || !sym->node) return 0;
    ASTNode* fn = sym->node;
    if (fn->type != AST_FUNCTION_DEFINITION && fn->type != AST_BUILDER_FUNCTION)
        return 0;
    /* An annotated return type is authoritative, whatever the body does. */
    Type* rt = fn->node_type;
    if (rt && rt->kind != TYPE_VOID && rt->kind != TYPE_UNKNOWN) return 0;
    /* Unannotated: void exactly when no `return <value>` reaches codegen.
     * has_return_value is the same predicate codegen uses to decide, so the
     * two cannot disagree. */
    return !tc_has_return_value(fn);
}

int typecheck_function_call(ASTNode* call, SymbolTable* table) {
    if (!call || call->type != AST_FUNCTION_CALL) return 0;

    /* #928 UFCS, shape (a): the parser tagged this as a method-call on a
     * non-identifier receiver (e.g. `expect(5).to_eq(...)`) and stashed the
     * receiver subtree at children[0]. Rewrite to `method(recv, args)` up
     * front so the normal resolution + arity path below sees a plain call.
     * If no free function matches the receiver type, fall through with the
     * tag cleared so the standard Undefined-function diagnostic fires. */
    if (call->annotation && strcmp(call->annotation, "ufcs") == 0) {
        if (!try_ufcs_rewrite(call, table)) {
            /* Not a UFCS match. Drop the tag; restore a useful name for the
             * error (the bare method name is already in call->value). */
            free(call->annotation);
            call->annotation = NULL;
        }
    }

    // heap.free(p) — the counterpart to heap.new(T) (issue #564). Not a
    // real function symbol; codegen lowers it to free(p). Validate the
    // single argument is a pointer (a *T from heap.new, or any ptr) and
    // stamp the call's type as void. Reject misuse with a clear message
    // before the generic "undefined function" path runs.
    if (call->value && strcmp(call->value, "heap.free") == 0) {
        if (call->child_count != 1) {
            type_error("heap.free(p) takes exactly one pointer argument",
                       call->line, call->column);
            set_node_type(call, create_type(TYPE_VOID));
            return 0;
        }
        typecheck_expression(call->children[0], table);
        Type* arg = infer_type(call->children[0], table);
        int ok = !arg || arg->kind == TYPE_PTR || arg->kind == TYPE_UNKNOWN;
        if (arg) free_type(arg);
        if (!ok) {
            type_error("heap.free(p), argument must be a pointer "
                       "(a `*T` from heap.new)", call->line, call->column);
            set_node_type(call, create_type(TYPE_VOID));
            return 0;
        }
        set_node_type(call, create_type(TYPE_VOID));
        return 1;
    }

    // Use qualified lookup to handle namespaced calls like string.new -> string_new
    Symbol* symbol = lookup_qualified_symbol(table, call->value);

    /* #924 re-export: a `hub.fn(...)` call where hub re-exports fn from
     * another module resolves (via lookup_qualified_symbol) to the defining
     * module's symbol. Rewrite call->value to the origin-qualified form so
     * codegen emits `<origin>_fn` (the cloned definition) rather than the
     * non-existent `hub_fn`. Gate on a single-dot qualified name whose local
     * `hub_fn` symbol genuinely doesn't exist. */
    if (symbol && call->value && global_module_registry) {
        char* dot = strchr(call->value, '.');
        if (dot && dot != call->value && !strchr(dot + 1, '.')) {
            char hubname[256];
            size_t hl = (size_t)(dot - call->value);
            if (hl < sizeof(hubname)) {
                memcpy(hubname, call->value, hl);
                hubname[hl] = '\0';
                AetherModule* hub = module_find(hubname);
                AetherModule* origin = module_resolve_reexport(hub, dot + 1);
                if (origin && origin->name &&
                    strcmp(origin->name, hubname) != 0) {
                    char rewritten[512];
                    snprintf(rewritten, sizeof(rewritten), "%s.%s",
                             origin->name, dot + 1);
                    free(call->value);
                    call->value = strdup(rewritten);
                }
            }
        }
    }

    /* #1035: a qualified call resolved to a BARE symbol — a
     * non-prefixed export like std.os's `aether_args_count` (see the
     * exports-gated fallback in lookup_qualified_symbol). Codegen
     * lowers dots to underscores, so `os.aether_args_count` would
     * otherwise emit the non-existent `os_aether_args_count`; rewrite
     * call->value to the bare name the symbol actually carries. */
    if (symbol && call->value && symbol->name) {
        char* dot = strchr(call->value, '.');
        if (dot && strcmp(symbol->name, dot + 1) == 0 &&
            strcmp(symbol->name, call->value) != 0) {
            char* bare = strdup(symbol->name);
            if (bare) {
                free(call->value);
                call->value = bare;
            }
        }
    }

    // Issue #333 DSL receiver fallback: when the call is bare-name
    // (no dot in call->value) and the symbol resolved through the
    // <receiver>_<name> rewrite in lookup_symbol, the symbol's
    // canonical C name differs from call->value. Rewrite call->value
    // to the qualified form `<receiver>.<name>` so codegen (which
    // converts `.` to `_`) emits the resolved C symbol rather than
    // the bare identifier — without this, you get
    // "implicit function declaration" warnings and a link error.
    if (symbol && call->value && !strchr(call->value, '.') &&
        symbol->name && strcmp(symbol->name, call->value) != 0) {
        // The fallback rewrite is `<receiver>_<name>`. Reverse the
        // direction so call->value carries `<receiver>.<name>` —
        // matching the convention used by lookup_qualified_symbol's
        // qualified-form input. Find the underscore that separates
        // the receiver prefix from the original bare name (suffix
        // matches call->value).
        size_t orig_len = strlen(call->value);
        size_t resolved_len = strlen(symbol->name);
        if (resolved_len > orig_len + 1 &&
            strcmp(symbol->name + resolved_len - orig_len, call->value) == 0 &&
            symbol->name[resolved_len - orig_len - 1] == '_') {
            size_t prefix_len = resolved_len - orig_len - 1;
            size_t qualified_len = prefix_len + 1 + orig_len + 1;
            char* qualified = malloc(qualified_len);
            if (qualified) {
                memcpy(qualified, symbol->name, prefix_len);
                qualified[prefix_len] = '.';
                memcpy(qualified + prefix_len + 1, call->value, orig_len);
                qualified[qualified_len - 1] = '\0';
                free(call->value);
                call->value = qualified;
            }
        }
    }

    // Rewrite import alias to qualified name for codegen (e.g. "release" -> "build.release")
    if (symbol && symbol->is_function && call->value) {
        const char* alias_target = find_import_alias(call->value);
        if (alias_target) {
            free(call->value);
            call->value = strdup(alias_target);
        }
    }

    // Typed C function-pointer local call: `fp(a, b)` where `fp` is
    // a local of type `fn(T1, T2, ...) -> R` (is_fnptr=1).  Validate
    // each argument and stamp the call's return type, but DON'T
    // rewrite to `call(fp, a, b)` — the codegen has a dedicated
    // fnptr-call branch that wants the original AST shape so it can
    // emit `((R (*)(T1, T2))(fp))(a, b)` inline.
    if (symbol && !symbol->is_function && symbol->type &&
        symbol->type->kind == TYPE_FUNCTION && symbol->type->is_fnptr &&
        call->value) {
        for (int i = 0; i < call->child_count; i++) {
            typecheck_expression(call->children[i], table);
        }
        if (symbol->type->return_type) {
            set_node_type(call, clone_type(symbol->type->return_type));
        } else {
            set_node_type(call, create_type(TYPE_VOID));
        }
        return 1;
    }

    // Phase A3 (foundation for #260 D pure-Aether middleware): if the
    // call's target name resolves to a local variable whose type is
    // TYPE_FUNCTION (a closure / function-typed value), this is a
    // direct invocation of a function-typed local — `handler(req, res)`
    // where `handler` is a local variable. Transparently rewrite the
    // AST to flow through the existing `call(fn, args...)` codegen
    // path (codegen_expr.c:~2155). This lets users write the natural
    // form rather than the workaround `call(handler, req, res)`.
    //
    // (The is_fnptr=1 case was handled above; this block is reached
    // only for closure-shaped fn-typed values.)
    if (symbol && !symbol->is_function && symbol->type &&
        symbol->type->kind == TYPE_FUNCTION && !symbol->type->is_fnptr &&
        call->value) {
        // Build a new child list: [fn_ref, original_args...]
        ASTNode* fn_ref = create_ast_node(AST_IDENTIFIER, call->value,
                                          call->line, call->column);
        set_node_type(fn_ref, clone_type(symbol->type));

        int old_count = call->child_count;
        ASTNode** new_children = malloc(sizeof(ASTNode*) * (old_count + 1));
        new_children[0] = fn_ref;
        for (int i = 0; i < old_count; i++) {
            new_children[i + 1] = call->children[i];
        }
        if (call->children) free(call->children);
        call->children = new_children;
    call->child_capacity = 0;
        call->child_count = old_count + 1;

        // Rename the call from <varname> to "call" so codegen routes
        // through the existing closure-invocation path.
        free(call->value);
        call->value = strdup("call");

        // Type-check argument expressions (skip the new fn_ref at
        // index 0; its type is already set above).
        for (int i = 1; i < call->child_count; i++) {
            typecheck_expression(call->children[i], table);
        }

        // The call's return type is the function-type's return slot,
        // when known. Otherwise leave UNKNOWN — type inference may
        // refine it later.
        if (symbol->type->return_type) {
            set_node_type(call, clone_type(symbol->type->return_type));
        } else {
            set_node_type(call, create_type(TYPE_UNKNOWN));
        }
        return 1;
    }

    /* #749: dispatch through a function-pointer struct field —
     * `recv.fnptrfield(args)`. The parser collapsed the member-access
     * callee to the dotted name `recv.field` (dropping the receiver
     * subtree), and it did not resolve as a function symbol above. If
     * `recv` is a local struct / pointer-to-struct whose `field` is a
     * function pointer, this is a typed indirect call — typecheck the
     * args against the field's signature, stamp the return type, and tag
     * the node so codegen emits `(recv.field)(args)` / `(recv->field)(args)`.
     * Single-level receiver only (a bare local); the part before the dot
     * must contain no further dots. */
    if ((!symbol || !symbol->is_function) && call->value) {
        char* dot = strrchr(call->value, '.');
        size_t rlen = dot ? (size_t)(dot - call->value) : 0;
        if (dot && rlen > 0 && rlen < 200 && !memchr(call->value, '.', rlen)) {
            char recv[200];
            memcpy(recv, call->value, rlen);
            recv[rlen] = '\0';
            int is_ptr = 0;
            Type* fsig = resolve_fnptr_struct_field(table, recv, dot + 1, &is_ptr);
            if (fsig) {
                for (int i = 0; i < call->child_count; i++) {
                    typecheck_expression(call->children[i], table);
                }
                set_node_type(call, fsig->return_type
                    ? clone_type(fsig->return_type) : create_type(TYPE_UNKNOWN));
                if (call->annotation) free(call->annotation);
                call->annotation = strdup(is_ptr ? "fnfield_ptr" : "fnfield_val");
                return 1;
            }
        }
    }

    /* #928 UFCS, shape (b): a `recv.method(args)` where `recv` is a local
     * value (not a module, not a struct field, not a fnptr field — all
     * tried above and declined). Last resort: rewrite to `method(recv,
     * args)` if `method` is a free function whose first param matches
     * typeof(recv), then re-resolve and fall through to the normal
     * arity/type-check path. */
    if ((!symbol || !symbol->is_function) && call->value &&
        strchr(call->value, '.')) {
        if (try_ufcs_rewrite(call, table)) {
            symbol = lookup_qualified_symbol(table, call->value);
        }
    }

    if (!symbol || !symbol->is_function) {
        char error_msg[256];
        // Check if this is a visibility rejection (not-exported) rather than truly undefined
        if (call->value && strchr(call->value, '.') && global_module_registry) {
            char* tmp = strdup(call->value);
            char* dot = strchr(tmp, '.');
            *dot = '\0';
            if (is_export_blocked(tmp, dot + 1)) {
                snprintf(error_msg, sizeof(error_msg),
                         "'%s' is not exported from module '%s'", dot + 1, tmp);
            } else {
                snprintf(error_msg, sizeof(error_msg),
                         "Undefined function '%s'", call->value);
            }
            free(tmp);
        } else {
            snprintf(error_msg, sizeof(error_msg),
                     "Undefined function '%s'", call->value ? call->value : "?");
        }
        type_error(error_msg, call->line, call->column);
        return 0;
    }

    // Arity check: user-defined functions have their AST node stored
    if (symbol->node && (symbol->node->type == AST_FUNCTION_DEFINITION || symbol->node->type == AST_BUILDER_FUNCTION)) {
        int expected = count_function_params(symbol->node);
        int required = count_required_params(symbol->node);
        int ctx_first = has_ctx_first_param(symbol->node);
        int got = call->child_count;

        /* A bare trailing block cannot stand in for an `fn` parameter.
         *
         * `f(x) { ... }` parses as a DSL block: it inlines at the call site
         * and is never hoisted, so there is no closure VALUE to pass. When
         * the callee's last parameter is `fn`, codegen then emits a
         * reference to a `_closure_fn_N` it never generated and the C
         * compiler fails with `'_closure_fn_0' undeclared` — a leaked
         * internal symbol name, at a line the user cannot act on.
         *
         * Note this cannot live in the arity-mismatch branch below: the
         * trailing block still occupies an argument slot, so `got` equals
         * `expected` and that branch never runs. The signature is what
         * distinguishes the two readings, not the count. */
        if (last_param_is_fn(symbol->node)) {
            for (int i = 0; i < call->child_count; i++) {
                ASTNode* c = call->children[i];
                if (c && c->type == AST_CLOSURE && c->value &&
                    strcmp(c->value, "trailing") == 0) {
                    char cb_msg[320];
                    snprintf(cb_msg, sizeof(cb_msg),
                             "'%s' takes a closure as its last parameter, but "
                             "the trailing block here is a DSL block that runs "
                             "inline. Write `callback { ... }` (or `|params| "
                             "{ ... }`) so it becomes a real closure the "
                             "function can hold and call later",
                             call->value ? call->value : "function");
                    type_error(cb_msg, call->line, call->column);
                    return 0;
                }
            }
        }

        // If mismatch, try excluding trailing closures (for functions that
        // don't accept fn params but have trailing blocks for DSL syntax)
        if (got != expected && !(ctx_first && got == expected - 1)) {
            int non_closure = 0;
            for (int i = 0; i < call->child_count; i++) {
                if (call->children[i] && call->children[i]->type != AST_CLOSURE) {
                    non_closure++;
                }
            }
            if (non_closure == expected ||
                (ctx_first && non_closure == expected - 1)) {
                got = non_closure; // trailing closures are DSL blocks, not args
            }
        }
        // Functions with _ctx as the first param accept either:
        //   - expected args (caller passed _ctx explicitly), or
        //   - expected-1 args (builder DSL auto-injects _ctx at the call site)
        // Phase A2.1 default arguments: a call with `got` between
        // `required` and `expected` (inclusive) is also OK — the
        // missing trailing args get filled in below from the
        // declared defaults.
        // A C-variadic callee (declared `f(fixed..., ...)`, marked with
        // annotation "varargs") accepts any number of trailing args
        // beyond its named parameters — so any count >= the named-param
        // count is fine.
        int is_variadic_callee =
            annotation_has_marker(symbol->node->annotation, "varargs");
        int arity_ok = (got == expected) ||
                       (ctx_first && got == expected - 1) ||
                       (got >= required && got < expected) ||
                       (is_variadic_callee && got >= expected);
        if (!arity_ok) {
            char error_msg[256];
            if (required < expected) {
                snprintf(error_msg, sizeof(error_msg),
                         "Function '%s' expects %d-%d argument(s), got %d",
                         call->value, required, expected, got);
            } else {
                snprintf(error_msg, sizeof(error_msg),
                         "Function '%s' expects %d argument(s), got %d",
                         call->value, expected, got);
            }
            type_error(error_msg, call->line, call->column);
            return 0;
        }

        // Phase A2.1 default-arg fill: if the caller passed fewer
        // args than the callee declared (within the required..total
        // window allowed above), append clones of the declared
        // default expressions to the call's child list. After this
        // codegen sees a fully-populated call and emits the right C.
        // Defaults trail required, so the missing slots are always
        // the trailing tail.
        if (got >= required && got < expected) {
            int param_idx = 0;
            for (int i = 0; i < symbol->node->child_count - 1 &&
                            call->child_count < expected; i++) {
                ASTNode* p = symbol->node->children[i];
                if (!p) continue;
                if (p->type == AST_GUARD_CLAUSE) continue;
                if (p->type != AST_VARIABLE_DECLARATION &&
                    p->type != AST_PATTERN_VARIABLE &&
                    p->type != AST_PATTERN_LITERAL) continue;
                // Skip param indexes the caller already supplied.
                if (param_idx < got) {
                    param_idx++;
                    continue;
                }
                if (!param_has_default(p)) {
                    // Should be unreachable given the trailing-default
                    // rule, but defensively: error instead of silently
                    // filling with an undefined value.
                    char err[256];
                    snprintf(err, sizeof(err),
                             "Function '%s' parameter %d has no default, caller must supply it",
                             call->value, param_idx + 1);
                    type_error(err, call->line, call->column);
                    return 0;
                }
                ASTNode* default_clone = clone_ast_node(p->children[0]);
                if (!default_clone) {
                    type_error("internal: failed to clone default expression",
                               call->line, call->column);
                    return 0;
                }
                // Phase A2.2: rewrite source-location intrinsics in
                // the clone so they capture the caller's location
                // rather than the function definition's. Closes #265.
                rewrite_caller_site_intrinsics(default_clone, call->line, call->column);
                add_child(call, default_clone);
                param_idx++;
            }
        }
    }

    /* #952: arity check for extern (C FFI) functions. An extern's AST node
     * holds only its parameters as children (no body, no guard), so the
     * declared param count is the number of those children. A `varargs`
     * extern (declared with a trailing `...`, e.g. `printf(fmt, ...)`)
     * accepts any number of args beyond its named ones, so only the named
     * minimum is enforced. Without this, over- or under-applying an extern —
     * e.g. calling the zero-arg `math.deg_to_rad()` constant as
     * `math.deg_to_rad(x)` — slipped past `ae check` and surfaced only as a
     * raw gcc "too many arguments" error from the generated C. */
    if (symbol->node && symbol->node->type == AST_EXTERN_FUNCTION) {
        ASTNode* extern_node = symbol->node;
        int expected = 0;
        for (int i = 0; i < extern_node->child_count; i++) {
            ASTNode* p = extern_node->children[i];
            if (p && p->type == AST_IDENTIFIER) expected++;
        }
        int got = call->child_count;
        /* Variadic externs (`f(named..., ...)`) accept any number of args
         * beyond the named ones. Both the bare `extern` form and the
         * `@extern("c_name")` form (whose annotation also carries
         * `c_symbol:NAME`) record it as a `varargs` marker. */
        int is_variadic = annotation_has_marker(extern_node->annotation, "varargs");
        /* A `_ctx`-first extern (builder-DSL boundary, e.g. std.host's
         * `input(_ctx, name, type)`) is called with `_ctx` auto-injected at
         * the call site, so the caller supplies expected-1 args. Extern
         * params are AST_IDENTIFIER nodes (not the function-definition param
         * shapes has_ctx_first_param expects), so detect `_ctx` directly. */
        int ctx_first = (extern_node->child_count > 0 &&
                         extern_node->children[0] &&
                         extern_node->children[0]->type == AST_IDENTIFIER &&
                         extern_node->children[0]->value &&
                         strcmp(extern_node->children[0]->value, "_ctx") == 0);
        int arity_ok = is_variadic
            ? (got >= expected || (ctx_first && got >= expected - 1))
            : (got == expected || (ctx_first && got == expected - 1));
        if (!arity_ok) {
            char error_msg[256];
            int low = ctx_first ? (expected - 1) : expected;
            if (is_variadic) {
                snprintf(error_msg, sizeof(error_msg),
                         "Function '%s' expects at least %d argument(s), got %d",
                         call->value, low, got);
            } else {
                snprintf(error_msg, sizeof(error_msg),
                         "Function '%s' expects %d argument(s), got %d",
                         call->value, low, got);
            }
            type_error(error_msg, call->line, call->column);
            return 0;
        }

        /* #1033: tuple-typed extern parameters (by-value C struct args).
         * A `(T1, T2, ...)` param accepts exactly a tuple-literal
         * argument with a matching element count; codegen packs it into
         * the synthesized `_tuple_*` struct. Conservative slice: scalar
         * / byte / f32 / bool / ptr elements, no nesting, no strings
         * (string unwrap semantics inside a by-value struct are a
         * follow-up). Also rejects a tuple literal aimed at a
         * non-tuple param — there is nothing sane to emit for it. */
        {
            int pi = 0;
            int arg_base = ctx_first ? -1 : 0;  /* _ctx injected: params lead args by 1 */
            for (int ci = 0; ci < extern_node->child_count; ci++) {
                ASTNode* p = extern_node->children[ci];
                if (!p || p->type != AST_IDENTIFIER) continue;
                int ai = pi + arg_base;
                pi++;
                if (ai < 0 || ai >= call->child_count) continue;
                ASTNode* arg = call->children[ai];
                if (!arg) continue;
                int param_is_tuple = (p->node_type &&
                                      p->node_type->kind == TYPE_TUPLE);
                if (!param_is_tuple && arg->type == AST_TUPLE_LITERAL) {
                    char error_msg[256];
                    snprintf(error_msg, sizeof(error_msg),
                             "tuple literal passed to non-tuple parameter "
                             "%d of extern '%s'", pi, call->value);
                    type_error(error_msg, arg->line, arg->column);
                    return 0;
                }
                if (!param_is_tuple) continue;

                /* The param's element kinds must be in the supported by-value
                 * slice (scalar / byte / f32 / bool / ptr). Shared by both the
                 * tuple-literal and tuple-value argument forms below. */
                for (int ei = 0; ei < p->node_type->tuple_count; ei++) {
                    TypeKind ek = p->node_type->tuple_types[ei]
                                  ? p->node_type->tuple_types[ei]->kind
                                  : TYPE_UNKNOWN;
                    if (ek != TYPE_INT && ek != TYPE_INT64 &&
                        ek != TYPE_FLOAT && ek != TYPE_FLOAT32 &&
                        ek != TYPE_BYTE && ek != TYPE_BOOL &&
                        ek != TYPE_PTR) {
                        char error_msg[256];
                        snprintf(error_msg, sizeof(error_msg),
                                 "tuple extern parameters support scalar, "
                                 "byte, f32, bool, and ptr elements only "
                                 "(parameter %d of '%s')", pi, call->value);
                        type_error(error_msg, arg->line, arg->column);
                        return 0;
                    }
                }

                if (arg->type == AST_TUPLE_LITERAL) {
                    if (arg->child_count != p->node_type->tuple_count) {
                        char error_msg[256];
                        snprintf(error_msg, sizeof(error_msg),
                                 "tuple argument for parameter %d of extern '%s' "
                                 "has %d element(s), expected %d",
                                 pi, call->value, arg->child_count,
                                 p->node_type->tuple_count);
                        type_error(error_msg, arg->line, arg->column);
                        return 0;
                    }
                    /* Stamp the literal with the param's tuple type so codegen
                     * emits the matching `_tuple_*` compound literal without
                     * re-deriving it. */
                    if (arg->node_type) free_type(arg->node_type);
                    arg->node_type = clone_type(p->node_type);
                } else {
                    /* #1062: also accept a tuple-typed value (a variable, or the
                     * result of a tuple-returning extern) whose type matches the
                     * parameter's tuple type. The value already is the
                     * synthesized `_tuple_*` struct in the generated C, so this
                     * is a pass-through of the same typedef, not new codegen,
                     * which lets pass-through FFI chains like
                     * `save(load(...))` skip the destructure/re-parenthesize
                     * boilerplate. */
                    Type* at = infer_type(arg, table);
                    int matches = at && at->kind == TYPE_TUPLE &&
                                  at->tuple_count == p->node_type->tuple_count;
                    for (int ei = 0; matches && ei < p->node_type->tuple_count; ei++) {
                        Type* pe = p->node_type->tuple_types[ei];
                        Type* ae = at->tuple_types ? at->tuple_types[ei] : NULL;
                        /* Match on the element's emitted C type name, which is
                         * what codegen keys the `_tuple_*` typedef on, not just
                         * its TypeKind: `int` and the aliased `int8_t`/`size_t`
                         * share a kind but produce different C structs, and a
                         * `ptr` differs from a typed `*Struct` by pointee. An
                         * arg element with no inferred type (an inference gap)
                         * is left to the C compiler rather than over-rejected,
                         * preserving the earlier leniency. */
                        if (!ae) continue;
                        const char* pc = pe ? get_c_type(pe) : NULL;
                        char* pcs = pc ? strdup(pc) : NULL;  /* get_c_type reuses a rotating static buffer */
                        const char* ac = get_c_type(ae);
                        if (!pcs || !ac || strcmp(pcs, ac) != 0) matches = 0;
                        free(pcs);
                    }
                    if (at) free_type(at);
                    if (!matches) {
                        char error_msg[256];
                        snprintf(error_msg, sizeof(error_msg),
                                 "parameter %d of extern '%s' is tuple-typed; "
                                 "pass a matching tuple value or a parenthesized "
                                 "tuple literal, e.g. (x, y)", pi, call->value);
                        type_error(error_msg, arg->line, arg->column);
                        return 0;
                    }
                }
            }
        }
    }

    // Issue #333 DSL block receiver scoping: when the call is in
    // member-access form (`receiver.method(args) { body }`) and has
    // trailing AST_CLOSURE children, stamp those closures with the
    // receiver's namespace so unqualified calls inside the body can
    // fall back through `<receiver>_<name>` (see lookup_symbol).
    //
    // Receiver resolution: split call->value on `.`. If the prefix
    // is a visible namespace (`bash` in `bash.test(...)`), use it
    // verbatim. If the prefix is a local variable of struct type
    // (`b: Builder`), use the type's name (`Builder`). The rewrite
    // is the same in both cases — namespace and struct-method helpers
    // share the `<prefix>_<name>` codegen convention.
    //
    // Stamping uses the AST_CLOSURE node's annotation field with a
    // `dsl_recv:` prefix; AST_CLOSURE typecheck reads it and seeds
    // closure_scope->dsl_receiver. Idempotent: existing annotations
    // (none today on AST_CLOSURE; `defer` factory annotations live
    // on AST_VARIABLE_DECLARATION) are preserved.
    if (call->value && strchr(call->value, '.')) {
        const char* dot = strchr(call->value, '.');
        size_t prefix_len = (size_t)(dot - call->value);
        if (prefix_len > 0 && prefix_len < 256) {
            char prefix[256];
            memcpy(prefix, call->value, prefix_len);
            prefix[prefix_len] = '\0';

            const char* dsl_recv_name = NULL;
            char type_name_buf[256];

            // Namespace receiver: directly visible.
            if (is_visible_namespace(prefix, table)) {
                dsl_recv_name = prefix;
            } else {
                // Typed-value receiver: prefix is a local variable;
                // use its struct type's name.
                Symbol* recv_sym = lookup_symbol(table, prefix);
                if (recv_sym && recv_sym->type) {
                    const char* tn = type_name(recv_sym->type);
                    if (tn && tn[0] != '\0' &&
                        strlen(tn) < sizeof(type_name_buf) &&
                        strcmp(tn, "unknown") != 0 &&
                        strcmp(tn, "int") != 0 &&
                        strcmp(tn, "long") != 0 &&
                        strcmp(tn, "float") != 0 &&
                        strcmp(tn, "double") != 0 &&
                        strcmp(tn, "bool") != 0 &&
                        strcmp(tn, "string") != 0) {
                        strncpy(type_name_buf, tn, sizeof(type_name_buf) - 1);
                        type_name_buf[sizeof(type_name_buf) - 1] = '\0';
                        dsl_recv_name = type_name_buf;
                    }
                }
            }

            if (dsl_recv_name) {
                for (int i = 0; i < call->child_count; i++) {
                    ASTNode* child = call->children[i];
                    if (child && child->type == AST_CLOSURE) {
                        // Idempotent: skip if already stamped.
                        if (child->annotation &&
                            strncmp(child->annotation, "dsl_recv:", 9) == 0) {
                            continue;
                        }
                        size_t a_len = 9 + strlen(dsl_recv_name) + 1;
                        char* ann = malloc(a_len);
                        if (ann) {
                            snprintf(ann, a_len, "dsl_recv:%s", dsl_recv_name);
                            // Defensive: if annotation is unset, set it.
                            // If it's set to something else (no current
                            // user of AST_CLOSURE annotation, but be safe),
                            // leave it alone so we don't stomp.
                            if (!child->annotation) {
                                child->annotation = ann;
                            } else {
                                free(ann);
                            }
                        }
                    }
                }
            }
        }
    }

    // #1044 implicit enum selector for arguments: `f(Blue)` where the parameter
    // is an enum type resolves the bare member to the enum constant BEFORE the
    // argument is typechecked (otherwise a bare member fails as an undefined
    // variable). Reuses the same param<->arg alignment (ctx injection, extern
    // vs user body slot) as the argument checks further below.
    if (symbol && symbol->node &&
        (symbol->node->type == AST_FUNCTION_DEFINITION ||
         symbol->node->type == AST_BUILDER_FUNCTION ||
         symbol->node->type == AST_EXTERN_FUNCTION)) {
        int c_has_ctx = has_ctx_first_param(symbol->node);
        int c_expected = count_function_params(symbol->node);
        int c_arg_offset = (c_has_ctx && call->child_count == c_expected - 1) ? -1 : 0;
        int c_scan_limit = (symbol->node->type == AST_EXTERN_FUNCTION)
                           ? symbol->node->child_count
                           : symbol->node->child_count - 1;
        int c_pidx = 0;
        for (int i = 0; i < c_scan_limit; i++) {
            ASTNode* p = symbol->node->children[i];
            if (!p || p->type == AST_GUARD_CLAUSE) continue;
            if (p->type != AST_VARIABLE_DECLARATION &&
                p->type != AST_PATTERN_VARIABLE &&
                p->type != AST_IDENTIFIER) continue;
            int aslot = c_pidx + c_arg_offset;
            c_pidx++;
            if (aslot < 0 || aslot >= call->child_count) continue;
            if (p->node_type && p->node_type->kind == TYPE_ENUM)
                coerce_bare_enum_member(call->children[aslot], p->node_type, table);
        }
    }

    // Type check arguments and validate types against parameters
    for (int i = 0; i < call->child_count; i++) {
        typecheck_expression(call->children[i], table);
    }

    /* Duration parameter-passing strictness (issue #586): plain int /
     * long / uint64 / float passed to a Duration-typed parameter
     * silently reinterprets as nanoseconds, which bit PR #583 (the
     * test that called `client.set_timeout(req, 5)` got a 5ns timeout
     * and failed on macOS). The comparison rule already rejects
     * `dur > 5` at infer_binary_type; this extends the same rule to
     * parameter-passing for BOTH extern and user-defined callees.
     *
     * Bare numeric literals must use a unit suffix (`5s`, `200ms`,
     * `100ns`); identifier values typed as a plain integer need to
     * be wrapped in an explicit construction. The error message
     * names the literal forms so the fix is obvious.
     *
     * Scope is intentionally narrow to TYPE_DURATION; if/when other
     * tagged-int types land (Bytes, Pixels, …) generalise then. */
    if (symbol->node &&
        (symbol->node->type == AST_FUNCTION_DEFINITION ||
         symbol->node->type == AST_BUILDER_FUNCTION ||
         symbol->node->type == AST_EXTERN_FUNCTION)) {
        int has_ctx = has_ctx_first_param(symbol->node);
        int arg_offset = 0;
        int got = call->child_count;
        int expected = count_function_params(symbol->node);
        /* Mirror the arity logic at line ~3957: when the callee
         * declares _ctx first and the caller's arg count is
         * expected-1, the DSL auto-injects _ctx, so param index 0
         * has no corresponding arg slot — skip it by starting
         * arg_offset at -1 (param[1] aligns to arg[0]). */
        if (has_ctx && got == expected - 1) arg_offset = -1;
        int param_idx = 0;
        /* Contract folding, call-site tier (docs/contract-folding.md):
         * collect the parameter-name -> argument-expression bindings while
         * the loop below walks them anyway; the callee's `requires`/`where`
         * clauses are evaluated under this environment after the loop. */
        ContractEnv cf_env = {{0}, {0}, 0, g_enum_program};
        /* User-defined / builder functions store the body as the final
         * child; extern declarations do not. Cap the loop accordingly
         * so we don't try to treat the body as a parameter. */
        int param_scan_limit =
            (symbol->node->type == AST_EXTERN_FUNCTION)
                ? symbol->node->child_count
                : symbol->node->child_count - 1;
        for (int i = 0; i < param_scan_limit; i++) {
            ASTNode* param = symbol->node->children[i];
            if (!param) continue;
            if (param->type == AST_GUARD_CLAUSE) continue;
            /* Param-decl shapes:
             *   - User-defined fn:  AST_VARIABLE_DECLARATION or AST_PATTERN_VARIABLE
             *   - Extern fn:        AST_IDENTIFIER (parser builds them this way;
             *                       see parse_extern_declaration in parser.c)
             * Skip anything that isn't one of these param shapes. */
            if (param->type != AST_VARIABLE_DECLARATION &&
                param->type != AST_PATTERN_VARIABLE &&
                param->type != AST_IDENTIFIER) continue;

            int arg_slot = param_idx + arg_offset;
            param_idx++;
            if (arg_slot < 0 || arg_slot >= call->child_count) continue;

            if (cf_env.count < CONTRACT_ENV_MAX_PARAMS && param->value) {
                cf_env.names[cf_env.count] = param->value;
                cf_env.args[cf_env.count] = call->children[arg_slot];
                cf_env.count++;
            }

            Type* param_type = param->node_type;
            /* #480: a distinct-typed parameter (or a distinct argument to a
             * non-distinct parameter) requires an explicit `as` cast at the
             * call boundary — the capability-token use case (`GrantedFD =
             * distinct int` cannot receive a raw int). Scoped to distinct
             * types; Aether's argument checking is otherwise lenient.
             *
             * #1132 extends the same rule to bitstructs, for the same reason: a
             * bitstruct is a packed layout, not a number, so letting one slide
             * into a plain integer parameter (or vice versa) would silently
             * reinterpret it. Crossing the boundary is an explicit `as`. */
            if (param_type) {
                ASTNode* darg = call->children[arg_slot];
                if (darg) {
                    Type* da = infer_type(darg, table);
                    int nominal = param_type->distinct_name || (da && da->distinct_name) ||
                                  param_type->kind == TYPE_BITSTRUCT ||
                                  (da && da->kind == TYPE_BITSTRUCT);
                    if (da && da->kind != TYPE_UNKNOWN && nominal &&
                        !is_type_compatible(da, param_type)) {
                        int bs = (param_type->kind == TYPE_BITSTRUCT ||
                                  da->kind == TYPE_BITSTRUCT);
                        char emsg[340];
                        snprintf(emsg, sizeof(emsg),
                            "Argument %d '%s' of '%s': expected %s, got %s, a "
                            "%s type needs an explicit `as` cast at the "
                            "boundary", arg_slot + 1,
                            param->value ? param->value : "?",
                            call->value ? call->value : "?",
                            type_name(param_type), type_name(da),
                            bs ? "bitstruct" : "distinct");
                        type_error(emsg, darg->line, darg->column);
                    }
                    if (da) free_type(da);
                }
            }
            if (!param_type || param_type->kind != TYPE_DURATION) continue;

            ASTNode* arg = call->children[arg_slot];
            if (!arg) continue;
            Type* arg_type = infer_type(arg, table);
            if (!arg_type) continue;

            int arg_is_plain_numeric =
                arg_type->kind == TYPE_INT ||
                arg_type->kind == TYPE_INT64 ||
                arg_type->kind == TYPE_UINT64 ||
                arg_type->kind == TYPE_UINT32 ||
                arg_type->kind == TYPE_UINT16 ||
                arg_type->kind == TYPE_UINT8 ||
                arg_type->kind == TYPE_FLOAT ||
                arg_type->kind == TYPE_LONGDOUBLE;
            int arg_is_duration = arg_type->kind == TYPE_DURATION;
            int arg_is_unknown = arg_type->kind == TYPE_UNKNOWN;

            if (arg_is_plain_numeric && !arg_is_duration && !arg_is_unknown) {
                char error_msg[384];
                const char* pname = param->value ? param->value : "?";
                snprintf(error_msg, sizeof(error_msg),
                         "Cannot pass %s where Duration expected "
                         "(argument %d '%s' of '%s'). "
                         "Use a time-unit suffix on the literal: "
                         "5ns, 5us, 5ms, 5s, 5m, 5h, 5d (compound "
                         "forms like 2m30s also accepted).",
                         type_name(arg_type),
                         arg_slot + 1, pname,
                         call->value ? call->value : "?");
                type_error(error_msg, arg->line, arg->column);
            }
            free_type(arg_type);
        }

        /* Contract folding, call-site tier: with the parameter -> argument
         * environment built above, a `requires` / `where` predicate that is
         * decidably FALSE for THIS call's constant arguments is a compile
         * error — the call could never execute legally, so the runtime panic
         * is promoted to a build failure. `divide(10, 0)` against `b: int
         * where b != 0` fails here instead of at run time.
         *
         * UNKNOWN — any runtime operand, a call, member access, a partial
         * environment — keeps today's runtime check and says nothing; the
         * only way this can fire is when every value the predicate needs is
         * a compile-time constant. Platform-conditional code cannot
         * false-positive: `when` arms are pruned BEFORE typecheck
         * (optimizer.c), so a dead-platform call never reaches this walk.
         * These errors deliberately fire even under --no-contracts: that
         * flag removes runtime CHECKS, and suppressing free compile-time
         * correctness findings with it would conflate the two.
         *
         * Escape hatch for intentionally-unreachable calls: route the value
         * through a runtime variable — only constant arguments bind. */
        for (int ci = 0; ci < symbol->node->child_count; ci++) {
            ASTNode* cl = symbol->node->children[ci];
            if (!cl || cl->type != AST_REQUIRES_CLAUSE || cl->child_count == 0)
                continue;
            if (contract_eval_predicate(cl->children[0], &cf_env) != CONTRACT_FALSE)
                continue;
            char ptxt[512];
            ContractStr ps = { ptxt, sizeof(ptxt), 0 };
            contract_sprint_expr(&ps, cl->children[0]);
            contract_str_terminate(&ps);
            char emsg[768];
            snprintf(emsg, sizeof(emsg),
                     "precondition violation at compile time: %s in %s, this "
                     "call's constant arguments can never satisfy it (the same "
                     "check would panic at run time)",
                     ptxt, call->value ? call->value : "?");
            type_error(emsg, call->line, call->column);
            break;   /* one violation per call site is enough */
        }
    }

    // Validate argument types for extern functions (which always have typed params)
    // Skip for user-defined functions since type inference may not have set param types yet
    if (symbol->node && symbol->node->type == AST_EXTERN_FUNCTION) {
        int param_idx = 0;
        for (int i = 0; i < symbol->node->child_count - 1 && param_idx < call->child_count; i++) {
            ASTNode* param = symbol->node->children[i];
            if (!param) { param_idx++; continue; }
            if (param->type == AST_VARIABLE_DECLARATION || param->type == AST_PATTERN_VARIABLE) {
                Type* param_type = param->node_type;
                if (param_type && param_type->kind != TYPE_UNKNOWN &&
                    call->children[param_idx] != NULL) {
                    Type* arg_type = infer_type(call->children[param_idx], table);
                    if (arg_type && arg_type->kind != TYPE_UNKNOWN &&
                        !is_type_compatible(arg_type, param_type)) {
                        char error_msg[256];
                        snprintf(error_msg, sizeof(error_msg),
                                 "Argument %d of '%s': expected %s, got %s",
                                 param_idx + 1, call->value,
                                 type_name(param_type), type_name(arg_type));
                        type_error(error_msg, call->children[param_idx]->line,
                                   call->children[param_idx]->column);
                    }
                    if (arg_type) free_type(arg_type);
                }
                param_idx++;
            }
        }
    }

    /* Inference already stamped this node with a clone of the same function
     * type, and a bare assignment orphans it: one leaked function type per
     * call in the program, which was most of what the compiler leaked
     * (#1667). */
    set_node_type(call, symbol->type ? clone_type(symbol->type)
                                     : create_type(TYPE_UNKNOWN));

    // select() infers its type from the first named arg's value
    if (call->value && strcmp(call->value, "select") == 0 &&
        (!call->node_type || call->node_type->kind == TYPE_UNKNOWN)) {
        for (int i = 0; i < call->child_count; i++) {
            ASTNode* arg = call->children[i];
            if (arg && arg->type == AST_NAMED_ARG &&
                arg->child_count > 0 && arg->children[0] &&
                arg->children[0]->node_type &&
                arg->children[0]->node_type->kind != TYPE_UNKNOWN) {
                if (call->node_type) free_type(call->node_type);
                call->node_type = clone_type(arg->children[0]->node_type);
                break;
            }
        }
    }

    return 1;
}
