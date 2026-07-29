#include "test_harness.h"
#include <string.h>

// External test registration functions
void register_scheduler_tests(void);
void register_stress_tests(void);
void register_zerocopy_tests(void);
void register_lockfree_mailbox_tests(void);
void register_memory_arena_tests(void);
void register_memory_pool_tests(void);
void register_lexer_tests(void);
void register_scheduler_optimization_tests(void);
void register_worksteal_race_tests(void);

void print_test_help(const char* program) {
    printf("Aether Test Runner\n\n");
    printf("Usage:\n");
    printf("  %s                    Run all tests\n", program);
    printf("  %s --category=NAME    Run tests in specific category\n", program);
    printf("  %s --list-categories  List all test categories\n", program);
    printf("  %s --help             Show this help\n", program);
    printf("\n");
    printf("Categories:\n");
    printf("  compiler    - Lexer, parser, type checker, code generator\n");
    printf("  runtime     - Actor system, scheduler, message passing\n");
    printf("  collections - HashMap, Set, Vector, PriorityQueue\n");
    printf("  network     - HTTP, TCP, networking utilities\n");
    printf("  memory      - Arena allocators, memory pools, leak detection\n");
    printf("  stdlib      - Standard library functions\n");
    printf("  parser      - Parser-specific tests\n");
    printf("  other       - Miscellaneous tests\n");
}

int main(int argc, char** argv) {
    // Parse arguments
    if (argc > 1) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            print_test_help(argv[0]);
            return 0;
        }
        
        if (strcmp(argv[1], "--list-categories") == 0) {
            printf("Available test categories:\n");
            printf("  compiler\n  runtime\n  collections\n  network\n");
            printf("  memory\n  stdlib\n  parser\n  other\n");
            return 0;
        }
        
        if (strncmp(argv[1], "--category=", 11) == 0) {
            const char* category_name = argv[1] + 11;
            TestCategory category = TEST_CATEGORY_OTHER;
            
            if (strcmp(category_name, "compiler") == 0) category = TEST_CATEGORY_COMPILER;
            else if (strcmp(category_name, "runtime") == 0) category = TEST_CATEGORY_RUNTIME;
            else if (strcmp(category_name, "collections") == 0) category = TEST_CATEGORY_COLLECTIONS;
            else if (strcmp(category_name, "network") == 0) category = TEST_CATEGORY_NETWORK;
            else if (strcmp(category_name, "memory") == 0) category = TEST_CATEGORY_MEMORY;
            else if (strcmp(category_name, "stdlib") == 0) category = TEST_CATEGORY_STDLIB;
            else if (strcmp(category_name, "parser") == 0) category = TEST_CATEGORY_PARSER;
            else if (strcmp(category_name, "other") == 0) category = TEST_CATEGORY_OTHER;
            else {
                fprintf(stderr, "Unknown category: %s\n", category_name);
                fprintf(stderr, "Use --list-categories to see available categories\n");
                return 1;
            }
            
            run_tests_by_category(category);
            return 0;
        }
        
        fprintf(stderr, "Unknown option: %s\n", argv[1]);
        fprintf(stderr, "Use --help for usage information\n");
        return 1;
    }
    
    printf("Aether Language Test Suite\n");
    printf("==========================\n\n");
    
    // Register tests
    register_scheduler_tests();
    register_stress_tests();
    register_zerocopy_tests();
    register_lockfree_mailbox_tests();
    register_memory_arena_tests();
    register_memory_pool_tests();
    register_lexer_tests();
    register_scheduler_optimization_tests();
    register_worksteal_race_tests();
    
    run_all_tests();

    // Flush all output before exit (helps on Windows)
    fflush(stdout);
    fflush(stderr);

    return 0;
}
