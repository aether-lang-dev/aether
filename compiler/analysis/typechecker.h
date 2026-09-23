#ifndef TYPECHECKER_H
#define TYPECHECKER_H

#include <stdio.h>   /* FILE — used by emit_effects_json (#889) */
#include "../ast.h"

typedef struct Symbol {
    char* name;
    Type* type;
    int is_actor;
    int is_function;
    int is_state;
    int is_module_alias;        // Indicates this is a module alias
    char* alias_target;         // The actual module name for aliases
    ASTNode* node;  // Pointer to AST node (for structs, functions, etc.)
    int type_inferred;          // 1 if this binding's type was inferred
                                // from a bare `x = expr` declaration (no
                                // annotation) and is a 32-bit int — drives
                                // the #698 silent-narrowing guard.
    int width_explicit;         // #869: 1 if this binding was declared with an
                                // EXPLICIT integer-type annotation (`uint64 v
                                // = 0`). Set once at declaration and never
                                // cleared by the width-inference pass (unlike
                                // `type`/`type_inferred`, which it mutates), so
                                // a later bare re-bind can preserve the declared
                                // width instead of re-narrowing it.
    // #2124: for a local the early inference pass parks in its flat table,
    // the function (or main / closure) body it was bound in. A later
    // binding of the same name in the SAME body widens the entry (the
    // hoisted-local join); one from another body replaces it, as it
    // always did — names are reused freely across functions.
    void* inferred_in;
    // #2173: which type-inference walk added this symbol (0: none). A walk
    // may refine only the symbols it added itself; anything else it must
    // shadow, because the unwind at its end can remove what it added but not
    // restore what it overwrote. A stamp makes that check O(1) where walking
    // the scope's list made rebinding quadratic in a function's locals.
    unsigned walk_id;
    struct Symbol* next;
    // #2007: chain within the scope's hash bucket. A symbol is at the head
    // of its bucket chain exactly when it is the newest of its name in the
    // scope, which is the one lookup_symbol_local must return.
    struct Symbol* hash_next;
} Symbol;

// Linked list of identifier names, used for hide / seal-except sets.
typedef struct NameNode {
    char* name;
    struct NameNode* next;
} NameNode;

typedef struct SymbolTable {
    Symbol* symbols;
    struct SymbolTable* parent;
    // #2007: hash index over `symbols`, so a lookup is O(1) rather than a
    // walk of the whole list. The file scope holds every top-level function,
    // and every identifier resolution that misses a local scope walked it,
    // which made type checking quadratic in the number of functions. Kept in
    // step with `symbols` by add_symbol / add_module_alias / pop_symbol;
    // nothing else may splice the list. NULL until the scope has enough
    // symbols for the index to pay for itself.
    Symbol** buckets;
    int bucket_count;
    int symbol_count;
    // Hide / seal directives that apply to this scope. They affect
    // lookups that would otherwise resolve to the parent chain — local
    // bindings in `symbols` are always visible regardless.
    NameNode* hidden_names;       // names blocked from outer scopes
    NameNode* seal_whitelist;     // if non-NULL, ONLY these names may resolve to outer scopes
    int is_sealed;                // 1 if a `seal except` directive is in effect (whitelist may be empty)
    // Issue #243 sealed-scope follow-up: 1 when this scope (or any
    // ancestor) is the body of a function that was cloned in from a
    // transitively-merged module via module_merge_into_program. Such
    // bodies legitimately need to call into other transitively-merged
    // namespaces (e.g. a cloned `client_post_json` calls
    // `json.stringify(...)`) even though the user never wrote
    // `import std.json` themselves. User code (where this flag is 0)
    // can only resolve qualified calls to namespaces it explicitly
    // imported. Propagated from parent in create_symbol_table.
    int inside_merged_body;
    // Issue #333 DSL block receiver scoping: when this scope is the
    // body of a trailing closure bound to a member-access call
    // (`receiver.method(args) { body }`), this names the receiver's
    // namespace (or struct type) so unqualified calls inside `body`
    // can fall back to `<dsl_receiver>_<name>` before erroring as
    // undefined. NULL when the scope is not a DSL block body.
    // Resolution order in lookup_symbol: local, then this fallback,
    // then parent chain. Local definitions still win; existing
    // qualified calls and selective imports unchanged.
    char* dsl_receiver;
} SymbolTable;

// Symbol table functions
SymbolTable* create_symbol_table(SymbolTable* parent);
void free_symbol_table(SymbolTable* table);
void add_symbol(SymbolTable* table, const char* name, Type* type, int is_actor, int is_function, int is_state);
Symbol* lookup_symbol(SymbolTable* table, const char* name);
Symbol* lookup_symbol_local(SymbolTable* table, const char* name);
/* #2007: unlink and free the newest symbol of `table`. The one way to remove
 * a symbol, so the hash index stays in step with the list. */
void pop_symbol(SymbolTable* table);

// Hide / seal directive helpers
void scope_hide_name(SymbolTable* table, const char* name);
void scope_seal_except(SymbolTable* table, const char* name);
int  scope_name_is_hidden(SymbolTable* table, const char* name);
int  scope_name_in_whitelist(SymbolTable* table, const char* name);

// Module alias functions
void add_module_alias(SymbolTable* table, const char* alias, const char* module_name);
Symbol* resolve_module_alias(SymbolTable* table, const char* name);
Symbol* lookup_qualified_symbol(SymbolTable* table, const char* qualified_name);

// Namespace-visibility helper (issue #243). Returns 1 if a qualified
// call `<name>.<member>` is allowed from a scope whose `table` is
// passed in. Merged-body scopes (table->inside_merged_body) see all
// transitively-merged namespaces; user code only sees explicit
// imports.
int is_visible_namespace(const char* name, SymbolTable* table);
/* Is `name` a builtin codegen lowers by name (`println`, `exit`, `sleep`,
 * ...)? Such a name belongs to no module and needs no import. */
int is_builtin_function_name(const char* name);

// Type checking functions
int typecheck_program(ASTNode* program);
int typecheck_node(ASTNode* node, SymbolTable* table);
int typecheck_actor_definition(ASTNode* actor, SymbolTable* table);
int typecheck_function_definition(ASTNode* func, SymbolTable* table);
int typecheck_struct_definition(ASTNode* struct_def, SymbolTable* table);
int typecheck_statement(ASTNode* stmt, SymbolTable* table);
int typecheck_expression(ASTNode* expr, SymbolTable* table);
int typecheck_binary_expression(ASTNode* expr, SymbolTable* table);
int typecheck_function_call(ASTNode* call, SymbolTable* table);

// Type inference functions
Type* infer_type(ASTNode* expr, SymbolTable* table);
Type* infer_binary_type(ASTNode* left, ASTNode* right, AeTokenType operator);
Type* infer_unary_type(ASTNode* operand, AeTokenType operator);

// Type compatibility functions
int is_type_compatible(Type* from, Type* to);
int is_assignable(Type* from, Type* to);
int is_callable(Type* type);

// Utility functions
AeTokenType get_token_type_from_string(const char* str);

// Error reporting
void type_error(const char* message, int line, int column);
void type_warning(const char* message, int line, int column);

// #889: emit the DERIVED per-function effect/purity analysis as JSON to `out`.
// For each top-level user function, reports the whole-program transitive
// capability reachability (fs/net/os) and purity — computed from the call
// graph + capability classification, NOT from author-written @no_* tags (which
// an attacker could simply omit). Fail-closed: a function that transitively
// reaches a raw `extern` is treated as reaching every capability (`"extern":
// true`), matching how the `--with=` gate treats unclassifiable externs.
// Consumed by external auditors (e.g. aeb's supply-chain veto).
void emit_effects_json(FILE* out, ASTNode* program);

#endif
