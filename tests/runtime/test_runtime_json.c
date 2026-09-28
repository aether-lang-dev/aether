#include "test_harness.h"
#include "../../std/json/aether_json.h"

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
    ASSERT_NOT_NULL(strstr(json_str, "name"));
    ASSERT_NOT_NULL(strstr(json_str, "Bob"));

    free(json_str);
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
