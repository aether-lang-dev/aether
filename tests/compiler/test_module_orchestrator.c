#include "../runtime/test_harness.h"
#include "../../compiler/aether_module.h"
#include "../../compiler/ast.h"

// Test: module registry init and shutdown
TEST_CATEGORY(module_registry_init_shutdown, TEST_CATEGORY_COMPILER) {
    module_registry_init();
    ASSERT_NOT_NULL(global_module_registry);
    ASSERT_EQ(0, global_module_registry->module_count);
    module_registry_shutdown();
    // After shutdown, registry should be NULL
    ASSERT_NULL(global_module_registry);
}

// Test: create, register, and find modules
TEST_CATEGORY(module_create_and_find, TEST_CATEGORY_COMPILER) {
    module_registry_init();

    AetherModule* mod = module_create("std.math", "/fake/path.ae");
    ASSERT_NOT_NULL(mod);
    ASSERT_STREQ("std.math", mod->name);
    ASSERT_STREQ("/fake/path.ae", mod->file_path);
    ASSERT_NULL(mod->ast);
    ASSERT_EQ(0, mod->export_count);
    ASSERT_EQ(0, mod->import_count);

    module_register(mod);
    ASSERT_EQ(1, global_module_registry->module_count);

    // Find registered module
    AetherModule* found = module_find("std.math");
    ASSERT_NOT_NULL(found);
    ASSERT_STREQ("std.math", found->name);

    // Missing module returns NULL
    AetherModule* missing = module_find("std.nonexistent");
    ASSERT_NULL(missing);

    module_registry_shutdown();
}

// Test: module caching (same pointer returned)
TEST_CATEGORY(module_caching_same_pointer, TEST_CATEGORY_COMPILER) {
    module_registry_init();

    AetherModule* mod = module_create("test.mod", "/fake.ae");
    module_register(mod);

    AetherModule* lookup1 = module_find("test.mod");
    AetherModule* lookup2 = module_find("test.mod");
    ASSERT_TRUE(lookup1 == lookup2);
    ASSERT_TRUE(lookup1 == mod);

    module_registry_shutdown();
}

// Test: dependency graph with no cycle
TEST_CATEGORY(dependency_graph_no_cycle, TEST_CATEGORY_COMPILER) {
    DependencyGraph* graph = dependency_graph_create();
    ASSERT_NOT_NULL(graph);

    dependency_graph_add_edge(graph, "__main__", "std.math");
    dependency_graph_add_edge(graph, "__main__", "std.string");
    dependency_graph_add_edge(graph, "std.string", "std.math");

    ASSERT_EQ(3, graph->node_count);
    ASSERT_FALSE(dependency_graph_has_cycle(graph));

    dependency_graph_free(graph);
}

// Test: dependency graph cycle detection
TEST_CATEGORY(dependency_graph_cycle_detection, TEST_CATEGORY_COMPILER) {
    DependencyGraph* graph = dependency_graph_create();

    // Create a cycle: A -> B -> C -> A
    dependency_graph_add_edge(graph, "A", "B");
    dependency_graph_add_edge(graph, "B", "C");
    ASSERT_FALSE(dependency_graph_has_cycle(graph));

    dependency_graph_add_edge(graph, "C", "A");
    ASSERT_TRUE(dependency_graph_has_cycle(graph));

    dependency_graph_free(graph);
}

// Test: dependency graph node deduplication
TEST_CATEGORY(dependency_graph_node_dedup, TEST_CATEGORY_COMPILER) {
    DependencyGraph* graph = dependency_graph_create();

    DependencyNode* n1 = dependency_graph_add_node(graph, "mod_a");
    DependencyNode* n2 = dependency_graph_add_node(graph, "mod_a");
    ASSERT_TRUE(n1 == n2);
    ASSERT_EQ(1, graph->node_count);

    dependency_graph_add_node(graph, "mod_b");
    ASSERT_EQ(2, graph->node_count);

    dependency_graph_free(graph);
}

// Test: module export tracking
TEST_CATEGORY(module_exports_tracking, TEST_CATEGORY_COMPILER) {
    module_registry_init();

    AetherModule* mod = module_create("test.exports", "/fake.ae");

    module_add_export(mod, "my_func");
    module_add_export(mod, "my_struct");
    ASSERT_EQ(2, mod->export_count);

    ASSERT_TRUE(module_is_exported(mod, "my_func"));
    ASSERT_TRUE(module_is_exported(mod, "my_struct"));
    ASSERT_FALSE(module_is_exported(mod, "private_func"));

    // Adding duplicate export should not increase count
    module_add_export(mod, "my_func");
    ASSERT_EQ(2, mod->export_count);

    module_register(mod);
    module_registry_shutdown();
}

// Test: module import tracking
TEST_CATEGORY(module_imports_tracking, TEST_CATEGORY_COMPILER) {
    module_registry_init();

    AetherModule* mod = module_create("app.main", "/fake.ae");

    module_add_import(mod, "std.math");
    module_add_import(mod, "std.string");
    ASSERT_EQ(2, mod->import_count);

    // Adding duplicate import should not increase count
    module_add_import(mod, "std.math");
    ASSERT_EQ(2, mod->import_count);

    module_register(mod);
    module_registry_shutdown();
}

// Test: module_orchestrate with empty program (no imports)
TEST_CATEGORY(module_orchestrate_empty_program, TEST_CATEGORY_COMPILER) {
    ASTNode* program = create_ast_node(AST_PROGRAM, NULL, 0, 0);
    ASSERT_NOT_NULL(program);

    int result = module_orchestrate(program);
    ASSERT_TRUE(result);
    ASSERT_NOT_NULL(global_module_registry);
    ASSERT_EQ(0, global_module_registry->module_count);

    module_registry_shutdown();
    free_ast_node(program);
}
