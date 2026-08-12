#ifndef CODEGEN_H
#define CODEGEN_H

#include <stdio.h>
#include "../ast.h"
#include "../../runtime/actors/aether_message_registry.h"

// Maximum defer nesting depth (scope depth * statements per scope)
#define MAX_DEFER_STACK 256
#define MAX_SCOPE_DEPTH 64

/* #1140 — which exits a deferred statement fires on.
 *
 *   DEFER_ALWAYS  `defer x`        every exit (the pre-existing behaviour, and
 *                                  what every compiler-synthesised cleanup
 *                                  carrier uses)
 *   DEFER_TRY     `defer try x`    only a SUCCESSFUL return
 *   DEFER_CATCH   `defer catch x`  only an ERROR return
 *
 * "Error" means the function returned a non-empty error slot — Aether's
 * `(value, err)` convention, and `T!`, which is the same shape. */
typedef enum {
    DEFER_ALWAYS = 0,
    DEFER_TRY,
    DEFER_CATCH
} DeferMode;

/* #1140 — how a given exit site should filter the defer stack. A plain scope
 * exit / break / continue is not a function outcome at all, so only the
 * unconditional defers run there. */
typedef enum {
    DEFER_EXIT_PLAIN = 0,   /* break / continue / falling off a scope */
    DEFER_EXIT_SUCCESS,     /* a return whose error slot is known empty */
    DEFER_EXIT_ERROR,       /* a return whose error slot is known non-empty */
    DEFER_EXIT_RUNTIME      /* a return whose outcome is only known at run time */
} DeferExit;

typedef struct {
    FILE* output;
    int indent_level;
    int actor_count;
    int function_count;
    char* current_actor;
    char** actor_state_vars;
    int state_var_count;

    // While set, references to actor state fields emit
    // `<alias>->field` instead of the usual `self->field`.
    // The `spawn_<Actor>` function initialises `actor->...` at
    // alloc time and the receive-timeout expression there has
    // `actor` in scope but NOT `self`; setting this to "actor"
    // for the duration of that expression lets users write
    // `receive { … } after m_interval_ms -> { … }` instead of
    // hardcoding the timeout. Cleared back to NULL afterwards.
    const char* state_self_alias;
    MessageRegistry* message_registry;
    char** declared_vars;  // Track variables declared in current function
    int declared_var_count;
    // #790: names of locals currently bound to a `heap.new(T)` box (a
    // calloc'd, zero-initialised `*T`). Only these boxes have their
    // `_heap_<field>` ownership trackers guaranteed zero, so only on these is
    // it safe to route a `box.field = ...` store through the heap-string
    // assignment wrapper (free old if owned + set tracker). A raw
    // `malloc(...) as *T` has garbage trackers, so its field stores stay bare.
    // Reset per function; a var is removed when reassigned to a non-heap.new.
    char** heap_box_vars;
    int heap_box_var_count;
    // #701: names of module-level `var` globals (file-scope statics).
    // Populated once before any function body is emitted. A bare
    // `name = expr` inside a function whose name is in this set is a
    // WRITE to the global, not a new local declaration.
    char** module_global_vars;
    int module_global_var_count;
    int generating_lvalue;  // Track if we're generating an assignment target (lvalue)
    int in_condition;  // Track if we're in a condition (if/while) to avoid double parens
    int in_main_loop;  // Track if we're in main's loop for batch send optimization
    int in_main_function;  // Track if we're in main() so return -> goto main_exit
    int uses_main_exit;    // Track if any goto main_exit was emitted (suppress unused label)
    int interp_as_printf;  // When set, string interp generates printf() instead of snprintf+malloc
    ASTNode* program;  // Reference to program root for lookups

    // Source file path threaded through to expand `__FILE__` at codegen
    // time (#265). NULL is fine — `__FILE__` then emits "(unknown)".
    const char* source_file;

    // Last (file, line) pair emitted as a `#line N "path"` directive,
    // tracked so codegen_maybe_emit_line() doesn't re-emit identical
    // directives for every node sitting on the same source line.
    // last_line_file is borrowed (lives in some ASTNode->source_file
    // owned by the AST tree); never freed by codegen.
    const char* last_line_file;
    int last_line_num;

    // Header generation (--emit-header)
    int emit_header;         // Whether to emit a C header file
    FILE* header_file;       // Output stream for header
    const char* header_path; // Path to header file

    // --emit=<exe|lib|both> mode. Default is exe only.
    // lib: omit `int main(int,char**)` entry; emit aether_<name> alias stubs.
    // both: emit main() AND the aether_<name> stubs in the same .c file.
    int emit_exe;            // Emit the int main(int,char**) entry point
    int emit_lib;            // Emit aether_<name> alias stubs for top-level functions
    FILE* csrc_header_file;  // #996 --emit=csrc: if set, write each export's C
                             // prototype here (a distributable .h for the catalog)
    FILE* csrc_catalog_file; // #996 --emit=csrc: if set, write the aether_lib_meta
                             // catalog (functions/closures/constants) as JSON here
                             // (a machine-readable, binding-generator-friendly form
                             // of the same data the C struct carries)
    const char* csrc_capabilities; // #996 --emit=csrc: comma-joined --with grants
                             // (e.g. "fs,net"), NULL/"" when capability-empty; emitted
                             // as capability provenance in the JSON catalog

    // --emit-main=<func>: with --emit=lib, also emit a thin main(argc,argv)
    // shim that calls the named Aether function. Closes the exe/lib symmetry
    // (issue #268.3) — one .c can ship as both a loadable lib AND a binary.
    // NULL when not requested.
    const char* emit_main_target;

    // Track generated pattern matching functions to avoid duplicates
    char** generated_functions;
    int generated_function_count;

    // Defer stack: tracks deferred statements for LIFO execution at scope exit
    ASTNode* defer_stack[MAX_DEFER_STACK];
    // #1140: parallel to defer_stack — which exits this defer fires on.
    // Kept as a separate array rather than stashed on the AST node because
    // push_defer receives the deferred STATEMENT (the AST_DEFER_STATEMENT
    // parent, which carries the qualifier, is dropped at the push site), and
    // because the node's `annotation` slot is already spoken for by the
    // synthetic cleanup carriers (`heap_string_exit_free:` and friends).
    DeferMode defer_mode[MAX_DEFER_STACK];
    /* #1140: the kind of exit currently being emitted, so the defer-drain loops
     * know which conditional defers to fire. Set by a return site immediately
     * before it drains, and reset afterwards. Threading it through the
     * generator rather than through every emit_*_defers signature keeps the
     * dozen existing call sites unchanged — they all mean DEFER_EXIT_PLAIN. */
    DeferExit defer_exit;
    /* #1140: the C expression naming the error slot of the result being
     * returned, e.g. "_builder_ret._1". Only set when defer_exit is
     * DEFER_EXIT_RUNTIME — the conditional defers are then emitted under a
     * runtime test on it. NULL otherwise. Borrowed; not owned. */
    const char* defer_err_slot;
    int defer_count;
    int scope_defer_start[MAX_SCOPE_DEPTH];  // defer_count at scope entry
    int scope_depth;

    // try { } catch { } frame-pop tracking (issue #501). Incremented
    // before generating a try body, decremented after; the catch
    // handler runs with the counter back at the outer value because
    // the runtime aether_try_pop() is emitted before the handler.
    // Every return / panic site checks this and drains the pending
    // pops so a `return` inside a try body doesn't skip the cleanup
    // and leak a panic frame (AETHER_PANIC_MAX_DEPTH = 32).
    int try_frame_depth;

    // try_frame_depth snapshot at the entry of each enclosing loop.
    // `break` / `continue` only drain try frames pushed inside the
    // current loop body (`try_frame_depth - loop_try_base[depth-1]`),
    // not all live frames — a try whose `{` lexically precedes the
    // loop's `{` survives a break/continue.
    #define AETHER_MAX_LOOP_NEST 64
    int loop_try_base[AETHER_MAX_LOOP_NEST];
    int loop_nest_depth;

    // Labeled break/continue (#893). Parallel to loop_try_base, indexed by
    // loop_nest_depth. For each enclosing loop: its source label (or NULL),
    // the scope_depth of the scope CONTAINING the loop (so a labeled
    // break/continue can emit defers for exactly the scopes it unwinds), a
    // unique id used to mint the C goto labels, and whether the break/continue
    // target labels were actually referenced (so only a C label that is used
    // gets emitted, avoiding -Wunused-label).
    const char* loop_label[AETHER_MAX_LOOP_NEST];
    int loop_label_scope[AETHER_MAX_LOOP_NEST];
    int loop_label_id[AETHER_MAX_LOOP_NEST];
    int loop_label_break_used[AETHER_MAX_LOOP_NEST];
    int loop_label_continue_used[AETHER_MAX_LOOP_NEST];
    int next_loop_label_id;

    // try-clobbered locals: variable names modified inside any try
    // body of the current function.  Such locals — when declared
    // in an enclosing scope of the try — must be lowered as
    // `volatile T name` so the C compiler doesn't keep them in a
    // register that the panic's siglongjmp clobbers.  C99 7.13.2.1:
    // "All accessible objects have values, and all other components
    // of the abstract machine have state, as of the time longjmp
    // was called, except that the values of objects of automatic
    // storage duration that are local to the function containing
    // the invocation of the corresponding setjmp macro that do not
    // have volatile-qualified type and have been changed between
    // the setjmp invocation and longjmp call are indeterminate."
    // Set populated by mark_try_clobbered_vars() pre-pass; consulted
    // at AST_VARIABLE_DECLARATION emission sites when
    // try_frame_depth == 0 (only outer-scope decls need volatile;
    // inside-try decls live and die inside the try body).
    char** try_clobbered_vars;
    int try_clobbered_var_count;

    // Extern function parameter type registry — used at call sites for proper casts
    // e.g., list_add(void*, void*) called with int arg → cast to (void*)(intptr_t)
    struct ExternParamInfo {
        char* name;          // Aether-side name (the namespace surface)
        char* c_name;        // C symbol to emit at call sites; NULL = same as name.
                             //   Set to a different value when the extern was declared
                             //   via @extern("c_symbol") aether_name(...) — see #234.
        TypeKind* params;    // array of parameter kinds (TYPE_PTR, TYPE_INT, ...)
        Type** param_full;   // full Type* per param (borrowed from the extern's
                             //   AST, which outlives the generator). Needed when
                             //   the kind alone can't drive emission — a
                             //   TYPE_TUPLE param's element list is required to
                             //   pack the by-value struct literal (#1033). NULL
                             //   entries where the param had no node_type.
        int* params_aether;  // 1 per index when the param was declared
                             //   `name: @aether string` — receiver is Aether-emitted
                             //   C and dispatches on AetherString magic via str_len.
                             //   Codegen suppresses aether_string_data() unwrap for
                             //   that specific arg slot, so binary content with
                             //   embedded NULs round-trips intact. See #351. NULL
                             //   when no param carries the annotation (saves the
                             //   per-extern allocation in the common case).
        int* params_retain;  // 1 per index when the param was declared
                             //   `name: @retain string` — function stores /
                             //   retains the pointer beyond the call. Tells the
                             //   escape walker to mark a heap-string arg at
                             //   this slot as escaped, so the heap-string-
                             //   tracker wrapper and function-exit defer-free
                             //   both skip freeing it. Without this, default
                             //   string-param treatment is "read-only" — correct
                             //   for string.length / equals / println but a UAF
                             //   for retainers like string_list_add or
                             //   map_put_raw's key. See #420 follow-up. NULL
                             //   when no param carries the annotation.
        int param_count;
    }* extern_registry;
    int extern_registry_count;
    int extern_registry_capacity;

    // MSVC compat: counter for ask-operator temp variables (_ask_result_N)
    int ask_temp_counter;

    // Counter for message-send array hoist variables (_aether_arr_N). Every
    // `msg ! Foo { field: [a, b, c] }` with a composite array field
    // allocates a `static const T _aether_arr_N[] = {...}` at the send
    // site so the array's storage outlives the send-expression block and
    // the receiving actor can safely read through the pointer.
    int msg_arr_counter;

    // Match-as-expression: when non-NULL, match arms assign to this variable
    const char* match_result_var;

    // Cooperative preemption: insert sched_yield() at loop back-edges
    int preempt_loops;

    // Current function's return type (for multi-return codegen)
    Type* current_func_return_type;

    // Current function's AST node — needed by AST_RETURN_STATEMENT
    // codegen to find the function's `ensures` clauses (issue #348).
    // Set by generate_function_definition / generate_main_function on
    // entry, restored on exit. NULL outside a function-body context.
    ASTNode* current_function;

    // Issue #348 — when 1, codegen skips `requires`/`ensures` check
    // emission entirely (the per-call cost goes to zero, matching C's
    // -DNDEBUG for assert). Set from `--no-contracts` on the aetherc
    // command line.
    int no_contracts;

    // Tuple struct registry: track generated tuple typedefs to avoid duplicates
    char** tuple_type_names;    // e.g., "_tuple_int_string"
    int tuple_type_count;
    int tuple_type_capacity;

    // #340: emitted optional typedefs, e.g. "ae_opt_int" (parallel to tuples).
    char** opt_type_names;
    int opt_type_count;
    int opt_type_capacity;

    // Builder function registry: functions with _ctx: ptr as first param
    // get builder_context() auto-injected at call sites inside trailing blocks
    char** builder_funcs;
    int builder_func_count;
    int builder_func_capacity;
    int in_trailing_block;  // >0 when generating code inside a trailing block

    // Set to 1 immediately before generating a call whose result value is
    // discarded (a call used as an expression statement). The call codegen
    // captures and clears it at entry; when set, a VOID/discarded parent
    // call still drains its heap-returning inline args (the arg-temp wrap
    // emits a void-yielding `({ T t=...; call(...); free(t); })`). Cleared
    // at the call-codegen entry so it never leaks into nested arg calls.
    int discard_call_value;

    // When emitting a closure body, captures in this list are mutated and
    // therefore routed through _env->name on reassignment. Set per-closure
    // by emit_closure_definitions, cleared after the body. NULL when not
    // currently emitting a closure body.
    char** current_env_captures;
    int current_env_capture_count;

    // Route 1 heap-promotion: variables in this list are heap-allocated
    // cells (`int* name`) in the current function scope. Reads emit
    // `(*name)`; writes emit `(*name) = expr`. Set at the start of any
    // function/closure body whose enclosing function has promoted names,
    // cleared on exit. The set is the union of captures that ANY closure
    // in the enclosing function assigns to.
    char** current_promoted_captures;
    int current_promoted_capture_count;

    // Per-function promoted-name map. Built during discover_closures as
    // a precompute of which variables need heap promotion in each
    // function body. Looked up when generating that function's body
    // (either main's, a regular user function's, or a closure's — closures
    // inherit from their parent_func).
    struct PromotedFuncEntry {
        char* func_name;    // "main" for main, user function name otherwise
        char** names;       // promoted variable names
        int count;
    }* promoted_funcs;
    int promoted_func_count;
    int promoted_func_capacity;

    // Builder function registry: functions marked with 'builder' keyword
    // Block runs first (filling config), then function executes with config
    struct BuilderFuncEntry {
        char* name;
        char* factory;  // Factory function name, NULL means "map_new"
    } *builder_funcs_reg;
    int builder_func_reg_count;
    int builder_func_reg_capacity;

    /* Bare-fn → fn-typed-slot adapter registry. When a bare named
     * function is wrapped into an _AeClosure at a coercion site
     * (`(_AeClosure){.fn=name, .env=NULL}`), the real C signature of
     * `name` is `R(args)` but the closure-invocation trampoline calls
     * .fn as `R(void* env, args)`. Under -O2 (and silently on the
     * arg-shift path even at -O0) this UB-via-incompatible-pointer-
     * cast surfaces as wrong values. Fix: synthesise a per-bare-fn
     * adapter `_aether_bare_adapter_<name>(void* env, args) -> R` at
     * file scope that ignores env and forwards to the bare fn; the
     * wrap site stuffs the adapter's address into .fn instead. Filed
     * in aether/new_aevg_asks.md ASK 3 (and the LAYER 2 follow-up
     * in fn_return_float_cast.md). */
    char** bare_fn_adapter_names;
    int    bare_fn_adapter_count;
    int    bare_fn_adapter_capacity;

    // Closure support: track closures for hoisted C function generation
    int closure_counter;    // unique ID for closure env structs and functions
    // Map variable names to closure IDs (set during variable declaration codegen)
    struct ClosureVarMap {
        char* var_name;
        int closure_id;
    }* closure_var_map;
    int closure_var_count;
    int closure_var_capacity;
    // Pending closures: discovered during expression codegen, emitted at file scope
    struct ClosureInfo {
        int id;                  // unique closure ID
        ASTNode* closure_node;   // AST_CLOSURE node
        char** captures;         // captured variable names
        Type** capture_types;    // captured variable types
        int capture_count;
        char* parent_func;       // enclosing function name (for nested context)
    }* closures;
    int closure_count;
    int closure_capacity;

    // Heap-owned string variables: variables whose current value is a
    // heap-allocated string (from string_concat, string_substring, etc.).
    // Used to emit free() on reassignment and avoid freeing string literals.
    char** heap_string_vars;
    int heap_string_var_count;

    // Subset of `heap_string_vars`: names whose value is passed as a
    // function-call argument (or captured by a closure) somewhere in
    // the function body, in any context other than the RHS of `V = ...`
    // where the LHS is V itself. The wrapper at codegen_stmt.c:1611
    // skips the `free(_tmp_old)` when the var is escaped — the
    // recipient of the escape may have stored the pointer (map.put,
    // list.add, struct field, actor message field, closure capture)
    // and freeing the local would dangle the stored copy. Conservative
    // analysis: we do not know which callees store vs. consume, so we
    // assume any non-trivial use means storage. Result: alias-safe
    // (no UAF) at the cost of leaking the value over the function's
    // lifetime — strictly better than the pre-pass UAF. Populated by
    // mark_escaped_heap_string_vars (codegen_stmt.c) before the body
    // is generated, alongside hoist_heap_string_trackers.
    char** escaped_string_vars;
    int escaped_string_var_count;

    // Subset of `heap_string_vars`: names whose ONLY escape is via a
    // `return <name>;` statement — no container, struct-field, or
    // closure-capture site stores the pointer. The two-set split is
    // load-bearing for the uniform-heap return-escape contract:
    //
    //   - Container-escaped vars (the `escaped_string_vars` set above):
    //     wrapper-free at reassignment is suppressed because a recipient
    //     may have stored the pointer. Pre-existing PR #450 behaviour.
    //
    //   - Return-only-escaped vars: NO recipient stashes the pointer.
    //     The wrapper-free at reassignment MUST fire so intermediate
    //     accumulator buffers (e.g. avn-bench `sorted = string.concat(
    //     sorted, ...)` inside a loop) are reclaimed each iteration.
    //     The function-exit defer-free is still suppressed for these
    //     names (otherwise it would dangle the return value).
    //
    // mark_return_escaped_string_var populates this; the reassign-
    // wrapper at codegen_stmt.c:2533 consults `is_escaped_string_var`
    // alone (container-only) for the free decision, and the function-
    // exit defer pre-pass consults both for the defer-suppression.
    char** return_escaped_string_vars;
    int return_escaped_string_var_count;

    // #752: struct locals that escape via a return (directly or as a
    // tuple element). Such a struct's heap-string fields belong to the
    // caller once returned, so the function-exit <Struct>_destroy defer
    // must be suppressed — otherwise the fields are freed at callee exit
    // while the returned (shallow) copy still points at them, and the
    // caller reads dangling memory. Populated at each return site before
    // defers drain; consulted by try_emit_struct_destroy.
    char** return_escaped_struct_vars;
    int return_escaped_struct_var_count;

    // `*StringSeq` (cons-cell sequence) locals whose current value is a
    // heap-allocated, refcounted seq returned by an OWNING producer
    // (string_seq_cons / reverse / concat / take / drop / from_array /
    // split_to_seq / retain / empty — NOT string_seq_tail, which borrows
    // into an existing spine). Parallel to heap_string_vars: the codegen
    // frees the prior value on reassignment and at scope exit via
    // string_seq_free(), which is a refcount DECREMENT — so freeing a
    // shared spine is safe (it survives while another owner holds a ref).
    // Because every seq_* op retains or reads (never steals the caller's
    // ref), the only escapes are `return <name>` and a raw store into a
    // non-seq container; escaped_seq_vars records those so their
    // scope-exit free is suppressed. A `_seqheap_<name>` flag tracks
    // whether the slot currently owns a ref (set from an owning producer,
    // cleared by an explicit seq_free / borrowed assignment).
    char** seq_vars;
    int seq_var_count;
    char** escaped_seq_vars;
    int escaped_seq_var_count;

    // `string?` (optional-of-string) locals whose `.val` may own a heap
    // allocation. Parallel to heap_string_vars, but the value is a by-value
    // `ae_opt_string` struct (`{ int has; const char* val; }`), so the free
    // targets `<name>.val` guarded by `<name>.has`. The struct's `.has` bit
    // alone can't tell a heap `.val` from a borrowed literal, so a
    // `_heapopt_<name>` flag tracks ownership exactly like `_heap_<name>`
    // does for bare strings — set from is_heap_string_expr on the coerced
    // initializer/reassignment RHS. The prior `.val` is freed on reassign
    // and at scope exit via aether_heap_str_free. escaped_opt_str_vars
    // suppresses the exit-free when the optional is `return`ed (the caller
    // owns the buffer).
    char** opt_str_vars;
    int opt_str_var_count;
    char** escaped_opt_str_vars;
    int escaped_opt_str_var_count;

    // Ask/reply type map: request message name -> reply shape.
    // Built by scanning actor receive handlers for reply statements.
    struct ReplyTypeEntry {
        char* request_msg;   // e.g. "Add"
        char* reply_msg;     // e.g. "Result"; NULL for expression replies
        int scalar_kind;     // TypeKind of a bare-expression reply
                             // (`reply count`, #1324); TYPE_UNKNOWN for
                             // message-constructor replies
    }* reply_type_map;
    int reply_type_count;
    int reply_type_capacity;

    // Typed C function-pointer locals: variables whose source-level
    // type was `fn(T1, T2, ...) -> R` (or that got that type via an
    // `expr as fn(...)` cast on their initializer).  Storage in C is
    // void*; the call-site codegen consults this registry to emit
    // the matching `((R (*)(T1, T2))(name))(args)` cast.  Built by
    // generate_variable_declaration when the AST node's node_type
    // is TYPE_FUNCTION with is_fnptr=1.  Lookup is by Aether-side
    // name (post-namespace-mangling not relevant — fn-pointer locals
    // are always plain identifiers).
    struct FnPtrLocal {
        char* name;            // e.g. "fp"
        Type* signature;       // TYPE_FUNCTION with is_fnptr=1
    }* fnptr_locals;
    int fnptr_local_count;
    int fnptr_local_capacity;
} CodeGenerator;

// Code generation functions
CodeGenerator* create_code_generator(FILE* output);
CodeGenerator* create_code_generator_with_header(FILE* output, FILE* header, const char* header_path);
void free_code_generator(CodeGenerator* gen);
void generate_program(CodeGenerator* gen, ASTNode* program);
/* L4 validation: reject closures inside actor handlers that write to
   actor state fields. Call before generate_program. Returns 1 on
   success, 0 if errors were reported (compilation should abort). */
int validate_closure_state_mutations(CodeGenerator* gen, ASTNode* program);

/* `aetherc --diagnose=ownership` entry point. Prints to `out` the
   per-function string-ownership verdicts the codegen will use for the
   heap-string-tracker wrapper at codegen_stmt.c:1611-1631 — without
   actually running codegen. Lets downstreams find the "heap-string
   handed across an ownership-transfer boundary then variable
   reassigned" UAF shape at diagnose-time rather than at run-time. */
void codegen_diagnose_ownership(ASTNode* program, FILE* out);

// Header generation (for C embedding)
void emit_header_prologue(CodeGenerator* gen, const char* guard_name);
void emit_header_epilogue(CodeGenerator* gen);
void emit_message_to_header(CodeGenerator* gen, ASTNode* msg_def);
void emit_actor_to_header(CodeGenerator* gen, ASTNode* actor);
void generate_actor_definition(CodeGenerator* gen, ASTNode* actor);
void generate_function_definition(CodeGenerator* gen, ASTNode* func);
void generate_struct_definition(CodeGenerator* gen, ASTNode* struct_def);
void generate_main_function(CodeGenerator* gen, ASTNode* main);
void generate_statement(CodeGenerator* gen, ASTNode* stmt);
void generate_expression(CodeGenerator* gen, ASTNode* expr);
void generate_type(CodeGenerator* gen, Type* type);
void ensure_tuple_typedef(CodeGenerator* gen, Type* type);
void ensure_optional_typedef(CodeGenerator* gen, Type* type);   // #340
void collect_optional_typedefs(CodeGenerator* gen, ASTNode* node);   // #340
void emit_sum_typedef(CodeGenerator* gen, ASTNode* def);   // #914
void emit_enum_typedef(CodeGenerator* gen, ASTNode* def);  // #1044
void emit_optional_coerced(CodeGenerator* gen, ASTNode* value, Type* target);   // #340
int needs_optional_coerce(ASTNode* value, Type* target);   // #340
void emit_sum_coerced(CodeGenerator* gen, ASTNode* value, Type* target);   // #914
int needs_sum_coerce(ASTNode* value, Type* target);   // #914

// Utility functions
void indent(CodeGenerator* gen);
void print_indent(CodeGenerator* gen);
void print_line(CodeGenerator* gen, const char* format, ...);
void codegen_maybe_emit_line(CodeGenerator* gen, const ASTNode* node);
void print_expression(CodeGenerator* gen, ASTNode* expr);
const char* get_c_type(Type* type);

/* Current-position register for diagnostics raised from Type-only helpers
 * (get_c_type and friends), which have no AST node of their own to blame. */
void codegen_note_diag_pos(const ASTNode* node);
void codegen_note_diag_func(const char* name);
const char* codegen_diag_context(void);
const char* get_c_operator(const char* aether_op);

// Defer management
void push_defer(CodeGenerator* gen, ASTNode* stmt);
void push_defer_mode(CodeGenerator* gen, ASTNode* stmt, DeferMode mode);  /* #1140 */
void emit_defers_for_scope(CodeGenerator* gen);
void emit_all_defers(CodeGenerator* gen);
void enter_scope(CodeGenerator* gen);
void exit_scope(CodeGenerator* gen);

// Drain any in-flight try-frame pops at a non-local exit (return,
// panic, etc.) inside a try body.  No-op when not inside a try body.
// Issue #501.
void emit_try_pops_for_nonlocal_exit(CodeGenerator* gen);

// `break` / `continue` exit a loop but stay inside any try frames
// that were pushed BEFORE the loop body opened.  Drain only the
// frames pushed INSIDE the current loop body.  Issue #501.
void emit_try_pops_for_break_continue(CodeGenerator* gen);

// Issue #501 follow-up: track variables modified inside any try
// body of the current function.  Such locals — when declared in
// an enclosing scope of the try — must be lowered as `volatile T`
// so the C optimizer can't keep them in a register that gets
// clobbered by the panic's siglongjmp.
void mark_try_clobbered_var(CodeGenerator* gen, const char* name);
int  is_try_clobbered_var(CodeGenerator* gen, const char* name);
void clear_try_clobbered_vars(CodeGenerator* gen);

// Pre-pass: walk `body` and mark every variable assigned inside
// any AST_TRY_STATEMENT's body.  Called from
// generate_function_definition alongside hoist_heap_string_trackers
// (which it must run before, so the volatile prefix appears on
// the C decl).
void mark_try_clobbered_vars(CodeGenerator* gen, ASTNode* body);

// Return "volatile " when `name` is a try-clobbered local in this
// function AND we're emitting at an outer scope of any try body
// (i.e. gen->try_frame_depth == 0).  Empty string otherwise.
// Safe to concatenate verbatim before a C type at every
// AST_VARIABLE_DECLARATION emission site.  Issue #501 follow-up.
const char* try_volatile_qual_for(CodeGenerator* gen, const char* name);

#endif
