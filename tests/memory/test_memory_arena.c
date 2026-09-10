#include "../runtime/test_harness.h"
#include "../../runtime/memory/aether_arena.h"
#include <string.h>

TEST_CATEGORY(arena_create_destroy, TEST_CATEGORY_MEMORY) {
    Arena* arena = arena_create(1024);
    ASSERT_NOT_NULL(arena);
    ASSERT_EQ(1024, arena_get_size(arena));
    ASSERT_EQ(0, arena_get_used(arena));
    arena_destroy(arena);
}

TEST_CATEGORY(arena_alloc_single, TEST_CATEGORY_MEMORY) {
    Arena* arena = arena_create(1024);
    
    int* ptr = (int*)arena_alloc(arena, sizeof(int));
    ASSERT_NOT_NULL(ptr);
    *ptr = 42;
    ASSERT_EQ(42, *ptr);
    
    ASSERT_TRUE(arena_get_used(arena) >= sizeof(int));
    arena_destroy(arena);
}

TEST_CATEGORY(arena_alloc_multiple, TEST_CATEGORY_MEMORY) {
    Arena* arena = arena_create(1024);
    
    int* a = (int*)arena_alloc(arena, sizeof(int));
    int* b = (int*)arena_alloc(arena, sizeof(int));
    int* c = (int*)arena_alloc(arena, sizeof(int));
    
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_NOT_NULL(c);
    
    *a = 1;
    *b = 2;
    *c = 3;
    
    ASSERT_EQ(1, *a);
    ASSERT_EQ(2, *b);
    ASSERT_EQ(3, *c);
    
    arena_destroy(arena);
}

TEST_CATEGORY(arena_reset, TEST_CATEGORY_MEMORY) {
    Arena* arena = arena_create(1024);
    
    arena_alloc(arena, 100);
    size_t used_before = arena_get_used(arena);
    ASSERT_TRUE(used_before > 0);
    
    arena_reset(arena);
    ASSERT_EQ(0, arena_get_used(arena));
    
    arena_destroy(arena);
}

TEST_CATEGORY(arena_scope, TEST_CATEGORY_MEMORY) {
    Arena* arena = arena_create(1024);
    
    ArenaScope scope = arena_begin(arena);
    
    arena_alloc(arena, 100);
    arena_alloc(arena, 200);
    ASSERT_TRUE(arena_get_used(arena) > 0);
    
    arena_end(scope);
    ASSERT_EQ(0, arena_get_used(arena));
    
    arena_destroy(arena);
}

TEST_CATEGORY(arena_overflow_creates_chain, TEST_CATEGORY_MEMORY) {
    Arena* arena = arena_create(64);
    
    void* p1 = arena_alloc(arena, 32);
    void* p2 = arena_alloc(arena, 32);
    void* p3 = arena_alloc(arena, 32);
    
    ASSERT_NOT_NULL(p1);
    ASSERT_NOT_NULL(p2);
    ASSERT_NOT_NULL(p3);
    
    ASSERT_NOT_NULL(arena->next);
    
    arena_destroy(arena);
}

TEST_CATEGORY(arena_large_allocation, TEST_CATEGORY_MEMORY) {
    Arena* arena = arena_create(1024);
    
    void* large = arena_alloc(arena, 2048);
    ASSERT_NOT_NULL(large);
    
    ASSERT_NOT_NULL(arena->next);
    
    arena_destroy(arena);
}

TEST_CATEGORY(arena_alignment, TEST_CATEGORY_MEMORY) {
    Arena* arena = arena_create(1024);
    
    void* p1 = arena_alloc(arena, 1);
    void* p2 = arena_alloc(arena, 1);
    
    ASSERT_NOT_NULL(p1);
    ASSERT_NOT_NULL(p2);
    
    size_t addr1 = (size_t)p1;
    size_t addr2 = (size_t)p2;
    
    ASSERT_EQ(0, addr1 % 8);
    ASSERT_EQ(0, addr2 % 8);
    
    arena_destroy(arena);
}

TEST_CATEGORY(arena_string_operations, TEST_CATEGORY_MEMORY) {
    Arena* arena = arena_create(1024);
    
    char* str1 = (char*)arena_alloc(arena, 32);
    strcpy(str1, "Hello");
    
    char* str2 = (char*)arena_alloc(arena, 32);
    strcpy(str2, "World");
    
    ASSERT_STREQ("Hello", str1);
    ASSERT_STREQ("World", str2);
    
    arena_destroy(arena);
}

TEST_CATEGORY(arena_zero_size_defaults, TEST_CATEGORY_MEMORY) {
    Arena* arena = arena_create(0);
    ASSERT_NOT_NULL(arena);
    ASSERT_TRUE(arena_get_size(arena) > 0);
    arena_destroy(arena);
}

TEST_CATEGORY(arena_nested_scopes, TEST_CATEGORY_MEMORY) {
    Arena* arena = arena_create(1024);
    
    ArenaScope outer = arena_begin(arena);
    arena_alloc(arena, 100);
    
    ArenaScope inner = arena_begin(arena);
    arena_alloc(arena, 100);
    size_t inner_used = arena_get_used(arena);
    
    arena_end(inner);
    size_t outer_used = arena_get_used(arena);
    ASSERT_TRUE(outer_used < inner_used);
    
    arena_end(outer);
    ASSERT_EQ(0, arena_get_used(arena));
    
    arena_destroy(arena);
}

// Note: Tests are auto-registered via TEST_CATEGORY macro
void register_memory_arena_tests(void) {
    // Empty - tests registered by constructor
}
