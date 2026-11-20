/* `aetherc --emit=layout` (#3775): the layout of the structs, messages and
 * actors a file declares, from the table in analysis/type_layout.c, which
 * mirrors the C codegen emits for each type. See codegen_layout.h.
 *
 * A struct is laid out as generate_struct_definition emits it (fields in
 * declaration order), a message as emit_message_struct_body does (the id,
 * then message_struct_fields' order), an actor as generate_actor_definition
 * does (ActorBase, the state in declaration order, the step's inline payload
 * copy last, the whole aligned to 64). The suggested order is a stable sort
 * by alignment, largest first, which leaves no hole between fields because
 * every C type's size is a multiple of its alignment; only the tail is left.
 *
 * Whether `ae pad --fix` may move a struct's fields is decided here, so the
 * driver never rewrites what the compiler has not cleared. A struct whose
 * order something outside this program's source can see is refused with the
 * reason: C declares it (extern struct, @packed, @c_import, extern type), C
 * code is handed it (an extern function, a function an --emit=lib build
 * exports, a message struct --emit-header writes), or the field order is
 * itself data (a @derive(schema) table, @observable's field indices). A
 * file that reads raw offsets with std.mem cannot be followed exactly, so
 * every struct it reaches through a pointer, and every struct a function
 * doing such reads handles, is refused as well, saying it is conservative. */

#include "codegen_layout.h"
#include "codegen_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* sizeof(ActorBase) on a 64-bit target with the default 16-slot mailbox,
 * which runtime/scheduler/multicore_scheduler.h asserts, and its alignment
 * (its widest members are pointers and 64-bit atomics). AetherMsgInline is
 * AETHER_MSG_INLINE_BYTES (28 there) rounded up to its 8-byte members.
 * Other targets lay ActorBase out differently and are not modelled. */
#define LAYOUT_ACTOR_BASE_SIZE 952
#define LAYOUT_ACTOR_BASE_ALIGN 8
#define LAYOUT_MSG_INLINE_SIZE 32
#define LAYOUT_MSG_INLINE_ALIGN 8
#define LAYOUT_ACTOR_ALIGN 64

enum { LK_STRUCT, LK_MESSAGE, LK_ACTOR };
enum { LF_MOVABLE, LF_FIRST, LF_LAST };

typedef struct {
    const char* name;
    const char* type;
    long long off;
    long long size;
    int align;
    int pin;          /* LF_MOVABLE, or kept first or last by codegen */
} LField;

typedef struct {
    int kind;
    const char* name;
    const char* file;
    int line;
    int col;
    int known;
    const char* unknown_why;
    long long size;
    int align;
    int min_align;    /* the struct's own alignment attribute, 1 for none */
    int packed;
    LField* f;
    int nf;
    /* Derived. */
    long long holes;
    int nholes;
    long long tail;
    int* sug;         /* indices into f in the suggested order */
    long long sug_size;
    int reorderable;
    const char* reason;
} LRec;

typedef struct {
    const char* name;
    const char* reason;
} LMark;

typedef struct {
    LMark* v;
    int n;
    int cap;
} LMarks;

/* ---- the marks: which struct names may not move, and why ---- */

static const char* marks_get(const LMarks* m, const char* name) {
    for (int i = 0; i < m->n; i++)
        if (strcmp(m->v[i].name, name) == 0) return m->v[i].reason;
    return NULL;
}

static int marks_put(LMarks* m, const char* name, const char* reason) {
    if (!name || marks_get(m, name)) return 0;
    if (m->n == m->cap) {
        int cap = m->cap ? m->cap * 2 : 16;
        LMark* nv = (LMark*)realloc(m->v, (size_t)cap * sizeof(LMark));
        if (!nv) return 0;
        m->v = nv;
        m->cap = cap;
    }
    m->v[m->n].name = name;
    m->v[m->n].reason = reason;
    m->n++;
    return 1;
}

static ASTNode* def_named(ASTNode* program, ASTNodeType kind, const char* name) {
    if (!program || !name) return NULL;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == kind && c->value && strcmp(c->value, name) == 0) return c;
    }
    return NULL;
}

static void mark_type(LMarks* m, ASTNode* program, const Type* t, const char* reason, int depth);

/* Marks struct `name` and every struct it holds or points at: C code that
 * sees a struct sees what is inside it too. */
static void mark_struct(LMarks* m, ASTNode* program, const char* name, const char* reason,
                        int depth) {
    if (depth > 32 || !marks_put(m, name, reason)) return;
    ASTNode* sd = def_named(program, AST_STRUCT_DEFINITION, name);
    if (!sd) return;
    const char* through = aether_internf("%s (through %s)", reason, name);
    for (int i = 0; i < sd->child_count; i++) {
        ASTNode* f = sd->children[i];
        if (f && f->type == AST_STRUCT_FIELD) mark_type(m, program, f->node_type, through, depth + 1);
    }
}

static void mark_type(LMarks* m, ASTNode* program, const Type* t, const char* reason, int depth) {
    if (!t || depth > 32) return;
    switch (t->kind) {
        case TYPE_STRUCT:
            if (t->struct_name) mark_struct(m, program, t->struct_name, reason, depth + 1);
            return;
        case TYPE_PTR: case TYPE_ARRAY: case TYPE_OPTIONAL: case TYPE_ISOLATED:
            mark_type(m, program, t->element_type, reason, depth + 1);
            return;
        case TYPE_TUPLE:
            for (int i = 0; i < t->tuple_count; i++)
                mark_type(m, program, t->tuple_types[i], reason, depth + 1);
            return;
        case TYPE_SUM: {
            ASTNode* def = def_named(program, AST_SUM_TYPE_DEF, t->struct_name);
            for (int i = 0; def && i < def->child_count; i++) {
                ASTNode* v = def->children[i];
                if (v && v->type == AST_IDENTIFIER && v->value)
                    mark_struct(m, program, v->value, reason, depth + 1);
            }
            return;
        }
        default:
            return;
    }
}

static int is_local(ASTNode* c, const char* path) {
    if (!c || c->is_imported) return 0;
    if (c->source_file && path && strcmp(c->source_file, path) != 0) return 0;
    return 1;
}

static int ann_is(const char* ann, const char* s) { return ann && strcmp(ann, s) == 0; }

/* The struct annotations that name its own reason not to move. */
static const char* own_reason(ASTNode* sd) {
    const char* ann = sd->annotation;
    if (ann_is(ann, "extern")) return "extern struct: the C side declares this order";
    if (ann_is(ann, "extern_packed")) return "@packed: a packed C overlay, the C side declares this order";
    if (aether_is_c_import_annotation(ann)) return "@c_import: the C header lays it out";
    if (ann_is(ann, "extern_opaque")) return "extern type: opaque, the C header defines it";
    if (ann && strstr(ann, "schema")) return "@derive(schema): its field table follows declaration order";
    if (ann && strstr(ann, "observable"))
        return "@observable: observers are told field indices in declaration order";
    return NULL;
}

/* A call that reads or writes memory at a hard-coded offset: std.mem's
 * get_* and set_* accessors, resolved (`mem_get_long`) or as written. */
static int is_raw_mem_call(ASTNode* n) {
    if (!n || n->type != AST_FUNCTION_CALL || !n->value) return 0;
    const char* v = n->value;
    return strncmp(v, "mem_get_", 8) == 0 || strncmp(v, "mem_set_", 8) == 0 ||
           strncmp(v, "mem.get_", 8) == 0 || strncmp(v, "mem.set_", 8) == 0;
}

static ASTNode* first_raw_mem_call(ASTNode* n) {
    if (!n) return NULL;
    if (is_raw_mem_call(n)) return n;
    for (int i = 0; i < n->child_count; i++) {
        ASTNode* r = first_raw_mem_call(n->children[i]);
        if (r) return r;
    }
    return NULL;
}

/* The struct a node reaches through a pointer, or NULL. */
static const char* pointed_struct(ASTNode* n) {
    if (n->type == AST_PTR_AS_STRUCT_CAST || n->type == AST_HEAP_NEW) return n->value;
    const Type* t = n->node_type;
    if (t && t->kind == TYPE_PTR && t->element_type && t->element_type->kind == TYPE_STRUCT)
        return t->element_type->struct_name;
    return NULL;
}

static void mark_pointed(LMarks* m, ASTNode* program, ASTNode* n, ASTNode* site) {
    if (!n) return;
    const char* s = pointed_struct(n);
    if (s && !marks_get(m, s))
        mark_struct(m, program, s,
                    aether_internf("conservative: the file reads raw offsets with std.mem "
                                   "(line %d) and reaches it through a pointer (line %d)",
                                   site->line, n->line), 0);
    for (int i = 0; i < n->child_count; i++) mark_pointed(m, program, n->children[i], site);
}

static void mark_handled(LMarks* m, ASTNode* program, ASTNode* n, ASTNode* fn, ASTNode* site) {
    if (!n) return;
    const Type* t = n->node_type;
    while (t && (t->kind == TYPE_PTR || t->kind == TYPE_ARRAY || t->kind == TYPE_OPTIONAL))
        t = t->element_type;
    if (t && t->kind == TYPE_STRUCT && t->struct_name && !marks_get(m, t->struct_name))
        mark_struct(m, program, t->struct_name,
                    aether_internf("conservative: fn %s reads raw offsets with std.mem "
                                   "(line %d) and handles it", fn->value ? fn->value : "?",
                                   site->line), 0);
    for (int i = 0; i < n->child_count; i++) mark_handled(m, program, n->children[i], fn, site);
}

/* The struct a pointer-to-struct value points at: a `*S` expression or
 * `heap.new(S)`; NULL for anything else. */
static const char* struct_pointer_value(ASTNode* n) {
    if (!n) return NULL;
    if (n->type == AST_HEAP_NEW) return n->value;
    const Type* t = n->node_type;
    if (t && t->kind == TYPE_PTR && t->element_type && t->element_type->kind == TYPE_STRUCT)
        return t->element_type->struct_name;
    return NULL;
}

/* An untyped `ptr`: a pointer whose type says nothing of what it points at. */
static int is_untyped_ptr(const Type* t) {
    return t && t->kind == TYPE_PTR && !(t->element_type && t->element_type->kind == TYPE_STRUCT);
}

/* The parameter type a call hands argument `pos` to, when the callee is a
 * function or extern of the program; NULL when it is not known. */
static Type* callee_param_type(CodeGenerator* gen, ASTNode* program, ASTNode* call, int pos) {
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (!c || !c->value || strcmp(c->value, call->value) != 0) continue;
        if (c->type == AST_EXTERN_FUNCTION)
            return pos < c->child_count && c->children[pos] ? c->children[pos]->node_type : NULL;
        if (c->type == AST_FUNCTION_DEFINITION)
            return pos < fn_param_count(c) ? fn_param_type_at(gen, c, pos) : NULL;
    }
    return NULL;
}

/* A `*S` that becomes an untyped `ptr` can reach C that reads S's bytes
 * at offsets of its own (`fwrite(h as ptr, ...)`), which no type follows.
 * So S is refused, conservatively, when such a value is cast to `ptr`,
 * stored where a `ptr` is declared, or passed to a parameter that is not
 * `*S` itself; freeing it (`free`, `heap.free`) reads nothing. */
static void mark_untyped(CodeGenerator* gen, LMarks* m, ASTNode* program, ASTNode* n) {
    if (!n) return;
    if (is_untyped_ptr(n->node_type) && n->type != AST_FUNCTION_CALL) {
        for (int i = 0; i < n->child_count; i++) {
            const char* s = struct_pointer_value(n->children[i]);
            if (s && !marks_get(m, s))
                mark_struct(m, program, s,
                            aether_internf("conservative: a *%s becomes an untyped ptr (line %d), "
                                           "which C may read at offsets of its own", s,
                                           n->children[i]->line), 0);
        }
    }
    if (n->type == AST_FUNCTION_CALL && n->value && strcmp(n->value, "free") != 0 &&
        strcmp(n->value, "heap.free") != 0 && strcmp(n->value, "heap_free") != 0) {
        for (int i = 0; i < n->child_count; i++) {
            const char* s = struct_pointer_value(n->children[i]);
            if (!s || marks_get(m, s)) continue;
            Type* p = callee_param_type(gen, program, n, i);
            if (p && p->kind == TYPE_PTR && p->element_type &&
                p->element_type->kind == TYPE_STRUCT && p->element_type->struct_name &&
                strcmp(p->element_type->struct_name, s) == 0) continue;
            mark_struct(m, program, s,
                        aether_internf("conservative: a *%s is passed to %s (line %d), which "
                                       "takes it as something other than *%s", s, n->value,
                                       n->line, s), 0);
        }
    }
    for (int i = 0; i < n->child_count; i++) mark_untyped(gen, m, program, n->children[i]);
}

static void collect_marks(CodeGenerator* gen, ASTNode* program, const char* path, LMarks* m) {
    /* Order matters only for which reason a struct reports: its own
     * annotation first, then what C declares, then what C is handed. */
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_STRUCT_DEFINITION && c->value) {
            const char* why = own_reason(c);
            if (why) marks_put(m, c->value, why);
        }
    }
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        const char* ann = c ? c->annotation : NULL;
        if (!c || c->type != AST_STRUCT_DEFINITION || !c->value ||
            !(ann_is(ann, "extern") || ann_is(ann, "extern_packed") ||
              aether_is_c_import_annotation(ann))) continue;
        const char* why = aether_internf("held by extern struct %s (line %d), which C lays out",
                                         c->value, c->line);
        for (int j = 0; j < c->child_count; j++) {
            ASTNode* f = c->children[j];
            if (f && f->type == AST_STRUCT_FIELD) mark_type(m, program, f->node_type, why, 0);
        }
    }
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (!c || c->type != AST_EXTERN_FUNCTION || !c->value) continue;
        const char* why = aether_internf("passed to extern fn %s (line %d), which C implements",
                                         c->value, c->line);
        mark_type(m, program, c->node_type, why, 0);
        for (int j = 0; j < c->child_count; j++)
            if (c->children[j]) mark_type(m, program, c->children[j]->node_type, why, 0);
    }
    /* The functions an --emit=lib build of this file exports, by the rule
     * that build applies (lib_fn_is_local, and @c_callback ones). */
    int saved_lib = gen->emit_lib;
    gen->emit_lib = 1;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* fn = program->children[i];
        if (fn && fn->type == AST_EXPORT_STATEMENT && fn->child_count > 0) fn = fn->children[0];
        if (!fn || fn->type != AST_FUNCTION_DEFINITION || !fn->value || !is_local(fn, path) ||
            strcmp(fn->value, "main") == 0) continue;
        if (!lib_fn_is_local(gen, fn) && !fn_c_callback_def(gen, fn)) continue;
        const char* why = aether_internf("in the signature of fn %s (line %d), which an "
                                         "--emit=lib build exports to C", fn->value, fn->line);
        for (int pos = 0; pos < fn_param_count(fn); pos++)
            mark_type(m, program, fn_param_type_at(gen, fn, pos), why, 0);
        mark_type(m, program, fn_result_type(gen, fn), why, 0);
    }
    gen->emit_lib = saved_lib;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (!c || c->type != AST_MESSAGE_DEFINITION || !c->value) continue;
        const char* why = aether_internf("carried by message %s (line %d), whose struct "
                                         "--emit-header gives C hosts", c->value, c->line);
        for (int j = 0; j < c->child_count; j++) {
            ASTNode* f = c->children[j];
            if (f && f->type == AST_MESSAGE_FIELD) mark_type(m, program, f->node_type, why, 0);
        }
    }
    ASTNode* site = NULL;
    for (int i = 0; i < program->child_count && !site; i++)
        if (is_local(program->children[i], path)) site = first_raw_mem_call(program->children[i]);
    if (site) {
        for (int i = 0; i < program->child_count; i++) {
            ASTNode* c = program->children[i];
            if (!is_local(c, path)) continue;
            mark_pointed(m, program, c, site);
            ASTNode* own = first_raw_mem_call(c);
            if (own && (c->type == AST_FUNCTION_DEFINITION || c->type == AST_MAIN_FUNCTION))
                mark_handled(m, program, c, c, own);
        }
    }
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (is_local(c, path) && c->type != AST_EXTERN_FUNCTION && c->type != AST_STRUCT_DEFINITION)
            mark_untyped(gen, m, program, c);
    }
}

/* ---- laying records out ---- */

static long long align_up(long long n, int a) {
    return a > 1 ? (n + a - 1) / a * a : n;
}

/* Places the fields of `r` in the order `order` (indices into r->f) as C
 * does; returns the size, and writes the offsets when `offs` is not NULL. */
static long long place(const LRec* r, const int* order, long long* offs, int* align_out) {
    long long size = 0;
    int align = r->min_align > 0 ? r->min_align : 1;
    for (int k = 0; k < r->nf; k++) {
        const LField* f = &r->f[order[k]];
        long long off = r->packed ? size : align_up(size, f->align);
        if (offs) offs[order[k]] = off;
        size = off + f->size;
        if (!r->packed && f->align > align) align = f->align;
    }
    if (align_out) *align_out = r->packed ? 1 : align;
    return r->packed ? size : align_up(size, align);
}

static void finish(LRec* r) {
    r->holes = 0;
    r->nholes = 0;
    r->tail = 0;
    r->sug = NULL;
    r->sug_size = r->size;
    if (!r->known) return;
    long long end = 0;
    for (int i = 0; i < r->nf; i++) {
        if (r->f[i].off > end) { r->holes += r->f[i].off - end; r->nholes++; }
        if (r->f[i].off + r->f[i].size > end) end = r->f[i].off + r->f[i].size;
    }
    r->tail = r->size > end ? r->size - end : 0;
    r->sug = (int*)malloc((size_t)(r->nf > 0 ? r->nf : 1) * sizeof(int));
    if (!r->sug) return;
    int k = 0;
    for (int i = 0; i < r->nf; i++) if (r->f[i].pin == LF_FIRST) r->sug[k++] = i;
    /* Stable: alignments from the largest down, ties in declaration order. */
    int maxa = 1;
    for (int i = 0; i < r->nf; i++) if (r->f[i].align > maxa) maxa = r->f[i].align;
    for (int a = maxa; a >= 1; a--)
        for (int i = 0; i < r->nf; i++)
            if (r->f[i].pin == LF_MOVABLE && r->f[i].align == a) r->sug[k++] = i;
    for (int i = 0; i < r->nf; i++)
        if (r->f[i].pin == LF_MOVABLE && (r->f[i].align < 1 || r->f[i].align > maxa)) r->sug[k++] = i;
    for (int i = 0; i < r->nf; i++) if (r->f[i].pin == LF_LAST) r->sug[k++] = i;
    r->sug_size = r->packed ? r->size : place(r, r->sug, NULL, NULL);
    if (r->sug_size >= r->size) {
        /* Nothing smaller: the suggestion is the order as it stands, which
         * is what makes a second `ae pad --fix` a no-op. */
        for (int i = 0; i < r->nf; i++) r->sug[i] = i;
        r->sug_size = r->size;
    }
}

/* Messages and actors have no layout routine of their own in type_layout.c:
 * their fields are placed here, in the order codegen emits them. */
static void place_in_order(LRec* r) {
    int* ord = (int*)calloc((size_t)(r->nf > 0 ? r->nf : 1), sizeof(int));
    long long* offs = (long long*)calloc((size_t)(r->nf > 0 ? r->nf : 1), sizeof(long long));
    if (!ord || !offs) { free(ord); free(offs); r->unknown_why = "out of memory"; return; }
    for (int i = 0; i < r->nf; i++) ord[i] = i;
    r->size = place(r, ord, offs, &r->align);
    for (int i = 0; i < r->nf; i++) r->f[i].off = offs[i];
    free(ord);
    free(offs);
    r->known = 1;
}

static int add_field(LRec* r, int cap, const char* name, const char* type, long long size,
                     int align, int pin) {
    if (r->nf >= cap) return 0;
    LField* f = &r->f[r->nf++];
    f->name = name;
    f->type = type;
    f->off = 0;
    f->size = size;
    f->align = align;
    f->pin = pin;
    return 1;
}

/* The type as the source spells it. type_spelled has no word for the
 * unsigned and vector kinds a struct field can hold, so those are named
 * here as the parser reads them. */
static const char* type_display(const Type* t) {
    if (!t) return "unknown";
    if (!t->c_alias && !t->distinct_name) {
        switch (t->kind) {
            case TYPE_UINT8: return "uint8";
            case TYPE_UINT16: return "uint16";
            case TYPE_UINT32: return "uint32";
            case TYPE_F32X4: return "f32x4";
            case TYPE_F64X2: return "f64x2";
            case TYPE_I32X4: return "i32x4";
            case TYPE_I64X2: return "i64x2";
            case TYPE_I16X8: return "i16x8";
            case TYPE_F32X8: return "f32x8";
            case TYPE_I32X8: return "i32x8";
            default: break;
        }
    }
    return type_spelled((Type*)t);
}

static void struct_record(ASTNode* sd, ASTNode* program, const LayoutTarget* tgt, LRec* r) {
    r->kind = LK_STRUCT;
    r->min_align = 1;
    r->packed = ann_is(sd->annotation, "extern_packed");
    r->f = (LField*)calloc((size_t)(sd->child_count + 1), sizeof(LField));
    long long* offs = (long long*)calloc((size_t)(sd->child_count + 1), sizeof(long long));
    if (!r->f || !offs) { free(offs); r->known = 0; r->unknown_why = "out of memory"; return; }
    if (ann_is(sd->annotation, "extern_opaque")) {
        r->unknown_why = "extern type: opaque, the C header defines it";
        free(offs);
        return;
    }
    if (aether_is_c_import_annotation(sd->annotation)) {
        r->unknown_why = "@c_import: the C header lays it out";
        free(offs);
        return;
    }
    TypeLayout whole;
    int ok = struct_layout_of(sd, program, tgt, &whole, offs);
    for (int i = 0; i < sd->child_count; i++) {
        ASTNode* f = sd->children[i];
        if (!f || (f->type != AST_STRUCT_FIELD && f->type != AST_STRUCT_FIELD_UNION &&
                   f->type != AST_STRUCT_FIELD_NESTED)) continue;
        TypeLayout fl;
        const char* ty = f->type == AST_STRUCT_FIELD_UNION ? "union { ... }"
                       : f->type == AST_STRUCT_FIELD_NESTED ? "struct { ... }"
                       : type_display(f->node_type);
        if (!struct_member_layout_of(sd, i, program, tgt, &fl)) {
            if (!r->unknown_why)
                r->unknown_why = f->bit_width > 0
                    ? aether_internf("field %s is a C bitfield, which the C compiler places", f->value)
                    : aether_internf("field %s: %s, a type this table does not lay out", f->value, ty);
            ok = 0;
            fl.size = 0;
            fl.align = 1;
        }
        add_field(r, sd->child_count + 1, f->value, ty, fl.size, fl.align, LF_MOVABLE);
        r->f[r->nf - 1].off = offs[i];
    }
    free(offs);
    if (!ok) return;
    r->known = 1;
    r->size = whole.size;
    r->align = whole.align;
}

static void message_record(ASTNode* md, ASTNode* program, const LayoutTarget* tgt, LRec* r) {
    r->kind = LK_MESSAGE;
    r->min_align = 1;
    int cap = md->child_count + 2;
    r->f = (LField*)calloc((size_t)cap, sizeof(LField));
    ASTNode** order = (ASTNode**)calloc((size_t)cap, sizeof(ASTNode*));
    if (!r->f || !order) { free(order); r->unknown_why = "out of memory"; return; }
    add_field(r, cap, "_message_id", "int", 4, 4, LF_FIRST);
    int n = message_struct_fields(program, md, order);
    int ok = 1;
    for (int i = 0; i < n; i++) {
        ASTNode* f = order[i];
        TypeLayout fl;
        const char* ty = type_display(f->node_type);
        /* The lone int of an inline-payload message is intptr_t. */
        if (strcmp(message_field_c_type(md, f), "intptr_t") == 0) {
            fl.size = tgt->ptr_size;
            fl.align = tgt->ptr_size;
            ty = aether_internf("%s (intptr_t)", ty);
        } else if (!f->node_type || !type_layout_of(f->node_type, program, tgt, &fl)) {
            if (!r->unknown_why)
                r->unknown_why = aether_internf("field %s: %s, a type this table does not lay out",
                                                f->value, ty);
            ok = 0;
            fl.size = 0;
            fl.align = 1;
        }
        add_field(r, cap, f->value, ty, fl.size, fl.align, LF_MOVABLE);
    }
    free(order);
    if (!ok) return;
    place_in_order(r);
}

static void actor_record(CodeGenerator* gen, ASTNode* ad, ASTNode* program,
                         const LayoutTarget* tgt, LRec* r) {
    r->kind = LK_ACTOR;
    r->min_align = LAYOUT_ACTOR_ALIGN;
    int cap = ad->child_count + 3;
    r->f = (LField*)calloc((size_t)cap, sizeof(LField));
    if (!r->f) { r->unknown_why = "out of memory"; return; }
    int ok = tgt->ptr_size == 8 && tgt->i64_align == 8;
    if (!ok) r->unknown_why = "ActorBase is modelled for 64-bit targets only";
    add_field(r, cap, "_aether_base", "ActorBase", LAYOUT_ACTOR_BASE_SIZE,
              LAYOUT_ACTOR_BASE_ALIGN, LF_FIRST);
    for (int i = 0; i < ad->child_count; i++) {
        ASTNode* s = ad->children[i];
        if (!s || s->type != AST_STATE_DECLARATION || !s->value) continue;
        TypeLayout fl;
        const char* ty = type_display(s->node_type);
        if (actor_state_decl_is_void_ptr(gen, ad, s)) {
            fl.size = tgt->ptr_size;
            fl.align = tgt->ptr_size;
            ty = "ptr (used as one)";
        } else if (!s->node_type || !type_layout_of(s->node_type, program, tgt, &fl)) {
            if (!r->unknown_why)
                r->unknown_why = aether_internf("state %s: %s, a type this table does not lay out",
                                                s->value, ty);
            ok = 0;
            fl.size = 0;
            fl.align = 1;
        }
        add_field(r, cap, s->value, ty, fl.size, fl.align, LF_MOVABLE);
    }
    if (actor_has_step_inline(ad))
        add_field(r, cap, "_aether_step_inline", "AetherMsgInline", LAYOUT_MSG_INLINE_SIZE,
                  LAYOUT_MSG_INLINE_ALIGN, LF_LAST);
    if (!ok) return;
    place_in_order(r);
}

/* ---- writing the report ---- */

static const char* kind_name(int k) {
    return k == LK_MESSAGE ? "message" : k == LK_ACTOR ? "actor" : "struct";
}

static long long ceil_div(long long a, long long b) { return (a + b - 1) / b; }

/* Cache lines the record spans when it starts on a line, and at worst
 * when only its own alignment, or malloc's 16 bytes, is promised. */
static void lines_of(const LRec* r, long long* aligned64, long long* worst, int* worst_align) {
    long long sz = r->size > 0 ? r->size : 1;
    *aligned64 = ceil_div(sz, 64);
    int a = r->align > 16 ? r->align : 16;
    *worst_align = a;
    *worst = *aligned64;
    if (a >= 64) return;
    for (long long start = 0; start < 64; start += a) {
        long long n = ceil_div(start + sz, 64);
        if (n > *worst) *worst = n;
    }
}

static long long saving_of(const LRec* r) {
    return r->known && r->sug_size < r->size ? r->size - r->sug_size : 0;
}

static void print_holes(FILE* out, const LRec* r) {
    fprintf(out, "holes %lld", r->holes);
    if (r->nholes == 0) return;
    long long first = -1, end = 0;
    int same = 1;
    for (int i = 0; i < r->nf; i++) {
        if (r->f[i].off > end) {
            long long h = r->f[i].off - end;
            if (first < 0) first = h; else if (h != first) same = 0;
        }
        if (r->f[i].off + r->f[i].size > end) end = r->f[i].off + r->f[i].size;
    }
    if (same) { fprintf(out, " (%d x %lld)", r->nholes, first); return; }
    fputs(" (", out);
    end = 0;
    int k = 0;
    for (int i = 0; i < r->nf; i++) {
        if (r->f[i].off > end) fprintf(out, "%s%lld", k++ ? " + " : "", r->f[i].off - end);
        if (r->f[i].off + r->f[i].size > end) end = r->f[i].off + r->f[i].size;
    }
    fputc(')', out);
}

static void print_text(FILE* out, const LRec* r) {
    fprintf(out, "%s %s  %s:%d", kind_name(r->kind), r->name, r->file, r->line);
    if (!r->known) {
        fprintf(out, "  layout unknown: %s\n", r->unknown_why ? r->unknown_why : "not modelled");
        fprintf(out, "  reorderable: no (%s)\n\n", r->reason ? r->reason : "layout unknown");
        return;
    }
    long long l64, lw;
    int wa;
    lines_of(r, &l64, &lw, &wa);
    fprintf(out, "  size %lld  align %d  ", r->size, r->align);
    print_holes(out, r);
    fprintf(out, "  tail %lld  lines %lld (64-aligned)", r->tail, l64);
    if (wa < 64) fprintf(out, " / %lld (%d-aligned)", lw, wa);
    fputc('\n', out);
    int wn = 1, wt = 1;
    for (int i = 0; i < r->nf; i++) {
        int n = (int)strlen(r->f[i].name), t = (int)strlen(r->f[i].type);
        if (n > wn) wn = n;
        if (t > wt) wt = t;
    }
    long long end = 0;
    for (int i = 0; i < r->nf; i++) {
        if (r->f[i].off > end) fprintf(out, "  %4lld  (hole %lld)\n", end, r->f[i].off - end);
        fprintf(out, "  %4lld  %-*s  %-*s  %lld\n", r->f[i].off, wn, r->f[i].name, wt, r->f[i].type,
                r->f[i].size);
        if (r->f[i].off + r->f[i].size > end) end = r->f[i].off + r->f[i].size;
    }
    if (r->tail > 0) fprintf(out, "  %4lld  (tail %lld)\n", end, r->tail);
    long long s = saving_of(r);
    if (s > 0) {
        fputs("  suggested: ", out);
        int k = 0;
        for (int i = 0; i < r->nf; i++) {
            const LField* f = &r->f[r->sug[i]];
            if (f->pin != LF_MOVABLE) continue;
            fprintf(out, "%s%s", k++ ? ", " : "", f->name);
        }
        fprintf(out, "  -> size %lld (-%lld, -%lld%%)", r->sug_size, s,
                (s * 100 + r->size / 2) / r->size);
    } else {
        fputs("  suggested: keep this order (none is smaller)", out);
    }
    if (r->reorderable) fputs("  reorderable: yes\n\n", out);
    else fprintf(out, "  reorderable: no (%s)\n\n", r->reason ? r->reason : "");
}

static void json_str(FILE* out, const char* s) {
    fputc('"', out);
    for (const unsigned char* p = (const unsigned char*)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\') fprintf(out, "\\%c", *p);
        else if (*p < 0x20) fprintf(out, "\\u%04x", *p);
        else fputc(*p, out);
    }
    fputc('"', out);
}

static void print_json(FILE* out, const LRec* r, int first) {
    fprintf(out, "%s\n    {\"kind\": \"%s\", \"name\": ", first ? "" : ",", kind_name(r->kind));
    json_str(out, r->name);
    fputs(", \"file\": ", out);
    json_str(out, r->file);
    fprintf(out, ", \"line\": %d, \"known\": %s", r->line, r->known ? "true" : "false");
    if (!r->known) {
        fputs(", \"unknown\": ", out);
        json_str(out, r->unknown_why);
    } else {
        long long l64, lw;
        int wa;
        lines_of(r, &l64, &lw, &wa);
        fprintf(out, ", \"size\": %lld, \"align\": %d, \"holes\": %lld, \"hole_count\": %d, "
                     "\"tail\": %lld, \"lines_64\": %lld, \"lines_worst\": %lld, "
                     "\"worst_align\": %d,\n     \"fields\": [",
                r->size, r->align, r->holes, r->nholes, r->tail, l64, lw, wa);
        for (int i = 0; i < r->nf; i++) {
            fprintf(out, "%s{\"name\": ", i ? ", " : "");
            json_str(out, r->f[i].name);
            fputs(", \"type\": ", out);
            json_str(out, r->f[i].type);
            fprintf(out, ", \"offset\": %lld, \"size\": %lld, \"align\": %d}", r->f[i].off,
                    r->f[i].size, r->f[i].align);
        }
        fputs("],\n     \"suggested\": [", out);
        int k = 0;
        for (int i = 0; i < r->nf; i++) {
            const LField* f = &r->f[r->sug[i]];
            if (f->pin != LF_MOVABLE) continue;
            if (k++) fputs(", ", out);
            json_str(out, f->name);
        }
        fprintf(out, "], \"suggested_size\": %lld, \"saving\": %lld", r->sug_size, saving_of(r));
    }
    fprintf(out, ", \"reorderable\": %s, \"reason\": ", r->reorderable ? "true" : "false");
    json_str(out, r->reorderable ? "" : r->reason);
    fputc('}', out);
}

/* A struct's name as its own file spells it: the display name without the
 * `module.` a program that imports it sees it under. */
static const char* source_struct_name(const char* cname) {
    const char* d = ast_struct_display_name(cname);
    const char* dot = d ? strrchr(d, '.') : NULL;
    return dot ? dot + 1 : d;
}

static void print_plan(FILE* out, const LRec* r) {
    if (r->kind != LK_STRUCT || !r->reorderable || saving_of(r) <= 0) return;
    /* The name as the source spells it, which is what --fix looks for. */
    fprintf(out, "fix\t%s\t%d\t%d\t%s\t%lld\t%lld\t", r->file, r->line, r->col,
            source_struct_name(r->name), r->size, r->sug_size);
    for (int i = 0; i < r->nf; i++) fprintf(out, "%s%s", i ? "," : "", r->f[i].name);
    fputc('\t', out);
    for (int i = 0; i < r->nf; i++) fprintf(out, "%s%s", i ? "," : "", r->f[r->sug[i]].name);
    fputc('\n', out);
}

static void print_asserts(FILE* out, const LRec* r) {
    if (!r->known) return;
    /* An empty struct is GNU C's size 0; another compiler gets a byte. */
    int gnu_only = r->size == 0;
    if (gnu_only) fputs("#if defined(__GNUC__)\n", out);
    fprintf(out, "_Static_assert(sizeof(%s) == %lld, \"%s %s: size %lld\");\n", r->name, r->size,
            kind_name(r->kind), r->name, r->size);
    fprintf(out, "_Static_assert(_Alignof(%s) == %d, \"%s %s: align %d\");\n", r->name, r->align,
            kind_name(r->kind), r->name, r->align);
    for (int i = 0; i < r->nf; i++)
        fprintf(out, "_Static_assert(offsetof(%s, %s) == %lld, \"%s.%s at %lld\");\n", r->name,
                r->f[i].name, r->f[i].off, r->name, r->f[i].name, r->f[i].off);
    if (gnu_only) fputs("#endif\n", out);
}

int codegen_layout_report(CodeGenerator* gen, ASTNode* program, const char* path,
                          const LayoutTarget* tgt, const char* target_name,
                          LayoutReportFormat format, FILE* out) {
    LMarks marks = { NULL, 0, 0 };
    collect_marks(gen, program, path, &marks);

    int cap = program->child_count > 0 ? program->child_count : 1;
    LRec* recs = (LRec*)calloc((size_t)cap, sizeof(LRec));
    if (!recs) { free(marks.v); fprintf(stderr, "aetherc: out of memory in --emit=layout\n"); return 1; }
    int n = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (!c || !c->value || !is_local(c, path)) continue;
        if (c->type != AST_STRUCT_DEFINITION && c->type != AST_MESSAGE_DEFINITION &&
            c->type != AST_ACTOR_DEFINITION) continue;
        LRec* r = &recs[n++];
        r->name = c->value;
        r->file = c->source_file ? c->source_file : (path ? path : "<input>");
        r->line = c->line;
        r->col = c->column;
        if (c->type == AST_STRUCT_DEFINITION) struct_record(c, program, tgt, r);
        else if (c->type == AST_MESSAGE_DEFINITION) message_record(c, program, tgt, r);
        else actor_record(gen, c, program, tgt, r);
        finish(r);
        if (r->kind == LK_MESSAGE)
            r->reason = "a message: codegen orders its fields by alignment (#3773), "
                        "the source is not rewritten";
        else if (r->kind == LK_ACTOR)
            r->reason = "actor state: codegen lays it out after ActorBase in declaration "
                        "order, the source is not rewritten";
        else if (marks_get(&marks, c->value))
            r->reason = marks_get(&marks, c->value);
        else if (!r->known)
            r->reason = aether_internf("layout unknown: %s", r->unknown_why ? r->unknown_why : "");
        r->reorderable = r->kind == LK_STRUCT && r->known && !r->reason;
    }

    int n_struct = 0, n_msg = 0, n_actor = 0;
    long long holes = 0, tail = 0, saving = 0, all_saving = 0, largest = 0;
    const char* largest_name = NULL;
    for (int i = 0; i < n; i++) {
        LRec* r = &recs[i];
        if (r->kind == LK_STRUCT) n_struct++;
        else if (r->kind == LK_MESSAGE) n_msg++;
        else n_actor++;
        holes += r->holes;
        tail += r->tail;
        long long s = saving_of(r);
        all_saving += s;
        if (r->reorderable) {
            saving += s;
            if (s > largest) { largest = s; largest_name = r->name; }
        }
    }

    switch (format) {
        case LAYOUT_REPORT_TEXT:
            fprintf(out, "layout: %s  target %s (pointers %d, int64 align %d, long double %d/%d)\n\n",
                    path ? path : "<input>", target_name, tgt->ptr_size, tgt->i64_align,
                    tgt->ldbl_size, tgt->ldbl_align);
            for (int i = 0; i < n; i++) print_text(out, &recs[i]);
            fprintf(out, "summary: %d struct%s, %d message%s, %d actor%s; holes %lld, tail %lld; "
                         "reorderable saving %lld (largest %lld%s%s); all saving %lld\n",
                    n_struct, n_struct == 1 ? "" : "s", n_msg, n_msg == 1 ? "" : "s",
                    n_actor, n_actor == 1 ? "" : "s", holes, tail, saving, largest,
                    largest_name ? " in " : "", largest_name ? largest_name : "", all_saving);
            break;
        case LAYOUT_REPORT_JSON:
            fputs("{\"file\": ", out);
            json_str(out, path);
            fputs(", \"target\": {\"name\": ", out);
            json_str(out, target_name);
            fprintf(out, ", \"ptr\": %d, \"int64_align\": %d, \"long_double\": [%d, %d]},\n"
                         "  \"types\": [", tgt->ptr_size, tgt->i64_align, tgt->ldbl_size,
                    tgt->ldbl_align);
            for (int i = 0; i < n; i++) print_json(out, &recs[i], i == 0);
            fprintf(out, "],\n  \"summary\": {\"structs\": %d, \"messages\": %d, \"actors\": %d, "
                         "\"holes\": %lld, \"tail\": %lld, \"reorderable_saving\": %lld, "
                         "\"largest_saving\": %lld, \"largest_name\": ",
                    n_struct, n_msg, n_actor, holes, tail, saving, largest);
            json_str(out, largest_name ? largest_name : "");
            fprintf(out, ", \"all_saving\": %lld}}\n", all_saving);
            break;
        case LAYOUT_REPORT_PLAN: {
            /* `ae pad --fix` decides over every file of a run: a struct a
             * module declares may be one a program that imports it hands
             * to C. So the plan also says whether this file is a program,
             * which other files' declarations it sees, and every struct it
             * refuses, wherever that struct is declared. */
            int has_main = 0;
            for (int i = 0; i < program->child_count; i++) {
                ASTNode* c = program->children[i];
                if (c && is_local(c, path) &&
                    (c->type == AST_MAIN_FUNCTION ||
                     (c->type == AST_FUNCTION_DEFINITION && c->value && !strcmp(c->value, "main"))))
                    has_main = 1;
            }
            fprintf(out, "file\t%s\t%d\n", path ? path : "", has_main);
            for (int i = 0; i < program->child_count; i++) {
                ASTNode* c = program->children[i];
                if (!c || !c->source_file || is_local(c, path)) continue;
                int seen = 0;
                for (int j = 0; j < i && !seen; j++) {
                    ASTNode* d = program->children[j];
                    seen = d && d->source_file && !is_local(d, path) &&
                           strcmp(d->source_file, c->source_file) == 0;
                }
                if (!seen) fprintf(out, "uses\t%s\n", c->source_file);
            }
            for (int i = 0; i < marks.n; i++) {
                ASTNode* sd = def_named(program, AST_STRUCT_DEFINITION, marks.v[i].name);
                const char* file = sd && sd->source_file ? sd->source_file : (path ? path : "");
                fprintf(out, "refuse\t%s\t%s\t%s\n", file, source_struct_name(marks.v[i].name),
                        marks.v[i].reason);
            }
            for (int i = 0; i < n; i++) print_plan(out, &recs[i]);
            break;
        }
        case LAYOUT_REPORT_ASSERTS:
            fprintf(out, "\n/* ae pad --verify: the layout aetherc --emit=layout reports for %s "
                         "(target %s), checked against this C compiler. */\n",
                    path ? path : "<input>", target_name);
            /* The C before this carries the .ae file's #line numbers; a
             * failed assert names itself instead. */
            fputs("#line 1 \"ae-pad-verify\"\n#include <stddef.h>\n", out);
            for (int i = 0; i < n; i++) print_asserts(out, &recs[i]);
            break;
    }

    for (int i = 0; i < n; i++) { free(recs[i].f); free(recs[i].sug); }
    free(recs);
    free(marks.v);
    return 0;
}
