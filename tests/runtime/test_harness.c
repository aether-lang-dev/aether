#include "test_harness.h"
#include <string.h>
#include <stdatomic.h>
#include "../../runtime/utils/aether_thread.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define MAX_TESTS 1000

// Global test failure handling
jmp_buf test_failure_jmp;
int test_failure_flag = 0;

typedef struct {
    const char* name;
    TestFunction func;
    TestCategory category;
    long duration_ms;
    bool passed;
} TestEntry;

static TestEntry tests[MAX_TESTS];
static int test_count = 0;
static int passed_count = 0;
static int failed_count = 0;

const char* get_category_name(TestCategory category) {
    switch (category) {
        case TEST_CATEGORY_COMPILER: return "Compiler";
        case TEST_CATEGORY_RUNTIME: return "Runtime";
        case TEST_CATEGORY_COLLECTIONS: return "Collections";
        case TEST_CATEGORY_NETWORK: return "Network";
        case TEST_CATEGORY_MEMORY: return "Memory";
        case TEST_CATEGORY_STDLIB: return "Stdlib";
        case TEST_CATEGORY_PARSER: return "Parser";
        case TEST_CATEGORY_OTHER: return "Other";
        default: return "Unknown";
    }
}

void register_test(const char* name, TestFunction func) {
    register_test_with_category(name, func, TEST_CATEGORY_OTHER);
}

void register_test_with_category(const char* name, TestFunction func, TestCategory category) {
    if (test_count >= MAX_TESTS) {
        fprintf(stderr, "ERROR: Too many tests (max %d)\n", MAX_TESTS);
        exit(1);
    }
    tests[test_count].name = name;
    tests[test_count].func = func;
    tests[test_count].category = category;
    tests[test_count].duration_ms = 0;
    tests[test_count].passed = false;
    test_count++;
}

static long get_time_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* A test that hangs is ended by this watchdog, which names it (#2608). The
 * harness prints a test's name only once it finishes, so a hung one showed
 * the test before it, or nothing at all: on the Windows runner the job ran
 * on until the runner was lost, and no log was kept. AE_C_TEST_TIMEOUT sets
 * the limit in seconds (0 turns the watchdog off); the slowest test takes
 * about a second. */
static _Atomic(const char*) watch_name = NULL;
static atomic_long watch_start_ms = 0;     /* 0: no test is running */
static atomic_int watch_stop = 0;
static long watch_limit_ms = 0;
static pthread_t watch_thread;
static int watch_running = 0;

static void watch_nap(void) {
#ifdef _WIN32
    Sleep(100);
#else
    usleep(100 * 1000);
#endif
}

static void* test_watchdog(void* arg) {
    (void)arg;
    while (!atomic_load(&watch_stop)) {
        watch_nap();
        long start = atomic_load(&watch_start_ms);
        if (start != 0 && get_time_ms() - start > watch_limit_ms) {
            const char* name = atomic_load(&watch_name);
            fflush(stdout);
            fprintf(stderr, "\n  [TIMEOUT] %s ran past %ld s and is ended: it is hung "
                            "(AE_C_TEST_TIMEOUT sets the limit)\n",
                    name ? name : "(unnamed test)", watch_limit_ms / 1000);
            fflush(stderr);
            _exit(124);
        }
    }
    return NULL;
}

static void watchdog_start(void) {
    const char* env = getenv("AE_C_TEST_TIMEOUT");
    long secs = env && *env ? strtol(env, NULL, 10) : 120;
    if (secs <= 0 || watch_running) return;
    watch_limit_ms = secs * 1000;
    atomic_store(&watch_stop, 0);
    watch_running = pthread_create(&watch_thread, NULL, test_watchdog, NULL) == 0;
}

static void watchdog_stop(void) {
    if (!watch_running) return;
    atomic_store(&watch_stop, 1);
    pthread_join(watch_thread, NULL);
    watch_running = 0;
}

static void watch_begin(const char* name, long start) {
    atomic_store(&watch_name, name);
    atomic_store(&watch_start_ms, start);
}

static void watch_end(void) {
    atomic_store(&watch_start_ms, 0);
}

void run_all_tests(void) {
    long total_start = get_time_ms();
    watchdog_start();
    
    printf("%s=== Aether Test Suite ===%s\n\n", COLOR_CYAN, COLOR_RESET);
    printf("Running %d test(s) across %d categories...\n\n", test_count, 8);
    fflush(stdout);
    
    // Group tests by category
    for (TestCategory cat = TEST_CATEGORY_COMPILER; cat <= TEST_CATEGORY_OTHER; cat++) {
        int cat_tests = 0;
        for (int i = 0; i < test_count; i++) {
            if (tests[i].category == cat) cat_tests++;
        }
        
        if (cat_tests == 0) continue;
        
        printf("%s%s Tests (%d tests)%s\n", COLOR_YELLOW, get_category_name(cat), cat_tests, COLOR_RESET);
        fflush(stdout);

        for (volatile int i = 0; i < test_count; i++) {
            if (tests[i].category != cat) continue;
            
            // Reset failure flag
            test_failure_flag = 0;
            
            // Time the test
            long start = get_time_ms();
            watch_begin(tests[i].name, start);
            
            // Set up jump point for test failures
            if (setjmp(test_failure_jmp) == 0) {
                // Run the test
                tests[i].func();
                
                // If we reach here, test passed
                if (!test_failure_flag) {
                    tests[i].passed = true;
                    passed_count++;
                    long duration = get_time_ms() - start;
                    tests[i].duration_ms = duration;
                    printf("  %s[PASS]%s %s (%ldms)\n", COLOR_GREEN, COLOR_RESET, tests[i].name, duration);
                }
            } else {
                // We jumped here from a failed assertion
                tests[i].passed = false;
                failed_count++;
                long duration = get_time_ms() - start;
                tests[i].duration_ms = duration;
                printf("  %s[FAIL]%s %s (%ldms)\n", COLOR_RED, COLOR_RESET, tests[i].name, duration);
            }
            watch_end();
            fflush(stdout);
        }
        printf("\n");
    }
    watchdog_stop();
    
    long total_duration = get_time_ms() - total_start;
    
    // Summary
    printf("%s========================================%s\n", COLOR_CYAN, COLOR_RESET);
    printf("Summary: ");
    if (failed_count == 0) {
        printf("%s%d passed%s, ", COLOR_GREEN, passed_count, COLOR_RESET);
    } else {
        printf("%d passed, ", passed_count);
    }
    
    if (failed_count > 0) {
        printf("%s%d failed%s, ", COLOR_RED, failed_count, COLOR_RESET);
    } else {
        printf("%d failed, ", failed_count);
    }
    printf("%d total ", test_count);
    printf("(%ld.%03lds total)\n", total_duration / 1000, total_duration % 1000);
    
    // Find slowest tests
    printf("\nSlowest tests:\n");
    /* Track which entries have been reported by INDEX. Comparing durations
     * against the running maximum, as this once did, excluded nothing: the
     * same test came out on top of every pass and got printed five times. */
    int shown[5];
    int shown_count = 0;
    for (int rank = 0; rank < 5 && rank < test_count; rank++) {
        int slowest_idx = -1;
        long slowest_time = 0;

        for (int i = 0; i < test_count; i++) {
            bool already_shown = false;
            for (int j = 0; j < shown_count; j++) {
                if (shown[j] == i) {
                    already_shown = true;
                    break;
                }
            }

            if (!already_shown && tests[i].duration_ms > slowest_time) {
                slowest_time = tests[i].duration_ms;
                slowest_idx = i;
            }
        }

        if (slowest_idx < 0 || slowest_time <= 0) break;
        printf("  %ldms - %s\n", tests[slowest_idx].duration_ms, tests[slowest_idx].name);
        shown[shown_count++] = slowest_idx;
    }
    
    printf("%s========================================%s\n", COLOR_CYAN, COLOR_RESET);
    fflush(stdout);
    
    if (failed_count > 0) {
        exit(1);
    }
}

void run_tests_by_category(TestCategory category) {
    printf("%s=== Aether Test Suite - %s Tests ===%s\n\n", 
           COLOR_CYAN, get_category_name(category), COLOR_RESET);
    
    int cat_test_count = 0;
    for (int i = 0; i < test_count; i++) {
        if (tests[i].category == category) cat_test_count++;
    }
    
    printf("Running %d %s test(s)...\n\n", cat_test_count, get_category_name(category));
    fflush(stdout);
    
    long total_start = get_time_ms();
    watchdog_start();
    
    for (int i = 0; i < test_count; i++) {
        if (tests[i].category != category) continue;
        
        // Reset failure flag
        test_failure_flag = 0;
        
        // Time the test
        long start = get_time_ms();
        watch_begin(tests[i].name, start);
        
        // Set up jump point for test failures
        if (setjmp(test_failure_jmp) == 0) {
            // Run the test
            tests[i].func();
            
            // If we reach here, test passed
            if (!test_failure_flag) {
                tests[i].passed = true;
                passed_count++;
                long duration = get_time_ms() - start;
                tests[i].duration_ms = duration;
                printf("  %s✓%s %s (%ldms)\n", COLOR_GREEN, COLOR_RESET, tests[i].name, duration);
            }
        } else {
            // We jumped here from a failed assertion
            tests[i].passed = false;
            failed_count++;
            long duration = get_time_ms() - start;
            tests[i].duration_ms = duration;
            printf("  %s✗%s %s (%ldms)\n", COLOR_RED, COLOR_RESET, tests[i].name, duration);
        }
        watch_end();
        fflush(stdout);
    }
    watchdog_stop();
    
    long total_duration = get_time_ms() - total_start;
    
    printf("\n%s========================================%s\n", COLOR_CYAN, COLOR_RESET);
    printf("Summary: %d passed, %d failed (%ld.%03lds)\n", 
           passed_count, failed_count, total_duration / 1000, total_duration % 1000);
    printf("%s========================================%s\n", COLOR_CYAN, COLOR_RESET);
    
    if (failed_count > 0) {
        exit(1);
    }
}

int get_test_count(void) {
    return test_count;
}

int get_passed_count(void) {
    return passed_count;
}

int get_failed_count(void) {
    return failed_count;
}
