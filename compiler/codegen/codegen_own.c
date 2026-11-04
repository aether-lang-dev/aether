/* R2/R3 for every owning type, generated once per type (#2921).
 *
 * A value of an owning type holds counted parts: strings, closure
 * environments, and composites (structs, tuples, optionals, fixed-size
 * arrays, sums) of those. Every place that holds one does
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
 * calls for a struct part. A sum's apply its variant struct's to the variant
 * the tag names (#3005): a sum owns what its variants own (R1). Every type
 * that reaches a place is registered by the walk over the program
 * (own_collect_types) and the functions are emitted once all types are
 * complete (emit_own_ops), prototypes first, so nested composites call each
 * other in any order. */

#include "codegen_internal.h"

typedef struct {
    Type* type;
    const char* mangled;
} OwnType;

/* Slice ownership: a slice type whose elements are slices that own
 * something of their own (strings, or slices again) has its element drop
 * generated, `_ae_sdrop_<M>(p)`, the drop of one element at `p`
 * (own_slice_elem_drop_fn). */
static Type** g_sdrop_types = NULL;
static int g_sdrop_count = 0;
static int g_sdrop_cap = 0;

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

/* A sum's variants (#3005), as its definition names them, wherever the Type
 * came from (one the type checker did not fill has no variant list):
 * own_sum_arity counts them, own_sum_variant gives the i-th one's name when
 * that variant struct owns, NULL when it does not. */
static ASTNode* own_sum_def(CodeGenerator* gen, Type* t) {
    if (!gen || !gen->program || !t || t->kind != TYPE_SUM || !t->struct_name) return NULL;
    return find_sum_definition_by_name(gen->program, t->struct_name);
}

static int own_sum_arity(CodeGenerator* gen, Type* t) {
    ASTNode* def = own_sum_def(gen, t);
    return def ? def->child_count : 0;
}

static const char* own_sum_variant(CodeGenerator* gen, Type* t, int i) {
    ASTNode* def = own_sum_def(gen, t);
    ASTNode* v = def && i < def->child_count ? def->children[i] : NULL;
    if (!v || v->type != AST_IDENTIFIER || !v->value) return NULL;
    ASTNode* sd = find_struct_definition_by_name(gen->program, v->value);
    return sd && struct_owns_heap_strings(gen, sd) ? v->value : NULL;
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
    case TYPE_SUM:
        for (int i = 0; i < own_sum_arity(gen, t); i++)
            if (own_sum_variant(gen, t, i)) return 1;
        return 0;
    default:
        return 0;
    }
}

/* A composite whose functions this file emits (a struct has its own). */
static int own_is_composite(Type* t) {
    return t->kind == TYPE_TUPLE || t->kind == TYPE_OPTIONAL || t->kind == TYPE_SUM ||
           (t->kind == TYPE_ARRAY && t->array_size > 0);
}

/* An owning composite C holds as a value (a tuple, an optional, a sum):
 * every place takes, copies and drops one by the functions this file
 * emits for its type, whichever of those kinds it is (#3005). */
int own_value_composite(CodeGenerator* gen, Type* t) {
    return t && (t->kind == TYPE_TUPLE || t->kind == TYPE_OPTIONAL || t->kind == TYPE_SUM) &&
           own_type_owns(gen, t);
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
    } else if (t->kind != TYPE_SUM) {   /* a sum's parts are structs */
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

static const char* own_slice_mangle(Type* t) {
    if (type_is_slice(t)) return cg_internf("s_%s", own_slice_mangle(t->element_type));
    return own_mangle(t);
}

static void own_register_sdrop(CodeGenerator* gen, Type* t);
static void emit_slice_drops(CodeGenerator* gen);

static void own_collect_type(CodeGenerator* gen, Type* t, int depth) {
    if (!t || depth > 16) return;
    own_register(gen, t);
    if (type_is_slice(t) && t->element_type && type_is_slice(t->element_type))
        own_register_sdrop(gen, t->element_type);
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
    /* #3549: a slice made for a call (slice ownership's `slice_temp`) is
     * dropped by the call's argument temporaries, through the drop of a
     * whole slice of its type. */
    if (node->type == AST_FUNCTION_CALL && type_is_slice(node->node_type) &&
        annotation_has_marker(node->annotation, "slice_temp"))
        own_register_sdrop(gen, node->node_type);
    /* #3549: and a cell that owns its slice drops it through the same. */
    if (node->type == AST_VARIABLE_DECLARATION && type_is_slice(node->node_type) &&
        annotation_has_marker(node->annotation, "slice_cell"))
        own_register_sdrop(gen, node->node_type);
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

/* OWN_PUBLISH of the part of type `pt` at lvalue `lv` as one call, for an
 * expression as well as a statement, or NULL when it has nothing to publish
 * (a closure's environment is counted atomically already; a string
 * sequence's cells are not published, as own_emit_part does not). */
const char* own_publish_call(CodeGenerator* gen, Type* pt, const char* lv) {
    if (!own_type_owns(gen, pt)) return NULL;
    switch (pt->kind) {
    case TYPE_STRING:
        return cg_internf("aether_string_publish(%s)", lv);
    case TYPE_FUNCTION:
    case TYPE_PTR:
        return NULL;
    case TYPE_STRUCT:
        return cg_internf("%s_publish(&%s)", struct_owning_strings(gen, pt), lv);
    default:
        return cg_internf("_ae_publish_%s(%s%s)", own_find(gen, pt)->mangled,
                          pt->kind == TYPE_ARRAY ? "" : "&", lv);
    }
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
            /* (void*): cl.exe reads the `const` of a `const char*` array's
             * elements as the array's (C4090, #2799). */
            fprintf(o, "    memset((void*)&%s, 0, sizeof(%s));\n", lv, lv);
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
    case TYPE_SUM:
        /* The variant the tag names, by its struct's functions (#3005). */
        fprintf(o, "  switch (p->tag) {\n");
        for (int i = 0; i < own_sum_arity(gen, t); i++) {
            const char* v = own_sum_variant(gen, t, i);
            if (!v) continue;
            Type* vt = create_type(TYPE_STRUCT);
            vt->struct_name = strdup(v);
            fprintf(o, "  case %s__%s: {\n", t->struct_name, v);
            own_emit_part(gen, vt, cg_internf("p->data.%s_", v), op);
            fprintf(o, "    break;\n  }\n");
            free_type(vt);
        }
        fprintf(o, "  default: break;\n  }\n");
        return;
    default:
        return;
    }
}

void emit_own_ops(CodeGenerator* gen) {
    if (g_own_count == 0) {
        emit_slice_drops(gen);
        g_own_emitted = 1;
        return;
    }
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
    emit_slice_drops(gen);
    g_own_emitted = 1;
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
    if (t->kind == TYPE_STRING) return "_aether_string_drop_unwind";
    if (t->kind == TYPE_FUNCTION) return "_aether_closure_drop_unwind";
    if (t->kind == TYPE_PTR) return "_aether_seq_drop_unwind";
    const char* s = struct_owning_strings(gen, t);
    if (s) return cg_internf("_aether_udrop_%s", s);
    return own_fn(gen, t, "drop_unwind");
}

const char* own_fn(CodeGenerator* gen, Type* t, const char* op) {
    return cg_internf("_ae_%s_%s", op, own_find(gen, t)->mangled);
}

/* The drop of one element of a slice whose elements are of type `et`, as
 * aether_slice_drop takes it: NULL when an element owns nothing, the
 * element type's unwind drop when it is counted (a string, a closure, a
 * struct or composite that holds one), and for a slice element the drop of
 * that slice (its rows are owned by the slice that holds them). */
const char* own_slice_elem_drop_fn(CodeGenerator* gen, Type* et) {
    if (!et) return "NULL";
    if (type_is_slice(et)) {
        const char* inner = own_slice_elem_drop_fn(gen, et->element_type);
        if (strcmp(inner, "NULL") == 0) return "aether_slice_drop_plain";
        own_register_sdrop(gen, et);
        return cg_internf("_ae_sdrop_%s", own_slice_mangle(et));
    }
    if (et->kind == TYPE_ARRAY) return "NULL";   /* a slice of arrays is not grown */
    if (!own_type_owns(gen, et)) return "NULL";
    return own_unwind_fn(gen, et);
}

static void own_register_sdrop(CodeGenerator* gen, Type* t) {
    if (!t || !type_is_slice(t) || !t->element_type) return;
    /* A slice of plain elements is dropped by aether_slice_drop_plain. */
    if (strcmp(own_slice_elem_drop_fn(gen, t->element_type), "NULL") == 0) return;
    const char* m = own_slice_mangle(t);
    for (int i = 0; i < g_sdrop_count; i++)
        if (strcmp(own_slice_mangle(g_sdrop_types[i]), m) == 0) return;
    if (g_own_emitted) {
        fprintf(stderr, "aetherc: internal error: the slice drop of %s was asked for after "
                        "it was emitted\n", get_c_type(t));
        exit(1);
    }
    if (g_sdrop_count == g_sdrop_cap) {
        g_sdrop_cap = g_sdrop_cap ? g_sdrop_cap * 2 : 8;
        g_sdrop_types = (Type**)aether_xrealloc(g_sdrop_types,
                                                (size_t)g_sdrop_cap * sizeof(Type*));
    }
    g_sdrop_types[g_sdrop_count++] = clone_type(t);
}

/* The element drops own_slice_elem_drop_fn names, prototypes first. */
static void emit_slice_drops(CodeGenerator* gen) {
    FILE* o = gen->output;
    if (g_sdrop_count == 0) return;
    fprintf(o, "\n/* Slice ownership: the drop of a slice that is an element of another. */\n");
    for (int i = 0; i < g_sdrop_count; i++)
        fprintf(o, "static AETHER_MAYBE_UNUSED void _ae_sdrop_%s(const void* p);\n",
                own_slice_mangle(g_sdrop_types[i]));
    for (int i = 0; i < g_sdrop_count; i++) {
        Type* t = g_sdrop_types[i];
        fprintf(o, "static AETHER_MAYBE_UNUSED void _ae_sdrop_%s(const void* p) {\n",
                own_slice_mangle(t));
        fprintf(o, "    aether_slice_drop((AetherSlice*)(void*)p, sizeof(%s), %s);\n",
                get_c_type(t->element_type), own_slice_elem_drop_fn(gen, t->element_type));
        fprintf(o, "}\n");
    }
}

void own_reset(void) {
    for (int i = 0; i < g_sdrop_count; i++) free_type(g_sdrop_types[i]);
    free(g_sdrop_types);
    g_sdrop_types = NULL;
    g_sdrop_count = g_sdrop_cap = 0;
    for (int i = 0; i < g_own_count; i++) free_type(g_own_types[i].type);
    free(g_own_types);
    g_own_types = NULL;
    g_own_count = g_own_cap = 0;
    g_own_emitted = 0;
}
