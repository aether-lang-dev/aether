#ifndef CODEGEN_H
#define CODEGEN_H

#include <stdio.h>
#include "../ast.h"
#include "../aether_strmap.h"
#include "../../runtime/actors/aether_message_registry.h"

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
    // #2124: the type a HOISTED local was declared with (parallel to
    // declared_vars; NULL for a local declared where it was bound). A
    // hoisted local is one C variable for the whole function, so a later
    // bare re-bind in a sibling branch or loop body with a value that
    // cannot flow into that type is reported in the language's terms
    // instead of by the C compiler.
    Type** declared_var_types;
    // #3083: name -> its first slot in declared_vars + 1 (NULL: not
    // declared), so a lookup is one probe, not a scan of every local.
    StrMap declared_index;
    // #3083: every name `reads_root` (the function being emitted) reads, by
    // tree_reads_name's rule, gathered by one walk of it.
    const ASTNode* reads_root;
    StrMap reads_index;
    // #2124: the body of the function (or main) being generated, walked
    // by the hoisters to find every binding of a name across sibling
    // branches and loop bodies, so a hoisted local is declared with the
    // numeric join of all of them (`int` in one branch, `long` in the
    // other: `int64_t`), the type the typechecker sees for it too.
    ASTNode* hoist_scope_body;
    // #701: names of module-level `var` globals (file-scope statics).
    // Populated once before any function body is emitted. A bare
    // `name = expr` inside a function whose name is in this set is a
    // WRITE to the global, not a new local declaration.
    char** module_global_vars;
    // Parallel to module_global_vars: 1 for the program's own top-level
    // `var`s (no origin_module), which a module's code never sees.
    unsigned char* module_global_var_is_program;
    // #3522: parallel too, 1 for a var declared `string`: the one kind that
    // has a lock of its own (a `ptr` var holding a string is a pointer, R4).
    unsigned char* module_global_var_is_string;
    // The module the function being emitted was merged from (its
    // origin_module), NULL for the program's own code. Set on entry to each
    // top-level function and main; closures inherit their function's.
    const char* current_origin_module;
    int module_global_var_count;
    int generating_lvalue;  // Track if we're generating an assignment target (lvalue)
    /* #3029: versioning on the unbounded-view sentinel. While a function's
     * view body is emitted, `slice_view_names` are its slice parameters,
     * every one an unbounded view there, indexed with no test of the
     * sentinel; `fn_cname_override` is that body's C name. While the
     * function itself is emitted, `slice_view_dispatch` names the same
     * parameters for the test at its entry that calls the view body. */
    const char** slice_view_names;
    int slice_view_count;
    const char* fn_cname_override;
    const char** slice_view_dispatch;
    int slice_view_dispatch_count;
    int in_const_init;  // #2799: depth inside a file-scope constant's initializer, where a
                        // constant named is written as its own initializer (ISO C)
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
    int lib_shared_runtime;  // #2297: the library links the shared runtime (catalog says so)
    const char* lib_id;      // #2931: the library's name (--lib-id), which names its string release
    int lib_actors;          // #2297: an imported binary library runs actors, so main() runs the scheduler
    struct ASTNode* trailing_stmt_call; // the call whose trailing block the current statement runs itself
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

    // Track generated pattern matching functions to avoid duplicates.
    // #2007: a set, asked once per top-level definition; a list walk made
    // that quadratic in the number of functions.
    StrMap generated_functions;

    // #2466: "Actor.field" for every actor state field the program uses as a
    // pointer (codegen_actor.c, state_field_is_ptr). Built by one walk of
    // the program on the first ask.
    StrMap actor_ptr_fields;
    int actor_ptr_fields_ready;

    // #2369: answers to "does this function / struct field / local always
    // hold a heap.new box?" (codegen_stmt.c, zb_*), keyed by the question.
    // The field and local questions walk the program or a function body, so
    // each is answered once.
    StrMap zeroed_box_memo;
    StrMap fault_members;        // a fault member's name -> its content literal (R1)

    // Defer stack: tracks deferred statements for LIFO execution at scope exit
    // As deep as the program needs (#3574): the stacks below were fixed
    // arrays (256 defers, 64 scopes, 64 loops, 64 switches), and past them
    // a defer was dropped (a string local's release among them), or a
    // scope, a loop or a switch went unrecorded while its exit still
    // popped one, so break, continue and the scope-exit drains acted on
    // another's. cg_defer_room, cg_scope_room, cg_loop_room and
    // cg_switch_room grow them; code_generator_release frees them.
    ASTNode** defer_stack;
    // #1140: parallel to defer_stack — which exits this defer fires on.
    // Kept as a separate array rather than stashed on the AST node because
    // push_defer receives the deferred STATEMENT (the AST_DEFER_STATEMENT
    // parent, which carries the qualifier, is dropped at the push site), and
    // because the node's `annotation` slot is already spoken for by the
    // synthetic cleanup carriers (`heap_string_exit_free:` and friends).
    DeferMode* defer_mode;
    int defer_cap;
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
    int* scope_defer_start;  // defer_count at scope entry
    int scope_cap;
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
    int* loop_try_base;
    int loop_nest_cap;
    int loop_nest_depth;

    // Labeled break/continue (#893). Parallel to loop_try_base, indexed by
    // loop_nest_depth. For each enclosing loop: its source label (or NULL),
    // the scope_depth of the scope CONTAINING the loop (so a labeled
    // break/continue can emit defers for exactly the scopes it unwinds), a
    // unique id used to mint the C goto labels, and whether the break/continue
    // target labels were actually referenced (so only a C label that is used
    // gets emitted, avoiding -Wunused-label).
    const char** loop_label;
    int* loop_label_scope;
    int* loop_label_id;
    int* loop_label_break_used;
    int* loop_label_continue_used;
    /* #2742: the loop steps in a statement at the end of its body (a
     * C-style for whose increment is a binding), after its __ae_cont_N
     * label, so a `continue` to it jumps there instead of C `continue`,
     * which would skip the step. */
    int* loop_step_in_body;
    int next_loop_label_id;
    /* #2378 threaded dispatch: a `while true { head; switch sel { ... } }`
     * loop lowers to computed goto where the C compiler has labels-as-values.
     * loop_td_id[level] is the loop's threaded id (0 = not threaded), and
     * loop_td_node[level] the loop, so a `continue` that targets it can emit
     * the head and the indirect jump. td_switch / td_id / td_arm mark the
     * loop's own switch while its arms are generated, so each arm gets a label.
     * next_td_id numbers threaded loops per translation unit. */
    int* loop_td_id;
    struct ASTNode** loop_td_node;
    struct ASTNode* td_switch;
    int td_id;
    int td_arm;
    int next_td_id;

    /* #2696: an unlabeled `break` leaves the innermost loop or `switch`.
     * The switches around the statement being generated, innermost last;
     * one is the target when it began inside the innermost loop
     * (switch_loop_depth == loop_nest_depth). For each: the scope_depth
     * containing it and the try_frame_depth at its entry, so a break runs
     * the defers and pops the try frames of only what it leaves, and a C
     * label id (__ae_brk_N, numbered with the loops' labels) for a break
     * that cannot be a C `break`. */
    int switch_nest_depth;
    int switch_nest_cap;
    int* switch_loop_depth;
    int* switch_scope;
    int* switch_try_base;
    int* switch_c_depth;
    int* switch_brk_id;
    int* switch_brk_used;
    /* #2696: the C statements a C `break` leaves (a loop or a `switch`)
     * open around the code being generated. A break is a C `break` only
     * when its target's own C statement is the innermost of them
     * (loop_c_depth / switch_c_depth equal to this); otherwise a C statement
     * the target did not emit stands between (a sum `match` lowers to a C
     * `switch`) or the target emitted none (a ranged `switch` lowers to an
     * if-chain, switch_c_depth -1), and the break jumps to the target's
     * __ae_brk_N label instead. */
    int c_break_depth;
    int* loop_c_depth;

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
    // at declaration sites (try_volatile_qual_for). Each name carries
    // the deepest nesting of `try` bodies, counted from the scope the
    // walk started at (a function, a closure, a receive arm), that
    // writes it: a local declared at a shallower depth crosses that
    // setjmp and needs volatile.
    char** try_clobbered_vars;
    int* try_clobbered_depths;
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
                             //   retains the pointer beyond the call, so the
                             //   call hands it a reference of its own (R4).
                             //   See #420 follow-up. NULL when no param
                             //   carries the annotation.
        int* params_noescape; // 1 per index when the param was declared
                             //   `name: @noescape ptr` (or `fn`): the function
                             //   uses the argument only during the call, so a
                             //   closure passed there is released by the caller
                             //   once the call returns, as after an Aether
                             //   callee that keeps nothing (#2523). NULL when
                             //   no param carries the annotation.
        int* params_holds;   // 1 per index when the param was declared
                             //   `name: @into ptr` (the call's result holds the
                             //   argument past the call, #2990) or `name: @keep
                             //   ptr` (a binary import's stub for an Aether
                             //   function that keeps it, #3013), so a closure
                             //   passed there is given a heap box with a
                             //   reference of its own, as for `@retain`. NULL
                             //   when no param carries the annotation.
        int param_count;
        int is_variadic;     // declared with a trailing `...`: an argument
                             //   past param_count is a C vararg (#2914)
        Type* ret_type;      // #1286: the declared return type (borrowed from
                             // the extern's AST), so a call returning `T[]`
                             // can be wrapped into a slice view.
    }* extern_registry;
    int extern_registry_count;
    int extern_registry_capacity;
    /* #3546: name -> registry index + 1, of the first entry of each name,
     * filled as entries are registered (find_extern_registry_index). */
    StrMap extern_registry_index;

    // MSVC compat: counter for ask-operator temp variables (_ask_result_N)
    int ask_temp_counter;

    /* #3151: the C int the next ask expression writes its status to
     * (AETHER_ASK_*), set by `v, err = actor ? Msg {}` for its ask alone;
     * NULL for an ask whose failure reads as the zero value. */
    const char* ask_status_var;

    // Counter for message-send array hoist variables (_aether_arr_N). Every
    // `msg ! Foo { field: [a, b, c] }` with a composite array field
    // allocates a `static const T _aether_arr_N[] = {...}` at the send
    // site so the array's storage outlives the send-expression block and
    // the receiving actor can safely read through the pointer.
    int msg_arr_counter;

    // Match-as-expression: when non-NULL, match arms assign to this variable
    const char* match_result_var;
    // #2461: the result variable is an owning string local: each value arm
    // takes its value and releases the one the local held.
    int match_result_string;
    // #2497: when non-NULL, the result is a struct of this name that owns
    // heap strings: each value arm is taken with emit_struct_take, and when
    // match_result_replace is set it replaces the value the result held.
    const char* match_result_struct;
    int match_result_replace;
    // #2514: when non-NULL, the result variable is a promoted capture of
    // this type: each value arm stores into its cell (emit_cell_store).
    Type* match_result_cell;
    // When non-NULL, the result is an optional of this type: each value arm
    // is wrapped into it as a value bound to it would be (a bare value, or
    // `none`), and a `string?` result's ownership flag follows the arm.
    Type* match_result_opt;
    // #2993: when non-NULL, the result is the temporary of a `match` used
    // as an operand, of this owning type or fixed-size array type: each
    // value arm is taken into it and the value it held dropped
    // (emit_owning_store), or an array filled (emit_array_fill). The
    // expression around the `match` reads the temporary as a place.
    Type* match_result_own;
    // #2993: when non-NULL, the declaration `x = match ...` being bound:
    // each value arm is stored into its place as `x = <the arm's value>`
    // stores it (emit_bound_match_store), whatever the place is: a local, a
    // closure's cell, a module variable, an actor's state.
    struct ASTNode* match_result_bound;
    // #2996: when non-NULL, the type of the place a returned or operand
    // `match` stores into (the result returned, the operand's temporary):
    // an arm giving a variant of a sum that place is takes it wrapped into
    // the sum.
    Type* match_result_type;

    // #2054: set while the body of a string-returning closure is emitted.
    // A closure is called through a value its caller cannot classify, so
    // every string it returns is handed over owned (the uniform-heap wrap
    // a heap-returning function's returns get), and every string result of
    // call() is tracked as owned on the caller's side.
    int in_string_closure;

    // Cooperative preemption: insert sched_yield() at loop back-edges
    int preempt_loops;

    // Nonzero while the series collapse emits the loop it could not replace
    // (its overflow fallback), so that loop is emitted as written (#2271).
    int series_collapse_off;

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

    /* #2520: the string literals of the program that hold a NUL, one entry
     * per distinct byte sequence. Each is emitted once, at file scope, as a
     * static pinned AetherString `_ae_slit_<i>` (emit_static_string_literals),
     * and every use of the literal is the address of that object. */
    char** static_str_bytes;
    int* static_str_lens;
    int static_str_count;
    int static_str_capacity;
    /* Which of them the C names, the pinned string (bit 1) or only its
     * bytes (bit 2), and the offset in `output` their definitions go in at
     * once the program is written (emit_used_static_string_literals); -1
     * when every one was written up front instead, the output being one
     * that cannot be read back. A literal every use of which is written as
     * plain C characters ("x" to puts) is left out: an unused static const
     * is -Wunused-const-variable, an error under -Wall -Werror. */
    unsigned char* static_str_used;
    long static_str_pos;

    // Builder function registry: functions with _ctx: ptr as first param
    // get builder_context() auto-injected at call sites inside trailing blocks
    char** builder_funcs;
    int builder_func_count;
    int builder_func_capacity;
    int in_trailing_block;  // >0 when generating code inside a trailing block
    /* #2791: set by emit_builder_block_body for the trailing block it is about
     * to emit: the builder call's config variable and the function that
     * releases it, so a way out of the block before the builder runs
     * releases it. NULL otherwise; emit_trailing_block_body consumes it. */
    const char* builder_cfg_var;
    const char* builder_cfg_release;

    // The call whose result value its expression statement throws away, set
    // just before the statement emits it. A VOID/discarded parent call still
    // drains its heap-returning inline args (the arg-temp wrap emits a
    // void-yielding `(t = ..., call(...), free(t))`). It is that call
    // node's alone: one emitted ahead of it, an argument or an operand the
    // evaluation-order wrapper hoists, is a different node and keeps its
    // value. A flag consumed by the first call emitted went to the hoisted
    // operand instead (#2585, #2589).
    ASTNode* discard_call_node;

    // When emitting a closure body, captures in this list are mutated and
    // therefore routed through _env->name on reassignment. Set per-closure
    // by emit_closure_definitions, cleared after the body. NULL when not
    // currently emitting a closure body.
    char** current_env_captures;
    int current_env_capture_count;
    // The captures the closure being emitted only reads, bound at entry as
    // `T name = _env->name;`: the env keeps the value, the name borrows it.
    const char** current_alias_captures;
    int current_alias_capture_count;

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
    /* #2847: how many of them have a prototype ahead of the functions
     * (emit_bare_fn_adapter_decls), so an erased call may name them. */
    int    bare_fn_adapter_declared;
    /* #2478: what calling each function of the program can change (a module
     * global, memory a parameter reaches), memoised per definition by
     * order_fn_effects in codegen_expr.c. */
    struct OrderFnEffects* order_fn_effects;
    int    order_fn_effect_count;
    int    order_fn_effect_capacity;
    int*   order_fn_index;       /* their entries by definition, hashed */
    int    order_fn_index_cap;

    // Closure support: track closures for hoisted C function generation
    int closure_counter;    // unique ID for closure env structs and functions
    // Map variable names to closure IDs (set during variable declaration codegen)
    struct ClosureVarMap {
        char* scope;        // #2513: the scope declaring the variable
        char* var_name;
        int closure_id;
    }* closure_var_map;
    int closure_var_count;
    int closure_var_capacity;
    /* #2513: the scope whose variables closure_var_map lookups resolve
     * against while a body is emitted: a function's name, "main", or a
     * receive arm's or a hoisted closure's synthetic name, as
     * discover_closures_scoped names them. */
    const char* closure_var_scope;
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
    // #2894: a call returning an array is wanted as its result struct
    // (a fill or a return takes it), not held in a statement temporary.
    int array_call_raw;
    // #2690: parallel to heap_string_vars, how many `try` bodies of the
    // function enclose each one's C declaration (gen->try_frame_depth when
    // it was marked). A store nested in more of them journals the string
    // for the frame the declaration is in (emit_unwind_track_local).
    int* heap_string_var_try_depths;


    // #2499 copy-on-keep: the `string` parameters of the body being
    // generated that hold a reference of their own (captured on entry),
    // heap-tracked locals from there on. Their exit free, when one is
    // armed, is journaled for panic unwinding as soon as it is
    // (push_heap_string_exit_free_defers), before any statement runs.
    char** captured_string_params;
    int captured_string_param_count;


    // #2369: which pointer-to-struct parameters only ever hold a heap.new
    // box, because every call passes one (ZbParams, private to
    // codegen_stmt.c). Built once per program, the first time a box is
    // asked about: 0 not built, 1 building, 2 built.
    void* zb_params;
    int zb_params_state;

    // #2618: the program's `string[N]` locals and parameters, which of them
    // own their elements, and which element reads are views (StrArrs,
    // private to codegen_stmt.c). Built once, the first time it is asked.
    void* str_arrays;



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
    /* AST nodes codegen synthesises rather than reads from the program: the
     * defer carriers, the free-call statements it builds for scope exits, the
     * folded literals the optimizer substitutes. They are not reachable from
     * the program AST, so free_ast_node(program) never sees them and every
     * one of them used to leak (#1667). Owned here, released in
     * free_code_generator. */
    ASTNode** synthesised_nodes;
    int synthesised_count;
    int synthesised_cap;
    /* The program uses the sandbox builtins (sandbox_push and friends), so
     * the sandbox stack and checker are emitted. A `try` then saves the
     * sandbox depth and its catch restores it: a panic that unwinds out of
     * an enforced block, or out of a trusted call inside one, must not leave
     * the depth where the panic found it. */
    int uses_sandbox;
    /* #2765: the temporaries an expression needs, declared ahead of the
     * statement that holds it, so the expression is a C comma expression:
     * a GNU statement expression `({ ... })` is not C, and cl.exe rejects
     * it. They are declared ahead of the outermost statement
     * generate_statement is emitting, whose text starts at hoist_start in
     * `output` (-1 while none is, or when the output cannot be read back
     * and rewritten); hoist_decls holds the declarations collected for it.
     * hoist_output_ok is 0 until probed, 1 when `output` can be rewritten,
     * -1 when it cannot (a temporary is then a statement-expression local,
     * which only a GCC-compatible compiler takes). */
    int hoist_depth;
    long hoist_start;
    char* hoist_decls;
    size_t hoist_decls_len;
    size_t hoist_decls_cap;
    /* #2923: a plain `defer` a panic leaves runs from the unwind journal,
     * through a thunk written where the defer is: the thunks, how many, and
     * the offset their prototypes go in at once the program is written. */
    char* defer_thunks;
    size_t defer_thunks_len;
    size_t defer_thunks_cap;
    int defer_thunk_count;
    long defer_proto_pos;
    /* #3413: the drop functions of owned pointer types the unwind journal
     * runs through a thunk (owned_drop_thunk), each named once; the thunks
     * are written with the defer thunks. */
    const char** owned_thunks;
    int owned_thunk_count;
    int owned_thunk_cap;
    int hoist_output_ok;
    /* Was a `goto main_exit;` written (a `return` in main)? Its label is
     * written only then. */
    int main_exit_targeted;
    /* program_releases_messages, once per program: 0 not known yet, 1 no,
     * 2 yes, for the program it was worked out for (#3142). */
    int releases_messages;
    const void* releases_messages_of;
} CodeGenerator;

// Code generation functions
CodeGenerator* create_code_generator(FILE* output);
CodeGenerator* create_code_generator_with_header(FILE* output, FILE* header, const char* header_path);
void free_code_generator(CodeGenerator* gen);
/* Room for one more defer, scope, enclosing loop or enclosing switch on
 * the generator's stacks, which grow as deep as the program nests (#3574). */
void cg_defer_room(CodeGenerator* gen);
void cg_scope_room(CodeGenerator* gen);
void cg_loop_room(CodeGenerator* gen);
void cg_switch_room(CodeGenerator* gen);
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
void emit_c_string_literal(CodeGenerator* gen, const char* str);
void emit_c_string_body(CodeGenerator* gen, const char* str, int printf_format);  // #2512
/* #2520: `len` bytes, NULs included, as the inside of a C string literal. */
void emit_c_string_bytes(CodeGenerator* gen, const char* str, size_t len, int printf_format);
/* A string literal node as an Aether string value: the static, pinned
 * AetherString that carries it (R1). */
void emit_string_literal_node(CodeGenerator* gen, const ASTNode* lit);
/* A string literal node as the characters C reads (R4): a C string literal,
 * or the pinned string's byte array when the literal holds a NUL. */
void emit_string_literal_chars(CodeGenerator* gen, const ASTNode* lit);
/* #2520: `lit` as an fwrite of all its bytes to stdout (print of a literal). */
void emit_string_literal_write(CodeGenerator* gen, const ASTNode* lit, int newline);
void emit_print_literal_format(CodeGenerator* gen, const ASTNode* lit);
int  static_string_literal_index(CodeGenerator* gen, const char* bytes, int len);
void emit_static_string_literals(CodeGenerator* gen, ASTNode* program);
void emit_used_static_string_literals(CodeGenerator* gen);
int  binary_is_string_compare(const ASTNode* expr);                      // #2515
void emit_string_compare_open(CodeGenerator* gen, const char* op);       // #2515
void emit_string_compare_close(CodeGenerator* gen, const char* op);      // #2515   // #2467
void generate_type(CodeGenerator* gen, Type* type);
void ensure_tuple_typedef(CodeGenerator* gen, Type* type);
void ensure_optional_typedef(CodeGenerator* gen, Type* type);   // #340
const char* c_result_type(Type* t);   // #2894
void collect_optional_typedefs(CodeGenerator* gen, ASTNode* node);   // #340
void collect_tuple_typedefs(CodeGenerator* gen, ASTNode* node);      // #3077
void emit_sum_typedef(CodeGenerator* gen, ASTNode* def);   // #914
void emit_enum_typedef(CodeGenerator* gen, ASTNode* def);  // #1044
void emit_optional_coerced(CodeGenerator* gen, ASTNode* value, Type* target);   // #340
int needs_optional_coerce(ASTNode* value, Type* target);   // #340
int emit_int_ptr_bridged(CodeGenerator* gen, ASTNode* expr, TypeKind target);  // #2218
void emit_sum_coerced(CodeGenerator* gen, ASTNode* value, Type* target);   // #914
int needs_sum_coerce(ASTNode* value, Type* target);   // #914
/* #3071: a variant as an array literal's element in an array of a sum,
 * wrapped as the sum (and taken, when the sum owns); 0 when it needs none. */
int emit_sum_element(CodeGenerator* gen, ASTNode* el, Type* elem);

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
void codegen_own_node(CodeGenerator* gen, ASTNode* node);
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
int  is_try_clobbered_var(CodeGenerator* gen, const char* name);
void clear_try_clobbered_vars(CodeGenerator* gen);

// The set belongs to one C function's body. A body emitted while another
// is open (a closure's) saves the open one's set and restores it after.
typedef struct {
    char** names;
    int* depths;
    int count;
} TryClobberedSet;
void save_try_clobbered_vars(CodeGenerator* gen, TryClobberedSet* saved);
void restore_try_clobbered_vars(CodeGenerator* gen, TryClobberedSet* saved);

// Pre-pass: walk `body` and mark every variable assigned inside
// any AST_TRY_STATEMENT's body, with how deeply such bodies nest
// there.  Run for each C function's body (a function, main, a
// closure, a receive arm) after the set is cleared and before its
// signature, its parameter or pattern bindings and its hoists are
// emitted, so every declaration of a try-written name sees it.
void mark_try_clobbered_vars(CodeGenerator* gen, ASTNode* body);

// Return "volatile " when `name` is written inside a `try` body nested
// deeper than the declaration being emitted (gen->try_frame_depth):
// the local then holds a value the setjmp of that `try` must see after
// a panic. Empty string otherwise. Issue #501 follow-up; a local
// declared inside one `try` and written in a nested one, or a catch
// binding written in a `try` of its handler, is volatile too (#2739).
const char* try_volatile_qual_for(CodeGenerator* gen, const char* name);

// The C type `c_type` qualified by `vq` (try_volatile_qual_for's answer)
// as a declaration of the variable spells it: after the `*` of a pointer
// (`const char* volatile`), before anything else. #2618.
const char* volatile_decl_type(const char* vq, const char* c_type);

// #2931: the function a library built from `source_file` (or as `package`)
// exports for giving back a string one of its exports returned.
const char* lib_string_release_symbol_for(const char* lib_id, const char* package,
                                          const char* source_file);

#endif
