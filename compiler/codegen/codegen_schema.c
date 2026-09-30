/* codegen_schema.c — the field tables `@derive(schema)` asks for (#2298).
 *
 * For each struct the derive pass marked `schema`, and for every Aether
 * struct reachable from one through a field (by value, as an array element,
 * or through a `*T` pointer), this emits one static constant AetherSchema
 * (runtime/aether_schema.h) and a static getter returning its address.
 * `T_schema()`'s body is an AST_SCHEMA_OF node that lowers to a call of the
 * getter. Offsets and sizes are C's own offsetof/sizeof over the struct the
 * compiler emitted, so the table cannot drift from the layout.
 *
 * The getters are declared before any table is defined and a field names a
 * nested table by its getter, so tables may refer to each other in any
 * order, a struct that points at itself included. */
#include "codegen_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An Aether struct with a C body the compiler emitted: the only kind whose
 * layout offsetof can describe. Extern, opaque and header-imported structs
 * are left out (a field of one of those types has no nested table). */
static ASTNode* schema_struct_def(CodeGenerator* gen, const char* name) {
    if (!name) return NULL;
    ASTNode* sd = find_struct_definition_by_name(gen->program, name);
    if (!sd || sd->type != AST_STRUCT_DEFINITION || !sd->value) return NULL;
    if (sd->annotation && strncmp(sd->annotation, "extern", 6) == 0) return NULL;
    return sd;
}

/* The struct a field of type `t` leads to: `T`, `*T`, or an array of
 * either. NULL for anything else. */
static const char* schema_target_struct(const Type* t) {
    while (t && t->kind == TYPE_ARRAY) t = t->element_type;
    if (!t) return NULL;
    if (t->kind == TYPE_STRUCT) return t->struct_name;
    if (t->kind == TYPE_PTR && t->element_type && t->element_type->kind == TYPE_STRUCT)
        return t->element_type->struct_name;
    return NULL;
}

typedef struct {
    ASTNode** defs;
    int count;
    int cap;
} SchemaSet;

static int schema_set_has(const SchemaSet* s, const ASTNode* sd) {
    for (int i = 0; i < s->count; i++) if (s->defs[i] == sd) return 1;
    return 0;
}

static void schema_set_collect(CodeGenerator* gen, SchemaSet* s, ASTNode* sd) {
    if (!sd || schema_set_has(s, sd)) return;
    if (s->count == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->defs = (ASTNode**)aether_xrealloc(s->defs, (size_t)s->cap * sizeof(ASTNode*));
    }
    s->defs[s->count++] = sd;
    for (int i = 0; i < sd->child_count; i++) {
        ASTNode* f = sd->children[i];
        if (!f || f->type != AST_STRUCT_FIELD) continue;
        schema_set_collect(gen, s, schema_struct_def(gen, schema_target_struct(f->node_type)));
    }
}

/* Matches the AETHER_SCHEMA_* kinds in runtime/aether_schema.h. */
static int schema_kind(const Type* t) {
    if (!t) return 0;
    switch (t->kind) {
        case TYPE_BOOL:       return 1;
        case TYPE_BYTE:       return 2;
        case TYPE_INT:        return 3;
        case TYPE_INT64:      return 4;
        case TYPE_UINT8:      return 5;
        case TYPE_UINT16:     return 6;
        case TYPE_UINT32:     return 7;
        case TYPE_UINT64:     return 8;
        case TYPE_FLOAT:      return 9;
        case TYPE_FLOAT32:    return 10;
        case TYPE_LONGDOUBLE: return 11;
        case TYPE_DURATION:   return 12;
        case TYPE_STRING:     return 13;
        case TYPE_PTR:        return 14;
        case TYPE_STRUCT:     return 15;
        case TYPE_ARRAY:      return t->array_size > 0 ? 16 : 17;
        case TYPE_ENUM:       return 18;
        case TYPE_FUNCTION:   return 19;
        default:              return 0;
    }
}

/* Append `t`'s source spelling to buf[*len..cap). */
static void schema_spell(const Type* t, char* buf, size_t cap, size_t* len) {
#define SPELL(...) do { \
        if (*len < cap) { \
            int _n = snprintf(buf + *len, cap - *len, __VA_ARGS__); \
            if (_n > 0) *len += (size_t)_n < cap - *len ? (size_t)_n : cap - *len - 1; \
        } \
    } while (0)
    if (!t) { SPELL("?"); return; }
    if (t->distinct_name) { SPELL("%s", t->distinct_name); return; }
    if (t->c_alias) { SPELL("%s", t->c_alias); return; }
    switch (t->kind) {
        case TYPE_INT:        SPELL("int"); break;
        case TYPE_INT64:      SPELL("long"); break;
        case TYPE_UINT64:     SPELL("uint64"); break;
        case TYPE_UINT32:     SPELL("uint32"); break;
        case TYPE_UINT16:     SPELL("uint16"); break;
        case TYPE_UINT8:      SPELL("uint8"); break;
        case TYPE_DURATION:   SPELL("Duration"); break;
        case TYPE_FLOAT:      SPELL("float"); break;
        case TYPE_FLOAT32:    SPELL("f32"); break;
        case TYPE_LONGDOUBLE: SPELL("longdouble"); break;
        case TYPE_BOOL:       SPELL("bool"); break;
        case TYPE_BYTE:       SPELL("byte"); break;
        case TYPE_STRING:     SPELL("string"); break;
        case TYPE_FUNCTION:   SPELL("fn"); break;
        case TYPE_PTR:
            if (t->element_type) { SPELL("*"); schema_spell(t->element_type, buf, cap, len); }
            else SPELL("ptr");
            break;
        case TYPE_ARRAY:
            if (t->index_enum_name) {
                SPELL("[%s]", t->index_enum_name);
                schema_spell(t->element_type, buf, cap, len);
            } else {
                schema_spell(t->element_type, buf, cap, len);
                if (t->array_size > 0) SPELL("[%d]", t->array_size);
                else SPELL("[]");
            }
            break;
        case TYPE_OPTIONAL:
            schema_spell(t->element_type, buf, cap, len);
            SPELL("?");
            break;
        case TYPE_STRUCT:
        case TYPE_ENUM:
        case TYPE_SUM:
            SPELL("%s", t->struct_name ? t->struct_name : "?");
            break;
        default:
            SPELL("%s", type_to_string((Type*)t));
            break;
    }
#undef SPELL
}

/* A C string literal of the identifier-or-type text `s` (no characters in
 * it need escaping: identifiers, `*`, `[`, `]`, digits, `?`). */
static void schema_emit_name(CodeGenerator* gen, const char* s) {
    fputc('"', gen->output);
    for (const char* c = s ? s : ""; *c; c++) {
        if (*c == '"' || *c == '\\') fputc('\\', gen->output);
        fputc(*c, gen->output);
    }
    fputc('"', gen->output);
}

/* One attribute argument initializer: {kind, text, i, f}. */
static void schema_emit_arg(CodeGenerator* gen, ASTNode* lit) {
    const char* v = lit->value ? lit->value : "";
    int k = lit->node_type ? lit->node_type->kind : TYPE_UNKNOWN;
    if (k == TYPE_STRING) {
        fprintf(gen->output, "{3, ");
        generate_expression(gen, lit);   /* the literal lowering, escapes and all */
        fprintf(gen->output, ", 0LL, 0.0}");
    } else if (k == TYPE_BOOL) {
        int b = strcmp(v, "true") == 0;
        fprintf(gen->output, "{4, \"%s\", %dLL, %d.0}", b ? "true" : "false", b, b);
    } else if (k == TYPE_FLOAT) {
        /* The decimal spelling is a C double literal already; `i` is its
         * truncation, computed here so no out-of-range conversion is left
         * for the C compiler to reject. */
        double d = strtod(v, NULL);
        long long i = (d >= -9.2e18 && d <= 9.2e18) ? (long long)d : 0;
        fprintf(gen->output, "{2, ");
        schema_emit_name(gen, v);
        fprintf(gen->output, ", %lldLL, %s}", i, v);
    } else {
        /* An integer: decimal, or 0x / 0o / 0b (underscores already gone). */
        int neg = v[0] == '-';
        const char* d = neg ? v + 1 : v;
        unsigned long long u = 0;
        if (d[0] == '0' && (d[1] == 'x' || d[1] == 'X')) u = strtoull(d + 2, NULL, 16);
        else if (d[0] == '0' && (d[1] == 'o' || d[1] == 'O')) u = strtoull(d + 2, NULL, 8);
        else if (d[0] == '0' && (d[1] == 'b' || d[1] == 'B')) u = strtoull(d + 2, NULL, 2);
        else u = strtoull(d, NULL, 10);
        long long i = neg ? -(long long)u : (long long)u;
        fprintf(gen->output, "{1, ");
        schema_emit_name(gen, v);
        fprintf(gen->output, ", %lldLL, %.17g}", i, (double)i);
    }
}

static void schema_emit_table(CodeGenerator* gen, ASTNode* sd) {
    const char* S = sd->value;
    int nfields = 0;
    for (int i = 0; i < sd->child_count; i++) {
        ASTNode* f = sd->children[i];
        if (f && f->type == AST_STRUCT_FIELD) nfields++;
    }

    /* Attribute argument and attribute arrays, per field that has any. */
    int fi = 0;
    for (int i = 0; i < sd->child_count; i++) {
        ASTNode* f = sd->children[i];
        if (!f || f->type != AST_STRUCT_FIELD) continue;
        int nattr = 0;
        for (int a = 0; a < f->child_count; a++) {
            ASTNode* attr = f->children[a];
            if (!attr || attr->type != AST_FIELD_ATTRIBUTE) continue;
            if (attr->child_count > 0) {
                fprintf(gen->output, "static const AetherSchemaArg _ae_schema_%s_a%d_%d[] = {",
                        S, fi, nattr);
                for (int g = 0; g < attr->child_count; g++) {
                    fprintf(gen->output, g ? ", " : " ");
                    schema_emit_arg(gen, attr->children[g]);
                }
                fprintf(gen->output, " };\n");
            }
            nattr++;
        }
        if (nattr > 0) {
            fprintf(gen->output, "static const AetherSchemaAttr _ae_schema_%s_a%d[] = {", S, fi);
            int k = 0;
            for (int a = 0; a < f->child_count; a++) {
                ASTNode* attr = f->children[a];
                if (!attr || attr->type != AST_FIELD_ATTRIBUTE) continue;
                fprintf(gen->output, k ? ", {" : " {");
                schema_emit_name(gen, attr->value);
                if (attr->child_count > 0)
                    fprintf(gen->output, ", %d, _ae_schema_%s_a%d_%d}", attr->child_count, S, fi, k);
                else
                    fprintf(gen->output, ", 0, NULL}");
                k++;
            }
            fprintf(gen->output, " };\n");
        }
        fi++;
    }

    if (nfields > 0) {
        fprintf(gen->output, "static const AetherSchemaField _ae_schema_%s_fields[] = {\n", S);
        fi = 0;
        for (int i = 0; i < sd->child_count; i++) {
            ASTNode* f = sd->children[i];
            if (!f || f->type != AST_STRUCT_FIELD) continue;
            Type* t = f->node_type;
            char spelled[256];
            size_t len = 0;
            spelled[0] = '\0';
            schema_spell(t, spelled, sizeof(spelled), &len);
            int kind = schema_kind(t);
            int elem_kind = (t && t->kind == TYPE_ARRAY) ? schema_kind(t->element_type) : 0;
            int alen = (t && t->kind == TYPE_ARRAY && t->array_size > 0) ? t->array_size : 0;
            ASTNode* nested = schema_struct_def(gen, schema_target_struct(t));
            int nattr = 0;
            for (int a = 0; a < f->child_count; a++)
                if (f->children[a] && f->children[a]->type == AST_FIELD_ATTRIBUTE) nattr++;

            fprintf(gen->output, "    {");
            schema_emit_name(gen, f->value);
            fprintf(gen->output, ", ");
            schema_emit_name(gen, spelled);
            fprintf(gen->output, ", %d, %d, %d, offsetof(struct %s, %s), sizeof(((struct %s*)0)->%s), ",
                    kind, elem_kind, alen, S, f->value, S, f->value);
            /* A string field's `_heap_<name>` ownership tracker sits right
             * after it in every Aether struct (codegen_func.c). */
            if (t && t->kind == TYPE_STRING)
                fprintf(gen->output, "(long)offsetof(struct %s, _heap_%s), ", S, f->value);
            else
                fprintf(gen->output, "-1L, ");
            if (nested) fprintf(gen->output, "_ae_schema_%s, ", nested->value);
            else fprintf(gen->output, "NULL, ");
            if (nattr > 0) fprintf(gen->output, "%d, _ae_schema_%s_a%d}", nattr, S, fi);
            else fprintf(gen->output, "0, NULL}");
            fprintf(gen->output, "%s\n", fi + 1 < nfields ? "," : "");
            fi++;
        }
        fprintf(gen->output, "};\n");
    }

    fprintf(gen->output, "static const AetherSchema _ae_schema_%s_table = { ", S);
    schema_emit_name(gen, S);
    if (nfields > 0)
        fprintf(gen->output, ", sizeof(struct %s), %d, _ae_schema_%s_fields };\n", S, nfields, S);
    else
        fprintf(gen->output, ", sizeof(struct %s), 0, NULL };\n", S);
    fprintf(gen->output,
            "static const AetherSchema* _ae_schema_%s(void) { return &_ae_schema_%s_table; }\n",
            S, S);
}

void emit_schema_tables(CodeGenerator* gen, ASTNode* program) {
    if (!gen || !program) return;
    SchemaSet set = { NULL, 0, 0 };
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* sd = program->children[i];
        if (!sd || sd->type != AST_STRUCT_DEFINITION) continue;
        if (!annotation_has_marker(sd->annotation, "schema")) continue;
        schema_set_collect(gen, &set, schema_struct_def(gen, sd->value));
    }
    if (set.count == 0) { free(set.defs); return; }

    fprintf(gen->output, "\n/* @derive(schema) field tables (#2298) */\n");
    /* The layout only: std.schema's externs declare the accessors. */
    fprintf(gen->output, "#define AETHER_SCHEMA_LAYOUT_ONLY\n#include \"aether_schema.h\"\n");
    for (int i = 0; i < set.count; i++)
        fprintf(gen->output, "static const AetherSchema* _ae_schema_%s(void);\n", set.defs[i]->value);
    for (int i = 0; i < set.count; i++) schema_emit_table(gen, set.defs[i]);
    fprintf(gen->output, "\n");
    free(set.defs);
}
