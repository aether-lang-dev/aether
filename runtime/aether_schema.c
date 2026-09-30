/* aether_schema.c — reading the tables `@derive(schema)` emits (#2298).
 * The layout and the contract of each accessor are in aether_schema.h. */
#include "aether_schema.h"

#include <string.h>

static const AetherSchemaField* field_at(const void* s, int field) {
    const AetherSchema* sc = (const AetherSchema*)s;
    if (!sc || field < 0 || field >= sc->field_count || !sc->fields) return NULL;
    return &sc->fields[field];
}

static const AetherSchemaAttr* attr_at(const void* s, int field, int attr) {
    const AetherSchemaField* f = field_at(s, field);
    if (!f || attr < 0 || attr >= f->attr_count || !f->attrs) return NULL;
    return &f->attrs[attr];
}

static const AetherSchemaArg* arg_at(const void* s, int field, int attr, int arg) {
    const AetherSchemaAttr* a = attr_at(s, field, attr);
    if (!a || arg < 0 || arg >= a->argc || !a->args) return NULL;
    return &a->args[arg];
}

const char* aether_schema_name(const void* s) {
    return s ? ((const AetherSchema*)s)->name : NULL;
}

int aether_schema_size(const void* s) {
    return s ? (int)((const AetherSchema*)s)->size : 0;
}

int aether_schema_field_count(const void* s) {
    return s ? ((const AetherSchema*)s)->field_count : 0;
}

int aether_schema_field_index(const void* s, const char* name) {
    const AetherSchema* sc = (const AetherSchema*)s;
    if (!sc || !name) return -1;
    for (int i = 0; i < sc->field_count; i++) {
        if (sc->fields[i].name && strcmp(sc->fields[i].name, name) == 0) return i;
    }
    return -1;
}

const char* aether_schema_field_name(const void* s, int field) {
    const AetherSchemaField* f = field_at(s, field);
    return f ? f->name : NULL;
}

const char* aether_schema_field_type(const void* s, int field) {
    const AetherSchemaField* f = field_at(s, field);
    return f ? f->type : NULL;
}

int aether_schema_field_kind(const void* s, int field) {
    const AetherSchemaField* f = field_at(s, field);
    return f ? f->kind : AETHER_SCHEMA_OTHER;
}

int aether_schema_field_elem_kind(const void* s, int field) {
    const AetherSchemaField* f = field_at(s, field);
    return f ? f->elem_kind : AETHER_SCHEMA_OTHER;
}

int aether_schema_field_len(const void* s, int field) {
    const AetherSchemaField* f = field_at(s, field);
    return f ? f->len : 0;
}

int aether_schema_field_offset(const void* s, int field) {
    const AetherSchemaField* f = field_at(s, field);
    return f ? (int)f->offset : -1;
}

int aether_schema_field_size(const void* s, int field) {
    const AetherSchemaField* f = field_at(s, field);
    return f ? (int)f->size : 0;
}

int aether_schema_field_heap_offset(const void* s, int field) {
    const AetherSchemaField* f = field_at(s, field);
    return f ? (int)f->heap_offset : -1;
}

const void* aether_schema_field_schema(const void* s, int field) {
    const AetherSchemaField* f = field_at(s, field);
    return (f && f->nested) ? (const void*)f->nested() : NULL;
}

int aether_schema_attr_count(const void* s, int field) {
    const AetherSchemaField* f = field_at(s, field);
    return f ? f->attr_count : 0;
}

int aether_schema_attr_index(const void* s, int field, const char* name) {
    const AetherSchemaField* f = field_at(s, field);
    if (!f || !name) return -1;
    for (int i = 0; i < f->attr_count; i++) {
        if (f->attrs[i].name && strcmp(f->attrs[i].name, name) == 0) return i;
    }
    return -1;
}

const char* aether_schema_attr_name(const void* s, int field, int attr) {
    const AetherSchemaAttr* a = attr_at(s, field, attr);
    return a ? a->name : NULL;
}

int aether_schema_attr_arg_count(const void* s, int field, int attr) {
    const AetherSchemaAttr* a = attr_at(s, field, attr);
    return a ? a->argc : 0;
}

int aether_schema_attr_arg_kind(const void* s, int field, int attr, int arg) {
    const AetherSchemaArg* v = arg_at(s, field, attr, arg);
    return v ? v->kind : 0;
}

int64_t aether_schema_attr_int(const void* s, int field, int attr, int arg) {
    const AetherSchemaArg* v = arg_at(s, field, attr, arg);
    return v ? (int64_t)v->i : 0;
}

double aether_schema_attr_float(const void* s, int field, int attr, int arg) {
    const AetherSchemaArg* v = arg_at(s, field, attr, arg);
    return v ? v->f : 0.0;
}

const char* aether_schema_attr_text(const void* s, int field, int attr, int arg) {
    const AetherSchemaArg* v = arg_at(s, field, attr, arg);
    return v ? v->text : NULL;
}
