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

/* Names codegen builds as it goes (C types, mangled and normalised names,
 * cell declarations, field paths): each spelling kept once, for the life of
 * the process, so a name is never cut to a buffer's size and a pointer
 * handed out stays valid however many more are built (#2539). Out of memory
 * ends the compile, as aether_xrealloc does. */
const char* cg_intern(const char* s);
const char* cg_intern_n(const char* s, size_t n);
const char* cg_internf(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/* #2765: an expression's temporaries (codegen_stmt.c). A construct that
 * needs some (an argument freed after the call, a string taken into an
 * owning slot) opens a TempSeq and writes its parts once:
 *
 *     seq_open; t = seq_let(ctype, prefix); <value>; seq_next; <use of t>;
 *     seq_next; ...; <result>; seq_close
 *
 * While a statement is being emitted, that is the comma expression
 * `(t = <value>, <use>, <result>)`, its temporaries declared ahead of the
 * statement: plain C, which every C compiler takes. With no statement to
 * declare them ahead of, it is the GNU statement expression
 * `({ T t = <value>; <use>; <result>; })`, which only a GCC-compatible
 * compiler takes. A part is an expression in both: a conditional is
 * `c ? a : b`, not an `if` statement. An ordered sequence (an operand
 * evaluated ahead, #2478) infers its temporaries' types with __auto_type on
 * the GCC path and spells them elsewhere, through the _AE_SEQ_* macros.
 * seq_decl declares a temporary the parts assign on their own (each arm of
 * a conditional). */
typedef struct { int stmt_expr; int ordered; } TempSeq;
int hoist_ready(CodeGenerator* gen);
void seq_open(CodeGenerator* gen, TempSeq* s);
void seq_open_ordered(CodeGenerator* gen, TempSeq* s, int typed);
const char* seq_let(CodeGenerator* gen, TempSeq* s, const char* ctype, const char* prefix);
void seq_let_named(CodeGenerator* gen, TempSeq* s, const char* ctype, const char* name);
void seq_decl(CodeGenerator* gen, TempSeq* s, const char* ctype, const char* name);
void seq_next(CodeGenerator* gen, TempSeq* s);
void seq_close(CodeGenerator* gen, TempSeq* s);
/* A temporary `ctype name;` declared ahead of the statement being emitted
 * (hoist_ready), for both compilers. */
void hoist_declare(CodeGenerator* gen, const char* ctype, const char* name);
/* hoist_declare with a NULL initializer, and the scope-exit release of such
 * a temporary that lives to its statement's end (call_returns_view). */
void hoist_declare_null(CodeGenerator* gen, const char* ctype, const char* name);
void hoist_append_text(CodeGenerator* gen, const char* text);
void push_scoped_temp_free(CodeGenerator* gen, const char* name);
/* Where declarations go ahead of a statement, judged by the `n` bytes of C
 * before it: 0 there, 1 after a null statement (a label precedes), 2 in a
 * block with the statement (an unbraced body). */
int codegen_hoist_site(const char* s, size_t n);
/* #2765: a construct that runs statements (an `or` with a block handler, a
 * propagating `!`, a call with a trailing block whose value is used), run
 * ahead of its statement, its value bound for the statement to read
 * (codegen_expr.c). */
int is_control_value(CodeGenerator* gen, ASTNode* e);
int emit_control_prelude(CodeGenerator* gen, ASTNode* e, int discarded);
/* #2798: the same anywhere in a statement: does `e` hold one, and run the
 * ones a value or a list of operands holds, with what is evaluated before
 * them, ahead of the statement (1 when every one is bound). */
/* #2478, #2813: the memoised call effects (codegen_expr.c), freed. */
void order_fn_effects_release(CodeGenerator* gen);
int ctl_contains(CodeGenerator* gen, ASTNode* e);
int ctl_linearize_value(CodeGenerator* gen, ASTNode* e, int discarded);
int ctl_linearize_list(CodeGenerator* gen, ASTNode** ops, int n);
/* The arguments of a builder call, evaluated after its block, left to
 * right, ahead of the call. */
void ctl_builder_args(CodeGenerator* gen, ASTNode* call);
/* A builder call whose block has run, as `fn(args..., cfg)` through the
 * ordinary call emitter; and whether `call` is the one being emitted so. */
void emit_builder_call(CodeGenerator* gen, ASTNode* call, const char* cfg);
int builder_call_configured(ASTNode* call);
typedef struct {
    ASTNode* trailing_stmt_call;
    ASTNode* call;
    const char* var;
} BuilderCfg;
BuilderCfg builder_cfg_begin(CodeGenerator* gen, ASTNode* call, const char* cfg);
void builder_cfg_end(CodeGenerator* gen, BuilderCfg saved);
/* `<lhs> = <value>;` as a statement of its own, outside any Aether
 * statement (an actor state's initializer): temporaries and constructs as
 * generate_statement handles a statement's. */
void generate_value_statement(CodeGenerator* gen, const char* lhs, ASTNode* value, Type* t);
const char* c_member_decl(Type* t, const char* name);
int try_emit_cell_release(CodeGenerator* gen, ASTNode* deferred);
int builder_never_uses_config(ASTNode* builder);
void own_rebound_binding_take(CodeGenerator* gen, const char* name, Type* t, ASTNode* body,
                              int line, int column);
void push_builder_config_exit_release(CodeGenerator* gen, ASTNode* builder, ASTNode* body);
const char* user_defer_ctx(ASTNode* body);
void emit_defer_thunks(CodeGenerator* gen);
int control_prelude_depth(void);
void control_prelude_end(int depth);

/* Utilities (codegen.c) */
void indent(CodeGenerator* gen);
void unindent(CodeGenerator* gen);
void print_indent(CodeGenerator* gen);
void print_line(CodeGenerator* gen, const char* format, ...);
void codegen_maybe_emit_line(CodeGenerator* gen, const ASTNode* node);
const char* get_c_type(Type* type);
const char* const_array_elem_c_type(Type* t);
/* The C element type of a lane vector C's own operators apply to (f32x4,
 * f64x2, i32x4, i64x2, i16x8), NULL otherwise; and whether a scalar of `kind`
 * splats across one (converted to that element type first). */
const char* lane_element_c_type(TypeKind kind);
int is_scalar_splat_kind(TypeKind kind);
int is_c_reserved_word(const char* name);
const char* safe_c_name(const char* name);
// #976: mangle a VALUE/variable identifier that is a C reserved keyword
// (`short`, `int`, `char`, …) so it emits as a valid C identifier. Unlike
// safe_c_name (which also renames libc symbols for functions), this touches
// keywords ONLY — a local named `open` is a valid C identifier and must keep
// its spelling. Both return an interned name (cg_intern), valid for good.
int is_c_keyword(const char* name);
const char* safe_value_name(const char* name);
const char* get_c_operator(const char* aether_op);
void generate_type(CodeGenerator* gen, Type* type);
void emit_fnptr_decl(CodeGenerator* gen, Type* sig, const char* name);
const char* fnptr_c_spelling(Type* sig, const char* name);   /* #2651 */
int is_fnptr_type(Type* t);
int fn_is_inline_candidate(ASTNode* func);
int is_var_declared(CodeGenerator* gen, const char* var_name);
void mark_var_declared(CodeGenerator* gen, const char* var_name);
void mark_var_declared_typed(CodeGenerator* gen, const char* var_name, Type* hoisted_type);
Type* declared_var_type(CodeGenerator* gen, const char* var_name);
void truncate_declared_vars(CodeGenerator* gen, int saved_count);
void clear_declared_vars(CodeGenerator* gen);
void clear_fnptr_locals(CodeGenerator* gen);
int is_module_global_var(CodeGenerator* gen, const char* name);
int is_actor_state_var(CodeGenerator* gen, const char* name);   /* #2505 */
void register_module_global_var(CodeGenerator* gen, const char* name, int is_program);
int is_heap_string_var(CodeGenerator* gen, const char* var_name);
void mark_heap_string_var(CodeGenerator* gen, const char* var_name);
void unmark_heap_string_var(CodeGenerator* gen, const char* var_name);
void clear_heap_string_vars(CodeGenerator* gen);
int heap_string_var_try_depth(CodeGenerator* gen, const char* var_name);         /* #2690 */
int set_heap_string_var_try_depth(CodeGenerator* gen, const char* var_name, int depth);

/* *StringSeq local registry (parallel to the heap-string set). A seq
 * var owns a refcounted spine freed by string_seq_free (a decrement). */
/* Classifier: does the expression produce an OWNED *StringSeq (a fresh
 * ref the caller must free), vs a borrowed spine pointer (seq_tail)?
 * Defined in codegen_stmt.c. */
int seq_value_is_fresh(CodeGenerator* gen, ASTNode* e);
void emit_seq_take(CodeGenerator* gen, ASTNode* e);

/* Classifier: does the expression produce a heap-allocated string?
 * Defined in codegen_stmt.c. Used by the codegen_expr.c argument-
 * temp lifetime wrap (ArgDrainSub) to detect heap-returning
 * function calls in argument position. */
int is_heap_string_expr(CodeGenerator* gen, ASTNode* expr);
/* #2750: a call of an Aether function returning `string?` (handed over
 * owned, #2740), and `!` / `??` over one; a heap string nobody else holds
 * (a call, an interpolation, an `or`, such an unwrap) that the expression
 * reading it frees; and whether a `string?` argument is free after a call. */
int is_owned_opt_string_call(CodeGenerator* gen, ASTNode* e);
int is_owned_string_unwrap(CodeGenerator* gen, ASTNode* e);
int is_fresh_heap_string_temp(CodeGenerator* gen, ASTNode* e);

/* Does evaluating the expression have a side effect (a call, a send, a
 * va_* op)? Defined in codegen_expr.c. Gates the series-collapse
 * optimizer in codegen_stmt.c and the source-order hoist of string-
 * interpolation segments (#2195). */
int codegen_expr_has_side_effects(ASTNode* node);

/* String-interpolation segment classifiers (#2195), defined in
 * codegen_expr.c and unit-tested directly. */
int interp_segment_is_text(ASTNode* ch);
int interp_segment_is_heap_call(CodeGenerator* gen, ASTNode* ch);
int interp_order_hoist_boundary(ASTNode* interp);
const char* interp_temp_c_type(Type* t);
/* #2478: the elements of an initializer list (an array literal, a returned
 * tuple) evaluated into temps, in source order, ahead of the statement the
 * list feeds; generate_statement releases the bindings when the statement
 * ends (order_prelude_end of the depth it saw). See codegen_expr.c. */
void order_prelude_begin(CodeGenerator* gen, ASTNode** items, int n, const char* target);
/* #2513: closure_var_map, by scope and name (see codegen_expr.c). */
int closure_var_id(CodeGenerator* gen, const char* scope, const char* name);
void closure_var_bind(CodeGenerator* gen, const char* scope, const char* name, int cid);
int order_prelude_depth(void);
void order_prelude_end(int depth);
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
 * query. Skips nested function / closure scopes. Defined in
 * codegen_stmt.c. */

/* #2461: how an owning string slot (a local, a struct field, a return value)
 * takes a value that may view a buffer someone else owns: a struct field
 * read, or an `if` / `match` whose arm is one or is a heap-tracked local. See
 * the comment above is_owned_string_field_read in codegen_stmt.c. */
enum {
    STR_TAKE_BORROW = 0,    /* every arm is borrowed (literal, parameter) */
    STR_TAKE_OWNED = 1,     /* every arm hands the slot a buffer to free */
    STR_TAKE_RUNTIME = 2    /* depends on the arm taken at run time */
};
int is_owned_string_field_read(ASTNode* e);
/* The C symbol `@extern("sym")` binds an extern to, or NULL. */
const char* extern_c_symbol(const ASTNode* ext);
/* The system header that declares a libc function whose `extern`
 * declaration codegen skips, or NULL when the prelude includes it (#2676). */
const char* libc_extern_header(const char* name);
/* The runtime free a call to `fn` is ("string_free", "string_release",
 * "release"), under its own name or an `@extern` alias, or NULL. */
const char* consuming_free_symbol(CodeGenerator* gen, const char* fn);
int struct_is_observable(CodeGenerator* gen, const char* struct_name);
/* A call that takes the caller's reference to the string at argument `idx`
 * (a free, or a function whose body frees that parameter), and the two
 * shapes an owned field read takes there: handed over, or freed. */
int callee_consumes_string_arg(CodeGenerator* gen, const char* func_name, int idx);
int field_read_can_hand_off(ASTNode* e);
void emit_string_field_handoff(CodeGenerator* gen, ASTNode* e);
/* How owned field read `arg`, argument `idx` of a call of `func_name`, is
 * handed over: STRING_FIELD_CONSUMED to a parameter the callee frees, else
 * 0. STRING_FIELD_TAKEN is a read evaluated ahead of a call that borrows it
 * (order_operand_hands_off): its temporary takes a reference of its own, so
 * a later operand that stores into the field leaves it valid. */
enum { STRING_FIELD_CONSUMED = 1, STRING_FIELD_TAKEN = 2 };
int string_field_handoff_kind(CodeGenerator* gen, const char* func_name, int idx, ASTNode* arg);
/* Such a field read evaluated ahead of its call (in the ordering sequence
 * `s`, or ahead of the statement for NULL): handed over to temporary
 * `value` as it is read, the name of the flag that says it was returned;
 * and the defer that frees the temporary on an exit taken before the call,
 * dropped with drop_defer once the call is written. */
const char* emit_string_field_handover_ahead(CodeGenerator* gen, ASTNode* e, int kind,
                                             TempSeq* s, const char* value);
ASTNode* push_handover_exit_free(CodeGenerator* gen, const char* own, const char* value);
void drop_defer(CodeGenerator* gen, ASTNode* carrier);
void emit_string_field_free(CodeGenerator* gen, ASTNode* e);
/* #2618: the same for an element of an array that owns its elements, when
 * string_element_free_kind says it is one. */
int string_element_free_kind(CodeGenerator* gen, ASTNode* e);
void emit_string_element_free(CodeGenerator* gen, ASTNode* e);
int string_take_kind(CodeGenerator* gen, ASTNode* e);
int string_take_is_view(CodeGenerator* gen, ASTNode* e);
int alias_source_must_copy(CodeGenerator* gen, const char* src_name);
void emit_string_take(CodeGenerator* gen, ASTNode* e, const char* target);


/* Resolve a callee's parameter kind by name + index. Consults the
 * extern registry first, then the user-fn definitions in
 * gen->program. Returns TYPE_UNKNOWN when the callee can't be
 * resolved or the index is out of range. Defined in codegen_stmt.c. */
TypeKind lookup_callee_param_kind(CodeGenerator* gen, const char* func_name, int param_idx);

/* Does `func_name` declare a `-> string` result? */
int callee_returns_string(CodeGenerator* gen, const char* func_name);

/* Does `func_name` resolve to a user function with a body (not an extern)?
 * Defined in codegen_stmt.c. */
int callee_has_visible_body(CodeGenerator* gen, const char* func_name);

/* Normalise a callee name's dots to underscores. The AST stores
   source-level callees in dotted form (`"string.concat"`) but stdlib
   externs, the generated C call sites, and the various callee registries
   (heap-string allowlist, builder-funcs registry, extern param-type
   table) all use the underscored form. Use this whenever you're about to
   look up by callee name. Returns `raw` itself when it has no dot, else
   an interned copy (cg_intern); "" for NULL. */
const char* codegen_normalise_callee(const char* raw);

/* Defer management (codegen.c) */
void push_defer(CodeGenerator* gen, ASTNode* stmt);
void push_defer_mode(CodeGenerator* gen, ASTNode* stmt, DeferMode mode);  /* #1140 */
void push_auto_defer(CodeGenerator* gen, const char* free_fn, const char* var_name);
void emit_defers_for_scope(CodeGenerator* gen);
void emit_defers_through_scope(CodeGenerator* gen, int floor_depth);
void emit_all_defers(CodeGenerator* gen);
void enter_scope(CodeGenerator* gen);
void exit_scope(CodeGenerator* gen);

/* Expression generation (codegen_expr.c) */
void generate_expression(CodeGenerator* gen, ASTNode* expr);

/* Message field helpers (codegen_expr.c) — shared with codegen_stmt.c */
MessageFieldDef* find_msg_field(MessageDef* msg_def, const char* name);
Type* message_field_type(ASTNode* program, const char* msg, const char* field);
void emit_message_field_init(CodeGenerator* gen, MessageFieldDef* fdef, ASTNode* rhs);
int message_has_array_fields(CodeGenerator* gen, ASTNode* message);
void emit_message_field_inits(CodeGenerator* gen, ASTNode* message, MessageDef* msg_def);
void emit_message_array_fills(CodeGenerator* gen, ASTNode* message, const char* lv, TempSeq* s);
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

/* Mark every string variable in `body` as a string local and hoist
   its declaration to function-entry scope (#405). Called from
   codegen_func.c immediately after hoist_if_branch_vars.

   `is_heap_string_expr` reads `gen->program` to look up callees;
   the call site of `hoist_heap_string_trackers` (and the
   per-stmt call sites of `is_heap_string_expr`)
   therefore must not run before generate_program populates that
   field. All current call sites are inside the function-body
   codegen path which runs after that point. */
void hoist_heap_string_trackers(CodeGenerator* gen, ASTNode* body);

int call_returns_owned_closure(CodeGenerator* gen, ASTNode* call);   /* #2506 */
const char* call_c_name(CodeGenerator* gen, const char* func_name);
/* #2915: the extern table's name for the extern a call to `func_name`
 * reaches, resolved as call_c_name resolves its C symbol, or NULL. */
const char* extern_name_for_call(CodeGenerator* gen, const char* func_name);
ASTNode* closure_container_store_value(CodeGenerator* gen, ASTNode* call);   /* #2518 */
/* The string value a list add, list set or map put takes, or NULL. */
int callee_param_is_string(CodeGenerator* gen, const char* func_name, int param_idx);
/* Release the parameter table box_is_heap_new builds. */
void zb_params_free(CodeGenerator* gen);
/* #2618: `string[N]` arrays that own their elements, and the element reads
 * a slot takes as views (codegen_stmt.c). */
void str_arrays_free(CodeGenerator* gen);
int string_array_owns(CodeGenerator* gen, ASTNode* scope, const char* name);
int is_owned_string_element(CodeGenerator* gen, ASTNode* e);
int is_owning_array_string_element(CodeGenerator* gen, ASTNode* e);
void code_generator_release(CodeGenerator* gen);
/* The statement temporary holding a call's struct result, or NULL. */
const char* stmt_struct_temp_of(const ASTNode* expr);
/* Drop `e as T` between string types before ownership is decided. */
void erase_string_retype_casts(ASTNode* node);
void emit_message_string_copy(CodeGenerator* gen, const char* lv, ASTNode* init);
ASTNode* message_field_init_expr(ASTNode* message, const char* name);
/* Does some return site of `fn_def` hand back a heap string? Memoised on
 * the definition; the callers' ownership decisions and the bare-fn adapter
 * read the same verdict. */
int function_def_returns_heap_string(CodeGenerator* gen, ASTNode* fn_def);
/* The same for tuple position `position` (#420). */
int function_def_returns_heap_at(CodeGenerator* gen, ASTNode* fn_def, int position);
/* #2019: declare the shared heap cell for a promoted capture and queue its
 * scope-exit release. The initial value is `init_expr` when given, else the
 * C text `init_text`. The cell is reference-counted, so the release is
 * always sound; see the _AeCellHeader helpers in the generated prologue. */
void emit_promoted_cell_declaration(CodeGenerator* gen, const char* name,
                                    const char* c_type, Type* var_type,
                                    ASTNode* init_expr, const char* init_text,
                                    int line, int column);
/* #2474: a cell holding a fixed-size array (`E[N]`) is a pointer to the
 * whole array, `E (*name)[N]`; see the definitions in codegen_stmt.c. */
int promoted_cell_array_len(const char* c_type, const char** elem);
/* #2516: a fixed-size array parameter is passed as `E _param_x[N]` and
 * copied into the body's own array (see codegen_stmt.c). */
int is_sized_array_param(Type* t);
void emit_sized_array_param_declarator(CodeGenerator* gen, Type* t, const char* name);
void emit_sized_array_param_copy(CodeGenerator* gen, Type* t, const char* name, ASTNode* scope);
const char* promoted_cell_pointer(const char* c_type, const char* name);
/* The cell for a promoted PARAMETER (a function's or a closure's), seeded
 * from the C parameter `param_cname`; see the definition for why a string
 * cell takes its own reference. */
void emit_promoted_param_cell(CodeGenerator* gen, const char* name,
                              const char* c_type, const char* param_cname,
                              int line, int column);
/* #2755: closure cells (codegen_stmt.c). The scope declaring the promoted
 * `name` seen from scope `scope`, and its body (codegen_expr.c); whether
 * the cell of `name` in the scope being emitted gives back the closures it
 * holds, or pins them because one may be kept bare; whether `e` reads a
 * closure cell; and the release of the remembered answers. */
ASTNode* promoted_cell_declaring_scope(CodeGenerator* gen, const char* scope, const char* name,
                                       ASTNode** body);
int is_closure_cell_read(CodeGenerator* gen, ASTNode* e);
/* The program's own definition of `name`, or NULL. Shared rather than
   duplicated: the builtin fast-paths need it to know when a program has
   defined a function of its own with a builtin's name. */
ASTNode* find_function_definition_by_name(ASTNode* program, const char* name);
ASTNode* find_extern_declaration_by_name(ASTNode* program, const char* name);

/* Push a function-exit release for every hoisted string local: a
 * local owns what it holds, so its scope exit releases it (issue #420
 * follow-up). Run before body codegen. See codegen_stmt.c. */
void push_heap_string_exit_free_defers(CodeGenerator* gen, ASTNode* body);
void stmt_struct_temps_set(ASTNode** nodes, const char** names, int count);
void stmt_struct_temps_get(ASTNode*** nodes, const char*** names, int* count);
int  call_returns_view_of_temp(CodeGenerator* gen, ASTNode* call);
int  call_returns_tuple_view_of_temp(CodeGenerator* gen, ASTNode* call);
int  tuple_call_returns_heap_at(CodeGenerator* gen, ASTNode* call, int j);
void collect_stmt_struct_temps(CodeGenerator* gen, ASTNode* e,
                               ASTNode*** nodes, int* count, int* cap);
void clear_captured_string_params(CodeGenerator* gen);

/* *StringSeq local lifecycle (parallel to the heap-string passes). */

/* `string?` heap-ownership lifecycle (parallel to the seq passes). */
void hoist_opt_string_locals(CodeGenerator* gen, ASTNode* body);

/* Actor generation (codegen_actor.c) */
void generate_actor_definition(CodeGenerator* gen, ASTNode* actor);
/* #2466: is state field `field` of actor `actor_name` emitted with an atomic
   C type (so a read from outside the actor goes through atomic_load)? */
int actor_state_field_is_atomic(CodeGenerator* gen, const char* actor_name,
                                const char* field);

/* Extern function registry — tracks param types for call-site cast emission */
void register_extern_func(CodeGenerator* gen, ASTNode* ext);
int is_extern_func(CodeGenerator* gen, const char* func_name);

/* Typed fn-pointer locals: variables whose source-level type is
 * `fn(T1, T2, ...) -> R` (storage = void*; call-site emits typed cast). */
void register_fnptr_local(CodeGenerator* gen, const char* name, Type* sig);
Type* lookup_fnptr_local(CodeGenerator* gen, const char* name);
Type* lookup_fnptr_global(CodeGenerator* gen, const char* name);   /* #2200 */
int is_fnptr_const_decl(const ASTNode* cd);                        /* #2648 */
int fn_const_decl(const ASTNode* cd);                              /* #2648 */
TypeKind lookup_extern_param_kind(CodeGenerator* gen, const char* func_name, int param_idx);
/* Full Type* for an extern's parameter (borrowed from the extern's AST),
 * or NULL. The kind alone can't drive tuple-param emission — packing the
 * by-value `_tuple_*` struct literal needs the element list (#1033). */
Type* lookup_extern_param_type(CodeGenerator* gen, const char* func_name, int param_idx);
/* #1286: the declared return type of a registered extern, or NULL. */
Type* lookup_extern_return_type(CodeGenerator* gen, const char* func_name);
/* #1286: emit `expr` as a C pointer to its first element (`T*`): a slice
 * yields `(T*)(s).ptr`, anything else is emitted as-is. */
void generate_expression_as_elem_ptr(CodeGenerator* gen, ASTNode* expr);
/* #1286: emit `expr` as an AetherSlice: a `T[N]` array is wrapped with its
 * static length, a slice is emitted as-is. */
void generate_expression_as_slice(CodeGenerator* gen, ASTNode* expr);
/* #2767: is `e` the empty slice, `null` or `[]` in a `T[]` slot (the
 * typechecker's AST_SLICE_FROM_ARRAY over either)? It views no storage, so
 * a file-scope static takes it as the constant `{0}`. */
int is_empty_slice_value(const ASTNode* e);
/* #2723: emit `arg` for a parameter of type `param_type`: an array a `try`
 * body may have written, for a `T[N]` parameter, as a copy of its own. */
void generate_sized_array_argument(CodeGenerator* gen, ASTNode* arg, Type* param_type);
int is_aether_extern_param(CodeGenerator* gen, const char* func_name, int param_idx);
/* #2914: is argument `arg_idx` of a call to extern `func_name` a C vararg,
 * past the declared parameters of an extern declared with `...`? */
int is_extern_vararg_position(CodeGenerator* gen, const char* func_name, int arg_idx);
/* R4: is argument `arg_idx` of a call to `func_name` in a C-variadic `...`,
 * an extern's or an Aether function's? */
int is_vararg_call_position(CodeGenerator* gen, const char* func_name, int arg_idx);
/* Returns 1 if extern `func_name`'s parameter at `param_idx` was
   declared `@retain`: the function keeps the pointer beyond the call,
   so the call hands it a reference of its own (R4). 0 for non-extern
   callees, missing annotations, or out-of-range index. See
   codegen_func.c. */
int is_retain_extern_param(CodeGenerator* gen, const char* func_name, int param_idx);
int is_noescape_extern_param(CodeGenerator* gen, const char* func_name, int param_idx);   /* #2523 */
const char* lookup_extern_c_name(CodeGenerator* gen, const char* func_name);

/* Builder function registry — functions where block configures first, then function executes */
int is_builder_func_reg(CodeGenerator* gen, const char* func_name);
const char* get_builder_factory(CodeGenerator* gen, const char* func_name);

/* A trailing block's statements, in its own scope (codegen_stmt.c). */
void emit_trailing_block_body(CodeGenerator* gen, ASTNode* body);
/* #2791: a builder call's trailing block, whose config the C variable
 * `cfg_var` holds: as emit_trailing_block_body, and a way out of the block
 * before the builder runs releases the config. */
void emit_builder_block_body(CodeGenerator* gen, ASTNode* body, const char* cfg_var,
                             const char* builder_name);
/* The block of a call's trailing DSL closure: not a closure the function
 * takes as a `fn` parameter. NULL when the call has none (codegen_expr.c). */
ASTNode* trailing_dsl_block(CodeGenerator* gen, ASTNode* call);

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
void discover_fn_values(CodeGenerator* gen);                       /* #2586 */
int fn_value_name(const char* name);
int emitting_marked_fn_value(CodeGenerator* gen);
int typed_fnptr_call(CodeGenerator* gen, ASTNode* call);
int param_may_hold_caller_string(const Type* t);
int fnptr_call_returns_string(CodeGenerator* gen, ASTNode* call);
int call_frees_arg(CodeGenerator* gen, const char* fn);
int call_param_keeps(CodeGenerator* gen, const char* func_name, int idx);

/* Function/struct generation (codegen_func.c) */
int has_return_value(ASTNode* node);
/* #2528 (codegen_actor.c): a `string` state field, which owns what it
 * holds and is released by `_aether_state_drop_<Actor>`. */
int state_field_owns_string(ASTNode* state_decl);
/* #2528: a `string[N]` (1) / `fn[N]` (2) state field, length in `*len`. */
int state_array_owned(ASTNode* state_decl, int* len);
/* #2688: a `T[N]` state field whose struct T owns strings or closures: T's
 * name (each element is the actor's own), or NULL. */
const char* state_struct_array_owned(CodeGenerator* gen, ASTNode* state_decl, int* len);
/* #2528 (codegen_stmt.c): `v` as the value an owned string element takes. */
void emit_owned_string_element(CodeGenerator* gen, ASTNode* v);

/* Struct-field heap-string ownership (#465). The struct typedef
 * emitter (generate_struct_definition) emits a `_aether_drop_<Name>()`
 * for a struct whose fields own. The statement-codegen consumes
 * these helpers at struct-local declaration sites (push the
 * scope-exit destroy defer) and at field-write sites (the store
 * takes the new value and releases the old). */
/* #2525: a `fn` field (a closure value, not a raw C function pointer): it
 * holds a reference of its own to the closure's env, released by
 * `_aether_drop_<Name>` / `_replace`, retained by `_dup`. */
int struct_field_is_closure(ASTNode* field);
/* #2528: a `string[N]` (1) or `fn[N]` (2) field whose elements the struct
 * owns, length in `*len`; 0 otherwise. */
int struct_field_owned_array(ASTNode* field, int* len);
/* #2525: store the closure value `e` into a slot that holds a reference of
 * its own: a fresh closure (a literal, a call handing one over) is adopted,
 * anything else (a local, a field, an element, a parameter) is retained. */
void emit_closure_take(CodeGenerator* gen, ASTNode* e);
/* #2890: is `v` a closure or a bare function, which a `ptr` slot takes as
 * a box (a closure, taken) or as the code address (a bare function)? Store
 * it there so; 0, emitting nothing, when it is neither. */
int fn_value_into_ptr(CodeGenerator* gen, ASTNode* v);
int emit_closure_into_ptr(CodeGenerator* gen, ASTNode* v);
/* #2528: an array literal whose elements the holder owns: `kind` 1 copies
 * each string (a fresh one is adopted), 2 takes each closure. */
void emit_owned_array_literal(CodeGenerator* gen, ASTNode* lit, int kind);
/* #2688: a struct literal's fixed-size array field given another array
 * (`Shelf { names: arr }`): it is `{0}` in the literal, and
 * emit_struct_literal_array_field then fills that field of the literal's
 * temporary `slit`, element by element, each taken as the field keeps it. */
int struct_literal_field_takes_array(CodeGenerator* gen, const char* sname, ASTNode* field_init);
void emit_struct_literal_array_field(CodeGenerator* gen, const char* sname, ASTNode* field_init,
                                     const char* slit);
/* #2690: a struct value a `try` body writes is declared volatile; may the
 * lvalue `lv` (a local, or a field or element held by one) be such a value?
 * Its release and replace are then the generated `_aether_vdrop_<Name>`
 * and `_aether_vstore_<Name>`, which these name. */
int lvalue_may_be_volatile(CodeGenerator* gen, ASTNode* lv);
void emit_struct_unwind_track(CodeGenerator* gen, const char* var_name, const char* struct_name);
const char* struct_destroy_fn(const char* sname, int is_volatile);
const char* struct_replace_fn(const char* sname, int is_volatile);
/* #2497: does a value of this struct own heap strings, in a `string` field
 * of its own or in a field that is itself such a struct held by value? It
 * then has `_aether_drop_<Name>` / `_replace` / `_heap_free` / `_cell_release`,
 * which release the nested struct's strings too. */
int struct_owns_heap_strings(CodeGenerator* gen, ASTNode* struct_def);
ASTNode* owning_struct_field_def(CodeGenerator* gen, ASTNode* field);
/* #2525: as above, but a fixed-size array field of such structs qualifies
 * too, its length in `*len` (0 for a direct field). */
ASTNode* owning_struct_field_def_n(CodeGenerator* gen, ASTNode* field, int* len);
/* #2497: store `e` into a slot that owns a `sname` struct value (see the
 * definition in codegen_stmt.c). struct_take_shape: is `e` a value that
 * views a struct owned elsewhere (a variable, a field, an element, an `if`
 * over such), which a take copies or moves, rather than a fresh one? */
int struct_take_shape(ASTNode* e);
void emit_struct_take(CodeGenerator* gen, ASTNode* e, const char* sname,
                      const char* target);
const char* struct_owning_strings(CodeGenerator* gen, Type* t);
ASTNode* find_struct_definition_by_name(ASTNode* program, const char* name);

/* #2921: R2/R3 for composite types (codegen_own.c). */
typedef enum { OWN_RETAIN, OWN_DROP, OWN_ZERO, OWN_PUBLISH } OwnOp;
int own_type_owns(CodeGenerator* gen, Type* t);
void own_emit_part(CodeGenerator* gen, Type* pt, const char* lv, OwnOp op);
void own_emit_prototypes(CodeGenerator* gen, Type* t);
void emit_composite_take(CodeGenerator* gen, ASTNode* e, Type* t);
void emit_owning_store(CodeGenerator* gen, const char* lv, Type* t, ASTNode* rhs, int publish);
void push_own_drop(CodeGenerator* gen, const char* name, Type* t, int line, int column);
void push_own_drop_carrier(CodeGenerator* gen, const char* name, Type* t, int line, int column);
void emit_value_take(CodeGenerator* gen, ASTNode* e, Type* t);
ASTNode* find_message_definition(ASTNode* program, const char* name);
int message_owns_fields(CodeGenerator* gen, const char* name);
int expr_is_c_view_of_slice(CodeGenerator* gen, const ASTNode* expr);
void emit_array_fill(CodeGenerator* gen, const char* dst, ASTNode* e, Type* at);
void emit_array_fill_c(CodeGenerator* gen, const char* dst, const char* src, Type* at, int retain);
void emit_array_result_value(CodeGenerator* gen, ASTNode* e, Type* at);
void emit_owning_array_store(CodeGenerator* gen, const char* name, Type* at, ASTNode* rhs);
int struct_param_written(ASTNode* n, const char* pname);
void emit_string_take_owned(CodeGenerator* gen, ASTNode* e, int cell);
void own_collect_types(CodeGenerator* gen, ASTNode* node);
void emit_own_ops(CodeGenerator* gen);
const char* own_fn(CodeGenerator* gen, Type* t, const char* op);
const char* own_cell_release_fn(const char* c_type);
const char* own_place_lvalue(Type* t, const char* name);
const char* own_unwind_fn(CodeGenerator* gen, Type* t);
void own_reset(void);

/* #2298: emit the `@derive(schema)` field tables and their getters
 * (codegen_schema.c). */
void emit_schema_tables(CodeGenerator* gen, ASTNode* program);

/* @c_callback annotation helpers (#235). A function declared with
   `@c_callback aether_name(...)` (or `@c_callback("c_sym") aether_name(...)`)
   gets a stable, externally-visible C symbol so it can be passed across
   module boundaries as a function pointer to C externs. Codegen drops
   the `static` storage class for these even when imported, and uses
   the chosen C symbol at the C-decl name slot and at every value-
   position reference. */
int is_c_callback(ASTNode* func);
const char* c_callback_symbol(ASTNode* func);
/* sandbox.enforce trusted calls (codegen.c, see analysis/sandbox_trust.h). */
ASTNode* sandbox_trust_target(CodeGenerator* gen, const ASTNode* call);
/* The wrapper's C name, interned. */
const char* sandbox_trust_wrapper_name(ASTNode* def, int site);
/* Whether a top-level function is emitted `static`. All three emit sites
   (definition, combined multi-clause definition, forward declaration) must
   agree or C rejects the file with "static declaration follows non-static
   declaration". #1366 */
int fn_has_internal_linkage(ASTNode* func);
int lib_fn_is_local(CodeGenerator* gen, ASTNode* fn);
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
void aether_register_c_import_union(const char* name);
const char* aether_c_tag(const char* name);
int aether_is_c_import_annotation(const char* annotation);

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
ASTNode* aether_c_struct_chain(ASTNode* macc, const char** path);
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
void emit_closure_declarations(CodeGenerator* gen);
void emit_closure_definitions(CodeGenerator* gen);
/* L4 validation: reject closures inside actor handlers that write to
   actor state fields. Run after discover_closures, before codegen.
   Returns 1 on success, 0 if errors were reported. */
int validate_closure_state_mutations(CodeGenerator* gen, ASTNode* program);
/* Route 1 promotion queries (populated by discover_closures): */
void get_promoted_names_for_func(CodeGenerator* gen, const char* func_name,
                                 char*** out_names, int* out_count);
int is_promoted_capture(CodeGenerator* gen, const char* name);
/* The C type of `var_name` as `parent_func` declares it (a function name,
 * "main", a receive arm or a hoisted closure scope); "int" when unknown. */
const char* lookup_var_c_type(CodeGenerator* gen, const char* var_name, const char* parent_func);
Type* promoted_cell_type(CodeGenerator* gen, const char* name);   /* #2797 */
const char* promoted_cell_release_fn(CodeGenerator* gen, const char* c_type);
const char* struct_owning_strings(CodeGenerator* gen, Type* t);
/* `retain_closures`: 1 where the value at `lvalue` stays a holder of its
 * closure fields (a parameter, a cell a copy is returned from), 0 where it
 * is moved out (#2525). */
void push_struct_destroy_defer(CodeGenerator* gen, const char* var_name,
                               Type* struct_type, int line, int col);

/* Internal helpers shared across files */
int contains_send_expression(ASTNode* node);
const char* get_single_int_field(MessageDef* msg_def);
/* The fields of a message in the order its C struct declares them, and the
 * C type each has there; the .c and the --emit-header file share them (#2517). */
int message_struct_fields(ASTNode* msg_def, ASTNode** out);
const char* message_field_c_type(ASTNode* msg_def, ASTNode* field);
void generate_default_return_value(CodeGenerator* gen, Type* type);
int is_function_generated(CodeGenerator* gen, const char* func_name);
/* ------------------------------------------------------------------
 * #2007: the program index.
 *
 * Codegen asks the top level the same few questions over and over: the
 * clauses of a function by name, the first definition by name, whether a
 * name is an extern this TU declares, whether it is a @c_callback. Each
 * used to be a walk of every top-level node, once per call site or per
 * identifier, and together they made codegen quadratic in the number of
 * functions. The index answers all of them from one pass over the
 * program. It is keyed to the program node and its child_count and
 * rebuilt when either changes; generate_program resets it after the
 * pre-passes that rename definitions, since those change keys without
 * changing the count.
 * ------------------------------------------------------------------ */
typedef struct {
    ASTNode** nodes;           /* AST_FUNCTION_DEFINITION / AST_BUILDER_FUNCTION, program order */
    int count;
    int capacity;
    ASTNode* c_callback;       /* the first @c_callback clause, or NULL (#2897) */
} DefClauses;

typedef struct {
    ASTNode* program;
    int child_count;
    ASTNode* main_fn;          /* the AST_MAIN_FUNCTION, or NULL */
    StrMap defs;               /* name -> DefClauses* */
    StrMap externs;            /* name -> ASTNode* (own or imported AST_EXTERN_FUNCTION) */
    StrMap c_callbacks;        /* name -> const char* C symbol */
    StrMap clause_scopes;      /* `__clause_<ptr>` -> its clause (#2644, fn_scope_name) */
    StrMap def_children;       /* `<ptr>` of a definition's child (its body, a
                                * parameter) -> the definition (#2897) */
    StrMap def_sets;           /* `<ptr>` of a definition -> the DefClauses it
                                * is one of (#2897, fn_def_clause_set) */
} ProgramIndex;

ProgramIndex* program_index(ASTNode* program);
void program_index_reset(void);
/* The function definition (a clause of a set, or a single one) that has
 * `child` among its children, or NULL (#2897). */
ASTNode* program_index_def_of_child(ASTNode* program, ASTNode* child);
/* The clauses defined under `name`, or NULL. */
const DefClauses* program_index_clauses(ASTNode* program, const char* name);
void mark_function_generated(CodeGenerator* gen, const char* func_name);
int count_function_clauses(ASTNode* program, const char* func_name);
ASTNode** collect_function_clauses(ASTNode* program, const char* func_name, int* out_count);

/* #2644: a function written as several clauses is a set of real functions:
 * each clause a static C function of its own (clause_c_name), emitted with
 * a single function's whole setup, and the set's name a dispatcher that
 * tests each clause's patterns and guard in order and returns what the
 * matching clause returns (generate_combined_function). */
/* The clauses of the set `fn_def` is one of, or NULL for a single definition. */
const DefClauses* fn_def_clause_set(ASTNode* program, ASTNode* fn_def);
/* The scope name of `fn`'s variables for the closure analyses: its name, or
 * `__clause_<ptr>` for a clause of a set. */
const char* fn_scope_name(ASTNode* program, ASTNode* fn);
ASTNode* find_clause_by_scope_name(ASTNode* program, const char* scope);
/* #2664: the @c_callback definition binding `fn`'s symbol (a set's first
 * annotated clause), or NULL. */
ASTNode* fn_c_callback_def(CodeGenerator* gen, ASTNode* fn);
const char* clause_c_name(ASTNode* program, ASTNode* clause);
/* #2645: the type `fn` returns in C (NULL for void), decided over every
 * clause of a set, and its spelling. */
Type* fn_result_type(CodeGenerator* gen, ASTNode* fn);
void emit_fn_result_c_type(CodeGenerator* gen, ASTNode* fn);
/* The parameters of a definition in C signature order (variables, literal,
 * struct and list patterns), and the type a position has across a set. */
int fn_param_count(ASTNode* fn);
ASTNode* fn_param_at(ASTNode* fn, int pos);
Type* fn_param_type_at(CodeGenerator* gen, ASTNode* fn, int pos);
const char* fn_list_param_elem_ctype(CodeGenerator* gen, ASTNode* fn, int pos);

#endif
