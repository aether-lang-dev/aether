// @derive(schema) field tables (#2298): the accessors in
// runtime/aether_schema.c, held to a hand-built table without a compiler in
// the loop. The compiler half (that `@derive(schema)` emits this layout) is
// tests/compiler/test_codegen.c; the two together end to end are
// tests/regression/test_issue2298_derive_schema.ae.
#include "test_harness.h"
#include "../../runtime/aether_schema.h"
#include <string.h>

typedef struct { int a; double b; const char* c; int _heap_c; } Rec;
typedef struct { int x; } Inner;

static const AetherSchemaField inner_fields[] = {
    { "x", "int", AETHER_SCHEMA_INT, 0, 0, offsetof(Inner, x), sizeof(int), -1, NULL, 0, NULL },
};
static const AetherSchema inner_table = { "Inner", sizeof(Inner), 1, inner_fields };
static const AetherSchema* inner_schema(void) { return &inner_table; }

static const AetherSchemaArg range_args[] = {
    { AETHER_SCHEMA_ARG_FLOAT, "-1.5", -1, -1.5 },
    { AETHER_SCHEMA_ARG_INT, "10", 10, 10.0 },
};
static const AetherSchemaAttr b_attrs[] = {
    { "range", 2, range_args },
    { "hidden", 0, NULL },
};
static const AetherSchemaField rec_fields[] = {
    { "a", "int", AETHER_SCHEMA_INT, 0, 0, offsetof(Rec, a), sizeof(int), -1, inner_schema, 0, NULL },
    { "b", "float", AETHER_SCHEMA_FLOAT, 0, 0, offsetof(Rec, b), sizeof(double), -1, NULL, 2, b_attrs },
    { "c", "string", AETHER_SCHEMA_STRING, 0, 0, offsetof(Rec, c), sizeof(char*),
      (long)offsetof(Rec, _heap_c), NULL, 0, NULL },
};
static const AetherSchema rec_table = { "Rec", sizeof(Rec), 3, rec_fields };

TEST_CATEGORY(schema_reads_the_struct_and_its_fields, TEST_CATEGORY_RUNTIME) {
    const void* s = &rec_table;
    ASSERT_STREQ("Rec", aether_schema_name(s));
    ASSERT_EQ((int)sizeof(Rec), aether_schema_size(s));
    ASSERT_EQ(3, aether_schema_field_count(s));
    ASSERT_EQ(1, aether_schema_field_index(s, "b"));
    ASSERT_EQ(-1, aether_schema_field_index(s, "zzz"));
    ASSERT_STREQ("float", aether_schema_field_type(s, 1));
    ASSERT_EQ(AETHER_SCHEMA_FLOAT, aether_schema_field_kind(s, 1));
    ASSERT_EQ((int)offsetof(Rec, b), aether_schema_field_offset(s, 1));
    ASSERT_EQ((int)sizeof(double), aether_schema_field_size(s, 1));
    ASSERT_EQ((int)offsetof(Rec, _heap_c), aether_schema_field_heap_offset(s, 2));
    ASSERT_EQ(-1, aether_schema_field_heap_offset(s, 0));
    ASSERT_TRUE(aether_schema_field_schema(s, 0) == (const void*)&inner_table);
    ASSERT_NULL(aether_schema_field_schema(s, 1));
}

TEST_CATEGORY(schema_reads_attributes_and_arguments, TEST_CATEGORY_RUNTIME) {
    const void* s = &rec_table;
    ASSERT_EQ(2, aether_schema_attr_count(s, 1));
    ASSERT_EQ(1, aether_schema_attr_index(s, 1, "hidden"));
    ASSERT_EQ(-1, aether_schema_attr_index(s, 1, "range2"));
    ASSERT_STREQ("range", aether_schema_attr_name(s, 1, 0));
    ASSERT_EQ(2, aether_schema_attr_arg_count(s, 1, 0));
    ASSERT_EQ(0, aether_schema_attr_arg_count(s, 1, 1));
    ASSERT_EQ(AETHER_SCHEMA_ARG_FLOAT, aether_schema_attr_arg_kind(s, 1, 0, 0));
    ASSERT_TRUE(aether_schema_attr_float(s, 1, 0, 0) == -1.5);
    ASSERT_EQ(-1, (int)aether_schema_attr_int(s, 1, 0, 0));
    ASSERT_EQ(10, (int)aether_schema_attr_int(s, 1, 0, 1));
    ASSERT_STREQ("10", aether_schema_attr_text(s, 1, 0, 1));
}

/* Every accessor on no table, and on each index out of range, reads as
 * nothing rather than past an array. */
TEST_CATEGORY(schema_is_null_and_bounds_safe, TEST_CATEGORY_RUNTIME) {
    const void* s = &rec_table;
    ASSERT_NULL(aether_schema_name(NULL));
    ASSERT_EQ(0, aether_schema_size(NULL));
    ASSERT_EQ(0, aether_schema_field_count(NULL));
    ASSERT_EQ(-1, aether_schema_field_index(NULL, "a"));
    ASSERT_EQ(-1, aether_schema_field_index(s, NULL));
    ASSERT_NULL(aether_schema_field_name(s, 3));
    ASSERT_NULL(aether_schema_field_name(s, -1));
    ASSERT_NULL(aether_schema_field_type(NULL, 0));
    ASSERT_EQ(AETHER_SCHEMA_OTHER, aether_schema_field_kind(s, 9));
    ASSERT_EQ(AETHER_SCHEMA_OTHER, aether_schema_field_elem_kind(s, 9));
    ASSERT_EQ(0, aether_schema_field_len(s, 9));
    ASSERT_EQ(-1, aether_schema_field_offset(s, 9));
    ASSERT_EQ(0, aether_schema_field_size(s, 9));
    ASSERT_EQ(-1, aether_schema_field_heap_offset(s, 9));
    ASSERT_NULL(aether_schema_field_schema(s, 9));
    ASSERT_EQ(0, aether_schema_attr_count(s, 9));
    ASSERT_EQ(-1, aether_schema_attr_index(s, 9, "range"));
    ASSERT_NULL(aether_schema_attr_name(s, 1, 2));
    ASSERT_NULL(aether_schema_attr_name(s, 0, 0));   /* a field with none */
    ASSERT_EQ(0, aether_schema_attr_arg_count(s, 1, 5));
    ASSERT_EQ(0, aether_schema_attr_arg_kind(s, 1, 0, 2));
    ASSERT_EQ(0, (int)aether_schema_attr_int(s, 1, 1, 0));   /* @hidden has none */
    ASSERT_TRUE(aether_schema_attr_float(s, 1, 0, -1) == 0.0);
    ASSERT_NULL(aether_schema_attr_text(NULL, 0, 0, 0));
}
