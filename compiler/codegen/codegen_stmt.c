#include "codegen_internal.h"
#include "optimizer.h"
#include "../aether_module.h"
#include "../analysis/contract_eval.h"
#include "../analysis/typechecker.h"
#include "../analysis/hoist.h"
#include "../aether_error.h"
#include <limits.h>
#include <stdint.h>

// ============================================================================
// ARITHMETIC SERIES LOOP COLLAPSE
//
// Detects while loops of the form:
//   while counter < bound {              // or <=
//       acc1 = acc1 + invariant_expr1    // any number of accumulators
//       acc2 = acc2 + counter            // or counter * C: a linear sum
//       counter = counter + step         // a positive integer literal
//   }
//
// and replaces them with the closed form. With c0 the counter on entry and
// T the trip count,
//   T = ceil((bound - c0) / step)          for <
//   T = floor((bound - c0) / step) + 1     for <=
// the loop leaves
//   acc     = acc + invariant * T
//   acc     = acc + C * (T*c0 + step * T*(T-1)/2)
//             (T*(T+1)/2 when the accumulator follows the increment and so
//             adds the stepped value)
//   counter = c0 + step * T
//
// Works for any starting value and any bound expression, runtime ones
// included; a guard keeps the accumulators unchanged when the loop would
// not run at all.
//
// Only integer loops are collapsed: an `int` or `long` counter, bound,
// accumulators and addends. Those wrap (-fwrapv), and every step of the
// closed form is exact modulo 2^64, so the result is the loop's to the
// bit. A float series is left alone: repeated addition rounds at every step
// and the product does not. (Before #2271 a float loop went through
// (int64_t) and ended at its bound: `while a < 0.0 { a = a + 6.28 }` from
// -0.51 left 0.0, and an integer step that did not divide the distance was
// truncated the same way.)
//
// The one wrap the closed form cannot follow is the counter's own last
// step: when c0 + step*T passes the counter type's maximum, the loop wraps
// round and carries on. That is checked at run time, and the loop then
// runs as written.
// ============================================================================

#define MAX_SERIES_ACCUMULATORS 16

/* printf into a fresh heap string as long as the text needs, for the
 * defer annotations and memo keys built from names. A fixed buffer cut a
 * long name, and the cut text named a different variable or answered for
 * one that shares its prefix (#2539). The caller frees the result; out of
 * memory ends the compile, as aether_xrealloc does. */
static char* heap_strf(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
static char* heap_strf(const char* fmt, ...) {
    size_t cap = 128;
    for (;;) {
        char* s = (char*)aether_xrealloc(NULL, cap);
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(s, cap, fmt, ap);
        va_end(ap);
        if (n >= 0 && (size_t)n < cap) return s;
        free(s);
        /* A C library that answers -1 for a cut (old msvcrt) gets doubling. */
        cap = n >= 0 ? (size_t)n + 1 : cap * 2;
    }
}

/* #2333: can a `catch NAME` binding be a heap-tracked local? Not when
 * NAME is reached through something other than a plain C local in the
 * handler (a closure-env field, a promoted capture cell, actor state):
 * those keep the borrowed binding. */
static int catch_binding_can_own(CodeGenerator* gen, const char* name) {
    if (!gen || !name || strcmp(name, "_") == 0) return 0;
    if (is_promoted_capture(gen, name)) return 0;
    for (int e = 0; e < gen->current_env_capture_count; e++) {
        if (gen->current_env_captures[e] &&
            strcmp(gen->current_env_captures[e], name) == 0) return 0;
    }
    if (gen->current_actor) {
        for (int s = 0; s < gen->state_var_count; s++) {
            if (gen->actor_state_vars[s] &&
                strcmp(gen->actor_state_vars[s], name) == 0) return 0;
        }
    }
    return 1;
}

/* A string local that lives for one C block and shadows any local of its
 * name there: a `catch` name (#2333) or a C-style for loop's string variable
 * (#2742). While the block is generated the name is a string local declared
 * at this try depth (#2690); scoped_string_local_end rolls its marks back,
 * so an enclosing variable of the same name keeps its own. */
typedef struct {
    const char* name;
    int was_heap;
    int shadowed_depth;
} ScopedStringLocal;

static void scoped_string_local_begin(CodeGenerator* gen, ScopedStringLocal* sl,
                                      const char* name, ASTNode* block,
                                      int block_marks_only) {
    sl->name = name;
    sl->was_heap = is_heap_string_var(gen, name);
    mark_heap_string_var(gen, name);
    /* #2690: declared here, whatever a local it shadows was. */
    sl->shadowed_depth = set_heap_string_var_try_depth(gen, name, gen->try_frame_depth);
}

/* The release of the value the scoped local holds when its block ends,
 * queued on the scope the caller has entered for the block, so it runs on
 * fall-through, on a `break` or `continue` out of the block and on a
 * `return` alike. */
static void push_scoped_string_local_free(CodeGenerator* gen, const ScopedStringLocal* sl,
                                          int line, int column) {
    ASTNode* carrier = create_ast_node(AST_EXPRESSION_STATEMENT, NULL, line, column);
    if (!carrier) return;
    if (carrier->annotation) free(carrier->annotation);
    carrier->annotation = heap_strf("heap_string_exit_free:%s", sl->name);
    codegen_own_node(gen, carrier);
    push_defer(gen, carrier);
    print_indent(gen);
    fprintf(gen->output, "aether_unwind_track_str_place((const void*)&%s, NULL);\n", sl->name);
}

static void scoped_string_local_end(CodeGenerator* gen, const ScopedStringLocal* sl) {
    if (!sl->was_heap) unmark_heap_string_var(gen, sl->name);
    else set_heap_string_var_try_depth(gen, sl->name, sl->shadowed_depth);
}

/* `const char* <name> = NULL;`, a string local that holds no value yet
 * (#2690: volatile where a `try` body writes it), recorded as a string so a
 * binding of another kind is checked against it (#2124). */
static void emit_string_local_decl(CodeGenerator* gen, const char* name) {
    print_indent(gen);
    fprintf(gen->output, "%s %s = NULL;\n",
            volatile_decl_type(try_volatile_qual_for(gen, name), "const char*"), name);
    Type* st = create_type(TYPE_STRING);
    mark_var_declared_typed(gen, name, st);
    free_type(st);
}

/* The catch branch of a try whose binding is a string local of the
 * handler's scope (R2): it owns the reason, as the catcher was handed it
 * (aether_panic_owned) or taken (a literal as it is, a C string copied).
 * Emits, inside the `else` of the setjmp test:
 *
 *   if (!_aether_try_N->reason_release) NAME = aether_uniform_heap_str(NAME, 0);
 *   aether_try_pop();
 *   { aether_unwind_track_str_place(&NAME, NULL);   // outer frame's journal
 *     handler ... ; deferred: aether_heap_str_free(NAME); }
 *
 * The C declarations shadow any enclosing ones, and the release is a defer
 * of the handler's own scope, so it runs on fall-through, `break`/`continue`,
 * and `return` alike. */
static void emit_owned_catch_handler(CodeGenerator* gen, const char* name,
                                     ASTNode* handler, int uid) {
    int saved_var_count = gen->declared_var_count;

    print_line(gen, "if (!_aether_try_%d->reason_release) %s = aether_uniform_heap_str(%s, 0);",
               uid, name, name);
    print_line(gen, "aether_try_pop();");

    ScopedStringLocal sl;
    scoped_string_local_begin(gen, &sl, name, handler, 0);


    print_line(gen, "{");
    indent(gen);
    enter_scope(gen);
    push_scoped_string_local_free(gen, &sl, handler->line, handler->column);
    generate_statement(gen, handler);
    exit_scope(gen);
    unindent(gen);
    print_line(gen, "}");
    truncate_declared_vars(gen, saved_var_count);
    scoped_string_local_end(gen, &sl);
}

/* `panic(expr)`. A reason the program built at run time is handed to the
 * catcher to own (#2333): a heap-classified expression goes through
 * aether_panic_owned with the heap-string free, and a string local moves
 * its reference out (left empty, so neither the unwind drain nor its
 * scope-exit release reclaims what the catcher now holds). Anything else
 * (a literal, a borrowed string) is borrowed as before. */
static void emit_panic_call(CodeGenerator* gen, ASTNode* arg) {
    if (arg && arg->type == AST_IDENTIFIER && arg->value &&
        is_heap_string_var(gen, arg->value) && catch_binding_can_own(gen, arg->value)) {
        const char* v = arg->value;
        print_line(gen, "{ const char* _ae_pr = %s; %s = NULL; "
                        "aether_panic_owned(_ae_pr, aether_unwind_free_str); }",
                   v, v);
        return;
    }
    print_indent(gen);
    if (arg && arg->type != AST_IDENTIFIER && is_heap_string_expr(gen, arg)) {
        fprintf(gen->output, "aether_panic_owned(");
        generate_expression(gen, arg);
        fprintf(gen->output, ", aether_unwind_free_str);\n");
        return;
    }
    fprintf(gen->output, "aether_panic(");
    generate_expression(gen, arg);
    fprintf(gen->output, ");\n");
}

// Returns 1 if the expression tree references the named variable.
static int expr_references_var(ASTNode* node, const char* var_name) {
    if (!node || !var_name) return 0;
    if (node->type == AST_IDENTIFIER && node->value &&
        strcmp(node->value, var_name) == 0) return 1;
    for (int i = 0; i < node->child_count; i++) {
        if (expr_references_var(node->children[i], var_name)) return 1;
    }
    return 0;
}

// Counts identifier-node occurrences of `var_name` in the subtree.
// Assignment/declaration targets are carried on `->value` of the
// statement node (not as identifier children), so this naturally
// counts reads/uses, not write targets.
static int count_var_identifier_uses(ASTNode* node, const char* var_name) {
    if (!node || !var_name) return 0;
    int n = (node->type == AST_IDENTIFIER && node->value &&
             strcmp(node->value, var_name) == 0) ? 1 : 0;
    for (int i = 0; i < node->child_count; i++) {
        n += count_var_identifier_uses(node->children[i], var_name);
    }
    return n;
}

// Decides whether `src_name` (the bare-identifier RHS of an alias
// assignment `dest = src`) is still live after the alias point, in
// which case the alias must NOT move its reference out; it takes one
// of its own instead.
//
// The move hands the source's reference to the alias and leaves the
// source empty: a last-use optimization, only sound when the source is
// dead after the alias. If the source is read again (e.g. `content`
// read after a `rest = content` alias-then-reassign loop), a move once
// freed the source's buffer out from under it (see 180-regression.md).
//
// We approximate liveness conservatively: if the source identifier
// appears anywhere in the function besides the single alias-init use,
// assume it may be read later and take it. Worst case we take when a
// move would have sufficed; we never wrongly move. With no function
// context, default to the take. A struct literal's field takes a local
// by the same rule (#2602).
int alias_source_must_copy(CodeGenerator* gen, const char* src_name) {
    if (!gen || !gen->current_function || !src_name) return 1;
    return count_var_identifier_uses(gen->current_function, src_name) > 1;
}

/* #2461: taking a string into an owning slot (a local, a struct field, a
 * return value) from an expression that only VIEWS a buffer someone else
 * owns.
 *
 * A struct owns the strings in its fields (#465): it releases one when the
 * field is reassigned, when the struct is replaced, and when it goes out of
 * scope. A field read is therefore a view of the struct's buffer, and so is
 * an `if` or `match` whose chosen arm is a field read or a string local.
 * The slots once took such a value as a borrow with nothing keeping the
 * buffer alive, so the value dangled as soon as the source let go of it.
 * The slot now takes it arm by arm (emit_string_take). */

/* A read of a `string` field of an Aether struct, by value or through a
 * pointer. A header-defined struct's field is the C header's `const char*`,
 * which the struct does not own, so reading it views nothing the struct
 * releases. */
int is_owned_string_field_read(ASTNode* e) {
    if (!e || e->type != AST_MEMBER_ACCESS || !e->value ||
        e->child_count < 1 || !e->children[0]) return 0;
    if (!e->node_type || e->node_type->kind != TYPE_STRING) return 0;
    Type* ot = e->children[0]->node_type;
    const char* sname = NULL;
    if (ot && ot->kind == TYPE_STRUCT) {
        sname = ot->struct_name;
    } else if (ot && ot->kind == TYPE_PTR && ot->element_type &&
               ot->element_type->kind == TYPE_STRUCT) {
        sname = ot->element_type->struct_name;
    }
    return sname && !aether_is_c_import_struct(sname);
}

/* #2618: string arrays, and the elements a slot takes as views.
 *
 * A local `string[N]` array owns its elements, as a string array cell does
 * (#2474): an element is a counted string the array holds a reference to,
 * or a literal it holds as it is. A store takes a value of its own and
 * gives back the one it replaces (_aether_str_cell_set), the scope exit
 * gives back what is left, and a parameter or a closure's capture takes a
 * reference to each element it copies in (_aether_str_elem_dup). Before,
 * nothing owned an element: a fresh string stored there leaked, and a view
 * stored there (a field of a statement temporary) dangled once its owner
 * let go of it.
 *
 * Only an array some store into which is not provably a literal pays for
 * this: a table of literals keeps the plain C array it was, with nothing to
 * release at its exit. An element of an owning array, of a `string[N]`
 * parameter (whose elements are its caller's, maybe an owning array's), of
 * a `string[N]` struct field (#2528), of an actor's `string[N]` state or of
 * a `string[]` slice (#2689, which a local borrows instead,
 * is_slice_string_element) is a view: a slot that keeps it takes a copy,
 * as it does a struct's string field (string_take_is_view), and a function
 * returning one hands its caller a copy (return_expr_is_heap).
 *
 * Which array a name means is worked out once for the whole program, by
 * scope: the body of each function, closure and receive arm (the node the
 * codegen emits it with as hoist_scope_body) declares its parameters and
 * locals, and a use of a name belongs to the innermost scope declaring it.
 * So the answer does not depend on which function is being emitted when it
 * is asked, as the memoised return classifiers require. */
typedef struct {
    ASTNode* scope;     /* the body (or the actor) declaring it */
    const char* name;
    int is_array;       /* a `string[N]`; any other declaration only shadows */
    int param;          /* a parameter: its elements are its caller's */
    int state;          /* an actor's state field, owned by the actor */
    int owns;           /* some store into it is not provably a literal */
} StrArr;

typedef struct {
    ASTNode* scope;     /* the scope's key, or NULL for an expression closure */
    ASTNode* closure;   /* the closure this frame is the body of, or NULL */
} StrArrFrame;

#define STR_ARR_MAX_DEPTH 128

typedef struct {
    StrArr* arrs;
    int count, cap;
    StrMap at;              /* "<scope>|<name>" -> index + 1 */
    StrMap names;           /* every name some declaration binds as a `string[N]` */
    /* Element reads, settled once every store is seen. */
    ASTNode** uses;
    int* use_arrs;
    int use_count, use_cap;
    StrMap views;           /* "<element read>" -> 1, or 2 when the array owns */
    StrArrFrame frames[STR_ARR_MAX_DEPTH];
    int depth;
} StrArrs;

static int str_arr_type(const Type* t) {
    return t && type_is_sized_array(t) && t->array_size > 0 && t->element_type &&
           t->element_type->kind == TYPE_STRING;
}

/* The names declared as a `string[N]` anywhere (a local, a parameter, a
 * state field): only a use of one of them can mean such an array, so the
 * walk resolves nothing else. */
static void str_arr_collect_names(StrArrs* sa, ASTNode* n) {
    if (!n) return;
    if ((n->type == AST_VARIABLE_DECLARATION || n->type == AST_PATTERN_VARIABLE ||
         n->type == AST_CLOSURE_PARAM || n->type == AST_STATE_DECLARATION) &&
        n->value && str_arr_type(n->node_type))
        strmap_put(&sa->names, n->value, (void*)1);
    for (int i = 0; i < n->child_count; i++) str_arr_collect_names(sa, n->children[i]);
}

static int str_arr_add(StrArrs* sa, ASTNode* scope, const char* name, int is_array,
                       int param, int state) {
    char* key = heap_strf("%p|%s", (void*)scope, name);
    void* v = strmap_get(&sa->at, key);
    int idx;
    if (v) {
        idx = (int)(intptr_t)v - 1;
    } else {
        if (sa->count == sa->cap) {
            sa->cap = sa->cap ? sa->cap * 2 : 32;
            sa->arrs = aether_xrealloc(sa->arrs, sizeof(*sa->arrs) * (size_t)sa->cap);
        }
        idx = sa->count++;
        memset(&sa->arrs[idx], 0, sizeof(sa->arrs[idx]));
        sa->arrs[idx].scope = scope;
        sa->arrs[idx].name = name;
        strmap_put(&sa->at, key, (void*)(intptr_t)(idx + 1));
    }
    free(key);
    sa->arrs[idx].is_array |= is_array;
    sa->arrs[idx].param |= param;
    sa->arrs[idx].state |= state;
    return idx;
}

/* The innermost declaration of `name` in the frames below `top`, or -1;
 * its frame goes to `*frame`. */
static int str_arr_resolve(StrArrs* sa, const char* name, int top, int* frame) {
    if (!strmap_has(&sa->names, name)) return -1;
    for (int f = top - 1; f >= 0; f--) {
        if (!sa->frames[f].scope) continue;
        char* key = heap_strf("%p|%s", (void*)sa->frames[f].scope, name);
        void* v = strmap_get(&sa->at, key);
        free(key);
        if (v) {
            if (frame) *frame = f;
            return (int)(intptr_t)v - 1;
        }
    }
    return -1;
}

/* The scope a function-like node opens: the body the codegen emits it with
 * as hoist_scope_body, or NULL. A trailing block is emitted inside the
 * function that calls it, so it opens none. */
static ASTNode* str_arr_scope_of(ASTNode* n) {
    switch (n->type) {
        case AST_FUNCTION_DEFINITION:
        case AST_BUILDER_FUNCTION:
        case AST_CLOSURE:
            if (n->type == AST_CLOSURE && n->value && strcmp(n->value, "trailing") == 0)
                return NULL;
            for (int i = n->child_count - 1; i >= 0; i--) {
                if (n->children[i] && n->children[i]->type == AST_BLOCK) return n->children[i];
            }
            return NULL;
        case AST_MAIN_FUNCTION:
            return n->child_count > 0 && n->children[0] &&
                   n->children[0]->type == AST_BLOCK ? n->children[0] : NULL;
        case AST_RECEIVE_ARM:
        case AST_TIMEOUT_ARM:
            return n->child_count >= 2 && n->children[1] &&
                   n->children[1]->type == AST_BLOCK ? n->children[1] : NULL;
        default:
            return NULL;
    }
}

static int str_arr_opens_scope(ASTNode* n) {
    return (n->type == AST_CLOSURE && !(n->value && strcmp(n->value, "trailing") == 0)) ||
           str_arr_scope_of(n) != NULL;
}

/* Declare the locals bound in `n` (not in a nested function or closure,
 * which declare their own) in the top frame. A binding of a name an
 * enclosing frame declares is a store into that one (a closure writing a
 * capture, a handler writing state), not a declaration. */
static void str_arr_declare_locals(StrArrs* sa, ASTNode* n) {
    if (!n || str_arr_opens_scope(n) || n->type == AST_ACTOR_DEFINITION) return;
    if (n->type == AST_VARIABLE_DECLARATION && n->value && strmap_has(&sa->names, n->value) &&
        str_arr_resolve(sa, n->value, sa->depth - 1, NULL) < 0) {
        str_arr_add(sa, sa->frames[sa->depth - 1].scope, n->value,
                    str_arr_type(n->node_type), 0, 0);
    }
    for (int i = 0; i < n->child_count; i++) str_arr_declare_locals(sa, n->children[i]);
}

/* A value a store puts in an array without making it own anything. */
static int str_arr_value_is_literal(ASTNode* v) {
    if (!v) return 1;
    if (v->type == AST_NULL_LITERAL) return 1;
    return v->type == AST_LITERAL && (!v->node_type || v->node_type->kind == TYPE_STRING);
}

static void str_arr_walk(StrArrs* sa, ASTNode* n);

static void str_arr_enter(StrArrs* sa, ASTNode* fnlike, ASTNode* scope, ASTNode* closure) {
    if (sa->depth >= STR_ARR_MAX_DEPTH) return;
    sa->frames[sa->depth].scope = scope;
    sa->frames[sa->depth].closure = closure;
    sa->depth++;
    if (scope) {
        for (int i = 0; i < fnlike->child_count; i++) {
            ASTNode* p = fnlike->children[i];
            if (!p || p == scope || !p->value) continue;
            if ((p->type == AST_VARIABLE_DECLARATION || p->type == AST_PATTERN_VARIABLE ||
                 p->type == AST_CLOSURE_PARAM) && strmap_has(&sa->names, p->value)) {
                str_arr_add(sa, scope, p->value, str_arr_type(p->node_type), 1, 0);
            }
        }
        str_arr_declare_locals(sa, scope);
    }
    for (int i = 0; i < fnlike->child_count; i++) str_arr_walk(sa, fnlike->children[i]);
    sa->depth--;
}

static void str_arr_note_use(StrArrs* sa, ASTNode* n, int a) {
    if (sa->use_count == sa->use_cap) {
        sa->use_cap = sa->use_cap ? sa->use_cap * 2 : 64;
        sa->uses = aether_xrealloc(sa->uses, sizeof(*sa->uses) * (size_t)sa->use_cap);
        sa->use_arrs = aether_xrealloc(sa->use_arrs, sizeof(*sa->use_arrs) * (size_t)sa->use_cap);
    }
    sa->uses[sa->use_count] = n;
    sa->use_arrs[sa->use_count++] = a;
}

/* The string array `name` names here, or -1. */
static int str_arr_here(StrArrs* sa, const char* name, int* frame) {
    int a = name ? str_arr_resolve(sa, name, sa->depth, frame) : -1;
    return a >= 0 && sa->arrs[a].is_array ? a : -1;
}

static void str_arr_walk(StrArrs* sa, ASTNode* n) {
    if (!n) return;
    if (str_arr_opens_scope(n)) {
        str_arr_enter(sa, n, str_arr_scope_of(n), n->type == AST_CLOSURE ? n : NULL);
        return;
    }
    if (n->type == AST_ACTOR_DEFINITION) {
        if (sa->depth >= STR_ARR_MAX_DEPTH) return;
        sa->frames[sa->depth].scope = n;
        sa->frames[sa->depth].closure = NULL;
        sa->depth++;
        for (int i = 0; i < n->child_count; i++) {
            ASTNode* d = n->children[i];
            if (d && d->type == AST_STATE_DECLARATION && d->value &&
                strmap_has(&sa->names, d->value))
                str_arr_add(sa, n, d->value, str_arr_type(d->node_type), 0, 1);
        }
        for (int i = 0; i < n->child_count; i++) str_arr_walk(sa, n->children[i]);
        sa->depth--;
        return;
    }
    int a;
    if (n->type == AST_ARRAY_ACCESS && n->child_count >= 2 && n->children[0] &&
        n->children[0]->type == AST_IDENTIFIER &&
        (a = str_arr_here(sa, n->children[0]->value, NULL)) >= 0) {
        str_arr_note_use(sa, n, a);
    }
    /* `arr[i] = v`, `arr[i] += v` */
    if ((n->type == AST_ASSIGNMENT || n->type == AST_COMPOUND_ASSIGNMENT ||
         (n->type == AST_BINARY_EXPRESSION && n->value && strcmp(n->value, "=") == 0)) &&
        n->child_count >= 2 && n->children[0] && n->children[0]->type == AST_ARRAY_ACCESS &&
        n->children[0]->child_count >= 2 && n->children[0]->children[0] &&
        n->children[0]->children[0]->type == AST_IDENTIFIER &&
        (a = str_arr_here(sa, n->children[0]->children[0]->value, NULL)) >= 0 &&
        (n->type == AST_COMPOUND_ASSIGNMENT || !str_arr_value_is_literal(n->children[1]))) {
        sa->arrs[a].owns = 1;
    }
    /* `string.free(arr[i])`: only an array that owns the element can leave
     * it empty (emit_string_element_free); a parameter freeing its caller's
     * element would have the caller free it again. */
    if (n->type == AST_FUNCTION_CALL && n->value && n->child_count == 1 &&
        (strcmp(n->value, "string.free") == 0 || strcmp(n->value, "string_free") == 0 ||
         strcmp(n->value, "string.release") == 0 || strcmp(n->value, "string_release") == 0 ||
         strcmp(n->value, "release") == 0) &&
        n->children[0] && n->children[0]->type == AST_ARRAY_ACCESS &&
        n->children[0]->child_count >= 2 && n->children[0]->children[0] &&
        n->children[0]->children[0]->type == AST_IDENTIFIER &&
        (a = str_arr_here(sa, n->children[0]->children[0]->value, NULL)) >= 0) {
        sa->arrs[a].owns = 1;
    }
    /* `arr = [...]`, `arr = other` */
    if (n->type == AST_VARIABLE_DECLARATION && n->child_count > 0 &&
        (a = str_arr_here(sa, n->value, NULL)) >= 0) {
        ASTNode* init = n->children[0];
        if (init && init->type == AST_SLICE_FROM_ARRAY && init->child_count > 0)
            init = init->children[0];
        if (init && init->type == AST_ARRAY_LITERAL) {
            for (int i = 0; i < init->child_count; i++) {
                if (!str_arr_value_is_literal(init->children[i])) sa->arrs[a].owns = 1;
            }
        } else if (init) {
            sa->arrs[a].owns = 1;
        }
    }
    for (int i = 0; i < n->child_count; i++) str_arr_walk(sa, n->children[i]);
}

static StrArrs* str_arrs(CodeGenerator* gen) {
    if (gen->str_arrays) return (StrArrs*)gen->str_arrays;
    StrArrs* sa = aether_xrealloc(NULL, sizeof(StrArrs));
    memset(sa, 0, sizeof(*sa));
    strmap_init(&sa->at);
    strmap_init(&sa->names);
    strmap_init(&sa->views);
    gen->str_arrays = sa;
    if (gen->program) str_arr_collect_names(sa, gen->program);
    if (gen->program && strmap_count(&sa->names) > 0) str_arr_walk(sa, gen->program);
    for (int i = 0; i < sa->use_count; i++) {
        StrArr* s = &sa->arrs[sa->use_arrs[i]];
        if (!s->param && !s->owns && !s->state) continue;
        char key[32];
        snprintf(key, sizeof(key), "%p", (void*)sa->uses[i]);
        strmap_put(&sa->views, key, (void*)(intptr_t)(s->owns && !s->state ? 2 : 1));
    }
    free(sa->uses);
    free(sa->use_arrs);
    sa->uses = NULL;
    sa->use_arrs = NULL;
    return sa;
}

void str_arrays_free(CodeGenerator* gen) {
    StrArrs* sa = (StrArrs*)gen->str_arrays;
    if (!sa) return;
    free(sa->arrs);
    strmap_free(&sa->at);
    strmap_free(&sa->names);
    strmap_free(&sa->views);
    free(sa);
    gen->str_arrays = NULL;
}

/* Does the local or parameter `name` of the body `scope` (hoist_scope_body)
 * own its elements? */
int string_array_owns(CodeGenerator* gen, ASTNode* scope, const char* name) {
    if (!gen || !scope || !name) return 0;
    StrArrs* sa = str_arrs(gen);
    char* key = heap_strf("%p|%s", (void*)scope, name);
    void* v = strmap_get(&sa->at, key);
    free(key);
    if (!v) return 0;
    StrArr* s = &sa->arrs[(int)(intptr_t)v - 1];
    return s->is_array && s->owns && !s->state;
}

static void emit_unwind_track_place(CodeGenerator* gen, const char* addr, const char* fn);
static void emit_unwind_track_call(CodeGenerator* gen, const char* call);

/* The scope exit of an owning `string[N]` array `name`: each element it
 * holds is given back, and a panic does through the journal. */
static void push_str_array_release_defer(CodeGenerator* gen, const char* name, int n,
                                         int line, int column) {
    Type* t = create_type(TYPE_ARRAY);
    t->array_size = n;
    t->element_type = create_type(TYPE_STRING);
    push_own_drop(gen, name, t, line, column);
    free_type(t);
}

/* `o.names` where `names` is a `string[N]` field of an Aether struct, which
 * owns its elements (#2528). */
static int is_owned_string_array_field(CodeGenerator* gen, ASTNode* base) {
    if (!base || base->type != AST_MEMBER_ACCESS || !base->value || base->child_count < 1 ||
        !base->children[0] || !gen->program) return 0;
    Type* ht = base->children[0]->node_type;
    const char* sname = NULL;
    if (ht && ht->kind == TYPE_STRUCT) sname = ht->struct_name;
    else if (ht && ht->kind == TYPE_PTR && ht->element_type &&
             ht->element_type->kind == TYPE_STRUCT) sname = ht->element_type->struct_name;
    if (!sname || aether_is_c_import_struct(sname)) return 0;
    ASTNode* sdef = find_struct_definition_by_name(gen->program, sname);
    for (int i = 0; sdef && i < sdef->child_count; i++) {
        ASTNode* f = sdef->children[i];
        if (f && f->type == AST_STRUCT_FIELD && f->value && strcmp(f->value, base->value) == 0)
            return struct_field_owned_array(f, NULL) == 1;
    }
    return 0;
}

/* #2689: an element of a `string[]` slice. A slice owns none of its
 * elements, and what does is not known where it is read: an array that
 * owns them, a struct's field, a `make` buffer, C memory. So its element
 * is a view that a slot it escapes to copies: a return, a struct field, a
 * container, a call handing it back (is_owned_string_element). Through a
 * slice of an owning array, a returned element was handed over borrowed,
 * and dangled once the array let go of it. A local bound to one takes a
 * reference of its own, as every place does (R3): a store through the slice
 * releases the element it replaces, so code that moves elements through a
 * local (`t = s[0]; s[0] = s[1]; s[1] = t`) keeps each alive while it moves
 * (#2921). */
static int is_slice_string_element(ASTNode* e) {
    if (!e || e->type != AST_ARRAY_ACCESS || e->child_count < 2 || !e->children[0]) return 0;
    Type* t = e->children[0]->node_type;
    return type_is_slice(t) && t->element_type && t->element_type->kind == TYPE_STRING;
}

/* Is `e` an element read a slot takes as a view (above)? */
int is_owned_string_element(CodeGenerator* gen, ASTNode* e) {
    if (!gen || !e || e->type != AST_ARRAY_ACCESS || e->child_count < 2 || !e->children[0])
        return 0;
    ASTNode* base = e->children[0];
    if (is_slice_string_element(e)) return 1;   /* #2689 */
    if (!str_arr_type(base->node_type)) return 0;
    if (base->type == AST_MEMBER_ACCESS) return is_owned_string_array_field(gen, base);
    if (base->type != AST_IDENTIFIER) return 0;
    char key[32];
    snprintf(key, sizeof(key), "%p", (void*)e);
    return strmap_get(&str_arrs(gen)->views, key) != NULL;
}

/* Is `e` an element of a local or parameter array that owns its elements,
 * so a store into it takes the value and frees the one it replaces? */
int is_owning_array_string_element(CodeGenerator* gen, ASTNode* e) {
    if (!gen || !e || e->type != AST_ARRAY_ACCESS || e->child_count < 2 || !e->children[0] ||
        e->children[0]->type != AST_IDENTIFIER) return 0;
    char key[32];
    snprintf(key, sizeof(key), "%p", (void*)e);
    return strmap_get(&str_arrs(gen)->views, key) == (void*)2;
}

/* The type a match expression's arms yield: the first value an arm yields
 * (match_arm_value, so a block arm's final expression counts, #2496). */
static Type* match_value_type(ASTNode* m) {
    for (int i = 1; m && i < m->child_count; i++) {
        ASTNode* arm = m->children[i];
        if (!arm || arm->type != AST_MATCH_ARM || arm->child_count < 2) continue;
        ASTNode* v = match_arm_value(arm->children[1]);
        if (v && v->node_type && v->node_type->kind != TYPE_UNKNOWN) return v->node_type;
    }
    return NULL;
}

static int string_take_join(int a, int b) {
    return a == b ? a : STR_TAKE_RUNTIME;
}

static int is_cell_string_element(CodeGenerator* gen, ASTNode* e);
static int tuple_call_pos_copied(CodeGenerator* gen, ASTNode* call, int j);   /* #2619 */

static int is_opt_string_type(Type* t);
static int is_env_capture_name(CodeGenerator* gen, const char* name);

/* The string a `string?` holder that gives it back when it is re-bound
 * yields: `o ?? d` and `o!`, where `o` is a closure cell (#2795), which
 * owns its string and frees it at the next store. A local, a field or any
 * owning slot takes a copy of it, as it takes a copy of a `string` cell's
 * string (is_captured_string); taken as it was, a view outlived the
 * re-bind and read freed memory. A `??` whose default is a fresh string is
 * left alone: the copy would leak the default when it is the one taken.
 * A `string?` module var is no such holder: like a `string` one it keeps
 * every string it was given (#2800), so a view of it stays valid however
 * the var is re-bound, a binding or an evaluation-order temporary alike. */
/* Is `o` such a holder: a `string?` closure cell? */
static int is_owning_opt_string_holder(CodeGenerator* gen, ASTNode* o) {
    if (!o || o->type != AST_IDENTIFIER || !o->value || !is_opt_string_type(o->node_type))
        return 0;
    return is_promoted_capture(gen, o->value) || is_env_capture_name(gen, o->value);
}

static int is_owning_opt_string_view(CodeGenerator* gen, ASTNode* e) {
    if (!e || (e->type != AST_NULL_COALESCE && e->type != AST_TUPLE_UNWRAP) ||
        e->child_count < 1) return 0;
    if (!is_owning_opt_string_holder(gen, e->children[0])) return 0;
    if (e->type == AST_NULL_COALESCE && e->child_count > 1 &&
        is_heap_string_expr(gen, e->children[1])) return 0;
    return 1;
}

/* A read an owning slot takes as a copy: a struct's string field, an
 * element of a string array whose elements are owned (#2474, #2618), or a
 * call that may hand one of those back. Context-free without `cells`; with
 * it, an element of an array a closure writes and the string of a `string?`
 * cell (is_owning_opt_string_view) count too. */
static int is_string_view_read(CodeGenerator* gen, ASTNode* e, int cells) {
    return is_owned_string_field_read(e) || is_owned_string_element(gen, e) ||
           (cells && is_cell_string_element(gen, e)) ||
           (cells && is_owning_opt_string_view(gen, e));
}

static int is_env_capture_name(CodeGenerator* gen, const char* name);

/* Is `name` a capture the closure being emitted only reads, bound at its
 * entry from the env (`const char* s = _env->s;`)? */
static int is_alias_capture_name(CodeGenerator* gen, const char* name) {
    for (int i = 0; name && i < gen->current_alias_capture_count; i++) {
        if (gen->current_alias_captures[i] &&
            strcmp(gen->current_alias_captures[i], name) == 0) return 1;
    }
    return 0;
}

/* A string a closure reaches through its environment or a shared cell: the
 * env or the cell holds it and frees it with itself, so an owning slot
 * (a struct field, above all) takes a copy (emit_string_take), not the
 * pointer (#2574). */
static int is_captured_string(CodeGenerator* gen, ASTNode* e) {
    return e && e->type == AST_IDENTIFIER && e->value && e->node_type &&
           e->node_type->kind == TYPE_STRING &&
           (is_promoted_capture(gen, e->value) || is_env_capture_name(gen, e->value) ||
            is_alias_capture_name(gen, e->value));
}

int string_take_kind(CodeGenerator* gen, ASTNode* e) {
    if (!e) return STR_TAKE_BORROW;
    if (e->type == AST_IF_EXPRESSION && e->child_count >= 3) {
        return string_take_join(string_take_kind(gen, e->children[1]),
                                string_take_kind(gen, e->children[2]));
    }
    if (e->type == AST_MATCH_STATEMENT) {
        int k = -1;
        for (int i = 1; i < e->child_count; i++) {
            ASTNode* arm = e->children[i];
            if (!arm || arm->type != AST_MATCH_ARM || arm->child_count < 2) continue;
            ASTNode* value = match_arm_value(arm->children[1]);
            int ak = value ? string_take_kind(gen, value) : STR_TAKE_BORROW;
            k = k < 0 ? ak : string_take_join(k, ak);
        }
        return k < 0 ? STR_TAKE_BORROW : k;
    }
    if (is_string_view_read(gen, e, 1)) return STR_TAKE_OWNED;
    if (is_captured_string(gen, e)) return STR_TAKE_OWNED;
    if (e->type == AST_IDENTIFIER) {
        return (e->value && is_heap_string_var(gen, e->value))
               ? STR_TAKE_RUNTIME : STR_TAKE_BORROW;
    }
    return is_heap_string_expr(gen, e) ? STR_TAKE_OWNED : STR_TAKE_BORROW;
}

/* Is `e` a view an owning slot must take with emit_string_take rather than
 * by its classic paths (fresh heap value adopted, bare alias moved/copied,
 * anything else borrowed)? A field read or an element of a string array
 * whose elements are owned (#2474, #2618), or a call that may hand one back,
 * a call that only hands a heap-tracked local back (#2548), or an `if` with
 * an arm that is not a plain borrow. A `match` binds through its own result
 * variable, which the match binding routes separately. */
int string_take_is_view(CodeGenerator* gen, ASTNode* e) {
    if (!e) return 0;
    if (is_string_view_read(gen, e, 1)) return 1;
    if (is_captured_string(gen, e)) return 1;
    return e->type == AST_IF_EXPRESSION &&
           string_take_kind(gen, e) != STR_TAKE_BORROW;
}

/* Is `name` a capture the closure being emitted reaches through `_env->`? */
static int is_env_capture_name(CodeGenerator* gen, const char* name) {
    for (int e = 0; name && e < gen->current_env_capture_count; e++) {
        if (gen->current_env_captures[e] &&
            strcmp(gen->current_env_captures[e], name) == 0) return 1;
    }
    return 0;
}

/* Emit `e` as the value an owning slot takes (R2): a fresh string moved in,
 * a string local at its last use moved out (left empty), anything else
 * taken (a counted string retained, a pinned literal as it is, a C string
 * copied). `target` is the slot's own name when it is a local: its own value
 * is taken, never moved, so `w = if c { w } else { ... }` keeps it. */
void emit_string_take(CodeGenerator* gen, ASTNode* e, const char* target) {
    /* `o ?? d` over a `string?`: a place's string is retained, a call's
     * handed over; the default is taken as any value is (a fresh one moved
     * in), so neither leaks nor is taken twice. */
    if (e && e->type == AST_NULL_COALESCE && e->child_count >= 2 && e->children[0] &&
        is_opt_string_type(e->children[0]->node_type)) {
        TempSeq s;
        seq_open(gen, &s);
        const char* ov = seq_let(gen, &s, "ae_opt_string", "_ae_oc");
        generate_expression(gen, e->children[0]);
        seq_next(gen, &s);
        if (e->children[0]->type == AST_FUNCTION_CALL)
            fprintf(gen->output, "%s.has ? %s.val : ", ov, ov);
        else
            fprintf(gen->output, "%s.has ? aether_uniform_heap_str(%s.val, 0) : ", ov, ov);
        emit_string_take(gen, e->children[1], target);
        seq_close(gen, &s);
        return;
    }
    if (e && e->type == AST_IF_EXPRESSION && e->child_count >= 3) {
        fprintf(gen->output, "((");
        generate_expression(gen, e->children[0]);
        fprintf(gen->output, ") ? ");
        emit_string_take(gen, e->children[1], target);
        fprintf(gen->output, " : ");
        emit_string_take(gen, e->children[2], target);
        fprintf(gen->output, ")");
        return;
    }
    /* A closure's shared cell or env keeps the string it holds: taken. */
    int captured = e && e->type == AST_IDENTIFIER && e->value &&
                   (is_promoted_capture(gen, e->value) || is_env_capture_name(gen, e->value) ||
                    is_alias_capture_name(gen, e->value));
    if (!is_string_view_read(gen, e, 1) && !captured && e && e->type == AST_IDENTIFIER &&
        e->value && is_heap_string_var(gen, e->value) &&
        !(target && strcmp(e->value, target) == 0) && !alias_source_must_copy(gen, e->value)) {
        /* Its last use: the reference moves out and the local is left
         * empty, so its scope exit releases nothing. */
        TempSeq s;
        seq_open(gen, &s);
        const char* mv = seq_let(gen, &s, "const char*", "_ae_mv");
        generate_expression(gen, e);
        seq_next(gen, &s);
        generate_expression(gen, e);
        fprintf(gen->output, " = NULL");
        seq_next(gen, &s);
        fprintf(gen->output, "%s", mv);
        seq_close(gen, &s);
        return;
    }
    if (!is_string_view_read(gen, e, 1) && !captured && is_heap_string_expr(gen, e) &&
        !(e->type == AST_IDENTIFIER)) {
        fprintf(gen->output, "(const char*)(");
        generate_expression(gen, e);
        fprintf(gen->output, ")");
        return;
    }
    /* R5: a literal is pinned (R1); taking it is the literal itself. */
    if (e && e->type == AST_LITERAL && e->value && e->node_type &&
        e->node_type->kind == TYPE_STRING) {
        emit_string_literal_node(gen, e);
        return;
    }
    fprintf(gen->output, "aether_uniform_heap_str((const char*)(");
    generate_expression(gen, e);
    fprintf(gen->output, "), 0)");
}

/* Emit `e` taken as a value the receiver owns (emit_string_take). With
 * `cell`, for a closure's string cell, whose release frees only counted
 * strings, a plain buffer is made one on the way in. */
void emit_string_take_owned(CodeGenerator* gen, ASTNode* e, int cell) {
    if (cell) fprintf(gen->output, "_aether_str_cell_own(");
    emit_string_take(gen, e, NULL);
    if (cell) fprintf(gen->output, ", 1)");
}

/* `name = v` for a string local: the new value taken, then the one the
 * local held released. */
static void emit_string_take_rebind(CodeGenerator* gen, const char* name, ASTNode* rhs) {
    fprintf(gen->output, "{ const char* _ae_new = ");
    emit_string_take(gen, rhs, name);
    fprintf(gen->output, "; const char* _tmp_old = %s; %s = _ae_new; aether_heap_str_free(_tmp_old); }\n",
            name, name);
}


/* Bit width of an integer the series collapse is exact for: Aether's
 * wrapping `int` (C int) and `long` (int64_t); 0 for anything else. */
static int series_int_width(const ASTNode* n) {
    if (!n || !n->node_type) return 0;
    if (n->node_type->kind == TYPE_INT) return 32;
    if (n->node_type->kind == TYPE_INT64) return 64;
    return 0;
}

/* The value of a literal written in plain decimal digits; 0 when the literal
 * is anything else (a float, another base, a separator) or does not fit. */
static int series_decimal_literal(const ASTNode* n, unsigned long long* out) {
    if (!n || n->type != AST_LITERAL || !n->value || !n->value[0]) return 0;
    unsigned long long v = 0;
    for (const char* c = n->value; *c; c++) {
        if (*c < '0' || *c > '9') return 0;
        unsigned d = (unsigned)(*c - '0');
        if (v > (ULLONG_MAX - d) / 10) return 0;
        v = v * 10 + d;
    }
    *out = v;
    return 1;
}

/* A counter or accumulator of the series, read or written: the identifier as
 * the rest of codegen spells it, so an actor's state field is `self->name`, a
 * promoted capture its cell and a closure's capture its env slot. Printing
 * the bare name wrote a local that did not exist (#2505). */
static void series_var(CodeGenerator* gen, ASTNode* id) {
    generate_expression(gen, id);
}

// Try to detect and emit a collapsed arithmetic series loop.
// Returns 1 if the loop was collapsed and emitted; 0 otherwise (caller emits normally).
static int try_emit_series_collapse(CodeGenerator* gen, ASTNode* while_node) {
    if (!while_node || while_node->child_count < 2) return 0;
    if (gen->series_collapse_off) return 0;

    ASTNode* condition = while_node->children[0];
    ASTNode* body      = while_node->children[1];

    // 1. Condition must be "counter < bound" or "counter <= bound"
    if (!condition || condition->type != AST_BINARY_EXPRESSION || !condition->value) return 0;
    int is_lt  = strcmp(condition->value, "<")  == 0;
    int is_lte = strcmp(condition->value, "<=") == 0;
    if (!is_lt && !is_lte) return 0;
    if (condition->child_count < 2) return 0;

    ASTNode* cond_left  = condition->children[0];   // the counter
    ASTNode* cond_right = condition->children[1];   // the bound

    if (!cond_left || cond_left->type != AST_IDENTIFIER || !cond_left->value) return 0;
    const char* counter_var = cond_left->value;

    // Integer counter and bound only: the closed form is exact for nothing else.
    int counter_width = series_int_width(cond_left);
    if (!counter_width || !series_int_width(cond_right)) return 0;

    // Bound must not have side effects
    if (codegen_expr_has_side_effects(cond_right)) return 0;

    // 2. Body: get statement list
    ASTNode** stmts;
    int stmt_count;
    if (!body) return 0;
    if (body->type == AST_BLOCK && body->child_count == 1 &&
        body->children[0] && body->children[0]->type == AST_BLOCK) {
        body = body->children[0];
    }
    if (body->type == AST_BLOCK) {
        stmts      = body->children;
        stmt_count = body->child_count;
    } else {
        stmts      = &body;
        stmt_count = 1;
    }
    if (stmt_count == 0) return 0;

    // 3. Parse each statement
    ASTNode*           acc_ids[MAX_SERIES_ACCUMULATORS];        // its identifier node
    ASTNode*           acc_addends[MAX_SERIES_ACCUMULATORS];
    int                acc_width[MAX_SERIES_ACCUMULATORS];
    int                acc_is_linear[MAX_SERIES_ACCUMULATORS];  // addend is counter or counter*C
    unsigned long long acc_scale[MAX_SERIES_ACCUMULATORS];      // C for a linear addend
    int                acc_stmt[MAX_SERIES_ACCUMULATORS];       // position in the body
    int                acc_count   = 0;
    int                counter_idx = -1;
    unsigned long long counter_step = 0;

    // Also collect the set of target variable names for later checks.
    const char* stmt_targets[MAX_SERIES_ACCUMULATORS + 1];  // +1 for counter
    int stmt_target_count = 0;

    for (int i = 0; i < stmt_count; i++) {
        ASTNode* s = stmts[i];
        if (!s) return 0;

        // Every statement must be an assignment of the form: target = target + expr
        // The parser emits AST_VARIABLE_DECLARATION for all "x = expr" statements:
        //   s->value      = target variable name
        //   s->children[0] = RHS expression
        if (s->type != AST_VARIABLE_DECLARATION) return 0;
        if (!s->value || s->child_count < 1) return 0;

        const char* target = s->value;
        ASTNode*    rhs    = s->children[0];

        if (!rhs || rhs->type != AST_BINARY_EXPRESSION) return 0;
        if (!rhs->value || strcmp(rhs->value, "+") != 0) return 0;
        if (rhs->child_count < 2) return 0;

        ASTNode* rhs_left  = rhs->children[0];
        ASTNode* rhs_right = rhs->children[1];

        // Identify the "self" side and the "addend" side
        int left_is_self  = rhs_left  && rhs_left->type  == AST_IDENTIFIER &&
                            rhs_left->value  && strcmp(rhs_left->value,  target) == 0;
        int right_is_self = rhs_right && rhs_right->type == AST_IDENTIFIER &&
                            rhs_right->value && strcmp(rhs_right->value, target) == 0;
        if (!left_is_self && !right_is_self) return 0;

        ASTNode* self   = left_is_self ? rhs_left : rhs_right;
        ASTNode* addend = left_is_self ? rhs_right : rhs_left;
        if (!addend) return 0;

        // Track this target for bound-mutation check later
        if (stmt_target_count >= MAX_SERIES_ACCUMULATORS + 1) return 0;
        stmt_targets[stmt_target_count++] = target;

        if (strcmp(target, counter_var) == 0) {
            // Counter increment: one of them, by a positive integer literal
            // that fits the counter's type.
            if (counter_idx >= 0) return 0;
            unsigned long long step;
            if (!series_decimal_literal(addend, &step) || step == 0) return 0;
            if (step > (counter_width == 32 ? (unsigned long long)INT32_MAX
                                             : (unsigned long long)INT64_MAX)) return 0;
            counter_step = step;
            counter_idx = i;
        } else {
            // Accumulator: an integer variable, adding either a loop-invariant
            // integer (constant series) or the counter itself / counter*C
            // (linear sum).
            if (acc_count >= MAX_SERIES_ACCUMULATORS) return 0;
            int width = series_int_width(self);
            if (!width) return 0;

            int addend_is_counter = 0;
            unsigned long long scale = 1;

            if (addend->type == AST_IDENTIFIER && addend->value &&
                strcmp(addend->value, counter_var) == 0) {
                // Plain counter addend: acc = acc + i
                addend_is_counter = 1;
            } else if (addend->type == AST_BINARY_EXPRESSION && addend->value &&
                       strcmp(addend->value, "*") == 0 && addend->child_count >= 2) {
                // Possibly scaled counter: acc = acc + i * C  or  acc = acc + C * i
                ASTNode* ml = addend->children[0];
                ASTNode* mr = addend->children[1];
                if (ml && ml->type == AST_IDENTIFIER && ml->value &&
                    strcmp(ml->value, counter_var) == 0 &&
                    series_decimal_literal(mr, &scale)) {
                    addend_is_counter = 1;
                } else if (mr && mr->type == AST_IDENTIFIER && mr->value &&
                           strcmp(mr->value, counter_var) == 0 &&
                           series_decimal_literal(ml, &scale)) {
                    addend_is_counter = 1;
                }
                // The loop computes i * C in the product's own type, which
                // wraps there. That matches the closed form modulo 2^width
                // only when the product is at least as wide as the
                // accumulator.
                if (addend_is_counter && series_int_width(addend) < width) return 0;
            }

            if (!addend_is_counter) {
                // Invariant addend: an integer that does not depend on the counter
                if (!series_int_width(addend)) return 0;
                if (expr_references_var(addend, counter_var)) return 0;
                if (codegen_expr_has_side_effects(addend)) return 0;
            }
            acc_ids[acc_count]       = self;
            acc_addends[acc_count]   = addend;
            acc_width[acc_count]     = width;
            acc_is_linear[acc_count] = addend_is_counter;
            acc_scale[acc_count]     = scale;
            acc_stmt[acc_count]      = i;
            acc_count++;
        }
    }

    if (counter_idx < 0) return 0;

    // 3b. Bound-mutation check: if any loop body statement assigns to a variable
    // referenced in the bound expression, the bound changes per-iteration.
    for (int i = 0; i < stmt_target_count; i++) {
        if (expr_references_var(cond_right, stmt_targets[i])) return 0;
    }

    // 3c. Addend invariance check: verify no addend references a variable modified
    // by any other statement in the loop body.
    // Skip for linear accumulators — their "addend" is the counter itself, which is
    // expected to be in the write-set; the formula accounts for that by design.
    for (int i = 0; i < acc_count; i++) {
        if (acc_is_linear[i]) continue;
        for (int j = 0; j < stmt_target_count; j++) {
            if (expr_references_var(acc_addends[i], stmt_targets[j])) return 0;
        }
    }

    // 4. Emit the closed form, guarded by the loop's own condition so a loop
    // that would not run leaves everything unchanged. Inside, the trip count
    // is at least 1. All arithmetic is in uint64_t, where it is exact modulo
    // 2^64; each result is then narrowed to its variable's type, which is
    // what the loop's wrapping additions produce.
    const char* counter_ctype = counter_width == 32 ? "int" : "int64_t";
    const char* counter_max   = counter_width == 32 ? "INT32_MAX" : "INT64_MAX";

    /* The closed form stands for the loop, so a C error in it is the loop's. */
    codegen_maybe_emit_line(gen, condition);
    print_indent(gen);
    fprintf(gen->output, "if ((");
    series_var(gen, cond_left);
    fprintf(gen->output, ") %s (", is_lte ? "<=" : "<");
    generate_expression(gen, cond_right);
    fprintf(gen->output, ")) {\n");
    indent(gen);

    // Distance to the bound, and how many steps the counter has before it
    // passes its type's maximum.
    print_indent(gen);
    fprintf(gen->output, "uint64_t _ae_sd = (uint64_t)(int64_t)(");
    generate_expression(gen, cond_right);
    fprintf(gen->output, ") - (uint64_t)(int64_t)");
    series_var(gen, cond_left);
    fprintf(gen->output, ";\n");
    print_indent(gen);
    fprintf(gen->output, "uint64_t _ae_sh = ((uint64_t)%s - (uint64_t)(int64_t)", counter_max);
    series_var(gen, cond_left);
    fprintf(gen->output, ") / %lluULL;\n", counter_step);

    // The trip count, when the counter's last step does not wrap.
    print_indent(gen);
    if (is_lte) {
        fprintf(gen->output, "if (_ae_sd / %lluULL < _ae_sh) {\n", counter_step);
    } else {
        fprintf(gen->output, "if (_ae_sd / %lluULL + (_ae_sd %% %lluULL != 0) <= _ae_sh) {\n",
                counter_step, counter_step);
    }
    indent(gen);
    print_indent(gen);
    if (is_lte) {
        fprintf(gen->output, "uint64_t _ae_sn = _ae_sd / %lluULL + 1;\n", counter_step);
    } else {
        fprintf(gen->output, "uint64_t _ae_sn = _ae_sd / %lluULL + (_ae_sd %% %lluULL != 0);\n",
                counter_step, counter_step);
    }

    int emitted_linear = 0;
    for (int i = 0; i < acc_count; i++) {
        const char* acc_ctype = acc_width[i] == 32 ? "int" : "int64_t";
        codegen_maybe_emit_line(gen, stmts[acc_stmt[i]]);
        print_indent(gen);
        if (acc_is_linear[i]) {
            // Sum of the counter values the statement sees: c0 + k*step for
            // k = 0..T-1, or k = 1..T when it follows the increment.
            //   T*c0 + step * T*(T-1)/2     or     T*c0 + step * T*(T+1)/2
            // T*(T±1)/2 halves whichever factor is even, so it is exact
            // modulo 2^64 without a 128-bit product.
            int after = acc_stmt[i] > counter_idx;
            const char* tri = after
                ? "(_ae_sn % 2 == 0 ? (_ae_sn / 2) * (_ae_sn + 1) : _ae_sn * (_ae_sn / 2 + 1))"
                : "(_ae_sn % 2 == 0 ? (_ae_sn / 2) * (_ae_sn - 1) : _ae_sn * ((_ae_sn - 1) / 2))";
            series_var(gen, acc_ids[i]);
            fprintf(gen->output, " = (%s)((uint64_t)(int64_t)", acc_ctype);
            series_var(gen, acc_ids[i]);
            fprintf(gen->output, " + %lluULL * ((uint64_t)(int64_t)", acc_scale[i]);
            series_var(gen, cond_left);
            fprintf(gen->output, " * _ae_sn + %lluULL * ", counter_step);
            fputs(tri, gen->output);
            fprintf(gen->output, "));\n");
            emitted_linear = 1;
        } else {
            // Invariant addend: added once per trip.
            series_var(gen, acc_ids[i]);
            fprintf(gen->output, " = (%s)((uint64_t)(int64_t)", acc_ctype);
            series_var(gen, acc_ids[i]);
            fprintf(gen->output, " + (uint64_t)(int64_t)(");
            generate_expression(gen, acc_addends[i]);
            fprintf(gen->output, ") * _ae_sn);\n");
        }
    }

    // counter = c0 + step * T
    codegen_maybe_emit_line(gen, stmts[counter_idx]);
    print_indent(gen);
    series_var(gen, cond_left);
    fprintf(gen->output, " = (%s)((uint64_t)(int64_t)", counter_ctype);
    series_var(gen, cond_left);
    fprintf(gen->output, " + _ae_sn * %lluULL);\n", counter_step);

    unindent(gen);
    print_indent(gen);
    fprintf(gen->output, "} else {\n");
    indent(gen);
    // The counter would wrap on its last step: run the loop as written.
    gen->series_collapse_off++;
    print_indent(gen);
    generate_statement(gen, while_node);
    gen->series_collapse_off--;
    unindent(gen);
    print_indent(gen);
    fprintf(gen->output, "}\n");

    unindent(gen);
    print_indent(gen);
    fprintf(gen->output, "}\n");

    if (emitted_linear) {
        global_opt_stats.linear_loops_collapsed++;
    } else {
        global_opt_stats.series_loops_collapsed++;
    }
    return 1;
}

/* #1047: emit the boolean condition that a case selector matches `val_var`.
 * Handles a single value (== , or string_equals for strings), an inclusive
 * (`lo..=hi`) / half-open (`lo..<hi`) range, and a comma-list of these (OR).
 * Shared by match arms and the ranged-switch if-chain lowering. */
static void emit_selector_condition(CodeGenerator* gen, ASTNode* sel,
                                    const char* val_var, int is_string) {
    if (!sel) { fprintf(gen->output, "0"); return; }
    if (sel->type == AST_MATCH_ALT) {
        fprintf(gen->output, "(");
        for (int i = 0; i < sel->child_count; i++) {
            if (i > 0) fprintf(gen->output, " || ");
            emit_selector_condition(gen, sel->children[i], val_var, is_string);
        }
        fprintf(gen->output, ")");
        return;
    }
    if (sel->type == AST_MATCH_RANGE && sel->child_count >= 2) {
        int inclusive = sel->annotation && strcmp(sel->annotation, "inclusive") == 0;
        fprintf(gen->output, "(%s >= ", val_var);
        generate_expression(gen, sel->children[0]);
        fprintf(gen->output, " && %s %s ", val_var, inclusive ? "<=" : "<");
        generate_expression(gen, sel->children[1]);
        fprintf(gen->output, ")");
        return;
    }
    if (is_string) {
        fprintf(gen->output, "(%s && string_equals(%s, ", val_var, val_var);
        generate_expression(gen, sel);
        fprintf(gen->output, "))");
    } else {
        /* No wrapping parens: every caller already parenthesises (the
         * if-opener or the ALT joiner), and `if ((x == A))` trips clang's
         * default -Wparentheses-equality on every match a user compiles. */
        fprintf(gen->output, "%s == ", val_var);
        generate_expression(gen, sel);
    }
}

/* #1047: does this case selector contain a range (directly, or as an element
 * of a comma-list)? A C `switch` can't express a range, so a switch with any
 * ranged case is lowered to an if-else chain instead. */
static int selector_has_range(ASTNode* sel) {
    if (!sel) return 0;
    if (sel->type == AST_MATCH_RANGE) return 1;
    if (sel->type == AST_MATCH_ALT) {
        for (int i = 0; i < sel->child_count; i++)
            if (sel->children[i] && sel->children[i]->type == AST_MATCH_RANGE) return 1;
    }
    return 0;
}

/* #1047: the C type of a switch/match scrutinee, for the temp that a lowered
 * if-chain compares against. Mirrors the match `_match_val` typing. */
static const char* scrutinee_c_type(ASTNode* expr) {
    Type* t = expr ? expr->node_type : NULL;
    if (!t) return "int";
    if (t->kind == TYPE_STRING || t->kind == TYPE_PTR) return "const char*";
    if (t->kind == TYPE_FLOAT)      return "double";
    if (t->kind == TYPE_FLOAT32)    return "float";
    if (t->kind == TYPE_LONGDOUBLE) return "long double";
    if (t->kind == TYPE_INT64 || t->kind == TYPE_DURATION) return "int64_t";
    /* An unsigned value is compared as itself: as an int, 5000000000 was
     * 705032704 and matched that arm instead of its own. */
    if (t->kind == TYPE_UINT64)     return "uint64_t";
    if (t->kind == TYPE_UINT32)     return "uint32_t";
    if (t->kind == TYPE_UINT16)     return "uint16_t";
    if (t->kind == TYPE_UINT8)      return "uint8_t";
    if (t->kind == TYPE_BYTE)       return "unsigned char";
    if (t->kind == TYPE_BOOL)       return "bool";
    return "int";
}

/* #2727: is the value a switch compares something a C `switch` takes? A
 * string, a pointer or a float is compared by an if-chain instead. */
static int switch_subject_is_integer(ASTNode* expr) {
    Type* t = expr ? expr->node_type : NULL;
    if (!t) return 1;
    return !(t->kind == TYPE_STRING || t->kind == TYPE_PTR || t->kind == TYPE_FLOAT ||
             t->kind == TYPE_FLOAT32 || t->kind == TYPE_LONGDOUBLE);
}

static void generate_list_pattern_condition(CodeGenerator* gen, ASTNode* pattern,
                                            const char* len_name,
                                            int is_seq_match) {
    if (!pattern) return;

    if (is_seq_match) {
        /* StringSeq cons cell. Empty list = NULL pointer; non-empty = any
         * non-NULL cell (which by construction has at least one head and
         * a tail — never partially-initialised, see string_seq_cons). */
        if (pattern->type == AST_PATTERN_LIST && pattern->child_count == 0) {
            fprintf(gen->output, "%s == NULL", len_name);
        } else if (pattern->type == AST_PATTERN_LIST) {
            /* Fixed-arity list pattern `[a, b, c]` against a StringSeq —
             * compare cached length so an O(1) test is enough. */
            fprintf(gen->output, "%s != NULL && %s->length == %d",
                    len_name, len_name, pattern->child_count);
        } else if (pattern->type == AST_PATTERN_CONS) {
            fprintf(gen->output, "%s != NULL", len_name);
        }
        return;
    }

    if (pattern->type == AST_PATTERN_LIST) {
        if (pattern->child_count == 0) {
            fprintf(gen->output, "%s == 0", len_name);
        } else {
            fprintf(gen->output, "%s == %d", len_name, pattern->child_count);
        }
    } else if (pattern->type == AST_PATTERN_CONS) {
        fprintf(gen->output, "%s >= 1", len_name);
    }
}

// Check if any binding in the pattern is actually used by the arm body
static int pattern_needs_array(ASTNode* pattern, ASTNode* body) {
    if (!pattern || !body) return 0;
    if (pattern->type == AST_PATTERN_LIST) {
        for (int i = 0; i < pattern->child_count; i++) {
            ASTNode* elem = pattern->children[i];
            if (elem && elem->type == AST_PATTERN_VARIABLE && elem->value &&
                expr_references_var(body, elem->value)) return 1;
        }
    } else if (pattern->type == AST_PATTERN_CONS && pattern->child_count >= 2) {
        ASTNode* head = pattern->children[0];
        ASTNode* tail = pattern->children[1];
        if (head && head->type == AST_PATTERN_VARIABLE && head->value &&
            expr_references_var(body, head->value)) return 1;
        if (tail && tail->type == AST_PATTERN_VARIABLE && tail->value &&
            expr_references_var(body, tail->value)) return 1;
    }
    return 0;
}

/* A match arm's pattern binding, just declared in the arm's C block, is a
 * local of the arm: a `h = ...` in the arm rebinds it, and no hoist in the
 * arm declares it again there ("redefinition of 'h'"). The caller drops it
 * when the arm ends. `kind` is the C declaration's: an element (`int`), a
 * `T[]` tail, a StringSeq's head (string) or tail (pointer). */
static void mark_pattern_binding(CodeGenerator* gen, const char* name, TypeKind kind) {
    Type* t = kind == TYPE_ARRAY ? create_array_type(create_type(TYPE_INT), -1)
                                 : create_type(kind);
    mark_var_declared_typed(gen, name, t);
    free_type(t);
}

static void generate_list_pattern_bindings(CodeGenerator* gen, ASTNode* pattern,
                                           ASTNode* match_expr, const char* len_name,
                                           ASTNode* body, int is_seq_match) {
    if (!pattern) return;

    if (is_seq_match) {
        /* StringSeq cons-cell bindings. The seq pointer is already in
         * scope as `len_name` (which holds the StringSeq* itself for
         * the seq-match path). For `[h|t]` we read s->head and
         * s->tail; for `[a, b, c]` (fixed-arity over a seq) we walk
         * the cells. Skipped per-binding when the body never
         * references the binding name — same `pattern_needs_array`
         * optimisation the int-array path uses. */
        if (pattern->type == AST_PATTERN_CONS && pattern->child_count >= 2) {
            ASTNode* head = pattern->children[0];
            ASTNode* tail = pattern->children[1];
            if (head && head->type == AST_PATTERN_VARIABLE && head->value) {
                if (expr_references_var(body, head->value)) {
                    print_line(gen, "const char* %s = %s->head;",
                               head->value, len_name);
                    mark_pattern_binding(gen, head->value, TYPE_STRING);
                    /* A view of the cell's string: one the arm rebinds
                     * takes a reference of its own first. */
                    if (struct_param_written(body, head->value))
                        print_line(gen, "%s = aether_uniform_heap_str(%s, 0);",
                                   head->value, head->value);
                }
            }
            if (tail && tail->type == AST_PATTERN_VARIABLE && tail->value) {
                if (expr_references_var(body, tail->value)) {
                    print_line(gen, "StringSeq* %s = %s->tail;",
                               tail->value, len_name);
                    Type* st = make_string_seq_ptr_type();
                    mark_var_declared_typed(gen, tail->value, st);
                    own_rebound_binding_take(gen, tail->value, st, body, tail->line,
                                             tail->column);
                    free_type(st);
                }
            }
        } else if (pattern->type == AST_PATTERN_LIST && pattern->child_count > 0) {
            /* Fixed-arity over a seq — walk i cells. Cheap because we
             * already know the length matches (see the condition
             * emitted above), so no per-iteration NULL guard. */
            for (int i = 0; i < pattern->child_count; i++) {
                ASTNode* elem = pattern->children[i];
                if (elem && elem->type == AST_PATTERN_VARIABLE && elem->value &&
                    expr_references_var(body, elem->value)) {
                    print_indent(gen);
                    fprintf(gen->output, "const char* %s = ", elem->value);
                    fprintf(gen->output, "%s", len_name);
                    for (int j = 0; j < i; j++) fprintf(gen->output, "->tail");
                    fprintf(gen->output, "->head;\n");
                    mark_pattern_binding(gen, elem->value, TYPE_STRING);
                }
            }
        }
        return;
    }

    // Only declare the array pointer if this arm actually uses element bindings
    int needs_arr = pattern_needs_array(pattern, body);
    if (needs_arr) {
        print_indent(gen);
        fprintf(gen->output, "int* _match_arr = ");
        generate_expression_as_elem_ptr(gen, match_expr);   /* #1286 */
        fprintf(gen->output, ";\n");
    }

    if (pattern->type == AST_PATTERN_LIST && pattern->child_count > 0) {
        for (int i = 0; i < pattern->child_count; i++) {
            ASTNode* elem = pattern->children[i];
            if (elem && elem->type == AST_PATTERN_VARIABLE && elem->value) {
                if (expr_references_var(body, elem->value)) {
                    print_line(gen, "int %s = _match_arr[%d];", elem->value, i);
                    mark_pattern_binding(gen, elem->value, TYPE_INT);
                }
            }
        }
    } else if (pattern->type == AST_PATTERN_CONS && pattern->child_count >= 2) {
        ASTNode* head = pattern->children[0];
        ASTNode* tail = pattern->children[1];

        if (head && head->type == AST_PATTERN_VARIABLE && head->value) {
            if (expr_references_var(body, head->value)) {
                print_line(gen, "int %s = _match_arr[0];", head->value);
                mark_pattern_binding(gen, head->value, TYPE_INT);
            }
        }
        if (tail && tail->type == AST_PATTERN_VARIABLE && tail->value) {
            if (expr_references_var(body, tail->value)) {
                /* #1286: the tail is a `T[]` slice over the rest; `_len`
                 * is kept for code that still reads the companion. */
                print_line(gen, "AetherSlice %s = aether_slice_make(&_match_arr[1], (uint64_t)(%s - 1));",
                           tail->value, len_name);
                print_line(gen, "int %s_len = %s - 1;", tail->value, len_name);
                mark_pattern_binding(gen, tail->value, TYPE_ARRAY);
            }
        }
    }
}

static int has_list_patterns(ASTNode* match_stmt) {
    for (int i = 1; i < match_stmt->child_count; i++) {
        ASTNode* arm = match_stmt->children[i];
        if (arm && arm->type == AST_MATCH_ARM && arm->child_count >= 1) {
            ASTNode* pattern = arm->children[0];
            if (pattern && (pattern->type == AST_PATTERN_LIST ||
                           pattern->type == AST_PATTERN_CONS)) {
                return 1;
            }
        }
    }
    return 0;
}

// Forward declarations.

// The first top-level definition named `name` — for a pattern-matched
// multi-clause function, the first clause, whose body is representative
// for return-type purposes. Called once per user-fn call site from
// several codegen paths; #2007 routed it through the program index, so
// it no longer scans the top level per call.
ASTNode* find_function_definition_by_name(ASTNode* program,
                                          const char* name) {
    const DefClauses* dc = program_index_clauses(program, name);
    return (dc && dc->count > 0) ? dc->nodes[0] : NULL;
}

// Sibling of find_function_definition_by_name for AST_EXTERN_FUNCTION
// nodes. Used by is_heap_string_expr to consult the `@heap` annotation
// on extern returns. Two-step scan because externs don't get cloned
// into the program AST by module_merge_into_program — they stay in
// their owning module's AST (see compiler/aether_module.c:1283-1284).
// Same traversal pattern as codegen_diagnose_ownership at line 4142.
ASTNode* find_extern_declaration_by_name(ASTNode* program,
                                                const char* name) {
    if (!program || !name) return NULL;
    /* Pass 1 — direct extern declarations in the program AST (the
     * shape user code uses for non-module C bindings). */
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_EXTERN_FUNCTION &&
            c->value && strcmp(c->value, name) == 0) {
            return c;
        }
    }
    /* Pass 2 — externs reachable through `import` statements. The
     * stdlib's `extern http_request_body(...)` lives in std.http's
     * module AST; without this pass, call sites in the consuming
     * program never resolve to the annotated declaration. */
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (!c || c->type != AST_IMPORT_STATEMENT || !c->value) continue;
        AetherModule* mod_entry = module_find(c->value);
        ASTNode* mod_ast = mod_entry ? mod_entry->ast : NULL;
        if (!mod_ast) continue;
        for (int j = 0; j < mod_ast->child_count; j++) {
            ASTNode* decl = mod_ast->children[j];
            if (decl && decl->type == AST_EXTERN_FUNCTION &&
                decl->value && strcmp(decl->value, name) == 0) {
                return decl;
            }
        }
    }
    return NULL;
}

// Does an extern declaration carry a `@heap` return annotation, parsed
// in parse_extern_declaration as the string token "heap_return"
// appended to (or set as) the extern's annotation slot? Used by
// is_heap_string_expr's extern branch.
//
// substring-match rather than exact equality so the annotation can
// coexist with other extern-level annotations (`c_symbol:...` from the
// `@extern("name")` form, or any future tags) — the same combine-
// dedupe pattern the per-param annotation handler uses at
// parser.c:2800-2813.
static int extern_returns_heap_string(ASTNode* ext) {
    if (!ext || ext->type != AST_EXTERN_FUNCTION || !ext->annotation) {
        return 0;
    }
    return annotation_has_marker(ext->annotation, "heap_return");
}

/* Is `e` a string sequence its taker owns, moved rather than retained
 * (R3): a sequence literal, or a call of an Aether function or a closure
 * returning `*StringSeq` (a return hands one reference over), or of an
 * extern whose result says `@heap` (R4: `string.seq_tail` lends). */
int seq_value_is_fresh(CodeGenerator* gen, ASTNode* e) {
    if (!e || !e->node_type || !is_string_seq_ptr_type(e->node_type)) return 0;
    if (e->type == AST_ARRAY_LITERAL) return 1;
    if (e->type != AST_FUNCTION_CALL || !e->value) return 0;
    ASTNode* ext = gen->program ? find_extern_declaration_by_name(
                                      gen->program, codegen_normalise_callee(e->value))
                                : NULL;
    if (!ext || find_function_definition_by_name(gen->program, codegen_normalise_callee(e->value)))
        return 1;
    return extern_returns_heap_string(ext);
}

/* `e` as the value a place of type `*StringSeq` takes: a fresh sequence
 * moved in, any other retained. An array literal there is a sequence
 * literal, whatever the place (a field, a message, a tuple position). */
void emit_seq_take(CodeGenerator* gen, ASTNode* e) {
    if (e->type == AST_ARRAY_LITERAL && !is_string_seq_ptr_type(e->node_type)) {
        if (e->node_type) free_type(e->node_type);
        e->node_type = make_string_seq_ptr_type();
    }
    if (e->type == AST_IF_EXPRESSION && e->child_count >= 3) {
        fprintf(gen->output, "((");
        generate_expression(gen, e->children[0]);
        fprintf(gen->output, ") ? ");
        emit_seq_take(gen, e->children[1]);
        fprintf(gen->output, " : ");
        emit_seq_take(gen, e->children[2]);
        fprintf(gen->output, ")");
        return;
    }
    if (seq_value_is_fresh(gen, e) || e->type == AST_NULL_LITERAL) {
        generate_expression(gen, e);
        return;
    }
    fprintf(gen->output, "string_seq_retain(");
    generate_expression(gen, e);
    fprintf(gen->output, ")");
}

int or_fallible_value_slot_is_heap(CodeGenerator* gen, ASTNode* fallible);



/* Is `expr` a fresh string its taker owns, moved rather than retained (R2):
 * a call of an Aether function returning `string` or of an `@heap` extern,
 * an interpolation, a string local (its own), and the composites over those
 * (an `or` over a heap value slot, an owned `string?` unwrapped, a string
 * reply, a closure's string result). */
int is_heap_string_expr(CodeGenerator* gen, ASTNode* expr) {
    if (!expr) return 0;

    /* #2619: a string a call may hand back from a struct temporary of the
     * statement is copied where it is made (call_returns_view_of_temp), so
     * what it yields is a fresh heap string, adopted or freed as one. */
    if (expr->type == AST_FUNCTION_CALL && call_returns_view_of_temp(gen, expr)) return 1;

    /* #2586: a string a call through a typed fn pointer returns is the
     * caller's (the fn-value convention, discover_fn_values). */
    if (expr->type == AST_FUNCTION_CALL && fnptr_call_returns_string(gen, expr)) return 1;

    // String interpolation (non-printf mode) allocates via _aether_interp.
    if (expr->type == AST_STRING_INTERP) {
        return 1;
    }

    /* #2750: the string an owned `string?` call yields under `!` or `??`,
     * and a heap value slot under `!`. */
    if (is_owned_string_unwrap(gen, expr)) return 1;

    /* A string result of call(): a string-returning closure hands every
     * result over owned (see should_uniform_heap_return), so the caller
     * owns what it gets, whichever return site produced it (#2054). */
    if (expr->type == AST_FUNCTION_CALL && expr->value &&
        strcmp(expr->value, "call") == 0 &&
        expr->node_type && expr->node_type->kind == TYPE_STRING) {
        return 1;
    }

    /* An ask whose handler replies with a string. The reply statement
     * deep-copies the text so the asker's read outlives the handler's own
     * scope exit, which makes the value the asker receives owned: without
     * tracking it here, every string reply leaks one copy per ask. */
    if (expr->type == AST_SEND_ASK && expr->node_type &&
        expr->node_type->kind == TYPE_STRING) {
        return 1;
    }

    /* `fallible or handler` whose result is a string and whose success
     * value slot is heap: the `or` lowering takes the handler's value
     * via aether_uniform_heap_str so BOTH paths hand over one reference,
     * making the whole expression uniformly heap; the caller moves it into
     * its place. Must stay in lock-step with the boxing in the AST_OR_ELSE
     * codegen — both gate on or_fallible_value_slot_is_heap. */
    /* The `or`-node's own node_type is not reliably stamped in every
     * position (e.g. a `return f() or { … }` node can reach here with a
     * NULL node_type), so derive string-ness from the fallible's VALUE
     * slot type (`tuple_types[0]`) rather than the `or`-node itself —
     * that is the type the expression actually yields, and it is what
     * or_fallible_value_slot_is_heap already keys on. */
    if (expr->type == AST_OR_ELSE && expr->child_count >= 1 &&
        expr->children[0] && expr->children[0]->node_type &&
        expr->children[0]->node_type->kind == TYPE_TUPLE &&
        expr->children[0]->node_type->tuple_count >= 2 &&
        expr->children[0]->node_type->tuple_types[0] &&
        expr->children[0]->node_type->tuple_types[0]->kind == TYPE_STRING &&
        or_fallible_value_slot_is_heap(gen, expr->children[0])) {
        return 1;
    }

    /* A string local owns what it holds (R2). */
    if (expr->type == AST_IDENTIFIER && expr->value &&
        gen && is_heap_string_var(gen, expr->value)) {
        return 1;
    }

    if (expr->type == AST_FUNCTION_CALL && expr->value) {
        // `string.concat(...)` is the dotted name; the declarations use
        // the underscore form.
        const char* fn = codegen_normalise_callee(expr->value);
        /* R3: an Aether function returning `string` hands its caller one
         * reference; R1: an extern declared `@heap` hands over the string
         * it returns, typed `string` or `ptr` (a `*StringSeq` is not one). */
        if (gen && gen->program) {
            ASTNode* fn_def = find_function_definition_by_name(gen->program, fn);
            if (fn_def)
                return expr->node_type && expr->node_type->kind == TYPE_STRING &&
                       function_def_returns_heap_string(gen, fn_def);
            ASTNode* ext_def = find_extern_declaration_by_name(gen->program, fn);
            if (ext_def && extern_returns_heap_string(ext_def) &&
                !is_string_seq_ptr_type(expr->node_type)) return 1;
        }
    }

    return 0;
}

int function_def_returns_heap_at(CodeGenerator* gen, ASTNode* fn_def,
                                  int position);


/* #2670, #2671: is `name` a plain closure local of `owner`, the function or
 * closure whose body a walk reads: not a cell (a name promoted in its scope),
 * a module-level `var` or an actor's state, each of which takes a closure
 * its own way? Answered in the function being emitted from the scope in
 * force; for another top-level function (its callers ask when they judge
 * what it keeps or hands back), from the names promoted in that function's
 * scope, which are what its own emission will find in force, so caller and
 * callee agree. Anything else (a closure literal, a function nested in an
 * actor) is not answered: the caller keeps its old, conservative rule. */
static int closure_local_is_plain(CodeGenerator* gen, ASTNode* owner, const char* name) {
    if (!name) return 0;
    if (owner == gen->current_function) {
        return !is_promoted_capture(gen, name) && !is_module_global_var(gen, name) &&
               !is_actor_state_var(gen, name);
    }
    if (!owner || owner->type != AST_FUNCTION_DEFINITION || !owner->value || !gen->program ||
        find_function_definition_by_name(gen->program, owner->value) != owner) return 0;
    char** promoted = NULL;
    int count = 0;
    get_promoted_names_for_func(gen, owner->value, &promoted, &count);
    for (int i = 0; i < count; i++) {
        if (promoted[i] && strcmp(promoted[i], name) == 0) return 0;
    }
    return !is_module_global_var(gen, name);
}

static int closure_view_binding(ASTNode* rhs);

/* #2668: does `decl` (`a = b`, both closures), a statement of `owner`'s
 * body, give the local `a` a reference of its own to the env of `b`, a
 * closure local or parameter? The binding retains (emit_closure_alias_take),
 * so `a` owns what it holds, released by its scope or by its next binding,
 * and `b` keeps releasing its own. Before, `b` stopped owning its value at
 * such a binding and `a` never started: `get_last = get` in a loop body
 * leaked one env per pass. Not for a cell, a global or an actor's state,
 * which take their values their own way (closure_local_is_plain). A call's
 * closure is handed over (R3), so `a = keep(b)` adopts it and is none. */
static int closure_alias_binding_in(CodeGenerator* gen, ASTNode* owner, ASTNode* decl) {
    if (!decl || decl->type != AST_VARIABLE_DECLARATION || !decl->value ||
        decl->child_count < 1) return 0;
    ASTNode* rhs = decl->children[0];
    if (!rhs) return 0;
    Type* t = rhs->node_type;
    if (!t || t->kind != TYPE_FUNCTION || t->is_fnptr) return 0;
    Type* vt = decl->node_type;
    if (vt && vt->kind != TYPE_UNKNOWN && !(vt->kind == TYPE_FUNCTION && !vt->is_fnptr)) return 0;
    if (rhs->type == AST_IDENTIFIER) {
        if (!rhs->value || strcmp(rhs->value, decl->value) == 0) return 0;
    } else if (!closure_view_binding(rhs)) {
        return 0;
    }
    return closure_local_is_plain(gen, owner, decl->value);
}

/* The same, for a statement of the function being emitted: what its emission
 * does. */
static int closure_alias_binding(CodeGenerator* gen, ASTNode* decl) {
    return closure_alias_binding_in(gen, gen->current_function, decl);
}


int function_def_returns_heap_at(CodeGenerator* gen, ASTNode* fn_def,
                                  int position);

/* Heap evidence for a return-site expression inside the classifiers.
 * Bare identifiers MUST resolve structurally against the analysed
 * function's own body (see the INVARIANT above): is_heap_string_expr
 * consults the currently-emitting function's tracker table for
 * identifiers, which is the wrong function whenever classification is
 * triggered from a caller's destructure site. `fn_name` is the analysed
 * function's. */


/* #2815: is `o` a `string?` named by an identifier that is not a
 * parameter of `fn_def` (a local, a cell, or a module var)? A local owns
 * the string it holds and frees it at scope exit, after a return value
 * read from it is taken and before the caller uses it, so a return takes
 * a copy of it (or the some binding over it is one). A parameter's string
 * is the caller's and outlives the call; a module var keeps its strings,
 * so a copy of one costs a copy only. */
static int opt_string_name_owned(CodeGenerator* gen, ASTNode* o, ASTNode* fn_def) {
    if (!o || o->type != AST_IDENTIFIER || !o->value || !is_opt_string_type(o->node_type))
        return 0;
    for (int i = 0; fn_def && i < fn_def->child_count; i++) {
        ASTNode* c = fn_def->children[i];
        if (c && (c->type == AST_PATTERN_VARIABLE || c->type == AST_VARIABLE_DECLARATION) &&
            c->value && strcmp(c->value, o->value) == 0) {
            /* A parameter a closure writes is a cell holding a string of
             * its own (#2795), given back when the cell goes. */
            char** promoted = NULL;
            int count = 0;
            get_promoted_names_for_func(gen, fn_scope_name(gen->program, fn_def), &promoted,
                                        &count);
            for (int k = 0; k < count; k++)
                if (promoted[k] && strcmp(promoted[k], o->value) == 0) return 1;
            return 0;
        }
    }
    return 1;
}

/* #2815: is `e` `o ?? d` or `o!` over such a `string?` (opt_string_name_owned)? */
static int opt_string_local_unwrap(CodeGenerator* gen, ASTNode* e, ASTNode* fn_def) {
    if (!e || (e->type != AST_NULL_COALESCE && e->type != AST_TUPLE_UNWRAP) ||
        e->child_count < 1) return 0;
    return opt_string_name_owned(gen, e->children[0], fn_def);
}


/* R3: an Aether function returning `string` hands its caller one reference
 * from every return; the type it is emitted with says so. */
int function_def_returns_heap_string(CodeGenerator* gen, ASTNode* fn_def) {
    if (!fn_def || (fn_def->type != AST_FUNCTION_DEFINITION &&
                    fn_def->type != AST_BUILDER_FUNCTION)) return 0;
    Type* t = fn_result_type(gen, fn_def);
    return t && t->kind == TYPE_STRING;
}

/* #2369: heap.new box provenance.
 *
 * A field store releases the value it replaces through any struct pointer
 * (every box Aether allocates starts with its trackers zeroed: heap.new
 * calloc's it, and a call of C's malloc is lowered to calloc, see
 * call_is_c_malloc). What still depends on where a box was made is
 * whether something destroys it with its fields: heap.free does, while a box
 * from `malloc(n) as *T`, a C pointer or a list element is usually freed with
 * free(), which releases none of them. A copy of a `string` parameter stored
 * into such a box would have no releaser (param_store_through_raw_pointer).
 *
 * zb_expr answers "does this expression always yield a heap.new(T) box, or
 * null?" by following the value back to where it was made:
 *   - heap.new(T), and null / none (a store through null faults anyway);
 *   - a cast (`x as *T`, `x as ptr`) of such a value. The cast does not
 *     touch the memory, but a box of another struct has its trackers at other
 *     offsets, so the struct must be the same T;
 *   - a call to an Aether function every `return` of which is such a value;
 *   - a local every binding of which, anywhere in its function, is such a
 *     value (flow-insensitive, so a loop that rebinds it later still counts);
 *   - a struct field every store into which, anywhere in the program (struct
 *     literals included), is such a value.
 * Everything else, a parameter, a C extern's result, a list element, a
 * global, is of unknown origin and answers no. "No" is always the safe
 * answer, so a cycle or the depth limit resolves to it. */
#define ZB_MAX_DEPTH 24
#define ZB_IN_PROGRESS ((void*)1)
#define ZB_YES ((void*)2)
#define ZB_NO ((void*)3)

static int zb_expr(CodeGenerator* gen, ASTNode* e, ASTNode* ctx,
                   const char* sname, int depth);

static const char* zb_struct_of(Type* t) {
    if (!t) return NULL;
    if (t->kind == TYPE_STRUCT) return t->struct_name;
    if (t->kind == TYPE_PTR && t->element_type &&
        t->element_type->kind == TYPE_STRUCT) return t->element_type->struct_name;
    return NULL;
}

static int zb_is_name(ASTNode* n, const char* name) {
    return n && n->type == AST_IDENTIFIER && n->value && strcmp(n->value, name) == 0;
}

static int zb_mentions(ASTNode* n, const char* name) {
    if (!n) return 0;
    if (n->value && strcmp(n->value, name) == 0) return 1;
    for (int i = 0; i < n->child_count; i++) {
        if (zb_mentions(n->children[i], name)) return 1;
    }
    return 0;
}

/* A node whose `value` names something other than a binding of a local:
 * a use, a field, a callee, a type. Any other node spelling the name (a
 * pattern variable, a closure parameter, a catch binding) binds it in a way
 * the walk below does not follow, so the local is of unknown origin. */
static int zb_value_not_a_binding(ASTNode* n) {
    switch (n->type) {
        case AST_IDENTIFIER: case AST_MEMBER_ACCESS: case AST_OPTIONAL_CHAIN:
        case AST_FUNCTION_CALL: case AST_LITERAL: case AST_NAMED_ARG:
        case AST_HEAP_NEW: case AST_STRUCT_LITERAL: case AST_PTR_AS_STRUCT_CAST:
        case AST_SIZEOF: case AST_OFFSETOF:
            return 1;
        default:
            return 0;
    }
}

/* Is every binding of local `name` under `n` a zeroed `sname` box? Sets
 * `*found` when it sees one, so a name never bound here (a global, a capture
 * the closure does not bind) is not mistaken for a local. */
static int zb_bindings(CodeGenerator* gen, ASTNode* n, ASTNode* ctx, const char* name,
                       const char* sname, int depth, int* found) {
    if (!n) return 1;
    switch (n->type) {
        case AST_VARIABLE_DECLARATION:
            if (n->value && strcmp(n->value, name) == 0) {
                *found = 1;
                /* No initialiser: a tuple-destructure target or a bare typed
                 * declaration, bound to something this walk cannot see. */
                if (n->child_count == 0 || !n->children[0]) return 0;
                if (!zb_expr(gen, n->children[0], ctx, sname, depth + 1)) return 0;
            }
            break;
        case AST_ASSIGNMENT:
        case AST_BINARY_EXPRESSION:
            if (n->child_count >= 2 && zb_is_name(n->children[0], name) &&
                (n->type == AST_ASSIGNMENT || (n->value && strcmp(n->value, "=") == 0))) {
                *found = 1;
                if (!zb_expr(gen, n->children[1], ctx, sname, depth + 1)) return 0;
            }
            break;
        case AST_COMPOUND_ASSIGNMENT:
            if (n->child_count >= 1 && zb_is_name(n->children[0], name)) return 0;
            break;
        case AST_UNARY_EXPRESSION:
            /* `&b` can be written through; `b++` rebinds it. */
            if (n->child_count >= 1 && zb_is_name(n->children[0], name) && n->value &&
                (strcmp(n->value, "&") == 0 || strcmp(n->value, "++") == 0 ||
                 strcmp(n->value, "--") == 0)) return 0;
            break;
        default:
            if (n->value && strcmp(n->value, name) == 0 && !zb_value_not_a_binding(n))
                return 0;
            break;
    }
    for (int i = 0; i < n->child_count; i++) {
        if (!zb_bindings(gen, n->children[i], ctx, name, sname, depth, found)) return 0;
    }
    return 1;
}

static int zb_memo_get(CodeGenerator* gen, const char* key, int* answer) {
    void* v = strmap_get(&gen->zeroed_box_memo, key);
    if (!v) return 0;
    *answer = (v == ZB_YES);
    return 1;
}

static int zb_contains(ASTNode* n, ASTNode* target) {
    if (!n) return 0;
    if (n == target) return 1;
    for (int i = 0; i < n->child_count; i++) {
        if (zb_contains(n->children[i], target)) return 1;
    }
    return 0;
}

/* The function, main or builder whose body holds `closure`, or NULL (a
 * closure in an actor handler or a global initialiser). */
static ASTNode* zb_enclosing_function(CodeGenerator* gen, ASTNode* closure) {
    if (!gen->program) return NULL;
    char key[64];
    snprintf(key, sizeof(key), "P|%p", (void*)closure);
    void* v = strmap_get(&gen->zeroed_box_memo, key);
    if (v) return v == ZB_NO ? NULL : (ASTNode*)v;
    ASTNode* found = NULL;
    for (int i = 0; !found && i < gen->program->child_count; i++) {
        ASTNode* d = gen->program->children[i];
        if (d && (d->type == AST_FUNCTION_DEFINITION || d->type == AST_MAIN_FUNCTION ||
                  d->type == AST_BUILDER_FUNCTION) && zb_contains(d, closure))
            found = d;
    }
    strmap_put(&gen->zeroed_box_memo, key, found ? (void*)found : ZB_NO);
    return found;
}

static int zb_param_holds_boxes(CodeGenerator* gen, ASTNode* fn, const char* name,
                                const char* sname);

static int zb_local(CodeGenerator* gen, ASTNode* ctx, const char* name,
                    const char* sname, int depth) {
    if (!ctx || !name) return 0;
    if (is_module_global_var(gen, name)) return 0;
    /* Inside a closure the name is either the closure's own or a capture,
     * and a closure's write to a capture is a write to the captured
     * variable (#2458). Both are bindings somewhere in the enclosing
     * function's body, closures included, so that body is the scope to
     * walk; a closure parameter of the name anywhere in it is refused by
     * the walk, which keeps a shadowing parameter from passing as the
     * captured box. */
    if (ctx->type == AST_CLOSURE) ctx = zb_enclosing_function(gen, ctx);
    if (!ctx) return 0;
    ASTNode* body;
    int is_param = 0;
    if (ctx->type == AST_FUNCTION_DEFINITION || ctx->type == AST_MAIN_FUNCTION ||
        ctx->type == AST_BUILDER_FUNCTION) {
        if (ctx->child_count == 0) return 0;
        /* A parameter: its value is whatever the callers pass, a box only
         * where every call passes one (zb_param_holds_boxes), and then
         * whatever the body rebinds it to. */
        for (int i = 0; i < ctx->child_count - 1; i++) {
            if (zb_mentions(ctx->children[i], name)) {
                if (!zb_param_holds_boxes(gen, ctx, name, sname)) return 0;
                is_param = 1;
                break;
            }
        }
        body = ctx->children[ctx->child_count - 1];
    } else {
        return 0;
    }
    /* The map copies its keys, so each is built whole and freed here. */
    char* key = heap_strf("L|%p|%s|%s", (void*)ctx, name, sname);
    int answer;
    if (zb_memo_get(gen, key, &answer)) { free(key); return answer; }
    strmap_put(&gen->zeroed_box_memo, key, ZB_IN_PROGRESS);
    int found = 0;
    answer = zb_bindings(gen, body, ctx, name, sname, depth, &found) && (found || is_param);
    strmap_put(&gen->zeroed_box_memo, key, answer ? ZB_YES : ZB_NO);
    free(key);
    return answer;
}

static int zb_returns(CodeGenerator* gen, ASTNode* n, ASTNode* fn, const char* sname,
                      int depth, int* saw) {
    if (!n) return 1;
    if (ast_is_own_function(n)) return 1;   /* its returns are its own */
    if (n->type == AST_RETURN_STATEMENT) {
        *saw = 1;
        return n->child_count > 0 && zb_expr(gen, n->children[0], fn, sname, depth + 1);
    }
    for (int i = 0; i < n->child_count; i++) {
        if (!zb_returns(gen, n->children[i], fn, sname, depth, saw)) return 0;
    }
    return 1;
}

/* Does anything in the program bind `name` as a variable: a local, a
 * parameter, a closure parameter, a pattern, a global? A call spelled with
 * such a name may invoke that variable's closure rather than the top-level
 * function of the same name, so the function's returns say nothing about it. */
static int zb_binds_name(ASTNode* n, const char* name) {
    if (!n) return 0;
    switch (n->type) {
        case AST_FUNCTION_DEFINITION: case AST_BUILDER_FUNCTION:
            for (int i = 0; i + 1 < n->child_count; i++) {
                if (zb_mentions(n->children[i], name)) return 1;
            }
            break;
        case AST_ASSIGNMENT: case AST_BINARY_EXPRESSION:
            if (n->child_count >= 2 && zb_is_name(n->children[0], name) &&
                (n->type == AST_ASSIGNMENT || (n->value && strcmp(n->value, "=") == 0)))
                return 1;
            break;
        case AST_IDENTIFIER: case AST_MEMBER_ACCESS: case AST_OPTIONAL_CHAIN:
        case AST_FUNCTION_CALL: case AST_LITERAL: case AST_NAMED_ARG:
            break;
        default:
            /* Declarations, closure parameters, patterns, state. */
            if (n->value && strcmp(n->value, name) == 0) return 1;
            break;
    }
    for (int i = 0; i < n->child_count; i++) {
        if (zb_binds_name(n->children[i], name)) return 1;
    }
    return 0;
}

static int zb_call(CodeGenerator* gen, const char* name, const char* sname, int depth) {
    if (!name || !gen->program) return 0;
    char* key = heap_strf("R|%s|%s", name, sname);
    int answer;
    if (zb_memo_get(gen, key, &answer)) { free(key); return answer; }
    char* bkey = heap_strf("B|%s", name);
    int bound;
    if (!zb_memo_get(gen, bkey, &bound)) {
        bound = is_module_global_var(gen, name);
        for (int i = 0; !bound && i < gen->program->child_count; i++) {
            ASTNode* d = gen->program->children[i];
            /* The definitions themselves name it; look inside them. */
            if (d && (d->type == AST_FUNCTION_DEFINITION || d->type == AST_EXTERN_FUNCTION ||
                      d->type == AST_BUILDER_FUNCTION) &&
                d->value && strcmp(d->value, name) == 0) {
                for (int j = 0; !bound && j < d->child_count; j++)
                    bound = zb_binds_name(d->children[j], name);
                continue;
            }
            bound = zb_binds_name(d, name);
        }
        strmap_put(&gen->zeroed_box_memo, bkey, bound ? ZB_YES : ZB_NO);
    }
    free(bkey);
    if (bound) {
        strmap_put(&gen->zeroed_box_memo, key, ZB_NO);
        free(key);
        return 0;
    }
    strmap_put(&gen->zeroed_box_memo, key, ZB_IN_PROGRESS);
    /* Every definition of the name must qualify (a guarded function has one
     * per clause); an extern or anything else of that name does not. */
    int defs = 0;
    answer = 1;
    for (int i = 0; answer && i < gen->program->child_count; i++) {
        ASTNode* d = gen->program->children[i];
        if (!d || !d->value || strcmp(d->value, name) != 0) continue;
        if (d->type != AST_FUNCTION_DEFINITION || d->child_count == 0) { answer = 0; break; }
        int saw = 0;
        answer = zb_returns(gen, d->children[d->child_count - 1], d, sname, depth, &saw) && saw;
        defs++;
    }
    answer = answer && defs > 0;
    strmap_put(&gen->zeroed_box_memo, key, answer ? ZB_YES : ZB_NO);
    free(key);
    return answer;
}

/* Does `lhs` name field `field` of struct `owner`? 1 yes, 0 no, -1 when it
 * names a field of that name on an object whose type is not known. */
static int zb_names_field(ASTNode* lhs, const char* owner, const char* field) {
    if (!lhs || !lhs->value || strcmp(lhs->value, field) != 0) return 0;
    if (lhs->type == AST_OPTIONAL_CHAIN) return -1;
    if (lhs->type != AST_MEMBER_ACCESS) return 0;
    const char* s = (lhs->child_count > 0 && lhs->children[0])
                    ? zb_struct_of(lhs->children[0]->node_type) : NULL;
    if (!s) return -1;
    return strcmp(s, owner) == 0;
}

static int zb_field_stores(CodeGenerator* gen, ASTNode* n, ASTNode* ctx, const char* owner,
                           const char* field, const char* sname, int depth, int* saw) {
    if (!n) return 1;
    /* A closure keeps its enclosing function as the scope (see zb_local). */
    if (n->type == AST_FUNCTION_DEFINITION || n->type == AST_MAIN_FUNCTION ||
        n->type == AST_BUILDER_FUNCTION) {
        ctx = n;
    } else if (n->type == AST_ACTOR_DEFINITION) {
        ctx = NULL;
    }
    if ((n->type == AST_ASSIGNMENT ||
         (n->type == AST_BINARY_EXPRESSION && n->value && strcmp(n->value, "=") == 0)) &&
        n->child_count >= 2) {
        int hit = zb_names_field(n->children[0], owner, field);
        if (hit < 0) return 0;
        if (hit && !zb_expr(gen, n->children[1], ctx, sname, depth + 1)) return 0;
        if (hit) *saw = 1;
    }
    if ((n->type == AST_COMPOUND_ASSIGNMENT ||
         (n->type == AST_UNARY_EXPRESSION && n->value && strcmp(n->value, "&") == 0)) &&
        n->child_count >= 1 && zb_names_field(n->children[0], owner, field) != 0) {
        return 0;
    }
    if (n->type == AST_STRUCT_LITERAL) {
        const char* lit = zb_struct_of(n->node_type);
        for (int i = 0; i < n->child_count; i++) {
            ASTNode* fi = n->children[i];
            if (!fi || fi->type != AST_ASSIGNMENT || fi->child_count < 1 || !fi->value ||
                strcmp(fi->value, field) != 0) continue;
            if (!lit && !(n->value && strcmp(n->value, owner) == 0)) return 0;
            if (lit && strcmp(lit, owner) != 0) continue;
            if (!zb_expr(gen, fi->children[0], ctx, sname, depth + 1)) return 0;
            *saw = 1;
        }
    }
    for (int i = 0; i < n->child_count; i++) {
        if (!zb_field_stores(gen, n->children[i], ctx, owner, field, sname, depth, saw))
            return 0;
    }
    return 1;
}

static int zb_field(CodeGenerator* gen, const char* owner, const char* field,
                    const char* sname, int depth) {
    if (!gen->program) return 0;
    char* key = heap_strf("F|%s|%s|%s", owner, field, sname);
    int answer;
    if (zb_memo_get(gen, key, &answer)) { free(key); return answer; }
    strmap_put(&gen->zeroed_box_memo, key, ZB_IN_PROGRESS);
    /* A field no Aether code stores into is filled by C, raw memory writes
     * or nothing at all: of unknown origin, not vacuously a box. */
    int saw = 0;
    answer = zb_field_stores(gen, gen->program, NULL, owner, field, sname, depth, &saw) && saw;
    strmap_put(&gen->zeroed_box_memo, key, answer ? ZB_YES : ZB_NO);
    free(key);
    return answer;
}

static int zb_expr(CodeGenerator* gen, ASTNode* e, ASTNode* ctx,
                   const char* sname, int depth) {
    if (!e || !sname || depth > ZB_MAX_DEPTH) return 0;
    switch (e->type) {
        case AST_NULL_LITERAL:
        case AST_NONE_LITERAL:
            return 1;
        case AST_HEAP_NEW: {
            const char* made = NULL;
            if (e->node_type && e->node_type->kind == TYPE_PTR)
                made = zb_struct_of(e->node_type);
            if (!made) made = e->value;
            return made && strcmp(made, sname) == 0;
        }
        case AST_PTR_AS_STRUCT_CAST:
            if (!e->value || strcmp(e->value, sname) != 0) return 0;
            return e->child_count > 0 && zb_expr(gen, e->children[0], ctx, sname, depth + 1);
        case AST_VALUE_CAST:
            if (!e->node_type || e->node_type->kind != TYPE_PTR) return 0;
            return e->child_count > 0 && zb_expr(gen, e->children[0], ctx, sname, depth + 1);
        case AST_IDENTIFIER:
            return zb_local(gen, ctx, e->value, sname, depth + 1);
        case AST_FUNCTION_CALL:
            return zb_call(gen, e->value, sname, depth + 1);
        case AST_MEMBER_ACCESS: {
            if (!e->value || e->child_count < 1 || !e->children[0]) return 0;
            const char* owner = zb_struct_of(e->children[0]->node_type);
            return owner && zb_field(gen, owner, e->value, sname, depth + 1);
        }
        default:
            return 0;
    }
}

/* Which pointer-to-struct parameters only ever hold a heap.new box (#2369).
 *
 * A parameter holds what its callers pass, so it holds only boxes when every
 * call passes one: a box made there, or a parameter of the calling function
 * that in turn holds only boxes. That needs every call to be visible, so a
 * function qualifies only when the program calls it directly and in no
 * other way:
 *   - it has internal linkage (fn_has_internal_linkage: an imported
 *     module's function, or a file-local `name_`). A top-level function of
 *     the entry file is an external symbol C may call (#703), from an
 *     `--extra` file or a plugin, with a struct it allocated itself;
 *   - not in a library or header build (C calls those through stubs);
 *   - one definition taking plain parameters, not a builder (its `_ctx` is
 *     injected, shifting every argument), never given a trailing block;
 *   - its name used nowhere but as a callee: a value, a variable, a field, a
 *     literal or an annotation spelling it, or an extern of that name in
 *     any module, could reach a call this walk does not see;
 *   - every call passing one argument per parameter (a named argument is
 *     not followed; a default is filled in before codegen).
 * A function nothing calls stays unknown too.
 *
 * Parameters that pass themselves on form a graph, recursion included. Each
 * starts as "only boxes"; a call passing anything else marks its parameter
 * unknown, and unknown spreads along the graph until nothing changes. What
 * is left holds only boxes, since every value a parameter can hold comes in
 * through some call from outside the cycle it sits on. The arguments are
 * judged by zb_expr while every parameter answers "unknown", the safe
 * answer; the memo those answers filled is dropped afterwards, so later
 * questions see the parameters' own. */
typedef struct {
    const char* fn;     /* the function's name */
    int idx;            /* the parameter's position */
    const char* sname;  /* the struct it points to */
    int unknown;        /* some call may pass something other than a box */
    int* feeds;         /* the parameters it is passed on to */
    int feed_count;
    int feed_cap;
} ZbParam;

typedef struct {
    StrMap at;          /* "fn|idx" -> index into params, plus 1 */
    ZbParam* params;
    int count;
    int cap;
} ZbParams;

typedef struct {
    ASTNode* call;
    ASTNode* ctx;       /* the innermost closure around the call, else fn */
    ASTNode* fn;        /* the top-level function, main or builder, or NULL */
    int next;           /* the next call of the same callee, or -1 */
} ZbSite;

typedef struct {
    StrMap mentions;    /* every name spelled other than as a callee */
    StrMap first;       /* callee -> index of its last-seen call, plus 1 */
    ZbSite* sites;
    int site_count;
    int site_cap;
    const char** notes; /* annotations, which may name a function */
    int note_count;
    int note_cap;
} ZbScan;

/* A name as `codegen_normalise_callee` would spell it, without interning
 * it: every identifier and literal of the program passes through here. */
static void zb_scan_mention_normalised(ZbScan* sc, const char* name) {
    size_t n = strlen(name);
    char* tmp = (char*)aether_xrealloc(NULL, n + 1);
    for (size_t i = 0; i <= n; i++) tmp[i] = name[i] == '.' ? '_' : name[i];
    strmap_put(&sc->mentions, tmp, ZB_YES);
    free(tmp);
}

void zb_params_free(CodeGenerator* gen) {
    ZbParams* zp = (ZbParams*)gen->zb_params;
    if (!zp) return;
    for (int i = 0; i < zp->count; i++) free(zp->params[i].feeds);
    free(zp->params);
    strmap_free(&zp->at);
    free(zp);
    gen->zb_params = NULL;
}

static void zb_scan_mention(ZbScan* sc, const char* name) {
    if (!name || !name[0]) return;
    strmap_put(&sc->mentions, name, ZB_YES);
    if (strchr(name, '.')) zb_scan_mention_normalised(sc, name);
}

/* `a.b.c` spelled by a member-access chain over an identifier, or NULL. */
static char* zb_dotted_path(ASTNode* n) {
    if (!n || !n->value) return NULL;
    if (n->type == AST_IDENTIFIER) return strdup(n->value);
    if (n->type != AST_MEMBER_ACCESS || n->child_count != 1) return NULL;
    char* base = zb_dotted_path(n->children[0]);
    if (!base) return NULL;
    char* path = heap_strf("%s.%s", base, n->value);
    free(base);
    return path;
}

static void zb_scan_note(ZbScan* sc, const char* note) {
    if (!note) return;
    if (sc->note_count == sc->note_cap) {
        sc->note_cap = sc->note_cap ? sc->note_cap * 2 : 64;
        sc->notes = aether_xrealloc(sc->notes, sizeof(*sc->notes) * sc->note_cap);
    }
    sc->notes[sc->note_count++] = note;
}

static void zb_scan(ZbScan* sc, ASTNode* n, ASTNode* fn, ASTNode* ctx) {
    if (!n) return;
    zb_scan_note(sc, n->annotation);
    if (n->type == AST_FUNCTION_CALL && n->value) {
        const char* callee = codegen_normalise_callee(n->value);
        if (sc->site_count == sc->site_cap) {
            sc->site_cap = sc->site_cap ? sc->site_cap * 2 : 256;
            sc->sites = aether_xrealloc(sc->sites, sizeof(*sc->sites) * sc->site_cap);
        }
        void* last = strmap_get(&sc->first, callee);
        ZbSite* s = &sc->sites[sc->site_count];
        s->call = n;
        s->ctx = ctx;
        s->fn = fn;
        s->next = last ? (int)(intptr_t)last - 1 : -1;
        strmap_put(&sc->first, callee, (void*)(intptr_t)(sc->site_count + 1));
        sc->site_count++;
    } else if (n->value) {
        zb_scan_mention(sc, n->value);
        if (n->type == AST_MEMBER_ACCESS) {
            char* path = zb_dotted_path(n);
            zb_scan_mention(sc, path);
            free(path);
        }
    }
    ASTNode* inner = n->type == AST_CLOSURE ? n : ctx;
    for (int i = 0; i < n->child_count; i++) zb_scan(sc, n->children[i], fn, inner);
}

static int zb_is_param_node(ASTNode* c) {
    switch (c->type) {
        case AST_VARIABLE_DECLARATION: case AST_PATTERN_VARIABLE: case AST_PATTERN_LITERAL:
        case AST_PATTERN_STRUCT: case AST_PATTERN_LIST: case AST_PATTERN_CONS:
            return 1;
        default:
            return 0;
    }
}

/* The parameter count of a function whose parameters are all plain names,
 * or -1 (a pattern parameter is matched, not bound by position). */
static int zb_plain_param_count(ASTNode* fn) {
    int k = 0;
    for (int i = 0; i + 1 < fn->child_count; i++) {
        ASTNode* c = fn->children[i];
        if (!c || !zb_is_param_node(c)) continue;
        if (c->type != AST_VARIABLE_DECLARATION && c->type != AST_PATTERN_VARIABLE) return -1;
        if (!c->value) return -1;
        k++;
    }
    return k;
}

static ASTNode* zb_param_node(ASTNode* fn, int pos) {
    int k = 0;
    for (int i = 0; i + 1 < fn->child_count; i++) {
        ASTNode* c = fn->children[i];
        if (!c || !zb_is_param_node(c)) continue;
        if (k++ == pos) return c;
    }
    return NULL;
}

static int zb_param_pos(ASTNode* fn, const char* name) {
    int k = 0;
    for (int i = 0; i + 1 < fn->child_count; i++) {
        ASTNode* c = fn->children[i];
        if (!c || !zb_is_param_node(c)) continue;
        if (c->value && strcmp(c->value, name) == 0) return k;
        k++;
    }
    return -1;
}

static ZbParam* zb_param_find(ZbParams* zp, const char* fn, int idx) {
    char* key = heap_strf("%s|%d", fn, idx);
    void* v = strmap_get(&zp->at, key);
    free(key);
    return v ? &zp->params[(int)(intptr_t)v - 1] : NULL;
}

static int zb_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_';
}

/* Does annotation `note` spell `name` as a whole identifier? */
static int zb_note_names(const char* note, const char* name) {
    size_t n = strlen(name);
    for (const char* at = strstr(note, name); at; at = strstr(at + 1, name)) {
        if ((at == note || !zb_ident_char(at[-1])) && !zb_ident_char(at[n])) return 1;
    }
    return 0;
}

/* Does function definition `fn` qualify (see above), given the scan? The
 * cheap tests first: most functions fail one of them. */
static int zb_fn_qualifies(CodeGenerator* gen, ZbScan* sc, ASTNode* fn) {
    if (!fn->value || !fn_has_internal_linkage(fn) || is_builder_func_reg(gen, fn->value))
        return 0;
    int n = zb_plain_param_count(fn);
    void* first = strmap_get(&sc->first, fn->value);
    if (n < 1 || !first) return 0;
    ASTNode* p0 = zb_param_node(fn, 0);
    if (!p0 || (p0->value && strcmp(p0->value, "_ctx") == 0)) return 0;
    const DefClauses* dc = program_index_clauses(gen->program, fn->value);
    if (!dc || dc->count != 1 || dc->nodes[0] != fn) return 0;
    if (strmap_has(&sc->mentions, fn->value)) return 0;
    for (int s = (int)(intptr_t)first - 1; s >= 0; s = sc->sites[s].next) {
        ASTNode* call = sc->sites[s].call;
        if (call->child_count != n) return 0;
        for (int a = 0; a < call->child_count; a++) {
            ASTNode* arg = call->children[a];
            if (!arg || arg->type == AST_NAMED_ARG ||
                (arg->type == AST_CLOSURE && arg->value && strcmp(arg->value, "trailing") == 0))
                return 0;
        }
    }
    for (int i = 0; i < sc->note_count; i++) {
        if (zb_note_names(sc->notes[i], fn->value)) return 0;
    }
    return 1;
}

/* Does `fn` take a pointer-to-struct parameter, the only kind asked about? */
static int zb_fn_takes_box(ASTNode* fn) {
    int n = zb_plain_param_count(fn);
    for (int k = 0; k < n; k++) {
        ASTNode* p = zb_param_node(fn, k);
        if (p && p->node_type && p->node_type->kind == TYPE_PTR && zb_struct_of(p->node_type))
            return 1;
    }
    return 0;
}

static void zb_params_build(CodeGenerator* gen) {
    ZbParams* zp = aether_xrealloc(NULL, sizeof(ZbParams));
    memset(zp, 0, sizeof(*zp));
    strmap_init(&zp->at);
    gen->zb_params = zp;
    /* A library or header build is called from C, which this walk cannot
     * see: every parameter stays of unknown origin. */
    if (!gen->program || gen->emit_lib || gen->emit_header || gen->csrc_header_file ||
        gen->csrc_catalog_file) {
        gen->zb_params_state = 2;
        return;
    }

    ZbScan sc;
    memset(&sc, 0, sizeof(sc));
    strmap_init(&sc.mentions);
    strmap_init(&sc.first);
    for (int i = 0; i < gen->program->child_count; i++) {
        ASTNode* d = gen->program->children[i];
        if (!d) continue;
        if (d->type == AST_FUNCTION_DEFINITION || d->type == AST_MAIN_FUNCTION ||
            d->type == AST_BUILDER_FUNCTION) {
            /* The definition spells its own name; everything in it counts. */
            zb_scan_note(&sc, d->annotation);
            for (int j = 0; j < d->child_count; j++) zb_scan(&sc, d->children[j], d, d);
        } else {
            zb_scan(&sc, d, NULL, NULL);
        }
    }
    /* Externs stay in their modules' trees: one spelling a function's
     * symbol (`@extern("name")` or the same name) would call it from
     * where the walk cannot see. */
    for (int m = 0; global_module_registry && m < global_module_registry->module_count; m++) {
        AetherModule* mod = global_module_registry->modules[m];
        for (int i = 0; mod && mod->ast && i < mod->ast->child_count; i++) {
            ASTNode* d = mod->ast->children[i];
            if (!d || d->type != AST_EXTERN_FUNCTION) continue;
            zb_scan_mention(&sc, d->value);
            zb_scan_note(&sc, d->annotation);
        }
    }

    /* The candidates: each pointer-to-struct parameter of a function that
     * qualifies; any other function's parameters are left out, which is
     * "unknown" to every question about them. */
    for (int i = 0; i < gen->program->child_count; i++) {
        ASTNode* d = gen->program->children[i];
        if (!d || d->type != AST_FUNCTION_DEFINITION || !zb_fn_takes_box(d) ||
            !zb_fn_qualifies(gen, &sc, d)) continue;
        int n = zb_plain_param_count(d);
        for (int k = 0; k < n; k++) {
            ASTNode* p = zb_param_node(d, k);
            if (!p || !p->node_type || p->node_type->kind != TYPE_PTR) continue;
            const char* sname = zb_struct_of(p->node_type);
            if (!sname) continue;
            if (zp->count == zp->cap) {
                zp->cap = zp->cap ? zp->cap * 2 : 64;
                zp->params = aether_xrealloc(zp->params, sizeof(*zp->params) * zp->cap);
            }
            ZbParam* zq = &zp->params[zp->count];
            memset(zq, 0, sizeof(*zq));
            zq->fn = d->value;
            zq->idx = k;
            zq->sname = sname;
            char* key = heap_strf("%s|%d", d->value, k);
            strmap_put(&zp->at, key, (void*)(intptr_t)(zp->count + 1));
            free(key);
            zp->count++;
        }
    }

    /* What each call passes. A parameter of the calling function that its
     * body leaves alone (zb_bindings: never bound, written through `&`,
     * stepped, or shadowed by a closure parameter or pattern) is an edge;
     * anything else is judged as it stands. */
    gen->zb_params_state = 1;
    /* Whether a caller leaves a parameter alone, asked once per caller and
     * name: a function passing its parameter on many times walked its body
     * for every call. */
    StrMap untouched;
    strmap_init(&untouched);
    for (int q = 0; q < zp->count; q++) {
        void* first = strmap_get(&sc.first, zp->params[q].fn);
        for (int s = first ? (int)(intptr_t)first - 1 : -1; s >= 0; s = sc.sites[s].next) {
            ZbSite* site = &sc.sites[s];
            ASTNode* arg = site->call->children[zp->params[q].idx];
            ASTNode* caller = site->fn;
            int pos = -1;
            if (arg->type == AST_IDENTIFIER && arg->value && caller &&
                caller->type == AST_FUNCTION_DEFINITION && caller->child_count > 0)
                pos = zb_param_pos(caller, arg->value);
            int bound = 0;
            if (pos >= 0) {
                char* ukey = heap_strf("%p|%s|%s", (void*)caller, arg->value, zp->params[q].sname);
                void* known = strmap_get(&untouched, ukey);
                if (known) {
                    bound = known == ZB_NO;
                } else {
                    if (!zb_bindings(gen, caller->children[caller->child_count - 1], caller,
                                     arg->value, zp->params[q].sname, 0, &bound))
                        bound = 1;
                    strmap_put(&untouched, ukey, bound ? ZB_NO : ZB_YES);
                }
                free(ukey);
            }
            if (pos >= 0 && !bound) {
                ZbParam* src = zb_param_find(zp, caller->value, pos);
                if (!src || strcmp(src->sname, zp->params[q].sname) != 0) {
                    zp->params[q].unknown = 1;
                    break;
                }
                if (src->feed_count == src->feed_cap) {
                    src->feed_cap = src->feed_cap ? src->feed_cap * 2 : 4;
                    src->feeds = aether_xrealloc(src->feeds, sizeof(int) * src->feed_cap);
                }
                src->feeds[src->feed_count++] = q;
            } else if (!zb_expr(gen, arg, site->ctx, zp->params[q].sname, 0)) {
                zp->params[q].unknown = 1;
                break;
            }
        }
    }

    /* Unknown spreads to every parameter it is passed on to. */
    int* work = aether_xrealloc(NULL, sizeof(int) * (zp->count + 1));
    int top = 0;
    for (int q = 0; q < zp->count; q++) {
        if (zp->params[q].unknown) work[top++] = q;
    }
    while (top > 0) {
        ZbParam* p = &zp->params[work[--top]];
        for (int f = 0; f < p->feed_count; f++) {
            ZbParam* t = &zp->params[p->feeds[f]];
            if (!t->unknown) {
                t->unknown = 1;
                work[top++] = p->feeds[f];
            }
        }
    }
    free(work);

    free(sc.sites);
    free(sc.notes);
    strmap_free(&sc.mentions);
    strmap_free(&sc.first);
    strmap_free(&untouched);
    /* Answers given while every parameter was unknown are dropped, except
     * which function holds a closure ("P|"), which no parameter changes. */
    StrMap kept;
    strmap_init(&kept);
    for (int i = 0; i < strmap_count(&gen->zeroed_box_memo); i++) {
        const char* k = strmap_key_at(&gen->zeroed_box_memo, i);
        if (k && k[0] == 'P' && k[1] == '|')
            strmap_put(&kept, k, strmap_value_at(&gen->zeroed_box_memo, i));
    }
    strmap_free(&gen->zeroed_box_memo);
    gen->zeroed_box_memo = kept;
    gen->zb_params_state = 2;
}

/* Does parameter `name` of `fn` hold only `sname` boxes on entry? */
static int zb_param_holds_boxes(CodeGenerator* gen, ASTNode* fn, const char* name,
                                const char* sname) {
    if (gen->zb_params_state == 0) zb_params_build(gen);
    if (gen->zb_params_state != 2 || !gen->zb_params) return 0;
    if (fn->type != AST_FUNCTION_DEFINITION || !fn->value) return 0;
    int k = zb_param_pos(fn, name);
    if (k < 0) return 0;
    ZbParam* p = zb_param_find((ZbParams*)gen->zb_params, fn->value, k);
    return p && !p->unknown && strcmp(p->sname, sname) == 0;
}

/* #1879: emit a NESTED-path field assignment (`o.inner.name = ...`).
 *
 * The identifier-only path below cannot serve this: it splices `obj->value`
 * into the generated C as a bare name, and here the object is itself a member
 * access. The result was once a plain store, so the string leaked while the
 * identical write spelled on the inner pointer released correctly.
 *
 * The object expression is bound to a temporary first: it is emitted more
 * than once (the old value's read and the store) and re-evaluating it would
 * run any side effects more than once.
 *
 * The previous value is released wherever the pointer to the inner struct
 * came from (#2369): every box Aether allocates starts zeroed (heap.new, and
 * malloc lowered to calloc), so the read is sound. */
/* Does the struct value `obj` live behind a pointer somewhere along its
 * path (`p.inner`, `items[0].inner` of a slice)? A string stored there can
 * be read by other threads, so it is published (#2919). */
static int value_path_through_pointer(ASTNode* obj) {
    while (obj && (obj->type == AST_MEMBER_ACCESS || obj->type == AST_ARRAY_ACCESS) &&
           obj->child_count > 0 && obj->children[0]) {
        Type* ht = obj->children[0]->node_type;
        if (ht && (ht->kind == TYPE_PTR || (obj->type == AST_ARRAY_ACCESS && type_is_slice(ht))))
            return 1;
        obj = obj->children[0];
    }
    return 0;
}

static int struct_array_local_owns(CodeGenerator* gen, const char* name);
static ASTNode* actor_state_decl(CodeGenerator* gen, const char* name);

/* #2688: does the array named `name` own a value per element: a local
 * array of structs that own strings or closures, or such an actor state
 * field? Its elements' trackers can then be read. */
static int struct_array_owns(CodeGenerator* gen, const char* name) {
    return struct_array_local_owns(gen, name) ||
           (is_actor_state_var(gen, name) &&
            state_struct_array_owned(gen, actor_state_decl(gen, name), NULL));
}

/* #2690: may the struct value `lv` be declared volatile: is it a local a
 * `try` body writes, or a part of one held by value (a field, an element of
 * a fixed-size array)? Through a pointer or a slice it is memory of its
 * own, never volatile. Named, not bound: a same-named local elsewhere
 * counts, which only costs the copy the volatile helpers make. */
int lvalue_may_be_volatile(CodeGenerator* gen, ASTNode* lv) {
    while (lv) {
        if (lv->type == AST_IDENTIFIER) return lv->value && is_try_clobbered_var(gen, lv->value);
        if (lv->child_count < 1 || !lv->children[0]) return 0;
        Type* ht = lv->children[0]->node_type;
        if (lv->type == AST_MEMBER_ACCESS && ht && ht->kind == TYPE_STRUCT) {
            lv = lv->children[0];
            continue;
        }
        if (lv->type == AST_ARRAY_ACCESS && is_sized_array_param(ht)) {
            lv = lv->children[0];
            continue;
        }
        return 0;
    }
    return 0;
}

/* The release or replace of an owning struct `sname` held where `volatile`
 * says: the generated `_volatile` helpers take a volatile pointer. */
const char* struct_destroy_fn(const char* sname, int is_volatile) {
    return cg_internf(is_volatile ? "_aether_vdrop_%s" : "_aether_drop_%s", sname);
}

const char* struct_replace_fn(const char* sname, int is_volatile) {
    return cg_internf(is_volatile ? "_aether_vstore_%s" : "_aether_store_%s", sname);
}

/* #2497: may the trackers of the value struct `obj` (a field held by value,
 * `o.inner`) be read to release the previous value? Its trackers are its
 * holder's: a local struct value's are (as for `v.name = ...`), and so are
 * those of one reached through a pointer, whose box starts zeroed (#2369).
 * An element of an array of such structs (`items[0]`, `o.items[0]`) is its
 * array's: a local array that owns its elements starts zeroed or
 * initialised (#2528), an actor's state starts zeroed (#2688), and a field
 * array is its holder's. */
static int value_path_trackers_are_initialised(CodeGenerator* gen, ASTNode* obj) {
    while (obj) {
        if (obj->type == AST_ARRAY_ACCESS && obj->child_count >= 2 && obj->children[0] &&
            is_sized_array_param(obj->children[0]->node_type)) {
            ASTNode* arr = obj->children[0];
            if (arr->type == AST_IDENTIFIER)
                return arr->value && struct_array_owns(gen, arr->value);
            obj = arr;
            continue;
        }
        if (obj->type != AST_MEMBER_ACCESS || obj->child_count != 1 || !obj->children[0] ||
            !obj->children[0]->node_type) break;
        ASTNode* holder = obj->children[0];
        if (holder->node_type->kind == TYPE_PTR) return 1;
        if (holder->node_type->kind != TYPE_STRUCT) return 0;
        obj = holder;
    }
    return obj && obj->type == AST_IDENTIFIER && obj->node_type &&
           obj->node_type->kind == TYPE_STRUCT;
}

static int emit_nested_field_heap_assign(CodeGenerator* gen, ASTNode* lhs,
                                         ASTNode* rhs, ASTNode* obj) {
    Type* obj_type = obj->node_type;
    if (!obj_type) return 0;
    /* A struct POINTER, or (#2497) a struct held by value in another one
     * (`o.inner.name = ...`). The latter used to be a plain store; replacing
     * or destroying `o` releases `o.inner`'s strings, so the field takes and
     * releases as any other place does. */
    int by_value = obj_type->kind == TYPE_STRUCT && obj_type->struct_name;
    const char* sname = by_value ? obj_type->struct_name
                        : (obj_type->kind == TYPE_PTR && obj_type->element_type &&
                           obj_type->element_type->kind == TYPE_STRUCT)
                          ? obj_type->element_type->struct_name : NULL;
    if (!sname) return 0;
    /* A header-defined struct's fields are the C header's
     * (emit_c_import_string_field_store). */
    if (aether_is_c_import_struct(sname)) return 0;
    if (!gen->program) return 0;

    ASTNode* sdef = find_struct_definition_by_name(gen->program, sname);
    if (!sdef) return 0;
    ASTNode* matching_field = NULL;
    for (int fi = 0; fi < sdef->child_count; fi++) {
        ASTNode* f = sdef->children[fi];
        if (f && f->type == AST_STRUCT_FIELD &&
            f->value && strcmp(f->value, lhs->value) == 0) {
            matching_field = f;
            break;
        }
    }
    if (!matching_field || !matching_field->node_type ||
        matching_field->node_type->kind != TYPE_STRING) return 0;
    if (!struct_owns_heap_strings(gen, sdef)) return 0;

    /* Unique per emission: these blocks nest (an argument to the RHS may
     * itself be a nested-path assignment), and a fixed name would shadow. */
    static int nested_tgt_seq = 0;
    char tgt[32];
    snprintf(tgt, sizeof(tgt), "_ae_ntgt%d", nested_tgt_seq++);

    int release_old = by_value ? value_path_trackers_are_initialised(gen, obj) : 1;
    /* A struct held by a local a `try` body writes is volatile, and so is
     * the pointer to it (#2690). */
    int vol = by_value && lvalue_may_be_volatile(gen, obj);
    print_indent(gen);
    fprintf(gen->output, "{ %s%s* %s = %s(", sname, vol ? " volatile" : "", tgt,
            by_value ? "&" : "");
    generate_expression(gen, obj);
    fprintf(gen->output, ");");
    if (release_old) {
        fprintf(gen->output, " const char* _tmp_old = %s->%s;", tgt, lhs->value);
    }
    /* R3: the field takes the value (moved when fresh or a local's last use,
     * else retained) and releases the one it held. */
    fprintf(gen->output, " %s->%s = ", tgt, lhs->value);
    emit_string_take_owned(gen, rhs, 0);
    fprintf(gen->output, ";");
    if (release_old) fprintf(gen->output, " aether_heap_str_free(_tmp_old);");
    if (!by_value || value_path_through_pointer(obj))
        fprintf(gen->output, " aether_string_publish(%s->%s);", tgt, lhs->value);
    fprintf(gen->output, " }\n");
    return 1;
}

/* A `string` field of a header-defined struct (`extern struct ... @c_import`)
 * is the header's `const char*`, read by C. An Aether string may be a wrapped
 * AetherString (interpolation, substring and the like build one), whose
 * header would reach C in place of the characters, so the store takes the
 * payload with aether_string_data, as a call to a C extern does. Any spelling
 * of the object: a value (`v.f`), a pointer (`p.f`), a nested path
 * (`t.inner.f`). C's field keeps what it is given, so it is given a
 * reference of its own (R4), which nothing in the program releases. */
static int emit_c_import_string_field_store(CodeGenerator* gen, ASTNode* lhs, ASTNode* rhs) {
    if (lhs->type != AST_MEMBER_ACCESS || lhs->child_count != 1 || !lhs->children[0]) return 0;
    if (!lhs->node_type || lhs->node_type->kind != TYPE_STRING) return 0;
    Type* ot = lhs->children[0]->node_type;
    const char* sname = NULL;
    if (ot && ot->kind == TYPE_STRUCT) sname = ot->struct_name;
    else if (ot && ot->kind == TYPE_PTR && ot->element_type &&
             ot->element_type->kind == TYPE_STRUCT) sname = ot->element_type->struct_name;
    if (!sname || !aether_is_c_import_struct(sname)) return 0;
    print_indent(gen);
    gen->generating_lvalue = 1;
    generate_expression(gen, lhs);
    gen->generating_lvalue = 0;
    /* NULL stays NULL: aether_string_data maps it to "", and a C field told
     * apart from an empty string by being NULL must stay so. */
    fprintf(gen->output, " = ");
    TempSeq s;
    seq_open(gen, &s);
    const char* cs = seq_let(gen, &s, "const void*", "_ae_cs");
    fprintf(gen->output, "(const void*)(");
    emit_string_take_owned(gen, rhs, 0);
    fprintf(gen->output, ")");
    seq_next(gen, &s);
    fprintf(gen->output, "%s ? aether_string_data(%s) : (const char*)0", cs, cs);
    seq_close(gen, &s);
    fprintf(gen->output, ";\n");
    return 1;
}

/* #2497: `o.inner = v` where `inner` is a struct held by value that owns
 * strings. The field is part of `o`, released with it, so it takes `v`
 * (emit_struct_take) and replaces what it held. Where the holder's trackers
 * cannot be read (a struct value that is not a local's) the old value is not
 * read: the store only takes `v`. */
static int emit_struct_valued_field_store(CodeGenerator* gen, ASTNode* lhs, ASTNode* rhs) {
    const char* fs = struct_owning_strings(gen, lhs->node_type);
    if (!fs) return 0;
    ASTNode* holder = lhs->children[0];
    Type* ht = holder->node_type;
    int trusted;
    if (ht && ht->kind == TYPE_STRUCT) {
        trusted = value_path_trackers_are_initialised(gen, lhs);
    } else if (ht && ht->kind == TYPE_PTR) {
        trusted = 1;
    } else {
        return 0;
    }
    print_indent(gen);
    if (trusted) {
        fprintf(gen->output, "%s(&(", struct_replace_fn(fs, lvalue_may_be_volatile(gen, lhs)));
        generate_expression(gen, lhs);
        fprintf(gen->output, "), ");
        emit_struct_take(gen, rhs, fs, NULL);
        fprintf(gen->output, ");\n");
    } else {
        generate_expression(gen, lhs);
        fprintf(gen->output, " = ");
        emit_struct_take(gen, rhs, fs, NULL);
        fprintf(gen->output, ";\n");
    }
    return 1;
}

/* #2525: `o.cb = v` where `cb` is a closure field. The field holds a
 * reference of its own: the store takes `v` (emit_closure_take) and gives
 * back the reference to the value it held, when the holder's fields can be
 * read (the same trust as a string field's tracker: a local struct value,
 * or any box, which starts zeroed, #2369). A header-defined struct's fields
 * are C's: a plain store. */
static int emit_closure_field_store(CodeGenerator* gen, ASTNode* lhs, ASTNode* rhs) {
    if (!lhs->node_type || lhs->node_type->kind != TYPE_FUNCTION || lhs->node_type->is_fnptr) return 0;
    ASTNode* holder = lhs->children[0];
    Type* ht = holder->node_type;
    const char* sname = NULL;
    int trusted;
    if (ht && ht->kind == TYPE_STRUCT) {
        sname = ht->struct_name;
        trusted = value_path_trackers_are_initialised(gen, lhs);
    } else if (ht && ht->kind == TYPE_PTR && ht->element_type &&
               ht->element_type->kind == TYPE_STRUCT) {
        sname = ht->element_type->struct_name;
        trusted = 1;
    } else {
        return 0;
    }
    if (!sname || aether_is_c_import_struct(sname)) return 0;
    print_indent(gen);
    fprintf(gen->output, "{ _AeClosure _ae_cv = ");
    emit_closure_take(gen, rhs);
    fprintf(gen->output, "; ");
    if (trusted) {
        fprintf(gen->output, "void* _ae_old = (");
        generate_expression(gen, lhs);
        fprintf(gen->output, ").env; ");
    }
    generate_expression(gen, lhs);
    fprintf(gen->output, " = _ae_cv;");
    if (trusted) fprintf(gen->output, " _aether_closure_env_release(_ae_old);");
    fprintf(gen->output, " }\n");
    return 1;
}

/* #2528: does the local array `name` own its elements (declared from a
 * literal of structs that own strings or closures, with a scope-exit
 * drop pending)? */
static int struct_array_local_owns(CodeGenerator* gen, const char* name) {
    for (int i = 0; i < gen->defer_count; i++) {
        ASTNode* d = gen->defer_stack[i];
        const char* a = d ? d->annotation : NULL;
        if (a && strncmp(a, "own_drop:", 9) == 0 && strcmp(a + 9, name) == 0) return 1;
    }
    return 0;
}

/* #2528: `v` as the value an owned `string[N]` element takes: a fresh heap
 * string is adopted, anything else (a literal, a local, a field) copied, so
 * every element is the array's own, as a string array cell's are (#2474). */
void emit_owned_string_element(CodeGenerator* gen, ASTNode* v) {
    int fresh = is_fresh_heap_string_temp(gen, v);
    if (fresh) {
        generate_expression(gen, v);
        return;
    }
    fprintf(gen->output, "aether_uniform_heap_str((const char*)(");
    generate_expression(gen, v);
    fprintf(gen->output, "), 0)");
}

void emit_owned_array_literal(CodeGenerator* gen, ASTNode* lit, int kind) {
    fprintf(gen->output, "{");
    for (int i = 0; i < lit->child_count; i++) {
        if (i > 0) fprintf(gen->output, ", ");
        if (kind == 1) emit_owned_string_element(gen, lit->children[i]);
        else emit_closure_take(gen, lit->children[i]);
    }
    fprintf(gen->output, "}");
}

/* #2688: a fixed-size array field bound from a whole array, in a struct's
 * literal (`Shelf { names: arr }`) or by a store (`s.names = arr`,
 * `s.names = [...]`). C neither initialises nor assigns an array from an
 * array, and these did not compile. The elements are copied one by one,
 * each taken as the field keeps it (array_field_elem_kind): a `string[N]`
 * field takes a reference to each element of another array
 * (_aether_str_elem_dup: a counted string shared, anything else copied), as
 * a parameter that owns its elements does (#2618), and a copy of each
 * element of a literal (emit_owned_string_element); a `fn[N]` field a
 * reference to each env; an array of structs that own strings a value of
 * its own per element. Anything else is copied as it is. */
enum { AF_PLAIN, AF_STRING, AF_CLOSURE, AF_STRUCT };

static ASTNode* struct_field_named(CodeGenerator* gen, const char* sname, const char* fname) {
    ASTNode* sdef = gen->program && sname ? find_struct_definition_by_name(gen->program, sname)
                                          : NULL;
    for (int i = 0; sdef && fname && i < sdef->child_count; i++) {
        ASTNode* f = sdef->children[i];
        if (f && f->type == AST_STRUCT_FIELD && f->value && strcmp(f->value, fname) == 0) return f;
    }
    return NULL;
}

/* How the field `fdef` of the struct `sname` keeps each element; the struct
 * name of an AF_STRUCT element goes to `*es`. A header-defined struct owns
 * nothing it holds. */
static int array_field_elem_kind(CodeGenerator* gen, const char* sname, ASTNode* fdef,
                                 const char** es) {
    if (!fdef || aether_is_c_import_struct(sname)) return AF_PLAIN;
    int oak = struct_field_owned_array(fdef, NULL);
    if (oak == 1) return AF_STRING;
    if (oak == 2) return AF_CLOSURE;
    int alen = 0;
    ASTNode* inner = owning_struct_field_def_n(gen, fdef, &alen);
    if (inner && alen > 0) {
        *es = inner->value;
        return AF_STRUCT;
    }
    return AF_PLAIN;
}

/* Fill the zeroed array `into` (a C lvalue) with the elements the field
 * takes from `src`: an array literal's values, or another array's
 * elements, read once through a pointer that accepts a volatile array (one
 * a `try` body writes, #2618). */
static void emit_array_field_take(CodeGenerator* gen, int kind, const char* es, Type* at,
                                  ASTNode* src, const char* into) {
    if (src && src->type == AST_SLICE_FROM_ARRAY && src->child_count > 0) src = src->children[0];
    if (src && src->type == AST_ARRAY_LITERAL) {
        for (int k = 0; k < src->child_count && k < at->array_size; k++) {
            ASTNode* v = src->children[k];
            fprintf(gen->output, " %s[%d] = ", into, k);
            if (kind == AF_STRING) emit_owned_string_element(gen, v);
            else if (kind == AF_CLOSURE) emit_closure_take(gen, v);
            else if (kind == AF_STRUCT) emit_struct_take(gen, v, es, NULL);
            else generate_expression(gen, v);
            fprintf(gen->output, ";");
        }
        return;
    }
    if (src && src->type == AST_FUNCTION_CALL) {
        /* #2894: a call's result, its elements handed over. */
        fprintf(gen->output, " ");
        emit_array_fill(gen, into, src, at);
        fprintf(gen->output, ";");
        return;
    }
    fprintf(gen->output, " { %s const volatile* _ae_src = (", get_c_type(at->element_type));
    generate_expression(gen, src);
    fprintf(gen->output, "); for (int _ae_k = 0; _ae_k < %d; _ae_k++) ", at->array_size);
    if (kind == AF_STRING) {
        fprintf(gen->output, "%s[_ae_k] = _aether_str_elem_dup(_ae_src[_ae_k]); }", into);
    } else if (kind == AF_CLOSURE) {
        fprintf(gen->output, "{ %s[_ae_k] = _ae_src[_ae_k]; _aether_closure_env_retain(%s[_ae_k].env); } }",
                into, into);
    } else if (kind == AF_STRUCT) {
        fprintf(gen->output, "%s[_ae_k] = _aether_copy_%s(_ae_src[_ae_k]); }", into, es);
    } else {
        fprintf(gen->output, "%s[_ae_k] = _ae_src[_ae_k]; }", into);
    }
}

/* A struct literal's field `fname` given the array `src` (not a literal):
 * it stays `{0}` in the literal, and this fills `<slit>.<fname>` once the
 * literal, held in the temporary `slit`, is built (codegen_expr.c). */
int struct_literal_field_takes_array(CodeGenerator* gen, const char* sname, ASTNode* field_init) {
    if (!field_init || field_init->child_count < 1 || !field_init->children[0]) return 0;
    ASTNode* v = field_init->children[0];
    if (v->type == AST_ARRAY_LITERAL || !is_sized_array_param(v->node_type)) return 0;
    ASTNode* fdef = struct_field_named(gen, sname, field_init->value);
    return fdef && is_sized_array_param(fdef->node_type);
}

void emit_struct_literal_array_field(CodeGenerator* gen, const char* sname, ASTNode* field_init,
                                     const char* slit) {
    ASTNode* fdef = struct_field_named(gen, sname, field_init->value);
    const char* es = NULL;
    int kind = array_field_elem_kind(gen, sname, fdef, &es);
    Type* at = fdef->node_type;
    ASTNode* src = field_init->children[0];
    if (src && src->type == AST_SLICE_FROM_ARRAY && src->child_count > 0) src = src->children[0];
    const char* into = cg_internf("%s.%s", slit, field_init->value);
    if (src && src->type == AST_FUNCTION_CALL) {
        emit_array_fill(gen, into, src, at);   /* #2894, #2886 */
        return;
    }
    /* #2765: an expression, a call per field (the loop is the helper's), as
     * emit_array_field_take fills it with statements. The source is read
     * through a pointer that accepts a volatile array. */
    if (kind == AF_STRING) fprintf(gen->output, "_aether_str_elems_take(%s, ", into);
    else if (kind == AF_CLOSURE) fprintf(gen->output, "_aether_closure_elems_take(%s, ", into);
    else if (kind == AF_STRUCT) fprintf(gen->output, "_aether_dup_each_%s(%s, ", es, into);
    else fprintf(gen->output, "(void)_aether_array_arg_copy(%s, ", into);
    fprintf(gen->output, "(");
    generate_expression(gen, src);
    if (kind == AF_PLAIN) fprintf(gen->output, "), sizeof(%s))", into);
    else fprintf(gen->output, "), %d)", at->array_size);
}

/* A whole-array store into an array that owns its elements as a field
 * does: every new element is taken into a temporary before any is stored,
 * so a source that reads the array (`o.f = o.f`, `o.f = [o.f[1], o.f[0]]`)
 * sees it from before the store; then, with `release`, each store gives
 * back the element it replaces. The caller prints the destination between
 * the two halves. */
static void emit_whole_array_store_begin(CodeGenerator* gen, int kind, const char* es, Type* at,
                                         ASTNode* rhs) {
    const char* elem = get_c_type(at->element_type);
    print_indent(gen);
    fprintf(gen->output, "{ %s _ae_farr[%d] = {0};", elem, at->array_size);
    emit_array_field_take(gen, kind, es, at, rhs, "_ae_farr");
    fprintf(gen->output, " %s volatile* _ae_fdst = (", elem);
}

static void emit_whole_array_store_end(CodeGenerator* gen, int kind, const char* es, Type* at,
                                       int release) {
    fprintf(gen->output, "); for (int _ae_k = 0; _ae_k < %d; _ae_k++) ", at->array_size);
    if (!release || kind == AF_PLAIN) {
        fprintf(gen->output, "_ae_fdst[_ae_k] = _ae_farr[_ae_k]; }\n");
    } else if (kind == AF_STRING) {
        fprintf(gen->output, "{ const char* _ae_old = _ae_fdst[_ae_k]; _ae_fdst[_ae_k] = _ae_farr[_ae_k]; "
                             "if (_ae_old) aether_heap_str_free(_ae_old); } }\n");
    } else if (kind == AF_CLOSURE) {
        fprintf(gen->output, "{ void* _ae_old = _ae_fdst[_ae_k].env; _ae_fdst[_ae_k] = _ae_farr[_ae_k]; "
                             "_aether_closure_env_release(_ae_old); } }\n");
    } else {
        /* The destination is reached through a volatile pointer, which
         * accepts a field of a volatile local (#2690) as well. */
        fprintf(gen->output, "_aether_vstore_%s(&_ae_fdst[_ae_k], _ae_farr[_ae_k]); }\n", es);
    }
}

/* #2688: `names = arr` / `names = [...]` into an actor's fixed-size array
 * state field `sdecl`. A `string[N]` / `fn[N]` one owns its elements
 * (state_array_owned, released by destroy_state), as a struct's does. */
static int emit_state_array_store(CodeGenerator* gen, ASTNode* sdecl, ASTNode* rhs) {
    if (!sdecl || !is_sized_array_param(sdecl->node_type) || !rhs) return 0;
    if (rhs->type != AST_ARRAY_LITERAL && rhs->type != AST_SLICE_FROM_ARRAY &&
        !is_sized_array_param(rhs->node_type)) return 0;
    int oak = state_array_owned(sdecl, NULL);
    /* An array of structs that own strings or closures owns a value per
     * element, as a struct's such field does: a `_dup` of each element
     * taken, each replaced one given back. Copied as plain values, the
     * state shared the strings its source then freed, and leaked the ones
     * it replaced. */
    const char* es = state_struct_array_owned(gen, sdecl, NULL);
    int kind = oak == 1 ? AF_STRING : oak == 2 ? AF_CLOSURE : es ? AF_STRUCT : AF_PLAIN;
    emit_whole_array_store_begin(gen, kind, es, sdecl->node_type, rhs);
    fprintf(gen->output, "self->%s", sdecl->value);
    emit_whole_array_store_end(gen, kind, es, sdecl->node_type, 1);
    return 1;
}

/* `o.f = src` into a fixed-size array field `f`, releasing what it replaces
 * where the holder's elements can be read (as for one element,
 * emit_struct_element_store). */
static int emit_array_field_store(CodeGenerator* gen, ASTNode* lhs, ASTNode* rhs) {
    if (!lhs || lhs->type != AST_MEMBER_ACCESS || !lhs->value || lhs->child_count != 1 ||
        !lhs->children[0] || !is_sized_array_param(lhs->node_type) || !rhs) return 0;
    if (rhs->type != AST_ARRAY_LITERAL && rhs->type != AST_SLICE_FROM_ARRAY &&
        !is_sized_array_param(rhs->node_type)) return 0;
    ASTNode* holder = lhs->children[0];
    Type* ht = holder->node_type;
    const char* sname = NULL;
    int trusted = 0;
    if (ht && ht->kind == TYPE_STRUCT) {
        sname = ht->struct_name;
        trusted = value_path_trackers_are_initialised(gen, lhs);
    } else if (ht && ht->kind == TYPE_PTR && ht->element_type &&
               ht->element_type->kind == TYPE_STRUCT) {
        sname = ht->element_type->struct_name;
        trusted = 1;
    }
    ASTNode* fdef = struct_field_named(gen, sname, lhs->value);
    if (!fdef || !is_sized_array_param(fdef->node_type)) return 0;
    const char* es = NULL;
    int kind = array_field_elem_kind(gen, sname, fdef, &es);
    emit_whole_array_store_begin(gen, kind, es, fdef->node_type, rhs);
    generate_expression(gen, lhs);
    emit_whole_array_store_end(gen, kind, es, fdef->node_type, trusted);
    return 1;
}

/* #2525, #2528: an element store into an array that owns its elements:
 * `o.slots[i] = v` on a fixed-size array field of structs that own strings
 * or closures, `o.names[i] = v` / `o.cbs[i] = v` on a `string[N]` / `fn[N]`
 * field, or `arr[i] = v` on a local array of such structs. The element is a
 * value of its own, released with its holder, so the store takes `v` and
 * replaces what the element held, as `o.inner = v` does
 * (emit_struct_valued_field_store). Only where the holder's slots can be
 * read (a local struct value or owning local array, a box). */
static int emit_struct_element_store(CodeGenerator* gen, ASTNode* lhs, ASTNode* rhs) {
    if (!lhs || lhs->type != AST_ARRAY_ACCESS || lhs->child_count < 2 || !lhs->children[0]) return 0;
    ASTNode* arr = lhs->children[0];
    if (!arr->node_type || arr->node_type->kind != TYPE_ARRAY || arr->node_type->array_size <= 0) return 0;
    Type* et = lhs->node_type ? lhs->node_type : arr->node_type->element_type;
    /* An element of an owning local array, whatever it holds: it takes `v`
     * and drops what it held, the index evaluated once. */
    if (arr->type == AST_IDENTIFIER && arr->value && struct_array_local_owns(gen, arr->value) &&
        own_type_owns(gen, et)) {
        print_indent(gen);
        fprintf(gen->output, "{ %s %s* _ae_ep = &(", get_c_type(et),
                lvalue_may_be_volatile(gen, lhs) ? "volatile" : "");
        generate_expression(gen, lhs);
        fprintf(gen->output, ");\n");
        print_indent(gen);
        emit_owning_store(gen, "(*_ae_ep)", et, rhs, 0);
        print_indent(gen);
        fprintf(gen->output, "}\n");
        return 1;
    }
    const char* es = struct_owning_strings(gen, et);
    int kind = es ? 3 : (et && et->kind == TYPE_STRING) ? 1
                      : (et && et->kind == TYPE_FUNCTION && !et->is_fnptr) ? 2 : 0;
    if (!kind) return 0;
    int trusted = 0;
    if (arr->type == AST_MEMBER_ACCESS) {
        ASTNode* holder = arr->children[0];
        Type* ht = holder ? holder->node_type : NULL;
        const char* sname = NULL;
        if (ht && ht->kind == TYPE_STRUCT) {
            sname = ht->struct_name;
            trusted = value_path_trackers_are_initialised(gen, arr);
        } else if (ht && ht->kind == TYPE_PTR && ht->element_type &&
                   ht->element_type->kind == TYPE_STRUCT) {
            sname = ht->element_type->struct_name;
            trusted = 1;
        }
        if (!sname || aether_is_c_import_struct(sname)) return 0;
        /* A `string[N]` / `fn[N]` field of a struct that does not own its
         * elements is a plain store. */
        if (kind != 3) {
            ASTNode* sdef = find_struct_definition_by_name(gen->program, sname);
            ASTNode* fdef = NULL;
            for (int i = 0; sdef && i < sdef->child_count; i++) {
                ASTNode* f = sdef->children[i];
                if (f && f->type == AST_STRUCT_FIELD && f->value && arr->value &&
                    strcmp(f->value, arr->value) == 0) { fdef = f; break; }
            }
            if (!fdef || struct_field_owned_array(fdef, NULL) != kind) return 0;
        }
    } else if (arr->type == AST_IDENTIFIER && arr->value && is_actor_state_var(gen, arr->value)) {
        /* #2528: a `string[N]` / `fn[N]` state field owns its elements
         * (destroy_state releases them), and so does one of structs that
         * own strings or closures (#2688); the store reads `self->f[i]`. */
        ASTNode* sd = actor_state_decl(gen, arr->value);
        int sk = state_struct_array_owned(gen, sd, NULL) ? 3 : state_array_owned(sd, NULL);
        if (sk != kind) return 0;
        trusted = 1;
    } else if (arr->type == AST_IDENTIFIER && arr->value && kind == 3) {
        if (!struct_array_local_owns(gen, arr->value)) return 0;
        trusted = 1;
    } else {
        return 0;
    }
    if (!trusted) return 0;
    print_indent(gen);
    if (kind == 3) {
        fprintf(gen->output, "%s(&(", struct_replace_fn(es, lvalue_may_be_volatile(gen, lhs)));
        generate_expression(gen, lhs);
        fprintf(gen->output, "), ");
        emit_struct_take(gen, rhs, es, NULL);
        fprintf(gen->output, ");\n");
    } else if (kind == 1) {
        fprintf(gen->output, "{ const char* _ae_old = ");
        generate_expression(gen, lhs);
        fprintf(gen->output, "; ");
        generate_expression(gen, lhs);
        fprintf(gen->output, " = ");
        emit_owned_string_element(gen, rhs);
        fprintf(gen->output, "; if (_ae_old) aether_heap_str_free(_ae_old); }\n");
    } else {
        fprintf(gen->output, "{ _AeClosure _ae_cv = ");
        emit_closure_take(gen, rhs);
        fprintf(gen->output, "; void* _ae_old = (");
        generate_expression(gen, lhs);
        fprintf(gen->output, ").env; ");
        generate_expression(gen, lhs);
        fprintf(gen->output, " = _ae_cv; _aether_closure_env_release(_ae_old); }\n");
    }
    return 1;
}

static int emit_slice_element_store(CodeGenerator* gen, ASTNode* lhs, ASTNode* rhs);
static int emit_composite_field_store(CodeGenerator* gen, ASTNode* lhs, ASTNode* rhs);

static int emit_struct_field_heap_assign(CodeGenerator* gen, ASTNode* lhs, ASTNode* rhs) {
    if (!gen || !lhs || !rhs) return 0;
    if (emit_slice_element_store(gen, lhs, rhs)) return 1;   /* #2921 */
    if (emit_composite_field_store(gen, lhs, rhs)) return 1;  /* #2921 */
    if (emit_struct_element_store(gen, lhs, rhs)) return 1;
    if (emit_array_field_store(gen, lhs, rhs)) return 1;   /* #2688 */
    if (lhs->type != AST_MEMBER_ACCESS || !lhs->value) return 0;
    if (lhs->child_count != 1 || !lhs->children[0]) return 0;
    if (emit_c_import_string_field_store(gen, lhs, rhs)) return 1;
    if (emit_struct_valued_field_store(gen, lhs, rhs)) return 1;
    if (emit_closure_field_store(gen, lhs, rhs)) return 1;
    /* #1879: a nested path (`o.inner.name`) has a MEMBER_ACCESS object rather
     * than a bare identifier. Handle it separately -- the code below splices
     * the object in as a name. So is an element of an array of structs
     * (`items[0].name`, `o.items[0].name`), whose store was a plain one: it
     * left the element's tracker as it was, so a literal stored over an
     * owned string was freed with the element, and a fresh string leaked. */
    if (lhs->children[0]->type == AST_MEMBER_ACCESS ||
        lhs->children[0]->type == AST_ARRAY_ACCESS) {
        return emit_nested_field_heap_assign(gen, lhs, rhs, lhs->children[0]);
    }
    ASTNode* obj = lhs->children[0];
    Type* obj_type = obj->node_type;
    if (!obj_type) return 0;
    /* Any other object (a cast `(p as *T).name`, a call) is a pointer the
     * store evaluates once (_ae_op). */
    int named = obj->type == AST_IDENTIFIER && obj->value;
    if (!named && obj_type->kind != TYPE_PTR) return 0;
    /* Accept a value struct (`v.field = ...`, accessor ".") and a heap-boxed
     * struct pointer from heap.new (`p.field = ...`, accessor "->"). The
     * pointer case (#790) lets a heap.new'd struct own its string fields: the
     * box takes the string exactly as a value struct does, so the generated
     * `_aether_heap_free_<Name>` / scope-exit destructor releases it. */
    const char* acc;
    const char* struct_name;
    if (obj_type->kind == TYPE_STRUCT && obj_type->struct_name) {
        acc = ".";
        struct_name = obj_type->struct_name;
    } else if (obj_type->kind == TYPE_PTR && obj_type->element_type &&
               obj_type->element_type->kind == TYPE_STRUCT &&
               obj_type->element_type->struct_name) {
        /* #1879: ANY struct pointer, not only a heap.new local or a pointer
         * parameter. A local ALIAS (`p = o.inner; p.name = ...`) is neither,
         * so it used to fall through to a bare store and leak -- the same
         * "ownership depends on how you spell it" defect as the nested path,
         * reached by binding the inner pointer to a name first. */
        /* #2369: and the store releases the value it replaces through any
         * of them. Reading the old value is sound on every box Aether
         * allocates: heap.new calloc's it, and a call of C's malloc is
         * lowered to calloc (call_is_c_malloc), so it is never garbage
         * (#1873). Before, only a pointer traced back to heap.new
         * released the old value; one from a call returning a list element
         * or a cast of a `ptr` parameter leaked it. */
        acc = "->";
        struct_name = obj_type->element_type->struct_name;
    } else {
        return 0;
    }
    /* `extern struct ... @c_import`: the layout is the C header's and owns
     * nothing; its string fields were stored above
     * (emit_c_import_string_field_store). */
    if (aether_is_c_import_struct(struct_name)) return 0;
    if (!gen->program) return 0;
    ASTNode* sdef = find_struct_definition_by_name(gen->program, struct_name);
    if (!sdef) return 0;
    ASTNode* matching_field = NULL;
    for (int fi = 0; fi < sdef->child_count; fi++) {
        ASTNode* f = sdef->children[fi];
        if (f && f->type == AST_STRUCT_FIELD &&
            f->value && strcmp(f->value, lhs->value) == 0) {
            matching_field = f;
            break;
        }
    }
    if (!matching_field || !matching_field->node_type ||
        matching_field->node_type->kind != TYPE_STRING) return 0;

    /* A variable a closure writes lives in a shared cell (#2458): the name
     * is the cell pointer, and the struct is `(*name)`, as every other use
     * of it spells it (the AST_IDENTIFIER emission). */
    /* An actor's state is a field of `self`, spelled as an identifier
     * expression spells it (`self->kept.name`); bare, the store named an
     * undeclared local. */
    const char* objs = !named ? "_ae_op"
                       : is_promoted_capture(gen, obj->value)
                       ? cg_internf("(*%s)", obj->value)
                       : is_actor_state_var(gen, obj->value)
                       ? cg_internf("%s->%s", gen->state_self_alias ? gen->state_self_alias : "self",
                                    obj->value)
                       : obj->value;
    print_indent(gen);
    /* R3: the field takes the value and releases the one it held; through a
     * pointer, other threads may read it, so it is published (#2919). */
    fprintf(gen->output, "{ ");
    if (!named) {
        fprintf(gen->output, "%s* _ae_op = (%s*)(", struct_name, struct_name);
        generate_expression(gen, obj);
        fprintf(gen->output, "); ");
    }
    fprintf(gen->output, "const char* _tmp_old = %s%s%s; %s%s%s = ",
            objs, acc, lhs->value, objs, acc, lhs->value);
    emit_string_take_owned(gen, rhs, 0);
    fprintf(gen->output, "; aether_heap_str_free(_tmp_old);");
    if (acc[0] == '-')
        fprintf(gen->output, " aether_string_publish(%s%s%s);", objs, acc, lhs->value);
    fprintf(gen->output, " }\n");
    return 1;
}

/* Is `call` a call of a C function (an extern), whose result it lends
 * rather than hands over (R4)? */
static int call_is_extern(CodeGenerator* gen, ASTNode* call) {
    if (!call || call->type != AST_FUNCTION_CALL || !call->value || !gen->program) return 0;
    const char* fn = codegen_normalise_callee(call->value);
    return !find_function_definition_by_name(gen->program, fn) &&
           find_extern_declaration_by_name(gen->program, fn) != NULL;
}

/* May evaluating `e` hold an owning value in a statement temporary (a
 * call, an array or a tuple literal inside it, not `e` itself at `top`),
 * whose block-exit drop a return must run after taking `e`? */
static int value_holds_temps(CodeGenerator* gen, ASTNode* e, int top) {
    if (!e || e->type == AST_CLOSURE) return 0;
    if (!top && (e->type == AST_ARRAY_LITERAL || e->type == AST_FUNCTION_CALL ||
                 e->type == AST_TUPLE_LITERAL) && own_type_owns(gen, e->node_type))
        return 1;
    for (int i = 0; i < e->child_count; i++)
        if (value_holds_temps(gen, e->children[i], 0)) return 1;
    return 0;
}

/* The array value `e` wrapped in the optional `t` of its array type: the
 * optional is a temporary its array is filled into (emit_array_fill), as C
 * initializes no array from an array value. */
static void emit_optional_array_wrap(CodeGenerator* gen, ASTNode* e, Type* t) {
    TempSeq s;
    seq_open(gen, &s);
    const char* ov = seq_let(gen, &s, get_c_type(t), "_ae_oa");
    fprintf(gen->output, "(%s){ .has = 1 }", get_c_type(t));
    seq_next(gen, &s);
    emit_array_fill(gen, cg_internf("%s.val", ov), e, t->element_type);
    seq_next(gen, &s);
    fprintf(gen->output, "%s", ov);
    seq_close(gen, &s);
}

/* #2921: `e` as the value a place of the owning composite type `t` takes:
 * a call's result is moved in, each string position the call only lends
 * taken (an extern's borrowed one); anything else is copied, every part
 * retained. */
void emit_composite_take(CodeGenerator* gen, ASTNode* e, Type* t) {
    if (t->kind == TYPE_FUNCTION) {
        emit_closure_take(gen, e);
        return;
    }
    if (t->kind == TYPE_PTR) {
        emit_seq_take(gen, e);
        return;
    }
    /* A bare value or `none` into an optional is wrapped, its value taken. */
    if (t->kind == TYPE_OPTIONAL && needs_optional_coerce(e, t)) {
        if (e->type == AST_NONE_LITERAL) {
            fprintf(gen->output, "(%s){0}", get_c_type(t));
            return;
        }
        if (type_is_sized_array(t->element_type)) {
            emit_optional_array_wrap(gen, e, t);
            return;
        }
        fprintf(gen->output, "(%s){ .has = 1, .val = ", get_c_type(t));
        emit_value_take(gen, e, t->element_type);
        fprintf(gen->output, " }");
        return;
    }
    /* A tuple literal is a fresh value, its compound literal with each
     * position taken. */
    if (e->type == AST_TUPLE_LITERAL && t->kind == TYPE_TUPLE) {
        fprintf(gen->output, "(%s){ ", get_c_type(t));
        for (int i = 0; i < e->child_count && i < t->tuple_count; i++) {
            fprintf(gen->output, "%s", i ? ", " : "");
            emit_value_take(gen, e->children[i], t->tuple_types[i]);
        }
        fprintf(gen->output, " }");
        return;
    }
    /* A call hands its result over (R3), C's lending it: an Aether
     * function's or a closure's is moved in as it is. */
    if (e->type == AST_FUNCTION_CALL && t->kind != TYPE_TUPLE && !call_is_extern(gen, e)) {
        generate_expression(gen, e);
        return;
    }
    TempSeq s;
    seq_open(gen, &s);
    const char* tv = seq_let(gen, &s, get_c_type(t), "_ae_ct");
    generate_expression(gen, e);
    if (e->type == AST_FUNCTION_CALL && t->kind == TYPE_TUPLE) {
        for (int j = 0; j < t->tuple_count; j++) {
            if (!t->tuple_types[j] || t->tuple_types[j]->kind != TYPE_STRING ||
                tuple_call_returns_heap_at(gen, e, j)) continue;
            seq_next(gen, &s);
            fprintf(gen->output, "%s._%d = aether_uniform_heap_str(%s._%d, 0)", tv, j, tv, j);
        }
    } else {
        seq_next(gen, &s);
        fprintf(gen->output, "%s(&%s)", own_fn(gen, t, "retain"), tv);
    }
    seq_next(gen, &s);
    fprintf(gen->output, "%s", tv);
    seq_close(gen, &s);
}

/* `e` as the value a place of owning type `t` takes (R3): a string, a
 * closure, a struct or a composite, each by its own take. */
void emit_value_take(CodeGenerator* gen, ASTNode* e, Type* t) {
    const char* sname = struct_owning_strings(gen, t);
    if (t && t->kind == TYPE_STRING) emit_string_take_owned(gen, e, 0);
    else if (t && t->kind == TYPE_FUNCTION && !t->is_fnptr) emit_closure_take(gen, e);
    else if (sname) emit_struct_take(gen, e, sname, NULL);
    else if (own_type_owns(gen, t)) emit_composite_take(gen, e, t);
    else generate_expression(gen, e);
}

/* #2894: the array lvalue `dst`, of type `at`, filled with the array value
 * `e` as a place of its own takes it: an array literal's elements each
 * taken, a call's result (returned in its struct, c_result_type) moved out,
 * any other array (a local, a parameter, a field, an element) copied with a
 * reference taken to each owning element. One expression. */
void emit_array_fill(CodeGenerator* gen, const char* dst, ASTNode* e, Type* at) {
    if (e->type == AST_ARRAY_LITERAL) {
        fprintf(gen->output, "(");
        for (int i = 0; i < e->child_count; i++) {
            fprintf(gen->output, "%s%s[%d] = ", i ? ", " : "", dst, i);
            emit_value_take(gen, e->children[i], at->element_type);
        }
        fprintf(gen->output, "%s)", e->child_count ? "" : "(void)0");
        return;
    }
    TempSeq s;
    seq_open(gen, &s);
    if (e->type == AST_FUNCTION_CALL) {
        const char* w = seq_let(gen, &s, c_result_type(at), "_ae_ar");
        gen->array_call_raw++;
        generate_expression(gen, e);
        gen->array_call_raw--;
        seq_next(gen, &s);
        fprintf(gen->output, "(void)memcpy((void*)%s, (const void*)%s.a, sizeof(%s.a))", dst, w, w);
    } else {
        fprintf(gen->output, "(void)memcpy((void*)%s, (const void*)(", dst);
        generate_expression(gen, e);
        fprintf(gen->output, "), sizeof(%s[0]) * %d)", dst, at->array_size);
        if (own_type_owns(gen, at)) {
            seq_next(gen, &s);
            fprintf(gen->output, "%s((%s*)%s)", own_fn(gen, at, "retain"),
                    get_c_type(at->element_type), dst);
        }
    }
    seq_close(gen, &s);
}

/* #2894: the array lvalue `dst` filled from the array lvalue `src` of type
 * `at`: its elements moved (`retain` 0, `src` gives them up) or each owning
 * one retained. One expression. */
void emit_array_fill_c(CodeGenerator* gen, const char* dst, const char* src, Type* at, int retain) {
    fprintf(gen->output, "((void)memcpy((void*)%s, (const void*)%s, sizeof(%s[0]) * %d)",
            dst, src, dst, at->array_size);
    if (retain && own_type_owns(gen, at))
        fprintf(gen->output, ", %s((%s*)%s)", own_fn(gen, at, "retain"),
                get_c_type(at->element_type), dst);
    fprintf(gen->output, ")");
}

/* #2894: the array value `e` as the result of a function returning the
 * array type `at`: a call's result passes through, anything else is put in
 * the result's struct (emit_array_fill). */
void emit_array_result_value(CodeGenerator* gen, ASTNode* e, Type* at) {
    if (e->type == AST_FUNCTION_CALL) {
        gen->array_call_raw++;
        generate_expression(gen, e);
        gen->array_call_raw--;
        return;
    }
    TempSeq s;
    seq_open(gen, &s);
    const char* r = seq_let(gen, &s, c_result_type(at), "_ae_rr");
    fprintf(gen->output, "(%s){0}", c_result_type(at));
    seq_next(gen, &s);
    emit_array_fill(gen, cg_internf("%s.a", r), e, at);
    seq_next(gen, &s);
    fprintf(gen->output, "%s", r);
    seq_close(gen, &s);
}

/* #2921: `name = <array>` for a fixed-size array local or parameter of
 * owning elements, declared already: the new value is filled into an array
 * of its own (emit_array_fill), the old one dropped, the new one copied in,
 * so a value that reads the array reads it whole. */
void emit_owning_array_store(CodeGenerator* gen, const char* name, Type* at, ASTNode* rhs) {
    const char* et = get_c_type(at->element_type);
    fprintf(gen->output, "{ %s _ae_new[%d] = {0}; ", et, at->array_size);
    emit_array_fill(gen, "_ae_new", rhs, at);
    fprintf(gen->output, "; %s((%s*)%s); memcpy((void*)%s, (const void*)_ae_new, sizeof(_ae_new)); }\n",
            own_fn(gen, at, "drop"), et, name, name);
}

/* #2921: R3's store of `rhs` into the place `lv` of the owning type `t`
 * (anything but a fixed-size array): the new value is taken, published when
 * other threads may read the place (#2919), and the value the place held is
 * dropped once the new one is in. */
void emit_owning_store(CodeGenerator* gen, const char* lv, Type* t, ASTNode* rhs, int publish) {
    fprintf(gen->output, "{ %s _ae_old = %s; %s = ", get_c_type(t), lv, lv);
    emit_value_take(gen, rhs, t);
    fprintf(gen->output, ";\n");
    if (publish) own_emit_part(gen, t, lv, OWN_PUBLISH);
    own_emit_part(gen, t, "_ae_old", OWN_DROP);
    print_indent(gen);
    fprintf(gen->output, "}\n");
}

/* #2921: `o.f = v` for a field of an owning tuple, optional or string
 * sequence type: the field takes `v` and drops what it held; through a
 * pointer, other threads may read it, so what it takes is published
 * (#2919). */
static int emit_composite_field_store(CodeGenerator* gen, ASTNode* lhs, ASTNode* rhs) {
    if (!lhs || lhs->type != AST_MEMBER_ACCESS || lhs->child_count != 1 || !lhs->children[0])
        return 0;
    Type* ft = lhs->node_type;
    Type* ot = lhs->children[0]->node_type;
    if (!ft || (ft->kind != TYPE_TUPLE && ft->kind != TYPE_OPTIONAL && !is_string_seq_ptr_type(ft)) ||
        !own_type_owns(gen, ft) || !ot || (ot->kind != TYPE_STRUCT && ot->kind != TYPE_PTR))
        return 0;
    const char* sname = ot->kind == TYPE_STRUCT ? ot->struct_name
                        : ot->element_type && ot->element_type->kind == TYPE_STRUCT
                        ? ot->element_type->struct_name : NULL;
    if (!sname || aether_is_c_import_struct(sname)) return 0;
    print_indent(gen);
    fprintf(gen->output, "{ %s* _ae_fp = &(", get_c_type(ft));
    generate_expression(gen, lhs);
    fprintf(gen->output, ");\n");
    print_indent(gen);
    emit_owning_store(gen, "(*_ae_fp)", ft, rhs, ot->kind == TYPE_PTR);
    print_indent(gen);
    fprintf(gen->output, "}\n");
    return 1;
}

/* #2921: `p[i] = v` through a slice: the element is a place of the array the
 * slice views, which owns it, so the store takes `v` and drops what the
 * element held. Returns 0 for any other store. */
static int emit_slice_element_store(CodeGenerator* gen, ASTNode* lhs, ASTNode* rhs) {
    if (!lhs || lhs->type != AST_ARRAY_ACCESS || lhs->child_count < 2 || !lhs->children[0])
        return 0;
    Type* at = lhs->children[0]->node_type;
    if (!at || at->kind != TYPE_ARRAY || at->array_size > 0 || !at->element_type ||
        at->element_type->kind == TYPE_ARRAY || !own_type_owns(gen, at->element_type))
        return 0;
    /* A view of C memory (`p as string[]`, an extern's slice) owns nothing
     * (R4): a string stored into it gives C its characters, and the program
     * keeps the string alive while C reads them (the checker refuses one
     * built in the statement, which nothing would keep, E0902). */
    if (lhs->children[0]->type == AST_PTR_AS_ARRAY_CAST ||
        expr_is_c_view_of_slice(gen, lhs->children[0])) {
        if (at->element_type->kind != TYPE_STRING) return 0;
        print_indent(gen);
        generate_expression(gen, lhs);
        fprintf(gen->output, " = aether_string_data(");
        generate_expression(gen, rhs);
        fprintf(gen->output, ");\n");
        return 1;
    }
    const char* ct = get_c_type(at->element_type);
    print_indent(gen);
    fprintf(gen->output, "{ %s* _ae_ep = &(", ct);
    generate_expression(gen, lhs);
    fprintf(gen->output, ");\n");
    print_indent(gen);
    emit_owning_store(gen, "(*_ae_ep)", at->element_type, rhs, 0);
    print_indent(gen);
    fprintf(gen->output, "}\n");
    return 1;
}

/* An owning local a binding handles itself: a tuple, an optional or a
 * closure (#2921; a string and a struct have paths of their own). */
static int composite_local_type(CodeGenerator* gen, Type* t) {
    return t && (t->kind == TYPE_TUPLE || t->kind == TYPE_OPTIONAL || t->kind == TYPE_FUNCTION ||
                 t->kind == TYPE_PTR) &&
           own_type_owns(gen, t);
}

/* #2921: a binding of a local of an owning tuple or optional type. The local owns its
 * parts: a binding takes the value (emit_composite_take) and drops what the
 * local held after the new value is in; every exit of its scope drops what
 * it holds, and a panic does through the journal (#2923). Returns 0 for
 * any other binding. */
static int emit_composite_local_binding(CodeGenerator* gen, ASTNode* stmt) {
    Type* t = stmt->node_type;
    const char* name = stmt->value;
    if (!composite_local_type(gen, t) || !name || strcmp(name, "_") == 0 ||
        stmt->child_count == 0 || !stmt->children[0] ||
        is_promoted_capture(gen, name) || is_module_global_var(gen, name)) return 0;
    ASTNode* rhs = stmt->children[0];
    /* A match is bound arm by arm (the match path, which stores each arm's
     * value as emit_owning_store does): the local is only declared here. */
    if (rhs->type == AST_MATCH_STATEMENT) {
        if (!is_var_declared(gen, name) && !is_actor_state_var(gen, name)) {
            print_indent(gen);
            fprintf(gen->output, "%s %s = {0};\n",
                    volatile_decl_type(try_volatile_qual_for(gen, name), get_c_type(t)), name);
            mark_var_declared_typed(gen, name, t);
            push_own_drop(gen, name, t, stmt->line, stmt->column);
        }
        return 0;
    }
    /* An actor's state field: a store into the actor's place, which its
     * destroy_state drops. */
    if (is_actor_state_var(gen, name)) {
        print_indent(gen);
        emit_owning_store(gen, cg_internf("%s->%s", gen->state_self_alias ? gen->state_self_alias
                                                                          : "self", name),
                          t, rhs, 0);
        return 1;
    }
    const char* ct = get_c_type(t);
    print_indent(gen);
    if (is_var_declared(gen, name)) {
        fprintf(gen->output, "{ %s _ae_old = %s; %s = ", ct, name, name);
        emit_composite_take(gen, rhs, t);
        fprintf(gen->output, ";\n");
        own_emit_part(gen, t, "_ae_old", OWN_DROP);
        print_indent(gen);
        fprintf(gen->output, "}\n");
        return 1;
    }
    fprintf(gen->output, "%s %s = ", volatile_decl_type(try_volatile_qual_for(gen, name), ct),
            name);
    emit_composite_take(gen, rhs, t);
    fprintf(gen->output, ";\n");
    mark_var_declared_typed(gen, name, t);
    push_own_drop(gen, name, t, stmt->line, stmt->column);
    return 1;
}

/* R3: a binding of an owning tuple, optional, closure or string sequence
 * type that borrows its value (a parameter, a pattern's binding) and that
 * `body` rebinds takes a reference of its own first, which its scope exit
 * drops, so the rebind releases nothing of the value's owner. */
void own_rebound_binding_take(CodeGenerator* gen, const char* name, Type* t, ASTNode* body,
                              int line, int column) {
    if (!composite_local_type(gen, t) || !struct_param_written(body, name)) return;
    own_emit_part(gen, t, name, OWN_RETAIN);
    push_own_drop(gen, name, t, line, column);
}

/* #2921: the local `name` of the owning composite type `t` (a tuple, an
 * optional, a fixed-size array) is journaled where it is declared, and every
 * exit of the scope drops what it holds (try_emit_own_drop). */
void push_own_drop(CodeGenerator* gen, const char* name, Type* t, int line, int column) {
    emit_unwind_track_place(gen, cg_internf("%s%s", t->kind == TYPE_ARRAY ? "" : "&", name),
                            own_unwind_fn(gen, t));
    push_own_drop_carrier(gen, name, t, line, column);
}

/* The scope-exit drop of push_own_drop alone, for a place whose journal
 * entry is written where it is set. */
void push_own_drop_carrier(CodeGenerator* gen, const char* name, Type* t, int line, int column) {
    ASTNode* carrier = create_ast_node(AST_EXPRESSION_STATEMENT, NULL, line, column);
    if (carrier->annotation) free(carrier->annotation);
    carrier->annotation = heap_strf("own_drop:%s", name);
    carrier->node_type = clone_type(t);
    codegen_own_node(gen, carrier);
    push_defer(gen, carrier);
}

/* Struct-reassignment helper (#465). For `<var> = <value>` where `<var>`
 * is a struct local with heap-string fields, emit
 * `_aether_store_<Struct>(&<var>, <value>);`: the previous struct's owned strings
 * are reclaimed, except one the new value takes over (`r = Rec { name:
 * r.name }`), which moves to it. Returns 1 if it emitted the assignment;
 * 0 leaves the caller to emit the bare one. */
static int emit_struct_replace_assign(CodeGenerator* gen, const char* var_name,
                                      Type* var_type, ASTNode* value) {
    if (!gen || !var_name || !var_type || !value) return 0;
    if (var_type->kind != TYPE_STRUCT || !var_type->struct_name) return 0;
    if (!gen->program) return 0;
    ASTNode* sdef = find_struct_definition_by_name(gen->program, var_type->struct_name);
    if (!sdef || !struct_owns_heap_strings(gen, sdef)) return 0;
    fprintf(gen->output, "%s(&%s, ",
            struct_replace_fn(var_type->struct_name, is_try_clobbered_var(gen, var_name)),
            var_name);
    /* #2497: a struct another owner keeps is copied or moved in. */
    emit_struct_take(gen, value, var_type->struct_name, var_name);
    fprintf(gen->output, ");\n");
    return 1;
}

/* Should the return statement at this site route its value through
 * the uniform-heap shim? True when: the enclosing function is not
 * main, the return is single-value, the function is classifier-
 * classified heap-returning, and the return expression's static type
 * is `string` (TYPE_STRING) or implied to be so by the function's
 * declared return type. The shim only makes sense for string returns
 * — wrapping a non-string would emit a type-mismatched call. */
static int should_uniform_heap_return(CodeGenerator* gen, ASTNode* stmt) {
    if (!gen || !stmt) return 0;
    if (gen->in_main_function) return 0;
    if (stmt->child_count != 1) return 0;
    ASTNode* ret = stmt->children[0];
    if (!ret) return 0;
    /* Skip the wrap for AST_PRINT_STATEMENT-as-return — that path
     * never propagates a value to the caller, only side-effects. */
    if (ret->type == AST_PRINT_STATEMENT) return 0;
    /* A string-returning closure hands every result over owned (#2054):
     * its caller reaches it through a value and cannot ask which return
     * sites are heap the way it can for a named function. */
    if (gen->in_string_closure) return 1;
    if (!gen->current_function) return 0;
    if (!function_def_returns_heap_string(gen, gen->current_function)) return 0;
    /* Static type gate. Prefer the function's declared return type
     * (the C compiler's actual constraint). Fall back to the
     * expression's stamped type if the function lacks a typed
     * return slot. */
    Type* ret_type = gen->current_func_return_type
        ? gen->current_func_return_type
        : ret->node_type;
    if (!ret_type || ret_type->kind != TYPE_STRING) return 0;
    return 1;
}

/* Emit a return expression as a take into the caller's place (R3): the
 * caller receives one reference whichever branch produced the value.
 *
 * Returns 1 if the take was emitted, 0 if the expression should be
 * emitted raw (the caller is responsible for raw emission when this
 * helper declines, e.g. for tuple / void returns).
 *
 * Invariant: only call when the enclosing function is classifier-
 * classified heap-returning. */
/* #2586: is the function being emitted one used as a typed fn-pointer
 * value, returning a single string? Its owned returns are then marked for a
 * call through a pointer (aether_fnptr_give). Not a closure's body, nor a
 * tuple position. */
int emitting_marked_fn_value(CodeGenerator* gen) {
    ASTNode* fn = gen ? gen->current_function : NULL;
    return fn && (fn->type == AST_FUNCTION_DEFINITION) && !gen->in_string_closure &&
           gen->current_func_return_type &&
           gen->current_func_return_type->kind == TYPE_STRING &&
           fn_value_name(fn->value);
}

static int emit_uniform_heap_return_expr(CodeGenerator* gen, ASTNode* expr) {
    if (!expr) return 0;
    /* Tuple returns flow through their own per-position channel
     * (Type.tuple_heap_flags + the AST_TUPLE_DESTRUCTURE handler).
     * Multi-value returns reach the caller's RHS site element by
     * element, not as a single pointer, so the uniform-heap shim
     * is the wrong shape there — the caller is responsible. */
    int give = emitting_marked_fn_value(gen);
    if (give) fprintf(gen->output, "aether_fnptr_give(");
    if (expr->type == AST_IF_EXPRESSION && string_take_is_view(gen, expr)) {
        /* #2461: the arm that runs is taken as a binding would take it (a
         * local moved out of this scope, a field read copied, a fresh
         * value adopted); the shim copies only what is still borrowed. */
        emit_string_take_owned(gen, expr, 0);
        if (give) fprintf(gen->output, ")");
        return 1;
    }
    /* #2815: `o ?? d` over a `string?` local, `d` a fresh string: the
     * local's string is copied (the local frees its own at scope exit)
     * and the default, when it is the one taken, adopted as it is. The
     * plain path below copies the whole value, which would leak a fresh
     * default. */
    if (expr->type == AST_NULL_COALESCE && expr->child_count >= 2 && expr->children[1] &&
        expr->children[1]->type != AST_IDENTIFIER &&
        is_heap_string_expr(gen, expr->children[1]) &&
        opt_string_local_unwrap(gen, expr, NULL)) {
        /* #2885: a sequence of temporaries (seq_open), declared ahead of
         * the return, as every expression temporary is on the plain C
         * path; spelled as a statement expression, it was GNU C there. */
        TempSeq rs;
        seq_open(gen, &rs);
        const char* ro = seq_let(gen, &rs, "ae_opt_string", "_ro");
        generate_expression(gen, expr->children[0]);
        seq_next(gen, &rs);
        fprintf(gen->output, "%s.has ? aether_uniform_heap_str(%s.val, 0) : (const char*)(", ro, ro);
        generate_expression(gen, expr->children[1]);
        fprintf(gen->output, ")");
        seq_close(gen, &rs);
        if (give) fprintf(gen->output, ")");
        return 1;
    }
    /* R3: a return is a store into the caller's place: a local on its
     * last use moves out (its exit free then releases nothing), anything
     * else is taken. */
    emit_string_take_owned(gen, expr, 0);
    if (give) fprintf(gen->output, ")");
    return 1;
}

/* R3: a `string` position of an Aether function's tuple result is handed
 * over from every return; the type it is emitted with says so. */
int function_def_returns_heap_at(CodeGenerator* gen, ASTNode* fn_def,
                                  int position) {
    if (!fn_def || position < 0 || (fn_def->type != AST_FUNCTION_DEFINITION &&
                                    fn_def->type != AST_BUILDER_FUNCTION)) return 0;
    Type* t = fn_result_type(gen, fn_def);
    return t && t->kind == TYPE_TUPLE && position < t->tuple_count && t->tuple_types &&
           t->tuple_types[position] && t->tuple_types[position]->kind == TYPE_STRING;
}

/* Is tuple `position` of `fallible`'s result classified heap by
 * `function_def_returns_heap_at`? Shared by the `or` lowering's
 * error-slot free (position = last) and its value-slot uniform-heap
 * boxing (position 0). True only when the fallible is a call to a
 * resolvable user function — for such a function every return at that
 * position hands over one reference (emit_tuple_return_position).
 * Callees with no resolvable definition (externs, unknowns) are
 * conservatively non-heap, matching the `v, e = f()` destructure
 * default. */
static int or_fallible_slot_is_heap(CodeGenerator* gen, ASTNode* fallible,
                                    int position) {
    if (!fallible || fallible->type != AST_FUNCTION_CALL || !fallible->value ||
        !gen || !gen->program) {
        return 0;
    }
    Type* tup = fallible->node_type;
    if (!tup || tup->kind != TYPE_TUPLE || tup->tuple_count < 2) return 0;
    if (position < 0 || position >= tup->tuple_count) return 0;
    if (tuple_call_pos_copied(gen, fallible, position)) return 1;   /* #2619 */
    const char* fn = codegen_normalise_callee(fallible->value);
    ASTNode* callee = find_function_definition_by_name(gen->program, fn);
    if (!callee) return 0;
    return function_def_returns_heap_at(gen, callee, position);
}

/* Is the error (last) slot of `fallible` a heap-owned string that the
 * `or` lowering must release when it discards it? When true, every return
 * (including the `""` success sentinel) hands the caller one reference
 * (emit_tuple_return_position); when false (e.g. `string.to_long`
 * returning the raw literal "invalid long") the slot is not the caller's
 * and must NOT be freed. Same gate the `v, e = f()` destructure site uses
 * for `e`, keeping the two forms consistent. */
int or_fallible_error_slot_is_heap(CodeGenerator* gen, ASTNode* fallible) {
    Type* tup = fallible ? fallible->node_type : NULL;
    if (!tup || tup->kind != TYPE_TUPLE || tup->tuple_count < 2) return 0;
    return or_fallible_slot_is_heap(gen, fallible, tup->tuple_count - 1);
}

/* Is the VALUE (position 0) slot of `fallible` a heap-owned string? Used
 * by the `or` lowering to decide uniform-heap boxing of the result: when
 * the success value is heap, the `or`-expression is classified heap (the
 * caller owns it) and the handler's value is taken via
 * `aether_uniform_heap_str` so BOTH paths hand over one reference. */
int or_fallible_value_slot_is_heap(CodeGenerator* gen, ASTNode* fallible) {
    return or_fallible_slot_is_heap(gen, fallible, 0);
}

/* Emit one position of a multi-value `return e0, e1, ...`. When the
 * enclosing function classifies tuple position `j` as a heap-string
 * position (`function_def_returns_heap_at`), route that position's
 * value through `aether_uniform_heap_str` so EVERY return path hands
 * the caller a malloc-owned pointer it can free uniformly: a heap
 * value passes through (fast path), a literal / borrowed value is
 * malloc-duplicated. Without this, a tuple-returning function with
 * a mixed string position — heap on some return sites, a borrowed
 * literal on others, the zlib/cryptography/lzf decode shape
 * (`return owned, n, ""` vs `return "", 0, "err"`) — either leaked
 * the heap branch (caller told not to free) or, post-OR-fold, would
 * free a string literal on the error branch. The wrap closes both.
 * Non-heap positions and non-string positions emit raw.
 * See string-new-with-length-heap-annotation.md. */
/* #2501: is `call` a `call(f, ...)` whose `f` is known to be one closure
 * literal of this program (codegen's closure_var_map), whose tuple string
 * slots are handed over owned? A function value or an unknown closure may
 * return borrowed slots, which stay the caller's to leave alone. */
static int call_targets_closure_literal(CodeGenerator* gen, ASTNode* call) {
    if (!call || call->type != AST_FUNCTION_CALL || !call->value ||
        strcmp(call->value, "call") != 0 || call->child_count < 1) return 0;
    ASTNode* f = call->children[0];
    if (!f) return 0;
    if (f->type == AST_CLOSURE) return 1;
    if (f->type != AST_IDENTIFIER || !f->value) return 0;
    return closure_var_id(gen, gen->closure_var_scope, f->value) >= 0;
}

static int is_opt_string_type(Type* t);
static void emit_opt_string_return(CodeGenerator* gen, ASTNode* v, Type* rt);

/* #2751: does the function or closure being emitted hand every `string?`
 * position of its tuple result over owned? A named function and a closure
 * do (emit_tuple_return_position), as a named function's single `string?`
 * result is (#2740); main returns no tuple. */
static int emitting_owned_opt_string_tuple(CodeGenerator* gen) {
    return gen && gen->current_function && !gen->in_main_function &&
           (gen->current_function->type == AST_FUNCTION_DEFINITION ||
            gen->current_function->type == AST_CLOSURE);
}

/* #2894: a tuple position of a fixed-size array type: the compound literal
 * holds it as `{0}` and emit_tuple_array_fills fills it after, C having no
 * array initialised from an array. */
static Type* tuple_return_array_at(CodeGenerator* gen, int j) {
    Type* rt = gen->current_func_return_type;
    if (!rt || rt->kind != TYPE_TUPLE || j < 0 || j >= rt->tuple_count) return NULL;
    return is_sized_array_param(rt->tuple_types[j]) ? rt->tuple_types[j] : NULL;
}

static int tuple_return_has_array(CodeGenerator* gen, ASTNode* stmt) {
    for (int j = 0; j < stmt->child_count; j++)
        if (tuple_return_array_at(gen, j)) return 1;
    return 0;
}

/* The array positions of the tuple `var` a `return` built. */
static void emit_tuple_array_fills(CodeGenerator* gen, ASTNode* stmt, const char* var) {
    for (int j = 0; j < stmt->child_count; j++) {
        Type* at = tuple_return_array_at(gen, j);
        if (!at) continue;
        print_indent(gen);
        emit_array_fill(gen, cg_internf("%s._%d", var, j), stmt->children[j], at);
        fprintf(gen->output, ";\n");
    }
}

static void emit_tuple_return_position(CodeGenerator* gen, ASTNode* expr,
                                       int j) {
    Type* rt0 = gen ? gen->current_func_return_type : NULL;
    if (tuple_return_array_at(gen, j)) {
        fprintf(gen->output, "{0}");   /* #2894: filled after */
        return;
    }
    /* #2751: a `string?` slot hands over a string of its own, as a single
     * `string?` result does (#2740): a fresh one or an owning local's as it
     * is, anything borrowed copied. The caller that destructures the call
     * owns it (the AST_TUPLE_DESTRUCTURE handler). Before, the slot carried
     * whatever was returned, and nobody freed a fresh one. */
    if (rt0 && rt0->kind == TYPE_TUPLE && j >= 0 && j < rt0->tuple_count &&
        is_opt_string_type(rt0->tuple_types[j]) && emitting_owned_opt_string_tuple(gen)) {
        emit_opt_string_return(gen, expr, rt0->tuple_types[j]);
        return;
    }
    /* #2652: an optional slot wraps its value (or zeroes `none`), as a
     * `-> T?` return does; the bare value was emitted into the struct. */
    if (rt0 && rt0->kind == TYPE_TUPLE && j >= 0 && j < rt0->tuple_count &&
        needs_optional_coerce(expr, rt0->tuple_types[j])) {
        emit_optional_coerced(gen, expr, rt0->tuple_types[j]);
        return;
    }
    int pos_heap = 0;
    if (gen && gen->current_function && !gen->in_main_function) {
        Type* rt = gen->current_func_return_type;
        if (rt && rt->kind == TYPE_TUPLE && j >= 0 && j < rt->tuple_count &&
            rt->tuple_types[j] && rt->tuple_types[j]->kind == TYPE_STRING) {
            /* #2501: a closure hands every string slot over owned, as a
             * string closure does its result (#2054): its caller reaches it
             * through a value and cannot ask which returns are heap. */
            pos_heap = gen->current_function->type == AST_CLOSURE ||
                       function_def_returns_heap_at(gen, gen->current_function, j);
        }
    }
    Type* pt = rt0 && rt0->kind == TYPE_TUPLE && j >= 0 && j < rt0->tuple_count
               ? rt0->tuple_types[j] : NULL;
    if (pos_heap) {
        emit_uniform_heap_return_expr(gen, expr);
    } else if (pt && struct_owning_strings(gen, pt) && !gen->in_main_function) {
        /* R3: each position hands over a value of its own. */
        emit_struct_take(gen, expr, struct_owning_strings(gen, pt), NULL);
    } else if (pt && own_type_owns(gen, pt) && pt->kind != TYPE_STRING &&
               !gen->in_main_function) {
        emit_composite_take(gen, expr, pt);   /* a closure: emit_closure_take */
    } else {
        generate_expression(gen, expr);
    }
}

// Recursive: collect every string variable name: every variable that
// appears as the LHS of an AST_VARIABLE_DECLARATION (in Aether, "decl"
// covers both first-assignment and reassignment) whose RHS is a string
// or whose type is annotated TYPE_STRING.
//
// Walking is purely structural: every nested block, every loop body,
// every if-then / if-else, every match arm. The hoist must see all
// of them so a name first-assigned at depth-3 and reassigned at
// depth-1 is still declared at function scope.
//
// Issue #405: without this pre-pass a string variable's state was
// declared at the C scope where the variable was first seen, which
// went out of scope when control left that block, so cross-block
// reassignment either failed to compile or leaked the old value.
static void collect_heap_string_var_names(CodeGenerator* gen, ASTNode* node,
                                          const char** names,
                                          int* count, int cap) {
    if (!node || *count >= cap) return;

    if (node->type == AST_VARIABLE_DECLARATION && node->value &&
        strcmp(node->value, "_") != 0 &&
        !is_module_global_var(gen, node->value)) {
        /* #701/#744: a module-level string `var` is a file-scope
         * `static const char*`, not a function local. It must NOT be
         * collected as a heap-tracked local — doing so would (a) hoist
         * a shadowing `const char* <name> = NULL;` over the global,
         * (b) route the write through the reassignment wrapper into
         * that shadow (so cross-function reads never see the new
         * value), and (c) push a function-exit defer-free that frees
         * the process-lifetime global (a UAF/double-free across
         * calls). Skipping the name here leaves is_var_declared false,
         * so the AST_VARIABLE_DECLARATION emitter routes the write to
         * the static (is_module_global_var branch) exactly like the
         * scalar case, and the global is never freed — the latest
         * value persists for the process lifetime, mirroring the
         * scalar/#701 model. This matches how hoist_loop_vars and the
         * if-branch hoists already skip module globals by name. */
        /* An optional-typed LHS (`b: string? = <string>`) is an
         * `ae_opt_string`, NOT a bare `const char*`. Without this guard
         * the initializer-type check below grabs it (the RHS is
         * TYPE_STRING), hoists a `const char* b`, and the optional
         * decl-site then assigns an `ae_opt_string` struct into that
         * `char*` slot — a C type error. Optional-of-string locals are
         * owned exclusively by the opt-str registry
         * (collect_opt_str_var_names); skip them here. */
        if (node->node_type && node->node_type->kind == TYPE_OPTIONAL) {
            for (int i = 0; i < node->child_count; i++)
                collect_heap_string_var_names(gen, node->children[i], names, count, cap);
            return;
        }
        // Decide whether this declaration's LHS is a string local.
        // Bare `_` is a per-use discard, never a tracked variable —
        // skipped here so a string-typed `_` destructure slot doesn't
        // get hoisted as a `const char* _` the codegen then reuses.
        int needs_tracker = 0;
        if (node->child_count > 0 && is_heap_string_expr(gen, node->children[0])) {
            needs_tracker = 1;
        }
        // Type-annotated string variable (covers `s: string = ""`).
        if (!needs_tracker && node->node_type &&
            node->node_type->kind == TYPE_STRING) {
            needs_tracker = 1;
        }
        // Initializer-typed string (covers `s = ""` where the
        // typechecker stamped TYPE_STRING on the RHS).
        if (!needs_tracker && node->child_count > 0 && node->children[0] &&
            node->children[0]->node_type &&
            node->children[0]->node_type->kind == TYPE_STRING) {
            needs_tracker = 1;
        }
        if (needs_tracker) {
            int already = 0;
            for (int i = 0; i < *count; i++) {
                if (strcmp(names[i], node->value) == 0) { already = 1; break; }
            }
            if (!already && *count < cap) {
                names[(*count)++] = node->value;
            }
        }
    }

    for (int i = 0; i < node->child_count; i++) {
        collect_heap_string_var_names(gen, node->children[i], names, count, cap);
    }
}

// Mark every string variable in `body` as a string local and hoist
// its C-level `const char* <name> = NULL;` declaration to
// function-entry scope. Caller invokes this after parameters are
// declared and before the body is generated.
//
// After this runs, `is_heap_string_var(gen, name)` and
// `is_var_declared(gen, name)` both return true for every collected
// non-special name, so the per-stmt codegen routes ALL assignments
// (including the original "first" assignment) through the
// reassignment path, which reads `_tmp_old` from the function-scoped
// variable, never from a freshly-declared per-block stack slot.
//
// History: the first #405 fix left the C variable at its first-use
// point. A name first-assigned in two sibling C blocks then got two
// uninitialised C variables, and the second block's first assignment
// freed stack garbage (glibc abort).
//
// Skipped names (still marked; only the C var hoist is skipped):
//   - already-declared (function parameters, vars hoisted by
//     hoist_if_branch_vars before this pass) — would emit a
//     duplicate declaration.
//   - actor state vars — accessed via `self->name`; a local
//     `const char* name = NULL` would be unused (-Wunused-variable
//     under -Werror) and shadow nothing useful.
//   - env captures — closure body accesses via `_env->name`.
//   - promoted captures — declared as `int* name = malloc(...)` by
//     the closure-promotion path; declaring `const char*` here
//     would create a conflicting pre-decl.
/* The state declaration of `name` in the actor being emitted, or NULL. */
static ASTNode* actor_state_decl(CodeGenerator* gen, const char* name) {
    if (!gen->current_actor || !gen->program || !name || !is_actor_state_var(gen, name))
        return NULL;
    for (int a = 0; a < gen->program->child_count; a++) {
        ASTNode* actor = gen->program->children[a];
        if (!actor || actor->type != AST_ACTOR_DEFINITION || !actor->value ||
            strcmp(actor->value, gen->current_actor) != 0) continue;
        for (int i = 0; i < actor->child_count; i++) {
            ASTNode* sd = actor->children[i];
            if (sd && sd->type == AST_STATE_DECLARATION && sd->value &&
                strcmp(sd->value, name) == 0) return sd;
        }
    }
    return NULL;
}

/* #2904: is `name` a `ptr` state field of the actor being emitted? */
static int actor_state_is_ptr(CodeGenerator* gen, const char* name) {
    ASTNode* sd = actor_state_decl(gen, name);
    return sd && sd->node_type && sd->node_type->kind == TYPE_PTR;
}

/* #2528: is `name` a `string` state field of the actor being emitted, whose
 * tracker is a field of the actor struct? */
static int actor_state_string_tracked(CodeGenerator* gen, const char* name) {
    return state_field_owns_string(actor_state_decl(gen, name));
}

/* Is `name` a `string` parameter of the function or closure `fn`? */
static int is_string_param_of(ASTNode* fn, const char* name) {
    for (int i = 0; fn && name && i < fn->child_count; i++) {
        ASTNode* p = fn->children[i];
        if (p && p->value && strcmp(p->value, name) == 0 &&
            (p->type == AST_PATTERN_VARIABLE || p->type == AST_CLOSURE_PARAM ||
             (p->type == AST_VARIABLE_DECLARATION && fn->type != AST_CLOSURE)))
            return p->node_type && p->node_type->kind == TYPE_STRING;
    }
    return 0;
}

void hoist_heap_string_trackers(CodeGenerator* gen, ASTNode* body) {
    if (!body || !gen) return;
    const char* names[256];  // 256 string vars per fn is generous
    int count = 0;
    collect_heap_string_var_names(gen, body, names, &count, 256);
    for (int i = 0; i < count; i++) {
        const char* name = names[i];
        /* A string state field is the actor's, not a local here: a take
         * of its value retains it (the state keeps its own). */
        if (!is_heap_string_var(gen, name)) {
            if (actor_state_string_tracked(gen, name)) continue;
            mark_heap_string_var(gen, name);
        }
        /* R3: a parameter is borrowed; one the body rebinds holds a string
         * of its own from the start, released at its exit like a local. */
        if (is_string_param_of(gen->current_function, name) && !is_promoted_capture(gen, name))
            print_line(gen, "%s = aether_uniform_heap_str(%s, 0);", name, name);

        /* C-variable hoist (see the function comment above). Skip names
         * that aren't simple function-local C vars. */
        if (is_var_declared(gen, name)) continue;
        if (gen->current_actor) {
            int is_state = 0;
            for (int s = 0; s < gen->state_var_count; s++) {
                if (gen->actor_state_vars[s] &&
                    strcmp(gen->actor_state_vars[s], name) == 0) {
                    is_state = 1; break;
                }
            }
            if (is_state) continue;
        }
        int is_env_cap = 0;
        for (int e = 0; e < gen->current_env_capture_count; e++) {
            if (gen->current_env_captures[e] &&
                strcmp(gen->current_env_captures[e], name) == 0) {
                is_env_cap = 1; break;
            }
        }
        if (is_env_cap) continue;
        if (is_promoted_capture(gen, name)) continue;

        /* #2690: and so is the local itself. Hoisted here, it was declared
         * plain however a `try` body wrote it, so after a panic it could
         * hold its value from before the `try`. */
        emit_string_local_decl(gen, name);
    }
}

/* True for a `string?` type: an optional wrapping a bare string. The
 * heap-ownership tracking only applies to string payloads (scalar
 * optionals like `int?` carry no allocation). */
static int is_opt_string_type(Type* t) {
    return t && t->kind == TYPE_OPTIONAL &&
           t->element_type && t->element_type->kind == TYPE_STRING;
}

/* #2750: is `e` a call of an Aether function returning `string?`? Such a
 * function hands every string it returns over owned (emit_opt_string_return,
 * #2740). Bound to a local the result is that local's (is_heap_opt_string_rhs);
 * anywhere nothing binds it, it is a temporary of the expression that uses
 * it, which frees it: under `!` and `??` (is_owned_string_unwrap), a
 * comparison, as a call's argument, or thrown away. Nothing freed it there,
 * and since #2740 copies a returned literal, even `lookup()! == "Ada"`
 * leaked. A closure or a C function is not held to #2740, so its result
 * stays as it was. */
static ASTNode* callee_def(CodeGenerator* gen, const char* func_name);

int is_owned_opt_string_call(CodeGenerator* gen, ASTNode* e) {
    if (!gen || !e || e->type != AST_FUNCTION_CALL || !e->value ||
        !is_opt_string_type(e->node_type) || strcmp(e->value, "call") == 0 ||
        typed_fnptr_call(gen, e)) return 0;
    ASTNode* def = callee_def(gen, e->value);
    return def && def->type == AST_FUNCTION_DEFINITION && is_opt_string_type(def->node_type);
}

/* `call!` or `call ?? d` over such a call, yielding its string: a string of
 * its own, as fresh as the call's (the `??` default is copied where it is
 * not fresh itself, AST_NULL_COALESCE), which is_heap_string_expr counts as
 * a heap string, so every consumer adopts or frees it as it does a
 * `string.concat` result. So is `call!` over a (value, err) call whose value
 * slot is a heap string (or_fallible_value_slot_is_heap, the rule the `or`
 * boxes by): the value is the caller's, and `pair()! == "x"` leaked it. */
int is_owned_string_unwrap(CodeGenerator* gen, ASTNode* e) {
    if (!e || e->child_count < 1) return 0;
    ASTNode* o = e->children[0];
    if (e->type == AST_TUPLE_UNWRAP && o && o->node_type && o->node_type->kind == TYPE_TUPLE &&
        o->node_type->tuple_count >= 2 && o->node_type->tuple_types &&
        o->node_type->tuple_types[0] && o->node_type->tuple_types[0]->kind == TYPE_STRING)
        return or_fallible_value_slot_is_heap(gen, o);
    if (!is_owned_opt_string_call(gen, o)) return 0;
    if (e->type == AST_TUPLE_UNWRAP) return 1;
    return e->type == AST_NULL_COALESCE && e->child_count >= 2 && e->children[1] &&
           !(e->children[1]->node_type && e->children[1]->node_type->kind == TYPE_OPTIONAL);
}

/* An expression whose value is a heap string nobody else holds, which the
 * expression that reads it must free: a call or an interpolation that
 * makes one, a fallible's `or`, an unwrapped `string?` call. */
int is_fresh_heap_string_temp(CodeGenerator* gen, ASTNode* e) {
    if (!e) return 0;
    if (e->type != AST_FUNCTION_CALL && e->type != AST_STRING_INTERP &&
        e->type != AST_OR_ELSE && !is_owned_string_unwrap(gen, e)) return 0;
    return is_heap_string_expr(gen, e);
}

/* Is a `string?` argument the call `func_name` receives at `ai` (an owned
 * temporary, is_owned_opt_string_call) free once the call returns? Only
 * with proof the parameter keeps nothing: it does not escape the callee's
 * body, or escapes only through a `string?` return, which copies it
 * (#2740). Without a body to read it is kept, a leak, never a free under a
 * parameter that holds it. */

/* Does `rhs`, given to a `string?` holder, hand its string over? A call's
 * result does (R3); a bare heap string does; anything else (a place, `??`,
 * `?.`) is copied by its holder. */
static int is_heap_opt_string_rhs(CodeGenerator* gen, ASTNode* rhs) {
    if (!rhs || rhs->type == AST_NONE_LITERAL) return 0;
    if (rhs->node_type && rhs->node_type->kind == TYPE_OPTIONAL)
        return rhs->type == AST_FUNCTION_CALL;
    return is_heap_string_expr(gen, rhs);
}

/* Collect `string?` local declaration names. Keyed on the LHS or initializer carrying a
 * `string?` type. */
static void collect_opt_str_var_names(CodeGenerator* gen, ASTNode* node,
                                      const char** names, int* count, int cap) {
    if (!node || *count >= cap) return;
    if (node->type == AST_VARIABLE_DECLARATION && node->value &&
        strcmp(node->value, "_") != 0 &&
        !is_module_global_var(gen, node->value)) {
        int is_opt = is_opt_string_type(node->node_type);
        if (!is_opt && node->child_count > 0 && node->children[0] &&
            is_opt_string_type(node->children[0]->node_type)) {
            is_opt = 1;
        }
        if (is_opt) {
            int already = 0;
            for (int i = 0; i < *count; i++)
                if (strcmp(names[i], node->value) == 0) { already = 1; break; }
            if (!already && *count < cap) names[(*count)++] = node->value;
        }
    }
    for (int i = 0; i < node->child_count; i++)
        collect_opt_str_var_names(gen, node->children[i], names, count, cap);
}

/* Declare every `string?` local at function scope, where it has always
 * been declared, and push its drop: it owns what it holds, as every optional
 * local does (#2921), and every binding stores into it. */
void hoist_opt_string_locals(CodeGenerator* gen, ASTNode* body) {
    if (!body || !gen) return;
    const char* names[256];
    int count = 0;
    collect_opt_str_var_names(gen, body, names, &count, 256);
    for (int i = 0; i < count; i++) {
        const char* name = names[i];
        if (is_var_declared(gen, name) || is_actor_state_var(gen, name) ||
            is_env_capture_name(gen, name) || is_promoted_capture(gen, name)) continue;
        Type* t = create_type(TYPE_OPTIONAL);
        t->element_type = create_type(TYPE_STRING);
        print_indent(gen);
        fprintf(gen->output, "%s %s = (ae_opt_string){0};\n",
                volatile_decl_type(try_volatile_qual_for(gen, name), "ae_opt_string"), name);
        mark_var_declared_typed(gen, name, t);
        push_own_drop(gen, name, t, body->line, body->column);
        free_type(t);
    }
}

/* Look up the parameter type-kind for the n-th argument of a callee
 * named `func_name` (in either dotted source-form or underscored
 * extern-form). Returns the param's TypeKind, or TYPE_UNKNOWN if the
 * callee can't be resolved. */
/* Does the parameter at `idx` of the callee keep what it is given past the
 * call: an Aether function's `@keep` one, an extern's `@retain` one (R4)?
 * Any other borrows it for the call. */
int call_param_keeps(CodeGenerator* gen, const char* func_name, int idx) {
    if (!gen || !func_name || idx < 0) return 0;
    if (is_retain_extern_param(gen, func_name, idx)) return 1;
    ASTNode* def = callee_def(gen, func_name);
    for (int i = 0, pi = 0; def && i < def->child_count; i++) {
        ASTNode* p = def->children[i];
        if (!p || p->type == AST_GUARD_CLAUSE || p->type == AST_BLOCK ||
            p->type == AST_REQUIRES_CLAUSE || p->type == AST_ENSURES_CLAUSE) continue;
        if (pi++ == idx) return p->annotation && strstr(p->annotation, "keep_param") != NULL;
    }
    return 0;
}

TypeKind lookup_callee_param_kind(CodeGenerator* gen,
                                          const char* func_name,
                                          int param_idx) {
    if (!gen || !func_name || param_idx < 0) return TYPE_UNKNOWN;
    const char* fn = codegen_normalise_callee(func_name);
    /* Externs first — registered with param-kind table. */
    TypeKind k = lookup_extern_param_kind(gen, fn, param_idx);
    if (k != TYPE_UNKNOWN) return k;
    /* Fall back to user-defined fn lookup via the program AST. */
    if (gen->program) {
        ASTNode* fn_def = find_function_definition_by_name(gen->program, fn);
        if (fn_def && param_idx < fn_def->child_count) {
            ASTNode* param = fn_def->children[param_idx];
            if (param && param->node_type) {
                return param->node_type->kind;
            }
        }
    }
    return TYPE_UNKNOWN;
}

static int is_nonstoring_builtin(const char* fn);
static int is_consuming_free(CodeGenerator* gen, const char* fn);


/* The definition the call of `func_name` reaches. */
static ASTNode* callee_def(CodeGenerator* gen, const char* func_name) {
    if (!gen || !gen->program || !func_name) return NULL;
    return find_function_definition_by_name(gen->program, codegen_normalise_callee(func_name));
}

/* Parameter `param_idx` of the definition `fn_def` when it binds a
 * `string`, or NULL. */
static ASTNode* def_string_param(ASTNode* fn_def, int param_idx) {
    if (!fn_def || param_idx < 0 || param_idx >= fn_def->child_count) return NULL;
    ASTNode* param = fn_def->children[param_idx];
    if (!param || !param->value || !param->node_type || param->node_type->kind != TYPE_STRING ||
        (param->type != AST_VARIABLE_DECLARATION && param->type != AST_PATTERN_VARIABLE)) return NULL;
    return param;
}

/* The `string` parameter `param_idx` of `func_name`, or NULL: of the first
 * clause that binds one there, for a set (#2644: a literal pattern in the
 * first clause hid the string the others take). */
static ASTNode* callee_string_param_node(CodeGenerator* gen, const char* func_name, int param_idx) {
    if (!gen || !gen->program || !func_name || param_idx < 0) return NULL;
    const DefClauses* dc = program_index_clauses(gen->program, codegen_normalise_callee(func_name));
    for (int c = 0; dc && c < dc->count; c++) {
        ASTNode* param = def_string_param(dc->nodes[c], param_idx);
        if (param) return param;
    }
    return NULL;
}

int callee_param_is_string(CodeGenerator* gen, const char* func_name, int param_idx) {
    return callee_string_param_node(gen, func_name, param_idx) != NULL;
}

static int subtree_mentions_param(ASTNode* node, const char* pname);
/* #2499: can a value of type `t` be a string the caller owns? A closure
 * call through an erased `fn` checks no argument types, so a string can
 * reach a `ptr` (or untyped-pointer) parameter as well as a `string` one. */
int param_may_hold_caller_string(const Type* t) {
    if (!t) return 0;   /* an untyped closure parameter is an int */
    return t->kind == TYPE_STRING || t->kind == TYPE_PTR ||
           t->kind == TYPE_UNKNOWN || t->kind == TYPE_WILDCARD ||
           t->kind == TYPE_OPTIONAL;
}

/* Does the user function `func_name` declare a `-> string` return? Only
 * then is the call-site identity-drain meaningful (it compares the call
 * result pointer against the passed temp). */
int callee_returns_string(CodeGenerator* gen, const char* func_name) {
    if (!gen || !gen->program || !func_name) return 0;
    const char* fn = codegen_normalise_callee(func_name);
    ASTNode* fn_def = find_function_definition_by_name(gen->program, fn);
    if (!fn_def) return 0;
    return fn_def->node_type && fn_def->node_type->kind == TYPE_STRING;
}

/* True when `func_name` resolves to a user function with a visible body
 * block in the merged program AST. Externs and unknown callees have no
 * body. */
int callee_has_visible_body(CodeGenerator* gen, const char* func_name) {
    if (!gen || !gen->program || !func_name) return 0;
    const char* fn = codegen_normalise_callee(func_name);
    ASTNode* fn_def = find_function_definition_by_name(gen->program, fn);
    if (!fn_def) return 0;
    for (int i = fn_def->child_count - 1; i >= 0; i--) {
        if (fn_def->children[i] && fn_def->children[i]->type == AST_BLOCK) {
            return 1;
        }
    }
    return 0;
}

/* Does `pname` appear as an identifier anywhere in this subtree? */
static int subtree_mentions_param(ASTNode* node, const char* pname) {
    if (!node) return 0;
    if (node->type == AST_IDENTIFIER && node->value &&
        strcmp(node->value, pname) == 0) return 1;
    for (int i = 0; i < node->child_count; i++) {
        if (subtree_mentions_param(node->children[i], pname)) return 1;
    }
    return 0;
}

/* Built-in callees that take a string argument but never keep the pointer
 * past the call: the print family (print / println / print_char) writes the
 * bytes and returns, and the read-only accessors listed in
 * is_nonstoring_builtin. The frees are NOT in this list; see
 * is_consuming_free below. */
/* The frees. They neither store nor return the pointer, but they DO end its
 * life: a fresh argument passed to one is not released again after the call
 * (arg_taken_by_call). */
const char* consuming_free_symbol(CodeGenerator* gen, const char* fn) {
    if (!fn) return NULL;
    if (strcmp(fn, "release") == 0) return "release";
    if (strcmp(fn, "string_release") == 0) return "string_release";
    if (strcmp(fn, "string_free") == 0) return "string_free";
    /* `@extern("string_free") _release_x(s: string)` (std.jsonpath) is the
     * same free under another name; seen by its name only, the free went
     * unrecognised, and the local it freed was freed again at exit. */
    if (!gen || !gen->program) return NULL;
    ProgramIndex* ix = program_index(gen->program);
    ASTNode* ext = ix ? (ASTNode*)strmap_get(&ix->externs, fn) : NULL;
    const char* sym = ext ? extern_c_symbol(ext) : NULL;
    if (sym && strcmp(sym, "string_free") == 0) return "string_free";
    if (sym && strcmp(sym, "string_release") == 0) return "string_release";
    return NULL;
}

static int is_consuming_free(CodeGenerator* gen, const char* fn) {
    return consuming_free_symbol(gen, fn) != NULL;
}

int call_frees_arg(CodeGenerator* gen, const char* fn) {
    return is_consuming_free(gen, fn) || (fn && strcmp(fn, "string_seq_free") == 0);
}

static int is_nonstoring_builtin(const char* fn) {
    if (!fn) return 0;
    return strcmp(fn, "print") == 0 ||
           strcmp(fn, "println") == 0 ||
           strcmp(fn, "print_char") == 0 ||
           /* Pure read-only views into a string's bytes/length. These
            * return a non-owning view (or a scalar) and never stash the
            * argument pointer, so passing a heap string to one is not an
            * escape. Stdlib wrappers funnel params through these before
            * handing the raw bytes to a synchronous C extern
            * (`file_delete_raw(aether_string_data(path))`), which had
            * falsely marked `path` escaped and leaked every interpolated
            * argument. */
           strcmp(fn, "aether_string_data") == 0 ||
           strcmp(fn, "aether_string_len") == 0 ||
           strcmp(fn, "aether_string_length") == 0 ||
           strcmp(fn, "_aether_safe_str") == 0 ||
           /* std.string.bytes: a BORROWED byte[] view of the string's own
            * storage (#2301). It does not retain the argument, and a slice
            * does not extend its owner's lifetime (the documented slice
            * contract, and bytes()'s own), so a NAMED heap string passed to
            * it keeps its scope-exit free. Without this, routing a scope-
            * owned string through `string.bytes(s)` to a write withheld the
            * free and leaked it on every call.
            *
            * Deliberately NOT listed: aether_string_raw_ptr, the accessor in
            * bytes()'s body. The call-argument drain decides whether a
            * TEMPORARY can be freed straight after the call by walking the
            * callee's body; with the accessor listed, that walk stops seeing
            * `s` flow into the returned view, and `string.bytes(f())` frees
            * f()'s result while the view still points into it. Unlisted,
            * a temporary is left alone (it leaks, which is safe). */
           strcmp(fn, "string_bytes") == 0 ||
           /* string_seq_join walks the spine read-only and copies the
            * bytes into a fresh buffer; it retains neither the seq nor
            * the separator. Without this, a `s = seq_cons(x, s)`
            * accumulator that is later joined has its whole loop
            * marked escaped, and every intermediate spine ref leaks. */
           strcmp(fn, "string_seq_join") == 0 ||
           strcmp(fn, "string_join") == 0 ||
           /* std.bytes readers that yield a scalar. They cannot retain the
            * pointer they are given, and since #1380 they reject anything that
            * is not an AetherBytes outright. Without these, a heap string
            * passed to bytes.length/get had its scope-exit free suppressed and
            * leaked unless the caller added an explicit release(). */
           strcmp(fn, "aether_bytes_length") == 0 ||
           strcmp(fn, "aether_bytes_capacity") == 0 ||
           strcmp(fn, "aether_bytes_get") == 0 ||
           strcmp(fn, "aether_bytes_get_le16") == 0 ||
           strcmp(fn, "aether_bytes_get_le32") == 0 ||
           strcmp(fn, "aether_bytes_get_le64") == 0 ||
           strcmp(fn, "aether_bytes_get_be16") == 0 ||
           strcmp(fn, "aether_bytes_get_be32") == 0 ||
           strcmp(fn, "aether_bytes_get_be64") == 0 ||
           /* Copies the source bytes into the buffer; retains neither. */
           strcmp(fn, "aether_bytes_copy_from_string") == 0;
}

/* Emit a builder block's body as its own C scope.
 *
 * CRITICAL: the `{ ... }` braces must be matched by a defer scope. Cleanup
 * queued inside the block (a heap string, a closure-capture cell) names a C
 * variable declared between those braces; emitting it at the enclosing
 * function's exit instead puts the free where the name is out of scope, and
 * the generated C does not compile. */
void emit_trailing_block_body(CodeGenerator* gen, ASTNode* body) {
    if (!gen || !body) return;
    int saved_var_count = gen->declared_var_count;
    print_indent(gen);
    fprintf(gen->output, "{\n");
    gen->indent_level++;
    enter_scope(gen);
    /* Every caller pushed this block's builder context just before calling
     * here. Its pop is the scope's first defer, so it runs after the
     * block's own defers on every exit: the fall-through end below, a
     * return, a break/continue out of the block (see try_emit_builder_ctx_pop
     * in codegen.c). Callers do not emit the pop themselves. */
    ASTNode* ctx_pop = create_ast_node(AST_EXPRESSION_STATEMENT, NULL,
                                       body->line, body->column);
    if (ctx_pop) {
        if (ctx_pop->annotation) free(ctx_pop->annotation);
        ctx_pop->annotation = strdup("builder_ctx_pop");
        codegen_own_node(gen, ctx_pop);
        push_defer(gen, ctx_pop);
    }
    /* #2791: a builder call's config, released on a way out of the block
     * before the builder runs (try_emit_builder_cfg_release), not at the
     * block's natural end, after which the builder takes it. The block
     * holds it under a name of its own: every builder call names its
     * config `_bcfg`, so in a nested call's block the outer one's is
     * shadowed, and a way out of both must still release each. */
    const char* own = NULL;
    if (gen->builder_cfg_var && gen->builder_cfg_release) {
        static int own_counter = 0;
        own = cg_internf("_ae_cfg_own%d", own_counter++);
        print_line(gen, "void* %s = %s;", own, gen->builder_cfg_var);
        /* #2810: the stack entry the call pushed records the release, for
         * a `try` that catches a panic leaving this block. */
        print_line(gen, "_aether_ctx_own(%s, %s);", gen->builder_cfg_var,
                   gen->builder_cfg_release);
        ASTNode* release = create_ast_node(AST_EXPRESSION_STATEMENT, NULL,
                                           body->line, body->column);
        if (release) {
            if (release->annotation) free(release->annotation);
            release->annotation = heap_strf("builder_cfg_release:%s:%s", own,
                                            gen->builder_cfg_release);
            codegen_own_node(gen, release);
            push_defer(gen, release);
        }
    }
    gen->builder_cfg_var = NULL;
    gen->builder_cfg_release = NULL;
    gen->in_trailing_block++;
    for (int si = 0; si < body->child_count; si++) {
        generate_statement(gen, body->children[si]);
    }
    gen->in_trailing_block--;
    /* The natural end: the builder takes the config from here. */
    if (own) print_line(gen, "%s = NULL;", own);
    exit_scope(gen);
    gen->indent_level--;
    print_indent(gen);
    fprintf(gen->output, "}\n");
    truncate_declared_vars(gen, saved_var_count);
}

/* #2791: the C function that releases what a builder's config factory
 * made, or NULL when none is known: `map_free` for the default `map_new`,
 * and for a `<stem>_new` factory the `<stem>_free(ptr)` returning nothing
 * that the program defines or declares (`list_new`, `list_free`): a C
 * `void (*)(void*)`, as the context stack records it (#2810). Any other
 * factory's object belongs to its builder's protocol, which the compiler
 * cannot release for it. */
static const char* builder_config_release(CodeGenerator* gen, const char* builder_name) {
    const char* factory = get_builder_factory(gen, builder_name);
    if (!factory) return NULL;
    size_t n = strlen(factory);
    if (n <= 4 || strcmp(factory + n - 4, "_new") != 0) return NULL;
    const char* release = cg_internf("%.*s_free", (int)(n - 4), factory);
    if (strcmp(factory, "map_new") == 0) return release;
    if (is_extern_func(gen, release)) {
        /* One `ptr` parameter: the second slot reads as unknown. */
        if (lookup_extern_param_kind(gen, release, 0) != TYPE_PTR ||
            lookup_extern_param_type(gen, release, 1) != NULL)
            return NULL;
        Type* rt = lookup_extern_return_type(gen, release);
        if (rt && rt->kind != TYPE_VOID) return NULL;
        return lookup_extern_c_name(gen, release);
    }
    ASTNode* def = gen->program ? find_function_definition_by_name(gen->program, release) : NULL;
    if (!def || def->type != AST_FUNCTION_DEFINITION || fn_param_count(def) != 1) return NULL;
    if (def->node_type && def->node_type->kind != TYPE_VOID &&
        def->node_type->kind != TYPE_UNKNOWN)
        return NULL;
    if (def->child_count > 0 && has_return_value(def->children[def->child_count - 1]))
        return NULL;
    ASTNode* p = fn_param_at(def, 0);
    if (!p || !p->node_type || p->node_type->kind != TYPE_PTR) return NULL;
    return codegen_normalise_callee(safe_c_name(release));
}

/* Does the builder `builder` never name its config? Read from its own
 * body: such a builder neither frees the config nor returns it. */
int builder_never_uses_config(ASTNode* builder) {
    if (!builder || builder->type != AST_BUILDER_FUNCTION) return 0;
    for (int i = 0; i < builder->child_count; i++)
        if (builder->children[i] && builder->children[i]->type == AST_BLOCK)
            return !expr_references_var(builder->children[i], "_builder");
    return 0;
}

/* #2871: a builder owns the config its call hands it (it frees it, or
 * returns it as what it built). One whose body never names `_builder` does
 * neither, so its exit releases the config through its factory's release. */
void push_builder_config_exit_release(CodeGenerator* gen, ASTNode* builder, ASTNode* body) {
    if (!builder->value || !body || !builder_never_uses_config(builder)) return;
    const char* release = builder_config_release(gen, builder->value);
    if (!release) return;
    ASTNode* carrier = create_ast_node(AST_EXPRESSION_STATEMENT, NULL, body->line, body->column);
    if (carrier->annotation) free(carrier->annotation);
    carrier->annotation = heap_strf("builder_cfg_release:_builder:%s", release);
    codegen_own_node(gen, carrier);
    push_defer(gen, carrier);
}

void emit_builder_block_body(CodeGenerator* gen, ASTNode* body, const char* cfg_var,
                             const char* builder_name) {
    gen->builder_cfg_var = cfg_var;
    gen->builder_cfg_release = builder_config_release(gen, builder_name);
    emit_trailing_block_body(gen, body);
}

static int env_scan_is_real_closure(ASTNode* n) {
    return n && n->type == AST_CLOSURE &&
           !(n->value && strcmp(n->value, "trailing") == 0);
}

/* #2525: a closure read out of a field or an element (`x = h.cb`), which a
 * binding retains, so the local holds a reference of its own. */
static int actor_state_string_tracked(CodeGenerator* gen, const char* name);

static int closure_view_binding(ASTNode* rhs) {
    return rhs && (rhs->type == AST_MEMBER_ACCESS || rhs->type == AST_ARRAY_ACCESS) &&
           rhs->node_type && rhs->node_type->kind == TYPE_FUNCTION && !rhs->node_type->is_fnptr;
}

/* #2528: a closure an ask brings back (`x = k ? Get {}`): the reply handed
 * the asker a reference of its own, so the binding owns it like a literal. */
static int closure_ask_binding(ASTNode* rhs) {
    return rhs && rhs->type == AST_SEND_ASK && rhs->node_type &&
           rhs->node_type->kind == TYPE_FUNCTION && !rhs->node_type->is_fnptr;
}

/* Is `e` a read of a closure cell of the scope being emitted? */
int is_closure_cell_read(CodeGenerator* gen, ASTNode* e) {
    return e && e->type == AST_IDENTIFIER && e->value && e->node_type &&
           e->node_type->kind == TYPE_FUNCTION && !e->node_type->is_fnptr &&
           is_promoted_capture(gen, e->value);
}

/* R3: does `call` hand its caller a closure of its own? Every call whose
 * result is a closure does, but a C function's, which lends it. */
int call_returns_owned_closure(CodeGenerator* gen, ASTNode* call) {
    return call && call->type == AST_FUNCTION_CALL && call->node_type &&
           call->node_type->kind == TYPE_FUNCTION && !call->node_type->is_fnptr &&
           !call_is_extern(gen, call);
}

/* ------------------------------------------------------------------
 * Closure-capture cell lifetime (#2019)
 *
 * A variable a closure mutates is promoted to a heap cell (`T* n = ...`)
 * that the closure's env points at. The cell is shared between the
 * declaring scope and every env built from it, and neither side knows
 * how long the other lives: a callback passed to fs.walk is gone when the
 * call returns, a handler handed to a widget or a timer fires long after
 * the scope has ended, and a closure that is returned outlives its whole
 * function.
 *
 * So the cell is reference-counted. The scope holds one reference from
 * declaration to exit; each env takes one when it is built and gives it
 * back in its generated destructor; whoever releases last frees. That
 * makes the scope-exit release below unconditional — there is no escape
 * analysis to get wrong in either direction, no cell freed under a live
 * callback and no cell leaked because a walk could not see through a
 * tuple destructure or an extern that owns the closure.
 *
 * The three places that declare a cell (a first assignment, a tuple
 * destructure slot, a promoted parameter) all come through here so the
 * shape is written once.
 * ------------------------------------------------------------------ */
/* #2474: a fixed-size array's C type spells as `E[N]` (get_c_type). Its
 * length N, with the element's C type in `elem`; 0 when `c_type` is not an
 * array. */
int promoted_cell_array_len(const char* c_type, const char** elem) {
    size_t len = c_type ? strlen(c_type) : 0;
    const char* br = len ? strrchr(c_type, '[') : NULL;
    if (!br || c_type[len - 1] != ']') return 0;
    int count = atoi(br + 1);
    if (count <= 0) return 0;
    *elem = cg_intern_n(c_type, (size_t)(br - c_type));
    return count;
}

/* #2474: the C spelling of a pointer to a promoted cell holding `c_type`,
 * declaring `name`, or the bare type for a cast when `name` is NULL. A cell
 * of a scalar, a string or a struct is `T* name`. An array's `E[N]` is no
 * declarator, so its cell is a pointer to the whole array, `E (*name)[N]`:
 * then `(*name)`, the spelling every use of a promoted name gets (the
 * AST_IDENTIFIER emission), is the array itself, and indexes, decays to a
 * pointer, passes as a slice and has the array's `sizeof`, as the array
 * does. Interned (#2539): a 600-byte buffer cut a long type or name. */
const char* promoted_cell_pointer(const char* c_type, const char* name) {
    const char* elem;
    int count = promoted_cell_array_len(c_type, &elem);
    if (count > 0) return cg_internf("%s (*%s)[%d]", elem, name ? name : "", count);
    return cg_internf("%s*%s%s", c_type, name ? " " : "", name ? name : "");
}

/* The value a string cell, or a string element of an array cell (#2474),
 * takes. The cell owns what it holds (its last release frees it, and a
 * store frees the value it replaces), so it takes a buffer of its own
 * (emit_string_take_owned: a fresh value adopted, a heap-tracked local
 * moved or copied, a view or a borrow copied). A borrowed string stored as
 * it stood was released by the cell as well as by its owner: a parameter
 * a closure captured was released by the closure's env and by the cell
 * (#2514). A literal is stored as it is: the cell frees only refcounted
 * strings. */
static void emit_cell_element_value(CodeGenerator* gen, int str_elem, ASTNode* v) {
    if (str_elem && !(v->type == AST_LITERAL && v->node_type &&
                      v->node_type->kind == TYPE_STRING)) {
        emit_string_take_owned(gen, v, 1);
    } else {
        generate_expression(gen, v);
    }
}

/* #2755: a `fn` variable a closure writes lives in a cell
 * (emit_promoted_cell_declaration) that holds a reference of its own to the
 * closure in it: its seed and every store take the value (a fresh one
 * adopted, any other retained), a store gives back the one it replaces, and
 * the cell's last holder gives back the last (_aether_cell_release_closure).
 * A call lends the cell's closure with a reference held for the call
 * (arg_drain_select), since the call may rebind the cell; whatever keeps or
 * returns it takes a reference of its own (R3). */
static void emit_closure_cell_value(CodeGenerator* gen, ASTNode* value, const char* text) {
    if (value) emit_closure_take(gen, value);
    else fprintf(gen->output, "_aether_closure_retain(%s)", text);
}

/* The tail of a store into closure cell `name` whose new value is in
 * `_ae_cv`: the cell takes it, then gives back the one it held. */
static void emit_closure_cell_swap(CodeGenerator* gen, const char* name) {
    fprintf(gen->output, "; void* _ae_old = (*%s).env; *%s = _ae_cv; _aether_closure_env_release(_ae_old); }\n",
            name, name);
}

static int type_is_closure_value(Type* t) {
    return t && t->kind == TYPE_FUNCTION && !t->is_fnptr;
}

/* #2755: store into closure cell `name`: the new value is taken before the
 * one it replaces is given back, so a value made from the old one (`f =
 * wrap(f)`) has taken its reference by then. */
static void emit_closure_cell_store(CodeGenerator* gen, const char* name, ASTNode* value) {
    fprintf(gen->output, "{ _AeClosure _ae_cv = ");
    emit_closure_take(gen, value);
    emit_closure_cell_swap(gen, name);
}

/* The same for a closure given as C text, which the cell retains. */
static void emit_closure_cell_store_text(CodeGenerator* gen, const char* name, const char* text) {
    fprintf(gen->output, "{ _AeClosure _ae_cv = _aether_closure_retain(%s)", text);
    emit_closure_cell_swap(gen, name);
}

/* #2795: `value` as the `string?` a cell of that type holds: a string of its
 * own, as a string cell takes one (emit_cell_element_value). A bare string
 * is wrapped and `none` is nothing, written as the holder keeps them, and
 * -1 is returned. An optional is written as it is, and the return value is
 * how its holder takes its string once it has bound it
 * (emit_opt_owner_store): adopted when the value hands it over
 * (is_heap_opt_string_rhs, 1) and copied when it is borrowed (0), through
 * _aether_str_cell_own. Taken inside the value, as a statement expression
 * with a temporary of its own, the plain C path got GNU C (#2885). */
static int emit_opt_string_cell_value(CodeGenerator* gen, ASTNode* value) {
    if (!value || value->type == AST_NONE_LITERAL) {
        fprintf(gen->output, "(ae_opt_string){0}");
        return -1;
    }
    if (!(value->node_type && value->node_type->kind == TYPE_OPTIONAL)) {
        fprintf(gen->output, "(ae_opt_string){ .has = 1, .val = ");
        emit_cell_element_value(gen, 1, value);
        fprintf(gen->output, " }");
        return -1;
    }
    generate_expression(gen, value);
    return is_heap_opt_string_rhs(gen, value) ? 1 : 0;
}

/* #2795: `value` stored into `lvalue`, an optional of type `t` that keeps
 * what it holds: a closure cell (`(*name)`, emit_opt_cell_store) or a
 * module-level `var` (#2800). The value is taken as the optional
 * (emit_optional_coerced), and a `string?` takes a string of its own. A
 * cell, `gives_back`, gives back the one it held, which its last release
 * would otherwise (_aether_cell_release_optstr). `text`, when set, is the value's C text, and
 * `text_own` how the holder takes its string (emit_opt_string_cell_value's
 * answer for it). */
static void emit_opt_owner_store(CodeGenerator* gen, const char* lvalue, Type* t, ASTNode* value,
                                 const char* text, int text_own, int gives_back) {
    const char* tc = cg_intern(get_c_type(t));
    if (strcmp(tc, "ae_opt_string") == 0) {
        fprintf(gen->output, "{ ae_opt_string _ae_nv = ");
        int own = text_own;
        if (text) fprintf(gen->output, "%s", text);
        else own = emit_opt_string_cell_value(gen, value);
        fprintf(gen->output, ";");
        if (own >= 0)
            fprintf(gen->output, " if (_ae_nv.has) _ae_nv.val = _aether_str_cell_own(_ae_nv.val, %d);",
                    own);
        if (gives_back)
            fprintf(gen->output, " ae_opt_string _ae_ov = %s; %s = _ae_nv; "
                    "if (_ae_ov.has) _aether_str_cell_free_val(_ae_ov.val); }\n", lvalue, lvalue);
        else
            fprintf(gen->output, " %s = _ae_nv; }\n", lvalue);
        return;
    }
    fprintf(gen->output, "%s = ", lvalue);
    if (text) fprintf(gen->output, "%s", text);
    else if (value) emit_optional_coerced(gen, value, t);
    else fprintf(gen->output, "(%s){0}", tc);
    fprintf(gen->output, ";\n");
}

static void emit_opt_cell_store(CodeGenerator* gen, const char* name, Type* t, ASTNode* value,
                                const char* text, int text_own) {
    emit_opt_owner_store(gen, cg_internf("(*%s)", name), t, value, text, text_own, 1);
}

/* `name = value` into the cell of the promoted capture `name`, of type `t`:
 * the one shape every store into a cell takes (a reassignment, a match
 * arm). A string cell owns its string and a struct cell its struct's
 * strings (#2458), so each takes a value of its own and gives up the one
 * it held (#2514), and a closure cell its closure (#2755). A whole
 * fixed-size array is stored by emit_cell_array_store. */
static void emit_cell_store(CodeGenerator* gen, const char* name, Type* t, ASTNode* value) {
    const char* ct = t ? get_c_type(t) : NULL;
    const char* sname = struct_owning_strings(gen, t);
    Type* cell_t = promoted_cell_type(gen, name);
    Type* ot = (cell_t && cell_t->kind == TYPE_OPTIONAL) ? cell_t : t;
    if (ot && (ot->kind == TYPE_OPTIONAL || ot->kind == TYPE_TUPLE || is_string_seq_ptr_type(ot)) &&
        !is_opt_string_type(ot) && own_type_owns(gen, ot)) {
        /* #2921: a tuple, optional or string sequence cell takes the value
         * and drops the one it held; the closures sharing it may run
         * anywhere (#2919). */
        emit_owning_store(gen, cg_internf("(*%s)", name), ot, value, 1);
    } else if ((t && t->kind == TYPE_OPTIONAL) || (cell_t && cell_t->kind == TYPE_OPTIONAL)) {
        emit_opt_cell_store(gen, name, (cell_t && cell_t->kind == TYPE_OPTIONAL) ? cell_t : t,
                            value, NULL, -1);   /* #2795 */
    } else if (type_is_closure_value(t) || (value && type_is_closure_value(value->node_type))) {
        emit_closure_cell_store(gen, name, value);
    } else if (ct && strcmp(ct, "const char*") == 0 && cell_t && cell_t->kind == TYPE_PTR) {
        /* #2797: a string into a `ptr` cell (the value is typed as the
         * string it is, as a string and a raw pointer convert freely): the
         * cell takes a string of its own, as a string cell would, and
         * holds its characters (_aether_ptr_cell_set_str). The string
         * handed over as it stood was the closure's environment copy, its
         * header in place of the characters, freed with the environment. */
        fprintf(gen->output, "_aether_ptr_cell_set_str(%s, ", name);
        emit_cell_element_value(gen, 1, value);
        fprintf(gen->output, ");\n");
    } else if (ct && strcmp(ct, "const char*") == 0) {
        fprintf(gen->output, "_aether_str_cell_set_shared(%s, ", name);
        emit_cell_element_value(gen, 1, value);
        fprintf(gen->output, ");\n");
    } else if (sname) {
        /* #2497: a struct owned elsewhere is copied in. */
        fprintf(gen->output, "_aether_store_%s(%s, ", sname, name);
        emit_struct_take(gen, value, sname, NULL);
        fprintf(gen->output, ");\n");
    } else {
        fprintf(gen->output, "*%s = ", name);
        generate_expression(gen, value);
        fprintf(gen->output, ";\n");
    }
}

/* #2474: is `e` an element of a promoted string array (`arr[i]` where a
 * closure writes `arr`)? The cell owns the element and frees it when it is
 * replaced, so a slot that keeps it takes a copy, as it does a struct's
 * string field (is_owned_string_field_read). */
static int is_cell_string_element(CodeGenerator* gen, ASTNode* e) {
    if (!e || e->type != AST_ARRAY_ACCESS || e->child_count < 2) return 0;
    ASTNode* base = e->children[0];
    if (!base || base->type != AST_IDENTIFIER || !base->value ||
        !is_promoted_capture(gen, base->value)) return 0;
    Type* t = base->node_type;
    return t && type_is_sized_array(t) && t->element_type &&
           t->element_type->kind == TYPE_STRING;
}

/* #2474: `arr[i] = v` into a promoted string array. The cell owns its
 * elements, so the store frees the one it replaces and takes a buffer of
 * its own, as a store into a string cell does. So does a local or
 * parameter array that owns its elements (#2618). */
static int emit_cell_string_element_store(CodeGenerator* gen, ASTNode* lhs, ASTNode* rhs) {
    if (!rhs || !(is_cell_string_element(gen, lhs) || is_owning_array_string_element(gen, lhs)))
        return 0;
    fprintf(gen->output, "_aether_str_cell_set(&");
    generate_expression(gen, lhs);
    fprintf(gen->output, ", ");
    emit_cell_element_value(gen, 1, rhs);
    fprintf(gen->output, ");\n");
    return 1;
}

/* #2618: `arr = [...]` or `arr = other` into a local or parameter string
 * array that owns its elements. As for a cell (emit_cell_array_store), every
 * new element is taken into a temporary first, in order, so one that reads
 * the array (`arr = [arr[1], arr[0]]`) takes its value from before the
 * stores; then each store gives back the element it replaces. Another
 * array's elements are shared or copied (_aether_str_elem_dup); elements
 * past a shorter literal's are emptied. */
static void emit_owned_str_array_store(CodeGenerator* gen, ASTNode* stmt, Type* at) {
    ASTNode* src = stmt->children[0];
    if (src && src->type == AST_SLICE_FROM_ARRAY && src->child_count > 0) src = src->children[0];
    int n = at->array_size;
    if (src && src->type == AST_ARRAY_LITERAL) {
        fprintf(gen->output, "{ const char* _ae_sarr[%d] = {0};", n);
        for (int k = 0; k < src->child_count && k < n; k++) {
            fprintf(gen->output, " _ae_sarr[%d] = ", k);
            emit_cell_element_value(gen, 1, src->children[k]);
            fprintf(gen->output, ";");
        }
    } else {
        fprintf(gen->output, "{ const char* const* _ae_src = (const char* const*)(");
        generate_expression(gen, src);
        fprintf(gen->output,
                "); const char* _ae_sarr[%d]; for (int _ae_k = 0; _ae_k < %d; _ae_k++) _ae_sarr[_ae_k] = _aether_str_elem_dup(_ae_src[_ae_k]);",
                n, n);
    }
    fprintf(gen->output,
            " for (int _ae_k = 0; _ae_k < %d; _ae_k++) _aether_str_cell_set(&%s[_ae_k], _ae_sarr[_ae_k]); }\n",
            n, stmt->value);
}

/* #2474: `arr = [...]` where `arr` is a promoted fixed-size array. An array
 * does not assign, so the literal's elements are stored into the cell and
 * the ones past the literal's zeroed, as #2289 does for a plain array. The
 * literal is evaluated whole, in order, into a temporary first, so an
 * element that reads the array (`arr = [arr[1], arr[0]]`) reads the value
 * from before the store. A string array's cell frees each element it
 * replaces. Returns 0 for any other shape. */
static int emit_cell_array_store(CodeGenerator* gen, ASTNode* stmt) {
    if (!stmt || stmt->child_count < 1 || !stmt->value) return 0;
    Type* at = declared_var_type(gen, stmt->value);
    if (!at || !type_is_sized_array(at) || at->array_size <= 0) return 0;
    ASTNode* lit = stmt->children[0];
    if (lit && lit->type == AST_SLICE_FROM_ARRAY && lit->child_count > 0)
        lit = lit->children[0];
    /* #2921: an array of closures, structs, tuples or optionals takes the
     * new value whole and drops the old (a string array keeps its cell
     * path below). */
    if (lit && at->element_type && at->element_type->kind != TYPE_STRING &&
        own_type_owns(gen, at) &&
        (lit->type == AST_ARRAY_LITERAL || is_sized_array_param(lit->node_type))) {
        if (lit->child_count > at->array_size && lit->type == AST_ARRAY_LITERAL) return 0;
        emit_owning_array_store(gen, cg_internf("(*%s)", stmt->value), at, lit);
        return 1;
    }
    if (lit && lit->type != AST_ARRAY_LITERAL && is_sized_array_param(lit->node_type)) {
        /* #2516: another array's elements, copied in. A string array's
         * cell takes a copy of each, as it does a stored element. */
        const char* elem = get_c_type(at->element_type);
        if (strcmp(elem, "const char*") == 0) {
            fprintf(gen->output, "{ const char* const* _ae_src = (const char* const*)(");
            generate_expression(gen, lit);
            fprintf(gen->output,
                    "); for (int _ae_k = 0; _ae_k < %d; _ae_k++) _aether_str_cell_set_shared(&(*%s)[_ae_k], aether_uniform_heap_str(_ae_src[_ae_k], 0)); }\n",
                    at->array_size, stmt->value);
        } else {
            fprintf(gen->output, "memmove(*%s, (const void*)(", stmt->value);
            generate_expression(gen, lit);
            fprintf(gen->output, "), sizeof(*%s));\n", stmt->value);
        }
        return 1;
    }
    if (!lit || lit->type != AST_ARRAY_LITERAL) return 0;
    if (lit->child_count > at->array_size) {
        char msg[300];
        snprintf(msg, sizeof(msg),
                 "array literal of %d elements assigned to '%s', which holds %d: an array keeps the size of its first binding",
                 lit->child_count, stmt->value, at->array_size);
        AetherError e = { stmt->source_file, NULL, stmt->line, stmt->column, msg,
                          NULL, NULL, AETHER_ERR_TYPE_MISMATCH };
        aether_error_report(&e);
        return 1;
    }
    const char* elem = get_c_type(at->element_type);
    int str_elem = strcmp(elem, "const char*") == 0;
    fprintf(gen->output, "{ %s _ae_cell_arr[%d] = {0};", elem, at->array_size);
    for (int k = 0; k < lit->child_count; k++) {
        fprintf(gen->output, " _ae_cell_arr[%d] = ", k);
        emit_cell_element_value(gen, str_elem, lit->children[k]);
        fprintf(gen->output, ";");
    }
    if (str_elem) {
        fprintf(gen->output,
                " for (int _ae_k = 0; _ae_k < %d; _ae_k++) _aether_str_cell_set_shared(&(*%s)[_ae_k], _ae_cell_arr[_ae_k]); }\n",
                at->array_size, stmt->value);
    } else {
        fprintf(gen->output, " memcpy(*%s, _ae_cell_arr, sizeof(_ae_cell_arr)); }\n",
                stmt->value);
    }
    return 1;
}

/* #2516: a fixed-size array parameter (`xs: int[3]`). A fixed-size array
 * is a value, as a struct is: binding one (`a = b`, or passing it) copies
 * its elements. C passes an array as a pointer to its first element, so the
 * signature takes `E _param_xs[N]` and the body starts by copying the
 * caller's elements into an array of its own, `E xs[N]`. A parameter a
 * closure writes is seeded into its cell instead (emit_promoted_param_cell).
 * The signature used to spell `int[3] xs`, which is not a C declarator. */
int is_sized_array_param(Type* t) {
    return type_is_sized_array(t) && t->array_size > 0;
}

void emit_sized_array_param_declarator(CodeGenerator* gen, Type* t, const char* name) {
    fprintf(gen->output, "%s _param_%s[%d]", get_c_type(t->element_type), name, t->array_size);
}

/* #2618: a `string[N]` parameter of the body `scope` that owns its elements
 * (a store into it is not a literal) takes a reference to each of its
 * caller's, so a store gives back its own and never the caller's, and its
 * exit gives them all back. */
void emit_sized_array_param_copy(CodeGenerator* gen, Type* t, const char* name, ASTNode* scope) {
    /* A parameter array a `try` body writes is volatile, as a local one is
     * (#2690), and is copied element by element: memcpy takes no volatile
     * destination. */
    const char* vq = try_volatile_qual_for(gen, name);
    if (vq[0]) {
        fprintf(gen->output, "%s %s[%d]; for (int _ae_k = 0; _ae_k < %d; _ae_k++) %s[_ae_k] = _param_%s[_ae_k];",
                volatile_decl_type(vq, get_c_type(t->element_type)), name, t->array_size,
                t->array_size, name, name);
    } else {
        fprintf(gen->output, "%s %s[%d]; memcpy(%s, _param_%s, sizeof(%s));",
                get_c_type(t->element_type), name, t->array_size, name, name, name);
    }
    if (str_arr_type(t) && string_array_owns(gen, scope, name)) {
        fprintf(gen->output,
                " for (int _ae_k = 0; _ae_k < %d; _ae_k++) %s[_ae_k] = _aether_str_elem_dup(%s[_ae_k]);",
                t->array_size, name, name);
        push_str_array_release_defer(gen, name, t->array_size, 0, 0);
    }
    fprintf(gen->output, "\n");
    /* #2921: an array of other owning elements borrows its caller's, as a
     * struct parameter does; one the body writes into takes references of
     * its own and drops them at its exit. */
    if (!str_arr_type(t) && own_type_owns(gen, t) && struct_param_written(scope, name)) {
        print_indent(gen);
        fprintf(gen->output, "%s((%s*)%s);\n", own_fn(gen, t, "retain"),
                get_c_type(t->element_type), name);
        push_own_drop(gen, name, t, 0, 0);
    }
}

/* The function that gives back one reference to a promoted cell of C type
 * `c_type`; the last holder frees the cell. A string cell owns its heap
 * string and a struct cell the struct's owned string fields, so those free
 * the value first (`_aether_cell_drop_<Name>`, generate_struct_definition); an
 * int / ptr cell is freed as it is. The scope exit and each closure env's
 * destructor both release through here, so they agree. */
const char* promoted_cell_release_fn(CodeGenerator* gen, const char* c_type) {
    if (c_type && strcmp(c_type, "const char*") == 0) return "_aether_cell_release_str";
    /* #2795: a `string?` cell owns its string too. */
    if (c_type && strcmp(c_type, "ae_opt_string") == 0) return "_aether_cell_release_optstr";
    /* #2797: a `ptr` cell, the string a string store gave it. */
    if (c_type && strcmp(c_type, "void*") == 0) return "_aether_cell_release_ptr";
    /* #2755: a closure cell holds a reference to its closure. */
    if (c_type && strcmp(c_type, "_AeClosure") == 0) return "_aether_cell_release_closure";
    /* #2921: and a string sequence cell to its sequence. */
    if (c_type && strcmp(c_type, "StringSeq*") == 0) return "_aether_cell_release_seq";
    /* #2921: a tuple, optional or array that owns drops its parts; a string
     * array cell owns every element (#2474). */
    const char* composite = own_cell_release_fn(c_type);
    if (composite) return composite;
    ASTNode* sdef = (c_type && gen->program && !aether_is_c_import_struct(c_type))
                        ? find_struct_definition_by_name(gen->program, c_type)
                        : NULL;
    if (sdef && struct_owns_heap_strings(gen, sdef)) return cg_internf("_aether_cell_drop_%s", c_type);
    return "_aether_cell_release";
}

void emit_promoted_cell_declaration(CodeGenerator* gen, const char* name,
                                    const char* c_type, Type* var_type,
                                    ASTNode* init_expr, const char* init_text,
                                    int line, int column) {
    if (!c_type || c_type[0] == 0) c_type = "int";
    /* Interned: the caller's spelling may be a buffer the initialiser's own
     * emission below reuses before the release reads the type again. */
    c_type = cg_intern(c_type);
    const char* cell_decl = promoted_cell_pointer(c_type, name);
    const char* cell_type = promoted_cell_pointer(c_type, NULL);
    const char* elem = "";
    /* #2797: a `ptr` cell has a second slot, for the string a string store
     * gives it (_aether_ptr_cell_set_str); zeroed, it holds none. */
    fprintf(gen->output, "%s = (%s)_aether_cell_new(%ssizeof(%s));",
            cell_decl, cell_type, strcmp(c_type, "void*") == 0 ? "2 * " : "", c_type);
    int arr_len = promoted_cell_array_len(c_type, &elem);
    /* No initialiser is the hoisted shape (#2024): the cell is declared
     * ahead of the loop or branch that first assigns it, and the
     * allocation is zero-filled, so the value is defined until then. */
    if (arr_len > 0) {
        /* #2474: an array does not assign. The cell starts zeroed, as the
         * plain array's `= {0}` does, and an array literal's elements are
         * stored into it in order. */
        ASTNode* lit = init_expr;
        if (lit && lit->type == AST_SLICE_FROM_ARRAY && lit->child_count > 0)
            lit = lit->children[0];
        if (!init_expr && init_text) {
            /* #2516: seeded from an array (a parameter's elements). A
             * string cell owns its elements, so it takes a reference to
             * each, as a string parameter's cell does. */
            if (strcmp(elem, "const char*") == 0)
                fprintf(gen->output, " for (int _ae_k = 0; _ae_k < %d; _ae_k++) (*%s)[_ae_k] = aether_str_capture((%s)[_ae_k]);",
                        arr_len, name, init_text);
            else
                fprintf(gen->output, " memcpy(*%s, %s, sizeof(*%s));", name, init_text, name);
        } else if (lit && lit->type == AST_ARRAY_LITERAL && lit->child_count > arr_len) {
            char msg[300];
            snprintf(msg, sizeof(msg),
                     "array literal of %d elements initialises '%s', which holds %d",
                     lit->child_count, name, arr_len);
            AetherError e = { lit->source_file, NULL, line, column, msg,
                              NULL, NULL, AETHER_ERR_TYPE_MISMATCH };
            aether_error_report(&e);
        } else if (lit && lit->type == AST_ARRAY_LITERAL) {
            int str_elem = strcmp(elem, "const char*") == 0;
            for (int k = 0; k < lit->child_count; k++) {
                fprintf(gen->output, " (*%s)[%d] = ", name, k);
                emit_cell_element_value(gen, str_elem, lit->children[k]);
                fprintf(gen->output, ";");
            }
        }
    } else if (init_expr || init_text) {
        fprintf(gen->output, " *%s = ", name);
        if (strcmp(c_type, "_AeClosure") == 0) {
            /* #2755: a closure cell takes a reference of its own, the
             * caller's argument retained rather than borrowed. */
            emit_closure_cell_value(gen, init_expr, init_text);
        } else if (init_expr && strcmp(c_type, "const char*") == 0) {
            /* #2461, #2514: a string cell frees what it holds. */
            emit_cell_element_value(gen, 1, init_expr);
        } else if (strcmp(c_type, "StringSeq*") == 0) {
            /* #2921: a string sequence cell holds a reference of its own. */
            if (init_expr) emit_seq_take(gen, init_expr);
            else fprintf(gen->output, "string_seq_retain(%s)", init_text);
        } else if (init_expr) {
            generate_expression(gen, init_expr);
        } else {
            fprintf(gen->output, "%s", init_text);
        }
        fprintf(gen->output, ";");
    }
    fprintf(gen->output, "\n");
    /* #2474: an array cell records its type, which a later whole-array
     * store reads its length and element type from (as #2289's does). */
    if (arr_len > 0 && var_type && type_is_sized_array(var_type))
        mark_var_declared_typed(gen, name, var_type);
    else
        mark_var_declared(gen, name);
    /* The cell is journaled while in scope (#2923), and its scope exit
     * gives back this scope's reference (try_emit_cell_release). */
    const char* release_fn = promoted_cell_release_fn(gen, c_type);
    emit_unwind_track_call(gen, cg_internf("aether_unwind_track_place((const void*)&%s, NULL, %s)",
                                           name, release_fn));
    ASTNode* carrier = create_ast_node(AST_EXPRESSION_STATEMENT, NULL, line, column);
    if (carrier->annotation) free(carrier->annotation);
    carrier->annotation = heap_strf("cell_release:%s:%s", release_fn, name);
    codegen_own_node(gen, carrier);
    push_defer(gen, carrier);
}

int try_emit_cell_release(CodeGenerator* gen, ASTNode* deferred) {
    const char* a = deferred ? deferred->annotation : NULL;
    if (!a || strncmp(a, "cell_release:", 13) != 0) return 0;
    const char* fn = a + 13;
    const char* name = strchr(fn, ':') + 1;
    print_indent(gen);
    fprintf(gen->output, "aether_unwind_forget((const void*)&%s); %.*s(%s);\n", name,
            (int)(name - 1 - fn), fn, name);
    return 1;
}

/* A string cell owns the refcounted string it holds: overwriting it or the
 * last release gives that reference back (_aether_str_cell_set /
 * _aether_cell_release_str). A parameter's string is borrowed from the
 * caller, so seeding the cell with it as it stands made the first write in
 * a closure, or the scope exit, free the caller's string (#2463). The cell
 * takes a reference of its own. So does a closure cell (#2755,
 * emit_closure_cell_value): the caller's closure stays the caller's to
 * release. */
void emit_promoted_param_cell(CodeGenerator* gen, const char* name,
                              const char* c_type, const char* param_cname,
                              int line, int column) {
    const char* init = param_cname;
    if (c_type && strcmp(c_type, "const char*") == 0) {
        /* aether_str_capture: a refcounted string is retained and a plain
         * buffer copied, so the cell never holds the caller's pointer
         * (#2499 relies on it: a caller may free a plain heap argument
         * after a closure call). */
        init = cg_internf("aether_str_capture(%s)", param_cname);
    }
    emit_promoted_cell_declaration(gen, name, c_type, NULL, NULL, init, line, column);
    if (c_type && strcmp(c_type, "ae_opt_string") == 0) {
        /* #2795: a `string?` cell owns its string too: a refcounted one
         * shared through a reference of its own, anything else copied. A
         * statement once the cell holds the parameter (#2885): taken inside
         * its initializer, with a temporary of its own, it was a GNU
         * statement expression on the plain C path. */
        print_indent(gen);
        fprintf(gen->output, "if ((*%s).has) (*%s).val = _aether_str_elem_dup((*%s).val);\n",
                name, name, name);
    }
}

/* Push a function-exit release for every hoisted string local (issue #420
 * follow-up, R2): a local owns what it holds, so its scope exit releases it
 * unconditionally. The defer carries an annotation
 * `"heap_string_exit_free:<name>"` that emit_defers_* recognise
 * (try_emit_heap_string_exit_free). Each local is also journaled here, so a
 * panic releases what it holds. */
void push_heap_string_exit_free_defers(CodeGenerator* gen, ASTNode* body) {
    if (!gen || !body) return;
    if (gen->heap_string_var_count <= 0) return;
    for (int i = 0; i < gen->heap_string_var_count; i++) {
        const char* name = gen->heap_string_vars[i];
        if (!name) continue;
        /* Skip closure-env vars and promoted captures — they have
         * their own defer-free shapes via the existing closure /
         * promoted-cell paths (see claim_closure_local_env and
         * emit_promoted_cell_declaration below). Adding a heap-
         * string-exit defer on top would emit a free on a name
         * that doesn't live as a `const char*` at function
         * scope. */
        int is_env_cap = 0;
        for (int e = 0; e < gen->current_env_capture_count; e++) {
            if (gen->current_env_captures[e] &&
                strcmp(gen->current_env_captures[e], name) == 0) {
                is_env_cap = 1; break;
            }
        }
        if (is_env_cap) continue;
        if (is_promoted_capture(gen, name)) continue;
        /* Build the defer carrier: an AST_EXPRESSION_STATEMENT
         * whose annotation encodes `heap_string_exit_free:<name>`.
         * The body is empty — emit_defers_for_scope and
         * emit_all_defers pick the annotation up and
         * emit the release directly without descending
         * into the body. */
        ASTNode* carrier = create_ast_node(AST_EXPRESSION_STATEMENT, NULL,
                                            body->line, body->column);
        if (carrier) {
            if (carrier->annotation) free(carrier->annotation);
            carrier->annotation = heap_strf("heap_string_exit_free:%s", name);
            codegen_own_node(gen, carrier);
            push_defer(gen, carrier);
        }
        /* #2923: the place is journaled as its exit free is armed, at the
         * frame it is declared in, so a panic leaving that frame releases
         * what it holds then, and one caught inside leaves it to the local. */
        print_indent(gen);
        fprintf(gen->output, "aether_unwind_track_str_place((const void*)&%s, NULL);\n", name);
    }
}

// Collect the names of top-level AST_VARIABLE_DECLARATION nodes in a
// block. Used by the if/else hoist below to find variables that are
// first-assigned in BOTH branches — those need to be visible after the
// `if`, so we declare them at the outer scope before opening the if.
//
// Pulls only direct children (not nested blocks) since a name introduced
// inside a deeper `while` of the then-branch should NOT escape to the
// post-if scope.
// When both arms of an if/else first-assign the same variable name,
// hoist a single declaration to the enclosing scope so the post-block
// code can read it. Without this, both arms emit a C-local declaration
// and the variable goes out of scope at the closing `}`. See
// docs/notes/compiler_notes_from_vcr_port.md item #2 for the original
// repro and rationale.
//
// Names that appear in only one arm are deliberately NOT hoisted —
// using such a name after the if would be undefined behavior at the
// Aether level, and the existing scope-restore in AST_IF_STATEMENT
// keeps that locality. Names already declared before the if are also
// skipped (they're already in scope).
/* Emit a hoisted C local declaration `<type> <name>;` that is correct
 * for ARRAY types too. get_c_type(TYPE_ARRAY) returns "T[N]", which is
 * a valid type spelling only in the postfix-declarator position
 * (`T name[N]`), NOT as a prefix (`T[N] name;` is invalid C). The
 * loop/branch var hoisters below pre-declare a var as `<c_type> <name>;`
 * — fine for scalars/pointers, but for a `byte[8]`-style fixed array it
 * produced `unsigned char[8] name;`. Split the array case out so it
 * emits `elem name[N];`. (Hit repeatedly by the Redis port's per-loop
 * scratch buffers; the first-statement-in-block decl path already does
 * this correctly, this fixes the not-first / hoisted path.) */
/* The zero initializer for a hoisted local. A hoisted variable is
 * declared at function scope and assigned inside the branch or loop
 * body that binds it; on a path that skips the body its value is
 * indeterminate, and reading an indeterminate value is undefined — not
 * a garbage value, undefined: gcc's interprocedural constant
 * propagation may treat it as whatever the other call sites pass (a
 * literal 0), so a parameter that "genuinely varies" arrives as a
 * constant, with no sanitizer able to say so (#2128 is the shape of that
 * report). Initialized, the variable is 0/NULL/{0} on such a path and
 * every read is defined. Spelled per kind rather than as a universal
 * `{0}`, which clang flags on scalars (-Wbraced-scalar-init). */
static const char* hoisted_zero_init(Type* t, const char* c_type) {
    if (t) {
        switch (t->kind) {
            case TYPE_INT: case TYPE_INT64: case TYPE_UINT64: case TYPE_UINT32:
            case TYPE_UINT16: case TYPE_UINT8: case TYPE_BYTE: case TYPE_BOOL:
            case TYPE_FLOAT: case TYPE_FLOAT32: case TYPE_LONGDOUBLE:
            case TYPE_DURATION: case TYPE_ENUM:
            case TYPE_BITSET: case TYPE_BITSTRUCT:   /* backing integers */
                return " = 0";
            case TYPE_ISOLATED:
                return hoisted_zero_init(t->element_type, c_type);
            case TYPE_F32X4: case TYPE_F64X2: case TYPE_I32X4: case TYPE_I64X2:
            case TYPE_I16X8: case TYPE_F32X8: case TYPE_I32X8:
                return " = {0}";   /* #2146: a vector, not a scalar 0 */
            case TYPE_PTR: case TYPE_STRING: case TYPE_ACTOR_REF:
                return " = NULL";
            case TYPE_FUNCTION:
                return t->is_fnptr ? " = NULL" : " = {0}";
            default:
                break;
        }
    }
    size_t n = c_type ? strlen(c_type) : 0;
    if (n && c_type[n - 1] == '*') return " = NULL";
    return " = {0}";
}

/* #2124: a local first bound inside a branch or loop body is hoisted to
 * function scope, one variable for every binding of the name in the
 * function. Its declared type is the numeric join of those bindings
 * (`int` here, `long` there: `int64_t`), so none of them narrows and the
 * type matches what the early inference pass records for a use after
 * the branches. Non-numeric kinds keep `seed` (a clash is reported at
 * the binding). Nested closures are their own functions and are not
 * walked. Returns a fresh Type, or NULL when nothing widened `seed`. */
static Type* sibling_join_type(CodeGenerator* gen, const char* name, Type* seed) {
    if (!gen->hoist_scope_body) return NULL;
    return hoist_join_type(gen->hoist_scope_body, name, seed);
}

/* #2124: the kinds a number local holds; a pointer cannot flow into one
 * (nor one into a pointer) whatever is_type_compatible says for the C
 * interop cases. */
static int rebind_is_number(TypeKind k) {
    return k == TYPE_INT || k == TYPE_INT64 || k == TYPE_UINT64 || k == TYPE_UINT32 ||
           k == TYPE_UINT16 || k == TYPE_UINT8 || k == TYPE_BYTE || k == TYPE_BOOL ||
           k == TYPE_FLOAT || k == TYPE_FLOAT32 || k == TYPE_LONGDOUBLE;
}

/* The C declaration of a local hoisted out of the loop or the branches that
 * bind it, zeroed, with the scope-exit release of what it owns; `line` and
 * `column` place that release. */
static void emit_hoisted_local_decl(CodeGenerator* gen, Type* var_type,
                                     const char* name, int line, int column) {
    /* #2690: volatile where a `try` body writes it, as at its binding. */
    const char* vq = try_volatile_qual_for(gen, name);
    if (var_type && var_type->kind == TYPE_ARRAY && var_type->array_size > 0) {
        const char* elem = volatile_decl_type(vq, get_c_type(var_type->element_type));
        fprintf(gen->output, "%s %s[%d] = {0};\n", elem, name, var_type->array_size);
        /* #2618: one array for every binding of the name, released where
         * it is declared. */
        if (str_arr_type(var_type) && string_array_owns(gen, gen->hoist_scope_body, name) &&
            !is_promoted_capture(gen, name))
            push_str_array_release_defer(gen, name, var_type->array_size, line, column);
        else if (!str_arr_type(var_type) && own_type_owns(gen, var_type) &&
                 !is_promoted_capture(gen, name))
            push_own_drop(gen, name, var_type, line, column);   /* #2921 */
        return;
    }
    if (composite_local_type(gen, var_type) && !is_promoted_capture(gen, name)) {
        fprintf(gen->output, "%s %s = {0};\n",
                volatile_decl_type(vq, get_c_type(var_type)), name);
        push_own_drop(gen, name, var_type, line, column);   /* #2921 */
        return;
    }
    const char* c_type = get_c_type(var_type);
    fprintf(gen->output, "%s %s%s;\n", volatile_decl_type(vq, c_type), name,
            hoisted_zero_init(var_type, c_type));
    /* A struct that owns strings is released where it is declared, as at
     * its binding: each binding replaces the zeroed value (the local is
     * declared already), so nothing else destroyed it, and the strings it
     * held at the end leaked. */
    push_struct_destroy_defer(gen, name, var_type, line, column);
}

/* Declare, before an if/else or a switch with a default, the names every arm
 * binds (hoist_common_decl_names, shared with the typechecker), so the
 * binding survives the arm (#2732 added the switch). */
static void hoist_common_vars(CodeGenerator* gen, ASTNode* stmt) {
    const char* names[HOIST_MAX_NAMES];
    int count = hoist_common_decl_names(stmt, names, HOIST_MAX_NAMES);

    for (int i = 0; i < count; i++) {
        const char* n = names[i];

        // Skip if already declared at outer scope.
        if (is_var_declared(gen, n)) continue;
        /* #744: never shadow a module-level `var` global with a hoisted
         * local — the write is routed to the file-scope static by the
         * variable-declaration emitter. */
        if (is_module_global_var(gen, n)) continue;
        /* #2505: nor an actor's state field, which `n` writes in a handler. */
        if (is_actor_state_var(gen, n)) continue;

        // Recover a usable type from the first arm that has one.
        ASTNode* decl = hoist_common_first_decl(stmt, n);
        Type* var_type = hoist_decl_type(decl);
        Type* joined = sibling_join_type(gen, n, var_type);
        if (joined) var_type = joined;
        print_indent(gen);
        /* #2024: a mutated capture is hoisted as its cell, not as a plain
         * value -- see hoist_loop_vars. */
        if (is_promoted_capture(gen, n)) {
            emit_promoted_cell_declaration(gen, n, get_c_type(var_type), var_type, NULL, NULL,
                                           decl ? decl->line : stmt->line,
                                           decl ? decl->column : stmt->column);
            if (joined) free_type(joined);
            continue;
        }
        mark_var_declared_typed(gen, n, var_type);
        emit_hoisted_local_decl(gen, var_type, n, 0, 0);
        if (joined) free_type(joined);
    }
}

// Pre-declare variables from a while/for loop body so they're visible
// at function scope in the generated C. Without this, variables first
// assigned inside a while block are C-block-scoped and invisible to
// subsequent while blocks in the same function.
/* One declaration hoist_loop_vars declares before its while (the order
 * and the set come from hoist_loop_decls, shared with the typechecker). */
static void hoist_loop_var(ASTNode* child, void* user) {
    CodeGenerator* gen = (CodeGenerator*)user;
    /* #744: don't hoist a module-level `var` global as a loop-
     * scoped local — it would shadow the file-scope static. */
    /* #2505: nor an actor's state field: `kept = kept + 1` in a handler's
     * loop writes self->kept, and a hoisted local of that name shadowed it
     * for the loop's closed form. */
    if (!is_var_declared(gen, child->value) &&
        !is_module_global_var(gen, child->value) &&
        !is_actor_state_var(gen, child->value)) {
        // Determine type
        Type* var_type = child->node_type;
        if ((!var_type || var_type->kind == TYPE_VOID || var_type->kind == TYPE_UNKNOWN)
            && child->child_count > 0 && child->children[0] && child->children[0]->node_type) {
            var_type = child->children[0]->node_type;
        }
        Type* joined = sibling_join_type(gen, child->value, var_type);
        if (joined) var_type = joined;
        const char* c_type = get_c_type(var_type);
        print_indent(gen);
        /* #2024: a variable a closure mutates lives in a heap
         * cell, not a plain value, and every later write to it
         * is `*name = ...`. Hoisting it as `T name;` declared
         * the wrong thing and the first assignment in the body
         * then dereferenced an int. The cell is hoisted instead
         * -- same scope as any other hoisted loop variable, so
         * one cell across iterations, and its release is queued
         * at this scope's exit like a first assignment would. */
        if (is_promoted_capture(gen, child->value)) {
            emit_promoted_cell_declaration(gen, child->value, c_type, var_type, NULL, NULL,
                                           child->line, child->column);
            if (joined) free_type(joined);
            return;
        }
        mark_var_declared_typed(gen, child->value, var_type);
        /* Zero-initialize struct hoists so the first-iteration
         * struct-destroy call (#465) sees zero fields instead
         * of stack-uninitialised garbage.
         * Without this, the first `b = Box { ... }` inside the
         * loop body runs `Box_destroy(&b)` on uninit memory
         * and may free a garbage pointer. The {0} initialiser
         * is C99-portable and a no-op for non-struct types
         * either (the C compiler does the right thing). */
        if (var_type && var_type->kind == TYPE_STRUCT) {
            /* #2690: volatile where a `try` body writes it, as at its
             * binding. */
            fprintf(gen->output, "%s %s = {0};\n",
                    volatile_decl_type(try_volatile_qual_for(gen, child->value), c_type),
                    child->value);
            /* Push the function-exit struct-destroy defer
             * here too — the in-loop reassignment path
             * doesn't run the first-declaration codegen
             * that normally pushes the defer (the var is
             * already-declared via this hoist). Without
             * this, the final loop-iteration's heap fields
             * never get reclaimed at function exit. */
            push_struct_destroy_defer(gen, child->value, var_type, child->line, child->column);
        } else {
            emit_hoisted_local_decl(gen, var_type, child->value, 0, 0);
            /* #2480: the hoisted local is the loop's one C variable, so the
             * env free is queued here, outside the loop; each iteration's
             * rebinding frees the env it replaces. */
        }
        if (joined) free_type(joined);
    }
}

static void hoist_loop_vars(CodeGenerator* gen, ASTNode* body) {
    hoist_loop_decls(body, hoist_loop_var, gen);
}

/* ---- #2378: threaded dispatch for interpreter loops -----------------------
 *
 *     while true {
 *         <head statements>
 *         switch <selector> { case ...: { ... continue } ... }
 *         <tail statements>
 *     }
 *
 * is an interpreter's dispatch loop. Where the C compiler has labels-as-values
 * (GCC, Clang, MinGW, Emscripten), each arm gets a C label and a static table
 * maps a selector value to it, and a `continue` that targets the loop runs the
 * head again and jumps straight to the next arm (`goto *table[sel]`) instead of
 * going round the loop. Each arm then ends in its own indirect branch, which
 * predicts far better than the one shared jump a `switch` compiles to, and the
 * loop head drops out of the hot path. Actor message dispatch already lowers
 * this way (codegen_actor.c).
 *
 * Correctness never depends on the table. The loop and the `switch` are
 * emitted exactly as before; the labels and the table are added under
 * AE_TD_GUARD, and a selector with no table entry jumps back to the `switch`,
 * which handles it as it always did. Without labels-as-values (MSVC), or with
 * AETHER_NO_THREADED_DISPATCH defined, a `continue` is the plain `continue;` of
 * today's output. An arm that falls off the end of the `switch` also goes round
 * the loop as before, so a loop that only partly matches the shape is still
 * correct, just less threaded.
 *
 * The tail runs only when an arm breaks or falls off, or nothing matches, as
 * in the plain loop, and is emitted once, so it is unrestricted (interpreters
 * put their slow-path exit and unknown-opcode error there).
 *
 * The shape is recognised conservatively: the head is re-emitted at every
 * `continue` site, so it may only assign scalar values (no strings, closures or
 * interpolation, whose ownership tracking is per-site); the selector must be an
 * integer; and every case value must be an integer known at compile time,
 * between 0 and AE_TD_MAX_TABLE, since it is a table index. Anything else
 * lowers exactly as today. */
static int find_labeled_loop_level(CodeGenerator* gen, const char* name);

#define AE_TD_MAX_TABLE 4096
#define AE_TD_GUARD "#if (defined(__GNUC__) || defined(__clang__)) && !defined(AETHER_NO_THREADED_DISPATCH)"

static int td_type_is_int(Type* t) {
    if (!t) return 0;
    switch (t->kind) {
        case TYPE_INT: case TYPE_INT64: case TYPE_UINT64: case TYPE_UINT32:
        case TYPE_UINT16: case TYPE_UINT8: case TYPE_BYTE: case TYPE_ENUM:
            return 1;
        default:
            return 0;
    }
}

/* No closure, interpolation, value-producing block, or string anywhere in e. */
static int td_tree_is_plain(ASTNode* e) {
    if (!e) return 1;
    switch (e->type) {
        case AST_CLOSURE: case AST_STRING_INTERP: case AST_IF_EXPRESSION:
        case AST_MATCH_STATEMENT: case AST_BLOCK: case AST_DEFER_STATEMENT:
        case AST_TRY_STATEMENT:
            return 0;
        default:
            break;
    }
    if (e->node_type && e->node_type->kind == TYPE_STRING) return 0;
    for (int i = 0; i < e->child_count; i++) {
        if (!td_tree_is_plain(e->children[i])) return 0;
    }
    return 1;
}

/* The case values of one arm, appended to vals/arms. 0 when one is not a
 * compile-time integer in [0, AE_TD_MAX_TABLE). */
static int td_case_values(CodeGenerator* gen, ASTNode* sel, int arm,
                          int64_t* vals, int* arms, int* n, int cap) {
    if (!sel) return 0;
    if (sel->type == AST_MATCH_ALT) {
        for (int a = 0; a < sel->child_count; a++) {
            if (!td_case_values(gen, sel->children[a], arm, vals, arms, n, cap)) return 0;
        }
        return 1;
    }
    int64_t v;
    if (!contract_eval_int64(sel, gen->program, &v)) return 0;
    if (v < 0 || v >= AE_TD_MAX_TABLE || *n >= cap) return 0;
    vals[*n] = v;
    arms[*n] = arm;
    (*n)++;
    return 1;
}

/* 1 when some `continue` in `n` targets the loop: an unlabeled one not
 * inside a nested loop, or one naming `label` at any depth. Closures are not
 * searched: a continue cannot leave one. */
static int td_has_continue(ASTNode* n, const char* label, int nested) {
    if (!n) return 0;
    if (n->type == AST_CLOSURE) return 0;
    if (n->type == AST_CONTINUE_STATEMENT) {
        if (n->value) return label && strcmp(n->value, label) == 0;
        return !nested;
    }
    int inner = nested;
    if (n->type == AST_WHILE_LOOP || n->type == AST_FOR_LOOP) inner = 1;
    for (int i = 0; i < n->child_count; i++) {
        if (td_has_continue(n->children[i], label, inner)) return 1;
    }
    return 0;
}

/* 1 while the loop head is being emitted (the loop's own pass, or a copy at
 * a continue site). A `continue` in the head then stays a plain `continue;`
 * instead of expanding into another copy of the head, which would recurse;
 * inside a copy it still leaves the switch for the loop, so it is correct. */
static int g_td_in_head = 0;

/* A head statement can be copied to every continue site: no closures,
 * interpolation, strings, defers, try, nested loops or switches, and no
 * `break` (in a copy, which sits inside the switch, it would leave the switch
 * instead of the loop). `if` statements and `continue` are fine. */
static int td_head_ok(ASTNode* n) {
    if (!n) return 1;
    switch (n->type) {
        case AST_CLOSURE: case AST_STRING_INTERP: case AST_IF_EXPRESSION:
        case AST_MATCH_STATEMENT: case AST_DEFER_STATEMENT: case AST_TRY_STATEMENT:
        case AST_WHILE_LOOP: case AST_FOR_LOOP: case AST_SWITCH_STATEMENT:
        case AST_BREAK_STATEMENT:
            return 0;
        default:
            break;
    }
    if (n->node_type && n->node_type->kind == TYPE_STRING) return 0;
    for (int i = 0; i < n->child_count; i++) {
        if (!td_head_ok(n->children[i])) return 0;
    }
    return 1;
}

/* Index of the loop body's dispatch switch: its first top-level `switch`, or
 * -1. Statements before it are the head; statements after it are the tail,
 * which runs only when an arm breaks or falls off, or no arm matches, exactly
 * as in the plain loop, so it is emitted once and is not restricted. */
static int td_switch_index(ASTNode* body) {
    if (!body || body->type != AST_BLOCK) return -1;
    for (int i = 0; i < body->child_count; i++) {
        if (body->children[i] && body->children[i]->type == AST_SWITCH_STATEMENT) return i;
    }
    return -1;
}

/* AETHER_EXPLAIN_THREADED=1: say why a loop that looks like a dispatch loop
 * (`while <always true> { ...; switch ... }`) was not threaded. */
static void td_explain(ASTNode* loop, const char* why) {
    static int on = -1;
    if (on < 0) {
        const char* v = getenv("AETHER_EXPLAIN_THREADED");
        on = (v && *v && strcmp(v, "0") != 0) ? 1 : 0;
    }
    if (!on || !loop || loop->child_count < 2 || !loop->children[1]) return;
    /* Neither the while nor the switch node carries a position; the
     * switch's selector expression does. */
    ASTNode* body = loop->children[1];
    int si = td_switch_index(body);
    ASTNode* at = si >= 0 ? body->children[si] : loop;
    if (at->child_count > 0 && at->children[0] && at->children[0]->line > 0) at = at->children[0];
    fprintf(stderr, "%s:%d: note: dispatch loop not threaded: %s\n",
            at->source_file ? at->source_file : "?", at->line, why);
}

/* The loop's switch when `loop` has the threaded shape, else NULL. */
static ASTNode* td_loop_switch(CodeGenerator* gen, ASTNode* loop) {
    if (!loop || loop->child_count < 2) return NULL;
    ASTNode* cond = loop->children[0];
    ASTNode* body = loop->children[1];
    int si = td_switch_index(body);
    if (si < 0) return NULL;
    ASTNode* sw = body->children[si];
    if (sw->child_count < 2) return NULL;
    /* An always-true condition: `true`, `1`, `1 == 1`, a true const. */
    ContractEnv env;
    memset(&env, 0, sizeof env);
    env.program = gen->program;
    if (!cond || contract_eval_predicate(cond, &env) != CONTRACT_TRUE) return NULL;
    /* From here the loop has the shape; anything that stops it is worth saying. */
    ASTNode* sel = sw->children[0];
    if (!sel || !td_type_is_int(sel->node_type)) {
        td_explain(loop, "the switch selector is not an integer");
        return NULL;
    }
    if (!td_tree_is_plain(sel)) {
        td_explain(loop, "the switch selector is not a plain expression");
        return NULL;
    }
    for (int i = 0; i < si; i++) {
        if (!td_head_ok(body->children[i])) {
            td_explain(loop, "a statement before the switch cannot be repeated at each continue "
                             "(it has a string, closure, defer, try, nested loop, switch or break)");
            return NULL;
        }
    }
    int64_t vals[512];
    int arms[512];
    int n = 0;
    for (int i = 1; i < sw->child_count; i++) {
        ASTNode* c = sw->children[i];
        if (!c || c->type != AST_CASE_STATEMENT) return NULL;
        if (c->value && strcmp(c->value, "default") == 0) continue;
        if (c->child_count < 1 || selector_has_range(c->children[0])) {
            td_explain(loop, "a case is a range");
            return NULL;
        }
        if (!td_case_values(gen, c->children[0], i, vals, arms, &n, 512)) {
            td_explain(loop, "a case value is not an integer constant in 0..4095 known at compile time");
            return NULL;
        }
    }
    if (n == 0) return NULL;
    /* Threading pays off only through a `continue` that targets this loop,
     * and without one the table and labels would go unreferenced. */
    for (int i = 1; i < sw->child_count; i++) {
        if (td_has_continue(sw->children[i], loop->value, 0)) return sw;
    }
    td_explain(loop, "no arm continues the loop");
    return NULL;
}

/* The static table: one entry per case value, pointing at its arm's label. */
static void td_emit_table(CodeGenerator* gen, ASTNode* sw, int id) {
    int64_t vals[512];
    int arms[512];
    int n = 0;
    int64_t max = 0;
    for (int i = 1; i < sw->child_count; i++) {
        ASTNode* c = sw->children[i];
        if (c->value && strcmp(c->value, "default") == 0) continue;
        td_case_values(gen, c->children[0], i, vals, arms, &n, 512);
    }
    for (int k = 0; k < n; k++) {
        if (vals[k] > max) max = vals[k];
    }
    print_line(gen, AE_TD_GUARD);
    print_line(gen, "int64_t _ae_td_sel_%d = 0;", id);
    print_indent(gen);
    fprintf(gen->output, "static void* const _ae_td_tbl_%d[%lld] = {", id, (long long)(max + 1));
    for (int k = 0; k < n; k++) {
        fprintf(gen->output, "%s[%lld] = &&_ae_td_%d_arm%d", k ? ", " : " ",
                (long long)vals[k], id, arms[k]);
    }
    fprintf(gen->output, " };\n");
    print_line(gen, "#endif");
}

/* What a `continue` targeting threaded loop `level` becomes: the loop-head
 * checks, the head statements, the selector, then a jump to the next arm (or
 * back to the switch when the value has no entry). `fallback` is the line the
 * continue lowers to without labels-as-values. */
static void td_emit_dispatch(CodeGenerator* gen, int level, const char* fallback) {
    ASTNode* loop = gen->loop_td_node[level];
    int id = gen->loop_td_id[level];
    ASTNode* body = loop->children[1];
    int si = td_switch_index(body);
    ASTNode* sw = body->children[si];
    print_line(gen, AE_TD_GUARD);
    print_line(gen, "{");
    indent(gen);
    if (gen->preempt_loops) {
        print_line(gen, "if (--_aether_reductions <= 0) { _aether_reductions = 10000; sched_yield(); }");
    }
    if (gen->emit_lib) {
        print_line(gen, "if (aether_caps_armed && aether_caps_deadline_tripped()) { __aether_abort_call(); goto _ae_td_exit_%d; }", id);
    }
    g_td_in_head++;
    for (int i = 0; i < si; i++) {
        generate_statement(gen, body->children[i]);
    }
    g_td_in_head--;
    print_indent(gen);
    fprintf(gen->output, "_ae_td_sel_%d = (int64_t)(", id);
    generate_expression(gen, sw->children[0]);
    fprintf(gen->output, ");\n");
    print_line(gen, "if ((uint64_t)_ae_td_sel_%d < (uint64_t)(sizeof(_ae_td_tbl_%d) / sizeof(_ae_td_tbl_%d[0])) && _ae_td_tbl_%d[_ae_td_sel_%d]) goto *_ae_td_tbl_%d[_ae_td_sel_%d];",
               id, id, id, id, id, id, id);
    print_line(gen, "goto _ae_td_sw_%d;", id);
    unindent(gen);
    print_line(gen, "}");
    print_line(gen, "#else");
    print_line(gen, "%s", fallback);
    print_line(gen, "#endif");
}

/* The threaded loop a `continue` targets, as its loop level, or -1. */
static int td_continue_target(CodeGenerator* gen, ASTNode* stmt) {
    if (g_td_in_head) return -1;
    int lvl;
    if (stmt->value) {
        lvl = find_labeled_loop_level(gen, stmt->value);
    } else {
        lvl = gen->loop_nest_depth - 1;
    }
    if (lvl < 0 || lvl >= AETHER_MAX_LOOP_NEST) return -1;
    return gen->loop_td_id[lvl] ? lvl : -1;
}

// Pre-hoist variables first-declared inside if-statement branches at
// the enclosing function-body scope, when:
//   (a) the variable is referenced *outside* (after) the if-block, and
//   (b) the existing hoist_common_vars hasn't already handled
//       it (which only fires when both branches declare the variable
//       and they have a common else).
//
// Without this, a sequence like
//
//     if cond1 { x = ... }
//     if cond2 { x = ... }
//     return x
//
// emits C where each branch C-scopes `x` inside its own `{ ... }`,
// and the function-scope `return x` can't see it. Closes #278.
//
// This is over-hoisting: any variable first-written inside any if
// gets a function-scope declaration. Harmless in C (just a tentative
// definition); the inner branches' `Type x = expr` becomes an
// assignment to the outer-scope `x`. The codegen's existing
// is_var_declared check skips re-declaration in the inner branch.

// ============================================================
// Issue #348 — Eiffel-style `requires` / `ensures` contracts.
// ============================================================
//
// The parser attaches each clause as an AST_REQUIRES_CLAUSE or
// AST_ENSURES_CLAUSE child of AST_FUNCTION_DEFINITION; the predicate
// expression is the clause node's single child. Codegen lowers each
// to an `if (!(<expr>)) aether_panic(...)` shaped check at the right
// scope:
//
//   `requires`  → emitted at function entry, after parameters are
//                  declared and before any user code runs.
//                  Parameters are in scope.
//
//   `ensures`   → emitted before every `return <expr>;` site,
//                  wrapped in a C block scope `{ <T> result = <expr>;
//                  ... return result; }` so the predicate's `result`
//                  identifier resolves to a fresh local that holds
//                  the about-to-be-returned value. Each return site
//                  gets its own copy of every check; partial-return
//                  paths through if/else / match all stay correct.
//
// `--no-contracts` (CodeGenerator::no_contracts) skips emission
// entirely — the per-call cost goes to zero, mirroring C's
// `-DNDEBUG` for assert.
//
// Diagnostic message format:
//
//   precondition violation: <predicate-text> in <fn-name>
//   postcondition violation: <predicate-text> in <fn-name>
//
// `<predicate-text>` comes from a small reverse-printer
// (`fprint_expr_text`) that round-trips the AST back to source-like
// form. It's intentionally simple — covers identifiers, literals,
// binary/unary ops, member access, function calls — so the panic
// message names the specific failed predicate even when a function
// has multiple clauses. Anything the round-tripper doesn't handle
// falls through to the literal string `"<expr>"`, which is still
// disambiguated by the surrounding "<predicate-text> in <fn-name>"
// line+column info from the panic stack trace (issue #347).
// Emit one `if (!(<predicate>)) aether_panic("<role> violation: <text>
// in <fn>");` block for a single clause. If the predicate is
// provably constant-true at compile time, skip emission entirely
// (zero per-call cost — analog of `static_assert` for the trivial
// case). A constant-false predicate falls through to runtime
// emission so the panic surface still names the failed clause; the
// runtime trip is observable to the test suite without aetherc
// having to refuse the build.
static void emit_contract_check(CodeGenerator* gen,
                                ASTNode* clause,
                                const char* role,
                                const char* fn_name) {
    if (!clause || clause->child_count == 0) return;
    ASTNode* predicate = clause->children[0];
    /* Const-fold through the shared evaluator (contract folding — see
     * docs/contract-folding.md). Definition-site env: no parameter bindings,
     * but the program handle so `const` names and enum members resolve —
     * `requires cap > MIN_CAP` now elides when both sides are constants,
     * which the older literal-only folder here could not do.
     *
     * Only TRUE elides. A decidably-FALSE predicate was already rejected by
     * the typechecker before codegen ran; if one ever reaches here anyway
     * (UNKNOWN to the typechecker but false at run time), the runtime check
     * below still fires, so the belt keeps its braces. */
    ContractEnv cenv = {{0}, {0}, 0, gen->program};
    if (contract_eval_predicate(predicate, &cenv) == CONTRACT_TRUE) {
        /* Trivially-true predicate. Drop the runtime check — the
         * generated C should be byte-for-byte identical to a
         * function written without the clause. Emit a comment for
         * the curious reader inspecting the .c output. */
        print_indent(gen);
        fprintf(gen->output, "/* %s elided (always-true): ", role);
        char buf[1024];
        ContractStr s = { buf, sizeof(buf), 0 };
        contract_sprint_expr(&s, predicate);
        contract_str_terminate(&s);
        for (const char* p = buf; *p; p++) {
            /* Defensively split any star-slash sequence so the
             * predicate text can't accidentally terminate the
             * surrounding C comment. */
            if (p[0] == '*' && p[1] == '/') { fputs("* /", gen->output); p++; }
            else fputc(*p, gen->output);
        }
        fprintf(gen->output, " */\n");
        return;
    }
    print_indent(gen);
    fprintf(gen->output, "if (!(");
    generate_expression(gen, predicate);
    /* #1378: a stable, greppable category token leads the message, so CI and
       downstream triage can match on the category instead of prose that each
       site words differently. The human detail follows the colon. */
    fprintf(gen->output, ")) aether_panic(\"%s_violation: ", role);
    /* Re-render the predicate text into the C string literal. We
     * escape backslash and double-quote; everything else passes
     * through (Aether-source-level printable ASCII is safe in C
     * literals). */
    char buf[1024];
    ContractStr s = { buf, sizeof(buf), 0 };
    contract_sprint_expr(&s, predicate);
    contract_str_terminate(&s);
    for (const char* p = buf; *p; p++) {
        if (*p == '\\' || *p == '"') fputc('\\', gen->output);
        fputc(*p, gen->output);
    }
    fprintf(gen->output, " in %s\");\n", fn_name ? fn_name : "<fn>");
}

void emit_contract_preconditions(CodeGenerator* gen, ASTNode* func) {
    if (!gen || !func || gen->no_contracts) return;
    const char* fn_name = func->value ? func->value : "<fn>";
    for (int i = 0; i < func->child_count; i++) {
        ASTNode* c = func->children[i];
        if (c && c->type == AST_REQUIRES_CLAUSE) {
            emit_contract_check(gen, c, "precondition", fn_name);
        }
    }
}

// Emit `ensures` checks before a return. Caller has already opened a
// fresh `{` scope and emitted `<T> result = <expr>;` so `result` is
// in scope as a C local. Returns 1 if any check was emitted.
int emit_contract_postconditions(CodeGenerator* gen, ASTNode* func) {
    if (!gen || !func || gen->no_contracts) return 0;
    const char* fn_name = func->value ? func->value : "<fn>";
    int emitted = 0;
    for (int i = 0; i < func->child_count; i++) {
        ASTNode* c = func->children[i];
        if (c && c->type == AST_ENSURES_CLAUSE) {
            emit_contract_check(gen, c, "postcondition", fn_name);
            emitted = 1;
        }
    }
    return emitted;
}

// Returns 1 iff `func` has at least one AST_ENSURES_CLAUSE child.
// Used by the AST_RETURN_STATEMENT codegen to decide whether to
// route through the result-local + post-check wrapper.
static int function_has_ensures(ASTNode* func) {
    if (!func) return 0;
    for (int i = 0; i < func->child_count; i++) {
        if (func->children[i] &&
            func->children[i]->type == AST_ENSURES_CLAUSE) {
            return 1;
        }
    }
    return 0;
}

void hoist_if_branch_vars(CodeGenerator* gen, ASTNode* body) {
    if (!body) return;
    /* Which names: hoist_if_branch_candidates (shared with the typechecker,
     * which must accept exactly the reads this makes compile). */
    const char* names[HOIST_MAX_NAMES];
    int count = hoist_if_branch_candidates(body, names, HOIST_MAX_NAMES);
    for (int n = 0; n < count; n++) {
        const char* name = names[n];
        if (is_var_declared(gen, name)) continue;
        /* #744: a module-level `var` global first assigned inside an
         * if-branch must NOT be hoisted as a fresh function local —
         * that local shadows the file-scope `static`, so every write
         * lands in the local and the global keeps its initializer
         * forever (a silent miscompile; regression in #701). The
         * assignment is already routed to the global by the
         * AST_VARIABLE_DECLARATION emitter (is_module_global_var), so
         * skip it here exactly as that path does. */
        if (is_module_global_var(gen, name)) continue;
        if (is_actor_state_var(gen, name)) continue;   /* #2505: self->name */
        ASTNode* first_decl = hoist_if_branch_first_decl(body, name);
        if (!first_decl) continue;
        Type* var_type = first_decl->node_type;
        if ((!var_type || var_type->kind == TYPE_VOID || var_type->kind == TYPE_UNKNOWN)
            && first_decl->child_count > 0 && first_decl->children[0]
            && first_decl->children[0]->node_type) {
            var_type = first_decl->children[0]->node_type;
        }
        Type* joined = sibling_join_type(gen, name, var_type);
        if (joined) var_type = joined;
        const char* c_type = get_c_type(var_type);
        print_indent(gen);
        /* #2024: same as hoist_loop_vars -- a mutated capture is hoisted
         * as its cell, not as a plain value. */
        if (is_promoted_capture(gen, name)) {
            emit_promoted_cell_declaration(gen, name, c_type, var_type, NULL, NULL,
                                           first_decl->line, first_decl->column);
            if (joined) free_type(joined);
            continue;
        }
        /* Declared as a loop's hoist declares it (#2877): a fixed-size
         * array as `E name[N]`, which its C type `E[N]` cannot spell before
         * the name, so every array a branch bound and the code after it
         * read failed in C. */
        emit_hoisted_local_decl(gen, var_type, name, first_decl->line, first_decl->column);
        mark_var_declared_typed(gen, name, var_type);
        if (joined) free_type(joined);
    }
}


/* A struct type whose values own heap strings: it has a `_aether_drop_<Name>`.
 * NULL otherwise. */
const char* struct_owning_strings(CodeGenerator* gen, Type* t) {
    if (!gen || !gen->program || !t || t->kind != TYPE_STRUCT || !t->struct_name) return NULL;
    ASTNode* sdef = find_struct_definition_by_name(gen->program, t->struct_name);
    return (sdef && struct_owns_heap_strings(gen, sdef)) ? t->struct_name : NULL;
}

/* #2749: is `arg`, a direct argument of `call`, a struct literal owning
 * strings that the callee only borrows? A function or closure the compiler
 * emits takes a struct parameter as a copy of its caller's value whose
 * strings stay the caller's (it disowns them on entry, or takes references
 * of its own when it keeps the value, #2582), so the literal's strings are
 * the calling statement's to give back once the call is done, as a struct
 * a call returns is (#2582). Bound to a local first, the local's scope exit
 * destroys it; passed as it stood, nothing did, and every call leaked the
 * literal's strings. Every clause's parameter there must be a plain
 * variable of the literal's struct type: an optional, a sum or a struct
 * pattern takes the value another way, and a callee the compiler cannot
 * see (an extern, a closure it cannot name) keeps the literal alive as
 * before. */
static int borrowed_struct_literal_arg(CodeGenerator* gen, ASTNode* call, ASTNode* arg) {
    if (!gen || !gen->program || !call || call->type != AST_FUNCTION_CALL || !call->value ||
        !arg || arg->type != AST_STRUCT_LITERAL || !arg->node_type ||
        arg->node_type->kind != TYPE_STRUCT || !arg->node_type->struct_name ||
        !struct_owning_strings(gen, arg->node_type)) return 0;
    const char* sname = arg->node_type->struct_name;
    int is_closure_call = strcmp(call->value, "call") == 0;
    int pos = -1, user_args = 0;
    for (int i = is_closure_call ? 1 : 0; i < call->child_count; i++) {
        ASTNode* a = call->children[i];
        if (!a) continue;
        if (a->type == AST_NAMED_ARG) return 0;
        if (a->type == AST_CLOSURE && a->value && strcmp(a->value, "trailing") == 0) return 0;
        if (a == arg) pos = user_args;
        user_args++;
    }
    if (pos < 0) return 0;
    if (is_closure_call) {
        /* A closure literal of this program, named by its variable. */
        ASTNode* f = call->child_count > 0 ? call->children[0] : NULL;
        ASTNode* lit = NULL;
        if (f && f->type == AST_IDENTIFIER && f->value && !is_promoted_capture(gen, f->value)) {
            int id = closure_var_id(gen, gen->closure_var_scope, f->value);
            for (int cj = 0; id >= 0 && cj < gen->closure_count; cj++) {
                if (gen->closures[cj].id == id) lit = gen->closures[cj].closure_node;
            }
        }
        if (!lit) return 0;
        int seen = 0;
        for (int i = 0; i < lit->child_count; i++) {
            ASTNode* p = lit->children[i];
            if (!p || p->type != AST_CLOSURE_PARAM) continue;
            if (seen++ != pos) continue;
            return p->node_type && p->node_type->kind == TYPE_STRUCT &&
                   p->node_type->struct_name && strcmp(p->node_type->struct_name, sname) == 0;
        }
        return 0;
    }
    if (!callee_has_visible_body(gen, call->value)) return 0;
    const DefClauses* dc = program_index_clauses(gen->program, codegen_normalise_callee(call->value));
    if (!dc || dc->count == 0) return 0;
    for (int c = 0; c < dc->count; c++) {
        ASTNode* fdef = dc->nodes[c];
        if (!fdef || fdef->type != AST_FUNCTION_DEFINITION || fn_param_count(fdef) != user_args)
            return 0;
        ASTNode* p = fn_param_at(fdef, pos);
        if (!p || p->type != AST_PATTERN_VARIABLE || !p->node_type ||
            p->node_type->kind != TYPE_STRUCT || !p->node_type->struct_name ||
            strcmp(p->node_type->struct_name, sname) != 0) return 0;
    }
    return 1;
}

/* #2582: what a statement's value holds of the struct-returning calls in it.
 * TEMP_FREE: the statement keeps nothing of `e` past its own evaluation (an
 * argument consumed by its call, the object of a read the slot copies): a
 * struct a call returns there is a temporary. TEMP_KEPT: `e` is what the
 * statement keeps (the bound or returned value, an `if` arm or `or` default
 * that becomes it, a struct literal's field, an array element); the slot
 * that takes it copies a view (a field read), so the object of such a read
 * is still a temporary. TEMP_RAW: `e` flows as it is into a kept value
 * (an argument a call may hand back), so neither it nor the struct a read
 * of it reaches into is a temporary. */
enum { TEMP_FREE = 0, TEMP_KEPT = 1, TEMP_RAW = 2 };

static void collect_value_struct_temps(CodeGenerator* gen, ASTNode* e, int kept,
                                       ASTNode*** nodes, int* count, int* cap);
static int call_may_hand_back_args(CodeGenerator* gen, ASTNode* call);

/* The calls in an expression statement whose value is a struct, owning
 * strings, returned by value: temporaries the statement destroys once done
 * (see the expression-statement path). Not into a closure, whose body is
 * its own function. A struct literal a call borrows is one too (#2749),
 * unless the call's result may be a view into it, and it keeps what its
 * fields hold, which go with it. */
void collect_stmt_struct_temps(CodeGenerator* gen, ASTNode* e,
                               ASTNode*** nodes, int* count, int* cap) {
    if (!e || e->type == AST_CLOSURE) return;
    if (e->type == AST_FUNCTION_CALL && e->node_type &&
        e->node_type->kind == TYPE_STRUCT && struct_owning_strings(gen, e->node_type)) {
        if (*count == *cap) {
            *cap = *cap ? *cap * 2 : 4;
            *nodes = (ASTNode**)aether_xrealloc(*nodes, sizeof(ASTNode*) * (size_t)*cap);
        }
        (*nodes)[(*count)++] = e;
    }
    int lit_args = e->type == AST_FUNCTION_CALL &&
                   (!call_may_hand_back_args(gen, e) || call_returns_view_of_temp(gen, e) ||
                    call_returns_tuple_view_of_temp(gen, e));
    for (int i = 0; i < e->child_count; i++) {
        ASTNode* c = e->children[i];
        if (lit_args && borrowed_struct_literal_arg(gen, e, c)) {
            if (*count == *cap) {
                *cap = *cap ? *cap * 2 : 4;
                *nodes = (ASTNode**)aether_xrealloc(*nodes, sizeof(ASTNode*) * (size_t)*cap);
            }
            (*nodes)[(*count)++] = c;
            collect_value_struct_temps(gen, c, TEMP_KEPT, nodes, count, cap);
            continue;
        }
        collect_stmt_struct_temps(gen, c, nodes, count, cap);
    }
}

/* #2582: is `n` an access chain (fields, elements) rooted at identifier
 * `pname`? */
static int access_chain_rooted_at(ASTNode* n, const char* pname) {
    while (n && (n->type == AST_MEMBER_ACCESS || n->type == AST_ARRAY_ACCESS) &&
           n->child_count >= 1)
        n = n->children[0];
    return n && n->type == AST_IDENTIFIER && n->value && strcmp(n->value, pname) == 0;
}

/* #2582: can a value of type `t` point into memory an argument owns: a
 * string, a pointer, an array or a tuple holding one? */
static int type_can_view(Type* t) {
    if (!t) return 1;
    if (t->kind == TYPE_STRING || t->kind == TYPE_PTR || t->kind == TYPE_ARRAY) return 1;
    if (t->kind == TYPE_TUPLE) {
        for (int i = 0; i < t->tuple_count; i++)
            if (type_can_view(t->tuple_types[i])) return 1;
    }
    return 0;
}

static int mentions_any_param(ASTNode* fn_def, ASTNode* e) {
    for (int i = 0; i < fn_def->child_count; i++) {
        ASTNode* prm = fn_def->children[i];
        if (prm && prm->value &&
            (prm->type == AST_PATTERN_VARIABLE || prm->type == AST_VARIABLE_DECLARATION) &&
            subtree_mentions_param(e, prm->value)) return 1;
    }
    return 0;
}

/* Does a `return` under `n` give, where the result can point into memory
 * (type_can_view, per tuple position), a value that mentions a parameter
 * of `fn_def`? A nested closure's returns are its own. */
static int returns_view_params_walk(ASTNode* fn_def, ASTNode* n, Type* rt) {
    if (!n || ast_is_own_function(n)) return 0;
    if (n->type == AST_RETURN_STATEMENT) {
        for (int j = 0; j < n->child_count; j++) {
            Type* pt = (rt && rt->kind == TYPE_TUPLE && n->child_count > 1 && j < rt->tuple_count)
                       ? rt->tuple_types[j] : rt;
            if (type_can_view(pt) && mentions_any_param(fn_def, n->children[j])) return 1;
        }
        return 0;
    }
    for (int i = 0; i < n->child_count; i++)
        if (returns_view_params_walk(fn_def, n->children[i], rt)) return 1;
    return 0;
}

/* #2582: may call `call`'s result be, or point into, one of its arguments as
 * it came? Then an argument is not the statement's to destroy while the
 * result lives.
 *
 * A scalar result holds nothing, and neither does a fresh heap string
 * (`string.concat`, a function whose every return is one). A function or
 * closure the compiler emits returns a struct of its own (a struct
 * parameter it keeps takes references on entry, struct_param_kept); any
 * other result it gives can be an argument only through a `return` whose
 * value mentions a parameter where the result can point into memory, as
 * `first(s, n) { return s }` hands back the caller's pointer, which may
 * point into the argument's struct (`first(make_item(w).name, 1)`). A
 * fallible `int!` returning a literal error is no view. Anything a C
 * function returns could be its argument. */
static int call_may_hand_back_args(CodeGenerator* gen, ASTNode* call) {
    if (!call || call->type != AST_FUNCTION_CALL) return 0;
    Type* rt = call->node_type;
    if (rt) {
        switch (rt->kind) {
            case TYPE_INT: case TYPE_INT64: case TYPE_UINT64: case TYPE_UINT32:
            case TYPE_UINT16: case TYPE_UINT8: case TYPE_DURATION: case TYPE_FLOAT:
            case TYPE_LONGDOUBLE: case TYPE_FLOAT32: case TYPE_BOOL: case TYPE_BYTE:
            case TYPE_VOID:
                return 0;
            default:
                break;
        }
    }
    if (rt && rt->kind == TYPE_STRING && is_heap_string_expr(gen, call)) return 0;
    const char* fn = call->value ? codegen_normalise_callee(call->value) : NULL;
    if (!fn) return 1;
    if (strcmp(fn, "call") == 0) {
        /* A closure follows the convention the compiler emits it by: a
         * struct or string it returns is its own (a uniform-heap string
         * return, #2499). */
        return !(rt && (rt->kind == TYPE_STRUCT || rt->kind == TYPE_STRING));
    }
    if (is_nonstoring_builtin(fn)) return 0;
    if (!callee_has_visible_body(gen, call->value)) return 1;
    if (rt && rt->kind == TYPE_STRUCT) return 0;
    ASTNode* fn_def = find_function_definition_by_name(gen->program, fn);
    if (!fn_def) return 1;
    return returns_view_params_walk(fn_def, fn_def, rt);
}

/* #2619: is a struct a call returns, owning strings, somewhere in `e`? Or a
 * struct literal a call borrows (#2749), which is a temporary as well. */
static int holds_struct_call(CodeGenerator* gen, ASTNode* e) {
    if (!e || e->type == AST_CLOSURE) return 0;
    if (e->type == AST_FUNCTION_CALL && e->node_type && e->node_type->kind == TYPE_STRUCT &&
        struct_owning_strings(gen, e->node_type)) return 1;
    for (int i = 0; i < e->child_count; i++) {
        if (e->type == AST_FUNCTION_CALL && borrowed_struct_literal_arg(gen, e, e->children[i]))
            return 1;
        if (holds_struct_call(gen, e->children[i])) return 1;
    }
    return 0;
}

/* #2619: a `string` call that may hand back an argument as it came
 * (call_may_hand_back_args) where an argument holds a struct another call
 * returns, which its statement destroys as a temporary. Its result is
 * copied where it is made (generate_expression), so it never points into
 * the temporary and the temporary can go with the statement. Before, the
 * argument was kept alive for good: `b = first(make_item(w).name, 1)`
 * leaked the struct on every call. */
static int g_view_check = 0;
int call_returns_view_of_temp(CodeGenerator* gen, ASTNode* call) {
    if (g_view_check || !call || call->type != AST_FUNCTION_CALL || !call->node_type ||
        call->node_type->kind != TYPE_STRING) return 0;
    int any = 0;
    for (int i = 0; i < call->child_count && !any; i++)
        any = holds_struct_call(gen, call->children[i]) ||
              borrowed_struct_literal_arg(gen, call, call->children[i]);   /* #2749 */
    if (!any) return 0;
    g_view_check = 1;   /* the question below asks is_heap_string_expr */
    int r = call_may_hand_back_args(gen, call);
    g_view_check = 0;
    return r;
}

/* #2619: the tuple counterpart of call_returns_view_of_temp: a call
 * returning a tuple whose positions are strings, scalars or structs, that
 * may hand back an argument holding a struct another call returned. Each
 * string position is copied where the call is made (generate_expression),
 * so a destructure or an `or` takes it owned and the struct goes with its
 * statement. A tuple holding a pointer or an array may still point into the
 * argument, so its argument is kept alive instead. */
int call_returns_tuple_view_of_temp(CodeGenerator* gen, ASTNode* call) {
    if (g_view_check || !call || call->type != AST_FUNCTION_CALL || !call->node_type ||
        call->node_type->kind != TYPE_TUPLE) return 0;
    Type* t = call->node_type;
    int strings = 0;
    for (int i = 0; i < t->tuple_count; i++) {
        Type* et = t->tuple_types[i];
        if (!et) return 0;
        if (et->kind == TYPE_PTR || et->kind == TYPE_ARRAY || et->kind == TYPE_TUPLE) return 0;
        if (et->kind == TYPE_STRING) strings++;
    }
    if (!strings) return 0;
    int any = 0;
    for (int i = 0; i < call->child_count && !any; i++)
        any = holds_struct_call(gen, call->children[i]) ||
              borrowed_struct_literal_arg(gen, call, call->children[i]);   /* #2749 */
    if (!any) return 0;
    g_view_check = 1;
    int r = call_may_hand_back_args(gen, call);
    g_view_check = 0;
    return r;
}

/* Is string position `j` of tuple call `call` heap as its callee returns
 * it: every return of a user function wraps it, or an extern declares it
 * `@heap`? */
int tuple_call_returns_heap_at(CodeGenerator* gen, ASTNode* call, int j) {
    if (!gen || !gen->program || !call || call->type != AST_FUNCTION_CALL || !call->value) return 0;
    const char* fn = codegen_normalise_callee(call->value);
    ASTNode* callee = find_function_definition_by_name(gen->program, fn);
    if (callee) return function_def_returns_heap_at(gen, callee, j);
    ASTNode* ext = find_extern_declaration_by_name(gen->program, fn);
    return ext && ext->node_type && ext->node_type->kind == TYPE_TUPLE &&
           ext->node_type->tuple_heap_flags && j < ext->node_type->tuple_count &&
           ext->node_type->tuple_heap_flags[j];
}

/* #2619: is string position `j` of `call` one the call yields copied? */
static int tuple_call_pos_copied(CodeGenerator* gen, ASTNode* call, int j) {
    return call && call->node_type && call->node_type->kind == TYPE_TUPLE &&
           j >= 0 && j < call->node_type->tuple_count && call->node_type->tuple_types[j] &&
           call->node_type->tuple_types[j]->kind == TYPE_STRING &&
           call_returns_tuple_view_of_temp(gen, call);
}

/* R3: does `n` write into the struct parameter `pname`: rebind it, or
 * store into a field or an element of it (`p.f = v`, `p.xs[i] = v`)? Such
 * a parameter is made a value of its own on entry, so the store releases
 * nothing of its caller's; any other borrows. */
int struct_param_written(ASTNode* n, const char* pname) {
    if (!n || !pname) return 0;
    if (n->type == AST_VARIABLE_DECLARATION && n->value && strcmp(n->value, pname) == 0) return 1;
    if (n->type == AST_BINARY_EXPRESSION && n->value && strcmp(n->value, "=") == 0 &&
        n->child_count >= 1 && n->children[0] &&
        (n->children[0]->type == AST_MEMBER_ACCESS || n->children[0]->type == AST_ARRAY_ACCESS) &&
        access_chain_rooted_at(n->children[0], pname)) return 1;
    for (int i = 0; i < n->child_count; i++)
        if (struct_param_written(n->children[i], pname)) return 1;
    return 0;
}

static void add_value_temp(ASTNode* e, ASTNode*** nodes, int* count, int* cap) {
    if (*count == *cap) {
        *cap = *cap ? *cap * 2 : 4;
        *nodes = (ASTNode**)aether_xrealloc(*nodes, sizeof(ASTNode*) * (size_t)*cap);
    }
    (*nodes)[(*count)++] = e;
}

/* #2582: collects the struct-returning calls in `e` that are temporaries of
 * the statement, given how much of `e` it keeps. A block, a `match` or a
 * closure holds statements of its own, with temporaries of their own. */
static void collect_value_struct_temps(CodeGenerator* gen, ASTNode* e, int kept,
                                       ASTNode*** nodes, int* count, int* cap) {
    if (!e || e->type == AST_CLOSURE || e->type == AST_BLOCK ||
        e->type == AST_MATCH_STATEMENT) return;
    if (kept != TEMP_FREE) {
        switch (e->type) {
            case AST_IF_EXPRESSION:
                for (int i = 0; i < e->child_count; i++)
                    collect_value_struct_temps(gen, e->children[i], i > 0 ? kept : TEMP_FREE,
                                               nodes, count, cap);
                return;
            case AST_OR_ELSE: case AST_NULL_COALESCE: case AST_VALUE_CAST:
                for (int i = 0; i < e->child_count; i++)
                    collect_value_struct_temps(gen, e->children[i], kept, nodes, count, cap);
                return;
            case AST_STRUCT_LITERAL: case AST_ARRAY_LITERAL:
                for (int i = 0; i < e->child_count; i++) {
                    ASTNode* c = e->children[i];
                    /* A struct literal's field is an AST_ASSIGNMENT named by
                     * the field, holding its value. */
                    if (c && e->type == AST_STRUCT_LITERAL && c->type == AST_ASSIGNMENT) {
                        for (int k = 0; k < c->child_count; k++)
                            collect_value_struct_temps(gen, c->children[k], kept, nodes, count, cap);
                    } else {
                        collect_value_struct_temps(gen, c, kept, nodes, count, cap);
                    }
                }
                return;
            case AST_FUNCTION_CALL: {
                int raw = call_may_hand_back_args(gen, e) && !call_returns_view_of_temp(gen, e) &&
                          !call_returns_tuple_view_of_temp(gen, e);
                for (int i = 0; i < e->child_count; i++) {
                    /* #2749: a struct literal the call borrows. */
                    if (!raw && borrowed_struct_literal_arg(gen, e, e->children[i])) {
                        add_value_temp(e->children[i], nodes, count, cap);
                        collect_value_struct_temps(gen, e->children[i], TEMP_KEPT, nodes, count, cap);
                        continue;
                    }
                    collect_value_struct_temps(gen, e->children[i], raw ? TEMP_RAW : TEMP_FREE,
                                               nodes, count, cap);
                }
                return;
            }
            case AST_MEMBER_ACCESS: case AST_ARRAY_ACCESS:
                if (kept == TEMP_RAW) {
                    for (int i = 0; i < e->child_count; i++)
                        collect_value_struct_temps(gen, e->children[i], i == 0 ? TEMP_RAW : TEMP_FREE,
                                                   nodes, count, cap);
                    return;
                }
                break;
            default:
                break;
        }
    }
    if (e->type == AST_FUNCTION_CALL && e->node_type &&
        e->node_type->kind == TYPE_STRUCT && struct_owning_strings(gen, e->node_type))
        add_value_temp(e, nodes, count, cap);
    for (int i = 0; i < e->child_count; i++) {
        /* #2749: a struct literal a call borrows is a temporary too, and
         * keeps what its fields hold. */
        if (e->type == AST_FUNCTION_CALL && borrowed_struct_literal_arg(gen, e, e->children[i])) {
            add_value_temp(e->children[i], nodes, count, cap);
            collect_value_struct_temps(gen, e->children[i], TEMP_KEPT, nodes, count, cap);
            continue;
        }
        collect_value_struct_temps(gen, e->children[i], TEMP_FREE, nodes, count, cap);
    }
}

/* A call with a trailing block (a builder) is emitted twice by the
 * declaration paths, so a temporary in it would be assigned twice. */
static int has_trailing_block_call(ASTNode* e) {
    if (!e || e->type == AST_CLOSURE) return 0;
    if (e->type == AST_FUNCTION_CALL) {
        for (int i = 0; i < e->child_count; i++) {
            ASTNode* c = e->children[i];
            if (c && c->type == AST_CLOSURE && c->value && strcmp(c->value, "trailing") == 0)
                return 1;
        }
    }
    for (int i = 0; i < e->child_count; i++)
        if (has_trailing_block_call(e->children[i])) return 1;
    return 0;
}

/* #2582: the temporaries of a declaration's initializer, an assignment's
 * value or a return's values. The expression statement collects its own
 * (collect_stmt_struct_temps). A return in main() leaves through
 * main_exit, a return lowered from a `match` returns from inside its arms
 * (emit_match_result_value has their temporaries), and a return of a call
 * with no value runs the defers before the call, so those are left as they
 * were. */
static int statement_value_struct_temps(CodeGenerator* gen, ASTNode* stmt,
                                        ASTNode*** nodes, int* count, int* cap) {
    switch (stmt->type) {
        case AST_VARIABLE_DECLARATION:
            if (stmt->child_count < 1 || has_trailing_block_call(stmt->children[0])) return 0;
            collect_value_struct_temps(gen, stmt->children[0], TEMP_KEPT, nodes, count, cap);
            break;
        case AST_ASSIGNMENT:
            if (stmt->child_count < 2 || has_trailing_block_call(stmt->children[1])) return 0;
            collect_value_struct_temps(gen, stmt->children[1], TEMP_KEPT, nodes, count, cap);
            break;
        case AST_TUPLE_DESTRUCTURE: {
            /* `a, b = f(...)`: the last child is the value. */
            ASTNode* v = stmt->child_count >= 2 ? stmt->children[stmt->child_count - 1] : NULL;
            if (!v || has_trailing_block_call(v)) return 0;
            collect_value_struct_temps(gen, v, TEMP_KEPT, nodes, count, cap);
            break;
        }
        case AST_RETURN_STATEMENT:
            if (gen->in_main_function) return 0;
            for (int i = 0; i < stmt->child_count; i++) {
                ASTNode* v = stmt->children[i];
                if (!v || v->type == AST_MATCH_STATEMENT || !v->node_type ||
                    v->node_type->kind == TYPE_VOID || has_trailing_block_call(v)) return 0;
            }
            for (int i = 0; i < stmt->child_count; i++)
                collect_value_struct_temps(gen, stmt->children[i], TEMP_KEPT, nodes, count, cap);
            break;
        default:
            return 0;
    }
    return *count;
}

typedef struct {
    ASTNode** nodes;
    const char** names;
    ASTNode** carriers;
    int count;
    int mapped;
    ASTNode** outer_nodes;
    const char** outer_names;
    int outer_count;
} ValueTemps;

/* #2582: declares each temporary in `nodes` zeroed on a line of its own
 * where code is being emitted, so one a short circuit skips destroys
 * nothing, puts its destroy on the defer stack (a `return` or `break` from
 * inside the value, as from an `or` handler, destroys it on the way out),
 * and maps the calls to their temporaries for the call emitter. Takes
 * ownership of `nodes`. */
static void temps_open(CodeGenerator* gen, ASTNode** nodes, int count, int line, int column,
                       ValueTemps* vt) {
    static int seq = 0;
    memset(vt, 0, sizeof(*vt));
    vt->nodes = nodes;
    vt->count = count;
    vt->names = (const char**)aether_xrealloc(NULL, sizeof(char*) * (size_t)count);
    vt->carriers = (ASTNode**)aether_xrealloc(NULL, sizeof(ASTNode*) * (size_t)count);
    for (int i = 0; i < count; i++) {
        vt->names[i] = cg_internf("_ae_vtmp%d", seq++);
        print_indent(gen);
        fprintf(gen->output, "%s %s = {0};\n", get_c_type(nodes[i]->node_type), vt->names[i]);
        int before = gen->defer_count;
        push_struct_destroy_defer(gen, vt->names[i], nodes[i]->node_type, line, column);
        vt->carriers[i] = gen->defer_count > before ? gen->defer_stack[before] : NULL;
    }
    stmt_struct_temps_get(&vt->outer_nodes, &vt->outer_names, &vt->outer_count);
    stmt_struct_temps_set(vt->nodes, vt->names, vt->count);
    vt->mapped = 1;
}

/* The calls are emitted: later code emitted inside the temporaries' life
 * (a match's arms) maps calls of its own. */
static void temps_unmap(ValueTemps* vt) {
    if (!vt->mapped) return;
    stmt_struct_temps_set(vt->outer_nodes, vt->outer_names, vt->outer_count);
    vt->mapped = 0;
}

/* Each temporary leaves the defer stack and, unless `destroy` is 0 (a
 * return, whose exit already destroyed it with the other defers), is
 * destroyed in place. */
static void temps_close(CodeGenerator* gen, ValueTemps* vt, int destroy) {
    temps_unmap(vt);
    for (int i = vt->count - 1; i >= 0; i--) {
        for (int d = gen->defer_count - 1; vt->carriers[i] && d >= 0; d--) {
            if (gen->defer_stack[d] == vt->carriers[i]) {
                gen->defer_stack[d] = NULL;
                if (d == gen->defer_count - 1) gen->defer_count--;
                break;
            }
        }
        if (destroy) {
            print_indent(gen);
            fprintf(gen->output, "_aether_drop_%s(&%s);\n",
                    struct_owning_strings(gen, vt->nodes[i]->node_type), vt->names[i]);
        }
    }
    while (gen->defer_count > 0 && !gen->defer_stack[gen->defer_count - 1] &&
           (gen->scope_depth <= 0 ||
            gen->defer_count > gen->scope_defer_start[gen->scope_depth - 1]))
        gen->defer_count--;
    free(vt->nodes);
    free(vt->names);
    free(vt->carriers);
    vt->nodes = NULL;
    vt->count = 0;
}

/* #2582: the temporaries of `e`, given how much of it the code around keeps,
 * opened where code is being emitted. 0 when there are none. */
static int value_temps_open_expr(CodeGenerator* gen, ASTNode* e, int kept, ValueTemps* vt) {
    ASTNode** nodes = NULL;
    int count = 0, cap = 0;
    collect_value_struct_temps(gen, e, kept, &nodes, &count, &cap);
    if (count == 0) {
        free(nodes);
        memset(vt, 0, sizeof(*vt));
        return 0;
    }
    temps_open(gen, nodes, count, e->line, e->column, vt);
    return 1;
}

/* #2582: a declaration's, an assignment's, a tuple destructure's or a
 * return's temporaries, opened just ahead of the statement. */
static int value_temps_open(CodeGenerator* gen, ASTNode* stmt, ValueTemps* vt) {
    memset(vt, 0, sizeof(*vt));
    if (stmt->type != AST_VARIABLE_DECLARATION && stmt->type != AST_ASSIGNMENT &&
        stmt->type != AST_TUPLE_DESTRUCTURE && stmt->type != AST_RETURN_STATEMENT) return 0;
    ASTNode** nodes = NULL;
    int count = 0, cap = 0;
    if (statement_value_struct_temps(gen, stmt, &nodes, &count, &cap) == 0) {
        free(nodes);
        return 0;
    }
    temps_open(gen, nodes, count, stmt->line, stmt->column, vt);
    return 1;
}

static void value_temps_close(CodeGenerator* gen, ASTNode* stmt, ValueTemps* vt) {
    temps_close(gen, vt, stmt->type != AST_RETURN_STATEMENT);
}

/* #2582: an `if` or loop condition. Its value is a bool, which holds nothing
 * of a struct a call in it returns, so each such struct is destroyed as
 * soon as the condition is evaluated, every time it is: the condition
 * becomes a sequence of temporaries (#2765) holding them. */
static void emit_condition(CodeGenerator* gen, ASTNode* cond) {
    ASTNode** nodes = NULL;
    int count = 0, cap = 0;
    collect_value_struct_temps(gen, cond, TEMP_FREE, &nodes, &count, &cap);
    gen->in_condition = 1;
    if (count == 0) {
        free(nodes);
        generate_expression(gen, cond);
        gen->in_condition = 0;
        return;
    }
    const char** names = (const char**)aether_xrealloc(NULL, sizeof(char*) * (size_t)count);
    TempSeq s;
    seq_open(gen, &s);
    for (int i = 0; i < count; i++) {
        const char* ct = get_c_type(nodes[i]->node_type);
        names[i] = seq_let(gen, &s, ct, "_ae_ctmp");
        fprintf(gen->output, "(%s){0}", ct);
        seq_next(gen, &s);
    }
    ASTNode** outer_nodes;
    const char** outer_names;
    int outer_count;
    stmt_struct_temps_get(&outer_nodes, &outer_names, &outer_count);
    stmt_struct_temps_set(nodes, names, count);
    const char* cv = seq_let(gen, &s, "int", "_ae_cv");
    fprintf(gen->output, "!!(");
    generate_expression(gen, cond);
    fprintf(gen->output, ")");
    stmt_struct_temps_set(outer_nodes, outer_names, outer_count);
    for (int i = 0; i < count; i++) {
        seq_next(gen, &s);
        fprintf(gen->output, "_aether_drop_%s(&%s)",
                struct_owning_strings(gen, nodes[i]->node_type), names[i]);
    }
    seq_next(gen, &s);
    fprintf(gen->output, "%s", cv);
    seq_close(gen, &s);
    gen->in_condition = 0;
    free(nodes);
    free(names);
}

/* #2497: the struct counterpart of emit_string_take. A struct that owns
 * heap strings (directly or in a struct field held by value) is stored into
 * an owning slot (a local, a field, a match result, a return value) by
 * taking it: a fresh value (a struct literal, a call) is adopted; a struct
 * a local owns is moved out of it on its last use (its trackers cleared, so
 * its scope exit releases nothing); anything else that only views a struct
 * owned elsewhere (a parameter, a variable still in use, a field, an
 * element) is copied with `_aether_copy_<Name>`. Storing the view as it was made two
 * owners of every string: `b = a` freed `a`'s strings twice. */
int struct_take_shape(ASTNode* e) {
    if (e && e->type == AST_TUPLE_UNWRAP && e->child_count > 0 && e->children[0]) {
        /* `o!` reads the payload of the optional place `o` (#2921); a
         * call's `!` hands its value over. */
        ASTNode* o = e->children[0];
        return o->type == AST_IDENTIFIER || o->type == AST_MEMBER_ACCESS ||
               o->type == AST_ARRAY_ACCESS;
    }
    return e && (e->type == AST_IDENTIFIER || e->type == AST_MEMBER_ACCESS ||
                 e->type == AST_ARRAY_ACCESS || e->type == AST_IF_EXPRESSION);
}

/* Does local `name` own its struct value: is its scope-exit destroy
 * pending? Parameters, pattern bindings and other copies do not. */
static int struct_local_owns(CodeGenerator* gen, const char* name) {
    size_t n = strlen(name);
    for (int i = 0; i < gen->defer_count; i++) {
        ASTNode* d = gen->defer_stack[i];
        const char* a = d ? d->annotation : NULL;
        if (a && strncmp(a, "struct_destroy:", 15) == 0 &&
            strncmp(a + 15, name, n) == 0 && a[15 + n] == ':') return 1;
    }
    return 0;
}

void emit_struct_take(CodeGenerator* gen, ASTNode* e, const char* sname,
                      const char* target) {
    if (e && e->type == AST_IF_EXPRESSION && e->child_count >= 3) {
        fprintf(gen->output, "((");
        generate_expression(gen, e->children[0]);
        fprintf(gen->output, ") ? ");
        emit_struct_take(gen, e->children[1], sname, target);
        fprintf(gen->output, " : ");
        emit_struct_take(gen, e->children[2], sname, target);
        fprintf(gen->output, ")");
        return;
    }
    if (!struct_take_shape(e) ||
        (e->type == AST_IDENTIFIER && target && e->value && strcmp(e->value, target) == 0)) {
        generate_expression(gen, e);
        return;
    }
    if (e->type == AST_IDENTIFIER && e->value && struct_local_owns(gen, e->value) &&
        !is_promoted_capture(gen, e->value) && !is_env_capture_name(gen, e->value) &&
        !alias_source_must_copy(gen, e->value)) {
        /* The local hands its value over and disowns what it held
         * (_aether_move_<Name>): a function, so the move is an expression
         * to any C compiler (#2765). */
        fprintf(gen->output, "_aether_%smove_%s(&%s)",
                is_try_clobbered_var(gen, e->value) ? "volatile_" : "", sname, e->value);
        return;
    }
    fprintf(gen->output, "_aether_copy_%s(", sname);
    generate_expression(gen, e);
    fprintf(gen->output, ")");
}

/* #2525: is `e` a closure value nobody else holds: a closure literal, or a
 * call that hands one over (call_returns_owned_closure)? An `if` over two
 * such is one too. */
static int closure_value_is_fresh(CodeGenerator* gen, ASTNode* e) {
    if (!e) return 0;
    if (env_scan_is_real_closure(e)) return 1;
    /* #2528: a reply's closure was taken for the asker. */
    if (e->type == AST_SEND_ASK) return 1;
    /* R3: every call hands its closure over, but a C function's. */
    if (e->type == AST_FUNCTION_CALL) return !call_is_extern(gen, e);
    if (e->type == AST_IF_EXPRESSION && e->child_count >= 3) {
        return closure_value_is_fresh(gen, e->children[1]) &&
               closure_value_is_fresh(gen, e->children[2]);
    }
    return 0;
}

/* #2525: the closure counterpart of emit_string_take, for a slot that holds
 * a reference of its own (a struct or message field, a global, an actor's
 * state). A fresh value's reference is adopted; a view of a closure held
 * elsewhere (a local, a parameter, a field, an element, a call returning a
 * borrowed one) is retained, and its holder keeps releasing its own. A bare
 * named function has no env: nothing to retain. An `if` over values takes
 * each branch on its own. */
void emit_closure_take(CodeGenerator* gen, ASTNode* e) {
    if (e && e->type == AST_IF_EXPRESSION && e->child_count >= 3 &&
        !closure_value_is_fresh(gen, e)) {
        fprintf(gen->output, "((");
        generate_expression(gen, e->children[0]);
        fprintf(gen->output, ") ? ");
        emit_closure_take(gen, e->children[1]);
        fprintf(gen->output, " : ");
        emit_closure_take(gen, e->children[2]);
        fprintf(gen->output, ")");
        return;
    }
    int bare_fn = e && e->type == AST_IDENTIFIER && e->value && gen->program &&
                  !is_var_declared(gen, e->value) &&
                  find_function_definition_by_name(gen->program, e->value);
    if (bare_fn) {
        /* A function named as a value: the closure over its adapter. */
        register_bare_fn_adapter(gen, e->value);
        fprintf(gen->output, "(_AeClosure){ .fn = (void(*)(void))_aether_bare_adapter_%s, .env = NULL }",
                e->value);
        return;
    }
    if (closure_value_is_fresh(gen, e)) {
        generate_expression(gen, e);
        return;
    }
    fprintf(gen->output, "_aether_closure_retain(");
    generate_expression(gen, e);
    fprintf(gen->output, ")");
}


/* #752 (caller side): a struct local that RECEIVES ownership of a
 * returned struct — a tuple-unpack target, or a local initialised from a
 * struct-returning call — owns that struct's heap-string fields and must
 * free them at scope exit. Push the same `_aether_drop_<Struct>` defer the
 * struct-literal declaration path uses. The callee already transferred
 * ownership (its own destroy was suppressed via mark_return_escaped_
 * struct_var), so this is the single owner; no double-free. Gated on the
 * struct actually having heap-string fields (else the defer is a no-op
 * we skip emitting). */
void push_struct_destroy_defer(CodeGenerator* gen, const char* var_name,
                                      Type* struct_type, int line, int col) {
    if (!gen->program || !var_name || !struct_type ||
        struct_type->kind != TYPE_STRUCT || !struct_type->struct_name) return;
    ASTNode* sdef = find_struct_definition_by_name(gen->program, struct_type->struct_name);
    if (!sdef || !struct_owns_heap_strings(gen, sdef)) return;
    ASTNode* carrier = create_ast_node(AST_EXPRESSION_STATEMENT, NULL, line, col);
    if (carrier) {
        if (carrier->annotation) free(carrier->annotation);
        carrier->annotation = heap_strf("struct_destroy:%s:%s",
                                        var_name, struct_type->struct_name);
        codegen_own_node(gen, carrier);
        push_defer(gen, carrier);
        emit_struct_unwind_track(gen, var_name, struct_type->struct_name);
    }
}

/* #2923: a local that owns (a struct, an array) is journaled by its address
 * while it is in scope, so a panic leaving the scope drops what it holds
 * then; its scope exits forget it. A declaration being written holds the
 * journal call until its `;` (unwind_track_hold), at the statement level it
 * was pushed at, never inside a statement nested in its initializer. */
#define UNWIND_TRACK_MAX 32
static const char* g_utrack_call[UNWIND_TRACK_MAX];
static int g_utrack_count = 0;
static int g_utrack_hold_depth = -1;

static void unwind_track_print(CodeGenerator* gen, const char* call) {
    print_indent(gen);
    fprintf(gen->output, "%s;\n", call);
}

/* The journal call `call`, now or at the `;` of the declaration being
 * written. */
static void emit_unwind_track_call(CodeGenerator* gen, const char* call) {
    if (g_utrack_hold_depth < 0 || gen->hoist_depth > g_utrack_hold_depth) {
        unwind_track_print(gen, call);
        return;
    }
    if (g_utrack_count == UNWIND_TRACK_MAX) {
        fprintf(stderr, "Fatal: too many owning locals declared in one statement\n");
        exit(1);
    }
    g_utrack_call[g_utrack_count++] = call;
}

/* The place at `addr` journaled with the drop `fn`. */
static void emit_unwind_track_place(CodeGenerator* gen, const char* addr, const char* fn) {
    emit_unwind_track_call(gen, cg_internf("aether_unwind_track((const void*)%s, %s)", addr, fn));
}

void emit_struct_unwind_track(CodeGenerator* gen, const char* var_name, const char* struct_name) {
    emit_unwind_track_place(gen, cg_internf("&%s", var_name),
                            cg_internf("_aether_udrop_%s", struct_name));
}

static void unwind_track_hold(CodeGenerator* gen) {
    if (g_utrack_hold_depth < 0) g_utrack_hold_depth = gen->hoist_depth;
}

static void unwind_track_release(CodeGenerator* gen) {
    if (g_utrack_hold_depth < 0 || gen->hoist_depth > g_utrack_hold_depth) return;
    g_utrack_hold_depth = -1;
    for (int i = 0; i < g_utrack_count; i++) unwind_track_print(gen, g_utrack_call[i]);
    g_utrack_count = 0;
}

// #893: innermost enclosing loop whose source label matches `name`, or -1.
// Searches the loop-nest stack from the inside out, so a reused label binds
// to the nearest loop (the standard rule).
static int find_labeled_loop_level(CodeGenerator* gen, const char* name) {
    if (!name) return -1;
    for (int d = gen->loop_nest_depth - 1; d >= 0; d--) {
        if (gen->loop_label[d] && strcmp(gen->loop_label[d], name) == 0) return d;
    }
    return -1;
}

/* #2696: enters a `switch` as the target of the unlabeled breaks in its
 * cases. Its C statement, if it lowers to one, sets switch_c_depth once
 * opened; an if-chain leaves it -1. Returns its level, or -1 past
 * AETHER_MAX_SWITCH_NEST. */
static int switch_target_push(CodeGenerator* gen) {
    if (gen->switch_nest_depth >= AETHER_MAX_SWITCH_NEST) return -1;
    int lvl = gen->switch_nest_depth++;
    gen->switch_loop_depth[lvl] = gen->loop_nest_depth;
    gen->switch_scope[lvl] = gen->scope_depth;
    gen->switch_try_base[lvl] = gen->try_frame_depth;
    gen->switch_c_depth[lvl] = -1;
    gen->switch_brk_id[lvl] = gen->next_loop_label_id++;
    gen->switch_brk_used[lvl] = 0;
    return lvl;
}

/* Leaves the switch at `lvl`, after its last line: where a break that
 * could not be a C `break` jumps. */
static void switch_target_pop(CodeGenerator* gen, int lvl) {
    if (lvl < 0) return;
    if (gen->switch_brk_used[lvl]) print_line(gen, "__ae_brk_%d: ;", gen->switch_brk_id[lvl]);
    gen->switch_nest_depth = lvl;
}

/* The switch an unlabeled `break` leaves, as its level, or -1 when it
 * leaves the innermost loop: the innermost switch is nearer when it began
 * inside that loop. */
static int break_switch_target(CodeGenerator* gen) {
    int top = gen->switch_nest_depth - 1;
    if (top >= 0 && gen->switch_loop_depth[top] == gen->loop_nest_depth) return top;
    return -1;
}

/* The statements of switch case `c` from `first` (a `case` holds its
 * selector first): a scope of their own, as an `if` branch is, so a `defer`
 * in them runs when the case ends, only when it ran, and a name they bind
 * first is theirs (#2696). The caller emits the C braces. */
static void emit_case_body(CodeGenerator* gen, ASTNode* c, int first) {
    int saved_var_count = gen->declared_var_count;
    enter_scope(gen);
    for (int i = first; i < c->child_count; i++) generate_statement(gen, c->children[i]);
    exit_scope(gen);
    truncate_declared_vars(gen, saved_var_count);
}

// #340: emit `value` coerced to the optional type `target`. A bare value of
// the inner type is implicitly wrapped (`{.has=1, .val=value}`); `none`
// becomes `{0}`; an expression already of optional type passes through. Used
// wherever a value flows into a `T?` slot — var-decl init, assignment, return,
// call argument.
void emit_optional_coerced(CodeGenerator* gen, ASTNode* value, Type* target) {
    if (!value || !target || target->kind != TYPE_OPTIONAL) {
        if (value) generate_expression(gen, value);
        return;
    }
    const char* tc = get_c_type(target);
    if (value->type == AST_NONE_LITERAL) {
        fprintf(gen->output, "(%s){0}", tc);
        return;
    }
    if (value->node_type && value->node_type->kind == TYPE_OPTIONAL) {
        generate_expression(gen, value);   // already an optional
        return;
    }
    if (type_is_sized_array(target->element_type)) {
        emit_optional_array_wrap(gen, value, target);
        return;
    }
    fprintf(gen->output, "(%s){ .has = 1, .val = ", tc);
    Type* inner = target->element_type;
    if (value->type == AST_TUPLE_LITERAL && inner && inner->kind == TYPE_TUPLE) {
        /* A tuple literal is no C expression of its own: it is the
         * tuple's compound literal, each element cast to its slot's type,
         * as an extern's tuple argument is packed (#1033). Emitted as an
         * expression it was its elements' text run together (`(3, 4)`
         * became `34`). */
        fprintf(gen->output, "(%s){ ", get_c_type(inner));
        for (int i = 0; i < value->child_count; i++) {
            Type* et = i < inner->tuple_count && inner->tuple_types ? inner->tuple_types[i] : NULL;
            fprintf(gen->output, "%s(%s)(", i > 0 ? ", " : "", et ? get_c_type(et) : "int");
            generate_expression(gen, value->children[i]);
            fprintf(gen->output, ")");
        }
        fprintf(gen->output, " }");
    } else if (inner && inner->kind == TYPE_SUM && needs_sum_coerce(value, inner)) {
        /* A variant into a sum's optional is wrapped into the sum first, as
         * it is into the sum itself (#914): the optional holds a Shape, not
         * the Circle. */
        emit_sum_coerced(gen, value, inner);
    } else {
        generate_expression(gen, value);
    }
    fprintf(gen->output, " }");
}

/* #2794, #2795: the C text of slot `j` of tuple temporary `_tup<id>`, of
 * position type `pt`, as an optional of C type `dst_c` holds it: an
 * optional position as it is, any other wrapped, as emit_optional_coerced
 * wraps a value. For a holder that owns its string (`own_str`: a
 * `string?` cell or global), the string is its own: adopted when the call
 * handed it over (`owned`), copied otherwise (_aether_str_cell_own). An
 * optional position is taken by its holder once bound (emit_opt_owner_store,
 * #2885): `*take` says how (-1 when the text is already its own). */
static const char* opt_slot_text(const char* dst_c, Type* pt, int tmp_id, int j, int owned,
                                 int own_str, int* take) {
    int optstr = own_str && strcmp(dst_c, "ae_opt_string") == 0;
    if (take) *take = -1;
    if (pt && pt->kind == TYPE_OPTIONAL) {
        if (optstr && take) *take = owned ? 1 : 0;
        return cg_internf("_tup%d._%d", tmp_id, j);
    }
    if (optstr)
        return cg_internf("(ae_opt_string){ .has = 1, .val = _aether_str_cell_own(_tup%d._%d, %d) }",
                          tmp_id, j, owned ? 1 : 0);
    return cg_internf("(%s){ .has = 1, .val = _tup%d._%d }", dst_c, tmp_id, j);
}

/* #2796: the type of the module-level `var` `name`, or NULL. */
static Type* module_global_decl_type(CodeGenerator* gen, const char* name) {
    for (int i = 0; gen->program && name && i < gen->program->child_count; i++) {
        ASTNode* c = gen->program->children[i];
        if (c && c->type == AST_EXPORT_STATEMENT && c->child_count > 0) c = c->children[0];
        if (c && (c->type == AST_CONST_DECLARATION || c->type == AST_VARIABLE_DECLARATION) &&
            c->annotation && strcmp(c->annotation, "global_var") == 0 && c->value &&
            strcmp(c->value, name) == 0)
            return c->node_type;
    }
    return NULL;
}

/* #2796: slot `j` of `_tup<id>`, of position type `pt`, into the
 * module-level `var` `name`, taken as `name = value` takes a value into a
 * global (#701, #2461, #2525): the slot's value (adopted when the call
 * handed it over, `owned` / `closure_owned`, else taken), published since
 * any thread may read the global (#2919), and the one the global held
 * released. An optional global (#2800) takes the slot as the optional, a
 * bare value wrapped (emit_opt_owner_store). */
static void emit_destructure_global_store(CodeGenerator* gen, const char* name, Type* pt,
                                          int tmp_id, int j, int owned, int closure_owned) {
    Type* gt = module_global_decl_type(gen, name);
    if (gt && gt->kind == TYPE_OPTIONAL) {
        const char* gc = cg_intern(get_c_type(gt));
        int take;
        const char* text = opt_slot_text(gc, pt, tmp_id, j, owned, 1, &take);
        emit_opt_owner_store(gen, name, gt, NULL, text, take, 1);
        if (own_type_owns(gen, gt)) own_emit_part(gen, gt, name, OWN_PUBLISH);
        return;
    }
    if (pt && pt->kind == TYPE_FUNCTION && !pt->is_fnptr) {
        fprintf(gen->output, "{ void* _ae_old = %s.env; %s = %s_tup%d._%d%s; "
                "_aether_closure_env_release(_ae_old); }\n",
                name, name, closure_owned ? "(" : "_aether_closure_retain(", tmp_id, j, ")");
        return;
    }
    if (pt && pt->kind == TYPE_STRING && gt && gt->kind == TYPE_PTR) {
        fprintf(gen->output, "%s = (void*)aether_uniform_heap_str(_tup%d._%d, %d);\n",
                name, tmp_id, j, owned ? 1 : 0);
        return;
    }
    if (pt && pt->kind == TYPE_STRING) {
        /* As `g = v` stores it: taken, published, the old one released. */
        fprintf(gen->output, "{ const char* _ae_old = %s; %s = aether_uniform_heap_str(_tup%d._%d, %d); "
                "aether_string_publish(%s); aether_heap_str_free(_ae_old); }\n",
                name, name, tmp_id, j, owned ? 1 : 0, name);
        return;
    }
    if (pt && own_type_owns(gen, pt) && pt->kind != TYPE_ARRAY) {
        /* A struct or composite: the global's own copy (#2921). */
        const char* src = cg_internf("_tup%d._%d", tmp_id, j);
        fprintf(gen->output, "{ %s _ae_old = %s; %s = %s;\n", get_c_type(pt), name, name, src);
        if (!owned) own_emit_part(gen, pt, name, OWN_RETAIN);
        own_emit_part(gen, pt, name, OWN_PUBLISH);
        own_emit_part(gen, pt, "_ae_old", OWN_DROP);
        print_indent(gen);
        fprintf(gen->output, "}\n");
        return;
    }
    fprintf(gen->output, "%s = _tup%d._%d;\n", name, tmp_id, j);
}

/* #2806: slot `j` of `_tup<id>`, of position type `pt`, into the state
 * field `name` of the actor being generated, taken as the bare `name = v`
 * takes a value into state: a string of its own (adopted when the call
 * handed it over, `owned`, taken otherwise), the one it held released; a reference of its
 * own to a closure, giving back the one it held; a struct that owns strings
 * replaced (the tuple's struct is the destructure's, #752); an optional a
 * bare value wraps into. */
static void emit_destructure_state_store(CodeGenerator* gen, const char* name, Type* pt,
                                         int tmp_id, int j, int owned, int closure_owned) {
    if (pt && pt->kind == TYPE_STRING) {
        fprintf(gen->output, "{ const char* _tmp_old = self->%s; self->%s = %s_tup%d._%d%s;",
                name, name, owned ? "(" : "aether_uniform_heap_str(", tmp_id, j,
                owned ? ")" : ", 0)");
        fprintf(gen->output, " aether_heap_str_free(_tmp_old); }\n");
        return;
    }
    if (pt && pt->kind == TYPE_FUNCTION && !pt->is_fnptr) {
        fprintf(gen->output, "{ void* _ae_old = self->%s.env; self->%s = %s_tup%d._%d%s; "
                "_aether_closure_env_release(_ae_old); }\n",
                name, name, closure_owned ? "(" : "_aether_closure_retain(", tmp_id, j, ")");
        return;
    }
    const char* sname = struct_owning_strings(gen, pt);
    if (sname) {
        fprintf(gen->output, "_aether_store_%s(&self->%s, _tup%d._%d);\n", sname, name, tmp_id, j);
        return;
    }
    Type* st = actor_state_decl(gen, name) ? actor_state_decl(gen, name)->node_type : NULL;
    if (st && st->kind == TYPE_OPTIONAL) {
        fprintf(gen->output, "self->%s = %s;\n", name,
                opt_slot_text(cg_intern(get_c_type(st)), pt, tmp_id, j, 0, 0, NULL));
        return;
    }
    fprintf(gen->output, "self->%s = _tup%d._%d;\n", name, tmp_id, j);
}

static void emit_match_result_value(CodeGenerator* gen, ASTNode* result);

// #2459: a match arm's block body is its own scope, the way an `if` / `switch`
// arm's block is (AST_BLOCK): its defers run when the arm ends, only when that
// arm was taken, and the names it declares do not leak into the next arm. The
// caller has already opened the arm's C braces.
//
// #2496: when the match is an expression, the block yields its final value
// (match_arm_value) to the match's result, inside the block's scope so its
// locals are still live and before its defers run, as a return's value is
// taken before a function's. The block's other statements are statements:
// a `match` among them is not the outer match's value, so the result
// variable is out of reach while they are emitted.
static void emit_match_arm_block(CodeGenerator* gen, ASTNode* block) {
    int saved_var_count = gen->declared_var_count;
    const char* result_var = gen->match_result_var;
    int result_string = gen->match_result_string;
    const char* result_struct = gen->match_result_struct;
    ASTNode* value = result_var ? match_arm_value(block) : NULL;
    int stmts = block->child_count - (value ? 1 : 0);
    gen->match_result_var = NULL;
    gen->match_result_string = 0;
    gen->match_result_struct = NULL;
    enter_scope(gen);
    for (int j = 0; j < stmts; j++) {
        generate_statement(gen, block->children[j]);
    }
    gen->match_result_var = result_var;
    gen->match_result_string = result_string;
    gen->match_result_struct = result_struct;
    if (value && value->type == AST_MATCH_STATEMENT) {
        /* A nested match yields for this one: its arms assign the same
         * result. */
        generate_statement(gen, value);
    } else if (value) {
        emit_match_result_value(gen, value);
    }
    gen->match_result_var = NULL;
    gen->match_result_string = 0;
    gen->match_result_struct = NULL;
    exit_scope(gen);
    gen->match_result_var = result_var;
    gen->match_result_string = result_string;
    gen->match_result_struct = result_struct;
    truncate_declared_vars(gen, saved_var_count);
}

// A match arm's value: assigned to the match's result variable when the
// match is an expression, else evaluated as a statement. #2461: an owning
// string result takes the value (emit_string_take) and records in the
// match's flag whether it owns it.
static void emit_match_result_value_now(CodeGenerator* gen, ASTNode* result);

/* #2582: an arm's value, with the temporaries of the structs calls in it
 * return, destroyed once the match's result has taken what it keeps. */
static void emit_match_result_value(CodeGenerator* gen, ASTNode* result) {
    ValueTemps vt;
    int has = value_temps_open_expr(gen, result,
                                    gen->match_result_var ? TEMP_KEPT : TEMP_FREE, &vt);
    /* #2798: the constructs that run statements in the value, run ahead of
     * it as a statement's are (stmt_control_prelude). */
    int ctl_depth = control_prelude_depth();
    int order_depth = order_prelude_depth();
    if (hoist_ready(gen) && ctl_contains(gen, result)) ctl_linearize_value(gen, result, 0);
    emit_match_result_value_now(gen, result);
    control_prelude_end(ctl_depth);
    order_prelude_end(order_depth);
    if (has) temps_close(gen, &vt, 1);
}

static void emit_match_result_value_now(CodeGenerator* gen, ASTNode* result) {
    print_indent(gen);
    if (gen->match_result_var && gen->match_result_cell) {
        /* #2514: a closure's variable takes the arm's value in its cell. */
        emit_cell_store(gen, gen->match_result_var, gen->match_result_cell, result);
        return;
    }
    if (gen->match_result_var && gen->match_result_struct) {
        /* #2497: an owning struct result takes the arm's struct. */
        if (gen->match_result_replace) {
            fprintf(gen->output, "%s(&%s, ",
                    struct_replace_fn(gen->match_result_struct,
                                      is_try_clobbered_var(gen, gen->match_result_var)),
                    gen->match_result_var);
        } else {
            fprintf(gen->output, "%s = ", gen->match_result_var);
        }
        emit_struct_take(gen, result, gen->match_result_struct, gen->match_result_var);
        fprintf(gen->output, gen->match_result_replace ? ");\n" : ";\n");
        return;
    }
    if (gen->match_result_var && gen->match_result_string) {
        emit_string_take_rebind(gen, gen->match_result_var, result);
        return;
    }
    if (gen->match_result_var && gen->match_result_opt) {
        /* An optional result takes the arm's value as a binding of the
         * optional would: wrapped, or `none`; one that owns drops what it
         * held (R3). */
        if (own_type_owns(gen, gen->match_result_opt)) {
            emit_owning_store(gen, gen->match_result_var, gen->match_result_opt, result, 0);
            return;
        }
        fprintf(gen->output, "%s = ", gen->match_result_var);
        emit_optional_coerced(gen, result, gen->match_result_opt);
        fprintf(gen->output, ";\n");
        return;
    }
    if (gen->match_result_var) {
        fprintf(gen->output, "%s = ", gen->match_result_var);
        generate_expression(gen, result);
        fprintf(gen->output, ";\n");
        return;
    }
    /* A match whose value nothing reads drops the arm's value, and the C
     * says so: an arm `1 -> 10` was the statement `10;`, which clang warns
     * computes nothing (-Wunused-value, on by default), and that warning
     * reached the user from a correct program. A void call is fine under
     * the cast too. */
    fprintf(gen->output, "(void)(");
    generate_expression(gen, result);
    fprintf(gen->output, ");\n");
}

// #340: emit a match arm's result body — mirrors the generic match dispatch
// (block / statement / expression), including match-as-expression's
// `match_result_var` assignment so `let r = match m { ... }` works.
static void emit_opt_match_arm(CodeGenerator* gen, ASTNode* result) {
    if (!result) return;
    if (result->type == AST_BLOCK) {
        emit_match_arm_block(gen, result);
    } else if (result->type == AST_PRINT_STATEMENT ||
               result->type == AST_RETURN_STATEMENT ||
               result->type == AST_VARIABLE_DECLARATION) {
        generate_statement(gen, result);
    } else {
        emit_match_result_value(gen, result);
    }
}

/* #2740: a `string?` a function returns is its caller's to free when it
 * holds a string (is_heap_opt_string_rhs takes a call's result as owned), so
 * each return hands over a string of its own, as a `-> string` return does
 * (emit_uniform_heap_return_expr): a fresh one, or a heap-tracked local's
 * own, is handed over as it is, and anything borrowed (a parameter, a
 * literal, an element of an array or a slice, a field) is copied. An
 * optional value is passed on when it comes from a call (which handed it
 * over) or from a `string?` local that owns its string (whose exit free the
 * return suppresses), and its string copied otherwise. Before, a borrowed
 * string was returned as it was, and the caller freed it. */
static void emit_opt_string_return(CodeGenerator* gen, ASTNode* v, Type* rt) {
    const char* tc = get_c_type(rt);
    if (v->type == AST_NONE_LITERAL) {
        fprintf(gen->output, "(%s){0}", tc);
        return;
    }
    if (v->type == AST_IF_EXPRESSION && v->child_count >= 3) {
        /* Each arm as a return of its own. */
        fprintf(gen->output, "((");
        generate_expression(gen, v->children[0]);
        fprintf(gen->output, ") ? ");
        emit_opt_string_return(gen, v->children[1], rt);
        fprintf(gen->output, " : ");
        emit_opt_string_return(gen, v->children[2], rt);
        fprintf(gen->output, ")");
        return;
    }
    if (!(v->node_type && v->node_type->kind == TYPE_OPTIONAL)) {
        fprintf(gen->output, "(%s){ .has = 1, .val = ", tc);
        emit_uniform_heap_return_expr(gen, v);
        fprintf(gen->output, " }");
        return;
    }
    if (v->type == AST_FUNCTION_CALL) {
        generate_expression(gen, v);
        return;
    }
    /* A place keeps its string: the caller gets a reference of its own. */
    TempSeq s;
    seq_open(gen, &s);
    const char* ro = seq_let(gen, &s, tc, "_ae_ro");
    generate_expression(gen, v);
    seq_next(gen, &s);
    fprintf(gen->output, "%s.has ? (void)(%s.val = aether_uniform_heap_str(%s.val, 0)) : (void)0",
            ro, ro, ro);
    seq_next(gen, &s);
    fprintf(gen->output, "%s", ro);
    seq_close(gen, &s);
}

/* #2751: does tuple call `call` hand over every `string?` position owned? A
 * named function does, and so does a closure literal this program names
 * (call_targets_closure_literal), both through emit_tuple_return_position.
 * A C function or a closure the compiler cannot name is not held to it. */
static int tuple_call_owns_opt_strings(CodeGenerator* gen, ASTNode* call) {
    if (!gen || !gen->program || !call || call->type != AST_FUNCTION_CALL || !call->value)
        return 0;
    if (strcmp(call->value, "call") == 0) return call_targets_closure_literal(gen, call);
    ASTNode* def = find_function_definition_by_name(gen->program,
                                                    codegen_normalise_callee(call->value));
    return def && def->type == AST_FUNCTION_DEFINITION && !typed_fnptr_call(gen, call);
}

/* What the tuple call `rhs` (whose definition is `callee_def`, or NULL)
 * hands over at position `j`, for the code that binds it or throws it
 * away: a heap string, a `string?` it owns, or a closure whose env
 * reference it gives. */
typedef struct {
    int is_string;
    int is_heap;
    int opt_owned;
    int is_closure;
    int closure_owned;
} TuplePosOwn;

static TuplePosOwn tuple_pos_own(CodeGenerator* gen, ASTNode* rhs, ASTNode* callee_def, int j) {
    Type* rhs_type = rhs ? rhs->node_type : NULL;
    Type* pt = rhs_type && rhs_type->kind == TYPE_TUPLE && j < rhs_type->tuple_count
               ? rhs_type->tuple_types[j] : NULL;
    TuplePosOwn po = { 0, 0, 0, 0, 0 };
    /* Defaults to 0 for any case the analyzer can't classify. */
    po.is_string = pt && pt->kind == TYPE_STRING;
    if (po.is_string && tuple_call_pos_copied(gen, rhs, j)) {
        po.is_heap = 1;   /* #2619: copied where the call was made */
    } else if (po.is_string) {
        if (callee_def) {
            po.is_heap = function_def_returns_heap_at(gen, callee_def, j);
        } else if (rhs_type->tuple_heap_flags) {
            po.is_heap = rhs_type->tuple_heap_flags[j];
        } else if (call_targets_closure_literal(gen, rhs)) {
            /* #2501: a closure's string slots are owned
             * (emit_tuple_return_position). */
            po.is_heap = 1;
        }
    }
    /* #2751: a `string?` position a named function or a closure literal
     * hands over owned (emit_tuple_return_position, as a single `string?`
     * result, #2740): the local it binds owns its string, as one a single
     * `string?` call is bound to does. It was taken with its flag left 0, so
     * a fresh string leaked on every call. */
    po.opt_owned = pt && is_opt_string_type(pt) && tuple_call_owns_opt_strings(gen, rhs);
    /* R3: every closure position of a call is handed over, but a C
     * function's: the local it binds releases it, `_` at once. */
    po.is_closure = pt && pt->kind == TYPE_FUNCTION && !pt->is_fnptr;
    po.closure_owned = po.is_closure && rhs && rhs->type == AST_FUNCTION_CALL &&
                       !call_is_extern(gen, rhs);
    return po;
}

static ASTNode* tuple_call_def(CodeGenerator* gen, ASTNode* call) {
    if (!call || call->type != AST_FUNCTION_CALL || !call->value || !gen->program) return NULL;
    return find_function_definition_by_name(gen->program, codegen_normalise_callee(call->value));
}

/* A tuple call thrown away, `pair(w)` as a statement: does it hand over
 * something at some position (tuple_pos_own)? */
static int tuple_call_discard_owns(CodeGenerator* gen, ASTNode* call) {
    if (!call || call->type != AST_FUNCTION_CALL || !call->node_type ||
        call->node_type->kind != TYPE_TUPLE) return 0;
    ASTNode* def = tuple_call_def(gen, call);
    for (int j = 0; j < call->node_type->tuple_count; j++) {
        TuplePosOwn po = tuple_pos_own(gen, call, def, j);
        if (po.is_heap || po.opt_owned || po.closure_owned) return 1;
    }
    return 0;
}

/* Each thing it hands over is given back, as `_, _ = pair(w)` gives it
 * back (the destructure's `_` slots). Thrown away bare, its strings
 * leaked. */
static void emit_tuple_call_discard(CodeGenerator* gen, ASTNode* call) {
    static int td_seq = 0;
    const char* td = cg_internf("_ae_td%d", td_seq++);
    ASTNode* def = tuple_call_def(gen, call);
    ensure_tuple_typedef(gen, call->node_type);
    fprintf(gen->output, "{ %s %s = ", get_c_type(call->node_type), td);
    generate_expression(gen, call);
    fprintf(gen->output, ";");
    for (int j = 0; j < call->node_type->tuple_count; j++) {
        TuplePosOwn po = tuple_pos_own(gen, call, def, j);
        if (po.is_heap)
            fprintf(gen->output, " if (%s._%d) aether_heap_str_free(%s._%d);", td, j, td, j);
        else if (po.opt_owned)
            fprintf(gen->output, " if (%s._%d.has) aether_heap_str_free((void*)%s._%d.val);",
                    td, j, td, j);
        else if (po.closure_owned)
            fprintf(gen->output, " _aether_closure_env_release(%s._%d.env);", td, j);
    }
    fprintf(gen->output, " }");
}

static int emit_tuple_passthrough_return(CodeGenerator* gen, ASTNode* v) {
    Type* rt = gen->current_func_return_type;
    if (!rt || rt->kind != TYPE_TUPLE || gen->in_main_function || !gen->current_function ||
        !v || !v->node_type || v->node_type->kind != TYPE_TUPLE || !own_type_owns(gen, rt))
        return 0;
    /* R3: the caller gets references of its own, a call's moved through. */
    emit_composite_take(gen, v, rt);
    return 1;
}

// Emit a function's return value. #340: when the return type is `T?`,
// coerce a bare value / `none` into the optional (`return 5` / `return none`).
// Otherwise use the uniform-heap-return wrap or the plain expression.
static void emit_return_value(CodeGenerator* gen, ASTNode* stmt) {
    if (!stmt || stmt->child_count == 0) return;
    /* #2894: an array result goes back in its struct. */
    if (!gen->in_main_function && is_sized_array_param(gen->current_func_return_type)) {
        emit_array_result_value(gen, stmt->children[0], gen->current_func_return_type);
        return;
    }
    if (!gen->in_main_function && is_opt_string_type(gen->current_func_return_type)) {
        emit_opt_string_return(gen, stmt->children[0], gen->current_func_return_type);
        return;
    }
    if (emit_tuple_passthrough_return(gen, stmt->children[0])) return;   /* #2751 */
    if (gen->current_func_return_type &&
        gen->current_func_return_type->kind == TYPE_OPTIONAL &&
        needs_optional_coerce(stmt->children[0], gen->current_func_return_type)) {
        emit_optional_coerced(gen, stmt->children[0], gen->current_func_return_type);
        return;
    }
    // #914: `return Circle {...}` from a `-> Shape` function wraps the variant.
    if (gen->current_func_return_type &&
        gen->current_func_return_type->kind == TYPE_SUM &&
        needs_sum_coerce(stmt->children[0], gen->current_func_return_type)) {
        emit_sum_coerced(gen, stmt->children[0], gen->current_func_return_type);
        return;
    }
    // #913: `return value` from a `T!` function wraps the success value into
    // the `(value, "")` result tuple. An explicit `return value, "err"` is a
    // 2-value return handled by the multi-return path and never reaches here;
    // a value that is already the result tuple (forwarding another result)
    // passes through unchanged.
    if (gen->current_func_return_type &&
        gen->current_func_return_type->is_result) {
        ASTNode* v = stmt->children[0];
        if (!(v->node_type && v->node_type->kind == TYPE_TUPLE)) {
            fprintf(gen->output, "(%s){ ._0 = ",
                    get_c_type(gen->current_func_return_type));
            /* When position 0 of this `T!` is classified heap, EVERY
             * value-slot return, including this single-value auto-wrap,
             * hands the caller one reference (R3), exactly as the
             * multi-value `emit_tuple_return_position` does. Gate on the
             * same classifier the caller's destructure uses, and only for a
             * string value slot. */
            Type* v0 = gen->current_func_return_type->tuple_types
                       ? gen->current_func_return_type->tuple_types[0] : NULL;
            int wrap_v0 = v0 && v0->kind == TYPE_STRING &&
                          gen->current_function &&
                          function_def_returns_heap_at(gen, gen->current_function, 0);
            if (wrap_v0) {
                emit_uniform_heap_return_expr(gen, v);
            } else {
                generate_expression(gen, v);
            }
            /* The success sentinel is a string the caller owns when the
             * error position is heap (some return builds its error), as
             * every other return there hands one over: the `or` and the
             * destructure free that slot on both paths. */
            Type* v1 = gen->current_func_return_type->tuple_count > 1 &&
                       gen->current_func_return_type->tuple_types
                       ? gen->current_func_return_type->tuple_types[1] : NULL;
            int wrap_v1 = v1 && v1->kind == TYPE_STRING && gen->current_function &&
                          !gen->in_main_function &&
                          (gen->current_function->type == AST_FUNCTION_DEFINITION ||
                           gen->current_function->type == AST_BUILDER_FUNCTION) &&
                          function_def_returns_heap_at(gen, gen->current_function, 1);
            fprintf(gen->output, wrap_v1
                    ? ", ._1 = aether_uniform_heap_str((const char*)(\"\"), 0) }"
                    : ", ._1 = \"\" }");
            return;
        }
    }
    /* R3: a struct returned hands the caller a value of its own: a local
     * moves out on its last use, anything else (a parameter, a field, an
     * element, a capture) is copied, an `if` arm by arm. */
    {
        ASTNode* v = stmt->children[0];
        /* A closure body has no declared return type here: its value's
         * type says. */
        Type* srt = gen->current_func_return_type ? gen->current_func_return_type
                                                   : (v ? v->node_type : NULL);
        const char* rs = struct_owning_strings(gen, srt);
        if (rs && v) {
            emit_struct_take(gen, v, rs, NULL);
            return;
        }
        /* R3: a closure returned hands the caller a reference of its own:
         * a fresh closure or a call's result moves, anything else (a local,
         * a parameter, a field, an element, a capture) is retained, and
         * its holder releases its own. */
        Type* rt = srt;
        if (rt && rt->kind == TYPE_FUNCTION && !rt->is_fnptr && v && !gen->in_main_function) {
            emit_closure_take(gen, v);
            return;
        }
        /* And a string sequence the same way. */
        if (is_string_seq_ptr_type(rt) && v && !gen->in_main_function) {
            emit_seq_take(gen, v);
            return;
        }
    }
    if (should_uniform_heap_return(gen, stmt)) {
        emit_uniform_heap_return_expr(gen, stmt->children[0]);
    } else if (gen->current_func_return_type &&
               emit_int_ptr_bridged(gen, stmt->children[0],
                                    gen->current_func_return_type->kind)) {
        /* #2218: int <-> ptr return, cast emitted. */
    } else {
        generate_expression(gen, stmt->children[0]);
    }
}

// #340: does `value` need optional coercion to flow into `target`? True when
// target is `T?` and value isn't already that optional (a bare T, or `none`).
int needs_optional_coerce(ASTNode* value, Type* target) {
    if (!value || !target || target->kind != TYPE_OPTIONAL) return 0;
    if (value->type == AST_NONE_LITERAL) return 1;
    if (value->node_type && value->node_type->kind == TYPE_OPTIONAL) return 0;
    return 1;
}

// #914: does `value` need sum coercion to flow into `target`? True when target
// is a sum and value is one of its variant structs (not already the sum).
int needs_sum_coerce(ASTNode* value, Type* target) {
    if (!value || !target || target->kind != TYPE_SUM) return 0;
    Type* vt = value->node_type;
    if (!vt || vt->kind != TYPE_STRUCT || !vt->struct_name) return 0;
    for (int i = 0; i < target->tuple_count; i++) {
        Type* var = target->tuple_types[i];
        if (var && var->struct_name &&
            strcmp(var->struct_name, vt->struct_name) == 0) return 1;
    }
    return 0;
}

// #914: wrap a variant struct value into its sum:
//   (Shape){ .tag = Shape__Circle, .data.Circle_ = <value> }
// If `value` isn't a coercible variant, emit it bare.
void emit_sum_coerced(CodeGenerator* gen, ASTNode* value, Type* target) {
    if (!needs_sum_coerce(value, target)) {
        if (value) generate_expression(gen, value);
        return;
    }
    const char* sname = target->struct_name;
    const char* variant = value->node_type->struct_name;
    fprintf(gen->output, "(%s){ .tag = %s__%s, .data.%s_ = ",
            sname, sname, variant, variant);
    generate_expression(gen, value);
    fprintf(gen->output, " }");
}

// #340: optional-chain assignment `recv?.field = rhs` — a no-op when the
// optional is `none`, else writes the field. Returns 1 if it emitted the
// store (so the caller skips its normal assignment path), 0 otherwise.
// `&(recv)` evaluates the receiver once and works for any lvalue receiver
// (variable / array element / struct field); the `->val` access uses
// `->`/`.` per whether the wrapped type is a pointer-to-struct or a value
// struct. Both assignment shapes the parser can produce route here:
// AST_ASSIGNMENT and AST_EXPRESSION_STATEMENT > AST_BINARY_EXPRESSION(`=`).
static int emit_optional_chain_assign(CodeGenerator* gen, ASTNode* lhs, ASTNode* rhs) {
    if (!lhs || lhs->type != AST_OPTIONAL_CHAIN || !lhs->value ||
        lhs->child_count == 0)
        return 0;
    ASTNode* recv = lhs->children[0];
    Type* ot = recv ? recv->node_type : NULL;
    if (!ot || ot->kind != TYPE_OPTIONAL) return 0;
    Type* inner = ot->element_type;
    const char* acc = (inner && inner->kind == TYPE_PTR) ? "->" : ".";
    static int oca_counter = 0;
    int id = oca_counter++;
    fprintf(gen->output, "{ %s* _oca%d = &(", get_c_type(ot), id);
    generate_expression(gen, recv);
    fprintf(gen->output, "); if (_oca%d->has) _oca%d->val%s%s = ",
            id, id, acc, lhs->value);
    generate_expression(gen, rhs);
    fprintf(gen->output, "; }\n");
    return 1;
}

/* #2220 — observable struct models.
 *
 * A statement that stores into a field (`m.f = v`, `p.f = v`, `a.b.c = v`,
 * and the compound forms the parser desugars to them) is followed by one
 * `aether_observe_notify(<object>)` per observable object whose bytes the
 * store changed, innermost first:
 *   - a value-typed base (`m` : T, or `a.b` : T) is notified by address
 *     (`&m`), and the walk continues outward because the enclosing value's
 *     bytes changed too;
 *   - a pointer-typed base (`p` : *T) is notified by the pointer itself, and
 *     the walk stops: the holder of the pointer did not change.
 * The hook runs after the statement was emitted whichever assignment path
 * emitted it (heap-string tracking, @c_struct overlays, trailing-block
 * builders), which is why it lives at the generate_statement boundary rather
 * than inside each path. */
int struct_is_observable(CodeGenerator* gen, const char* struct_name) {
    if (!gen || !struct_name) return 0;
    ASTNode* sd = find_struct_definition_by_name(gen->program, struct_name);
    return sd && annotation_has_marker(sd->annotation, "observable");
}

/* The field-store LHS of `stmt`, or NULL when the statement is not one. */
static ASTNode* field_store_lhs(ASTNode* stmt) {
    if (!stmt) return NULL;
    ASTNode* lhs = NULL;
    if (stmt->type == AST_ASSIGNMENT && stmt->child_count >= 2) {
        lhs = stmt->children[0];
    } else if (stmt->type == AST_EXPRESSION_STATEMENT && stmt->child_count > 0) {
        ASTNode* inner = stmt->children[0];
        if (inner && inner->type == AST_BINARY_EXPRESSION && inner->value &&
            strcmp(inner->value, "=") == 0 && inner->child_count == 2) {
            lhs = inner->children[0];
        }
    }
    return (lhs && lhs->type == AST_MEMBER_ACCESS && lhs->child_count >= 1) ? lhs : NULL;
}

/* The index of field `field` in struct `struct_name`, in declaration order
 * (0 for the first), or -1 (any field) when it is not one of its fields. */
static int observable_field_index(CodeGenerator* gen, const char* struct_name,
                                  const char* field) {
    ASTNode* sd = find_struct_definition_by_name(gen->program, struct_name);
    if (!sd || !field) return -1;
    int idx = 0;
    for (int i = 0; i < sd->child_count; i++) {
        ASTNode* f = sd->children[i];
        if (!f || f->type != AST_STRUCT_FIELD) continue;
        if (f->value && strcmp(f->value, field) == 0) return idx;
        idx++;
    }
    return -1;
}

/* Notify `base`'s observers that its field `field` (an index) changed
 * (#2299: the field, so a replicator or undo log knows which). */
static void emit_observe_notify(CodeGenerator* gen, ASTNode* base, int by_address, int field) {
    print_indent(gen);
    fprintf(gen->output, "aether_observe_notify_field(%s(", by_address ? "&" : "");
    int saved = gen->generating_lvalue;
    gen->generating_lvalue = 1;
    generate_expression(gen, base);
    gen->generating_lvalue = saved;
    fprintf(gen->output, "), %d);\n", field);
}

static void emit_observable_store_notify(CodeGenerator* gen, ASTNode* stmt) {
    ASTNode* node = field_store_lhs(gen ? stmt : NULL);
    /* At each level `node` is `base.field`: the field of `base` that holds
     * what changed, so an outer object hears which of its own fields did. */
    while (node && node->type == AST_MEMBER_ACCESS && node->child_count >= 1) {
        ASTNode* base = node->children[0];
        Type* bt = base ? base->node_type : NULL;
        if (bt && bt->kind == TYPE_STRUCT) {
            if (struct_is_observable(gen, bt->struct_name))
                emit_observe_notify(gen, base, 1,
                                    observable_field_index(gen, bt->struct_name, node->value));
            node = base;   /* the enclosing value changed too */
            continue;
        }
        if (bt && bt->kind == TYPE_PTR && bt->element_type &&
            bt->element_type->kind == TYPE_STRUCT) {
            const char* sn = bt->element_type->struct_name;
            if (struct_is_observable(gen, sn))
                emit_observe_notify(gen, base, 0, observable_field_index(gen, sn, node->value));
        }
        break;
    }
}

static void generate_statement_body(CodeGenerator* gen, ASTNode* stmt);

/* --- An expression's temporaries (#2765) ---------------------------------
 *
 * A construct that needs temporaries in the middle of an expression (an
 * argument freed after the call, a string taken into an owning slot, an
 * operand evaluated first) used to open a GNU statement expression
 * `({ T t = a; f(t); t; })`. That is not C: cl.exe rejects it, and so the
 * non-GCC path (AETHER_GCC_COMPAT 0) did not compile. It is now a comma
 * expression `(t = a, f(t), t)` whose temporaries are declared ahead of the
 * outermost statement generate_statement is emitting. That statement's text
 * is already written when the expression is, so the declarations are put in
 * front of it once it is done: its text is read back from the output and
 * written again after them. Declarations only: every temporary is assigned
 * where the statement expression initialised it, so it is evaluated exactly
 * as before, in a loop condition once per test. */

/* Can `output` be read back and rewritten? Probed once: a stream opened
 * for writing only (or a pipe) cannot, and its temporaries are then
 * statement-expression locals, as before. */
static int hoist_output_usable(CodeGenerator* gen) {
    if (gen->hoist_output_ok) return gen->hoist_output_ok > 0;
    FILE* out = gen->output;
    gen->hoist_output_ok = -1;
    /* An error already on the stream is left for the close to report: the
     * probe below clears the one a failed read leaves. */
    if (!out || ferror(out) || fflush(out) != 0) return 0;
    long at = ftell(out);
    if (at < 0 || fseek(out, at, SEEK_SET) != 0) return 0;
    (void)fgetc(out);
    int readable = !ferror(out);
    clearerr(out);
    if (fseek(out, at, SEEK_SET) != 0) return 0;
    if (readable) gen->hoist_output_ok = 1;
    return readable;
}

int hoist_ready(CodeGenerator* gen) {
    return gen->hoist_depth > 0 && gen->hoist_start >= 0;
}

static void hoist_append(CodeGenerator* gen, const char* text) {
    size_t n = strlen(text);
    if (gen->hoist_decls_len + n + 1 > gen->hoist_decls_cap) {
        size_t cap = gen->hoist_decls_cap ? gen->hoist_decls_cap * 2 : 256;
        while (cap < gen->hoist_decls_len + n + 1) cap *= 2;
        gen->hoist_decls = (char*)aether_xrealloc(gen->hoist_decls, cap);
        gen->hoist_decls_cap = cap;
    }
    memcpy(gen->hoist_decls + gen->hoist_decls_len, text, n + 1);
    gen->hoist_decls_len += n;
}

/* Where the outermost statement starts, judged by the code before it: 0
 * where a declaration may go (after `{`, `;` or `}`), 1 after a label (a
 * declaration may not follow one in C11, so a null statement goes first),
 * 2 where one statement is expected (the body of an unbraced `if`, `else` or
 * loop: the statement is wrapped in a block with its declarations). Lines
 * that are preprocessor directives and comments are skipped. */
int codegen_hoist_site(const char* s, size_t n) {
    size_t end = n;
    while (end > 0) {
        size_t ls = end;
        while (ls > 0 && s[ls - 1] != '\n') ls--;
        /* The line s[ls, end): drop a `//` comment outside literals, then
         * skip it if it is a directive or holds nothing. */
        size_t le = end;
        char quote = 0;
        for (size_t i = ls; i < end; i++) {
            if (quote) {
                if (s[i] == '\\') i++;
                else if (s[i] == quote) quote = 0;
            } else if (s[i] == '"' || s[i] == '\'') {
                quote = s[i];
            } else if (s[i] == '/' && i + 1 < end && s[i + 1] == '/') {
                le = i;
                break;
            }
        }
        size_t fs = ls;
        while (fs < le && (s[fs] == ' ' || s[fs] == '\t')) fs++;
        while (le > fs && (s[le - 1] == ' ' || s[le - 1] == '\t' ||
                           s[le - 1] == '\r' || s[le - 1] == '\n')) le--;
        if (fs < le && s[fs] == '#') le = fs;
        /* A block comment that ends the line: skip back to its start. */
        while (le >= fs + 2 && s[le - 1] == '/' && s[le - 2] == '*') {
            size_t k = le - 2;
            while (k >= 2 && !(s[k - 2] == '/' && s[k - 1] == '*')) k--;
            if (k < 2) return 0;
            le = k - 2;
            if (le < fs) {
                /* The comment began on an earlier line: carry on there. */
                end = le;
                goto next_line;
            }
            while (le > fs && (s[le - 1] == ' ' || s[le - 1] == '\t')) le--;
        }
        if (le > fs) {
            char c = s[le - 1];
            if (c == '{' || c == ';' || c == '}') return 0;
            if (c == ':') return 1;
            return 2;
        }
        end = ls > 0 ? ls - 1 : 0;
        continue;
    next_line:;
    }
    return 0;
}

/* Put the declarations collected for the statement whose text began at
 * `start` in front of it. They go on the statement's first line, so the
 * line numbers its #line directives give are kept; a statement that starts
 * with a directive gets them on a line of their own ahead of it. */
#define HOIST_LOOKBACK 1024
static void hoist_patch(CodeGenerator* gen, long start) {
    FILE* out = gen->output;
    char before[HOIST_LOOKBACK];
    char* body = NULL;
    if (fflush(out) != 0) goto fail;
    long end = ftell(out);
    if (end < start) goto fail;
    size_t n = (size_t)(end - start);
    long from = start > HOIST_LOOKBACK ? start - HOIST_LOOKBACK : 0;
    size_t nb = (size_t)(start - from);
    body = (char*)aether_xrealloc(NULL, n + 1);
    if (fseek(out, from, SEEK_SET) != 0 ||
        fread(before, 1, nb, out) != nb ||
        fread(body, 1, n, out) != n ||
        fseek(out, start, SEEK_SET) != 0) goto fail;
    int site = codegen_hoist_site(before, nb);
    size_t lead = 0;
    while (lead < n && (body[lead] == ' ' || body[lead] == '\t')) lead++;
    int directive_first = lead < n && body[lead] == '#';
    if (site == 1) fputs("; ", out);
    if (site == 2) fputs("{ ", out);
    fputs(gen->hoist_decls, out);
    if (directive_first) fputc('\n', out);
    if (site == 2) {
        /* Close the block on the statement's last line, unless that line
         * is a directive. */
        size_t k = n;
        while (k > 0 && (body[k - 1] == '\n' || body[k - 1] == '\r' ||
                         body[k - 1] == ' ' || body[k - 1] == '\t')) k--;
        size_t ls = k;
        while (ls > 0 && body[ls - 1] != '\n') ls--;
        while (ls < k && (body[ls] == ' ' || body[ls] == '\t')) ls++;
        if (ls < k && body[ls] == '#') {
            fwrite(body, 1, n, out);
            fputs(n > 0 && body[n - 1] == '\n' ? "}\n" : "\n}\n", out);
        } else {
            fwrite(body, 1, k, out);
            fputs(" }", out);
            fwrite(body + k, 1, n - k, out);
        }
    } else {
        fwrite(body, 1, n, out);
    }
    free(body);
    return;
fail:
    free(body);
    fseek(out, 0, SEEK_END);
    fprintf(out, "\n#error \"aetherc: could not declare an expression's temporaries\"\n");
}

static void hoist_begin(CodeGenerator* gen) {
    if (gen->hoist_depth++ > 0) return;
    gen->hoist_decls_len = 0;
    gen->hoist_start = hoist_output_usable(gen) ? ftell(gen->output) : -1;
}

static void hoist_end(CodeGenerator* gen) {
    if (--gen->hoist_depth > 0) return;
    long start = gen->hoist_start;
    gen->hoist_start = -1;
    if (start >= 0 && gen->hoist_decls_len > 0) hoist_patch(gen, start);
    gen->hoist_decls_len = 0;
}

/* The parts of a construct that needs temporaries (see codegen_internal.h). */
void seq_open(CodeGenerator* gen, TempSeq* s) {
    s->stmt_expr = !hoist_ready(gen);
    s->ordered = 0;
    fputs(s->stmt_expr ? "({ " : "(", gen->output);
}

/* An evaluation-order sequence (#2478): a temporary holds an operand as C
 * types it, inferred by __auto_type on the GCC path, so the operation reads
 * the value it would have read in place. Without __auto_type the operand's
 * Aether type (`typed`: every temporary has one) is spelled instead, as the
 * declaration ahead of the statement; the _AE_SEQ_* macros pick the
 * spelling on AETHER_GCC_COMPAT. Untyped, or with no statement to declare
 * ahead of, it stays a statement expression. */
void seq_open_ordered(CodeGenerator* gen, TempSeq* s, int typed) {
    s->stmt_expr = !typed || !hoist_ready(gen);
    s->ordered = 1;
    fputs(s->stmt_expr ? "({ " : "_AE_SEQ_OPEN ", gen->output);
}

/* A temporary of the sequence named `name` (unique to the translation unit:
 * temporaries declared ahead of one statement share its scope), and the
 * start of its assignment. */
void seq_let_named(CodeGenerator* gen, TempSeq* s, const char* ctype, const char* name) {
    if (s->stmt_expr) {
        fprintf(gen->output, "%s %s = ", s->ordered ? "__auto_type" : ctype, name);
        return;
    }
    hoist_append(gen, s->ordered ? cg_internf("_AE_C11_ONLY(%s %s;) ", ctype, name)
                                 : cg_internf("%s %s; ", ctype, name));
    if (s->ordered) fprintf(gen->output, "_AE_SEQ_LET(%s) ", name);
    else fprintf(gen->output, "%s = ", name);
}

void hoist_declare(CodeGenerator* gen, const char* ctype, const char* name) {
    hoist_append(gen, cg_internf("%s %s; ", ctype, name));
}

void hoist_declare_null(CodeGenerator* gen, const char* ctype, const char* name) {
    hoist_append(gen, cg_internf("%s %s = NULL; ", ctype, name));
}

void hoist_append_text(CodeGenerator* gen, const char* text) {
    hoist_append(gen, text);
}

void push_scoped_temp_free(CodeGenerator* gen, const char* name) {
    ASTNode* carrier = create_ast_node(AST_EXPRESSION_STATEMENT, NULL, 0, 0);
    if (!carrier) return;
    if (carrier->annotation) free(carrier->annotation);
    carrier->annotation = heap_strf("scoped_temp_free:%s", name);
    codegen_own_node(gen, carrier);
    push_defer(gen, carrier);
}

/* A temporary the sequence assigns later, in a part of its own (each arm of
 * a conditional): declared, and nothing written in the sequence for it but
 * the declaration a statement expression needs. Takes no seq_next. Its
 * type is spelled, never inferred (there is no value to infer it from), so
 * it is declared the same way in an ordered sequence. */
void seq_decl(CodeGenerator* gen, TempSeq* s, const char* ctype, const char* name) {
    if (s->stmt_expr) fprintf(gen->output, "%s %s; ", ctype, name);
    else hoist_declare(gen, ctype, name);
}

/* A fresh temporary `<prefix><n>`. The temporaries ahead of one statement
 * share its scope, and other counters number names of their own
 * (`_ae_own<n>`, `_ad_<n>`), so a prefix here is never one of theirs: two
 * counters would mint the same name. */
const char* seq_let(CodeGenerator* gen, TempSeq* s, const char* ctype, const char* prefix) {
    static int seq = 0;
    const char* name = cg_internf("%s%d", prefix, seq++);
    seq_let_named(gen, s, ctype, name);
    return name;
}

void seq_next(CodeGenerator* gen, TempSeq* s) {
    fputs(s->stmt_expr ? "; " : s->ordered ? " _AE_SEQ_NEXT " : ", ", gen->output);
}

void seq_close(CodeGenerator* gen, TempSeq* s) {
    fputs(s->stmt_expr ? "; })" : s->ordered ? " _AE_SEQ_CLOSE" : ")", gen->output);
}

/* The call whose trailing block this statement runs itself: a declaration's
 * initializer, an assignment's right side, an expression statement's call.
 * generate_expression lowers a trailing call anywhere else (a return value,
 * an argument) on its own; this tells it which one not to. */
static ASTNode* stmt_trailing_call(CodeGenerator* gen, ASTNode* stmt) {
    ASTNode* call = NULL;
    if (stmt->type == AST_VARIABLE_DECLARATION && stmt->child_count > 0) {
        call = stmt->children[0];
    } else if (stmt->type == AST_ASSIGNMENT && stmt->child_count >= 2) {
        call = stmt->children[1];
    } else if (stmt->type == AST_EXPRESSION_STATEMENT && stmt->child_count > 0) {
        call = stmt->children[0];
        if (call && call->type == AST_BINARY_EXPRESSION && call->value &&
            strcmp(call->value, "=") == 0 && call->child_count == 2) {
            call = call->children[1];
        }
    }
    if (!call || call->type != AST_FUNCTION_CALL) return NULL;
    return trailing_dsl_block(gen, call) ? call : NULL;
}

/* #2765, #2798: the constructs that run statements (emit_control_prelude)
 * in what `stmt` evaluates, run ahead of it with what it evaluates before
 * them (ctl_linearize_value): its value (an initializer, the value assigned
 * or added in, a returned value, an `if` condition or a `match` subject, an
 * expression statement), or its list of values (a multi-value `return`, a
 * `print`), or an assignment's target indexes and then its value. A loop's
 * condition is evaluated again on every pass, at the top of the body, and a
 * `for` loop's init and increment where they run (the loop emitters). A
 * statement that keeps the struct results of the calls in its value in
 * temporaries it opens itself (an `if` condition, a `match` subject, an
 * expression statement) has this run once they are open (`temps_open`), by
 * its emitter (stmt_control_prelude_deferred): the operands evaluated ahead
 * hold those results in them too. g_ctl_deferred is the statement waiting
 * for that. */
static ASTNode* g_ctl_deferred = NULL;

/* #2798: a builder call its statement runs (gen->trailing_stmt_call) runs
 * the builder's block first and evaluates its arguments after it, so the
 * constructs in them run there, from the statement's emitter, after the
 * block: stmt_control_run leaves such a statement alone. */
static int stmt_runs_builder(CodeGenerator* gen) {
    ASTNode* call = gen->trailing_stmt_call;
    return call && call->value && is_builder_func_reg(gen, call->value);
}

static void emit_builder_args_prelude(CodeGenerator* gen, ASTNode* call) {
    ctl_builder_args(gen, call);
}

/* The part of builder call `call` its statement runs ahead of it: a fresh
 * config pushed as the context, the trailing block run to fill it, then the
 * arguments (#2798). Returns the config's C name, which the statement's own
 * emitter then passes to the call (emit_builder_call): the call's value is
 * stored, or thrown away, as any call's is, a string bound to a tracked
 * local freeing the one it replaces, one thrown away freed. Assigned by
 * hand before, a builder's string result was never tracked, and leaked.
 * The config is declared ahead of the statement, so the statement itself
 * can be a declaration of what the call returns (#2874). */
static int g_builder_cfg_seq = 0;
static const char* emit_builder_prelude(CodeGenerator* gen, ASTNode* call) {
    const char* cfg = cg_internf("_bcfg%d", g_builder_cfg_seq++);
    print_indent(gen);
    /* (void*)(intptr_t) bridges int-returning factories (aether-ui's
     * `_surface_window_factory() -> int`): a bare int to pointer
     * conversion is an error under GCC 14. */
    if (hoist_ready(gen)) {
        hoist_declare(gen, "void*", cfg);
        fprintf(gen->output, "%s = (void*)(intptr_t)%s();\n", cfg,
                get_builder_factory(gen, call->value));
    } else {
        fprintf(gen->output, "void* %s = (void*)(intptr_t)%s();\n", cfg,
                get_builder_factory(gen, call->value));
    }
    print_indent(gen);
    fprintf(gen->output, "_aether_ctx_push(%s);\n", cfg);
    ASTNode* block = trailing_dsl_block(gen, call);
    if (block) emit_builder_block_body(gen, block, cfg, call->value);
    emit_builder_args_prelude(gen, call);
    return cfg;
}

/* Is `call` a builder call with a trailing block that a statement runs,
 * not yet configured (builder_call_configured)? */
static int builder_call_to_run(CodeGenerator* gen, ASTNode* call) {
    return call && call->type == AST_FUNCTION_CALL && call->value &&
           is_builder_func_reg(gen, call->value) && trailing_dsl_block(gen, call) &&
           !builder_call_configured(call);
}

static void stmt_control_run(CodeGenerator* gen, ASTNode* stmt, int temps_open) {
    if (stmt_runs_builder(gen)) return;
    ASTNode* v = NULL;
    ASTNode* ops[64];
    int n = 0;
    int whole = 0;          /* an expression statement that is nothing else */
    int own_temps = 0;      /* the statement sets up its struct temporaries */
    switch (stmt->type) {
        case AST_VARIABLE_DECLARATION:
        case AST_CONST_DECLARATION:
            if (stmt->child_count >= 1) v = stmt->children[0];
            break;
        case AST_ASSIGNMENT: {
            if (stmt->child_count < 2) break;
            ASTNode* lhs = stmt->children[0];
            if (lhs && lhs->type == AST_IDENTIFIER) {
                v = stmt->children[1];
                break;
            }
            /* `a[i] = v`: the target's indexes, then the value (#2516). */
            ASTNode* t = lhs;
            while (t && (t->type == AST_MEMBER_ACCESS || t->type == AST_ARRAY_ACCESS) &&
                   t->child_count > 0) {
                if (t->type == AST_ARRAY_ACCESS && t->child_count >= 2 && n < 63)
                    ops[n++] = t->children[1];
                t = t->children[0];
            }
            /* Collected from the outside in; evaluated from the inside out. */
            for (int i = 0; i < n / 2; i++) {
                ASTNode* x = ops[i];
                ops[i] = ops[n - 1 - i];
                ops[n - 1 - i] = x;
            }
            ops[n++] = stmt->children[1];
            break;
        }
        case AST_COMPOUND_ASSIGNMENT:
            if (stmt->child_count >= 2) v = stmt->children[1];
            break;
        case AST_TUPLE_DESTRUCTURE:
            if (stmt->child_count >= 2) v = stmt->children[stmt->child_count - 1];
            break;
        case AST_RETURN_STATEMENT:
        case AST_PRINT_STATEMENT:
            /* A value, or values evaluated in order (a multi-value return). */
            if (stmt->type == AST_RETURN_STATEMENT && stmt->child_count == 1) {
                v = stmt->children[0];
                break;
            }
            for (int i = 0; i < stmt->child_count && n < 64; i++) ops[n++] = stmt->children[i];
            break;
        case AST_IF_STATEMENT:
        case AST_MATCH_STATEMENT:
            if (stmt->child_count >= 1) v = stmt->children[0];
            own_temps = 1;
            break;
        case AST_EXPRESSION_STATEMENT:
            if (stmt->child_count < 1 || !stmt->children[0]) break;
            v = stmt->children[0];
            if (v->type == AST_BINARY_EXPRESSION && v->value && strcmp(v->value, "=") == 0 &&
                v->child_count == 2 && v->children[0] && v->children[0]->type == AST_IDENTIFIER) {
                v = v->children[1];
            } else {
                whole = 1;
            }
            own_temps = 1;
            break;
        default:
            break;
    }
    if (v) {
        ops[0] = v;
        n = 1;
    }
    int any = 0;
    for (int i = 0; i < n && !any; i++) any = ctl_contains(gen, ops[i]);
    if (!any) return;
    if (own_temps && !temps_open) {
        ASTNode** nodes = NULL;
        int count = 0, cap = 0;
        collect_value_struct_temps(gen, v, stmt->type == AST_MATCH_STATEMENT ? TEMP_KEPT : TEMP_FREE,
                                   &nodes, &count, &cap);
        if (stmt->type == AST_EXPRESSION_STATEMENT)
            collect_stmt_struct_temps(gen, stmt->children[0], &nodes, &count, &cap);
        free(nodes);
        if (count > 0) {
            g_ctl_deferred = stmt;
            return;
        }
    }
    if (v) ctl_linearize_value(gen, v, whole);
    else if (hoist_ready(gen)) ctl_linearize_list(gen, ops, n);
}

static void stmt_control_prelude(CodeGenerator* gen, ASTNode* stmt) {
    stmt_control_run(gen, stmt, 0);
}

/* The statement's emitter has opened its struct temporaries: run what
 * stmt_control_prelude left for then, if it left anything. */
static void stmt_control_prelude_deferred(CodeGenerator* gen, ASTNode* stmt) {
    if (g_ctl_deferred != stmt) return;
    g_ctl_deferred = NULL;
    stmt_control_run(gen, stmt, 1);
}

/* #2798: condition `cond`, which holds a construct that runs statements,
 * evaluated as statements here: the struct results of the calls in it held
 * in temporaries as emit_condition holds them, then its constructs and what
 * it evaluates before them (ctl_linearize_value), then its value into an
 * int, then the temporaries destroyed. Returns the int's name. */
static const char* emit_condition_ahead(CodeGenerator* gen, ASTNode* cond) {
    static int seq = 0;
    ASTNode** nodes = NULL;
    int count = 0, cap = 0;
    collect_value_struct_temps(gen, cond, TEMP_FREE, &nodes, &count, &cap);
    const char** names = count > 0
        ? (const char**)aether_xrealloc(NULL, sizeof(char*) * (size_t)count) : NULL;
    for (int i = 0; i < count; i++) {
        const char* ct = get_c_type(nodes[i]->node_type);
        names[i] = cg_internf("_ae_cta%d", seq++);
        hoist_declare(gen, ct, names[i]);
        print_indent(gen);
        fprintf(gen->output, "%s = (%s){0};\n", names[i], ct);
    }
    ASTNode** outer_nodes;
    const char** outer_names;
    int outer_count;
    stmt_struct_temps_get(&outer_nodes, &outer_names, &outer_count);
    if (count > 0) stmt_struct_temps_set(nodes, names, count);
    ctl_linearize_value(gen, cond, 0);
    const char* cv = cg_internf("_ae_cva%d", seq++);
    hoist_declare(gen, "int", cv);
    print_indent(gen);
    fprintf(gen->output, "%s = !!(", cv);
    gen->in_condition = 1;
    generate_expression(gen, cond);
    gen->in_condition = 0;
    fprintf(gen->output, ");\n");
    stmt_struct_temps_set(outer_nodes, outer_names, outer_count);
    for (int i = 0; i < count; i++) {
        print_indent(gen);
        fprintf(gen->output, "_aether_drop_%s(&%s);\n",
                struct_owning_strings(gen, nodes[i]->node_type), names[i]);
    }
    free(nodes);
    free(names);
    return cv;
}

/* #2798: a loop condition `cond` that holds a construct running statements:
 * emitted at the top of the loop's body, `<constructs>; if (!(cond))
 * break;`, so it is evaluated, constructs and all, on every pass, a
 * `continue` included. 0 when the loop keeps its condition in its head. */
static int loop_condition_in_body(CodeGenerator* gen, ASTNode* cond) {
    return cond && hoist_ready(gen) && ctl_contains(gen, cond);
}

static void emit_loop_condition_in_body(CodeGenerator* gen, ASTNode* cond) {
    int ctl_depth = control_prelude_depth();
    int order_depth = order_prelude_depth();
    ASTNode** nodes = NULL;
    int count = 0, cap = 0;
    collect_value_struct_temps(gen, cond, TEMP_FREE, &nodes, &count, &cap);
    free(nodes);
    if (count > 0) {
        /* Struct results the condition holds: destroyed once it is known. */
        const char* cv = emit_condition_ahead(gen, cond);
        print_indent(gen);
        fprintf(gen->output, "if (!%s) break;\n", cv);
    } else {
        ctl_linearize_value(gen, cond, 0);
        print_indent(gen);
        fprintf(gen->output, "if (!(");
        emit_condition(gen, cond);
        fprintf(gen->output, ")) break;\n");
    }
    control_prelude_end(ctl_depth);
    order_prelude_end(order_depth);
}

/* `<lhs> = <value>;` emitted as a statement of its own where no Aether
 * statement is being emitted (an actor state's initializer, in its spawn
 * function), as generate_statement emits one: its temporaries declared
 * ahead of it (#2765), and the constructs in it that run statements run
 * first (#2798). */
/* #2923: a plain `defer` runs on every exit of its scope (language
 * reference), a caught panic's too. While its scope is live it is journaled
 * with a thunk that runs it over the frame's locals (the journal drains
 * before the jump, so the frame is still there); a normal exit forgets the
 * entry and runs it in place (emit_deferred_one). */
#define DEFER_CAP_MAX 64
typedef struct { const char* name; const char* decl; } DeferCap;

static void defer_cap_add(DeferCap* caps, int* n, const char* name, const char* decl) {
    for (int i = 0; i < *n; i++)
        if (strcmp(caps[i].name, name) == 0) return;
    if (*n == DEFER_CAP_MAX) {
        fprintf(stderr, "Fatal: a defer reads more than %d locals\n", DEFER_CAP_MAX);
        exit(1);
    }
    caps[*n].name = name;
    caps[(*n)++].decl = decl;
}

/* The C locals the deferred statement `n` reads or writes, each with its
 * declaration: a variable as it is declared (a promoted one is its cell
 * pointer), its string tracker, the actor (`self`) a state field is reached
 * through, the env an env-backed capture is. */
static void defer_caps_walk(CodeGenerator* gen, ASTNode* n, DeferCap* caps, int* count) {
    if (!n) return;
    if (n->type == AST_IDENTIFIER && n->value) {
        const char* name = n->value;
        if (is_var_declared(gen, name) && !is_module_global_var(gen, name)) {
            Type* t = declared_var_type(gen, name);
            if (!t) t = n->node_type;
            if (!t) {
                fprintf(stderr, "Fatal: a defer reads '%s', whose type is not known\n", name);
                exit(1);
            }
            const char* decl = is_promoted_capture(gen, name)
                                   ? promoted_cell_pointer(get_c_type(t), name)
                                   : c_member_decl(t, name);
            defer_cap_add(caps, count, name, decl);
        } else if (gen->current_actor && is_actor_state_var(gen, name)) {
            defer_cap_add(caps, count, "self", cg_internf("%s* self", gen->current_actor));
        }
        for (int e = 0; e < gen->current_env_capture_count; e++) {
            if (!gen->current_env_captures[e] || strcmp(gen->current_env_captures[e], name) != 0)
                continue;
            for (int ci = 0; ci < gen->closure_count; ci++)
                if (gen->closures[ci].closure_node == gen->current_function)
                    defer_cap_add(caps, count, "_env",
                                  cg_internf("_closure_env_%d* _env", gen->closures[ci].id));
        }
    }
    for (int i = 0; i < n->child_count; i++) defer_caps_walk(gen, n->children[i], caps, count);
}

static ASTNode** g_udefer_node = NULL;
static int* g_udefer_id = NULL;
static int g_udefer_count = 0;

/* The journal entry of the plain `defer` whose statement is `body`, as its
 * registration named it, or NULL (emit_deferred_one). */
const char* user_defer_ctx(ASTNode* body) {
    for (int i = g_udefer_count - 1; i >= 0; i--)
        if (g_udefer_node[i] == body) return cg_internf("_ae_dfc%d", g_udefer_id[i]);
    return NULL;
}

/* The thunk `_ae_defer_<id>`: the statement over copies of the locals it
 * reads, copied back after, kept in gen->defer_thunks. It is generated
 * here, where the statement's names mean what they mean in place: written
 * at the end of the output, read back, and written over by what follows
 * (the thunks appended at the end make the file at least as long again). */
static void emit_defer_thunk(CodeGenerator* gen, ASTNode* body, int id, DeferCap* caps, int n) {
    FILE* tmp = gen->output;
    fflush(tmp);
    long start = ftell(tmp);
    int hoist_depth = gen->hoist_depth;
    long hoist_start = gen->hoist_start;
    size_t hoist_len = gen->hoist_decls_len;
    char* hoist_decls = NULL;
    if (hoist_len) {
        hoist_decls = (char*)aether_xrealloc(NULL, hoist_len);
        memcpy(hoist_decls, gen->hoist_decls, hoist_len);
    }
    int indent = gen->indent_level;
    const char* line_file = gen->last_line_file;
    int line_num = gen->last_line_num;
    gen->hoist_depth = 0;
    gen->hoist_start = -1;
    gen->hoist_decls_len = 0;
    gen->indent_level = 1;
    gen->last_line_file = NULL;
    gen->last_line_num = 0;
    fprintf(tmp, "static void _ae_defer_%d(const void* _p) {\n", id);
    fprintf(tmp, "    void* const* _c = (void* const*)_p; (void)_c;\n");
    for (int i = 0; i < n; i++)
        fprintf(tmp, "    %s; memcpy(&%s, _c[%d], sizeof(%s));\n", caps[i].decl, caps[i].name, i,
                caps[i].name);
    enter_scope(gen);
    generate_statement(gen, body);
    exit_scope(gen);
    for (int i = 0; i < n; i++)
        fprintf(tmp, "    memcpy(_c[%d], &%s, sizeof(%s));\n", i, caps[i].name, caps[i].name);
    fprintf(tmp, "}\n");
    fflush(tmp);
    size_t len = (size_t)(ftell(tmp) - start);
    if (gen->defer_thunks_len + len + 1 > gen->defer_thunks_cap) {
        gen->defer_thunks_cap = (gen->defer_thunks_len + len + 1) * 2;
        gen->defer_thunks = aether_xrealloc(gen->defer_thunks, gen->defer_thunks_cap);
    }
    if (fseek(tmp, start, SEEK_SET) != 0 ||
        fread(gen->defer_thunks + gen->defer_thunks_len, 1, len, tmp) != len ||
        fseek(tmp, start, SEEK_SET) != 0) {
        fprintf(stderr, "Fatal: a defer's thunk could not be read back\n");
        exit(1);
    }
    gen->defer_thunks_len += len;
    gen->defer_thunks[gen->defer_thunks_len] = '\0';
    gen->hoist_depth = hoist_depth;
    gen->hoist_start = hoist_start;
    if (hoist_len) memcpy(gen->hoist_decls, hoist_decls, hoist_len);
    gen->hoist_decls_len = hoist_len;
    free(hoist_decls);
    gen->indent_level = indent;
    gen->last_line_file = line_file;
    gen->last_line_num = line_num;
}

static void emit_defer_registration(CodeGenerator* gen, ASTNode* body) {
    DeferCap caps[DEFER_CAP_MAX];
    int n = 0;
    defer_caps_walk(gen, body, caps, &n);
    int id = gen->defer_thunk_count++;
    emit_defer_thunk(gen, body, id, caps, n);
    print_indent(gen);
    fprintf(gen->output, "void* _ae_dfc%d[%d] = { ", id, n ? n : 1);
    for (int i = 0; i < n; i++) fprintf(gen->output, "%s(void*)&%s", i ? ", " : "", caps[i].name);
    fprintf(gen->output, "%s }; aether_unwind_track((const void*)_ae_dfc%d, _ae_defer_%d);\n",
            n ? "" : "0", id, id);
    g_udefer_node = aether_xrealloc(g_udefer_node, sizeof(ASTNode*) * (size_t)(g_udefer_count + 1));
    g_udefer_id = aether_xrealloc(g_udefer_id, sizeof(int) * (size_t)(g_udefer_count + 1));
    g_udefer_node[g_udefer_count] = body;
    g_udefer_id[g_udefer_count++] = id;
}

/* The thunks, after everything else, and their prototypes where the
 * function bodies start (gen->defer_proto_pos): the file from there is
 * written again after them. */
void emit_defer_thunks(CodeGenerator* gen) {
    if (gen->defer_thunk_count == 0) return;
    fputs("\n// Deferred statements a panic runs (#2923)\n", gen->output);
    fputs(gen->defer_thunks, gen->output);
    fflush(gen->output);
    long end = ftell(gen->output);
    long pos = gen->defer_proto_pos;
    size_t n = (size_t)(end - pos);
    char* tail = (char*)aether_xrealloc(NULL, n);
    if (fseek(gen->output, pos, SEEK_SET) != 0 || fread(tail, 1, n, gen->output) != n ||
        fseek(gen->output, pos, SEEK_SET) != 0) {
        fprintf(stderr, "Fatal: the defer thunks' prototypes could not be written\n");
        exit(1);
    }
    for (int i = 0; i < gen->defer_thunk_count; i++)
        fprintf(gen->output, "static void _ae_defer_%d(const void* _p);\n", i);
    fwrite(tail, 1, n, gen->output);
    fflush(gen->output);
    free(tail);
    free(g_udefer_node);
    free(g_udefer_id);
    g_udefer_node = NULL;
    g_udefer_id = NULL;
    g_udefer_count = 0;
}

void generate_value_statement(CodeGenerator* gen, const char* lhs, ASTNode* value, Type* t) {
    hoist_begin(gen);
    int order_depth = order_prelude_depth();
    int ctl_depth = control_prelude_depth();
    if (hoist_ready(gen) && ctl_contains(gen, value)) ctl_linearize_value(gen, value, 0);
    print_indent(gen);
    if (type_is_sized_array(t)) {
        emit_array_fill(gen, lhs, value, t);
    } else {
        fprintf(gen->output, "%s = ", lhs);
        if (t && t->kind != TYPE_STRING && own_type_owns(gen, t)) emit_value_take(gen, value, t);
        else generate_expression(gen, value);
    }
    fprintf(gen->output, ";\n");
    control_prelude_end(ctl_depth);
    order_prelude_end(order_depth);
    hoist_end(gen);
}

void generate_statement(CodeGenerator* gen, ASTNode* stmt) {
    if (!stmt) return;
    hoist_begin(gen);   /* #2765 */
    ASTNode* saved_trailing = gen->trailing_stmt_call;
    gen->trailing_stmt_call = stmt_trailing_call(gen, stmt);
    ValueTemps vt;
    int has_vt = value_temps_open(gen, stmt, &vt);   /* #2582 */
    int order_depth = order_prelude_depth();   /* #2478 */
    int ctl_depth = control_prelude_depth();   /* #2765 */
    if (hoist_ready(gen)) {
        codegen_maybe_emit_line(gen, stmt);
        stmt_control_prelude(gen, stmt);
    }
    /* A builder call with a trailing block that this statement stores or
     * throws away (gen->trailing_stmt_call): its block and arguments run
     * here, then the statement is emitted as for any value, the call made
     * with the filled config. Whatever the target: a local, a struct field,
     * a module global, an actor's state (#2873), a new declaration (#2874).
     * Each statement form used to run the block itself after a plain
     * assignment, or not at all: the call ran twice, once with a NULL
     * config (datastar#4), a field, a global or a state took the value with
     * the block dropped, and a declaration of a struct did not compile. */
    ASTNode* bcall = gen->trailing_stmt_call;
    int builder_stmt = builder_call_to_run(gen, bcall);
    BuilderCfg bsaved = { NULL, NULL, NULL };
    if (builder_stmt) {
        const char* cfg = emit_builder_prelude(gen, bcall);
        bsaved = builder_cfg_begin(gen, bcall, cfg);
    }
    generate_statement_body(gen, stmt);
    if (builder_stmt) builder_cfg_end(gen, bsaved);
    if (g_ctl_deferred == stmt) g_ctl_deferred = NULL;
    control_prelude_end(ctl_depth);
    order_prelude_end(order_depth);
    if (has_vt) value_temps_close(gen, stmt, &vt);
    gen->trailing_stmt_call = saved_trailing;
    emit_observable_store_notify(gen, stmt);
    unwind_track_release(gen);
    hoist_end(gen);
}

static void generate_statement_body(CodeGenerator* gen, ASTNode* stmt) {
    if (!stmt) return;

    codegen_note_diag_pos(stmt);

    // Emit `#line N "src.ae"` so gcc errors, gdb breakpoints, and
    // gcov reports reference the .ae source the user wrote, not the
    // mid-file position of the merged .c output. Dedup'd inside
    // codegen_maybe_emit_line — back-to-back statements on the same
    // source line emit one directive, not two.
    codegen_maybe_emit_line(gen, stmt);

    switch (stmt->type) {
        case AST_CONST_DECLARATION: {
            // Local constant: const <type> <name> = <value>;
            // or const arr[] = [v1, v2, ...];
            if (stmt->value && stmt->child_count > 0) {
                mark_var_declared(gen, stmt->value);
                if (stmt->annotation && strcmp(stmt->annotation, "array_const") == 0 &&
                    is_empty_slice_value(stmt->children[0])) {
                    /* #2767: no elements: the empty slice, as at file scope. */
                    fprintf(gen->output, "static const AetherSlice %s = {0};\n", stmt->value);
                } else if (stmt->annotation && strcmp(stmt->annotation, "array_const") == 0) {
                    // static const T NAME[] = {v1, v2, ...};
                    Type* elem_type = (stmt->node_type && stmt->node_type->element_type)
                                      ? stmt->node_type->element_type : NULL;
                    const char* ctype = elem_type ? const_array_elem_c_type(elem_type) : "const char*";
                    fprintf(gen->output, "static const %s %s[] = ", ctype, stmt->value);
                    generate_expression(gen, stmt->children[0]);
                    fprintf(gen->output, ";\n");
                } else {
                    Type* var_type = stmt->node_type;
                    if ((!var_type || var_type->kind == TYPE_VOID || var_type->kind == TYPE_UNKNOWN)
                        && stmt->children[0] && stmt->children[0]->node_type) {
                        var_type = stmt->children[0]->node_type;
                    }
                    // STRING already emits "const char*", skip extra const qualifier
                    if (var_type && var_type->kind == TYPE_STRING) {
                        generate_type(gen, var_type);
                    } else {
                        fprintf(gen->output, "const ");
                        generate_type(gen, var_type);
                    }
                    fprintf(gen->output, " %s = ", stmt->value);
                    generate_expression(gen, stmt->children[0]);
                    fprintf(gen->output, ";\n");
                }
            }
            break;
        }
        case AST_TUPLE_DESTRUCTURE: {
            // a, b = func() — last child is RHS, others are variable declarations
            if (stmt->child_count < 2) break;
            int var_count = stmt->child_count - 1;
            ASTNode* rhs = stmt->children[var_count];

            // Infer tuple type from RHS
            Type* rhs_type = rhs->node_type;
            if (rhs_type && rhs_type->kind == TYPE_TUPLE) {
                ensure_tuple_typedef(gen, rhs_type);
            }

            // Generate: _tuple_X_Y _tmp = func();
            const char* tuple_type_name = rhs_type ? get_c_type(rhs_type) : "_tuple_unknown";
            static int tuple_tmp_counter = 0;
            int tmp_id = tuple_tmp_counter++;
            print_indent(gen);
            fprintf(gen->output, "%s _tup%d = ", tuple_type_name, tmp_id);
            generate_expression(gen, rhs);
            fprintf(gen->output, ";\n");

            /* Per-position heap-ness lookup for the destructure
             * (issue #420). For each tuple position `j`, decide
             * whether the source value at that position is a fresh
             * heap allocation that the destructured LHS now owns.
             * Computed once up-front so the per-LHS loop can route
             * correctly:
             *
             *   - User-defined tuple-returning fn: walk return-sites
             *     via `function_def_returns_heap_at`, AND-fold per
             *     position. Memoised on the fn's annotation slot.
             *   - Extern or any other RHS: read the per-position
             *     `tuple_heap_flags[j]` populated by the parser
             *     when an `@heap` annotation is in scope. NULL
             *     flags ⇒ all 0 (borrow) — preserves the silent
             *     pre-#420 behaviour for unannotated externs.
             *
             * Heap classification is only meaningful for TYPE_STRING
             * positions; non-string positions keep their plain
             * assignment shape regardless of the flag. */
            ASTNode* callee_def = NULL;
            if (rhs && rhs->type == AST_FUNCTION_CALL && rhs->value) {
                /* Source-level callees land in the AST in dotted form
                 * (`"json.get_string"`) but the merged user-fn lives in
                 * the program AST under the underscored namespace-
                 * prefixed name (`"json_get_string"`). Without
                 * normalisation the lookup misses every cross-module
                 * callee, and a position the callee hands over is not
                 * seen as one. Same dot-normalisation pattern
                 * `is_heap_string_expr` already uses on its hardcoded
                 * fast-path lookups. */
                const char* fn = codegen_normalise_callee(rhs->value);
                callee_def = find_function_definition_by_name(gen->program, fn);
            }

            // Generate: type a = _tmp._0; type b = _tmp._1; ...
            for (int j = 0; j < var_count; j++) {
                ASTNode* var = stmt->children[j];

                TuplePosOwn po = tuple_pos_own(gen, rhs, callee_def, j);
                int pos_is_string = po.is_string;
                int pos_is_heap = po.is_heap;
                int pos_opt_owned = po.opt_owned;
                int pos_closure = po.is_closure;
                int pos_closure_owned = po.closure_owned;
                /* A closure position a C function lends is taken. */
                int pos_alias = pos_closure && !pos_closure_owned;
                const char* slot_value = pos_alias
                    ? cg_internf("_aether_closure_retain(_tup%d._%d)", tmp_id, j)
                    : cg_internf("_tup%d._%d", tmp_id, j);
                Type* pos_t = (rhs_type && rhs_type->kind == TYPE_TUPLE &&
                               j < rhs_type->tuple_count) ? rhs_type->tuple_types[j] : NULL;

                /* #2894: an array position. A call's tuple hands its
                 * elements over, so the local takes them as they are; a
                 * tuple another place holds keeps its own, and the local
                 * retains each. Thrown away, a call's are dropped. */
                if (pos_t && is_sized_array_param(pos_t) && var->value) {
                    int moved = rhs && rhs->type == AST_FUNCTION_CALL;
                    const char* src = cg_internf("_tup%d._%d", tmp_id, j);
                    const char* et = get_c_type(pos_t->element_type);
                    if (strcmp(var->value, "_") == 0) {
                        if (moved && own_type_owns(gen, pos_t)) {
                            print_indent(gen);
                            fprintf(gen->output, "%s((%s*)%s);\n", own_fn(gen, pos_t, "drop"), et, src);
                        }
                        continue;
                    }
                    print_indent(gen);
                    if (!is_var_declared(gen, var->value)) {
                        fprintf(gen->output, "%s %s[%d] = {0}; ", et, var->value, pos_t->array_size);
                        emit_array_fill_c(gen, var->value, src, pos_t, !moved);
                        fprintf(gen->output, ";\n");
                        mark_var_declared_typed(gen, var->value, pos_t);
                        if (own_type_owns(gen, pos_t))
                            push_own_drop(gen, var->value, pos_t, var->line, var->column);
                    } else if (own_type_owns(gen, pos_t)) {
                        fprintf(gen->output, "{ %s _ae_new[%d]; ", et, pos_t->array_size);
                        emit_array_fill_c(gen, "_ae_new", src, pos_t, !moved);
                        fprintf(gen->output, "; %s((%s*)%s); memcpy((void*)%s, (const void*)_ae_new, sizeof(_ae_new)); }\n",
                                own_fn(gen, pos_t, "drop"), et, var->value, var->value);
                    } else {
                        emit_array_fill_c(gen, var->value, src, pos_t, 0);
                        fprintf(gen->output, ";\n");
                    }
                    continue;
                }

                /* #2921: a struct, tuple or optional position that owns,
                 * as an array position is: a call's tuple hands the part
                 * over, a tuple another place holds keeps it and the local
                 * takes a reference of its own. */
                if (pos_t && var->value && own_type_owns(gen, pos_t) &&
                    (pos_t->kind == TYPE_STRUCT || pos_t->kind == TYPE_TUPLE ||
                     pos_t->kind == TYPE_OPTIONAL || pos_t->kind == TYPE_FUNCTION) &&
                    !is_promoted_capture(gen, var->value) &&
                    !is_module_global_var(gen, var->value) &&
                    !is_actor_state_var(gen, var->value)) {
                    int moved = rhs && rhs->type == AST_FUNCTION_CALL;
                    const char* src = cg_internf("_tup%d._%d", tmp_id, j);
                    const char* ct = get_c_type(pos_t);
                    if (strcmp(var->value, "_") == 0) {
                        if (moved) own_emit_part(gen, pos_t, src, OWN_DROP);
                        continue;
                    }
                    if (!is_var_declared(gen, var->value)) {
                        print_indent(gen);
                        fprintf(gen->output, "%s %s = %s;\n", ct, var->value, src);
                        if (!moved) own_emit_part(gen, pos_t, var->value, OWN_RETAIN);
                        mark_var_declared_typed(gen, var->value, pos_t);
                        push_own_drop(gen, var->value, pos_t, var->line, var->column);
                    } else {
                        print_indent(gen);
                        fprintf(gen->output, "{ %s _ae_old = %s; %s = %s;\n", ct, var->value,
                                var->value, src);
                        if (!moved) own_emit_part(gen, pos_t, var->value, OWN_RETAIN);
                        own_emit_part(gen, pos_t, "_ae_old", OWN_DROP);
                        print_indent(gen);
                        fprintf(gen->output, "}\n");
                    }
                    continue;
                }

                /* `_` discard slot. If the position is a heap value,
                 * the destructure target has no name to free against
                 * — emit an immediate `free` so the heap allocation
                 * doesn't leak across the destructure. Non-heap
                 * positions stay no-ops. */
                if (var->value && strcmp(var->value, "_") == 0) {
                    if (pos_is_heap) {
                        print_indent(gen);
                        fprintf(gen->output,
                                "if (_tup%d._%d) aether_heap_str_free(_tup%d._%d);\n",
                                tmp_id, j, tmp_id, j);
                    } else if (pos_opt_owned) {
                        print_indent(gen);
                        fprintf(gen->output,
                                "if (_tup%d._%d.has) aether_heap_str_free((void*)_tup%d._%d.val);\n",
                                tmp_id, j, tmp_id, j);
                    } else if (pos_closure_owned) {
                        print_indent(gen);
                        fprintf(gen->output, "_aether_closure_env_release(_tup%d._%d.env);\n",
                                tmp_id, j);
                    }
                    continue;
                }

                /* A fn-pointer slot binds a fn-pointer local, so `f(a, b)`
                 * is a call through the slot's signature, as for a local
                 * declared with one; emitted bare, it called a `void*`
                 * (found with #2636). A closure in the function already
                 * found it through register_fnptr_decls_in. */
                if (rhs_type && rhs_type->kind == TYPE_TUPLE && j < rhs_type->tuple_count &&
                    is_fnptr_type(rhs_type->tuple_types[j]))
                    register_fnptr_local(gen, var->value, rhs_type->tuple_types[j]);

                // Prefer tuple element type over var's node_type (may be UNKNOWN)
                const char* var_type;
                if (rhs_type && rhs_type->kind == TYPE_TUPLE && j < rhs_type->tuple_count &&
                    rhs_type->tuple_types[j]->kind != TYPE_UNKNOWN) {
                    var_type = get_c_type(rhs_type->tuple_types[j]);
                } else {
                    var_type = get_c_type(var->node_type);
                }
                print_indent(gen);
                // Promoted-capture aware destructure: same routing as the
                // AST_VARIABLE_DECLARATION single-name path. At first use
                // declare the heap cell + defer free; on reassignment write
                // through the cell. Without this, a closure-body destructure
                // of a captured name miscompiled as `name = _tup._N` against
                // a `T**` slot — produced a -Wincompatible-pointer-types
                // warning and a runtime segfault on the next deref of the
                // captured slot (closure-shadow-tuple-destructure, svn-aether
                // porter Round 238/239). The matching is_assigned_to fix in
                // codegen_expr.c teaches the promotion analysis to see
                // tuple-destructure targets as writes.
                if (is_promoted_capture(gen, var->value)) {
                    Type* cell_t = promoted_cell_type(gen, var->value);
                    /* #2794, #2795: an optional cell takes the slot as the
                     * optional it holds (a bare value wrapped), a `string?`
                     * cell a string of its own. The bare value was stored
                     * into the `ae_opt_<T>`, which did not compile. */
                    Type* opt_t = (var->node_type && var->node_type->kind == TYPE_OPTIONAL)
                                      ? var->node_type
                                      : (cell_t && cell_t->kind == TYPE_OPTIONAL) ? cell_t : NULL;
                    if (opt_t) {
                        const char* oc = cg_intern(get_c_type(opt_t));
                        int own = pos_is_string ? pos_is_heap : pos_opt_owned;
                        if (!is_var_declared(gen, var->value)) {
                            emit_promoted_cell_declaration(gen, var->value, oc, opt_t, NULL, NULL,
                                                           stmt->line, stmt->column);
                            print_indent(gen);
                        }
                        int take;
                        const char* text = opt_slot_text(oc, pos_t, tmp_id, j, own, 1, &take);
                        emit_opt_cell_store(gen, var->value, opt_t, NULL, text, take);
                        continue;
                    }
                    /* #2797: a string into a `ptr` cell: the cell takes a
                     * string of its own (adopted when the call handed it
                     * over) and holds its characters, as emit_cell_store
                     * stores one. */
                    if (pos_is_string && cell_t && cell_t->kind == TYPE_PTR &&
                        is_var_declared(gen, var->value)) {
                        fprintf(gen->output, "_aether_ptr_cell_set_str(%s, _aether_str_cell_own(_tup%d._%d, %d));\n",
                                var->value, tmp_id, j, pos_is_heap);
                        continue;
                    }
                    /* #2514: a string cell owns what it holds, so a slot the
                     * call did not hand over is copied in. */
                    char slot[96];
                    if (pos_is_string)
                        snprintf(slot, sizeof(slot), "aether_uniform_heap_str(_tup%d._%d, %d)",
                                 tmp_id, j, pos_is_heap);
                    else
                        snprintf(slot, sizeof(slot), "_tup%d._%d", tmp_id, j);
                    if (!is_var_declared(gen, var->value)) {
                        emit_promoted_cell_declaration(gen, var->value, var_type, NULL, NULL, slot,
                                                       stmt->line, stmt->column);
                    } else if (pos_is_string) {
                        fprintf(gen->output, "_aether_str_cell_set_shared(%s, %s);\n", var->value, slot);
                    } else if (var_type && strcmp(var_type, "_AeClosure") == 0) {
                        emit_closure_cell_store_text(gen, var->value, slot);   /* #2755 */
                    } else {
                        fprintf(gen->output, "*%s = %s;\n", var->value, slot);
                    }
                    if (pos_closure_owned) {
                        print_indent(gen);
                        fprintf(gen->output, "_aether_closure_env_release(_tup%d._%d.env);\n",
                                tmp_id, j);
                    }
                    continue;
                }
                /* #2806: an actor's state field the destructure names, in
                 * a handler, is written as `label = v` writes it, where a
                 * handler local of the same name was declared and the
                 * state kept its value. */
                if (is_actor_state_var(gen, var->value)) {
                    emit_destructure_state_store(gen, var->value, pos_t, tmp_id, j,
                                                 pos_is_string ? pos_is_heap : pos_opt_owned,
                                                 pos_closure_owned);
                    continue;
                }
                /* #2796: a module-level `var` the destructure names is
                 * written, as `gx = value` writes it (#701), where a local
                 * of the same name was declared and the global kept its
                 * value. A local or parameter already declared shadows it. */
                if (!is_var_declared(gen, var->value) && is_module_global_var(gen, var->value)) {
                    emit_destructure_global_store(gen, var->value, pos_t, tmp_id, j,
                                                  pos_is_string ? pos_is_heap : pos_opt_owned,
                                                  pos_closure_owned);
                    continue;
                }
                if (is_var_declared(gen, var->value)) {
                    int destruct_is_env_cap = 0;
                    for (int ec = 0; ec < gen->current_env_capture_count; ec++) {
                        if (gen->current_env_captures[ec] &&
                            strcmp(gen->current_env_captures[ec], var->value) == 0) {
                            destruct_is_env_cap = 1;
                            break;
                        }
                    }
                    if (destruct_is_env_cap) {
                        fprintf(gen->output, "_env->%s = _tup%d._%d;\n", var->value, tmp_id, j);
                        continue;
                    }
                    /* #2751: a `string?` local is replaced as a binding
                     * replaces it (the AST_VARIABLE_DECLARATION path): the
                     * string it owned is freed, and its flag says whether it
                     * owns the new one. #2794: a `string` position is
                     * wrapped, as `s = "x"` wraps, and owned when the call
                     * hands its string over. */
                    Type* vot = var->value ? declared_var_type(gen, var->value) : NULL;
                    if (!vot) vot = var->node_type;
                    if (var->value && is_opt_string_type(vot) && pos_t &&
                        (is_opt_string_type(pos_t) || pos_is_string) &&
                        !is_promoted_capture(gen, var->value) &&
                        !is_module_global_var(gen, var->value) &&
                        !is_actor_state_var(gen, var->value)) {
                        /* R3: the local takes the slot (a call's string is
                         * handed over, any other retained) and drops what it
                         * held. */
                        int moved = rhs && rhs->type == AST_FUNCTION_CALL &&
                                    (pos_is_string ? pos_is_heap : pos_opt_owned);
                        const char* slot = opt_slot_text("ae_opt_string", pos_t, tmp_id, j, 0, 0, NULL);
                        if (!is_var_declared(gen, var->value)) {
                            fprintf(gen->output, "ae_opt_string %s = %s;%s\n", var->value, slot,
                                    moved ? "" : cg_internf(" %s(&%s);", own_fn(gen, vot, "retain"),
                                                            var->value));
                            mark_var_declared_typed(gen, var->value, vot);
                            push_own_drop(gen, var->value, vot, var->line, var->column);
                        } else {
                            fprintf(gen->output, "{ ae_opt_string _ae_old = %s; %s = %s;%s %s(&_ae_old); }\n",
                                    var->value, var->value, slot,
                                    moved ? "" : cg_internf(" %s(&%s);", own_fn(gen, vot, "retain"),
                                                            var->value),
                                    own_fn(gen, vot, "drop"));
                        }
                        continue;
                    }
                    /* #2794: any other optional local takes a bare value
                     * wrapped, as `a = 7` does, where it stored the bare
                     * value into the `ae_opt_<T>` and did not compile. */
                    if (var->node_type && var->node_type->kind == TYPE_OPTIONAL && pos_t &&
                        pos_t->kind != TYPE_OPTIONAL) {
                        fprintf(gen->output, "%s = %s;\n", var->value,
                                opt_slot_text(cg_intern(get_c_type(var->node_type)), pos_t,
                                              tmp_id, j, 0, 0, NULL));
                        continue;
                    }
                    /* String-typed LHS hoisted as a string local: it
                     * owns the position and releases what it held
                     * (issue #420). This holds for both the first and a
                     * later destructure of a hoisted var: the hoist
                     * initialised `<lhs> = NULL`, so on first use the
                     * release is a no-op. */
                    int lhs_is_tracked = (var->value &&
                                          is_heap_string_var(gen, var->value));
                    /* Wrapper-emission gate. Fire whenever the LHS is a
                     * string local, regardless of whether the destructure
                     * POSITION's type is string: if the previous value was
                     * a string (e.g. `pkg_include = "${name}*"`) and the
                     * destructure now reassigns it from a non-string
                     * position (e.g. `pkg_include, _ = map.get(...)`
                     * where position 0 is TYPE_PTR), the old value is
                     * still released and the new one taken. */
                    if (lhs_is_tracked) {
                        /* R2: the local owns the position, taken when the
                         * call did not hand a string over (a pointer
                         * position's C string copied), and releases what
                         * it held. */
                        const char* taken = pos_is_string && pos_is_heap
                            ? cg_internf("_tup%d._%d", tmp_id, j)
                            : cg_internf("aether_uniform_heap_str((const char*)_tup%d._%d, 0)",
                                         tmp_id, j);
                        fprintf(gen->output,
                            "{ const char* _tmp_old = %s; %s = %s; aether_heap_str_free(_tmp_old); }\n",
                            var->value, var->value, taken);
                        continue;
                    }
                    fprintf(gen->output, "%s = %s;\n", var->value, slot_value);
                } else {
                    mark_var_declared(gen, var->value);
                    fprintf(gen->output, "%s %s = %s;\n", var_type, var->value, slot_value);
                    /* A hoisted string local (rare for first-decl
                     * since the hoist also marks the var declared): the
                     * local owns the position, taken when the call did not
                     * hand it over (R2). */
                    if (pos_is_string && !pos_is_heap && var->value &&
                        is_heap_string_var(gen, var->value))
                        print_line(gen, "%s = aether_uniform_heap_str(%s, 0);", var->value, var->value);
                    /* #752: a struct-typed tuple position transfers
                     * ownership of its heap-string fields to this LHS —
                     * free them at scope exit. */
                    if (rhs_type && rhs_type->kind == TYPE_TUPLE &&
                        j < rhs_type->tuple_count) {
                        push_struct_destroy_defer(gen, var->value,
                                                  rhs_type->tuple_types[j],
                                                  stmt->line, stmt->column);
                    }
                }
            }
            break;
        }

        case AST_VARIABLE_DECLARATION: {
            if (emit_composite_local_binding(gen, stmt)) break;   /* #2921 */
            // #340: optional-typed local (`x: T? = ...`). Emit the
            // declaration / re-bind with implicit `T -> T?` coercion of the
            // initializer (a bare value wraps, `none` zero-inits, an optional
            // passes through). A declaration with no initializer defaults to
            // `none`. Handled here because the generic declarator paths below
            // don't understand the `ae_opt_<T>` tagged-struct wrap.
            if (stmt->value && strcmp(stmt->value, "_") != 0 &&
                stmt->node_type && stmt->node_type->kind == TYPE_OPTIONAL &&
                /* A `match` bound to an optional is bound arm by arm, as
                 * any bound match is (below), each arm's value wrapped. */
                !(stmt->child_count > 0 && stmt->children[0] &&
                  stmt->children[0]->type == AST_MATCH_STATEMENT)) {
                /* #2795: an optional a closure writes is a cell, as any
                 * variable a closure writes is (the closure's env takes
                 * the cell). It was declared and stored as a plain value,
                 * so the closure's capture of it did not compile. The
                 * first binding makes the cell, zeroed (`none`), and
                 * every binding stores into it. */
                if (is_promoted_capture(gen, stmt->value)) {
                    if (!is_var_declared(gen, stmt->value)) {
                        print_indent(gen);
                        emit_promoted_cell_declaration(gen, stmt->value,
                                                       get_c_type(stmt->node_type),
                                                       stmt->node_type, NULL, NULL,
                                                       stmt->line, stmt->column);
                    }
                    print_indent(gen);
                    emit_opt_cell_store(gen, stmt->value, stmt->node_type,
                                        stmt->child_count > 0 ? stmt->children[0] : NULL, NULL,
                                        -1);
                    break;
                }
                /* #2800: a module-level optional `var` the binding names,
                 * with no local or parameter of that name declared, is
                 * written, as `g = v` writes any global (#701). R3: it takes
                 * the value, publishes what it took since any thread may
                 * read it (#2919), and drops the one it held. */
                if (!is_var_declared(gen, stmt->value) &&
                    is_module_global_var(gen, stmt->value)) {
                    print_indent(gen);
                    if (own_type_owns(gen, stmt->node_type) && stmt->child_count > 0)
                        emit_owning_store(gen, stmt->value, stmt->node_type, stmt->children[0], 1);
                    else
                        emit_opt_owner_store(gen, stmt->value, stmt->node_type,
                                             stmt->child_count > 0 ? stmt->children[0] : NULL,
                                             NULL, -1, 0);
                    break;
                }
                print_indent(gen);
                if (!is_var_declared(gen, stmt->value)) {
                    /* #2902: volatile where a `try` body writes it, as
                     * every other local is (#2618, #2690); a `string?`
                     * is declared by hoist_opt_string_locals. */
                    fprintf(gen->output, "%s %s",
                            volatile_decl_type(try_volatile_qual_for(gen, stmt->value),
                                               get_c_type(stmt->node_type)),
                            stmt->value);
                    mark_var_declared(gen, stmt->value);
                } else {
                    fprintf(gen->output, "%s", stmt->value);
                }
                fprintf(gen->output, " = ");
                if (stmt->child_count > 0 && stmt->children[0]) {
                    emit_optional_coerced(gen, stmt->children[0], stmt->node_type);
                } else {
                    fprintf(gen->output, "(%s){0}", get_c_type(stmt->node_type));
                }
                fprintf(gen->output, ";\n");
                break;
            }

            // #914: sum-typed local (`s: Shape = Circle {...}`). Emit the
            // declaration / re-bind, wrapping a variant struct initializer into
            // the tagged union (a value already of the sum type passes through).
            if (stmt->value && strcmp(stmt->value, "_") != 0 &&
                stmt->node_type && stmt->node_type->kind == TYPE_SUM) {
                print_indent(gen);
                if (!is_var_declared(gen, stmt->value)) {
                    /* #2902: volatile where a `try` body writes it. */
                    fprintf(gen->output, "%s %s",
                            volatile_decl_type(try_volatile_qual_for(gen, stmt->value),
                                               get_c_type(stmt->node_type)),
                            stmt->value);
                    mark_var_declared(gen, stmt->value);
                } else {
                    fprintf(gen->output, "%s", stmt->value);
                }
                fprintf(gen->output, " = ");
                if (stmt->child_count > 0 && stmt->children[0]) {
                    emit_sum_coerced(gen, stmt->children[0], stmt->node_type);
                } else {
                    fprintf(gen->output, "(%s){0}", get_c_type(stmt->node_type));
                }
                fprintf(gen->output, ";\n");
                break;
            }
            /* Bare `_` is a per-use discard, not a real variable.
             * `_ = <expr>` evaluates the RHS for its side effects and
             * throws the value away — no declaration, no type, no
             * shared lvalue. Without this, `_` was one C variable
             * whose type was fixed by its first use, so a function
             * discarding (say) a string at one site and an int at
             * another emitted conflicting `_` declarations and failed
             * the C compile (aeb-ae-help-and-toolchain-feedback.md
             * #4). A freshly-allocated heap string handed to `_` is
             * freed so the discard doesn't leak; everything else is a
             * plain `(void)` cast. A match RHS falls through to the
             * normal match-as-expression path. */
            if (stmt->value && strcmp(stmt->value, "_") == 0 &&
                stmt->child_count > 0 && stmt->children[0] &&
                stmt->children[0]->type != AST_MATCH_STATEMENT) {
                ASTNode* drhs = stmt->children[0];
                int fresh_heap =
                    (drhs->type == AST_FUNCTION_CALL ||
                     drhs->type == AST_STRING_INTERP ||
                     is_owned_string_unwrap(gen, drhs)) &&
                    is_heap_string_expr(gen, drhs);
                print_indent(gen);
                if (is_owned_opt_string_call(gen, drhs)) {
                    /* #2750: a `string?` an Aether function handed over. */
                    fprintf(gen->output, "{ %s _od = ", get_c_type(drhs->node_type));
                    generate_expression(gen, drhs);
                    fprintf(gen->output, "; if (_od.has) aether_heap_str_free((void*)_od.val); }\n");
                } else if (fresh_heap) {
                    fprintf(gen->output, "aether_heap_str_free((void*)(");
                    generate_expression(gen, drhs);
                    fprintf(gen->output, "));\n");
                } else if (drhs->type == AST_CLOSURE || call_returns_owned_closure(gen, drhs)) {
                    /* #2507: a closure made or handed over, thrown away. */
                    fprintf(gen->output, "_aether_closure_env_release((");
                    generate_expression(gen, drhs);
                    fprintf(gen->output, ").env);\n");
                } else if (type_is_sized_array(drhs->node_type)) {
                    /* An array is no C value: it fills a temporary, which
                     * drops what it took. */
                    Type* dt = drhs->node_type;
                    fprintf(gen->output, "{ %s; ", c_member_decl(dt, "_od"));
                    emit_array_fill(gen, "_od", drhs, dt);
                    if (own_type_owns(gen, dt))
                        fprintf(gen->output, "; %s(_od); }\n", own_fn(gen, dt, "drop"));
                    else
                        fprintf(gen->output, "; (void)_od; }\n");
                } else if ((drhs->type == AST_STRUCT_LITERAL || drhs->type == AST_FUNCTION_CALL) &&
                           drhs->node_type && drhs->node_type->kind != TYPE_STRING &&
                           drhs->node_type->kind != TYPE_FUNCTION &&
                           drhs->node_type->kind != TYPE_ARRAY &&
                           own_type_owns(gen, drhs->node_type)) {
                    /* #2921: a struct or composite handed over and thrown
                     * away is dropped. */
                    Type* dt = drhs->node_type;
                    const char* ds = struct_owning_strings(gen, dt);
                    fprintf(gen->output, "{ %s _od = ", get_c_type(dt));
                    if (ds) generate_expression(gen, drhs);
                    else emit_composite_take(gen, drhs, dt);
                    fprintf(gen->output, "; %s(&_od); }\n",
                            ds ? cg_internf("_aether_drop_%s", ds) : own_fn(gen, dt, "drop"));
                } else {
                    fprintf(gen->output, "(void)(");
                    generate_expression(gen, drhs);
                    fprintf(gen->output, ");\n");
                }
                break;
            }
            // Register fn-pointer locals so call-site codegen can emit
            // the matching C function-pointer cast.  Two sources:
            //   - explicit annotation:  `fp: fn(int, int) -> int = ...`
            //   - inferred-from-cast:   `fp = expr as fn(int, int) -> int`
            // In both cases, the AST node carries TYPE_FUNCTION with
            // is_fnptr=1 by typecheck time (parser sets it on `as fn`
            // casts; the var-decl node inherits from the initializer
            // via the typechecker's RHS inference pass).
            if (stmt->value) {
                Type* fnptr_sig = NULL;
                if (stmt->node_type && stmt->node_type->kind == TYPE_FUNCTION &&
                    stmt->node_type->is_fnptr) {
                    fnptr_sig = stmt->node_type;
                } else if (stmt->child_count > 0 && stmt->children[0] &&
                           stmt->children[0]->type == AST_PTR_AS_FN_CAST &&
                           stmt->children[0]->node_type &&
                           stmt->children[0]->node_type->kind == TYPE_FUNCTION &&
                           stmt->children[0]->node_type->is_fnptr) {
                    fnptr_sig = stmt->children[0]->node_type;
                }
                if (fnptr_sig) {
                    register_fnptr_local(gen, stmt->value, fnptr_sig);
                }
            }

            // Check if this is a state variable assignment in an actor
            int is_state_var = 0;
            if (gen->current_actor && stmt->value) {
                for (int i = 0; i < gen->state_var_count; i++) {
                    if (strcmp(stmt->value, gen->actor_state_vars[i]) == 0) {
                        is_state_var = 1;
                        break;
                    }
                }
            }
            
            if (is_state_var) {
                /* Generate as assignment to self->field. */
                if (stmt->child_count > 0 &&
                    emit_state_array_store(gen, actor_state_decl(gen, stmt->value),
                                           stmt->children[0])) {
                    /* #2688: a fixed-size array state field, element by element. */
                } else if (stmt->child_count > 0 && actor_state_string_tracked(gen, stmt->value)) {
                    /* R3: a string state field owns what it holds. The store
                     * takes the value (moved when fresh or a local's last
                     * use, else retained, so a message field the envelope
                     * releases after the handler stays the state's) and
                     * releases the one it replaces. */
                    fprintf(gen->output, "{ const char* _ae_new = ");
                    emit_string_take(gen, stmt->children[0], stmt->value);
                    fprintf(gen->output, "; const char* _tmp_old = self->%s; self->%s = _ae_new; "
                            "aether_heap_str_free(_tmp_old); }\n", stmt->value, stmt->value);
                } else if (stmt->child_count > 0 && actor_state_is_ptr(gen, stmt->value) &&
                           fn_value_into_ptr(gen, stmt->children[0])) {
                    /* #2904: a `ptr` state field takes a closure boxed and a
                     * bare function as its address, as every `ptr` slot
                     * does (emit_closure_into_ptr). */
                    fprintf(gen->output, "self->%s = ", stmt->value);
                    emit_closure_into_ptr(gen, stmt->children[0]);
                    fprintf(gen->output, ";\n");
                } else if (stmt->child_count > 0 && stmt->children[0] &&
                           stmt->children[0]->node_type &&
                           stmt->children[0]->node_type->kind == TYPE_FUNCTION &&
                           !stmt->children[0]->node_type->is_fnptr) {
                    /* #2525: state holds a reference of its own to a closure
                     * (a message field's or a local's is retained, a fresh
                     * one adopted) and gives back the one it held. */
                    fprintf(gen->output, "{ void* _ae_old = self->%s.env; self->%s = ",
                            stmt->value, stmt->value);
                    emit_closure_take(gen, stmt->children[0]);
                    fprintf(gen->output, "; _aether_closure_env_release(_ae_old); }\n");
                } else if (stmt->child_count > 0 && stmt->children[0] &&
                           struct_owning_strings(gen, stmt->children[0]->node_type)) {
                    /* A struct in state that owns strings or closures is
                     * replaced as a local one is (#465, #2497): the old
                     * value's are released and the new value is taken, a
                     * copy of one another owner keeps. A plain store leaked
                     * the old value's strings and shared a local struct's,
                     * which the handler's exit then freed. */
                    const char* sname = struct_owning_strings(gen, stmt->children[0]->node_type);
                    fprintf(gen->output, "_aether_store_%s(&self->%s, ", sname, stmt->value);
                    emit_struct_take(gen, stmt->children[0], sname, stmt->value);
                    fprintf(gen->output, ");\n");
                } else {
                    fprintf(gen->output, "self->%s", stmt->value);
                    if (stmt->child_count > 0) {
                        fprintf(gen->output, " = ");
                        generate_expression(gen, stmt->children[0]);
                    }
                    fprintf(gen->output, ";\n");
                }
            } else {
                // Match-as-expression: x = match val { ... }
                if (stmt->child_count > 0 && stmt->children[0] &&
                    stmt->children[0]->type == AST_MATCH_STATEMENT) {
                    /* #2514: a variable a closure writes lives in its cell,
                     * declared here, empty, when the match is its first
                     * binding; each arm stores into the cell. */
                    Type* cell_type = NULL;
                    if (is_promoted_capture(gen, stmt->value)) {
                        cell_type = (stmt->node_type && stmt->node_type->kind != TYPE_UNKNOWN)
                                    ? stmt->node_type : match_value_type(stmt->children[0]);
                        if (!is_var_declared(gen, stmt->value)) {
                            print_indent(gen);
                            emit_promoted_cell_declaration(gen, stmt->value,
                                get_c_type(cell_type), cell_type, NULL, NULL,
                                stmt->line, stmt->column);
                        }
                    }
                    if (!is_var_declared(gen, stmt->value)) {
                        mark_var_declared(gen, stmt->value);
                        // Infer type from first match arm result
                        const char* c_type = get_c_type(stmt->node_type);
                        ASTNode* match_node = stmt->children[0];
                        if (!stmt->node_type || stmt->node_type->kind == TYPE_UNKNOWN) {
                            Type* arm_type = match_value_type(match_node);
                            if (arm_type) c_type = get_c_type(arm_type);
                        }
                        /* #2497: a struct that owns strings starts empty
                         * (its trackers read by the replace each arm does)
                         * and is released at scope exit, as a struct a
                         * literal or call initialises is. */
                        Type* mt = (stmt->node_type && stmt->node_type->kind != TYPE_UNKNOWN)
                                   ? stmt->node_type : match_value_type(match_node);
                        const char* msname = struct_owning_strings(gen, mt);
                        int opt_result = mt && mt->kind == TYPE_OPTIONAL;
                        int str_result = c_type && strcmp(c_type, "const char*") == 0;
                        print_indent(gen);
                        fprintf(gen->output, (msname || opt_result) ? "%s %s = {0};\n"
                                             : str_result ? "%s %s = NULL;\n" : "%s %s;\n",
                                volatile_decl_type(try_volatile_qual_for(gen, stmt->value), c_type),
                                stmt->value);   /* #2690 */
                        if (msname) {
                            push_struct_destroy_defer(gen, stmt->value, mt,
                                                      stmt->line, stmt->column);
                        }
                        /* #2461: a string local a match binds owns what its
                         * arms hand it, as every other string local does. */
                        if (str_result && !is_heap_string_var(gen, stmt->value) &&
                            !is_promoted_capture(gen, stmt->value))
                            mark_heap_string_var(gen, stmt->value);
                    }
                    /* #2461: a string local takes each arm's value as it
                     * would take it on its own (emit_string_take_rebind)
                     * and releases the value it held before, like the
                     * reassignment wrapper. The arms used to store a bare
                     * pointer: a borrow of a buffer its owner then freed,
                     * and the old value leaked. A block or statement arm
                     * leaves the local as it was. */
                    const char* saved_mvar = gen->match_result_var;
                    int saved_mstring = gen->match_result_string;
                    const char* saved_mstruct = gen->match_result_struct;
                    int saved_mreplace = gen->match_result_replace;
                    Type* saved_mcell = gen->match_result_cell;
                    Type* saved_mopt = gen->match_result_opt;
                    /* #2497: a local that owns a string-owning struct takes
                     * each arm's struct and releases the one it held. */
                    Type* bound = declared_var_type(gen, stmt->value);
                    if (!bound || bound->kind != TYPE_STRUCT) bound = stmt->node_type;
                    if (!bound || bound->kind != TYPE_STRUCT)
                        bound = match_value_type(stmt->children[0]);
                    const char* bound_struct =
                        (!is_promoted_capture(gen, stmt->value) &&
                         !is_env_capture_name(gen, stmt->value))
                        ? struct_owning_strings(gen, bound) : NULL;
                    int owning = is_heap_string_var(gen, stmt->value) &&
                                 !is_promoted_capture(gen, stmt->value) &&
                                 !is_env_capture_name(gen, stmt->value);
                    // Generate match with result assignment
                    gen->match_result_var = stmt->value;
                    gen->match_result_string = owning;
                    gen->match_result_struct = bound_struct;
                    gen->match_result_replace = 1;
                    gen->match_result_cell = cell_type;
                    /* An optional local takes each arm's value wrapped. */
                    {
                        Type* ot = declared_var_type(gen, stmt->value);
                        if (!ot || ot->kind != TYPE_OPTIONAL) ot = stmt->node_type;
                        gen->match_result_opt = (ot && ot->kind == TYPE_OPTIONAL && !cell_type)
                                                ? ot : NULL;
                    }
                    generate_statement(gen, stmt->children[0]);
                    gen->match_result_opt = saved_mopt;
                    gen->match_result_var = saved_mvar;
                    gen->match_result_string = saved_mstring;
                    gen->match_result_struct = saved_mstruct;
                    gen->match_result_replace = saved_mreplace;
                    gen->match_result_cell = saved_mcell;
                    break;
                }

                // Route 1: promoted captures are heap-allocated cells. In an
                // outer function body, the FIRST assignment declares
                // `int* name = malloc(...); *name = <init>;` and queues a
                // defer for free(). Subsequent writes emit `*name = <expr>;`.
                // In a closure body, the name is never newly declared (it's
                // aliased from _env->name in the prologue), so all writes
                // are dereferences.
                if (is_promoted_capture(gen, stmt->value)) {
                    if (!is_var_declared(gen, stmt->value)) {
                        // First occurrence in this scope — declaration:
                        // allocate the shared cell, initialise it, and
                        // defer the scope's release (#2019).
                        emit_promoted_cell_declaration(gen, stmt->value,
                            get_c_type(stmt->node_type), stmt->node_type,
                            stmt->child_count > 0 ? stmt->children[0] : NULL, "0",
                            stmt->line, stmt->column);
                    } else if (emit_cell_array_store(gen, stmt)) {
                        /* #2474: a whole fixed-size array, stored into its cell. */
                    } else if (stmt->child_count > 0) {
                        // Reassignment: write through the pointer. A string
                        // cell OWNS its value, so it frees the superseded
                        // string as it stores the new one (else a per-
                        // iteration running-max / accumulator leaks every
                        // prior value).
                        emit_cell_store(gen, stmt->value, stmt->node_type, stmt->children[0]);
                    } else {
                        fprintf(gen->output, "*%s;\n", stmt->value);
                    }
                    break;
                }

                // If we're in a closure body and this name is a mutated capture,
                // route the write through _env-> so mutations persist on the env
                // struct rather than dying with a stack-local alias.
                // NOTE: with Route 1, this path is bypassed for promoted names
                // (handled above). It remains as a fallback for the pre-Route-1
                // env-cap mechanism.
                int is_env_cap = 0;
                for (int ec = 0; ec < gen->current_env_capture_count; ec++) {
                    if (gen->current_env_captures[ec] &&
                        strcmp(gen->current_env_captures[ec], stmt->value) == 0) {
                        is_env_cap = 1;
                        break;
                    }
                }
                if (is_env_cap) {
                    fprintf(gen->output, "_env->%s", stmt->value);
                    if (stmt->child_count > 0) {
                        fprintf(gen->output, " = ");
                        generate_expression(gen, stmt->children[0]);
                    }
                    fprintf(gen->output, ";\n");
                    break;
                }

                // #701: a `name = expr` whose name is a module-level `var`
                // global is a WRITE to that file-scope static, not a new
                // local — the global is in scope in every same-module
                // function (the issue's required semantics: reads and writes
                // are plain identifier access). Reads already resolve to the
                // bare identifier, so only the write side needs steering. The
                // guard on !is_var_declared lets a same-named parameter or an
                // earlier in-function local legitimately shadow the global.
                if (stmt->value &&
                    !is_var_declared(gen, stmt->value) &&
                    is_module_global_var(gen, stmt->value)) {
                    print_indent(gen);
                    ASTNode* grhs = stmt->child_count > 0 ? stmt->children[0] : NULL;
                    /* #2811: a `ptr` global holds a string as the pointer
                     * it is, through a cast: the string is a `const char*`,
                     * which C does not assign to a `void*` without a
                     * warning in the user's build. */
                    Type* gt = module_global_decl_type(gen, stmt->value);
                    const char* gcast =
                        gt && gt->kind == TYPE_PTR && grhs && grhs->node_type &&
                                grhs->node_type->kind == TYPE_STRING
                            ? cg_internf("(%s)", get_c_type(gt)) : "";
                    if (grhs && grhs->node_type && grhs->node_type->kind == TYPE_STRING &&
                        !(gt && gt->kind == TYPE_PTR)) {
                        /* R3: a module `var` owns the string it holds, as a
                         * local does: the store takes the new value, publishes
                         * it since any thread may read the global (#2919), and
                         * releases the one it replaces. */
                        fprintf(gen->output, "{ const char* _ae_new = ");
                        emit_string_take(gen, grhs, stmt->value);
                        fprintf(gen->output, "; const char* _ae_old = %s; %s = _ae_new; "
                                "aether_string_publish(%s); aether_heap_str_free(_ae_old); }\n",
                                stmt->value, stmt->value, stmt->value);
                        break;
                    }
                    if (gt && gt->kind == TYPE_PTR && grhs && grhs->node_type &&
                        grhs->node_type->kind == TYPE_FUNCTION) {
                        /* #2890: a `ptr` global takes a closure boxed, as a
                         * `ptr` field and a `ptr` parameter do, and a bare
                         * function as its code address. */
                        fprintf(gen->output, "%s = ", stmt->value);
                        if (!emit_closure_into_ptr(gen, grhs)) generate_expression(gen, grhs);
                        fprintf(gen->output, ";\n");
                        break;
                    }
                    if (stmt->child_count > 0 && stmt->children[0] &&
                        stmt->children[0]->node_type &&
                        stmt->children[0]->node_type->kind == TYPE_FUNCTION &&
                        !stmt->children[0]->node_type->is_fnptr) {
                        /* #2525: a global holds a reference of its own to a
                         * closure and gives back the one it held when
                         * rebound; the last one lives as long as the program. */
                        fprintf(gen->output, "{ void* _ae_old = %s.env; %s = ",
                                stmt->value, stmt->value);
                        emit_closure_take(gen, stmt->children[0]);
                        fprintf(gen->output, "; _aether_closure_env_release(_ae_old); }\n");
                        break;
                    }
                    /* R3: a struct, tuple or optional that owns is stored
                     * as a string is above, the old value dropped. */
                    Type* gvt = gt ? gt : (grhs ? grhs->node_type : NULL);
                    if (grhs && gvt && gvt->kind != TYPE_ARRAY && gvt->kind != TYPE_STRING &&
                        gvt->kind != TYPE_FUNCTION && own_type_owns(gen, gvt)) {
                        emit_owning_store(gen, stmt->value, gvt, grhs, 1);
                        break;
                    }
                    fprintf(gen->output, "%s", stmt->value);
                    if (stmt->child_count > 0) {
                        fprintf(gen->output, " = %s", gcast);
                        generate_expression(gen, stmt->children[0]);
                    }
                    fprintf(gen->output, ";\n");
                    break;
                }

                // Check if this is a reassignment (Python-style)
                if (is_var_declared(gen, stmt->value)) {
                    /* #2124: a hoisted local is one C variable for the
                     * whole function. A bare re-bind here — in a sibling
                     * branch or loop body the typechecker could not see
                     * as the same variable — with a value that cannot
                     * flow into the hoisted type used to be a C error
                     * ("assignment to 'const char *' from 'int'"); say
                     * it in the language's terms. The numeric and
                     * pointer conversions the language permits pass. */
                    Type* hoisted = declared_var_type(gen, stmt->value);
                    Type* here = stmt->node_type;
                    if ((!here || here->kind == TYPE_UNKNOWN) && stmt->child_count > 0 &&
                        stmt->children[0]) here = stmt->children[0]->node_type;
                    if (stmt->type_inferred && hoisted && here &&
                        hoisted->kind != TYPE_UNKNOWN && here->kind != TYPE_UNKNOWN &&
                        /* Same kind is the same C variable whatever the
                         * nominal wrapper (a distinct string into a
                         * string-hoisted local; the typechecker owns the
                         * nominal rules). Optionals have their own re-bind
                         * rules there too. */
                        here->kind != hoisted->kind &&
                        here->kind != TYPE_OPTIONAL && hoisted->kind != TYPE_OPTIONAL &&
                        !((here->kind == TYPE_PTR && hoisted->kind == TYPE_STRING) ||
                          (here->kind == TYPE_STRING && hoisted->kind == TYPE_PTR)) &&
                        (!is_type_compatible(here, hoisted) ||
                         (here->kind == TYPE_PTR && rebind_is_number(hoisted->kind)) ||
                         (rebind_is_number(here->kind) && hoisted->kind == TYPE_PTR))) {
                        char msg[400];
                        snprintf(msg, sizeof(msg),
                                 "cannot bind '%s' as %s: it is bound as %s in another branch or "
                                 "loop body of this function, and a local first bound inside a "
                                 "branch or loop body is one variable for the whole function. Use "
                                 "a new name for the %s value, or convert it to %s",
                                 stmt->value, type_to_string(here), type_to_string(hoisted),
                                 type_to_string(here), type_to_string(hoisted));
                        AetherError e = { stmt->source_file, NULL, stmt->line, stmt->column, msg,
                                          NULL, NULL, AETHER_ERR_TYPE_MISMATCH };
                        aether_error_report(&e);
                    }
                    /* Self-assignment peephole. `p = p` is a no-op at
                     * the semantic level, so nothing is emitted. Through
                     * the rebind wrapper, avn's `noop_free(p: string)
                     * { p = p }` shim once double-freed the caller's
                     * string on every call. See
                     * path-b-self-assign-param-doublefree.md filing.
                     *
                     * The guard fires before any other wrapper-
                     * emission branch, so it covers every code path
                     * downstream uniformly. */
                    if (stmt->child_count > 0 && stmt->children[0] &&
                        stmt->children[0]->type == AST_IDENTIFIER &&
                        stmt->children[0]->value && stmt->value &&
                        strcmp(stmt->children[0]->value, stmt->value) == 0) {
                        break;
                    }
                    /* #2289: an array literal bound to a fixed-size array
                     * that is already declared -- hoisted out of the loop
                     * or branch it is written in, or bound earlier -- is
                     * stored element by element, in order: C accepts
                     * `{...}` only in a declaration. Elements past the
                     * literal's are zeroed, as the declaration's
                     * initializer zeroes them, so the array holds exactly
                     * the new value. */
                    if (stmt->child_count > 0 && stmt->children[0] &&
                        stmt->children[0]->type == AST_ARRAY_LITERAL &&
                        hoisted && hoisted->kind == TYPE_ARRAY && hoisted->array_size > 0) {
                        ASTNode* lit = stmt->children[0];
                        if (lit->child_count > hoisted->array_size) {
                            char msg[300];
                            snprintf(msg, sizeof(msg),
                                     "array literal of %d elements assigned to '%s', which holds %d: an array keeps the size of its first binding",
                                     lit->child_count, stmt->value, hoisted->array_size);
                            AetherError e = { stmt->source_file, NULL, stmt->line, stmt->column, msg,
                                              NULL, NULL, AETHER_ERR_TYPE_MISMATCH };
                            aether_error_report(&e);
                            break;
                        }
                        if (str_arr_type(hoisted) &&
                            string_array_owns(gen, gen->hoist_scope_body, stmt->value)) {
                            emit_owned_str_array_store(gen, stmt, hoisted);   /* #2618 */
                            break;
                        }
                        /* #2921: an array of owning elements takes each
                         * element of the literal into a new array, then drops
                         * the old one and copies the new one in: an element
                         * that reads the array reads it whole. */
                        if (own_type_owns(gen, hoisted)) {
                            emit_owning_array_store(gen, stmt->value, hoisted, lit);
                            break;
                        }
                        /* #2478: the literal is evaluated before it is
                         * stored, in source order: an element that reads
                         * the array sees it from before the stores. */
                        order_prelude_begin(gen, lit->children, lit->child_count, stmt->value);
                        for (int ei = 0; ei < lit->child_count; ei++) {
                            if (ei > 0) print_indent(gen);
                            fprintf(gen->output, "%s[%d] = ", stmt->value, ei);
                            generate_expression(gen, lit->children[ei]);
                            fprintf(gen->output, ";\n");
                        }
                        if (lit->child_count < hoisted->array_size) {
                            if (lit->child_count > 0) print_indent(gen);
                            fprintf(gen->output, "memset((void*)&%s[%d], 0, sizeof(%s[0]) * %d);\n",
                                    stmt->value, lit->child_count, stmt->value,
                                    hoisted->array_size - lit->child_count);
                        }
                        break;
                    }
                    /* #2516: another fixed-size array bound to this one
                     * copies its elements (the typechecker holds both to
                     * one length); C does not assign arrays. memmove: the
                     * source may be the array itself. */
                    if (stmt->child_count > 0 && stmt->children[0] &&
                        hoisted && is_sized_array_param(hoisted) &&
                        is_sized_array_param(stmt->children[0]->node_type)) {
                        /* #2894, #2921: an array of owning elements, or a
                         * call's result, takes the new value whole. */
                        if (own_type_owns(gen, hoisted) ||
                            stmt->children[0]->type == AST_FUNCTION_CALL) {
                            if (own_type_owns(gen, hoisted)) {
                                emit_owning_array_store(gen, stmt->value, hoisted, stmt->children[0]);
                            } else {
                                emit_array_fill(gen, stmt->value, stmt->children[0], hoisted);
                                fprintf(gen->output, ";\n");
                            }
                            break;
                        }
                        /* #2528: an owning local array replaces each element
                         * with a copy of the source's (a self-copy keeps
                         * every element). */
                        if (str_arr_type(hoisted) &&
                            string_array_owns(gen, gen->hoist_scope_body, stmt->value)) {
                            emit_owned_str_array_store(gen, stmt, hoisted);   /* #2618 */
                            break;
                        }
                        const char* owning_elem = struct_owning_strings(gen, hoisted->element_type);
                        if (owning_elem && struct_array_local_owns(gen, stmt->value)) {
                            /* Either array may be one a `try` body writes,
                             * whose elements are volatile (#2690). */
                            fprintf(gen->output, "{ %s const volatile* _ae_src = ", owning_elem);
                            generate_expression(gen, stmt->children[0]);
                            fprintf(gen->output, "; for (int _ae_k = 0; _ae_k < %d; _ae_k++) %s(&%s[_ae_k], _aether_copy_%s(_ae_src[_ae_k])); }\n",
                                    hoisted->array_size,
                                    struct_replace_fn(owning_elem,
                                                      is_try_clobbered_var(gen, stmt->value)),
                                    stmt->value, owning_elem);
                            break;
                        }
                        /* An array a `try` body writes is volatile: the
                         * casts keep memmove taking it (#2691). */
                        fprintf(gen->output, "memmove((void*)%s, (const void*)(", stmt->value);
                        generate_expression(gen, stmt->children[0]);
                        fprintf(gen->output, "), sizeof(%s));\n", stmt->value);
                        break;
                    }
                    // Already declared - generate assignment only.
                    //
                    // For string-tracked variables (issue #405) the
                    // assignment must go through the heap-aware
                    // wrapper for *every* string→string transition so
                    // the tracker stays in lock-step with the actual
                    // pointer's heap-ness. Four transitions, all
                    // handled by one shape:
                    //   heap → heap  : free old, _heap=1
                    //   heap → lit   : free old, _heap=0
                    //   lit  → heap  : no free (_heap was 0), _heap=1
                    //   lit  → lit   : no free, _heap=0
                    // The wrapper does this uniformly via `if (_heap_X)
                    // free(_tmp_old); _heap_X = <init_heap>`. Without
                    // this, heap→lit would leave _heap stale and a
                    // later free could attempt to release a literal.
                    int rhs_is_heap = (stmt->child_count > 0 &&
                                       is_heap_string_expr(gen, stmt->children[0]));
                    int var_is_string = is_heap_string_var(gen, stmt->value);
                    /* #2904: a `ptr` local takes a closure boxed and a bare
                     * function as its address, as every `ptr` slot does
                     * (emit_closure_into_ptr). */
                    Type* ptr_local_t = declared_var_type(gen, stmt->value);
                    if (!ptr_local_t) ptr_local_t = stmt->node_type;
                    if (ptr_local_t && ptr_local_t->kind == TYPE_PTR && stmt->child_count > 0 &&
                        fn_value_into_ptr(gen, stmt->children[0])) {
                        fprintf(gen->output, "%s = ", stmt->value);
                        emit_closure_into_ptr(gen, stmt->children[0]);
                        fprintf(gen->output, ";\n");
                    } else if (var_is_string && stmt->child_count > 0) {
                        emit_string_take_rebind(gen, stmt->value, stmt->children[0]);
                    } else if (rhs_is_heap) {
                        // Non-string-typed variable being reassigned
                        // to a heap string. Rare (type-inference edge
                        // cases): it owns the string from here on.
                        if (!is_heap_string_var(gen, stmt->value))
                            mark_heap_string_var(gen, stmt->value);
                        emit_string_take_rebind(gen, stmt->value, stmt->children[0]);
                    } else if (closure_alias_binding(gen, stmt)) {
                        /* #2668: the closure it aliases counted this binding
                         * as one that takes a reference of its own, so it
                         * does here too, where the local frees nothing (one
                         * that escapes keeps what it was given). */
                        fprintf(gen->output, "%s = ", stmt->value);
                        emit_closure_take(gen, stmt->children[0]);
                        fprintf(gen->output, ";\n");
                    } else {
                        // Plain non-string assignment.
                        /* Struct-reassignment heap cleanup (#465).
                         * `b = Box { ... }` overwrites the struct
                         * wholesale; without an explicit destroy of
                         * the previous instance, every heap-string
                         * field's old buffer leaks. Emit
                         * `_aether_store_<Struct>(&<var>, <value>)`, which
                         * stores the new value and destroys the one it
                         * replaces. */
                        Type* vtype = stmt->node_type;
                        if ((!vtype || vtype->kind != TYPE_STRUCT) &&
                            stmt->child_count > 0 && stmt->children[0]) {
                            vtype = stmt->children[0]->node_type;
                        }
                        if (stmt->child_count > 0 &&
                            emit_struct_replace_assign(gen, stmt->value, vtype,
                                                       stmt->children[0])) {
                            /* emitted */
                        } else if (stmt->child_count > 0) {
                            fprintf(gen->output, "%s = ", stmt->value);
                            generate_expression(gen, stmt->children[0]);
                            fprintf(gen->output, ";\n");
                        } else {
                            /* A re-declaration with no initializer, of a name
                             * something already declared: `byte[8] scratch`
                             * inside a loop, whose declaration the array hoist
                             * lifted to function scope. There is nothing left
                             * to assign, and emitting the bare name left
                             * `scratch;` in the output, a dead statement that
                             * is -Wunused-value in the user's own build. */
                            fprintf(gen->output, "\n");
                        }
                    }
                    // Handle trailing blocks on reassignment (same as first declaration)
                    if (stmt->child_count > 0 && stmt->children[0] &&
                        stmt->children[0]->type == AST_FUNCTION_CALL) {
                        ASTNode* reinit_call = stmt->children[0];
                        int reinit_is_builder = reinit_call->value &&
                            is_builder_func_reg(gen, reinit_call->value);
                        int reinit_has_trailing = 0;
                        for (int tc = 0; tc < reinit_call->child_count; tc++) {
                            if (reinit_call->children[tc] && reinit_call->children[tc]->type == AST_CLOSURE &&
                                reinit_call->children[tc]->value &&
                                strcmp(reinit_call->children[tc]->value, "trailing") == 0) {
                                reinit_has_trailing = 1;
                                break;
                            }
                        }
                        if (reinit_has_trailing && !reinit_is_builder) {
                            // REGULAR PATTERN: push reassigned value as context, run block
                            for (int tc = 0; tc < reinit_call->child_count; tc++) {
                                ASTNode* trailing = reinit_call->children[tc];
                                if (trailing && trailing->type == AST_CLOSURE &&
                                    trailing->value && strcmp(trailing->value, "trailing") == 0) {
                                    for (int bi = 0; bi < trailing->child_count; bi++) {
                                        if (trailing->children[bi] && trailing->children[bi]->type == AST_BLOCK) {
                                            print_indent(gen);
                                            fprintf(gen->output, "_aether_ctx_push((void*)(intptr_t)%s);\n",
                                                    safe_c_name(stmt->value));
                                            emit_trailing_block_body(gen, trailing->children[bi]);
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else if (stmt->child_count > 0 &&
                           string_take_is_view(gen, stmt->children[0])) {
                    /* #2461: first binding of a string local (one the
                     * function-entry hoist did not declare) to a view: declare
                     * it and its tracker, then take the value as the
                     * reassignment path does. */
                    Type* st = create_type(TYPE_STRING);
                    print_indent(gen);
                    fprintf(gen->output, "%s %s = NULL;",
                            volatile_decl_type(try_volatile_qual_for(gen, stmt->value),
                                               "const char*"), stmt->value);
                    mark_var_declared_typed(gen, stmt->value, st);
                    free_type(st);
                    if (!is_heap_string_var(gen, stmt->value))
                        mark_heap_string_var(gen, stmt->value);
                    fprintf(gen->output, "\n");
                    print_indent(gen);
                    emit_string_take_rebind(gen, stmt->value, stmt->children[0]);
                } else {
                    // First declaration - generate type + variable
                    unwind_track_hold(gen);
                    /* #2289: a fixed-size array records its type, so a
                     * later binding of an array literal to the same name
                     * knows how many elements the C array holds. */
                    {
                        Type* arr_decl = NULL;
                        int arr_owned = 0;
                        if (stmt->node_type && stmt->node_type->kind == TYPE_ARRAY) {
                            if (stmt->node_type->array_size > 0) arr_decl = stmt->node_type;
                        } else if (stmt->child_count > 0 && stmt->children[0] &&
                                   stmt->children[0]->type == AST_ARRAY_LITERAL &&
                                   stmt->children[0]->child_count > 0) {
                            /* The `int name[N]` declaration below, for an
                             * initializer the type system left untyped. */
                            arr_decl = create_array_type(create_type(TYPE_INT),
                                                         stmt->children[0]->child_count);
                            arr_owned = 1;
                        }
                        mark_var_declared_typed(gen, stmt->value, arr_decl);
                        if (arr_owned) free_type(arr_decl);
                    }

                    // Detect if initializer is an array literal (type system may not tag empty arrays)
                    /* #1286: a literal into a `T[]` binding arrives wrapped
                     * in the typechecker's slice coercion; look through it. */
                    ASTNode* init_lit = (stmt->child_count > 0) ? stmt->children[0] : NULL;
                    if (init_lit && init_lit->type == AST_SLICE_FROM_ARRAY &&
                        init_lit->child_count > 0 &&
                        init_lit->children[0]->type == AST_ARRAY_LITERAL)
                        init_lit = init_lit->children[0];
                    int is_array_init = (init_lit && init_lit->type == AST_ARRAY_LITERAL);
                    /* #2478: an initializer list leaves its elements' order
                     * open; the ones that need it run first, in order. */
                    if (is_array_init)
                        order_prelude_begin(gen, init_lit->children, init_lit->child_count, NULL);
                    /* #1286: >= 0 when a `T[]` local takes an array literal
                     * (the literal's length; the slice views a hidden array). */
                    int slice_lit_len = -1;

                    // Handle array types specially (C syntax: int name[size])
                    /* Issue #501 follow-up: volatile prefix for
                     * try-clobbered locals.  Same `vq` value used
                     * across each branch below so we only consult
                     * the set once. */
                    const char* vq = try_volatile_qual_for(gen, stmt->value);
                    /* #2497: set when this local takes a string-owning struct
                     * that another owner keeps (emit_struct_take). */
                    const char* decl_struct_take = NULL;
                    /* #2618: a `string[N]` local that owns its elements. */
                    int decl_str_array = 0;
                    /* #2921: an array of closures, tuples or optionals bound
                     * from a literal starts empty and takes each element
                     * (emit_array_fill), and its scope exit drops them; string
                     * and struct arrays have their own owning paths. */
                    Type* dat = stmt->node_type;
                    int decl_own_fill = is_array_init && dat && type_is_sized_array(dat) &&
                                        dat->array_size > 0 && dat->element_type &&
                                        own_type_owns(gen, dat) &&
                                        dat->element_type->kind != TYPE_STRING &&
                                        !struct_owning_strings(gen, dat->element_type) &&
                                        !is_promoted_capture(gen, stmt->value) &&
                                        !is_module_global_var(gen, stmt->value) &&
                                        !is_actor_state_var(gen, stmt->value);
                    /* #2516: a fixed-size array bound from another one
                     * (`string[2] b = a`) copies its elements, as a later
                     * binding of it does; C does not initialise an array
                     * from an array, and `= a` did not compile. */
                    ASTNode* decl_array_src =
                        (stmt->node_type && is_sized_array_param(stmt->node_type) &&
                         stmt->child_count > 0 && stmt->children[0] && !is_array_init &&
                         is_sized_array_param(stmt->children[0]->node_type))
                            ? stmt->children[0] : NULL;
                    if (stmt->node_type && stmt->node_type->kind == TYPE_ARRAY) {
                        const char* elem_type = get_c_type(stmt->node_type->element_type);
                        if (stmt->node_type->array_size > 0) {
                            fprintf(gen->output, "%s %s[%d]", volatile_decl_type(vq, elem_type),
                                    stmt->value, stmt->node_type->array_size);
                            if (string_array_owns(gen, gen->hoist_scope_body, stmt->value) &&
                                !is_promoted_capture(gen, stmt->value) &&
                                !is_module_global_var(gen, stmt->value) &&
                                !is_actor_state_var(gen, stmt->value)) {
                                decl_str_array = 1;
                                push_str_array_release_defer(gen, stmt->value,
                                                             stmt->node_type->array_size,
                                                             stmt->line, stmt->column);
                            }
                            /* #2528: a local array of structs that own
                             * strings or closures owns its elements: each
                             * is destroyed at scope exit, an element store
                             * replaces (emit_struct_element_store). */
                            const char* owning_elem = struct_owning_strings(gen, stmt->node_type->element_type);
                            if (owning_elem && is_array_init && !is_promoted_capture(gen, stmt->value))
                                push_own_drop(gen, stmt->value, stmt->node_type, stmt->line,
                                              stmt->column);
                        } else {
                            /* #1286: a `T[]` local is a slice. A literal
                             * initializer lands in a hidden fixed array the
                             * slice then views. */
                            if (is_array_init && init_lit->child_count > 0) {
                                slice_lit_len = init_lit->child_count;
                                fprintf(gen->output, "%s _ae_slit_%s[%d] = ",
                                        elem_type, stmt->value, slice_lit_len);
                                generate_expression(gen, init_lit);
                                fprintf(gen->output, ";\n");
                                print_indent(gen);
                            } else if (is_array_init) {
                                slice_lit_len = 0;
                            }
                            fprintf(gen->output, "%sAetherSlice %s", vq, stmt->value);
                        }
                    } else if (is_array_init) {
                        // Type system missed array type but initializer is array literal
                        int arr_size = stmt->children[0]->child_count;
                        if (arr_size > 0) {
                            fprintf(gen->output, "%sint %s[%d]", vq, stmt->value, arr_size);
                        } else {
                            // Empty array [] - use NULL pointer
                            fprintf(gen->output, "%s %s", volatile_decl_type(vq, "int*"), stmt->value);
                        }
                    } else if (stmt->child_count > 0 && stmt->children[0] &&
                               (stmt->children[0]->type == AST_MESSAGE_CONSTRUCTOR ||
                                stmt->children[0]->type == AST_STRUCT_LITERAL) &&
                               stmt->children[0]->value) {
                        // Message/struct constructor — use the constructor name as type.
                        // A header-defined struct is spelled `struct Name`, as
                        // get_c_type spells it: the header need not typedef the tag
                        // (a union `union Name`, #2561).
                        int c_tagged = stmt->children[0]->type == AST_STRUCT_LITERAL &&
                                       aether_is_c_import_struct(stmt->children[0]->value);
                        fprintf(gen->output, "%s%s%s%s %s", vq,
                                c_tagged ? aether_c_tag(stmt->children[0]->value) : "",
                                c_tagged ? " " : "",
                                stmt->children[0]->value, stmt->value);
                        /* Struct-field heap-string ownership (#465).
                         * Push a function-exit defer that calls the
                         * auto-emitted _aether_drop_<Struct>(&<var>) to
                         * free any heap-owned string fields. The
                         * destroy function is no-op for structs
                         * without heap-string fields, but we only
                         * push the defer when the field set actually
                         * needs cleanup (skips the no-op call). */
                        if (stmt->children[0]->type == AST_STRUCT_LITERAL && gen->program) {
                            ASTNode* sdef = find_struct_definition_by_name(
                                gen->program, stmt->children[0]->value);
                            if (sdef && struct_owns_heap_strings(gen, sdef)) {
                                ASTNode* carrier = create_ast_node(
                                    AST_EXPRESSION_STATEMENT, NULL,
                                    stmt->line, stmt->column);
                                if (carrier) {
                                    if (carrier->annotation) free(carrier->annotation);
                                    carrier->annotation = heap_strf(
                                        "struct_destroy:%s:%s",
                                        stmt->value, stmt->children[0]->value);
                                    codegen_own_node(gen, carrier);
                                    push_defer(gen, carrier);
                                    emit_struct_unwind_track(gen, stmt->value,
                                                             stmt->children[0]->value);
                                }
                            }
                        }
                    } else {
                        // Determine the best type for this variable
                        Type* var_type = stmt->node_type;

                        // If type is void/unknown, try to get it from the initializer
                        if ((!var_type || var_type->kind == TYPE_VOID || var_type->kind == TYPE_UNKNOWN)
                            && stmt->child_count > 0 && stmt->children[0]) {
                            ASTNode* init = stmt->children[0];
                            // Check initializer's own node_type
                            if (init->node_type && init->node_type->kind != TYPE_VOID
                                && init->node_type->kind != TYPE_UNKNOWN) {
                                var_type = init->node_type;
                            }
                            // For function calls, look up the function's return type
                            else if (init->type == AST_FUNCTION_CALL && init->value) {
                                /* As its definition declares it, over every
                                 * clause of a set (#2645). */
                                ASTNode* fn = find_function_definition_by_name(gen->program, init->value);
                                Type* rt = fn ? fn_result_type(gen, fn) : NULL;
                                if (rt) var_type = rt;
                            }
                        }

                        /* Issue #501 follow-up: `vq` from above
                         * carries "volatile " when this local is
                         * modified inside a try body of the current
                         * function and we're emitting the decl at
                         * outer scope.  Without this, the C optimizer
                         * may keep the local in a register that the
                         * panic's siglongjmp clobbers and the catch
                         * handler reads the stale pre-try value.
                         * C99 7.13.2.1. */
                        /* #2618: after the `*` of a pointer. */
                        const char* vct = vq[0] ? get_c_type(var_type) : NULL;
                        int vq_after = vct && vct[0] && vct[strlen(vct) - 1] == '*';
                        if (!vq_after) fprintf(gen->output, "%s", vq);
                        generate_type(gen, var_type);
                        fprintf(gen->output, "%s %s", vq_after ? " volatile" : "", stmt->value);
                        /* #752 (caller side): a struct-with-heap-fields
                         * received from a struct-returning CALL transfers
                         * ownership to this local — free its fields at
                         * scope exit. Gated on a function-call initializer
                         * (an owned, freshly-returned struct); a plain
                         * alias (`o2 = o`) is not a call and gets no
                         * defer, so there's no double-free. */
                        if (stmt->child_count > 0 && stmt->children[0] &&
                            stmt->children[0]->type == AST_FUNCTION_CALL) {
                            push_struct_destroy_defer(gen, stmt->value, var_type,
                                                      stmt->line, stmt->column);
                        }
                        /* #2497: `o2 = o`, `x = o.inner`, `e = arr[i]`: the
                         * local used to alias a struct its owner frees, while
                         * a later replace of it freed that owner's strings
                         * too. It now takes the struct (moved out of a local
                         * on its last use, copied otherwise) and owns it. */
                        if (stmt->child_count > 0 &&
                            struct_take_shape(stmt->children[0]) &&
                            struct_owning_strings(gen, var_type)) {
                            decl_struct_take = struct_owning_strings(gen, var_type);
                            push_struct_destroy_defer(gen, stmt->value, var_type,
                                                      stmt->line, stmt->column);
                        }
                    }

                    if (stmt->child_count > 0) {
                        if (is_array_init && stmt->children[0]->child_count == 0) {
                            // Empty array literal gets NULL, not {}
                            fprintf(gen->output, " = NULL");
                        } else {
                            /* Live-source alias copy (180-regression.md):
                             * `dest = src` where src is a still-live heap-
                             * tracked local must NOT steal src's buffer.
                             * Take a binary-safe defensive copy so dest
                             * owns an independent buffer and src is left
                             * intact for its later reads. The matching
                             * heap-flag block below sets `_heap_dest = 1`
                             * and leaves `_heap_src` untouched. */
                            ASTNode* di = stmt->children[0];
                            int copy_alias = (di && di->type == AST_IDENTIFIER &&
                                              di->value &&
                                              is_heap_string_var(gen, di->value) &&
                                              strcmp(di->value, stmt->value) != 0 &&
                                              alias_source_must_copy(gen, di->value));
                            fprintf(gen->output, " = ");
                            if (slice_lit_len == 0) {
                                fprintf(gen->output, "aether_slice_make((void*)0, 0)");
                            } else if (slice_lit_len > 0) {
                                fprintf(gen->output, "aether_slice_make(_ae_slit_%s, %d)",
                                        stmt->value, slice_lit_len);
                            } else if (copy_alias) {
                                fprintf(gen->output, "aether_uniform_heap_str(");
                                generate_expression(gen, di);
                                fprintf(gen->output, ", 0)");
                            } else if (decl_array_src || decl_own_fill) {
                                fprintf(gen->output, "{0}");
                            } else if (decl_str_array && is_array_init) {
                                /* #2618: each element taken as a store into
                                 * the array takes it. */
                                fprintf(gen->output, "{");
                                for (int ei = 0; ei < init_lit->child_count; ei++) {
                                    fprintf(gen->output, ei ? ", " : " ");
                                    emit_cell_element_value(gen, 1, init_lit->children[ei]);
                                }
                                fprintf(gen->output, " }");
                            } else if (decl_struct_take) {
                                emit_struct_take(gen, stmt->children[0],
                                                 decl_struct_take, stmt->value);
                            } else if (stmt->node_type && stmt->node_type->kind == TYPE_PTR &&
                                       fn_value_into_ptr(gen, stmt->children[0])) {
                                /* #2904: a `ptr` local takes a closure boxed
                                 * and a bare function as its address, as
                                 * every `ptr` slot does. */
                                emit_closure_into_ptr(gen, stmt->children[0]);
                            } else if (closure_view_binding(stmt->children[0]) &&
                                       !is_promoted_capture(gen, stmt->value) &&
                                       !is_module_global_var(gen, stmt->value) &&
                                       !is_actor_state_var(gen, stmt->value)) {
                                /* #2525: `x = h.cb` holds a reference of its
                                 * own, released by this scope, so `h` may
                                 * be destroyed first. */
                                emit_closure_take(gen, stmt->children[0]);
                            } else if (closure_alias_binding(gen, stmt)) {
                                /* #2668: `a = b` likewise, and `b` keeps
                                 * releasing its own. */
                                emit_closure_take(gen, stmt->children[0]);
                            } else {
                                generate_expression(gen, stmt->children[0]);
                            }
                        }
                    }

                    /* #2618: an owning array starts empty, so its exit
                     * frees nothing it did not take. */
                    if (decl_str_array && stmt->child_count == 0) fprintf(gen->output, " = {0}");
                    fprintf(gen->output, ";\n");
                    unwind_track_release(gen);
                    if (decl_array_src) {
                        /* #2894, #2921: the array takes the value whole (a
                         * call's result moved out, another array's elements
                         * retained), and drops what it holds at its exit. */
                        print_indent(gen);
                        emit_array_fill(gen, stmt->value, decl_array_src, stmt->node_type);
                        fprintf(gen->output, ";\n");
                        if (!decl_str_array && own_type_owns(gen, stmt->node_type) &&
                            !is_promoted_capture(gen, stmt->value))
                            push_own_drop(gen, stmt->value, stmt->node_type, stmt->line,
                                          stmt->column);
                    }
                    if (decl_own_fill) {
                        print_indent(gen);
                        emit_array_fill(gen, stmt->value, stmt->children[0], dat);
                        fprintf(gen->output, ";\n");
                        push_own_drop(gen, stmt->value, dat, stmt->line, stmt->column);
                    }
                    // Emit heap-ownership flag for string variables.
                    // This flag is checked at reassignment to avoid freeing
                    // string literals; it's set to 1 after the first heap
                    // string assignment (string_concat, string_substring, etc.).
                    {
                        Type* vt = stmt->node_type;
                        if ((!vt || vt->kind == TYPE_UNKNOWN || vt->kind == TYPE_VOID)
                            && stmt->child_count > 0 && stmt->children[0]
                            && stmt->children[0]->node_type) {
                            vt = stmt->children[0]->node_type;
                        }
                        int is_string_var = (vt && vt->kind == TYPE_STRING);
                        // Also detect string by initializer: literal string or string function
                        if (!is_string_var && stmt->child_count > 0 && stmt->children[0]) {
                            ASTNode* init = stmt->children[0];
                            if (init->type == AST_LITERAL && init->value &&
                                init->node_type && init->node_type->kind == TYPE_STRING) {
                                is_string_var = 1;
                            }
                            if (is_heap_string_expr(gen, init)) {
                                is_string_var = 1;
                            }
                        }
                        if (is_string_var) {
                            int init_heap = (stmt->child_count > 0 &&
                                             is_heap_string_expr(gen, stmt->children[0]));
                            /* Alias-ownership transfer at first declaration.
                             * `sorted = new_sorted` (bare-id RHS to a heap-
                             * tracked local) MUST move the ownership flag,
                             * not duplicate it. Otherwise both slots claim
                             * the same buffer and one of them double-frees
                             * (function-exit defer for the source, caller's
                             * reassign-wrapper for the destination). The
                             * reassignment branch at codegen_stmt.c:2647
                             * has the same logic; this is the mirror for
                             * the first-declaration shape. */
                            ASTNode* alias_init = (stmt->child_count > 0) ? stmt->children[0] : NULL;
                            int init_is_alias_to_heap_var =
                                (alias_init &&
                                 alias_init->type == AST_IDENTIFIER &&
                                 alias_init->value &&
                                 is_heap_string_var(gen, alias_init->value) &&
                                 strcmp(alias_init->value, stmt->value) != 0);
                            print_indent(gen);
                            /* Live-source guard (180-regression.md): when
                             * the source is read again after this alias,
                             * a move would free its buffer out from under
                             * it on the alias's next reassignment. The
                             * `dest = aether_uniform_heap_str(src, 0)`
                             * rewrite above already replaced the bare
                             * alias with a defensive copy, so here the
                             * dest simply owns its own copy and the source
                             * keeps its flag untouched. */
                            int alias_copied = (init_is_alias_to_heap_var &&
                                                alias_source_must_copy(gen, alias_init->value));
                            const char* name = stmt->value;
                            int hoisted = is_heap_string_var(gen, name);
                            if (alias_copied || init_heap) {
                                /* Its own already: a copy, or a fresh value. */
                            } else if (init_is_alias_to_heap_var) {
                                /* R2: moved from an owning source, left empty. */
                                fprintf(gen->output, "%s = NULL;", alias_init->value);
                            } else if (stmt->child_count > 0) {
                                /* R2: the local owns what it holds; a borrowed
                                 * initializer is taken. */
                                fprintf(gen->output, "%s = aether_uniform_heap_str(%s, 0);", name, name);
                            }
                            if (!hoisted) mark_heap_string_var(gen, name);
                            fprintf(gen->output, "\n");
                        }
                    }
                    // Record variable→closure mapping for closure invocation.
                    // If the variable was previously bound to a different
                    // closure (e.g. reassigned from |a,b|->a+b to |a,b|->a*b),
                    // mark the entry as ambiguous (closure_id = -1) so
                    // call() falls back to generic function-pointer dispatch
                    // through .fn — which always reflects the currently-stored
                    // closure, not whichever one was first assigned.
                    if (stmt->child_count > 0 && stmt->children[0] &&
                        stmt->children[0]->type == AST_CLOSURE &&
                        stmt->children[0]->value && stmt->value) {
                        closure_var_bind(gen, gen->closure_var_scope, stmt->value,
                                         atoi(stmt->children[0]->value));

                        /* Free the env at scope exit when the value stays
                         * here (#2480). The free used to be pushed only when
                         * closure_var_map had no entry for the name yet, but
                         * discover_closures seeds that map for every closure
                         * binding before any statement is emitted, so the
                         * free was never pushed and every call leaked the
                         * env and the cells it holds. */
                    } else if (stmt->child_count > 0 && stmt->children[0] &&
                               stmt->children[0]->type == AST_FUNCTION_CALL && stmt->value) {
                        /* #2494: the result of a function that returns a
                         * closure only its caller holds is freed here too. */
                    } else if (stmt->child_count > 0 &&
                               (closure_view_binding(stmt->children[0]) ||
                                closure_ask_binding(stmt->children[0]) ||
                                closure_alias_binding(gen, stmt)) &&
                               stmt->value) {
                        /* #2525: a retained field or element read, likewise;
                         * #2528: a reply's closure too; #2668: an alias. */
                    }
                    // Suppress unused-variable warning for arrays used with list
                    // pattern matching — the paired _len variable may be the only
                    // one used when patterns only check size ([], [_], wildcard).
                    if (is_array_init || (stmt->node_type && stmt->node_type->kind == TYPE_ARRAY)) {
                        print_line(gen, "(void)%s;", stmt->value);
                    }

                    // Handle trailing blocks on function calls used as initializers
                    // e.g., root = make_container("root") { ... }
                    if (stmt->child_count > 0 && stmt->children[0] &&
                        stmt->children[0]->type == AST_FUNCTION_CALL) {
                        ASTNode* init_call = stmt->children[0];
                        int init_is_defer = init_call->value &&
                            is_builder_func_reg(gen, init_call->value);

                        /* A builder's block ran ahead of the statement
                         * (generate_statement). */
                        if (!init_is_defer) {
                            // REGULAR PATTERN: function already called, push result as context
                            for (int tc = 0; tc < init_call->child_count; tc++) {
                                ASTNode* trailing = init_call->children[tc];
                                if (trailing && trailing->type == AST_CLOSURE &&
                                    trailing->value && strcmp(trailing->value, "trailing") == 0) {
                                    for (int bi = 0; bi < trailing->child_count; bi++) {
                                        if (trailing->children[bi] &&
                                            trailing->children[bi]->type == AST_BLOCK) {
                                            // Push the variable's value as builder context
                                            print_indent(gen);
                                            fprintf(gen->output, "_aether_ctx_push((void*)(intptr_t)%s);\n",
                                                    safe_c_name(stmt->value));
                                            emit_trailing_block_body(gen, trailing->children[bi]);
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            break;
        }

        case AST_ASSIGNMENT:
            if (stmt->child_count >= 2) {
                ASTNode* lhs = stmt->children[0];
                ASTNode* rhs = stmt->children[1];

                /* #340: optional-chain assignment `recv?.field = rhs`. */
                if (emit_optional_chain_assign(gen, lhs, rhs)) break;

                // Check if RHS is a function call with a trailing block
                int assign_has_trailing = 0;
                if (rhs && rhs->type == AST_FUNCTION_CALL) {
                    for (int tc = 0; tc < rhs->child_count; tc++) {
                        if (rhs->children[tc] && rhs->children[tc]->type == AST_CLOSURE &&
                            rhs->children[tc]->value &&
                            strcmp(rhs->children[tc]->value, "trailing") == 0) {
                            assign_has_trailing = 1;
                            break;
                        }
                    }
                }

                /* Struct-field heap-string ownership (#465). Helper
                 * handles both AST_ASSIGNMENT and AST_EXPRESSION_
                 * STATEMENT-wrapping-BINARY-`=` shapes (the parser
                 * lands at different node types depending on whether
                 * the LHS is a struct field vs. an array element vs.
                 * a bare local — keep both paths routed through the
                 * same wrapper). */
                if (emit_struct_field_heap_assign(gen, lhs, rhs)) break;
                if (emit_cell_string_element_store(gen, lhs, rhs)) break;   /* #2474 */

                /* #891 @c_struct overlay write (incl. nested `s.a.b = v`):
                 * lowers to a width-correct aether_mem_set_<width> at the
                 * cumulative offset (no C `->field =`). */
                if (lhs && lhs->type == AST_MEMBER_ACCESS &&
                    aether_c_struct_overlay_lhs(lhs)) {
                    const char* cpath = NULL;
                    ASTNode* root = aether_c_struct_chain(lhs, &cpath);
                    const char* sname = root->node_type->element_type->struct_name;
                    long off = 0; const char* width = NULL;
                    if (aether_c_struct_resolve(sname, cpath, &off, &width) && width) {
                        fprintf(gen->output, "aether_mem_set_%s((void*)(", width);
                        generate_expression(gen, root);
                        fprintf(gen->output, "), %ld, ", off);
                        generate_expression(gen, rhs);
                        fprintf(gen->output, ");\n");
                        break;
                    }
                }

                /* #1132 bitstruct field write: `b.f = v` lowers to a
                 * read-modify-write on the backing word —
                 *   b = (b & ~(mask << lo)) | ((v & mask) << lo);
                 * The RHS is masked before shifting, so an out-of-range value
                 * truncates to the field rather than corrupting its neighbours.
                 * No C bitfield is involved, so the layout is exact. */
                if (lhs && lhs->type == AST_MEMBER_ACCESS && lhs->value) {
                    const char* bname = aether_bitstruct_base_name(lhs);
                    if (bname) {
                        int lo = 0, hi = 0, is_bool = 0;
                        const char* backing = NULL;
                        if (aether_bitstruct_resolve(bname, lhs->value, &lo, &hi,
                                                     &is_bool, &backing)) {
                            unsigned long long mask = aether_bitstruct_mask(lo, hi);
                            ASTNode* base = lhs->children[0];
                            print_indent(gen);
                            generate_expression(gen, base);
                            fprintf(gen->output, " = (%s)((", backing ? backing : "unsigned char");
                            generate_expression(gen, base);
                            fprintf(gen->output, " & ~(0x%llxULL << %d)) | ((((unsigned long long)(",
                                    mask, lo);
                            generate_expression(gen, rhs);
                            fprintf(gen->output, ")) & 0x%llxULL) << %d));\n", mask, lo);
                            break;
                        }
                    }
                }

                // Generate the assignment itself
                gen->generating_lvalue = 1;
                generate_expression(gen, lhs);
                gen->generating_lvalue = 0;
                fprintf(gen->output, " = ");
                generate_expression(gen, rhs);
                fprintf(gen->output, ";\n");

                // Handle trailing blocks on the RHS function call
                // Same logic as VAR_DECLARATION trailing block handler
                if (assign_has_trailing && rhs->type == AST_FUNCTION_CALL) {
                    int assign_is_builder = rhs->value &&
                        is_builder_func_reg(gen, rhs->value);

                    /* A builder's ran ahead of the assignment (above). */
                    if (!assign_is_builder) {
                        // REGULAR PATTERN: push assigned value as context, run block
                        for (int tc = 0; tc < rhs->child_count; tc++) {
                            ASTNode* trailing = rhs->children[tc];
                            if (trailing && trailing->type == AST_CLOSURE &&
                                trailing->value && strcmp(trailing->value, "trailing") == 0) {
                                for (int bi = 0; bi < trailing->child_count; bi++) {
                                    if (trailing->children[bi] &&
                                        trailing->children[bi]->type == AST_BLOCK) {
                                        // Push the variable's value as builder context
                                        print_indent(gen);
                                        // For simple identifiers, use the variable name directly
                                        fprintf(gen->output, "_aether_ctx_push((void*)(intptr_t)");
                                        gen->generating_lvalue = 1;
                                        generate_expression(gen, lhs);
                                        gen->generating_lvalue = 0;
                                        fprintf(gen->output, ");\n");
                                        emit_trailing_block_body(gen, trailing->children[bi]);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            break;

        case AST_COMPOUND_ASSIGNMENT: {
            // node->value = variable name, children[0] = operator literal, children[1] = RHS
            if (stmt->child_count >= 2 && stmt->value && stmt->children[0] && stmt->children[0]->value) {
                const char* op = stmt->children[0]->value;  // "+=", "-=", etc.

                // Check if this is a state variable in an actor
                int is_state_var = 0;
                if (gen->current_actor && stmt->value) {
                    for (int i = 0; i < gen->state_var_count; i++) {
                        if (strcmp(stmt->value, gen->actor_state_vars[i]) == 0) {
                            is_state_var = 1;
                            break;
                        }
                    }
                }

                /* #2428: `v op= x` on an eight-lane value is `v = helper(v, x)`
                 * (the halves form is a struct; see codegen_expr.c). The
                 * typechecker records the target's type on the node for this. */
                TypeKind tk8 = stmt->node_type ? stmt->node_type->kind : TYPE_UNKNOWN;
                const char* fn8 = NULL;
                if ((tk8 == TYPE_F32X8 || tk8 == TYPE_I32X8) && op[0] && op[1] == '=') {
                    switch (op[0]) {
                        case '+': fn8 = "add"; break;
                        case '-': fn8 = "sub"; break;
                        case '*': fn8 = "mul"; break;
                        case '/': fn8 = "div"; break;
                        case '%': fn8 = "mod"; break;
                        default: break;
                    }
                }
                /* The place written. A variable a closure captures by
                 * reference lives in a shared cell, and its C name is the
                 * cell's POINTER (`int* x`), in the closure body and in the
                 * declaring function alike: plain assignment writes `*x`.
                 * `x += 1` was emitted as is, which is pointer arithmetic on
                 * the cell, so the value never changed and a later `*x`
                 * read the wrong memory, with no diagnostic. */
                const char* target = stmt->value;
                if (is_state_var)
                    target = cg_internf("self->%s", stmt->value);
                else if (is_promoted_capture(gen, stmt->value))
                    target = cg_internf("(*%s)", stmt->value);
                if (fn8) {
                    const char* pfx = tk8 == TYPE_F32X8 ? "f32x8" : "i32x8";
                    Type* rt = stmt->children[1]->node_type;
                    fprintf(gen->output, "%s = _ae_%s_%s(%s, ", target, pfx, fn8, target);
                    if (rt && rt->kind == tk8) {
                        generate_expression(gen, stmt->children[1]);
                    } else {
                        fprintf(gen->output, "_ae_%s_splat((%s)(", pfx, tk8 == TYPE_F32X8 ? "float" : "int");
                        generate_expression(gen, stmt->children[1]);
                        fprintf(gen->output, "))");
                    }
                    fprintf(gen->output, ");\n");
                    break;
                }
                /* A scalar into a four-lane (or two-lane) value splats once it
                 * is the element type, as for the binary operator
                 * (codegen_expr.c): `v *= s` with a float variable `s` beside
                 * an f32x4 is a truncating conversion C refuses. */
                Type* rt4 = stmt->children[1]->node_type;
                const char* elem4 = lane_element_c_type(tk8);
                if (elem4 && rt4 && is_scalar_splat_kind(rt4->kind)) {
                    fprintf(gen->output, "%s %s ((%s)(", target, op, elem4);
                    generate_expression(gen, stmt->children[1]);
                    fprintf(gen->output, "));\n");
                    break;
                }
                fprintf(gen->output, "%s %s ", target, op);
                generate_expression(gen, stmt->children[1]);
                fprintf(gen->output, ";\n");
            }
            break;
        }

        case AST_IF_STATEMENT:
            // Hoist any variable that's first-assigned in BOTH branches to
            // the outer scope before opening the if. Without this, the
            // C-side declarations stay block-local and disappear at the
            // closing `}`, even though Aether semantics expect them to
            // survive the merge. See docs/notes/compiler_notes_from_vcr_port.md
            // item #2.
            hoist_common_vars(gen, stmt);

            /* #2798: a condition holding a construct that runs statements
             * and struct results it keeps in temporaries, evaluated ahead. */
            const char* if_cv = NULL;
            if (g_ctl_deferred == stmt && stmt->child_count > 0) {
                g_ctl_deferred = NULL;
                if_cv = emit_condition_ahead(gen, stmt->children[0]);
                print_indent(gen);
            }
            fprintf(gen->output, "if (");
            if (if_cv) fprintf(gen->output, "%s", if_cv);
            else if (stmt->child_count > 0) emit_condition(gen, stmt->children[0]);   /* #2582 */
            fprintf(gen->output, ") {\n");

            {
                // Save declared_var_count before if-body.  Variables declared
                // inside if/else blocks live in separate C scopes and must not
                // leak to sibling statements (fixes Issue #2: sibling if blocks
                // re-using the same variable name).
                int saved_var_count = gen->declared_var_count;

                indent(gen);
                if (stmt->child_count > 1) {
                    generate_statement(gen, stmt->children[1]);
                }
                unindent(gen);

                if (stmt->child_count > 2) {
                    // Restore: else-branch sees only pre-if declarations.
                    truncate_declared_vars(gen, saved_var_count);

                    /* Anchor the `} else {` line. Without a directive here
                     * it inherits the then-branch's drifting count and lands
                     * on the line of the ELSE BODY, so gcov charges the
                     * branch-decision counter to a statement that never ran
                     * and an untaken branch reports as covered. */
                    codegen_maybe_emit_line(gen, stmt->children[2]);
                    print_line(gen, "} else {");
                    indent(gen);
                    generate_statement(gen, stmt->children[2]);
                    unindent(gen);
                }

                // Restore after entire if/else: variables declared inside
                // if/else blocks do not leak to subsequent sibling statements.
                truncate_declared_vars(gen, saved_var_count);
            }

            print_line(gen, "}");
            break;
            
        case AST_FOR_LOOP: {
            /* #2698: the variable the init declares is the loop's, declared
             * for its condition, increment and body, so a binding of its name
             * in the body assigns it. Not recorded, `i = i + 1` in the body
             * declared a new `int i` there that shadowed it, and a loop that
             * steps its variable in the body never ended. */
            int saved_var_count = gen->declared_var_count;
            /* #2737: a loop variable a closure in the body writes is a
             * promoted capture, read and written through its cell (`*i`) by
             * the condition, the increment, the body and the closure. It is
             * declared as any other promoted local is, ahead of the C `for`,
             * in a block of its own that ends with the loop: one cell for
             * every iteration, as `i = 0` before a while gives, released
             * after the loop (a closure that outlives it holds a reference of
             * its own). The defer scope it opens sits outside the loop's, so
             * a `break` or `continue` leaves the cell alone, and a `return`,
             * or a labeled exit of an outer loop, releases it. Declared in
             * the header as `int i`, the uses dereferenced an int. */
            ASTNode* for_init = stmt->child_count > 0 ? stmt->children[0] : NULL;
            const char* loop_var = for_init && for_init->type == AST_VARIABLE_DECLARATION
                                   ? for_init->value : NULL;
            int cell_init = loop_var && is_promoted_capture(gen, loop_var);
            /* #2742: the loop's variable is owned as the binding a while-loop
             * rewrite makes, in the same block of its own:
             *  - a string: the loop declares its own `s` and `_heap_s`,
             *    shadowing any local of the name as the C `for` declaration
             *    did, binds the init through the statement path, which takes
             *    the value as any owning binding does, and frees the value
             *    the loop ends with when the block ends;
             *  - any other variable no enclosing binding declares yet: the
             *    init is the statement that declares it, with whatever its
             *    kind needs (a struct that owns strings is destroyed when the
             *    block ends).
             * Those defers sit outside the loop's scope, so a `break` or
             * `continue` keeps the value, and a `return`, or a labeled exit
             * of an outer loop, frees it. Declared in the header, the
             * variable owned nothing: the value it ended with was never
             * freed. Any other loop variable keeps the header declaration:
             * one that shadows an enclosing local or a module `var` (the
             * statement would assign those), and an actor's state field,
             * which a handler reads and writes as `self->name`. */
            int state_var = loop_var && is_actor_state_var(gen, loop_var);
            int str_init = !cell_init && loop_var && !state_var && for_init->node_type &&
                           for_init->node_type->kind == TYPE_STRING;
            int stmt_init = !cell_init && !str_init && loop_var && !state_var &&
                            !is_var_declared(gen, loop_var) &&
                            !is_module_global_var(gen, loop_var);
            ScopedStringLocal str_local = {0};
            int own_block = cell_init || str_init || stmt_init;
            if (own_block) {
                print_line(gen, "{");
                indent(gen);
                enter_scope(gen);
            }
            if (cell_init) {
                print_indent(gen);
                emit_promoted_cell_declaration(gen, loop_var,
                    get_c_type(for_init->node_type), for_init->node_type,
                    for_init->child_count > 0 ? for_init->children[0] : NULL, "0",
                    for_init->line, for_init->column);
            } else if (str_init) {
                emit_string_local_decl(gen, loop_var);
                scoped_string_local_begin(gen, &str_local, loop_var, stmt, 1);
                push_scoped_string_local_free(gen, &str_local, for_init->line, for_init->column);
                generate_statement(gen, for_init);
            } else if (stmt_init) {
                generate_statement(gen, for_init);
            }
            /* #2742: an increment that binds a variable (`s = f(s)`, `i += 2`)
             * is the statement the parser made of it, emitted as the end of
             * each pass, where a while-loop rewrite binds it, so it takes the
             * ownership any binding does (a string it replaces is freed, a
             * cell is stored through). A `continue` jumps to it (see
             * loop_step_in_body). In the header it was a raw C assignment. */
            ASTNode* step = stmt->child_count > 2 ? stmt->children[2] : NULL;
            int step_stmt = step && (step->type == AST_VARIABLE_DECLARATION ||
                                     step->type == AST_COMPOUND_ASSIGNMENT);
            /* #2798: an increment expression holding a construct that runs
             * statements is emitted where a step statement is, with its
             * constructs ahead of it. */
            int step_ctl = step && !step_stmt && hoist_ready(gen) && ctl_contains(gen, step);
            /* #2798: so are the constructs in an init the header keeps, ahead
             * of the loop: nothing in it is evaluated before the init. */
            if (!own_block && for_init && hoist_ready(gen)) {
                ASTNode* iv = for_init;
                if (iv->type == AST_VARIABLE_DECLARATION) {
                    iv = iv->child_count > 0 ? iv->children[0] : NULL;
                } else if (iv->type == AST_BINARY_EXPRESSION && iv->value &&
                           strcmp(iv->value, "=") == 0 && iv->child_count == 2 &&
                           iv->children[0] && iv->children[0]->type == AST_IDENTIFIER) {
                    iv = iv->children[1];
                }
                if (iv && ctl_contains(gen, iv)) {
                    ctl_linearize_value(gen, iv, 0);
                    print_indent(gen);
                }
            }
            if (own_block) print_indent(gen);
            fprintf(gen->output, "for (");
            if (!own_block && for_init) {
                ASTNode* init = for_init;
                if (init->type == AST_VARIABLE_DECLARATION) {
                    generate_type(gen, init->node_type);
                    fprintf(gen->output, " %s", init->value);
                    if (init->child_count > 0) {
                        fprintf(gen->output, " = ");
                        generate_expression(gen, init->children[0]);
                    }
                    if (init->value) mark_var_declared(gen, init->value);
                } else {
                    generate_expression(gen, init);
                }
            }
            fprintf(gen->output, "; ");
            /* #2798: a condition holding a construct that runs statements is
             * evaluated at the top of the body, each pass, after the step. */
            int for_cond_in_body = stmt->child_count > 1 &&
                                   loop_condition_in_body(gen, stmt->children[1]);
            if (stmt->child_count > 1 && stmt->children[1] && !for_cond_in_body) {
                generate_expression(gen, stmt->children[1]); // condition
            }
            // Note: If no condition, C for loop becomes infinite (for (;;))
            fprintf(gen->output, "; ");
            if (step && !step_stmt && !step_ctl) {
                generate_expression(gen, step); // increment
            }
            fprintf(gen->output, ") {\n");
            
            indent(gen);
            if (gen->preempt_loops) {
                print_line(gen, "if (--_aether_reductions <= 0) { _aether_reductions = 10000; sched_yield(); }");
            }
            // Issue #343 codegen tripwire: under --emit=lib, emit a
            // deadline check at every loop head. The check is one
            // TLS read + one atomic load + branch; clock_gettime
            // only when the deadline is armed. Zero cost on
            // --emit=exe builds (the if (gen->emit_lib) gate elides
            // the print entirely).
            if (gen->emit_lib) {
                print_line(gen, "if (aether_caps_armed && aether_caps_deadline_tripped()) { __aether_abort_call(); break; }");
            }
            /* Issue #501: snapshot try_frame_depth at loop entry.
             * #893: record the loop's label / containing scope / C-label id. */
            int for_lbl_idx = -1;
            if (gen->loop_nest_depth < AETHER_MAX_LOOP_NEST) {
                for_lbl_idx = gen->loop_nest_depth;
                gen->loop_try_base[for_lbl_idx] = gen->try_frame_depth;
                gen->loop_label[for_lbl_idx] = stmt->value;
                gen->loop_label_scope[for_lbl_idx] = gen->scope_depth;
                gen->loop_label_id[for_lbl_idx] = gen->next_loop_label_id++;
                gen->loop_label_break_used[for_lbl_idx] = 0;
                gen->loop_label_continue_used[for_lbl_idx] = 0;
                gen->loop_step_in_body[for_lbl_idx] = step_stmt || step_ctl;
                gen->loop_c_depth[for_lbl_idx] = gen->c_break_depth + 1;
                gen->loop_nest_depth++;
            }
            gen->c_break_depth++;
            if (for_cond_in_body) emit_loop_condition_in_body(gen, stmt->children[1]);
            if (stmt->child_count > 3 && stmt->children[3]) {
                // Body is always a statement (could be a block or single statement)
                generate_statement(gen, stmt->children[3]); // body
            }
            gen->c_break_depth--;
            if (gen->loop_nest_depth > 0) gen->loop_nest_depth--;
            /* #893: the continue target, end of the body, so falling through
             * runs the step (the C `for`'s increment, or the step statement
             * below, #2742) then re-tests the condition. */
            if (for_lbl_idx >= 0 && gen->loop_label_continue_used[for_lbl_idx]) {
                print_line(gen, "__ae_cont_%d: ;", gen->loop_label_id[for_lbl_idx]);
            }
            if (step_stmt) {
                generate_statement(gen, step);
            } else if (step_ctl) {
                int ctl_depth = control_prelude_depth();
                int order_depth = order_prelude_depth();
                ctl_linearize_value(gen, step, 1);
                print_indent(gen);
                /* Its value is thrown away, and said so (-Wunused-value). */
                fprintf(gen->output, "(void)(");
                generate_expression(gen, step);
                fprintf(gen->output, ");\n");
                control_prelude_end(ctl_depth);
                order_prelude_end(order_depth);
            }
            unindent(gen);

            print_line(gen, "}");
            /* #893: the break target, after the loop, for a labeled break or
             * one a C `break` cannot express (#2696). */
            if (for_lbl_idx >= 0 && gen->loop_label_break_used[for_lbl_idx]) {
                print_line(gen, "__ae_brk_%d: ;", gen->loop_label_id[for_lbl_idx]);
            }
            if (own_block) {
                /* #2737: the loop's reference to its cell; #2742: what the
                 * value the loop ends with owns. */
                exit_scope(gen);
                unindent(gen);
                print_line(gen, "}");
            }
            if (str_init) scoped_string_local_end(gen, &str_local);
            truncate_declared_vars(gen, saved_var_count);
            break;
        }

        case AST_WHILE_LOOP: {
            // OPTIMIZATION: Try to collapse arithmetic series loops into O(1) expressions.
            // Only attempt when not inside actors and no sends (sends need batch treatment).
            int has_sends = contains_send_expression(stmt);
            if (!has_sends && try_emit_series_collapse(gen, stmt)) {
                break;  // collapsed — done
            }

            // Batch optimization: only in main() (not inside actors)
            // Uses queue_enqueue_batch to reduce atomics from N to num_cores
            if (has_sends && gen->current_actor == NULL) {
                print_line(gen, "scheduler_send_batch_start();");
                gen->in_main_loop = 1;
            }

            // Hoist variable declarations from loop body to function scope
            // so they're visible to subsequent while blocks
            if (stmt->child_count > 1) {
                hoist_loop_vars(gen, stmt->children[1]);
            }

            /* #2378: an interpreter dispatch loop gets a threaded-dispatch
             * table (see td_loop_switch). Not under batched sends, whose loop
             * shape is managed separately. */
            ASTNode* td_sw = NULL;
            int td_id = 0;
            if (!has_sends && gen->loop_nest_depth < AETHER_MAX_LOOP_NEST) {
                td_sw = td_loop_switch(gen, stmt);
                if (td_sw) {
                    td_id = ++gen->next_td_id;
                    td_emit_table(gen, td_sw, td_id);
                }
            }

            if (td_id) print_indent(gen);   /* the table ended on a fresh line */
            /* #2798: a condition holding a construct that runs statements is
             * evaluated at the top of the body, each pass. */
            int while_cond_in_body = stmt->child_count > 0 &&
                                     loop_condition_in_body(gen, stmt->children[0]);
            fprintf(gen->output, "while (");
            if (while_cond_in_body) fprintf(gen->output, "1");
            else if (stmt->child_count > 0) emit_condition(gen, stmt->children[0]);   /* #2582 */
            fprintf(gen->output, ") {\n");

            indent(gen);
            // Cooperative preemption: yield to OS at loop back-edges
            if (gen->preempt_loops) {
                print_line(gen, "if (--_aether_reductions <= 0) { _aether_reductions = 10000; sched_yield(); }");
            }
            // Issue #343 codegen tripwire — see AST_FOR_LOOP comment.
            if (gen->emit_lib) {
                print_line(gen, "if (aether_caps_armed && aether_caps_deadline_tripped()) { __aether_abort_call(); break; }");
            }
            /* Issue #501: snapshot try_frame_depth at loop entry so
             * `break` / `continue` inside the body drains only
             * frames pushed *inside* the loop, not outer-scope tries.
             * #893: record the loop's label / containing scope / a fresh
             * C-label id in the same slot. */
            int while_lbl_idx = -1;
            if (gen->loop_nest_depth < AETHER_MAX_LOOP_NEST) {
                while_lbl_idx = gen->loop_nest_depth;
                gen->loop_try_base[while_lbl_idx] = gen->try_frame_depth;
                gen->loop_label[while_lbl_idx] = stmt->value;
                gen->loop_label_scope[while_lbl_idx] = gen->scope_depth;
                gen->loop_label_id[while_lbl_idx] = gen->next_loop_label_id++;
                gen->loop_label_break_used[while_lbl_idx] = 0;
                gen->loop_label_continue_used[while_lbl_idx] = 0;
                gen->loop_step_in_body[while_lbl_idx] = 0;
                gen->loop_td_id[while_lbl_idx] = td_id;
                gen->loop_td_node[while_lbl_idx] = td_id ? stmt : NULL;
                gen->loop_c_depth[while_lbl_idx] = gen->c_break_depth + 1;
                gen->loop_nest_depth++;
            }
            gen->c_break_depth++;
            ASTNode* saved_td_switch = gen->td_switch;
            int saved_td_id = gen->td_id;
            int saved_in_head = g_td_in_head;
            if (td_id) {
                gen->td_switch = td_sw;
                gen->td_id = td_id;
                g_td_in_head = 1;   /* cleared when the dispatch switch starts */
            }
            if (while_cond_in_body) emit_loop_condition_in_body(gen, stmt->children[0]);
            if (stmt->child_count > 1) {
                generate_statement(gen, stmt->children[1]);
            }
            gen->td_switch = saved_td_switch;
            gen->td_id = saved_td_id;
            g_td_in_head = saved_in_head;
            /* The slot is reused by the next loop at this depth, whatever
             * kind it is; a stale id would thread that loop's continues. */
            if (while_lbl_idx >= 0) {
                gen->loop_td_id[while_lbl_idx] = 0;
                gen->loop_td_node[while_lbl_idx] = NULL;
            }
            gen->c_break_depth--;
            if (gen->loop_nest_depth > 0) gen->loop_nest_depth--;
            /* #893: labeled-continue target — end of the loop body, after the
             * body's scope-exit defers, so the loop re-tests the condition. */
            if (while_lbl_idx >= 0 && gen->loop_label[while_lbl_idx] &&
                gen->loop_label_continue_used[while_lbl_idx]) {
                print_line(gen, "__ae_cont_%d: ;", gen->loop_label_id[while_lbl_idx]);
            }
            unindent(gen);

            print_line(gen, "}");

            /* #893: the break target, after the loop, for a labeled break or
             * one a C `break` cannot express (#2696). */
            if (while_lbl_idx >= 0 && gen->loop_label_break_used[while_lbl_idx]) {
                print_line(gen, "__ae_brk_%d: ;", gen->loop_label_id[while_lbl_idx]);
            }
            /* #2378: where a threaded continue lands when the --emit=lib
             * deadline trips (the loop head's own check uses `break`). */
            if (td_id && gen->emit_lib) {
                print_line(gen, AE_TD_GUARD);
                print_line(gen, "_ae_td_exit_%d: ;", td_id);
                print_line(gen, "#endif");
            }

            if (has_sends && gen->current_actor == NULL) {
                print_line(gen, "scheduler_send_batch_flush();");
                gen->in_main_loop = 0;
            }
            break;
        }
            
        case AST_MATCH_STATEMENT:
            // Generate match as a series of if-else statements
            // match (x) { 1 -> a, 2 -> b, _ -> c }
            // becomes: { T _match_val = x; if (_match_val == 1) { a; } else if ... }
            // Using a temp variable avoids re-evaluating the match expression per arm.
            /* #2768: the names every arm of a statement match binds, when
             * an arm runs for every value, are declared before it, as both
             * arms of an if/else are. A match whose value is used (it has a
             * result variable) keeps its arms' names in the arms, as the
             * checker does. */
            if (!gen->match_result_var) hoist_common_vars(gen, stmt);
            if (stmt->child_count > 0) {
                ASTNode* match_expr = stmt->children[0];

                // #340: optional `match` — `match m { none -> A  some(v) -> B }`
                // lowers to a presence test on the tagged struct. Handled
                // before the generic numeric / list / seq match lowering.
                if (match_expr->node_type && match_expr->node_type->kind == TYPE_OPTIONAL) {
                    Type* inner = match_expr->node_type->element_type;
                    const char* inner_c = inner ? get_c_type(inner) : "int";
                    const char* oc = get_c_type(match_expr->node_type);
                    static int om_counter = 0;
                    int id = om_counter++;
                    ASTNode *none_body = NULL, *some_body = NULL, *wild_body = NULL;
                    const char* some_bind = NULL;
                    /* Arms are tried in source order: the first arm for each
                     * case is the one that runs, and none runs after a `_`
                     * (#2728; a repeated or late arm took over). */
                    for (int i = 1; i < stmt->child_count && !wild_body; i++) {
                        ASTNode* arm = stmt->children[i];
                        if (!arm || arm->type != AST_MATCH_ARM || arm->child_count < 2) continue;
                        ASTNode* pat = arm->children[0];
                        ASTNode* body = arm->children[1];
                        if (pat->type == AST_NONE_LITERAL) {
                            if (!none_body) none_body = body;
                        } else if (pat->type == AST_PATTERN_VARIABLE && pat->annotation &&
                                 strcmp(pat->annotation, "some_pattern") == 0) {
                            if (!some_body) { some_body = body; some_bind = pat->value; }
                        } else if (pat->node_type && pat->node_type->kind == TYPE_WILDCARD) {
                            wild_body = body;
                        } else if (pat->type == AST_IDENTIFIER && pat->value &&
                                   strcmp(pat->value, "_") == 0) {
                            wild_body = body;
                        }
                    }
                    print_line(gen, "{");
                    indent(gen);
                    print_indent(gen);
                    fprintf(gen->output, "%s _om%d = ", oc, id);
                    generate_expression(gen, match_expr);
                    fprintf(gen->output, ";\n");
                    print_indent(gen);
                    fprintf(gen->output, "if (!_om%d.has) {\n", id);
                    indent(gen);
                    emit_opt_match_arm(gen, none_body ? none_body : wild_body);
                    unindent(gen);
                    print_line(gen, "} else {");
                    indent(gen);
                    /* #2795: a `string?` closure cell gives its string back
                     * at its next store, which the arm may run (a closure
                     * call that re-binds it), so the `some` binding is a
                     * copy the arm owns, as below. So does a `string?`
                     * local, at its re-bind or its scope exit, which a
                     * `return s` in the arm runs before the caller reads
                     * the binding (#2815). A module var keeps every string
                     * it held, so its binding stays a view. */
                    int holder_copy = !is_owned_opt_string_call(gen, match_expr) && some_bind &&
                                      (is_owning_opt_string_holder(gen, match_expr) ||
                                       (match_expr->type == AST_IDENTIFIER && match_expr->value &&
                                        is_opt_string_type(match_expr->node_type) &&
                                        is_var_declared(gen, match_expr->value) &&
                                        !is_module_global_var(gen, match_expr->value)));
                    if (is_owned_opt_string_call(gen, match_expr) || holder_copy) {
                        /* #2750: the string of a `string?` an Aether function
                         * handed over is the match's, and nothing else frees
                         * it: the `some` binding (or, with none, a hidden
                         * one) owns it for the arm, as a local bound to a
                         * heap string does, freed when the arm ends unless
                         * the arm hands it on (returns it, stores it, binds
                         * a local to it). */
                        ASTNode* arm_body = some_body ? some_body : wild_body;
                        const char* own = some_bind ? some_bind : cg_internf("_om%d_s", id);
                        int saved_var_count = gen->declared_var_count;
                        print_indent(gen);
                        fprintf(gen->output, "%s %s = %s_om%d.val%s;\n",
                                volatile_decl_type(try_volatile_qual_for(gen, own), "const char*"),
                                own, holder_copy ? "aether_uniform_heap_str(" : "", id,
                                holder_copy ? ", 0)" : "");
                        /* The binding is a local of the arm: a `v = ...` there
                         * rebinds it, and no hoist declares it again. */
                        if (some_bind) mark_var_declared_typed(gen, some_bind, inner);
                        ScopedStringLocal sl;
                        scoped_string_local_begin(gen, &sl, own, arm_body, 1);
                        print_line(gen, "{");
                        indent(gen);
                        enter_scope(gen);
                        push_scoped_string_local_free(gen, &sl, stmt->line, stmt->column);
                        emit_opt_match_arm(gen, arm_body);
                        exit_scope(gen);
                        unindent(gen);
                        print_line(gen, "}");
                        truncate_declared_vars(gen, saved_var_count);
                        scoped_string_local_end(gen, &sl);
                        unindent(gen);
                        print_line(gen, "}");
                        unindent(gen);
                        print_line(gen, "}");
                        break;
                    }
                    int saved_some_vars = gen->declared_var_count;
                    if (some_bind) {
                        print_indent(gen);
                        fprintf(gen->output, "%s %s = _om%d.val;\n", inner_c, some_bind, id);
                        /* A local of the arm, declared here: a `v = ...` in
                         * the arm rebinds it, and no hoist in the arm declares
                         * it again in the same C block ("redefinition"). */
                        mark_var_declared_typed(gen, some_bind, inner);
                    }
                    emit_opt_match_arm(gen, some_body ? some_body : wild_body);
                    truncate_declared_vars(gen, saved_some_vars);
                    unindent(gen);
                    print_line(gen, "}");
                    unindent(gen);
                    print_line(gen, "}");
                    break;
                }

                // #914 sum `match` — switch on the tag enum, narrowing the
                // matched value to each variant struct inside its case so
                // `s.field` reads the right union member. Handled before the
                // generic numeric / list / seq match lowering.
                if (match_expr->node_type && match_expr->node_type->kind == TYPE_SUM) {
                    Type* st = match_expr->node_type;
                    const char* sname = st->struct_name ? st->struct_name : "_sum";
                    static int sm_counter = 0;
                    int id = sm_counter++;
                    // The scrutinee is narrowed in each arm only when it is a
                    // plain variable (so `s.field` has a name to bind to).
                    const char* scrut = (match_expr->type == AST_IDENTIFIER)
                                        ? match_expr->value : NULL;
                    print_line(gen, "{");
                    indent(gen);
                    print_indent(gen);
                    fprintf(gen->output, "%s _sm%d = ", sname, id);
                    generate_expression(gen, match_expr);
                    fprintf(gen->output, ";\n");
                    print_indent(gen);
                    fprintf(gen->output, "switch (_sm%d.tag) {\n", id);
                    indent(gen);
                    /* #2696: a `break` in an arm leaves the loop or switch
                     * around the match, not this C switch. */
                    gen->c_break_depth++;
                    /* Arms are tried in source order, and a C switch tries
                     * them by value: a variant's first arm is its case, and
                     * an arm after `_` never runs (#2728). Emitting them made
                     * a duplicate `case`, which C rejects, or let a variant
                     * below `_` take its value back. */
                    const char* seen_variants[256];
                    int nseen = 0;
                    int after_wild = 0;
                    for (int i = 1; i < stmt->child_count && !after_wild; i++) {
                        ASTNode* arm = stmt->children[i];
                        if (!arm || arm->type != AST_MATCH_ARM || arm->child_count < 2) continue;
                        ASTNode* pat = arm->children[0];
                        ASTNode* body = arm->children[1];
                        int is_wild =
                            (pat->node_type && pat->node_type->kind == TYPE_WILDCARD) ||
                            (pat->type == AST_IDENTIFIER && pat->value &&
                             strcmp(pat->value, "_") == 0);
                        // A bare variant pattern (`Circle ->`) arrives as an
                        // AST_IDENTIFIER (parse_expression); accept that and
                        // the AST_PATTERN_VARIABLE form.
                        const char* variant = NULL;
                        if (!is_wild && pat->value &&
                            (pat->type == AST_IDENTIFIER ||
                             pat->type == AST_PATTERN_VARIABLE))
                            variant = pat->value;
                        if (variant) {
                            int dup = 0;
                            for (int s = 0; s < nseen; s++)
                                if (strcmp(seen_variants[s], variant) == 0) { dup = 1; break; }
                            if (dup) continue;
                            if (nseen < 256) seen_variants[nseen++] = variant;
                        }
                        if (is_wild) after_wild = 1;
                        print_indent(gen);
                        if (is_wild) {
                            fprintf(gen->output, "default: {\n");
                        } else if (variant) {
                            fprintf(gen->output, "case %s__%s: {\n", sname, variant);
                        } else {
                            continue;   // unrecognised arm — typechecker flagged it
                        }
                        indent(gen);
                        if (variant && scrut) {
                            print_indent(gen);
                            fprintf(gen->output, "%s %s = _sm%d.data.%s_; (void)%s;\n",
                                    variant, scrut, id, variant, scrut);
                        }
                        emit_opt_match_arm(gen, body);
                        print_line(gen, "break;");
                        unindent(gen);
                        print_line(gen, "}");
                    }
                    gen->c_break_depth--;
                    unindent(gen);
                    print_line(gen, "}");
                    unindent(gen);
                    print_line(gen, "}");
                    break;
                }

                // Check if any arm uses list patterns
                int uses_list_patterns = has_list_patterns(stmt);
                /* When the matched expression is *StringSeq, the
                 * "length variable" is actually a pointer to the
                 * cons cell — we walk it via NULL-checks and
                 * head/tail dereferences instead of array
                 * slicing. The flag below toggles between the
                 * two lowerings end-to-end. See
                 * std/collections/aether_stringseq.h for the
                 * cell layout. */
                int is_seq_match = uses_list_patterns &&
                    is_string_seq_ptr_type(match_expr->node_type);
                char len_name[64] = "_match_len";
                ValueTemps subject_temps;
                int has_subject_temps = 0;

                // Wrap match in a block and store the match expression in a temp
                // to avoid evaluating it multiple times (could have side effects).
                print_line(gen, "{");
                indent(gen);

                // If using list patterns, generate length variable for conditions
                if (is_seq_match) {
                    snprintf(len_name, sizeof(len_name), "_match_seq");
                    print_indent(gen);
                    fprintf(gen->output, "StringSeq* %s = ", len_name);
                    generate_expression(gen, match_expr);
                    fprintf(gen->output, ";\n");
                } else if (uses_list_patterns && type_is_slice(match_expr->node_type)) {
                    /* #1286: a slice carries its own length. */
                    print_indent(gen);
                    fprintf(gen->output, "int %s = (int)aether_slice_len(", len_name);
                    generate_expression(gen, match_expr);
                    fprintf(gen->output, ");\n");
                } else if (uses_list_patterns) {
                    print_indent(gen);
                    fprintf(gen->output, "int %s = ", len_name);
                    generate_expression(gen, match_expr);
                    fprintf(gen->output, "_len;\n");
                } else {
                    // Emit temp variable for the match expression value, of
                    // the type a switch's lowered temp has.
                    const char* match_c_type = scrutinee_c_type(match_expr);
                    /* #2582: a struct a call in the subject returns lives
                     * until the match is done, since the subject (a string
                     * field of it, say) is read by every arm. */
                    has_subject_temps = value_temps_open_expr(gen, match_expr, TEMP_KEPT,
                                                              &subject_temps);
                    stmt_control_prelude_deferred(gen, stmt);   /* #2798 */
                    print_indent(gen);
                    fprintf(gen->output, "%s _match_val = ", match_c_type);
                    generate_expression(gen, match_expr);
                    fprintf(gen->output, ";\n");
                    if (has_subject_temps) temps_unmap(&subject_temps);
                }

                for (int i = 1; i < stmt->child_count; i++) {
                    ASTNode* match_arm = stmt->children[i];
                    if (!match_arm || match_arm->type != AST_MATCH_ARM || match_arm->child_count < 2) continue;

                    ASTNode* pattern = match_arm->children[0];
                    ASTNode* result = match_arm->children[1];

                    // Check if wildcard pattern. The string literal "_" is
                    // a string to compare with, not the wildcard.
                    int is_wildcard = (pattern->type == AST_IDENTIFIER &&
                                      pattern->value &&
                                      strcmp(pattern->value, "_") == 0) ||
                                     (pattern->node_type &&
                                      pattern->node_type->kind == TYPE_WILDCARD);

                    // Check if list pattern
                    int is_list_pattern = (pattern->type == AST_PATTERN_LIST ||
                                          pattern->type == AST_PATTERN_CONS);

                    if (is_wildcard) {
                        // else clause
                        if (i > 1) {
                            print_indent(gen);
                            fprintf(gen->output, "else {\n");
                        } else {
                            print_indent(gen);
                            fprintf(gen->output, "{\n");
                        }
                    } else if (is_list_pattern) {
                        // List pattern clause
                        if (i > 1) {
                            print_indent(gen);
                            fprintf(gen->output, "else if (");
                        } else {
                            print_indent(gen);
                            fprintf(gen->output, "if (");
                        }
                        generate_list_pattern_condition(gen, pattern, len_name, is_seq_match);
                        fprintf(gen->output, ") {\n");
                    } else {
                        // Regular literal/expression pattern
                        if (i > 1) {
                            print_indent(gen);
                            fprintf(gen->output, "else if (");
                        } else {
                            print_indent(gen);
                            fprintf(gen->output, "if (");
                        }
                        // Use _match_val (temp) instead of re-evaluating
                        // match_expr. emit_selector_condition (#1047) handles a
                        // single value, an inclusive/half-open range, or a
                        // comma-list. For strings it uses string_equals (NULL
                        // -safe, magic-aware: _match_val may be a magic
                        // AetherString, so a raw strcmp would compare the struct
                        // header; the NULL guard makes a NULL scrutinee match no
                        // pattern).
                        Type* mexpr_type = match_expr->node_type;
                        int mexpr_is_string = mexpr_type && mexpr_type->kind == TYPE_STRING;
                        emit_selector_condition(gen, pattern, "_match_val", mexpr_is_string);
                        fprintf(gen->output, ") {\n");
                    }

                    indent(gen);

                    // Generate list pattern bindings if needed. They are the
                    // arm's own: a binding the arm rebinds is released as the
                    // arm ends (own_rebound_binding_take).
                    int saved_arm_vars = gen->declared_var_count;
                    if (is_list_pattern) {
                        enter_scope(gen);
                        generate_list_pattern_bindings(gen, pattern, match_expr, len_name, result, is_seq_match);
                    }

                    if (result->type == AST_BLOCK) {
                        // Already a block, generate its statements
                        emit_match_arm_block(gen, result);
                    } else if (result->type == AST_PRINT_STATEMENT
                            || result->type == AST_RETURN_STATEMENT
                            || result->type == AST_VARIABLE_DECLARATION) {
                        // Statement-level node (e.g. print, return)
                        generate_statement(gen, result);
                    } else {
                        // Single expression — assign to result var or emit as statement
                        emit_match_result_value(gen, result);
                    }
                    if (is_list_pattern) exit_scope(gen);
                    truncate_declared_vars(gen, saved_arm_vars);
                    unindent(gen);
                    print_line(gen, "}");
                }

                if (has_subject_temps) temps_close(gen, &subject_temps, 1);
                // Close the match scoping block
                unindent(gen);
                print_line(gen, "}");
            }
            break;

        case AST_SWITCH_STATEMENT: {
            // #1047: a C `switch` can't express a ranged case (`case 1..=5:`).
            // Aether's switch has no fall-through (each case auto-breaks), so it
            // is semantically identical to an if-else chain. When any case is
            // ranged, lower the whole switch to an if-chain (comparing a temp
            // scrutinee via emit_selector_condition); plain / comma-list-only
            // switches keep the C `switch` for readable output.
            /* A C switch takes an integer; a string, a pointer or a float is
             * compared by the if-chain, as a match is (#2727). */
            int needs_chain = stmt->child_count > 0 && !switch_subject_is_integer(stmt->children[0]);
            for (int i = 1; i < stmt->child_count; i++) {
                ASTNode* c = stmt->children[i];
                if (c && c->type == AST_CASE_STATEMENT && c->child_count > 0 &&
                    !(c->value && strcmp(c->value, "default") == 0) &&
                    selector_has_range(c->children[0])) { needs_chain = 1; break; }
            }

            /* #2732: names every case of a switch with a default binds are
             * declared before it, as both arms of an if/else are. */
            hoist_common_vars(gen, stmt);

            /* #2696: an unlabeled `break` in a case leaves the switch, in
             * whichever form it lowers to. */
            int sw_lvl = switch_target_push(gen);

            if (!needs_chain && stmt == gen->td_switch && gen->td_id && stmt->child_count > 0) {
                /* #2378: the dispatch switch of a threaded loop. The selector
                 * goes through _ae_td_sel_N so a continue whose value has no
                 * table entry can jump back to the switch (_ae_td_sw_N)
                 * without re-running the head; each arm gets a label (see
                 * AST_CASE_STATEMENT). Without labels-as-values this is the
                 * plain switch of today. */
                int id = gen->td_id;
                gen->td_switch = NULL;   /* a nested switch is not the dispatch */
                g_td_in_head = 0;        /* the head is done; arms dispatch */
                fprintf(gen->output, "\n");
                print_line(gen, AE_TD_GUARD);
                print_indent(gen);
                fprintf(gen->output, "_ae_td_sel_%d = (int64_t)(", id);
                generate_expression(gen, stmt->children[0]);
                fprintf(gen->output, ");\n");
                print_line(gen, "_ae_td_sw_%d: ;", id);
                print_line(gen, "switch (_ae_td_sel_%d) {", id);
                print_line(gen, "#else");
                print_indent(gen);
                fprintf(gen->output, "switch (");
                generate_expression(gen, stmt->children[0]);
                fprintf(gen->output, ") {\n");
                print_line(gen, "#endif");
                indent(gen);
                gen->c_break_depth++;
                if (sw_lvl >= 0) gen->switch_c_depth[sw_lvl] = gen->c_break_depth;
                for (int i = 1; i < stmt->child_count; i++) {
                    gen->td_arm = i;
                    generate_statement(gen, stmt->children[i]);
                }
                gen->c_break_depth--;
                gen->td_arm = -1;
                unindent(gen);
                print_line(gen, "}");
                switch_target_pop(gen, sw_lvl);
                break;
            }

            if (!needs_chain) {
                int saved_td_arm = gen->td_arm;
                gen->td_arm = -1;
                fprintf(gen->output, "switch (");
                if (stmt->child_count > 0) generate_expression(gen, stmt->children[0]);
                fprintf(gen->output, ") {\n");
                indent(gen);
                gen->c_break_depth++;
                if (sw_lvl >= 0) gen->switch_c_depth[sw_lvl] = gen->c_break_depth;
                for (int i = 1; i < stmt->child_count; i++)
                    generate_statement(gen, stmt->children[i]);
                gen->c_break_depth--;
                unindent(gen);
                print_line(gen, "}");
                gen->td_arm = saved_td_arm;
                switch_target_pop(gen, sw_lvl);
                break;
            }

            // Ranged switch -> if-else chain. No C statement a `break` leaves:
            // one that leaves the switch jumps past the chain (#2696).
            ASTNode* sexpr = stmt->child_count > 0 ? stmt->children[0] : NULL;
            int is_str = sexpr && sexpr->node_type && sexpr->node_type->kind == TYPE_STRING;
            print_line(gen, "{");
            indent(gen);
            print_indent(gen);
            fprintf(gen->output, "%s _switch_val = ", scrutinee_c_type(sexpr));
            if (sexpr) generate_expression(gen, sexpr);
            else fprintf(gen->output, "0");
            fprintf(gen->output, ";\n");

            ASTNode* default_case = NULL;
            int emitted = 0;
            for (int i = 1; i < stmt->child_count; i++) {
                ASTNode* c = stmt->children[i];
                if (!c || c->type != AST_CASE_STATEMENT) continue;
                int is_default = (c->value && strcmp(c->value, "default") == 0);
                if (is_default) { default_case = c; continue; }
                print_indent(gen);
                fprintf(gen->output, emitted ? "else if (" : "if (");
                emit_selector_condition(gen, c->children[0], "_switch_val", is_str);
                fprintf(gen->output, ") {\n");
                indent(gen);
                emit_case_body(gen, c, 1);
                unindent(gen);
                print_line(gen, "}");
                emitted = 1;
            }
            if (default_case) {
                print_indent(gen);
                fprintf(gen->output, emitted ? "else {\n" : "{\n");
                indent(gen);
                emit_case_body(gen, default_case, 0);
                unindent(gen);
                print_line(gen, "}");
            }
            unindent(gen);
            print_line(gen, "}");
            switch_target_pop(gen, sw_lvl);
            break;
        }
            
        case AST_CASE_STATEMENT: {
            int is_default = (stmt->value && strcmp(stmt->value, "default") == 0);
            if (is_default) {
                print_line(gen, "default:");
            } else {
                // #1047: a comma-list case (`case 7, 8, 9:`) lowers to one C
                // `case` label per value, sharing the body (no fall-through in
                // between, Aether auto-breaks after the shared body). A ranged
                // case never reaches here (AST_SWITCH_STATEMENT diverts a switch
                // with any range to an if-chain).
                ASTNode* sel = stmt->child_count > 0 ? stmt->children[0] : NULL;
                if (sel && sel->type == AST_MATCH_ALT) {
                    for (int a = 0; a < sel->child_count; a++) {
                        fprintf(gen->output, "case ");
                        generate_expression(gen, sel->children[a]);
                        fprintf(gen->output, ":\n");
                    }
                } else {
                    fprintf(gen->output, "case ");
                    if (sel) generate_expression(gen, sel);
                    fprintf(gen->output, ":\n");
                }
            }

            /* #2378: an arm of a threaded loop's dispatch switch gets the
             * label its table entries point at. Consumed here, so a switch
             * nested in the arm's body is never labelled. */
            if (!is_default && gen->td_arm >= 0 && gen->td_id) {
                print_line(gen, AE_TD_GUARD);
                print_line(gen, "_ae_td_%d_arm%d: ;", gen->td_id, gen->td_arm);
                print_line(gen, "#endif");
            }
            gen->td_arm = -1;

            indent(gen);
            // Generate all statements in the case block (skip first child which is the case value)
            int start_idx = is_default ? 0 : 1;
            print_line(gen, "{");
            indent(gen);
            emit_case_body(gen, stmt, start_idx);
            unindent(gen);
            print_line(gen, "}");
            // Auto-insert `break` unless the case ends with its own
            // control-flow exit. Aether's `switch` has no
            // fallthrough — that's the surprising-default-everyone-
            // gets-wrong-once C inherited; we don't carry it forward.
            // (Empty `case 7: case 8: ...` chaining isn't supported
            // via this path — each case currently must have a body.
            // If we add empty-fallthrough later it'll be an explicit
            // syntax, not the silent default.)
            int needs_break = 1;
            int total = stmt->child_count;
            int last = total - 1;
            if (last >= start_idx) {
                ASTNode* tail_stmt = stmt->children[last];
                if (tail_stmt) {
                    ASTNodeType t = tail_stmt->type;
                    if (t == AST_RETURN_STATEMENT ||
                        t == AST_BREAK_STATEMENT ||
                        t == AST_CONTINUE_STATEMENT) {
                        needs_break = 0;
                    }
                }
            }
            if (needs_break) {
                print_line(gen, "break;");
            }
            unindent(gen);
            break;
        }
            
        case AST_RETURN_STATEMENT: {
            // #1054: `return match x { ... }`, a match in return position, is a
            // value-producing expression, not a statement. Lower it via the same
            // result-variable mechanism the working `v = match x { ... }` form
            // uses: declare a temp of the return type, run the match with each
            // arm assigning the temp, then re-dispatch as `return <temp>` so all
            // the return machinery (contracts, defers, escape drains) applies
            // uniformly. Without this, a bare match in return position emitted
            // `return;` (void) followed by an orphaned match whose arm bodies
            // were dead expression-statements, so the function returned garbage.
            if (stmt->child_count == 1 && stmt->children[0] &&
                stmt->children[0]->type == AST_MATCH_STATEMENT) {
                ASTNode* m = stmt->children[0];
                // Return type: the function's declared type, else infer from the
                // first arm's result (mirrors the `v = match` path above).
                Type* rt = gen->current_func_return_type;
                const char* ct = (rt && rt->kind != TYPE_VOID && rt->kind != TYPE_UNKNOWN)
                                 ? get_c_type(rt) : NULL;
                if (!ct && match_value_type(m)) ct = get_c_type(match_value_type(m));
                if (!ct) ct = "int";
                static int ret_match_ctr = 0;
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "_ret_match%d", ret_match_ctr++);
                /* #2461: a string the function returns is the caller's, so
                 * the arms take their values (a local of this function
                 * moved out, a field read retained) into a local the
                 * return then moves out. */
                int str_ret = strcmp(ct, "const char*") == 0;
                print_indent(gen);
                fprintf(gen->output, str_ret ? "%s %s = NULL;\n" : "%s %s;\n", ct, tmp);
                if (str_ret) mark_heap_string_var(gen, tmp);
                const char* saved = gen->match_result_var;
                int saved_string = gen->match_result_string;
                const char* saved_struct = gen->match_result_struct;
                int saved_replace = gen->match_result_replace;
                Type* saved_cell = gen->match_result_cell;
                Type* saved_opt = gen->match_result_opt;
                gen->match_result_cell = NULL;
                /* A match returned from an optional result: each arm's
                 * value wrapped into it. */
                gen->match_result_opt = (rt && rt->kind == TYPE_OPTIONAL &&
                                         !is_opt_string_type(rt)) ? rt : NULL;
                gen->match_result_var = tmp;
                gen->match_result_string = str_ret;
                /* #2497: the caller adopts a returned struct, so each arm hands
                 * over one this function no longer owns. */
                gen->match_result_struct = struct_owning_strings(gen, rt);
                gen->match_result_replace = 0;
                generate_statement(gen, m);
                gen->match_result_opt = saved_opt;
                gen->match_result_var = saved;
                gen->match_result_string = saved_string;
                gen->match_result_struct = saved_struct;
                gen->match_result_replace = saved_replace;
                gen->match_result_cell = saved_cell;
                // Re-dispatch as `return <tmp>` to reuse all return machinery.
                ASTNode* rid = create_ast_node(AST_IDENTIFIER, tmp, stmt->line, stmt->column);
                rid->node_type = rt ? clone_type(rt)
                                    : (m->node_type ? clone_type(m->node_type) : NULL);
                ASTNode* rstmt = create_ast_node(AST_RETURN_STATEMENT, NULL,
                                                 stmt->line, stmt->column);
                add_child(rstmt, rid);
                generate_statement(gen, rstmt);
                free_ast_node(rstmt);
                break;
            }
            // Issue #348 — postcondition checks. When the enclosing
            // function has any `ensures` clauses AND we're emitting
            // a single-value, non-main return AND --no-contracts is
            // off, route through a fresh C scope: assign the return
            // expression to a local `result`, run the checks, then
            // `return result`. Each return site gets its own copy of
            // every check; the C scope hides any outer `result`.
            //
            // Skip when the function is `main` (the existing
            // main_exit goto chain is fine — main has no callers
            // expecting postconditions) or when the return is
            // multi-value (tuple semantics for `result` aren't yet
            // defined; multi-value contracts are an out-of-scope
            // follow-up).
            if (!gen->in_main_function &&
                stmt->child_count == 1 &&
                gen->current_function &&
                function_has_ensures(gen->current_function) &&
                !gen->no_contracts) {
                Type* ret_type = stmt->children[0]->node_type;
                const char* ret_c_type =
                    (ret_type && ret_type->kind != TYPE_VOID && ret_type->kind != TYPE_UNKNOWN)
                    ? c_result_type(ret_type)
                    : (gen->current_func_return_type &&
                       gen->current_func_return_type->kind != TYPE_VOID &&
                       gen->current_func_return_type->kind != TYPE_UNKNOWN)
                        ? c_result_type(gen->current_func_return_type)
                        : "int";
                print_indent(gen);
                fprintf(gen->output, "{\n");
                gen->indent_level++;
                print_indent(gen);
                fprintf(gen->output, "%s result = ", ret_c_type);
                emit_return_value(gen, stmt);   // #340: coerces into `T?`
                fprintf(gen->output, ";\n");
                emit_contract_postconditions(gen, gen->current_function);
                /* Drain function-level defers BEFORE returning so
                 * cleanup happens between the postcondition check
                 * and the return — same ordering as the regular
                 * defer-aware return path further down. */
                if (gen->defer_count > 0) {
                    emit_all_defers(gen);
                }
                /* Issue #501: drain in-flight try frames before
                 * returning, so a `return` inside a try body doesn't
                 * leak the panic frame. */
                emit_try_pops_for_nonlocal_exit(gen);
                print_indent(gen);
                fprintf(gen->output, "return result;\n");
                gen->indent_level--;
                print_indent(gen);
                fprintf(gen->output, "}\n");
                break;
            }
            // In main(), all returns go through main_exit so scheduler_wait() always runs
            if (gen->in_main_function) {
                if (gen->defer_count > 0) {
                    emit_all_defers(gen);
                }
                /* Issue #501: drain in-flight try frames before
                 * the goto so a `return` inside a try body in
                 * main() doesn't leak the panic frame.  goto and
                 * return are both non-local exits as far as the
                 * try-frame stack is concerned. */
                emit_try_pops_for_nonlocal_exit(gen);
                print_indent(gen);
                if (stmt->child_count > 0 && stmt->children[0] &&
                    stmt->children[0]->type != AST_PRINT_STATEMENT) {
                    fprintf(gen->output, "main_exit_ret = ");
                    generate_expression(gen, stmt->children[0]);
                    fprintf(gen->output, "; goto main_exit;\n");
                } else {
                    if (stmt->child_count > 0 && stmt->children[0] &&
                        stmt->children[0]->type == AST_PRINT_STATEMENT) {
                        generate_statement(gen, stmt->children[0]);
                        print_indent(gen);
                    }
                    print_line(gen, "goto main_exit;");
                }
                break;
            }
            // Emit ALL defers before return (unwind entire function)
            if (gen->defer_count > 0 ||
                (stmt->child_count == 1 && value_holds_temps(gen, stmt->children[0], 1))) {
                // Multi-value return + defer: build a _builder_ret typed
                // as the function's tuple return so the existing defer-
                // unwind machinery still applies. Without this branch,
                // we'd save children[0]'s type alone and the C compiler
                // would reject `return _builder_ret;` against the tuple-
                // typed function. Issue #254. Mirrors the no-defer
                // multi-value path below at the "return (_tuple_X_Y){...}"
                // line — same tuple-literal shape, just stuffed into
                // _builder_ret first.
                if (stmt->child_count > 1) {
                    /* Each defer-unwinding return declares its own
                     * `_builder_ret`, so it gets its own C scope: two
                     * returns in one Aether block (`return a` then an
                     * unreachable `return b`, as a converted build file
                     * had) otherwise redeclare it in the same C scope
                     * and clang/gcc reject the redefinition. */
                    print_line(gen, "{");
                    gen->indent_level++;
                    print_indent(gen);
                    Type* tuple = NULL;
                    int owned = 0;
                    if (gen->current_func_return_type &&
                        gen->current_func_return_type->kind == TYPE_TUPLE) {
                        tuple = gen->current_func_return_type;
                    } else {
                        tuple = create_type(TYPE_TUPLE);
                        tuple->tuple_count = stmt->child_count;
                        tuple->tuple_types = malloc(stmt->child_count * sizeof(Type*));
                        for (int j = 0; j < stmt->child_count; j++) {
                            tuple->tuple_types[j] = stmt->children[j]->node_type
                                ? clone_type(stmt->children[j]->node_type)
                                : create_type(TYPE_INT);
                        }
                        owned = 1;
                    }
                    ensure_tuple_typedef(gen, tuple);
                    const char* tname = get_c_type(tuple);
                    /* #2478: the positions are evaluated left to right. */
                    order_prelude_begin(gen, stmt->children, stmt->child_count, NULL);
                    fprintf(gen->output, "%s _builder_ret = (%s){", tname, tname);
                    for (int j = 0; j < stmt->child_count; j++) {
                        if (j > 0) fprintf(gen->output, ", ");
                        emit_tuple_return_position(gen, stmt->children[j], j);
                    }
                    fprintf(gen->output, "};\n");
                    emit_tuple_array_fills(gen, stmt, "_builder_ret");
                    if (owned) free_type(tuple);
                    /* #752: a struct element of the returned tuple is
                     * handed over as a value of its own
                     * (emit_tuple_return_position), so the local's
                     * exit-time destroy still runs. */
                    for (int j = 0; j < stmt->child_count; j++) {
                    }
                    // Multi-value returns can't be returning a closure
                    // (closures aren't tuples), so the closure-of-captures
                    // protection logic the single-value path runs is
                    // unnecessary here — drain the defers and emit the
                    // return.
                    /* #1140: this is a `(value, err)` return, so `defer try` /
                     * `defer catch` fire here — conditionally on the error slot.
                     * `_builder_ret` is already constructed above, so the guard
                     * can test it directly, using the same "non-NULL and
                     * non-empty" convention the rest of the compiler uses for
                     * "this result carries an error". A literal `""` in the
                     * error position would let us decide statically, but the
                     * runtime test costs a predictable compare on the return
                     * path and keeps this to one code path. */
                    {
                        char errslot[32];
                        snprintf(errslot, sizeof(errslot), "_builder_ret._%d",
                                 stmt->child_count - 1);
                        DeferExit prev_exit = gen->defer_exit;
                        const char* prev_slot = gen->defer_err_slot;
                        gen->defer_exit = DEFER_EXIT_RUNTIME;
                        gen->defer_err_slot = errslot;
                        emit_all_defers(gen);
                        gen->defer_exit = prev_exit;
                        gen->defer_err_slot = prev_slot;
                    }
                    /* Issue #501: drain try frames. */
                    emit_try_pops_for_nonlocal_exit(gen);
                    print_line(gen, "return _builder_ret;");
                    gen->indent_level--;
                    print_line(gen, "}");
                    break;
                }
                // For return with value, save to temp first
                if (stmt->child_count > 0 && stmt->children[0] &&
                    stmt->children[0]->type != AST_PRINT_STATEMENT) {
                    /* Own C scope per return, as in the multi-value
                     * branch above: a second return in the same block
                     * would otherwise redeclare `_builder_ret`. */
                    print_line(gen, "{");
                    gen->indent_level++;
                    print_indent(gen);
                    /* Pick the C type for `_builder_ret`. Order:
                     *   1. The function's declared return type
                     *      (`gen->current_func_return_type`) — this
                     *      is what the C compiler will check the
                     *      `return _builder_ret;` against, so using
                     *      anything else risks a type-mismatch
                     *      compile error.
                     *   2. The expression's stamped node_type — used
                     *      when the function doesn't declare a
                     *      return type (Aether's type inference
                     *      stamps the expression).
                     *   3. `int` — last-resort fallback so we always
                     *      emit something compilable.
                     * Without (1), bare-identifier returns whose
                     * node_type isn't filled in (a common gap) fell
                     * through to `int`, which the C compiler then
                     * rejected against a `const char*`-returning
                     * function. Surfaced when the function-exit
                     * defer-free pre-pass started pushing defers
                     * for previously-leaking heap-string locals,
                     * which forced this branch to take effect for
                     * functions that previously skipped it. */
                    Type* fn_ret = gen->current_func_return_type;
                    Type* ret_type = (fn_ret &&
                                      fn_ret->kind != TYPE_VOID &&
                                      fn_ret->kind != TYPE_UNKNOWN)
                                     ? fn_ret
                                     : stmt->children[0]->node_type;
                    const char* ret_c_type = (ret_type && ret_type->kind != TYPE_VOID && ret_type->kind != TYPE_UNKNOWN)
                                             ? c_result_type(ret_type) : "int";
                    fprintf(gen->output, "%s _builder_ret = ", ret_c_type);
                    /* R3: every return hands the caller one reference,
                     * whichever branch produced it. See
                     * emit_uniform_heap_return_expr. */
                    emit_return_value(gen, stmt);   // #340: coerces into `T?`
                    fprintf(gen->output, ";\n");
                    /* Every env-free here is sound to run: a closure local
                     * the value returns has none (claim_closure_local_env
                     * counts a return as an escape), and a closure that the
                     * returned one captured is kept by the reference the
                     * capture took (#2494). Holding those frees back, as
                     * this return used to, only leaked the captured env. */
                    /* #1140: a single-value `return v` is a SUCCESS exit — in a
                     * `T!` function it is wrapped as `{._0 = v, ._1 = ""}`, and
                     * in an ordinary function there is no error channel at all.
                     * Either way `defer try` fires and `defer catch` does not,
                     * and it is known statically, so no runtime guard. */
                    {
                        DeferExit prev_exit = gen->defer_exit;
                        gen->defer_exit = DEFER_EXIT_SUCCESS;
                        emit_all_defers(gen);
                        gen->defer_exit = prev_exit;
                    }
                    /* Issue #501: drain try frames. */
                    emit_try_pops_for_nonlocal_exit(gen);
                    print_line(gen, "return _builder_ret;");
                    gen->indent_level--;
                    print_line(gen, "}");
                } else if (stmt->child_count > 0 && stmt->children[0] &&
                           stmt->children[0]->type == AST_PRINT_STATEMENT) {
                    emit_all_defers(gen);
                    generate_statement(gen, stmt->children[0]);
                    /* Issue #501: drain try frames. */
                    emit_try_pops_for_nonlocal_exit(gen);
                    print_line(gen, "return;");
                } else {
                    emit_all_defers(gen);
                    /* Issue #501: drain try frames. */
                    emit_try_pops_for_nonlocal_exit(gen);
                    print_line(gen, "return;");
                }
            } else {
                // No defers - original behavior
                if (stmt->child_count > 0 && stmt->children[0] &&
                    stmt->children[0]->type == AST_PRINT_STATEMENT) {
                    generate_statement(gen, stmt->children[0]);
                    /* Issue #501: drain try frames. */
                    emit_try_pops_for_nonlocal_exit(gen);
                    print_line(gen, "return;");
                } else if (stmt->child_count > 1) {
                    // Multi-value return: return a, b → return (_tuple_X_Y){a, b}
                    /* Issue #501: drain try frames first; the
                     * print_indent below is for the `return ...`
                     * line, which lands after the drained pops. */
                    emit_try_pops_for_nonlocal_exit(gen);
                    print_indent(gen);
                    // Use the function's known return type if it's a tuple
                    // (avoids UNKNOWN types from unresolved identifiers)
                    Type* tuple = NULL;
                    int owned = 0;
                    if (gen->current_func_return_type &&
                        gen->current_func_return_type->kind == TYPE_TUPLE) {
                        tuple = gen->current_func_return_type;
                    } else {
                        // Fallback: build from expression types
                        tuple = create_type(TYPE_TUPLE);
                        tuple->tuple_count = stmt->child_count;
                        tuple->tuple_types = malloc(stmt->child_count * sizeof(Type*));
                        for (int j = 0; j < stmt->child_count; j++) {
                            tuple->tuple_types[j] = stmt->children[j]->node_type
                                ? clone_type(stmt->children[j]->node_type)
                                : create_type(TYPE_INT);
                        }
                        owned = 1;
                    }
                    ensure_tuple_typedef(gen, tuple);
                    const char* tname = get_c_type(tuple);
                    /* #2478: the positions are evaluated left to right. */
                    order_prelude_begin(gen, stmt->children, stmt->child_count, NULL);
                    int arrays = tuple_return_has_array(gen, stmt);
                    if (arrays) fprintf(gen->output, "{ %s _ae_tr = (%s){", tname, tname);
                    else fprintf(gen->output, "return (%s){", tname);
                    for (int j = 0; j < stmt->child_count; j++) {
                        if (j > 0) fprintf(gen->output, ", ");
                        emit_tuple_return_position(gen, stmt->children[j], j);
                    }
                    fprintf(gen->output, "};\n");
                    if (arrays) {
                        emit_tuple_array_fills(gen, stmt, "_ae_tr");
                        print_line(gen, "return _ae_tr; }");
                    }
                    if (owned) free_type(tuple);
                } else {
                    /* Issue #501: drain try frames before the
                     * print_indent for the `return <expr>;` line. */
                    emit_try_pops_for_nonlocal_exit(gen);
                    print_indent(gen);
                    fprintf(gen->output, "return");
                    if (stmt->child_count > 0) {
                        fprintf(gen->output, " ");
                        /* R3 on the no-defer single-value path,
                         * as on the defer-aware path.
                         * #340: emit_return_value also coerces a
                         * bare value / `none` into a `T?` return. */
                        emit_return_value(gen, stmt);
                    }
                    fprintf(gen->output, ";\n");
                }
            }
            break;
        }

        case AST_BREAK_STATEMENT: {
            int lvl = stmt->value ? find_labeled_loop_level(gen, stmt->value) : -1;
            if (stmt->value && lvl >= 0) {
                /* #893: labeled break — unwind every scope nested inside the
                 * target loop (run their defers), drain the try frames pushed
                 * since that loop was entered, then jump past the loop. */
                emit_defers_through_scope(gen, gen->loop_label_scope[lvl]);
                int drop = gen->try_frame_depth - gen->loop_try_base[lvl];
                for (int i = 0; i < drop; i++) {
                    print_indent(gen);
                    fprintf(gen->output, "aether_try_pop();\n");
                }
                gen->loop_label_break_used[lvl] = 1;
                print_line(gen, "goto __ae_brk_%d;", gen->loop_label_id[lvl]);
            } else if ((lvl = break_switch_target(gen)) >= 0) {
                /* #2696: the break leaves the innermost switch, and only it:
                 * the scopes inside the switch run their defers and the try
                 * frames pushed inside it are popped, not those of the loop
                 * body around it. A C `break` reaches the end of the switch
                 * only when the switch is the innermost C statement it
                 * leaves; from an if-chain, or through a sum match's C
                 * switch, it jumps there. */
                if (gen->scope_depth > gen->switch_scope[lvl])
                    emit_defers_through_scope(gen, gen->switch_scope[lvl]);
                for (int i = gen->switch_try_base[lvl]; i < gen->try_frame_depth; i++)
                    print_line(gen, "aether_try_pop();");
                if (gen->switch_c_depth[lvl] == gen->c_break_depth) {
                    print_line(gen, "break;");
                } else {
                    gen->switch_brk_used[lvl] = 1;
                    print_line(gen, "goto __ae_brk_%d;", gen->switch_brk_id[lvl]);
                }
            } else {
                /* Unwind every scope nested inside the innermost loop,
                 * not just the current one: a `break` inside an `if` (or
                 * a trailing block) inside the loop body leaves those
                 * scopes too, and their defers -- a trailing block's
                 * builder-context pop among them -- must run. Same rule
                 * as the labeled form above. */
                int top = gen->loop_nest_depth - 1;
                if (top >= 0)
                    emit_defers_through_scope(gen, gen->loop_label_scope[top]);
                else
                    emit_defers_for_scope(gen);
                /* Issue #501: drain try frames pushed inside the current
                 * loop body so `break` from inside a try { } in a loop
                 * doesn't leak the panic frame. */
                emit_try_pops_for_break_continue(gen);
                /* #2696: through a sum match's C switch, a C `break` would
                 * leave that switch, not the loop. */
                if (top >= 0 && gen->loop_c_depth[top] != gen->c_break_depth) {
                    gen->loop_label_break_used[top] = 1;
                    print_line(gen, "goto __ae_brk_%d;", gen->loop_label_id[top]);
                } else {
                    print_line(gen, "break;");
                }
            }
            break;
        }

        case AST_CONTINUE_STATEMENT: {
            int lvl = stmt->value ? find_labeled_loop_level(gen, stmt->value) : -1;
            if (stmt->value && lvl >= 0) {
                /* #893: labeled continue — unwind nested scopes and inner try
                 * frames, then jump to the end of the target loop's body (where
                 * the loop re-tests / increments). */
                emit_defers_through_scope(gen, gen->loop_label_scope[lvl]);
                int drop = gen->try_frame_depth - gen->loop_try_base[lvl];
                for (int i = 0; i < drop; i++) {
                    print_indent(gen);
                    fprintf(gen->output, "aether_try_pop();\n");
                }
                gen->loop_label_continue_used[lvl] = 1;
                if (gen->loop_td_id[lvl] && !g_td_in_head) {
                    /* #2378: into a threaded loop — dispatch directly. */
                    char fallback[64];
                    snprintf(fallback, sizeof fallback, "goto __ae_cont_%d;", gen->loop_label_id[lvl]);
                    td_emit_dispatch(gen, lvl, fallback);
                } else {
                    print_line(gen, "goto __ae_cont_%d;", gen->loop_label_id[lvl]);
                }
            } else {
                /* Unwind every scope nested inside the innermost loop,
                 * not just the current one: a `continue` inside an `if` (or
                 * a trailing block) inside the loop body leaves those
                 * scopes too, and their defers -- a trailing block's
                 * builder-context pop among them -- must run. Same rule
                 * as the labeled form above. */
                if (gen->loop_nest_depth > 0)
                    emit_defers_through_scope(gen, gen->loop_label_scope[gen->loop_nest_depth - 1]);
                else
                    emit_defers_for_scope(gen);
                /* Issue #501: drain inside-loop try frames. */
                emit_try_pops_for_break_continue(gen);
                int td_lvl = td_continue_target(gen, stmt);
                int top = gen->loop_nest_depth - 1;
                if (top >= 0 && gen->loop_step_in_body[top]) {
                    /* #2742: the step is the statement after the label. */
                    gen->loop_label_continue_used[top] = 1;
                    print_line(gen, "goto __ae_cont_%d;", gen->loop_label_id[top]);
                } else if (td_lvl >= 0) {
                    /* #2378: run the head and jump to the next arm. */
                    td_emit_dispatch(gen, td_lvl, "continue;");
                } else {
                    print_line(gen, "continue;");
                }
            }
            break;
        }

        case AST_DEFER_STATEMENT:
            // Push deferred statement to stack - will be executed at scope exit.
            // #1140: `value` carries the qualifier ("try" / "catch"; NULL for a
            // plain `defer`), set by the parser. The stack holds the deferred
            // STATEMENT, so the mode travels alongside in gen->defer_mode[].
            if (stmt->child_count > 0) {
                DeferMode mode = DEFER_ALWAYS;
                if (stmt->value) {
                    if (strcmp(stmt->value, "try") == 0)        mode = DEFER_TRY;
                    else if (strcmp(stmt->value, "catch") == 0) mode = DEFER_CATCH;
                }
                if (mode == DEFER_ALWAYS) emit_defer_registration(gen, stmt->children[0]);
                push_defer_mode(gen, stmt->children[0], mode);
            }
            break;

        case AST_TRY_STATEMENT: {
            // try { body } catch name { handler }
            // Emit:
            //   { AetherJmpFrame* _af = aether_try_push();
            //     if (sigsetjmp(_af->buf, 1) == 0) {
            //         body
            //         aether_try_pop();
            //     } else {
            //         const char* NAME = _af->reason ? _af->reason : "panic";
            //         aether_try_pop();
            //         handler
            //     }
            //   }
            //
            // Each try site gets a uniquely-named frame variable so nested
            // try blocks don't shadow each other at the C level.
            //
            // Issue #501: a `return` (or any non-fall-through exit) from
            // inside the body skips the body's `aether_try_pop()` and
            // leaks a panic frame.  We bump gen->try_frame_depth while
            // generating the body so every AST_RETURN_STATEMENT codegen
            // inside emits a draining `aether_try_pop()` first.  The
            // catch handler runs with the depth back at caller's value
            // because the runtime pop is emitted before the handler
            // body — a return inside catch has no live frame to drain.
            if (stmt->child_count != 2) break;
            ASTNode* body = stmt->children[0];
            ASTNode* catch_clause = stmt->children[1];
            if (!body || !catch_clause || catch_clause->type != AST_CATCH_CLAUSE ||
                !catch_clause->value || catch_clause->child_count < 1) break;

            static int s_try_counter = 0;
            int uid = ++s_try_counter;

            print_line(gen, "{");
            indent(gen);
            /* A panic that unwinds out of an enforced block (or out of a
             * trusted call inside one) skips the code that pops the sandbox
             * or restores its depth. Catching it puts the depth back to what
             * it was here, so a catch inside the block stays sandboxed and
             * one outside it is no longer held to the block's grants. */
            if (gen->uses_sandbox) {
                print_line(gen, "int _aether_try_sbx_%d = _aether_sandbox_depth;", uid);
            }
            /* #2810: so does a panic out of a trailing block: the builder
             * context stack goes back to its depth here, and the configs of
             * the builder blocks the panic abandoned are released. */
            print_line(gen, "int _aether_try_ctx_%d = _aether_ctx_depth;", uid);
            print_line(gen, "AetherJmpFrame* _aether_try_%d = aether_try_push();", uid);
            print_line(gen, "if (AETHER_SIGSETJMP(_aether_try_%d->buf, 1) == 0) {", uid);
            indent(gen);
            gen->try_frame_depth++;
            // Body runs inside the if; it already emits its own { } via AST_BLOCK.
            generate_statement(gen, body);
            gen->try_frame_depth--;
            print_line(gen, "aether_try_pop();");
            unindent(gen);
            print_line(gen, "} else {");
            indent(gen);
            if (gen->uses_sandbox) {
                print_line(gen, "_aether_sandbox_depth = _aether_try_sbx_%d;", uid);
            }
            print_line(gen, "_aether_ctx_unwind(_aether_try_ctx_%d);", uid);
            /* #2739: a binding a `try` of its handler writes is volatile:
             * the handler is outside that try, whose setjmp sees it. */
            print_line(gen, "%s %s = _aether_try_%d->reason ? _aether_try_%d->reason : \"panic\";",
                       volatile_decl_type(try_volatile_qual_for(gen, catch_clause->value),
                                          "const char*"),
                       catch_clause->value, uid, uid);
            /* #2333: the catcher owns the reason. A binding that is a
             * plain local of the handler owns it as any string local does;
             * `_` and a binding the handler reaches through a closure
             * environment, a capture cell or actor state borrow it from a
             * hidden owner of the handler's scope. */
            if (catch_binding_can_own(gen, catch_clause->value)) {
                emit_owned_catch_handler(gen, catch_clause->value,
                                         catch_clause->children[0], uid);
            } else {
                char owner[32];
                snprintf(owner, sizeof(owner), "_ae_rs%d", uid);
                print_line(gen, "const char* %s = %s;", owner, catch_clause->value);
                emit_owned_catch_handler(gen, owner, catch_clause->children[0], uid);
            }
            print_line(gen, "(void)%s;", catch_clause->value);
            unindent(gen);
            print_line(gen, "}");
            unindent(gen);
            print_line(gen, "}");
            break;
        }

        case AST_PANIC_STATEMENT: {
            // panic(reason_expr);  → capture backtrace at the call site,
            // then aether_panic(reason). Capturing into TLS before the
            // noreturn call is what gives the runtime stack-trace path
            // (issue #347) the user's caller frames — calling backtrace()
            // from inside aether_panic alone loses them under -O2 because
            // tail-call + noreturn collapses the caller's frame.
            if (stmt->child_count < 1) break;
            print_indent(gen);
            fprintf(gen->output, "aether_panic_capture_stack();\n");
            emit_panic_call(gen, stmt->children[0]);
            break;
        }
            
        case AST_EXPRESSION_STATEMENT:
            if (stmt->child_count > 0) {
                ASTNode* inner = stmt->children[0];
                /* The stored value's temporaries, still open for the plain
                 * store below when the value's constructs ran with them
                 * (#2798). */
                ValueTemps held_vt;
                memset(&held_vt, 0, sizeof(held_vt));
                int held_vt_open = 0;

                /* Struct-field heap-string ownership (#465). Many
                 * parsers land `<var>.<field> = <rhs>` as
                 * AST_EXPRESSION_STATEMENT > AST_BINARY_EXPRESSION
                 * (op="=") rather than AST_ASSIGNMENT. Same wrapper
                 * applies — route through the helper before any
                 * other expression handling. */
                if (inner && inner->type == AST_BINARY_EXPRESSION &&
                    inner->value && strcmp(inner->value, "=") == 0 &&
                    inner->child_count == 2) {
                    /* #340: optional-chain assignment `recv?.field = rhs`. */
                    if (emit_optional_chain_assign(gen, inner->children[0],
                                                   inner->children[1])) {
                        break;
                    }
                    /* #2582: a struct a call in the stored value returns is a
                     * temporary of the statement, as for an AST_ASSIGNMENT
                     * (value_temps_open): `o.f = make_item(w).tag` and
                     * `arr[0] = make_item(w).tag` copy the field and leave
                     * the struct to be destroyed here. These stores were
                     * emitted before the expression statement's own
                     * temporaries were opened, so the struct leaked. */
                    ValueTemps store_vt;
                    int has_store_vt = !has_trailing_block_call(inner->children[1]) &&
                                       value_temps_open_expr(gen, inner->children[1], TEMP_KEPT,
                                                             &store_vt);
                    /* #2798: the constructs in the value run once its
                     * temporaries are open, whichever store takes it: the
                     * temporaries then stay open until the statement is
                     * done, the plain store's too. */
                    int ctl_ran = has_store_vt && g_ctl_deferred == stmt;
                    if (ctl_ran) stmt_control_prelude_deferred(gen, stmt);
                    int stored = emit_struct_field_heap_assign(gen, inner->children[0],
                                                               inner->children[1]) ||
                                 emit_cell_string_element_store(gen, inner->children[0],
                                                                inner->children[1]);   /* #2474 */
                    if (has_store_vt && ctl_ran && !stored) {
                        held_vt = store_vt;
                        held_vt_open = 1;
                    } else if (has_store_vt) {
                        temps_close(gen, &store_vt, 1);
                    }
                    if (stored) break;
                    /* `obj.field = builder(args) { ... }`: the right side
                     * carries a trailing block. Emitted as a plain binary
                     * `=` the block was never visited — the whole body was
                     * silently dropped (aether-ui: every widget built inside
                     * `st.pane = vstack() { ... }` went missing). The
                     * AST_ASSIGNMENT case runs the block (builder or
                     * regular pattern), so route this shape through it. */
                    ASTNode* arhs = inner->children[1];
                    int rhs_trailing = 0;
                    if (arhs && arhs->type == AST_FUNCTION_CALL) {
                        for (int tc = 0; tc < arhs->child_count; tc++) {
                            if (arhs->children[tc] && arhs->children[tc]->type == AST_CLOSURE &&
                                arhs->children[tc]->value &&
                                strcmp(arhs->children[tc]->value, "trailing") == 0) {
                                rhs_trailing = 1;
                                break;
                            }
                        }
                    }
                    if (rhs_trailing) {
                        ASTNode as_assign = *inner;
                        as_assign.type = AST_ASSIGNMENT;
                        print_indent(gen);
                        /* The body, not the wrapper: the enclosing
                         * generate_statement runs the observable-store
                         * hook once for this statement (#2220). */
                        generate_statement_body(gen, &as_assign);
                        break;
                    }
                }

                // Check if this function call has a trailing block
                int has_trailing = 0;
                if (inner && inner->type == AST_FUNCTION_CALL) {
                    for (int tc = 0; tc < inner->child_count; tc++) {
                        if (inner->children[tc] && inner->children[tc]->type == AST_CLOSURE &&
                            inner->children[tc]->value &&
                            strcmp(inner->children[tc]->value, "trailing") == 0) {
                            has_trailing = 1;
                            break;
                        }
                    }
                }

                // Check if this is a builder function call with trailing block
                int is_builder_call = has_trailing && inner->value &&
                    is_builder_func_reg(gen, inner->value);

                /* A builder's block ran ahead of the statement
                 * (generate_statement): the call is a plain one below. */
                if (has_trailing && !is_builder_call) {
                    // REGULAR PATTERN: function runs first, block decorates
                    // Check if function returns void (no return value to capture)
                    int returns_void = 1;
                    if (inner->node_type && inner->node_type->kind != TYPE_VOID &&
                        inner->node_type->kind != TYPE_UNKNOWN) {
                        returns_void = 0;
                    }
                    // Also check what its definition returns (over every
                    // clause of a set, #2645)
                    if (inner->value) {
                        ASTNode* fdef = find_function_definition_by_name(gen->program, inner->value);
                        if (fdef && fn_result_type(gen, fdef)) returns_void = 0;
                    }

                    if (!returns_void) {
                        // Capture return value and push as context
                        print_indent(gen);
                        fprintf(gen->output, "_aether_ctx_push((void*)(intptr_t)");
                        generate_expression(gen, inner);
                        fprintf(gen->output, ");\n");
                    } else {
                        // Void function — just call it, push NULL context
                        generate_expression(gen, inner);
                        fprintf(gen->output, ";\n");
                        print_indent(gen);
                        fprintf(gen->output, "_aether_ctx_push((void*)0);\n");
                    }
                } else {
                    /* `exit(code)` is noreturn — libc exit() terminates the
                     * process immediately, so any function-exit defer-frees
                     * emitted AFTER this call (at the function's natural end)
                     * never run, leaking every live heap local. Emit all
                     * pending defers FIRST so the heap is reclaimed before the
                     * process ends. The defers are flag-guarded (`if(_heap_x)`)
                     * and emit_all_defers is non-destructive, so other (non-
                     * exit) paths still get their own scope-exit frees. This
                     * is what made tests ending in `exit(0)` leak all their
                     * string/seq locals. */
                    if (inner && inner->type == AST_FUNCTION_CALL && inner->value &&
                        strcmp(inner->value, "exit") == 0) {
                        emit_all_defers(gen);
                    }
                    /* A bare call-statement discards the call's value, so
                     * heap-returning inline args must still be drained even
                     * when the callee's declared return type is VOID or
                     * unknown (e.g. `check(label, string.from_long(n), ...)`).
                     * Signal that to the call codegen; the flag is captured
                     * and cleared at the call entry, and reset here too so
                     * it can never leak past this statement. */
                    if (inner && inner->type == AST_FUNCTION_CALL) {
                        gen->discard_call_node = inner;
                    }
                    if (inner && call_returns_owned_closure(gen, inner)) {
                        /* #2507: a closure a call hands over, discarded:
                         * nothing else holds it. */
                        gen->discard_call_node = NULL;
                        fprintf(gen->output, "{ _AeClosure _ae_dc = ");
                        generate_expression(gen, inner);
                        fprintf(gen->output, "; _aether_closure_env_release(_ae_dc.env); }\n");
                    } else {
                        /* A statement whose value is thrown away is
                         * -Wunused-value in the user's own build: a
                         * line-leading `- b`, a bare name, an `x ? y` ask
                         * whose reply is ignored. The cast says what the
                         * statement means (evaluate, keep nothing) and costs
                         * nothing at runtime.
                         *
                         * An allow-list, not a deny-list: some statements
                         * lower to something that is not an expression at all
                         * (a bare array literal emits a braced initializer,
                         * and `(void)({2, 99})` is a statement-expression
                         * containing a brace list, which does not compile).
                         * Calls are left out too: discarding a call's result
                         * is ordinary and warns nowhere. */
                        /* A bare array-literal statement (`[1 + 1, 99]` on
                         * its own line) has no value and no C spelling: the
                         * literal lowers to a braced initializer, and
                         * `{2, 99};` is not a statement any C compiler
                         * accepts. It used to be emitted anyway, so a program
                         * the parser accepts produced C that did not build.
                         * Emit the elements as discarded expressions instead:
                         * anything with a side effect still runs, in order. */
                        if (inner && inner->type == AST_ARRAY_LITERAL) {
                            if (inner->child_count == 0) {
                                fprintf(gen->output, "(void)0;\n");
                            } else {
                                for (int ei = 0; ei < inner->child_count; ei++) {
                                    fprintf(gen->output, "(void)(");
                                    generate_expression(gen, inner->children[ei]);
                                    fprintf(gen->output, ");%s",
                                            ei + 1 == inner->child_count ? "\n" : " ");
                                }
                            }
                            gen->discard_call_node = NULL;
                            break;
                        }
                        /* A struct a call returns by value, owning strings,
                         * that nothing in this statement keeps: the result
                         * thrown away, or the argument of another call (a
                         * fluent chain, `expect_str(s).to_equal(t)`). The
                         * callee's parameter only borrows it (a struct
                         * parameter disowns its copy on entry, and a store
                         * of it copies), so nothing holds its strings past
                         * the statement, and nothing used to free them: the
                         * statement now keeps each in a temporary and
                         * destroys it once done, as a local's scope exit
                         * would. Zeroed first, so one a short circuit
                         * skipped destroys nothing. So does a statement
                         * that frees the string it throws away (below):
                         * one read from such a struct's field and handed to
                         * a call left the struct's strings behind (#2817). */
                        ASTNode** st_nodes = NULL;
                        int st_count = 0, st_cap = 0;
                        collect_stmt_struct_temps(gen, inner, &st_nodes, &st_count, &st_cap);
                        const char** st_names = NULL;
                        ASTNode** st_outer_nodes = NULL;
                        const char** st_outer_names = NULL;
                        int st_outer_count = 0;
                        stmt_struct_temps_get(&st_outer_nodes, &st_outer_names, &st_outer_count);
                        if (st_count > 0) {
                            st_names = (const char**)aether_xrealloc(NULL, sizeof(char*) * (size_t)st_count);
                            static int st_seq = 0;
                            fprintf(gen->output, "{ ");
                            for (int ti = 0; ti < st_count; ti++) {
                                st_names[ti] = cg_internf("_ae_stmp%d", st_seq++);
                                fprintf(gen->output, "%s %s = {0}; ",
                                        get_c_type(st_nodes[ti]->node_type), st_names[ti]);
                            }
                            stmt_struct_temps_set(st_nodes, st_names, st_count);
                        }
                        stmt_control_prelude_deferred(gen, stmt);   /* #2798 */
                        /* A thrown-away string the statement owns (a
                         * heap-returning call, an interpolation, an ask
                         * answered with a string): freed here, as `_ = e`
                         * frees it. Discarded bare, it leaked one buffer
                         * per statement. */
                        /* #2750: so is a `string?` an Aether function
                         * handed over, when it holds one. */
                        if (tuple_call_discard_owns(gen, inner)) {
                            gen->discard_call_node = NULL;
                            emit_tuple_call_discard(gen, inner);
                        } else if (is_owned_opt_string_call(gen, inner)) {
                            gen->discard_call_node = NULL;
                            fprintf(gen->output, "{ %s _od = ", get_c_type(inner->node_type));
                            generate_expression(gen, inner);
                            fprintf(gen->output, "; if (_od.has) aether_heap_str_free((void*)_od.val); }");
                        } else if (inner && (inner->type == AST_FUNCTION_CALL ||
                                             inner->type == AST_STRING_INTERP ||
                                             inner->type == AST_SEND_ASK ||
                                             is_owned_string_unwrap(gen, inner)) &&
                                   is_heap_string_expr(gen, inner)) {
                            /* The value is used (freed), not discarded: an
                             * argument drain keeps the result. */
                            gen->discard_call_node = NULL;
                            fprintf(gen->output, "aether_heap_str_free((void*)(");
                            generate_expression(gen, inner);
                            fprintf(gen->output, "));");
                        } else {
                            int discards_value = inner && (
                                inner->type == AST_IDENTIFIER ||
                                inner->type == AST_LITERAL ||
                                inner->type == AST_UNARY_EXPRESSION ||
                                inner->type == AST_SEND_ASK ||
                                (inner->type == AST_BINARY_EXPRESSION &&
                                 !(inner->value && strcmp(inner->value, "=") == 0)));
                            if (discards_value) fprintf(gen->output, "(void)(");
                            generate_expression(gen, inner);
                            if (discards_value) fprintf(gen->output, ")");
                            fprintf(gen->output, ";");
                        }
                        if (st_count > 0) {
                            stmt_struct_temps_set(st_outer_nodes, st_outer_names, st_outer_count);
                            for (int ti = 0; ti < st_count; ti++) {
                                fprintf(gen->output, " _aether_drop_%s(&%s);",
                                        struct_owning_strings(gen, st_nodes[ti]->node_type),
                                        st_names[ti]);
                            }
                            fprintf(gen->output, " }");
                            free(st_nodes);
                            free(st_names);
                        }
                        fprintf(gen->output, "\n");
                    }
                    gen->discard_call_node = NULL;
                }

                // Trailing blocks for non-defer: emit closure body as inline statements after the call
                if (inner && inner->type == AST_FUNCTION_CALL && !is_builder_call) {
                    for (int tc = 0; tc < inner->child_count; tc++) {
                        ASTNode* trailing = inner->children[tc];
                        if (trailing && trailing->type == AST_CLOSURE &&
                            trailing->value && strcmp(trailing->value, "trailing") == 0) {
                            for (int bi = 0; bi < trailing->child_count; bi++) {
                                if (trailing->children[bi] &&
                                    trailing->children[bi]->type == AST_BLOCK) {
                                    emit_trailing_block_body(gen, trailing->children[bi]);
                                    break;
                                }
                            }
                        }
                    }
                }
                if (held_vt_open) temps_close(gen, &held_vt, 1);
            }
            break;

        case AST_PRINT_STATEMENT:
            // Generate printf call with all arguments
            if (stmt->child_count > 0) {
                ASTNode* first_arg = stmt->children[0];

                // Interpolated string: delegate directly to expression codegen (emits printf(...)),
                // then flush like every other print below.
                if (stmt->child_count == 1 && first_arg->type == AST_STRING_INTERP) {
                    gen->interp_as_printf = 1;
                    generate_expression(gen, first_arg);
                    gen->interp_as_printf = 0;
                    fprintf(gen->output, ";\n");
                    fprintf(gen->output, "fflush(stdout);\n");
                    break;
                }

                // Check if we have a single typed argument (not a string literal)
                if (stmt->child_count == 1 && first_arg->node_type &&
                    !(first_arg->type == AST_LITERAL && first_arg->node_type->kind == TYPE_STRING)) {

                    Type* arg_type = first_arg->node_type;

                    // Generate printf with appropriate format string based on type
                    if (arg_type->kind == TYPE_INT) {
                        // Narrow to (int) to match %d: a single-scalar message
                        // field rides the intptr_t payload slot, so a TYPE_INT
                        // value can be stored wider. Genuine ints only reach
                        // here (ptr/actor-ref use %s), so nothing is truncated.
                        fprintf(gen->output, "printf(\"%%d\", (int)");
                        generate_expression(gen, first_arg);
                        fprintf(gen->output, ");\n");
                    } else if (arg_type->kind == TYPE_FLOAT || arg_type->kind == TYPE_FLOAT32) {
                        fprintf(gen->output, "printf(\"%%f\", ");
                        generate_expression(gen, first_arg);
                        fprintf(gen->output, ");\n");
                    } else if (arg_type->kind == TYPE_LONGDOUBLE) {
                        fprintf(gen->output, "printf(\"%%Lf\", ");
                        generate_expression(gen, first_arg);
                        fprintf(gen->output, ");\n");
                    } else if (arg_type->kind == TYPE_STRING) {
                        /* A heap-producing CALL in bare argument position
                         * (`print(string.concat(a, b))`) is a temporary
                         * nothing else owns: print-and-free via the owned
                         * helper or it leaks per call. Bare identifiers
                         * stay unwrapped, their scope-exit defer owns the
                         * free (wrapping them would double-free). The
                         * interpolation path already handles its own
                         * temporaries; this closes the direct-arg form. */
                        if (first_arg->type == AST_FUNCTION_CALL &&
                            is_heap_string_expr(gen, first_arg)) {
                            fprintf(gen->output, "_aether_print_owned(");
                            generate_expression(gen, first_arg);
                            fprintf(gen->output, ");\n");
                        } else {
                            // NULL-safe, and by length (#2521)
                            fprintf(gen->output, "_aether_print_str(");
                            generate_expression(gen, first_arg);
                            fprintf(gen->output, ");\n");
                        }
                    } else if (arg_type->kind == TYPE_BOOL) {
                        fprintf(gen->output, "printf(\"%%s\", ");
                        generate_expression(gen, first_arg);
                        fprintf(gen->output, " ? \"true\" : \"false\");\n");
                    } else if (arg_type->kind == TYPE_INT64) {
                        fprintf(gen->output, "printf(\"%%lld\", (long long)");
                        generate_expression(gen, first_arg);
                        fprintf(gen->output, ");\n");
                    } else if (arg_type->kind == TYPE_UINT32) {
                        /* Through %d a uint32 past 2^31 prints negative;
                         * uint32_t is unsigned int on every target, so %u
                         * is its conversion. */
                        fprintf(gen->output, "printf(\"%%u\", ");
                        generate_expression(gen, first_arg);
                        fprintf(gen->output, ");\n");
                    } else if (arg_type->kind == TYPE_DURATION) {
                        fprintf(gen->output, "printf(\"%%s\", _aether_duration_repr(");
                        generate_expression(gen, first_arg);
                        fprintf(gen->output, "));\n");
                    } else if (arg_type->kind == TYPE_PTR) {
                        // NULL-safe, and by length (#2521)
                        fprintf(gen->output, "_aether_print_str(");
                        generate_expression(gen, first_arg);
                        fprintf(gen->output, ");\n");
                    } else {
                        // Unknown type - default to %d
                        fprintf(gen->output, "printf(\"%%d\", ");
                        generate_expression(gen, first_arg);
                        fprintf(gen->output, ");\n");
                    }
                } else if (stmt->child_count == 1) {
                    // String literal - print directly
                    ASTNode* arg = stmt->children[0];
                    if (arg->type == AST_LITERAL && arg->node_type && arg->node_type->kind == TYPE_STRING) {
                        emit_print_literal_format(gen, arg);
                        fprintf(gen->output, ";\n");
                    } else {
                        // Unknown type - default to %d
                        fprintf(gen->output, "printf(\"%%d\", ");
                        generate_expression(gen, arg);
                        fprintf(gen->output, ");\n");
                    }
                } else {
                    // Multiple arguments - first is format string
                    // Auto-fix format specifiers based on argument types to prevent
                    // undefined behavior (e.g. print("Test: %s", 201) would crash)
                    ASTNode* fmt_arg = stmt->children[0];
                    if (fmt_arg->type == AST_LITERAL && fmt_arg->node_type &&
                        fmt_arg->node_type->kind == TYPE_STRING && fmt_arg->value) {
                        // Parse format string and replace specifiers with type-correct ones
                        const char* fmt = fmt_arg->value;
                        fprintf(gen->output, "printf(\"");
                        int arg_idx = 1;  // index into stmt->children for arguments
                        for (int fi = 0; fmt[fi]; fi++) {
                            if (fmt[fi] == '%' && fmt[fi + 1]) {
                                fi++;
                                // Skip flags, width, precision
                                while (fmt[fi] == '-' || fmt[fi] == '+' || fmt[fi] == ' ' ||
                                       fmt[fi] == '#' || fmt[fi] == '0') fi++;
                                while (fmt[fi] >= '0' && fmt[fi] <= '9') fi++;
                                if (fmt[fi] == '.') {
                                    fi++;
                                    while (fmt[fi] >= '0' && fmt[fi] <= '9') fi++;
                                }
                                if (fmt[fi] == '%') {
                                    // Literal %%
                                    fprintf(gen->output, "%%%%");
                                } else if (arg_idx < stmt->child_count) {
                                    // Replace with type-correct specifier
                                    ASTNode* arg = stmt->children[arg_idx];
                                    Type* atype = arg->node_type;
                                    if (atype && atype->kind == TYPE_LONGDOUBLE) {
                                        fprintf(gen->output, "%%Lf");
                                    } else if (atype && (atype->kind == TYPE_FLOAT || atype->kind == TYPE_FLOAT32)) {
                                        fprintf(gen->output, "%%f");
                                    } else if (atype && atype->kind == TYPE_INT64) {
                                        fprintf(gen->output, "%%lld");
                                    } else if (atype && atype->kind == TYPE_UINT32) {
                                        fprintf(gen->output, "%%u");
                                    } else if (atype && atype->kind == TYPE_DURATION) {
                                        fprintf(gen->output, "%%s");
                                    } else if (atype && (atype->kind == TYPE_STRING || atype->kind == TYPE_PTR)) {
                                        fprintf(gen->output, "%%s");
                                    } else if (atype && atype->kind == TYPE_BOOL) {
                                        fprintf(gen->output, "%%s");
                                    } else {
                                        fprintf(gen->output, "%%d");
                                    }
                                    arg_idx++;
                                } else {
                                    // More specifiers than args — keep original
                                    fprintf(gen->output, "%%%c", fmt[fi]);
                                }
                            } else {
                                // Re-escape special characters for C string output
                                switch (fmt[fi]) {
                                    case '\n': fprintf(gen->output, "\\n");  break;
                                    case '\t': fprintf(gen->output, "\\t");  break;
                                    case '\r': fprintf(gen->output, "\\r");  break;
                                    case '\0': fprintf(gen->output, "\\0");  break;
                                    case '\\': fprintf(gen->output, "\\\\"); break;
                                    case '"':  fprintf(gen->output, "\\\""); break;
                                    default:   fprintf(gen->output, "%c", fmt[fi]); break;
                                }
                            }
                        }
                        fprintf(gen->output, "\", ");
                        // Emit arguments with type-safe wrappers
                        for (int i = 1; i < stmt->child_count; i++) {
                            if (i > 1) fprintf(gen->output, ", ");
                            ASTNode* arg = stmt->children[i];
                            Type* atype = arg->node_type;
                            if (atype && atype->kind == TYPE_INT64) {
                                fprintf(gen->output, "(long long)");
                                generate_expression(gen, arg);
                            } else if (atype && atype->kind == TYPE_DURATION) {
                                fprintf(gen->output, "_aether_duration_repr(");
                                generate_expression(gen, arg);
                                fprintf(gen->output, ")");
                            } else if (atype && atype->kind == TYPE_BOOL) {
                                generate_expression(gen, arg);
                                fprintf(gen->output, " ? \"true\" : \"false\"");
                            } else if (atype && (atype->kind == TYPE_STRING || atype->kind == TYPE_PTR)) {
                                fprintf(gen->output, "_aether_safe_str(");
                                generate_expression(gen, arg);
                                fprintf(gen->output, ")");
                            } else if (atype && atype->kind == TYPE_INT) {
                                // Narrow to (int) to match the %d chosen above:
                                // a single-scalar message field rides the
                                // intptr_t payload slot, so a TYPE_INT value can
                                // be stored wider. Genuine ints only.
                                fprintf(gen->output, "(int)");
                                generate_expression(gen, arg);
                            } else {
                                generate_expression(gen, arg);
                            }
                        }
                        fprintf(gen->output, ");\n");
                    } else {
                        // Non-literal format string — use %s to prevent format injection
                        fprintf(gen->output, "printf(\"%%s\", ");
                        generate_expression(gen, stmt->children[0]);
                        fprintf(gen->output, ");\n");
                    }
                }
                // Flush stdout so partial-line output appears immediately
                // (without this, print(".") in a loop won't show until \n)
                fprintf(gen->output, "fflush(stdout);\n");
            }
            break;

        case AST_SEND_STATEMENT:
        case AST_SPAWN_ACTOR_STATEMENT:
            // Unreachable in a program that type-checks: generic send() /
            // spawn_actor() are rejected during type checking with a diagnostic
            // pointing at `actor ! Message { ... }` and `spawn(ActorType())`.
            // Kept as a defensive marker so a future path that reaches codegen
            // with one of these nodes fails the C compile loudly rather than
            // silently emitting nothing.
            fprintf(gen->output,
                    "#error internal: generic send()/spawn_actor() reached codegen\n");
            break;

        case AST_BLOCK: {
            // Save declared_var_count before the block. Variables declared
            // inside the block live in its C `{ ... }` scope and must not
            // leak to sibling statements that follow — otherwise a sibling
            // bare-block writing the same name is codegen'd as a
            // reassignment (no type on LHS) even though C scope already
            // closed the earlier declaration. This mirrors what the
            // AST_IF_STATEMENT path does at the `if`/`else` branch boundaries.
            int saved_var_count = gen->declared_var_count;
            print_line(gen, "{");
            indent(gen);
            enter_scope(gen);  // Track defer scope
            for (int i = 0; i < stmt->child_count; i++) {
                generate_statement(gen, stmt->children[i]);
            }
            exit_scope(gen);  // Emit defers and pop scope
            unindent(gen);
            print_line(gen, "}");
            truncate_declared_vars(gen, saved_var_count);
            break;
        }
        
        case AST_REPLY_STATEMENT:
            if (stmt->child_count > 0) {
                ASTNode* reply_expr = stmt->children[0];

                if (reply_expr->type == AST_MESSAGE_CONSTRUCTOR && reply_expr->value) {
                    MessageDef* msg_def = lookup_message(gen->message_registry, reply_expr->value);
                    if (msg_def) {
                        print_indent(gen);
                        // Construct the reply message (validates fields at compile time)
                        fprintf(gen->output, "{ %s _reply = { ._message_id = %d",
                                reply_expr->value, msg_def->message_id);
                        emit_message_field_inits(gen, reply_expr, msg_def);
                        fprintf(gen->output, " }; ");
                        emit_message_array_fills(gen, reply_expr, "_reply", NULL);

                        /* Deep-copy heap-string reply-message fields
                         * (#466). The reply message crosses an actor
                         * boundary back to the asker; without deep-
                         * copy the asker's reply-extraction reads
                         * the handler's heap-string after the
                         * handler's defer-free has run. */
                        for (MessageFieldDef* f = msg_def->fields; f; f = f->next) {
                            if (f->type_kind == TYPE_STRING) {
                                const char* lv = cg_internf("_reply.%s", f->name);
                                emit_message_string_copy(gen, lv,
                                    message_field_init_expr(reply_expr, f->name));
                            }
                        }

                        /* Send reply back to the waiting asker via the
                         * scheduler reply slot. #2528: a reply that owns
                         * strings or closures names its release, for the
                         * case nobody takes it. */
                        int reply_owns = 0;
                        for (MessageFieldDef* f = msg_def->fields; f; f = f->next) {
                            if (f->type_kind == TYPE_STRING ||
                                (f->type_kind == TYPE_FUNCTION && f->c_type &&
                                 strcmp(f->c_type, "_AeClosure") == 0)) { reply_owns = 1; break; }
                        }
                        if (reply_owns) {
                            fprintf(gen->output, "scheduler_reply_owned((ActorBase*)self, &_reply, sizeof(%s), (void (*)(void*))_aether_msg_release_%s); }\n",
                                    reply_expr->value, reply_expr->value);
                        } else {
                            fprintf(gen->output, "scheduler_reply((ActorBase*)self, &_reply, sizeof(%s)); }\n",
                                    reply_expr->value);
                        }
                    } else {
                        fprintf(stderr,
                                "aetherc: line %d: reply references unknown message type '%s'\n",
                                stmt->line, reply_expr->value);
                        exit(1);
                    }
                } else {
                    /* Expression reply (#1324): `reply count`. Deliver a
                     * typed copy through the same scheduler_reply slot the
                     * message form uses; the asker derefs the buffer as
                     * this type (see the AST_SEND_ASK scalar branch, which
                     * MUST stay in sync with this switch). */
                    TypeKind k = (reply_expr->node_type)
                        ? reply_expr->node_type->kind : TYPE_INT;
                    print_indent(gen);
                    if (k == TYPE_FUNCTION && !reply_expr->node_type->is_fnptr) {
                        /* #2528: the asker's binding owns the reply's
                         * closure, so a view is retained and a fresh one
                         * handed over. */
                        fprintf(gen->output, "{ _AeClosure _reply_val = ");
                        emit_closure_take(gen, reply_expr);
                        fprintf(gen->output,
                                "; scheduler_reply_owned((ActorBase*)self, &_reply_val, "
                                "sizeof(_reply_val), _aether_release_closure_buf); }\n");
                    } else if (k == TYPE_STRING) {
                        /* Deep-copy so the asker's read outlives the
                         * handler's defer-free, same contract as
                         * message-field string replies (#466). */
                        fprintf(gen->output, "{ const char* _reply_val = ");
                        generate_expression(gen, reply_expr);
                        fprintf(gen->output, "; ");
                        emit_message_string_copy(gen, "_reply_val", reply_expr);
                        /* #2528: the copy is released if nobody takes it. */
                        fprintf(gen->output,
                                "scheduler_reply_owned((ActorBase*)self, &_reply_val, "
                                "sizeof(const char*), _aether_release_string_buf); }\n");
                    } else {
                        const char* c_type = "int";
                        switch (k) {
                            case TYPE_FLOAT:      c_type = "double"; break;
                            case TYPE_LONGDOUBLE: c_type = "long double"; break;
                            case TYPE_BOOL:       c_type = "int"; break;
                            case TYPE_INT64:      c_type = "int64_t"; break;
                            case TYPE_UINT64:     c_type = "uint64_t"; break;
                            case TYPE_DURATION:   c_type = "int64_t"; break;
                            case TYPE_PTR:        c_type = "void*"; break;
                            default:              c_type = "int"; break;
                        }
                        fprintf(gen->output, "{ %s _reply_val = (%s)(", c_type, c_type);
                        generate_expression(gen, reply_expr);
                        fprintf(gen->output,
                                "); scheduler_reply((ActorBase*)self, &_reply_val, "
                                "sizeof(_reply_val)); }\n");
                    }
                }
            }
            break;
            
        default:
            for (int i = 0; i < stmt->child_count; i++) {
                generate_statement(gen, stmt->children[i]);
            }
            break;
    }
}

// =====================================================================
// Ownership diagnosis (--diagnose=ownership)
//
// Prints how each string store is compiled under the ownership model
// (docs/design/ownership-model.md) without running codegen: a place owns
// one reference to what it holds; a store moves a fresh value, or an
// owning local at its last use, and takes (retains) anything else; a
// literal is pinned and costs no count; a release happens where a place is
// overwritten and where it goes away; a function hands its caller one
// reference from every return. A local still handed to C where C may keep
// it (R4) is kept alive instead of released, and says so.
// =====================================================================

static const char* diag_store_shape(CodeGenerator* gen, ASTNode* rhs, const char** how) {
    if (rhs->type == AST_LITERAL) { *how = "take"; return "literal: pinned, no count"; }
    if (rhs->type == AST_STRING_INTERP) { *how = "move"; return "fresh: an interpolation"; }
    if (rhs->type == AST_IDENTIFIER) {
        *how = "move/take";
        return "a local: moved at its last use, else taken";
    }
    if (is_heap_string_expr(gen, rhs)) { *how = "move"; return "fresh: a call's result"; }
    *how = "take";
    return "borrowed: retained";
}

static void diag_walk_assignments(CodeGenerator* gen, ASTNode* node,
                                   FILE* out, int* found_any) {
    if (!node) return;
    /* Nested function and closure definitions get their own pass. */
    if (node->type == AST_FUNCTION_DEFINITION ||
        node->type == AST_BUILDER_FUNCTION ||
        node->type == AST_CLOSURE) {
        return;
    }
    if (node->type == AST_VARIABLE_DECLARATION && node->value &&
        node->child_count > 0) {
        ASTNode* rhs = node->children[0];
        int lhs_string = (node->node_type && node->node_type->kind == TYPE_STRING) ||
                         (rhs->node_type && rhs->node_type->kind == TYPE_STRING);
        if (lhs_string) {
            const char* how = NULL;
            const char* shape = diag_store_shape(gen, rhs, &how);
            fprintf(out, "    line %4d: %-20s = ...   %-10s [%s]\n",
                    node->line, node->value, how, shape);
            *found_any = 1;
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        diag_walk_assignments(gen, node->children[i], out, found_any);
    }
}

void codegen_diagnose_ownership(ASTNode* program, FILE* out) {
    if (!program || !out) return;

    /* Build a minimal CodeGenerator. The predicates below read
     * `gen->program` (for the user-fn structural-escape lookup) and
     * `gen->extern_registry` (for the type-based escape param lookup
     * `lookup_callee_param_kind` does to keep `string.length(s)` from
     * over-marking `s` as escaped). The rest of the struct stays
     * zeroed. */
    CodeGenerator gen;
    memset(&gen, 0, sizeof(gen));
    gen.program = program;
    /* The verdicts are codegen's, on the tree codegen reads. */
    erase_string_retype_casts(program);
    discover_fn_values(&gen);   /* #2586 */
    /* Populate the extern registry so extern parameters resolve as they
     * do in codegen. Mirrors the registration generate_program
     * does at the top of normal codegen — both direct extern children
     * AND externs reachable via `import` statements (the std.string,
     * std.map, … pulled in by the program live in module ASTs, not
     * the program's own children). */
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* ch = program->children[i];
        if (!ch) continue;
        if (ch->type == AST_EXTERN_FUNCTION && ch->value) {
            register_extern_func(&gen, ch);
        } else if (ch->type == AST_IMPORT_STATEMENT && ch->value) {
            AetherModule* mod_entry = module_find(ch->value);
            ASTNode* mod_ast = mod_entry ? mod_entry->ast : NULL;
            if (mod_ast) {
                for (int j = 0; j < mod_ast->child_count; j++) {
                    ASTNode* decl = mod_ast->children[j];
                    if (decl && decl->type == AST_EXTERN_FUNCTION &&
                        decl->value) {
                        register_extern_func(&gen, decl);
                    }
                }
            }
        }
    }

    fprintf(out, "=== aether ownership diagnosis ===\n");
    fprintf(out, "(how each string store is compiled: a place owns one reference;\n"
                 " a store moves a fresh value or a local at its last use, and takes\n"
                 " anything else; docs/design/ownership-model.md)\n\n");

    /* Pass 1: string-returning user functions. */
    fprintf(out, "[1] string-returning user functions\n");
    int sr_count = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (!c) continue;
        if (c->type != AST_FUNCTION_DEFINITION &&
            c->type != AST_BUILDER_FUNCTION) continue;
        /* For function defs, `node_type` holds the return type
         * directly (not a TYPE_FUNCTION wrapper) — see codegen_func.c
         * where `func->node_type` is read straight as the return type. */
        if (!c->node_type || c->node_type->kind != TYPE_STRING) continue;
        fprintf(out, "  %-30s line %4d   hands its caller one reference from every return\n",
                c->value ? c->value : "(anonymous)", c->line);
        sr_count++;
    }
    if (sr_count == 0) {
        fprintf(out, "  (none)\n");
    }

    /* Pass 2: string stores, by function. The prelude codegen runs is
     * replayed (the heap-string locals), so each store is reported as
     * codegen treats it. State is cleared
     * between functions so a name shadowed across functions does not carry
     * over. */
    fprintf(out, "\n[2] string stores\n\n");
    int total = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (!c) continue;
        if (c->type != AST_FUNCTION_DEFINITION &&
            c->type != AST_BUILDER_FUNCTION &&
            c->type != AST_MAIN_FUNCTION) continue;

        /* Prelude — replicate what generate_function_definition runs
         * before the body, minus the emit. */
        clear_heap_string_vars(&gen);
        const char* heap_names[256];
        int heap_count = 0;
        for (int j = 0; j < c->child_count; j++) {
            collect_heap_string_var_names(&gen, c->children[j],
                                          heap_names, &heap_count, 256);
        }
        for (int n = 0; n < heap_count; n++) {
            if (!is_heap_string_var(&gen, heap_names[n])) {
                mark_heap_string_var(&gen, heap_names[n]);
            }
        }

        fprintf(out, "  %s (line %d):\n",
                c->value ? c->value : "(anonymous)", c->line);
        int found = 0;
        for (int j = 0; j < c->child_count; j++) {
            diag_walk_assignments(&gen, c->children[j], out, &found);
        }
        if (!found) {
            fprintf(out, "    (no string-typed assignments)\n");
        }
        total++;
    }
    /* Final cleanup so the temporary registries don't outlive the
     * stack-allocated `gen`. */
    clear_heap_string_vars(&gen);
    if (total == 0) {
        fprintf(out, "  (no functions in program)\n");
    }

    fprintf(out,
            "\nA line marked KEPT ALIVE names a local passed where C may keep\n"
            "the pointer (a `ptr` or `@retain` parameter of an extern): it is\n"
            "not released, so C's copy stays good. Declare the extern's\n"
            "parameter `@noescape` if C only reads it during the call.\n"
            "\n=== end diagnosis ===\n");
    code_generator_release(&gen);
}

/* Does the call take the reference its string argument `idx` holds: a
 * function that frees what it is given (string.free and the like)? An
 * Aether function may not free a parameter it borrows (E0901), so none of
 * them does. */
int callee_consumes_string_arg(CodeGenerator* gen, const char* func_name, int idx) {
    if (!gen || !func_name || idx < 0) return 0;
    return is_consuming_free(gen, codegen_normalise_callee(func_name));
}

/* An owned field read whose struct can be reached again to clear its
 * tracker: through a pointer, or a struct value that is a variable or a
 * field of one reached through a variable or a pointer (an lvalue; a call's
 * struct result has no address). */
int field_read_can_hand_off(ASTNode* e) {
    if (!is_owned_string_field_read(e)) return 0;
    ASTNode* obj = e->children[0];
    while (obj && obj->node_type && obj->node_type->kind != TYPE_PTR) {
        if (obj->type == AST_IDENTIFIER) return 1;
        /* A call's struct result the statement keeps in a temporary
         * (collect_stmt_struct_temps) and destroys: reached through it. */
        if (obj->type == AST_FUNCTION_CALL) return stmt_struct_temp_of(obj) != NULL;
        if (obj->type != AST_MEMBER_ACCESS || obj->child_count != 1) return 0;
        obj = obj->children[0];
    }
    return obj && obj->node_type != NULL;
}

/* `_ae_fo`, the address of the struct whose field is read, as a temporary
 * of `s` (an ordered sequence: __auto_type types it on the GCC path, and
 * the pointer type spelled here elsewhere, volatile for a value a `try`
 * body writes). A path rooted at a call held in a statement temporary is
 * evaluated into the temporary first and then addressed through it:
 * `(temp = call()).inner` has no address. */
static const char* emit_field_owner(CodeGenerator* gen, ASTNode* obj, TempSeq* s) {
    const char* fo;
    if (obj->node_type->kind == TYPE_PTR) {
        fo = seq_let(gen, s, get_c_type(obj->node_type), "_ae_fo");
        generate_expression(gen, obj);
        seq_next(gen, s);
        return fo;
    }
    const char* pt = cg_internf("%s%s*", get_c_type(obj->node_type),
                                lvalue_may_be_volatile(gen, obj) ? " volatile" : "");
    ASTNode* root = obj;
    while (root->type == AST_MEMBER_ACCESS && root->child_count == 1 &&
           root->children[0]->node_type && root->children[0]->node_type->kind != TYPE_PTR)
        root = root->children[0];
    const char* temp = root->type == AST_FUNCTION_CALL ? stmt_struct_temp_of(root) : NULL;
    if (!temp) {
        fo = seq_let(gen, s, pt, "_ae_fo");
        fprintf(gen->output, "&(");
        generate_expression(gen, obj);
        fprintf(gen->output, ")");
        seq_next(gen, s);
        return fo;
    }
    fprintf(gen->output, "(void)");
    generate_expression(gen, root);
    seq_next(gen, s);
    fo = seq_let(gen, s, pt, "_ae_fo");
    fprintf(gen->output, "&%s", temp);
    /* The member names from the root out to `obj`. */
    int depth = 0;
    for (ASTNode* n = obj; n != root; n = n->children[0]) depth++;
    for (int d = depth; d > 0; d--) {
        ASTNode* n = obj;
        for (int k = 1; k < d; k++) n = n->children[0];
        fprintf(gen->output, ".%s", n->value);
    }
    seq_next(gen, s);
    return fo;
}

/* emit_field_owner as statements ahead of the statement being emitted:
 * the address into a temporary declared there (hoist_declare), whose name
 * is returned. */
static int g_field_owner_ahead = 0;
static const char* emit_field_owner_stmt(CodeGenerator* gen, ASTNode* obj) {
    const char* fo = cg_internf("_ae_foa%d", g_field_owner_ahead++);
    if (obj->node_type->kind == TYPE_PTR) {
        hoist_declare(gen, get_c_type(obj->node_type), fo);
        print_indent(gen);
        fprintf(gen->output, "%s = (", fo);
        generate_expression(gen, obj);
        fprintf(gen->output, ");\n");
        return fo;
    }
    hoist_declare(gen, cg_internf("%s%s*", get_c_type(obj->node_type),
                                  lvalue_may_be_volatile(gen, obj) ? " volatile" : ""), fo);
    ASTNode* root = obj;
    while (root->type == AST_MEMBER_ACCESS && root->child_count == 1 &&
           root->children[0]->node_type && root->children[0]->node_type->kind != TYPE_PTR)
        root = root->children[0];
    const char* temp = root->type == AST_FUNCTION_CALL ? stmt_struct_temp_of(root) : NULL;
    print_indent(gen);
    if (!temp) {
        fprintf(gen->output, "%s = &(", fo);
        generate_expression(gen, obj);
        fprintf(gen->output, ");\n");
        return fo;
    }
    fprintf(gen->output, "(void)(");
    generate_expression(gen, root);
    fprintf(gen->output, ");\n");
    print_indent(gen);
    fprintf(gen->output, "%s = &%s", fo, temp);
    int depth = 0;
    for (ASTNode* n = obj; n != root; n = n->children[0]) depth++;
    for (int d = depth; d > 0; d--) {
        ASTNode* n = obj;
        for (int k = 1; k < d; k++) n = n->children[0];
        fprintf(gen->output, ".%s", n->value);
    }
    fprintf(gen->output, ";\n");
    return fo;
}

/* Field read `e`, which a call frees (STRING_FIELD_CONSUMED) or borrows
 * (STRING_FIELD_TAKEN), evaluated ahead of that call, by the ordering
 * wrapper (in its sequence `s`) or ahead of the statement (`s` NULL). The
 * value goes into `value`. For a free the field gives its reference up to it
 * as the free would take it (a counted string), the struct reached through
 * its address; for a borrow the temporary takes a reference of its own.
 * Whether the temporary owns it goes into a flag, whose name is returned. From the read on the temporary owns the string: a later
 * operand that stores into the field, or replaces the struct, frees the
 * old value no more, and the call takes the temporary's. If the call is not
 * reached (an `or` handler's `return`, a propagating `!`), the flag says
 * the temporary's string is to be freed (push_handover_exit_free). Handed
 * over at the call instead, the field kept its tracker until then, so a
 * store in between freed the string the call then freed or kept again. */
const char* emit_string_field_handover_ahead(CodeGenerator* gen, ASTNode* e, int kind,
                                             TempSeq* s, const char* value) {
    const char* own = cg_internf("%s_own", value);
    if (s) {
        const char* fo = emit_field_owner(gen, e->children[0], s);
        if (kind == STRING_FIELD_TAKEN) {
            /* Declared with the statement, so the block's exit releases it;
             * a statement run again (a loop condition) releases the one
             * before first. */
            hoist_declare_null(gen, "const char*", value);
            hoist_append_text(gen, cg_internf("int %s = 0; ", own));
            fprintf(gen->output, "aether_unwind_track_str_place((const void*)&%s, (const void*)&%s), ",
                    value, own);
            fprintf(gen->output, "%s ? aether_heap_str_free(%s) : (void)0, "
                    "%s = aether_uniform_heap_str(%s->%s, 0), %s = 1",
                    own, value, value, fo, e->value, own);
            seq_next(gen, s);
            return own;
        }
        seq_let_named(gen, s, "const char*", value);
        fprintf(gen->output, "%s->%s", fo, e->value);
        seq_next(gen, s);
        seq_let_named(gen, s, "int", own);
        fprintf(gen->output, "1");
        seq_next(gen, s);
        fprintf(gen->output, "%s->%s = (const char*)0", fo, e->value);
        seq_next(gen, s);
        return own;
    }
    const char* fo = emit_field_owner_stmt(gen, e->children[0]);
    if (kind == STRING_FIELD_TAKEN) {
        hoist_declare_null(gen, "const char*", value);
        hoist_append_text(gen, cg_internf("int %s = 0; ", own));
    } else {
        hoist_declare(gen, "const char*", value);
        hoist_declare(gen, "int", own);
    }
    print_indent(gen);
    if (kind == STRING_FIELD_TAKEN) {
        fprintf(gen->output, "aether_unwind_track_str_place((const void*)&%s, (const void*)&%s);\n",
                value, own);
        print_indent(gen);
        fprintf(gen->output, "if (%s) aether_heap_str_free(%s);\n", own, value);
        print_indent(gen);
        fprintf(gen->output, "%s = aether_uniform_heap_str(%s->%s, 0); %s = 1;\n",
                value, fo, e->value, own);
        return own;
    }
    fprintf(gen->output, "%s = %s->%s; %s = 1; %s->%s = (const char*)0;\n", value, fo, e->value,
            own, fo, e->value);
    return own;
}

/* The exit free of such a temporary: a defer, `if (own) free(value)`, that
 * fires on any exit taken before the call runs (emit_deferred_one,
 * try_emit_handover_exit_free); dropped once the call is written
 * (drop_defer). */
ASTNode* push_handover_exit_free(CodeGenerator* gen, const char* own, const char* value) {
    ASTNode* carrier = create_ast_node(AST_EXPRESSION_STATEMENT, NULL, 0, 0);
    if (!carrier) return NULL;
    if (carrier->annotation) free(carrier->annotation);
    carrier->annotation = heap_strf("handover_exit_free:%s:%s", own, value);
    codegen_own_node(gen, carrier);
    push_defer(gen, carrier);
    return carrier;
}

/* Take defer `carrier` off the stack: it no longer fires on any exit. The
 * entries above it stay; empty ones at the top of the current scope go. */
void drop_defer(CodeGenerator* gen, ASTNode* carrier) {
    if (!carrier) return;
    for (int d = gen->defer_count - 1; d >= 0; d--) {
        if (gen->defer_stack[d] != carrier) continue;
        gen->defer_stack[d] = NULL;
        break;
    }
    while (gen->defer_count > 0 && !gen->defer_stack[gen->defer_count - 1] &&
           (gen->scope_depth <= 0 ||
            gen->defer_count > gen->scope_defer_start[gen->scope_depth - 1]))
        gen->defer_count--;
}

/* The field's value, handed over. The frees a callee can take a reference
 * with (is_consuming_free) are the runtime's, which release a counted
 * AetherString and leave a plain malloc'd buffer alone, so only a counted
 * one leaves the field: the field keeps a plain buffer, and frees it. */
void emit_string_field_handoff(CodeGenerator* gen, ASTNode* e) {
    TempSeq s;
    seq_open_ordered(gen, &s, 1);
    const char* fo = emit_field_owner(gen, e->children[0], &s);
    const char* fv = seq_let(gen, &s, "const char*", "_ae_fv");
    fprintf(gen->output, "%s->%s", fo, e->value);
    seq_next(gen, &s);
    fprintf(gen->output, "%s->%s = (const char*)0", fo, e->value);
    seq_next(gen, &s);
    fprintf(gen->output, "%s", fv);
    seq_close(gen, &s);
}

int string_field_handoff_kind(CodeGenerator* gen, const char* func_name, int idx, ASTNode* arg) {
    if (!func_name || !field_read_can_hand_off(arg)) return 0;
    return callee_consumes_string_arg(gen, func_name, idx) ? STRING_FIELD_CONSUMED : 0;
}

/* #2618: `string.free(arr[i])` / `string.release(arr[i])` / `release(arr[i])`
 * on an element its array owns: the element is given back and emptied, so
 * neither the next store into it nor the array's release frees it again.
 * Freed as it was, the array freed it a second time. A local, parameter or
 * cell array holds counted strings (_aether_str_cell_set): 1. A `string[N]`
 * struct field or state field holds any heap string it took (#2528): 2.
 * 0 for an element of an array that does not own its elements. */
int string_element_free_kind(CodeGenerator* gen, ASTNode* e) {
    if (is_cell_string_element(gen, e) || is_owning_array_string_element(gen, e)) return 1;
    ASTNode* base = e && e->type == AST_ARRAY_ACCESS && e->child_count >= 2 ? e->children[0] : NULL;
    /* An element of a slice: the array it views owns it (#2921). */
    if (base && base->node_type && base->node_type->kind == TYPE_ARRAY &&
        base->node_type->array_size <= 0 && base->node_type->element_type &&
        base->node_type->element_type->kind == TYPE_STRING) return 2;
    if (!base || !str_arr_type(base->node_type)) return 0;
    if (base->type == AST_MEMBER_ACCESS) return is_owned_string_array_field(gen, base) ? 2 : 0;
    return base->type == AST_IDENTIFIER && base->value && is_actor_state_var(gen, base->value)
           ? 2 : 0;
}

void emit_string_element_free(CodeGenerator* gen, ASTNode* e) {
    if (string_element_free_kind(gen, e) == 1) {
        fprintf(gen->output, "_aether_str_cell_set(&(");
        generate_expression(gen, e);
        fprintf(gen->output, "), (const char*)0)");
        return;
    }
    TempSeq s;
    seq_open(gen, &s);
    const char* ep = seq_let(gen, &s, "const char**", "_ae_ep");
    fprintf(gen->output, "(const char**)&(");
    generate_expression(gen, e);
    fprintf(gen->output, ")");
    seq_next(gen, &s);
    const char* ev = seq_let(gen, &s, "const char*", "_ae_ev");
    fprintf(gen->output, "*%s", ep);
    seq_next(gen, &s);
    fprintf(gen->output, "*%s = (const char*)0", ep);
    seq_next(gen, &s);
    fprintf(gen->output, "aether_heap_str_free(%s)", ev);
    seq_close(gen, &s);
}

/* `string.free(s.f)` / `string.release(s.f)` / `release(s.f)`: the field
 * gives back what it holds and is left empty, so neither the next store
 * into it nor the struct's destroy releases it again. */
void emit_string_field_free(CodeGenerator* gen, ASTNode* e) {
    TempSeq s;
    seq_open_ordered(gen, &s, 1);
    const char* fo = emit_field_owner(gen, e->children[0], &s);
    const char* fv = seq_let(gen, &s, "const char*", "_ae_fv");
    fprintf(gen->output, "%s->%s", fo, e->value);
    seq_next(gen, &s);
    fprintf(gen->output, "%s->%s = (const char*)0", fo, e->value);
    seq_next(gen, &s);
    fprintf(gen->output, "aether_heap_str_free(%s)", fv);
    seq_close(gen, &s);
}
