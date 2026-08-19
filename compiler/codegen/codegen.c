#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include "codegen_internal.h"
#include "aether_stdlib_symbols.h"   /* #1366: generated */
#include "../aether_module.h"
#include "../aether_error.h"
#include "../analysis/actor_reply.h"

/* Set of struct names declared `extern struct Name @c_import`.
 * aetherc does not emit typedefs for these because the C header owns the
 * layout. Some headers, such as <time.h> for `struct tm`, also do not ship
 * `typedef struct Name Name;`, so bare `Name*` is not portable. */
static char** g_c_import_struct_names = NULL;
static int g_c_import_struct_count = 0;
static int g_c_import_struct_capacity = 0;

void aether_register_c_import_struct(const char* name) {
    if (!name) return;
    for (int i = 0; i < g_c_import_struct_count; i++) {
        if (strcmp(g_c_import_struct_names[i], name) == 0) return;
    }
    if (g_c_import_struct_count >= g_c_import_struct_capacity) {
        int new_capacity = g_c_import_struct_capacity ? g_c_import_struct_capacity * 2 : 8;
        char** grown = (char**)realloc(g_c_import_struct_names,
            (size_t)new_capacity * sizeof(char*));
        if (!grown) {
            fprintf(stderr, "aetherc: out of memory registering @c_import struct\n");
            exit(1);
        }
        g_c_import_struct_names = grown;
        g_c_import_struct_capacity = new_capacity;
    }
    char* copy = strdup(name);
    if (!copy) {
        fprintf(stderr, "aetherc: out of memory registering @c_import struct\n");
        exit(1);
    }
    g_c_import_struct_names[g_c_import_struct_count++] = copy;
}

int aether_is_c_import_struct(const char* name) {
    if (!name) return 0;
    for (int i = 0; i < g_c_import_struct_count; i++) {
        if (strcmp(g_c_import_struct_names[i], name) == 0) return 1;
    }
    return 0;
}

/* ---- #891 @c_struct typed overlay registry ---------------------------------
 * A @c_struct is a pure-Aether typed lens over a raw `ptr`: each field carries
 * an explicit byte offset and a type, and field access lowers to a
 * width-correct mem_get_* and mem_set_* at that offset (NOT a C `->field`, so no C
 * struct is declared or #include'd). Killing the hand-picked-width footgun
 * (#868) is the point: the accessor width is DERIVED from the field type.
 *
 * `width` is a short token naming the mem accessor family: "byte", "int16",
 * "uint16", "int", "uint32", "long", "u64", "float32", "float64", "ptr".
 * A field whose `nested` names another @c_struct is a sub-overlay: `a.b.c`
 * adds the offsets and resolves the leaf width. */
typedef struct { char* name; char* width; long offset; char* nested; } CStructField;
typedef struct { char* name; CStructField* fields; int nfields; int cap; } CStructDef;
static CStructDef* g_cstructs = NULL;
static int g_cstruct_count = 0, g_cstruct_cap = 0;

static CStructDef* cstruct_find(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < g_cstruct_count; i++)
        if (strcmp(g_cstructs[i].name, name) == 0) return &g_cstructs[i];
    return NULL;
}

int aether_is_c_struct_overlay(const char* name) {
    return cstruct_find(name) != NULL;
}

void aether_register_c_struct(const char* name) {
    if (!name || cstruct_find(name)) return;
    if (g_cstruct_count >= g_cstruct_cap) {
        int nc = g_cstruct_cap ? g_cstruct_cap * 2 : 8;
        CStructDef* g = (CStructDef*)realloc(g_cstructs, (size_t)nc * sizeof(CStructDef));
        if (!g) { fprintf(stderr, "aetherc: OOM registering @c_struct\n"); exit(1); }
        g_cstructs = g; g_cstruct_cap = nc;
    }
    CStructDef* d = &g_cstructs[g_cstruct_count++];
    d->name = strdup(name); d->fields = NULL; d->nfields = 0; d->cap = 0;
}

void aether_c_struct_add_field(const char* sname, const char* fname,
                               const char* width, long offset, const char* nested) {
    CStructDef* d = cstruct_find(sname);
    if (!d) return;
    if (d->nfields >= d->cap) {
        int nc = d->cap ? d->cap * 2 : 8;
        CStructField* f = (CStructField*)realloc(d->fields, (size_t)nc * sizeof(CStructField));
        if (!f) { fprintf(stderr, "aetherc: OOM adding @c_struct field\n"); exit(1); }
        d->fields = f; d->cap = nc;
    }
    CStructField* f = &d->fields[d->nfields++];
    f->name = strdup(fname);
    f->width = width ? strdup(width) : NULL;
    f->offset = offset;
    f->nested = nested ? strdup(nested) : NULL;
}

/* Given a member-access chain `root.f1.f2...fN` whose ultimate receiver is a
 * @c_struct overlay pointer, return that root receiver node and write the
 * dotted field path ("f1.f2...fN") into `out`. Returns NULL if the chain's
 * root receiver is not a @c_struct overlay pointer (caller falls back to
 * ordinary member access). `macc` must be an AST_MEMBER_ACCESS node. */
ASTNode* aether_c_struct_chain(ASTNode* macc, char* out, size_t outsz) {
    if (!macc || macc->type != AST_MEMBER_ACCESS) return NULL;
    const char* parts[32];
    int n = 0;
    ASTNode* cur = macc;
    while (cur && cur->type == AST_MEMBER_ACCESS && cur->value && cur->child_count > 0) {
        if (n < 32) parts[n++] = cur->value;
        cur = cur->children[0];
    }
    if (!cur || !cur->node_type || cur->node_type->kind != TYPE_PTR ||
        !cur->node_type->element_type ||
        cur->node_type->element_type->kind != TYPE_STRUCT ||
        !cur->node_type->element_type->struct_name ||
        !aether_is_c_struct_overlay(cur->node_type->element_type->struct_name))
        return NULL;
    out[0] = '\0';
    size_t used = 0;
    for (int i = n - 1; i >= 0; i--) {
        size_t pl = strlen(parts[i]);
        if (used + pl + 2 >= outsz) break;
        if (used) out[used++] = '.';
        memcpy(out + used, parts[i], pl);
        used += pl;
        out[used] = '\0';
    }
    return cur;
}

/* Predicate: is this member-access node a write/read against a @c_struct
 * overlay (its chain root is an overlay pointer)? */
int aether_c_struct_overlay_lhs(ASTNode* macc) {
    char tmp[256];
    return aether_c_struct_chain(macc, tmp, sizeof(tmp)) != NULL;
}

/* Resolve `struct.field` (field may be a dotted chain for nested overlays)
 * to a (cumulative offset, leaf width token). Returns 1 on success. */
int aether_c_struct_resolve(const char* sname, const char* field,
                            long* out_offset, const char** out_width) {
    CStructDef* d = cstruct_find(sname);
    if (!d || !field) return 0;
    long acc = 0;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", field);
    char* seg = buf;
    while (seg && *seg) {
        char* dot = strchr(seg, '.');
        if (dot) *dot = '\0';
        CStructField* found = NULL;
        for (int i = 0; i < d->nfields; i++)
            if (strcmp(d->fields[i].name, seg) == 0) { found = &d->fields[i]; break; }
        if (!found) return 0;
        acc += found->offset;
        if (dot) {
            /* must descend into a nested overlay */
            if (!found->nested) return 0;
            d = cstruct_find(found->nested);
            if (!d) return 0;
            seg = dot + 1;
        } else {
            if (found->nested) return 0;  /* leaf access on a nested field */
            *out_offset = acc;
            *out_width = found->width;
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * #1132 bitstruct registry.
 *
 * A bitstruct is a named layout over one unsigned integer. There is no C struct
 * and no C bitfield: `b.field` lowers to a shift/mask against the backing word,
 * and `b.field = v` to a read-modify-write. That is the whole reason the feature
 * exists — a C bitfield's signedness and allocation order are implementation-
 * defined (gcc makes `int x : 3` SIGNED, so 0b111 reads back as -1), which makes
 * C bitfields unusable for wire formats and is the bug this replaces.
 *
 * Fields are stored with their INCLUSIVE bit range; the parser has already
 * normalised `..<` to `..=` so nothing downstream cares which was written.
 * ------------------------------------------------------------------------- */
typedef struct { char* name; int lo; int hi; int is_bool; } BitStructField;
typedef struct {
    char* name;
    char* backing;        /* the C type of the storage word */
    BitStructField* fields;
    int nfields, cap;
} BitStructDef;
static BitStructDef* g_bitstructs = NULL;
static int g_bitstruct_count = 0, g_bitstruct_cap = 0;

static BitStructDef* bitstruct_find(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < g_bitstruct_count; i++)
        if (strcmp(g_bitstructs[i].name, name) == 0) return &g_bitstructs[i];
    return NULL;
}

int aether_is_bitstruct(const char* name) {
    return bitstruct_find(name) != NULL;
}

void aether_register_bitstruct(const char* name, const char* backing) {
    if (!name || bitstruct_find(name)) return;
    if (g_bitstruct_count >= g_bitstruct_cap) {
        int nc = g_bitstruct_cap ? g_bitstruct_cap * 2 : 8;
        BitStructDef* nb = realloc(g_bitstructs, (size_t)nc * sizeof(BitStructDef));
        if (!nb) return;
        g_bitstructs = nb;
        g_bitstruct_cap = nc;
    }
    BitStructDef* d = &g_bitstructs[g_bitstruct_count++];
    d->name = strdup(name);
    d->backing = strdup(backing ? backing : "unsigned char");
    d->fields = NULL;
    d->nfields = 0;
    d->cap = 0;
}

void aether_bitstruct_add_field(const char* sname, const char* fname,
                                int lo, int hi, int is_bool) {
    BitStructDef* d = bitstruct_find(sname);
    if (!d || !fname) return;
    if (d->nfields >= d->cap) {
        int nc = d->cap ? d->cap * 2 : 8;
        BitStructField* nf = realloc(d->fields, (size_t)nc * sizeof(BitStructField));
        if (!nf) return;
        d->fields = nf;
        d->cap = nc;
    }
    BitStructField* f = &d->fields[d->nfields++];
    f->name = strdup(fname);
    f->lo = lo;
    f->hi = hi;
    f->is_bool = is_bool;
}

/* Resolve `bitstruct.field` to its inclusive bit range. Returns 1 on success. */
int aether_bitstruct_resolve(const char* sname, const char* field,
                            int* out_lo, int* out_hi, int* out_is_bool,
                            const char** out_backing) {
    BitStructDef* d = bitstruct_find(sname);
    if (!d || !field) return 0;
    for (int i = 0; i < d->nfields; i++) {
        if (strcmp(d->fields[i].name, field) == 0) {
            if (out_lo)      *out_lo = d->fields[i].lo;
            if (out_hi)      *out_hi = d->fields[i].hi;
            if (out_is_bool) *out_is_bool = d->fields[i].is_bool;
            if (out_backing) *out_backing = d->backing;
            return 1;
        }
    }
    return 0;
}

/* The bitstruct name a member-access node reads from, or NULL if the base is not
 * a bitstruct-typed expression. Deliberately shallow: a bitstruct field is a
 * scalar, so there is no `a.b.c` chain to walk (unlike a @c_struct overlay). */
const char* aether_bitstruct_base_name(ASTNode* macc) {
    if (!macc || macc->type != AST_MEMBER_ACCESS || macc->child_count == 0) return NULL;
    ASTNode* base = macc->children[0];
    if (!base || !base->node_type) return NULL;
    if (base->node_type->kind != TYPE_BITSTRUCT) return NULL;
    return base->node_type->struct_name;
}

/* The all-ones mask for an inclusive [lo,hi] range, as an unsigned long long. */
unsigned long long aether_bitstruct_mask(int lo, int hi) {
    int width = hi - lo + 1;
    if (width >= 64) return ~0ULL;
    return ((1ULL << width) - 1ULL);
}

/* #891: map a @c_struct field type to the mem_get_* / set_* width token, the
 * compiler choosing the accessor (killing the hand-picked-width footgun). If
 * the field's type names another @c_struct, set *nested to that name and
 * return NULL (the field is a sub-overlay, addressed by offset only). */
const char* c_struct_field_width(Type* t, const char** nested) {
    if (nested) *nested = NULL;
    if (!t) return "long";
    switch (t->kind) {
        case TYPE_BYTE:
        case TYPE_UINT8:    return "byte";
        case TYPE_UINT16:   return "uint16";
        case TYPE_UINT32:   return "uint32";
        case TYPE_INT:      return "int";
        case TYPE_INT64:
        case TYPE_UINT64:
        case TYPE_DURATION: return "long";
        case TYPE_FLOAT:    return "float64";
        case TYPE_PTR:      return "ptr";
        case TYPE_STRUCT:
            /* A nested @c_struct field: addressed by offset, descended into. */
            if (t->struct_name && nested) { *nested = t->struct_name; return NULL; }
            return "long";
        default:            return "long";
    }
}

// Map Aether type to C typename for array element declarations.
// For arrays, the size goes after the name in C, so we need just the
// element type, not the full "T[N]" form that get_c_type produces.
const char* const_array_elem_c_type(Type* t) {
    if (!t) return "const char*";
    if (t->c_alias) return t->c_alias;
    switch (t->kind) {
        case TYPE_STRING:  return "char*";  // STRING type already includes 'const' in its c_type
        case TYPE_INT:     return "int";
        case TYPE_INT64:   return "int64_t";
        case TYPE_UINT64:  return "uint64_t";
        case TYPE_UINT32:  return "uint32_t";
        case TYPE_UINT16:  return "uint16_t";
        case TYPE_UINT8:   return "uint8_t";
        case TYPE_FLOAT:   return "double";
        case TYPE_LONGDOUBLE: return "long double";
        case TYPE_FLOAT32: return "float";
        case TYPE_PTR:     return "void*";
        case TYPE_BYTE:    return "unsigned char";
        case TYPE_BOOL:    return "_Bool";
        default:           return "int";
    }
}

// Check if an AST tree uses sandbox builtins (sandbox_install, sandbox_push, spawn_sandboxed, etc.)
static int uses_sandbox(ASTNode* node) {
    if (!node) return 0;
    if (node->type == AST_FUNCTION_CALL && node->value) {
        if (strcmp(node->value, "sandbox_install") == 0 ||
            strcmp(node->value, "sandbox_uninstall") == 0 ||
            strcmp(node->value, "sandbox_push") == 0 ||
            strcmp(node->value, "sandbox_pop") == 0 ||
            strcmp(node->value, "spawn_sandboxed") == 0) {
            return 1;
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        if (uses_sandbox(node->children[i])) return 1;
    }
    return 0;
}

// Check if an AST node contains send expressions (for batch optimization)
int contains_send_expression(ASTNode* node) {
    if (!node) return 0;
    if (node->type == AST_SEND_FIRE_FORGET || node->type == AST_SEND_STATEMENT) return 1;
    for (int i = 0; i < node->child_count; i++) {
        if (contains_send_expression(node->children[i])) return 1;
    }
    return 0;
}

// Tree-shake of merged-but-unused stdlib functions runs in
// module_prune_unreachable() before typecheck — see aether_module.c.
// Codegen no longer needs a separate pass; by this point the AST only
// contains functions the user actually reaches.

static int is_inlineable_scalar(int type_kind) {
    switch (type_kind) {
        case TYPE_INT: case TYPE_INT64: case TYPE_UINT64: case TYPE_DURATION: case TYPE_PTR:
            return 1;
        default:
            return 0;
    }
}

// Returns the field name if msg has exactly one scalar field that fits in intptr_t
// (eligible for inline encoding), or NULL otherwise.  Inline messages skip heap
// allocation entirely — the field value is stored in Message.payload_int.
const char* get_single_int_field(MessageDef* msg_def) {
    if (!msg_def || !msg_def->fields) return NULL;
    MessageFieldDef* field = msg_def->fields;
    if (field->next != NULL) return NULL;
    return is_inlineable_scalar(field->type_kind) ? field->name : NULL;
}

/* Take ownership of a node codegen built itself. Such nodes hang off the
 * defer stack or a rewritten statement, never off the program AST, so
 * free_ast_node(program) does not reach them (#1667). */
void codegen_own_node(CodeGenerator* gen, ASTNode* node) {
    if (!gen || !node) return;
    if (gen->synthesised_count == gen->synthesised_cap) {
        int cap = gen->synthesised_cap ? gen->synthesised_cap * 2 : 32;
        ASTNode** grown = (ASTNode**)realloc(gen->synthesised_nodes,
                                             (size_t)cap * sizeof(ASTNode*));
        if (!grown) return;   /* the node stays alive; losing the free beats losing the node */
        gen->synthesised_nodes = grown;
        gen->synthesised_cap = cap;
    }
    gen->synthesised_nodes[gen->synthesised_count++] = node;
}

CodeGenerator* create_code_generator(FILE* output) {
    CodeGenerator* gen = malloc(sizeof(CodeGenerator));
    gen->synthesised_nodes = NULL;
    gen->synthesised_count = 0;
    gen->synthesised_cap = 0;
    gen->output = output;
    gen->indent_level = 0;
    gen->actor_count = 0;
    gen->function_count = 0;
    gen->current_actor = NULL;
    gen->actor_state_vars = NULL;
    gen->state_var_count = 0;
    gen->state_self_alias = NULL;
    gen->message_registry = create_message_registry();
    gen->declared_vars = NULL;
    gen->declared_var_count = 0;
    gen->heap_box_vars = NULL;
    gen->heap_box_var_count = 0;
    gen->module_global_vars = NULL;
    gen->module_global_var_count = 0;
    gen->generating_lvalue = 0;  // Not generating lvalue by default
    gen->interp_as_printf = 0;  // Default: interp generates _aether_interp() not printf()
    gen->in_condition = 0;  // Not in condition by default
    gen->in_main_loop = 0;  // Not in main loop by default
    gen->in_main_function = 0;
    gen->emit_header = 0;
    gen->header_file = NULL;
    gen->header_path = NULL;
    gen->csrc_header_file = NULL;  /* #996 --emit=csrc */
    gen->csrc_catalog_file = NULL; /* #996 --emit=csrc */
    gen->csrc_capabilities = NULL; /* #996 --emit=csrc */
    gen->emit_exe = 1;
    gen->emit_lib = 0;
    gen->emit_main_target = NULL;
    gen->generated_functions = NULL;
    gen->generated_function_count = 0;
    // Initialize defer tracking
    gen->defer_count = 0;
    gen->scope_depth = 0;
    memset(gen->defer_stack, 0, sizeof(gen->defer_stack));
    memset(gen->scope_defer_start, 0, sizeof(gen->scope_defer_start));
    /* #1140: every defer is unconditional until a `defer try` / `defer catch`
     * says otherwise, and a bare scope exit is not a function outcome. */
    memset(gen->defer_mode, 0, sizeof(gen->defer_mode));   /* DEFER_ALWAYS */
    gen->defer_exit = DEFER_EXIT_PLAIN;
    gen->defer_err_slot = NULL;
    // Issue #501: try/catch frame-pop tracking
    gen->try_frame_depth = 0;
    gen->loop_nest_depth = 0;
    memset(gen->loop_try_base, 0, sizeof(gen->loop_try_base));
    // #893: labeled break/continue tracking
    memset(gen->loop_label, 0, sizeof(gen->loop_label));
    memset(gen->loop_label_scope, 0, sizeof(gen->loop_label_scope));
    memset(gen->loop_label_id, 0, sizeof(gen->loop_label_id));
    memset(gen->loop_label_break_used, 0, sizeof(gen->loop_label_break_used));
    memset(gen->loop_label_continue_used, 0, sizeof(gen->loop_label_continue_used));
    gen->next_loop_label_id = 0;
    // Issue #501 follow-up: try-clobbered locals tracking
    gen->try_clobbered_vars = NULL;
    gen->try_clobbered_var_count = 0;
    // Extern function parameter registry
    gen->extern_registry = NULL;
    gen->extern_registry_count = 0;
    gen->extern_registry_capacity = 0;
    // MSVC compat: counter for ask-operator temp variables
    gen->ask_temp_counter = 0;
    // Counter for message-send array hoist variables
    gen->msg_arr_counter = 0;
    // #line-directive dedup state
    gen->last_line_file = NULL;
    gen->last_line_num = 0;
    gen->match_result_var = NULL;
    gen->preempt_loops = 0;
    gen->current_func_return_type = NULL;
    gen->current_function = NULL;
    gen->no_contracts = 0;
    gen->tuple_type_names = NULL;
    gen->tuple_type_count = 0;
    gen->tuple_type_capacity = 0;
    gen->opt_type_names = NULL;       // #340
    gen->opt_type_count = 0;
    gen->opt_type_capacity = 0;
    // Builder function registry
    gen->builder_funcs = NULL;
    gen->builder_func_count = 0;
    gen->builder_func_capacity = 0;
    gen->in_trailing_block = 0;
    gen->discard_call_value = 0;
    gen->current_env_captures = NULL;
    gen->current_env_capture_count = 0;
    gen->current_promoted_captures = NULL;
    gen->current_promoted_capture_count = 0;
    gen->promoted_funcs = NULL;
    gen->promoted_func_count = 0;
    gen->promoted_func_capacity = 0;
    // Builder function registry
    gen->builder_funcs_reg = NULL;
    gen->builder_func_reg_count = 0;
    gen->builder_func_reg_capacity = 0;
    // Bare-fn adapter registry
    gen->bare_fn_adapter_names = NULL;
    gen->bare_fn_adapter_count = 0;
    gen->bare_fn_adapter_capacity = 0;
    // Closure support
    gen->closure_counter = 0;
    gen->closures = NULL;
    gen->closure_count = 0;
    gen->closure_capacity = 0;
    gen->closure_var_map = NULL;
    gen->closure_var_count = 0;
    gen->closure_var_capacity = 0;
    // Heap string ownership tracking
    gen->heap_string_vars = NULL;
    gen->heap_string_var_count = 0;
    gen->escaped_string_vars = NULL;
    gen->escaped_string_var_count = 0;
    gen->return_escaped_string_vars = NULL;
    gen->return_escaped_string_var_count = 0;
    gen->return_escaped_struct_vars = NULL;
    gen->return_escaped_struct_var_count = 0;
    // *StringSeq ownership tracking — MUST be zero-initialised here:
    // the CodeGenerator is field-initialised (not calloc'd), so leaving
    // these uninitialised made the first clear_seq_vars() free garbage
    // pointers (glibc abort / SEGV on Linux CI; tolerated on macOS).
    gen->seq_vars = NULL;
    gen->seq_var_count = 0;
    gen->escaped_seq_vars = NULL;
    gen->escaped_seq_var_count = 0;
    // `string?` heap-ownership tracking — same zero-init requirement as
    // the seq registry above (field-initialised, not calloc'd).
    gen->opt_str_vars = NULL;
    gen->opt_str_var_count = 0;
    gen->escaped_opt_str_vars = NULL;
    gen->escaped_opt_str_var_count = 0;
    // Ask/reply type map
    gen->reply_type_map = NULL;
    gen->reply_type_count = 0;
    gen->reply_type_capacity = 0;
    // Typed fn-pointer locals
    gen->fnptr_locals = NULL;
    gen->fnptr_local_count = 0;
    gen->fnptr_local_capacity = 0;
    return gen;
}

/* Register an fn-pointer-typed local so call-site codegen can emit
 * the matching C function-pointer cast.  `sig` must be a TYPE_FUNCTION
 * with is_fnptr=1; ownership stays with the AST (we keep a borrow,
 * which is safe since the AST outlives codegen). */
void register_fnptr_local(CodeGenerator* gen, const char* name, Type* sig) {
    if (!gen || !name || !sig) return;
    /* Replace existing entry on shadow/reassign — most recent type
     * wins, matching scoping behaviour for local shadowing. */
    for (int i = 0; i < gen->fnptr_local_count; i++) {
        if (gen->fnptr_locals[i].name && strcmp(gen->fnptr_locals[i].name, name) == 0) {
            gen->fnptr_locals[i].signature = sig;
            return;
        }
    }
    if (gen->fnptr_local_count >= gen->fnptr_local_capacity) {
        int new_cap = gen->fnptr_local_capacity ? gen->fnptr_local_capacity * 2 : 8;
        gen->fnptr_locals = aether_xrealloc(gen->fnptr_locals,
            (size_t)new_cap * sizeof(*gen->fnptr_locals));
        gen->fnptr_local_capacity = new_cap;
    }
    gen->fnptr_locals[gen->fnptr_local_count].name = strdup(name);
    gen->fnptr_locals[gen->fnptr_local_count].signature = sig;
    gen->fnptr_local_count++;
}

/* Look up an fn-pointer local by name.  Returns the TYPE_FUNCTION
 * signature (with is_fnptr=1) or NULL if not registered. */
Type* lookup_fnptr_local(CodeGenerator* gen, const char* name) {
    if (!gen || !name) return NULL;
    for (int i = 0; i < gen->fnptr_local_count; i++) {
        if (gen->fnptr_locals[i].name && strcmp(gen->fnptr_locals[i].name, name) == 0) {
            return gen->fnptr_locals[i].signature;
        }
    }
    return NULL;
}

CodeGenerator* create_code_generator_with_header(FILE* output, FILE* header, const char* header_path) {
    CodeGenerator* gen = create_code_generator(output);
    gen->emit_header = 1;
    gen->header_file = header;
    gen->header_path = header_path;
    return gen;
}

void free_code_generator(CodeGenerator* gen) {
    if (gen) {
        /* The emitted-typedef registries: one strdup'd name per distinct
         * tuple / optional / sum shape in the program (#1667). */
        for (int i = 0; i < gen->tuple_type_count; i++) {
            free(gen->tuple_type_names[i]);
        }
        free(gen->tuple_type_names);
        gen->tuple_type_names = NULL;
        gen->tuple_type_count = 0;
        gen->tuple_type_capacity = 0;
        for (int i = 0; i < gen->opt_type_count; i++) {
            free(gen->opt_type_names[i]);
        }
        free(gen->opt_type_names);
        gen->opt_type_names = NULL;
        gen->opt_type_count = 0;
        gen->opt_type_capacity = 0;
        for (int i = 0; i < gen->synthesised_count; i++) {
            free_ast_node(gen->synthesised_nodes[i]);
        }
        free(gen->synthesised_nodes);
        gen->synthesised_nodes = NULL;
        gen->synthesised_count = 0;
        gen->synthesised_cap = 0;
        if (gen->current_actor) free(gen->current_actor);
        if (gen->actor_state_vars) {
            for (int i = 0; i < gen->state_var_count; i++) {
                free(gen->actor_state_vars[i]);
            }
            free(gen->actor_state_vars);
        }
        if (gen->declared_vars) {
            for (int i = 0; i < gen->declared_var_count; i++) {
                free(gen->declared_vars[i]);
            }
            free(gen->declared_vars);
        }
        if (gen->module_global_vars) {
            for (int i = 0; i < gen->module_global_var_count; i++) {
                free(gen->module_global_vars[i]);
            }
            free(gen->module_global_vars);
        }
        clear_heap_string_vars(gen);
    clear_seq_vars(gen);
    clear_opt_str_vars(gen);
        clear_escaped_string_vars(gen);
        clear_try_clobbered_vars(gen);  /* Issue #501 follow-up */
        if (gen->message_registry) {
            free_message_registry(gen->message_registry);
        }
        if (gen->generated_functions) {
            for (int i = 0; i < gen->generated_function_count; i++) {
                free(gen->generated_functions[i]);
            }
            free(gen->generated_functions);
        }
        if (gen->extern_registry) {
            for (int i = 0; i < gen->extern_registry_count; i++) {
                free(gen->extern_registry[i].name);
                free(gen->extern_registry[i].c_name);
                free(gen->extern_registry[i].params);
                free(gen->extern_registry[i].params_aether);
                free(gen->extern_registry[i].params_retain);
                /* param_full's entries alias AST-owned Types; only the
                 * array is ours to free. */
                free(gen->extern_registry[i].param_full);
            }
            free(gen->extern_registry);
        }
        if (gen->reply_type_map) {
            for (int i = 0; i < gen->reply_type_count; i++) {
                free(gen->reply_type_map[i].request_msg);
                free(gen->reply_type_map[i].reply_msg);
            }
            free(gen->reply_type_map);
        }
        free(gen);
    }
}

// Helper: check if variable was already declared in current function
int is_var_declared(CodeGenerator* gen, const char* var_name) {
    for (int i = 0; i < gen->declared_var_count; i++) {
        if (strcmp(gen->declared_vars[i], var_name) == 0) {
            return 1;
        }
    }
    return 0;
}

// #701: is `name` a module-level `var` global (a file-scope static)?
int is_module_global_var(CodeGenerator* gen, const char* name) {
    if (!name) return 0;
    for (int i = 0; i < gen->module_global_var_count; i++) {
        if (strcmp(gen->module_global_vars[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

// #701: record a module-level `var` global name (deduped).
void register_module_global_var(CodeGenerator* gen, const char* name) {
    if (!name || is_module_global_var(gen, name)) return;
    char** grown = realloc(gen->module_global_vars,
                           sizeof(char*) * (gen->module_global_var_count + 1));
    if (!grown) return;
    gen->module_global_vars = grown;
    gen->module_global_vars[gen->module_global_var_count++] = strdup(name);
}

// Helper: mark variable as declared in current function
/* Drop declared-var entries above `saved_count` (scope exit). The three
 * scope-restore sites used to truncate declared_var_count directly, which
 * leaked every strdup'd name declared inside the scope; in long-lived
 * hosts of the compiler (aetherc lsp reparses per keystroke) that grew
 * without bound. */
void truncate_declared_vars(CodeGenerator* gen, int saved_count) {
    for (int i = saved_count; i < gen->declared_var_count; i++) {
        free(gen->declared_vars[i]);
    }
    gen->declared_var_count = saved_count;
}

void mark_var_declared(CodeGenerator* gen, const char* var_name) {
    char** new_vars = realloc(gen->declared_vars, sizeof(char*) * (gen->declared_var_count + 1));
    if (!new_vars) return;
    gen->declared_vars = new_vars;
    gen->declared_vars[gen->declared_var_count] = strdup(var_name);
    gen->declared_var_count++;
}

// Helper: clear declared vars (call at function start)
void clear_declared_vars(CodeGenerator* gen) {
    if (gen->declared_vars) {
        for (int i = 0; i < gen->declared_var_count; i++) {
            free(gen->declared_vars[i]);
        }
        free(gen->declared_vars);
    }
    gen->declared_vars = NULL;
    gen->declared_var_count = 0;
    /* #790: heap.new box provenance is per-function. */
    if (gen->heap_box_vars) {
        for (int i = 0; i < gen->heap_box_var_count; i++) free(gen->heap_box_vars[i]);
        free(gen->heap_box_vars);
    }
    gen->heap_box_vars = NULL;
    gen->heap_box_var_count = 0;
}

// #790: is `var_name` currently bound to a heap.new(T) box?
int is_heap_box_var(CodeGenerator* gen, const char* var_name) {
    if (!var_name) return 0;
    for (int i = 0; i < gen->heap_box_var_count; i++) {
        if (strcmp(gen->heap_box_vars[i], var_name) == 0) return 1;
    }
    return 0;
}

// #790: record that `var_name` now holds a heap.new(T) box.
void mark_heap_box_var(CodeGenerator* gen, const char* var_name) {
    if (!var_name || is_heap_box_var(gen, var_name)) return;
    char** nv = realloc(gen->heap_box_vars, sizeof(char*) * (gen->heap_box_var_count + 1));
    if (!nv) return;
    gen->heap_box_vars = nv;
    gen->heap_box_vars[gen->heap_box_var_count] = strdup(var_name);
    gen->heap_box_var_count++;
}

// #790: `var_name` was reassigned to something other than a heap.new box —
// drop it so a later `var.field = ...` store falls back to a bare assignment.
void unmark_heap_box_var(CodeGenerator* gen, const char* var_name) {
    if (!var_name) return;
    for (int i = 0; i < gen->heap_box_var_count; i++) {
        if (strcmp(gen->heap_box_vars[i], var_name) == 0) {
            free(gen->heap_box_vars[i]);
            gen->heap_box_vars[i] = gen->heap_box_vars[gen->heap_box_var_count - 1];
            gen->heap_box_var_count--;
            return;
        }
    }
}

// Helper: check if variable currently holds a heap-allocated string
int is_heap_string_var(CodeGenerator* gen, const char* var_name) {
    for (int i = 0; i < gen->heap_string_var_count; i++) {
        if (strcmp(gen->heap_string_vars[i], var_name) == 0) {
            return 1;
        }
    }
    return 0;
}

// Helper: mark variable as holding a heap-allocated string
void mark_heap_string_var(CodeGenerator* gen, const char* var_name) {
    if (is_heap_string_var(gen, var_name)) return;
    char** new_vars = realloc(gen->heap_string_vars, sizeof(char*) * (gen->heap_string_var_count + 1));
    if (!new_vars) return;
    gen->heap_string_vars = new_vars;
    gen->heap_string_vars[gen->heap_string_var_count] = strdup(var_name);
    gen->heap_string_var_count++;
}

// Helper: clear heap string vars (call at function start)
void clear_heap_string_vars(CodeGenerator* gen) {
    if (gen->heap_string_vars) {
        for (int i = 0; i < gen->heap_string_var_count; i++) {
            free(gen->heap_string_vars[i]);
        }
        free(gen->heap_string_vars);
    }
    gen->heap_string_vars = NULL;
    gen->heap_string_var_count = 0;
}

// Helper: check whether a heap-string variable has escaped — its
// value has been passed somewhere (call argument outside its own
// assignment's RHS, closure capture, etc.) where the recipient may
// have stored the pointer. The wrapper at codegen_stmt.c:1611 uses
// this to gate `free(_tmp_old)` and avoid dangling the stored copy.
// Conservative: alias-safe (no UAF) at the cost of leaking. See
// mark_escaped_heap_string_vars for the analysis.
int is_escaped_string_var(CodeGenerator* gen, const char* var_name) {
    if (!gen || !var_name) return 0;
    for (int i = 0; i < gen->escaped_string_var_count; i++) {
        if (strcmp(gen->escaped_string_vars[i], var_name) == 0) {
            return 1;
        }
    }
    return 0;
}

// Helper: mark a heap-string variable as escaped.
void mark_escaped_string_var(CodeGenerator* gen, const char* var_name) {
    if (!gen || !var_name) return;
    if (is_escaped_string_var(gen, var_name)) return;
    char** new_vars = realloc(gen->escaped_string_vars,
                              sizeof(char*) * (gen->escaped_string_var_count + 1));
    if (!new_vars) return;
    gen->escaped_string_vars = new_vars;
    gen->escaped_string_vars[gen->escaped_string_var_count] = strdup(var_name);
    gen->escaped_string_var_count++;
}

// Helper: clear escaped string vars (call at function start, alongside
// clear_heap_string_vars).
void clear_escaped_string_vars(CodeGenerator* gen) {
    if (!gen) return;
    if (gen->escaped_string_vars) {
        for (int i = 0; i < gen->escaped_string_var_count; i++) {
            free(gen->escaped_string_vars[i]);
        }
        free(gen->escaped_string_vars);
    }
    gen->escaped_string_vars = NULL;
    gen->escaped_string_var_count = 0;
    if (gen->return_escaped_string_vars) {
        for (int i = 0; i < gen->return_escaped_string_var_count; i++) {
            free(gen->return_escaped_string_vars[i]);
        }
        free(gen->return_escaped_string_vars);
    }
    gen->return_escaped_string_vars = NULL;
    gen->return_escaped_string_var_count = 0;
    if (gen->return_escaped_struct_vars) {
        for (int i = 0; i < gen->return_escaped_struct_var_count; i++) {
            free(gen->return_escaped_struct_vars[i]);
        }
        free(gen->return_escaped_struct_vars);
    }
    gen->return_escaped_struct_vars = NULL;
    gen->return_escaped_struct_var_count = 0;
}

/* ---- *StringSeq local registry (parallel to heap_string_vars) ----
 * A seq var owns a refcounted spine; string_seq_free is a decrement, so
 * freeing on reassign / scope-exit is refcount-safe even when shared. */
int is_seq_var(CodeGenerator* gen, const char* var_name) {
    if (!gen || !var_name) return 0;
    for (int i = 0; i < gen->seq_var_count; i++) {
        if (strcmp(gen->seq_vars[i], var_name) == 0) return 1;
    }
    return 0;
}

void mark_seq_var(CodeGenerator* gen, const char* var_name) {
    if (!gen || !var_name) return;
    if (is_seq_var(gen, var_name)) return;
    char** nv = realloc(gen->seq_vars, sizeof(char*) * (gen->seq_var_count + 1));
    if (!nv) return;
    gen->seq_vars = nv;
    gen->seq_vars[gen->seq_var_count++] = strdup(var_name);
}

int is_escaped_seq_var(CodeGenerator* gen, const char* var_name) {
    if (!gen || !var_name) return 0;
    for (int i = 0; i < gen->escaped_seq_var_count; i++) {
        if (strcmp(gen->escaped_seq_vars[i], var_name) == 0) return 1;
    }
    return 0;
}

void mark_escaped_seq_var(CodeGenerator* gen, const char* var_name) {
    if (!gen || !var_name) return;
    if (is_escaped_seq_var(gen, var_name)) return;
    char** nv = realloc(gen->escaped_seq_vars,
                        sizeof(char*) * (gen->escaped_seq_var_count + 1));
    if (!nv) return;
    gen->escaped_seq_vars = nv;
    gen->escaped_seq_vars[gen->escaped_seq_var_count++] = strdup(var_name);
}

void clear_seq_vars(CodeGenerator* gen) {
    if (!gen) return;
    if (gen->seq_vars) {
        for (int i = 0; i < gen->seq_var_count; i++) free(gen->seq_vars[i]);
        free(gen->seq_vars);
    }
    gen->seq_vars = NULL;
    gen->seq_var_count = 0;
    if (gen->escaped_seq_vars) {
        for (int i = 0; i < gen->escaped_seq_var_count; i++) free(gen->escaped_seq_vars[i]);
        free(gen->escaped_seq_vars);
    }
    gen->escaped_seq_vars = NULL;
    gen->escaped_seq_var_count = 0;
}

/* ---- `string?` (optional-of-string) heap registry ----
 * Parallel to the seq registry above, but frees `<name>.val` (guarded by
 * both the `_heapopt_<name>` ownership flag and the struct's `.has` bit)
 * via aether_heap_str_free. See the header comment on `opt_str_vars`. */
int is_opt_str_var(CodeGenerator* gen, const char* var_name) {
    if (!gen || !var_name) return 0;
    for (int i = 0; i < gen->opt_str_var_count; i++) {
        if (strcmp(gen->opt_str_vars[i], var_name) == 0) return 1;
    }
    return 0;
}

void mark_opt_str_var(CodeGenerator* gen, const char* var_name) {
    if (!gen || !var_name) return;
    if (is_opt_str_var(gen, var_name)) return;
    char** nv = realloc(gen->opt_str_vars, sizeof(char*) * (gen->opt_str_var_count + 1));
    if (!nv) return;
    gen->opt_str_vars = nv;
    gen->opt_str_vars[gen->opt_str_var_count++] = strdup(var_name);
}

int is_escaped_opt_str_var(CodeGenerator* gen, const char* var_name) {
    if (!gen || !var_name) return 0;
    for (int i = 0; i < gen->escaped_opt_str_var_count; i++) {
        if (strcmp(gen->escaped_opt_str_vars[i], var_name) == 0) return 1;
    }
    return 0;
}

void mark_escaped_opt_str_var(CodeGenerator* gen, const char* var_name) {
    if (!gen || !var_name) return;
    if (is_escaped_opt_str_var(gen, var_name)) return;
    char** nv = realloc(gen->escaped_opt_str_vars,
                        sizeof(char*) * (gen->escaped_opt_str_var_count + 1));
    if (!nv) return;
    gen->escaped_opt_str_vars = nv;
    gen->escaped_opt_str_vars[gen->escaped_opt_str_var_count++] = strdup(var_name);
}

void clear_opt_str_vars(CodeGenerator* gen) {
    if (!gen) return;
    if (gen->opt_str_vars) {
        for (int i = 0; i < gen->opt_str_var_count; i++) free(gen->opt_str_vars[i]);
        free(gen->opt_str_vars);
    }
    gen->opt_str_vars = NULL;
    gen->opt_str_var_count = 0;
    if (gen->escaped_opt_str_vars) {
        for (int i = 0; i < gen->escaped_opt_str_var_count; i++)
            free(gen->escaped_opt_str_vars[i]);
        free(gen->escaped_opt_str_vars);
    }
    gen->escaped_opt_str_vars = NULL;
    gen->escaped_opt_str_var_count = 0;
}

/* Return-escape sibling. See the header comment on
 * `return_escaped_string_vars` for the two-set rationale. */
int is_return_escaped_string_var(CodeGenerator* gen, const char* var_name) {
    if (!gen || !var_name) return 0;
    for (int i = 0; i < gen->return_escaped_string_var_count; i++) {
        if (strcmp(gen->return_escaped_string_vars[i], var_name) == 0) {
            return 1;
        }
    }
    return 0;
}

void mark_return_escaped_string_var(CodeGenerator* gen, const char* var_name) {
    if (!gen || !var_name) return;
    if (is_return_escaped_string_var(gen, var_name)) return;
    char** new_vars = realloc(gen->return_escaped_string_vars,
                              sizeof(char*) * (gen->return_escaped_string_var_count + 1));
    if (!new_vars) return;
    gen->return_escaped_string_vars = new_vars;
    gen->return_escaped_string_vars[gen->return_escaped_string_var_count] = strdup(var_name);
    gen->return_escaped_string_var_count++;
}

/* #752: a struct local returned (directly or as a tuple element)
 * transfers ownership of its heap-string fields to the caller, so the
 * function-exit <Struct>_destroy defer must NOT fire for it. */
int is_return_escaped_struct_var(CodeGenerator* gen, const char* var_name) {
    if (!gen || !var_name) return 0;
    for (int i = 0; i < gen->return_escaped_struct_var_count; i++) {
        if (strcmp(gen->return_escaped_struct_vars[i], var_name) == 0) {
            return 1;
        }
    }
    return 0;
}

void mark_return_escaped_struct_var(CodeGenerator* gen, const char* var_name) {
    if (!gen || !var_name) return;
    if (is_return_escaped_struct_var(gen, var_name)) return;
    char** new_vars = realloc(gen->return_escaped_struct_vars,
                              sizeof(char*) * (gen->return_escaped_struct_var_count + 1));
    if (!new_vars) return;
    gen->return_escaped_struct_vars = new_vars;
    gen->return_escaped_struct_vars[gen->return_escaped_struct_var_count] = strdup(var_name);
    gen->return_escaped_struct_var_count++;
}

// Normalise a callee name's dots to underscores. Used by every
// callee-name lookup in codegen — heap-string allowlist
// (codegen_stmt.c:is_heap_string_expr), builder-funcs registry
// (codegen_expr.c), extern param-type lookup
// (codegen_stmt.c:lookup_callee_param_kind). Source-level
// `"string.concat"` becomes `"string_concat"` so it matches the
// underscored form the registries are keyed on.
const char* codegen_normalise_callee(const char* raw, char* out, size_t out_size) {
    if (!out || out_size == 0) return out;
    if (!raw) { out[0] = '\0'; return out; }
    size_t n = strlen(raw);
    if (n >= out_size) n = out_size - 1;
    memcpy(out, raw, n);
    out[n] = '\0';
    for (char* p = out; *p; p++) {
        if (*p == '.') *p = '_';
    }
    return out;
}

// Helper: check if a function was already generated
int is_function_generated(CodeGenerator* gen, const char* func_name) {
    for (int i = 0; i < gen->generated_function_count; i++) {
        if (strcmp(gen->generated_functions[i], func_name) == 0) {
            return 1;
        }
    }
    return 0;
}

// Helper: mark a function as generated
void mark_function_generated(CodeGenerator* gen, const char* func_name) {
    char** new_funcs = realloc(gen->generated_functions,
                               sizeof(char*) * (gen->generated_function_count + 1));
    if (!new_funcs) return;
    gen->generated_functions = new_funcs;
    gen->generated_functions[gen->generated_function_count] = strdup(func_name);
    gen->generated_function_count++;
}

// Helper: count how many function clauses exist with the same name
int count_function_clauses(ASTNode* program, const char* func_name) {
    int count = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if ((child->type == AST_FUNCTION_DEFINITION || child->type == AST_BUILDER_FUNCTION) &&
            child->value && strcmp(child->value, func_name) == 0) {
            count++;
        }
    }
    return count;
}

// Helper: collect all function clauses with the same name
ASTNode** collect_function_clauses(ASTNode* program, const char* func_name, int* out_count) {
    int count = count_function_clauses(program, func_name);
    if (count == 0) {
        *out_count = 0;
        return NULL;
    }

    ASTNode** clauses = malloc(sizeof(ASTNode*) * count);
    int idx = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if ((child->type == AST_FUNCTION_DEFINITION || child->type == AST_BUILDER_FUNCTION) &&
            child->value && strcmp(child->value, func_name) == 0) {
            clauses[idx++] = child;
        }
    }
    *out_count = count;
    return clauses;
}

void indent(CodeGenerator* gen) {
    gen->indent_level++;
}

void unindent(CodeGenerator* gen) {
    if (gen->indent_level > 0) {
        gen->indent_level--;
    }
}

void print_indent(CodeGenerator* gen) {
    for (int i = 0; i < gen->indent_level; i++) {
        fprintf(gen->output, "    ");
    }
}

// ============================================================================
// Defer Implementation - Real LIFO execution at scope exit
// ============================================================================

// Push a deferred statement onto the stack (unconditional — fires at every exit).
void push_defer(CodeGenerator* gen, ASTNode* stmt) {
    push_defer_mode(gen, stmt, DEFER_ALWAYS);
}

/* #1140 — push a deferred statement with an explicit firing mode. Every
 * pre-existing caller (including all the compiler-synthesised cleanup carriers)
 * goes through push_defer above and so is DEFER_ALWAYS; only a user's
 * `defer try` / `defer catch` reaches here with anything else. */
void push_defer_mode(CodeGenerator* gen, ASTNode* stmt, DeferMode mode) {
    if (gen->defer_count < MAX_DEFER_STACK) {
        gen->defer_mode[gen->defer_count] = mode;
        gen->defer_stack[gen->defer_count++] = stmt;
    } else {
        AetherError w = {NULL, NULL, 0, 0, "defer stack overflow, too many nested defers",
                         "simplify scope nesting or reduce number of deferred statements",
                         NULL, AETHER_ERR_NONE};
        aether_warning_report(&w);
    }
}

// Enter a new scope - remember where defers started for this scope
void enter_scope(CodeGenerator* gen) {
    if (gen->scope_depth < MAX_SCOPE_DEPTH) {
        gen->scope_defer_start[gen->scope_depth] = gen->defer_count;
        gen->scope_depth++;
    }
}

// Heap-string-exit-free defer carrier (issue #420 follow-up).
//
// `push_heap_string_exit_free_defers` (codegen_stmt.c) pushes an
// AST_EXPRESSION_STATEMENT whose annotation encodes the variable
// name as `"heap_string_exit_free:<name>"`. The defer-emit loops
// (here and in emit_all_defers_protected) detect the prefix and
// render the conditional-free directly:
//
//     if (_heap_<name>) { free((void*)<name>); <name> = NULL; _heap_<name> = 0; }
//
// without descending through generate_statement (the carrier's
// body is intentionally empty). The reset-after-free is defensive:
// a defer can be drained from emit_all_defers at a return site;
// the C var is dead from the caller's perspective either way, but
// keeping the tracker honest means a re-emitted scope-exit drain
// is idempotent. Returns 1 if the defer was a heap-exit-free
// carrier and was rendered; 0 otherwise (caller falls through to
// the standard `generate_statement` path).
static int try_emit_heap_string_exit_free(CodeGenerator* gen, ASTNode* deferred) {
    if (!deferred || !deferred->annotation) return 0;
    const char* prefix = "heap_string_exit_free:";
    size_t plen = strlen(prefix);
    if (strncmp(deferred->annotation, prefix, plen) != 0) return 0;
    const char* name = deferred->annotation + plen;
    if (!*name) return 0;
    print_indent(gen);
    fprintf(gen->output,
            "/* deferred */ if (_heap_%s) { aether_heap_str_free(%s); %s = NULL; _heap_%s = 0; }\n",
            name, name, name, name);
    return 1;
}

/* *StringSeq scope-exit free carrier. Annotation: "seq_exit_free:<name>".
 * string_seq_free is a refcount decrement, so this is safe even when the
 * spine is shared; the flag guard + NULL keep it idempotent against an
 * explicit string.seq_free that already ran. */
static int try_emit_seq_exit_free(CodeGenerator* gen, ASTNode* deferred) {
    if (!deferred || !deferred->annotation) return 0;
    const char* prefix = "seq_exit_free:";
    size_t plen = strlen(prefix);
    if (strncmp(deferred->annotation, prefix, plen) != 0) return 0;
    const char* name = deferred->annotation + plen;
    if (!*name) return 0;
    print_indent(gen);
    fprintf(gen->output,
            "/* deferred */ if (_seqheap_%s) { string_seq_free(%s); %s = NULL; _seqheap_%s = 0; }\n",
            name, name, name, name);
    return 1;
}

/* `string?` scope-exit free carrier. Annotation: "opt_str_exit_free:<name>".
 * Frees the optional's `.val` buffer when the slot both currently owns a
 * heap allocation (`_heapopt_<name>`, set from is_heap_string_expr on the
 * coerced RHS) AND is present (`.has`). aether_heap_str_free dispatches on
 * the AetherString magic header, so a borrowed literal would be a no-op
 * even if it slipped through — but the flag guard keeps it precise. The
 * flag/`.has` reset keeps a re-emitted drain (multiple return sites)
 * idempotent. */
static int try_emit_opt_str_exit_free(CodeGenerator* gen, ASTNode* deferred) {
    if (!deferred || !deferred->annotation) return 0;
    const char* prefix = "opt_str_exit_free:";
    size_t plen = strlen(prefix);
    if (strncmp(deferred->annotation, prefix, plen) != 0) return 0;
    const char* name = deferred->annotation + plen;
    if (!*name) return 0;
    print_indent(gen);
    fprintf(gen->output,
            "/* deferred */ if (_heapopt_%s && %s.has) { aether_heap_str_free((void*)%s.val); %s.val = NULL; %s.has = 0; _heapopt_%s = 0; }\n",
            name, name, name, name, name, name);
    return 1;
}

/* Struct-destructor defer carrier (#465). Annotation:
 *   "struct_destroy:<varname>:<StructName>"
 * Pushed by the struct-local-declaration site in codegen_stmt.c
 * for every local struct variable whose definition has at least
 * one heap-string field. The emitted defer calls the auto-
 * generated <StructName>_destroy(&<varname>) function, which
 * walks every `_heap_<field>` tracker and frees the matching
 * field's buffer if set. */
static int try_emit_struct_destroy(CodeGenerator* gen, ASTNode* deferred) {
    if (!deferred || !deferred->annotation) return 0;
    const char* prefix = "struct_destroy:";
    size_t plen = strlen(prefix);
    if (strncmp(deferred->annotation, prefix, plen) != 0) return 0;
    const char* rest = deferred->annotation + plen;
    /* Split "<varname>:<StructName>" on the last colon — struct
     * names don't contain colons, var names shouldn't either. */
    const char* sep = strchr(rest, ':');
    if (!sep || !sep[1]) return 0;
    size_t var_len = (size_t)(sep - rest);
    if (var_len == 0 || var_len > 200) return 0;
    char var_buf[256];
    memcpy(var_buf, rest, var_len);
    var_buf[var_len] = '\0';
    const char* struct_name = sep + 1;
    /* #752: suppress the destroy when this struct escaped via a return —
     * its heap-string fields now belong to the caller. Consume the defer
     * (return 1) so it is not freed at callee exit. */
    if (is_return_escaped_struct_var(gen, var_buf)) {
        return 1;
    }
    print_indent(gen);
    fprintf(gen->output,
            "/* deferred */ %s_destroy(&%s);\n",
            struct_name, var_buf);
    return 1;
}

/* #1140 — should the defer at stack slot `i` fire at the exit currently being
 * emitted?
 *
 *   DEFER_ALWAYS  fires everywhere (this is every pre-existing defer, and every
 *                 compiler-synthesised cleanup carrier).
 *   DEFER_TRY     fires only on a successful return.
 *   DEFER_CATCH   fires only on an error return.
 *
 * A `break` / `continue` / plain scope exit is not a function outcome, so
 * neither conditional kind fires there. When the outcome is only known at run
 * time (the usual `return v, err` case), both kinds are emitted but each is
 * wrapped in a runtime test on the error slot — see emit_deferred_one. */
static int defer_fires_at_exit(CodeGenerator* gen, int i) {
    switch (gen->defer_mode[i]) {
        case DEFER_ALWAYS: return 1;
        case DEFER_TRY:
            return gen->defer_exit == DEFER_EXIT_SUCCESS ||
                   gen->defer_exit == DEFER_EXIT_RUNTIME;
        case DEFER_CATCH:
            return gen->defer_exit == DEFER_EXIT_ERROR ||
                   gen->defer_exit == DEFER_EXIT_RUNTIME;
    }
    return 1;
}

/* #1140 — emit one deferred statement, wrapping it in a runtime guard when the
 * exit's success/error outcome is not statically known. The guard uses exactly
 * the convention the rest of the compiler uses for "this result carries an
 * error": a non-NULL, non-empty error slot. Non-empty is read through
 * aether_string_data, since the slot holds either physical string shape and
 * indexing a refcounted one raw reads its magic header as content. */
static void emit_deferred_one(CodeGenerator* gen, int i) {
    ASTNode* deferred = gen->defer_stack[i];
    if (!deferred) return;

    DeferMode mode = gen->defer_mode[i];
    int guarded = (mode != DEFER_ALWAYS &&
                   gen->defer_exit == DEFER_EXIT_RUNTIME &&
                   gen->defer_err_slot != NULL);
    if (guarded) {
        print_indent(gen);
        fprintf(gen->output, "if (%s(%s && aether_string_data((const void*)%s)[0])) {\n",
                mode == DEFER_CATCH ? "" : "!",
                gen->defer_err_slot, gen->defer_err_slot);
        gen->indent_level++;
    }

    if (!try_emit_heap_string_exit_free(gen, deferred) &&
        !try_emit_seq_exit_free(gen, deferred) &&
        !try_emit_opt_str_exit_free(gen, deferred) &&
        !try_emit_struct_destroy(gen, deferred)) {
        print_indent(gen);
        fprintf(gen->output, "/* deferred%s */ ",
                mode == DEFER_TRY ? " (try)" : mode == DEFER_CATCH ? " (catch)" : "");
        generate_statement(gen, deferred);
    }

    if (guarded) {
        gen->indent_level--;
        print_indent(gen);
        fprintf(gen->output, "}\n");
    }
}

// Emit deferred statements for current scope only (in reverse order)
void emit_defers_for_scope(CodeGenerator* gen) {
    if (gen->scope_depth <= 0) return;

    int scope_start = gen->scope_defer_start[gen->scope_depth - 1];

    // Emit defers in LIFO order (reverse)
    for (int i = gen->defer_count - 1; i >= scope_start; i--) {
        if (!gen->defer_stack[i]) continue;
        if (!defer_fires_at_exit(gen, i)) continue;
        emit_deferred_one(gen, i);
    }
}

// #893: emit defers for every scope from the innermost down to (but NOT
// including) `floor_depth`. Labeled break/continue unwind multiple scopes at
// once — a `break L` exits every scope nested inside the scope that CONTAINS
// loop L — so the defers of all of them must run before the goto. `floor_depth`
// is the scope_depth that contains the labeled loop, recorded when the loop was
// entered. Emits only; it does not mutate scope/defer state, so the normal
// fall-through path still runs exit_scope for each scope as usual.
void emit_defers_through_scope(CodeGenerator* gen, int floor_depth) {
    if (gen->scope_depth <= 0) return;
    if (floor_depth < 0) floor_depth = 0;
    if (floor_depth >= gen->scope_depth) { emit_defers_for_scope(gen); return; }

    int scope_start = gen->scope_defer_start[floor_depth];
    for (int i = gen->defer_count - 1; i >= scope_start; i--) {
        if (!gen->defer_stack[i]) continue;
        if (!defer_fires_at_exit(gen, i)) continue;
        emit_deferred_one(gen, i);
    }
}

// Exit scope - emit defers and pop scope
void exit_scope(CodeGenerator* gen) {
    emit_defers_for_scope(gen);

    if (gen->scope_depth > 0) {
        // Pop all defers for this scope
        gen->defer_count = gen->scope_defer_start[gen->scope_depth - 1];
        gen->scope_depth--;
    }
}

// Is `deferred` the synthetic "free(<name>.env)" defer for the closure var
// called `name`? The defers inserted by codegen_stmt.c at closure-variable
// declaration sites have a fixed shape: EXPRESSION_STATEMENT > FUNCTION_CALL
// "free" > IDENTIFIER "<name>.env".
static int is_env_free_for(ASTNode* deferred, const char* name) {
    if (!deferred || !name) return 0;
    if (deferred->type != AST_EXPRESSION_STATEMENT || deferred->child_count < 1) return 0;
    ASTNode* call = deferred->children[0];
    if (!call || call->type != AST_FUNCTION_CALL || !call->value ||
        strcmp(call->value, "free") != 0 || call->child_count < 1) return 0;
    ASTNode* arg = call->children[0];
    if (!arg || arg->type != AST_IDENTIFIER || !arg->value) return 0;
    size_t nlen = strlen(name);
    if (strncmp(arg->value, name, nlen) != 0) return 0;
    if (strcmp(arg->value + nlen, ".env") != 0) return 0;
    return 1;
}

// Is `deferred` the synthetic "free(<name>)" defer for a Route 1 promoted
// cell? Shape: EXPRESSION_STATEMENT > FUNCTION_CALL "free" > IDENTIFIER
// "<name>" where the arg was marked `raw_promoted` by codegen_stmt.c.
static int is_promoted_free_for(ASTNode* deferred, const char* name) {
    if (!deferred || !name) return 0;
    if (deferred->type != AST_EXPRESSION_STATEMENT || deferred->child_count < 1) return 0;
    ASTNode* call = deferred->children[0];
    if (!call || call->type != AST_FUNCTION_CALL || !call->value ||
        strcmp(call->value, "free") != 0 || call->child_count < 1) return 0;
    ASTNode* arg = call->children[0];
    if (!arg || arg->type != AST_IDENTIFIER || !arg->value) return 0;
    if (!arg->annotation || strcmp(arg->annotation, "raw_promoted") != 0) return 0;
    return strcmp(arg->value, name) == 0;
}

// Emit ALL deferred statements (for return - unwinds entire function).
// `protected_names` and `protected_count` list closure variable names whose
// env-free defer should be suppressed — used at return sites where the
// closure's env is still live through the returned value.
void emit_all_defers_protected(CodeGenerator* gen, char** protected_names, int protected_count) {
    // Emit all defers in LIFO order across all scopes. A defer is suppressed
    // when either (a) it frees the env of a closure variable in the protected
    // list, or (b) it frees a Route 1 promoted cell whose name matches a
    // protected name (because the escaping closure's env captures the
    // pointer, and the caller now owns the cell).
    for (int i = gen->defer_count - 1; i >= 0; i--) {
        ASTNode* deferred = gen->defer_stack[i];
        if (!deferred) continue;
        /* #1140: skip the conditional defers that don't apply to this exit. */
        if (!defer_fires_at_exit(gen, i)) continue;
        int skip = 0;
        for (int p = 0; p < protected_count; p++) {
            if (!protected_names[p]) continue;
            if (is_env_free_for(deferred, protected_names[p]) ||
                is_promoted_free_for(deferred, protected_names[p])) {
                skip = 1;
                break;
            }
        }
        if (skip) {
            print_indent(gen);
            fprintf(gen->output, "/* deferred (suppressed: escapes via return) */\n");
            continue;
        }
        emit_deferred_one(gen, i);
    }
}

void emit_all_defers(CodeGenerator* gen) {
    emit_all_defers_protected(gen, NULL, 0);
}

// Drain any in-flight try-frame pops at a non-local exit inside a
// try body.  See issue #501.
//
// The AST_TRY_STATEMENT codegen emits `aether_try_pop()` at the
// natural end of the try body block.  A `return` (or any other
// non-fall-through exit) from inside the body bypasses that pop and
// leaks one panic frame per call; after AETHER_PANIC_MAX_DEPTH = 32
// calls the runtime aborts.  This helper fires one
// `aether_try_pop();` per live try frame so the runtime stack
// matches the codegen-state counter on entry to the next
// instruction.  The codegen-state counter is itself unchanged: it
// reflects lexical nesting, not runtime control flow.  In practice
// every call site is immediately followed by a `return ...;`, so
// the next instruction we care about is in the caller's frame.
//
// The catch handler is unaffected: AST_TRY_STATEMENT emits
// aether_try_pop() before the handler runs, and the codegen-state
// counter is back to caller's value while the handler is generated,
// so this helper correctly fires zero pops for returns inside catch.
void emit_try_pops_for_nonlocal_exit(CodeGenerator* gen) {
    if (!gen || gen->try_frame_depth <= 0) return;
    for (int i = 0; i < gen->try_frame_depth; i++) {
        print_indent(gen);
        fprintf(gen->output, "aether_try_pop();\n");
    }
}

// `break` / `continue` drain only try frames pushed inside the
// current loop body — frames pushed outside the loop survive a
// break/continue and must NOT be popped here.
void emit_try_pops_for_break_continue(CodeGenerator* gen) {
    if (!gen || gen->loop_nest_depth <= 0) return;
    int base = gen->loop_try_base[gen->loop_nest_depth - 1];
    int drop = gen->try_frame_depth - base;
    if (drop <= 0) return;
    for (int i = 0; i < drop; i++) {
        print_indent(gen);
        fprintf(gen->output, "aether_try_pop();\n");
    }
}

// ----------------------------------------------------------------------
// Issue #501 follow-up: try-clobbered locals tracking.
//
// A C local that's declared in an enclosing scope of a `setjmp` and
// modified between `setjmp` and `longjmp` has indeterminate value
// after longjmp unless declared `volatile`.  Aether's try/catch
// codegen emits `AETHER_SIGSETJMP` and `aether_panic` siglongjmps;
// vars assigned inside a try body that were declared OUTSIDE the
// body are at risk.  These helpers populate and query a per-
// function set of clobbered names; the AST_VARIABLE_DECLARATION
// codegen consults `is_try_clobbered_var` to decide whether to
// prefix the emitted C type with `volatile`.
// ----------------------------------------------------------------------

int is_try_clobbered_var(CodeGenerator* gen, const char* name) {
    if (!gen || !name) return 0;
    for (int i = 0; i < gen->try_clobbered_var_count; i++) {
        if (gen->try_clobbered_vars[i] &&
            strcmp(gen->try_clobbered_vars[i], name) == 0) return 1;
    }
    return 0;
}

void mark_try_clobbered_var(CodeGenerator* gen, const char* name) {
    if (!gen || !name) return;
    if (is_try_clobbered_var(gen, name)) return;
    char** grown = realloc(gen->try_clobbered_vars,
                           sizeof(char*) * (gen->try_clobbered_var_count + 1));
    if (!grown) return;
    gen->try_clobbered_vars = grown;
    gen->try_clobbered_vars[gen->try_clobbered_var_count] = strdup(name);
    gen->try_clobbered_var_count++;
}

void clear_try_clobbered_vars(CodeGenerator* gen) {
    if (!gen) return;
    if (gen->try_clobbered_vars) {
        for (int i = 0; i < gen->try_clobbered_var_count; i++) {
            free(gen->try_clobbered_vars[i]);
        }
        free(gen->try_clobbered_vars);
    }
    gen->try_clobbered_vars = NULL;
    gen->try_clobbered_var_count = 0;
}

// Walk `body` looking for AST_TRY_STATEMENT nodes; for each, walk
// the try body recursively and mark every variable name on the LHS
// of any assignment-shaped node.  Conservative: a var declared
// *inside* the try body also gets marked, but the volatile prefix
// is only applied at AST_VARIABLE_DECLARATION sites where
// gen->try_frame_depth == 0 (i.e. only when we're emitting a decl
// in an outer scope of the try, never inside one).
// Helper: when `lhs` is an assignment LHS, find the bare variable
// name at its root and mark it as try-clobbered.  For:
//   x = ...           → mark "x"
//   x.field = ...     → mark "x"  (member access)
//   x[i] = ...        → mark "x"  (array index)
//   x.f.g = ...       → mark "x"  (nested member access)
//   x[i].f = ...      → mark "x"
// Conservative: marks the storage owner even when the LHS writes
// to a heap-allocated sub-object (where volatile wouldn't matter)
// — harmless, just slightly more `volatile` than strictly needed.
static void mark_lhs_root_var(CodeGenerator* gen, ASTNode* lhs) {
    while (lhs) {
        if (lhs->type == AST_IDENTIFIER && lhs->value) {
            mark_try_clobbered_var(gen, lhs->value);
            return;
        }
        if (lhs->type == AST_MEMBER_ACCESS && lhs->child_count > 0) {
            lhs = lhs->children[0];
            continue;
        }
        if (lhs->type == AST_ARRAY_ACCESS && lhs->child_count > 0) {
            lhs = lhs->children[0];
            continue;
        }
        // Unrecognised LHS shape — give up (no var to mark).
        return;
    }
}

static void scan_try_body_for_writes(CodeGenerator* gen, ASTNode* node) {
    if (!node) return;
    // AST_VARIABLE_DECLARATION carries `value` = var name.  This is
    // the shape the Aether parser emits for `x = expr` when `x` is
    // a bare identifier at statement scope.  Mark unconditionally —
    // the volatile prefix is only applied at AST_VARIABLE_DECLARATION
    // emission sites when gen->try_frame_depth == 0 (only outer-scope
    // decls need volatile; inside-try decls live and die inside the
    // try body's C scope).
    if (node->type == AST_VARIABLE_DECLARATION && node->value) {
        mark_try_clobbered_var(gen, node->value);
    }
    // AST_ASSIGNMENT: an explicit re-assignment shape used by some
    // parser paths (struct-literal field-init for one; the general
    // `lhs = rhs` statement path goes via AST_BINARY_EXPRESSION,
    // see below).
    if (node->type == AST_ASSIGNMENT && node->child_count > 0 &&
        node->children[0]) {
        mark_lhs_root_var(gen, node->children[0]);
    }
    // AST_BINARY_EXPRESSION with op `=` / `+=` / `-=` / etc. — this
    // is the shape Aether's parser produces for a member-access or
    // array-index assignment like `b.val = 42` or `arr[i] = x`.
    // `parse_binary_expression` consumes the `=` token like any
    // other operator and builds a binary node; the statement layer
    // wraps it in AST_EXPRESSION_STATEMENT.
    if (node->type == AST_BINARY_EXPRESSION && node->value &&
        node->child_count >= 1) {
        const char* op = node->value;
        int is_assign =
            (op[0] == '=' && op[1] == '\0') ||
            (op[0] != '\0' && op[1] == '=' && op[2] == '\0' &&
             (op[0] == '+' || op[0] == '-' || op[0] == '*' ||
              op[0] == '/' || op[0] == '%' || op[0] == '&' ||
              op[0] == '|' || op[0] == '^'));
        if (is_assign) {
            mark_lhs_root_var(gen, node->children[0]);
        }
    }
    // AST_COMPOUND_ASSIGNMENT: a few parser paths produce this
    // discrete node type instead of folding into AST_BINARY_EXPRESSION.
    if (node->type == AST_COMPOUND_ASSIGNMENT && node->child_count > 0 &&
        node->children[0]) {
        mark_lhs_root_var(gen, node->children[0]);
    }
    for (int i = 0; i < node->child_count; i++) {
        scan_try_body_for_writes(gen, node->children[i]);
    }
}

static void walk_for_try_clobbered(CodeGenerator* gen, ASTNode* node) {
    if (!node) return;
    if (node->type == AST_TRY_STATEMENT && node->child_count >= 1) {
        // children[0] is the try body; children[1] is the catch clause.
        // Vars modified inside the body are the at-risk set.
        scan_try_body_for_writes(gen, node->children[0]);
    }
    for (int i = 0; i < node->child_count; i++) {
        walk_for_try_clobbered(gen, node->children[i]);
    }
}

void mark_try_clobbered_vars(CodeGenerator* gen, ASTNode* body) {
    if (!gen || !body) return;
    walk_for_try_clobbered(gen, body);
}

// Returns "volatile " when `name` is a try-clobbered local and the
// current codegen position is OUTSIDE any try body in this
// function.  An inside-try declaration is scoped to the try body
// and lives/dies inside it; only outer-scope decls cross the
// setjmp boundary and need volatile.  Returns "" otherwise.
const char* try_volatile_qual_for(CodeGenerator* gen, const char* name) {
    if (!gen || !name) return "";
    if (gen->try_frame_depth > 0) return "";
    if (!is_try_clobbered_var(gen, name)) return "";
    return "volatile ";
}

// ============================================================================
// Header Generation Functions (for --emit-header)
// ============================================================================

// Convert filename to uppercase guard name (e.g., "counter.h" -> "COUNTER_H")
static void make_guard_name(const char* path, char* guard, size_t guard_size) {
    const char* filename = path;
    // Find last path separator
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') filename = p + 1;
    }

    size_t i = 0;
    for (; filename[i] && i < guard_size - 1; i++) {
        char c = filename[i];
        if (c == '.') guard[i] = '_';
        else if (c >= 'a' && c <= 'z') guard[i] = c - 32;  // toupper
        else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') guard[i] = c;
        else guard[i] = '_';
    }
    guard[i] = '\0';
}

void emit_header_prologue(CodeGenerator* gen, const char* guard_name) {
    if (!gen->header_file) return;

    char guard[256];
    if (guard_name) {
        strncpy(guard, guard_name, sizeof(guard) - 1);
        guard[sizeof(guard) - 1] = '\0';
    } else if (gen->header_path) {
        make_guard_name(gen->header_path, guard, sizeof(guard));
    } else {
        snprintf(guard, sizeof(guard), "AETHER_GENERATED_H");
    }

    fprintf(gen->header_file, "// Auto-generated by aetherc - DO NOT EDIT\n");
    fprintf(gen->header_file, "// Generated from Aether source for C embedding\n");
    fprintf(gen->header_file, "#ifndef %s\n", guard);
    fprintf(gen->header_file, "#define %s\n\n", guard);
    fprintf(gen->header_file, "#include <stdint.h>\n");
    fprintf(gen->header_file, "#include \"runtime/scheduler/multicore_scheduler.h\"\n");
    fprintf(gen->header_file, "\n");
    fprintf(gen->header_file, "// Forward declarations\n");
}

void emit_header_epilogue(CodeGenerator* gen) {
    if (!gen->header_file) return;

    fprintf(gen->header_file, "\n#endif // header guard\n");
}

void emit_message_to_header(CodeGenerator* gen, ASTNode* msg_def) {
    if (!gen->header_file || !msg_def || !msg_def->value) return;

    const char* msg_name = msg_def->value;
    MessageDef* msg_entry = lookup_message(gen->message_registry, msg_name);
    int msg_id = msg_entry ? msg_entry->message_id : 0;

    fprintf(gen->header_file, "\n// Message: %s\n", msg_name);
    fprintf(gen->header_file, "#define MSG_%s %d\n", msg_name, msg_id);

    // Generate struct typedef
    fprintf(gen->header_file, "typedef struct {\n");
    fprintf(gen->header_file, "    int _message_id;\n");

    // Check if this message uses the inline payload_int path (single int field).
    // If so, the field must be intptr_t to match Message.payload_int's width,
    // which is pointer-sized to allow actor refs stored in int message fields.
    MessageDef* reg_def = lookup_message(gen->message_registry, msg_name);
    int uses_inline = reg_def && get_single_int_field(reg_def) != NULL;

    for (int i = 0; i < msg_def->child_count; i++) {
        ASTNode* field = msg_def->children[i];
        if (field && field->type == AST_MESSAGE_FIELD && field->value) {
            const char* c_type = "int";  // Default
            if (field->node_type) {
                c_type = get_c_type(field->node_type);
            }
            // Inline-path int fields use intptr_t to match payload_int width
            if (uses_inline && field->node_type && field->node_type->kind == TYPE_INT) {
                c_type = "intptr_t";
            }
            fprintf(gen->header_file, "    %s %s;\n", c_type, field->value);
        }
    }

    fprintf(gen->header_file, "} %s;\n", msg_name);
}

void emit_actor_to_header(CodeGenerator* gen, ASTNode* actor) {
    if (!gen->header_file || !actor || !actor->value) return;

    const char* actor_name = actor->value;

    fprintf(gen->header_file, "\n// Actor: %s\n", actor_name);
    fprintf(gen->header_file, "typedef struct %s %s;\n", actor_name, actor_name);
    fprintf(gen->header_file, "%s* spawn_%s(void);\n", actor_name, actor_name);

    // Generate typed send helpers for each message this actor handles
    // We look for receive handlers in the actor definition
    for (int i = 0; i < actor->child_count; i++) {
        ASTNode* child = actor->children[i];
        if (child && child->type == AST_RECEIVE_STATEMENT) {
            // Each handler arm in the receive statement
            for (int j = 0; j < child->child_count; j++) {
                ASTNode* handler = child->children[j];
                if (handler && handler->type == AST_RECEIVE_ARM && handler->value) {
                    const char* msg_name = handler->value;
                    MessageDef* msg_def = lookup_message(gen->message_registry, msg_name);
                    int msg_id = msg_def ? msg_def->message_id : 0;

                    // Generate inline send helper
                    fprintf(gen->header_file, "\nstatic inline void %s_%s(%s* actor",
                            actor_name, msg_name, actor_name);

                    // Add parameters for each field
                    if (msg_def && msg_def->fields) {
                        MessageFieldDef* field = msg_def->fields;
                        while (field) {
                            const char* c_type = "int";
                            switch (field->type_kind) {
                                case TYPE_INT: c_type = "int"; break;
                                /* See get_c_type() — Aether `float` is always C `double`. */
                                case TYPE_FLOAT: c_type = "double"; break;
                                case TYPE_LONGDOUBLE: c_type = "long double"; break;
                                case TYPE_STRING: c_type = "const char*"; break;
                                case TYPE_BOOL: c_type = "int"; break;
                                case TYPE_BYTE: c_type = "unsigned char"; break;
                                default: c_type = "int"; break;
                            }
                            fprintf(gen->header_file, ", %s %s", c_type, field->name);
                            field = field->next;
                        }
                    }

                    fprintf(gen->header_file, ") {\n");
                    fprintf(gen->header_file, "    Message msg = {0};\n");
                    fprintf(gen->header_file, "    msg.type = %d;\n", msg_id);

                    // For single-int messages, use payload_int
                    if (msg_def && msg_def->fields && !msg_def->fields->next &&
                        msg_def->fields->type_kind == TYPE_INT) {
                        fprintf(gen->header_file, "    msg.payload_int = %s;\n", msg_def->fields->name);
                    }

                    fprintf(gen->header_file, "    scheduler_send_remote((ActorBase*)actor, msg, -1);\n");
                    fprintf(gen->header_file, "}\n");
                }
            }
        }
    }
}

void print_line(CodeGenerator* gen, const char* format, ...) {
    print_indent(gen);

    va_list args;
    va_start(args, format);
    vfprintf(gen->output, format, args);
    va_end(args);

    fprintf(gen->output, "\n");
}

// Emit a `#line N "path"` directive when this node sits on a source
// line different from the last-emitted directive. Idempotent — back-
// to-back nodes on the same line trigger no output. NULL nodes,
// nodes without a source_file (synthetic), and nodes with line <= 0
// are silently skipped — codegen falls through to the previously
// active `#line`, which is safe because we always have one set by
// the start of the first real function. Path is emitted with `"`
// and `\\` escaped so paths containing those characters round-trip
// through the C preprocessor.
void codegen_maybe_emit_line(CodeGenerator* gen, const ASTNode* node) {
    if (!gen || !node) return;
    if (!node->source_file || node->line <= 0) return;
    if (gen->last_line_file &&
        node->line == gen->last_line_num &&
        strcmp(node->source_file, gen->last_line_file) == 0) {
        return;
    }
    fprintf(gen->output, "#line %d \"", node->line);
    for (const char* p = node->source_file; *p; p++) {
        if (*p == '\\' || *p == '"') fputc('\\', gen->output);
        fputc(*p, gen->output);
    }
    fprintf(gen->output, "\"\n");
    gen->last_line_file = node->source_file;
    gen->last_line_num = node->line;
}

// Check if a name is a C/C++ reserved keyword OR a libc / POSIX symbol
// that would cause a link-time collision when emitted as a bare C
// function. Hits get prefixed with `ae_` by `safe_c_name` so the
// Aether-side identifier (e.g. `bind`) keeps its natural spelling
// while the C symbol becomes `ae_bind` and stays out of libc's way.
//
// Closes #436 facet B (flat C symbol namespace). The architectural
// goal — module-qualified codegen for every Aether function — is a
// larger refactor that breaks `--emit=lib` consumers who dlsym() for
// unprefixed names. The list-based mitigation here covers the
// concrete collision class (libc / POSIX symbols, the ones that
// actually cause linker breaks in practice) while preserving the
// existing C ABI surface. The Aether-side name resolution is
// unchanged; only the EMITTED C symbol is renamed when a collision
// is in flight.
//
// The list is curated from POSIX.1-2017 + common glibc/musl
// extensions. Categories:
//   - C/C++ reserved keywords (cannot be C identifiers at all)
//   - libc network sockets API (the class the issue called out)
//   - libc POSIX I/O (open / read / write / pipe / fcntl / ioctl)
//   - libc process control (fork / exec / wait / kill / signal)
//   - libc memory + dynamic linking
//   - libc string + stdio
//   - libc time + env
// Each category is delimited by a comment so future additions land
// in the right block.
int is_c_reserved_word(const char* name) {
    static const char* reserved[] = {
        // ── C keywords ─────────────────────────────────────────
        "auto", "break", "case", "char", "const", "continue", "default", "do",
        "double", "else", "enum", "extern", "float", "for", "goto", "if",
        "inline", "int", "long", "register", "restrict", "return", "short",
        "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
        "unsigned", "void", "volatile", "while",
        // ── C99 / C11 keywords ─────────────────────────────────
        "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex", "_Generic",
        "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local",
        // ── libc network sockets API (POSIX.1 + BSD) ───────────
        // This is the original #436 facet B trigger: an Aether
        // function literally named `bind` collides with libc's
        // `bind(2)` and the linker silently picks the wrong one.
        "socket", "bind", "listen", "accept", "connect", "shutdown",
        "send", "recv", "sendto", "recvfrom", "sendmsg", "recvmsg",
        "getsockname", "getpeername", "getsockopt", "setsockopt",
        "select", "poll", "epoll_create", "epoll_ctl", "epoll_wait",
        "kqueue", "kevent", "inet_pton", "inet_ntop",
        "gethostbyname", "getaddrinfo", "freeaddrinfo",
        // ── libc POSIX I/O ─────────────────────────────────────
        "open", "openat", "close", "read", "write", "pread", "pwrite",
        "readv", "writev", "lseek", "fcntl", "ioctl", "pipe", "pipe2",
        "dup", "dup2", "dup3", "fsync", "fdatasync", "sync",
        "mkdir", "mkdirat", "rmdir", "unlink", "unlinkat", "rename", "renameat",
        "link", "linkat", "symlink", "symlinkat", "readlink", "readlinkat",
        "chmod", "fchmod", "chown", "fchown", "lchown",
        "stat", "fstat", "lstat", "fstatat", "access", "faccessat",
        "truncate", "ftruncate", "umask",
        "fopen", "fclose", "freopen", "fread", "fwrite", "fseek", "ftell",
        "rewind", "fflush", "fileno", "feof", "ferror",
        "fprintf", "fscanf", "vfprintf", "vfscanf",
        "fgets", "fputs", "fgetc", "fputc", "ungetc",
        "getc", "putc", "getchar", "putchar",
        // ── libc process control ───────────────────────────────
        "fork", "vfork", "execl", "execlp", "execle",
        "execv", "execvp", "execve", "execveat",
        "wait", "waitpid", "waitid", "wait3", "wait4",
        "kill", "killpg", "raise", "signal", "sigaction", "sigprocmask",
        "alarm", "pause", "sleep", "usleep", "nanosleep",
        "getpid", "getppid", "gettid", "getsid", "getpgrp", "getpgid",
        "setpgid", "setsid", "setpgrp",
        "getuid", "geteuid", "getgid", "getegid",
        "setuid", "seteuid", "setgid", "setegid",
        "abort", "exit", "_exit", "_Exit", "atexit",
        // ── libc memory + dynamic linking ──────────────────────
        "malloc", "calloc", "realloc", "reallocarray", "free",
        "mmap", "munmap", "mremap", "mprotect", "madvise", "msync",
        "brk", "sbrk", "posix_memalign", "aligned_alloc", "valloc",
        "dlopen", "dlsym", "dlclose", "dlerror", "dladdr",
        // ── libc string + stdio + memory ops ───────────────────
        "strlen", "strnlen", "strcpy", "strncpy", "stpcpy", "stpncpy",
        "strcat", "strncat", "strcmp", "strncmp", "strcasecmp", "strncasecmp",
        "strchr", "strrchr", "strstr", "strdup", "strndup",
        "strtok", "strtok_r", "strtol", "strtoll", "strtoul", "strtoull",
        "strtod", "strtof", "strerror",
        "sprintf", "snprintf", "vsprintf", "vsnprintf",
        "sscanf", "vsscanf",
        "printf", "puts", "gets", "perror",
        "memcpy", "memmove", "memset", "memcmp", "memchr", "memrchr",
        // ── libc time + env + misc ─────────────────────────────
        "time", "clock", "gettimeofday", "clock_gettime", "clock_settime",
        "gmtime", "localtime", "mktime", "asctime", "ctime", "strftime",
        "getenv", "setenv", "unsetenv", "putenv", "clearenv",
        "getcwd", "chdir", "fchdir", "system",
        NULL
    };
    for (int i = 0; reserved[i]; i++) {
        if (strcmp(name, reserved[i]) == 0) return 1;
    }
    return 0;
}

// Mangle an Aether name to avoid C reserved word collision.
// Returns a static buffer — caller must use before next call.
const char* safe_c_name(const char* name) {
    if (!name) return name;
    if (!is_c_reserved_word(name)) return name;
    static char buf[280];
    snprintf(buf, sizeof(buf), "ae_%s", name);
    return buf;
}

// #976: is `name` a C reserved keyword (as opposed to a libc symbol)?
// These are the names that cannot be a C identifier AT ALL — a variable,
// parameter, or struct field spelled this way is a hard C syntax error, not
// a link-time collision. A perfectly ordinary Aether identifier (`short`,
// `long`, `char`, `default`) lands here and must be mangled in codegen so it
// still compiles. Includes C89/C99/C11 keywords plus the common
// header-provided keywords/macros (`bool`/`true`/`false` from <stdbool.h>,
// C23 spellings) that the generated prelude may pull in.
int is_c_keyword(const char* name) {
    if (!name) return 0;
    static const char* kw[] = {
        // C89
        "auto", "break", "case", "char", "const", "continue", "default", "do",
        "double", "else", "enum", "extern", "float", "for", "goto", "if",
        "int", "long", "register", "return", "short", "signed", "sizeof",
        "static", "struct", "switch", "typedef", "union", "unsigned", "void",
        "volatile", "while",
        // C99 / C11
        "inline", "restrict",
        "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex", "_Generic",
        "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local",
        // <stdbool.h> / C23 spellings that behave as keywords/macros
        "bool", "true", "false", "nullptr", "typeof", "typeof_unqual",
        "alignas", "alignof", "static_assert", "thread_local", "constexpr",
        NULL
    };
    for (int i = 0; kw[i]; i++) {
        if (strcmp(name, kw[i]) == 0) return 1;
    }
    return 0;
}

/* #1256 follow-up: names the Windows SDK claims at file scope. With module
 * consts lowered to real C identifiers (static const) instead of #defines,
 * a const named DOUBLE collides with windef.h's `typedef double DOUBLE` in
 * any generated TU that includes <windows.h>: "redeclared as a different
 * kind of symbol". TRUE/FALSE are macros there, which is worse: the
 * preprocessor rewrites the declaration itself. Mangled on every platform
 * so programs behave identically on POSIX and Windows. Curated to the
 * windef.h/winnt.h names plausible as user constants. */
static int is_winapi_reserved_name(const char* name) {
    if (!name) return 0;
    static const char* win[] = {
        "BOOL", "BOOLEAN", "BYTE", "CHAR", "DOUBLE", "DWORD", "DWORD64",
        "FLOAT", "HANDLE", "INT", "INT8", "INT16", "INT32", "INT64",
        "LONG", "LONG64", "LPSTR", "LPCSTR", "LPVOID", "PVOID", "SHORT",
        "SIZE_T", "SSIZE_T", "UCHAR", "UINT", "UINT8", "UINT16", "UINT32",
        "UINT64", "ULONG", "ULONG64", "USHORT", "VOID", "WCHAR", "WORD",
        "TRUE", "FALSE", "NULL", "IGNORE", "INFINITE", "ERROR", "OPTIONAL",
        NULL
    };
    for (int i = 0; win[i]; i++) {
        if (strcmp(name, win[i]) == 0) return 1;
    }
    return 0;
}

// #976: mangle a value/variable identifier that is a C keyword to a valid C
// identifier. Non-keywords pass through unchanged, so normal code is emitted
// verbatim and only the (previously broken) keyword names are rewritten. The
// `ae_` prefix matches safe_c_name's convention. Must be applied at EVERY
// site that emits this value's C name (declaration, reference, parameter,
// capture field, loop var, …) so the spelling stays consistent.
const char* safe_value_name(const char* name) {
    if (!name) return name;
    if (!is_c_keyword(name)) return name;
    static char buf[280];
    snprintf(buf, sizeof(buf), "ae_%s", name);
    return buf;
}

/* Source position of the construct codegen is lowering right now.
 * get_c_type() and the other Type-only helpers have no AST node to blame,
 * so a diagnostic raised from inside them would print with no file or line
 * and be impossible to act on. generate_statement / generate_expression /
 * the function emitter keep this current as they walk. */
/* Copied, not borrowed: the names belong to AST nodes, and a caller that
 * frees a program and compiles another in the same process (the compiler's
 * own unit tests do exactly that) would otherwise leave these pointing into
 * freed memory, to be read by the next diagnostic. */
static char g_diag_file[512];
static int g_diag_line = 0;
static int g_diag_column = 0;
static char g_diag_func[160];
static char g_diag_context[192];

void codegen_note_diag_pos(const ASTNode* node) {
    if (!node || node->line <= 0) return;
    g_diag_line = node->line;
    g_diag_column = node->column;
    if (node->source_file) {
        snprintf(g_diag_file, sizeof(g_diag_file), "%s", node->source_file);
    }
}

void codegen_note_diag_func(const char* name) {
    if (name) {
        snprintf(g_diag_func, sizeof(g_diag_func), "%s", name);
    } else {
        g_diag_func[0] = '\0';
    }
}

/* NULL rather than "" when unset: the reporter prints the filename only when
 * it has one, and an empty string is not the same as absent. */
static const char* codegen_diag_file(void) {
    return g_diag_file[0] ? g_diag_file : NULL;
}

const char* codegen_diag_context(void) {
    if (!g_diag_func[0]) return NULL;
    snprintf(g_diag_context, sizeof(g_diag_context), "in function %s", g_diag_func);
    return g_diag_context;
}

const char* get_c_type(Type* type) {
    if (!type) {
        AetherError w = {NULL, NULL, 0, 0, "internal: NULL type in codegen, defaulting to int",
                         "this is a compiler bug, please report it", NULL, AETHER_ERR_NONE};
        aether_warning_report(&w);
        return "int";
    }

    /* C ABI scalar alias — emit the exact C spelling (size_t,
     * uint32_t, intptr_t, ...) so an Aether `extern` prototype matches
     * the system header. The alias's `kind` still drives everything
     * else; only the emitted name differs. */
    if (type->c_alias) {
        return type->c_alias;
    }

    switch (type->kind) {
        case TYPE_INT: return "int";
        case TYPE_INT64: return "int64_t";
        case TYPE_UINT64: return "uint64_t";
        case TYPE_UINT32: return "uint32_t";
        case TYPE_UINT16: return "uint16_t";
        case TYPE_UINT8: return "uint8_t";
        case TYPE_DURATION: return "int64_t";
        /* Aether `float` lowers to C `double`. The naming is legacy
         * (Aether predates having two FP types), but the storage and
         * ABI have always been 8-byte IEEE-754 — local variables are
         * stored as C-double via bare `1.0` literals (which are
         * C-double, not C-float), and extern function signatures
         * already emit `double` (see codegen_func.c:325). Struct
         * fields and forward declarations were the holdouts emitting
         * 4-byte C `float`, which created an ABI mismatch when an
         * Aether-defined function was called from C with a `double`
         * argument. Now consistent everywhere. */
        case TYPE_FLOAT: return "double";
        case TYPE_LONGDOUBLE: return "long double";
        case TYPE_FLOAT32: return "float";
        case TYPE_BOOL: return "int";
        /* `unsigned char` (not `uint8_t`) so the compiler's strict-aliasing
         * exemption applies: code may legally read or write any other
         * type's bytes through an `unsigned char *`. uint8_t is a typedef
         * of `unsigned char` on most platforms but the C standard does
         * not require it; using uint8_t* to scrape bytes from another
         * type's storage is technically a strict-aliasing violation.
         * `unsigned char` is the right C type for "the octet at this
         * address" semantics — which is exactly what `byte` means. */
        case TYPE_BYTE: return "unsigned char";
        case TYPE_STRING: return "const char*";
        case TYPE_VOID: return "void";
        case TYPE_ACTOR_REF: {
            // Rotating buffers prevent clobber when get_c_type() is called
            // multiple times in the same printf/expression
            static char buffers[4][256];
            static int buf_idx = 0;
            char* buffer = buffers[buf_idx++ & 3];
            if (type->element_type && type->element_type->kind == TYPE_STRUCT && type->element_type->struct_name) {
                snprintf(buffer, 256, "%s*", type->element_type->struct_name);
            } else {
                snprintf(buffer, 256, "void*");
            }
            return buffer;
        }
        case TYPE_MESSAGE: return "Message";
        case TYPE_PTR: {
            /* `*StructName` (TYPE_PTR with a TYPE_STRUCT element) renders
             * as `StructName*` so the C type carries the struct identity
             * across declarations / parameters / returns. Bare `ptr` (no
             * element type) stays `void*`. Mirrors the TYPE_ACTOR_REF
             * shape just below.
             *
             * For `@c_import` structs we emit `struct Name*` instead:
             * aetherc doesn't synthesise a `typedef struct Name Name;`
             * for those (the header is supposed to), but some POSIX
             * headers (notably <time.h> with `struct tm`) don't ship
             * the convenience typedef.  `struct Name*` is universally
             * portable. */
            if (type->element_type && type->element_type->kind == TYPE_STRUCT &&
                type->element_type->struct_name) {
                static char buffers[4][256];
                static int buf_idx = 0;
                char* buffer = buffers[buf_idx++ & 3];
                const char* sname = type->element_type->struct_name;
                if (aether_is_c_struct_overlay(sname)) {
                    /* #891: a @c_struct overlay pointer is just a raw `void*`
                     * in the emitted C — there is no C struct type. Field
                     * access lowers to mem accessors at offsets. */
                    return "void*";
                } else if (aether_is_c_import_struct(sname)) {
                    snprintf(buffer, 256, "struct %s*", sname);
                } else {
                    snprintf(buffer, 256, "%s*", sname);
                }
                return buffer;
            }
            return "void*";
        }
        case TYPE_STRUCT: {
            static char buffers[4][256];
            static int buf_idx = 0;
            char* buffer = buffers[buf_idx++ & 3];
            const char* sname = type->struct_name ? type->struct_name : "unnamed";
            if (type->struct_name && aether_is_c_import_struct(type->struct_name)) {
                snprintf(buffer, 256, "struct %s", sname);
            } else {
                snprintf(buffer, 256, "%s", sname);
            }
            return buffer;
        }
        case TYPE_SUM: {
            // #914: a sum type lowers to `typedef struct Name { Name_tag tag;
            // union {...} data; } Name;` (emitted by emit_sum_typedefs), so the
            // C type is just the sum's name.
            static char buffers[4][256];
            static int buf_idx = 0;
            char* buffer = buffers[buf_idx++ & 3];
            snprintf(buffer, 256, "%s", type->struct_name ? type->struct_name : "_sum");
            return buffer;
        }
        case TYPE_ENUM: {
            // #1044: an enum lowers to `typedef enum { Name_Member = v, ... }
            // Name;` (emitted by emit_enum_typedef), so the C type is the name.
            static char buffers[4][256];
            static int buf_idx = 0;
            char* buffer = buffers[buf_idx++ & 3];
            snprintf(buffer, 256, "%s", type->struct_name ? type->struct_name : "_enum");
            return buffer;
        }
        case TYPE_ISOLATED:
            /* #479: Isolated[T] is a compile-time-only, move-only wrapper. It
             * lowers to the C type of the wrapped T with zero runtime cost
             * (mirrors distinct types); isolate()/consume() are no-ops. */
            return type->element_type ? get_c_type(type->element_type) : "void*";
        case TYPE_BITSET:
            /* #1046: bit_set[E] is a set of enum members backed by an unsigned
             * 64-bit word (one bit per member, at the member's enum value).
             * Set operations lower to bitwise ops; zero runtime cost. */
            return "unsigned long long";
        case TYPE_BITSTRUCT:
            /* #1132: a bitstruct IS its backing integer — there is no C struct
             * and, deliberately, no C bitfield (whose signedness and layout are
             * implementation-defined). Field access lowers to shift/mask against
             * this word, so the layout is exact and portable. */
            return type->element_type ? get_c_type(type->element_type)
                                      : "unsigned char";
        case TYPE_ARRAY: {
            static char buffers[4][256];
            static int buf_idx = 0;
            char* buffer = buffers[buf_idx++ & 3];
            const char* element_type = get_c_type(type->element_type);
            if (type->array_size > 0) {
                snprintf(buffer, 256, "%s[%d]", element_type, type->array_size);
            } else {
                snprintf(buffer, 256, "%s*", element_type);
            }
            return buffer;
        }
        case TYPE_TUPLE: {
            static char buffers[4][256];
            static int buf_idx = 0;
            char* buffer = buffers[buf_idx++ & 3];
            int pos = snprintf(buffer, 256, "_tuple");
            for (int i = 0; i < type->tuple_count && pos < 240; i++) {
                const char* elem = get_c_type(type->tuple_types[i]);
                // Sanitize: "const char*" -> "string", "void*" -> "ptr",
                // and the space-containing spellings that would otherwise
                // produce an invalid identifier (#1033: byte tuple fields).
                if (strcmp(elem, "const char*") == 0) elem = "string";
                else if (strcmp(elem, "void*") == 0) elem = "ptr";
                else if (strcmp(elem, "unsigned char") == 0) elem = "byte";
                else if (strcmp(elem, "long double") == 0) elem = "longdouble";
                // Any remaining non-identifier character (the `*` in a
                // struct-pointer element like `Node*`, or an embedded space)
                // would make an invalid C typedef name; map it to '_', the
                // same sanitization the optional-type namer below applies.
                if (pos < 255) buffer[pos++] = '_';
                for (const char* e = elem; *e && pos < 255; e++) {
                    char c = *e;
                    int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '_';
                    buffer[pos++] = ok ? c : '_';
                }
                buffer[pos] = '\0';
            }
            return buffer;
        }
        case TYPE_OPTIONAL: {
            // #340: `T?` lowers to a per-T tagged struct `ae_opt_<T>`
            // (`{ bool has; T val; }`), emitted on first use by
            // ensure_optional_typedef. The mangled name uses the inner C
            // type, identifier-sanitised (string/ptr aliases, non-alnum -> _).
            static char buffers[4][256];
            static int buf_idx = 0;
            char* buffer = buffers[buf_idx++ & 3];
            const char* inner = type->element_type ? get_c_type(type->element_type) : "int";
            if (strcmp(inner, "const char*") == 0) inner = "string";
            else if (strcmp(inner, "void*") == 0) inner = "ptr";
            char safe[200];
            int si = 0;
            for (const char* p = inner; *p && si < 199; p++) {
                char c = *p;
                int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '_';
                safe[si++] = ok ? c : '_';
            }
            safe[si] = '\0';
            snprintf(buffer, 256, "ae_opt_%s", safe);
            return buffer;
        }
        case TYPE_FUNCTION:
            /* Typed C function pointer (storage = void*).  The call
             * site injects a `((R (*)(T1, T2))(v))` cast — keeping the
             * storage as void* avoids C-side typedef synthesis and
             * keeps the FFI path uniform across all signatures. */
            if (type->is_fnptr) return "void*";
            return "_AeClosure";
        case TYPE_UNKNOWN: {
            AetherError w = {codegen_diag_file(), NULL, g_diag_line, g_diag_column,
                             "unresolved type in codegen, defaulting to int",
                             "add explicit type annotation or check that the variable is initialized",
                             codegen_diag_context(), AETHER_ERR_NONE};
            aether_warning_report(&w);
            return "int";
        }
        default: {
            char wbuf[128];
            snprintf(wbuf, sizeof(wbuf), "internal: unknown type kind %d in codegen, defaulting to void", type->kind);
            AetherError w = {codegen_diag_file(), NULL, g_diag_line, g_diag_column, wbuf,
                             "this is a compiler bug, please report it",
                             codegen_diag_context(), AETHER_ERR_NONE};
            aether_warning_report(&w);
            return "void";
        }
    }
}

// Map an Aether type to the stable public ABI type used in aether_<name>
// alias stubs emitted by --emit=lib. This is a *public contract* — language
// bindings (Java Panama, Python ctypes, SWIG) see these signatures.
//
// Returns NULL if the type cannot be represented across the FFI boundary
// in v1 (e.g. tuples, structs, closures). Callers should skip emitting
// an alias for that function and emit a diagnostic instead.
//
// Type mapping:
//   int     -> int32_t         (fixed width for cross-language clarity)
//   int64   -> int64_t
//   uint64  -> uint64_t
//   float   -> float           (IEEE 754 binary32, matches C float)
//   bool    -> int32_t         (0/1)
//   string  -> const char*
//   ptr     -> AetherValue*    (opaque handle; see runtime/aether_config.h)
//   void    -> void            (return type only; rejected as a parameter)
//
// AetherValue* is a forward-declared opaque type in aether_config.h that
// wraps whatever internal representation Aether uses for maps/lists/ptrs.
static const char* get_abi_type(Type* type) {
    if (!type) return NULL;
    if (type->c_alias) return type->c_alias;
    switch (type->kind) {
        case TYPE_INT:    return "int32_t";
        case TYPE_INT64:  return "int64_t";
        case TYPE_UINT64: return "uint64_t";
        case TYPE_UINT32: return "uint32_t";
        case TYPE_UINT16: return "uint16_t";
        case TYPE_UINT8:  return "uint8_t";
        case TYPE_DURATION: return "int64_t";
        /* Aether `float` is C `double` (8 bytes, binary64) — see
         * get_c_type() for rationale. The public ABI (`aether_*`
         * wrapper symbols emitted with --emit=lib) follows suit. */
        case TYPE_FLOAT:  return "double";
        case TYPE_LONGDOUBLE: return "long double";
        case TYPE_FLOAT32: return "float";
        case TYPE_BOOL:   return "int32_t";
        case TYPE_BYTE:   return "unsigned char";
        case TYPE_STRING: return "const char*";
        case TYPE_VOID:   return "void";
        case TYPE_PTR:    return "AetherValue*";
        default:          return NULL;
    }
}

// Emit `aether_<name>` alias stubs after normal top-level function emission.
// Called from generate_program only when --emit=lib or --emit=both is set.
//
// For each top-level AST_FUNCTION_DEFINITION whose parameter and return types
// are all representable in the public ABI, emit a wrapper:
//
//   int32_t aether_sum(int32_t a, int32_t b) { return sum(a, b); }
//
// Functions with unsupported types (tuples, structs, closures) are skipped
// with a compile-time warning so the user knows the function won't be
// exposed across the FFI boundary.
static void emit_lib_alias_stubs(CodeGenerator* gen, ASTNode* program) {
    if (!gen || !gen->emit_lib || !program) return;

    fprintf(gen->output, "\n/* --- aether_<name> alias stubs (--emit=lib) --- */\n");
    fprintf(gen->output, "#include <stdint.h>\n");
    fprintf(gen->output, "typedef struct AetherValue AetherValue;  /* opaque */\n");

    /* #996 --emit=csrc: header preamble (include guard + stdint). Each export's
     * prototype is written into the loop below. `string` params/returns cross
     * the ABI as `const char*` (aether_string_data-unwrapped), matching the
     * alias-stub contract. */
    if (gen->csrc_header_file) {
        fprintf(gen->csrc_header_file,
                "/* Auto-generated by aetherc --emit=csrc. The public C ABI of\n"
                " * this Aether library: aether_<name> catalog exports. */\n"
                "#ifndef AETHER_EMIT_CSRC_H\n#define AETHER_EMIT_CSRC_H\n"
                "#include <stdint.h>\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n");
    }
    /* A `string`-returning exported function now yields a magic
     * AetherString internally; the C ABI contract is a plain `const char*`,
     * so unwrap to the payload at the boundary (matches the pre-magic
     * plain-char* return the C consumer expects). aether_string_data is
     * magic-aware and NULL-safe. */
    fprintf(gen->output, "extern const char* aether_string_data(const void*);\n\n");

    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        ASTNode* fn = child;
        // Unwrap `export` declarations.
        if (child->type == AST_EXPORT_STATEMENT && child->child_count > 0) {
            fn = child->children[0];
        }
        if (!fn || fn->type != AST_FUNCTION_DEFINITION || !fn->value) continue;
        // Skip cloned-from-import functions (they're marked static and
        // are an implementation detail of the importer).
        if (fn->is_imported) continue;

        // Skip trailing-underscore "private helper" convention: `foo_`
        // means file-local. Emitting an aether_<name> alias for it
        // (a) leaks an internal name into the public ABI, and
        // (b) causes a duplicate-symbol link error when two .ae files
        //     in the same --namespace bundle pick the same helper
        //     name (they each generate their own alias). Closes #279.
        size_t name_len = strlen(fn->value);
        if (name_len > 0 && fn->value[name_len - 1] == '_') continue;

        // Check that every param type is ABI-representable.
        // The last non-guard, non-block child is the body; everything before
        // is parameters (plus optional guard clauses).
        int ok = 1;
        const char* param_types[32];
        const char* param_names[32];
        int param_count = 0;
        for (int p = 0; p < fn->child_count; p++) {
            ASTNode* c = fn->children[p];
            if (c->type == AST_GUARD_CLAUSE) continue;
            if (c->type == AST_BLOCK) continue;
            if (c->type == AST_VARIABLE_DECLARATION || c->type == AST_PATTERN_VARIABLE) {
                const char* t = get_abi_type(c->node_type);
                if (!t || strcmp(t, "void") == 0 || param_count >= 32) { ok = 0; break; }
                param_types[param_count] = t;
                param_names[param_count] = c->value ? c->value : "_unnamed";
                param_count++;
            } else {
                // Pattern literals, struct patterns, list patterns — not ABI-safe.
                ok = 0;
                break;
            }
        }
        if (!ok) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "function '%s' has a parameter type that isn't representable in the --emit=lib ABI; skipping alias stub",
                     fn->value);
            AetherError w = {NULL, NULL, fn->line, fn->column, msg,
                             "use only int, int64, uint64, float, bool, string, or ptr in public API functions",
                             NULL, AETHER_ERR_NONE};
            aether_warning_report(&w);
            continue;
        }

        // Return type. If the function has a return-with-value but node_type
        // is void/unknown, fall back to int32_t (mirrors the internal rule
        // of defaulting unknown returns to int).
        //
        // EXCEPT: if the return type is non-NULL but a *tuple*, the
        // alias would emit a signature that doesn't match the function
        // (`int32_t aether_helper(...)` calling a `_tuple_int_int`
        // returner). Skip the alias entirely with the same warning the
        // parameter-side check uses. Closes #277.
        const char* ret_abi = get_abi_type(fn->node_type);
        int returns_value = has_return_value(fn);
        int return_is_tuple = (fn->node_type && fn->node_type->kind == TYPE_TUPLE);
        if (return_is_tuple) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "function '%s' returns a tuple; --emit=lib alias stub skipped (tuples aren't part of the public ABI)",
                     fn->value);
            AetherError w = {NULL, NULL, fn->line, fn->column, msg,
                             "wrap the tuple-returning function with one that returns a single ABI-safe value if it should be exposed across the library boundary",
                             NULL, AETHER_ERR_NONE};
            aether_warning_report(&w);
            continue;
        }
        if (!ret_abi) {
            if (returns_value) {
                ret_abi = "int32_t";
            } else {
                ret_abi = "void";
            }
        }

        // #996 --emit=csrc: mirror the public prototype into the header.
        if (gen->csrc_header_file) {
            fprintf(gen->csrc_header_file, "%s aether_%s(", ret_abi, fn->value);
            if (param_count == 0) {
                fprintf(gen->csrc_header_file, "void");
            } else {
                for (int k = 0; k < param_count; k++) {
                    if (k > 0) fprintf(gen->csrc_header_file, ", ");
                    fprintf(gen->csrc_header_file, "%s %s", param_types[k], param_names[k]);
                }
            }
            fprintf(gen->csrc_header_file, ");\n");
        }

        // Emit: RET aether_NAME(PARAMS) { [return] NAME(args); }
        fprintf(gen->output, "%s aether_%s(", ret_abi, fn->value);
        if (param_count == 0) {
            fprintf(gen->output, "void");
        } else {
            for (int k = 0; k < param_count; k++) {
                if (k > 0) fprintf(gen->output, ", ");
                fprintf(gen->output, "%s %s", param_types[k], param_names[k]);
            }
        }
        fprintf(gen->output, ") {\n    ");
        int ret_is_string = (fn->node_type && fn->node_type->kind == TYPE_STRING);
        if (strcmp(ret_abi, "void") != 0) fprintf(gen->output, "return ");
        /* Unwrap a magic AetherString return to its C `char*` payload so
         * C callers (dlsym'd function pointers, etc.) read the bytes, not
         * the struct header. */
        if (ret_is_string) fprintf(gen->output, "aether_string_data((const void*)(");
        fprintf(gen->output, "%s(", fn->value);
        for (int k = 0; k < param_count; k++) {
            if (k > 0) fprintf(gen->output, ", ");
            fprintf(gen->output, "%s", param_names[k]);
        }
        fprintf(gen->output, ")");
        if (ret_is_string) fprintf(gen->output, "))");
        fprintf(gen->output, ";\n}\n");
    }

    /* #996 --emit=csrc: close the header guard. */
    if (gen->csrc_header_file) {
        fprintf(gen->csrc_header_file,
                "\n#ifdef __cplusplus\n}\n#endif\n#endif /* AETHER_EMIT_CSRC_H */\n");
    }
}

// --emit=lib symbol-catalog metadata (issue #403, MVP).
//
// Append to every library build:
//
//   * a static array of AetherLibFunction entries — one per
//     exported function (the same functions emit_lib_alias_stubs
//     wraps), carrying Aether name / C symbol / signature / source
//     file / source line.
//   * a static AetherLibMeta struct wrapping the array + schema
//     metadata.
//   * `aether_lib_meta()` — the public entry point. Consumers
//     dlsym for "aether_lib_meta" and call it.
//
// The schema is stable C structs of `const char*` and `int`. No
// dynamic allocation, no parsing, no JSON. Plain dlsym + struct
// walk. Mirrors the host-side declaration in runtime/aether_lib_meta.h.
//
// Tooling: `ae lib-info <path>` dlopens an artifact and prints the
// metadata in human-readable form. Same data is callable from any
// FFI consumer (Python ctypes, Java Panama, Ruby Fiddle, hand-rolled
// dlsym).
//
// v1 emits function entries only. The struct's `closure_count` /
// `closures` slots are reserved at zero/NULL so v2 can extend
// (closure-context records — captures + capture types per closure
// reachable from an export) without an ABI break.
/* Build a function's descriptive signature `(type1, type2, ...) -> retType`
 * as a malloc'd string the caller frees. Single source of truth for both the
 * C-literal catalog and the JSON catalog. Each type_to_string result (a
 * pointer into a shared static buffer) is copied into `buf` immediately, so a
 * later type_to_string call can't clobber an earlier field. */
static char* fn_signature_string(ASTNode* fn) {
    char buf[1024];
    int pos = 0;
    buf[pos++] = '(';
    int first = 1;
    for (int i = 0; i < fn->child_count && pos < (int)sizeof(buf) - 64; i++) {
        ASTNode* c = fn->children[i];
        if (!c) continue;
        if (c->type != AST_PATTERN_VARIABLE && c->type != AST_VARIABLE_DECLARATION) continue;
        if (!first) pos += snprintf(buf + pos, sizeof(buf) - pos, ", ");
        first = 0;
        const char* tname = c->node_type ? type_to_string(c->node_type) : "unknown";
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s", tname);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, ") -> ");
    /* No-return-type and TYPE_UNKNOWN both mean "void" at the
     * source-level surface (Aether's `foo() { ... }` with no
     * `-> T` is a void function). Render as "void" rather than
     * leaking the internal "UNKNOWN" tag through the diagnostic. */
    const char* rt = (!fn->node_type ||
                      fn->node_type->kind == TYPE_UNKNOWN ||
                      fn->node_type->kind == TYPE_VOID)
                         ? "void" : type_to_string(fn->node_type);
    snprintf(buf + pos, sizeof(buf) - pos, "%s", rt);
    return strdup(buf);
}

static void emit_lib_metadata_signature_for(FILE* out, ASTNode* fn) {
    /* Format: `(type1, type2, ...) -> retType`, the same shape that
     * docs/stdlib-reference.md uses. The signature is descriptive, not
     * parseable; consumers use it for display and switch on c_symbol for
     * actual dispatch. */
    char* s = fn_signature_string(fn);
    fputs(s, out);
    free(s);
}

/* Emit `s` as a JSON string literal (surrounding quotes included) with full
 * RFC 8259 escaping. NULL renders as an empty string. Unlike the C-literal
 * escaper this must also escape control characters and is used for fields
 * that can carry arbitrary bytes (source paths with backslashes on Windows,
 * const string values). */
static void emit_json_string(FILE* out, const char* s) {
    if (!s) { fputs("\"\"", out); return; }
    fputc('"', out);
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        switch (*p) {
            case '"':  fputs("\\\"", out); break;
            case '\\': fputs("\\\\", out); break;
            case '\b': fputs("\\b", out);  break;
            case '\f': fputs("\\f", out);  break;
            case '\n': fputs("\\n", out);  break;
            case '\r': fputs("\\r", out);  break;
            case '\t': fputs("\\t", out);  break;
            default:
                if (*p < 0x20) fprintf(out, "\\u%04x", *p);
                else fputc(*p, out);
                break;
        }
    }
    fputc('"', out);
}

static void emit_lib_metadata_c_string_literal(FILE* out, const char* s) {
    /* Defensive C-string emission. NULL → "" so consumers never see
     * a null pointer in metadata fields they expected to be a
     * string. Backslash and double-quote get the standard escape;
     * everything else passes through (Aether identifiers and type
     * names are restricted to ASCII printable). */
    if (!s) { fputs("\"\"", out); return; }
    fputc('"', out);
    for (const char* p = s; *p; p++) {
        if (*p == '\\' || *p == '"') fputc('\\', out);
        fputc(*p, out);
    }
    fputc('"', out);
}

/* Resolve a variable's source-level Type* within a function's AST, for
 * rendering closure-capture types in the lib metadata (v2). Checks the
 * function's parameters first, then top-level declarations in its body.
 * Returns NULL when the name can't be resolved (caller renders "ptr"). */
static Type* find_var_type_in_function(ASTNode* fn, const char* name) {
    if (!fn || !name) return NULL;
    ASTNode* body = NULL;
    for (int i = 0; i < fn->child_count; i++) {
        ASTNode* c = fn->children[i];
        if (!c) continue;
        if (c->type == AST_BLOCK) { body = c; break; }
        if ((c->type == AST_VARIABLE_DECLARATION || c->type == AST_PATTERN_VARIABLE) &&
            c->value && strcmp(c->value, name) == 0) {
            return c->node_type;
        }
    }
    if (!body) return NULL;
    for (int i = 0; i < body->child_count; i++) {
        ASTNode* s = body->children[i];
        if (!s) continue;
        if (s->type == AST_VARIABLE_DECLARATION && s->value &&
            strcmp(s->value, name) == 0) {
            if (s->node_type) return s->node_type;
            if (s->child_count > 0 && s->children[0]) return s->children[0]->node_type;
        }
    }
    return NULL;
}

/* Local copy of "first return expression in a subtree" — the codegen_expr
 * version is static to that TU. Used only for best-effort closure return
 * type rendering in the lib metadata. */
static ASTNode* meta_find_return_expr(ASTNode* node) {
    if (!node) return NULL;
    if (node->type == AST_RETURN_STATEMENT && node->child_count > 0) return node->children[0];
    for (int i = 0; i < node->child_count; i++) {
        ASTNode* f = meta_find_return_expr(node->children[i]);
        if (f) return f;
    }
    return NULL;
}

/* Render a closure literal's signature as "|T1, T2| -> R" from its AST.
 * The closure node's resolved node_type is unreliable at metadata-emit
 * time (params/return are resolved lazily during closure codegen), so
 * we read the AST_CLOSURE_PARAM children directly and best-effort the
 * return type. Returns a malloc'd string the caller frees. */
static char* render_closure_sig_ast(ASTNode* cnode) {
    char buf[512];
    int pos = snprintf(buf, sizeof(buf), "|");
    int first = 1;
    for (int i = 0; i < cnode->child_count && pos < (int)sizeof(buf) - 32; i++) {
        ASTNode* p = cnode->children[i];
        if (!p || p->type != AST_CLOSURE_PARAM) continue;
        if (!first) pos += snprintf(buf + pos, sizeof(buf) - pos, ", ");
        first = 0;
        const char* t = p->node_type ? type_to_string(p->node_type) : "?";
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s", t);
    }
    /* Return type: prefer the closure type's return slot; otherwise the
     * type of the body's return/arrow expression; default void. */
    const char* rt = "void";
    if (cnode->node_type && cnode->node_type->kind == TYPE_FUNCTION &&
        cnode->node_type->return_type &&
        cnode->node_type->return_type->kind != TYPE_UNKNOWN &&
        cnode->node_type->return_type->kind != TYPE_VOID) {
        rt = type_to_string(cnode->node_type->return_type);
    } else {
        /* Locate the body: an AST_BLOCK (statement body) or a bare
         * expression child (arrow body). Render the return/arrow expr's
         * type when known; "?" when a value is returned but its type
         * isn't resolved yet (closure bodies type-check lazily); "void"
         * only when there is genuinely no return value. */
        ASTNode* rexpr = NULL;
        int has_value = 0;
        for (int i = cnode->child_count - 1; i >= 0; i--) {
            ASTNode* b = cnode->children[i];
            if (!b || b->type == AST_CLOSURE_PARAM) continue;
            if (b->type == AST_BLOCK) {
                rexpr = meta_find_return_expr(b);
                has_value = (rexpr != NULL);
            } else {
                rexpr = b;          /* arrow body expression */
                has_value = 1;
            }
            break;
        }
        if (has_value) {
            rt = (rexpr && rexpr->node_type &&
                  rexpr->node_type->kind != TYPE_UNKNOWN &&
                  rexpr->node_type->kind != TYPE_VOID)
                     ? type_to_string(rexpr->node_type) : "?";
        }
    }
    char tail[64];
    snprintf(tail, sizeof(tail), "| -> %s", rt);
    /* rt may point at type_to_string's static buffer; copy via snprintf
     * into tail before it can be clobbered, then append. */
    if (pos < (int)sizeof(buf) - (int)strlen(tail) - 1) {
        snprintf(buf + pos, sizeof(buf) - pos, "%s", tail);
    }
    return strdup(buf);
}

/* Does this function's first parameter look like an injected builder
 * context (`_ctx: ptr`)? That convention (and the `builder` keyword,
 * caught separately via is_builder_func_reg) is what marks a function
 * as a trailing-block DSL entry point. */
static int fn_takes_builder_context(ASTNode* fn) {
    if (!fn) return 0;
    for (int i = 0; i < fn->child_count; i++) {
        ASTNode* c = fn->children[i];
        if (!c) continue;
        if (c->type == AST_GUARD_CLAUSE) continue;
        if (c->type == AST_BLOCK) break;
        if (c->type == AST_VARIABLE_DECLARATION || c->type == AST_PATTERN_VARIABLE) {
            return (c->value && strcmp(c->value, "_ctx") == 0 &&
                    c->node_type && c->node_type->kind == TYPE_PTR);
        }
        break;  /* first param only */
    }
    return 0;
}

/* Map an exported const's Type* to the catalog `type` string (v3). Returns
 * NULL for kinds we don't carry across the boundary (arrays, structs, ptr,
 * etc.) so the caller skips the record rather than emit a half-truth. */
static const char* lib_const_type_string(Type* t) {
    if (!t) return NULL;
    switch (t->kind) {
        case TYPE_INT:    return "int";
        case TYPE_INT64:  return "long";
        case TYPE_BOOL:   return "bool";
        case TYPE_STRING: return "string";
        case TYPE_FLOAT:
        case TYPE_LONGDOUBLE: return "float";
        default:          return NULL;
    }
}

/* Render an exported const's value expression to a source-ready Aether
 * literal, written into `out` as a C-string literal (the rehydrated stub
 * drops it verbatim after `const NAME = `). Strings are re-quoted and
 * escaped; numbers/bools pass through. Returns 1 if it could render a
 * scalar/string literal, 0 otherwise (caller skips the const). Only a bare
 * `AST_LITERAL` is rendered — a const whose RHS is an expression (`A + 1`,
 * a call) is conservatively skipped rather than mis-rendered. */
static int emit_lib_const_value_literal(FILE* out, ASTNode* val, Type* t) {
    if (!val || val->type != AST_LITERAL || !val->value || !t) return 0;
    if (t->kind == TYPE_STRING) {
        /* Re-emit as a quoted, escaped Aether string literal — the whole
         * quoted form goes into the catalog `value` field, so the stub's
         * `const NAME = "..."` is syntactically a string literal. */
        fputc('"', out);            /* opening quote of the C-string field   */
        fputc('\\', out); fputc('"', out);  /* opening quote of the Aether literal */
        for (const char* p = val->value; *p; p++) {
            unsigned char ch = (unsigned char)*p;
            switch (*p) {
                case '\n': fputs("\\n", out); break;
                case '\t': fputs("\\t", out); break;
                case '\r': fputs("\\r", out); break;
                /* Backslash and double-quote must survive BOTH the C-string
                 * field and the inner Aether literal, so escape twice. */
                case '\\': fputs("\\\\\\\\", out); break;
                case '"':  fputs("\\\\\\\"", out); break;
                default:
                    if (ch < 0x20 || ch == 0x7F) fprintf(out, "\\\\x%02x", ch);
                    else fputc(*p, out);
                    break;
            }
        }
        fputc('\\', out); fputc('"', out);  /* closing quote of the Aether literal */
        fputc('"', out);            /* closing quote of the C-string field   */
        return 1;
    }
    if (t->kind == TYPE_BOOL || t->kind == TYPE_INT || t->kind == TYPE_INT64 ||
        t->kind == TYPE_FLOAT || t->kind == TYPE_LONGDOUBLE) {
        /* Numeric / bool literal: the source token is already a valid Aether
         * literal (incl. 0x / 0o / 0b prefixes, which Aether re-accepts).
         * Emit it as the C-string field via the shared escaper. */
        emit_lib_metadata_c_string_literal(out, val->value);
        return 1;
    }
    return 0;
}

static void emit_lib_metadata(CodeGenerator* gen, ASTNode* program) {
    if (!gen || !gen->emit_lib || !program) return;

    /* Collect exportable function definitions in one pass so we can
     * emit `function_count` correctly without seeking back over
     * gen->output. Skip imported / cloned functions (they're static
     * in the artifact and not part of the public surface). */
    int max_fns = program->child_count;
    ASTNode** fns = (ASTNode**)malloc(sizeof(ASTNode*) * (max_fns > 0 ? max_fns : 1));
    int fn_count = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        ASTNode* fn = child;
        if (child && child->type == AST_EXPORT_STATEMENT && child->child_count > 0) {
            fn = child->children[0];
        }
        if (!fn) continue;
        if (fn->type != AST_FUNCTION_DEFINITION || !fn->value) continue;
        if (fn->is_imported) continue;
        /* Trailing-underscore convention marks a function file-local
         * (matches emit_lib_alias_stubs:898ish). Skip — same gate. */
        size_t nlen = strlen(fn->value);
        if (nlen > 0 && fn->value[nlen - 1] == '_') continue;
        /* @c_callback functions are always eligible — the user opted
         * the bare Aether name into the C ABI directly. Plain
         * functions must satisfy the same param/return gates as
         * emit_lib_alias_stubs (otherwise the catalog would advertise
         * a c_symbol that doesn't actually exist in the artifact). */
        if (!c_callback_symbol(fn)) {
            int param_ok = 1;
            for (int p = 0; p < fn->child_count && param_ok; p++) {
                ASTNode* c = fn->children[p];
                if (!c) continue;
                if (c->type == AST_GUARD_CLAUSE) continue;
                if (c->type == AST_BLOCK) break;
                if (c->type == AST_VARIABLE_DECLARATION ||
                    c->type == AST_PATTERN_VARIABLE) {
                    const char* t = get_abi_type(c->node_type);
                    if (!t || strcmp(t, "void") == 0) param_ok = 0;
                } else {
                    param_ok = 0;
                }
            }
            if (!param_ok) continue;
            if (fn->node_type && fn->node_type->kind == TYPE_TUPLE) continue;
        }
        fns[fn_count++] = fn;
    }

    /* --- v3: collect exported module-level scalar/string consts ---
     *
     * Export gate: if the module has an `exports (...)` list, a const is
     * cataloged only when its name appears there; absent any exports list
     * (export-everything modules) all top-level consts are eligible — the
     * same lenient posture the function pass takes. Imported/cloned consts
     * (is_imported) and typed const ARRAYS (annotation "array_const") and
     * mutable globals (annotation "global_var") are never cataloged. */
    const char** export_names = NULL;
    int export_name_count = 0;
    int has_exports_list = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if (child && child->type == AST_LINK_DIRECTIVE) {
            continue;  /* consumed by emit_link_requirements */
        }
        if (child && child->type == AST_EXPORTS_LIST) {
            has_exports_list = 1;
            export_names = (const char**)malloc(
                sizeof(char*) * (child->child_count > 0 ? child->child_count : 1));
            for (int j = 0; j < child->child_count; j++) {
                ASTNode* id = child->children[j];
                if (id && id->value) export_names[export_name_count++] = id->value;
            }
            break;
        }
    }
    /* Gather candidate const decls (name + type both renderable). */
    int max_consts = program->child_count;
    ASTNode** consts = (ASTNode**)malloc(sizeof(ASTNode*) * (max_consts > 0 ? max_consts : 1));
    const char** const_type = (const char**)malloc(sizeof(char*) * (max_consts > 0 ? max_consts : 1));
    int const_count = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        ASTNode* cd = child;
        if (child && child->type == AST_EXPORT_STATEMENT && child->child_count > 0) {
            cd = child->children[0];
        }
        if (!cd || cd->type != AST_CONST_DECLARATION || !cd->value) continue;
        if (cd->is_imported) continue;
        if (cd->annotation && (strcmp(cd->annotation, "array_const") == 0 ||
                               strcmp(cd->annotation, "global_var") == 0)) continue;
        if (cd->child_count < 1) continue;
        const char* ts = lib_const_type_string(cd->node_type);
        if (!ts) continue;  /* unsupported const type — skip, no half-record */
        /* export gate */
        if (has_exports_list) {
            int listed = 0;
            for (int k = 0; k < export_name_count; k++) {
                if (strcmp(export_names[k], cd->value) == 0) { listed = 1; break; }
            }
            if (!listed) continue;
        }
        consts[const_count] = cd;
        const_type[const_count] = ts;
        const_count++;
    }

    fprintf(gen->output,
        "\n/* --- aether_lib_meta() symbol catalog (issue #403) --- */\n");
    fprintf(gen->output, "#include <stddef.h>\n");
    /* Re-declare the schema layout-compatibly. The header that
     * defines AetherLibMeta in runtime/aether_lib_meta.h doesn't
     * have to be on the include path of every aetherc-emitted .c
     * file — emit a layout-equivalent definition under different
     * tag names and cast at the boundary, same trick
     * emit_describe_c uses. */
    fprintf(gen->output,
        "struct _AetherLibFn { const char* aether_name; const char* c_symbol;\n"
        "    const char* signature; const char* source_file; int source_line; };\n");
    fprintf(gen->output,
        "struct _AetherLibCap { const char* name; const char* type; };\n");
    fprintf(gen->output,
        "struct _AetherLibClosure { const char* name; const char* role;\n"
        "    const char* enclosing_export; const char* signature;\n"
        "    int capture_count; const struct _AetherLibCap* captures;\n"
        "    const char* source_file; int source_line; };\n");
    fprintf(gen->output,
        "struct _AetherLibConst { const char* name; const char* type;\n"
        "    const char* value; };\n");
    fprintf(gen->output,
        "struct _AetherLibMeta { const char* schema_version; const char* aether_version;\n"
        "    const char* primary_source; int function_count;\n"
        "    const struct _AetherLibFn* functions;\n"
        "    int closure_count; const struct _AetherLibClosure* closures;\n"
        "    int constant_count; const struct _AetherLibConst* constants; };\n\n");

    fprintf(gen->output,
        "static const struct _AetherLibFn _aether_lib_fns[] = {\n");
    for (int i = 0; i < fn_count; i++) {
        ASTNode* fn = fns[i];
        fprintf(gen->output, "    { ");
        emit_lib_metadata_c_string_literal(gen->output, fn->value);
        fprintf(gen->output, ", ");
        /* C symbol resolution:
         *   * @c_callback functions are exported with their bare
         *     Aether name (no mangling); the @c_callback annotation
         *     opts the symbol into the public surface directly.
         *   * Plain functions get the `aether_<name>` alias from
         *     emit_lib_alias_stubs. The user's bare name is also
         *     emitted but is not the canonical FFI surface.
         * Consumers `dlsym(handle, c_symbol)` should always return
         * a callable pointer regardless of which path the function
         * took. */
        const char* cb_sym = c_callback_symbol(fn);
        char c_sym[256];
        if (cb_sym) {
            snprintf(c_sym, sizeof(c_sym), "%s", cb_sym);
        } else {
            snprintf(c_sym, sizeof(c_sym), "aether_%s", fn->value);
        }
        emit_lib_metadata_c_string_literal(gen->output, c_sym);
        fprintf(gen->output, ", \"");
        /* Signature directly into the C-string literal — characters
         * are all safe ASCII (parens, comma, arrow, alphanumerics). */
        emit_lib_metadata_signature_for(gen->output, fn);
        fprintf(gen->output, "\", ");
        emit_lib_metadata_c_string_literal(gen->output,
            fn->source_file ? fn->source_file : "");
        fprintf(gen->output, ", %d },\n", fn->line);
    }
    fprintf(gen->output, "};\n\n");

    /* --- v2 closure-context records (schema 1.1) ---
     *
     * The closure surface reachable from the exported functions, so a
     * downstream Aether consumer can reconstruct a closure-with-context
     * builder DSL at full fidelity. Three sources, in this order:
     *
     *   1. builder      — an export that takes an injected `_ctx`
     *                     (the `builder` keyword or the `_ctx: ptr`
     *                     first-param convention). Call it with a
     *                     trailing block.
     *   2. param        — a closure-typed (`fn`) parameter of an export.
     *   3. literal      — a hoisted closure literal in an export's body
     *                     (gen->closures[] whose parent_func is one of
     *                     the exports), with its captured variables.
     *
     * Each capture array is emitted first (named _aether_lib_caps_N) so
     * the closure array can point at it; everything is `static const`
     * in .rodata, no allocation, same contract as the function table. */
    int clo_count = 0;
    /* Closure-record candidate set: every top-level, non-imported,
     * non-trailing-underscore function definition (and `builder`-keyword
     * function) — WITHOUT the ABI-param gate fns[] applies. The closure
     * surface (fn-typed params, builder `_ctx`) is exactly what the
     * flattened ABI drops, so it rides on functions fns[] excludes; their
     * bare symbols still have external linkage for an Aether consumer. */
    int cfn_cap = program->child_count > 0 ? program->child_count : 1;
    ASTNode** cfns = (ASTNode**)malloc(sizeof(ASTNode*) * cfn_cap);
    int cfn_count = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        ASTNode* fn = child;
        if (child && child->type == AST_EXPORT_STATEMENT && child->child_count > 0) {
            fn = child->children[0];
        }
        if (!fn || !fn->value) continue;
        if (fn->type != AST_FUNCTION_DEFINITION && fn->type != AST_BUILDER_FUNCTION) continue;
        if (fn->is_imported) continue;
        size_t nlen = strlen(fn->value);
        if (nlen > 0 && fn->value[nlen - 1] == '_') continue;
        cfns[cfn_count++] = fn;
    }
    /* Bookkeeping sized to: 1 builder slot + every param per candidate,
     * plus one per discovered closure literal. */
    int max_records = gen->closure_count + 1;
    for (int i = 0; i < cfn_count; i++) max_records += 1 + cfns[i]->child_count;
    int* lit_ci      = (int*)malloc(sizeof(int) * max_records);   /* gen->closures index, or -1 */
    int* cap_arr_id  = (int*)malloc(sizeof(int) * max_records);   /* _aether_lib_caps_N id, or -1 */
    const char** rec_name = (const char**)malloc(sizeof(char*) * max_records);
    const char** rec_role = (const char**)malloc(sizeof(char*) * max_records);
    const char** rec_encl = (const char**)malloc(sizeof(char*) * max_records);
    char**       rec_sig  = (char**)malloc(sizeof(char*) * max_records);
    const char** rec_src  = (const char**)malloc(sizeof(char*) * max_records);
    int*         rec_line = (int*)malloc(sizeof(int) * max_records);

    int cap_arr_next = 0;
    /* 1 + 2: per candidate, builder flag then closure-typed params. */
    for (int i = 0; i < cfn_count; i++) {
        ASTNode* fn = cfns[i];
        if (fn->type == AST_BUILDER_FUNCTION ||
            is_builder_func_reg(gen, fn->value) || fn_takes_builder_context(fn)) {
            lit_ci[clo_count] = -1; cap_arr_id[clo_count] = -1;
            rec_name[clo_count] = fn->value;
            rec_role[clo_count] = "builder";
            rec_encl[clo_count] = fn->value;
            rec_sig[clo_count]  = NULL;   /* render the fn's own signature inline below */
            rec_src[clo_count]  = fn->source_file ? fn->source_file : "";
            rec_line[clo_count] = fn->line;
            clo_count++;
        }
        for (int p = 0; p < fn->child_count; p++) {
            ASTNode* c = fn->children[p];
            if (!c) continue;
            if (c->type == AST_GUARD_CLAUSE) continue;
            if (c->type == AST_BLOCK) break;
            if ((c->type == AST_VARIABLE_DECLARATION || c->type == AST_PATTERN_VARIABLE) &&
                c->node_type && c->node_type->kind == TYPE_FUNCTION) {
                lit_ci[clo_count] = -1; cap_arr_id[clo_count] = -1;
                rec_name[clo_count] = c->value ? c->value : "";
                rec_role[clo_count] = "param";
                rec_encl[clo_count] = fn->value;
                rec_sig[clo_count]  = strdup(type_to_string(c->node_type));
                rec_src[clo_count]  = fn->source_file ? fn->source_file : "";
                rec_line[clo_count] = fn->line;
                clo_count++;
            }
        }
    }
    /* 3: hoisted closure literals whose parent function is an export. */
    for (int ci = 0; ci < gen->closure_count; ci++) {
        const char* pf = gen->closures[ci].parent_func;
        if (!pf) continue;
        ASTNode* owner = NULL;
        for (int i = 0; i < cfn_count; i++) {
            if (cfns[i]->value && strcmp(cfns[i]->value, pf) == 0) { owner = cfns[i]; break; }
        }
        if (!owner) continue;   /* parent is main / synthetic / non-exported */
        ASTNode* cnode = gen->closures[ci].closure_node;
        int cap_n = gen->closures[ci].capture_count;
        int this_cap_arr = -1;
        if (cap_n > 0) {
            this_cap_arr = cap_arr_next++;
            fprintf(gen->output,
                "static const struct _AetherLibCap _aether_lib_caps_%d[] = {\n",
                this_cap_arr);
            for (int k = 0; k < cap_n; k++) {
                const char* cap_name = gen->closures[ci].captures[k];
                Type* ct = find_var_type_in_function(owner, cap_name);
                fprintf(gen->output, "    { ");
                emit_lib_metadata_c_string_literal(gen->output, cap_name ? cap_name : "");
                fprintf(gen->output, ", ");
                emit_lib_metadata_c_string_literal(gen->output, ct ? type_to_string(ct) : "ptr");
                fprintf(gen->output, " },\n");
            }
            fprintf(gen->output, "};\n");
        }
        lit_ci[clo_count]    = ci;
        cap_arr_id[clo_count] = this_cap_arr;
        rec_name[clo_count]  = "";   /* hoisted closures are anonymous at source level */
        rec_role[clo_count]  = "literal";
        rec_encl[clo_count]  = owner->value;
        rec_sig[clo_count]   = cnode ? render_closure_sig_ast(cnode) : NULL;
        rec_src[clo_count]   = (cnode && cnode->source_file) ? cnode->source_file
                                 : (owner->source_file ? owner->source_file : "");
        rec_line[clo_count]  = (cnode && cnode->line) ? cnode->line : owner->line;
        clo_count++;
    }

    if (clo_count > 0) {
        fprintf(gen->output,
            "\nstatic const struct _AetherLibClosure _aether_lib_closures[] = {\n");
        for (int r = 0; r < clo_count; r++) {
            fprintf(gen->output, "    { ");
            emit_lib_metadata_c_string_literal(gen->output, rec_name[r]);
            fprintf(gen->output, ", ");
            emit_lib_metadata_c_string_literal(gen->output, rec_role[r]);
            fprintf(gen->output, ", ");
            emit_lib_metadata_c_string_literal(gen->output, rec_encl[r]);
            fprintf(gen->output, ", ");
            if (rec_sig[r]) {
                emit_lib_metadata_c_string_literal(gen->output, rec_sig[r]);
            } else if (lit_ci[r] == -1 && strcmp(rec_role[r], "builder") == 0) {
                /* Builder: render the export's own signature now (the
                 * static type_to_string buffer is safe at point of use). */
                ASTNode* bf = NULL;
                for (int i = 0; i < cfn_count; i++) {
                    if (cfns[i]->value && strcmp(cfns[i]->value, rec_encl[r]) == 0) { bf = cfns[i]; break; }
                }
                fputc('"', gen->output);
                if (bf) emit_lib_metadata_signature_for(gen->output, bf);
                fputc('"', gen->output);
            } else {
                fputs("\"\"", gen->output);
            }
            int cap_n = (lit_ci[r] >= 0) ? gen->closures[lit_ci[r]].capture_count : 0;
            fprintf(gen->output, ", %d, ", cap_n);
            if (cap_arr_id[r] >= 0) {
                fprintf(gen->output, "_aether_lib_caps_%d", cap_arr_id[r]);
            } else {
                fputs("NULL", gen->output);
            }
            fprintf(gen->output, ", ");
            emit_lib_metadata_c_string_literal(gen->output, rec_src[r]);
            fprintf(gen->output, ", %d },\n", rec_line[r]);
        }
        fprintf(gen->output, "};\n\n");
    }

    /* --- v3 constant records (schema 1.2) --- */
    if (const_count > 0) {
        fprintf(gen->output,
            "\nstatic const struct _AetherLibConst _aether_lib_consts[] = {\n");
        for (int i = 0; i < const_count; i++) {
            ASTNode* cd = consts[i];
            fprintf(gen->output, "    { ");
            emit_lib_metadata_c_string_literal(gen->output, cd->value);
            fprintf(gen->output, ", ");
            emit_lib_metadata_c_string_literal(gen->output, const_type[i]);
            fprintf(gen->output, ", ");
            if (!emit_lib_const_value_literal(gen->output, cd->children[0], cd->node_type)) {
                /* Shouldn't happen — we gated on a renderable type above —
                 * but never emit a malformed record: fall back to "". */
                fputs("\"\"", gen->output);
            }
            fprintf(gen->output, " },\n");
        }
        fprintf(gen->output, "};\n\n");
    }

    /* Pull the primary source path off the first non-imported fn —
     * approximation of "the .ae the user passed to aetherc". The
     * artifact may bundle multiple sources (concat-ae); naming the
     * first one is the simplest stable convention. */
    const char* primary_src = "";
    for (int i = 0; i < fn_count; i++) {
        if (fns[i]->source_file) { primary_src = fns[i]->source_file; break; }
    }
    /* const-only modules (no exported functions) still want a source path. */
    if (!primary_src[0]) {
        for (int i = 0; i < const_count; i++) {
            if (consts[i]->source_file) { primary_src = consts[i]->source_file; break; }
        }
    }
    fprintf(gen->output,
        "static const struct _AetherLibMeta _aether_lib_meta = {\n");
    /* Schema is the highest feature level present: "1.2" once any constant
     * record exists, else "1.1" with closures, else "1.0" — which keeps a
     * function-only artifact byte-identical to v1 for existing readers. The
     * trailing slots are always written (the struct always has them); older
     * readers stop at the count/pointer they know. */
    const char* schema = (const_count > 0) ? "1.2"
                       : (clo_count > 0)   ? "1.1"
                       : "1.0";
    fprintf(gen->output, "    \"%s\", \"", schema);
    fprintf(gen->output, "%s", "0.0.0-dev");  /* version string filled in by build glue if available */
    fprintf(gen->output, "\", ");
    emit_lib_metadata_c_string_literal(gen->output, primary_src);
    fprintf(gen->output, ", %d, _aether_lib_fns, ", fn_count);
    if (clo_count > 0) fprintf(gen->output, "%d, _aether_lib_closures, ", clo_count);
    else               fprintf(gen->output, "0, NULL, ");
    if (const_count > 0) fprintf(gen->output, "%d, _aether_lib_consts\n};\n\n", const_count);
    else                 fprintf(gen->output, "0, NULL\n};\n\n");

    /* #996 --emit=csrc: serialize the identical catalog as JSON alongside the
     * C struct. Driven by the same fns[]/closure/const tables emitted above, so
     * the JSON can never drift from aether_lib_meta(). Deterministic and
     * human-diffable (2-space indent, source order) so the source artifact is
     * content-addressable and any language's binding generator can consume it. */
    if (gen->csrc_catalog_file) {
        FILE* j = gen->csrc_catalog_file;
        fputs("{\n", j);
        fputs("  \"schema_version\": ", j); emit_json_string(j, schema);      fputs(",\n", j);
        fputs("  \"aether_version\": ", j); emit_json_string(j, "0.0.0-dev"); fputs(",\n", j);
        fputs("  \"primary_source\": ", j); emit_json_string(j, primary_src); fputs(",\n", j);

        /* capabilities: the --with grants this artifact was built with. The
         * emitted C only contains code paths for granted capabilities, so this
         * is the syscall surface a consumer can inspect before compiling it. */
        fputs("  \"capabilities\": [", j);
        const char* caps = gen->csrc_capabilities;
        int cap_first = 1;
        if (caps && caps[0]) {
            const char* start = caps;
            for (const char* p = caps; ; p++) {
                if (*p == ',' || *p == '\0') {
                    if (p > start) {
                        char tmp[64];
                        int n = (int)(p - start);
                        if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
                        memcpy(tmp, start, (size_t)n); tmp[n] = '\0';
                        if (!cap_first) fputs(", ", j);
                        cap_first = 0;
                        emit_json_string(j, tmp);
                    }
                    if (*p == '\0') break;
                    start = p + 1;
                }
            }
        }
        fputs("],\n", j);

        /* functions */
        fputs("  \"functions\": [", j);
        for (int i = 0; i < fn_count; i++) {
            ASTNode* fn = fns[i];
            const char* cb_sym = c_callback_symbol(fn);
            char c_sym[256];
            if (cb_sym) snprintf(c_sym, sizeof(c_sym), "%s", cb_sym);
            else        snprintf(c_sym, sizeof(c_sym), "aether_%s", fn->value);
            char* sig = fn_signature_string(fn);
            fputs(i == 0 ? "\n" : ",\n", j);
            fputs("    { \"aether_name\": ", j);  emit_json_string(j, fn->value);
            fputs(", \"c_symbol\": ", j);         emit_json_string(j, c_sym);
            fputs(", \"signature\": ", j);        emit_json_string(j, sig);
            fputs(", \"source_file\": ", j);      emit_json_string(j, fn->source_file ? fn->source_file : "");
            fprintf(j, ", \"source_line\": %d }", fn->line);
            free(sig);
        }
        fputs(fn_count ? "\n  ],\n" : "],\n", j);

        /* closures (schema 1.1) */
        fputs("  \"closures\": [", j);
        for (int r = 0; r < clo_count; r++) {
            char* built_sig = NULL;
            const char* sig = rec_sig[r];
            if (!sig && strcmp(rec_role[r], "builder") == 0) {
                for (int i = 0; i < cfn_count; i++) {
                    if (cfns[i]->value && strcmp(cfns[i]->value, rec_encl[r]) == 0) {
                        built_sig = fn_signature_string(cfns[i]); sig = built_sig; break;
                    }
                }
            }
            fputs(r == 0 ? "\n" : ",\n", j);
            fputs("    { \"name\": ", j);             emit_json_string(j, rec_name[r]);
            fputs(", \"role\": ", j);                 emit_json_string(j, rec_role[r]);
            fputs(", \"enclosing_export\": ", j);     emit_json_string(j, rec_encl[r]);
            fputs(", \"signature\": ", j);            emit_json_string(j, sig ? sig : "");
            fputs(", \"captures\": [", j);
            if (lit_ci[r] >= 0 && gen->closures[lit_ci[r]].capture_count > 0) {
                ASTNode* owner = NULL;
                for (int i = 0; i < cfn_count; i++) {
                    if (cfns[i]->value && strcmp(cfns[i]->value, rec_encl[r]) == 0) { owner = cfns[i]; break; }
                }
                int cap_n = gen->closures[lit_ci[r]].capture_count;
                for (int k = 0; k < cap_n; k++) {
                    const char* cap_name = gen->closures[lit_ci[r]].captures[k];
                    Type* ct = owner ? find_var_type_in_function(owner, cap_name) : NULL;
                    if (k) fputs(", ", j);
                    fputs("{ \"name\": ", j);  emit_json_string(j, cap_name ? cap_name : "");
                    fputs(", \"type\": ", j);  emit_json_string(j, ct ? type_to_string(ct) : "ptr");
                    fputs(" }", j);
                }
            }
            fputs("]", j);
            fputs(", \"source_file\": ", j);  emit_json_string(j, rec_src[r] ? rec_src[r] : "");
            fprintf(j, ", \"source_line\": %d }", rec_line[r]);
            free(built_sig);
        }
        fputs(clo_count ? "\n  ],\n" : "],\n", j);

        /* constants (schema 1.2) */
        fputs("  \"constants\": [", j);
        for (int i = 0; i < const_count; i++) {
            ASTNode* cd = consts[i];
            ASTNode* lit = cd->child_count > 0 ? cd->children[0] : NULL;
            const char* val = (lit && lit->type == AST_LITERAL && lit->value) ? lit->value : "";
            fputs(i == 0 ? "\n" : ",\n", j);
            fputs("    { \"name\": ", j);   emit_json_string(j, cd->value);
            fputs(", \"type\": ", j);       emit_json_string(j, const_type[i]);
            fputs(", \"value\": ", j);      emit_json_string(j, val);
            fputs(" }", j);
        }
        fputs(const_count ? "\n  ]\n" : "]\n", j);
        fputs("}\n", j);
    }

    for (int r = 0; r < clo_count; r++) free(rec_sig[r]);
    free(cfns);
    free(lit_ci); free(cap_arr_id); free(rec_name); free(rec_role);
    free(rec_encl); free(rec_sig); free(rec_src); free(rec_line);
    free(consts); free(const_type);
    if (export_names) free(export_names);

    /* The exported entry point. Returns a pointer to the static
     * meta struct. The local `_AetherLibMeta` tag is layout-
     * compatible with the host's `AetherLibMeta` declared in
     * runtime/aether_lib_meta.h — direct callers including the
     * header see the canonical name; dlsym callers cast the
     * function-pointer to whatever return type they want. Either
     * works because the C linker resolves on symbol name alone,
     * not signature.
     *
     * Weak linkage (#1590): an orchestrator-driven multi-TU build
     * (aeb's shape — one --emit=lib C per module, one final link)
     * otherwise gets one external aether_lib_meta per TU and dies
     * with duplicate-symbol errors, forcing back exactly the
     * -Wl,--allow-multiple-definition escape hatch the 0.539
     * static-clone link model retired (and which ld64 rejects).
     * Weak keeps the lone-.so dlsym contract byte-identical (a
     * single definition still wins) while an N-TU link resolves to
     * the first TU's catalog and proceeds. GCC/Clang/MinGW all
     * honour the attribute across ELF/Mach-O/COFF; the non-GNU
     * fallback keeps the old strong emission (single-TU only). */
    fprintf(gen->output,
        "/* Weak: N --emit=lib TUs may be linked into one artifact (#1590). */\n"
        "#ifndef AETHER_LIB_META_WEAK\n"
        "#  if defined(__GNUC__) || defined(__clang__)\n"
        "#    define AETHER_LIB_META_WEAK __attribute__((weak))\n"
        "#  else\n"
        "#    define AETHER_LIB_META_WEAK\n"
        "#  endif\n"
        "#endif\n"
        "AETHER_LIB_META_WEAK const struct _AetherLibMeta* aether_lib_meta(void) {\n"
        "    return &_aether_lib_meta;\n"
        "}\n\n");

    free(fns);
}

// --emit-main=<func> shim. Issue #268.3.
//
/* #1333: emit the message-id to name table used by message tracing.
 *
 * The runtime records msg.type, an integer the message registry assigned. Only
 * the compiler knows the names behind those ids, so it writes them out as a
 * static table and registers it at startup. Indexed by id with NULL for the
 * gaps (the reserved IO_READY id is not dense with the user ones), so a lookup
 * is a bounds check and a load.
 *
 * Everything here sits inside `#ifdef AETHER_TRACE`: without the flag the
 * generated C contains no table, no call, and no reference to the tracing
 * runtime at all. */
static void emit_trace_message_name_table(CodeGenerator* gen) {
    if (!gen || !gen->message_registry) return;

    int max_id = -1;
    for (MessageDef* m = gen->message_registry->messages; m; m = m->next) {
        if (m->message_id > max_id) max_id = m->message_id;
    }
    if (max_id < 0) return;

    /* A pathological reserved id would make the table enormous for no gain;
     * the trace falls back to printing the raw id in that case. */
    if (max_id > 4096) return;

    print_line(gen, "#ifdef AETHER_TRACE");
    /* Declared here rather than via an include: the generated C is compiled
     * with a variety of include sets and this keeps the table self-contained. */
    print_line(gen, "extern void aether_trace_register_message_names(const char* const*, int, int);");
    fprintf(gen->output, "static const char* const _aether_trace_msg_names[%d] = {\n", max_id + 1);
    for (int id = 0; id <= max_id; id++) {
        const char* name = NULL;
        for (MessageDef* m = gen->message_registry->messages; m; m = m->next) {
            if (m->message_id == id) { name = m->name; break; }
        }
        if (name) fprintf(gen->output, "    [%d] = \"%s\",\n", id, name);
    }
    fprintf(gen->output, "};\n");
    print_line(gen, "aether_trace_register_message_names(_aether_trace_msg_names, %d, 0);", max_id + 1);
    print_line(gen, "#endif");
    print_line(gen, "");
}

// Emits a thin `int main(int argc, char** argv)` that calls the named
// Aether function. Lets one source compile to both a loadable library
// (`aether_*` exports) AND a regular binary (with this main as entry),
// closing the symmetry between --emit=exe and --emit=lib.
//
// Validation:
//   - The target must be a top-level Aether function defined in this TU
//     (not an extern, not an actor/struct/message). Compile error if
//     missing or wrong shape.
//   - Zero parameters required (the shim is a 'no-arg entry'; passing
//     argv into the Aether function is a separate, larger feature).
//   - Return type may be int / int32 / int64 (passed through as exit
//     code) or void (always returns 0). Other return types are an
//     error — the C `main` signature is fixed.
static void emit_main_shim_for_target(CodeGenerator* gen, ASTNode* program) {
    if (!gen || !gen->emit_main_target || !program) return;
    if (!gen->emit_lib) {
        // --emit-main is only meaningful with --emit=lib; if the user
        // is on --emit=exe, the program already has its own main()
        // (or would, via AST_MAIN_FUNCTION). Quietly ignore rather
        // than fail — the flag combination is the user's choice.
        return;
    }

    const char* target = gen->emit_main_target;
    ASTNode* fn = NULL;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        ASTNode* candidate = c;
        if (c && c->type == AST_EXPORT_STATEMENT && c->child_count > 0) {
            candidate = c->children[0];
        }
        if (!candidate || !candidate->value) continue;
        if (candidate->type != AST_FUNCTION_DEFINITION) continue;
        if (strcmp(candidate->value, target) != 0) continue;
        fn = candidate;
        break;
    }
    if (!fn) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "--emit-main=%s: no top-level function named '%s' in this translation unit",
                 target, target);
        AetherError e = {NULL, NULL, 0, 0, msg,
                         "name a function defined at the top level of this file (or one of its directly-merged modules)",
                         NULL, AETHER_ERR_NONE};
        aether_error_report(&e);
        return;
    }

    // Reject non-zero-arg targets — the shim signature is fixed.
    int param_count = 0;
    for (int p = 0; p < fn->child_count; p++) {
        ASTNode* c = fn->children[p];
        if (c->type == AST_GUARD_CLAUSE || c->type == AST_BLOCK) continue;
        if (c->type == AST_VARIABLE_DECLARATION || c->type == AST_PATTERN_VARIABLE ||
            c->type == AST_PATTERN_LITERAL    || c->type == AST_PATTERN_LIST     ||
            c->type == AST_PATTERN_CONS       || c->type == AST_PATTERN_STRUCT) {
            param_count++;
        }
    }
    if (param_count != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "--emit-main=%s: target function takes %d parameter(s); the shim entry must be zero-arg",
                 target, param_count);
        AetherError e = {NULL, NULL, fn->line, fn->column, msg,
                         "rewrite the target as a zero-arg wrapper, or call it from a zero-arg helper that the shim points at",
                         NULL, AETHER_ERR_NONE};
        aether_error_report(&e);
        return;
    }

    // Decide whether to forward the target's return value as the exit code.
    int returns_int = 0;
    int returns_void = 0;
    if (fn->node_type) {
        switch (fn->node_type->kind) {
            case TYPE_INT: case TYPE_INT64:
                returns_int = 1; break;
            case TYPE_VOID: case TYPE_UNKNOWN:
                returns_void = 1; break;
            default:
                break;
        }
    } else {
        returns_void = 1;
    }
    if (!returns_int && !returns_void && !has_return_value(fn)) {
        returns_void = 1;
    }
    if (!returns_int && !returns_void) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "--emit-main=%s: target function's return type isn't int / int32 / int64 / void; can't be forwarded as a process exit code",
                 target);
        AetherError e = {NULL, NULL, fn->line, fn->column, msg,
                         "make the target return int (the exit code) or void",
                         NULL, AETHER_ERR_NONE};
        aether_error_report(&e);
        return;
    }

    fprintf(gen->output, "\n/* --- main(argc,argv) shim (--emit-main=%s) --- */\n", target);
    fprintf(gen->output, "int main(int argc, char** argv) {\n");
    fprintf(gen->output, "    (void)argc; (void)argv;\n");
    if (returns_int) {
        fprintf(gen->output, "    return (int)%s();\n", target);
    } else {
        fprintf(gen->output, "    %s();\n", target);
        fprintf(gen->output, "    return 0;\n");
    }
    fprintf(gen->output, "}\n");
}

// Emit a typedef for a tuple type if not already emitted
void ensure_tuple_typedef(CodeGenerator* gen, Type* type) {
    if (!type || type->kind != TYPE_TUPLE) return;
    const char* name = get_c_type(type);

    // Check if already emitted
    for (int i = 0; i < gen->tuple_type_count; i++) {
        if (strcmp(gen->tuple_type_names[i], name) == 0) return;
    }

    // Emit typedef
    fprintf(gen->output, "typedef struct { ");
    for (int i = 0; i < type->tuple_count; i++) {
        fprintf(gen->output, "%s _%d; ", get_c_type(type->tuple_types[i]), i);
    }
    fprintf(gen->output, "} %s;\n", name);

    // Register
    if (gen->tuple_type_count >= gen->tuple_type_capacity) {
        gen->tuple_type_capacity = gen->tuple_type_capacity ? gen->tuple_type_capacity * 2 : 8;
        gen->tuple_type_names = aether_xrealloc(gen->tuple_type_names, gen->tuple_type_capacity * sizeof(char*));
    }
    gen->tuple_type_names[gen->tuple_type_count++] = strdup(name);
}

// #340: emit `typedef struct { bool has; <T> val; } ae_opt_<T>;` once per
// concrete optional type. Skips optional-of-unknown (an unpinned `none` that
// never reached a concrete context — it produces no storage).
void ensure_optional_typedef(CodeGenerator* gen, Type* type) {
    if (!type || type->kind != TYPE_OPTIONAL) return;
    if (!type->element_type || type->element_type->kind == TYPE_UNKNOWN) return;
    const char* name = get_c_type(type);
    for (int i = 0; i < gen->opt_type_count; i++) {
        if (strcmp(gen->opt_type_names[i], name) == 0) return;
    }
    // get_c_type uses rotating static buffers, so snapshot the inner C type
    // name before `name` (also a rotating buffer) could be reused.
    char inner_c[256];
    snprintf(inner_c, sizeof(inner_c), "%s", get_c_type(type->element_type));
    char opt_name[256];
    snprintf(opt_name, sizeof(opt_name), "%s", name);
    fprintf(gen->output, "typedef struct { int has; %s val; } %s;\n", inner_c, opt_name);
    if (gen->opt_type_count >= gen->opt_type_capacity) {
        gen->opt_type_capacity = gen->opt_type_capacity ? gen->opt_type_capacity * 2 : 8;
        gen->opt_type_names = aether_xrealloc(gen->opt_type_names, gen->opt_type_capacity * sizeof(char*));
    }
    gen->opt_type_names[gen->opt_type_count++] = strdup(opt_name);
}

// #340: walk the whole program AST, emitting an optional typedef for every
// concrete `T?` that appears as a node's type (var/param/field/return/expr).
// Optional typedefs must precede the function bodies that use them, so this
// runs in the type-emission pre-pass.
void collect_optional_typedefs(CodeGenerator* gen, ASTNode* node) {
    if (!node) return;
    if (node->node_type && node->node_type->kind == TYPE_OPTIONAL) {
        ensure_optional_typedef(gen, node->node_type);
    }
    for (int i = 0; i < node->child_count; i++) {
        collect_optional_typedefs(gen, node->children[i]);
    }
}

// #914: emit a sum/variant type's C lowering — a tag enum plus a tagged union
// embedding each variant struct by value:
//   typedef enum { Shape__Circle, Shape__Rect } Shape_tag;
//   typedef struct Shape { Shape_tag tag; union { Circle Circle_; Rect Rect_; } data; } Shape;
// Emitted after all struct bodies (the union members embed the variants by
// value) and before function forward declarations.
void emit_sum_typedef(CodeGenerator* gen, ASTNode* def) {
    if (!def || def->type != AST_SUM_TYPE_DEF || !def->value) return;
    const char* name = def->value;
    fprintf(gen->output, "typedef enum { ");
    int n = 0;
    for (int i = 0; i < def->child_count; i++) {
        ASTNode* v = def->children[i];
        if (!v || v->type != AST_IDENTIFIER || !v->value) continue;
        if (n > 0) fprintf(gen->output, ", ");
        fprintf(gen->output, "%s__%s", name, v->value);
        n++;
    }
    fprintf(gen->output, " } %s_tag;\n", name);
    fprintf(gen->output, "typedef struct %s {\n    %s_tag tag;\n    union {\n", name, name);
    for (int i = 0; i < def->child_count; i++) {
        ASTNode* v = def->children[i];
        if (!v || v->type != AST_IDENTIFIER || !v->value) continue;
        fprintf(gen->output, "        %s %s_;\n", v->value, v->value);
    }
    fprintf(gen->output, "    } data;\n} %s;\n", name);
}

// #1044: emit an enum's C lowering, `typedef enum { Name_Member [= v], ... }
// Name;`. A member with an explicit value emits `= <value>`; the rest rely on
// C's auto-increment (previous + 1, first defaults to 0), which matches the
// Aether semantics exactly. Zero runtime cost; the members are the C constants
// that `Enum.Member` resolves to.
void emit_enum_typedef(CodeGenerator* gen, ASTNode* def) {
    if (!def || def->type != AST_ENUM_DEFINITION || !def->value) return;
    const char* name = def->value;
    fprintf(gen->output, "typedef enum {");
    int n = 0;
    for (int i = 0; i < def->child_count; i++) {
        ASTNode* m = def->children[i];
        if (!m || m->type != AST_ENUM_MEMBER || !m->value) continue;
        fprintf(gen->output, "%s %s_%s", n > 0 ? "," : "", name, m->value);
        if (m->child_count > 0 && m->children[0]) {
            fprintf(gen->output, " = ");
            generate_expression(gen, m->children[0]);
        }
        n++;
    }
    fprintf(gen->output, " } %s;\n", name);
}

const char* get_c_operator(const char* aether_op) {
    if (!aether_op) return "";
    
    if (strcmp(aether_op, "&&") == 0) return "&&";
    if (strcmp(aether_op, "||") == 0) return "||";
    if (strcmp(aether_op, "==") == 0) return "==";
    if (strcmp(aether_op, "!=") == 0) return "!=";
    if (strcmp(aether_op, "<") == 0) return "<";
    if (strcmp(aether_op, "<=") == 0) return "<=";
    if (strcmp(aether_op, ">") == 0) return ">";
    if (strcmp(aether_op, ">=") == 0) return ">=";
    if (strcmp(aether_op, "+") == 0) return "+";
    if (strcmp(aether_op, "-") == 0) return "-";
    if (strcmp(aether_op, "*") == 0) return "*";
    if (strcmp(aether_op, "/") == 0) return "/";
    if (strcmp(aether_op, "%") == 0) return "%";
    if (strcmp(aether_op, "!") == 0) return "!";
    if (strcmp(aether_op, "=") == 0) return "=";
    if (strcmp(aether_op, "++") == 0) return "++";
    if (strcmp(aether_op, "--") == 0) return "--";
    if (strcmp(aether_op, "&") == 0) return "&";
    if (strcmp(aether_op, "|") == 0) return "|";
    if (strcmp(aether_op, "^") == 0) return "^";
    if (strcmp(aether_op, "~") == 0) return "~";
    if (strcmp(aether_op, "<<") == 0) return "<<";
    if (strcmp(aether_op, ">>") == 0) return ">>";

    return aether_op;
}

void generate_type(CodeGenerator* gen, Type* type) {
    fprintf(gen->output, "%s", get_c_type(type));
}

/* #750: emit a C function-pointer declarator for a `fn(...)->R`
 * (TYPE_FUNCTION, is_fnptr) parameter. The pointed-to name must sit
 * INSIDE the `(*...)`, so this can't use the plain `<type> <name>`
 * shape get_c_type provides (is_fnptr collapses to "void*" there).
 *   name != NULL  ->  `R (*name)(T1, T2)`   (a definition parameter)
 *   name == NULL  ->  `R (*)(T1, T2)`       (a prototype's abstract declarator)
 * The element type strings mirror the call-site cast in codegen_expr.c
 * so a fn-ptr param, its prototype, and a call through it all agree. */
void emit_fnptr_decl(CodeGenerator* gen, Type* sig, const char* name) {
    const char* ret_c = (sig && sig->return_type)
                        ? get_c_type(sig->return_type) : "void";
    fprintf(gen->output, "%s (*%s)(", ret_c, name ? name : "");
    if (sig && sig->param_count > 0) {
        for (int i = 0; i < sig->param_count; i++) {
            if (i > 0) fprintf(gen->output, ", ");
            fprintf(gen->output, "%s", get_c_type(sig->param_types[i]));
        }
    } else {
        fprintf(gen->output, "void");
    }
    fprintf(gen->output, ")");
}

/* True for a parameter/local whose declared type is a typed C function
 * pointer (`fn(...)->R`), the shape #750 emits specially. */
int is_fnptr_type(Type* t) {
    return t && t->kind == TYPE_FUNCTION && t->is_fnptr;
}

// Generate a default return value for pattern match failures
// This outputs a sentinel value that indicates "no match" for this clause
void generate_default_return_value(CodeGenerator* gen, Type* type) {
    if (!type) {
        fprintf(gen->output, "0");
        return;
    }
    switch (type->kind) {
        case TYPE_INT:
            fprintf(gen->output, "0");
            break;
        case TYPE_FLOAT:
            fprintf(gen->output, "0.0");
            break;
        case TYPE_LONGDOUBLE:
            fprintf(gen->output, "0.0L");
            break;
        case TYPE_STRING:
            fprintf(gen->output, "\"\"");
            break;
        case TYPE_BOOL:
            fprintf(gen->output, "0");
            break;
        case TYPE_VOID:
            // For void functions, just return without value
            // The caller should handle this - output nothing
            break;
        case TYPE_PTR:
            fprintf(gen->output, "NULL");
            break;
        default:
            fprintf(gen->output, "0");
            break;
    }
}

// Check if an AST subtree contains any return statements
static int has_return_statement(ASTNode* node) {
    if (!node) return 0;
    if (node->type == AST_RETURN_STATEMENT) return 1;
    for (int i = 0; i < node->child_count; i++) {
        if (has_return_statement(node->children[i])) return 1;
    }
    return 0;
}

void generate_main_function(CodeGenerator* gen, ASTNode* main) {
    if (!main || main->type != AST_MAIN_FUNCTION) return;

    // --emit=lib only: suppress the C `int main(int,char**)` entry point.
    // If the .ae file defined main(), its body is currently dropped in
    // lib-only mode — library consumers call the exported top-level
    // functions directly, not main(). A future Shape B extension could
    // emit an `aether_main()` wrapper so hosts can invoke the script's
    // entry point explicitly.
    if (!gen->emit_exe) return;

    /* Track `main` as the current function so body-structural queries
     * (e.g. body_assigns_var_from_heap, used by the map/list owned-
     * value routing) can reach `main`'s body. Restored at function end.
     * All other `gen->current_function` consumers gate on
     * `!gen->in_main_function` first, so a main node here is inert
     * for them. */
    ASTNode* prev_current_function = gen->current_function;
    gen->current_function = main;

    print_line(gen, "int main(int argc, char** argv) {");
    indent(gen);
    clear_declared_vars(gen);  // Reset for main function
    clear_heap_string_vars(gen);
    clear_seq_vars(gen);
    clear_opt_str_vars(gen);
    clear_escaped_string_vars(gen);
    clear_try_clobbered_vars(gen);  /* Issue #501 follow-up */
    // Reset defer state for main function and enter scope
    gen->defer_count = 0;
    gen->scope_depth = 0;
    enter_scope(gen);

    // Set UTF-8 console codepage on Windows so programs can print Unicode correctly
    print_line(gen, "#ifdef _WIN32");
    print_line(gen, "SetConsoleOutputCP(65001);  // CP_UTF8");
    print_line(gen, "SetConsoleCP(65001);");
    // Force stdout/stderr to binary mode on Windows so printf does not
    // translate every "\n" into "\r\n" on the way to a redirected file.
    // Aether programs already emit explicit "\n" terminators; the CRT
    // translation makes byte-exact output comparisons (and any binary
    // data piped through stdout) unreliable.
    print_line(gen, "_setmode(_fileno(stdout), _O_BINARY);");
    print_line(gen, "_setmode(_fileno(stderr), _O_BINARY);");
    print_line(gen, "#endif");
    // Initialize command-line arguments
    print_line(gen, "aether_args_init(argc, argv);");
    // Opt-in OS-enforced sandbox: if AETHER_CAPSICUM=1 (FreeBSD), enter
    // capability mode here — after rtld has loaded every shared library,
    // before any user code or file/socket access. No-op otherwise.
    print_line(gen, "aether_capsicum_autosandbox();");
    // main_exit_ret and main_exit: label are needed when actors exist
    // (scheduler cleanup) or when main() contains return statements.
    int needs_main_exit = gen->actor_count > 0 || has_return_statement(main);
    gen->uses_main_exit = needs_main_exit;
    if (needs_main_exit) {
        print_line(gen, "int main_exit_ret = 0;");
    }
    print_line(gen, "");

    // Initialize scheduler with recommended core count if actors were defined
    if (gen->actor_count > 0) {
        print_line(gen, "// Initialize Aether runtime with auto-detected optimizations");
        print_line(gen, "// TIER 1 (always-on): Actor pooling, Direct send, Adaptive batching");
        print_line(gen, "// TIER 2 (auto-detect): SIMD (if AVX2/NEON), MWAIT (if supported)");
        print_line(gen, "int num_cores = cpu_recommend_cores();");
        print_line(gen, "scheduler_init(num_cores);  // Auto-detects hardware capabilities");
        /* #1333: hand the runtime the message-id to name mapping. The runtime
         * only ever sees the integer id, so without this a trace reads as bare
         * ordinals. Emitted inside the AETHER_TRACE guard so a normal build
         * carries neither the table nor the call. */
        emit_trace_message_name_table(gen);
        print_line(gen, "");
        print_line(gen, "#ifdef AETHER_VERBOSE");
        print_line(gen, "aether_print_config();");
        print_line(gen, "#endif");
        print_line(gen, "");
        print_line(gen, "scheduler_start();");
        print_line(gen, "current_core_id = -1;  // Main thread is not a scheduler thread");
        print_line(gen, "");
    }
    
    if (main->child_count > 0) {
        gen->in_main_function = 1;
        // Publish main's promoted-captures set so var decls malloc
        // heap cells and reads/writes dereference.
        char** prev_promoted = gen->current_promoted_captures;
        int prev_promoted_count = gen->current_promoted_capture_count;
        get_promoted_names_for_func(gen, "main",
            &gen->current_promoted_captures, &gen->current_promoted_capture_count);
        // Issue #405: hoist `_heap_<name>` companions for every
        // string variable in main() to function-entry scope so the
        // tracker is visible across every nesting depth. Without
        // this, cross-block reassignment from a heap-string-returning
        // user function fails to compile (`'_heap_x' undeclared`)
        // because the lazy tracker init was scope-local. Mirror of
        // the call in codegen_func.c::generate_function_definition.
        if (main->children[0] && main->children[0]->type == AST_BLOCK) {
            /* Issue #501 follow-up: mark try-clobbered vars in main()
             * so the `volatile` prefix is applied at decl sites for
             * locals modified inside a try body. */
            mark_try_clobbered_vars(gen, main->children[0]);
            hoist_heap_string_trackers(gen, main->children[0]);
            hoist_seq_trackers(gen, main->children[0]);
            hoist_opt_str_trackers(gen, main->children[0]);
            mark_escaped_heap_string_vars(gen, main->children[0]);
            mark_escaped_seq_vars(gen, main->children[0]);
            mark_escaped_opt_str_vars(gen, main->children[0]);
            /* Mirror the regular-function path in
             * codegen_func.c::generate_function_definition: push a
             * function-exit defer-free for every non-escaped
             * hoisted heap-string variable so heap allocations
             * surviving to main()'s tail are reclaimed at exit
             * instead of leaking per-process. Without this the
             * tracker would correctly free on every reassignment
             * but the FINAL allocation in main() would never be
             * freed — a constant per-program leak.
             *
             * Safety: the @retain annotations on string_retain/
             * release/free (std/string/module.ae) mark vars that
             * the user explicitly hands to a refcount call as
             * escaped, so the auto-free here cannot double-free
             * a buffer the user already released manually. */
            push_heap_string_exit_free_defers(gen, main->children[0]);
            push_seq_exit_free_defers(gen, main->children[0]);
            push_opt_str_exit_free_defers(gen, main->children[0]);
        }
        generate_statement(gen, main->children[0]);
        gen->current_promoted_captures = prev_promoted;
        gen->current_promoted_capture_count = prev_promoted_count;
        gen->in_main_function = 0;
    }
    
    // Clean up scheduler (all return paths in main() jump here via goto main_exit)
    // Only emit the label if it's actually targeted by a goto (actors or return
    // in main), otherwise GCC warns about an unused label.
    if (needs_main_exit) {
        print_line(gen, "main_exit:");
    }
    if (gen->actor_count > 0) {
        print_line(gen, "");
        print_line(gen, "// Wait for quiescence, stop scheduler threads, and join them");
        print_line(gen, "scheduler_shutdown();");
    }

    // Print message pool statistics (only for actor programs)
    if (gen->actor_count > 0) {
        print_line(gen, "");
        print_line(gen, "// Message pool statistics");
        print_line(gen, "{");
        indent(gen);
        print_line(gen, "uint64_t pool_hits = 0, pool_misses = 0, too_large = 0;");
        print_line(gen, "aether_message_pool_stats(&pool_hits, &pool_misses, &too_large);");
        print_line(gen, "if (pool_hits + pool_misses + too_large > 0) {");
        indent(gen);
        print_line(gen, "printf(\"\\n=== Message Pool Statistics ===\\n\");");
        print_line(gen, "printf(\"Pool hits:      %%llu\\n\", (unsigned long long)pool_hits);");
        print_line(gen, "printf(\"Pool misses:    %%llu (exhausted)\\n\", (unsigned long long)pool_misses);");
        print_line(gen, "printf(\"Too large:      %%llu (>256 bytes)\\n\", (unsigned long long)too_large);");
        print_line(gen, "uint64_t total = pool_hits + pool_misses + too_large;");
        print_line(gen, "double hit_rate = (double)pool_hits / total * 100.0;");
        print_line(gen, "printf(\"Hit rate:       %%.1f%%%%\\n\", hit_rate);");
        unindent(gen);
        print_line(gen, "}");
        unindent(gen);
        print_line(gen, "}");
        print_line(gen, "");
    }

    // Emit main function defers before return
    exit_scope(gen);

    if (needs_main_exit) {
        print_line(gen, "return main_exit_ret;");
    } else {
        print_line(gen, "return 0;");
    }
    unindent(gen);
    print_line(gen, "}");
    gen->current_function = prev_current_function;
}

/* Recursively find the first `reply <Msg> { ... }` message-constructor name
 * in a receive-arm body subtree. A reply may be nested in if/else, while,
 * or match branches — not just at the top level (#736) — so the
 * request->reply type map pre-pass must DESCEND, not only scan direct
 * children. Without this, a branched reply leaves the request unmapped and
 * the `?` ask reads a garbage reply slot. Does not descend into nested
 * function/actor/builder/closure definitions, whose replies (if any) belong
 * to a different receive scope. */

// #976: a value identifier that is a C reserved keyword (`short`, `int`,
// `char`, `default`, …) is a valid Aether identifier but an invalid C one, so
// emitting it verbatim breaks the C compile even though `ae check` passed.
// Rather than thread the mangler through the ~100 codegen sites that emit a
// variable's name (declarations, references, params, and derived temporaries
// like `_heap_<name>` / `_seqheap_<name>` / `<name>_len`), rewrite the name
// ONCE on the AST before codegen: every site then reads the already-safe name
// and stays consistent by construction.
//
// Rewritten node types, grouped by namespace — each namespace's declaration,
// initializer, reference, and pattern all read `->value`, so renaming every
// member of the group keeps that namespace internally consistent:
//   values — AST_IDENTIFIER (reads, assignment/destructure lvalues),
//            AST_VARIABLE_DECLARATION (locals, for-loop vars, params),
//            AST_PATTERN_VARIABLE (match bindings, pattern params)
//   fields — AST_STRUCT_FIELD / AST_MESSAGE_FIELD (declarations),
//            AST_ASSIGNMENT (struct/message constructor field — the parser
//              builds every AST_ASSIGNMENT as a constructor field-init; a
//              plain reassignment is an AST_VARIABLE_DECLARATION, so this node
//              type always carries a field name, never an lvalue),
//            AST_FIELD_INIT (the other constructor field-init shape),
//            AST_PATTERN_FIELD (receive-pattern field bindings),
//            AST_MEMBER_ACCESS (field reads `x.field`)
// Function names live on AST_FUNCTION_CALL->value / the function node and go
// through safe_c_name at emission with the same `ae_` prefix, so a keyword
// used as a function stays consistent too. AST_SUM_TYPE_DEF's children are
// variant TYPE names, not values, so its subtree is skipped. Module-qualified
// and method calls are folded to AST_FUNCTION_CALL during parsing, so by
// codegen an AST_MEMBER_ACCESS is a genuine field read (its non-keyword
// property accessors — durations, optionals — never match is_c_keyword).
static void mangle_keyword_value_idents(ASTNode* node) {
    if (!node) return;
    switch (node->type) {
        case AST_IDENTIFIER:
        case AST_VARIABLE_DECLARATION:
        case AST_PATTERN_VARIABLE:
        case AST_STRUCT_FIELD:
        case AST_MESSAGE_FIELD:
        case AST_ASSIGNMENT:
        case AST_FIELD_INIT:
        case AST_PATTERN_FIELD:
        case AST_MEMBER_ACCESS:
        case AST_CONST_DECLARATION:
            if (node->value && (is_c_keyword(node->value) ||
                                is_winapi_reserved_name(node->value))) {
                char safe[280];
                snprintf(safe, sizeof(safe), "ae_%s", node->value);
                char* dup = strdup(safe);
                if (dup) {
                    free(node->value);
                    node->value = dup;
                }
            }
            break;
        default:
            break;
    }
    if (node->type == AST_SUM_TYPE_DEF) return;  // children are type names
    for (int i = 0; i < node->child_count; i++) {
        mangle_keyword_value_idents(node->children[i]);
    }
}

static int stdlib_symbol_cmp(const void* key, const void* elem) {
    return strcmp((const char*)key, *(const char* const*)elem);
}

/* #1366: is `name` a bare C symbol the standard library exposes? libaether.a
   is on the link line whether or not the module that owns the symbol was
   imported, so a name that matches one collides even with no import to see.
   The table is generated from the std module.ae files at build time, because a
   hand-kept list is exactly what let `string_replace_all` reach a release. */
static int is_stdlib_c_symbol(const char* name) {
    if (!name) return 0;
    return bsearch(name, AETHER_STDLIB_SYMBOLS, AETHER_STDLIB_SYMBOL_COUNT,
                   sizeof(AETHER_STDLIB_SYMBOLS[0]), stdlib_symbol_cmp) != NULL;
}

/* #1366: does this TU declare an extern C function called `name`? Both the
   file's own externs and every imported module's externs land as declarations
   in the one emitted .c. */
static int tu_declares_extern(ASTNode* program, const char* name) {
    if (!program || !name) return 0;
    if (is_stdlib_c_symbol(name)) return 1;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if (!child) continue;
        if (child->type == AST_EXTERN_FUNCTION && child->value &&
            strcmp(child->value, name) == 0) {
            return 1;
        }
        if (child->type == AST_IMPORT_STATEMENT && child->value) {
            AetherModule* mod_entry = module_find(child->value);
            ASTNode* mod_ast = mod_entry ? mod_entry->ast : NULL;
            if (!mod_ast) continue;
            for (int j = 0; j < mod_ast->child_count; j++) {
                ASTNode* decl = mod_ast->children[j];
                if (decl && decl->type == AST_EXTERN_FUNCTION && decl->value &&
                    strcmp(decl->value, name) == 0) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* #1598: a renamed function is referenced in two shapes, and both have to
   move together or the emitted C names a symbol that no longer exists.

     _h(...)              AST_FUNCTION_CALL   — the call
     takes_fp(_h)         AST_IDENTIFIER      — the function used as a VALUE

   Renaming only the first is what made `server_get(raw, "/health", _h_health, 0)`
   fail with "'_h_health' undeclared; did you mean 'ae_h_health'?" — the
   compiler's own suggestion naming the definition it had just moved.

   The identifier half needs scope care that the call half does not: a local
   may legally shadow a top-level function name, and it is emitted verbatim
   (`_thing = 42` stays `_thing` while the function becomes `ae_thing`). So a
   subtree that REBINDS the name is skipped entirely — renaming there would
   rewrite the variable and break a program that compiles today.

   Skipping the whole subtree rather than tracking a scope stack is the
   conservative direction: it can only leave a reference un-renamed (the
   status quo ante for that one function), never rename a binding that should
   have stayed put. A shadowed name is also the case where passing the
   function as a value is unreachable anyway — the name resolves to the
   variable. */
static int subtree_rebinds_name(ASTNode* node, const char* name) {
    if (!node || !name) return 0;
    if ((node->type == AST_VARIABLE_DECLARATION ||
         node->type == AST_PATTERN_VARIABLE) &&
        node->value && strcmp(node->value, name) == 0) {
        return 1;
    }
    for (int i = 0; i < node->child_count; i++) {
        if (subtree_rebinds_name(node->children[i], name)) return 1;
    }
    return 0;
}

static void rename_refs_in(ASTNode* node, const char* from, const char* to) {
    if (!node) return;
    if (node->type == AST_IDENTIFIER && node->value &&
        strcmp(node->value, from) == 0) {
        char* dup = strdup(to);
        if (dup) {
            free(node->value);
            node->value = dup;
        }
        return;
    }
    for (int i = 0; i < node->child_count; i++) {
        rename_refs_in(node->children[i], from, to);
    }
}

static void rename_calls_to(ASTNode* node, const char* from, const char* to) {
    if (!node) return;
    if (node->type == AST_FUNCTION_CALL && node->value &&
        strcmp(node->value, from) == 0) {
        char* dup = strdup(to);
        if (dup) {
            free(node->value);
            node->value = dup;
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        rename_calls_to(node->children[i], from, to);
    }
}

/* Rename every reference to a renamed top-level function: calls anywhere,
   plus bare value references in function bodies that do not rebind the name.
   Walks the program's top-level children so each function body is a separate
   shadowing decision. */
static void rename_all_refs_to(ASTNode* program, const char* from, const char* to) {
    if (!program) return;
    rename_calls_to(program, from, to);
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* top = program->children[i];
        if (!top) continue;
        /* The definition's own name lives on the node's value, not in an
           AST_IDENTIFIER child, so bodies are all we need to walk. */
        if (subtree_rebinds_name(top, from)) continue;
        rename_refs_in(top, from, to);
    }
}

/* #1366: a top-level function whose name matches an extern the same TU
   declares would emit two conflicting declarations of one C identifier: the
   user's definition and the stdlib prototype an import dragged in. Making the
   definition `static` removes the cross-TU collision but not this one, so
   rename the user's function to the same `ae_` spelling safe_c_name already
   uses for libc collisions. Dotted stdlib calls (`string.replace_all`) carry a
   different AST value and are untouched; a bare call resolves to the user's
   function, which is what gets renamed with it. */
/* A leading underscore at file scope is RESERVED for the C implementation
   (C11 §7.1.3), and MSVCRT/UCRT use that namespace heavily: _write, _read,
   _open, _close, _access, _aligned_malloc and dozens more are real declared
   functions there. Emitting an Aether `_write(...)` verbatim therefore lands
   on top of a CRT prototype and fails to compile:

       error: conflicting types for '_write'; have 'void(const char*, const char*)'
       note: previous declaration ... int(int, const void*, unsigned int)

   Windows-only, because glibc declares none of these — so the identical
   program builds clean on Linux and dies on MinGW, with the error pointing at
   generated C rather than at the function name. Reported from the aeb line,
   where one shared `_write(path, content)` fixture broke 10 of 118 tests.

   Renaming is the fix rather than `static` alone: a file-scope static whose
   name matches a declared CRT prototype is STILL a conflicting-types error at
   compile time. Same `ae_` spelling and same call-rewrite as the #1366 extern
   collision below, so the two read as one mechanism.

   Deliberately narrow: only a LEADING underscore, only top-level user
   functions. The trailing-underscore file-local convention (#279) is
   untouched — `write_` is exactly what a caller should reach for instead, and
   it already gets internal linkage. */
static int name_is_c_reserved(const char* name) {
    return name && name[0] == '_';
}

static void rename_leading_underscore_functions(ASTNode* program) {
    if (!program) return;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* fn = program->children[i];
        if (!fn || (fn->type != AST_FUNCTION_DEFINITION &&
                    fn->type != AST_BUILDER_FUNCTION)) continue;
        /* @c_callback names must stay externally addressable verbatim, and an
           imported function was already renamed by its own module pass. */
        if (fn->is_imported || is_c_callback(fn) || !fn->value) continue;
        if (!name_is_c_reserved(fn->value)) continue;

        /* `ae` + the name keeps the underscore, so `_write` becomes
           `ae_write` — readable in a backtrace and out of the reserved
           namespace, since the leading character is no longer `_`. */
        char safe[280];
        snprintf(safe, sizeof(safe), "ae%s", fn->value);
        rename_all_refs_to(program, fn->value, safe);
        char* dup = strdup(safe);
        if (dup) {
            free(fn->value);
            fn->value = dup;
        }
    }
}

static void rename_extern_colliding_functions(ASTNode* program) {
    if (!program) return;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* fn = program->children[i];
        if (!fn || (fn->type != AST_FUNCTION_DEFINITION &&
                    fn->type != AST_BUILDER_FUNCTION)) continue;
        if (fn->is_imported || is_c_callback(fn) || !fn->value) continue;
        if (!tu_declares_extern(program, fn->value)) continue;

        char safe[280];
        snprintf(safe, sizeof(safe), "ae_%s", fn->value);
        rename_all_refs_to(program, fn->value, safe);
        char* dup = strdup(safe);
        if (dup) {
            free(fn->value);
            fn->value = dup;
        }
    }
}

/* ---- link-requirement emission (emit-link-requirements-from-import-graph) --
 *
 * aetherc resolves the full import graph, so it knows exactly which native
 * libraries a program pulls in. We emit that as a `// aether-link:` comment on
 * the first line of the generated C, so a downstream build that links the .c
 * doesn't have to rediscover the list by `undefined reference` archaeology:
 *
 *     // aether-link: -lsqlite3 -lssl -lcrypto -lpcre2-8
 *     AE_LINK="$(sed -n 's|^// aether-link:||p' out.c)"; cc ... out.c $AE_LINK
 *
 * Only libraries INTRODUCED BY imports appear here; the runtime baseline
 * (-laether -pthread -lm, winsock/bcrypt on Windows) stays with
 * `ae cflags --libs`, as the ask specifies. The mapping is a static table
 * keyed on the dotted module name — the same shape as the `gated[]`
 * capability table in aetherc.c — using the canonical pkg-config `-l` names,
 * which are stable across Linux/macOS/MinGW (only -L paths differ, and those
 * belong to the consumer). It is matched against the RESOLVED import closure
 * (global_module_registry, which module_orchestrate populates transitively),
 * so an import reached only through another module still contributes its libs.
 * Order is table order (stable); tokens are de-duplicated. */
typedef struct { const char* module; const char* libs; } AetherLinkReq;

/* Truthful, per-module: each entry names a module whose C backing directly
 * links the given lib (verified against the extern references in the std C
 * sources -- openssl in std/net + std/http/client + std/cryptography, pcre2 in
 * std/regex, nghttp2 in std/net + h2, zlib in std/zlib + http/middleware).
 * Matched by EXACT dotted name, deliberately: the pure-Aether crypto submodules
 * (std.cryptography.sha, .blake2, etc.) carry no native dep and must NOT
 * inherit std.cryptography's openssl. New native-backed modules add a row. */
static const AetherLinkReq g_link_reqs[] = {
    { "std.regex",            "-lpcre2-8" },
    { "std.net",              "-lssl -lcrypto -lnghttp2" },
    { "std.http",             "-lssl -lcrypto -lnghttp2" },
    { "std.http.client",      "-lssl -lcrypto" },
    { "std.http.server.h2",   "-lnghttp2 -lssl -lcrypto" },
    { "std.cryptography",     "-lssl -lcrypto" },
    { "std.zlib",             "-lz" },
    { "std.http.middleware",  "-lz" },
    { "std.audio",            "-lpthread -ldl -lm" },
    /* contrib.sqlite's row moved into contrib/sqlite/module.ae as
     * `@link(...)` (#1259): the module owns its native deps, the compiler
     * owns only the union-over-closure mechanism. */
};
static const int g_link_req_count = (int)(sizeof(g_link_reqs) / sizeof(g_link_reqs[0]));

/* True if `mod` (a dotted module name) is in the program's resolved import
 * closure. Prefers the transitive registry; falls back to direct imports on
 * the program AST if the registry is unavailable (e.g. a bare codegen call). */
static int program_imports_module(ASTNode* program, const char* mod) {
    if (global_module_registry) {
        for (int i = 0; i < global_module_registry->module_count; i++) {
            AetherModule* m = global_module_registry->modules[i];
            if (m && m->name && strcmp(m->name, mod) == 0) return 1;
        }
    }
    if (program) {
        for (int i = 0; i < program->child_count; i++) {
            ASTNode* c = program->children[i];
            if (c && c->type == AST_IMPORT_STATEMENT && c->value &&
                strcmp(c->value, mod) == 0) return 1;
        }
    }
    return 0;
}

/* Emit `// aether-link: <tokens>` as the first line of the TU when the import
 * closure introduces any native-library dependency. De-dupes tokens across
 * modules (e.g. std.http and std.cryptography both want -lssl -lcrypto). */
/* Split `flags` on spaces and append each token to toks[] unless already
 * present. Tokens are heap copies; the caller frees them after printing. */
static void add_link_tokens(const char* flags, const char** toks, int* ntok, int cap) {
    const char* s = flags;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        const char* start = s;
        while (*s && *s != ' ') s++;
        size_t len = (size_t)(s - start);
        int dup = 0;
        for (int k = 0; k < *ntok; k++) {
            if (strlen(toks[k]) == len && strncmp(toks[k], start, len) == 0) { dup = 1; break; }
        }
        if (!dup && *ntok < cap) {
            char* copy = (char*)malloc(len + 1);
            if (copy) { memcpy(copy, start, len); copy[len] = '\0'; toks[(*ntok)++] = copy; }
        }
    }
}

static void emit_link_requirements(CodeGenerator* gen, ASTNode* program) {
    /* Accumulate unique tokens in first-seen (table) order. */
    const char* toks[64];
    int ntok = 0;
    for (int r = 0; r < g_link_req_count; r++) {
        if (!program_imports_module(program, g_link_reqs[r].module)) continue;
        add_link_tokens(g_link_reqs[r].libs, toks, &ntok,
                        (int)(sizeof(toks) / sizeof(toks[0])));
    }

    /* #1259: module-declared deps. A module's own `@link("...")` directives
     * travel in its AST; union them across the resolved import closure (and
     * the entry program itself), so no module needs a row in the static
     * table above. Same dedup, same first-seen ordering. */
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_LINK_DIRECTIVE && c->value)
            add_link_tokens(c->value, toks, &ntok,
                            (int)(sizeof(toks) / sizeof(toks[0])));
    }
    if (global_module_registry) {
        for (int m = 0; m < global_module_registry->module_count; m++) {
            AetherModule* mod = global_module_registry->modules[m];
            if (!mod || !mod->ast) continue;
            for (int i = 0; i < mod->ast->child_count; i++) {
                ASTNode* c = mod->ast->children[i];
                if (c && c->type == AST_LINK_DIRECTIVE && c->value)
                    add_link_tokens(c->value, toks, &ntok,
                                    (int)(sizeof(toks) / sizeof(toks[0])));
            }
        }
    }
    if (ntok == 0) return;

    fputs("// aether-link:", gen->output);
    for (int i = 0; i < ntok; i++) {
        fprintf(gen->output, " %s", toks[i]);
        free((void*)toks[i]);
    }
    fputc('\n', gen->output);
}

void generate_program(CodeGenerator* gen, ASTNode* program) {
    if (!program || program->type != AST_PROGRAM) return;
    gen->program = program;

    /* Link requirements from the resolved import graph — the very first line
     * of the TU (a // comment is inert to the preprocessor, so it may precede
     * the MinGW #if below). */
    emit_link_requirements(gen, program);
    // #976: rewrite C-keyword value identifiers to a valid C spelling before
    // any codegen pass reads their names (must run before escape analysis and
    // emission, which both key off the identifier names).
    mangle_keyword_value_idents(program);
    rename_leading_underscore_functions(program);
    rename_extern_colliding_functions(program);
    // Note: `gen->program` is the source of truth for the
    // structural-escape-analysis lookup (issue #405). Setting it
    // here means every per-fn codegen pass beyond this point can
    // recognise user-defined `-> string` functions as heap-returning.

    // If emitting header, write prologue
    if (gen->emit_header && gen->header_file) {
        emit_header_prologue(gen, NULL);
    }

    // MinGW (native Windows): bind printf / vsnprintf / snprintf to the
    // C99-conformant __mingw_* family instead of the legacy MSVCRT ones.
    // Without this, on MINGW64 the MSVCRT implementations mishandle the
    // C99 conversions Aether emits (%lld / %llu / %zu / %g) AND
    // vsnprintf(NULL, 0, ...) returns -1 rather than the would-be length.
    // The latter breaks the two-pass sizing in _aether_interp below
    // (malloc(-1+1) → a 0-byte buffer, then nothing written) — i.e. empty
    // string interpolation, which is exactly issue #681. This must be set
    // before any system header is pulled in, so it is the first thing in
    // the generated translation unit. Compiles to nothing off MinGW.
    print_line(gen, "#if defined(__MINGW32__) && !defined(__USE_MINGW_ANSI_STDIO)");
    print_line(gen, "#define __USE_MINGW_ANSI_STDIO 1");
    print_line(gen, "#endif");

    // Generate includes for runtime libraries
    print_line(gen, "#include <stdio.h>");
    print_line(gen, "#include <stdlib.h>");
    print_line(gen, "#include <string.h>");
    print_line(gen, "#include <stdbool.h>");
    print_line(gen, "#include <stdatomic.h>");
    print_line(gen, "#include <stdint.h>");
    print_line(gen, "#include <time.h>");
    print_line(gen, "#include <setjmp.h>");
    print_line(gen, "#include \"aether_panic.h\"");
    /* Cons-cell sequence type — std.collections.string_seq.
     *
     * We include the header unconditionally rather than gating on
     * "program uses *StringSeq" because:
     *   1. Detecting use requires walking every type annotation in
     *      the AST, including transitively-imported externs.
     *   2. The header is small (one struct + 10 prototypes, only
     *      <stddef.h> transitively) so the cost of including it
     *      always is negligible.
     *   3. The full struct definition is required at the call site
     *      whenever a `match` arm pattern-matches `[h|t]` against a
     *      *StringSeq, because the lowered C reads `s->head` /
     *      `s->tail` directly. A forward decl wouldn't be enough.
     *   4. The runtime header path is wired via tools/ae.c's -I
     *      flags (-I.../std/collections), and libaether.a (or the
     *      from-source fallback build) provides the symbols.
     */
    print_line(gen, "#include \"aether_stringseq.h\"");
    /* Issue #343 codegen tripwire: when --emit=lib is set, the loop
     * codegen emits a check at every loop head that calls
     * aether_caps_deadline_tripped() / __aether_abort_call(). The
     * symbols live in runtime/aether_resource_caps.{h,c}, which the
     * --emit=lib build links via libaether.a; declare the prototypes
     * inline so the emitted .c compiles cleanly without pulling
     * runtime headers into user-program path. Zero cost on
     * --emit=exe builds — the gate elides both the extern decls and
     * the per-loop checks. */
    if (gen->emit_lib) {
        print_line(gen, "extern int  aether_caps_deadline_tripped(void);");
        print_line(gen, "extern void __aether_abort_call(void);");
    }
    print_line(gen, "#ifdef _WIN32");
    print_line(gen, "#define NOMINMAX");
    print_line(gen, "#include <windows.h>");
    print_line(gen, "#include <io.h>      // _setmode, _fileno");
    print_line(gen, "#include <fcntl.h>   // _O_BINARY");
    print_line(gen, "#elif defined(__EMSCRIPTEN__)");
    print_line(gen, "#include <emscripten.h>");
    print_line(gen, "#else");
    print_line(gen, "#include <unistd.h>");
    print_line(gen, "#include <sched.h>");
    print_line(gen, "#endif");
    /* aligned_alloc: C11 POSIX; Windows uses _aligned_malloc with swapped args */
    print_line(gen, "#ifdef _WIN32");
    print_line(gen, "#  define aether_aligned_alloc(align, size) _aligned_malloc((size), (align))");
    print_line(gen, "#else");
    print_line(gen, "#  define aether_aligned_alloc(align, size) aligned_alloc((align), (size))");
    print_line(gen, "#endif");
    print_line(gen, "#ifndef likely");
    print_line(gen, "#  if defined(__GNUC__) || defined(__clang__)");
    print_line(gen, "#    define likely(x)   __builtin_expect(!!(x), 1)");
    print_line(gen, "#    define unlikely(x) __builtin_expect(!!(x), 0)");
    print_line(gen, "#  else");
    print_line(gen, "#    define likely(x)   (x)");
    print_line(gen, "#    define unlikely(x) (x)");
    print_line(gen, "#  endif");
    print_line(gen, "#endif");
    // Cooperative preemption: reduction counter for loop yield points
    if (gen->preempt_loops) {
        print_line(gen, "static int _aether_reductions = 10000;");
        print_line(gen, "#ifdef _WIN32");
        print_line(gen, "#define sched_yield() SwitchToThread()");
        print_line(gen, "#elif defined(__EMSCRIPTEN__)");
        print_line(gen, "#define sched_yield() ((void)0)");
        print_line(gen, "#endif");
    }
    /* GCC/Clang vs MSVC: guards for statement expressions ({...}) and computed goto */
    /* Imported wrappers and trailing-underscore privates are emitted with
       internal linkage, so a translation unit that imports a module and calls
       only part of it has unused statics left over. Downstream builds with
       -Wall -Werror would fail on generated code they did not write, so the
       storage class carries the attribute rather than the consumer carrying a
       -Wno flag. */
    print_line(gen, "#ifndef AETHER_MAYBE_UNUSED");
    print_line(gen, "#  if defined(__GNUC__) || defined(__clang__)");
    print_line(gen, "#    define AETHER_MAYBE_UNUSED __attribute__((unused))");
    print_line(gen, "#  else");
    print_line(gen, "#    define AETHER_MAYBE_UNUSED");
    print_line(gen, "#  endif");
    print_line(gen, "#endif");
    print_line(gen, "#ifndef AETHER_GCC_COMPAT");
    print_line(gen, "#  if (defined(__GNUC__) || defined(__clang__)) && !defined(__EMSCRIPTEN__)");
    print_line(gen, "#    define AETHER_GCC_COMPAT 1");
    print_line(gen, "#  else");
    print_line(gen, "#    define AETHER_GCC_COMPAT 0");
    print_line(gen, "#  endif");
    print_line(gen, "#endif");
    /* Suppress unused-function warnings for runtime helpers that may not
       be called in every program (clock_ns, interp, safe_str) */
    print_line(gen, "#if defined(__GNUC__) || defined(__clang__)");
    print_line(gen, "#pragma GCC diagnostic push");
    print_line(gen, "#pragma GCC diagnostic ignored \"-Wunused-function\"");
    print_line(gen, "#endif");
    /* clock_ns helper — always available (used by timeout checks + clock_ns() builtin) */
    print_line(gen, "#ifdef _WIN32");
    print_line(gen, "static inline int64_t _aether_clock_ns(void) {");
    print_line(gen, "    LARGE_INTEGER freq, now;");
    print_line(gen, "    QueryPerformanceFrequency(&freq);");
    print_line(gen, "    QueryPerformanceCounter(&now);");
    print_line(gen, "    return (int64_t)((double)now.QuadPart / freq.QuadPart * 1000000000.0);");
    print_line(gen, "}");
    print_line(gen, "#elif defined(__EMSCRIPTEN__)");
    print_line(gen, "static inline int64_t _aether_clock_ns(void) {");
    print_line(gen, "    return (int64_t)(emscripten_get_now() * 1000000.0);");
    print_line(gen, "}");
    print_line(gen, "#elif defined(__STDC_HOSTED__) && (__STDC_HOSTED__ == 0)");
    print_line(gen, "static inline int64_t _aether_clock_ns(void) { return 0; }");
    print_line(gen, "#else");
    print_line(gen, "static inline int64_t _aether_clock_ns(void) {");
    print_line(gen, "    struct timespec _ts;");
    print_line(gen, "    clock_gettime(CLOCK_MONOTONIC, &_ts);");
    print_line(gen, "    return (int64_t)_ts.tv_sec * 1000000000LL + _ts.tv_nsec;");
    print_line(gen, "}");
    print_line(gen, "#endif");
    /* Uniform-heap return-escape helper. For every function classified
     * heap-returning by walk_returns_for_heap_check, every return site
     * routes its value through this helper:
     *
     *     return aether_uniform_heap_str(<expr>, <static-or-runtime-flag>);
     *
     * When the flag is true, the value is already heap-owned (e.g.
     * string.concat result, _aether_interp result, or a heap-tracked
     * local with `_heap_<name> == 1`) and ownership transfers to the
     * caller as-is — fast path is one branch + one return. When the
     * flag is false, the value is a literal / static pointer and the
     * caller's unconditional `free()` would crash on it; the helper
     * duplicates so the caller's free reclaims the dup uniformly.
     *
     * The cold-path dup mints a refcounted AetherString rather than a
     * bare buffer, because the caller cannot free a bare one. Aether's
     * `string` is either shape, and given a plain `char*` the runtime
     * cannot tell a heap payload from a static literal, so
     * `string.free(s)` no-ops on it and the value leaks: one block per
     * call at every `json.parse` error slot, every interpolation, every
     * HTTP body a caller dutifully frees (#1461).
     *
     * The hot path is untouched, so nothing that was already owned pays
     * for this, and the shape is not new to callers: the sibling path of
     * every one of these returns already hands back a string_concat
     * result, which is the same refcounted shape. Both `string_release`
     * and the codegen's `aether_heap_str_free` reclaim it, and printing
     * and comparison go through `aether_string_data`, which reads either
     * shape. What must NOT appear is a plain `free()` on the result: that
     * would reclaim the header and leak the payload.
     *
     * Binary-safety: Aether's `string` carries either plain `char*` or
     * a length-bearing `AetherString*` (magic-header struct). The cold-
     * path duplication must respect the source shape — a naive
     * `strlen` on an AetherString reads struct-header bytes and
     * truncates the payload at the first NUL inside the header. The
     * helper detects the magic header byte-by-byte (matching
     * `is_aether_string`'s ASan-clean shape) and uses the stored
     * `length` field. The duplicate is always a plain malloc-owned
     * buffer so the caller's plain `free()` reclaims it — the
     * helper's contract is "pointer the caller may free", not
     * "preserve AetherString shape". */
    print_line(gen, "#include <string.h>");
    print_line(gen, "#include <stdlib.h>");
    print_line(gen, "#include <stddef.h>");
    print_line(gen, "extern void* aether_caps_malloc(size_t bytes);");
    print_line(gen, "extern void* string_new_with_length(const char* data, int length);");
    print_line(gen, "static inline const char* aether_uniform_heap_str(const char* s, int is_heap) {");
    print_line(gen, "    if (!s) return (const char*)0;");
    print_line(gen, "    if (is_heap) return s;");
    print_line(gen, "    /* AetherString-aware length probe, see is_aether_string in");
    print_line(gen, "     * std/string/aether_string.h. Byte-by-byte to stay ASan-clean");
    print_line(gen, "     * on short literal allocations (e.g. \"x\"). */");
    print_line(gen, "    const unsigned char* _p = (const unsigned char*)s;");
    print_line(gen, "    const char* _data = s;");
    print_line(gen, "    size_t _n;");
    print_line(gen, "    if (_p[0] == 0xDE && _p[1] == 0xC0 && _p[2] == 0x57 && _p[3] == 0xAE) {");
    print_line(gen, "        /* Struct layout: magic(u32), ref_count(i32), length(size_t),");
    print_line(gen, "         * capacity(size_t), data(char*). Read length and data via");
    print_line(gen, "         * a typed view, the struct's data pointer is what we copy. */");
    print_line(gen, "        struct _AeStrHdr { unsigned int magic; int ref_count; size_t length; size_t capacity; char* data; };");
    print_line(gen, "        const struct _AeStrHdr* _h = (const struct _AeStrHdr*)s;");
    print_line(gen, "        _n = _h->length;");
    print_line(gen, "        _data = _h->data ? _h->data : s;");
    print_line(gen, "    } else {");
    print_line(gen, "        _n = strlen(s);");
    print_line(gen, "    }");
    print_line(gen, "    return (const char*)string_new_with_length(_data, (int)_n);");
    print_line(gen, "}");
    /* AetherString-aware heap-string release. A `_heap_<name>` slot
     * tracked by the codegen can hold two physically distinct shapes:
     *
     *   - a plain libc-malloc'd char* — string.concat, interpolation,
     *     and `@heap` externs (fs.realpath, os.run_capture) all return
     *     this; a plain free() reclaims it fully.
     *   - a refcounted AetherString* — struct plus a SEPARATELY
     *     allocated data buffer (`string_new_with_length`, the result
     *     shape behind `bytes.finish`). free() on the struct alone
     *     leaks the data buffer; the whole value must go through
     *     `string_release`, which frees both with their recorded
     *     sizes.
     *
     * Probe the AetherString magic header byte-by-byte with short-
     * circuit (ASan-clean on 1-byte allocations — identical shape to
     * aether_uniform_heap_str above and is_aether_string in
     * std/string/aether_string.h). On a hit, route to string_release;
     * string_release re-validates the full header internally, so a
     * plain char* whose first four bytes coincidentally match the
     * magic degrades to a 1-in-2^32 no-op (a leak, never a crash).
     * string_release is forward-declared with the `const char*`
     * signature the extern-registry pass also uses, so the duplicate
     * declaration is tolerated by C. */
    print_line(gen, "extern void string_release(const char*);");
    /* #1301 allocation journal: while a panic frame is live, every armed
     * deferred free is journaled so aether_panic() can run the frees a
     * longjmp would skip. This helper is the single free choke point,
     * so forgetting here keeps the journal free of already-freed
     * entries (the invariant that makes the unwind drain a leak-fix,
     * never a use-after-free). */
    print_line(gen, "typedef void (*AetherUnwindFree)(const void*);");
    print_line(gen, "extern void aether_unwind_track(const void*, AetherUnwindFree);");
    print_line(gen, "extern void aether_unwind_track_if(const void*, int, AetherUnwindFree);");
    print_line(gen, "extern void aether_unwind_forget(const void*);");
    print_line(gen, "static inline void aether_heap_str_free(const char* s) {");
    print_line(gen, "    if (!s) return;");
    print_line(gen, "    aether_unwind_forget(s);");
    print_line(gen, "    const unsigned char* _hp = (const unsigned char*)s;");
    print_line(gen, "    if (_hp[0] == 0xDE && _hp[1] == 0xC0 && _hp[2] == 0x57 && _hp[3] == 0xAE) {");
    print_line(gen, "        string_release(s);");
    print_line(gen, "    } else {");
    print_line(gen, "        free((void*)s);");
    print_line(gen, "    }");
    print_line(gen, "}");
    print_line(gen, "static void aether_unwind_free_str(const void* p) {");
    print_line(gen, "    aether_heap_str_free((const char*)p);");
    print_line(gen, "}");
    print_line(gen, "static inline void aether_unwind_track_str(const char* s) {");
    print_line(gen, "    aether_unwind_track(s, aether_unwind_free_str);");
    print_line(gen, "}");
    print_line(gen, "static inline void aether_unwind_track_str_if(const char* s, int owned) {");
    print_line(gen, "    if (owned) aether_unwind_track(s, aether_unwind_free_str);");
    print_line(gen, "}");
    /* Closure-env string capture: a closure env must OWN its captured
     * strings — the enclosing scope releases its own reference on
     * reassignment (loop-carried heap slots) and at scope exit, so a
     * borrowed pointer dangles by the time a STORED closure fires
     * (asks/closure-captured-heap-string-dangles.md). string_retain
     * no-ops on anything without the AetherString magic header
     * (literals, plain char*), so applying it to every read-only
     * string capture is safe. Forward-declared with the same
     * `const char*` signature the extern-registry pass uses (see
     * string_release above). Promoted (assigned-to) captures share a
     * heap cell instead and never come through here. The env's
     * reference is intentionally never released (closure envs have no
     * member-aware free today): a bounded leak per closure creation,
     * replacing a use-after-free. Residual: a PLAIN-malloc heap string
     * (magic-less, e.g. from an @heap extern) still can't be retained
     * — rare in capture position; noted in the ask. */
    print_line(gen, "extern void string_retain(const char*);");
    /* #1398: string_retain cannot take a reference to a magic-less pointer, so
       a plain malloc'd heap string was captured borrowed and freed by the
       enclosing scope while a stored closure still held it. The env now owns a
       real reference and gives it back in its generated teardown. */
    print_line(gen, "extern const char* aether_string_capture_owned(const char*);");
    print_line(gen, "extern void aether_string_release_captured(const char*);");
    print_line(gen, "static inline const char* aether_str_capture(const char* s) {");
    print_line(gen, "    return aether_string_capture_owned(s);");
    print_line(gen, "}");
    /* Prototypes for the magic-aware string builtins the codegen emits
     * directly (char_at -> string_char_at, str_eq / match-on-string ->
     * string_equals). These are not routed through the normal extern-call
     * path, so their prototypes would otherwise be missing — an implicit-
     * declaration error under C99/-Werror. Declared unconditionally;
     * harmless if unused. */
    /* Signatures MUST match the extern-registry forms (emitted when
     * std.string is imported) verbatim, or C sees conflicting
     * declarations. */
    print_line(gen, "int string_char_at(const char*, int);");
    print_line(gen, "int string_equals(const char*, const char*);");
    /* Codegen-internal: a `fn`-typed closure value stored into a list is
     * boxed and routed here so list_free reclaims the box + its env. Not
     * a std.collections extern, so declare it directly. */
    print_line(gen, "int list_add_closure_owned(void*, void*);");
    /* String interpolation helper — portable, always available */
    print_line(gen, "#include <stdarg.h>");
    /* Returns the REFCOUNTED shape, not a bare buffer. Aether's `string` is
     * either, and the runtime cannot free a bare one: given a plain char* it
     * cannot tell a heap payload from a literal, so `string.free` no-ops and
     * the value leaks. Every interpolated string a caller freed by hand, or
     * handed to a helper that frees, leaked exactly that way (#1543).
     *
     * The buffer is adopted rather than copied, so the cost is one header
     * allocation on top of the payload the format already needed. */
    print_line(gen, "extern void* string_alloc_inline(size_t length);");
    print_line(gen, "extern char* aether_string_mutable_data(void* s);");
    print_line(gen, "static void* _aether_interp(const char* fmt, ...) {");
    print_line(gen, "    va_list args, args2;");
    print_line(gen, "    va_start(args, fmt);");
    print_line(gen, "    va_copy(args2, args);");
    print_line(gen, "    int len = vsnprintf(NULL, 0, fmt, args);");
    print_line(gen, "    va_end(args);");
    print_line(gen, "    if (len < 0) { va_end(args2); return (void*)0; }");
    print_line(gen, "    void* owned = string_alloc_inline((size_t)len);");
    print_line(gen, "    if (!owned) { va_end(args2); return (void*)0; }");
    print_line(gen, "    vsnprintf(aether_string_mutable_data(owned), (size_t)len + 1, fmt, args2);");
    print_line(gen, "    va_end(args2);");
    print_line(gen, "    return owned;");
    print_line(gen, "}");
    /* NULL-safe string helper for print/println — avoids double-evaluating
     * the expression. Goes through aether_string_data() which dispatches
     * on the AetherString magic header so values returned by length-
     * bearing primitives (string_from_int, string_concat_wrapped,
     * fs.read_binary, …) print their payload bytes rather than the
     * struct header. Plain char* values pass through unchanged. */
    print_line(gen, "extern const char* aether_string_data(const void* s);");
    print_line(gen, "extern size_t aether_string_length(const void* s);");
    /* Issue #466: actor message heap-string deep-copy at send time.
     * `string_new_with_length` clones the source bytes into a fresh
     * refcounted AetherString. Exposed here so the message-send
     * codegen doesn't re-emit the prototype at every call site.
     * `string_release` is forward-declared with the extern-registry
     * pass's `const char*` signature above (next to the
     * aether_heap_str_free helper). Both that declaration and the
     * extern-registry / <Msg>_release_fields ones are identical, so
     * the duplicates are tolerated by C — no conflicting-types error. */
    print_line(gen, "extern void* string_new_with_length(const char* data, int length);");
    /* Issue #467: list / map heap-string-value owning-add helpers.
     * Prototypes emitted here so codegen can route `list.add(l,
     * heap_str)` / `map.put(m, k, heap_str)` calls to these symbols
     * even when the user's source doesn't `import std.list` /
     * `import std.collections` (the inline-extern shape some tests
     * use to declare just the raw functions). The stdlib archive
     * always contains the symbols.
     *
     * Parameter types match the extern-registry's pass exactly —
     * `void*` not `const void*` — so a duplicate decl from the
     * registry (when the source DOES import the module) doesn't
     * conflict. The header's `const void*` declaration is the
     * source-of-truth for consumers in C; the registry maps Aether's
     * `ptr` to `void*` without const, which is what this matches. */
    /* Adopting variants: the value is escaping into the container and
     * the caller's escape marking suppresses its own release, so the
     * container takes over that single reference without retaining.
     * The *_owned entries are the hand-callable ones that acquire
     * their own reference; routing codegen at them would leak a
     * refcount per add. */
    print_line(gen, "extern int list_add_string_adopted(void* list, void* item);");
    print_line(gen, "extern int map_put_string_adopted(void* map, const char* key, void* value);");
    /* The adopting rewrite has to preserve `list.add` / `map.put`'s
     * string-return contract while calling an int-returning entry. Doing that
     * inline as `(adopted(...) ? "" : "...")` builds an expression whose value
     * every statement-position call then discards, and a discarded ternary is
     * -Wunused-value: `list.add(l, string.copy(p))` warned in ordinary user
     * code. A function call discards its result silently, so the conversion
     * lives in these helpers instead. */
    print_line(gen, "static inline const char* _aether_list_add_adopted(void* list, void* item) {");
    print_line(gen, "    return list_add_string_adopted(list, item) ? \"\" : \"list.add failed\";");
    print_line(gen, "}");
    print_line(gen, "static inline const char* _aether_map_put_adopted(void* map, const char* key, void* value) {");
    print_line(gen, "    return map_put_string_adopted(map, key, value) ? \"\" : \"map.put failed\";");
    print_line(gen, "}");
    print_line(gen, "static inline const char* _aether_list_add_closure(void* list, void* box) {");
    print_line(gen, "    return list_add_closure_owned(list, box) ? \"\" : \"list.add failed\";");
    print_line(gen, "}");
    print_line(gen, "static inline const char* _aether_safe_str(const void* s) {");
    print_line(gen, "    if (!s) return \"(null)\";");
    print_line(gen, "    return aether_string_data(s);");
    print_line(gen, "}");
    /* Owned-print helpers: a heap-producing CALL in bare print/println
     * argument position (`println(string.concat(a, b))`) is a temporary
     * nothing else owns; these print it and free it in one composable
     * expression. Bare identifiers never route here, their scope-exit
     * defer owns the free. */
    print_line(gen, "static inline int _aether_println_owned(const char* s) {");
    print_line(gen, "    int _n = printf(\"%%s\\n\", _aether_safe_str(s));");
    print_line(gen, "    aether_heap_str_free((void*)s);");
    print_line(gen, "    return _n;");
    print_line(gen, "}");
    print_line(gen, "static inline int _aether_print_owned(const char* s) {");
    print_line(gen, "    int _n = printf(\"%%s\", _aether_safe_str(s));");
    print_line(gen, "    aether_heap_str_free((void*)s);");
    print_line(gen, "    return _n;");
    print_line(gen, "}");
    /* #927: multiple ${duration} interpolations can be live in a SINGLE
     * printf/snprintf call (all args evaluate before the format runs), so a
     * single shared static buffer would have every %%s slot show the last
     * result. Hand out a small ring of buffers — one per call — so up to
     * AE_DUR_BUFS distinct results coexist in one expression. */
    print_line(gen, "#define AE_DUR_BUFS 8");
    print_line(gen, "static inline const char* _aether_duration_repr(int64_t ns) {");
    print_line(gen, "    static char _bufs[AE_DUR_BUFS][64];");
    print_line(gen, "    static unsigned _slot = 0;");
    print_line(gen, "    char* _buf = _bufs[_slot];");
    print_line(gen, "    _slot = (_slot + 1) %% AE_DUR_BUFS;");
    print_line(gen, "    int64_t abs_ns = ns < 0 ? -ns : ns;");
    print_line(gen, "    struct _du { const char* suffix; int64_t scale; } units[] = {");
    print_line(gen, "        {\"d\", 86400000000000LL}, {\"h\", 3600000000000LL},");
    print_line(gen, "        {\"m\", 60000000000LL}, {\"s\", 1000000000LL},");
    print_line(gen, "        {\"ms\", 1000000LL}, {\"us\", 1000LL}, {\"ns\", 1LL}");
    print_line(gen, "    };");
    print_line(gen, "    for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); i++) {");
    print_line(gen, "        if (abs_ns >= units[i].scale || units[i].scale == 1) {");
    print_line(gen, "            if (ns %% units[i].scale == 0) {");
    print_line(gen, "                snprintf(_buf, 64, \"%%lld%%s\", (long long)(ns / units[i].scale), units[i].suffix);");
    print_line(gen, "            } else {");
    print_line(gen, "                double v = (double)ns / (double)units[i].scale;");
    print_line(gen, "                snprintf(_buf, 64, \"%%.9g%%s\", v, units[i].suffix);");
    print_line(gen, "            }");
    print_line(gen, "            return _buf;");
    print_line(gen, "        }");
    print_line(gen, "    }");
    print_line(gen, "    return \"0ns\";");
    print_line(gen, "}");
    // Built-in `sleep(ms)` lowers to aether_sleep_ms — a runtime helper
    // with a stable, prefixed name so user `extern sleep(...)` doesn't
    // conflict with libc's `unsigned int sleep(unsigned int)`. Issue #233.
    print_line(gen, "extern void aether_sleep_ms(int ms);");
    // Ref cells: heap-allocated mutable values for shared state in closures
    print_line(gen, "#if !AETHER_GCC_COMPAT");
    print_line(gen, "static void* _aether_ref_new(intptr_t val) { intptr_t* r = malloc(sizeof(intptr_t)); *r = val; return (void*)r; }");
    print_line(gen, "#endif");
    // Closure support: generic closure struct (function pointer + captured environment)
    print_line(gen, "typedef struct { void (*fn)(void); void* env; } _AeClosure;");
    /* Boxed form. The tag sits AFTER the {fn, env} prefix on purpose: that
     * prefix is the FFI layout std/collections and std/worker mirror and
     * embedders are documented to rely on, so putting the tag first would
     * silently turn every C-side reader's `fn` into the tag and call it.
     *
     * Boxing is the only way a valid closure pointer is ever produced, so
     * the tag is what tells a real box apart from a raw code address,
     * which is what a bare function coerced into a `ptr` slot leaves
     * behind. Unboxing that used to read `.env` off the function's own
     * machine code and jump through it (issue #1439). */
    print_line(gen, "typedef struct { void (*fn)(void); void* env; unsigned long long tag; } _AeClosureBox;");
    print_line(gen, "#define _AE_CLOSURE_TAG 0xAEC105EDB0CEDULL");
    print_line(gen, "static inline void* _aether_box_closure(_AeClosure c) {");
    print_line(gen, "    _AeClosureBox* p = (_AeClosureBox*)malloc(sizeof(_AeClosureBox));");
    print_line(gen, "    if (!p) return (void*)0;");
    print_line(gen, "    p->fn = c.fn; p->env = c.env; p->tag = _AE_CLOSURE_TAG;");
    print_line(gen, "    return (void*)p;");
    print_line(gen, "}");
    print_line(gen, "static inline _AeClosure _aether_unbox_closure(void* p) {");
    print_line(gen, "    if (!p || ((const _AeClosureBox*)p)->tag != _AE_CLOSURE_TAG) {");
    print_line(gen, "        aether_panic(\"unbox_closure() on a value that was never boxed. \"");
    print_line(gen, "                     \"A bare function stored into a `ptr` slot stays a raw code \"");
    print_line(gen, "                     \"pointer; only a value that crossed an `fn`-typed boundary or \"");
    print_line(gen, "                     \"went through box_closure() carries an environment. Declare the \"");
    print_line(gen, "                     \"slot `fn`, or box explicitly before storing it.\");");
    print_line(gen, "    }");
    print_line(gen, "    _AeClosure c;");
    print_line(gen, "    c.fn = ((const _AeClosureBox*)p)->fn;");
    print_line(gen, "    c.env = ((const _AeClosureBox*)p)->env;");
    print_line(gen, "    return c;");
    print_line(gen, "}");
    // Lazy evaluation: thunks (deferred computation with memoization)
    print_line(gen, "typedef struct { _AeClosure compute; intptr_t value; int evaluated; } _AeThunk;");
    print_line(gen, "static inline void* _aether_thunk_new(_AeClosure c) { _AeThunk* t = malloc(sizeof(_AeThunk)); t->compute = c; t->value = 0; t->evaluated = 0; return (void*)t; }");
    print_line(gen, "static inline intptr_t _aether_thunk_force(void* p) { _AeThunk* t = (_AeThunk*)p; if (!t->evaluated) { t->value = (intptr_t)((intptr_t(*)(void*))t->compute.fn)(t->compute.env); t->evaluated = 1; } return t->value; }");
    // thunk_free: free the thunk struct. The closure env is owned by
    // the closure variable (auto-deferred), not the thunk. The thunk
    // borrows the env pointer — freeing it here would double-free
    // when the closure variable's defer runs.
    print_line(gen, "static inline void _aether_thunk_free(void* p) { if (p) free(p); }");
    // Terminal raw mode helpers for interactive input
    // Only available on hosted POSIX systems (not embedded/bare-metal or Windows)
    print_line(gen, "#if !defined(_WIN32) && !defined(__EMSCRIPTEN__) && defined(__STDC_HOSTED__) && (__STDC_HOSTED__ == 1) && !defined(__arm__) && !defined(__thumb__)");
    print_line(gen, "#include <termios.h>");
    print_line(gen, "static struct termios _aether_orig_termios;");
    print_line(gen, "static void _aether_raw_mode(void) {");
    print_line(gen, "    tcgetattr(0, &_aether_orig_termios);");
    print_line(gen, "    struct termios raw = _aether_orig_termios;");
    print_line(gen, "    raw.c_lflag &= ~(ICANON | ECHO);");
    print_line(gen, "    tcsetattr(0, TCSANOW, &raw);");
    print_line(gen, "}");
    print_line(gen, "static void _aether_cooked_mode(void) {");
    print_line(gen, "    tcsetattr(0, TCSANOW, &_aether_orig_termios);");
    print_line(gen, "}");
    print_line(gen, "#else");
    print_line(gen, "static void _aether_raw_mode(void) {}");
    print_line(gen, "static void _aether_cooked_mode(void) {}");
    print_line(gen, "#endif");
    // Builder context stack: trailing blocks push/pop the return value
    print_line(gen, "static void* _aether_ctx_stack[64];");
    print_line(gen, "static int _aether_ctx_depth = 0;");
    print_line(gen, "static inline void _aether_ctx_push(void* ctx) { if (_aether_ctx_depth < 64) _aether_ctx_stack[_aether_ctx_depth++] = ctx; }");
    print_line(gen, "static inline void _aether_ctx_pop(void) { if (_aether_ctx_depth > 0) _aether_ctx_depth--; }");
    print_line(gen, "static inline void* _aether_ctx_get(void) { return _aether_ctx_depth > 0 ? _aether_ctx_stack[_aether_ctx_depth-1] : (void*)0; }");
    // Only emit sandbox bridge code if the program actually uses sandbox builtins.
    // This avoids preamble bloat and the list_size/list_get dependency for programs
    // that don't use sandboxing.
    bool has_sandbox = uses_sandbox(program);
    if (has_sandbox) {
        // Sandbox bridge: connects compiler-generated context stack to runtime checks.
        print_line(gen, "typedef int (*aether_sandbox_check_fn)(const char*, const char*);");
        print_line(gen, "extern aether_sandbox_check_fn _aether_sandbox_checker;");
        print_line(gen, "extern int list_size(void*);");
        print_line(gen, "extern void* list_get_raw(void*, int);");
        print_line(gen, "static int _aether_perms_allow(void* ctx, const char* category, const char* resource) {");
        print_line(gen, "    if (!ctx) return 1;");
        print_line(gen, "    int n = list_size(ctx);");
        print_line(gen, "    if (n == 0) return 0;");
        print_line(gen, "    for (int i = 0; i < n; i += 2) {");
        print_line(gen, "        const char* cat = (const char*)list_get_raw(ctx, i);");
        print_line(gen, "        const char* pat = (const char*)list_get_raw(ctx, i + 1);");
        print_line(gen, "        if (!cat || !pat) continue;");
        print_line(gen, "        if (cat[0] == '*' && pat[0] == '*') return 1;");
        print_line(gen, "        if (strcmp(cat, category) == 0) {");
        print_line(gen, "            int plen = strlen(pat);");
        print_line(gen, "            int rlen = strlen(resource);");
        print_line(gen, "            if (plen == 1 && pat[0] == '*') return 1;");
        print_line(gen, "            if (plen > 1 && pat[plen-1] == '*') {");
        print_line(gen, "                if (strncmp(pat, resource, plen-1) == 0) return 1;");
        print_line(gen, "            }");
        print_line(gen, "            if (plen > 1 && pat[0] == '*') {");
        print_line(gen, "                int slen = plen - 1;");
        print_line(gen, "                if (rlen >= slen && strcmp(resource + rlen - slen, pat + 1) == 0) return 1;");
        print_line(gen, "            }");
        print_line(gen, "            if (strcmp(pat, resource) == 0) return 1;");
        print_line(gen, "        }");
        print_line(gen, "    }");
        print_line(gen, "    return 0;");
        print_line(gen, "}");
        print_line(gen, "extern void aether_sandbox_audit(const char*, const char*, int);");
        print_line(gen, "static int _aether_sandbox_check_impl(const char* category, const char* resource) {");
        print_line(gen, "    if (_aether_ctx_depth <= 0) return 1;");
        print_line(gen, "    int _allowed = 1;");
        print_line(gen, "    for (int level = 0; level < _aether_ctx_depth; level++) {");
        print_line(gen, "        if (!_aether_perms_allow(_aether_ctx_stack[level], category, resource)) { _allowed = 0; break; }");
        print_line(gen, "    }");
        // Audit every in-process permission check — allowed and denied.
        // The sink is opt-in (AETHER_SANDBOX_AUDIT) and the ring buffer
        // is cheap, so this is unconditional. Note this runs only when a
        // sandbox is active (_aether_ctx_depth > 0).
        print_line(gen, "    aether_sandbox_audit(category, resource, _allowed);");
        print_line(gen, "    return _allowed;");
        print_line(gen, "}");
        print_line(gen, "static void _aether_sandbox_install(void) { _aether_sandbox_checker = _aether_sandbox_check_impl; }");
        print_line(gen, "static void _aether_sandbox_uninstall(void) { if (_aether_ctx_depth <= 0) _aether_sandbox_checker = 0; }");
        print_line(gen, "extern int aether_spawn_sandboxed(void* grant_list, const char* program, const char* arg);");
    }
    print_line(gen, "");
    // End of static helper definitions — close the warning suppression
    print_line(gen, "#if defined(__GNUC__) || defined(__clang__)");
    print_line(gen, "#pragma GCC diagnostic pop");
    print_line(gen, "#endif");
    print_line(gen, "");
    // Declare runtime args function (avoid full header to prevent conflicts with actor runtime)
    print_line(gen, "void aether_args_init(int argc, char** argv);");
    // Opt-in Capsicum self-sandbox hook (runtime/sandbox/capsicum_autosandbox.c).
    // No-op unless AETHER_CAPSICUM=1 and the platform is FreeBSD.
    print_line(gen, "void aether_capsicum_autosandbox(void);");
    print_line(gen, "");

    // Only include actor runtime if program uses actors
    bool has_actors = false;
    for (int i = 0; i < program->child_count; i++) {
        if (program->children[i] && program->children[i]->type == AST_ACTOR_DEFINITION) {
            has_actors = true;
            break;
        }
    }
    
    if (has_actors) {
        print_line(gen, "#include <stdatomic.h>");
        print_line(gen, "");
        print_line(gen, "// Aether runtime libraries");
        print_line(gen, "#include \"actor_state_machine.h\"");
        print_line(gen, "#include \"aether_send_message.h\"");
        print_line(gen, "#include \"aether_actor_thread.h\"");
        print_line(gen, "#include \"multicore_scheduler.h\"");
        print_line(gen, "#include \"aether_cpu_detect.h\"");
        print_line(gen, "#include \"aether_optimization_config.h\"");
        print_line(gen, "#include \"aether_bounds_check.h\"");
        print_line(gen, "#include \"aether_runtime_types.h\"");
        print_line(gen, "#include \"aether_compiler.h\"");
        print_line(gen, "");
        print_line(gen, "extern AETHER_TLS int current_core_id;");
        print_line(gen, "");
        print_line(gen, "// Benchmark timing function");
        print_line(gen, "static inline uint64_t rdtsc() {");
        print_line(gen, "#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))");
        print_line(gen, "    return __rdtsc();");
        print_line(gen, "#elif defined(__x86_64__) || defined(__i386__)");
        print_line(gen, "    unsigned int lo, hi;");
        print_line(gen, "    __asm__ __volatile__ (\"rdtsc\" : \"=a\" (lo), \"=d\" (hi));");
        print_line(gen, "    return ((uint64_t)hi << 32) | lo;");
        print_line(gen, "#elif (defined(__aarch64__) || defined(__arm__)) && defined(__unix__)");
        print_line(gen, "    struct timespec ts;");
        print_line(gen, "    clock_gettime(CLOCK_MONOTONIC, &ts);");
        print_line(gen, "    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;");
        print_line(gen, "#elif defined(__EMSCRIPTEN__)");
        print_line(gen, "    return (uint64_t)(emscripten_get_now() * 1000000.0);");
        print_line(gen, "#else");
        print_line(gen, "    return 0;");
        print_line(gen, "#endif");
        print_line(gen, "}");
        // MSVC ask-operator helper: does ask, extracts field by offset, frees reply
        print_line(gen, "#if !AETHER_GCC_COMPAT");
        print_line(gen, "#include <stddef.h>");
        print_line(gen, "static intptr_t _aether_ask_helper(ActorBase* target, void* msg, size_t msg_size, int timeout_ms, size_t field_offset, size_t field_size) {");
        print_line(gen, "    void* reply = scheduler_ask_message(target, msg, msg_size, timeout_ms);");
        print_line(gen, "    if (!reply) return 0;");
        print_line(gen, "    intptr_t val = 0;");
        print_line(gen, "    memcpy(&val, (char*)reply + field_offset, field_size < sizeof(intptr_t) ? field_size : sizeof(intptr_t));");
        print_line(gen, "    free(reply);");
        print_line(gen, "    return val;");
        print_line(gen, "}");
        print_line(gen, "#endif");
    }
    print_line(gen, "");

    // Pre-scan: merge tuple return types across all returns in each
    // function. This only mutates the AST (resolving UNKNOWN tuple
    // element types); the actual `typedef struct { ... } _tuple_...`
    // EMISSION is deferred until after the user struct bodies are
    // emitted below — a tuple whose element is a user struct (e.g.
    // `(Point, string)`) embeds that struct BY VALUE and so needs its
    // full definition visible first, else the generated C errors with
    // "unknown type name 'Point'" (issue #634). The merge must still
    // run here, before propagate_tuple_type_to_calls below.
    extern void merge_return_tuple_types(ASTNode* node, Type* merged);
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if (child && (child->type == AST_FUNCTION_DEFINITION || child->type == AST_BUILDER_FUNCTION) && child->node_type &&
            child->node_type->kind == TYPE_TUPLE) {
            merge_return_tuple_types(child, child->node_type);
        }
    }
    // Propagate merged return types to all function call sites in the program
    // (call node_types may have UNKNOWN elements from before the merge)
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if (!child) continue;
        // For each function with a tuple return type, update matching call sites
        if ((child->type == AST_FUNCTION_DEFINITION || child->type == AST_BUILDER_FUNCTION) && child->node_type &&
            child->node_type->kind == TYPE_TUPLE && child->value) {
            // Find all calls to this function in the program and update their node_type
            extern void propagate_tuple_type_to_calls(ASTNode* node, const char* func_name, Type* type);
            propagate_tuple_type_to_calls(program, child->value, child->node_type);
        }
        // Same propagation for tuple-returning externs — call sites
        // need to know they're consuming a `_tuple_T1_T2` struct so
        // the destructure (`a, b = extern_fn(...)`) emits correctly.
        if (child->type == AST_EXTERN_FUNCTION && child->node_type &&
            child->node_type->kind == TYPE_TUPLE && child->value) {
            extern void propagate_tuple_type_to_calls(ASTNode* node, const char* func_name, Type* type);
            propagate_tuple_type_to_calls(program, child->value, child->node_type);
        }
    }
    print_line(gen, "");

    // Pre-pass: identify builder functions (first param is _ctx: ptr).
    // These get builder_context() auto-injected at call sites inside
    // trailing blocks. We walk:
    //   1. program->children — locally defined functions (incl. those
    //      cloned in by module_merge_into_program) and any locally
    //      declared externs.
    //   2. for each AST_IMPORT_STATEMENT, the imported module's externs.
    //      Module externs aren't merged into program->children; they're
    //      emitted as C declarations during the import codegen pass and
    //      otherwise live only in the module registry. To recognize
    //      std.host's manifest builders (describe, input, event, etc.)
    //      as builder funcs, we walk those externs here too.
    //
    // Function params are AST_VARIABLE_DECLARATION / AST_PATTERN_VARIABLE;
    // extern params are AST_IDENTIFIER (different parser path) but carry
    // the same .value and .node_type info we need.

    // First, helper that registers a node if its first param is _ctx: ptr.
    // (Inlined as a lambda-style block to keep the pre-pass self-contained.)
    #define MAYBE_REGISTER_BUILDER(node) do { \
        ASTNode* _n = (node); \
        if (!_n || !_n->value) break; \
        int _is_func   = _n->type == AST_FUNCTION_DEFINITION; \
        int _is_extern = _n->type == AST_EXTERN_FUNCTION; \
        if (!_is_func && !_is_extern) break; \
        for (int _j = 0; _j < _n->child_count; _j++) { \
            ASTNode* _p = _n->children[_j]; \
            if (!_p) continue; \
            if (_p->type == AST_GUARD_CLAUSE || _p->type == AST_BLOCK) continue; \
            int _ok = _p->type == AST_PATTERN_VARIABLE \
                   || _p->type == AST_VARIABLE_DECLARATION \
                   || _p->type == AST_IDENTIFIER; \
            if (_ok && _p->value && strcmp(_p->value, "_ctx") == 0 && \
                _p->node_type && _p->node_type->kind == TYPE_PTR) { \
                if (gen->builder_func_count >= gen->builder_func_capacity) { \
                    gen->builder_func_capacity = gen->builder_func_capacity ? gen->builder_func_capacity * 2 : 16; \
                    gen->builder_funcs = aether_xrealloc(gen->builder_funcs, \
                        gen->builder_func_capacity * sizeof(char*)); \
                } \
                gen->builder_funcs[gen->builder_func_count++] = strdup(_n->value); \
            } \
            break; \
        } \
    } while (0)

    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if (!child) continue;
        if (child->type == AST_IMPORT_STATEMENT && child->value) {
            /* Walk the imported module's externs. */
            AetherModule* mod = module_find(child->value);
            if (mod && mod->ast) {
                for (int j = 0; j < mod->ast->child_count; j++) {
                    MAYBE_REGISTER_BUILDER(mod->ast->children[j]);
                }
            }
        } else {
            MAYBE_REGISTER_BUILDER(child);
        }
    }
    #undef MAYBE_REGISTER_BUILDER

    // Pre-pass: identify builder functions (marked with 'builder' keyword)
    // These get block-first execution: block fills config, then function runs with it
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if (!child || child->type != AST_BUILDER_FUNCTION || !child->value) continue;
        if (gen->builder_func_reg_count >= gen->builder_func_reg_capacity) {
            gen->builder_func_reg_capacity = gen->builder_func_reg_capacity ? gen->builder_func_reg_capacity * 2 : 16;
            gen->builder_funcs_reg = aether_xrealloc(gen->builder_funcs_reg,
                gen->builder_func_reg_capacity * sizeof(struct BuilderFuncEntry));
        }
        gen->builder_funcs_reg[gen->builder_func_reg_count].name = strdup(child->value);
        gen->builder_funcs_reg[gen->builder_func_reg_count].factory = child->annotation ? strdup(child->annotation) : NULL;
        gen->builder_func_reg_count++;
    }

    // Forward-declare struct types BEFORE function forward declarations,
    // so a function signature like `int header_flags(Header*)` resolves
    // even though the full `typedef struct Header { ... } Header;` is
    // emitted later (alongside actor / message bodies). Same shape as
    // the actor forward typedefs at line ~537. Without this, any
    // function whose param or return type is `*StructName` produces
    // `unknown type name 'StructName'` at the forward decl site.
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* sd = program->children[i];
        if (sd && sd->type == AST_STRUCT_DEFINITION && sd->value) {
            /* `extern struct ... @c_import` — the C header owns the
             * struct AND its typedef.  Suppress even the forward
             * `typedef struct Name Name;` so it can't collide with the
             * header's.  See redis-porting-language-gaps.md "P0:
             * Header-Defined C Struct Interop". */
            if (sd->annotation &&
                strcmp(sd->annotation, "extern_c_import") == 0) {
                /* Record this name so later emit sites can produce
                 * `struct Name*` instead of `Name*` when needed. */
                aether_register_c_import_struct(sd->value);
                continue;
            }
            fprintf(gen->output, "typedef struct %s %s;\n", sd->value, sd->value);
        }
        /* #891: register each @c_struct overlay (name + fields) so member
         * access can lower to width-correct mem_get_* / set_*. No C typedef is
         * emitted — the overlay is a pure-Aether lens, not a C struct. */
        if (sd && sd->type == AST_C_STRUCT_DEF && sd->value) {
            aether_register_c_struct(sd->value);
            for (int f = 0; f < sd->child_count; f++) {
                ASTNode* fld = sd->children[f];
                if (!fld || fld->type != AST_STRUCT_FIELD || !fld->value) continue;
                const char* nested = NULL;
                const char* width = c_struct_field_width(fld->node_type, &nested);
                aether_c_struct_add_field(sd->value, fld->value, width,
                                          (long)fld->bit_width, nested);
            }
        }
    }

    /* #1242 @c_verify: check every declared overlay offset against the C
     * header's real layout, at C compile time. A @c_struct offset is written by
     * hand (Aether never sees the header), so a field added upstream shifts the
     * layout and the overlay keeps reading the old offset: right type, wrong
     * bytes, no diagnostic anywhere. The width is asserted too, since declaring
     * `int` for a `long` field reads half of it and is equally silent.
     *
     * offsetof needs the C type to be complete here, which is why this is
     * opt-in: it only works when the owning header is in scope (`cflags =
     * "-include foo.h"`). Costs nothing at runtime, the assertions vanish after
     * the C compiler checks them. */
    {
        int any_verify = 0;
        for (int i = 0; i < program->child_count; i++) {
            ASTNode* c = program->children[i];
            if (c && c->type == AST_C_STRUCT_DEF && c->value &&
                annotation_has_marker(c->annotation, "c_verify")) { any_verify = 1; break; }
        }
        if (any_verify) {
            fprintf(gen->output, "#include <stddef.h>\n");
            for (int i = 0; i < program->child_count; i++) {
                ASTNode* sd = program->children[i];
                if (!sd || sd->type != AST_C_STRUCT_DEF || !sd->value) continue;
                if (!annotation_has_marker(sd->annotation, "c_verify")) continue;

                fprintf(gen->output,
                        "/* #1242 @c_verify %s: the C header owns the layout. */\n",
                        sd->value);
                for (int f = 0; f < sd->child_count; f++) {
                    ASTNode* fld = sd->children[f];
                    if (!fld || fld->type != AST_STRUCT_FIELD || !fld->value) continue;

                    fprintf(gen->output,
                        "_Static_assert(offsetof(%s, %s) == %ld,\n"
                        "    \"@c_struct %s: field '%s' is declared at offset %ld, "
                        "the C header puts it elsewhere\");\n",
                        sd->value, fld->value, (long)fld->bit_width,
                        sd->value, fld->value, (long)fld->bit_width);

                    /* Width: only for the field kinds whose byte size is exact.
                     * c_struct_field_width() folds anything unrecognised into
                     * "long", so asserting off its token would invent an
                     * 8-byte claim the author never made. */
                    const char* size_expr = NULL;
                    if (fld->node_type) {
                        switch (fld->node_type->kind) {
                            case TYPE_BYTE:
                            case TYPE_UINT8:  size_expr = "1"; break;
                            case TYPE_UINT16: size_expr = "2"; break;
                            case TYPE_INT:
                            case TYPE_UINT32: size_expr = "4"; break;
                            case TYPE_INT64:
                            case TYPE_UINT64: size_expr = "8"; break;
                            case TYPE_FLOAT:  size_expr = "sizeof(double)"; break;
                            case TYPE_PTR:    size_expr = "sizeof(void*)"; break;
                            default:          break;
                        }
                    }
                    if (size_expr) {
                        fprintf(gen->output,
                            "_Static_assert(sizeof(((%s*)0)->%s) == %s,\n"
                            "    \"@c_struct %s: field '%s' is declared %s bytes wide, "
                            "the C header disagrees\");\n",
                            sd->value, fld->value, size_expr,
                            sd->value, fld->value, size_expr);
                    }
                }
            }
        }
    }

    /* #891: if any @c_struct overlay exists, its field access lowers to
     * aether_mem_get_* and set_* runtime calls (linked from libaether.a). Emit
     * their prototypes here so the generated C compiles WITHOUT requiring the
     * user to `import std.mem` — the overlay IS the access surface. The
     * symbols are always linked; declaring them is a no-op if also pulled in
     * via std.mem. */
    {
        int any_overlay = 0;
        for (int i = 0; i < program->child_count; i++) {
            ASTNode* c = program->children[i];
            if (c && c->type == AST_C_STRUCT_DEF) { any_overlay = 1; break; }
        }
        if (any_overlay) {
            fprintf(gen->output,
                "/* #891 @c_struct overlay accessors (from libaether.a) */\n"
                "extern int     aether_mem_get_byte(void*, int);\n"
                "extern int     aether_mem_set_byte(void*, int, int);\n"
                "extern int     aether_mem_get_int16(void*, int);\n"
                "extern int     aether_mem_set_int16(void*, int, int);\n"
                "extern int     aether_mem_get_uint16(void*, int);\n"
                "extern int     aether_mem_set_uint16(void*, int, int);\n"
                "extern int     aether_mem_get_int(void*, int);\n"
                "extern int     aether_mem_set_int(void*, int, int);\n"
                "extern int     aether_mem_get_uint32(void*, int);\n"
                "extern int     aether_mem_set_uint32(void*, int, int);\n"
                "extern int64_t aether_mem_get_long(void*, int);\n"
                "extern int     aether_mem_set_long(void*, int, int64_t);\n"
                "extern double  aether_mem_get_float64(void*, int);\n"
                "extern int     aether_mem_set_float64(void*, int, double);\n"
                "extern void*   aether_mem_get_ptr(void*, int);\n"
                "extern int     aether_mem_set_ptr(void*, int, void*);\n");
        }
    }

    // Hoist FULL struct body emission to the top of the file (right
    // after forward typedefs), before any function bodies that might
    // do `view->field` member access against an imported or locally-
    // defined struct.  Without this hoist, an imported `extern struct`
    // whose AST_STRUCT_DEFINITION node is appended AFTER the function
    // definitions in the merged program tree would only have a
    // forward decl visible at the function-body emit site, producing
    // C errors like "invalid use of incomplete typedef".  Hoisting
    // here matches what a user would write in a hand-rolled .c file:
    // typedef + full body up top, function bodies below.
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* sd = program->children[i];
        if (sd && sd->type == AST_STRUCT_DEFINITION) {
            generate_struct_definition(gen, sd);
        }
    }

    // #914: sum/variant tagged-union typedefs. Emitted after all struct
    // bodies (each union member embeds a variant struct by value) and before
    // the function forward declarations that reference `Name` in signatures.
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* sd = program->children[i];
        if (sd && sd->type == AST_SUM_TYPE_DEF) {
            emit_sum_typedef(gen, sd);
        }
    }

    // #1044: first-class enum typedefs. Emitted here (with the other type
    // definitions, before function forward decls) so `Name` and its member
    // constants are in scope everywhere they are used.
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* ed = program->children[i];
        if (ed && ed->type == AST_ENUM_DEFINITION) {
            emit_enum_typedef(gen, ed);
        }
    }

    // #1132: bitstructs. Register the layout (so field access can lower to
    // shift/mask) and emit `typedef <backing> Name;`. There is deliberately no C
    // struct and no C bitfield here — the backing integer IS the value.
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* bd = program->children[i];
        if (!bd || bd->type != AST_BITSTRUCT_DEFINITION || !bd->value) continue;
        const char* backing = get_c_type(bd->node_type);
        aether_register_bitstruct(bd->value, backing);
        for (int f = 0; f < bd->child_count; f++) {
            ASTNode* fld = bd->children[f];
            if (!fld || fld->type != AST_BITSTRUCT_FIELD || !fld->value) continue;
            int is_bool = (fld->node_type && fld->node_type->kind == TYPE_BOOL);
            aether_bitstruct_add_field(bd->value, fld->value,
                                       fld->bit_lo, fld->bit_hi, is_bool);
        }
        fprintf(gen->output, "typedef %s %s;  /* bitstruct */\n", backing, bd->value);
    }
    if (g_bitstruct_count > 0) fprintf(gen->output, "\n");

    // Now that every user struct's full body is visible, emit the
    // synthesised tuple typedefs (deferred from the merge pre-scan
    // above). A tuple type embeds each element BY VALUE, so a
    // `(Point, string)` return needs `Point`'s definition first —
    // emitting the typedef here, after the struct bodies, fixes the
    // "unknown type name" errors (issue #634). Still before the
    // function forward declarations, which reference these typedefs.
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if (!child) continue;
        if ((child->type == AST_FUNCTION_DEFINITION || child->type == AST_BUILDER_FUNCTION) &&
            child->node_type && child->node_type->kind == TYPE_TUPLE) {
            ensure_tuple_typedef(gen, child->node_type);
        }
        // Externs with `-> (T1, T2, ...)` need the same typedef (#271).
        if (child->type == AST_EXTERN_FUNCTION && child->node_type &&
            child->node_type->kind == TYPE_TUPLE) {
            ensure_tuple_typedef(gen, child->node_type);
        }
        // Externs with tuple-typed PARAMETERS need theirs too (#1033) —
        // the prototype references the `_tuple_*` name for each by-value
        // struct param.
        if (child->type == AST_EXTERN_FUNCTION) {
            for (int j = 0; j < child->child_count; j++) {
                ASTNode* p = child->children[j];
                if (p && p->type == AST_IDENTIFIER && p->node_type &&
                    p->node_type->kind == TYPE_TUPLE) {
                    ensure_tuple_typedef(gen, p->node_type);
                }
            }
        }
        // Imported modules' externs with tuple returns — the import
        // pass forward-declares them and would otherwise reference an
        // undeclared `_tuple_T1_T2` typedef (#289).
        if (child->type == AST_IMPORT_STATEMENT && child->value) {
            AetherModule* mod_entry = module_find(child->value);
            ASTNode* mod_ast = mod_entry ? mod_entry->ast : NULL;
            if (mod_ast) {
                for (int j = 0; j < mod_ast->child_count; j++) {
                    ASTNode* decl = mod_ast->children[j];
                    if (!decl || decl->type != AST_EXTERN_FUNCTION) continue;
                    if (decl->node_type &&
                        decl->node_type->kind == TYPE_TUPLE) {
                        ensure_tuple_typedef(gen, decl->node_type);
                    }
                    // ... and their tuple-typed params (#1033).
                    for (int k = 0; k < decl->child_count; k++) {
                        ASTNode* p = decl->children[k];
                        if (p && p->type == AST_IDENTIFIER && p->node_type &&
                            p->node_type->kind == TYPE_TUPLE) {
                            ensure_tuple_typedef(gen, p->node_type);
                        }
                    }
                }
            }
        }
    }
    // #340: emit optional typedefs (after struct bodies, before fn fwd-decls).
    collect_optional_typedefs(gen, program);

    // Hoist top-level constants the same way.  Imported constants
    // (via `import mod` → cloned into the consumer's AST as
    // AST_CONST_DECLARATION with `is_imported=1`) land at the end of
    // the merged program tree, AFTER consumer function bodies that
    // may reference them.  Without this hoist, a `#define
    // mqtypes_JSOBJ_U_OFFSET (24)` produced from `const
    // JSOBJ_U_OFFSET = 24` in an imported module would appear too
    // late and the consumer's `(p + mqtypes.JSOBJ_U_OFFSET)`
    // expression would fail with `undeclared identifier`.  Same fix
    // shape as the struct hoist above.
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* cd = program->children[i];
        if (cd && cd->type == AST_CONST_DECLARATION &&
            cd->value && cd->child_count > 0) {
            codegen_note_diag_pos(cd);
            codegen_note_diag_func(NULL);
            if (cd->annotation && strcmp(cd->annotation, "array_const") == 0) {
                // static const T NAME[] = {v1, v2, ...};
                Type* elem_type = (cd->node_type && cd->node_type->element_type)
                                  ? cd->node_type->element_type : NULL;
                const char* ctype = elem_type ? const_array_elem_c_type(elem_type) : "const char*";
                fprintf(gen->output, "static const %s %s[] = ", ctype, cd->value);
                generate_expression(gen, cd->children[0]);
                fprintf(gen->output, ";\n");
            } else if (cd->annotation && strcmp(cd->annotation, "global_var") == 0) {
                // #701: mutable module-level global -> file-scope static.
                // A real lvalue (not a `#define`), so same-module functions
                // can read and write it as a plain C identifier. Record the
                // name so a bare `name = expr` inside a function body lowers
                // to a write to this static rather than a shadowing local.
                register_module_global_var(gen, cd->value);
                const char* ctype = get_c_type(cd->node_type);
                fprintf(gen->output, "static %s %s = ", ctype, cd->value);
                generate_expression(gen, cd->children[0]);
                fprintf(gen->output, ";\n");
            } else {
                /* Scoped C, not a #define: a macro has no scope, so a
                 * function parameter or local spelled like the const was
                 * textually rewritten into the literal (`int SCALE` became
                 * `int (99)`), breaking the documented shadowing guarantee
                 * (#1256, docs/module-system-design.md). A file-scope
                 * static const participates in C scoping, so inner
                 * declarations shadow it naturally. */
                const char* ctype = get_c_type(cd->node_type);
                /* get_c_type already says `const char*` for strings; avoid
                 * emitting a duplicate `const const`. */
                int already_const = strncmp(ctype, "const ", 6) == 0;
                fprintf(gen->output, "static %s%s %s = (",
                        already_const ? "" : "const ", ctype, cd->value);
                generate_expression(gen, cd->children[0]);
                fprintf(gen->output, ");\n");
            }
        }
    }

    /* #error-unification P3: fault members. Each mints an interned
     * `static const char <name>[] = "<ns>.<name>";` — a file-scope string
     * whose ADDRESS is stable within the TU (enabling a pointer fast-path)
     * and whose CONTENT is the qualified name (so cross-`.so` identity and
     * printing both work through the value itself). The member's C symbol is
     * `<name>` (already namespace-prefixed by the module-merge pass, exactly
     * like a const), so `fs.NotFound` — rewritten to that identifier —
     * evaluates to a `const char*` to the interned name. */
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* fd = program->children[i];
        if (!fd || fd->type != AST_FAULT_DEFINITION) continue;
        for (int mi = 0; mi < fd->child_count; mi++) {
            ASTNode* m = fd->children[mi];
            if (!m || !m->value || m->child_count == 0) continue;
            fprintf(gen->output, "static const char %s[] = ", m->value);
            generate_expression(gen, m->children[0]);   /* the string content */
            fprintf(gen->output, ";\n");
        }
    }

    // Generate forward declarations for all functions FIRST so that
    // hoisted closure functions can call them without implicit declarations.
    print_line(gen, "// Forward declarations");
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if (!child || (child->type != AST_FUNCTION_DEFINITION && child->type != AST_BUILDER_FUNCTION)) continue;
        if (!child->value) continue;
        codegen_note_diag_pos(child);
        codegen_note_diag_func(child->value);

        // Skip if already forward-declared (pattern matching generates combined functions)
        int already_declared = 0;
        for (int j = 0; j < i; j++) {
            ASTNode* prev = program->children[j];
            if (prev && (prev->type == AST_FUNCTION_DEFINITION || prev->type == AST_BUILDER_FUNCTION) &&
                prev->value && strcmp(prev->value, child->value) == 0) {
                already_declared = 1;
                break;
            }
        }
        if (already_declared) continue;

        // Imported functions are emitted as `static` in their definitions
        // (see generate_function_definition / generate_combined_function),
        // so the matching forward declaration must also be `static` —
        // otherwise C rejects the file with "static declaration follows
        // non-static declaration". `@c_callback` (#235) opts the function
        // out of `static` so it stays externally addressable; the forward
        // declaration follows suit. Trailing-underscore private helpers
        // (#279) match the same `static` rule.
        if (fn_has_internal_linkage(child)) {
            fprintf(gen->output, "static AETHER_MAYBE_UNUSED ");
        }

        // Determine return type. Mirrors generate_function_definition's
        // logic so the forward declaration and body always agree:
        //   - unannotated + has return-with-value → int (legacy default)
        //   - unannotated + no return-with-value  → void (issue #354)
        Type* ret_type = child->node_type;
        int func_has_return = has_return_value(child);
        int ret_unannotated = (!ret_type
                               || ret_type->kind == TYPE_VOID
                               || ret_type->kind == TYPE_UNKNOWN);
        if (ret_unannotated && func_has_return) {
            fprintf(gen->output, "int");
        } else if (ret_unannotated) {
            fprintf(gen->output, "void");
        } else {
            generate_type(gen, ret_type);
        }
        const char* cb_sym = c_callback_symbol(child);
        fprintf(gen->output, " %s(", cb_sym ? cb_sym : safe_c_name(child->value));

        // Generate parameter types
        int param_count = 0;
        for (int j = 0; j < child->child_count; j++) {
            ASTNode* param = child->children[j];
            if (param->type == AST_GUARD_CLAUSE || param->type == AST_BLOCK) continue;

            if (param->type == AST_PATTERN_LIST || param->type == AST_PATTERN_CONS) {
                if (param_count > 0) fprintf(gen->output, ", ");
                fprintf(gen->output, "int*, int");
                param_count++;
            } else if (param->type == AST_PATTERN_LITERAL ||
                       param->type == AST_PATTERN_VARIABLE ||
                       param->type == AST_PATTERN_STRUCT ||
                       param->type == AST_VARIABLE_DECLARATION) {
                if (param_count > 0) fprintf(gen->output, ", ");
                /* #750: fn-ptr param → abstract declarator `R (*)(T1,T2)`
                 * so the prototype matches the definition. */
                if (is_fnptr_type(param->node_type)) {
                    emit_fnptr_decl(gen, param->node_type, NULL);
                } else {
                    generate_type(gen, param->node_type);
                }
                param_count++;
            }
        }
        // Builder functions get hidden void* _builder as last parameter
        if (child->type == AST_BUILDER_FUNCTION) {
            if (param_count > 0) fprintf(gen->output, ", ");
            fprintf(gen->output, "void*");
            param_count++;
        }
        // C-variadic function: trailing `...` in the prototype, matching
        // the definition (see generate_function_definition).
        if (annotation_has_marker(child->annotation, "varargs")
            && param_count > 0) {
            fprintf(gen->output, ", ...");
        } else if (param_count == 0) {
            fprintf(gen->output, "void");
        }
        fprintf(gen->output, ");\n");
    }

    /* Forward typedef for each actor, so the BARE name is usable before the
     * actor's own `typedef struct X { ... } X;` is emitted.
     *
     * A closure that captures an actor handle gets an env slot typed from
     * lookup_var_c_type, which yields the bare "X" — but the closure env
     * typedef is emitted well before the actor struct, so the slot read as
     * an implicit int and the C compiler reported "initialization of 'X *'
     * from incompatible pointer type 'int *'". C allows repeating a
     * typedef with the same definition, so the later full definition is
     * unaffected. (#1626) */
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if (!child || child->type != AST_ACTOR_DEFINITION || !child->value) continue;
        fprintf(gen->output, "typedef struct %s %s;\n", child->value, child->value);
    }

    // Forward declarations for actor spawn functions (actors can spawn other actors
    // from within receive handlers, which appear before the spawn function definition)
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if (!child || child->type != AST_ACTOR_DEFINITION || !child->value) continue;
        fprintf(gen->output, "struct %s* spawn_%s(int preferred_core);\n", child->value, child->value);
    }
    print_line(gen, "");

    // Hoist extern declarations BEFORE closure emission so that closure
    // bodies which call imported externs (e.g. a `callback { argv =
    // list_new() }` block where list_new is `extern`'d in
    // contrib.aeocha) see a real prototype, not the C90 implicit-int
    // fallback. Without this hoist, closures emitted at line ~1830 would
    // appear in the C file before `void* list_new();` from the import
    // walk at line ~1881, and gcc/clang would reject the file with
    // "conflicting types" once the late prototype lands.
    //
    // Two sources walked: (1) locally declared `extern` (sibling of
    // function definitions in program->children), (2) externs reachable
    // through `import M` — for each import statement, walk the imported
    // module's AST and emit every extern. Same registry-lookup shape as
    // the (now removed) late emission inside the AST_IMPORT_STATEMENT
    // handler.
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if (!child) continue;
        if (child->type == AST_EXTERN_FUNCTION && child->value) {
            generate_extern_declaration(gen, child);
        } else if (child->type == AST_IMPORT_STATEMENT && child->value) {
            AetherModule* mod_entry = module_find(child->value);
            ASTNode* mod_ast = mod_entry ? mod_entry->ast : NULL;
            if (mod_ast) {
                for (int j = 0; j < mod_ast->child_count; j++) {
                    ASTNode* decl = mod_ast->children[j];
                    if (decl && decl->type == AST_EXTERN_FUNCTION && decl->value) {
                        generate_extern_declaration(gen, decl);
                    }
                }
            }
        }
    }
    print_line(gen, "");

    // Discover and emit closures AFTER forward declarations so hoisted
    // closure functions can call user-defined functions without
    // implicit function declaration errors (C99+).
    discover_closures(gen, program);
    // L4 validation: reject closures inside actor handlers that write
    // to actor state fields. aether_error_report increments the error
    // count; aetherc.c should check aether_error_count() after
    // generate_program returns and bail if non-zero.
    validate_closure_state_mutations(gen, program);

    /* Bare-fn → fn-typed-slot adapter discovery (ASK 3). Pre-walk the
     * program to register every bare-fn wrapped at a coercion site
     * (mirrors the lazy registration in the wrap codegen path). Run this
     * BEFORE closure bodies are emitted: a closure body can itself wrap a
     * bare fn (`runit(val)` inside a `callback { }`), and #943 showed the
     * emitted closure function then referenced an adapter that was only
     * defined later — undeclared in the closure's TU. Emitting the adapter
     * FORWARD DECLARATIONS before the closures fixes the ordering; the full
     * bodies still come after (they call the user fns by their real C name,
     * so they must follow the user fn definitions). */
    discover_bare_fn_adapters(gen);
    emit_bare_fn_adapter_decls(gen);

    if (gen->closure_count > 0) {
        print_line(gen, "// Closure declarations");
        emit_closure_declarations(gen);
    }

    emit_bare_fn_adapters(gen);

    // Pre-pass: build request->reply type map from actor receive handlers.
    // This lets the ? operator know the reply message type at codegen time.
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* actor = program->children[i];
        if (!actor || actor->type != AST_ACTOR_DEFINITION) continue;
        for (int j = 0; j < actor->child_count; j++) {
            ASTNode* recv = actor->children[j];
            if (!recv || recv->type != AST_RECEIVE_STATEMENT) continue;
            for (int k = 0; k < recv->child_count; k++) {
                ASTNode* arm = recv->children[k];
                if (!arm || arm->type != AST_RECEIVE_ARM || arm->child_count < 2) continue;
                ASTNode* pattern = arm->children[0];
                ASTNode* body = arm->children[1];
                if (!pattern || !pattern->value || !body) continue;
                const char* req_msg = pattern->value;
                /* One resolver, shared with the type checker, so the type of
                 * the ask EXPRESSION and the type of the variable receiving
                 * it cannot disagree. They did before #1537: this map knew a
                 * reply was a string and inference did not, so the value slot
                 * was declared int and the C compile failed. It searches the
                 * whole arm body, so a `reply` inside a branch still maps
                 * (#736). */
                AetherReplyShape shape;
                if (aether_resolve_reply(program, req_msg, &shape)) {
                    const char* reply_msg = shape.reply_msg;
                    ASTNode* reply_expr = shape.reply_expr;
                    if (gen->reply_type_count >= gen->reply_type_capacity) {
                        gen->reply_type_capacity = gen->reply_type_capacity ? gen->reply_type_capacity * 2 : 16;
                        gen->reply_type_map = aether_xrealloc(gen->reply_type_map,
                            gen->reply_type_capacity * sizeof(*gen->reply_type_map));
                    }
                    gen->reply_type_map[gen->reply_type_count].request_msg = strdup(req_msg);
                    gen->reply_type_map[gen->reply_type_count].reply_msg =
                        reply_msg ? strdup(reply_msg) : NULL;
                    gen->reply_type_map[gen->reply_type_count].scalar_kind =
                        reply_expr
                            ? (reply_expr->node_type ? (int)reply_expr->node_type->kind
                                                     : (int)TYPE_INT)
                            : (int)TYPE_UNKNOWN;
                    gen->reply_type_count++;
                }
            }
        }
    }

    // (reachable_funcs computed earlier, before the forward-decl loop.)

    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if (!child) continue;

        switch (child->type) {
            case AST_MODULE_DECLARATION:
                // Module declaration: just a comment in generated C
                print_line(gen, "// Module: %s", child->value ? child->value : "unnamed");
                print_line(gen, "");
                break;
            case AST_IMPORT_STATEMENT:
                // Import statement: extern declarations were already
                // hoisted to the forward-decl section above so closures
                // can see them. Here we only emit a marker comment for
                // C-output readability.
                if (child->value) {
                    const char* alias = NULL;
                    if (child->child_count > 0) {
                        ASTNode* last = child->children[child->child_count - 1];
                        if (last && last->type == AST_IDENTIFIER) {
                            alias = last->value;
                        }
                    }
                    if (alias) {
                        print_line(gen, "// Import: %s as %s", child->value, alias);
                    } else {
                        print_line(gen, "// Import: %s", child->value);
                    }
                }
                break;
            case AST_EXPORT_STATEMENT:
                // Export: just generate the item (exports are implicit in C)
                if (child->child_count > 0) {
                    ASTNode* exported = child->children[0];
                    print_line(gen, "// Exported:");
                    switch (exported->type) {
                        case AST_FUNCTION_DEFINITION:
                            // Handle exports like regular functions (pattern matching aware)
                            if (exported->value && !is_function_generated(gen, exported->value)) {
                                int clause_count = 0;
                                ASTNode** clauses = collect_function_clauses(program, exported->value, &clause_count);
                                if (clause_count > 1) {
                                    generate_combined_function(gen, clauses, clause_count);
                                } else {
                                    generate_function_definition(gen, exported);
                                }
                                mark_function_generated(gen, exported->value);
                                free(clauses);
                            }
                            break;
                        case AST_STRUCT_DEFINITION:
                            generate_struct_definition(gen, exported);
                            break;
                        case AST_ACTOR_DEFINITION:
                            generate_actor_definition(gen, exported);
                            break;
                        default:
                            break;
                    }
                }
                break;
            case AST_ACTOR_DEFINITION:
                generate_actor_definition(gen, child);
                // Emit to header if enabled
                if (gen->csrc_header_file) {
                    emit_actor_to_header(gen, child);
                }
                break;
            case AST_MESSAGE_DEFINITION:
                // Generate optimized message struct with field packing
                if (child && child->value) {
                    int field_count = 0;
                    for (int i = 0; i < child->child_count; i++) {
                        if (child->children[i] && child->children[i]->type == AST_MESSAGE_FIELD) {
                            field_count++;
                        }
                    }
                    
                    print_line(gen, "// Message: %s (%d fields)", child->value, field_count);
                    
                    // Align large messages to cache line
                    if (field_count > 4) {
                        print_line(gen, "#ifdef _MSC_VER");
                        print_line(gen, "__declspec(align(64))");
                        print_line(gen, "#endif");
                        print_line(gen, "typedef struct");
                        print_line(gen, "#if defined(__GNUC__) || defined(__clang__)");
                        print_line(gen, "__attribute__((aligned(64)))");
                        print_line(gen, "#endif");
                        print_line(gen, "%s {", child->value);
                    } else {
                        print_line(gen, "typedef struct %s {", child->value);
                    }
                    indent(gen);
                    print_line(gen, "int _message_id;");
                    
                    MessageFieldDef* first_field = NULL;
                    MessageFieldDef* last_field = NULL;
                    
                    // Detect single-int-field messages (inline payload_int path).
                    // Their int field must be intptr_t to match Message.payload_int width.
                    int int_field_count = 0, other_field_count = 0;
                    for (int i = 0; i < child->child_count; i++) {
                        ASTNode* f = child->children[i];
                        if (f && f->type == AST_MESSAGE_FIELD && f->node_type) {
                            if (f->node_type->kind == TYPE_INT) int_field_count++;
                            else other_field_count++;
                        }
                    }
                    int is_inline_msg = (int_field_count == 1 && other_field_count == 0);

                    // Pack int fields together first for better alignment
                    for (int i = 0; i < child->child_count; i++) {
                        ASTNode* field = child->children[i];
                        if (field && field->type == AST_MESSAGE_FIELD) {
                            if (field->node_type && (field->node_type->kind == TYPE_INT || field->node_type->kind == TYPE_BOOL)) {
                                print_indent(gen);
                                if (is_inline_msg && field->node_type->kind == TYPE_INT) {
                                    // intptr_t for inline-path field (matches payload_int width)
                                    fprintf(gen->output, "intptr_t");
                                } else {
                                    generate_type(gen, field->node_type);
                                }
                                fprintf(gen->output, " %s;\n", field->value);
                            }
                        }
                    }
                    
                    // Then pointer types
                    for (int i = 0; i < child->child_count; i++) {
                        ASTNode* field = child->children[i];
                        if (field && field->type == AST_MESSAGE_FIELD) {
                            if (field->node_type && (field->node_type->kind == TYPE_ACTOR_REF || field->node_type->kind == TYPE_STRING || field->node_type->kind == TYPE_PTR)) {
                                print_indent(gen);
                                generate_type(gen, field->node_type);
                                fprintf(gen->output, " %s;\n", field->value);
                            }
                        }
                    }
                    
                    // Finally other types
                    for (int i = 0; i < child->child_count; i++) {
                        ASTNode* field = child->children[i];
                        if (field && field->type == AST_MESSAGE_FIELD) {
                            if (field->node_type && field->node_type->kind != TYPE_INT && field->node_type->kind != TYPE_BOOL &&
                                field->node_type->kind != TYPE_ACTOR_REF && field->node_type->kind != TYPE_STRING && field->node_type->kind != TYPE_PTR) {
                                print_indent(gen);
                                generate_type(gen, field->node_type);
                                fprintf(gen->output, " %s;\n", field->value);
                            }
                        }
                    }
                    
                    // Build field list for registry. We store the resolved
                    // C type for each field so downstream codegen (receive
                    // destructuring, struct-literal send-side) can emit the
                    // correct type without re-deriving it from type_kind,
                    // which loses information for composite types like
                    // `string[]` (element type) and structs (struct name).
                    for (int i = 0; i < child->child_count; i++) {
                        ASTNode* field = child->children[i];
                        if (field && field->type == AST_MESSAGE_FIELD) {
                            MessageFieldDef* field_def = (MessageFieldDef*)malloc(sizeof(MessageFieldDef));
                            field_def->name = strdup(field->value);
                            field_def->type_kind = field->node_type ? field->node_type->kind : TYPE_UNKNOWN;
                            field_def->c_type = NULL;
                            field_def->element_c_type = NULL;
                            if (field->node_type) {
                                const char* resolved = get_c_type(field->node_type);
                                if (resolved) {
                                    field_def->c_type = strdup(resolved);
                                }
                                if (field->node_type->kind == TYPE_ARRAY &&
                                    field->node_type->element_type) {
                                    const char* elem = get_c_type(field->node_type->element_type);
                                    if (elem) {
                                        field_def->element_c_type = strdup(elem);
                                    }
                                }
                            }
                            field_def->next = NULL;

                            if (!first_field) {
                                first_field = field_def;
                                last_field = field_def;
                            } else {
                                last_field->next = field_def;
                                last_field = field_def;
                            }
                        }
                    }
                    unindent(gen);
                    print_line(gen, "} %s;", child->value);
                    print_line(gen, "");

                    /* Per-message heap-string-field release function
                     * (#466). Walked by the receive-handler dispatch
                     * before `aether_free_message` so the deep-
                     * copied AetherString allocations the sender
                     * stamped in (codegen_expr.c send-side) are
                     * reclaimed when the recipient is done with the
                     * message. No-op for messages with no string
                     * fields; the codegen_actor.c dispatch skips
                     * the call in that case via
                     * `message_has_string_field`. */
                    int msg_has_string_field = 0;
                    for (int fi = 0; fi < child->child_count; fi++) {
                        ASTNode* f = child->children[fi];
                        if (f && f->type == AST_MESSAGE_FIELD &&
                            f->node_type && f->node_type->kind == TYPE_STRING) {
                            msg_has_string_field = 1;
                            break;
                        }
                    }
                    if (msg_has_string_field) {
                        /* Forward-declare `string_release` here so the
                         * generated release_fields function compiles
                         * even on tests that don't `import std.string`
                         * (and therefore don't get the extern
                         * registry's prototype). The duplicate-but-
                         * matching declaration is tolerated by C —
                         * matches the registry's
                         * `void string_release(const char*)` exactly
                         * when std.string IS imported. */
                        print_line(gen, "extern void string_release(const char*);");
                        print_line(gen, "static inline void %s_release_fields(%s* m) {",
                                   child->value, child->value);
                        indent(gen);
                        print_line(gen, "if (!m) return;");
                        for (int fi = 0; fi < child->child_count; fi++) {
                            ASTNode* f = child->children[fi];
                            if (f && f->type == AST_MESSAGE_FIELD &&
                                f->node_type && f->node_type->kind == TYPE_STRING) {
                                print_line(gen, "if (m->%s) { string_release(m->%s); m->%s = (const char*)0; }",
                                           f->value, f->value, f->value);
                            }
                        }
                        unindent(gen);
                        print_line(gen, "}");
                        print_line(gen, "");
                    }

                    // Generate type-specific memory pool for this message type
                    print_line(gen, "// Type-specific memory pool for %s", child->value);
                    print_line(gen, "// DECLARE_TYPE_POOL(%s)", child->value);
                    print_line(gen, "// DECLARE_TLS_POOL(%s)", child->value);
                    print_line(gen, "");

                    register_message_type(gen->message_registry, child->value, first_field);

                    // Emit to header if enabled
                    if (gen->csrc_header_file) {
                        emit_message_to_header(gen, child);
                    }
                }
                break;
            case AST_BUILDER_FUNCTION:
            case AST_FUNCTION_DEFINITION:
                // Check if this function was already generated (handles pattern matching clauses)
                if (child->value && !is_function_generated(gen, child->value)) {
                    int clause_count = 0;
                    ASTNode** clauses = collect_function_clauses(program, child->value, &clause_count);

                    if (clause_count > 1) {
                        // Multiple clauses - generate combined function
                        generate_combined_function(gen, clauses, clause_count);
                    } else {
                        // Single clause - use standard generation
                        generate_function_definition(gen, child);
                    }

                    mark_function_generated(gen, child->value);
                    free(clauses);
                }
                break;
            case AST_STRUCT_DEFINITION:
                // Already emitted in the hoisted struct-body pass above
                // (before function forward decls), so function bodies
                // that do `view->field` member access see the full
                // struct layout regardless of where the struct decl
                // appeared in the merged program tree.
                break;
            case AST_SUM_TYPE_DEF:
                // #914: the tagged-union typedef was emitted in the hoisted
                // type pass above; nothing to emit for the declaration here.
                break;
            case AST_MAIN_FUNCTION:
                generate_main_function(gen, child);
                break;
            case AST_EXTERN_FUNCTION:
                // Already emitted in the forward-decl hoist pass above —
                // closures need extern prototypes visible before their
                // bodies, so all extern decls are hoisted there.
                break;
            case AST_CONST_DECLARATION:
                // Already emitted in the hoisted const-#define pass above.
                // Hoist matches the struct-definition hoist so imported
                // constants are visible to consumer function bodies
                // regardless of where the AST node appears in the
                // merged program tree.
                break;
            default:
                break;
        }
    }

    /* Closure BODIES, after the loop above has emitted the message
     * definitions (and so registered them with gen->message_registry).
     * A body containing `a ? Msg {}` needs that registry populated; the
     * forward declarations went out early, before this loop. (#1626) */
    if (gen->closure_count > 0) {
        print_line(gen, "// Closure definitions");
        emit_closure_definitions(gen);
    }

    // --emit=lib / --emit=both: append aether_<name> alias stubs that form
    // the public FFI surface. Must come after all normal function emission
    // so the aliases see the wrapped functions via their forward decls.
    emit_lib_alias_stubs(gen, program);

    // --emit=lib / --emit=both: append the symbol-catalog metadata
    // (issue #403). Auto-included on every library build so consumers
    // get function-name → C-symbol → signature → source-location
    // introspection by dlsym'ing `aether_lib_meta`. The CLI tool
    // `ae lib-info <path>` walks the same struct for human-readable
    // dumps. Must come after emit_lib_alias_stubs because the
    // metadata's `c_symbol` field references the alias names.
    emit_lib_metadata(gen, program);

    // --emit-main=<func> shim: with --emit=lib, append a thin main(argc,argv)
    // that calls the named function. Closes the exe/lib symmetry — one .c
    // ships as both a loadable library and a binary. Issue #268.3.
    emit_main_shim_for_target(gen, program);

    // Close header file if emitting
    if (gen->emit_header && gen->header_file) {
        emit_header_epilogue(gen);
    }
}
