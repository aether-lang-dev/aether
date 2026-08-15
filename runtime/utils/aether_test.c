#include "aether_test.h"
#include <stdlib.h>
#include <string.h>

AetherTestStats aether_test_stats = {0};

static AetherTest* tests = NULL;
static int test_count = 0;
static int test_capacity = 0;

void aether_register_test(const char* name, void (*test_func)(void)) {
    if (test_count >= test_capacity) {
        int grown_capacity = test_capacity == 0 ? 16 : test_capacity * 2;
        AetherTest* grown = (AetherTest*)realloc(tests, (size_t)grown_capacity * sizeof(AetherTest));
        if (!grown) {
            /* Registration runs from constructors before main, so there is no
             * caller to return a failure to; carrying on would write through
             * a null pointer one line down. */
            fprintf(stderr, "aether_register_test: out of memory registering '%s'\n", name);
            exit(1);
        }
        tests = grown;
        test_capacity = grown_capacity;
    }
    
    tests[test_count].name = name;
    tests[test_count].test_func = test_func;
    test_count++;
}

void aether_run_tests(void) {
    printf("\n\033[1m=== Running Aether Tests ===\033[0m\n\n");
    
    aether_test_stats.total = test_count;
    aether_test_stats.passed = 0;
    aether_test_stats.failed = 0;
    
    for (int i = 0; i < test_count; i++) {
        aether_test_stats.current_test = tests[i].name;
        printf("Test %d/%d: %s ... ", i + 1, test_count, tests[i].name);
        fflush(stdout);
        
        int failed_before = aether_test_stats.failed;
        tests[i].test_func();
        int failed_after = aether_test_stats.failed;
        
        if (failed_after == failed_before) {
            printf("\033[32mPASS\033[0m\n");
            aether_test_stats.passed++;
        } else {
            // Failure message already printed by assertion
        }
    }
    
    aether_test_summary();
}

void aether_test_summary(void) {
    printf("\n\033[1m=== Test Summary ===\033[0m\n");
    printf("Total:  %d\n", aether_test_stats.total);
    printf("\033[32mPassed: %d\033[0m\n", aether_test_stats.passed);
    
    if (aether_test_stats.failed > 0) {
        printf("\033[31mFailed: %d\033[0m\n", aether_test_stats.failed);
    } else {
        printf("Failed: 0\n");
    }
    
    printf("\n");
    
    if (aether_test_stats.failed == 0) {
        printf("\033[1m\033[32m✓ All tests passed!\033[0m\n\n");
    } else {
        printf("\033[1m\033[31m✗ Some tests failed\033[0m\n\n");
    }
}

