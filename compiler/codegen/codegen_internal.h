#ifndef CODEGEN_INTERNAL_H
#define CODEGEN_INTERNAL_H

#include "codegen.h"
#include "../parser/lexer.h"
#include "../parser/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

/* Utilities (codegen.c) */
void indent(CodeGenerator* gen);
void unindent(CodeGenerator* gen);
void print_indent(CodeGenerator* gen);
void print_line(CodeGenerator* gen, const char* format, ...);
void codegen_maybe_emit_line(CodeGenerator* gen, const ASTNode* node);
const char* get_c_type(Type* type);
const char* const_array_elem_c_type(Type* t);
int is_c_reserved_word(const char* name);
const char* safe_c_name(const char* name);
// #976: mangle a VALUE/variable identifier that is a C reserved keyword
// (`short`, `int`, `char`, …) so it emits as a valid C identifier. Unlike
// safe_c_name (which also renames libc symbols for functions), this touches
// keywords ONLY — a local named `open` is a valid C identifier and must keep
// its spelling. Returns a static buffer; use before the next call.
int is_c_keyword(const char* name);
const char* safe_value_name(const char* name);
const char* get_c_operator(const char* aether_op);
void generate_type(CodeGenerator* gen, Type* type);
void emit_fnptr_decl(CodeGenerator* gen, Type* sig, const char* name);
int is_fnptr_type(Type* t);
int is_var_declared(CodeGenerator* gen, const char* var_name);
void mark_var_declared(CodeGenerator* gen, const char* var_name);
void truncate_declared_vars(CodeGenerator* gen, int saved_count);
void clear_declared_vars(CodeGenerator* gen);
int is_heap_box_var(CodeGenerator* gen, const char* var_name);
void mark_heap_box_var(CodeGenerator* gen, const char* var_name);
void unmark_heap_box_var(CodeGenerator* gen, const char* var_name);
int is_module_global_var(CodeGenerator* gen, const char* name);
void register_module_global_var(CodeGenerator* gen, const char* name);
int is_heap_string_var(CodeGenerator* gen, const char* var_name);
void mark_heap_string_var(CodeGenerator* gen, const char* var_name);
void clear_heap_string_vars(CodeGenerator* gen);
int is_escaped_string_var(CodeGenerator* gen, const char* var_name);
void mark_escaped_string_var(CodeGenerator* gen, const char* var_name);
void clear_escaped_string_vars(CodeGenerator* gen);
int is_return_escaped_string_var(CodeGenerator* gen, const char* var_name);
void mark_return_escaped_string_var(CodeGenerator* gen, const char* var_name);
int is_return_escaped_struct_var(CodeGenerator* gen, const char* var_name);
void mark_return_escaped_struct_var(CodeGenerator* gen, const char* var_name);

/* *StringSeq local registry (parallel to the heap-string set). A seq
 * var owns a refcounted spine freed by string_seq_free (a decrement). */
int is_seq_var(CodeGenerator* gen, const char* var_name);
void mark_seq_var(CodeGenerator* gen, const char* var_name);
void clear_seq_vars(CodeGenerator* gen);
int is_escaped_seq_var(CodeGenerator* gen, const char* var_name);
void mark_escaped_seq_var(CodeGenerator* gen, const char* var_name);
int is_opt_str_var(CodeGenerator* gen, const char* var_name);
void mark_opt_str_var(CodeGenerator* gen, const char* var_name);
void clear_opt_str_vars(CodeGenerator* gen);
int is_escaped_opt_str_var(CodeGenerator* gen, const char* var_name);
void mark_escaped_opt_str_var(CodeGenerator* gen, const char* var_name);
/* Classifier: does the expression produce an OWNED *StringSeq (a fresh
 * ref the caller must free), vs a borrowed spine pointer (seq_tail)?
 * Defined in codegen_stmt.c. */
int is_seq_owning_expr(CodeGenerator* gen, ASTNode* expr);

/* Classifier: does the expression produce a heap-allocated string?
 * Defined in codegen_stmt.c. Used by the codegen_expr.c argument-
 * temp lifetime wrap (ArgDrainSub) to detect heap-returning
 * function calls in argument position. */
int is_heap_string_expr(CodeGenerator* gen, ASTNode* expr);
/* Whether the discarded error slot of an `or`-expression's fallible is a
 * heap-owned string the `or` lowering must release (vs a raw literal it
 * must not free). See the definition in codegen_stmt.c. */
int or_fallible_error_slot_is_heap(CodeGenerator* gen, ASTNode* fallible);
/* Whether an `or`-expression's fallible value slot (position 0) is a
 * heap-owned string — gates uniform-heap boxing of the `or` result so
 * the yielded value is freed at scope exit. See codegen_stmt.c. */
int or_fallible_value_slot_is_heap(CodeGenerator* gen, ASTNode* fallible);

/* Returns 1 if some `AST_VARIABLE_DECLARATION` for `var_name` inside
 * `node` assigns it from a heap-string source (`is_heap_string_expr`
 * of the RHS). A structural "is this variable ever heap-valued"
 * query — coarser than the runtime `_heap_<name>` flag but reliable
 * for escaped variables, whose flag the reassignment wrapper stops
 * maintaining. Skips nested function / closure scopes. Defined in
 * codegen_stmt.c; used by the map/list owned-value routing in
 * codegen_expr.c. */
int body_assigns_var_from_heap(CodeGenerator* gen, ASTNode* node,
                               const char* var_name);

/* Escape gate for heap-string arguments: returns 1 if a callee's
 * parameter slot of the given type-kind is treated as storage (the
 * recipient may stash the pointer beyond the call). Used by the
 * escape walker (codegen_stmt.c) AND by the codegen_expr.c
 * argument-temp lifetime wrap to decide whether freeing a hoisted
 * heap argument is safe. Defined in codegen_stmt.c. */
int call_arg_escapes(TypeKind param_kind);

/* Resolve a callee's parameter kind by name + index. Consults the
 * extern registry first, then the user-fn definitions in
 * gen->program. Returns TYPE_UNKNOWN when the callee can't be
 * resolved or the index is out of range. Defined in codegen_stmt.c. */
TypeKind lookup_callee_param_kind(CodeGenerator* gen, const char* func_name, int param_idx);

/* Interprocedural escape: does parameter `param_idx` of user function
 * `func_name` flow into an escaping sink within its body (a container
 * put / `@retain` / `ptr` callee param, transitively, or a return)?
 * Catches storing wrappers with `string`-typed value params that
 * `call_arg_escapes` alone would misjudge as read-only — the map-value
 * use-after-free. `depth` is the recursion guard (start at 0). Defined
 * in codegen_stmt.c. */
int callee_param_escapes_via_body(CodeGenerator* gen, const char* func_name, int param_idx, int depth);

/* For the call-site identity-drain (codegen_expr.c). `_store_escapes`:
 * does the callee's param escape via a STORE (anything but being directly
 * returned)? If so the value is owned by a sink and must NOT be freed.
 * `_returns_string`: does the callee declare `-> string` (so the result
 * pointer can be identity-compared against the passed temp)? Defined in
 * codegen_stmt.c. */
int callee_param_store_escapes_via_body(CodeGenerator* gen, const char* func_name, int param_idx);
int callee_returns_string(CodeGenerator* gen, const char* func_name);

/* True when `func_name` resolves to a user function with a visible body
 * block; only then may the body-walk override the conservative
 * call_arg_escapes heuristic. Defined in codegen_stmt.c. */
int callee_has_visible_body(CodeGenerator* gen, const char* func_name);

/* Normalise a callee name's dots to underscores, writing into `out`
   and returning `out`. The AST stores source-level callees in dotted
   form (`"string.concat"`) but stdlib externs, the generated C call
   sites, and the various callee registries (heap-string allowlist,
   builder-funcs registry, extern param-type table) all use the
   underscored form. Use this whenever you're about to look up by
   callee name. `out` must hold at least 256 bytes. */
const char* codegen_normalise_callee(const char* raw, char* out, size_t out_size);

/* Defer management (codegen.c) */
void push_defer(CodeGenerator* gen, ASTNode* stmt);
void push_defer_mode(CodeGenerator* gen, ASTNode* stmt, DeferMode mode);  /* #1140 */
void push_auto_defer(CodeGenerator* gen, const char* free_fn, const char* var_name);
void emit_defers_for_scope(CodeGenerator* gen);
void emit_defers_through_scope(CodeGenerator* gen, int floor_depth);
void emit_all_defers(CodeGenerator* gen);
void emit_all_defers_protected(CodeGenerator* gen, char** protected_names, int protected_count);
void enter_scope(CodeGenerator* gen);
void exit_scope(CodeGenerator* gen);

/* Expression generation (codegen_expr.c) */
void generate_expression(CodeGenerator* gen, ASTNode* expr);

/* Emit `call` so a transient capturing-closure argument's heap env is
 * freed after the call (codegen_expr.c). Caller must verify the receiving
 * parameter does not store/return the closure. */
void emit_closure_env_drained_call(CodeGenerator* gen, ASTNode* call,
                                   ASTNode* closure_node);

/* Message field helpers (codegen_expr.c) — shared with codegen_stmt.c */
MessageFieldDef* find_msg_field(MessageDef* msg_def, const char* name);
void emit_message_field_init(CodeGenerator* gen, MessageFieldDef* fdef, ASTNode* rhs);
void emit_message_array_hoists(CodeGenerator* gen, ASTNode* message, MessageDef* msg_def);

/* Statement generation (codegen_stmt.c) */
void generate_statement(CodeGenerator* gen, ASTNode* stmt);
/* Hoist variables first-declared inside if-statement branches at the
   enclosing function-body scope when they're referenced outside the
   if-block. Closes #278. Called from codegen_func.c before iterating
   the body's top-level statements. */
void hoist_if_branch_vars(CodeGenerator* gen, ASTNode* body);

/* Issue #348 — Eiffel-style runtime contracts. Emit pre- /
   postcondition checks for `requires` / `ensures` clauses attached
   to a function as AST_REQUIRES_CLAUSE / AST_ENSURES_CLAUSE
   children. Both are no-ops when CodeGenerator::no_contracts is set
   (matches C's -DNDEBUG for assert).

   emit_contract_preconditions is called once at function entry,
   immediately after parameters are declared and before the body.

   emit_contract_postconditions is called by AST_RETURN_STATEMENT
   codegen after writing `<T> result = <expr>;` into a fresh C scope
   so the predicate's `result` identifier resolves to the local.
   Returns 1 if any check was emitted (the caller uses this to
   decide whether to route through the result-local wrapper).
*/
void emit_contract_preconditions(CodeGenerator* gen, ASTNode* func);
int  emit_contract_postconditions(CodeGenerator* gen, ASTNode* func);

/* Hoist `_heap_<name>` companion trackers for every string variable
   in `body` to function-entry scope. Closes #405 — the architectural
   blocker that kept user-defined `-> string` functions from
   participating in the heap-string-reassignment wrapper. Called from
   codegen_func.c immediately after hoist_if_branch_vars.

   The structural escape analysis that recognises user-defined
   `-> string` functions reads `gen->program` to look up callee
   bodies; the call site of `hoist_heap_string_trackers` (and the
   per-stmt call sites of the underlying `is_heap_string_expr`)
   therefore must not run before generate_program populates that
   field. All current call sites are inside the function-body
   codegen path which runs after that point. */
void hoist_heap_string_trackers(CodeGenerator* gen, ASTNode* body);

/* Walk `body` and mark every heap-string variable whose value escapes
   — i.e. is passed as an argument in a function/method call (in any
   context other than the RHS of `V = ...` whose LHS is V), or
   captured by a closure body. The wrapper at codegen_stmt.c:1611
   skips its `free(_tmp_old)` for escaped vars to avoid dangling
   pointers stored on the recipient side (map.put values, list.add
   elements, struct fields, actor message fields, closure captures).
   Conservative: alias-safe at the cost of leaking the value over
   the function's lifetime. Run after hoist_heap_string_trackers so
   the heap-string-var registry is populated. */
void mark_escaped_heap_string_vars(CodeGenerator* gen, ASTNode* body);

/* Push function-exit defer-free statements for every hoisted
 * heap-string var that's NOT escaped. Closes the single-call
 * leak shape: a heap-string variable assigned once and never
 * reassigned still has a live allocation when the function
 * exits; without this defer it leaks per-call. Run after
 * mark_escaped_heap_string_vars (so escape state is final)
 * and before body codegen. See codegen_stmt.c for the
 * implementation rationale (issue #420 follow-up). */
void push_heap_string_exit_free_defers(CodeGenerator* gen, ASTNode* body);

/* *StringSeq local lifecycle (parallel to the heap-string passes). */
void hoist_seq_trackers(CodeGenerator* gen, ASTNode* body);
void mark_escaped_seq_vars(CodeGenerator* gen, ASTNode* body);
void push_seq_exit_free_defers(CodeGenerator* gen, ASTNode* body);

/* `string?` heap-ownership lifecycle (parallel to the seq passes). */
void hoist_opt_str_trackers(CodeGenerator* gen, ASTNode* body);
void mark_escaped_opt_str_vars(CodeGenerator* gen, ASTNode* body);
void push_opt_str_exit_free_defers(CodeGenerator* gen, ASTNode* body);

/* Actor generation (codegen_actor.c) */
void generate_actor_definition(CodeGenerator* gen, ASTNode* actor);

/* Extern function registry — tracks param types for call-site cast emission */
void register_extern_func(CodeGenerator* gen, ASTNode* ext);
int is_extern_func(CodeGenerator* gen, const char* func_name);

/* Typed fn-pointer locals: variables whose source-level type is
 * `fn(T1, T2, ...) -> R` (storage = void*; call-site emits typed cast). */
void register_fnptr_local(CodeGenerator* gen, const char* name, Type* sig);
Type* lookup_fnptr_local(CodeGenerator* gen, const char* name);
TypeKind lookup_extern_param_kind(CodeGenerator* gen, const char* func_name, int param_idx);
/* Full Type* for an extern's parameter (borrowed from the extern's AST),
 * or NULL. The kind alone can't drive tuple-param emission — packing the
 * by-value `_tuple_*` struct literal needs the element list (#1033). */
Type* lookup_extern_param_type(CodeGenerator* gen, const char* func_name, int param_idx);
int is_aether_extern_param(CodeGenerator* gen, const char* func_name, int param_idx);
/* Returns 1 if extern `func_name`'s parameter at `param_idx` was
   declared `@retain`. Tells the escape walker to mark a heap-string
   arg passed at this slot as escaped (function stores / retains the
   pointer beyond the call). 0 for non-extern callees, missing
   annotations, or out-of-range index. See codegen_func.c. */
int is_retain_extern_param(CodeGenerator* gen, const char* func_name, int param_idx);
const char* lookup_extern_c_name(CodeGenerator* gen, const char* func_name);

/* Builder function registry — functions where block configures first, then function executes */
int is_builder_func_reg(CodeGenerator* gen, const char* func_name);
const char* get_builder_factory(CodeGenerator* gen, const char* func_name);

/* Bare-fn → fn-typed-slot adapter registry (ASK 3). At every bare-fn
 * wrap site we call register_bare_fn_adapter(name); at file
 * finalisation we call emit_bare_fn_adapters() to dump the resulting
 * env-ignoring forwarder functions. See codegen.h field comment for
 * the bug-shape rationale. */
int  register_bare_fn_adapter(CodeGenerator* gen, const char* bare_fn_name);
void emit_bare_fn_adapters(CodeGenerator* gen);
/* #943: forward-declare the adapters before closure bodies (which may call
 * them) are emitted, so the closure functions see the prototype in scope. */
void emit_bare_fn_adapter_decls(CodeGenerator* gen);
void discover_bare_fn_adapters(CodeGenerator* gen);

/* Function/struct generation (codegen_func.c) */
int has_return_value(ASTNode* node);

/* Struct-field heap-string ownership (#465). The struct typedef
 * emitter (generate_struct_definition) appends a hidden
 * `int _heap_<field>` tracker per `string`-typed field, and a
 * `<Name>_destroy()` function. The statement-codegen consumes
 * these helpers at struct-local declaration sites (push the
 * scope-exit destroy defer) and at field-write sites (emit the
 * reassign-wrapper free). */
int struct_has_heap_string_field(ASTNode* struct_def);
ASTNode* find_struct_definition_by_name(ASTNode* program, const char* name);

/* @c_callback annotation helpers (#235). A function declared with
   `@c_callback aether_name(...)` (or `@c_callback("c_sym") aether_name(...)`)
   gets a stable, externally-visible C symbol so it can be passed across
   module boundaries as a function pointer to C externs. Codegen drops
   the `static` storage class for these even when imported, and uses
   the chosen C symbol at the C-decl name slot and at every value-
   position reference. */
int is_c_callback(ASTNode* func);
const char* c_callback_symbol(ASTNode* func);
/* Whether a top-level function is emitted `static`. All three emit sites
   (definition, combined multi-clause definition, forward declaration) must
   agree or C rejects the file with "static declaration follows non-static
   declaration". #1366 */
int fn_has_internal_linkage(ASTNode* func);
/* Look up a top-level @c_callback function by its current AST value
   (after any import-rename pass) and return its bound C symbol — or
   NULL when no such callback exists. Used at value-position
   AST_IDENTIFIER emission so a function name passed as a function
   pointer resolves to the linked symbol, not the Aether-side name. */
const char* lookup_c_callback_symbol(CodeGenerator* gen, const char* name);

/* @c_import struct registry (codegen.c).  Populated in the pre-scan
 * of the program so every later `*StructName` emit site can ask "do I
 * need to write `struct N*` instead of `N*`?".  Bare `N*` requires a
 * `typedef struct N N;` somewhere in scope; with `@c_import` aetherc
 * emits no typedef, and headers like <time.h> for `struct tm` don't
 * ship one either, so `N*` doesn't compile.  `struct N*` is the
 * universally-portable form. */
void aether_register_c_import_struct(const char* name);
int aether_is_c_import_struct(const char* name);

/* #891 @c_struct typed overlay registry. A @c_struct lowers field access to
 * width-correct mem_get_* / set_* at explicit offsets (pure Aether, no C struct).
 * Registered from the AST_C_STRUCT_DEF nodes before codegen; queried at member
 * access to pick the accessor. */
void aether_register_c_struct(const char* name);
int  aether_is_c_struct_overlay(const char* name);
void aether_c_struct_add_field(const char* sname, const char* fname,
                               const char* width, long offset, const char* nested);
/* Resolve struct.field (dotted for nested) -> cumulative offset + leaf width
 * token. Returns 1 on success, 0 if unknown. */
int  aether_c_struct_resolve(const char* sname, const char* field,
                             long* out_offset, const char** out_width);
/* Flatten a member-access chain to its overlay-pointer root receiver +
 * dotted field path; NULL if root isn't a @c_struct overlay. */
ASTNode* aether_c_struct_chain(ASTNode* macc, char* out, size_t outsz);
/* Predicate form of the above: is this member-access an overlay access? */
int aether_c_struct_overlay_lhs(ASTNode* macc);

/* #1132 bitstruct registry: a named bit layout over one unsigned integer.
 * Field access lowers to shift/mask on the backing word — never to a C bitfield,
 * whose signedness and layout are implementation-defined. */
void aether_register_bitstruct(const char* name, const char* backing);
int  aether_is_bitstruct(const char* name);
void aether_bitstruct_add_field(const char* sname, const char* fname,
                                int lo, int hi, int is_bool);
/* Resolve bitstruct.field -> its INCLUSIVE bit range (+ backing C type).
 * Returns 1 on success, 0 if unknown. */
int  aether_bitstruct_resolve(const char* sname, const char* field,
                              int* out_lo, int* out_hi, int* out_is_bool,
                              const char** out_backing);
/* The bitstruct name a member-access reads from, or NULL if its base isn't one. */
const char* aether_bitstruct_base_name(ASTNode* macc);
/* All-ones mask for an inclusive [lo,hi] bit range. */
unsigned long long aether_bitstruct_mask(int lo, int hi);

void generate_extern_declaration(CodeGenerator* gen, ASTNode* ext);
void generate_function_definition(CodeGenerator* gen, ASTNode* func);
void generate_struct_definition(CodeGenerator* gen, ASTNode* struct_def);
void generate_combined_function(CodeGenerator* gen, ASTNode** clauses, int clause_count);

/* Main/program (codegen.c) */
void generate_main_function(CodeGenerator* gen, ASTNode* main);

/* Closure support (codegen_expr.c) */
void discover_closures(CodeGenerator* gen, ASTNode* node);
void emit_closure_definitions(CodeGenerator* gen);
/* L4 validation: reject closures inside actor handlers that write to
   actor state fields. Run after discover_closures, before codegen.
   Returns 1 on success, 0 if errors were reported. */
int validate_closure_state_mutations(CodeGenerator* gen, ASTNode* program);
/* Route 1 promotion queries (populated by discover_closures): */
void get_promoted_names_for_func(CodeGenerator* gen, const char* func_name,
                                 char*** out_names, int* out_count);
int is_promoted_capture(CodeGenerator* gen, const char* name);

/* Internal helpers shared across files */
int contains_send_expression(ASTNode* node);
const char* get_single_int_field(MessageDef* msg_def);
void generate_default_return_value(CodeGenerator* gen, Type* type);
int is_function_generated(CodeGenerator* gen, const char* func_name);
void mark_function_generated(CodeGenerator* gen, const char* func_name);
int count_function_clauses(ASTNode* program, const char* func_name);
ASTNode** collect_function_clauses(ASTNode* program, const char* func_name, int* out_count);

#endif
