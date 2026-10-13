#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "typechecker.h"
#include "slice_coerce.h"
#include "sandbox_trust.h"
#include "hoist.h"
#include "type_inference.h"
#include "../aether_strmap.h"
#include "actor_reply.h"
#include "missing_return.h"
#include "slice_escape.h"
#include "quote.h"

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
    /* #2209: the merger's namespace for this module — its last segment
     * unless another loaded module ends the same way. */
    return module_namespace_of(module_path);
}

// Symbol table functions
SymbolTable* create_symbol_table(SymbolTable* parent) {
    SymbolTable* table = malloc(sizeof(SymbolTable));
    table->symbols = NULL;
    table->parent = parent;
    table->buckets = NULL;
    table->bucket_count = 0;
    table->symbol_count = 0;
    table->hidden_names = NULL;
    table->seal_whitelist = NULL;
    table->is_sealed = 0;
    // Inherit the merged-body origin so nested scopes (loops, blocks,
    // closures inside a merged function) resolve qualified names against
    // the same module's imports.
    table->merged_from = parent ? parent->merged_from : NULL;
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
    free(table->buckets);

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

// Returns 1 if a `hide` or `seal except` between `table` and the root
// blocks `prefix` as the head of a qualified name (`http` in `http.get`).
// The same rule lookup_symbol() applies to a bare name as it walks up:
// a binding of `prefix` in a scope at or below the blocking one wins,
// and a block or seal anywhere further up applies to every scope nested
// inside it, not only the scope that declared it.
static int qualified_prefix_blocked(SymbolTable* table, const char* prefix) {
    for (SymbolTable* t = table; t; t = t->parent) {
        int blocked_here = scope_name_is_hidden(t, prefix) ||
                           (t->is_sealed && !scope_name_in_whitelist(t, prefix));
        if (blocked_here) {
            for (SymbolTable* c = table; c; c = c->parent) {
                if (lookup_symbol_local(c, prefix)) return 0;
                if (c == t) break;
            }
            return 1;
        }
    }
    return 0;
}

// The member half of a qualified name whose prefix has already passed
// qualified_prefix_blocked(): `fs.exists` resolves through the mangled
// `fs_exists`, a name the user never wrote and could never whitelist.
// Checking it against hide/seal again made `seal except fs` refuse every
// `fs.<member>` call, so walk the chain without those checks.
static Symbol* lookup_member_symbol(SymbolTable* table, const char* name) {
    for (SymbolTable* t = table; t; t = t->parent) {
        Symbol* s = lookup_symbol_local(t, name);
        if (s) return s;
    }
    return NULL;
}

/* ------------------------------------------------------------------
 * #2007: the per-scope hash index.
 *
 * `symbols` stays the linked list every reader walks (newest first, so a
 * name bound twice in one scope resolves to the newer binding), and the
 * index mirrors it: each bucket is a chain of the symbols hashing there,
 * also newest first. Because both orders are insertion order, the list
 * head is always the head of its own bucket chain, which is what lets
 * pop_symbol remove it from both in O(1).
 *
 * A scope below SYMTAB_INDEX_MIN symbols is looked up by walking the
 * list, as before: a function body with four locals does not need a
 * table. The index is built the first time a scope crosses the
 * threshold and rebuilt when it outgrows its load.
 * ------------------------------------------------------------------ */
#define SYMTAB_INDEX_MIN 16

#define symtab_hash strmap_hash

static void symtab_rebuild_index(SymbolTable* table, int bucket_count) {
    Symbol** buckets = calloc((size_t)bucket_count, sizeof(Symbol*));
    if (!buckets) return;                     /* keep the old index or none */
    free(table->buckets);
    table->buckets = buckets;
    table->bucket_count = bucket_count;
    /* Chain oldest first so that each chain ends up newest-first, the
     * same order as the list. */
    int n = table->symbol_count;
    Symbol** order = malloc(sizeof(Symbol*) * (size_t)(n > 0 ? n : 1));
    if (!order) { free(table->buckets); table->buckets = NULL; table->bucket_count = 0; return; }
    int i = 0;
    for (Symbol* s = table->symbols; s && i < n; s = s->next) order[i++] = s;
    for (int k = i - 1; k >= 0; k--) {
        Symbol* s = order[k];
        unsigned b = symtab_hash(s->name) & (unsigned)(bucket_count - 1);
        s->hash_next = buckets[b];
        buckets[b] = s;
    }
    free(order);
}

/* Link a freshly created symbol at the head of the list and of its bucket. */
static void symtab_link(SymbolTable* table, Symbol* symbol) {
    symbol->next = table->symbols;
    symbol->hash_next = NULL;
    table->symbols = symbol;
    table->symbol_count++;
    if (!table->buckets) {
        if (table->symbol_count >= SYMTAB_INDEX_MIN) symtab_rebuild_index(table, 64);
        return;
    }
    if (table->symbol_count * 4 > table->bucket_count * 3) {
        symtab_rebuild_index(table, table->bucket_count * 2);
        return;                               /* the rebuild chained it */
    }
    unsigned b = symtab_hash(symbol->name) & (unsigned)(table->bucket_count - 1);
    symbol->hash_next = table->buckets[b];
    table->buckets[b] = symbol;
}

void pop_symbol(SymbolTable* table) {
    if (!table || !table->symbols) return;
    Symbol* symbol = table->symbols;
    table->symbols = symbol->next;
    table->symbol_count--;
    if (table->buckets) {
        unsigned b = symtab_hash(symbol->name) & (unsigned)(table->bucket_count - 1);
        if (table->buckets[b] == symbol) {
            table->buckets[b] = symbol->hash_next;
        } else {
            /* Cannot happen while the list is only ever changed through
             * this file, but a chain walk is the safe answer if it did. */
            for (Symbol* s = table->buckets[b]; s; s = s->hash_next) {
                if (s->hash_next == symbol) { s->hash_next = symbol->hash_next; break; }
            }
        }
    }
    if (symbol->name) free(symbol->name);
    if (symbol->type) free_type(symbol->type);
    if (symbol->alias_target) free(symbol->alias_target);
    free(symbol);
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
    symbol->inferred_in = NULL;
    symbol->walk_id = 0;
    symbol->branch_hoisted = 0;
    symbol->hoist_pending = 0;
    symbol->is_const = 0;
    symbol->inferred_closure = NULL;
    symbol->global_write = 0;
    symtab_link(table, symbol);
}

static Symbol* lookup_symbol_in(SymbolTable* table, const char* name, const char* merged_from);
static const char* find_import_alias(const char* name);

/* The program's own top-level `var`s (no origin_module), named once per
 * typecheck_program before anything is looked up. A module's code never
 * sees them: from a merged body (merged_from set) lookup_symbol_in does not
 * resolve one, so a module's bare `n = ...` binds its own local instead of
 * type-checking against, or writing, the program's `n`. Only a file-scope
 * entry no walk added is hidden: the inference pass pushes a function's
 * locals onto the same flat table, stamped with its walk id
 * (aether-ui asks/aether-program-global-leaks-into-module-scope.md). */
static const char** g_program_globals = NULL;
static int g_program_global_count = 0;

static int is_program_global_name(const char* name) {
    for (int i = 0; i < g_program_global_count; i++)
        if (strcmp(g_program_globals[i], name) == 0) return 1;
    return 0;
}

static void note_program_globals(ASTNode* program) {
    for (int i = 0; i < g_program_global_count; i++) free((char*)g_program_globals[i]);
    free(g_program_globals);
    g_program_globals = NULL;
    g_program_global_count = 0;
    if (!program) return;
    int n = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_CONST_DECLARATION && c->value && !c->origin_module &&
            c->annotation && strcmp(c->annotation, "global_var") == 0) n++;
    }
    if (n == 0) return;
    g_program_globals = malloc(sizeof(char*) * (size_t)n);
    if (!g_program_globals) return;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_CONST_DECLARATION && c->value && !c->origin_module &&
            c->annotation && strcmp(c->annotation, "global_var") == 0)
            if ((g_program_globals[g_program_global_count] = strdup(c->value)))
                g_program_global_count++;
    }
}

Symbol* lookup_symbol(SymbolTable* table, const char* name) {
    if (!table || !name) return NULL;
    return lookup_symbol_in(table, name, table->merged_from);
}

/* `merged_from` is the scope the lookup STARTED in: a merged body's, even
 * when the walk reaches a file scope whose own merged_from says otherwise. */
static Symbol* lookup_symbol_in(SymbolTable* table, const char* name, const char* merged_from) {
    if (!table || !name) return NULL;

    // Local bindings always win — `hide` and `seal except` only block
    // resolution that would walk OUT of this scope into a parent.
    Symbol* symbol = lookup_symbol_local(table, name);
    /* #2683: a hoisted local not yet bound in source order does not hide
     * the function of its name (see Symbol.hoist_pending); the walk goes on
     * outward to it. */
    if (symbol && symbol->hoist_pending) symbol = NULL;
    /* The program's own top-level `var`s are the program's: a module's code
     * (a merged body, table->merged_from) never resolves to one. A module's
     * own globals are merged under `<ns>_name`, so a bare name there that
     * finds a file-scope var found the program's. It did, and a module
     * function's `ok = file_delete_raw(path)` wrote a program's `var ok`
     * (aether-ui asks/aether-program-global-leaks-into-module-scope.md). */
    if (symbol && merged_from && !table->parent && symbol->walk_id == 0 &&
        !symbol->is_function && !symbol->is_actor && is_program_global_name(name))
        return NULL;
    /* Nor does it see the program's imports. The program's selective and
     * glob imports bind bare names (`import std.http (server_create)`)
     * here at file scope; a module's own imports were renamed to the
     * prefixed name (`http_server_create`) when it was merged. So a bare
     * name a merged body finds through the program's import is one the
     * module never imported, and whether it built depended on which
     * program imported the module (aeo's runner and agent disagreed). */
    if (symbol && merged_from && !table->parent && find_import_alias(name))
        return NULL;
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
        return lookup_symbol_in(table->parent, name, merged_from);
    }

    return NULL;
}

Symbol* lookup_symbol_local(SymbolTable* table, const char* name) {
    if (table->buckets) {
        /* #2007: the indexed path. The chain is newest-first like the
         * list, so the first hit is the binding the list walk would
         * have returned. */
        unsigned b = symtab_hash(name) & (unsigned)(table->bucket_count - 1);
        for (Symbol* s = table->buckets[b]; s; s = s->hash_next) {
            if (strcmp(s->name, name) == 0) return s;
        }
        return NULL;
    }
    Symbol* current = table->symbols;
    /* The scope chain is a linked list and this is the compiler's hottest
     * loop: a profile of one stdlib module puts strcmp at the top, reached
     * from here. Nearly every candidate differs in its first byte, so testing
     * that inline settles the comparison without a call. Both strings are
     * NUL-terminated, so index 0 is always readable, and a differing first
     * byte is exactly the case where strcmp would have returned non-zero. */
    int c0 = (unsigned char)name[0];
    while (current) {
        if ((unsigned char)current->name[0] == c0 &&
            strcmp(current->name, name) == 0) {
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
    symbol->type_inferred = 0;
    symbol->width_explicit = 0;
    symbol->inferred_in = NULL;
    symbol->walk_id = 0;
    symbol->branch_hoisted = 0;
    symbol->hoist_pending = 0;
    symbol->is_const = 0;
    symbol->inferred_closure = NULL;
    symbol->global_write = 0;
    symtab_link(table, symbol);
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
// Grown dynamically: a whole-program `--emit=lib` merge can register well over
// 64 namespaces via the transitive import BFS (e.g. std.http.client pulls the
// entire std.cryptography/TLS graph). A fixed cap silently dropped every
// namespace past the 64th, so a compiler-generated qualified call into a
// dropped namespace — an auto `sha1.free_ctx` cleanup for a transitively-reached
// Sha1Ctx — failed to resolve with a spurious E0301 once the graph crossed the
// threshold. The bug presented as "add one more module and it breaks."
static char** imported_namespaces = NULL;
static int namespace_count = 0;
static int namespace_cap = 0;
static char** user_explicit_namespaces = NULL;
static int user_explicit_namespace_count = 0;
static int user_explicit_namespace_cap = 0;

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
    // Check if already registered
    for (int i = 0; i < namespace_count; i++) {
        if (strcmp(imported_namespaces[i], ns) == 0) return;
    }
    if (namespace_count == namespace_cap) {
        int newcap = namespace_cap ? namespace_cap * 2 : 64;
        char** grown = (char**)realloc(imported_namespaces, (size_t)newcap * sizeof(char*));
        if (!grown) return;   /* OOM: skip rather than crash; worst case a spurious E0301 */
        imported_namespaces = grown;
        namespace_cap = newcap;
    }
    imported_namespaces[namespace_count++] = strdup(ns);
}

// Issue #243 sealed-scope follow-up. Records that the user *explicitly*
// wrote `import <module>` for the given namespace leaf. Called from the
// AST_IMPORT_STATEMENT visitor only when the import is not flagged
// synthetic (`annotation == "synthetic"`) — which means the import came
// from the user's source rather than from the BFS transitive-merge
// pass. Used by user-code qualified-call resolution to reject
// `lib_b.shout()` when the user only wrote `import lib_a`.
static void register_user_explicit_namespace(const char* ns) {
    for (int i = 0; i < user_explicit_namespace_count; i++) {
        if (strcmp(user_explicit_namespaces[i], ns) == 0) return;
    }
    if (user_explicit_namespace_count == user_explicit_namespace_cap) {
        int newcap = user_explicit_namespace_cap ? user_explicit_namespace_cap * 2 : 64;
        char** grown = (char**)realloc(user_explicit_namespaces, (size_t)newcap * sizeof(char*));
        if (!grown) return;
        user_explicit_namespaces = grown;
        user_explicit_namespace_cap = newcap;
    }
    user_explicit_namespaces[user_explicit_namespace_count++] = strdup(ns);
}

static int is_user_explicit_namespace(const char* name) {
    for (int i = 0; i < user_explicit_namespace_count; i++) {
        if (strcmp(user_explicit_namespaces[i], name) == 0) return 1;
    }
    return 0;
}

// Forward decl — defined below alongside the global registry it gates.
int is_imported_namespace(const char* name);

/* #2631: when the program being checked is itself a module file (it has an
 * `exports(...)` list, as `ae check lib/m/module.ae` sees it), the namespace
 * a build gives that module: the last segment it is imported under. A build
 * merges the module's functions as `<ns>_<name>` and its own `m.a()` calls
 * resolve to them, as a module sees itself (module_sees_namespace); checked
 * on its own, the file used to know no namespace at all, so `ae check`
 * rejected a call `ae build` accepted. NULL for a program. */
static char g_entry_self_ns_buf[256];
static const char* g_entry_self_ns = NULL;

/* The program's own (not merged) top-level definition named `name` that a
 * build reaches as `<ns>.<name>`: a function, builder, constant or
 * `@extern`, or NULL. */
static ASTNode* entry_own_definition(const char* name) {
    ASTNode* program = aether_typecheck_program_node();
    if (!program || !name) return NULL;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* d = program->children[i];
        if (d && d->type == AST_EXPORT_STATEMENT && d->child_count > 0) d = d->children[0];
        if (!d || !d->value || d->is_imported || d->origin_module) continue;
        int reachable = d->type == AST_FUNCTION_DEFINITION ||
                        d->type == AST_BUILDER_FUNCTION ||
                        d->type == AST_CONST_DECLARATION ||
                        (d->type == AST_EXTERN_FUNCTION && d->annotation &&
                         strncmp(d->annotation, "c_symbol:", 9) == 0);
        if (reachable && strcmp(d->value, name) == 0) return d;
    }
    return NULL;
}

/* #2632: does the file being compiled define `name` itself: a function,
 * builder, constant or extern written in it, not one merged in from a
 * module? Such a name shadows the same name a glob import would bind. */
static int program_defines_own(ASTNode* program, const char* name) {
    if (!program || !name) return 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* d = program->children[i];
        if (d && d->type == AST_EXPORT_STATEMENT && d->child_count > 0) d = d->children[0];
        if (!d || !d->value || d->is_imported || d->origin_module) continue;
        if ((d->type == AST_FUNCTION_DEFINITION || d->type == AST_BUILDER_FUNCTION ||
             d->type == AST_CONST_DECLARATION || d->type == AST_EXTERN_FUNCTION) &&
            strcmp(d->value, name) == 0) return 1;
    }
    return 0;
}

/* Is `prefix.name` a reference to the checked module's own `name`? A name
 * the module does not define falls through to the imports, so a module
 * importing a namespace that shares its last segment (#1780) still reaches
 * it for the rest; its own names come first, as in a build. */
static int entry_self_reference(const char* prefix, const char* name) {
    return g_entry_self_ns && prefix && strcmp(prefix, g_entry_self_ns) == 0 &&
           entry_own_definition(name) != NULL;
}

/* Does the checked module's `exports(...)` list leave `name` out? Read the
 * way a build reads it (module_exports_symbol): the bare name, or the
 * `<leaf>_name` spelling a std module lists. */
static int entry_export_blocked(const char* name) {
    ASTNode* program = aether_typecheck_program_node();
    if (!program || !name) return 0;
    char prefixed[512];
    snprintf(prefixed, sizeof(prefixed), "%s_%s",
             g_entry_self_ns ? g_entry_self_ns : "", name);
    int has_list = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* list = program->children[i];
        if (!list || list->type != AST_EXPORTS_LIST) continue;
        has_list = 1;
        for (int k = 0; k < list->child_count; k++) {
            ASTNode* n = list->children[k];
            if (n && n->value &&
                (strcmp(n->value, name) == 0 || strcmp(n->value, prefixed) == 0))
                return 0;
        }
    }
    return has_list;
}

/* The file scope a qualified own name resolves in: a local or parameter
 * that happens to share the name is not the module's definition. */
static Symbol* entry_self_symbol(SymbolTable* table, const char* name) {
    SymbolTable* root = table;
    while (root && root->parent) root = root->parent;
    return root ? lookup_symbol_local(root, name) : NULL;
}

// Gate for qualified-call resolution. A qualified call `mod.fn()` is
// allowed if either:
//   - The caller is inside a merged-module body (SymbolTable::merged_from,
//     propagated from the cloned declaration's origin_module): the
//     namespace is that module's own or one it imports. A module reaches
//     what it imported, not what some other module of the program did
//     (#2614): `top` calling `low.f()` while only `mid` imports `low`
//     built in every program and failed `ae check` of `top`, so `top`
//     worked by the accident of what else was linked in, and broke with
//     an error pointing at untouched code the day `mid` dropped `low`.
//   - The caller is user code (merged_from NULL); in that case only
//     namespaces the user explicitly imported are visible. This closes
//     the encapsulation hole left after the round-1 BFS-merge fix for
//     issue #243.
//
// `table` may be NULL during early symbol-table population; treat NULL
// as user-context (the strict path) — early registration paths don't
// resolve qualified user calls, so this is safe.
int is_visible_namespace(const char* name, SymbolTable* table) {
    /* Single channel: the SymbolTable's merged_from. Both walkers (the
     * typechecker, which creates per-function child tables, and the
     * type-inference pass, which walks against the global symbol table
     * directly) set it while inside a merged declaration, then restore
     * it on exit. */
    if (table && table->merged_from) {
        return module_sees_namespace(table->merged_from, name);
    }
    /* #2631: a module file checked on its own sees itself, as its merged
     * functions do in a build (module_sees_namespace); nothing more. */
    if (g_entry_self_ns && name && strcmp(name, g_entry_self_ns) == 0) return 1;
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

static AetherModule* module_find_by_name_or_leaf(const char* name);

// Check if a symbol is blocked by export visibility.
// Returns 1 if blocked (module has exports and symbol isn't one), 0 if allowed.
//
// #2172: the module is found by its full name OR its leaf. A qualified use
// carries the leaf (`language.to_title_case`) while a std module registers
// under its full path (`std.language`), so an exact-name lookup found no
// module for ANY std module and blocked nothing: every std module's export
// list was advisory, and `mem.long_to_ptr` was called all over the tree
// while missing from std.mem's. User modules register under the name they
// are used by, which is why they were enforced all along.
static int is_export_blocked(const char* namespace, const char* symbol) {
    /* #2631: the checked module's own name, held to its own exports list
     * as a build holds it (`selfq.hidden()` is E0303 in both). */
    if (entry_self_reference(namespace, symbol)) return entry_export_blocked(symbol);
    if (!global_module_registry) return 0;
    AetherModule* mod = module_find_by_name_or_leaf(namespace);
    return (mod && mod->export_count > 0 && !module_exports_symbol(mod, symbol));
}

/* Find a registered module by exact name, or by the last dot-component
 * of its name. Std modules register under their full path ("std.os")
 * while qualified call sites carry the leaf ("os.getenv"), so a plain
 * module_find(prefix) misses them. First leaf match wins; the caller's
 * exports gate keeps an ambiguous leaf from resolving anything the
 * matched module doesn't explicitly export. (#1035) */
static AetherModule* module_find_by_name_or_leaf(const char* name) {
    /* #2209: a qualified use now carries the module's assigned namespace,
     * which is the leaf unless two loaded modules share one. */
    return module_find_by_namespace(name);
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
    /* Every qualified lookup used to malloc, copy and free the name just to
     * split it on the dot. Identifiers are short, so the copy lives on the
     * stack unless it genuinely does not fit. */
    char name_stack[256];
    char* name_copy;
    int name_heap = 0;
    size_t qn_len = strlen(qualified_name);
    if (qn_len < sizeof(name_stack)) {
        memcpy(name_stack, qualified_name, qn_len + 1);
        name_copy = name_stack;
    } else {
        name_copy = strdup(qualified_name);
        if (!name_copy) return NULL;
        name_heap = 1;
    }
    char* dot = strchr(name_copy, '.');

    if (dot) {
        *dot = '\0';
        const char* prefix = name_copy;
        const char* suffix = dot + 1;

        // Enforce hide / seal on the prefix before any namespace resolution.
        // `hide http` must block both bare `http` AND `http.get(url)`, in
        // the hiding block and in every block, closure and trailing block
        // nested inside it.
        if (qualified_prefix_blocked(table, prefix)) {
            if (name_heap) free(name_copy);
            return NULL;
        }

        // Check if prefix is a module alias
        Symbol* alias_sym = resolve_module_alias(table, prefix);
        if (alias_sym && alias_sym->alias_target) {
            // Reconstruct with actual module name
            char resolved_name[512];
            snprintf(resolved_name, sizeof(resolved_name), "%s.%s",
                    alias_sym->alias_target, suffix);
            if (name_heap) free(name_copy);
            return lookup_symbol(table, resolved_name);
        }

        // Check if prefix is a namespace visible from this scope.
        // Issue #243, #2614: user code can only see namespaces it
        // explicitly imported; merged-body code sees its own module and
        // the modules that module imports. is_visible_namespace picks the
        // right set based on the table's merged_from.
        // Convert string.new -> string_new
        if (is_visible_namespace(prefix, table)) {
            // Enforce export visibility
            if (is_export_blocked(prefix, suffix)) {
                if (name_heap) free(name_copy);
                return NULL;
            }
            // #878: no selective-import gate here — the qualified `X.fn()`
            // surface is available whenever the namespace is visible, in any
            // import form. (export visibility + hide/seal above still apply.)
            char c_func_name[512];
            snprintf(c_func_name, sizeof(c_func_name), "%s_%s", prefix, suffix);
            Symbol* sym = lookup_member_symbol(table, c_func_name);
            /* #924 re-export: `hub.fn()` where hub re-exports an imported
             * `fn`. No local `hub_fn` symbol exists; redirect to the
             * defining module's `<origin>_fn`. */
            if (!sym && global_module_registry) {
                AetherModule* hub = module_find_by_namespace(prefix);
                AetherModule* origin = module_resolve_reexport(hub, suffix);
                if (origin && origin->name) {
                    char c_origin[512];
                    snprintf(c_origin, sizeof(c_origin), "%s_%s",
                             module_namespace_of(origin->name), suffix);
                    sym = lookup_member_symbol(table, c_origin);
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
                    sym = lookup_member_symbol(table, suffix);
                }
            }
            /* #2631: the checked module file's own definition, which a
             * build would have merged as `<prefix>_<suffix>`. The caller
             * rewrites the call to the bare name it carries, as #1035. */
            if (!sym && !(table && table->merged_from) &&
                entry_self_reference(prefix, suffix)) {
                sym = entry_self_symbol(table, suffix);
            }
            if (name_heap) free(name_copy);
            return sym;
        }
    }

    if (name_heap) free(name_copy);
    return lookup_symbol(table, qualified_name);
}

/* The file the top-level definition being checked came from. Modules are
 * merged into one program before checking, so a diagnostic raised inside an
 * imported function carried the module's line under the MAIN file's name
 * (`main.ae:4:17` for `noise/module.ae:4:17`, #2064) — a location that
 * exists and is wrong. typecheck_program sets this from each top-level
 * node's source_file; the reporter reads the module file for its snippet. */
static const char* g_tc_file = NULL;

static void tc_report(const char* message, int line, int column, AetherErrorCode code) {
    AetherError e = { g_tc_file, NULL, line, column, message, NULL, NULL, code };
    aether_error_report(&e);
}

/* The builtins whose calls codegen lowers by NAME, with the arity its
 * dispatch keys on (the `strcmp(func_name, "...")` chain in
 * generate_expression's call case; max -1 = any). A user definition with a
 * parameter count in that range would be defined and never called. A
 * definition outside it (a two-argument `isolate`, a zero-argument
 * `select`) is a plain function and stays legal. `atoi` yields to a user
 * definition in codegen and is not listed. */
static int builtin_lowered_by_name(const char* name, int params) {
    static const struct { const char* name; int min; int max; } names[] = {
        {"isolate", 1, 1}, {"consume", 1, 1}, {"release", 1, 1},
        {"call", 1, -1}, {"select", 1, -1}, {"force", 1, 1}, {"lazy", 1, 1},
        {"typeof", 0, -1}, {"is_type", 0, -1}, {"convert_type", 0, -1},
        {"char_at", 1, -1}, {"str_eq", 2, 2},
        {"ref", 1, 1}, {"ref_get", 1, 1}, {"ref_set", 2, 2}, {"ref_free", 1, 1},
        {"box_closure", 1, 1}, {"unbox_closure", 1, 1},
        {"print", 0, -1}, {"println", 0, -1}, {"print_char", 1, -1},
        {"read_char", 0, 0}, {"raw_mode", 0, 0}, {"cooked_mode", 0, 0},
        {"exit", 0, -1}, {"sleep", 1, 1}, {"free", 1, 1}, {"getenv", 1, 1},
        {"clock_ns", 0, 0}, {"wait_for_idle", 0, -1},
        {"builder_context", 0, -1}, {"builder_depth", 0, -1}, {"thunk_free", 1, 1},
        {"string_free", 1, 1}, {"string_release", 1, 1}, {"string_seq_free", 1, 1},
        {"sandbox_install", 0, 0}, {"sandbox_uninstall", 0, 0},
        {"sandbox_push", 1, 1}, {"sandbox_pop", 0, 0}, {"spawn_sandboxed", 2, -1},
        {NULL, 0, 0}
    };
    for (int i = 0; names[i].name; i++) {
        if (strcmp(name, names[i].name) != 0) continue;
        return params >= names[i].min && (names[i].max < 0 || params <= names[i].max);
    }
    return 0;
}

int is_builtin_function_name(const char* name) {
    if (!name) return 0;
    /* The table's smallest minimum arity is 2 (`str_eq`), so asking at 0, 1
     * and 2 parameters reaches every entry. */
    for (int params = 0; params <= 2; params++) {
        if (builtin_lowered_by_name(name, params)) return 1;
    }
    return 0;
}

/* Parameters of a function definition: every child that is not a body,
 * guard or contract clause. */
static int definition_param_count(ASTNode* def) {
    int count = 0;
    for (int i = 0; i < def->child_count; i++) {
        ASTNode* p = def->children[i];
        if (p && p->type != AST_GUARD_CLAUSE && p->type != AST_BLOCK &&
            p->type != AST_REQUIRES_CLAUSE && p->type != AST_ENSURES_CLAUSE) count++;
    }
    return count;
}

/* #698's narrowing message, for the two kinds of value that truncate when
 * stored into an INFERRED 32-bit int: a 64-bit integer, and — since the
 * same slot kept `int` storage while the checker went on typing the name
 * as float, so `${r}` printed garbage through `%g` — a float. */
static void narrowing_message(char* msg, size_t size, const char* nm, TypeKind assigned) {
    if (assigned == TYPE_FLOAT || assigned == TYPE_FLOAT32 || assigned == TYPE_LONGDOUBLE) {
        snprintf(msg, size,
            "narrowing assignment to '%s': its type was inferred as int from "
            "its initializer, but a float is assigned here and would truncate. "
            "Annotate the declaration to keep the fraction (e.g. `float %s = ...`), "
            "or write `int %s = ...` to make the narrowing explicit.", nm, nm, nm);
    } else if (assigned == TYPE_UINT32) {
        snprintf(msg, size,
            "narrowing assignment to '%s': its type was inferred as int from "
            "its initializer, but a uint32 is assigned here and values past "
            "2^31 would not fit. Annotate the declaration to keep it unsigned "
            "(e.g. `uint32 %s = ...`), or write `int %s = ...` to make the "
            "narrowing explicit.", nm, nm, nm);
    } else {
        snprintf(msg, size,
            "narrowing assignment to '%s': its type was inferred "
            "as 32-bit int from its initializer, but a 64-bit "
            "value is assigned here and would truncate. Annotate "
            "the declaration to keep 64 bits (e.g. `long %s = ...` "
            "or `uint64 %s = ...`), or write `int %s = ...` to "
            "make the narrowing explicit.", nm, nm, nm, nm);
    }
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
    tc_report(message, line, column, code);
    error_count++;
}

/* type_error with a `help:` line, reported against the same file. */
static void type_error_hint(const char* message, const char* hint, int line, int column) {
    AetherError e = { g_tc_file, NULL, line, column, message, hint, NULL,
                      AETHER_ERR_TYPE_MISMATCH };
    aether_error_report(&e);
    error_count++;
}

/* type_error_hint under a chosen error code. */
static void type_error_code_hint(const char* message, const char* hint,
                                 int line, int column, AetherErrorCode code) {
    AetherError e = { g_tc_file, NULL, line, column, message, hint, NULL, code };
    aether_error_report(&e);
    error_count++;
}

/* The program-level `const NAME = ...` (not a module `var`), or NULL: a
 * quote reads a constant with a literal value as that literal. */
ASTNode* tc_program_const(const char* name) {
    ASTNode* program = aether_typecheck_program_node();
    for (int i = 0; program && name && i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_CONST_DECLARATION && c->value && strcmp(c->value, name) == 0 &&
            !(c->annotation && strcmp(c->annotation, "global_var") == 0))
            return c;
    }
    return NULL;
}

/* The program-level `var NAME = ...` (a global variable), or NULL: a quote
 * reads one as a capture whose cell is the global itself. */
ASTNode* tc_program_global_var(const char* name) {
    ASTNode* program = aether_typecheck_program_node();
    for (int i = 0; program && name && i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_CONST_DECLARATION && c->value && strcmp(c->value, name) == 0 &&
            c->annotation && strcmp(c->annotation, "global_var") == 0)
            return c;
    }
    return NULL;
}

/* analysis/quote.c reports a construct a quote cannot hold through this. */
static void quote_report_error(const char* message, const char* hint, int line,
                               int column, AetherErrorCode code) {
    type_error_code_hint(message, hint, line, column, code);
}

/* #2614: when `prefix` in `prefix.name` is a module the program loaded but
 * the scope does not import, write the `help:` line naming the import and
 * return 1. Such a module is loaded because some other module imports it,
 * which is exactly why the call used to build: say so, since "check the
 * spelling" sends the reader the wrong way. A local of that name is not a
 * module reference, so it gets no hint. */
static int missing_import_hint(const char* prefix, SymbolTable* table,
                               char* out, size_t cap) {
    if (!prefix || !global_module_registry || is_visible_namespace(prefix, table))
        return 0;
    Symbol* local = lookup_symbol(table, prefix);
    if (local && !local->is_module_alias) return 0;
    AetherModule* m = module_find_by_namespace(prefix);
    if (!m || !m->name) return 0;
    if (table && table->merged_from) {
        snprintf(out, cap,
                 "module '%s' does not import '%s': add `import %s` to it "
                 "(another module of the program importing '%s' does not "
                 "make it visible here)",
                 table->merged_from, m->name, m->name, m->name);
    } else {
        snprintf(out, cap,
                 "'%s' is not imported by this file: add `import %s` "
                 "(another module of the program importing it does not make "
                 "it visible here)", m->name, m->name);
    }
    return 1;
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
    /* A tuple-typed operand is admitted above because a BOUND name often
     * carries a tuple type it does not hold. A call is not a binding: the
     * whole (value, err) struct went to printf and rendered as garbage. */
    if (t && t->kind == TYPE_TUPLE && child->type == AST_FUNCTION_CALL) {
        char msg[320];
        snprintf(msg, sizeof(msg),
            "cannot interpolate a call that returns %d values: destructure it "
            "first, e.g. `v, err = %s(...)` then `${v}`",
            t->tuple_count, child->value ? child->value : "f");
        type_error(msg, line, col);
        return;
    }
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
static int binary_mixes_distinct(Type* lt, Type* rt, AeTokenType operator);
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
    if (node->type == AST_CLOSURE || node->type == AST_QUOTE ||
        node->type == AST_FUNCTION_DEFINITION) return;
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
        .filename = g_tc_file, .source_code = NULL,
        .line = line, .column = column,
        .message = message, .suggestion = NULL,
        .context = NULL, .code = AETHER_ERR_NONE
    };
    aether_warning_report(&w);
    warning_count++;
}

/* True iff s is non-empty and every character is an ASCII digit — i.e. a bare
 * decimal integer literal (no sign, no suffix, no 0x). Used to spot a literal
 * byte count fed to `malloc(...) as *T`. */
static int is_all_digits(const char* s) {
    if (!s || !*s) return 0;
    for (const char* p = s; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
    }
    return 1;
}

// Return a human-readable type name (static buffer — for error messages only)
static const char* type_name_of_kind(TypeKind k) {
    switch (k) {
        case TYPE_F32X4: return "f32x4";
        case TYPE_F64X2: return "f64x2";
        case TYPE_I32X4: return "i32x4";
        case TYPE_I64X2: return "i64x2";
        case TYPE_I16X8: return "i16x8";
        case TYPE_F32X8: return "f32x8";
        case TYPE_I32X8: return "i32x8";
        default: return "a lane type";
    }
}

/* The name of t's representation: a distinct type reads as its base. */
static const char* base_type_name(Type* t) {
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
        case TYPE_FLOAT32:  return "f32";
        case TYPE_F32X4:    return "f32x4";
        case TYPE_F64X2:    return "f64x2";
        case TYPE_I32X4:    return "i32x4";
        case TYPE_I64X2:    return "i64x2";
        case TYPE_I16X8:    return "i16x8";
        case TYPE_F32X8:    return "f32x8";
        case TYPE_I32X8:    return "i32x8";
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
        case TYPE_SUM:      return t->struct_name ? t->struct_name : "sum";
        /* Named nominal types report their own name — "expected uint8_t, got B"
         * is actionable where "got unknown" is not. */
        case TYPE_ENUM:     return t->struct_name ? t->struct_name : "enum";
        case TYPE_BITSTRUCT: return t->struct_name ? t->struct_name : "bitstruct";
        case TYPE_BITSET:   return "bit_set";
        case TYPE_UNKNOWN:  return "unknown";
        default:            return "unknown";
    }
}

/* t as the user named it: a distinct type by its own name, which is what
 * tells `expected Tag, got string` apart from a plain mismatch. */
static const char* type_name(Type* t) {
    if (t && t->distinct_name) return t->distinct_name;
    /* A typed pointer as it is written, `*Apple`. As "ptr", a message about
     * `*Apple` and `*Pear` named both the same (#2611, #2624). A `const *T`
     * keeps its C spelling from base_type_name. */
    if (t && t->kind == TYPE_PTR && !t->c_alias && t->element_type &&
        t->element_type->kind != TYPE_UNKNOWN)
        return aether_internf("*%s", type_name(t->element_type));
    /* A typed C function pointer as it is written, `fn(ptr) -> string`. As
     * "closure" it was named after the other representation, the one it
     * cannot hold (#2634). */
    if (t && t->kind == TYPE_FUNCTION && t->is_fnptr) {
        char buf[256];
        size_t n = (size_t)snprintf(buf, sizeof(buf), "fn(");
        for (int i = 0; i < t->param_count && t->param_types && n < sizeof(buf); i++)
            n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s%s", i ? ", " : "",
                                  type_name(t->param_types[i]));
        if (n < sizeof(buf))
            n += (size_t)snprintf(buf + n, sizeof(buf) - n, ")");
        if (n < sizeof(buf) && t->return_type && t->return_type->kind != TYPE_VOID)
            snprintf(buf + n, sizeof(buf) - n, " -> %s", type_name(t->return_type));
        return aether_internf("%s", buf);
    }
    return base_type_name(t);
}

/* #2516: a fixed-size array or slice spelled as written (`int[3]`,
 * `string[]`); type_name says only "array". */
static const char* array_type_spell(Type* t, char* buf, size_t n) {
    if (!t || t->kind != TYPE_ARRAY) return type_name(t);
    if (t->array_size > 0)
        snprintf(buf, n, "%s[%d]", type_name(t->element_type), t->array_size);
    else
        snprintf(buf, n, "%s[]", type_name(t->element_type));
    return buf;
}

/* #2516: the binding `decl` just registered in `table` is a `const` (a
 * module `var` shares the node kind but is writable). */
static void mark_const_symbol(SymbolTable* table, ASTNode* decl) {
    if (!decl || decl->type != AST_CONST_DECLARATION || !decl->value ||
        (decl->annotation && strcmp(decl->annotation, "global_var") == 0)) return;
    Symbol* s = lookup_symbol_local(table, decl->value);
    if (s) s->is_const = 1;
}

/* #2516: a write to an element (or a field) of a `const`: `TABLE[i] = v`,
 * `TABLE[i] op= v`, `TABLE[i]++`. A const array is a read-only table (a
 * C `static const`), so the write is refused here rather than by the C
 * compiler against generated code. A bare `NAME = v` is not such a write:
 * it binds a local of that name, which shadows the constant. Returns 1
 * when it reported. */
static int reject_const_element_write(ASTNode* target, SymbolTable* table, ASTNode* at) {
    ASTNode* root = target;
    while (root && (root->type == AST_ARRAY_ACCESS || root->type == AST_MEMBER_ACCESS) &&
           root->child_count > 0)
        root = root->children[0];
    if (!root || root == target || root->type != AST_IDENTIFIER || !root->value) return 0;
    Symbol* s = lookup_symbol(table, root->value);
    if (!s || !s->is_const) return 0;
    char msg[300];
    snprintf(msg, sizeof(msg),
             "cannot write to an element of '%s': it is a constant, a read-only table",
             root->value);
    type_error_hint(msg, "copy it into an array of your own (`t = [...]`) and change that",
                    at->line, at->column);
    return 1;
}

static int is_integer_scalar(TypeKind kind) {
    return kind == TYPE_INT || kind == TYPE_INT64 || kind == TYPE_UINT64 ||
           kind == TYPE_UINT32 || kind == TYPE_UINT16 || kind == TYPE_UINT8;
}

/* #2146: the vector lane types. Lane-wise arithmetic, a comparison that
 * yields a mask rather than a bool, and no implicit conversion to or from
 * anything — a lane value is built with `lanes.splat` / `lanes.set` and
 * read back with `lanes.lane` / `.x`, so it can never be mistaken for a
 * scalar the way a one-element array might. */
static int is_lane_type(TypeKind kind) {
    return kind == TYPE_F32X4 || kind == TYPE_F64X2 ||
           kind == TYPE_I32X4 || kind == TYPE_I64X2 ||
           kind == TYPE_I16X8 ||
           kind == TYPE_F32X8 || kind == TYPE_I32X8;
}

/* The mask a comparison of this lane type yields: the SAME register width,
 * one integer lane per value lane. An f64x2 compared as an i32x4 would
 * reinterpret two 64-bit lanes as four 32-bit ones and scramble them. */
static TypeKind lane_mask_kind(TypeKind kind) {
    switch (kind) {
        case TYPE_F32X4: case TYPE_I32X4: return TYPE_I32X4;
        case TYPE_F64X2: case TYPE_I64X2: return TYPE_I64X2;
        case TYPE_I16X8: return TYPE_I16X8;
        case TYPE_F32X8: case TYPE_I32X8: return TYPE_I32X8;
        default: return TYPE_UNKNOWN;
    }
}

/* The scalar a lane of this vector holds, TYPE_UNKNOWN for a non-lane. */
static TypeKind lane_scalar_kind(TypeKind kind) {
    if (kind == TYPE_F32X4) return TYPE_FLOAT32;
    if (kind == TYPE_F64X2) return TYPE_FLOAT;
    if (kind == TYPE_I32X4) return TYPE_INT;
    if (kind == TYPE_I64X2) return TYPE_INT64;
    if (kind == TYPE_I16X8) return TYPE_INT;
    if (kind == TYPE_F32X8) return TYPE_FLOAT32;
    if (kind == TYPE_I32X8) return TYPE_INT;
    return TYPE_UNKNOWN;
}

static int is_numeric_scalar(TypeKind kind) {
    return kind == TYPE_INT || kind == TYPE_INT64 || kind == TYPE_UINT64 ||
           kind == TYPE_UINT32 || kind == TYPE_UINT16 || kind == TYPE_UINT8 ||
           kind == TYPE_FLOAT || kind == TYPE_FLOAT32 || kind == TYPE_LONGDOUBLE;
}

static TypeKind wider_integer_kind(TypeKind a, TypeKind b) {
    if (a == TYPE_UINT64 || b == TYPE_UINT64) return TYPE_UINT64;
    if (a == TYPE_INT64 || b == TYPE_INT64) return TYPE_INT64;
    /* C's usual arithmetic conversions: `unsigned int` wins over `int`,
     * so `uint32 + int` IS a uint32 in the C this lowers to. Calling it
     * int typed `y = u + 1` as `int y` and printed 4000000001 as
     * -294967295. uint16/uint8 promote to int, so those stay int. */
    if (a == TYPE_UINT32 || b == TYPE_UINT32) return TYPE_UINT32;
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

/* The type of the function's last declared parameter, or NULL. */
static Type* last_param_type(ASTNode* func) {
    if (!func) return NULL;
    ASTNode* last_param = NULL;
    for (int i = 0; i < func->child_count - 1; i++) {
        ASTNode* child = func->children[i];
        if (child && (child->type == AST_VARIABLE_DECLARATION ||
                      child->type == AST_PATTERN_VARIABLE ||
                      child->type == AST_PATTERN_LITERAL)) {
            last_param = child;
        }
    }
    return last_param ? last_param->node_type : NULL;
}

/* docs/quote.md: a quote goes only to a function whose last parameter is a
 * quote.Tree, and such a function takes a quote, not a closure, a callback
 * or a block that runs inline. Reports and returns 0 on a mismatch. */
static int check_quote_callee(ASTNode* call, Symbol* symbol) {
    if (!call || call->child_count == 0) return 1;
    ASTNode* last = call->children[call->child_count - 1];
    if (!last) return 1;
    ASTNode* fn = symbol ? symbol->node : NULL;
    int is_fn = fn && (fn->type == AST_FUNCTION_DEFINITION || fn->type == AST_BUILDER_FUNCTION);
    int takes_tree = is_fn && quote_is_tree_type(last_param_type(fn));
    const char* name = call->value ? call->value : "function";
    if (last->type == AST_QUOTE && !takes_tree) {
        char msg[320];
        snprintf(msg, sizeof(msg),
                 "'%s' is given a quote, but its last parameter is not a quote.Tree", name);
        type_error_code_hint(msg,
            "a quote hands its block over as a tree; the function receiving it "
            "declares its last parameter `t: quote.Tree` (import std.quote). To run "
            "the block, drop `quote`",
            last->line, last->column, AETHER_ERR_QUOTE_CALLEE);
        return 0;
    }
    if (takes_tree && last->type == AST_CLOSURE) {
        int trailing = last->value && strcmp(last->value, "trailing") == 0;
        char msg[320];
        snprintf(msg, sizeof(msg),
                 "'%s' takes a quote.Tree as its last parameter, but is given %s",
                 name, trailing ? "a block that runs inline" : "a closure or callback");
        type_error_code_hint(msg,
            "write `quote |params| { ... }` (or `quote { ... }`) so the block is "
            "handed over as a tree",
            last->line, last->column, AETHER_ERR_QUOTE_CALLEE);
        return 0;
    }
    return 1;
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

/* Does the module that DEFINES `callee_def` also declare at least one
 * `builder` function? "Same module" is matched by `source_file`: every node
 * keeps the .ae path it was parsed from.
 *
 * This scans the MODULE REGISTRY's per-module ASTs rather than the merged
 * program AST. The registry holds each module's full parsed AST
 * (AetherModule.ast, file_path == the nodes' source_file), so the builder
 * is visible there whatever the merge and the prune do with it; this was
 * written when the prune ran before type checking and had already removed
 * a builder the entry file never calls, exactly the misuse case, where
 * `mod.rspec() {...}` is written INSTEAD of `mod.bundle() {...}`. (Since
 * #2613 the prune drops unreached functions only after type checking.)
 *
 * This is the discriminator for the "setter called as node builder"
 * diagnostic below: a widget-style DSL module (panel/button, no builders)
 * must never be flagged, whereas a builder-DSL module (which has at least
 * one `builder`) is the only place a `_ctx`-first plain function is a
 * block SETTER meant to run inside a builder's trailing block. */
static int builder_in_module_ast(ASTNode* mod_ast, const char* src) {
    if (!mod_ast) return 0;
    for (int i = 0; i < mod_ast->child_count; i++) {
        ASTNode* c = mod_ast->children[i];
        if (c && c->type == AST_BUILDER_FUNCTION && c->source_file && src &&
            strcmp(c->source_file, src) == 0) {
            return 1;
        }
    }
    return 0;
}

static int module_defines_a_builder(ASTNode* callee_def) {
    if (!callee_def || !callee_def->source_file) return 0;
    const char* src = callee_def->source_file;
    if (global_module_registry) {
        for (int m = 0; m < global_module_registry->module_count; m++) {
            AetherModule* mod = global_module_registry->modules[m];
            if (mod && builder_in_module_ast(mod->ast, src)) return 1;
        }
    }
    /* Fallback: the entry file itself is not a registry module, so also
     * consult the merged program AST for a same-file builder (covers a
     * setter and builder both defined in the top-level program). */
    ASTNode* program = aether_typecheck_program_node();
    if (program && builder_in_module_ast(program, src)) return 1;
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

/* #2590: a struct literal whose every field is a compile-time constant or
 * such a literal itself. A module-level `var` takes one: it lowers to an
 * initializer list (`static T g = { .a = 0 };`), which C accepts for a
 * file-scope static, where the compound literal a struct literal is
 * elsewhere (`(T){ ... }`) is not a constant. */
static int is_const_struct_literal(ASTNode* e, SymbolTable* table) {
    if (!e || e->type != AST_STRUCT_LITERAL) return 0;
    for (int i = 0; i < e->child_count; i++) {
        ASTNode* f = e->children[i];
        if (!f || f->type != AST_ASSIGNMENT || f->child_count < 1) return 0;
        ASTNode* v = f->children[0];
        /* #2792: `[]` (or `null`, a constant anyway) for a `T[]` field,
         * which the field's coercion has made the empty slice: a constant,
         * as for a `T[]` global (#2785). */
        if (v && v->type == AST_SLICE_FROM_ARRAY && v->child_count > 0 && v->children[0] &&
            (v->children[0]->type == AST_NULL_LITERAL ||
             (v->children[0]->type == AST_ARRAY_LITERAL && v->children[0]->child_count == 0)))
            continue;
        if (!is_const_expression(v, table) && !is_const_struct_literal(v, table)) return 0;
    }
    return 1;
}

/* Defined below, beside typecheck_function_call — used by the
 * variable-declaration arm, which appears earlier in this file. */
static int call_yields_no_value(ASTNode* call, SymbolTable* table);
static int fn_yields_no_value(ASTNode* fn);
/* #2697, #2733, #2736: which `return`s have a value; defined beside
 * fn_yields_no_value. */
static void tc_yields_settle(ASTNode* program, SymbolTable* global);
static int tc_return_settle(ASTNode* ret, SymbolTable* table);
/* #2743: every arm of a `match` whose value is used gives one. */
static int check_match_value_arms(ASTNode* m, SymbolTable* table);

// Type compatibility functions
/* A `fn` with no signature: nothing known about parameters or result. */
static int fn_type_is_erased(const Type* t) {
    return t->param_count == 0 && t->return_type == NULL;
}

/* #2054: the type of `call(f, ...)`. The result slot of f's signature when f
 * carries one. A closure reached through an erased `fn` has none, and the
 * language's answer for that has always been int: the call is typed int and
 * marked `erased_call`, so the two contexts that can do better -- a typed
 * binding, a return from a function with a declared result -- recognise it
 * and retype it, and everything else (an argument, an operand) sees the int
 * it always saw. A fixed int with no mark was the bug: nothing could tell a
 * known int result from a guess. */
static int function_value_annotate(ASTNode* ident, SymbolTable* table);

static int is_erased_call(const ASTNode* n);

/* The first return statement under `node` that carries a value, not looking
 * into nested closures; codegen's find_first_return_expr, which picks the
 * closure's C return type, walks the same way. */
static ASTNode* closure_first_return_expr(ASTNode* node) {
    if (!node || ast_is_own_function(node)) return NULL;
    if (node->type == AST_RETURN_STATEMENT && node->child_count > 0 &&
        node->children[0] && node->children[0]->type != AST_PRINT_STATEMENT) {
        return node->children[0];
    }
    for (int i = 0; i < node->child_count; i++) {
        ASTNode* found = closure_first_return_expr(node->children[i]);
        if (found) return found;
    }
    return NULL;
}

/* The return statement closure_first_return_expr finds the value of. */
static ASTNode* closure_first_return_stmt(ASTNode* node) {
    if (!node || ast_is_own_function(node)) return NULL;
    if (node->type == AST_RETURN_STATEMENT && node->child_count > 0 &&
        node->children[0] && node->children[0]->type != AST_PRINT_STATEMENT) {
        return node;
    }
    for (int i = 0; i < node->child_count; i++) {
        ASTNode* found = closure_first_return_stmt(node->children[i]);
        if (found) return found;
    }
    return NULL;
}

static int closure_any_return_is_string(ASTNode* node) {
    if (!node || ast_is_own_function(node)) return 0;
    if (node->type == AST_RETURN_STATEMENT && node->child_count > 0 &&
        node->children[0] && node->children[0]->type != AST_PRINT_STATEMENT) {
        return node->children[0]->node_type &&
               node->children[0]->node_type->kind == TYPE_STRING;
    }
    for (int i = 0; i < node->child_count; i++) {
        if (closure_any_return_is_string(node->children[i])) return 1;
    }
    return 0;
}

/* #2460: what calling the closure literal `lit` yields, read from its
 * checked body by the rule codegen's resolve_closure_return_type uses for
 * the C signature (the first value return, a string if any return is one),
 * so the call and the function it calls agree. NULL when the body says
 * nothing usable: no value return, an unknown type, or a return that is
 * itself an erased call. */
static Type* closure_literal_result_type(ASTNode* lit) {
    if (!lit || lit->type != AST_CLOSURE) return NULL;
    ASTNode* body = NULL;
    for (int i = lit->child_count - 1; i >= 0; i--) {
        if (lit->children[i] && lit->children[i]->type == AST_BLOCK) {
            body = lit->children[i];
            break;
        }
    }
    /* #2501: a multi-value return gives the closure a tuple result, as it
     * gives a function one, so `s, k = call(f, 4)` destructures it. */
    ASTNode* first_ret = closure_first_return_stmt(body);
    if (first_ret && first_ret->child_count > 1) {
        Type* t = create_type(TYPE_TUPLE);
        t->tuple_count = first_ret->child_count;
        t->tuple_types = malloc((size_t)first_ret->child_count * sizeof(Type*));
        for (int j = 0; j < first_ret->child_count; j++) {
            Type* et = first_ret->children[j] ? first_ret->children[j]->node_type : NULL;
            t->tuple_types[j] = (et && et->kind != TYPE_UNKNOWN) ? clone_type(et)
                                                                 : create_type(TYPE_INT);
        }
        return t;
    }
    ASTNode* ret = closure_first_return_expr(body);
    if (!ret || !ret->node_type || is_erased_call(ret)) return NULL;
    if (closure_any_return_is_string(body)) return create_type(TYPE_STRING);
    TypeKind k = ret->node_type->kind;
    if (k == TYPE_UNKNOWN || k == TYPE_VOID) return NULL;
    return clone_type(ret->node_type);
}

/* The closure literal an expression is known to evaluate to, or NULL. */
static ASTNode* closure_literal_of(ASTNode* e, SymbolTable* table) {
    if (!e) return NULL;
    if (e->type == AST_CLOSURE) return e;
    Type* t = infer_type(e, table);
    ASTNode* lit = (t && t->kind == TYPE_FUNCTION) ? t->closure_literal : NULL;
    if (t) free_type(t);
    return lit;
}

/* Do two closure literals yield the same result type? */
static int closure_results_agree(ASTNode* a, ASTNode* b) {
    if (a == b) return 1;
    Type* ta = closure_literal_result_type(a);
    Type* tb = closure_literal_result_type(b);
    int same = ta && tb && types_equal(ta, tb);
    if (ta) free_type(ta);
    if (tb) free_type(tb);
    return same;
}

/* #2460: `sym`, which may hold a known closure literal, is assigned `lit`
 * (NULL: a closure value of unknown origin). The variable keeps telling
 * calls a result type only while every closure it is given yields that
 * type; anything else makes it what it was before, an erased `fn`. Returns
 * the literal the new binding may carry. */
static ASTNode* closure_rebind(Symbol* sym, ASTNode* lit) {
    if (!sym || !sym->type || sym->type->kind != TYPE_FUNCTION) return lit;
    ASTNode* had = sym->type->closure_literal;
    if (!had) return lit;
    if (lit && closure_results_agree(had, lit)) return lit;
    sym->type->closure_literal = NULL;
    return NULL;
}

static Type* call_builtin_result_type(ASTNode* call, SymbolTable* table) {
    if (call->child_count < 1 || !call->children[0]) return create_type(TYPE_UNKNOWN);
    function_value_annotate(call->children[0], table);   /* call(add_fn, ...) */
    Type* callee = infer_type(call->children[0], table);
    Type* out;
    /* #2460: an erased `fn` known to hold a closure literal (or the literal
     * itself) yields what the literal's body returns. */
    ASTNode* lit = call->children[0]->type == AST_CLOSURE ? call->children[0]
                 : (callee && callee->kind == TYPE_FUNCTION) ? callee->closure_literal
                 : NULL;
    Type* literal_result = NULL;
    if (callee && callee->kind == TYPE_FUNCTION && callee->return_type) {
        out = clone_type(callee->return_type);
    } else if ((literal_result = closure_literal_result_type(lit))) {
        out = literal_result;
    } else {
        out = create_type(TYPE_INT);
        if (!call->annotation) call->annotation = strdup("erased_call");
    }
    if (callee) free_type(callee);
    return out;
}

/* An erased call whose int is the default, not a known result. */
static int is_erased_call(const ASTNode* n) {
    return n && n->type == AST_FUNCTION_CALL && n->value &&
           strcmp(n->value, "call") == 0 &&
           n->annotation && strcmp(n->annotation, "erased_call") == 0;
}

/* #2484: the closure literal `call` invokes, when known. With no table the
 * callee's own stamped type is all there is. */
static ASTNode* call_callee_literal(ASTNode* call, SymbolTable* table) {
    if (!call || call->child_count < 1 || !call->children[0]) return NULL;
    ASTNode* callee = call->children[0];
    if (callee->type == AST_CLOSURE) return callee;
    if (table) return closure_literal_of(callee, table);
    return (callee->node_type && callee->node_type->kind == TYPE_FUNCTION)
               ? callee->node_type->closure_literal : NULL;
}

/* #2054 / #2484: an erased call takes the type `t` its context supplies (a
 * typed binding, a declared return). From here on its type is known, so it
 * stops counting as erased: a closure literal that returns it now yields
 * `t` (closure_literal_result_type), and codegen gives that closure a `t`
 * C return. When the callee is itself a closure literal whose result is
 * unknown because it returns erased calls, those returns take `t` too: a
 * closure that only passes another call's result through is typed by where
 * its own result is used. Without this the closure stayed `int` and a
 * pointer came back through it cut to 32 bits. */
static void type_erased_call(ASTNode* call, Type* t, SymbolTable* table, int depth) {
    if (!call || !t || depth > 8) return;
    set_node_type(call, clone_type(t));
    if (call->annotation) free(call->annotation);
    call->annotation = strdup("erased_call_typed");
    ASTNode* lit = call_callee_literal(call, table);
    if (!lit || lit->type != AST_CLOSURE) return;
    ASTNode* body = NULL;
    for (int i = lit->child_count - 1; i >= 0; i--) {
        if (lit->children[i] && lit->children[i]->type == AST_BLOCK) {
            body = lit->children[i];
            break;
        }
    }
    ASTNode* first = closure_first_return_expr(body);
    ASTNode* first_stmt = closure_first_return_stmt(body);
    if (!is_erased_call(first) || first_stmt->child_count != 1) return;   /* its result is its own */
    type_erased_call(first, t, NULL, depth + 1);
}

/* #2484: closure literals whose result comes from an erased call, checked
 * once the top-level item holding them is done: by then every typed use
 * that can give them a type (type_erased_call) has. One still unknown is
 * typed int, which truncates a pointer or a string, and is said so, as an
 * untyped binding of an erased call is. */
static ASTNode** g_tc_erased_closures = NULL;
static int g_tc_erased_closure_count = 0;
static int g_tc_erased_closure_cap = 0;

static void note_erased_result_closure(ASTNode* lit) {
    for (int i = 0; i < g_tc_erased_closure_count; i++) {
        if (g_tc_erased_closures[i] == lit) return;   /* checked twice */
    }
    if (g_tc_erased_closure_count >= g_tc_erased_closure_cap) {
        g_tc_erased_closure_cap = g_tc_erased_closure_cap ? g_tc_erased_closure_cap * 2 : 8;
        g_tc_erased_closures = aether_xrealloc(g_tc_erased_closures,
            (size_t)g_tc_erased_closure_cap * sizeof(ASTNode*));
    }
    g_tc_erased_closures[g_tc_erased_closure_count++] = lit;
}

static void warn_erased_result_closures(void) {
    for (int i = 0; i < g_tc_erased_closure_count; i++) {
        ASTNode* lit = g_tc_erased_closures[i];
        ASTNode* body = NULL;
        for (int j = lit->child_count - 1; j >= 0; j--) {
            if (lit->children[j] && lit->children[j]->type == AST_BLOCK) {
                body = lit->children[j];
                break;
            }
        }
        ASTNode* first = closure_first_return_expr(body);
        if (!is_erased_call(first) || closure_first_return_stmt(body)->child_count != 1) continue;
        type_warning("this closure returns what a closure called through an erased `fn` "
                     "returns, which has no known type, so it is typed to return int; "
                     "bind the result with its type and return that (e.g. `let r: ptr "
                     "= call(...)` then `return r`)",
                     first->line, first->column);
    }
    g_tc_erased_closure_count = 0;
}

/* #2055: a top-level function named in value position -- `op = add_fn`,
 * a branch of `if c { add_fn } else { mul_fn }` -- is a closure value. Its
 * symbol carries only the return type (that is what a call to it yields),
 * so the closure type is assembled from the definition: parameters in
 * order, the return slot, closure-shaped. The identifier is annotated so
 * codegen lowers it through the bare-fn adapter instead of naming the C
 * function, which is a raw pointer and not an _AeClosure. Returns 0 when
 * the name is not a function or a local shadows it. */
static int function_value_annotate(ASTNode* ident, SymbolTable* table) {
    if (!ident || ident->type != AST_IDENTIFIER || !ident->value) return 0;
    Symbol* sym = lookup_symbol(table, ident->value);
    if (!sym || !sym->is_function || !sym->node ||
        (sym->node->type != AST_FUNCTION_DEFINITION &&
         sym->node->type != AST_BUILDER_FUNCTION)) return 0;
    ASTNode* def = sym->node;
    int count = 0;
    for (int i = 0; i < def->child_count; i++) {
        ASTNode* p = def->children[i];
        if (p && p->type != AST_GUARD_CLAUSE && p->type != AST_BLOCK &&
            p->type != AST_REQUIRES_CLAUSE && p->type != AST_ENSURES_CLAUSE) count++;
    }
    Type** params = count ? malloc(sizeof(Type*) * (size_t)count) : NULL;
    int k = 0;
    for (int i = 0; i < def->child_count && k < count; i++) {
        ASTNode* p = def->children[i];
        if (!p || p->type == AST_GUARD_CLAUSE || p->type == AST_BLOCK ||
            p->type == AST_REQUIRES_CLAUSE || p->type == AST_ENSURES_CLAUSE) continue;
        params[k++] = p->node_type ? clone_type(p->node_type) : create_type(TYPE_INT);
    }
    Type* ret = (sym->type && sym->type->kind != TYPE_VOID) ? clone_type(sym->type) : NULL;
    set_node_type(ident, create_function_type(count, params, ret));
    if (ident->annotation) free(ident->annotation);
    ident->annotation = strdup("fn_value");
    return 1;
}

/* Spell a tuple type the way a signature does: `(int, string)`. */
static void tuple_type_spell(Type* t, char* out, size_t size) {
    size_t pos = 0;
    if (size == 0) return;
    out[pos++] = '(';
    for (int i = 0; i < t->tuple_count && pos + 1 < size; i++) {
        const char* name = t->tuple_types[i] ? type_name(t->tuple_types[i]) : "?";
        int w = snprintf(out + pos, size - pos, "%s%s", i ? ", " : "", name);
        if (w < 0) break;
        pos += (size_t)w;
        if (pos >= size) { pos = size - 1; break; }
    }
    if (pos + 1 < size) out[pos++] = ')';
    out[pos] = '\0';
}

/* A multi-value return bound to ONE name is a tuple, and a tuple passed
 * where the parameter takes a scalar is never what was meant. The front end
 * let it through (argument checking is lenient for user functions) and the
 * only report was the C compiler's `incompatible type for argument 1 …
 * argument is of type '_tuple_ptr_string'`, against generated code (the
 * same shape #1878 took out of binary operators). Name the fix: the value
 * was to be destructured. */
static void reject_tuple_argument(SymbolTable* table, ASTNode* call, ASTNode* arg,
                                  Type* arg_type, Type* param_type, int index,
                                  const char* param_name) {
    if (!arg_type || arg_type->kind != TYPE_TUPLE || !param_type ||
        param_type->kind == TYPE_TUPLE || param_type->kind == TYPE_UNKNOWN) {
        return;
    }
    /* Only a NAME bound to a tuple. A function passed as a value
     * (`via_fn(pair, 2, 3)`) infers to its return type, and a call in
     * argument position is typed by the callee's own rules. */
    if (arg->type != AST_IDENTIFIER || !arg->value) return;
    Symbol* sym = lookup_symbol(table, arg->value);
    if (!sym || sym->is_function) return;
    char spelled[256];
    tuple_type_spell(arg_type, spelled, sizeof(spelled));
    char emsg[512];
    snprintf(emsg, sizeof(emsg),
             "Argument %d '%s' of '%s': expected %s, got the %s tuple bound to "
             "'%s' — a multi-value return bound to one name stays a tuple; "
             "destructure it: `value, err = ...`",
             index, param_name ? param_name : "?", call->value ? call->value : "?",
             type_name(param_type), spelled, arg->value);
    type_error(emsg, arg->line, arg->column);
}

/* Is `value`, of type `value_type`, a closure headed for a typed C function
 * pointer slot (`fn(int) -> int`, is_fnptr)? A closure literal, or a name
 * bound to one. A named function is not: it lowers to its own address. */
static int closure_for_fnptr(SymbolTable* table, ASTNode* value, Type* value_type,
                             Type* slot_type) {
    if (!value || !slot_type || slot_type->kind != TYPE_FUNCTION || !slot_type->is_fnptr)
        return 0;
    if (value->type == AST_CLOSURE) return 1;
    if (value->type != AST_IDENTIFIER || !value->value ||
        !value_type || value_type->kind != TYPE_FUNCTION || value_type->is_fnptr) return 0;
    Symbol* sym = lookup_symbol(table, value->value);
    return sym && !sym->is_function;
}

/* A closure passed where the parameter is a typed C function pointer
 * (`fn(int) -> int`, is_fnptr). The parameter is a bare pointer with no
 * environment, so the closure cannot fit it; the front end let it through
 * and the C compiler reported `expected 'int (*)(int)' but argument is of
 * type '_AeClosure'` against generated code. A closure literal, or a name
 * bound to one, is refused here with the two spellings that work. A named
 * function passes: it lowers to its own address. */
static void reject_closure_for_fnptr(SymbolTable* table, ASTNode* call, ASTNode* arg,
                                     Type* arg_type, Type* param_type, int index,
                                     const char* param_name) {
    if (!closure_for_fnptr(table, arg, arg_type, param_type)) return;
    char emsg[512];
    snprintf(emsg, sizeof(emsg),
             "Argument %d '%s' of '%s': a closure cannot be passed as a typed function "
             "pointer — the parameter is a C function pointer with no environment. "
             "Take a bare `fn` parameter and call it with `call(%s, ...)`, or pass a "
             "named function (`f as fn(...)`).",
             index, param_name ? param_name : "?", call->value ? call->value : "?",
             param_name ? param_name : "f");
    type_error(emsg, arg->line, arg->column);
}

/* #2628: the same closure stored in a typed C function pointer slot other
 * than a parameter: a struct literal's field, a field or element store, a
 * binding of a local or module-level `var`, a function's result. The slot's
 * own type check let it through (types_equal does not look at is_fnptr, so
 * a closure and a function pointer of one signature compare equal), and gcc
 * refused the `_AeClosure` it was handed. `name` names the slot: the field
 * as `Struct.field`, the variable, the array. Returns 1 when it reported. */
typedef enum { FNPTR_SLOT_FIELD, FNPTR_SLOT_VARIABLE, FNPTR_SLOT_ELEMENT,
               FNPTR_SLOT_RESULT } FnptrSlotKind;
static int reject_closure_in_fnptr_slot(SymbolTable* table, ASTNode* value, Type* value_type,
                                        Type* slot_type, FnptrSlotKind kind, const char* name,
                                        int line, int column) {
    if (!closure_for_fnptr(table, value, value_type, slot_type)) return 0;
    const char* nm = name ? name : "?";
    char emsg[512] = "";
    switch (kind) {
        case FNPTR_SLOT_FIELD:
            snprintf(emsg, sizeof(emsg),
                     "Field '%s': a closure cannot be stored in a typed function pointer; "
                     "the field is a C function pointer with no environment. Declare it "
                     "as a bare `fn` field and call it with `call(...)`, or store a named "
                     "function.", nm);
            break;
        case FNPTR_SLOT_VARIABLE:
            snprintf(emsg, sizeof(emsg),
                     "Variable '%s': a closure cannot be bound to a typed function pointer; "
                     "'%s' is a C function pointer with no environment. Declare it as a "
                     "bare `fn` and call it with `call(%s, ...)`, or bind a named function.",
                     nm, nm, nm);
            break;
        case FNPTR_SLOT_ELEMENT:
            snprintf(emsg, sizeof(emsg),
                     "Element of '%s': a closure cannot be stored in a typed function "
                     "pointer; the element is a C function pointer with no environment. "
                     "Keep closures in a bare `fn` slot and call them with `call(...)`, or "
                     "store a named function.", nm);
            break;
        case FNPTR_SLOT_RESULT:
            snprintf(emsg, sizeof(emsg),
                     "Return value: a closure cannot be returned as a typed function "
                     "pointer; the function's result is a C function pointer with no "
                     "environment. Return a bare `fn` and call it with `call(...)`, or "
                     "return a named function.");
            break;
    }
    type_error(emsg, line, column);
    return 1;
}

/* #2491: a struct value passed where the parameter takes a different struct
 * (`length2(a: Wide)` given a `Narrow`). Argument checking for user
 * functions is lenient, so the front end let it through and gcc reported
 * "incompatible type for argument" against generated code, one error per
 * compile, while the same values in an assignment were already a type
 * error. Only by-value struct and sum types on both sides are compared: a
 * `*T` or `ptr` parameter is a pointer, and a variant struct into its sum
 * is the wrap is_type_compatible allows. */
static void reject_struct_argument(ASTNode* call, ASTNode* arg, Type* arg_type,
                                   Type* param_type, int index, const char* param_name) {
    if (!arg_type || !param_type) return;
    int arg_nominal = arg_type->kind == TYPE_STRUCT || arg_type->kind == TYPE_SUM;
    int param_nominal = param_type->kind == TYPE_STRUCT || param_type->kind == TYPE_SUM;
    if (!arg_nominal || !param_nominal || !arg_type->struct_name ||
        !param_type->struct_name || is_type_compatible(arg_type, param_type))
        return;
    char emsg[512];
    snprintf(emsg, sizeof(emsg),
             "Argument %d '%s' of '%s': expected %s, got %s",
             index, param_name ? param_name : "?", call->value ? call->value : "?",
             param_type->struct_name, arg_type->struct_name);
    type_error(emsg, arg->line, arg->column);
}

/* #2624: a typed pointer passed where the parameter takes a pointer to
 * another type: `buffer_size(b: *Buffer)` given `&p.ints`, an `*Ints`. The
 * struct rule above compares values only, so the front end let it through
 * and gcc reported "incompatible pointer type" against generated code; with
 * two structs sharing a layout prefix nothing at all would have failed, and
 * the callee would have read the wrong struct. Compared as an assignment
 * compares them (#1877): a bare `ptr` on either side is the universal
 * pointer, and so is a pointer to a @c_struct overlay, a `void*` in C. */
static void reject_pointer_argument(ASTNode* call, ASTNode* arg, Type* arg_type,
                                    Type* param_type, int index, const char* param_name) {
    if (!arg_type || !param_type || arg_type->kind != TYPE_PTR ||
        param_type->kind != TYPE_PTR) return;
    Type* want = param_type->element_type;
    Type* got = arg_type->element_type;
    if (!want || !got || want->kind == TYPE_UNKNOWN || got->kind == TYPE_UNKNOWN) return;
    if ((want->kind == TYPE_STRUCT && is_c_struct_name(want->struct_name)) ||
        (got->kind == TYPE_STRUCT && is_c_struct_name(got->struct_name))) return;
    if (is_type_compatible(arg_type, param_type)) return;
    char emsg[512];
    snprintf(emsg, sizeof(emsg),
             "Argument %d '%s' of '%s': expected %s, got %s",
             index, param_name ? param_name : "?", call->value ? call->value : "?",
             type_name(param_type), type_name(arg_type));
    type_error(emsg, arg->line, arg->column);
}

/* #2516: a fixed-size array parameter (`xs: int[3]`) takes an array of
 * that element type and length: the callee copies that many elements and
 * its `xs.len` is the declared length. A longer or shorter array, a slice
 * or a pointer has no such guarantee; a slice parameter (`int[]`) takes
 * those. */
static void reject_sized_array_argument(ASTNode* call, ASTNode* arg, Type* arg_type,
                                        Type* param_type, int index, const char* param_name) {
    if (!param_type || param_type->kind != TYPE_ARRAY || param_type->array_size <= 0 ||
        param_type->index_enum_name || !arg_type || arg_type->kind == TYPE_UNKNOWN) return;
    if (arg_type->kind == TYPE_ARRAY && arg_type->array_size == param_type->array_size &&
        is_type_compatible(arg_type->element_type, param_type->element_type) &&
        is_type_compatible(param_type->element_type, arg_type->element_type)) return;
    char want[96], got[96], emsg[512];
    snprintf(emsg, sizeof(emsg),
             "Argument %d '%s' of '%s': expected %s, got %s; a fixed-size array "
             "parameter takes an array of exactly that length (take a slice, `%s[]`, "
             "for any length)",
             index, param_name ? param_name : "?", call->value ? call->value : "?",
             array_type_spell(param_type, want, sizeof(want)),
             array_type_spell(arg_type, got, sizeof(got)),
             type_name(param_type->element_type));
    type_error(emsg, arg->line, arg->column);
}

/* #2688: a fixed-size array field bound from a whole array (`Shelf { names:
 * arr }`, `s.names = arr`) copies the source's elements into the field's,
 * as many as the field holds, so the source must hold exactly that many: a
 * shorter one was read past its end. An array literal may be shorter (the
 * rest is zeroed, as for a local) but not longer. A slice or a pointer has
 * no length known here, and is refused rather than checked when it runs:
 * copy its elements one by one. Reports and returns 1 on a mismatch.
 * `slot` names the field. */
static int reject_array_field_source(ASTNode* value, Type* value_type, Type* field_type,
                                     const char* slot, int line, int column) {
    if (!value || !field_type || field_type->kind != TYPE_ARRAY ||
        field_type->array_size <= 0 || field_type->index_enum_name) return 0;
    char want[96], got[96], msg[640];
    int n = field_type->array_size;
    if (value->type == AST_ARRAY_LITERAL) {
        if (value->child_count <= n) return 0;
        snprintf(msg, sizeof(msg),
                 "%s is %s and cannot take an array literal of %d elements: a fixed-size "
                 "array field holds exactly its declared number of elements",
                 slot, array_type_spell(field_type, want, sizeof(want)), value->child_count);
        type_error(msg, line, column);
        return 1;
    }
    if (!value_type || value_type->kind == TYPE_UNKNOWN) return 0;
    if (value_type->kind == TYPE_ARRAY && value_type->array_size > 0) {
        if (value_type->array_size == n) return 0;
        snprintf(msg, sizeof(msg),
                 "%s is %s and cannot take %s: binding an array to a fixed-size array field "
                 "copies its elements, so the two lengths must be the same",
                 slot, array_type_spell(field_type, want, sizeof(want)),
                 array_type_spell(value_type, got, sizeof(got)));
        type_error(msg, line, column);
        return 1;
    }
    if (value_type->kind != TYPE_ARRAY && value_type->kind != TYPE_PTR) return 0;
    snprintf(msg, sizeof(msg),
             "%s is %s and cannot take %s: its length is not known until the program "
             "runs, and a fixed-size array field takes an array of exactly its length",
             slot, array_type_spell(field_type, want, sizeof(want)),
             value_type->kind == TYPE_PTR ? "a pointer"
                                          : array_type_spell(value_type, got, sizeof(got)));
    type_error_hint(msg, "copy the elements one by one (`s.f[i] = xs[i]`), or bind an array of "
                    "that length", line, column);
    return 1;
}

/* #2468: `call(x, ...)` invokes a closure, and codegen lowers it through
 * `x.fn` / `x.env`. Any callee whose type is known and is not a closure was
 * let through, and the only report was the C compiler's `'_tuple_ptr_string'
 * has no member named 'fn'` against generated code; the common way in is a
 * `(value, err)` return such as `list.get` bound to one name. An unknown type
 * stays lenient: inference gaps must not reject a real closure. */
static void reject_non_closure_callee(ASTNode* call, SymbolTable* table) {
    if (call->child_count < 1 || !call->children[0]) return;
    ASTNode* callee = call->children[0];
    Type* t = infer_type(callee, table);
    if (!t) return;
    if (t->kind == TYPE_FUNCTION || t->kind == TYPE_UNKNOWN) {
        free_type(t);
        return;
    }
    char what[256];
    if (t->kind == TYPE_TUPLE) tuple_type_spell(t, what, sizeof(what));
    else snprintf(what, sizeof(what), "%s", type_name(t));
    char name[200];
    if (callee->type == AST_IDENTIFIER && callee->value)
        snprintf(name, sizeof(name), "'%s'", callee->value);
    else
        snprintf(name, sizeof(name), "the first argument");
    char msg[640];
    snprintf(msg, sizeof(msg),
             "call() needs a closure, but %s has type %s", name, what);
    const char* hint =
        t->kind == TYPE_TUPLE
            ? "a multi-value return bound to one name stays a tuple; destructure "
              "it (`value, err = ...`) and call the closure value"
        : t->kind == TYPE_PTR
            ? "a closure stored as a ptr (box_closure, a list element) is "
              "called through `call(unbox_closure(p), ...)`"
            : "pass a closure, a `fn`-typed value, or a function name";
    int line = callee->line ? callee->line : call->line;
    int column = callee->line ? callee->column : call->column;
    type_error_hint(msg, hint, line, column);
    free_type(t);
}

/* #2840 review: `none` and `[]` are built as a value of the slot they are
 * given to, an optional or a slice of its element type. Through an erased
 * `fn` (a bare `fn` with no closure literal known) the closure's parameter
 * types are not known at the call, so there is no type to build them as;
 * guessing one (an int optional) handed a `string?` parameter garbage.
 * Each is refused at the argument, naming the fix. */
static void reject_untyped_erased_args(ASTNode* call, SymbolTable* table) {
    if (call->child_count < 2 || !call->children[0] ||
        call->children[0]->type == AST_CLOSURE)
        return;
    Type* t = infer_type(call->children[0], table);
    int erased = t && t->kind == TYPE_FUNCTION && !t->is_fnptr && fn_type_is_erased(t) &&
                 !t->closure_literal;
    if (t) free_type(t);
    if (!erased) return;
    const char* callee = call->children[0]->type == AST_IDENTIFIER && call->children[0]->value
                             ? call->children[0]->value : "the closure";
    for (int i = 1; i < call->child_count; i++) {
        ASTNode* a = call->children[i];
        if (!a || ast_is_trailing_block(a)) continue;
        int is_none = a->type == AST_NONE_LITERAL;
        int is_empty = a->type == AST_ARRAY_LITERAL && a->child_count == 0;
        if (!is_none && !is_empty) continue;
        char msg[400], help[300];
        snprintf(msg, sizeof(msg),
                 "`%s` given to '%s', a bare `fn`: its parameter types are not known here, so "
                 "there is no %s type to build it as",
                 is_none ? "none" : "[]", callee, is_none ? "optional" : "slice");
        snprintf(help, sizeof(help),
                 "bind it to a local of the parameter's type first (e.g. `let o: %s = %s`) and "
                 "pass that, or, for a named function, take a typed function pointer (`%s: "
                 "fn(%s) -> int`)",
                 is_none ? "string?" : "int[]", is_none ? "none" : "[]", callee,
                 is_none ? "string?" : "int[]");
        type_error_hint(msg, help, a->line, a->column);
    }
}

static int g_tc_ptr_to_closure = 0;
int typecheck_ptr_to_closure_seen(void) { return g_tc_ptr_to_closure; }

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
        /* `byte` is C's `uint8_t`, as `uint8` is: a byte-coded enum (a
         * wire format) converts as a uint8-coded one does. */
        return is_integer_scalar(other) || other == TYPE_BYTE;
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

    /* #2054: a bare `fn` is the erased closure type. It is what a closure
     * becomes at a `fn` parameter or return, at box_closure, in a list; the
     * signature is gone but the value is the same _AeClosure. So it flows
     * into a signed `fn(T...) -> R` slot (an annotation restoring what was
     * erased) and a signed closure flows into a bare `fn` slot (erasing
     * again). A raw C function pointer (is_fnptr) is a different
     * representation and stays apart. */
    if (from->kind == TYPE_FUNCTION && to->kind == TYPE_FUNCTION &&
        !from->is_fnptr && !to->is_fnptr &&
        (fn_type_is_erased(from) || fn_type_is_erased(to))) return 1;

    // #2134 `f32`: C `float`, the GPU's number. It converts with `float`
    // and the integer kinds exactly as C converts float and double — a
    // value cast (`x as f32`) is the explicit spelling, and assignment
    // performs the same conversion, so `Viewport { x: 1.0 }` and
    // `verts[i] = v` need no ceremony. Arithmetic on it is `float`.
    if (from->kind == TYPE_FLOAT32 || to->kind == TYPE_FLOAT32) {
        TypeKind other = (from->kind == TYPE_FLOAT32) ? to->kind : from->kind;
        if (other == TYPE_FLOAT32 || other == TYPE_FLOAT || other == TYPE_LONGDOUBLE ||
            is_integer_scalar(other) || other == TYPE_BYTE) return 1;
    }
    /* #2146: a lane type is nominal — f32x4 is not f64x2, and neither is a
     * scalar. A scalar becomes a lane through `lanes.splat`, a lane becomes
     * scalars through `lanes.lane` / `.x`, and both say so at the call. */
    if (is_lane_type(from->kind) || is_lane_type(to->kind)) {
        return from->kind == to->kind;
    }

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
        /* ... and widen into a float losslessly, as byte and int do. */
        if (from_small && (to->kind == TYPE_FLOAT || to->kind == TYPE_LONGDOUBLE)) return 1;
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

    // #1877: pointer compatibility, with a BARE `ptr` on either side acting as
    // the universal pointer — exactly as `void*` does in C, and exactly as the
    // bare-actor_ref rule immediately below already does for actor refs.
    //
    // `types_equal` recurses into element_type, so `*LeafCert` vs bare `ptr`
    // compared element `struct LeafCert` against NULL and failed. Nothing then
    // permitted the widening, even though `int`, arrays and function pointers
    // all convert to `ptr` freely a few lines up. So a bare-`ptr` slot handed a
    // typed pointer was rejected with E0200 "Type mismatch in variable
    // initialization" — while the reverse (`ptr` into `*T`) and every other
    // conversion to `ptr` were fine.
    //
    // That is what blocked the native WebDriver binding: std.cryptography's
    // tls13_cert has `responder = issuer` (bare ptr) followed by
    // `responder = delegated` (*LeafCert), a plain C-style "maybe a delegated
    // responder" pattern that had no legal spelling.
    //
    // Two typed pointers still compare structurally, so `*Foo` vs `*Bar` stays
    // an error: this widens only to and from the deliberately-untyped `ptr`.
    if (from->kind == TYPE_PTR && to->kind == TYPE_PTR) {
        if (!from->element_type || !to->element_type) return 1;
        return is_type_compatible(from->element_type, to->element_type);
    }

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

    // int ↔ ptr compatibility (passing 0 to a ptr param, an actor-state
    // `ref = 0` later given a pointer). A LOCAL first bound as a number
    // and re-bound to a pointer is refused at the re-bind: the C the
    // local lowers to has one type, and GCC 14 makes that assignment an
    // error (-Wint-conversion).
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
    if (from->kind == TYPE_PTR && to->kind == TYPE_FUNCTION) {
        /* #2499: the closure behind the ptr may come from anywhere. */
        if (!to->is_fnptr) g_tc_ptr_to_closure = 1;
        return 1;
    }

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

/* #2623: the typed C function pointer (`fn(ptr, int)`, is_fnptr) that the
 * binding `stmt` writes, or NULL when it writes anything else. That is the
 * declaration's own annotation; for a bare `name = value`, the type of the
 * local it re-binds, or of the module-level `var` it assigns when no local
 * of the name is in scope. The global is found in the program, not the file
 * scope's table, where the early inference pass parks locals under their
 * own types. A `const` annotated with a typed fn pointer is that slot too,
 * holding the function's address as a `var` does (#2648); it is never
 * re-bound, so only its annotation counts. */
static ASTNode* global_var_decl(ASTNode* child);

/* The module-level `var` a bare `name = value` binding assigns: the
 * program's global of that name, when no local of it is in scope (#701).
 * NULL for any other binding. */
static ASTNode* rebound_global_var(ASTNode* stmt, SymbolTable* table) {
    if (stmt->type != AST_VARIABLE_DECLARATION || !stmt->type_inferred || !stmt->value)
        return NULL;
    for (SymbolTable* s = table; s && s->parent; s = s->parent)
        if (lookup_symbol_local(s, stmt->value)) return NULL;
    ASTNode* program = aether_typecheck_program_node();
    for (int i = 0; program && i < program->child_count; i++) {
        ASTNode* g = global_var_decl(program->children[i]);
        if (g && strcmp(g->value, stmt->value) == 0) return g;
    }
    return NULL;
}

static Type* fnptr_binding_slot(ASTNode* stmt, SymbolTable* table) {
    Type* t = stmt->node_type;
    if (stmt->type == AST_CONST_DECLARATION &&
        !(stmt->annotation && strcmp(stmt->annotation, "global_var") == 0))
        return t && t->kind == TYPE_FUNCTION && t->is_fnptr ? t : NULL;
    if (t && t->kind == TYPE_FUNCTION && t->is_fnptr) return t;
    if (!stmt->type_inferred || !stmt->value) return NULL;
    for (SymbolTable* s = table; s && s->parent; s = s->parent) {
        Symbol* local = lookup_symbol_local(s, stmt->value);
        if (local) {
            t = local->type;
            return t && t->kind == TYPE_FUNCTION && t->is_fnptr ? t : NULL;
        }
    }
    ASTNode* g = rebound_global_var(stmt, table);
    if (g) {
        t = g->node_type;
        return t && t->kind == TYPE_FUNCTION && t->is_fnptr ? t : NULL;
    }
    return NULL;
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

        case AST_SCHEMA_OF:        // #2298: a synthesized T_schema()'s body.
            return create_type(TYPE_PTR);

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

                /* Warn on `malloc(<integer-literal>) as *T` — a hand-computed
                 * byte count sized to the struct's current layout. The
                 * compiler does not know a pure-Aether struct's `sizeof` (C
                 * does), so it cannot check the number itself; but the pattern
                 * is the footgun: any layout change (e.g. the inline heap-
                 * string trackers added in v0.643.0, #1879) silently makes the
                 * literal under-allocate, and the overflow only shows up as
                 * heap corruption at runtime. `malloc(sizeof(T))` is layout-
                 * exact and immune. Only for real Aether structs — a @c_struct
                 * overlay has a C-defined size a literal can legitimately match.
                 * Skips `heap.new(T)`, which is the sized-allocation blessed path. */
                ASTNode* op = expr->children[0];
                if (!expr->warned &&
                    op && op->type == AST_FUNCTION_CALL && op->value &&
                    strcmp(op->value, "malloc") == 0 &&
                    op->child_count == 1 && op->children[0] &&
                    op->children[0]->type == AST_LITERAL &&
                    op->children[0]->value &&
                    is_all_digits(op->children[0]->value)) {
                    expr->warned = 1;
                    char wmsg[256];
                    snprintf(wmsg, sizeof(wmsg),
                        "`malloc(%s) as *%s` sizes the allocation by a literal "
                        "byte count; use `malloc(sizeof(%s))` so a struct-layout "
                        "change cannot silently under-allocate",
                        op->children[0]->value, expr->value, expr->value);
                    type_warning(wmsg, expr->line, expr->column);
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

        case AST_SLICE_FROM_ARRAY:
        case AST_SLICE_TO_PTR:
            /* #1286 coercion nodes carry the type they coerce to. */
            return expr->node_type ? clone_type(expr->node_type)
                                   : create_type(TYPE_UNKNOWN);

        case AST_SLICE_EXPR: {
            /* #1286 `s[lo..hi]`: a non-owning sub-slice of a `T[]` slice or a
             * `T[N]` array. The result is a `T[]` over the same elements. */
            if (expr->child_count == 0 || !expr->children[0])
                return create_type(TYPE_UNKNOWN);
            Type* base = infer_type(expr->children[0], table);
            if (!base) return create_type(TYPE_UNKNOWN);
            if (base->kind == TYPE_UNKNOWN) { free_type(base); return create_type(TYPE_UNKNOWN); }
            if (base->kind != TYPE_ARRAY || !base->element_type) {
                char msg[200];
                snprintf(msg, sizeof(msg),
                         "`[..]` sub-slicing is defined for a `T[]` slice or a "
                         "`T[N]` array, not for %s", type_name(base));
                free_type(base);
                type_error(msg, expr->line, expr->column);
                return create_type(TYPE_UNKNOWN);
            }
            Type* out = create_array_type(clone_type(base->element_type), -1);
            free_type(base);
            return out;
        }

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
            /* A function NAME under the cast is the function's address.
             * Marked so codegen emits the definition's C spelling: a user
             * function whose name is a libc symbol (`read`, `time`, ...)
             * is defined as `ae_<name>`, and the bare name under the cast
             * used to reach libc's function instead (#2064). */
            int operand_is_fn_addr = 0;
            {
                ASTNode* operand_node = expr->children[0];
                if (operand_node->type == AST_IDENTIFIER && operand_node->value) {
                    Symbol* fsym = lookup_symbol(table, operand_node->value);
                    if (fsym && fsym->is_function && fsym->node &&
                        (fsym->node->type == AST_FUNCTION_DEFINITION ||
                         fsym->node->type == AST_BUILDER_FUNCTION)) {
                        if (operand_node->annotation) free(operand_node->annotation);
                        operand_node->annotation = strdup("fn_addr");
                        operand_is_fn_addr = 1;
                    }
                }
            }
            /* A function's address is always a valid operand. Inferring the
             * bare name yields the function's RETURN type, so the pointer
             * check below passed `-> int` and `-> *T` functions by accident
             * and rejected `-> float` / `-> string` ones as "must be a ptr
             * value" (#2200: a `scale_impl as Scale` fallback for a float-
             * returning GL entry point). */
            Type* operand = operand_is_fn_addr ? NULL : infer_type(expr->children[0], table);
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
            /* The parser stashed the TYPE_FUNCTION (with signature) on
             * the node; the caller owns what infer_type returns and frees
             * it, so hand back a full clone. A hand-rolled shallow copy
             * used to share each parameter's element_type with the node:
             * the caller's free_type then took the `Thing` out of
             * `fn(ptr, *Thing, float)` from under the cast, and the next
             * clone_type of the node walked freed memory (#2094). */
            if (expr->node_type) {
                Type* dup = clone_type(expr->node_type);
                dup->is_fnptr = 1;  /* cast → raw fn-pointer */
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
            /* #2496: the first arm that yields a value (a block arm yields
             * its final expression). */
            for (int i = 1; i < expr->child_count; i++) {
                ASTNode* arm = expr->children[i];
                ASTNode* v = (arm && arm->child_count >= 2) ? match_arm_value(arm->children[1]) : NULL;
                if (v) return infer_type(v, table);
            }
            return create_type(TYPE_UNKNOWN);

        case AST_ARRAY_LITERAL:
            // Return the inferred array type
            return expr->node_type ? clone_type(expr->node_type) : create_type(TYPE_UNKNOWN);
            
        case AST_IDENTIFIER: {
            if (expr->annotation && strcmp(expr->annotation, "fn_value") == 0 && expr->node_type) {
                return clone_type(expr->node_type);
            }
            Symbol* symbol = lookup_symbol(table, expr->value);
            return (symbol && symbol->type) ? clone_type(symbol->type) : create_type(TYPE_UNKNOWN);
        }
        
        case AST_BINARY_EXPRESSION:
            /* Two distinct types mixed: already reported, and typed there
             * so the error does not cascade (typecheck_binary_expression). */
            if (expr->node_type && expr->child_count >= 2 &&
                binary_mixes_distinct(expr->children[0]->node_type,
                                      expr->children[1]->node_type,
                                      get_token_type_from_string(expr->value)))
                return clone_type(expr->node_type);
            return infer_binary_type(expr->children[0], expr->children[1],
                                   get_token_type_from_string(expr->value));

        case AST_UNARY_EXPRESSION:
            return infer_unary_type(expr->children[0], 
                                  get_token_type_from_string(expr->value));
            
        case AST_FUNCTION_CALL: {
            /* #1286 `make([]T, n)` is a `T[]` slice of n elements; the parser
             * stamped the array type on the call. */
            if (expr->value && strcmp(expr->value, "make") == 0 &&
                expr->node_type && expr->node_type->kind == TYPE_ARRAY)
                return clone_type(expr->node_type);
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
            if (expr->value && strcmp(expr->value, "call") == 0) {
                return call_builtin_result_type(expr, table);
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
            //
            // A bound value of the same name shadows the namespace (#2172):
            // with `import std.url`, a local `url` struct's `url.host` is a
            // field read, not a reach into std.url -- the same guard the
            // const-lookup path in typecheck_expression applies.
            if (expr->child_count > 0 && expr->children[0] &&
                expr->children[0]->type == AST_IDENTIFIER && expr->children[0]->value &&
                !lookup_symbol(table, expr->children[0]->value) &&
                is_visible_namespace(expr->children[0]->value, table) && expr->value &&
                is_export_blocked(expr->children[0]->value, expr->value)) {
                char msg[256];
                snprintf(msg, sizeof(msg), "'%s' is not exported from module '%s'",
                         expr->value, expr->children[0]->value);
                type_error(msg, expr->line, expr->column);
                return create_type(TYPE_UNKNOWN);
            }
            /* #1286: `s.len` on a slice or array is its element count, an
             * int (-1 for a view over a bare `ptr`, whose extent the
             * compiler cannot know). */
            if (expr->child_count > 0 && expr->children[0] && expr->value &&
                strcmp(expr->value, "len") == 0) {
                Type* obj = infer_type(expr->children[0], table);
                int is_arr = obj && obj->kind == TYPE_ARRAY;
                if (obj) free_type(obj);
                if (is_arr) return create_type(TYPE_INT);
            }
            /* #2146: `v.x` / `.y` / `.z` / `.w` on a lane value is that
             * lane's scalar — an f32 from an f32x4, a float from an f64x2,
             * an int from a mask. `.z` on an f64x2 is a type error (it has
             * two lanes), not a silent zero. */
            if (expr->child_count > 0 && expr->children[0] && expr->value) {
                Type* obj = infer_type(expr->children[0], table);
                if (obj && is_lane_type(obj->kind)) {
                    int idx = lane_accessor_index(obj->kind, expr->value);
                    TypeKind elem = lane_scalar_kind(obj->kind);
                    TypeKind objk = obj->kind;
                    free_type(obj);
                    if (idx >= 0) return create_type(elem);
                    char msg[200];
                    snprintf(msg, sizeof(msg),
                             "'%s' is not a lane of %s: its lanes are %s",
                             expr->value, type_name_of_kind(objk),
                             objk == TYPE_F64X2 ? "`.x` and `.y`"
                                                : "`.x`, `.y`, `.z` and `.w`");
                    type_error(msg, expr->line, expr->column);
                    return create_type(TYPE_UNKNOWN);
                }
                if (obj) free_type(obj);
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
                /* #2631: the checked module file's own constant, which a
                 * build merges as `<ns>_<name>`: the bare one it carries. */
                if ((!sym || !sym->type) && !(table && table->merged_from) &&
                    entry_self_reference(expr->children[0]->value, expr->value)) {
                    sym = entry_self_symbol(table, expr->value);
                    snprintf(qualified, sizeof(qualified), "%s", expr->value);
                }
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

/* #2151: the numeric literal an operand IS — the node itself, or the
 * literal under a unary minus/plus (`v * -0.5` is as much a constant as
 * `v * 0.5`). NULL for anything else. */
static ASTNode* numeric_literal_operand(ASTNode* n) {
    if (!n) return NULL;
    if (n->type == AST_LITERAL) return n;
    if (n->type == AST_UNARY_EXPRESSION && n->child_count == 1 && n->value &&
        (strcmp(n->value, "-") == 0 || strcmp(n->value, "+") == 0) &&
        n->children[0] && n->children[0]->type == AST_LITERAL)
        return n->children[0];
    return NULL;
}

/* The two distinct types a binary operator would mix, or 0. `USD + EUR` (two
 * distinct types over float) and `vec3 + vec4` (two over f32x4) are each a
 * type error: a distinct type has nominal identity, so the operator is no
 * more defined across two of them than an assignment or a call argument is.
 * `=` is an assignment and is checked as one; `in` is a bit_set test. */
static int binary_mixes_distinct(Type* lt, Type* rt, AeTokenType operator) {
    if (operator == TOKEN_ASSIGN || operator == TOKEN_IN) return 0;
    return lt && rt && lt->distinct_name && rt->distinct_name &&
           strcmp(lt->distinct_name, rt->distinct_name) != 0;
}

static Type* infer_binary_type_by_kind(ASTNode* left, ASTNode* right, AeTokenType operator);

/* A binary operator over distinct types (#480, docs/distinct-types.md).
 *  - Two DIFFERENT distinct types never mix: TYPE_UNKNOWN, which the caller
 *    reports naming both.
 *  - The SAME distinct type on both sides keeps it: `USD + USD` is a USD and
 *    `vec4 * vec4` a vec4 (a comparison is still a bool, or a lane mask).
 *  - A distinct lane type beside a SCALAR keeps it: `vec4 * 0.5` scales a
 *    vec4 the way `Duration * n` scales a Duration. The scalar is not the
 *    lane type's base (f32x4), so nothing is unwrapped.
 *  - A distinct type beside a plain value of its base (`USD + 1.0`,
 *    `USD + f` for a float f) is unchanged: it is computed over the base and
 *    the result is the base, which needs `as USD` to become a USD again. */
Type* infer_binary_type(ASTNode* left, ASTNode* right, AeTokenType operator) {
    Type* lt = left ? left->node_type : NULL;
    Type* rt = right ? right->node_type : NULL;
    if (binary_mixes_distinct(lt, rt, operator)) return create_type(TYPE_UNKNOWN);
    Type* result = infer_binary_type_by_kind(left, right, operator);
    if (!result || result->kind == TYPE_UNKNOWN || result->distinct_name ||
        operator == TOKEN_ASSIGN || operator == TOKEN_IN ||
        operator == TOKEN_EQUALS || operator == TOKEN_NOT_EQUALS ||
        operator == TOKEN_LESS || operator == TOKEN_LESS_EQUAL ||
        operator == TOKEN_GREATER || operator == TOKEN_GREATER_EQUAL ||
        operator == TOKEN_AND || operator == TOKEN_OR) return result;
    const char* keep = NULL;
    Type* dt = (lt && lt->distinct_name) ? lt : (rt && rt->distinct_name) ? rt : NULL;
    Type* other = (dt == lt) ? rt : lt;
    if (dt && other && dt->kind == result->kind) {
        if (other->distinct_name)
            keep = dt->distinct_name;              /* same distinct both sides */
        else if (is_lane_type(dt->kind) && !is_lane_type(other->kind))
            keep = dt->distinct_name;              /* distinct lane * scalar */
    }
    if (keep) result->distinct_name = strdup(keep);
    return result;
}

static Type* infer_binary_type_by_kind(ASTNode* left, ASTNode* right, AeTokenType operator) {
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

    /* #1878: a multi-value return bound to ONE name is a TUPLE, and an
     * operator with a tuple on exactly one side is not an operation at all.
     * The front end waved it through and the only symptom was clang's
     * `invalid operands to binary expression ('_tuple_int_string' and
     * 'char[1]')`, reported against generated C the reader never wrote. That
     * is the same shape of report that made #1855 expensive to track down,
     * and it is reachable from a four-line program. */
    if (left_type && right_type &&
        ((left_type->kind == TYPE_TUPLE) != (right_type->kind == TYPE_TUPLE))) {
        return create_type(TYPE_UNKNOWN);
    }

    /* #2146: a comparison of lanes is lane-wise, so it yields a MASK — an
     * i32x4 whose lane is all-ones where the comparison held — not a bool.
     * `lanes.select(m, a, b)` consumes it; `lanes.any` / `lanes.all` reduce
     * it to a bool where a branch is wanted. */
    if (left_type && right_type &&
        (is_lane_type(left_type->kind) || is_lane_type(right_type->kind))) {
        TypeKind lane = is_lane_type(left_type->kind) ? left_type->kind : right_type->kind;
        TypeKind other = is_lane_type(left_type->kind) ? right_type->kind : left_type->kind;
        TypeKind elem = lane_scalar_kind(lane);
        /* The same operand rule the arithmetic has: the other side is the
         * same lane type, or a scalar C splats across the lanes. */
        int operands_ok = (other == lane || other == elem || is_integer_scalar(other) ||
                           (elem == TYPE_FLOAT32 && other == TYPE_FLOAT));
        switch (operator) {
            case TOKEN_EQUALS: case TOKEN_NOT_EQUALS:
            case TOKEN_LESS: case TOKEN_LESS_EQUAL:
            case TOKEN_GREATER: case TOKEN_GREATER_EQUAL:
                return create_type(operands_ok ? lane_mask_kind(lane) : TYPE_UNKNOWN);
            /* A mask is not a bool: `m1 && m2` was typed bool below and
             * reached the C compiler, which rejects a vector there. Masks
             * combine with lanes.mask_and / mask_or, and reduce to a bool
             * with any / all. */
            case TOKEN_AND: case TOKEN_OR:
                return create_type(TYPE_UNKNOWN);
            default: break;
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
            /* #2146: lane-wise arithmetic. Two lanes of the same kind, or a
             * lane and a scalar of its element kind (or an integer literal),
             * which C splats across the lanes. Mixing f32x4 with f64x2, or
             * with a scalar of another kind, is a type error rather than a
             * silent widening: the point of the type is that the width is
             * what the author wrote. */
            if (is_lane_type(left_type->kind) || is_lane_type(right_type->kind)) {
                TypeKind lane = is_lane_type(left_type->kind) ? left_type->kind : right_type->kind;
                TypeKind other = is_lane_type(left_type->kind) ? right_type->kind : left_type->kind;
                TypeKind elem = lane_scalar_kind(lane);
                if (operator == TOKEN_MODULO && (elem == TYPE_FLOAT32 || elem == TYPE_FLOAT))
                    return create_type(TYPE_UNKNOWN);   /* no `%` on float lanes */
                if (other == lane || other == elem || is_integer_scalar(other) ||
                    (elem == TYPE_FLOAT32 && other == TYPE_FLOAT))
                    return create_type(lane);
                return create_type(TYPE_UNKNOWN);
            }
            if (left_type->kind == TYPE_FLOAT32 || right_type->kind == TYPE_FLOAT32) {
                /* #2151: arithmetic on f32 operands is done in C `float`.
                 * f32 op f32 and f32 op <integer> are f32, as C's usual
                 * conversions make them. A numeric LITERAL beside an f32
                 * operand takes the f32 (an untyped constant, as in Go):
                 * `v * 0.5` is one float multiply, not a double one narrowed
                 * at the store, and the literal is emitted with C's `f`
                 * suffix. A float (double) VALUE on the other side still
                 * widens the expression to float, as in C. Until 0.705 every
                 * f32 expression was computed in double, which left a
                 * compute-bound engine where it was: only the storage was
                 * narrow. */
                ASTNode* other = (left_type->kind == TYPE_FLOAT32) ? right : left;
                Type* other_t = (left_type->kind == TYPE_FLOAT32) ? right_type : left_type;
                if (other_t->kind == TYPE_FLOAT32 || is_integer_scalar(other_t->kind) ||
                    other_t->kind == TYPE_BYTE) {
                    return create_type(TYPE_FLOAT32);
                }
                ASTNode* lit = numeric_literal_operand(other);
                if (other_t->kind == TYPE_FLOAT && lit && lit->node_type) {
                    lit->node_type->kind = TYPE_FLOAT32;
                    if (other != lit && other->node_type) other->node_type->kind = TYPE_FLOAT32;
                    return create_type(TYPE_FLOAT32);
                }
                return create_type(TYPE_FLOAT);
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
                /* A shift has the type of its promoted LEFT operand; the
                 * count's type does not take part (`-8 >> u32_count` is
                 * a signed -1, not 4294967295). The 64-bit widening
                 * through the count is kept as it was (#697 computes the
                 * value in 64 bits). */
                TypeKind rk = right_type->kind;
                if ((operator == TOKEN_LSHIFT || operator == TOKEN_RSHIFT) && rk == TYPE_UINT32)
                    rk = TYPE_INT;
                return create_type(wider_integer_kind(left_type->kind, rk));
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

typedef struct {
    const char* name;
    const char* file;   /* the declaring node's own file, not the entry file */
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

    /* A lowered quote's body is the tree's now: each variable it captures
     * is read through the tree (docs/quote.md). */
    if (node->type == AST_QUOTE) {
        QuoteInfo* q = quote_of(node);
        for (int k = 0; q && k < q->ncaps; k++) {
            for (int i = 0; i < var_count; i++) {
                if (strcmp(vars[i].name, q->caps[k].name) == 0) vars[i].used = 1;
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

    // A call through a local's fn-pointer field, `f.get_text(p)`, uses `f`
    // too. The parser collapsed the callee to the dotted name `f.get_text`
    // and dropped the receiver, so there is no identifier for it; the
    // checker tagged the call (#749), and the receiver is the name before
    // the dot (#2635).
    if (node->type == AST_FUNCTION_CALL && node->value && node->annotation &&
        strncmp(node->annotation, "fnfield_", 8) == 0) {
        const char* dot = strchr(node->value, '.');
        size_t rlen = dot ? (size_t)(dot - node->value) : 0;
        for (int i = 0; i < var_count && rlen > 0; i++) {
            if (strlen(vars[i].name) == rlen &&
                strncmp(vars[i].name, node->value, rlen) == 0) {
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

// Collect variable declarations from a block (non-recursive into nested
// functions). `vars` NULL counts without recording, so the caller can size
// the array to the function: a fixed cap silently stopped tracking at the
// 257th declaration (#2144).
static int collect_declarations(ASTNode* node, TrackedVar* vars, int var_count,
                                const char** global_names, int global_count) {
    if (!node) return var_count;

    if (node->type == AST_VARIABLE_DECLARATION && node->value) {
        // Skip _ prefixed names (intentional discard) and writes to a
        // module-level `var` global (a store to the file-scope static,
        // not a local declaration).
        if (node->value[0] != '_' &&
            !is_module_global_name(global_names, global_count, node->value)) {
            if (!vars) { var_count++; goto children; }
            vars[var_count].name = node->value;
            /* #1946: an imported module's AST is merged into the importing
             * program before this pass runs, so the diagnostic must carry the
             * DECLARING node's file. Left NULL, the renderer falls back to the
             * active source context, which by then is the entry file again,
             * and the warning named a file the variable is not in. */
            vars[var_count].file = node->source_file;
            vars[var_count].line = node->line;
            vars[var_count].col = node->column;
            vars[var_count].used = 0;
            var_count++;
        }
    }

children:
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

    // Size to the function, then collect
    int var_count = 0;
    for (int i = 0; i < body->child_count; i++) {
        var_count = collect_declarations(body->children[i], NULL, var_count,
                                         global_names, global_count);
    }
    if (var_count == 0) return;
    TrackedVar* vars = (TrackedVar*)calloc((size_t)var_count, sizeof(TrackedVar));
    if (!vars) return;
    var_count = 0;
    for (int i = 0; i < body->child_count; i++) {
        var_count = collect_declarations(body->children[i], vars, var_count,
                                         global_names, global_count);
    }

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
                .filename = vars[i].file,
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
    free(vars);
}

/* The names of every module-level `var` global in the merged program (#701),
 * an imported module's under its prefixed name. Sized to the program: the
 * set was capped at 256 and a program importing enough modules (ae3d's
 * editor with ui + ae3d + aephysics) went past it, so a setter of any global
 * beyond the cap was reported as an unused local, and a `__pure(fn)` query
 * over a writer of one answered true (#2144). Returns malloc'd memory;
 * caller frees the array (the names are the AST's). */
static ASTNode* global_var_decl(ASTNode* child) {
    if (child && child->type == AST_EXPORT_STATEMENT && child->child_count > 0) {
        child = child->children[0];  // unwrap `export var ...`
    }
    if (child && child->type == AST_CONST_DECLARATION && child->value &&
        child->annotation && strcmp(child->annotation, "global_var") == 0) {
        return child;
    }
    return NULL;
}

static const char** collect_global_var_names(ASTNode* program, int* count) {
    int n = 0;
    for (int i = 0; i < program->child_count; i++) {
        if (global_var_decl(program->children[i])) n++;
    }
    const char** names = (const char**)malloc(sizeof(char*) * (size_t)(n > 0 ? n : 1));
    if (!names) { *count = 0; return NULL; }
    *count = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* decl = global_var_decl(program->children[i]);
        if (decl) names[(*count)++] = decl->value;
    }
    return names;
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

static void check_unreachable_code(ASTNode* body);

/* Check every statement block nested anywhere under `node`: if/else and
 * loop bodies, but also a call's trailing block (`build() { ... }`), a
 * closure body, a bare `{ }` block and match arms. Only the if and while
 * bodies used to be visited, so `return a` followed by `return b` inside a
 * trailing block went unwarned. */
static void check_unreachable_in_nested_blocks(ASTNode* node) {
    if (!node) return;
    for (int i = 0; i < node->child_count; i++) {
        ASTNode* c = node->children[i];
        if (!c) continue;
        if (c->type == AST_BLOCK) check_unreachable_code(c);
        else check_unreachable_in_nested_blocks(c);
    }
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
                    .filename = g_tc_file,
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

        // Recurse into nested blocks (if/else and loop bodies, trailing
        // blocks, closures, bare blocks, match arms).
        if (stmt && stmt->type == AST_BLOCK) check_unreachable_code(stmt);
        else check_unreachable_in_nested_blocks(stmt);
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
static void resolve_cfn_types(ASTNode* program);   /* #2200 */
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

/* #2186: the body of the function (or main) being checked -- codegen's
 * hoist_scope_body, over which a hoisted local's type is joined. */
static ASTNode* g_tc_fn_body = NULL;

/* #2733: what the `return`s being checked return from: the function, main
 * or closure whose body holds them. */
static ASTNode* g_tc_return_owner = NULL;
static void declare_if_branch_hoists(ASTNode* body, SymbolTable* table);


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
        char shown[160];
        snprintf(emsg, sizeof(emsg),
                 is_req
                     ? "`requires` predicate is always false, no call to '%s' "
                       "can ever satisfy it: %s"
                     : "`ensures` predicate is always false, '%s' can never "
                       "return legally: %s",
                 ast_source_name(func, shown, sizeof(shown)), ptxt);
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

/* #2475: the type of a const initializer built from operators (`1 << 30`,
 * `A | B`, `~MASK`, `flags.BASE << 2`), computed from the global table
 * without stamping the AST. Leaves are typed the way the second pass types
 * them (a numeric literal through infer_from_literal, a name through its
 * symbol, `ns.NAME` through the module's prefixed const) and the operators
 * through infer_binary_type / infer_unary_type, which read only the operand
 * types, so stack nodes stand in for the operands. Not stamping matters: a
 * member access that carried a node_type would skip the second pass's
 * rewrite to the prefixed identifier. TYPE_UNKNOWN when a leaf is not
 * resolved yet; the caller retries until nothing changes. */
static Type* const_initializer_type(ASTNode* e, SymbolTable* table) {
    if (!e) return create_type(TYPE_UNKNOWN);
    switch (e->type) {
        case AST_LITERAL:
            if (e->node_type && e->node_type->kind != TYPE_UNKNOWN)
                return clone_type(e->node_type);
            return e->value ? infer_from_literal(e->value) : create_type(TYPE_UNKNOWN);
        case AST_IDENTIFIER: {
            Symbol* s = e->value ? lookup_symbol(table, e->value) : NULL;
            return (s && !s->is_function && s->type) ? clone_type(s->type)
                                                     : create_type(TYPE_UNKNOWN);
        }
        case AST_MEMBER_ACCESS: {
            ASTNode* ns = e->child_count > 0 ? e->children[0] : NULL;
            if (!ns || ns->type != AST_IDENTIFIER || !ns->value || !e->value ||
                lookup_symbol(table, ns->value) || !is_visible_namespace(ns->value, table))
                return create_type(TYPE_UNKNOWN);
            char qualified[512];
            snprintf(qualified, sizeof(qualified), "%s_%s", ns->value, e->value);
            Symbol* s = lookup_symbol(table, qualified);
            return (s && !s->is_function && s->type) ? clone_type(s->type)
                                                     : create_type(TYPE_UNKNOWN);
        }
        case AST_BINARY_EXPRESSION: {
            if (e->child_count < 2) return create_type(TYPE_UNKNOWN);
            ASTNode l = { 0 }, r = { 0 };
            l.node_type = const_initializer_type(e->children[0], table);
            r.node_type = const_initializer_type(e->children[1], table);
            Type* t = infer_binary_type(&l, &r, get_token_type_from_string(e->value));
            free_type(l.node_type);
            free_type(r.node_type);
            return t;
        }
        case AST_UNARY_EXPRESSION: {
            if (e->child_count < 1) return create_type(TYPE_UNKNOWN);
            ASTNode o = { 0 };
            o.node_type = const_initializer_type(e->children[0], table);
            Type* t = infer_unary_type(&o, get_token_type_from_string(e->value));
            free_type(o.node_type);
            return t;
        }
        default:
            return e->node_type ? clone_type(e->node_type) : create_type(TYPE_UNKNOWN);
    }
}

/* #2475: a const whose initializer is an expression (`const MOVED = 1 << 30`)
 * was registered UNKNOWN (registration types a bare literal only, #1857)
 * and stayed so until the second pass reached the declaration. An imported
 * const lands at the end of the merged tree, and a same-file const can follow
 * its users, so every function that read it first bound `n.flags & MOVED`
 * to an untyped local and codegen guessed int. Type each such const from its
 * initializer once every const is registered, repeating while a pass makes
 * progress so a const defined through another (`B = A | 4`, in either order)
 * resolves too. */
static void resolve_const_initializer_types(ASTNode* program, SymbolTable* table) {
    int progress = 1;
    while (progress) {
        progress = 0;
        for (int i = 0; i < program->child_count; i++) {
            ASTNode* c = program->children[i];
            if (!c || c->type != AST_CONST_DECLARATION || !c->value ||
                c->child_count < 1)
                continue;
            Symbol* s = lookup_symbol_local(table, c->value);
            if (!s || (s->type && s->type->kind != TYPE_UNKNOWN)) continue;
            /* #2614: a merged constant names the modules its own module
             * imports, as the second pass checks it. */
            table->merged_from = c->origin_module;
            Type* t = const_initializer_type(c->children[0], table);
            table->merged_from = NULL;
            if (!t || t->kind == TYPE_UNKNOWN) {
                if (t) free_type(t);
                continue;
            }
            if (s->type) free_type(s->type);
            s->type = t;
            /* Same #929 marker the literal path carries for a global var. */
            if (c->type_inferred && t->kind == TYPE_INT) s->type_inferred = 1;
            progress = 1;
        }
    }
}

/* #2495: the top-level consts, module vars and const arrays, which codegen
 * emits as file-scope C definitions in program order. A C initializer can
 * only name a definition above it, so `const HIGH = LOW << 4` ahead of
 * `const LOW = 3` failed in the C compiler. */
typedef struct {
    ASTNode** decls;        /* the declarations, in program order */
    int* slots;             /* each one's index in program->children */
    int count;
    StrMap index;           /* name -> position in decls, plus one */
    char* state;            /* 0 unvisited, 1 on the DFS path, 2 placed */
    int* path;              /* the DFS path, for the cycle message */
    int path_len;
    ASTNode** order;        /* dependency order, filled as decls are placed */
    int placed;
    int reported;
} ConstOrder;

static void const_order_visit(ConstOrder* co, int k);

/* Visit every const that `n` names. After the second pass a cross-module
 * `ns.NAME` is already the identifier `ns_NAME`; one not rewritten (a pass
 * that stopped on an error) is looked up the same way. */
static void const_order_deps(ConstOrder* co, ASTNode* n) {
    if (!n || co->reported) return;
    const char* name = NULL;
    char qualified[512];
    if (n->type == AST_IDENTIFIER && n->value) {
        name = n->value;
    } else if (n->type == AST_MEMBER_ACCESS && n->value && n->child_count > 0 &&
               n->children[0] && n->children[0]->type == AST_IDENTIFIER &&
               n->children[0]->value) {
        snprintf(qualified, sizeof(qualified), "%s_%s", n->children[0]->value, n->value);
        name = qualified;
    }
    if (name) {
        void* hit = strmap_get(&co->index, name);
        if (hit) {
            const_order_visit(co, (int)(intptr_t)hit - 1);
            return;
        }
    }
    for (int i = 0; i < n->child_count; i++) const_order_deps(co, n->children[i]);
}

static void const_order_visit(ConstOrder* co, int k) {
    if (co->reported || co->state[k] == 2) return;
    if (co->state[k] == 1) {
        /* A cycle: report it once, at the declaration that starts it. */
        int start = co->path_len - 1;
        while (start > 0 && co->path[start] != k) start--;
        char chain[640];
        size_t pos = 0;
        chain[0] = '\0';
        for (int i = start; i <= co->path_len && pos + 1 < sizeof(chain); i++) {
            ASTNode* d = co->decls[i < co->path_len ? co->path[i] : k];
            int w = snprintf(chain + pos, sizeof(chain) - pos, "%s%s",
                             i > start ? " -> " : "", d->value);
            if (w < 0) break;
            pos += (size_t)w;
            if (pos >= sizeof(chain)) { pos = sizeof(chain) - 1; break; }
        }
        ASTNode* at = co->decls[k];
        char msg[800];
        snprintf(msg, sizeof(msg), "const '%s' depends on itself: %s",
                 at->value, chain);
        const char* saved = g_tc_file;
        g_tc_file = at->source_file;
        type_error_hint(msg, "a constant's initializer cannot name the constant "
                        "it defines, directly or through other constants",
                        at->line, at->column);
        g_tc_file = saved;
        co->reported = 1;
        return;
    }
    co->state[k] = 1;
    co->path[co->path_len++] = k;
    const_order_deps(co, co->decls[k]->children[0]);
    co->path_len--;
    if (co->reported) return;
    co->state[k] = 2;
    co->order[co->placed++] = co->decls[k];
}

/* Put the top-level consts in dependency order, in the slots they already
 * occupy, so codegen's program-order emission defines each before any
 * initializer names it. The sort is stable: a program whose consts are
 * already in order is left exactly as written. A cycle has no order and is
 * reported at its first declaration. */
static void order_const_declarations(ASTNode* program) {
    ConstOrder co;
    memset(&co, 0, sizeof(co));
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_CONST_DECLARATION && c->value && c->child_count > 0)
            co.count++;
    }
    if (co.count == 0) return;
    co.decls = malloc(sizeof(ASTNode*) * (size_t)co.count);
    co.slots = malloc(sizeof(int) * (size_t)co.count);
    co.state = calloc((size_t)co.count, 1);
    co.path = malloc(sizeof(int) * (size_t)co.count);
    co.order = malloc(sizeof(ASTNode*) * (size_t)co.count);
    strmap_init(&co.index);
    if (co.decls && co.slots && co.state && co.path && co.order) {
        int k = 0;
        for (int i = 0; i < program->child_count; i++) {
            ASTNode* c = program->children[i];
            if (!(c && c->type == AST_CONST_DECLARATION && c->value && c->child_count > 0))
                continue;
            co.decls[k] = c;
            co.slots[k] = i;
            strmap_put(&co.index, c->value, (void*)(intptr_t)(k + 1));
            k++;
        }
        for (int i = 0; i < co.count && !co.reported; i++) const_order_visit(&co, i);
        if (!co.reported) {
            for (int i = 0; i < co.count; i++) program->children[co.slots[i]] = co.order[i];
        }
    }
    free(co.decls);
    free(co.slots);
    free(co.state);
    free(co.path);
    free(co.order);
    strmap_free(&co.index);
}

static void tc_clauses_reset(void);

/* #2886: results no function can give (defined with typecheck_function_definition). */
static Type* result_array_type(Type* t);
static void refuse_array_result(const char* subject, Type* result, Type* arr, int line,
                                int column);
static void refuse_fn_type_array_result(const char* subject, Type* t, int line, int column);

int typecheck_program(ASTNode* program) {
    note_program_globals(program);
    if (!program || program->type != AST_PROGRAM) return 0;
    g_typecheck_program = program;
    g_tc_ptr_to_closure = 0;
    tc_clauses_reset();   /* #2647: a program's clause sets, indexed afresh */

    /* #2631: a module file checked on its own (it has an `exports(...)`
     * list, the test `program_is_module` below makes too) knows the
     * namespace a build gives it, so its own `m.a()` resolves as it does
     * once merged. Set before any pass resolves a qualified name. */
    g_entry_self_ns = NULL;
    for (int i = 0; i < program->child_count; i++) {
        if (program->children[i] && program->children[i]->type == AST_EXPORTS_LIST) {
            if (module_leaf_of_file(program->source_file, g_entry_self_ns_buf,
                                    sizeof(g_entry_self_ns_buf))[0]) {
                g_entry_self_ns = g_entry_self_ns_buf;
            }
            break;
        }
    }

    error_count = 0;
    warning_count = 0;
    namespace_count = 0;  // Reset imported namespaces
    user_explicit_namespace_count = 0;  // Reset user-explicit namespaces (issue #243)
    // (#878: per-module selective-import filter removed — nothing to reset)

    // Trusted names in `sandbox.enforce(perms, foo, bar) { ... }`: validate
    // them, mark the calls they cover, and reduce each such call to the
    // plain enforce(perms, block) the rest of checking sees. Its errors go
    // through type_error, so they count like any other.
    sandbox_trust_pass(program);

    // #480: resolve `type X = distinct Y` placeholders into distinct Types
    // across the whole AST before any type-checking or inference runs.
    resolve_distinct_types(program);

    // #2200: resolve `cfn Name(...) -> R` references — rewrite bare
    // TYPE_STRUCT{Name} use-sites into the named function-pointer signature.
    // After distinct resolution (a name is one or the other) and before any
    // type-checking, so a `p as Name` cast is already a function-pointer cast
    // when its operand is checked.
    resolve_cfn_types(program);

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
        int npg = 0;
        const char** pg = collect_global_var_names(program, &npg);
        resolve_purity_queries(program, program, pg, npg);
        free(pg);
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
    /* #2054: the result of call(f, ...) is f's result, resolved per call
     * site (call_builtin_result_type); a fixed int here was what the
     * inference pass stamped on every dynamic call before the checker saw
     * it, and a string-returning closure came back as its pointer. */
    add_symbol(global_table, "call", create_type(TYPE_UNKNOWN), 0, 1, 0);
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
    /* A module file (top-level `exports(...)`, checked on its own by
     * `ae check`) names its functions in its own namespace — `bytes.free`
     * is never a call by bare name — so the builtin-redefinition rule
     * below is for programs only. In a merged program a module's
     * definitions already carry their prefix. */
    int program_is_module = 0;
    for (int i = 0; i < program->child_count; i++) {
        if (program->children[i] && program->children[i]->type == AST_EXPORTS_LIST) {
            program_is_module = 1;
            break;
        }
    }
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
                    /* A call to one of these is lowered by NAME in codegen
                     * (`isolate(x)` is the identity, `release(s)` a free,
                     * ...), so a user definition of the same name used to
                     * register on top of the builtin and compile while every
                     * call still went to the builtin: the body never ran,
                     * with no diagnostic (#2073). Refuse at the definition,
                     * for the arity codegen intercepts. Builtins codegen does
                     * NOT intercept (`each`, `map`, `filter`) or that yield to
                     * a user definition (`atoi`) stay definable. */
                    if (prior && !prior->node && prior->is_function &&
                        !program_is_module &&
                        builtin_lowered_by_name(child->value, definition_param_count(child))) {
                        char msg[320];
                        snprintf(msg, sizeof(msg),
                            "'%s' is a builtin function and cannot be redefined: "
                            "a call by that name is lowered to the builtin, so "
                            "this body would never run; rename it (e.g. '%s_')",
                            child->value, child->value);
                        type_error(msg, child->line, child->column);
                    }
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
                /* #1857: a numeric literal is deliberately parsed as UNKNOWN
                 * ("let inference decide int or float"), and the const node is
                 * only typed in the second pass. Imported consts land at the
                 * END of the merged tree, so every function binding one to a
                 * local was checked FIRST and inferred UNKNOWN from this
                 * symbol. Codegen then defaulted the local to int: right by
                 * luck for an int const, a silent truncation for a float one
                 * (`k = fl.SCALE` with SCALE = 2.5 emitted `int k`, printing 4
                 * where the answer is 5). Type the literal here, with the same
                 * helper inference uses, so registration does not depend on
                 * which pass has run. */
                if (ctype->kind == TYPE_UNKNOWN && child->child_count > 0 &&
                    child->children[0]->type == AST_LITERAL &&
                    child->children[0]->value) {
                    Type* lit = infer_from_literal(child->children[0]->value);
                    if (lit) {
                        if (lit->kind != TYPE_UNKNOWN) {
                            free_type(ctype);
                            ctype = lit;
                            /* Stamp the nodes too, so the second pass and
                             * codegen agree with what was registered. */
                            if (!child->children[0]->node_type ||
                                child->children[0]->node_type->kind == TYPE_UNKNOWN) {
                                if (child->children[0]->node_type)
                                    free_type(child->children[0]->node_type);
                                child->children[0]->node_type = clone_type(ctype);
                            }
                            if (!child->node_type ||
                                child->node_type->kind == TYPE_UNKNOWN) {
                                if (child->node_type) free_type(child->node_type);
                                child->node_type = clone_type(ctype);
                            }
                        } else {
                            free_type(lit);
                        }
                    }
                }
                add_symbol(global_table, child->value, ctype, 0, 0, 0);
                mark_const_symbol(global_table, child);   /* #2516 */
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
                    // The qualified-call namespace prefix is the LEAF
                    // component (the bit after the last dot), not the
                    // whole sub-path. For `std.http.client` callers
                    // write `client.foo(...)`, not `http.client.foo(...)`.
                    // This matches what the orchestrator's merger uses
                    // when it prefixes wrapper function names — see
                    // module_get_namespace() in aether_module.c (#2209:
                    // the full path, underscored, when two loaded modules
                    // share a leaf).
                    const char* ns_leaf  = get_namespace_from_path(module_path);

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

    /* Before the selective-import aliases below clone the const symbols. */
    resolve_const_initializer_types(program, global_table);

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
                /* #2632: a name the file defines itself is not bound by a
                 * glob import, as a local item shadows a glob import in
                 * Rust. Registering it rewrote the file's own `bytes(n, s,
                 * l)` calls to `string.bytes` (and re-synced its symbol to
                 * that one), so std.number failed `ae check` and a program
                 * with its own `bytes` failed to build. A selective import
                 * names the clash explicitly and stays an error (E1000). */
                if (program_defines_own(program, local_name)) continue;
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

    /* #2736: which of the program's functions return a value, settled
     * before any body is checked, so it does not depend on the order they
     * are defined in. After inference (#2744): a call whose callee depends
     * on a receiver's type (a UFCS call, a field call) is resolved with the
     * types inference gave the definition's locals, as the checker will
     * resolve it. Inference reads none of these answers. */
    tc_yields_settle(program, global_table);

    // Second pass: type check all nodes
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* top = program->children[i];
        g_tc_file = top ? top->source_file : NULL;
        /* #2614: a merged declaration (a module's function, actor or
         * constant) resolves qualified names against its own module's
         * imports, whatever kind of node it is. */
        global_table->merged_from = top ? top->origin_module : NULL;
        typecheck_node(top, global_table);
        global_table->merged_from = NULL;
        warn_erased_result_closures();   /* #2484 */
    }
    g_tc_file = NULL;

    /* After the second pass, which turned each `ns.NAME` in an initializer
     * into the identifier the declaration carries. */
    order_const_declarations(program);

    // Collect module-level `var` global names (#701). A bare
    // `name = expr` inside a function whose `name` is one of these is a
    // write to the file-scope static, not a local declaration — the
    // unused-variable pass must not flag write-only setters for them.
    int global_var_count = 0;
    const char** global_var_names = collect_global_var_names(program, &global_var_count);

    // Third pass: unused variable + unreachable code analysis. Its
    // diagnostics name the file each definition came from, as the second
    // pass's do (#2064).
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        g_tc_file = child->source_file;
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
    g_tc_file = NULL;

    free(global_var_names);

    /* #2684: a function whose result is not void must return on every path.
     * Asked once the program type-checks: whether a `match` covers its type
     * depends on the type the checker gave the value it matches on. */
    if (error_count == 0) error_count += check_missing_returns(program);
    /* A slice of a function's own storage must not outlive it (#2785): asked
     * of the checked program, whose coercions into slices are explicit. */
    if (error_count == 0) error_count += check_slice_escapes(program);

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
        case AST_EXTERN_FUNCTION: {
            // Extern functions have no body to check - just a declaration.
            // #2886: a C function returns no array either, nor does a
            // function type given to or returned by one.
            Type* arr = result_array_type(node->node_type);
            if (arr) {
                char who[256];
                snprintf(who, sizeof(who), "extern '%s' returns", node->value ? node->value : "?");
                refuse_array_result(who, node->node_type, arr, node->line, node->column);
            } else {
                char who[256];
                snprintf(who, sizeof(who), "the result of extern '%s'", node->value ? node->value : "?");
                refuse_fn_type_array_result(who, node->node_type, node->line, node->column);
            }
            for (int i = 0; i < node->child_count; i++) {
                ASTNode* p = node->children[i];
                if (!p || !p->value) continue;
                char who[256];
                snprintf(who, sizeof(who), "parameter '%s' of extern '%s'", p->value,
                         node->value ? node->value : "?");
                refuse_fn_type_array_result(who, p->node_type, p->line, p->column);
            }
            return 1;
        }
        case AST_C_STRUCT_DEF:
            // #891 @c_struct overlay: a declaration of (field, type, offset)
            // tuples — no body, no statements to check. The name set is
            // collected (collect_c_struct_names) and the fields registered at
            // codegen for width/offset resolution. Nothing to typecheck here.
            return 1;
        case AST_STRUCT_DEFINITION:
            return typecheck_struct_definition(node, table);
        case AST_CFN_TYPE_DEF:
            // #2200: every reference was rewritten in resolve_cfn_types and
            // a duplicate name reported there; the declaration itself is
            // only a signature, nothing to check per node.
            return 1;
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
        case AST_MAIN_FUNCTION: {
            /* main's own scope, under the program table, like a function's,
             * so the locals codegen hoists to main's top have one (#2186). */
            ASTNode* saved_fn_body = g_tc_fn_body;
            ASTNode* saved_owner = g_tc_return_owner;
            g_tc_fn_body = node->child_count > 0 ? node->children[0] : NULL;
            g_tc_return_owner = node;
            SymbolTable* main_table = create_symbol_table(table);
            if (g_tc_fn_body) declare_if_branch_hoists(g_tc_fn_body, main_table);
            int ok = typecheck_statement(node, main_table);
            free_symbol_table(main_table);
            g_tc_fn_body = saved_fn_body;
            g_tc_return_owner = saved_owner;
            return ok;
        }
        case AST_CLOSURE:
        case AST_QUOTE:
        case AST_IF_EXPRESSION:
            /* An expression reached as a statement child (`return |x| { ... }`,
             * `return if c { a } else { b }`) was walked by typecheck_statement's
             * default case. For a closure that checked the body in the
             * enclosing scope, where its parameters were undefined, so a call
             * inside the body that used one was refused (#2054); for an
             * if-expression it skipped the branch typing that turns a bare
             * function name into a closure value (#2055). The expression path
             * does both. */
            return typecheck_expression(node, table);
        case AST_BINARY_EXPRESSION:
            /* Likewise `return a == null` with `a: byte[]`: the default case
             * walked the operands but not the binary-operator logic, so the
             * slice was never coerced to its element pointer and the C
             * compare was `AetherSlice == NULL` (#2301). */
            return typecheck_expression(node, table);
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

        /* #2633: a typed fn-pointer field takes a named function's address,
         * as a struct field does (#1240). The name infers its result, so the
         * test below compared `int` with `fn(int) -> int` and refused the one
         * value the field can hold. Stamped with the field's type, so codegen
         * spells the function instead of a closure of it. A closure has no
         * room for its environment here either (#2628). */
        if (value_expr && declared->kind == TYPE_FUNCTION && declared->is_fnptr) {
            char slot[256];
            snprintf(slot, sizeof(slot), "%s.%s", msg_name, field_init->value);
            if (reject_closure_in_fnptr_slot(table, value_expr, actual, declared,
                                             FNPTR_SLOT_FIELD, slot,
                                             value_expr->line, value_expr->column)) {
                free_type(actual);
                free_type(declared);
                continue;
            }
            if (fn_name_matches_signature(table, value_expr, declared)) {
                set_node_type(value_expr, clone_type(declared));
                free_type(actual);
                actual = clone_type(declared);
            }
        }

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

/* #2654: the message pattern a receive arm matches, as codegen finds it: a
 * `Msg(...) -> body` arm, or the first pattern of a V1 block. */
static ASTNode* receive_arm_message_pattern(ASTNode* arm) {
    if (!arm) return NULL;
    if (arm->type == AST_RECEIVE_ARM && arm->child_count >= 2)
        return arm->children[0] && arm->children[0]->type == AST_MESSAGE_PATTERN
               ? arm->children[0] : NULL;
    if (arm->type == AST_BLOCK) {
        for (int k = 0; k < arm->child_count; k++)
            if (arm->children[k] && arm->children[k]->type == AST_MESSAGE_PATTERN)
                return arm->children[k];
    }
    return NULL;
}

/* #2654: a second receive arm for a message the actor already receives
 * can never run. An arm matches by the message alone (a pattern binds
 * fields and takes no guard), and a message is dispatched to one handler
 * per message type; with two, the C had two definitions of that handler.
 * Each arm after the first for a message is refused, naming both, in any
 * of the actor's receive blocks. */
static void reject_duplicate_receive_arms(ASTNode* actor) {
    ASTNode* seen[256];
    int seen_count = 0;
    for (int i = 0; i < actor->child_count; i++) {
        ASTNode* rs = actor->children[i];
        if (!rs || rs->type != AST_RECEIVE_STATEMENT) continue;
        for (int j = 0; j < rs->child_count; j++) {
            ASTNode* pattern = receive_arm_message_pattern(rs->children[j]);
            if (!pattern || !pattern->value) continue;
            ASTNode* first = NULL;
            for (int s = 0; s < seen_count && !first; s++)
                if (strcmp(seen[s]->value, pattern->value) == 0) first = seen[s];
            if (first) {
                char msg[320];
                snprintf(msg, sizeof(msg),
                         "this receive arm for '%s' can never run: the arm for '%s' at "
                         "line %d already receives every '%s' message of actor '%s'. "
                         "Handle both cases in that one arm",
                         pattern->value, first->value, first->line, pattern->value,
                         actor->value ? actor->value : "?");
                type_error(msg, pattern->line, pattern->column);
            } else if (seen_count < (int)(sizeof(seen) / sizeof(seen[0]))) {
                seen[seen_count++] = pattern;
            }
        }
    }
}

int typecheck_actor_definition(ASTNode* actor, SymbolTable* table) {
    if (!actor || actor->type != AST_ACTOR_DEFINITION) return 0;

    reject_duplicate_receive_arms(actor);   /* #2654 */

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
                } else {
                    /* Any other initializer (`f() or { d }`) has the type
                     * it infers to. Left unknown, every use of the field in
                     * the handlers was untyped, and codegen took it for an
                     * int (#2821). */
                    Type* it = infer_type(init, actor_table);
                    if (it && it->kind != TYPE_UNKNOWN) set_node_type(child, it);
                    else if (it) free_type(it);
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
                                /* #2454: a binding named like a state field.
                                 * State is reached by its bare name, so the arm
                                 * cannot have both: codegen resolved `v` to the
                                 * state and the message's value was silently
                                 * lost. Refuse it, and say how to rename. */
                                for (int s = 0; s < actor->child_count; s++) {
                                    ASTNode* sd = actor->children[s];
                                    if (sd && sd->type == AST_STATE_DECLARATION && sd->value &&
                                        strcmp(sd->value, var_name) == 0) {
                                        char msg[200], hint[200];
                                        snprintf(msg, sizeof(msg),
                                                 "receive pattern binds '%s', which is also a state field of this actor",
                                                 var_name);
                                        snprintf(hint, sizeof(hint),
                                                 "bind the message field under another name: `%s(%s: new_%s)`",
                                                 pattern->value ? pattern->value : "Msg",
                                                 field->value, var_name);
                                        aether_error_with_suggestion(msg, field->line, field->column, hint);
                                        error_count++;
                                        break;
                                    }
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
    if (node->type == AST_QUOTE && scoped_mentions(node, name))
        return "it is captured by a quote, whose tree can outlive the block";
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
    if (node->type == AST_CLOSURE || node->type == AST_QUOTE || node->type == AST_FUNCTION_DEFINITION ||
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
        case AST_QUOTE:
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

/* The definition a call names, or NULL when the walk cannot see one (a
 * runtime builtin, an extern). A qualified call `ns.fn` into an imported
 * Aether module reaches the merged program as `ns_fn` (module_get_namespace
 * + the merger's prefix), so it is looked up under that spelling; the
 * effect and purity walks used to stop at every dotted call that was not a
 * capability namespace, so a module function that wrote a module global or
 * read a file was invisible to `__pure` and to `@no_fs` (#2144). */
static ASTNode* effect_resolve_callee(ASTNode* program, const char* name) {
    if (!name) return NULL;
    const char* dot = strchr(name, '.');
    if (!dot) return effect_find_func(program, name);
    char merged[256];
    size_t n = strlen(name);
    if (n >= sizeof(merged)) return NULL;
    for (size_t i = 0; i < n; i++) merged[i] = name[i] == '.' ? '_' : name[i];
    merged[n] = '\0';
    /* Only a merged module function: `value.method()` (UFCS on a local) is
     * also spelled `a.b` here, and must not be read as the program's own
     * `a_b` if one happens to exist. */
    ASTNode* def = effect_find_func(program, merged);
    return (def && def->is_imported) ? def : NULL;
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
        }
        {
            int seen = 0;
            for (int i = 0; i < *nvisited; i++)
                if (strcmp(visited[i], node->value) == 0) { seen = 1; break; }
            if (!seen && *nvisited < 512) {
                ASTNode* def = effect_resolve_callee(program, node->value);
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
            /* #2729: named as the source spells it (`checks.shell`), not by
             * the merged C name (`checks_shell`). */
            char shown[160];
            const char* name = ast_source_name(fn, shown, sizeof(shown));
            char msg[400];
            snprintf(msg, sizeof(msg),
                "function '%s' is declared `@no_%s` (or `@pure`) but reaches a "
                "`%s` operation through `%s`. Remove that call path, or drop the "
                "effect tag.", name, cap, cap, offending);
            g_tc_file = fn->source_file;   /* #2064: the definition's own file */
            type_error(msg, fn->line, fn->column);
            g_tc_file = NULL;
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
        }
        {
            int seen = 0;
            for (int i = 0; i < *nv; i++)
                if (strcmp(visited[i], node->value) == 0) { seen = 1; break; }
            if (!seen && *nv < 512) {
                ASTNode* def = effect_resolve_callee(program, node->value);
                if (def && def->child_count > 0) {
                    visited[(*nv)++] = node->value;
                    if (purity_scan(program, def, def->children[def->child_count - 1],
                                    globals, nglobals, visited, nv, depth + 1)) return 1;
                }
            }
        }
    }
    /* A bare `g = expr` on a module global parses as a declaration (#701),
     * so the write was never seen here and every setter folded to pure. */
    if (node->type == AST_VARIABLE_DECLARATION && node->value) {
        for (int g = 0; g < nglobals; g++)
            if (strcmp(globals[g], node->value) == 0) return 1;
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

    /* Bare C externs stay in the module registry rather than being cloned
     * into the merged program. Import registration uses those declarations
     * for both the return type and parameter checks, so resolve their named
     * types too. Otherwise an imported extern taking Grants still has the
     * parser's TYPE_STRUCT placeholder while its caller has distinct ptr. */
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (!c || c->type != AST_IMPORT_STATEMENT || !c->value) continue;
        AetherModule* mod = module_find(c->value);
        if (!mod || !mod->ast) continue;
        for (int j = 0; j < mod->ast->child_count; j++) {
            ASTNode* decl = mod->ast->children[j];
            if (decl && decl->type == AST_EXTERN_FUNCTION)
                distinct_rewrite_ast(decl, defs, ndefs);
        }
    }
}

// #2200 named C function-pointer types. `cfn Name(a: T1, b: T2) -> R` parses
// to AST_CFN_TYPE_DEF (value = Name, node_type = the TYPE_FUNCTION signature
// with is_fnptr=1). A use site `x: Name` or `p as Name` is parsed as a bare
// TYPE_STRUCT{Name}; this pass rewrites every such reference into a copy of
// the signature, exactly as resolve_distinct_types does for distinct aliases,
// so everything downstream sees the same `fn(T1, T2) -> R` it already knows.
// A `p as Name` cast was parsed as an AST_VALUE_CAST (the parser cannot tell a
// cfn name from a distinct one); once its target is a function pointer the
// node becomes the AST_PTR_AS_FN_CAST that `p as fn(...)` produces directly.
#define AETHER_MAX_CFNS 256
typedef struct { const char* name; Type* sig; } CfnDef;

/* Turn the in-place TYPE_STRUCT{cfnName} into the signature. The name is
 * dropped: a cfn is structural, two names over the same signature are one
 * type (as `fn(int) -> int` written twice is). */
static void cfn_apply(Type* t, Type* sig) {
    free(t->struct_name);
    t->struct_name = NULL;
    t->kind = TYPE_FUNCTION;
    t->is_fnptr = 1;
    t->param_count = sig->param_count;
    t->param_types = sig->param_count > 0
        ? (Type**)malloc((size_t)sig->param_count * sizeof(Type*)) : NULL;
    for (int i = 0; i < sig->param_count; i++)
        t->param_types[i] = clone_type(sig->param_types[i]);
    t->return_type = sig->return_type ? clone_type(sig->return_type)
                                      : create_type(TYPE_VOID);
}

static void cfn_rewrite_type(Type* t, CfnDef* defs, int ndefs, int depth) {
    if (!t || depth > 64) return;
    if (t->kind == TYPE_STRUCT && t->struct_name && !t->distinct_name) {
        for (int i = 0; i < ndefs; i++) {
            if (strcmp(t->struct_name, defs[i].name) == 0) {
                cfn_apply(t, defs[i].sig);
                break;
            }
        }
    }
    /* Recurse after the rewrite: a signature naming another cfn
     * (`cfn Register(cb: Callback)`) resolves that one too. */
    cfn_rewrite_type(t->element_type, defs, ndefs, depth + 1);
    cfn_rewrite_type(t->return_type, defs, ndefs, depth + 1);
    for (int i = 0; i < t->tuple_count; i++)
        if (t->tuple_types) cfn_rewrite_type(t->tuple_types[i], defs, ndefs, depth + 1);
    for (int i = 0; i < t->param_count; i++)
        if (t->param_types) cfn_rewrite_type(t->param_types[i], defs, ndefs, depth + 1);
}

static void cfn_rewrite_ast(ASTNode* n, CfnDef* defs, int ndefs) {
    if (!n) return;
    if (n->node_type) {
        cfn_rewrite_type(n->node_type, defs, ndefs, 0);
        if (n->type == AST_VALUE_CAST && n->node_type->kind == TYPE_FUNCTION &&
            n->node_type->is_fnptr) {
            n->type = AST_PTR_AS_FN_CAST;
        }
    }
    for (int i = 0; i < n->child_count; i++)
        cfn_rewrite_ast(n->children[i], defs, ndefs);
}

static ASTNode* unwrap_cfn_def(ASTNode* c) {
    if (c && c->type == AST_EXPORT_STATEMENT && c->child_count > 0) c = c->children[0];
    return (c && c->type == AST_CFN_TYPE_DEF && c->value && c->node_type) ? c : NULL;
}

static void resolve_cfn_types(ASTNode* program) {
    if (!program) return;
    CfnDef defs[AETHER_MAX_CFNS];
    int ndefs = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = unwrap_cfn_def(program->children[i]);
        if (!c) continue;
        int dup = 0;
        for (int j = 0; j < ndefs; j++) {
            if (strcmp(defs[j].name, c->value) == 0) { dup = 1; break; }
        }
        if (dup) {
            char msg[256];
            snprintf(msg, sizeof(msg), "cfn `%s` is declared twice", c->value);
            type_error(msg, c->line, c->column);
            continue;
        }
        if (ndefs < AETHER_MAX_CFNS) {
            defs[ndefs].name = c->value;
            defs[ndefs].sig = c->node_type;
            ndefs++;
        }
    }
    if (ndefs == 0) return;
    cfn_rewrite_ast(program, defs, ndefs);
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

/* #2886: a result no function can give. C returns no array, and the code
 * generator does not wrap one (making it a value a function returns is
 * #2894), so a fixed-size array result, alone or at a position of a tuple
 * result, compiled to C that did not build (`int[2] pair(void);`). The
 * offending array type of result type `t`, or NULL. A slice (`T[]`) is a
 * value and may be returned. */
static Type* result_array_type(Type* t) {
    if (!t) return NULL;
    if (type_is_sized_array(t)) return t;
    if (t->kind == TYPE_TUPLE) {
        for (int i = 0; t->tuple_types && i < t->tuple_count; i++) {
            Type* a = result_array_type(t->tuple_types[i]);
            if (a) return a;
        }
    }
    return NULL;
}

/* A function type written inside `t` (`t` itself, a parameter's or the
 * result's of one, an element's) whose result is such an array: the
 * function type, or NULL. */
static Type* fn_type_with_array_result(Type* t, int depth) {
    if (!t || depth > 16) return NULL;
    if (t->kind == TYPE_FUNCTION) {
        if (result_array_type(t->return_type)) return t;
        for (int i = 0; t->param_types && i < t->param_count; i++) {
            Type* f = fn_type_with_array_result(t->param_types[i], depth + 1);
            if (f) return f;
        }
        return fn_type_with_array_result(t->return_type, depth + 1);
    }
    for (int i = 0; t->tuple_types && i < t->tuple_count; i++) {
        Type* f = fn_type_with_array_result(t->tuple_types[i], depth + 1);
        if (f) return f;
    }
    return fn_type_with_array_result(t->element_type, depth + 1);
}

/* `arr` as it is written in Aether (`Doc[2]`, `[Dir]int`), for the help. */
static void array_type_spelling(Type* arr, char* out, size_t cap) {
    Type* el = arr->element_type;
    const char* e = el && el->kind == TYPE_STRUCT && el->struct_name ? el->struct_name
                                                                      : type_to_string(el);
    if (arr->index_enum_name) snprintf(out, cap, "[%s]%s", arr->index_enum_name, e);
    else snprintf(out, cap, "%s[%d]", e, arr->array_size);
}

/* Refuse result type `result` (holding the array `arr`) of what `subject`
 * names: "'pair' returns", "extern 'pair_c' returns", "this closure
 * returns", "parameter 'g' holds the function type || -> int[2], which
 * returns". */
static void refuse_array_result(const char* subject, Type* result, Type* arr, int line,
                                int column) {
    char spelled[128], msg[512], hint[256];
    array_type_spelling(arr, spelled, sizeof(spelled));
    if (result == arr) {
        snprintf(msg, sizeof(msg),
                 "%s %s, a fixed-size array: a function cannot return one, alone or in a "
                 "tuple (#2886)", subject, spelled);
    } else {
        snprintf(msg, sizeof(msg),
                 "%s %s, which holds the fixed-size array %s: a function cannot return one, "
                 "alone or in a tuple (#2886)", subject, type_to_string(result), spelled);
    }
    snprintf(hint, sizeof(hint),
             "wrap it in a struct and return the struct: struct Pair { v: %s }", spelled);
    type_error_code_hint(msg, hint, line, column, AETHER_ERR_ARRAY_RESULT);
}

/* Refuse a written type `t` of what `subject` names (a parameter, a field, a
 * local) when it holds a function type whose result is such an array. */
static void refuse_fn_type_array_result(const char* subject, Type* t, int line, int column) {
    Type* f = fn_type_with_array_result(t, 0);
    if (!f) return;
    char who[256];
    snprintf(who, sizeof(who), "%s holds the function type %s, which returns",
             subject, type_to_string(f));
    refuse_array_result(who, f->return_type, result_array_type(f->return_type), line, column);
}

/* The first `return` of a closure's own body whose value would make the
 * closure return such an array (alone, or at a position of a tuple): the
 * array type, the statement in *at. Not a nested closure's. */
static Type* closure_array_return(ASTNode* node, ASTNode** at) {
    if (!node || ast_is_own_function(node)) return NULL;
    if (node->type == AST_RETURN_STATEMENT) {
        for (int i = 0; i < node->child_count; i++) {
            ASTNode* v = node->children[i];
            Type* a = v ? result_array_type(v->node_type) : NULL;
            if (a) {
                *at = node;
                return a;
            }
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        Type* a = closure_array_return(node->children[i], at);
        if (a) return a;
    }
    return NULL;
}

int typecheck_function_definition(ASTNode* func, SymbolTable* table) {
    if (!func || (func->type != AST_FUNCTION_DEFINITION && func->type != AST_BUILDER_FUNCTION)) return 0;

    /* #2886: written (`-> int[2]`) or inferred (`return arr`). */
    {
        Type* arr = result_array_type(func->node_type);
        if (arr) {
            char who[256];
            snprintf(who, sizeof(who), "'%s' returns", func->value ? func->value : "function");
            refuse_array_result(who, func->node_type, arr, func->line, func->column);
        }
    }

    SymbolTable* func_table = create_symbol_table(table);

    // Issue #243 sealed scopes, #2614: a function cloned in from a module
    // resolves qualified calls against that module's imports. The origin
    // propagates from parent in create_symbol_table, so nested scopes
    // inside this body inherit it; on function exit we just free func_table.
    if (func->origin_module) {
        func_table->merged_from = func->origin_module;
    }

    // Add parameters to function's symbol table
    for (int i = 0; i < func->child_count - 1; i++) { // Last child is body
        ASTNode* param = func->children[i];
        if (param->type == AST_VARIABLE_DECLARATION || param->type == AST_PATTERN_VARIABLE) {
            Type* param_type = param->node_type ? clone_type(param->node_type) : create_type(TYPE_UNKNOWN);
            add_symbol(func_table, param->value, param_type, 0, 0, 0);
            if (param->value) {
                char who[192];
                snprintf(who, sizeof(who), "parameter '%s'", param->value);
                refuse_fn_type_array_result(who, param->node_type, param->line, param->column);
            }
        }
    }

    // Builder functions get implicit _builder: ptr parameter
    if (func->type == AST_BUILDER_FUNCTION) {
        add_symbol(func_table, "_builder", create_type(TYPE_PTR), 0, 0, 0);
    }

    /* A clause guard (`f(s) when s == "bob"`) reads the parameters, so it is
     * checked in their scope, as the body is. It was never checked, so its
     * nodes had no types and codegen could not tell a string comparison in
     * it from a scalar one: `s == "bob"` compiled to a C pointer compare
     * (#2515). */
    for (int i = 0; i < func->child_count - 1; i++) {
        ASTNode* guard = func->children[i];
        if (guard && guard->type == AST_GUARD_CLAUSE && guard->child_count > 0) {
            typecheck_expression(guard->children[0], func_table);
        }
    }

    // Type check function body. Track the return type so `return Blue` can
    // resolve a bare enum member (save/restore keeps nested functions correct).
    ASTNode* body = func->children[func->child_count - 1];
    Type* saved_ret = g_tc_return_type;
    g_tc_return_type = func->node_type;
    ASTNode* saved_fn_body = g_tc_fn_body;
    ASTNode* saved_owner = g_tc_return_owner;
    g_tc_fn_body = body;
    g_tc_return_owner = func;
    declare_if_branch_hoists(body, func_table);   /* #2186 */
    typecheck_statement(body, func_table);
    g_tc_fn_body = saved_fn_body;
    g_tc_return_owner = saved_owner;
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

/* #2822: does a value of type `t` own memory a struct field would have to
 * free or copy: a string, a closure, or a struct (held by value) with such
 * a field? */
static int type_owns_memory(Type* t, SymbolTable* table, int depth) {
    if (!t || depth > 16) return 0;
    if (t->kind == TYPE_STRING) return 1;
    if (t->kind == TYPE_FUNCTION) return !t->is_fnptr;
    /* An optional, an array (a fixed-size one holds its elements, which a
     * struct owning them frees) and a tuple own what their elements own. */
    if (t->kind == TYPE_OPTIONAL || t->kind == TYPE_ARRAY)
        return type_owns_memory(t->element_type, table, depth + 1);
    if (t->kind == TYPE_TUPLE) {
        for (int i = 0; t->tuple_types && i < t->tuple_count; i++)
            if (type_owns_memory(t->tuple_types[i], table, depth + 1)) return 1;
        return 0;
    }
    if (t->kind != TYPE_STRUCT || !t->struct_name) return 0;
    Symbol* s = lookup_symbol(table, t->struct_name);
    ASTNode* def = s ? s->node : NULL;
    if (!def || def->type != AST_STRUCT_DEFINITION) return 0;
    for (int i = 0; i < def->child_count; i++) {
        ASTNode* f = def->children[i];
        if (f && f->type == AST_STRUCT_FIELD && type_owns_memory(f->node_type, table, depth + 1))
            return 1;
    }
    return 0;
}

int typecheck_struct_definition(ASTNode* struct_def, SymbolTable* table) {
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
        
        /* #2886: a field of a function type that returns a fixed-size array. */
        {
            char who[192];
            snprintf(who, sizeof(who), "struct field '%s'", field->value ? field->value : "?");
            refuse_fn_type_array_result(who, field->node_type, field->line, field->column);
        }

        /* #2822: an optional field compiles (#2820), but one whose value
         * owns memory has none of a `string` field's ownership (destroy,
         * replace, dup, a take on store), so a string stored there would
         * leak or dangle. Refused until it has. */
        if (field->node_type->kind == TYPE_OPTIONAL &&
            type_owns_memory(field->node_type->element_type, table, 0)) {
            char error_msg[512];
            snprintf(error_msg, sizeof(error_msg),
                     "struct field '%s' is %s: an optional field whose value owns memory (a "
                     "string, a closure, or a struct holding one) is not supported yet "
                     "(#2822). Hold the value bare (a `string` with \"\" for none) or keep "
                     "the optional in a local",
                     field->value, type_to_string(field->node_type));
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

/* Is `name` bound in an enclosing LOCAL scope (not the program table)? */
static int bound_in_local_scope(SymbolTable* table, const char* name) {
    for (SymbolTable* t = table; t && t->parent; t = t->parent) {
        if (lookup_symbol_local(t, name)) return 1;
    }
    return 0;
}

/* #2186: declare `name` in `table` where codegen declares it -- the rules
 * are hoist.h's, shared with codegen, so a read the C allows is a read the
 * checker allows, and no other. Typed as codegen types the hoisted
 * declaration: `decl`'s type (the early inference pass's), joined over the
 * function body. Skipped, as codegen skips it, when the name is already a
 * local here or a program-level variable. */
static void declare_hoisted_local(SymbolTable* table, const char* name, Type* t) {
    if (!table || !table->parent || !name || !t) return;
    /* `_` is the discard binding: it names no value and is never declared
     * (codegen emits `(void)expr`). Hoisting it out of a loop body typed it
     * in the enclosing scope, so `_ = int_fn()` in a while followed by
     * `_ = string_fn()` after it failed with E0200. */
    if (strcmp(name, "_") == 0) return;
    if (bound_in_local_scope(table, name)) return;
    SymbolTable* root = table;
    while (root->parent) root = root->parent;
    Symbol* g = lookup_symbol_local(root, name);
    if (g && !g->is_function && !g->is_module_alias) return;
    Type* joined = hoist_join_type(g_tc_fn_body, name, t);
    add_symbol(table, name, joined ? joined : clone_type(t), 0, 0, 0);
    Symbol* s = lookup_symbol_local(table, name);
    if (s) {
        s->branch_hoisted = 1;
        /* #2683: declared ahead of its first binding, it must not hide a
         * function of its name before that binding (its own initializer
         * included): `if c { label = label() } else { ... }`. */
        s->hoist_pending = g && g->is_function;
    }
}

/* #2683: the checker has reached the first binding of `name` (its
 * initializer checked): a hoisted local of that name is a local from here
 * on. */
static void activate_hoisted_local(SymbolTable* table, const char* name) {
    for (SymbolTable* t = table; t && name; t = t->parent) {
        Symbol* s = lookup_symbol_local(t, name);
        if (s) {
            if (s->hoist_pending) s->hoist_pending = 0;
            return;
        }
    }
}

/* #2611: is a binding of type `here`, in a sibling branch or loop body of a
 * local hoisted as `hoisted`, of the same kind but another C type: a struct
 * or sum of another name, or a pointer to another type? Codegen's sibling
 * check refuses a binding of another kind and passes one of the same kind,
 * leaving the nominal rules to this checker, which handed the binding back
 * to codegen unjudged. So `view = block as *Pear` in one loop beside
 * `view = block as *Apple` in another reached the C compiler as an
 * assignment to the `Apple*` both share. A bare `ptr`, and a pointer to a
 * @c_struct overlay (a `void*` in C), take any pointer. */
static int sibling_binding_other_type(Type* here, Type* hoisted) {
    if (!here || !hoisted || here->kind != hoisted->kind) return 0;
    if (here->kind == TYPE_STRUCT || here->kind == TYPE_SUM)
        return here->struct_name && hoisted->struct_name &&
               strcmp(here->struct_name, hoisted->struct_name) != 0;
    if (here->kind != TYPE_PTR) return 0;
    Type* a = here->element_type;
    Type* b = hoisted->element_type;
    if (!a || !b || a->kind == TYPE_UNKNOWN || b->kind == TYPE_UNKNOWN) return 0;
    if ((a->kind == TYPE_STRUCT && is_c_struct_name(a->struct_name)) ||
        (b->kind == TYPE_STRUCT && is_c_struct_name(b->struct_name))) return 0;
    return !is_type_compatible(here, hoisted);
}

/* Function (or main) entry: hoist_if_branch_vars's names. */
static void declare_if_branch_hoists(ASTNode* body, SymbolTable* table) {
    const char* names[HOIST_MAX_NAMES];
    int n = hoist_if_branch_candidates(body, names, HOIST_MAX_NAMES);
    for (int i = 0; i < n; i++) {
        declare_hoisted_local(table, names[i],
                              hoist_decl_type(hoist_if_branch_first_decl(body, names[i])));
    }
}

/* Before an if/else, or a switch with a default (#2732): hoist_common_vars's
 * names (every arm declares them), typed from the first arm that types
 * them. */
static void declare_common_hoists(ASTNode* stmt, SymbolTable* table) {
    const char* names[HOIST_MAX_NAMES];
    int n = hoist_common_decl_names(stmt, names, HOIST_MAX_NAMES);
    for (int i = 0; i < n; i++) {
        declare_hoisted_local(table, names[i],
                              hoist_decl_type(hoist_common_first_decl(stmt, names[i])));
    }
}

/* Before a while: hoist_loop_vars's names. */
static void declare_loop_hoist(ASTNode* decl, void* user) {
    declare_hoisted_local((SymbolTable*)user, decl->value, hoist_decl_type(decl));
}

/* #2768: the matches being checked whose value is used: bound, returned,
 * or yielded by an arm block of another such match (the last statement of
 * the block). Codegen gives each a result variable and keeps its arms'
 * names in the arms; a statement match declares the names every arm binds
 * before it (declare_common_hoists), as an if/else does. */
#define TC_MAX_VALUE_MATCHES 256
static ASTNode* tc_value_matches[TC_MAX_VALUE_MATCHES];
static int tc_value_match_depth = 0;

static void tc_push_value_match(ASTNode* m) {
    if (tc_value_match_depth < TC_MAX_VALUE_MATCHES)
        tc_value_matches[tc_value_match_depth] = m;
    tc_value_match_depth++;   /* always increment so pop stays balanced */
}
static void tc_pop_value_match(void) {
    if (tc_value_match_depth > 0) tc_value_match_depth--;
}
static int tc_is_value_match(ASTNode* m) {
    int n = tc_value_match_depth < TC_MAX_VALUE_MATCHES
          ? tc_value_match_depth : TC_MAX_VALUE_MATCHES;
    for (int i = n - 1; i >= 0; i--)
        if (tc_value_matches[i] == m) return 1;
    return 0;
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
 * Reported only where the shadowing is certain. A bare identifier arm binds
 * nothing: over a scalar the value is compared with the variable of that
 * name, whose value is not known here, so such arms are never compared
 * (#2700); a warning on correct code would be worse than none. Over an enum,
 * a sum or an optional the set of cases is closed and every arm names one
 * of them, so those arms are compared too (#2728). */
static int match_arm_is_wildcard(ASTNode* p) {
    if (!p) return 0;
    if (p->node_type && p->node_type->kind == TYPE_WILDCARD) return 1;
    /* `_` is the wildcard; a string literal "_" is the one-character string
     * it spells. */
    return p->type == AST_IDENTIFIER && p->value && strcmp(p->value, "_") == 0;
}

/* A comparable key, malloc'd, for one compared value whose identity is
 * unambiguous; NULL for everything else, which is then left alone. The key
 * covers the whole value: a fixed 160-byte key made two long literals
 * sharing a prefix duplicates (#2521). `closed` is set over an enum, a sum
 * or an optional, where an identifier names a member or a variant (a member
 * already lowered to its constant `Enum_Member`) and never a variable. */
static char* match_arm_key(ASTNode* p, int closed) {
    if (!p || match_arm_is_wildcard(p)) return NULL;
    if (closed && p->type == AST_NONE_LITERAL) return strdup("none");
    if (closed && p->type == AST_PATTERN_VARIABLE && p->annotation &&
        strcmp(p->annotation, "some_pattern") == 0) return strdup("some");
    if (closed && (p->type == AST_IDENTIFIER || p->type == AST_PATTERN_VARIABLE) &&
        p->value) {
        size_t n = 3 + strlen(p->value) + 1;
        char* key = malloc(n);
        if (!key) return NULL;
        snprintf(key, n, "id:%s", p->value);
        return key;
    }
    if (p->type == AST_LITERAL && p->value) {
        /* By length: a string literal may hold a NUL (#2520). A NUL is
         * written as `\0` and a backslash as `\\`, so no two literals share
         * a key. */
        int n = ast_literal_length(p);
        char* key = malloc(4 + (size_t)n * 2 + 1);
        if (!key) return NULL;
        memcpy(key, "lit:", 4);
        size_t o = 4;
        for (int i = 0; i < n; i++) {
            char c = p->value[i];
            if (c == '\0') { key[o++] = '\\'; key[o++] = '0'; }
            else if (c == '\\') { key[o++] = '\\'; key[o++] = '\\'; }
            else key[o++] = c;
        }
        key[o] = '\0';
        return key;
    }
    if (p->type == AST_MEMBER_ACCESS && p->value &&
        p->child_count > 0 && p->children[0] && p->children[0]->value) {
        size_t n = 4 + strlen(p->children[0]->value) + 1 + strlen(p->value) + 1;
        char* key = malloc(n);
        if (!key) return NULL;
        snprintf(key, n, "mem:%s.%s", p->children[0]->value, p->value);
        return key;
    }
    return NULL;
}

static void warn_unreachable_arm(ASTNode* pattern, const char* msg,
                                 const char* suggestion) {
    AetherError w = {
        .filename = g_tc_file, .source_code = NULL,
        .line = pattern->line, .column = pattern->column,
        .message = msg, .suggestion = suggestion,
        .context = NULL, .code = AETHER_WARN_UNREACHABLE_ARM
    };
    aether_warning_report(&w);
    warning_count++;
}

/* #2727: the values a selector compares with: the selector itself, or each
 * value of a comma-list. A range is one value here. Returns the count written
 * to `out` (at most `cap`). */
static int selector_elements(ASTNode* sel, ASTNode** out, int cap) {
    if (!sel || cap <= 0) return 0;
    if (sel->type != AST_MATCH_ALT) { out[0] = sel; return 1; }
    int n = 0;
    for (int i = 0; i < sel->child_count && n < cap; i++)
        if (sel->children[i]) out[n++] = sel->children[i];
    return n;
}

#define SELECTOR_ELEMENTS_MAX 256

/* Run after the arms are checked, so a bare enum member is its constant by
 * then. An arm is unreachable below a `_`, or when every value it names is
 * already named by the arms above it (#2728: a comma-list counts value by
 * value, and over an enum, a sum or an optional an identifier is a case). */
static void check_match_arm_reachability(ASTNode* stmt, int closed) {
    if (!stmt) return;
    enum { MATCH_KEYS_MAX = 256 };
    char* keys[MATCH_KEYS_MAX];
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

        ASTNode* els[SELECTOR_ELEMENTS_MAX];
        int nel = selector_elements(pattern, els, SELECTOR_ELEMENTS_MAX);
        char* arm_keys[SELECTOR_ELEMENTS_MAX];
        int nak = 0, all_keyed = nel > 0, dup_line = -1, all_seen = 1;
        for (int e = 0; e < nel; e++) {
            char* key = match_arm_key(els[e], closed);
            if (!key) { all_keyed = 0; continue; }
            int seen = -1;
            for (int k = 0; k < nkeys && seen < 0; k++)
                if (strcmp(keys[k], key) == 0) seen = key_line[k];
            for (int k = 0; k < nak && seen < 0; k++)
                if (strcmp(arm_keys[k], key) == 0) seen = pattern->line;
            if (seen < 0) all_seen = 0;
            else if (dup_line < 0) dup_line = seen;
            arm_keys[nak++] = key;
        }
        if (all_keyed && all_seen && dup_line >= 0) {
            char msg[240];
            snprintf(msg, sizeof(msg),
                     "unreachable match arm: this case is already handled on "
                     "line %d", dup_line);
            warn_unreachable_arm(pattern, msg,
                                 "remove the duplicate, or change it to the case you meant");
        }
        for (int k = 0; k < nak; k++) {
            if (nkeys < MATCH_KEYS_MAX) {
                keys[nkeys] = arm_keys[k];
                key_line[nkeys] = pattern->line;
                nkeys++;
            } else {
                free(arm_keys[k]);
            }
        }
    }
    for (int k = 0; k < nkeys; k++) free(keys[k]);
}

/* ---- #2727: a case selector against the value it is compared with -------
 *
 * A `match` arm or a `switch` case compares the value matched with what it
 * names: a value, a range or a comma-list of them. Nothing checked that
 * against the value, so codegen's `_match_val == <pattern>` met it first: an
 * arm naming no variable reached the C compiler as an undeclared identifier,
 * a string arm over an int was a pointer compared with an integer that never
 * matched, and a literal arm over an optional or a sum was dropped without a
 * word. Each value is now checked as the comparison it lowers to. */

/* `name` with its indefinite article, for a message: "an int", "a string". */
static const char* with_article(const char* name) {
    if (!name || !*name) return "a value";
    char c = name[0];
    int vowel = c == 'a' || c == 'e' || c == 'i' || c == 'o' ||
                strncmp(name, "un", 2) == 0;
    return aether_internf("%s %s", vowel ? "an" : "a", name);
}

static int selector_in_switch(const char* what) {
    return what && strcmp(what, "case") == 0;
}

static void selector_mismatch(ASTNode* at, const char* what, const char* shown,
                              Type* got, Type* subject) {
    char msg[400];
    char help[300];
    snprintf(msg, sizeof(msg),
             "this %s compares the value matched (%s) with %s, %s, which it can "
             "never equal", what, type_name(subject), shown, with_article(type_name(got)));
    if (subject && subject->distinct_name)
        snprintf(help, sizeof(help),
                 "'%s' is a distinct type: write the value as one, `<value> as %s`",
                 subject->distinct_name, subject->distinct_name);
    else
        snprintf(help, sizeof(help),
                 "write a value of type %s here, or convert the value matched with `as`",
                 type_name(subject));
    type_error_code_hint(msg, help, at->line, at->column, AETHER_ERR_TYPE_MISMATCH);
}

/* How a value is shown in a message: a name or a literal as written. */
static const char* selector_shown(ASTNode* v) {
    if (v->type == AST_NULL_LITERAL) return "null";
    if (v->type == AST_IDENTIFIER && v->value)
        return aether_internf("'%s'", v->value);
    if (v->type == AST_LITERAL && v->value && v->node_type &&
        v->node_type->kind == TYPE_STRING)
        return aether_internf("\"%s\"", v->value);
    if (v->type == AST_LITERAL && v->value)
        return aether_internf("%s", v->value);
    return "this pattern";
}

static int selector_is_numeric(Type* t) {
    if (!t) return 1;
    TypeKind k = t->kind;
    return k == TYPE_UNKNOWN || is_numeric_scalar(k) || k == TYPE_BYTE || k == TYPE_ENUM;
}

/* Check one compared value of a selector, `v`, against the value matched,
 * of type `subject` (NULL when unknown). Returns 0 after reporting. */
static int check_selector_value(ASTNode* v, Type* subject, SymbolTable* table,
                                const char* what) {
    if (!v) return 1;
    if (v->type == AST_MATCH_RANGE) {
        if (subject && !selector_is_numeric(subject)) {   /* unknown is numeric */
            char msg[300];
            snprintf(msg, sizeof(msg),
                     "a range %s compares by order, and the value matched is %s, "
                     "which has none", what, with_article(type_name(subject)));
            type_error_code_hint(msg, "name the values one by one, separated by commas",
                                 v->line, v->column, AETHER_ERR_TYPE_MISMATCH);
            return 0;
        }
        int ok = 1;
        for (int i = 0; i < v->child_count && i < 2; i++)
            if (!check_selector_value(v->children[i], subject, table, what)) ok = 0;
        return ok;
    }
    int known = subject && subject->kind != TYPE_UNKNOWN;
    if (v->type == AST_NONE_LITERAL ||
        (v->type == AST_PATTERN_VARIABLE && v->annotation &&
         strcmp(v->annotation, "some_pattern") == 0)) {
        if (!known) return 1;
        char msg[300];
        snprintf(msg, sizeof(msg),
                 "`%s` matches an optional, and the value matched is %s",
                 v->type == AST_NONE_LITERAL ? "none" : "some(...)",
                 with_article(type_name(subject)));
        type_error_code_hint(msg,
                             selector_in_switch(what)
                                 ? "`none` and `some(name)` are `match` arms over a `T?` value"
                                 : "`none` and `some(name)` arms match a `T?` value",
                             v->line, v->column, AETHER_ERR_TYPE_MISMATCH);
        return 0;
    }
    if (v->type == AST_PATTERN_LIST || v->type == AST_PATTERN_CONS) {
        if (!known) return 1;
        char msg[300];
        snprintf(msg, sizeof(msg),
                 "a list pattern matches an array, a slice or a `*StringSeq`, and "
                 "the value matched is %s", with_article(type_name(subject)));
        type_error_code_hint(msg, "match the list itself, or use a value pattern here",
                             v->line, v->column, AETHER_ERR_TYPE_MISMATCH);
        return 0;
    }
    /* A bare member of the enum matched is that member's constant. */
    coerce_bare_enum_member(v, subject, table);
    if (match_arm_is_wildcard(v)) {
        char msg[200];
        snprintf(msg, sizeof(msg),
                 "`_` takes every value, so it cannot be one of the values a %s names", what);
        type_error_code_hint(msg,
                             selector_in_switch(what)
                                 ? "write `default:` for every other value"
                                 : "give `_` an arm of its own, after the others",
                             v->line, v->column, AETHER_ERR_SYNTAX);
        return 0;
    }
    if (v->type == AST_IDENTIFIER && v->value &&
        !(v->node_type && v->node_type->kind == TYPE_ENUM) &&
        !lookup_symbol(table, v->value)) {
        char msg[300];
        snprintf(msg, sizeof(msg),
                 "'%s' names no variable: a %s compares the value matched with what "
                 "it names, and binds no new name", v->value, what);
        type_error_code_hint(msg,
                             selector_in_switch(what)
                                 ? "write `default:` to take every other value, and use the "
                                   "value matched itself in its body"
                                 : "write `_` to take every other value, and use the value "
                                   "matched itself in the arm",
                             v->line, v->column, AETHER_ERR_UNDEFINED_VAR);
        return 0;
    }
    typecheck_expression(v, table);
    Type* got = infer_type(v, table);
    int ok = 1;
    if (got && subject && got->kind != TYPE_UNKNOWN && subject->kind != TYPE_UNKNOWN &&
        !is_type_compatible(got, subject)) {
        selector_mismatch(v, what, selector_shown(v), got, subject);
        ok = 0;
    }
    if (got) free_type(got);
    return ok;
}

/* Can a `switch` compare a value of `t` with its cases? It lowers to a C
 * switch over an integer, or to an if-chain of `==` (string_equals for a
 * string) for any other scalar. */
static int switch_subject_comparable(Type* t) {
    if (!t) return 1;
    switch (t->kind) {
        case TYPE_UNKNOWN: case TYPE_INT: case TYPE_INT64: case TYPE_UINT64:
        case TYPE_UINT32: case TYPE_UINT16: case TYPE_UINT8: case TYPE_BYTE:
        case TYPE_DURATION: case TYPE_FLOAT: case TYPE_FLOAT32: case TYPE_LONGDOUBLE:
        case TYPE_BOOL: case TYPE_STRING: case TYPE_PTR: case TYPE_ENUM:
            return 1;
        default:
            return 0;
    }
}

/* Check every value of selector `sel` against the value matched. */
static int check_selector(ASTNode* sel, Type* subject, SymbolTable* table,
                          const char* what) {
    ASTNode* els[SELECTOR_ELEMENTS_MAX];
    int n = selector_elements(sel, els, SELECTOR_ELEMENTS_MAX);
    int ok = 1;
    for (int i = 0; i < n; i++)
        if (!check_selector_value(els[i], subject, table, what)) ok = 0;
    return ok;
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

/* #2762: a slot of a tuple destructure re-binds a local an enclosing local
 * scope already binds (a closure's `a, b = two()` writes the outer `a` and
 * `b` it sees, as `a = 7` does), so the slot follows the rules a bare
 * re-bind follows in the AST_VARIABLE_DECLARATION case below: a value of
 * another kind is refused with the same diagnostic, so is one an inferred
 * int would truncate (#698), and the local keeps the type of its first
 * binding. Unchecked, the slot's C store did not compile (`const char*`
 * from `int`) or truncated a float into an int cell. A local hoisted out of
 * a branch or loop body, which that case joins over the body first, is left
 * as it was. Returns 0 once it has reported an error. */
static int destructure_slot_rebind(ASTNode* var, SymbolTable* table) {
    if (!var || !var->value || !var->node_type) return 1;
    Symbol* bound = NULL;
    SymbolTable* bound_in = NULL;
    for (SymbolTable* t = table; t && t->parent && !bound; t = t->parent) {
        bound = lookup_symbol_local(t, var->value);
        if (bound) bound_in = t;
    }
    if (!bound && is_program_global_name(var->value)) {
        /* #2800: a module-level optional `var` the slot writes (#2796)
         * keeps its optional type, as a bare re-bind adopts it (#340). */
        Symbol* g = lookup_symbol(table, var->value);
        if (g && g->type && g->type->kind == TYPE_OPTIONAL && !g->is_function &&
            var->node_type->kind != TYPE_OPTIONAL && is_assignable(var->node_type, g->type)) {
            free_type(var->node_type);
            var->node_type = clone_type(g->type);
        }
        return 1;
    }
    if (!bound || !bound->type || (bound->branch_hoisted && bound_in != table) ||
        bound->is_state || bound->is_function || bound->is_actor || bound->is_module_alias)
        return 1;
    Type* have = bound->type;
    Type* got = var->node_type;
    if (have->kind == TYPE_UNKNOWN || got->kind == TYPE_UNKNOWN || got->kind == TYPE_VOID)
        return 1;
    /* A string and a raw pointer convert freely, as for a bare re-bind. */
    int str_ptr_mix = (got->kind == TYPE_PTR && have->kind == TYPE_STRING) ||
                      (got->kind == TYPE_STRING && have->kind == TYPE_PTR);
    if (!str_ptr_mix &&
        (!is_assignable(got, have) ||
         (got->kind == TYPE_PTR && is_numeric_scalar(have->kind)) ||
         (is_numeric_scalar(got->kind) && have->kind == TYPE_PTR))) {
        char rmsg[400];
        snprintf(rmsg, sizeof(rmsg),
            "cannot re-bind '%s' as %s: it was bound as %s by its first "
            "assignment, and a local keeps that type. Use a new name for "
            "the %s value, or convert it to %s",
            var->value, type_name(got), type_name(have), type_name(got), type_name(have));
        type_error(rmsg, var->line, var->column);
        return 0;
    }
    if (bound->type_inferred && have->kind == TYPE_INT &&
        (got->kind == TYPE_INT64 || got->kind == TYPE_UINT64 || got->kind == TYPE_UINT32 ||
         got->kind == TYPE_FLOAT32 || got->kind == TYPE_FLOAT || got->kind == TYPE_LONGDOUBLE)) {
        char msg[420];
        narrowing_message(msg, sizeof(msg), var->value, got->kind);
        type_error(msg, var->line, var->column);
        return 0;
    }
    if (!str_ptr_mix) {
        free_type(var->node_type);
        var->node_type = clone_type(have);
    }
    return 1;
}

/* #2782: a type as a signature spells it, for the return messages: `int?`,
 * `int[]`, `(int, string)`. */
static const char* ret_type_spell(Type* t, char* buf, size_t n) {
    if (!t) return "unknown";
    if (t->distinct_name) return type_name(t);
    if (t->kind == TYPE_OPTIONAL && t->element_type) {
        char inner[160];
        snprintf(buf, n, "%s?", ret_type_spell(t->element_type, inner, sizeof(inner)));
        return buf;
    }
    if (t->kind == TYPE_ARRAY) return array_type_spell(t, buf, n);
    if (t->kind == TYPE_TUPLE) {
        tuple_type_spell(t, buf, n);
        return buf;
    }
    return type_name(t);
}

/* #2782: may a `return` give the value `v`, of type `vt`, where the result
 * is `rt`? The rules of a typed binding, `x: rt = v` (is_assignable), with
 * what a result adds to them and takes from them:
 *
 *  - a `string` and a bare `ptr` are both a C `const char*`: a C string
 *    returned from `-> string` is a borrowed string, and a string returned
 *    from `-> ptr` its pointer (the casting table; a bare re-bind mixes the
 *    two the same way);
 *  - a numeric scalar, `byte` included, converts to another as C converts
 *    it, as into any written slot, and a bool to an integer and back;
 *  - what C cannot do with the value is refused, although is_assignable
 *    lets a binding through: a closure becomes a `ptr` and back only by
 *    name (box_closure, unbox_closure), a pointer is no value optional, a
 *    slice is no typed pointer, and `[]` is the empty slice, which only an
 *    array or a pointer result can take.
 *
 * Returns 1 when it fits. When it does not, `*hint` is the fix to suggest,
 * or NULL for the generic one. */
static int return_value_fits(ASTNode* v, Type* vt, Type* rt, const char** hint) {
    *hint = NULL;
    if (!vt || !rt) return 1;
    if (v && v->type == AST_ARRAY_LITERAL && v->child_count == 0) {
        if (rt->kind == TYPE_ARRAY || rt->kind == TYPE_PTR || rt->kind == TYPE_UNKNOWN ||
            rt->kind == TYPE_WILDCARD)
            return 1;
        *hint = "`[]` is the empty slice; return a value of the result type";
        return 0;
    }
    if (vt->kind == TYPE_UNKNOWN || rt->kind == TYPE_UNKNOWN ||
        vt->kind == TYPE_WILDCARD || rt->kind == TYPE_WILDCARD)
        return 1;
    int plain = !vt->distinct_name && !rt->distinct_name;
    /* A value into an optional result: the optional wraps it, so it is
     * judged against what the optional holds; `none` and an optional are
     * judged whole. */
    if (plain && rt->kind == TYPE_OPTIONAL && vt->kind != TYPE_OPTIONAL && rt->element_type) {
        Type* inner = rt->element_type;
        if (vt->kind == TYPE_PTR && inner->kind != TYPE_PTR && inner->kind != TYPE_STRING &&
            inner->kind != TYPE_UNKNOWN) {
            *hint = "a pointer is not a value of the optional's type: return `none` for no value";
            return 0;
        }
        return return_value_fits(v, vt, inner, hint);
    }
    if (plain && ((vt->kind == TYPE_PTR && !vt->element_type && rt->kind == TYPE_STRING) ||
                  (vt->kind == TYPE_STRING && rt->kind == TYPE_PTR && !rt->element_type)))
        return 1;
    if (plain && (is_numeric_scalar(vt->kind) || vt->kind == TYPE_BYTE) &&
        (is_numeric_scalar(rt->kind) || rt->kind == TYPE_BYTE))
        return 1;
    /* A bool and an integer convert exactly, as C converts them and as a
     * condition reads an integer: a comparison returned from `-> int` is 1
     * or 0 (a derived `T_eq` is one), and an extern's int truth value
     * returned from `-> bool` is whether it is nonzero. */
    if (plain && ((vt->kind == TYPE_BOOL && (is_integer_scalar(rt->kind) || rt->kind == TYPE_BYTE)) ||
                  (rt->kind == TYPE_BOOL && (is_integer_scalar(vt->kind) || vt->kind == TYPE_BYTE))))
        return 1;
    if (vt->kind == TYPE_FUNCTION && !vt->is_fnptr && rt->kind == TYPE_PTR) {
        *hint = "a closure is a `ptr` only boxed: return `box_closure(f)`";
        return 0;
    }
    if (vt->kind == TYPE_PTR && rt->kind == TYPE_FUNCTION && !rt->is_fnptr) {
        *hint = "a closure kept as a `ptr` is a closure again through `unbox_closure(p)`";
        return 0;
    }
    if (type_is_slice(vt) && rt->kind == TYPE_PTR && rt->element_type) return 0;
    /* A slice returned as a pointer is its `.ptr` (slice_coerce_slot), a
     * pointer to its elements, so a typed pointer result takes it only to
     * elements of its own type. */
    if (v && v->type == AST_SLICE_TO_PTR && rt->kind == TYPE_PTR && rt->element_type &&
        rt->element_type->kind != TYPE_UNKNOWN && v->child_count > 0 && v->children[0]) {
        Type* st = v->children[0]->node_type;
        if (type_is_slice(st) && st->element_type &&
            !types_equal(st->element_type, rt->element_type))
            return 0;
    }
    return is_assignable(vt, rt);
}

/* #2782: a value an INFERRED int result would truncate, as an inferred
 * int local would (#698, the casting table): a 64-bit integer, a uint32,
 * a float. */
static int narrows_inferred_int(Type* vt, Type* rt) {
    if (!vt || !rt || rt->kind != TYPE_INT || rt->distinct_name || vt->distinct_name) return 0;
    return vt->kind == TYPE_INT64 || vt->kind == TYPE_UINT64 || vt->kind == TYPE_UINT32 ||
           vt->kind == TYPE_FLOAT32 || vt->kind == TYPE_FLOAT || vt->kind == TYPE_LONGDOUBLE;
}

/* What a `return` returns from, for the messages: a function by name, or
 * a closure. */
typedef struct {
    const char* fn_name;   /* NULL: a closure */
    int inferred;          /* the result type was inferred, not written */
    int position;          /* 1-based value of a multi-value return; 0 for one value */
    Type* whole;           /* with a position: the tuple the function returns */
} ReturnOwner;

/* #2782: check one value a `return` gives against the result type `rt`.
 * A value that branches (an if-expression, a `match` whose value is used)
 * gives each arm's value, checked on its own. Returns 1 when it fits. */
static int check_return_value(ASTNode* v, Type* rt, const ReturnOwner* own,
                              SymbolTable* table) {
    if (!v || !rt) return 1;
    if (v->type == AST_IF_EXPRESSION && v->child_count >= 3) {
        int ok = check_return_value(v->children[1], rt, own, table);
        return check_return_value(v->children[2], rt, own, table) && ok;
    }
    if (v->type == AST_MATCH_STATEMENT) {
        int ok = 1;
        for (int i = 0; i < v->child_count; i++) {
            ASTNode* nested = NULL;
            ASTNode** slot = match_arm_value_slot(v->children[i], &nested);
            if (slot) ok = check_return_value(*slot, rt, own, table) && ok;
            else if (nested) ok = check_return_value(nested, rt, own, table) && ok;
        }
        return ok;
    }
    /* `call(f, ...)` through an erased `fn` returns what the result type
     * says (#2054): the declaration is its annotation, so it fits it. The
     * return case has just typed it so (type_erased_call). */
    if (is_erased_call(v) ||
        (v->type == AST_FUNCTION_CALL && v->annotation &&
         strcmp(v->annotation, "erased_call_typed") == 0))
        return 1;
    /* So does `f(...)` through a bare `fn` value, the same call spelled
     * directly: its signature is gone, and the result type restores it. */
    if (v->type == AST_FUNCTION_CALL && v->value) {
        Symbol* s = lookup_symbol(table, v->value);
        if (s && !s->is_function && s->type && s->type->kind == TYPE_FUNCTION &&
            !s->type->is_fnptr && !s->type->closure_literal && fn_type_is_erased(s->type))
            return 1;
    }
    /* A function named as a value is its address, which no slot checks by
     * its result type. */
    if (v->type == AST_IDENTIFIER && v->value) {
        Symbol* s = lookup_symbol(table, v->value);
        if (s && s->is_function) return 1;
    }
    Type* vt = infer_type(v, table);
    /* A closure literal is a closure: inference leaves it unknown, and
     * its check just stamped it. */
    if (v->type == AST_CLOSURE && (!vt || vt->kind == TYPE_UNKNOWN) && v->node_type) {
        if (vt) free_type(vt);
        vt = clone_type(v->node_type);
    }
    const char* hint = NULL;
    int fits = return_value_fits(v, vt, rt, &hint);
    char want[200], got[200], msg[600], help[400];
    if (fits && own->inferred && narrows_inferred_int(vt, rt)) {
        const char* what = vt->kind == TYPE_UINT32 ? "a uint32, and values past 2^31 would not fit"
                         : (vt->kind == TYPE_INT64 || vt->kind == TYPE_UINT64)
                               ? "a 64-bit value, which would truncate"
                               : "a float, which would truncate";
        if (own->fn_name)
            snprintf(msg, sizeof(msg),
                     "narrowing return from '%s': its result type was inferred as int from "
                     "its first `return`, but this `return` gives %s", own->fn_name, what);
        else
            snprintf(msg, sizeof(msg),
                     "narrowing return from a closure: its result type was inferred as int "
                     "from its first `return`, but this `return` gives %s", what);
        snprintf(help, sizeof(help),
                 "write the result type to keep the value (`-> %s`), or `-> int` to make the "
                 "narrowing explicit", ret_type_spell(vt, got, sizeof(got)));
        type_error_hint(msg, help, v->line, v->column);
        free_type(vt);
        return 0;
    }
    if (fits) {
        if (vt) free_type(vt);
        return 1;
    }
    const char* vs = (v->type == AST_ARRAY_LITERAL && v->child_count == 0)
                         ? "the empty slice `[]`" : ret_type_spell(vt, got, sizeof(got));
    if (own->position) {
        char whole[240];
        snprintf(msg, sizeof(msg),
                 "Type mismatch in return: %s%s%s returns %s, and value %d here is %s, not %s",
                 own->fn_name ? "'" : "", own->fn_name ? own->fn_name : "this closure",
                 own->fn_name ? "'" : "", ret_type_spell(own->whole, whole, sizeof(whole)),
                 own->position, vs, ret_type_spell(rt, want, sizeof(want)));
    } else if (own->fn_name)
        snprintf(msg, sizeof(msg),
                 "Type mismatch in return: '%s' returns %s%s, but this value is %s",
                 own->fn_name, ret_type_spell(rt, want, sizeof(want)),
                 own->inferred ? " (inferred from its first `return`)" : "", vs);
    else
        snprintf(msg, sizeof(msg),
                 "Type mismatch in return: this closure returns %s (the type of its first "
                 "`return`, or string when any `return` gives one), but this value is %s",
                 ret_type_spell(rt, want, sizeof(want)), vs);
    if (!hint) {
        snprintf(help, sizeof(help), "%s",
                 !own->fn_name ? "return values of one type from the closure"
                 : own->inferred ? "return a value of that type, or write the result type the "
                                   "function means (`-> T`)"
                                 : "return a value of the result type, or change the result type");
        hint = help;
    }
    type_error_hint(msg, hint, v->line, v->column);
    if (vt) free_type(vt);
    return 0;
}

/* #2782: the values of the `return` statement `ret` against the result
 * type `rt`: one value, or a multi-value return position by position into
 * a tuple result. A `T!` result wraps what is returned into its own tuple,
 * and is checked where that wrapping is decided. Returns 1 when all fit. */
static int check_return_values(ASTNode* ret, Type* rt, const ReturnOwner* own,
                               SymbolTable* table) {
    if (!ret || !rt || ret->child_count == 0 || rt->kind == TYPE_VOID ||
        rt->kind == TYPE_UNKNOWN || rt->is_result)
        return 1;
    if (ret->child_count > 1) {
        if (rt->kind != TYPE_TUPLE || rt->tuple_count != ret->child_count) return 1;
        int ok = 1;
        for (int i = 0; i < ret->child_count; i++) {
            ReturnOwner at = *own;
            at.position = i + 1;
            at.whole = rt;
            ok = check_return_value(ret->children[i], rt->tuple_types[i], &at, table) && ok;
        }
        return ok;
    }
    ASTNode* v = ret->children[0];
    if (!v || v->type == AST_PRINT_STATEMENT) return 1;
    if (rt->kind == TYPE_TUPLE) {
        /* One value into a tuple result: a tuple of the same arity is
         * judged position by position. */
        Type* vt = infer_type(v, table);
        int ok = 1;
        if (vt && vt->kind == TYPE_TUPLE && vt->tuple_count == rt->tuple_count) {
            for (int i = 0; i < rt->tuple_count && ok; i++) {
                const char* hint = NULL;
                if (!return_value_fits(NULL, vt->tuple_types[i], rt->tuple_types[i], &hint))
                    ok = 0;
            }
            if (!ok) {
                char want[200], got[200], msg[600];
                snprintf(msg, sizeof(msg), "Type mismatch in return: %s%s%s returns %s, but this "
                         "value is %s", own->fn_name ? "'" : "",
                         own->fn_name ? own->fn_name : "this closure", own->fn_name ? "'" : "",
                         ret_type_spell(rt, want, sizeof(want)),
                         ret_type_spell(vt, got, sizeof(got)));
                type_error_hint(msg, "return a value of the result type, or change the result type",
                                v->line, v->column);
            }
            free_type(vt);
            return ok;
        }
        if (vt) free_type(vt);
    }
    return check_return_value(v, rt, own, table);
}

/* #2782: check the `return` statement `ret` against what it returns from
 * (g_tc_return_owner): a function's result type, written or inferred, or,
 * for a closure, the result its first value `return` gives it, as codegen's
 * resolve_closure_return_type picks the C signature (a closure has no
 * written result type). A trailing block's `return` is its function's.
 * Returns 1 when every value fits. */
static int check_return_statement(ASTNode* ret, SymbolTable* table) {
    ASTNode* o = g_tc_return_owner;
    ReturnOwner own = { o ? o->value : NULL, o ? o->type_inferred : 0, 0, NULL };
    Type* rt = g_tc_return_type;
    Type* made = NULL;
    if (!rt && o && o->type == AST_CLOSURE) {
        made = closure_literal_result_type(o);
        rt = made;
        own.fn_name = NULL;
        own.inferred = 1;
    }
    int ok = rt ? check_return_values(ret, rt, &own, table) : 1;
    if (made) free_type(made);
    return ok;
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
            int slots_ok = 1;
            for (int j = 0; j < var_count; j++) {
                ASTNode* var = stmt->children[j];
                if (var->node_type) free_type(var->node_type);
                var->node_type = clone_type(rhs_type->tuple_types[j]);
                // Don't register _ (discard) in symbol table
                if (var->value && strcmp(var->value, "_") != 0) {
                    /* #2762. A refused slot is bound already; the others are
                     * still registered, so their uses report nothing more. */
                    if (!destructure_slot_rebind(var, table)) {
                        slots_ok = 0;
                        continue;
                    }
                    add_symbol(table, var->value, clone_type(var->node_type), 0, 0, 0);
                    /* A slot is an inferred binding, as `a = ...` is: an
                     * int one is a 32-bit int a wider value would
                     * truncate (#698). */
                    if (var->node_type->kind == TYPE_INT) {
                        Symbol* s = lookup_symbol_local(table, var->value);
                        if (s) s->type_inferred = 1;
                    }
                }
            }

            free_type(rhs_type);
            return slots_ok;
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
            /* #2886: a local written as a function type that returns a
             * fixed-size array. */
            if (stmt->type == AST_VARIABLE_DECLARATION && stmt->value) {
                char who[192];
                snprintf(who, sizeof(who), "'%s'", stmt->value);
                refuse_fn_type_array_result(who, stmt->node_type, stmt->line, stmt->column);
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
                    tc_push_value_match(init);   /* #2768 */
                    typecheck_statement(init, table);
                    tc_pop_value_match();
                    check_match_value_arms(init, table);   /* #2743 */
                } else {
                    typecheck_expression(init, table);
                }
                /* #2683: the initializer read the name before the local
                 * existed; from here on it is the local. */
                activate_hoisted_local(table, stmt->value);
                /* #2623: a named function bound to a typed C function
                 * pointer is its address, as it is passed to a parameter of
                 * that type or stored in a field of one (#1240). Read as a
                 * closure value instead, a function returning nothing was
                 * typed `fn(ptr, int)` with no result slot where the
                 * annotation has `void`, so the slot refused the one function
                 * it was declared for; one returning a value passed, and was
                 * emitted as an _AeClosure into the `void*`. Marked as an
                 * address so codegen spells the definition's C name. */
                Type* fnptr_slot = fnptr_binding_slot(stmt, table);
                int fn_address = fnptr_slot &&
                                 fn_name_matches_signature(table, init, fnptr_slot);
                if (fn_address) {
                    set_node_type(init, clone_type(fnptr_slot));
                    if (init->annotation) free(init->annotation);
                    init->annotation = strdup("fn_addr");
                } else if (function_value_annotate(init, table)) {
                    /* A function is a value here, not a call. */
                }
                Type* init_type = fn_address ? clone_type(fnptr_slot)
                                             : infer_type(init, table);
                /* #2628: a closure has an environment the slot has nowhere
                 * to put. Refused before the generic checks below, which
                 * pass a closure of the slot's signature and call another
                 * one a plain mismatch. */
                if (!fn_address &&
                    reject_closure_in_fnptr_slot(table, init, init_type, fnptr_slot,
                                                 FNPTR_SLOT_VARIABLE, stmt->value,
                                                 init->line, init->column)) {
                    /* A declaration still declares its name, with its
                     * annotation, so a call through it is not reported
                     * again as an undefined function. */
                    if (fnptr_slot == stmt->node_type && stmt->value &&
                        !lookup_symbol_local(table, stmt->value))
                        add_symbol(table, stmt->value, clone_type(fnptr_slot), 0, 0, 0);
                    free_type(init_type);
                    return 0;
                }

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
                    /* Only as the var's own struct: an initializer list
                     * is not one for an optional or a sum wrapping it. */
                    Type* dt = stmt->node_type;
                    int struct_typed = !dt || dt->kind == TYPE_UNKNOWN ||
                                       (dt->kind == TYPE_STRUCT && dt->struct_name &&
                                        init->value && strcmp(dt->struct_name, init->value) == 0);
                    int is_struct_init = stmt->annotation &&
                                         strcmp(stmt->annotation, "global_var") == 0 &&
                                         struct_typed && is_const_struct_literal(init, table);
                    /* #2800: an optional `var` is initialised with its own
                     * constant: `none`, or a struct literal of constants
                     * for a struct optional, which codegen wraps
                     * (emit_module_global_vars). */
                    if (!is_struct_init && stmt->annotation &&
                        strcmp(stmt->annotation, "global_var") == 0 && dt &&
                        dt->kind == TYPE_OPTIONAL) {
                        Type* et = dt->element_type;
                        is_struct_init = init->type == AST_NONE_LITERAL ||
                                         (et && et->kind == TYPE_STRUCT && et->struct_name &&
                                          init->value && strcmp(et->struct_name, init->value) == 0 &&
                                          is_const_struct_literal(init, table));
                    }
                    /* #2807: so is an optional `const`'s `none`. */
                    if (!is_struct_init && !stmt->annotation && dt && dt->kind == TYPE_OPTIONAL &&
                        init->type == AST_NONE_LITERAL)
                        is_struct_init = 1;
                    /* #2767: `[]` into a `T[]` global is the empty slice, a
                     * constant as `null` is. */
                    int is_empty_slice = init->type == AST_ARRAY_LITERAL &&
                                         init->child_count == 0 &&
                                         type_is_slice(stmt->node_type);
                    if (!is_array_const && !is_struct_init && !is_empty_slice) {
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

                /* A local has one type, the one its first binding gave it.
                 * A bare re-bind with a value of another kind used to
                 * replace the symbol's type while the hoisted C variable
                 * took one of the two: `x = 5` then `x = "s"` failed in the
                 * C compiler with "assignment to 'const char *' from
                 * 'int'", nothing in the language's terms. Refuse it here,
                 * naming both types and the fix. Widenings and the numeric
                 * conversions is_assignable permits are untouched (the
                 * #698 guards below decide about those). */
                Symbol* bound = NULL;
                if (stmt->type_inferred && stmt->value) {
                    /* Only a binding in an enclosing LOCAL scope is the same
                     * variable. The early inference pass parks every local
                     * it sees in the global table under the LAST type it
                     * saw for the name, so the global fallback would report
                     * `x = 5` as re-binding a string. */
                    SymbolTable* bound_in = NULL;
                    for (SymbolTable* t = table; t && t->parent && !bound; t = t->parent) {
                        bound = lookup_symbol_local(t, stmt->value);
                        if (bound) bound_in = t;
                    }
                    /* #2186: a local hoisted out of a branch or loop body,
                     * bound again inside another one, is judged by
                     * codegen's sibling check, which joins every binding
                     * of the name first. Keep the join here for reads.
                     * The nominal rule is this checker's (#2611): see
                     * sibling_binding_other_type. */
                    if (bound && bound->branch_hoisted && bound_in != table) {
                        /* declare_hoisted_local typed the local before the
                         * block was checked, from what the early inference
                         * pass knew, and that pass leaves a binding such as
                         * an `as *T` view unknown. Codegen types the C
                         * declaration from the first binding once it is
                         * checked; so does the first binding checked here,
                         * joined over the body as declare_hoisted_local
                         * joins (#2611). */
                        if (bound->type && bound->type->kind == TYPE_UNKNOWN && init_type &&
                            init_type->kind != TYPE_UNKNOWN && init_type->kind != TYPE_VOID) {
                            Type* joined = hoist_join_type(g_tc_fn_body, stmt->value, init_type);
                            free_type(bound->type);
                            bound->type = joined ? joined : clone_type(init_type);
                        } else if (bound->type && init_type) {
                            Type* joined = numeric_join_type(bound->type, init_type);
                            if (joined) { free_type(bound->type); bound->type = joined; }
                        }
                        if (sibling_binding_other_type(init_type, bound->type)) {
                            char rmsg[512];
                            snprintf(rmsg, sizeof(rmsg),
                                "cannot bind '%s' as %s: it is bound as %s in another branch "
                                "or loop body of this function, and a local first bound inside "
                                "a branch or loop body is one variable for the whole function. "
                                "Use a new name for the %s value",
                                stmt->value, type_name(init_type), type_name(bound->type),
                                type_name(init_type));
                            type_error(rmsg, stmt->line, stmt->column);
                            free_type(init_type);
                            return 0;
                        }
                        bound = NULL;
                    }
                    /* A binding in a SIBLING block is not looked at: it is
                     * a C variable of its own unless codegen hoists it
                     * (hoist_if_branch_vars, only when the name is used
                     * outside the branches), and `std.cbor` binds the
                     * same name as int in one branch and long in
                     * another. */
                }
                /* Actor state is not a local: `state ref = 0` then
                 * `ref = <ptr>` is the documented wiring pattern and is
                 * typed by is_type_compatible's actor-ref rules. */
                if (bound && bound->type && !bound->is_state &&
                    !bound->is_function && !bound->is_actor && !bound->is_module_alias &&
                    /* `_` is the discard binding: it names no value the code
                     * reads back, so it is never "the same local" across
                     * assignments and its type does not stick. `_ = f()` then
                     * `_ = g()` with a different return type is legal, as it
                     * always was before locals kept their first type. */
                    !(stmt->value && strcmp(stmt->value, "_") == 0) &&
                    bound->type->kind != TYPE_UNKNOWN &&
                    init_type && init_type->kind != TYPE_UNKNOWN &&
                    init_type->kind != TYPE_VOID &&
                    /* A string is a nullable `const char*`: `s = null` and
                     * a raw C string pointer into it stay legal, as they
                     * always were. */
                    !((init_type->kind == TYPE_PTR && bound->type->kind == TYPE_STRING) ||
                      (init_type->kind == TYPE_STRING && bound->type->kind == TYPE_PTR)) &&
                    (!is_assignable(init_type, bound->type) ||
                     /* int <-> ptr is assignable for C interop, but a
                      * number local re-bound to `null` (or a pointer to a
                      * number) is the C compiler's -Wint-conversion error. */
                     (init_type->kind == TYPE_PTR && is_numeric_scalar(bound->type->kind)) ||
                     (is_numeric_scalar(init_type->kind) && bound->type->kind == TYPE_PTR))) {
                    char rmsg[400];
                    snprintf(rmsg, sizeof(rmsg),
                        "cannot re-bind '%s' as %s: it was bound as %s by its first "
                        "assignment, and a local keeps that type. Use a new name for "
                        "the %s value, or convert it to %s",
                        stmt->value, type_name(init_type), type_name(bound->type),
                        type_name(init_type), type_name(bound->type));
                    type_error(rmsg, stmt->line, stmt->column);
                    free_type(init_type);
                    return 0;
                }
                /* #2688: an actor's array state field is a field: it takes
                 * an array of its own length, or a literal no longer. */
                if (bound && bound->is_state && bound->type && bound->type->kind == TYPE_ARRAY &&
                    bound->type->array_size > 0) {
                    char slot[256];
                    snprintf(slot, sizeof(slot), "state field '%s'", stmt->value);
                    if (reject_array_field_source(init, init_type, bound->type, slot,
                                                  stmt->line, stmt->column)) {
                        free_type(init_type);
                        return 0;
                    }
                }
                /* #2516: binding a fixed-size array to an array that already
                 * exists copies its elements, and an array keeps the length
                 * of its first binding, so the two lengths must agree. (An
                 * array literal may be shorter: the rest is zeroed.) */
                if (bound && bound->type && bound->type->kind == TYPE_ARRAY &&
                    bound->type->array_size > 0 && init->type != AST_ARRAY_LITERAL &&
                    init_type && init_type->kind == TYPE_ARRAY && init_type->array_size > 0 &&
                    init_type->array_size != bound->type->array_size) {
                    char want[96], got[96], rmsg[400];
                    snprintf(rmsg, sizeof(rmsg),
                        "'%s' is %s and cannot take %s: an array keeps the length of its "
                        "first binding, and binding another array to it copies the elements",
                        stmt->value, array_type_spell(bound->type, want, sizeof(want)),
                        array_type_spell(init_type, got, sizeof(got)));
                    type_error(rmsg, stmt->line, stmt->column);
                    free_type(init_type);
                    return 0;
                }
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
                                  init_type->kind == TYPE_UINT64 ||
                                  init_type->kind == TYPE_UINT32 ||
                                  init_type->kind == TYPE_FLOAT32 ||
                                  init_type->kind == TYPE_FLOAT ||
                                  init_type->kind == TYPE_LONGDOUBLE)) {
                    const char* nm = stmt->value ? stmt->value : "x";
                    char msg[420];
                    narrowing_message(msg, sizeof(msg), nm, init_type->kind);
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

                /* #2054: `call(f, ...)` through an erased `fn` has no result
                 * type of its own. A typed binding is the annotation that
                 * restores it, so the call node takes the binding's type and
                 * the trampoline is cast to match. An untyped binding gets
                 * the `int` the language has always defaulted to, and hears
                 * about it: for any other result that default is a truncated
                 * pointer, not a wrong type. */
                if (is_erased_call(init)) {
                    if (stmt->node_type && stmt->node_type->kind != TYPE_UNKNOWN) {
                        type_erased_call(init, stmt->node_type, table, 0);
                        if (init_type) free_type(init_type);
                        init_type = clone_type(stmt->node_type);
                    } else {
                        char wmsg[320];
                        snprintf(wmsg, sizeof(wmsg),
                            "the closure called here has no known result type, so '%s' is "
                            "assumed int; declare the binding's type (e.g. `string %s = "
                            "call(...)`) for any other result",
                            stmt->value, stmt->value);
                        type_warning(wmsg, stmt->line, stmt->column);
                    }
                }

                /* The early inference pass typed a bare `x = expr` before
                 * anything imported was resolved and before an array literal
                 * had joined its elements, so its stamp can be the wrong
                 * numeric kind: `xs = [1, 2.5, 3]` as an int array, and
                 * `k = xs[1]` as an int from the same first element (#2119).
                 * An inferred, not annotated, declaration follows the type
                 * its initializer has now, for a numeric scalar or an array
                 * of one. */
                if (stmt->type_inferred && init && init_type && stmt->node_type) {
                    Type* have = stmt->node_type;
                    Type* want = init_type;
                    /* Only the wider kinds the early pass could not see are
                     * adopted; a narrow unsigned initializer keeps the int
                     * binding it has always had. */
                    int scalar_fix = have->kind == TYPE_INT &&
                                     (want->kind == TYPE_FLOAT || want->kind == TYPE_LONGDOUBLE ||
                                      want->kind == TYPE_INT64 || want->kind == TYPE_UINT64);
                    int array_fix = init->type == AST_ARRAY_LITERAL &&
                                    have->kind == TYPE_ARRAY && want->kind == TYPE_ARRAY &&
                                    have->element_type && want->element_type &&
                                    have->element_type->kind != want->element_type->kind;
                    /* #2841: the first binding of a name copies an optional
                     * or a sum the early pass took for what was last put in
                     * it (`o2 = Circle {...}` on a `Shape?` local, then `c2 =
                     * o2`, typed Circle). The value is the wider type, and
                     * the binding is a new local: it takes the value's. */
                    int first_binding = 1;
                    for (SymbolTable* t = table; t && t->parent && first_binding; t = t->parent)
                        if (stmt->value && lookup_symbol_local(t, stmt->value)) first_binding = 0;
                    int wider_fix = first_binding && have->kind != TYPE_UNKNOWN &&
                                    (want->kind == TYPE_OPTIONAL || want->kind == TYPE_SUM) &&
                                    !is_assignable(want, have) && is_assignable(have, want);
                    if (scalar_fix || array_fix || wider_fix) {
                        free_type(stmt->node_type);
                        stmt->node_type = clone_type(init_type);
                    }
                }
                /* A bare re-bind of a local ASSIGNS to it; the local keeps
                 * the type its first binding gave it. The binding used to
                 * take the initializer's type instead, so `f = 1.5` then
                 * `f = 2` went on as an int (`${f}` printed `(int)f`) and
                 * `f = u32` printed the double through `%u`. The value was
                 * checked assignable above; the conversion is C's, as for
                 * an annotated slot. (#869 and #340 did this for explicit
                 * integer widths and optionals; this is the general rule.) */
                if (bound && stmt->type_inferred && bound->type &&
                    bound->type->kind != TYPE_UNKNOWN) {
                    /* A string and a raw pointer are the one case left as
                     * it was: `v = map_get_raw(..)` (ptr) in one block and
                     * `v = lookup(..)` (string) in another share a
                     * `void*`/`const char*` variable C converts freely,
                     * and the binding goes on with the value's type. */
                    int str_ptr_mix =
                        (bound->type->kind == TYPE_STRING && init_type->kind == TYPE_PTR &&
                         init->type != AST_NULL_LITERAL) ||
                        (bound->type->kind == TYPE_PTR && init_type->kind == TYPE_STRING);
                    if (!str_ptr_mix) {
                        if (stmt->node_type) free_type(stmt->node_type);
                        stmt->node_type = clone_type(bound->type);
                        /* `s = null` clears a string (a nullable `const
                         * char*`); the literal takes the string type so
                         * the slot check below sees a string. */
                        if (init->type == AST_NULL_LITERAL && bound->type->kind == TYPE_STRING) {
                            free_type(init_type);
                            init_type = clone_type(bound->type);
                        }
                    }
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
                    if (init_type && type_is_sized_array(init_type) &&
                        init && init->type == AST_IDENTIFIER) {
                        stmt->node_type = create_type(TYPE_PTR);
                    } else {
                        stmt->node_type = clone_type(init_type);
                    }
                } else if (init && init->type == AST_NULL_LITERAL && type_is_slice(stmt->node_type)) {
                    /* #1286: `T[] s = null` is the empty slice. */
                } else if (!array_const_int_narrow && !is_assignable(init_type, stmt->node_type)) {
                    // Has explicit type but initializer doesn't match
                    free_type(init_type);
                    type_error("Type mismatch in variable initialization", stmt->line, stmt->column);
                    return 0;
                }
                /* #1286: `T[N]` / `null` into a `T[]` binding becomes a slice. */
                if (stmt->child_count > 0 && stmt->node_type)
                    slice_coerce_slot(&stmt->children[0], stmt->node_type, 0);
                /* #2785: so does one into a module-level `T[]` var that a
                 * bare `name = value` assigns, with no local of the name in
                 * scope. The binding writes the global, so it has the
                 * global's type, as a re-bind of a local has the local's. */
                if (stmt->child_count > 0) {
                    ASTNode* gv = rebound_global_var(stmt, table);
                    if (gv && type_is_slice(gv->node_type)) {
                        int empty = init->type == AST_NULL_LITERAL ||
                                    (init->type == AST_ARRAY_LITERAL && init->child_count == 0);
                        if (!empty && !is_assignable(init_type, gv->node_type)) {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "Type mismatch in assignment to '%s': expected %s, got %s",
                                     stmt->value, type_name(gv->node_type), type_name(init_type));
                            free_type(init_type);
                            type_error(msg, stmt->line, stmt->column);
                            return 0;
                        }
                        slice_coerce_slot(&stmt->children[0], gv->node_type, 0);
                        if (stmt->node_type) free_type(stmt->node_type);
                        stmt->node_type = clone_type(gv->node_type);
                    }
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
                /* #2460: an erased `fn` binding remembers the closure literal
                 * it holds, so `call(name, ...)` is typed by the literal's
                 * body rather than assumed int. The early inference pass
                 * typed the binding before the literal was checked, so the
                 * mark is (re)made here, and a re-bind keeps it only while
                 * the closures agree on their result (closure_rebind). */
                if (stmt->node_type && stmt->node_type->kind == TYPE_FUNCTION &&
                    !stmt->node_type->is_fnptr && fn_type_is_erased(stmt->node_type)) {
                    ASTNode* lit = closure_literal_of(init, table);
                    /* Only a binding in a local scope is the same variable;
                     * see `bound` above. */
                    Symbol* prior = NULL;
                    for (SymbolTable* t = table; t && t->parent && !prior && stmt->value;
                         t = t->parent) {
                        prior = lookup_symbol_local(t, stmt->value);
                    }
                    if (prior) lit = closure_rebind(prior, lit);
                    stmt->node_type->closure_literal = lit;
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
                mark_const_symbol(table, stmt);   /* #2516 */
                if (table->parent && tc_program_global_var(stmt->value)) {
                    Symbol* gw = lookup_symbol_local(table, stmt->value);
                    if (gw) gw->global_write = 1;
                }
                /* #698: carry the parser's inferred-type marker onto the
                 * binding, but only for a 32-bit int (the sole narrowing
                 * target). A later 64-bit re-bind then triggers the guard
                 * above / in AST_ASSIGNMENT. */
                if (stmt->type_inferred && stmt->node_type &&
                    (stmt->node_type->kind == TYPE_INT ||
                     (stmt->node_type->kind == TYPE_ARRAY && stmt->node_type->element_type &&
                      stmt->node_type->element_type->kind == TYPE_INT &&
                      stmt->child_count > 0 && stmt->children[0] &&
                      stmt->children[0]->type == AST_ARRAY_LITERAL))) {
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
                if (reject_const_element_write(left, table, stmt)) return 0;   /* #2516 */

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
                /* #1286: a `T[N]` array or `null` into a `T[]` binding. */
                int null_into_slice = right->type == AST_NULL_LITERAL &&
                                      type_is_slice(symbol->type);
                if (null_into_slice || is_assignable(right_type, symbol->type))
                    slice_coerce_slot(&stmt->children[1], symbol->type, 0);
                if (!null_into_slice && !is_assignable(right_type, symbol->type)) {
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
                                   right_type->kind == TYPE_UINT64 ||
                                   right_type->kind == TYPE_UINT32 ||
                                   right_type->kind == TYPE_FLOAT32 ||
                                   right_type->kind == TYPE_FLOAT ||
                                   right_type->kind == TYPE_LONGDOUBLE)) {
                    const char* nm = left->value ? left->value : "x";
                    char msg[420];
                    narrowing_message(msg, sizeof(msg), nm, right_type->kind);
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
                /* #2460: a closure variable given another closure keeps its
                 * known literal only if the two agree on their result. */
                closure_rebind(symbol, closure_literal_of(right, table));
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
                Type* rhs_type = infer_type(rhs, table);
                /* The target's type, for codegen (#2428: `v op= x` on an
                 * eight-lane value is a helper call). Always the symbol this
                 * scope resolved: the early pass stamps the function-level
                 * one, which is the wrong variable when a closure parameter
                 * shadows it. */
                if (stmt->node_type) free_type(stmt->node_type);
                stmt->node_type = symbol->type ? clone_type(symbol->type) : create_type(TYPE_UNKNOWN);
                if (rhs_type && (!rhs->node_type || rhs->node_type->kind == TYPE_UNKNOWN)) {
                    set_node_type(rhs, clone_type(rhs_type));
                }
                /* `x op= rhs` is `x = x op rhs` and is checked as that: the
                 * operator must be defined for the operand types (`s += "b"`
                 * on strings used to reach the C compiler as an invalid
                 * pointer addition) and the result must fit `x` — into an
                 * INFERRED int, a float, a 64-bit or a uint32 result is the
                 * #698 narrowing error, exactly as `x = x + rhs` is
                 * (`x = 0` then `x += 2.5` silently stored 2). */
                const char* opstr = stmt->children[0] ? stmt->children[0]->value : NULL;
                if (opstr && symbol->type && symbol->type->kind != TYPE_UNKNOWN &&
                    rhs_type && rhs_type->kind != TYPE_UNKNOWN) {
                    char base[8];
                    snprintf(base, sizeof(base), "%.*s", (int)strlen(opstr) - 1, opstr);
                    AeTokenType op = get_token_type_from_string(base);
                    ASTNode lhs = { 0 };
                    lhs.type = AST_IDENTIFIER;
                    lhs.value = stmt->value;
                    lhs.node_type = symbol->type;
                    lhs.line = stmt->line;
                    lhs.column = stmt->column;
                    Type* result = infer_binary_type(&lhs, rhs, op);
                    int result_ok = result && result->kind != TYPE_UNKNOWN;
                    if (op == TOKEN_PLUS && symbol->type->kind == TYPE_STRING &&
                        rhs_type->kind == TYPE_STRING) {
                        type_error("'+=' is not defined for strings, use \"${a}${b}\" interpolation "
                                   "or string.concat(a, b)", stmt->line, stmt->column);
                        if (result) free_type(result);
                        free_type(rhs_type);
                        return 0;
                    }
                    if (!result_ok) {
                        char emsg[400];
                        if (binary_mixes_distinct(symbol->type, rhs_type, op))
                            snprintf(emsg, sizeof(emsg),
                                     "'%s' mixes distinct types %s and %s: a distinct type does "
                                     "not combine with another one; convert the right side "
                                     "explicitly, `<value> as %s`",
                                     opstr, type_name(symbol->type), type_name(rhs_type),
                                     type_name(symbol->type));
                        else
                            snprintf(emsg, sizeof(emsg),
                                     "'%s' is not defined for %s and %s",
                                     opstr, type_name(symbol->type), type_name(rhs_type));
                        type_error(emsg, stmt->line, stmt->column);
                        if (result) free_type(result);
                        free_type(rhs_type);
                        return 0;
                    }
                    if (symbol->type_inferred && symbol->type->kind == TYPE_INT &&
                        (result->kind == TYPE_INT64 || result->kind == TYPE_UINT64 ||
                         result->kind == TYPE_UINT32 || result->kind == TYPE_FLOAT ||
                         result->kind == TYPE_FLOAT32 ||
                         result->kind == TYPE_LONGDOUBLE)) {
                        char msg[420];
                        narrowing_message(msg, sizeof(msg), stmt->value, result->kind);
                        type_error(msg, stmt->line, stmt->column);
                        free_type(result);
                        free_type(rhs_type);
                        return 0;
                    }
                    if (!is_assignable(result, symbol->type)) {
                        char emsg[300];
                        snprintf(emsg, sizeof(emsg),
                                 "Type mismatch in '%s': the result is %s but '%s' is %s%s",
                                 opstr, type_name(result), stmt->value, type_name(symbol->type),
                                 symbol->type->distinct_name ? " (a distinct type; convert with `as`)" : "");
                        type_error(emsg, stmt->line, stmt->column);
                        free_type(result);
                        free_type(rhs_type);
                        return 0;
                    }
                    free_type(result);
                }
                free_type(rhs_type);
                if (stmt->node_type && stmt->node_type->kind == TYPE_UNKNOWN && symbol->type) {
                    free_type(stmt->node_type);
                    stmt->node_type = clone_type(symbol->type);
                }
            }
            return 1;
        }

        case AST_IF_STATEMENT: {
            /* #2186: names both arms declare are declared before the if,
             * where codegen declares them (hoist_common_vars). */
            declare_common_hoists(stmt, table);
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
            
            // Type check increment (child 2). #2742: a binding increment
            // (`s = f(s)`, `i += 2`) is the statement a body would have.
            if (stmt->child_count > 2 && stmt->children[2]) {
                ASTNode* step = stmt->children[2];
                if (step->type == AST_VARIABLE_DECLARATION ||
                    step->type == AST_COMPOUND_ASSIGNMENT) {
                    typecheck_statement(step, loop_table);
                } else {
                    typecheck_expression(step, loop_table);
                }
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
            /* #2186: the body's locals are declared before the loop, where
             * codegen declares them (hoist_loop_vars). */
            if (stmt->child_count > 1) hoist_loop_decls(stmt->children[1], declare_loop_hoist, table);
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
            /* print's literal is a printf format: a conversion with no
             * argument read whatever the C stack held, so print("100% done")
             * printed garbage and print("a %s") crashed. Each conversion
             * needs an argument; `%%` is a literal percent. */
            if (stmt->child_count >= 1 &&
                stmt->children[0]->type == AST_LITERAL &&
                stmt->children[0]->node_type &&
                stmt->children[0]->node_type->kind == TYPE_STRING &&
                stmt->children[0]->value) {
                const char* fmt = stmt->children[0]->value;
                int flen = ast_literal_length(stmt->children[0]);
                int conversions = 0;
                for (int fi = 0; fi < flen; fi++) {
                    if (fmt[fi] != '%') continue;
                    if (fi + 1 < flen && fmt[fi + 1] == '%') { fi++; continue; }
                    conversions++;
                    if (conversions < stmt->child_count) continue;
                    char ebuf[96];
                    int end = fi + 1;
                    while (end < flen && end < fi + 8 && !((fmt[end] | 0x20) >= 'a' && (fmt[end] | 0x20) <= 'z')) end++;
                    if (end < flen && end < fi + 8) end++;
                    snprintf(ebuf, sizeof(ebuf), "print format '%.*s' has no argument",
                             end - fi, fmt + fi);
                    type_error_hint(ebuf, "write '%%' for a literal '%'",
                                    stmt->children[0]->line, stmt->children[0]->column);
                    return 0;
                }
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
                    /* byte/uint8/uint16 promote to int, so %d is their
                     * conversion and was warned about wrongly; uint32 is
                     * not (%d shows values past 2^31 negative), and the
                     * codegen corrects it to %u, so that one is reported. */
                    if ((spec == 'd' || spec == 'i') && ak != TYPE_INT && ak != TYPE_INT64 && ak != TYPE_BOOL &&
                        ak != TYPE_BYTE && ak != TYPE_UINT8 && ak != TYPE_UINT16) mismatch = 1;
                    if ((spec == 'u') && ak != TYPE_UINT32 && ak != TYPE_UINT16 && ak != TYPE_UINT8 &&
                        ak != TYPE_BYTE && ak != TYPE_INT) mismatch = 1;
                    if ((spec == 'f' || spec == 'g' || spec == 'e') && ak != TYPE_FLOAT && ak != TYPE_FLOAT32 && ak != TYPE_LONGDOUBLE) mismatch = 1;
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
            int value_match = tc_is_value_match(stmt);   /* #2768 */
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

            /* #2768: names every arm binds are declared before the match,
             * where codegen declares them (hoist_common_vars), when an arm
             * runs for every value, which the subject's type now says. Not
             * for a match whose value is used: codegen keeps its arms' names
             * in the arms. */
            if (!value_match) declare_common_hoists(stmt, table);

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

            int opt_match = (match_expr_type && match_expr_type->kind == TYPE_OPTIONAL);

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

                /* #2727: what an arm names must be something the value
                 * matched can be. A `some(name)` arm binds; every other arm
                 * compares the value with what it names. */
                int arm_wild = match_arm_is_wildcard(pattern);
                int some_arm = pattern->type == AST_PATTERN_VARIABLE && pattern->annotation &&
                               strcmp(pattern->annotation, "some_pattern") == 0;
                if (arm_wild) {
                    /* takes every value */
                } else if (opt_match) {
                    if (pattern->type != AST_NONE_LITERAL && !some_arm) {
                        const char* msg =
                            "a match over an optional takes `none`, `some(name)` and `_` "
                            "arms, and this arm is none of them";
                        type_error_code_hint(msg,
                                             "bind the value with `some(v)` and compare `v` "
                                             "inside that arm",
                                             pattern->line, pattern->column,
                                             AETHER_ERR_TYPE_MISMATCH);
                    }
                } else if (sum_match) {
                    if (some_arm || !pattern->value ||
                        (pattern->type != AST_IDENTIFIER &&
                         pattern->type != AST_PATTERN_VARIABLE)) {
                        char msg[300];
                        snprintf(msg, sizeof(msg),
                                 "a match over the sum type '%s' takes one variant per arm, "
                                 "or `_`",
                                 match_expr_type->struct_name ? match_expr_type->struct_name : "?");
                        type_error_code_hint(msg,
                                             "name a variant (`Circle ->`) and read its fields "
                                             "through the value matched inside the arm",
                                             pattern->line, pattern->column,
                                             AETHER_ERR_TYPE_MISMATCH);
                    }
                } else if (pattern->type == AST_PATTERN_LIST || pattern->type == AST_PATTERN_CONS) {
                    if (match_expr_type && match_expr_type->kind != TYPE_UNKNOWN &&
                        match_expr_type->kind != TYPE_ARRAY && !is_seq_match)
                        check_selector_value(pattern, match_expr_type, table, "match arm");
                } else if (!(enum_match && enum_def)) {
                    check_selector(pattern, match_expr_type, table, "match arm");
                    /* Its name is not bound here: say so once, at the arm. */
                    if (some_arm && pattern->value)
                        add_symbol(arm_table, pattern->value, create_type(TYPE_UNKNOWN), 0, 0, 0);
                }

                // #340: optional `match` — bind `some(v)` to the unwrapped
                // value (typed as the optional's inner T), and pin a `none`
                // arm's type to the matched optional so codegen compares the
                // right shape.
                if (match_expr_type && match_expr_type->kind == TYPE_OPTIONAL) {
                    if (some_arm && pattern->value) {
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
                    if (arm_wild) {
                        sum_wildcard = 1;
                        if (sum_wildcard_line < 0) sum_wildcard_line = pattern->line;
                    } else if (!some_arm && (pattern->type == AST_IDENTIFIER ||
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
                    if (arm_wild) {
                        enum_wildcard = 1;
                        if (enum_wildcard_line < 0) enum_wildcard_line = pattern->line;
                    } else {
                        /* #2727: each value of a comma-list (`Red, Green ->`)
                         * is a member as a lone one is; anything else is
                         * checked as the comparison it lowers to. */
                        ASTNode* els[SELECTOR_ELEMENTS_MAX];
                        int nel = selector_elements(pattern, els, SELECTOR_ELEMENTS_MAX);
                        for (int e = 0; e < nel; e++) {
                            ASTNode* el = els[e];
                            int el_some = el->type == AST_PATTERN_VARIABLE && el->annotation &&
                                          strcmp(el->annotation, "some_pattern") == 0;
                            if (el_some || match_arm_is_wildcard(el) || !el->value ||
                                (el->type != AST_IDENTIFIER && el->type != AST_PATTERN_VARIABLE)) {
                                check_selector_value(el, match_expr_type, table, "match arm");
                                continue;
                            }
                            size_t elen = strlen(enum_name);
                            const char* mem = NULL;
                            int bare = 0;
                            if (enum_has_member(enum_name, el->value)) {
                                mem = el->value;                      // bare `Member`
                                bare = 1;
                            } else if (strncmp(el->value, enum_name, elen) == 0 &&
                                       el->value[elen] == '_' &&
                                       enum_has_member(enum_name, el->value + elen + 1)) {
                                mem = el->value + elen + 1;           // lowered `Enum_Member`
                            }
                            if (!mem) {
                                char msg[256];
                                snprintf(msg, sizeof(msg),
                                    "'%s' is not a member of enum '%s'",
                                    el->value, enum_name ? enum_name : "?");
                                type_error(msg, el->line, el->column);
                                continue;
                            }
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
                                free(el->value);
                                el->value = strdup(qualified);
                                if (el->node_type) free_type(el->node_type);
                                el->node_type = clone_type(match_expr_type);
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
                    /* #2768: a match a value match's arm block yields is a
                     * value match too, as codegen emits it. */
                    ASTNode* yields = (value_match && body->type == AST_BLOCK)
                                    ? match_arm_value(body) : NULL;
                    int nested = yields && yields->type == AST_MATCH_STATEMENT;
                    if (nested) tc_push_value_match(yields);
                    typecheck_statement(body, arm_table);
                    if (nested) tc_pop_value_match();
                }

                // Propagate arm result type to the match node (for match-as-expression).
                // #2496: a block arm yields its final value, so its type is that
                // value's; a statement arm yields nothing and says nothing.
                ASTNode* yielded = match_arm_value(body);
                if (!stmt->node_type || stmt->node_type->kind == TYPE_UNKNOWN) {
                    if (yielded && yielded->node_type &&
                        yielded->node_type->kind != TYPE_UNKNOWN) {
                        set_node_type(stmt, clone_type(yielded->node_type));
                    }
                }

                free_symbol_table(arm_table);
            }

            /* #1377, #2728: arms an earlier arm already covers. Checked once
             * the arms are, so a bare enum member is its constant. */
            check_match_arm_reachability(stmt, enum_match || sum_match || opt_match);

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
                        .filename = g_tc_file, .source_code = NULL,
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
                        .filename = g_tc_file, .source_code = NULL,
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

        case AST_SWITCH_STATEMENT: {
            /* #2732: each case is a scope of its own, as an `if` branch is
             * and as codegen emits it; until now the cases were checked in
             * the enclosing scope, so a name first bound in one was taken
             * for a local after the switch and failed in C. The names every
             * case of a switch with a default binds are declared before it,
             * as both arms of an if/else are, and the function-level and
             * loop hoists take a switch's cases as they take an if's arms
             * (hoist.h). */
            declare_common_hoists(stmt, table);
            Type* subject = NULL;
            if (stmt->child_count > 0 && stmt->children[0]) {
                typecheck_expression(stmt->children[0], table);
                subject = infer_type(stmt->children[0], table);
                if (subject && !switch_subject_comparable(subject)) {
                    char msg[300];
                    snprintf(msg, sizeof(msg),
                             "a switch compares its value with each case, and %s "
                             "cannot be compared that way", with_article(type_name(subject)));
                    type_error_code_hint(msg,
                                         "use `match`, which takes `none` and `some(name)` "
                                         "arms, variants and list patterns",
                                         stmt->children[0]->line, stmt->children[0]->column,
                                         AETHER_ERR_TYPE_MISMATCH);
                    free_type(subject);
                    subject = create_type(TYPE_UNKNOWN);
                }
            }
            for (int i = 1; i < stmt->child_count; i++) {
                ASTNode* c = stmt->children[i];
                if (!c || c->type != AST_CASE_STATEMENT) {
                    typecheck_node(c, table);
                    continue;
                }
                int is_default = c->value && strcmp(c->value, "default") == 0;
                int first = is_default ? 0 : 1;
                /* #2727: a case selector against the value it compares. */
                if (!is_default && c->child_count > 0)
                    check_selector(c->children[0], subject, table, "case");
                SymbolTable* case_table = create_symbol_table(table);
                for (int j = first; j < c->child_count; j++)
                    typecheck_node(c->children[j], case_table);
                free_symbol_table(case_table);
            }
            if (subject) free_type(subject);
            return 1;
        }

        case AST_RETURN_STATEMENT:
            // #1044 implicit enum selector: `return Blue` in a function whose
            // return type is an enum resolves the bare member against it.
            //
            // #2337: a returned value is an EXPRESSION and is checked as one,
            // exactly as a declaration's initializer is. Walking it through
            // typecheck_node (the statement path) only visited its children,
            // so the expression rules never ran on anything returned: `return
            // (s as int) & 255` with a string `s` compiled, while the same
            // cast assigned to a local was refused. Match-as-expression is
            // the one value form the statement path owns.
            for (int i = 0; i < stmt->child_count; i++) {
                coerce_bare_enum_member(stmt->children[i], g_tc_return_type, table);
                if (stmt->children[i] && stmt->children[i]->type == AST_MATCH_STATEMENT) {
                    tc_push_value_match(stmt->children[i]);   /* #2768 */
                    typecheck_node(stmt->children[i], table);
                    tc_pop_value_match();
                } else
                    typecheck_expression(stmt->children[i], table);
            }
            /* #2697, #2733: a return of something with no value, a call of
             * a function that returns nothing or a `match` whose arms give
             * none, is that statement. */
            if (stmt->child_count == 1 && stmt->children[0] &&
                (stmt->children[0]->type == AST_FUNCTION_CALL ||
                 stmt->children[0]->type == AST_MATCH_STATEMENT)) {
                int settled = tc_return_settle(stmt, table);
                if (settled >= 0) return settled;
                /* #2743: a `match` returned for its value takes it from
                 * every arm. */
                if (stmt->children[0]->type == AST_MATCH_STATEMENT &&
                    check_match_value_arms(stmt->children[0], table) > 0)
                    return 0;
            }
            /* #1286: `return arr` from a `-> T[]` function returns a slice. */
            if (stmt->child_count == 1 && g_tc_return_type)
                slice_coerce_slot(&stmt->children[0], g_tc_return_type, 0);
            /* #2628: a closure returned as a typed fn pointer (`-> Getter`
             * for a `cfn Getter`). A closure's own `return` is not this
             * one: g_tc_return_type is cleared inside its body. */
            if (stmt->child_count == 1 && stmt->children[0] && g_tc_return_type &&
                g_tc_return_type->kind == TYPE_FUNCTION && g_tc_return_type->is_fnptr) {
                ASTNode* rv = stmt->children[0];
                Type* rvt = infer_type(rv, table);
                int refused = reject_closure_in_fnptr_slot(table, rv, rvt, g_tc_return_type,
                                                           FNPTR_SLOT_RESULT, NULL,
                                                           rv->line, rv->column);
                free_type(rvt);
                if (refused) return 0;
            }
            /* A multi-value return into a tuple type coerces each value
             * against its own position: `return null, "e"` from a
             * `-> (long[], string)` function is the empty slice and a
             * string, not a NULL written over the slice's fields. */
            if (stmt->child_count > 1 && g_tc_return_type &&
                g_tc_return_type->kind == TYPE_TUPLE &&
                g_tc_return_type->tuple_count == stmt->child_count) {
                for (int i = 0; i < stmt->child_count; i++)
                    slice_coerce_slot(&stmt->children[i], g_tc_return_type->tuple_types[i], 0);
            }
            /* #2054: `return call(f, ...)` through an erased fn takes the
             * function's declared result type, as a typed binding would;
             * the declaration is the annotation. */
            if (stmt->child_count == 1 && is_erased_call(stmt->children[0]) &&
                g_tc_return_type && !g_tc_return_type->is_result &&
                g_tc_return_type->kind != TYPE_VOID &&
                g_tc_return_type->kind != TYPE_UNKNOWN &&
                g_tc_return_type->kind != TYPE_TUPLE) {
                type_erased_call(stmt->children[0], g_tc_return_type, table, 0);
            }
            /* #2782: what is returned fits the result type, as a typed
             * binding's value fits its annotation. Unchecked, every
             * mismatch reached C: an int from `-> string`, a struct of
             * another name. */
            if (!check_return_statement(stmt, table)) return 0;
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
                if (expr->value && (strcmp(expr->value, "++") == 0 ||
                                    strcmp(expr->value, "--") == 0) &&
                    reject_const_element_write(expr->children[0], table, expr))   /* #2516 */
                    return 0;
                /* `~` flips bits, which a float lane does not have in any
                 * sense C accepts: it reached the C compiler as an invalid
                 * operand. Integer lanes (masks) keep it. */
                Type* ot = expr->children[0]->node_type;
                if (expr->value && strcmp(expr->value, "!") == 0 && ot && is_lane_type(ot->kind)) {
                    char msg[200];
                    snprintf(msg, sizeof(msg),
                             "'!' is not defined on %s: a mask is not a bool. Reduce it with "
                             "lanes.any / lanes.all, or flip its lanes with lanes.mask_not",
                             type_name_of_kind(ot->kind));
                    type_error(msg, expr->line, expr->column);
                    return 0;
                }
                if (expr->value && strcmp(expr->value, "~") == 0 && ot && is_lane_type(ot->kind) &&
                    (lane_scalar_kind(ot->kind) == TYPE_FLOAT32 || lane_scalar_kind(ot->kind) == TYPE_FLOAT)) {
                    char msg[160];
                    snprintf(msg, sizeof(msg), "'~' is not defined on %s: it has float lanes",
                             type_name_of_kind(ot->kind));
                    type_error(msg, expr->line, expr->column);
                    return 0;
                }
                /* #2591: `&` takes the address of storage, and a call's
                 * result has none: it reached the C compiler as "lvalue
                 * required as unary '&' operand". Storage for it would end
                 * with the statement or block that made it, so a pointer
                 * kept past that dangles; a local is explicit about how
                 * long it lives. `ref_get(r)` lowers to the cell itself, so
                 * it has an address. */
                if (expr->value && strcmp(expr->value, "&") == 0 &&
                    expr->children[0]->type == AST_FUNCTION_CALL &&
                    !(expr->children[0]->value &&
                      strcmp(expr->children[0]->value, "ref_get") == 0)) {
                    type_error("`&` needs a value stored somewhere, and a call's result is not: "
                               "bind it to a local first (`v = f()`, then `&v`)",
                               expr->line, expr->column);
                    return 0;
                }
                /* Through set_node_type: inference typed this node first, and
                 * a plain assignment orphaned that type (#1575), one per unary
                 * expression in every function checked. */
                set_node_type(expr, infer_unary_type(expr->children[0],
                                                     get_token_type_from_string(expr->value)));
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
                    /* #2614: `low` of a `low.CONST` this scope cannot see. */
                    char hint[512];
                    if (missing_import_hint(expr->value, table, hint, sizeof(hint))) {
                        type_error_code_hint(error_msg, hint, expr->line, expr->column,
                                             AETHER_ERR_UNDEFINED_VAR);
                        return 0;
                    }
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

        case AST_SCHEMA_OF:
            // #2298: synthesized by the derive pass, never written; the
            // address of a struct's field table.
            set_node_type(expr, create_type(TYPE_PTR));
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
            for (int i = 1; i < expr->child_count && i < 3; i++) {
                function_value_annotate(expr->children[i], table);
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

        case AST_SLICE_FROM_ARRAY:
        case AST_SLICE_TO_PTR:
            if (expr->child_count > 0) typecheck_expression(expr->children[0], table);
            return 1;

        case AST_SLICE_EXPR: {
            /* #1286: base must be a slice or array; each bound an integer. */
            for (int i = 0; i < expr->child_count; i++)
                typecheck_expression(expr->children[i], table);
            for (int i = 1; i < expr->child_count; i++) {
                Type* bt = infer_type(expr->children[i], table);
                if (bt && bt->kind != TYPE_INT && bt->kind != TYPE_INT64 &&
                    bt->kind != TYPE_UNKNOWN) {
                    char msg[160];
                    snprintf(msg, sizeof(msg),
                             "slice bound must be an integer, got %s", type_name(bt));
                    type_error(msg, expr->children[i]->line, expr->children[i]->column);
                }
                if (bt) free_type(bt);
            }
            Type* t = infer_type(expr, table);
            set_node_type(expr, t);
            return 1;
        }

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
            /* #2304: `s as ptr` on a slice is its element pointer, the
             * coercion a slice gets in any raw-pointer slot. */
            if (expr->child_count > 0 && expr->node_type &&
                expr->node_type->kind == TYPE_PTR &&
                type_is_slice(expr->children[0]->node_type)) {
                slice_coerce_to_ptr(&expr->children[0]);
            }
            Type* operand = expr->child_count > 0 ? infer_type(expr->children[0], table) : NULL;
            if (operand && expr->node_type) {
                int same = (operand->kind == expr->node_type->kind);
                /* #2337: `byte` is an unsigned 8-bit integer (`unsigned char`,
                 * the same C type as uint8), so `b as int` widens it and
                 * `n as byte` keeps the low 8 bits, as for uint8. It is not in
                 * is_numeric_scalar, whose other callers type arithmetic, where
                 * a byte has its own rules. */
                int numeric = (is_numeric_scalar(operand->kind) || operand->kind == TYPE_BYTE) &&
                              (is_numeric_scalar(expr->node_type->kind) ||
                               expr->node_type->kind == TYPE_BYTE);
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
                /* #2634: a typed fn pointer is a C pointer (its storage is a
                 * `void*`), so `f as ptr` is the pointer it holds, the way
                 * back from `p as fn(...)`. A closure is not one: it carries
                 * an environment. */
                int fnptr_to_ptr = operand->kind == TYPE_FUNCTION && operand->is_fnptr &&
                                   expr->node_type->kind == TYPE_PTR &&
                                   !expr->node_type->element_type;
                if (!same && !numeric && !bs_cast && !fnptr_to_ptr) {
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

        case AST_ARRAY_LITERAL: {
            /* Arrays are one-dimensional. A nested literal was typed as an
             * int array of its rows and emitted as `int grid[2] = {{...},
             * {...}}` — C warnings about braces around a scalar, then
             * "subscripted value is neither array nor pointer" at the
             * first `grid[i][j]`, none of it in the language's terms. */
            for (int i = 0; i < expr->child_count; i++) {
                ASTNode* el = expr->children[i];
                if (el && el->type == AST_ARRAY_LITERAL) {
                    type_error("an array literal cannot hold an array literal: arrays are "
                               "one-dimensional. Use a flat `T[rows * cols]` indexed as "
                               "`row * cols + col`, or a list of arrays",
                               el->line, el->column);
                    return 0;
                }
            }
            // Type check all array elements
            for (int i = 0; i < expr->child_count; i++) {
                typecheck_expression(expr->children[i], table);
            }
            /* The element type was chosen by the early inference pass from
             * the first element's type BEFORE this pass could resolve
             * anything imported: `[0.18 * math.PI, ...]` came out as an
             * int array (the product's type was unknown then, and unknown
             * lowers to int) and every element was truncated with no
             * diagnostic (#2119). Now that the elements are typed, the
             * array's element type is the numeric join of theirs — float
             * if any element is float — so a literal of float expressions
             * is a `double[]` whatever its first element looked like
             * early on. Non-numeric element kinds keep the early choice. */
            Type* join = NULL;
            for (int i = 0; i < expr->child_count; i++) {
                Type* et = expr->children[i] ? expr->children[i]->node_type : NULL;
                if (!et || !is_numeric_scalar(et->kind)) { join = NULL; break; }
                if (!join) { join = et; continue; }
                if (et->kind == TYPE_FLOAT || et->kind == TYPE_LONGDOUBLE) {
                    if (join->kind != TYPE_LONGDOUBLE) join = et;
                } else if (is_integer_scalar(join->kind)) {
                    /* Among integers only the 64-bit kinds widen; a
                     * uint32/uint16/byte-led literal keeps its kind (the
                     * `uint32 x = 4000000000; [x, 0]` array stays uint32). */
                    if (et->kind == TYPE_UINT64 ||
                        (et->kind == TYPE_INT64 && join->kind != TYPE_UINT64)) join = et;
                }
            }
            if (join) {
                Type* have = expr->node_type;
                if (!have || have->kind != TYPE_ARRAY || !have->element_type ||
                    have->element_type->kind != join->kind) {
                    set_node_type(expr, create_array_type(clone_type(join), expr->child_count));
                }
            }
            return 1;
        }

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

        case AST_QUOTE: {
            /* docs/quote.md: the body is checked as a closure's, then
             * lowered to the tree it stands for (analysis/quote.c) and taken
             * off the node, so nothing after this reads the quoted
             * statements as code to run. A quote met again (a call checked
             * twice) is already lowered. */
            Type* tree_type = create_type(TYPE_PTR);
            tree_type->distinct_name = strdup("Tree");
            if (expr->node_type) free_type(expr->node_type);
            expr->node_type = tree_type;
            if (expr->value) return 1;

            SymbolTable* quote_scope = create_symbol_table(table);
            int param_errors = 0;
            for (int i = 0; i < expr->child_count; i++) {
                ASTNode* child = expr->children[i];
                if (!child || child->type != AST_CLOSURE_PARAM || !child->value) continue;
                if (!child->node_type) {
                    char msg[256], hint[256];
                    snprintf(msg, sizeof(msg), "quote parameter '%s' has no type", child->value);
                    snprintf(hint, sizeof(hint),
                             "write its type, `| %s: float |`: the tree records every node's type, "
                             "and a quote does not guess one", child->value);
                    type_error_code_hint(msg, hint, child->line, child->column,
                                         AETHER_ERR_QUOTE_PARAM);
                    param_errors++;
                    /* Still a name in the body, so its uses are not each
                     * reported again as undefined. */
                    add_symbol(quote_scope, child->value, create_type(TYPE_UNKNOWN), 0, 0, 0);
                    continue;
                }
                add_symbol(quote_scope, child->value, clone_type(child->node_type), 0, 0, 0);
            }
            Type* enclosing_ret = g_tc_return_type;
            ASTNode* enclosing_owner = g_tc_return_owner;
            g_tc_return_type = NULL;
            g_tc_return_owner = expr;
            int before = error_count;
            for (int i = 0; i < expr->child_count; i++) {
                ASTNode* child = expr->children[i];
                if (child && child->type == AST_BLOCK) {
                    for (int j = 0; j < child->child_count; j++)
                        typecheck_statement(child->children[j], quote_scope);
                }
            }
            g_tc_return_type = enclosing_ret;
            g_tc_return_owner = enclosing_owner;
            free_symbol_table(quote_scope);
            if (param_errors || error_count != before) return 0;

            const char* qfile = expr->source_file ? expr->source_file : g_tc_file;
            if (quote_lower(expr, table, qfile, quote_report_error) != 0) return 0;
            /* Lowered: the body is the tree's now. */
            for (int i = 0; i < expr->child_count; i++) {
                if (expr->children[i]) free_ast_node(expr->children[i]);
            }
            expr->child_count = 0;
            return 1;
        }

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
                    char who[192];
                    snprintf(who, sizeof(who), "closure parameter '%s'", child->value);
                    refuse_fn_type_array_result(who, child->node_type, child->line, child->column);
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
            /* A `return` inside the body returns from the closure, not from
             * the enclosing function, so the function's return type must not
             * reach it: it would coerce a bare enum member against the wrong
             * type, and stamp an erased `call` in return position (#2054)
             * with a type that belongs to the outer function. */
            Type* enclosing_ret = g_tc_return_type;
            ASTNode* enclosing_owner = g_tc_return_owner;
            /* A call's trailing block runs inline: a `return` in it returns
             * from the function it is written in, and is checked against
             * that function's result type (#2766). */
            if (ast_is_own_function(expr)) {
                g_tc_return_type = NULL;
                g_tc_return_owner = expr;
            }
            for (int i = 0; i < expr->child_count; i++) {
                ASTNode* child = expr->children[i];
                if (child && child->type == AST_BLOCK) {
                    for (int j = 0; j < child->child_count; j++) {
                        typecheck_statement(child->children[j], closure_scope);
                    }
                }
            }
            g_tc_return_type = enclosing_ret;
            g_tc_return_owner = enclosing_owner;

            /* #2886: what a closure of its own returns (a call's trailing
             * block returns from its enclosing function, checked there). */
            if (ast_is_own_function(expr)) {
                for (int i = 0; i < expr->child_count; i++) {
                    ASTNode* body = expr->children[i];
                    if (!body || body->type != AST_BLOCK) continue;
                    ASTNode* at = NULL;
                    Type* arr = closure_array_return(body, &at);
                    if (arr && at) {
                        ASTNode* v = NULL;
                        for (int k = 0; k < at->child_count && !v; k++)
                            if (at->children[k] && result_array_type(at->children[k]->node_type))
                                v = at->children[k];
                        Type* result = at->child_count == 1 ? at->children[0]->node_type : NULL;
                        refuse_array_result(result ? "this closure returns"
                                                   : "this closure returns, among its values,",
                                            result ? result : arr, arr,
                                            v ? v->line : at->line, v ? v->column : at->column);
                    }
                }
            }

            free_symbol_table(closure_scope);

            if (!expr->node_type) {
                expr->node_type = create_type(TYPE_FUNCTION);
            }
            /* #2460: the literal's type stays the erased `fn` (it flows into
             * any `fn` slot), but it records which literal it is, so a call
             * through a binding, an alias, or a closure that returns it takes
             * the result type its body gives. */
            if (expr->node_type->kind == TYPE_FUNCTION && !expr->node_type->is_fnptr &&
                fn_type_is_erased(expr->node_type)) {
                expr->node_type->closure_literal = expr;
            }
            /* #2484: a closure that returns an erased call has no result
             * type until a typed use gives it one. */
            {
                ASTNode* body = NULL;
                for (int i = expr->child_count - 1; i >= 0; i--) {
                    if (expr->children[i] && expr->children[i]->type == AST_BLOCK) {
                        body = expr->children[i];
                        break;
                    }
                }
                if (is_erased_call(closure_first_return_expr(body)) &&
                    closure_first_return_stmt(body)->child_count == 1)
                    note_erased_result_closure(expr);
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

                // Propagate element type from the array expression. The
                // array's element type is authoritative: the early inference
                // pass may have stamped this access from the array's FIRST
                // element (`m[1]` of `[1, 2.5, 3]` as int), and a stale stamp
                // reached printf as `%d` on a double slot (#2119).
                if (arr_type && arr_type->kind == TYPE_ARRAY && arr_type->element_type &&
                    (!expr->node_type || expr->node_type->kind == TYPE_UNKNOWN ||
                     expr->node_type->kind != arr_type->element_type->kind)) {
                    if (expr->node_type) free_type(expr->node_type);
                    expr->node_type = clone_type(arr_type->element_type);
                }

                // Indexing a bare `ptr` (void*, element_type == NULL) has no
                // element type: it lowers to a C index on `void*` and leaks
                // `error: void value not ignored as it ought to be` from the C
                // compiler. A typed pointer (`*T`, element_type populated),
                // an array, and a string all index legitimately and are left
                // alone. The packed-buffer handles (std.intarr / floatarr /
                // longarr) are bare `ptr`, so `a[i]` on one lands here.
                //
                // The advice names the VIEW first (#2041). `[]` cannot work
                // on the handle -- that is what this diagnostic is about --
                // but it works on a typed view over the same buffer, so the
                // reader who wrote `a[i]` can have very nearly what they
                // wrote rather than being sent to a function call. The
                // accessors follow, because they are still the right answer
                // for a single access.
                if (arr_type && arr_type->kind == TYPE_PTR && !arr_type->element_type) {
                    type_error("`[]` indexing is not defined for `ptr`; a bare "
                               "pointer has no element type. For std.intarr / "
                               "floatarr / longarr take a typed view and index "
                               "that: `v = intarr.intarr_array(a)` then `v[i]`. "
                               "For a single access the accessor is simpler: "
                               "`intarr.intarr_get_unchecked(a, i)` / "
                               "`intarr.intarr_set_unchecked(a, i, v)`",
                               expr->line, expr->column);
                }
            }
            return 1;

        case AST_STRUCT_LITERAL: {
            // Type check struct literal field initializers
            Symbol* sdef_sym = expr->value ? lookup_symbol(table, expr->value) : NULL;
            ASTNode* sdef = (sdef_sym && sdef_sym->node &&
                             sdef_sym->node->type == AST_STRUCT_DEFINITION)
                            ? sdef_sym->node : NULL;
            int sdef_is_extern = sdef && sdef->annotation &&
                                 strncmp(sdef->annotation, "extern", 6) == 0;
            int fields_ok = 1;
            for (int i = 0; i < expr->child_count; i++) {
                ASTNode* field_init = expr->children[i];
                if (field_init && field_init->type == AST_ASSIGNMENT && field_init->child_count > 0) {
                    typecheck_expression(field_init->children[0], table);
                    /* #1286: an array into a `T[]` field becomes a slice; an
                     * `extern struct` field is C's `T*`, so a slice decays. */
                    if (sdef && field_init->value) {
                        for (int f = 0; f < sdef->child_count; f++) {
                            ASTNode* fd = sdef->children[f];
                            if (fd && fd->type == AST_STRUCT_FIELD && fd->value &&
                                strcmp(fd->value, field_init->value) == 0) {
                                slice_coerce_slot(&field_init->children[0], fd->node_type,
                                                  sdef_is_extern);
                                /* #2628: a closure in a typed fn-pointer field. */
                                ASTNode* fv = field_init->children[0];
                                /* #2688: an array field takes an array of its
                                 * own length. */
                                if (fv && fd->node_type && fd->node_type->kind == TYPE_ARRAY &&
                                    fd->node_type->array_size > 0) {
                                    Type* fvt = infer_type(fv, table);
                                    char slot[256];
                                    snprintf(slot, sizeof(slot), "field '%s.%s'",
                                             sdef->value ? sdef->value : "?", fd->value);
                                    if (reject_array_field_source(fv, fvt, fd->node_type, slot,
                                                                  fv->line, fv->column))
                                        fields_ok = 0;
                                    free_type(fvt);
                                }
                                if (fv && fd->node_type && fd->node_type->kind == TYPE_FUNCTION &&
                                    fd->node_type->is_fnptr) {
                                    Type* fvt = infer_type(fv, table);
                                    char slot[256];
                                    snprintf(slot, sizeof(slot), "%s.%s",
                                             sdef->value ? sdef->value : "?", fd->value);
                                    if (reject_closure_in_fnptr_slot(table, fv, fvt, fd->node_type,
                                                                     FNPTR_SLOT_FIELD, slot,
                                                                     fv->line, fv->column))
                                        fields_ok = 0;
                                    free_type(fvt);
                                }
                                break;
                            }
                        }
                    }
                }
            }
            // Struct literal type is already set during type inference
            return fields_ok;
        }
            
        case AST_MEMBER_ACCESS: {
            /* #2146: a lane read. This walk always runs, so it is where the
             * node's type is pinned (an interpolation reads node_type to pick
             * its format) and where `.z` on a two-lane vector is refused —
             * otherwise it reached the C as a member of a non-struct. */
            if (expr->child_count > 0 && expr->children[0] && expr->value) {
                /* infer_type only: the object may be a module NAMESPACE
                 * (`fs.read`), which is not a value and must not be
                 * type-checked as one — doing so reported every qualified
                 * call as an undefined variable. */
                Type* lt = infer_type(expr->children[0], table);
                if (lt && lt->kind == TYPE_ARRAY && strcmp(expr->value, "len") == 0) {
                    /* #1286 `s.len` */
                    free_type(lt);
                    typecheck_expression(expr->children[0], table);
                    set_node_type(expr, create_type(TYPE_INT));
                    return 1;
                }
                if (lt && is_lane_type(lt->kind)) {
                    int idx = lane_accessor_index(lt->kind, expr->value);
                    TypeKind elem = lane_scalar_kind(lt->kind);
                    TypeKind objk = lt->kind;
                    free_type(lt);
                    if (idx < 0) {
                        char msg[200];
                        snprintf(msg, sizeof(msg),
                                 "'%s' is not a lane of %s: its lanes are %s",
                                 expr->value, type_name_of_kind(objk),
                                 objk == TYPE_F64X2 ? "`.x` and `.y`"
                                 : (objk == TYPE_F32X8 || objk == TYPE_I32X8)
                                     ? "`.x`, `.y`, `.z` and `.w` for lanes 0-3, and `lanes.lane8` for any of the eight"
                                     : "`.x`, `.y`, `.z` and `.w`");
                        type_error(msg, expr->line, expr->column);
                        return 0;
                    }
                    /* Pin the object's type too: codegen reads it to emit the
                     * lane read, and a local bound from a unary expression
                     * (`inv = ~m`, `n = -v`) is not typed by the early
                     * inference pass, so `inv.x` reached the C as a member
                     * access on a vector. */
                    ASTNode* obj = expr->children[0];
                    if (!obj->node_type || obj->node_type->kind == TYPE_UNKNOWN)
                        set_node_type(obj, create_type(objk));
                    set_node_type(expr, create_type(elem));
                    return 1;
                }
                if (lt) free_type(lt);
            }
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
                    AetherModule* hub = module_find_by_namespace(expr->children[0]->value);
                    AetherModule* origin = module_resolve_reexport(hub, expr->value);
                    if (origin && origin->name) {
                        snprintf(qualified, sizeof(qualified), "%s_%s",
                                 module_namespace_of(origin->name), expr->value);
                        sym = lookup_symbol(table, qualified);
                    }
                }
                /* #2631: the checked module file's own constant, which a
                 * build merges as `<ns>_<name>`: the bare one it carries. */
                if ((!sym || !sym->type) && !table->merged_from &&
                    entry_self_reference(expr->children[0]->value, expr->value)) {
                    sym = entry_self_symbol(table, expr->value);
                    snprintf(qualified, sizeof(qualified), "%s", expr->value);
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

                /* An optional holds its value or none: a field is read
                 * through `?.`, after `!`, or where a none-check narrows
                 * it. Read plainly, it reached C as a field of the
                 * `ae_opt_<T>` wrapper (gcc: "'ae_opt_Shape' has no member
                 * named 'r'", #2841 review). */
                if (base_type && base_type->kind == TYPE_OPTIONAL && expr->value) {
                    char error_msg[400];
                    char tn[160];
                    snprintf(error_msg, sizeof(error_msg),
                             "Type '%s' has no field '%s': it is an optional, read the field "
                             "with `?.` or unwrap it first (`!`, or a none-check that "
                             "narrows it)",
                             ret_type_spell(base_type, tn, sizeof(tn)), expr->value);
                    free_type(base_type);
                    type_error(error_msg, expr->line, expr->column);
                    return 0;
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
                        set_node_type(expr, infer_type(expr, table));
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
                        set_node_type(expr, infer_type(expr, table));
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
                        set_node_type(expr, infer_type(expr, table));
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

/* #2481: the call whose result an assignment target writes into, or NULL.
 * A call's value is a temporary: `copy(q) = v`, `copy(q).x = v` and
 * `make_arr(q)[i] = v` (a by-value array) assign to something that no
 * longer exists, and the C compiler stopped at "lvalue required" against a
 * line of generated code. A field or element reached through a pointer is
 * real storage however the pointer was obtained (`get_ptr(q).x = v` with a
 * `*T` result, `xs(q)[i] = v` over a slice), so the walk stops there. A
 * call of unknown type inside the target is left alone: it may return a
 * pointer. */
static ASTNode* assign_target_temporary(ASTNode* target, SymbolTable* table, int top) {
    if (!target) return NULL;
    if (target->type == AST_FUNCTION_CALL) {
        if (top) return target;   /* no call result is assignable */
        Type* t = infer_type(target, table);
        int temp = t && t->kind != TYPE_UNKNOWN && t->kind != TYPE_PTR;
        if (t) free_type(t);
        return temp ? target : NULL;
    }
    if ((target->type != AST_MEMBER_ACCESS && target->type != AST_ARRAY_ACCESS) ||
        target->child_count < 1)
        return NULL;
    ASTNode* base = target->children[0];
    Type* bt = infer_type(base, table);
    int through_value = bt && (target->type == AST_MEMBER_ACCESS
                                   ? bt->kind == TYPE_STRUCT
                                   : type_is_sized_array(bt));
    if (bt) free_type(bt);
    return through_value ? assign_target_temporary(base, table, 0) : NULL;
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

    /* #1286: a slice meets a raw pointer. `s == null`, `s != p`, `s + n`
     * and `s - n` all act on the slice's pointer (`s + n` scales by the
     * element size, as the `T*` it used to be did); `a.buf = arr` / `x[i] =
     * arr` into a `T[]` slot takes a slice. The wrapped child is
     * re-read below so the rest of this function sees the new shape. */
    {
        int l_arr = left_type && left_type->kind == TYPE_ARRAY;
        int r_arr = right_type && right_type->kind == TYPE_ARRAY;
        int l_ptrish = left->type == AST_NULL_LITERAL || (left_type && left_type->kind == TYPE_PTR);
        int r_ptrish = right->type == AST_NULL_LITERAL || (right_type && right_type->kind == TYPE_PTR);
        int l_int = left_type && (left_type->kind == TYPE_INT || left_type->kind == TYPE_INT64);
        int r_int = right_type && (right_type->kind == TYPE_INT || right_type->kind == TYPE_INT64);
        int changed = 0;
        /* slice_coerce_to_ptr reads the operand node's own type; a parameter
         * identifier (`fn f(a: byte[]) { a == null }`) can reach here with
         * that unset even though infer_type knows it is a slice, so the
         * comparison was emitted as `AetherSlice == NULL` and failed in C.
         * Record what infer_type found before coercing (#2301). */
        if (l_arr && !left->node_type) left->node_type = clone_type(left_type);
        if (r_arr && !right->node_type) right->node_type = clone_type(right_type);
        if (operator == TOKEN_EQUALS || operator == TOKEN_NOT_EQUALS) {
            /* #2789: `s == []` compares with the empty slice, which views no
             * storage: its pointer is null, so it is `s == null`. */
            if (l_arr && slice_empty_literal_to_null(&expr->children[1])) {
                right = expr->children[1];
                r_ptrish = 1;
                changed = 1;
            }
            if (r_arr && slice_empty_literal_to_null(&expr->children[0])) {
                left = expr->children[0];
                l_ptrish = 1;
                changed = 1;
            }
            if (l_arr && r_ptrish) changed |= slice_coerce_to_ptr(&expr->children[0]);
            if (r_arr && l_ptrish) changed |= slice_coerce_to_ptr(&expr->children[1]);
        } else if (operator == TOKEN_PLUS || operator == TOKEN_MINUS) {
            if (l_arr && r_int) changed |= slice_coerce_to_ptr(&expr->children[0]);
            if (r_arr && l_int) changed |= slice_coerce_to_ptr(&expr->children[1]);
        } else if (operator == TOKEN_ASSIGN && left_type) {
            changed |= slice_coerce_slot(&expr->children[1], left_type, 0);
        }
        if (changed) {
            left = expr->children[0];
            right = expr->children[1];
            free_type(left_type);
            free_type(right_type);
            left_type = infer_type(left, table);
            right_type = infer_type(right, table);
        }
    }

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
        if (reject_const_element_write(left, table, expr)) {   /* #2516 */
            free_type(left_type);
            free_type(right_type);
            return 0;
        }
        ASTNode* temp = assign_target_temporary(left, table, 1);
        if (temp) {
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "cannot assign to %s of '%s(...)': a call's result is a "
                     "temporary, so the write would be lost",
                     temp == left ? "the result" : "a part of the result",
                     temp->value ? temp->value : "?");
            type_error_hint(msg, "bind the result to a variable, change it there, "
                            "and pass or store that variable",
                            expr->line, expr->column);
            free_type(left_type);
            free_type(right_type);
            return 0;
        }
        /* #2628: a closure stored in a typed fn-pointer field (`f.cb = |p|
         * ...`, through a pointer too) or element. Checked first: the test
         * below passes a closure of the slot's signature and calls any
         * other one a plain mismatch. */
        if (left_type && left_type->kind == TYPE_FUNCTION && left_type->is_fnptr) {
            FnptrSlotKind kind = FNPTR_SLOT_VARIABLE;
            char slot[256];
            snprintf(slot, sizeof(slot), "%s", left->value ? left->value : "?");
            if (left->type == AST_MEMBER_ACCESS && left->child_count > 0) {
                kind = FNPTR_SLOT_FIELD;
                Type* ot = infer_type(left->children[0], table);
                Type* st = ot && ot->kind == TYPE_PTR ? ot->element_type : ot;
                if (st && st->kind == TYPE_STRUCT && st->struct_name)
                    snprintf(slot, sizeof(slot), "%s.%s", st->struct_name,
                             left->value ? left->value : "?");
                free_type(ot);
            } else if (left->type == AST_ARRAY_ACCESS && left->child_count > 0) {
                kind = FNPTR_SLOT_ELEMENT;
                ASTNode* arr = left->children[0];
                snprintf(slot, sizeof(slot), "%s",
                         arr && arr->type == AST_IDENTIFIER && arr->value ? arr->value : "array");
            }
            if (reject_closure_in_fnptr_slot(table, right, right_type, left_type, kind, slot,
                                             right->line, right->column)) {
                free_type(left_type);
                free_type(right_type);
                return 0;
            }
        }
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
        /* #2688: a whole array stored into a fixed-size array field takes
         * one of the field's own length. */
        if (left->type == AST_MEMBER_ACCESS && left->value && left_type &&
            left_type->kind == TYPE_ARRAY && left_type->array_size > 0) {
            char slot[256];
            Type* ot = left->child_count > 0 ? infer_type(left->children[0], table) : NULL;
            Type* st = ot && ot->kind == TYPE_PTR ? ot->element_type : ot;
            if (st && st->kind == TYPE_STRUCT && st->struct_name)
                snprintf(slot, sizeof(slot), "field '%s.%s'", st->struct_name, left->value);
            else
                snprintf(slot, sizeof(slot), "field '%s'", left->value);
            free_type(ot);
            if (reject_array_field_source(right, right_type, left_type, slot,
                                          expr->line, expr->column)) {
                free_type(left_type);
                free_type(right_type);
                return 0;
            }
        }
        /* #698 for an element: `xs = [1, 2]` infers an int array, and
         * `xs[0] = 7.5` then truncated silently (is_assignable permits
         * float->int). The element type was inferred from the literal, not
         * chosen, so — exactly as for an inferred int local — say so and
         * name the fix. An annotated array (`int[2] xs = ...`) narrows
         * explicitly and is left alone. */
        if (left->type == AST_ARRAY_ACCESS && left->child_count >= 1 &&
            left->children[0] && left->children[0]->type == AST_IDENTIFIER &&
            left_type && left_type->kind == TYPE_INT &&
            right_type && (right_type->kind == TYPE_FLOAT ||
                           right_type->kind == TYPE_FLOAT32 ||
                           right_type->kind == TYPE_LONGDOUBLE)) {
            Symbol* arr_sym = lookup_symbol(table, left->children[0]->value);
            if (arr_sym && arr_sym->type_inferred) {
                const char* nm = left->children[0]->value;
                char msg[420];
                snprintf(msg, sizeof(msg),
                    "narrowing assignment to an element of '%s': its element type "
                    "was inferred as int from the array literal, but a float is "
                    "assigned here and would truncate. Write the literal with float "
                    "elements (e.g. `[1.0, 2.0]`), or annotate the declaration "
                    "(`int[N] %s = ...`) to make the narrowing explicit.", nm, nm);
                free_type(left_type);
                free_type(right_type);
                type_error(msg, expr->line, expr->column);
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
        if (binary_mixes_distinct(left_type, right_type, operator)) {
            /* #480: two distinct types are nominally separate, so an
             * operator does not mix them, as an assignment or a call
             * argument does not. Reported by their own names: by their
             * shared base it would read "float and float". */
            char msg[400];
            snprintf(msg, sizeof(msg),
                     "'%s' mixes distinct types %s and %s: a distinct type does not "
                     "combine with another one; convert one side explicitly, "
                     "`<value> as %s`",
                     expr->value ? expr->value : "?", left_type->distinct_name,
                     right_type->distinct_name, left_type->distinct_name);
            type_error(msg, expr->line, expr->column);
            /* Typed as the operator would type it for one of them, so the
             * one error is the report rather than the first of a cascade
             * (`if a < b` "must be boolean", `c as float` "unknown"). */
            int cmp = operator == TOKEN_EQUALS || operator == TOKEN_NOT_EQUALS ||
                      operator == TOKEN_LESS || operator == TOKEN_LESS_EQUAL ||
                      operator == TOKEN_GREATER || operator == TOKEN_GREATER_EQUAL ||
                      operator == TOKEN_AND || operator == TOKEN_OR;
            set_node_type(expr, cmp && !is_lane_type(left_type->kind)
                                    ? create_type(TYPE_BOOL) : clone_type(left_type));
            free_type(left_type);
            free_type(right_type);
            return 0;
        }
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

/* The declared type of `<recv>.<field>` where `recv` is a local struct or
 * pointer-to-struct, borrowed, or NULL. *out_is_ptr is set to 1 when the
 * receiver is a pointer-to-struct. */
static Type* local_struct_field_type(SymbolTable* table, const char* recv_name,
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
            if (out_is_ptr) *out_is_ptr = is_ptr;
            return f->node_type;
        }
    }
    return NULL;
}

/* #749: resolve `<recv>.<field>` where `recv` is a local struct or
 * pointer-to-struct whose `field` is a function-pointer member. Returns
 * the field's TYPE_FUNCTION (is_fnptr) Type* (borrowed, do not free) or
 * NULL. *out_is_ptr is set to 1 when the receiver is a pointer-to-struct
 * (codegen then emits `->field` rather than `.field`). */
static Type* resolve_fnptr_struct_field(SymbolTable* table, const char* recv_name,
                                        const char* field_name, int* out_is_ptr) {
    int is_ptr = 0;
    Type* t = local_struct_field_type(table, recv_name, field_name, &is_ptr);
    if (!t || t->kind != TYPE_FUNCTION || !t->is_fnptr) return NULL;
    if (out_is_ptr) *out_is_ptr = is_ptr;
    return t;
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

/* #928/#934 UFCS: the function `recv.method(...)` calls, for a receiver of
 * type `rtype`, as a heap string to put on the call node (the bare name, an
 * import's alias, or `mod.method`), or NULL when none applies. Shared by the
 * rewrite below and by the settle of which functions return a value, which
 * must resolve the call as the rewrite will (#2744). Two tiers, in order:
 *   (1) a same-file (bare) free function, and
 *   (2) #934: a function exported by an imported module, the case that
 *       makes library-provided fluent surfaces work. We honour the same
 *       visibility as a normal qualified `mod.method(recv)` call: walk the
 *       visible namespaces and accept the first whose `mod.method` resolves
 *       to a function with a matching first parameter. On an imported match
 *       the call is rewritten to the QUALIFIED form `mod.method` so normal
 *       resolution + codegen emit `mod_method`. */
static char* ufcs_resolve(const char* method, Type* rtype, SymbolTable* table) {
    if (!method) return NULL;
    Symbol* fsym = lookup_symbol(table, method);
    if (fsym && fsym->is_function && fsym->node &&
        ufcs_first_param_matches(fsym->node, rtype)) {
        /* A bare name can also be a SELECTIVELY imported function
         * (`import m (f)` then `x.f()`): the visible symbol is the import's
         * alias, which an ordinary `f(x)` call has renamed to its qualified
         * `m.f` by the time it reaches codegen. Shape (b) is rewritten after
         * that renaming, so without the same lookup here the call kept the
         * bare `f` and codegen emitted `f(x)`, an undeclared C function, not
         * `m_f(x)`. */
        const char* aliased = find_import_alias(method);
        return strdup(aliased ? aliased : method);
    }
    if (!global_module_registry) return NULL;
    for (int mi = 0; mi < global_module_registry->module_count; mi++) {
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
            return strdup(qualified);  /* imported qualified call */
        }
    }
    return NULL;
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

    char* resolved = ufcs_resolve(method, rtype, table);  /* the name to put on the call */

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
    if (ast_is_own_function(node)) return 0;   /* a trailing block's are ours (#2766) */
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
/* #2645, #2647: a function written as several clauses (`f(0) -> ...`,
 * `f(n) -> ...`) is one symbol, whose node is its last clause. What a call
 * of it yields, and which `requires` can apply to a call, are questions
 * about every clause. The clauses of each name, in program order, are
 * indexed once per program (as codegen's program index is, #2007); the
 * type checker frees no top-level node while it runs. */
typedef struct {
    ASTNode** nodes;
    int count;
    int cap;
} TcClauses;

static StrMap g_tc_clauses;
static ASTNode* g_tc_clauses_program = NULL;
static int g_tc_clauses_child_count = -1;

static void tc_clauses_reset(void) {
    for (int i = 0; i < strmap_count(&g_tc_clauses); i++) {
        TcClauses* tc = strmap_value_at(&g_tc_clauses, i);
        if (tc) { free(tc->nodes); free(tc); }
    }
    strmap_free(&g_tc_clauses);
    g_tc_clauses_program = NULL;
    g_tc_clauses_child_count = -1;
}

/* The clauses of the set `fn` is one of, or NULL for a single definition. */
static const TcClauses* tc_clauses_of(ASTNode* fn) {
    ASTNode* program = g_typecheck_program;
    if (!program || !fn || !fn->value) return NULL;
    if (g_tc_clauses_program != program || g_tc_clauses_child_count != program->child_count) {
        tc_clauses_reset();
        for (int i = 0; i < program->child_count; i++) {
            ASTNode* c = program->children[i];
            if (!c || !c->value ||
                (c->type != AST_FUNCTION_DEFINITION && c->type != AST_BUILDER_FUNCTION)) continue;
            TcClauses* tc = strmap_get(&g_tc_clauses, c->value);
            if (!tc) {
                tc = calloc(1, sizeof(TcClauses));
                if (!tc) continue;
                strmap_put(&g_tc_clauses, c->value, tc);
            }
            if (tc->count == tc->cap) {
                int cap = tc->cap ? tc->cap * 2 : 2;
                ASTNode** nn = realloc(tc->nodes, sizeof(ASTNode*) * (size_t)cap);
                if (!nn) continue;
                tc->nodes = nn;
                tc->cap = cap;
            }
            tc->nodes[tc->count++] = c;
        }
        g_tc_clauses_program = program;
        g_tc_clauses_child_count = program->child_count;
    }
    const TcClauses* tc = strmap_get(&g_tc_clauses, fn->value);
    if (!tc || tc->count < 2) return NULL;
    for (int i = 0; i < tc->count; i++)
        if (tc->nodes[i] == fn) return tc;
    return NULL;
}

/* ---- Which `return`s have a value (#2697, #2733, #2735, #2736) ----------
 *
 * A `return` gives its caller a value only when what it returns has one. A
 * call of a function that returns nothing has none, and neither has a
 * `match` whose arms all end in such a call or in a statement. A return of
 * one is that statement, then a return of nothing: `-> note(x)`, `-> {
 * note(x) }` and `{ return note(x) }` all end with the call (#2697, #2733).
 *
 * Whether a function returns a value so depends on the functions its
 * returns call, and the checker meets a call before the body of a callee
 * defined further down, whose returns it has not settled yet: with `f(x)
 * -> { g(x) }` defined before `g(x) -> { note(x) }`, f returned g's void
 * (#2736). The answer is settled for every function of the program before
 * any body is checked, as the least fixed point over the calls its returns
 * make: a function returns a value when its written result type is one, or
 * when some `return` of it has one, given what the functions it calls
 * there return. None does until shown to. */

#define TC_YIELDS_NONE  ((void*)1)
#define TC_YIELDS_VALUE ((void*)2)
static StrMap g_tc_yields;   /* function name -> TC_YIELDS_NONE / TC_YIELDS_VALUE */

static int tc_value_yields(ASTNode* v, SymbolTable* table, StrMap* locals, int depth);

/* The expression a match arm's body gives, as match_arm_value reads it but
 * without types: the body, or a block's last expression (not an assignment)
 * or last `match`. NULL when the arm ends in a statement. */
static ASTNode* tc_arm_value(ASTNode* body) {
    if (!body) return NULL;
    switch (body->type) {
        case AST_PRINT_STATEMENT: case AST_RETURN_STATEMENT:
        case AST_VARIABLE_DECLARATION: case AST_ASSIGNMENT:
        case AST_COMPOUND_ASSIGNMENT: case AST_EXPRESSION_STATEMENT:
            return NULL;
        case AST_BLOCK: {
            ASTNode* last = body->child_count > 0 ? body->children[body->child_count - 1] : NULL;
            if (!last) return NULL;
            if (last->type == AST_MATCH_STATEMENT) return last;
            if (last->type != AST_EXPRESSION_STATEMENT || last->child_count < 1) return NULL;
            ASTNode* e = last->children[0];
            if (!e || (e->type == AST_BINARY_EXPRESSION && e->value &&
                       strcmp(e->value, "=") == 0)) return NULL;
            return e;
        }
        default:
            return body;
    }
}

static ASTNode* tc_closure_body(ASTNode* closure) {
    for (int i = closure ? closure->child_count - 1 : -1; i >= 0; i--)
        if (closure->children[i] && closure->children[i]->type == AST_BLOCK)
            return closure->children[i];
    return NULL;
}

/* Issue #333: the receiver of a member-access call's trailing block
 * (typechecker.h). */
const char* dsl_receiver_of_call(ASTNode* call, SymbolTable* table, char* buf, size_t cap) {
    if (!call || !call->value || !buf || cap == 0) return NULL;
    const char* dot = strchr(call->value, '.');
    if (!dot) return NULL;
    size_t prefix_len = (size_t)(dot - call->value);
    if (prefix_len == 0 || prefix_len >= cap) return NULL;
    memcpy(buf, call->value, prefix_len);
    buf[prefix_len] = '\0';
    /* Namespace receiver: directly visible. */
    if (is_visible_namespace(buf, table)) return buf;
    /* Typed-value receiver: the prefix is a local variable; its struct
     * type's name. */
    Symbol* recv_sym = lookup_symbol(table, buf);
    if (!recv_sym || !recv_sym->type) return NULL;
    const char* tn = base_type_name(recv_sym->type);
    if (!tn || tn[0] == '\0' || strlen(tn) >= cap ||
        strcmp(tn, "unknown") == 0 || strcmp(tn, "int") == 0 ||
        strcmp(tn, "long") == 0 || strcmp(tn, "float") == 0 ||
        strcmp(tn, "double") == 0 || strcmp(tn, "bool") == 0 ||
        strcmp(tn, "string") == 0) return NULL;
    memmove(buf, tn, strlen(tn) + 1);
    return buf;
}

/* The receiver trailing block `block` of `call` resolves bare calls
 * through: the one the checker stamped on it, else the call's. */
static const char* tc_trailing_receiver(ASTNode* call, ASTNode* block, SymbolTable* table,
                                        char* buf, size_t cap) {
    if (block->annotation && strncmp(block->annotation, "dsl_recv:", 9) == 0)
        return block->annotation[9] ? block->annotation + 9 : NULL;
    return dsl_receiver_of_call(call, table, buf, cap);
}

static void tc_collect_locals(ASTNode* n, StrMap* locals);
static void tc_declare_locals(ASTNode* n, SymbolTable* scope);

/* Declare in `scope` every name the block `body` binds, with the type
 * inference gave it where that is known (tc_declare_locals), else unknown:
 * the names the checker's scope for the block holds. */
static void tc_declare_block_names(ASTNode* body, SymbolTable* scope) {
    if (!body || !scope) return;
    tc_declare_locals(body, scope);
    StrMap names;
    strmap_init(&names);
    tc_collect_locals(body, &names);
    for (int i = 0; i < strmap_count(&names); i++) {
        const char* nm = strmap_key_at(&names, i);
        if (nm && !lookup_symbol_local(scope, nm))
            add_symbol(scope, nm, create_type(TYPE_UNKNOWN), 0, 0, 0);
    }
    strmap_free(&names);
}

/* Does the bare call `name`, in the scope `table`, resolve through a DSL
 * block's receiver, as lookup_symbol resolves it (#333)? Scope by scope from
 * the innermost: one that binds the name keeps it, and a block's receiver
 * that has a `<receiver>_<name>` takes it, ahead of anything further out:
 * the function's own locals and the program's function of that name. */
static int tc_receiver_resolves(SymbolTable* table, const char* name) {
    if (!name || strchr(name, '.')) return 0;
    for (SymbolTable* t = table; t; t = t->parent) {
        if (lookup_symbol_local(t, name)) return 0;
        if (t->dsl_receiver && t->dsl_receiver[0]) {
            char rewritten[512];
            snprintf(rewritten, sizeof(rewritten), "%.250s_%.250s", t->dsl_receiver, name);
            for (SymbolTable* u = t; u; u = u->parent)
                if (lookup_symbol_local(u, rewritten)) return 1;
        }
    }
    return 0;
}

/* Does a `return` anywhere in `node` give a value? A nested closure's
 * returns are its own; a call's trailing block runs inline, so its returns
 * are the function's (#2766). A bare call in such a block resolves through
 * the call's DSL receiver (`<receiver>_<name>`), as the checker resolves it
 * in the block's scope (#333). */
static int tc_body_yields(ASTNode* node, SymbolTable* table, StrMap* locals, int depth) {
    if (!node || ast_is_own_function(node)) return 0;
    if (node->type == AST_RETURN_STATEMENT && node->child_count > 0 && node->children[0]) {
        if (node->child_count > 1) return 1;   /* `return a, b` */
        return tc_value_yields(node->children[0], table, locals, depth);
    }
    for (int i = 0; i < node->child_count; i++) {
        ASTNode* c = node->children[i];
        if (node->type == AST_FUNCTION_CALL && ast_is_trailing_block(c) && table) {
            char buf[256];
            const char* recv = tc_trailing_receiver(node, c, table, buf, sizeof(buf));
            if (recv) {
                /* The block's scope, as the checker builds it: the
                 * receiver, and the names the block binds (its own, ahead
                 * of the receiver's functions; tc_receiver_resolves). */
                ASTNode* body = tc_closure_body(c);
                SymbolTable* scope = create_symbol_table(table);
                scope->dsl_receiver = strdup(recv);
                tc_declare_block_names(body, scope);
                int y = tc_body_yields(body, scope, locals, depth);
                free_symbol_table(scope);
                if (y) return 1;
                continue;
            }
        }
        if (tc_body_yields(c, table, locals, depth)) return 1;
    }
    return 0;
}

/* Does a call through a value of function type `t` give a value? A typed C
 * function pointer by its result slot (none is void); a closure type by its
 * result when it carries one, and an erased `fn` is called for an int
 * (#2054). As typecheck_function_call types the call. */
static int tc_fn_type_yields(Type* t) {
    if (!t || t->kind != TYPE_FUNCTION) return 1;
    if (t->is_fnptr) return t->return_type && t->return_type->kind != TYPE_VOID;
    if (t->return_type) return t->return_type->kind != TYPE_VOID;
    return 1;
}

/* A name the definition binds, called: `bound` is what tc_collect_locals
 * recorded for it (a closure literal, a declaration typed `fn(...)`, or
 * NULL when neither is known). */
static int tc_bound_callee_yields(ASTNode* bound, SymbolTable* table, StrMap* locals,
                                  int depth) {
    if (!bound) return 1;
    if (bound->type == AST_CLOSURE)
        return tc_body_yields(tc_closure_body(bound), table, locals, depth + 1);
    return tc_fn_type_yields(bound->node_type);
}

static int tc_call_yields(ASTNode* call, SymbolTable* table, StrMap* locals, int depth);

/* #2744: `recv.name(...)` on a value `recv` of type `rtype` (NULL when not
 * known), resolved as typecheck_function_call resolves it once the
 * qualified lookup declined: a function-pointer field or a closure field of
 * `recv`'s struct (when `recv` is a local named `recv_name`), else the free
 * function UFCS rewrites it to. Returns -1 when none applies. */
static int tc_method_call_yields(const char* recv_name, const char* name, Type* rtype,
                                 SymbolTable* table, int depth) {
    if (recv_name) {
        int is_ptr = 0;
        Type* fsig = resolve_fnptr_struct_field(table, recv_name, name, &is_ptr);
        if (fsig) return tc_fn_type_yields(fsig);
        Type* ft = local_struct_field_type(table, recv_name, name, NULL);
        if (ft && ft->kind == TYPE_FUNCTION) return tc_fn_type_yields(ft);
    }
    Type* unknown = rtype ? NULL : create_type(TYPE_UNKNOWN);
    char* target = ufcs_resolve(name, rtype ? rtype : unknown, table);
    if (unknown) free_type(unknown);
    if (!target) return -1;
    ASTNode as_call;
    memset(&as_call, 0, sizeof(as_call));
    as_call.type = AST_FUNCTION_CALL;
    as_call.value = target;
    int yields = tc_call_yields(&as_call, table, NULL, depth + 1);
    free(target);
    return yields;
}

/* Does calling `call` give a value? `locals`, while the program's answers
 * are being settled, holds the names the definition binds itself (each to
 * the closure literal bound to it, if that is its only binding, or to its
 * declaration when that is typed `fn(...)`): such a name is not the
 * program's function of that name. The settle's scope also holds those
 * names with the types inference gave them, so a receiver's type is known.
 * In the checker, the scope `table` resolves the name, and the call's type
 * speaks for what is not one of the program's functions. */
static int tc_call_yields(ASTNode* call, SymbolTable* table, StrMap* locals, int depth) {
    if (!call->value || depth > 16) return 1;
    /* In a DSL block a bare call is the receiver's function before it is
     * one of the definition's own names: the lookup below finds it. */
    if (locals && strmap_has(locals, call->value) &&
        !tc_receiver_resolves(table, call->value))
        return tc_bound_callee_yields(strmap_get(locals, call->value), table, locals, depth);
    /* #2744: a call the parser tagged UFCS (`mk(n).show()`), before the
     * checker rewrote it: the function the rewrite will pick, for the type
     * inference gave the receiver (read, not computed: nothing is checked,
     * and nothing reported, before the checker runs). */
    if (call->annotation && strcmp(call->annotation, "ufcs") == 0) {
        if (!table || call->child_count < 1 || !call->children[0]) return 1;
        int y = tc_method_call_yields(NULL, call->value, call->children[0]->node_type,
                                      table, depth);
        return y < 0 ? 1 : y;
    }
    /* `call(k, x)`, which a checked `k(x)` of a closure binding becomes: the
     * closure literal it is known to call gives what its body returns, as
     * #2460 reads the literal's result type and codegen its C one. Through a
     * name bound to a typed `fn(...)`, its signature answers (#2744). */
    if (strcmp(call->value, "call") == 0 && call->child_count > 0 && call->children[0]) {
        ASTNode* f = call->children[0];
        if (f->type == AST_CLOSURE)
            return tc_body_yields(tc_closure_body(f), table, locals, depth + 1);
        if (locals) {
            if (f->type == AST_IDENTIFIER && f->value && strmap_has(locals, f->value))
                return tc_bound_callee_yields(strmap_get(locals, f->value), table, locals,
                                              depth);
        } else if (table) {
            ASTNode* lit = call_callee_literal(call, table);
            if (lit && lit->type == AST_CLOSURE)
                return tc_body_yields(tc_closure_body(lit), table, locals, depth + 1);
        }
    }
    /* #2744: `recv.name(...)` on a local the settle has not seen resolved:
     * a field of its struct, or the function UFCS gives it. A namespace of
     * that name is a module call, resolved below. In the checker the call
     * was rewritten already, or carries the field's type. */
    const char* dot = strchr(call->value, '.');
    if (locals && dot && dot != call->value && !strchr(dot + 1, '.')) {
        char recv[200];
        size_t rlen = (size_t)(dot - call->value);
        if (rlen < sizeof(recv)) {
            memcpy(recv, call->value, rlen);
            recv[rlen] = '\0';
            if (strmap_has(locals, recv) && !is_visible_namespace(recv, table)) {
                Symbol* rs = table ? lookup_symbol(table, recv) : NULL;
                int y = tc_method_call_yields(recv, dot + 1, rs ? rs->type : NULL, table,
                                              depth);
                if (y >= 0) return y;
            }
        }
    }
    /* A call through a function-pointer field, checked: the field's
     * signature (its call is typed unknown when the field returns nothing). */
    if (!locals && table && call->annotation &&
        (strcmp(call->annotation, "fnfield_ptr") == 0 ||
         strcmp(call->annotation, "fnfield_val") == 0) && dot) {
        char recv[200];
        size_t rlen = (size_t)(dot - call->value);
        if (rlen < sizeof(recv)) {
            memcpy(recv, call->value, rlen);
            recv[rlen] = '\0';
            Type* fsig = resolve_fnptr_struct_field(table, recv, dot + 1, NULL);
            if (fsig) return tc_fn_type_yields(fsig);
        }
    }
    Symbol* sym = table ? lookup_qualified_symbol(table, call->value) : NULL;
    if (sym && sym->node && sym->node->value &&
        (sym->node->type == AST_FUNCTION_DEFINITION ||
         sym->node->type == AST_BUILDER_FUNCTION)) {
        void* known = strmap_get(&g_tc_yields, sym->node->value);
        if (known) return known == TC_YIELDS_VALUE;
        return !fn_yields_no_value(sym->node);
    }
    if (call->node_type && call->node_type->kind == TYPE_VOID) return 0;
    if (sym && sym->is_function && sym->type && sym->type->kind == TYPE_VOID) return 0;
    return 1;
}

static int tc_value_yields(ASTNode* v, SymbolTable* table, StrMap* locals, int depth) {
    if (!v) return 1;
    switch (v->type) {
        case AST_PRINT_STATEMENT:
            return 0;
        case AST_FUNCTION_CALL:
            return tc_call_yields(v, table, locals, depth);
        case AST_MATCH_STATEMENT:
            if (depth > 64) return 1;
            for (int i = 1; i < v->child_count; i++) {
                ASTNode* arm = v->children[i];
                if (!arm || arm->type != AST_MATCH_ARM || arm->child_count < 2) continue;
                ASTNode* given = tc_arm_value(arm->children[1]);
                if (given && tc_value_yields(given, table, locals, depth + 1)) return 1;
            }
            return 0;
        default:
            return 1;
    }
}

/* The names a definition binds (parameters, locals, closure parameters,
 * pattern variables), each to the closure literal bound to it when that is
 * its only binding, or to its declaration when that is a parameter or a
 * local written with a `fn(...)` type, whose signature says what a call
 * through it gives (#2744); else NULL. */
static void tc_collect_locals(ASTNode* n, StrMap* locals) {
    if (!n) return;
    if (n->value) {
        if (n->type == AST_VARIABLE_DECLARATION) {
            ASTNode* init = n->child_count > 0 ? n->children[0] : NULL;
            int first = !strmap_has(locals, n->value);
            ASTNode* known = NULL;
            if (first && init && init->type == AST_CLOSURE)
                known = init;
            else if (first && !n->type_inferred && n->node_type &&
                     n->node_type->kind == TYPE_FUNCTION)
                known = n;
            strmap_put(locals, n->value, known);
        } else if (n->type == AST_PATTERN_VARIABLE || n->type == AST_CLOSURE_PARAM) {
            /* A parameter is a pattern variable carrying its written type. */
            int first = !strmap_has(locals, n->value);
            strmap_put(locals, n->value,
                       first && n->node_type && n->node_type->kind == TYPE_FUNCTION
                           ? n : NULL);
        } else if (n->type == AST_CONST_DECLARATION) {
            strmap_put(locals, n->value, NULL);
        }
    }
    for (int i = 0; i < n->child_count; i++) tc_collect_locals(n->children[i], locals);
}

/* #2744: declare in the settle's scope for a definition the names it binds,
 * each with the type its first typed declaration has after inference (a
 * parameter's written type, a local's inferred one), so a call on one of
 * them resolves as the checker will resolve it: a field of the receiver's
 * struct, or the function UFCS picks for the receiver's type. */
static void tc_declare_locals(ASTNode* n, SymbolTable* scope) {
    if (!n) return;
    if (n->value && (n->type == AST_VARIABLE_DECLARATION ||
                     n->type == AST_PATTERN_VARIABLE || n->type == AST_CLOSURE_PARAM) &&
        !lookup_symbol_local(scope, n->value)) {
        Type* t = n->node_type;
        if ((!t || t->kind == TYPE_UNKNOWN) && n->type == AST_VARIABLE_DECLARATION &&
            n->child_count > 0 && n->children[0])
            t = n->children[0]->node_type;
        if (t && t->kind != TYPE_UNKNOWN && t->kind != TYPE_VOID)
            add_symbol(scope, n->value, clone_type(t), 0, 0, 0);
    }
    for (int i = 0; i < n->child_count; i++) tc_declare_locals(n->children[i], scope);
}

static int tc_written_value_type(ASTNode* def) {
    Type* t = def->node_type;
    return !def->type_inferred && t && t->kind != TYPE_VOID && t->kind != TYPE_UNKNOWN;
}

static void tc_yields_settle(ASTNode* program, SymbolTable* global) {
    strmap_free(&g_tc_yields);
    strmap_init(&g_tc_yields);
    if (!program) return;

    int n = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_EXPORT_STATEMENT && c->child_count > 0) c = c->children[0];
        if (c && c->value && (c->type == AST_FUNCTION_DEFINITION ||
                              c->type == AST_BUILDER_FUNCTION)) n++;
    }
    if (n == 0) return;
    ASTNode** defs = calloc((size_t)n, sizeof(ASTNode*));
    StrMap* locals = calloc((size_t)n, sizeof(StrMap));
    SymbolTable** scopes = calloc((size_t)n, sizeof(SymbolTable*));
    if (!defs || !locals || !scopes) {
        free(defs); free(locals); free(scopes);
        return;
    }
    n = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_EXPORT_STATEMENT && c->child_count > 0) c = c->children[0];
        if (!c || !c->value || (c->type != AST_FUNCTION_DEFINITION &&
                                c->type != AST_BUILDER_FUNCTION)) continue;
        defs[n] = c;
        strmap_init(&locals[n]);
        tc_collect_locals(c, &locals[n]);
        /* A merged module's function resolves `ns.name` against that
         * module's imports, as it is checked (#2614). */
        scopes[n] = create_symbol_table(global);
        if (c->origin_module) scopes[n]->merged_from = c->origin_module;
        tc_declare_locals(c, scopes[n]);   /* #2744 */
        void* was = strmap_get(&g_tc_yields, c->value);
        strmap_put(&g_tc_yields, c->value,
                   (was == TC_YIELDS_VALUE || tc_written_value_type(c))
                       ? TC_YIELDS_VALUE : TC_YIELDS_NONE);
        n++;
    }

    for (int changed = 1; changed; ) {
        changed = 0;
        for (int i = 0; i < n; i++) {
            if (strmap_get(&g_tc_yields, defs[i]->value) == TC_YIELDS_VALUE) continue;
            ASTNode* body = defs[i]->child_count > 0
                ? defs[i]->children[defs[i]->child_count - 1] : NULL;
            if (tc_body_yields(body, scopes[i], &locals[i], 0)) {
                strmap_put(&g_tc_yields, defs[i]->value, TC_YIELDS_VALUE);
                changed = 1;
            }
        }
    }

    /* The answer is what the function's C type follows: a result type the
     * inference pass gave a function that returns nothing (a call there it
     * typed otherwise than the checker resolves it) declared `int f(...)`
     * around the bare returns the checker makes of its returns. */
    for (int i = 0; i < n; i++) {
        ASTNode* d = defs[i];
        Type* t = d->node_type;
        if (!d->type_inferred || !t || t->kind == TYPE_VOID || t->kind == TYPE_UNKNOWN) continue;
        if (strmap_get(&g_tc_yields, d->value) != TC_YIELDS_NONE) continue;
        free_type(d->node_type);
        d->node_type = create_type(TYPE_VOID);
        Symbol* fs = lookup_symbol_local(global, d->value);
        if (fs && fs->is_function) {
            if (fs->type) free_type(fs->type);
            fs->type = create_type(TYPE_VOID);
        }
    }

    for (int i = 0; i < n; i++) {
        strmap_free(&locals[i]);
        free_symbol_table(scopes[i]);
    }
    free(defs);
    free(locals);
    free(scopes);
}

/* Does the function, main or closure whose `return` is being checked give
 * a value? A function answers as its callers see it; main or a closure by
 * its returns. */
static int tc_owner_yields(SymbolTable* table) {
    ASTNode* owner = g_tc_return_owner;
    if (!owner) return 0;
    if ((owner->type == AST_FUNCTION_DEFINITION || owner->type == AST_BUILDER_FUNCTION) &&
        owner->value) {
        void* known = strmap_get(&g_tc_yields, owner->value);
        if (known) return known == TC_YIELDS_VALUE;
        return !fn_yields_no_value(owner);
    }
    ASTNode* body = owner->type == AST_CLOSURE ? tc_closure_body(owner)
                  : owner->child_count > 0 ? owner->children[0] : NULL;
    return tc_body_yields(body, table, NULL, 0);
}

/* #2743: a `match` whose value is used (bound, assigned, or returned when
 * some arm gives a value) takes its value from the arm that runs, so every
 * arm that ends in a value must have one. An arm ending in a call of a
 * function that returns nothing, or in a `match` none of whose arms gives a
 * value, has none: codegen stored that void in the match's result and the C
 * compiler stopped ("void value not ignored"). An arm that ends in a
 * statement (`return`, `break`, a print, an assignment) gives no value and
 * leaves the result as it is, as before. Returns the number of arms
 * refused. */
static int check_match_value_arms(ASTNode* m, SymbolTable* table) {
    int refused = 0;
    if (!m || m->type != AST_MATCH_STATEMENT) return 0;
    for (int i = 1; i < m->child_count; i++) {
        ASTNode* arm = m->children[i];
        if (!arm || arm->type != AST_MATCH_ARM || arm->child_count < 2) continue;
        ASTNode* given = tc_arm_value(arm->children[1]);
        if (!given) continue;
        if (given->type == AST_MATCH_STATEMENT) {
            if (tc_value_yields(given, table, NULL, 0)) {
                refused += check_match_value_arms(given, table);
                continue;
            }
            type_error_hint("this arm's `match` gives no value (each of its arms ends in a "
                            "statement or in a call that returns nothing), and the value of "
                            "the `match` it is an arm of is used",
                            "give the arm a value: put the inner `match` in a block and end "
                            "the block with one",
                            given->line, given->column);
            refused++;
            continue;
        }
        if (given->type != AST_FUNCTION_CALL || tc_value_yields(given, table, NULL, 0))
            continue;
        const char* callee = given->value ? given->value : "function";
        char msg[320];
        char help[320];
        snprintf(msg, sizeof(msg),
                 "'%s' returns no value, so this arm gives the `match` none, and the "
                 "value of the `match` is used", callee);
        snprintf(help, sizeof(help),
                 "give the arm a value: a block arm can make the call and end with one, "
                 "`{ %s(...)  0 }`", callee);
        type_error_hint(msg, help, given->line, given->column);
        refused++;
    }
    return refused;
}

/* A checked `return` of a call or a `match`, `ret`: when what it returns
 * has no value, it becomes the statement it returns. An arrow body's value
 * (marked `arrow_value` by the parser) is its last statement, so the body
 * ends with the call or the match; `return note(x)` written out is the
 * call, then a `return` of nothing. In a function, main or closure that
 * returns a value, such a `return` has nothing to give: refused. Returns
 * -1 when the value has one (the return stays), 1 when the return became
 * the statement, 0 when it was refused. */
static int tc_return_settle(ASTNode* ret, SymbolTable* table) {
    ASTNode* v = ret->children[0];
    if (tc_value_yields(v, table, NULL, 0)) return -1;
    int arrow = ret->annotation && strcmp(ret->annotation, "arrow_value") == 0;
    if (!arrow && tc_owner_yields(table)) {
        char msg[320];
        if (v->type == AST_FUNCTION_CALL)
            snprintf(msg, sizeof(msg),
                     "'%s' returns no value, so this `return` has nothing to give back",
                     v->value ? v->value : "function");
        else
            snprintf(msg, sizeof(msg),
                     "this `match` gives no value (each arm ends in a statement or in a "
                     "call that returns nothing), so this `return` has nothing to give back");
        type_error_hint(msg,
                        "a value is returned on another path, so every `return` needs one: "
                        "run this on a line of its own, then return a value",
                        ret->line, ret->column);
        return 0;
    }
    free(ret->annotation);
    ret->annotation = NULL;
    if (arrow) {
        /* A `match` runs as a statement in a block of its own. */
        ret->type = v->type == AST_MATCH_STATEMENT ? AST_BLOCK : AST_EXPRESSION_STATEMENT;
        return 1;
    }
    if (v->type != AST_MATCH_STATEMENT) {
        ASTNode* stmt = create_ast_node(AST_EXPRESSION_STATEMENT, NULL, v->line, v->column);
        add_child(stmt, v);
        ret->children[0] = stmt;
    }
    ret->type = AST_BLOCK;
    add_child(ret, create_ast_node(AST_RETURN_STATEMENT, NULL, ret->line, ret->column));
    return 1;
}

static int fn_yields_no_value(ASTNode* fn) {
    if (!fn) return 0;
    if (fn->type != AST_FUNCTION_DEFINITION && fn->type != AST_BUILDER_FUNCTION)
        return 0;
    /* A function of the program: settled before any body was checked, so
     * the answer does not depend on which was checked first (#2736). */
    void* known = fn->value ? strmap_get(&g_tc_yields, fn->value) : NULL;
    if (known) return known == TC_YIELDS_NONE;
    /* A clause set yields what codegen's fn_result_type decides over every
     * clause (#2645): a value when any clause declares a type or returns
     * one. */
    const TcClauses* set = tc_clauses_of(fn);
    int n = set ? set->count : 1;
    for (int c = 0; c < n; c++) {
        ASTNode* clause = set ? set->nodes[c] : fn;
        /* An annotated return type is authoritative, whatever the body does. */
        Type* rt = clause->node_type;
        if (rt && rt->kind != TYPE_VOID && rt->kind != TYPE_UNKNOWN) return 0;
        /* Unannotated: void exactly when no `return <value>` reaches codegen.
         * has_return_value is the same predicate codegen uses to decide, so
         * the two cannot disagree. */
        if (tc_has_return_value(clause)) return 0;
    }
    return 1;
}

/* Report the first `requires` of `fn` that the bindings in `env` decide
 * false at `call` (the call-site tier of contract folding). Returns 1 when
 * it reported one. */
static int fold_call_requires(ASTNode* call, ASTNode* fn, ContractEnv* env) {
    for (int ci = 0; ci < fn->child_count; ci++) {
        ASTNode* cl = fn->children[ci];
        if (!cl || cl->type != AST_REQUIRES_CLAUSE || cl->child_count == 0)
            continue;
        if (contract_eval_predicate(cl->children[0], env) != CONTRACT_FALSE)
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
        return 1;   /* one violation per call site is enough */
    }
    return 0;
}

/* Does the argument `arg` match the literal pattern `pat`: TRUE or FALSE
 * when both are known at compile time, UNKNOWN otherwise. */
static ContractTri literal_pattern_matches(ASTNode* pat, ASTNode* arg) {
    if (!pat || !arg || !pat->value || !pat->node_type) return CONTRACT_UNKNOWN;
    if (pat->node_type->kind == TYPE_INT) {
        int64_t v;
        if (!contract_eval_int64(arg, g_enum_program, &v)) return CONTRACT_UNKNOWN;
        return v == strtoll(pat->value, NULL, 0) ? CONTRACT_TRUE : CONTRACT_FALSE;
    }
    if (pat->node_type->kind == TYPE_BOOL) {
        ContractEnv none = {{0}, {0}, 0, g_enum_program};
        ContractTri t = contract_eval_predicate(arg, &none);
        if (t == CONTRACT_UNKNOWN) return CONTRACT_UNKNOWN;
        return (t == CONTRACT_TRUE) == (strcmp(pat->value, "true") == 0)
                   ? CONTRACT_TRUE : CONTRACT_FALSE;
    }
    if (pat->node_type->kind == TYPE_STRING && arg->type == AST_LITERAL && arg->value &&
        arg->node_type && arg->node_type->kind == TYPE_STRING) {
        return strcmp(arg->value, pat->value) == 0 ? CONTRACT_TRUE : CONTRACT_FALSE;
    }
    return CONTRACT_UNKNOWN;
}

/* #2647: a `requires` belongs to its clause: its dispatcher checks it when
 * it picks that clause, so the call-site fold applies a clause's contract
 * only to a call that can reach the clause. In order, a clause whose
 * literal pattern or guard this call's constant arguments rule out is
 * passed over, its contract with it; one they cannot rule out has its
 * contract folded under its own parameters (positions counting its literal
 * patterns); one they surely match (every literal pattern equal, the guard
 * true or absent) ends the walk, since no later clause is reached. The
 * last clause's contract was folded for every call, so `name_of(0)`, which
 * a first clause with no `requires` takes, failed on the second clause's
 * `requires n > 0`. Returns 0 when `fn` is a single definition. */
static int fold_clause_set_requires(ASTNode* call, ASTNode* fn) {
    const TcClauses* set = tc_clauses_of(fn);
    if (!set) return 0;
    for (int k = 0; k < set->count; k++) {
        ASTNode* clause = set->nodes[k];
        ContractEnv env = {{0}, {0}, 0, g_enum_program};
        int expected = count_function_params(clause);
        int offset = (has_ctx_first_param(clause) && call->child_count == expected - 1) ? -1 : 0;
        int can_match = 1, surely = 1, pos = 0;
        for (int i = 0; i < clause->child_count; i++) {
            ASTNode* p = clause->children[i];
            if (!p) continue;
            int is_var = p->type == AST_VARIABLE_DECLARATION || p->type == AST_PATTERN_VARIABLE;
            if (!is_var && p->type != AST_PATTERN_LITERAL && p->type != AST_PATTERN_STRUCT &&
                p->type != AST_PATTERN_LIST && p->type != AST_PATTERN_CONS) continue;
            int slot = pos++ + offset;
            ASTNode* arg = (slot >= 0 && slot < call->child_count) ? call->children[slot] : NULL;
            if (is_var) {
                if (arg && p->value && env.count < CONTRACT_ENV_MAX_PARAMS) {
                    env.names[env.count] = p->value;
                    env.args[env.count] = arg;
                    env.count++;
                }
            } else if (p->type == AST_PATTERN_LITERAL && p->value && strcmp(p->value, "_") != 0) {
                ContractTri m = literal_pattern_matches(p, arg);
                if (m == CONTRACT_FALSE) can_match = 0;
                if (m != CONTRACT_TRUE) surely = 0;
            } else if (p->type != AST_PATTERN_LITERAL) {
                surely = 0;   /* a struct or list pattern is not folded */
            }
        }
        for (int i = 0; can_match && i < clause->child_count; i++) {
            ASTNode* g = clause->children[i];
            if (!g || g->type != AST_GUARD_CLAUSE || g->child_count == 0) continue;
            ContractTri t = contract_eval_predicate(g->children[0], &env);
            if (t == CONTRACT_FALSE) can_match = 0;
            if (t != CONTRACT_TRUE) surely = 0;
        }
        if (!can_match) continue;
        if (fold_call_requires(call, clause, &env) || surely) break;
    }
    return 1;
}

static int call_yields_no_value(ASTNode* call, SymbolTable* table) {
    if (!call || call->type != AST_FUNCTION_CALL || !call->value) return 0;
    Symbol* sym = lookup_symbol(table, call->value);
    if (!sym || !sym->node) return 0;
    return fn_yields_no_value(sym->node);
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

    /* `call(f, x)` through a typed C function pointer (a `fn(int)` parameter
     * or a cast local) is the call `f(x)`. call() invokes a closure, whose C
     * shape (an environment beside the function) a function pointer does not
     * have: codegen emitted `f.fn(f.env, x)` and the C compiler stopped. */
    if (call->value && strcmp(call->value, "call") == 0 && call->child_count > 0 &&
        call->children[0] && call->children[0]->type == AST_IDENTIFIER &&
        call->children[0]->value) {
        Symbol* fs = lookup_symbol(table, call->children[0]->value);
        if (fs && !fs->is_function && fs->type && fs->type->kind == TYPE_FUNCTION &&
            fs->type->is_fnptr) {
            ASTNode* callee = call->children[0];
            free(call->value);
            call->value = strdup(callee->value);
            for (int i = 1; i < call->child_count; i++) call->children[i - 1] = call->children[i];
            call->child_count--;
            free_ast_node(callee);
        }
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
        /* #2586: codegen's ownership analyses tell such a call from a
         * direct one without its function's locals registered. */
        if (!call->annotation) call->annotation = strdup("fnptr_local_call");
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

        // The call's result type: the function-type's return slot when
        // known, else the erased-call default (#2054) -- the same rule as
        // the explicit call(f, ...) form, so both spellings type alike.
        set_node_type(call, call_builtin_result_type(call, table));
        reject_untyped_erased_args(call, table);
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
            /* #2653: a closure kept in a field, `h.cb(2)`, is `call(h.cb,
             * 2)`, as a closure local's call is `call(name, ...)` (above):
             * the collapsed callee is rebuilt as the member access and the
             * call is checked as the explicit form is. It named no
             * function, so it was "Undefined function 'h.cb'". */
            Type* ft = local_struct_field_type(table, recv, dot + 1, NULL);
            if (ft && ft->kind == TYPE_FUNCTION && !ft->is_fnptr) {
                ASTNode* obj = create_ast_node(AST_IDENTIFIER, recv, call->line, call->column);
                ASTNode* fn_ref = create_ast_node(AST_MEMBER_ACCESS, dot + 1,
                                                  call->line, call->column);
                add_child(fn_ref, obj);
                add_child(call, fn_ref);
                for (int i = call->child_count - 1; i > 0; i--)
                    call->children[i] = call->children[i - 1];
                call->children[0] = fn_ref;
                free(call->value);
                call->value = strdup("call");
                return typecheck_function_call(call, table);
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
        /* #2683: a local bound earlier in the body hides the function of
         * its name from that binding on. "Undefined function", with its
         * spelling hint, pointed away from the function defined above. */
        if (symbol && call->value && !strchr(call->value, '.')) {
            SymbolTable* root = table;
            while (root->parent) root = root->parent;
            Symbol* fn = lookup_symbol_local(root, call->value);
            if (fn && fn->is_function) {
                char hint[384];
                snprintf(error_msg, sizeof(error_msg),
                         "'%s' is a local here, which hides the function '%s'",
                         call->value, call->value);
                snprintf(hint, sizeof(hint),
                         "a local's scope starts after its initializer and lasts to the end "
                         "of the function, so the function cannot be called as '%s' here; "
                         "give the local another name",
                         call->value);
                type_error_code_hint(error_msg, hint, call->line, call->column,
                                     AETHER_ERR_UNDEFINED_FUNC);
                return 0;
            }
        }
        // Check if this is a visibility rejection (not-exported) rather than truly undefined
        if (call->value && strchr(call->value, '.') && global_module_registry) {
            char* tmp = strdup(call->value);
            char* dot = strchr(tmp, '.');
            *dot = '\0';
            char hint[512];
            if (missing_import_hint(tmp, table, hint, sizeof(hint))) {
                snprintf(error_msg, sizeof(error_msg),
                         "Undefined function '%s'", call->value);
                free(tmp);
                type_error_code_hint(error_msg, hint, call->line, call->column,
                                     AETHER_ERR_UNDEFINED_FUNC);
                return 0;
            }
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

    if (!check_quote_callee(call, symbol)) return 0;

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

        /* A block SETTER called as a top-level node BUILDER silently no-ops.
         *
         * In a builder DSL, a plain `_ctx`-first function whose body only
         * records config (e.g. `rspec(_ctx: ptr) { map.put(_ctx, ...) }`)
         * is meant to be CALLED INSIDE a builder's trailing block:
         * `mod.bundle() { rspec() }`. Written the old way — as a top-level
         * node with its OWN trailing block, `mod.rspec() { ... }` — it
         * compiles clean but runs nothing: the setter takes no closure, so
         * the block is a DSL container that is never entered.
         *
         * The check is deliberately NARROW to stay false-positive-free. A
         * legitimate DSL container (`panel(_ctx, title) { button() }`) has
         * the same shape, so we require ALL of:
         *   1. the callee is a plain AST_FUNCTION_DEFINITION (not a builder),
         *      whose first param is `_ctx: ptr` (has_ctx_first_param);
         *   2. this call carries its OWN trailing block (an AST_CLOSURE
         *      argument valued "trailing");
         *   3. the callee's module ALSO defines at least one `builder`; and
         *   4. the callee yields NO value (fn_yields_no_value on the
         *      resolved definition — `symbol` already went through the
         *      qualified lookup, which a fresh lookup by `mod.name` would not).
         * Condition 3 was the original discriminator: a widget-style DSL
         * module has no builders and is never flagged. It is not enough on
         * its own. A widget DSL can carry a few builders alongside dozens of
         * `_ctx`-first containers — aether-ui's `ui` module has `builder
         * window(...)` next to `vstack(_ctx, spacing)` — and under 3 alone
         * every `ui.vstack(10) { ... }` in every app was rejected as a
         * setter (aether-ui#147, the 0.667 pin bump). Condition 4 is the
         * semantic difference: a container RETURNS the handle its block runs
         * inside (that value is the block's `_ctx`), while a block setter
         * records config and returns nothing — which is exactly why its own
         * trailing block can never be entered. A void `_ctx`-first function
         * with a trailing block in a builder module is the misuse; one that
         * returns a value is a container whatever its module also declares. */
        if (symbol->node->type == AST_FUNCTION_DEFINITION &&
            has_ctx_first_param(symbol->node) &&
            module_defines_a_builder(symbol->node) &&
            fn_yields_no_value(symbol->node)) {
            for (int i = 0; i < call->child_count; i++) {
                ASTNode* c = call->children[i];
                if (c && c->type == AST_CLOSURE && c->value &&
                    strcmp(c->value, "trailing") == 0) {
                    /* Prefer the bare setter name (after any `mod.`) for the
                     * suggested fix; keep the qualified name for the builder
                     * hint's `mod.` prefix. */
                    const char* qual = call->value ? call->value : "function";
                    const char* bare = qual;
                    const char* dot = strrchr(qual, '.');
                    if (dot && dot[1]) bare = dot + 1;
                    char setter_msg[400];
                    snprintf(setter_msg, sizeof(setter_msg),
                             "'%s' is a block setter, not a node builder; call "
                             "it inside a builder's block (e.g. "
                             "`mod.bundle() { %s() }`), not as a top-level "
                             "`%s() { ... }`",
                             bare, bare, qual);
                    type_error(setter_msg, call->line, call->column);
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
    {
        char recv_buf[256];
        const char* dsl_recv_name = dsl_receiver_of_call(call, table, recv_buf,
                                                         sizeof(recv_buf));
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
                    reject_tuple_argument(table, call, darg, da, param_type, arg_slot + 1, param->value);
                    reject_closure_for_fnptr(table, call, darg, da, param_type, arg_slot + 1, param->value);
                    reject_struct_argument(call, darg, da, param_type, arg_slot + 1, param->value);
                    reject_pointer_argument(call, darg, da, param_type, arg_slot + 1, param->value);
                    reject_sized_array_argument(call, darg, da, param_type, arg_slot + 1, param->value);
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

            /* A bare `ptr` passed to a `T[]` (array) parameter is a silent,
             * dangerous coercion: the callee indexes it as a contiguous
             * `element*`, but a `ptr` from e.g. `string.split` is an opaque
             * handle (an AetherStringArray*, not the bare `AetherString**`), so
             * `arr[i]` reads past the struct header and segfaults with no
             * diagnostic. Reject it here and point at the `as` cast — the same
             * shape as the intarr `[]`-on-ptr error, and symmetric with the
             * array→ptr decay that IS allowed (that direction is safe). A `ptr`
             * that genuinely names the backing (e.g. `strarr.array(...)` returns
             * `string[]`, or `handle as string[]`) is already TYPE_ARRAY here
             * and passes. */
            if (param_type && param_type->kind == TYPE_ARRAY) {
                ASTNode* aarg = call->children[arg_slot];
                /* The `null` literal is not a bare ptr here: it is the empty
                 * slice (.len 0), the documented conversion slice_coerce_slot
                 * applies to a `T[]` slot. Without this, `f(null)` for an
                 * optional byte[] argument was rejected as a ptr. */
                if (aarg && aarg->type != AST_NULL_LITERAL) {
                    Type* aa = infer_type(aarg, table);
                    if (aa && aa->kind == TYPE_PTR) {
                        /* Render the array type as `elem[]` (e.g. "string[]")
                         * rather than the bare "array" type_name gives, so the
                         * message matches the source spelling. */
                        char aty[64];
                        snprintf(aty, sizeof(aty), "%s[]",
                            param_type->element_type
                                ? type_name(param_type->element_type) : "");
                        /* Sized for the fixed template (~230 chars) + three
                         * `aty` (<=64 each) + the param and call names; kept
                         * comfortably above the worst case so -Werror=format-
                         * truncation is satisfied without a runtime clamp. */
                        char emsg[768];
                        snprintf(emsg, sizeof(emsg),
                            "Argument %d '%s' of '%s': expected %s, got a bare "
                            "ptr. A ptr does not carry an array length or layout, "
                            "so indexing it as %s would read past the object and "
                            "crash. A container handle (intarr, longarr, floatarr, "
                            "strarr) is not its buffer: pass its bounded `array()` "
                            "view. Cast with `as %s` only if the ptr points at a "
                            "contiguous buffer of that type.",
                            arg_slot + 1, param->value ? param->value : "?",
                            call->value ? call->value : "?",
                            aty, aty, aty);
                        type_error(emsg, aarg->line, aarg->column);
                    }
                    if (aa) free_type(aa);
                }
            }

            /* #1286: a `T[N]` array into a `T[]` parameter becomes a slice; a
             * slice into a `ptr` parameter, or into any parameter of an
             * extern C function, decays to its pointer. */
            if (param_type)
                slice_coerce_slot(&call->children[arg_slot], param_type,
                                  symbol->node->type == AST_EXTERN_FUNCTION);

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
         * through a runtime variable: only constant arguments bind.
         *
         * A function written as several clauses folds each clause's own
         * contract, for the calls that can reach it (#2647). */
        if (!fold_clause_set_requires(call, symbol->node))
            fold_call_requires(call, symbol->node, &cf_env);
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
                    reject_tuple_argument(table, call, call->children[param_idx], arg_type,
                                          param_type, param_idx + 1, param->value);
                    reject_closure_for_fnptr(table, call, call->children[param_idx], arg_type,
                                             param_type, param_idx + 1, param->value);
                    if (arg_type && arg_type->kind != TYPE_TUPLE && arg_type->kind != TYPE_UNKNOWN &&
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
    /* #2314: `make([]T, n)` carries the `T[]` the parser stamped on it; the
     * `make` symbol's type is only a placeholder that marks it as a known
     * builtin, and stamping it here turned every make into a bare `ptr`. */
    int keeps_own_type = call->value && strcmp(call->value, "make") == 0 &&
                         call->node_type && call->node_type->kind == TYPE_ARRAY;
    if (!keeps_own_type)
        set_node_type(call, symbol->type ? clone_type(symbol->type)
                                         : create_type(TYPE_UNKNOWN));
    if (call->value && strcmp(call->value, "call") == 0) {
        set_node_type(call, call_builtin_result_type(call, table));
        reject_non_closure_callee(call, table);
        reject_untyped_erased_args(call, table);
    }

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
