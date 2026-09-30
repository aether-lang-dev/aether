/* derive.c — `@derive(...)` synthesizer pass (#338).
 *
 * v1 scope: `@derive(eq)` for structs whose every field is a
 * primitive (int, long, byte, bool, float) or a `string`. The
 * synthesizer walks each annotated struct and inserts a sibling
 * `T_eq(T a, T b) -> int` function into the program AST that
 * returns a left-folded `&&` chain over per-field equality
 * comparisons.
 *
 * Per-field op selection:
 *   primitive  → `(a.f == b.f)`
 *   string     → `(string_equals(a.f, b.f) == 1)`
 *
 * Out of v1 scope (each surfaces a clear compile-time error so
 * users hit a deliberate diagnostic, not a silent miss):
 *   - `format` / `clone` / `hash` derives — emit "not yet
 *     supported in v1" diagnostic; the eq pattern is the
 *     expansion seam for follow-ups.
 *   - Nested struct fields (would need topological sort over
 *     inter-struct dependencies + recursion to `<F>_eq`).
 *   - Array / list / map fields.
 *
 * Synthesized functions are static-equivalent in scope (no
 * `@export`); users wanting public derives wrap them or write
 * `@export` themselves in a follow-up commit's surface.
 *
 * `@derive(schema)` (#2298) synthesizes `T_schema() -> ptr`, whose body is
 * the internal AST_SCHEMA_OF node: codegen emits T's field table as static
 * constant data (runtime/aether_schema.h) and lowers the node to its
 * address. The pass checks what the table carries: the `@attr(...)`
 * attributes written after a field's type are accepted only on a struct
 * that derives schema (anywhere else they would be silently dropped), and a
 * field names each attribute once. It leaves a `schema` marker on the
 * struct for codegen.
 */

#include "derive.h"
#include "typechecker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bit flags for the derive list. Future commits add format/clone/
 * hash flags + their generators; today only DERIVE_EQ is honoured. */
#define DERIVE_EQ      0x01
#define DERIVE_FORMAT  0x02
#define DERIVE_CLONE   0x04
#define DERIVE_HASH    0x08
#define DERIVE_SCHEMA  0x10

static int parse_derive_list(const char* annotation,
                              int* out_flags,
                              char* unsupported_buf,
                              size_t unsupported_cap) {
    *out_flags = 0;
    if (unsupported_cap > 0) unsupported_buf[0] = '\0';
    if (!annotation || strncmp(annotation, "derive:", 7) != 0) return 0;
    const char* p = annotation + 7;
    /* The list runs to the end of the string or to the `;` that starts the
     * next annotation marker (`derive:eq;observable`). */
    while (*p && *p != ';') {
        const char* start = p;
        while (*p && *p != ',' && *p != ';') p++;
        size_t len = (size_t)(p - start);
        if (*p == ',') p++;
        if (len == 0) continue;
        if (len == 2 && memcmp(start, "eq", 2) == 0) {
            *out_flags |= DERIVE_EQ;
        } else if (len == 6 && memcmp(start, "format", 6) == 0) {
            *out_flags |= DERIVE_FORMAT;
        } else if (len == 5 && memcmp(start, "clone", 5) == 0) {
            *out_flags |= DERIVE_CLONE;
        } else if (len == 4 && memcmp(start, "hash", 4) == 0) {
            *out_flags |= DERIVE_HASH;
        } else if (len == 6 && memcmp(start, "schema", 6) == 0) {
            *out_flags |= DERIVE_SCHEMA;
        } else {
            /* Record the first unsupported derive name for the
             * diagnostic. Truncate gracefully on overflow. */
            if (unsupported_cap > 0 && unsupported_buf[0] == '\0') {
                size_t copy = len < unsupported_cap - 1 ? len : unsupported_cap - 1;
                memcpy(unsupported_buf, start, copy);
                unsupported_buf[copy] = '\0';
            }
            return -1;
        }
    }
    return 0;
}

/* Identify the field's lowering category for code-gen of T_eq. */
typedef enum {
    FIELD_PRIMITIVE_NUMERIC,  /* int, long, byte, bool, float */
    FIELD_STRING,             /* string */
    FIELD_UNSUPPORTED         /* struct, array, list, map, ptr */
} FieldKind;

static FieldKind classify_field(Type* t) {
    if (!t) return FIELD_UNSUPPORTED;
    switch (t->kind) {
        case TYPE_INT:
        case TYPE_INT64:
        case TYPE_FLOAT:
        case TYPE_LONGDOUBLE:
        case TYPE_BYTE:
        case TYPE_BOOL:
            return FIELD_PRIMITIVE_NUMERIC;
        case TYPE_STRING:
            return FIELD_STRING;
        default:
            return FIELD_UNSUPPORTED;
    }
}

/* Build `<receiver>.<field_name>`. */
static ASTNode* mk_field_access(const char* receiver_name,
                                const char* field_name,
                                int line, int column) {
    ASTNode* recv = create_ast_node(AST_IDENTIFIER, receiver_name, line, column);
    ASTNode* access = create_ast_node(AST_MEMBER_ACCESS, field_name, line, column);
    add_child(access, recv);
    return access;
}

/* Build `(a.f == b.f)`. The existing codegen lowers `==` on
 * TYPE_STRING children to `strcmp(...) == 0` automatically — no
 * special-case dispatch needed at the synthesis layer. Numeric
 * primitives use C `==` directly. */
static ASTNode* mk_field_eq(ASTNode* field,
                             const char* a_name, const char* b_name,
                             int line, int column,
                             int* ok_out) {
    *ok_out = 1;
    if (classify_field(field->node_type) == FIELD_UNSUPPORTED) {
        *ok_out = 0;
        return NULL;
    }
    ASTNode* la = mk_field_access(a_name, field->value, line, column);
    ASTNode* lb = mk_field_access(b_name, field->value, line, column);
    /* Stamp the operand types so codegen's string-vs-string check
     * sees TYPE_STRING and routes through strcmp. */
    if (field->node_type) {
        la->node_type = clone_type(field->node_type);
        lb->node_type = clone_type(field->node_type);
    }
    ASTNode* eq = create_ast_node(AST_BINARY_EXPRESSION, "==", line, column);
    add_child(eq, la);
    add_child(eq, lb);
    return eq;
}

/* Synthesize `int T_eq(T a, T b) { return field-chain; }`.
 * Returns NULL if any field has an unsupported type — the caller
 * surfaces the diagnostic. */
static ASTNode* synth_eq(ASTNode* struct_def) {
    if (!struct_def || !struct_def->value) return NULL;
    int line = struct_def->line;
    int column = struct_def->column;
    const char* type_name = struct_def->value;

    /* Fold field equality checks. Empty structs are trivially equal. */
    ASTNode* chain = NULL;
    int ok_overall = 1;
    for (int i = 0; i < struct_def->child_count; i++) {
        ASTNode* field = struct_def->children[i];
        if (!field || field->type != AST_STRUCT_FIELD) continue;
        int ok = 0;
        ASTNode* fcmp = mk_field_eq(field, "a", "b", line, column, &ok);
        if (!ok) {
            ok_overall = 0;
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "@derive(eq) on '%s': field '%s' has an unsupported type. "
                     "v1 supports primitive numeric and string fields only.",
                     type_name, field->value ? field->value : "?");
            type_error(msg, line, column);
            if (fcmp) {
                /* Drop the stray subtree we may have allocated before
                 * detecting the error. */
                /* No public free; orphan node leaks until program teardown.
                 * Acceptable for a compile-time error path. */
            }
            continue;
        }
        if (!chain) {
            chain = fcmp;
        } else {
            ASTNode* and_node = create_ast_node(AST_BINARY_EXPRESSION, "&&", line, column);
            add_child(and_node, chain);
            add_child(and_node, fcmp);
            chain = and_node;
        }
    }
    if (!ok_overall) return NULL;

    /* Empty-struct case: `return 1;` (always equal). */
    if (!chain) {
        chain = create_ast_node(AST_LITERAL, "1", line, column);
        chain->node_type = create_type(TYPE_INT);
    }

    /* Build the function definition. Function name: T_eq.
     * Precision spec caps the type-name component at 250 bytes
     * regardless of source length — keeps -Werror=format-truncation
     * builds happy. Identifiers above 250 bytes don't occur in
     * practice. */
    char fname[256];
    snprintf(fname, sizeof(fname), "%.250s_eq", type_name);
    ASTNode* func = create_ast_node(AST_FUNCTION_DEFINITION, fname, line, column);
    func->node_type = create_type(TYPE_INT);

    /* Parameters: a: T, b: T.
     * Type kind: TYPE_STRUCT with name = type_name. */
    for (int i = 0; i < 2; i++) {
        const char* pname = (i == 0) ? "a" : "b";
        ASTNode* param = create_ast_node(AST_VARIABLE_DECLARATION, pname, line, column);
        Type* t = create_type(TYPE_STRUCT);
        if (t->struct_name) free(t->struct_name);
        t->struct_name = strdup(type_name);
        param->node_type = t;
        add_child(func, param);
    }

    /* Body: { return chain; } */
    ASTNode* body = create_ast_node(AST_BLOCK, NULL, line, column);
    ASTNode* ret = create_ast_node(AST_RETURN_STATEMENT, NULL, line, column);
    add_child(ret, chain);
    add_child(body, ret);
    add_child(func, body);

    return func;
}

/* The number of `@attr` children on a struct field. */
static int field_attribute_count(const ASTNode* field) {
    int n = 0;
    for (int i = 0; i < field->child_count; i++) {
        if (field->children[i] && field->children[i]->type == AST_FIELD_ATTRIBUTE) n++;
    }
    return n;
}

/* Report `@attr` written on a field of a struct that does not derive
 * schema: nothing would carry it. Returns the number of errors. */
static int check_attributes_have_a_schema(ASTNode* struct_def) {
    int errors = 0;
    for (int i = 0; i < struct_def->child_count; i++) {
        ASTNode* field = struct_def->children[i];
        if (!field || field->type != AST_STRUCT_FIELD) continue;
        for (int a = 0; a < field->child_count; a++) {
            ASTNode* attr = field->children[a];
            if (!attr || attr->type != AST_FIELD_ATTRIBUTE) continue;
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "field attribute `@%.100s` on '%.100s.%.100s' needs `@derive(schema)` on "
                     "the struct: the schema table is what carries it",
                     attr->value ? attr->value : "?",
                     struct_def->value ? struct_def->value : "?",
                     field->value ? field->value : "?");
            type_error(msg, attr->line, attr->column);
            errors++;
            break;   /* one report per field */
        }
    }
    return errors;
}

/* Synthesize `T_schema() -> ptr { return <AST_SCHEMA_OF T>; }` after
 * checking the fields' attributes. Returns NULL after reporting an error. */
static ASTNode* synth_schema(ASTNode* struct_def) {
    if (!struct_def || !struct_def->value) return NULL;
    int line = struct_def->line;
    int column = struct_def->column;
    const char* type_name = struct_def->value;
    int ok = 1;

    for (int i = 0; i < struct_def->child_count; i++) {
        ASTNode* field = struct_def->children[i];
        if (!field || field->type != AST_STRUCT_FIELD) continue;
        if (field_attribute_count(field) < 2) continue;
        for (int a = 0; a < field->child_count; a++) {
            ASTNode* x = field->children[a];
            if (!x || x->type != AST_FIELD_ATTRIBUTE || !x->value) continue;
            for (int b = a + 1; b < field->child_count; b++) {
                ASTNode* y = field->children[b];
                if (!y || y->type != AST_FIELD_ATTRIBUTE || !y->value) continue;
                if (strcmp(x->value, y->value) != 0) continue;
                char msg[512];
                snprintf(msg, sizeof(msg),
                         "field '%.120s.%.120s' has `@%.120s` twice; a field names each "
                         "attribute once",
                         type_name, field->value ? field->value : "?", x->value);
                type_error(msg, y->line, y->column);
                ok = 0;
            }
        }
    }
    if (!ok) return NULL;

    char fname[256];
    snprintf(fname, sizeof(fname), "%.240s_schema", type_name);
    ASTNode* func = create_ast_node(AST_FUNCTION_DEFINITION, fname, line, column);
    func->node_type = create_type(TYPE_PTR);

    ASTNode* ref = create_ast_node(AST_SCHEMA_OF, type_name, line, column);
    ref->node_type = create_type(TYPE_PTR);
    ASTNode* body = create_ast_node(AST_BLOCK, NULL, line, column);
    ASTNode* ret = create_ast_node(AST_RETURN_STATEMENT, NULL, line, column);
    add_child(ret, ref);
    add_child(body, ret);
    add_child(func, body);
    return func;
}

int derive_synthesize_pass(ASTNode* program) {
    if (!program || program->type != AST_PROGRAM) return 0;
    int errors = 0;

    /* Two-pass: collect targets first (so we don't iterate while
     * appending), then synthesize. */
    typedef struct { ASTNode* sd; int flags; } DeriveTask;
    int task_count = 0;
    int task_cap = 0;
    DeriveTask* tasks = NULL;

    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if (!child || child->type != AST_STRUCT_DEFINITION) continue;
        if (!child->annotation || strncmp(child->annotation, "derive:", 7) != 0) {
            errors += check_attributes_have_a_schema(child);
            continue;
        }
        int flags = 0;
        char unsupported[64];
        if (parse_derive_list(child->annotation, &flags, unsupported, sizeof(unsupported)) < 0) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "@derive: unknown derive '%s' (supported: eq, schema)",
                     unsupported);
            type_error(msg, child->line, child->column);
            errors++;
            continue;
        }
        if (!(flags & DERIVE_SCHEMA)) errors += check_attributes_have_a_schema(child);
        if ((flags & ~(DERIVE_EQ | DERIVE_SCHEMA)) != 0) {
            /* Format / clone / hash flags asked but not yet
             * implemented. Surface a precise diagnostic so users
             * don't get silent omission. */
            const char* unsup = (flags & DERIVE_FORMAT) ? "format"
                              : (flags & DERIVE_CLONE)  ? "clone"
                              : (flags & DERIVE_HASH)   ? "hash"
                              : "<unknown>";
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "@derive(%s): not yet supported (eq and schema are; "
                     "format/clone/hash land in follow-up commits)",
                     unsup);
            type_error(msg, child->line, child->column);
            errors++;
            continue;
        }
        if (flags == 0) continue;

        if (task_count >= task_cap) {
            task_cap = task_cap == 0 ? 8 : task_cap * 2;
            DeriveTask* nt = (DeriveTask*)realloc(tasks, (size_t)task_cap * sizeof(DeriveTask));
            if (!nt) { free(tasks); return -1; }
            tasks = nt;
        }
        tasks[task_count].sd = child;
        tasks[task_count].flags = flags;
        task_count++;
    }

    /* Synthesize per task. Append synthesized functions to the
     * program's children — the existing children-append helper
     * (add_child) handles realloc for us. */
    for (int i = 0; i < task_count; i++) {
        DeriveTask t = tasks[i];
        if (t.flags & DERIVE_EQ) {
            ASTNode* fn = synth_eq(t.sd);
            if (!fn) {
                errors++;
                continue;
            }
            add_child(program, fn);
        }
        if (t.flags & DERIVE_SCHEMA) {
            ASTNode* fn = synth_schema(t.sd);
            if (!fn) {
                errors++;
                continue;
            }
            add_child(program, fn);
        }
        /* Drop the `derive:` marker so re-running the pass is a no-op. The
         * markers after it (`observable`, #2220) belong to later passes and
         * stay; `schema` is added for codegen, which emits the table. */
        char* rest = strchr(t.sd->annotation, ';');
        char* kept = rest ? strdup(rest + 1) : NULL;
        free(t.sd->annotation);
        t.sd->annotation = kept;
        if (t.flags & DERIVE_SCHEMA)
            t.sd->annotation = annotation_add_marker(t.sd->annotation, "schema");
    }

    free(tasks);
    return errors == 0 ? 0 : -1;
}
