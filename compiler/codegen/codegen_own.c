/* R2/R3 for every owning type, generated once per type (#2921).
 *
 * A value of an owning type holds counted parts: strings, closure
 * environments, and composites (structs, tuples, optionals, fixed-size
 * arrays) of those. Every place that holds one does
 * the same things with it, through functions generated for its type:
 *
 *   _ae_retain_<M>(T* p)        take a reference to every part of *p in
 *                               place: a bit copy becomes a value of its own
 *   _ae_drop_<M>(T* p)          release every part of *p and empty it
 *   _ae_publish_<M>(T* p)       publish every string part, for a place
 *                               other threads read (#2919)
 *   _ae_drop_unwind_<M>(p)      the drop the unwind journal runs on a place
 *                               a panic leaves (#2923)
 *
 * A fixed-size array's take the address of its first element. A struct's
 * are its _aether_copy_<Name> / _aether_drop_<Name> (codegen_func.c), which this file
 * calls for a struct part. Every type that reaches a place is registered by
 * the walk over the program (own_collect_types) and the functions are
 * emitted once all types are complete (emit_own_ops), prototypes first, so
 * nested composites call each other in any order. */

#include "codegen_internal.h"

typedef struct {
    Type* type;
    const char* mangled;
} OwnType;

static OwnType* g_own_types = NULL;
static int g_own_count = 0;
static int g_own_cap = 0;
static int g_own_emitted = 0;

static int is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_';
}

static const char* sanitize(const char* s) {
    size_t n = strlen(s);
    char* out = (char*)aether_xrealloc(NULL, n + 1);
    for (size_t i = 0; i < n; i++) out[i] = is_ident_char(s[i]) ? s[i] : '_';
    const char* r = cg_intern_n(out, n);
    free(out);
    return r;
}

static const char* own_mangle(Type* t) {
    if (t->kind == TYPE_ARRAY)
        return cg_internf("arr%d_%s", t->array_size, own_mangle(t->element_type));
    if (t->kind == TYPE_STRING) return "str";
    if (t->kind == TYPE_FUNCTION) return "fn";
    return sanitize(get_c_type(t));
}

int own_type_owns(CodeGenerator* gen, Type* t) {
    if (!t) return 0;
    switch (t->kind) {
    case TYPE_STRING:
        return 1;
    case TYPE_FUNCTION:
        return !t->is_fnptr;
    case TYPE_STRUCT:
        return struct_owning_strings(gen, t) != NULL;
    case TYPE_TUPLE:
        for (int i = 0; t->tuple_types && i < t->tuple_count; i++)
            if (own_type_owns(gen, t->tuple_types[i])) return 1;
        return 0;
    case TYPE_OPTIONAL:
        return own_type_owns(gen, t->element_type);
    case TYPE_ARRAY:
        return t->array_size > 0 && own_type_owns(gen, t->element_type);
    case TYPE_PTR:
        return is_string_seq_ptr_type(t);
    default:
        return 0;
    }
}

/* A composite whose functions this file emits (a struct has its own). */
static int own_is_composite(Type* t) {
    return t->kind == TYPE_TUPLE || t->kind == TYPE_OPTIONAL ||
           (t->kind == TYPE_ARRAY && t->array_size > 0);
}

static void own_register(CodeGenerator* gen, Type* t) {
    if (!t || !own_is_composite(t) || !own_type_owns(gen, t)) return;
    const char* m = own_mangle(t);
    for (int i = 0; i < g_own_count; i++)
        if (strcmp(g_own_types[i].mangled, m) == 0) return;
    if (g_own_emitted) {
        fprintf(stderr, "aetherc: internal error: the ownership functions of %s were "
                        "asked for after they were emitted\n", get_c_type(t));
        exit(1);
    }
    /* Parts first, so a part's functions are declared before its holder's
     * (the prototypes make the order free, the registry stays readable). */
    if (t->kind == TYPE_TUPLE) {
        for (int i = 0; t->tuple_types && i < t->tuple_count; i++)
            own_register(gen, t->tuple_types[i]);
    } else {
        own_register(gen, t->element_type);
    }
    if (g_own_count == g_own_cap) {
        g_own_cap = g_own_cap ? g_own_cap * 2 : 16;
        g_own_types = (OwnType*)aether_xrealloc(g_own_types, (size_t)g_own_cap * sizeof(OwnType));
    }
    g_own_types[g_own_count].type = clone_type(t);
    g_own_types[g_own_count].mangled = m;
    g_own_count++;
}

static void own_collect_type(CodeGenerator* gen, Type* t, int depth) {
    if (!t || depth > 16) return;
    own_register(gen, t);
    if (t->kind == TYPE_TUPLE) {
        for (int i = 0; t->tuple_types && i < t->tuple_count; i++)
            own_collect_type(gen, t->tuple_types[i], depth + 1);
    } else if (t->kind == TYPE_FUNCTION) {
        for (int i = 0; t->param_types && i < t->param_count; i++)
            own_collect_type(gen, t->param_types[i], depth + 1);
        own_collect_type(gen, t->return_type, depth + 1);
    } else {
        own_collect_type(gen, t->element_type, depth + 1);
    }
}

void own_collect_types(CodeGenerator* gen, ASTNode* node) {
    if (!node) return;
    own_collect_type(gen, node->node_type, 0);
    for (int i = 0; i < node->child_count; i++) own_collect_types(gen, node->children[i]);
}

static OwnType* own_find(CodeGenerator* gen, Type* t) {
    const char* m = own_mangle(t);
    for (int i = 0; i < g_own_count; i++)
        if (strcmp(g_own_types[i].mangled, m) == 0) return &g_own_types[i];
    fprintf(stderr, "aetherc: internal error: no ownership functions for %s\n", get_c_type(t));
    (void)gen;
    exit(1);
}

/* The C parameter type the functions of `t` take: a pointer to the value,
 * or to the first element of an array. */
static const char* own_ptr_type(Type* t) {
    if (t->kind == TYPE_ARRAY) return cg_internf("%s*", get_c_type(t->element_type));
    return cg_internf("%s*", get_c_type(t));
}

/* One statement on the part of type `pt` at lvalue `lv`: OWN_RETAIN takes a
 * reference to it, OWN_DROP releases it and empties the part, OWN_ZERO
 * empties it without a release (what a move leaves behind), OWN_PUBLISH
 * makes its strings' counts atomic, for a place other threads read
 * (#2919; a closure environment's count is atomic already). */
void own_emit_part(CodeGenerator* gen, Type* pt, const char* lv, OwnOp op) {
    FILE* o = gen->output;
    if (!own_type_owns(gen, pt)) return;
    switch (pt->kind) {
    case TYPE_STRING:
        if (op == OWN_RETAIN) fprintf(o, "    %s = aether_uniform_heap_str(%s, 0);\n", lv, lv);
        else if (op == OWN_DROP) fprintf(o, "    aether_heap_str_free(%s); %s = (const char*)0;\n", lv, lv);
        else if (op == OWN_PUBLISH) fprintf(o, "    aether_string_publish(%s);\n", lv);
        else fprintf(o, "    %s = (const char*)0;\n", lv);
        return;
    case TYPE_FUNCTION:
        if (op == OWN_PUBLISH) return;
        if (op == OWN_RETAIN) fprintf(o, "    _aether_closure_env_retain(%s.env);\n", lv);
        else if (op == OWN_DROP)
            fprintf(o, "    _aether_closure_env_release(%s.env); %s.env = (void*)0;\n", lv, lv);
        else fprintf(o, "    %s.env = (void*)0;\n", lv);
        return;
    case TYPE_PTR:   /* a string sequence: its cells are counted */
        if (op == OWN_RETAIN) fprintf(o, "    %s = string_seq_retain(%s);\n", lv, lv);
        else if (op == OWN_DROP) fprintf(o, "    string_seq_free(%s); %s = (StringSeq*)0;\n", lv, lv);
        else if (op == OWN_ZERO) fprintf(o, "    %s = (StringSeq*)0;\n", lv);
        return;
    case TYPE_STRUCT: {
        const char* s = struct_owning_strings(gen, pt);
        if (op == OWN_RETAIN) fprintf(o, "    %s = _aether_copy_%s(%s);\n", lv, s, lv);
        else if (op == OWN_DROP) fprintf(o, "    _aether_drop_%s(&%s);\n", s, lv);
        else if (op == OWN_PUBLISH) fprintf(o, "    %s_publish(&%s);\n", s, lv);
        else fprintf(o, "    (void)_aether_move_%s(&%s);\n", s, lv);
        return;
    }
    default: {
        if (op == OWN_ZERO) {
            fprintf(o, "    memset(&%s, 0, sizeof(%s));\n", lv, lv);
            return;
        }
        OwnType* ot = own_find(gen, pt);
        fprintf(o, "    _ae_%s_%s(%s%s);\n",
                op == OWN_RETAIN ? "retain" : op == OWN_DROP ? "drop" : "publish", ot->mangled,
                pt->kind == TYPE_ARRAY ? "" : "&", lv);
        return;
    }
    }
}

/* The prototypes of the functions of composite type `t`, for a struct's
 * functions that call them ahead of the definitions (emit_own_ops). */
void own_emit_prototypes(CodeGenerator* gen, Type* t) {
    if (!t || !own_is_composite(t) || !own_type_owns(gen, t)) return;
    OwnType* ot = own_find(gen, t);
    const char* pt = own_ptr_type(ot->type);
    fprintf(gen->output, "static inline AETHER_MAYBE_UNUSED void _ae_retain_%s(%s p);\n",
            ot->mangled, pt);
    fprintf(gen->output, "static inline AETHER_MAYBE_UNUSED void _ae_drop_%s(%s p);\n",
            ot->mangled, pt);
    fprintf(gen->output, "static inline AETHER_MAYBE_UNUSED void _ae_publish_%s(%s p);\n",
            ot->mangled, pt);
}

static void own_emit_body(CodeGenerator* gen, Type* t, OwnOp op) {
    FILE* o = gen->output;
    switch (t->kind) {
    case TYPE_TUPLE:
        for (int i = 0; t->tuple_types && i < t->tuple_count; i++)
            own_emit_part(gen, t->tuple_types[i], cg_internf("p->_%d", i), op);
        return;
    case TYPE_OPTIONAL:
        fprintf(o, "  if (p->has) {\n");
        own_emit_part(gen, t->element_type, "p->val", op);
        if (op == OWN_DROP) fprintf(o, "    p->has = 0;\n");
        fprintf(o, "  }\n");
        return;
    case TYPE_ARRAY:
        fprintf(o, "  for (int _i = 0; _i < %d; _i++) {\n", t->array_size);
        own_emit_part(gen, t->element_type, "p[_i]", op);
        fprintf(o, "  }\n");
        return;
    default:
        return;
    }
}

void emit_own_ops(CodeGenerator* gen) {
    g_own_emitted = 1;
    if (g_own_count == 0) return;
    FILE* o = gen->output;
    /* A tuple or optional reached only inside a body would get its typedef
     * there, out of the scope these functions are written in. */
    for (int i = 0; i < g_own_count; i++) {
        if (g_own_types[i].type->kind == TYPE_TUPLE) ensure_tuple_typedef(gen, g_own_types[i].type);
        else if (g_own_types[i].type->kind == TYPE_OPTIONAL)
            ensure_optional_typedef(gen, g_own_types[i].type);
    }
    fprintf(o, "\n/* Ownership of composite values (#2921): retain and drop every part. */\n");
    for (int i = 0; i < g_own_count; i++) {
        OwnType* ot = &g_own_types[i];
        const char* pt = own_ptr_type(ot->type);
        fprintf(o, "static inline AETHER_MAYBE_UNUSED void _ae_retain_%s(%s p);\n", ot->mangled, pt);
        fprintf(o, "static inline AETHER_MAYBE_UNUSED void _ae_drop_%s(%s p);\n", ot->mangled, pt);
        fprintf(o, "static inline AETHER_MAYBE_UNUSED void _ae_publish_%s(%s p);\n", ot->mangled, pt);
    }
    for (int i = 0; i < g_own_count; i++) {
        OwnType* ot = &g_own_types[i];
        const char* pt = own_ptr_type(ot->type);
        fprintf(o, "static inline AETHER_MAYBE_UNUSED void _ae_retain_%s(%s p) {\n", ot->mangled, pt);
        own_emit_body(gen, ot->type, OWN_RETAIN);
        fprintf(o, "}\n");
        fprintf(o, "static inline AETHER_MAYBE_UNUSED void _ae_drop_%s(%s p) {\n", ot->mangled, pt);
        own_emit_body(gen, ot->type, OWN_DROP);
        fprintf(o, "}\n");
        fprintf(o, "static inline AETHER_MAYBE_UNUSED void _ae_publish_%s(%s p) {\n", ot->mangled, pt);
        own_emit_body(gen, ot->type, OWN_PUBLISH);
        fprintf(o, "}\n");
        fprintf(o, "static AETHER_MAYBE_UNUSED void _ae_drop_unwind_%s(const void* p) {\n", ot->mangled);
        fprintf(o, "    _ae_drop_%s((%s)(void*)p);\n", ot->mangled, pt);
        fprintf(o, "}\n");
        /* A shared cell holding one (a variable a closure writes): the last
         * holder drops what it holds, then frees the cell (#2458). */
        fprintf(o, "static AETHER_MAYBE_UNUSED void _ae_cell_release_%s(const void* cell) {\n", ot->mangled);
        fprintf(o, "    if (!cell) return;\n");
        fprintf(o, "    _AeCellHeader* h = (_AeCellHeader*)cell - 1;\n");
        fprintf(o, "    if (_aether_cell_last(h)) { _ae_drop_%s((%s)(void*)cell); free(h); }\n", ot->mangled, pt);
        fprintf(o, "}\n");
    }
}

/* The cell release of the composite whose C type is spelled `c_type` (a
 * cell's type is known by its spelling, promoted_cell_release_fn), or NULL
 * when no owning composite has it. */
const char* own_cell_release_fn(const char* c_type) {
    for (int i = 0; c_type && i < g_own_count; i++)
        if (strcmp(get_c_type(g_own_types[i].type), c_type) == 0)
            return cg_internf("_ae_cell_release_%s", g_own_types[i].mangled);
    return NULL;
}

/* The local `name` of owning type `t` as an lvalue own_emit_part takes: a
 * volatile local (one a `try` body writes, #2690) seen through a plain
 * pointer, as the generated functions take it. */
const char* own_place_lvalue(Type* t, const char* name) {
    if (t->kind == TYPE_ARRAY) return cg_internf("((%s*)%s)", get_c_type(t->element_type), name);
    return cg_internf("(*(%s*)&%s)", get_c_type(t), name);
}

/* The drop the unwind journal runs on a place of owning type `t` (#2923). */
const char* own_unwind_fn(CodeGenerator* gen, Type* t) {
    if (t->kind == TYPE_FUNCTION) return "_aether_closure_drop_unwind";
    if (t->kind == TYPE_PTR) return "_aether_seq_drop_unwind";
    const char* s = struct_owning_strings(gen, t);
    if (s) return cg_internf("_aether_udrop_%s", s);
    return own_fn(gen, t, "drop_unwind");
}

const char* own_fn(CodeGenerator* gen, Type* t, const char* op) {
    return cg_internf("_ae_%s_%s", op, own_find(gen, t)->mangled);
}

void own_reset(void) {
    for (int i = 0; i < g_own_count; i++) free_type(g_own_types[i].type);
    free(g_own_types);
    g_own_types = NULL;
    g_own_count = g_own_cap = 0;
    g_own_emitted = 0;
}
