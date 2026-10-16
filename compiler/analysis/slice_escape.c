/* A slice of a function's own storage must not outlive the function. See
 * slice_escape.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "slice_escape.h"
#include "../aether_error.h"
#include "../aether_strmap.h"

#define LIVE ((void*)1)

typedef struct {
    StrMap globals;   /* module-level `var` names */
    int errors;
} Esc;

/* What holds a view of the frame's storage at one point of the function:
 * a set of paths, `v` for a local as a whole or `h.data` for a field of a
 * local value, each mapped to LIVE while it holds one (a cleared path maps
 * to NULL: a StrMap keeps its keys). */
typedef StrMap Views;

/* The `break`s and `continue`s of the loop being walked: what holds a view
 * where each leaves the body. */
typedef struct LoopExits {
    Views exits;
    Views next;
    struct LoopExits* outer;
} LoopExits;

/* One function, closure or `main`: what its frame holds. */
typedef struct Frame {
    ASTNode* owner;
    char shown[160];  /* its name as the source writes it, for messages */
    StrMap typed;     /* names a written-type declaration binds */
    StrMap locals;    /* name -> its binding node (a local or a parameter) */
    StrMap params;    /* the parameters among them */
    StrMap arrays;    /* the frame's own fixed-size arrays: name -> declaration */
    LoopExits* loop;  /* the innermost loop being walked */
    struct Frame* parent; /* the frame a closure is written in; NULL for a function */
    StrMap sticky;    /* paths a closure written here may give a view, from where it is written on */
    StrMap ever;      /* paths that held a view at some point of the walk */
} Frame;

/* ---- the path sets ---- */

static int key_on(const char* key, const char* root) {
    size_t n = strlen(root);
    return strncmp(key, root, n) == 0 && (key[n] == '\0' || key[n] == '.');
}

/* Does anything at or under `path` hold a view, or does a value it is part
 * of hold one as a whole? */
static int views_touch(const Views* v, const char* path) {
    for (int i = 0; i < strmap_count(v); i++) {
        if (!strmap_value_at(v, i)) continue;
        const char* k = strmap_key_at(v, i);
        if (key_on(k, path) || key_on(path, k)) return 1;
    }
    return 0;
}

static void views_set(Views* v, const char* path) {
    strmap_put(v, path, LIVE);
}

/* `path` and everything under it holds no view any more. */
static void views_clear(Views* v, const char* path) {
    for (int i = 0; i < strmap_count(v); i++) {
        const char* k = strmap_key_at(v, i);
        if (strmap_value_at(v, i) && key_on(k, path)) strmap_put(v, k, NULL);
    }
}

static void views_copy(Views* dst, const Views* src) {
    strmap_init(dst);
    for (int i = 0; i < strmap_count(src); i++)
        if (strmap_value_at(src, i)) strmap_put(dst, strmap_key_at(src, i), LIVE);
}

/* dst holds what either holds. Returns 1 when dst grew. */
static int views_join(Views* dst, const Views* src) {
    int grew = 0;
    for (int i = 0; i < strmap_count(src); i++) {
        if (!strmap_value_at(src, i)) continue;
        const char* k = strmap_key_at(src, i);
        if (!strmap_get(dst, k)) {
            strmap_put(dst, k, LIVE);
            grew = 1;
        }
    }
    return grew;
}

static void views_move(Views* dst, Views* src) {
    strmap_free(dst);
    *dst = *src;
}

/* `to` holds what `from` holds, path for path. */
static void views_alias(Views* v, const char* from, const char* to) {
    size_t n = strlen(from);
    int count = strmap_count(v);
    for (int i = 0; i < count; i++) {
        if (!strmap_value_at(v, i)) continue;
        const char* k = strmap_key_at(v, i);
        if (!key_on(k, from)) continue;
        char path[256];
        snprintf(path, sizeof(path), "%s%s", to, k + n);
        strmap_put(v, path, LIVE);
    }
}

/* ---- the frame ---- */

static ASTNode* unwrap_export(ASTNode* n) {
    if (n && n->type == AST_EXPORT_STATEMENT && n->child_count > 0) return n->children[0];
    return n;
}

static int node_line(ASTNode* n) {
    if (!n) return 0;
    if (n->line > 0) return n->line;
    for (int i = 0; i < n->child_count; i++) {
        int l = node_line(n->children[i]);
        if (l > 0) return l;
    }
    return 0;
}

static int node_column(ASTNode* n) {
    if (!n) return 0;
    if (n->line > 0) return n->column;
    for (int i = 0; i < n->child_count; i++) {
        if (node_line(n->children[i]) > 0) return node_column(n->children[i]);
    }
    return 0;
}

/* `x = v` writes the module-level `var x` when the frame declares no `x` of
 * its own with a written type (a bare binding of a global's name is a store
 * to the global, #701). */
static int writes_global(Esc* esc, Frame* f, ASTNode* decl) {
    return decl->value && decl->type_inferred && strmap_has(&esc->globals, decl->value) &&
           !strmap_has(&f->typed, decl->value) && !strmap_has(&f->params, decl->value);
}

static int is_sized_array_type(Type* t) {
    return t && t->kind == TYPE_ARRAY && t->array_size > 0;
}

static void collect_typed(Frame* f, ASTNode* n) {
    if (!n || ast_is_own_function(n)) return;
    if (n->type == AST_VARIABLE_DECLARATION && n->value && !n->type_inferred)
        strmap_put(&f->typed, n->value, n);
    for (int i = 0; i < n->child_count; i++) collect_typed(f, n->children[i]);
}

/* The names the frame binds, outside the closures in it (a trailing block is
 * part of it). */
static int outer_name(Frame* f, const char* name);

static void collect_locals(Esc* esc, Frame* f, ASTNode* n) {
    if (!n || ast_is_own_function(n)) return;
    /* A closure's bare binding of a name of a frame it is written in is a
     * store into that frame's variable, not a local of its own (#2846). */
    if (n->type == AST_VARIABLE_DECLARATION && n->value && !writes_global(esc, f, n) &&
        !(n->type_inferred && outer_name(f, n->value))) {
        if (!strmap_has(&f->locals, n->value)) strmap_put(&f->locals, n->value, n);
        if (is_sized_array_type(n->node_type) && !strmap_has(&f->arrays, n->value))
            strmap_put(&f->arrays, n->value, n);
    } else if ((n->type == AST_PATTERN_VARIABLE || n->type == AST_CATCH_CLAUSE) && n->value) {
        if (!strmap_has(&f->locals, n->value)) strmap_put(&f->locals, n->value, n);
    } else if (n->type == AST_TUPLE_DESTRUCTURE) {
        for (int i = 0; i + 1 < n->child_count; i++) {
            ASTNode* c = n->children[i];
            if (c && c->value && !strmap_has(&f->locals, c->value))
                strmap_put(&f->locals, c->value, c);
        }
    }
    for (int i = 0; i < n->child_count; i++) collect_locals(esc, f, n->children[i]);
}

static ASTNode* frame_body(ASTNode* owner) {
    if (owner->type == AST_MAIN_FUNCTION)
        return owner->child_count > 0 ? owner->children[0] : NULL;
    for (int i = owner->child_count - 1; i >= 0; i--)
        if (owner->children[i] && owner->children[i]->type == AST_BLOCK) return owner->children[i];
    return NULL;
}

static void frame_init(Esc* esc, Frame* f, ASTNode* owner, Frame* parent) {
    memset(f, 0, sizeof(*f));
    f->owner = owner;
    f->parent = parent;
    strmap_init(&f->typed);
    strmap_init(&f->locals);
    strmap_init(&f->params);
    strmap_init(&f->arrays);
    strmap_init(&f->sticky);
    strmap_init(&f->ever);
    if (owner->type == AST_MAIN_FUNCTION) {
        snprintf(f->shown, sizeof(f->shown), "'main'");
    } else if (owner->type == AST_CLOSURE) {
        snprintf(f->shown, sizeof(f->shown), "this closure");
    } else {
        char buf[160];
        snprintf(f->shown, sizeof(f->shown), "'%s'", ast_source_name(owner, buf, sizeof(buf)));
    }
    /* Parameters: a function's children ahead of its body, a closure's
     * AST_CLOSURE_PARAMs. A slice parameter views the caller's storage; a
     * fixed-size array parameter is the function's own copy of the
     * caller's array, storage of its frame as a local array is. */
    for (int i = 0; i < owner->child_count; i++) {
        ASTNode* p = owner->children[i];
        if (!p || p->type == AST_BLOCK || !p->value) continue;
        if (p->type == AST_PATTERN_VARIABLE || p->type == AST_VARIABLE_DECLARATION ||
            p->type == AST_CLOSURE_PARAM) {
            strmap_put(&f->locals, p->value, p);
            strmap_put(&f->params, p->value, p);
            if (is_sized_array_type(p->node_type)) strmap_put(&f->arrays, p->value, p);
        }
    }
    ASTNode* body = frame_body(owner);
    collect_typed(f, body);
    collect_locals(esc, f, body);
}

static void frame_free(Frame* f) {
    strmap_free(&f->typed);
    strmap_free(&f->locals);
    strmap_free(&f->params);
    strmap_free(&f->arrays);
    strmap_free(&f->sticky);
    strmap_free(&f->ever);
}

/* ---- what a value views ---- */

/* Set while a store a closure makes is checked: what it stores may be the
 * storage of the frames it is written in, or a view one of them held. */
static int g_with_outer = 0;
/* Set by frame_view_store: the value it judged is a view of an enclosing
 * frame's storage only, not of the closure's own. */
static int g_store_outer_only = 0;

/* Is `name` the closure frame's own: a parameter or a name it declares with
 * a written type? A bare binding of a name the enclosing frame has is a
 * store into that frame's variable. */
static int own_name(Frame* f, const char* name) {
    return strmap_has(&f->params, name) || strmap_has(&f->typed, name);
}

/* The frame a closure frame `f` is written in (or one around it) whose
 * variable `name` is: the closure neither takes it as a parameter nor
 * declares it with a written type. NULL when it is none of theirs. */
static Frame* outer_frame_of(Frame* f, const char* name) {
    for (Frame* a = f; a && a->parent; a = a->parent) {
        if (own_name(a, name)) return NULL;
        if (strmap_has(&a->parent->locals, name)) return a->parent;
    }
    return NULL;
}

static int outer_name(Frame* f, const char* name) {
    return outer_frame_of(f, name) != NULL;
}

/* With g_with_outer: is `name` one of the fixed-size arrays of a frame the
 * closure frame `f` is written in? */
static int outer_array(Frame* f, const char* name) {
    for (Frame* a = f; a && a->parent; a = a->parent) {
        if (own_name(a, name)) return 0;
        if (strmap_has(&a->parent->arrays, name)) return 1;
    }
    return 0;
}

static int key_on(const char* key, const char* root);
static int views_touch(const Views* v, const char* path);

/* With g_with_outer: did `path` hold a view at some point in a frame the
 * closure frame `f` is written in, or may a closure written there give it
 * one? */
static int outer_holds(Frame* f, const char* path) {
    char root[160];
    size_t n = strcspn(path, ".");
    if (n >= sizeof(root)) n = sizeof(root) - 1;
    memcpy(root, path, n);
    root[n] = '\0';
    for (Frame* a = f; a && a->parent; a = a->parent) {
        if (own_name(a, root)) return 0;
        if (views_touch(&a->parent->ever, path) || views_touch(&a->parent->sticky, path))
            return 1;
    }
    return 0;
}

/* Is `e` storage of the frame: a non-empty array literal (it lives in the
 * frame as the array it is), or one of the frame's own fixed-size arrays? */
static int frame_storage(Frame* f, ASTNode* e) {
    if (!e) return 0;
    if (e->type == AST_ARRAY_LITERAL) return e->child_count > 0;
    if (e->type == AST_IDENTIFIER && e->value)
        return strmap_has(&f->arrays, e->value) || (g_with_outer && outer_array(f, e->value));
    return 0;
}

/* Does `path` hold a view where `v` says what holds one: there, or by a
 * closure written in the frame (from where it is written on), or, while a
 * closure's store is checked, in a frame the closure is written in? */
static int holds_view(Frame* f, const Views* v, const char* path) {
    return views_touch(v, path) || views_touch(&f->sticky, path) ||
           (g_with_outer && outer_holds(f, path));
}

/* The path an lvalue or a read names: `h` for a local, `h.data.ptr` for a
 * field of one; an element (`a[i]`) is its array as a whole, since another
 * element may hold what this one does not. Writes it into `out` and returns
 * its root, or NULL when the root is not a name. `*whole` is set when an
 * element was taken. */
static ASTNode* value_path(ASTNode* e, char* out, size_t cap, int* whole) {
    const char* fields[16];
    int nf = 0;
    *whole = 0;
    while (e && (e->type == AST_MEMBER_ACCESS || e->type == AST_ARRAY_ACCESS) &&
           e->child_count > 0) {
        if (e->type == AST_ARRAY_ACCESS) {
            nf = 0;
            *whole = 1;
        } else if (nf < 16 && e->value) {
            fields[nf++] = e->value;
        }
        e = e->children[0];
    }
    if (!e || e->type != AST_IDENTIFIER || !e->value) return NULL;
    size_t pos = (size_t)snprintf(out, cap, "%s", e->value);
    for (int i = nf - 1; i >= 0 && pos < cap; i--)
        pos += (size_t)snprintf(out + pos, cap - pos, ".%s", fields[i]);
    return e;
}

static int frame_view(Frame* f, const Views* v, ASTNode* e);
static int closure_call_view(Frame* f, const Views* v, ASTNode* call);

/* Can a value of type `t` hold a view: a slice, or a value with fields or
 * elements (a struct, a tuple, an array, an optional, a sum), or a closure?
 * An unknown type might. */
static int may_hold_view(Type* t) {
    if (!t) return 1;
    switch (t->kind) {
        case TYPE_ARRAY: case TYPE_STRUCT: case TYPE_TUPLE: case TYPE_OPTIONAL:
        case TYPE_SUM: case TYPE_FUNCTION: case TYPE_UNKNOWN: case TYPE_WILDCARD:
            return 1;
        default:
            return 0;
    }
}

/* Does the closure `c` capture a name that holds a view here (one it does
 * not bind itself)? A captured slice is copied into the closure as it is,
 * and so still views the frame. A captured array is copied whole, and does
 * not. */
static int holds_view(Frame* f, const Views* v, const char* path);

static int captures_view(Frame* f, const Views* v, ASTNode* n, StrMap* own) {
    if (!n) return 0;
    if (n->type == AST_IDENTIFIER && n->value && holds_view(f, v, n->value) &&
        !strmap_has(own, n->value))
        return 1;
    for (int i = 0; i < n->child_count; i++)
        if (captures_view(f, v, n->children[i], own)) return 1;
    return 0;
}

static void collect_closure_names(ASTNode* n, StrMap* own) {
    if (!n) return;
    if ((n->type == AST_VARIABLE_DECLARATION || n->type == AST_CLOSURE_PARAM ||
         n->type == AST_PATTERN_VARIABLE) && n->value)
        strmap_put(own, n->value, n);
    for (int i = 0; i < n->child_count; i++) collect_closure_names(n->children[i], own);
}

/* Is the value `e`, read where `v` says what holds a view, a slice of the
 * frame's storage, or something holding one (a struct literal, a closure)? */
static int frame_view(Frame* f, const Views* v, ASTNode* e) {
    if (!e) return 0;
    switch (e->type) {
        case AST_SLICE_FROM_ARRAY:
            return e->child_count > 0 && frame_storage(f, e->children[0]);
        case AST_SLICE_EXPR:
            return e->child_count > 0 &&
                   (frame_storage(f, e->children[0]) || frame_view(f, v, e->children[0]));
        case AST_IDENTIFIER:
            return e->value && holds_view(f, v, e->value);
        case AST_MEMBER_ACCESS:
        case AST_ARRAY_ACCESS: {
            /* A field or an element is a view only when it can hold one:
             * an int read out of a slice views nothing. */
            if (!may_hold_view(e->node_type)) return 0;
            char path[256];
            int whole;
            return value_path(e, path, sizeof(path), &whole) && holds_view(f, v, path);
        }
        case AST_EXPRESSION_STATEMENT:
            return e->child_count > 0 && frame_view(f, v, e->children[0]);
        case AST_FUNCTION_CALL:
            return closure_call_view(f, v, e);
        case AST_IF_EXPRESSION:
            return (e->child_count > 1 && frame_view(f, v, e->children[1])) ||
                   (e->child_count > 2 && frame_view(f, v, e->children[2]));
        case AST_MATCH_STATEMENT:
            for (int i = 1; i < e->child_count; i++) {
                ASTNode* arm = e->children[i];
                if (arm && arm->type == AST_MATCH_ARM && arm->child_count > 1 &&
                    frame_view(f, v, match_arm_value(arm->children[1])))
                    return 1;
            }
            return 0;
        case AST_STRUCT_LITERAL:
        case AST_TUPLE_LITERAL:
            for (int i = 0; i < e->child_count; i++) {
                ASTNode* c = e->children[i];
                if (!c) continue;
                /* A struct literal's field init carries its value. */
                if (c->type == AST_ASSIGNMENT && c->child_count == 1) c = c->children[0];
                if (frame_view(f, v, c)) return 1;
            }
            return 0;
        case AST_CLOSURE: {
            if (!ast_is_own_function(e) ||
                (strmap_count(v) == 0 && strmap_count(&f->sticky) == 0 && !g_with_outer))
                return 0;
            StrMap own;
            strmap_init(&own);
            collect_closure_names(e, &own);
            int r = captures_view(f, v, e, &own);
            strmap_free(&own);
            return r;
        }
        default:
            return 0;
    }
}

/* ---- the messages ---- */

/* What the value that leaves is, for the message. */
static void describe(Frame* f, ASTNode* e, char* out, size_t cap) {
    if (!e) { snprintf(out, cap, "a slice of it"); return; }
    switch (e->type) {
        case AST_SLICE_FROM_ARRAY:
        case AST_SLICE_EXPR: {
            ASTNode* c = e->child_count > 0 ? e->children[0] : NULL;
            if (c && c->type == AST_ARRAY_LITERAL)
                snprintf(out, cap, "an array literal");
            else if (c && c->type == AST_IDENTIFIER && c->value && strmap_has(&f->params, c->value))
                snprintf(out, cap, "the array parameter `%s`, its own copy", c->value);
            else if (c && c->type == AST_IDENTIFIER && c->value &&
                     (strmap_has(&f->arrays, c->value) || outer_array(f, c->value)))
                snprintf(out, cap, "the array `%s`", c->value);
            else
                describe(f, c, out, cap);
            return;
        }
        case AST_IDENTIFIER:
            snprintf(out, cap, "`%s`, which holds a slice of it", e->value);
            return;
        case AST_MEMBER_ACCESS:
        case AST_ARRAY_ACCESS: {
            char path[160];
            int whole;
            if (value_path(e, path, sizeof(path), &whole))
                snprintf(out, cap, "`%s`, which holds a slice of it", path);
            else
                snprintf(out, cap, "a slice of it");
            return;
        }
        case AST_STRUCT_LITERAL:
        case AST_TUPLE_LITERAL:
            snprintf(out, cap, "a value holding a slice of it");
            return;
        case AST_CLOSURE:
            snprintf(out, cap, "a closure that captured a slice of it");
            return;
        case AST_FUNCTION_CALL:
            if (e->child_count > 0 && e->children[0] && e->children[0]->value)
                snprintf(out, cap, "what `%s` returns, a slice of it", e->children[0]->value);
            else
                snprintf(out, cap, "a slice of it");
            return;
        default:
            snprintf(out, cap, "a slice of it");
            return;
    }
}

static void report(Esc* esc, Frame* f, ASTNode* at, const char* message) {
    AetherError e = {
        at ? at->source_file : NULL, NULL, node_line(at), node_column(at), message,
        "copy the elements into storage that lives as long as the slice (a `make([]T, n)` "
        "buffer, or a module-level array), or use the slice only while the function runs",
        NULL, AETHER_ERR_DANGLING_VIEW
    };
    if (!e.filename && f->owner) e.filename = f->owner->source_file;
    aether_error_report(&e);
    esc->errors++;
}

typedef enum { LEAVES_BY_RETURN, LEAVES_TO_GLOBAL, LEAVES_TO_FIELD, LEAVES_TO_OUTER } Leaves;

/* `value`, a view of the frame's storage, leaves the frame. `target` names
 * the global or the variable whose field takes it. */
static void report_escape(Esc* esc, Frame* f, ASTNode* value, Leaves how, const char* target) {
    /* Room for the longest message whole: up to three frame names
     * (Frame.shown, 160 each) and the description (what, 200) beside the
     * text, so no message is cut short (and gcc's -Wformat-truncation,
     * an error under -Werror on Linux, has nothing to report). */
    char what[200], msg[1024];
    describe(f, value, what, sizeof(what));
    /* A closure storing a view of the frame it is written in: the storage
     * is that frame's. */
    if (f->parent && how != LEAVES_BY_RETURN && g_store_outer_only) {
        Frame* p = f->parent;
        if (how == LEAVES_TO_GLOBAL)
            snprintf(msg, sizeof(msg),
                     "the module-level `%s` is given by this closure a slice of the storage of %s "
                     "(%s), which ends when %s returns, while `%s` lasts as long as the program",
                     target, p->shown, what, p->shown, target);
        else
            snprintf(msg, sizeof(msg),
                     "a field of `%s`, which may outlive %s, is given by this closure a slice of "
                     "the storage of %s (%s): the storage ends when %s returns, so the slice "
                     "would view memory that is gone",
                     target ? target : "?", p->shown, p->shown, what, p->shown);
        report(esc, f, value, msg);
        return;
    }
    switch (how) {
        case LEAVES_TO_OUTER: {
            Frame* o = outer_frame_of(f, target);
            snprintf(msg, sizeof(msg),
                     "`%s`, a variable of %s, is given by this closure a slice of its own storage "
                     "(%s): the storage ends when the closure returns, while `%s` lives on",
                     target, o ? o->shown : "the frame it is written in", what, target);
            break;
        }
        case LEAVES_BY_RETURN:
            snprintf(msg, sizeof(msg),
                     "%s returns a slice of its own storage (%s): the storage ends when %s "
                     "returns, so the slice would view memory that is gone",
                     f->shown, what, f->shown);
            break;
        case LEAVES_TO_GLOBAL:
            snprintf(msg, sizeof(msg),
                     "the module-level `%s` is given a slice of the storage of %s (%s), which "
                     "ends when %s returns, while `%s` lasts as long as the program",
                     target, f->shown, what, f->shown, target);
            break;
        case LEAVES_TO_FIELD:
        default:
            if (target)
                snprintf(msg, sizeof(msg),
                         "a field of `%s`, which outlives %s, is given a slice of its own "
                         "storage (%s): the storage ends when %s returns, so the slice would "
                         "view memory that is gone",
                         target, f->shown, what, f->shown);
            else
                snprintf(msg, sizeof(msg),
                         "a field that outlives %s is given a slice of its own storage (%s): "
                         "the storage ends when %s returns, so the slice would view memory "
                         "that is gone",
                         f->shown, what, f->shown);
            break;
    }
    report(esc, f, value, msg);
}

/* ---- the walk ---- */

/* The left side and the value of a store: `a.f = v` (a `=` in an
 * expression) or an AST_ASSIGNMENT. */
static int store_parts(ASTNode* n, ASTNode** left, ASTNode** value) {
    if (n->type == AST_BINARY_EXPRESSION && n->value && strcmp(n->value, "=") == 0 &&
        n->child_count >= 2) {
        *left = n->children[0];
        *value = n->children[1];
        return 1;
    }
    if (n->type == AST_ASSIGNMENT && n->child_count >= 2) {
        *left = n->children[0];
        *value = n->children[1];
        return 1;
    }
    return 0;
}

/* Does the lvalue's root hold its fields in the frame: a local or parameter
 * struct, array or tuple (a value the function has a copy of), not a
 * pointer to one? */
static int root_is_frame_value(Frame* f, ASTNode* root) {
    if (!root) return 0;
    ASTNode* bind = strmap_get(&f->locals, root->value);
    if (!bind) return 0;
    Type* t = bind->node_type;
    if (!t || t->kind == TYPE_UNKNOWN) t = root->node_type;
    if ((!t || t->kind == TYPE_UNKNOWN) && bind->type == AST_VARIABLE_DECLARATION &&
        bind->child_count > 0 && bind->children[0])
        t = bind->children[0]->node_type;
    return t && (t->kind == TYPE_STRUCT || t->kind == TYPE_TUPLE ||
                 is_sized_array_type(t));
}

/* `path` (a local, or a field of a local value) is given `value`: from here
 * it holds what the value holds, and nothing it held before. A store into
 * one element of an array leaves what the other elements hold. */
static void bind_path(Frame* f, Views* v, const char* path, int element, ASTNode* value) {
    int holds = frame_view(f, v, value);
    if (holds) strmap_put(&f->ever, path, LIVE);
    char from[256];
    int whole = 0;
    ASTNode* src = NULL;
    if (holds && value && (value->type == AST_IDENTIFIER || value->type == AST_MEMBER_ACCESS))
        src = value_path(value, from, sizeof(from), &whole);
    /* What the value holds is read before the old contents go: `v = v[1..]`. */
    Views was;
    if (src) views_copy(&was, v);
    if (!element) views_clear(v, path);
    if (!holds) {
        if (src) strmap_free(&was);
        return;
    }
    if (src && !whole) {
        /* A copy holds what its source holds, path for path. */
        views_alias(&was, from, path);
        for (int i = 0; i < strmap_count(&was); i++) {
            const char* k = strmap_key_at(&was, i);
            if (strmap_value_at(&was, i) && key_on(k, path)) views_set(v, k);
        }
        strmap_free(&was);
        return;
    }
    if (src) strmap_free(&was);
    if (value && value->type == AST_STRUCT_LITERAL && !element) {
        /* A struct literal: the fields that hold a view. */
        int any = 0;
        for (int i = 0; i < value->child_count; i++) {
            ASTNode* c = value->children[i];
            if (!c || c->type != AST_ASSIGNMENT || c->child_count != 1 || !c->value) continue;
            if (frame_view(f, v, c->children[0])) {
                char fp[256];
                snprintf(fp, sizeof(fp), "%s.%s", path, c->value);
                views_set(v, fp);
                any = 1;
            }
        }
        if (any) return;
    }
    views_set(v, path);
}

static void flow(Esc* esc, Frame* f, ASTNode* n, Views* v, int rep);

/* What a store into a global or through a pointer gives away: in a closure,
 * a view of the storage of the frames it is written in counts as one of
 * its own, as the same store written in that frame would. */
static int frame_view_store(Frame* f, const Views* v, ASTNode* value) {
    int saved = g_with_outer;
    g_with_outer = 0;
    int own = frame_view(f, v, value);
    g_with_outer = f->parent != NULL;
    int r = own || frame_view(f, v, value);
    g_with_outer = saved;
    g_store_outer_only = r && !own;
    return r;
}

/* Each of `n`'s children, in order. */
static void flow_children(Esc* esc, Frame* f, ASTNode* n, Views* v, int rep, int from) {
    for (int i = from; i < n->child_count; i++) flow(esc, f, n->children[i], v, rep);
}

/* `n`'s children from `from`, each from the state `v` has here, joined:
 * the branches of an `if`, the arms of a `match`, the cases of a `switch`.
 * `falls` says control may pass the construct by none of them. */
static void flow_branches(Esc* esc, Frame* f, ASTNode* n, Views* v, int rep, int from,
                          int falls) {
    Views joined;
    if (falls) views_copy(&joined, v);
    else strmap_init(&joined);
    for (int i = from; i < n->child_count; i++) {
        Views arm;
        views_copy(&arm, v);
        flow(esc, f, n->children[i], &arm, rep);
        views_join(&joined, &arm);
        strmap_free(&arm);
    }
    views_move(v, &joined);
}

/* A loop: its body runs any number of times, so what holds a view at its
 * top is what held one before it or at the end of any pass, or where a
 * `continue` left; after it, also what held one where a `break` left. */
static void flow_loop(Esc* esc, Frame* f, ASTNode* n, Views* v, int rep) {
    LoopExits lx;
    strmap_init(&lx.exits);
    strmap_init(&lx.next);
    lx.outer = f->loop;
    f->loop = &lx;
    for (int pass = 0; pass < 32; pass++) {
        Views body;
        views_copy(&body, v);
        flow_children(esc, f, n, &body, 0, 0);
        int grew = views_join(v, &body);
        grew |= views_join(v, &lx.next);
        strmap_free(&body);
        if (!grew) break;
    }
    Views body;
    views_copy(&body, v);
    flow_children(esc, f, n, &body, rep, 0);
    views_join(v, &body);
    strmap_free(&body);
    views_join(v, &lx.exits);
    f->loop = lx.outer;
    strmap_free(&lx.exits);
    strmap_free(&lx.next);
}

/* A block: a name it declares with a written type is a variable of the
 * block, so what the name held outside it holds again after it (joined:
 * the block may also have bound the outer one). */
static void flow_block(Esc* esc, Frame* f, ASTNode* n, Views* v, int rep, int nested) {
    StrMap mine;
    strmap_init(&mine);
    if (nested) {
        for (int i = 0; i < n->child_count; i++) {
            ASTNode* c = n->children[i];
            if (c && c->type == AST_VARIABLE_DECLARATION && c->value && !c->type_inferred)
                strmap_put(&mine, c->value, c);
        }
    }
    Views before;
    if (strmap_count(&mine) > 0) views_copy(&before, v);
    flow_children(esc, f, n, v, rep, 0);
    if (strmap_count(&mine) > 0) {
        for (int i = 0; i < strmap_count(&mine); i++) {
            const char* name = strmap_key_at(&mine, i);
            views_clear(v, name);
            for (int j = 0; j < strmap_count(&before); j++) {
                const char* k = strmap_key_at(&before, j);
                if (strmap_value_at(&before, j) && key_on(k, name)) views_set(v, k);
            }
        }
        strmap_free(&before);
    }
    strmap_free(&mine);
}

/* A value a store may give a view of a frame's storage: anything that can
 * hold one but a call's result (taken to be none, as everywhere in this
 * check), `null` or `[]`. */
/* The names a closure declares itself: its parameters and written-type
 * declarations, and those of the closures nested in it. */
static void closure_own_names(Frame* f, ASTNode* n, StrMap* own) {
    if (!n) return;
    /* A bare binding of a name the frame `f` does not have is the closure's
     * own local, as a written-type one is (#2846). */
    if ((n->type == AST_CLOSURE_PARAM && n->value) ||
        (n->type == AST_VARIABLE_DECLARATION && n->value &&
         (!n->type_inferred || !strmap_has(&f->locals, n->value))))
        strmap_put(own, n->value, n);
    for (int i = 0; i < n->child_count; i++) closure_own_names(f, n->children[i], own);
}

/* The calls of the closure the frame names `name`: `name(...)`, which the
 * checker makes `call(name, ...)`. For each, whether the argument at the
 * closure's parameter `pos` may be a view where `j` says what holds one.
 * Returns 1 when one may. `*reads` counts the uses of the name that are no
 * call of it (passed on, stored, captured). */
static int called_with_view(Frame* f, ASTNode* n, const char* name, int pos, Views* j,
                            int* reads) {
    if (!n) return 0;
    int hit = 0, from = 0;
    if (n->type == AST_FUNCTION_CALL && n->value) {
        int base = -1;
        if (strcmp(n->value, "call") == 0 && n->child_count > 0 && n->children[0] &&
            n->children[0]->type == AST_IDENTIFIER && n->children[0]->value &&
            strcmp(n->children[0]->value, name) == 0) {
            base = 1;
            from = 1;
        } else if (strcmp(n->value, name) == 0) {
            base = 0;
        }
        if (base >= 0) {
            int k = 0;
            for (int i = base; i < n->child_count; i++) {
                ASTNode* a = n->children[i];
                if (!a || ast_is_trailing_block(a)) continue;
                /* An array of the frame passed to a slice parameter is
                 * viewed by it, coerced or not. */
                if (k++ == pos && (frame_view(f, j, a) || frame_storage(f, a))) hit = 1;
            }
        }
    }
    if (n->type == AST_IDENTIFIER && n->value && strcmp(n->value, name) == 0) (*reads)++;
    for (int i = from; i < n->child_count; i++)
        if (called_with_view(f, n->children[i], name, pos, j, reads)) hit = 1;
    return hit;
}

/* The stores of the closure body `n` (closures nested in it included, as
 * it may call them) into the frame's names (not `own`): each whose value
 * may be a view of the storage of the frame (`j` saying what holds one)
 * taints its path from here on, whenever the closure runs, or whether. */
static void taint_stores(Frame* f, ASTNode* n, StrMap* own, Views* j) {
    if (!n) return;
    if (n->type == AST_VARIABLE_DECLARATION && n->value && n->type_inferred &&
        n->child_count > 0 && !strmap_has(own, n->value) && strmap_has(&f->locals, n->value) &&
        frame_view(f, j, n->children[0]))
        strmap_put(&f->sticky, n->value, LIVE);
    ASTNode *left, *value;
    if (store_parts(n, &left, &value) && frame_view(f, j, value)) {
        char path[256];
        int element = 0;
        ASTNode* root = value_path(left, path, sizeof(path), &element);
        if (root && !strmap_has(own, root->value) && strmap_has(&f->locals, root->value))
            strmap_put(&f->sticky, path, LIVE);
    }
    for (int i = 0; i < n->child_count; i++) taint_stores(f, n->children[i], own, j);
}

/* The closure's own locals (in `own`, not parameters) a binding in `n`
 * gives a value that may be a view where `j` says what holds one. */
static void closure_bound_views(Frame* f, ASTNode* n, StrMap* own, Views* j, StrMap* out) {
    if (!n) return;
    if (n->type == AST_VARIABLE_DECLARATION && n->value && n->child_count > 0 &&
        strmap_has(own, n->value)) {
        ASTNode* d = strmap_get(own, n->value);
        if (d && d->type != AST_CLOSURE_PARAM && frame_view(f, j, n->children[0]))
            strmap_put(out, n->value, LIVE);
    }
    for (int i = 0; i < n->child_count; i++) closure_bound_views(f, n->children[i], own, j, out);
}

/* Which of the closure's own names (in `own`) may hold a view of the
 * frame's storage: a parameter the closure is called with a view for (or
 * any parameter that can hold one, when the closure is passed on, stored,
 * or not bound to a name, so its callers are not all here; a nested
 * closure's parameters too), and a local of its own bound to one. Each is
 * added to `j`; the others are taken out of it, where the frame has a name
 * of theirs that holds a view. */
static void closure_views(Frame* f, ASTNode* lit, const char* name, StrMap* own, Views* j) {
    for (int i = 0; i < strmap_count(own); i++) views_clear(j, strmap_key_at(own, i));
    int reads = 0;
    if (name) called_with_view(f, frame_body(f->owner), name, -1, j, &reads);
    int pos = 0;
    for (int i = 0; i < lit->child_count; i++) {
        ASTNode* p = lit->children[i];
        if (!p || p->type != AST_CLOSURE_PARAM || !p->value) continue;
        int view;
        if (!name || reads > 0) {
            view = may_hold_view(p->node_type);
        } else {
            int r = 0;
            view = called_with_view(f, frame_body(f->owner), name, pos, j, &r);
        }
        if (view) views_set(j, p->value);
        pos++;
    }
    /* A nested closure's parameters: their callers are in this closure. */
    for (int i = 0; i < strmap_count(own); i++) {
        ASTNode* d = strmap_value_at(own, i);
        if (d && d->type == AST_CLOSURE_PARAM && !views_touch(j, d->value) &&
            may_hold_view(d->node_type)) {
            int top = 0;
            for (int k = 0; k < lit->child_count; k++)
                if (lit->children[k] == d) top = 1;
            if (!top) views_set(j, d->value);
        }
    }
    /* The closure's locals bound to a view, to a fixpoint. */
    for (int pass = 0; pass < 8; pass++) {
        int grew = 0;
        StrMap todo;
        strmap_init(&todo);
        closure_bound_views(f, lit, own, j, &todo);
        for (int i = 0; i < strmap_count(&todo); i++)
            if (!views_touch(j, strmap_key_at(&todo, i))) {
                views_set(j, strmap_key_at(&todo, i));
                grew = 1;
            }
        strmap_free(&todo);
        if (!grew) break;
    }
}

/* A closure met in the frame's walk, bound to `name` (NULL when it is not):
 * what it may store into the frame's names taints them. Whether a stored
 * value may be a view of the frame's storage is judged as the frame's own
 * stores are: a fixed-size array of the frame, an array literal, or a name
 * that holds a view at some point of the walk (`ever`, complete on the
 * second walk), or where the closure is written (`v`). */
static void taint_closure(Frame* f, ASTNode* lit, const char* name, Views* v) {
    StrMap own;
    strmap_init(&own);
    closure_own_names(f, lit, &own);
    Views j;
    views_copy(&j, v);
    views_join(&j, &f->ever);
    views_join(&j, &f->sticky);
    closure_views(f, lit, name, &own, &j);
    taint_stores(f, lit, &own, &j);
    strmap_free(&j);
    strmap_free(&own);
}

/* Does one of the returns in the closure body `n` (not those of closures
 * nested in it) give a view where `j` says what holds one? An expression
 * body is the value it returns. */
static int returns_view(Frame* f, Views* j, ASTNode* n, int top) {
    if (!n || (!top && ast_is_own_function(n))) return 0;
    if (n->type == AST_RETURN_STATEMENT) {
        for (int i = 0; i < n->child_count; i++)
            if (frame_view(f, j, n->children[i])) return 1;
        return 0;
    }
    for (int i = 0; i < n->child_count; i++) {
        ASTNode* c = n->children[i];
        if (!c || c->type == AST_CLOSURE_PARAM) continue;
        if (top && c->type != AST_BLOCK) {
            if (frame_view(f, j, c)) return 1;
        } else if (returns_view(f, j, c, 0)) {
            return 1;
        }
    }
    return 0;
}

/* The closure literal the frame binds `name` to, once and only once. */
static ASTNode* closure_bound_once(ASTNode* n, const char* name, int* seen) {
    if (!n || ast_is_own_function(n)) return NULL;
    ASTNode* found = NULL;
    if (n->type == AST_VARIABLE_DECLARATION && n->value && strcmp(n->value, name) == 0) {
        (*seen)++;
        if (n->child_count > 0 && n->children[0] && n->children[0]->type == AST_CLOSURE &&
            ast_is_own_function(n->children[0]))
            found = n->children[0];
    }
    for (int i = 0; i < n->child_count; i++) {
        ASTNode* r = closure_bound_once(n->children[i], name, seen);
        if (r) found = r;
    }
    return found;
}

/* #2846: `call(get)` (what `get()` becomes) of a closure the frame binds to
 * `get`: its result views the frame's storage when one of the closure's
 * returns would, judged as its stores are. A call met while one is judged,
 * and another function's result, are taken to be none, as everywhere in
 * this check. */
static int g_judging_call = 0;

static int closure_call_view(Frame* f, const Views* v, ASTNode* call) {
    if (g_judging_call || !call->value || strcmp(call->value, "call") != 0 ||
        call->child_count == 0 || !call->children[0] ||
        call->children[0]->type != AST_IDENTIFIER || !call->children[0]->value)
        return 0;
    const char* name = call->children[0]->value;
    int seen = 0;
    ASTNode* lit = closure_bound_once(frame_body(f->owner), name, &seen);
    if (!lit || seen != 1) return 0;
    g_judging_call = 1;
    StrMap own;
    strmap_init(&own);
    closure_own_names(f, lit, &own);
    Views j;
    views_copy(&j, v);
    views_join(&j, &f->ever);
    views_join(&j, &f->sticky);
    closure_views(f, lit, name, &own, &j);
    int r = returns_view(f, &j, lit, 1);
    strmap_free(&j);
    strmap_free(&own);
    g_judging_call = 0;
    return r;
}

static void flow(Esc* esc, Frame* f, ASTNode* n, Views* v, int rep) {
    if (!n) return;
    if (n->type == AST_CLOSURE && ast_is_own_function(n)) {
        /* A closure is no flow of this frame's, but what it stores into
         * this frame's names it may store whenever it is called, and it
         * may be called from here on. */
        taint_closure(f, n, NULL, v);
        return;
    }
    if (ast_is_own_function(n)) return;
    switch (n->type) {
        case AST_BLOCK:
            flow_block(esc, f, n, v, rep, n != frame_body(f->owner));
            return;
        case AST_IF_STATEMENT:
            if (n->child_count > 0) flow(esc, f, n->children[0], v, rep);
            /* With no `else`, control may pass by the one branch. */
            flow_branches(esc, f, n, v, rep, 1, n->child_count < 3);
            return;
        case AST_MATCH_STATEMENT:
            if (n->child_count > 0) flow(esc, f, n->children[0], v, rep);
            /* A match that may run no arm (no `_` arm, not covering its
             * type) is joined with the state before it as well. */
            flow_branches(esc, f, n, v, rep, 1, !ast_match_covers(n));
            return;
        case AST_SWITCH_STATEMENT: {
            if (n->child_count > 0) flow(esc, f, n->children[0], v, rep);
            /* A case is entered from the switch or from the case before it
             * falling through; the switch may run none. */
            Views joined, prev;
            views_copy(&joined, v);
            strmap_init(&prev);
            for (int i = 1; i < n->child_count; i++) {
                Views arm;
                views_copy(&arm, v);
                views_join(&arm, &prev);
                flow(esc, f, n->children[i], &arm, rep);
                views_join(&joined, &arm);
                views_move(&prev, &arm);
            }
            strmap_free(&prev);
            views_move(v, &joined);
            return;
        }
        case AST_WHILE_LOOP:
        case AST_FOR_LOOP:
            flow_loop(esc, f, n, v, rep);
            return;
        case AST_BREAK_STATEMENT:
            if (f->loop) views_join(&f->loop->exits, v);
            return;
        case AST_CONTINUE_STATEMENT:
            if (f->loop) views_join(&f->loop->next, v);
            return;
        case AST_TRY_STATEMENT: {
            /* The handler runs from wherever the body panicked: from what
             * held a view before it or at any point of it. */
            Views body;
            views_copy(&body, v);
            if (n->child_count > 0) flow(esc, f, n->children[0], &body, rep);
            Views handler;
            views_copy(&handler, v);
            views_join(&handler, &body);
            for (int i = 1; i < n->child_count; i++) flow(esc, f, n->children[i], &handler, rep);
            views_join(&body, &handler);
            views_move(v, &body);
            return;
        }
        case AST_DEFER_STATEMENT: {
            /* It runs at the exit, from whatever holds a view then: checked
             * from here, which is no less. */
            Views later;
            views_copy(&later, v);
            flow_children(esc, f, n, &later, rep, 0);
            strmap_free(&later);
            return;
        }
        case AST_RETURN_STATEMENT:
            flow_children(esc, f, n, v, rep, 0);
            if (rep) {
                for (int i = 0; i < n->child_count; i++)
                    if (frame_view(f, v, n->children[i]))
                        report_escape(esc, f, n->children[i], LEAVES_BY_RETURN, NULL);
            }
            return;
        case AST_VARIABLE_DECLARATION:
            if (n->value && n->child_count > 0 && n->children[0] &&
                n->children[0]->type == AST_CLOSURE && ast_is_own_function(n->children[0]))
                taint_closure(f, n->children[0], n->value, v);
            else
                flow_children(esc, f, n, v, rep, 0);
            if (!n->value || n->child_count == 0) return;
            if (writes_global(esc, f, n)) {
                if (rep && frame_view_store(f, v, n->children[0]))
                    report_escape(esc, f, n->children[0], LEAVES_TO_GLOBAL, n->value);
                return;
            }
            if (n->type_inferred && !strmap_has(&f->locals, n->value) && outer_name(f, n->value)) {
                if (rep && frame_view(f, v, n->children[0]))
                    report_escape(esc, f, n->children[0], LEAVES_TO_OUTER, n->value);
                return;
            }
            bind_path(f, v, n->value, 0, n->children[0]);
            return;
        default:
            break;
    }
    ASTNode *left, *value;
    if (store_parts(n, &left, &value)) {
        flow_children(esc, f, n, v, rep, 0);
        if (left && left->type == AST_IDENTIFIER && left->value) {
            if (strmap_has(&f->locals, left->value))
                bind_path(f, v, left->value, 0, value);
            else if (rep && outer_name(f, left->value) && frame_view(f, v, value))
                report_escape(esc, f, value, LEAVES_TO_OUTER, left->value);
            else if (rep && strmap_has(&esc->globals, left->value) &&
                     frame_view_store(f, v, value))
                report_escape(esc, f, value, LEAVES_TO_GLOBAL, left->value);
            return;
        }
        char path[256];
        int element = 0;
        ASTNode* root = value_path(left, path, sizeof(path), &element);
        if (root && root_is_frame_value(f, root))
            bind_path(f, v, path, element, value);
        else if (rep && frame_view_store(f, v, value))
            report_escape(esc, f, value, LEAVES_TO_FIELD, root ? root->value : NULL);
        return;
    }
    flow_children(esc, f, n, v, rep, 0);
}

static void check_frame(Esc* esc, ASTNode* owner, Frame* parent);

/* The closures in `n` are frames of their own, written in `parent`. */
static void check_closures_in(Esc* esc, ASTNode* n, Frame* parent) {
    for (int i = 0; n && i < n->child_count; i++) {
        ASTNode* c = n->children[i];
        if (!c) continue;
        if (ast_is_own_function(c)) check_frame(esc, c, parent);
        else check_closures_in(esc, c, parent);
    }
}

static void check_frame(Esc* esc, ASTNode* owner, Frame* parent) {
    Frame f;
    frame_init(esc, &f, owner, parent);
    ASTNode* body = frame_body(owner);
    /* Two walks: the first only learns which paths ever hold a view, so
     * that a closure's stores are judged, on the second, against what its
     * names may hold whenever it runs; the second reports. */
    Views v;
    strmap_init(&v);
    flow(esc, &f, body, &v, 0);
    strmap_free(&v);
    strmap_init(&v);
    flow(esc, &f, body, &v, 1);
    strmap_free(&v);
    check_closures_in(esc, body, &f);
    frame_free(&f);
}

int check_slice_escapes(ASTNode* program) {
    if (!program) return 0;
    Esc esc;
    memset(&esc, 0, sizeof(esc));
    strmap_init(&esc.globals);
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* d = unwrap_export(program->children[i]);
        if (d && d->type == AST_CONST_DECLARATION && d->value && d->annotation &&
            strcmp(d->annotation, "global_var") == 0)
            strmap_put(&esc.globals, d->value, d);
    }
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* d = unwrap_export(program->children[i]);
        if (!d) continue;
        if (d->type == AST_FUNCTION_DEFINITION || d->type == AST_BUILDER_FUNCTION ||
            d->type == AST_MAIN_FUNCTION)
            check_frame(&esc, d, NULL);
    }
    strmap_free(&esc.globals);
    return esc.errors;
}
