#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <setjmp.h>
#include <time.h>
#include <stdint.h>

// ANSI color codes
#ifdef _WIN32
    #define COLOR_RED ""
    #define COLOR_GREEN ""
    #define COLOR_YELLOW ""
    #define COLOR_CYAN ""
    #define COLOR_RESET ""
#else
    #define COLOR_RED "\033[0;31m"
    #define COLOR_GREEN "\033[0;32m"
    #define COLOR_YELLOW "\033[1;33m"
    #define COLOR_CYAN "\033[0;36m"
    #define COLOR_RESET "\033[0m"
#endif

// Test categories
typedef enum {
    TEST_CATEGORY_COMPILER,
    TEST_CATEGORY_RUNTIME,
    TEST_CATEGORY_COLLECTIONS,
    TEST_CATEGORY_NETWORK,
    TEST_CATEGORY_MEMORY,
    TEST_CATEGORY_STDLIB,
    TEST_CATEGORY_PARSER,
    TEST_CATEGORY_OTHER
} TestCategory;

// Test failure handling
extern jmp_buf test_failure_jmp;
extern int test_failure_flag;

// Test result structure
typedef struct {
    const char* name;
    bool passed;
    const char* file;
    int line;
    const char* message;
    long duration_ms;
    TestCategory category;
} TestResult;

// Test function pointer
typedef void (*TestFunction)(void);

// Test registration
void register_test(const char* name, TestFunction func);
void register_test_with_category(const char* name, TestFunction func, TestCategory category);
void run_all_tests(void);
void run_tests_by_category(TestCategory category);
void list_tests(void);
int get_test_count(void);
int get_passed_count(void);
int get_failed_count(void);
const char* get_category_name(TestCategory category);

// Assertion macros - now use longjmp for proper test isolation
#define ASSERT_TRUE(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "ASSERT_TRUE failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            test_failure_flag = 1; \
            longjmp(test_failure_jmp, 1); \
        } \
    } while (0)

#define ASSERT_FALSE(condition) \
    do { \
        if (condition) { \
            fprintf(stderr, "ASSERT_FALSE failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            test_failure_flag = 1; \
            longjmp(test_failure_jmp, 1); \
        } \
    } while (0)

#define ASSERT_EQ(expected, actual) \
    do { \
        if ((expected) != (actual)) { \
            fprintf(stderr, "ASSERT_EQ failed at %s:%d: expected %lld, got %lld\n", \
                    __FILE__, __LINE__, (long long)(intptr_t)(expected), (long long)(intptr_t)(actual)); \
            test_failure_flag = 1; \
            longjmp(test_failure_jmp, 1); \
        } \
    } while (0)

#define ASSERT_NE(expected, actual) \
    do { \
        if ((expected) == (actual)) { \
            fprintf(stderr, "ASSERT_NE failed at %s:%d: both values are %lld\n", \
                    __FILE__, __LINE__, (long long)(intptr_t)(expected)); \
            test_failure_flag = 1; \
            longjmp(test_failure_jmp, 1); \
        } \
    } while (0)

#define ASSERT_STREQ(expected, actual) \
    do { \
        if (strcmp((expected), (actual)) != 0) { \
            fprintf(stderr, "ASSERT_STREQ failed at %s:%d: expected '%s', got '%s'\n", \
                    __FILE__, __LINE__, (expected), (actual)); \
            test_failure_flag = 1; \
            longjmp(test_failure_jmp, 1); \
        } \
    } while (0)

#define ASSERT_STRNE(expected, actual) \
    do { \
        if (strcmp((expected), (actual)) == 0) { \
            fprintf(stderr, "ASSERT_STRNE failed at %s:%d: both strings are '%s'\n", \
                    __FILE__, __LINE__, (expected)); \
            test_failure_flag = 1; \
            longjmp(test_failure_jmp, 1); \
        } \
    } while (0)

#define ASSERT_NULL(ptr) \
    do { \
        if ((ptr) != NULL) { \
            fprintf(stderr, "ASSERT_NULL failed at %s:%d: pointer is not NULL\n", \
                    __FILE__, __LINE__); \
            test_failure_flag = 1; \
            longjmp(test_failure_jmp, 1); \
        } \
    } while (0)

#define ASSERT_NOT_NULL(ptr) \
    do { \
        if ((ptr) == NULL) { \
            fprintf(stderr, "ASSERT_NOT_NULL failed at %s:%d: pointer is NULL\n", \
                    __FILE__, __LINE__); \
            test_failure_flag = 1; \
            longjmp(test_failure_jmp, 1); \
        } \
    } while (0)

// Test registration macro
#ifdef _MSC_VER
    // MSVC doesn't support constructor attribute
    #define TEST(name) \
        static void test_##name(void); \
        static int register_##name(void) { \
            register_test(#name, test_##name); \
            return 0; \
        } \
        static int register_##name##_dummy = register_##name(); \
        static void test_##name(void)
    
    #define TEST_CATEGORY(name, cat) \
        static void test_##name(void); \
        static int register_##name(void) { \
            register_test_with_category(#name, test_##name, cat); \
            return 0; \
        } \
        static int register_##name##_dummy = register_##name(); \
        static void test_##name(void)
#else
    #define TEST(name) \
        static void test_##name(void); \
        static void __attribute__((constructor)) register_##name(void) { \
            register_test(#name, test_##name); \
        } \
        static void test_##name(void)
    
    #define TEST_CATEGORY(name, cat) \
        static void test_##name(void); \
        static void __attribute__((constructor)) register_##name(void) { \
            register_test_with_category(#name, test_##name, cat); \
        } \
        static void test_##name(void)
#endif

#endif // TEST_HARNESS_H
