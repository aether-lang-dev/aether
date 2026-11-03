#include "test_harness.h"
#include "../../std/json/aether_json.h"
#include "../../std/string/aether_string.h"
#include "../../runtime/aether_resource_caps.h"
#include <stdlib.h>
#include <string.h>

TEST_CATEGORY(json_parse_null, TEST_CATEGORY_STDLIB) {
    JsonValue* value = json_parse_raw("null");

    ASSERT_NOT_NULL(value);
    ASSERT_TRUE(json_is_null(value));
    ASSERT_EQ(JSON_NULL, json_type(value));

    json_free(value);
}

TEST_CATEGORY(json_parse_bool, TEST_CATEGORY_STDLIB) {
    JsonValue* val_true = json_parse_raw("true");
    ASSERT_EQ(JSON_BOOL, json_type(val_true));
    ASSERT_EQ(1, json_get_bool(val_true));

    JsonValue* val_false = json_parse_raw("false");
    ASSERT_EQ(JSON_BOOL, json_type(val_false));
    ASSERT_EQ(0, json_get_bool(val_false));

    json_free(val_true);
    json_free(val_false);
}

TEST_CATEGORY(json_parse_number, TEST_CATEGORY_STDLIB) {
    JsonValue* value = json_parse_raw("42.5");

    ASSERT_NOT_NULL(value);
    ASSERT_EQ(JSON_NUMBER, json_type(value));
    ASSERT_EQ(42, json_get_int(value));

    json_free(value);
}

TEST_CATEGORY(json_is_integer_tells_integers_from_decimals, TEST_CATEGORY_STDLIB) {
    JsonValue* whole = json_parse_raw("[42, -7, 9223372036854775807, -9223372036854775808]");
    ASSERT_NOT_NULL(whole);
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ(1, json_is_integer(json_array_get_raw(whole, i)));
    }
    json_free(whole);

    // A fraction, an exponent, a magnitude past int64 and -0 are doubles.
    JsonValue* frac = json_parse_raw("[42.0, 4e1, 9223372036854775808, -0]");
    ASSERT_NOT_NULL(frac);
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ(0, json_is_integer(json_array_get_raw(frac, i)));
    }
    json_free(frac);

    JsonValue* built = json_create_int(5);
    ASSERT_EQ(1, json_is_integer(built));
    json_free(built);

    JsonValue* text = json_parse_raw("\"42\"");
    ASSERT_EQ(0, json_is_integer(text));
    json_free(text);
    ASSERT_EQ(0, json_is_integer(NULL));
}

TEST_CATEGORY(json_parse_string, TEST_CATEGORY_STDLIB) {
    JsonValue* value = json_parse_raw("\"hello world\"");

    ASSERT_NOT_NULL(value);
    ASSERT_EQ(JSON_STRING, json_type(value));

    const char* str_val = json_get_string_raw(value);
    ASSERT_NOT_NULL(str_val);
    ASSERT_STREQ("hello world", str_val);

    json_free(value);
}

TEST_CATEGORY(json_parse_array, TEST_CATEGORY_STDLIB) {
    JsonValue* value = json_parse_raw("[1, 2, 3]");

    ASSERT_NOT_NULL(value);
    ASSERT_EQ(JSON_ARRAY, json_type(value));
    ASSERT_EQ(3, json_array_size(value));

    JsonValue* first = json_array_get_raw(value, 0);
    ASSERT_EQ(1, json_get_int(first));

    json_free(value);
}

TEST_CATEGORY(json_parse_object, TEST_CATEGORY_STDLIB) {
    JsonValue* value = json_parse_raw("{\"name\":\"Alice\",\"age\":30}");

    ASSERT_NOT_NULL(value);
    ASSERT_EQ(JSON_OBJECT, json_type(value));

    ASSERT_TRUE(json_object_has(value, "name"));

    JsonValue* name_val = json_object_get_raw(value, "name");
    ASSERT_NOT_NULL(name_val);
    ASSERT_STREQ("Alice", json_get_string_raw(name_val));

    JsonValue* age_val = json_object_get_raw(value, "age");
    ASSERT_EQ(30, json_get_int(age_val));

    json_free(value);
}

TEST_CATEGORY(json_create_and_stringify, TEST_CATEGORY_STDLIB) {
    JsonValue* obj = json_create_object();

    JsonValue* name_val = json_create_string("Bob");
    json_object_set_raw(obj, "name", name_val);

    JsonValue* age_val = json_create_number(25);
    json_object_set_raw(obj, "age", age_val);

    char* json_str = json_stringify_raw(obj);
    ASSERT_NOT_NULL(json_str);
    ASSERT_NOT_NULL(strstr(aether_string_data(json_str), "name"));
    ASSERT_NOT_NULL(strstr(aether_string_data(json_str), "Bob"));

    string_release(json_str);
    json_free(obj);
}

TEST_CATEGORY(json_array_operations, TEST_CATEGORY_STDLIB) {
    JsonValue* arr = json_create_array();

    json_array_add_raw(arr, json_create_number(10));
    json_array_add_raw(arr, json_create_number(20));
    json_array_add_raw(arr, json_create_number(30));

    ASSERT_EQ(3, json_array_size(arr));
    ASSERT_EQ(20, json_get_int(json_array_get_raw(arr, 1)));

    json_free(arr);
}

// A value set into a container inside a parsed document is allocated in
// the document's arena and freed with it (#2719): an object in an array,
// an object in an object, an array in an object. Freeing an inner node is
// a no-op, as it was. Under the ASan runner a leaked arena fails this.
TEST_CATEGORY(json_set_into_parsed_inner_containers, TEST_CATEGORY_STDLIB) {
    JsonValue* root = json_parse_raw(
        "{\"objects\": [{\"fields\": {\"velocity\": 36.0}}], \"meta\": {\"tags\": [1]}}");
    ASSERT_NOT_NULL(root);
    JsonValue* item = json_array_get_raw(json_object_get_raw(root, "objects"), 0);
    JsonValue* fields = json_object_get_raw(item, "fields");
    ASSERT_EQ(1, json_object_set_raw(fields, "speed", json_create_number(10)));
    ASSERT_EQ(1, json_object_set_raw(item, "kind", json_create_string("Mover")));
    JsonValue* tags = json_object_get_raw(json_object_get_raw(root, "meta"), "tags");
    ASSERT_EQ(1, json_array_add_raw(tags, json_create_number(2)));

    // The writes are where they were made.
    ASSERT_EQ(10, json_get_int(json_object_get_raw(
        json_object_get_raw(json_array_get_raw(json_object_get_raw(root, "objects"), 0), "fields"), "speed")));
    ASSERT_EQ(2, json_array_size(tags));
    char* text = json_stringify_raw(root);
    ASSERT_NOT_NULL(strstr(aether_string_data(text), "\"speed\":10"));
    ASSERT_NOT_NULL(strstr(aether_string_data(text), "\"kind\":\"Mover\""));
    string_release(text);

    json_free(fields);   // borrowed: frees nothing
    json_free(root);
}

// An arena's first chunk is sized to what it will hold: a built container's
// is small (256 bytes of payload), a parsed document's is about 4 bytes per
// input byte, clamped to 256 B..16 KB. The arena's chunks are cap-aware
// allocations, so the accounted bytes (aether_caps_used_bytes) before and
// after read the first chunk directly, without timing or RSS. The bounds
// leave room for the chunk and arena headers and the root value, and are
// far below the 16 KB a built container used to take.
#define ARENA_HDR_SLACK 192u

static uint64_t json_accounted_for_parse(const char* text, JsonValue** out) {
    uint64_t before = aether_caps_used_bytes();
    *out = json_parse_raw(text);
    return aether_caps_used_bytes() - before;
}

TEST_CATEGORY(json_built_container_first_chunk_is_small, TEST_CATEGORY_STDLIB) {
    enum { N = 200 };
    JsonValue* objs[N];
    JsonValue* arrs[N];

    uint64_t before = aether_caps_used_bytes();
    for (int i = 0; i < N; i++) {
        objs[i] = json_create_object();
        ASSERT_NOT_NULL(objs[i]);
        ASSERT_EQ(1, json_object_set_raw(objs[i], "k", json_create_int(i)));
        arrs[i] = json_create_array();
        ASSERT_NOT_NULL(arrs[i]);
        ASSERT_EQ(1, json_array_add_raw(arrs[i], json_create_int(i)));
    }
    uint64_t total = aether_caps_used_bytes() - before;

    // Each small container holds its first chunk (256 B) plus headers and
    // one member array: well under 1 KB. With a 16 KB first chunk the 400
    // containers took over 6 MB; now they must fit in 400 KB.
    ASSERT_TRUE(total >= (uint64_t)(2 * N) * 256u);
    ASSERT_TRUE(total < (uint64_t)(2 * N) * 1024u);

    for (int i = 0; i < N; i++) {
        ASSERT_EQ(i, json_get_int(json_object_get_raw(objs[i], "k")));
        ASSERT_EQ(i, json_get_int(json_array_get_raw(arrs[i], 0)));
        json_free(objs[i]);
        json_free(arrs[i]);
    }
}

TEST_CATEGORY(json_built_container_arena_still_grows, TEST_CATEGORY_STDLIB) {
    // A small first chunk must not cap the container: members past it
    // double the arena as before.
    JsonValue* obj = json_create_object();
    char key[32];
    for (int i = 0; i < 500; i++) {
        snprintf(key, sizeof key, "key_%d", i);
        ASSERT_EQ(1, json_object_set_raw(obj, key, json_create_string("a value of some length")));
    }
    for (int i = 0; i < 500; i++) {
        snprintf(key, sizeof key, "key_%d", i);
        JsonValue* v = json_object_get_raw(obj, key);
        ASSERT_NOT_NULL(v);
        ASSERT_STREQ("a value of some length", json_get_string_raw(v));
    }
    json_free(obj);
}

TEST_CATEGORY(json_parsed_document_first_chunk_scales_with_input, TEST_CATEGORY_STDLIB) {
    JsonValue* v = NULL;
    uint64_t d;

    // A tiny reply takes the 256-byte minimum, not 16 KB.
    d = json_accounted_for_parse("42", &v);
    ASSERT_NOT_NULL(v);
    ASSERT_TRUE(d >= 256u);
    ASSERT_TRUE(d < 256u + ARENA_HDR_SLACK);
    json_free(v);

    d = json_accounted_for_parse("{\"ok\":true}", &v);
    ASSERT_NOT_NULL(v);
    ASSERT_TRUE(d >= 256u);
    ASSERT_TRUE(d < 256u + ARENA_HDR_SLACK);
    json_free(v);

    // A 1000-byte document (one long string, so the arena holds it all in
    // the first chunk) gets about 4 bytes per input byte.
    size_t n = 1000;
    char* doc = (char*)malloc(n + 1);
    memset(doc, 'x', n);
    doc[0] = '"';
    doc[n - 1] = '"';
    doc[n] = 0;
    d = json_accounted_for_parse(doc, &v);
    ASSERT_NOT_NULL(v);
    ASSERT_TRUE(d >= 4u * n);
    ASSERT_TRUE(d < 4u * n + ARENA_HDR_SLACK);
    json_free(v);
    free(doc);

    // Past 4 KB of input the first chunk is clamped at 16 KB.
    n = 8000;
    doc = (char*)malloc(n + 1);
    memset(doc, 'x', n);
    doc[0] = '"';
    doc[n - 1] = '"';
    doc[n] = 0;
    d = json_accounted_for_parse(doc, &v);
    ASSERT_NOT_NULL(v);
    ASSERT_TRUE(d >= 16u * 1024u);
    ASSERT_TRUE(d < 16u * 1024u + ARENA_HDR_SLACK);
    json_free(v);
    free(doc);
}
