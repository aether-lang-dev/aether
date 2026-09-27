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

/* #2218: module_prune_unreachable seeds every bare identifier as a possible
 * function reference, and a bare name reaches an imported `<mod>_<name>`
 * through the suffix match that glob imports rely on. A name the function
 * binds itself (a local, a parameter, a closure parameter) is that binding,
 * so it must not keep an unrelated imported function alive. */

static ASTNode* prune_node(ASTNodeType type, const char* value, ASTNode* child) {
    ASTNode* n = create_ast_node(type, value, 0, 0);
    if (child) add_child(n, child);
    return n;
}

/* An imported function or builder, as module merging clones it in. */
static ASTNode* prune_imported(ASTNodeType type, const char* name, ASTNode* body_stmt) {
    ASTNode* fn = create_ast_node(type, name, 0, 0);
    fn->is_imported = 1;
    add_child(fn, prune_node(AST_BLOCK, NULL, body_stmt));
    return fn;
}

/* main() { <stmt> } */
static ASTNode* prune_main(ASTNode* stmt) {
    return prune_node(AST_MAIN_FUNCTION, "main", prune_node(AST_BLOCK, NULL, stmt));
}

static int prune_has(ASTNode* program, const char* name) {
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->value && strcmp(c->value, name) == 0) return 1;
    }
    return 0;
}

/* glyphs_find() { record = 1; return record } */
static ASTNode* prune_fn_with_local_named(const char* fn_name, const char* local) {
    ASTNode* fn = prune_imported(AST_FUNCTION_DEFINITION, fn_name,
        prune_node(AST_VARIABLE_DECLARATION, local, prune_node(AST_LITERAL, "1", NULL)));
    add_child(fn->children[0],
              prune_node(AST_RETURN_STATEMENT, NULL, prune_node(AST_IDENTIFIER, local, NULL)));
    return fn;
}

TEST_CATEGORY(prune_drops_imported_fn_named_like_a_local, TEST_CATEGORY_COMPILER) {
    ASTNode* program = create_ast_node(AST_PROGRAM, NULL, 0, 0);
    add_child(program, prune_main(prune_node(AST_FUNCTION_CALL, "glyphs.find", NULL)));
    add_child(program, prune_fn_with_local_named("glyphs_find", "record"));
    add_child(program, prune_imported(AST_BUILDER_FUNCTION, "ui_record", NULL));

    module_prune_unreachable(program);

    ASSERT_TRUE(prune_has(program, "glyphs_find"));
    ASSERT_FALSE(prune_has(program, "ui_record"));
    free_ast_node(program);
}

TEST_CATEGORY(prune_keeps_glob_imported_fn_named_by_bare_identifier, TEST_CATEGORY_COMPILER) {
    /* main() { f = cube } with `import mathlist (*)`: the bare name is the
     * function, and the suffix match is what keeps `mathlist_cube`. */
    ASTNode* program = create_ast_node(AST_PROGRAM, NULL, 0, 0);
    add_child(program, prune_main(
        prune_node(AST_VARIABLE_DECLARATION, "f", prune_node(AST_IDENTIFIER, "cube", NULL))));
    add_child(program, prune_imported(AST_FUNCTION_DEFINITION, "mathlist_cube", NULL));

    module_prune_unreachable(program);

    ASSERT_TRUE(prune_has(program, "mathlist_cube"));
    free_ast_node(program);
}

TEST_CATEGORY(prune_drops_imported_fn_named_like_a_parameter, TEST_CATEGORY_COMPILER) {
    /* glyphs_find(record) { return record } */
    ASTNode* program = create_ast_node(AST_PROGRAM, NULL, 0, 0);
    add_child(program, prune_main(prune_node(AST_FUNCTION_CALL, "glyphs_find", NULL)));
    ASTNode* find = create_ast_node(AST_FUNCTION_DEFINITION, "glyphs_find", 0, 0);
    find->is_imported = 1;
    add_child(find, prune_node(AST_PATTERN_VARIABLE, "record", NULL));
    add_child(find, prune_node(AST_BLOCK, NULL,
        prune_node(AST_RETURN_STATEMENT, NULL, prune_node(AST_IDENTIFIER, "record", NULL))));
    add_child(program, find);
    add_child(program, prune_imported(AST_FUNCTION_DEFINITION, "ui_record", NULL));

    module_prune_unreachable(program);

    ASSERT_FALSE(prune_has(program, "ui_record"));
    free_ast_node(program);
}

TEST_CATEGORY(prune_drops_imported_fn_named_like_a_called_local, TEST_CATEGORY_COMPILER) {
    /* main() { f = |k| { ... }; f(1) }: calling a local closure is not a
     * call of any imported `<mod>_f`. */
    ASTNode* program = create_ast_node(AST_PROGRAM, NULL, 0, 0);
    ASTNode* main_fn = prune_main(
        prune_node(AST_VARIABLE_DECLARATION, "f",
            prune_node(AST_CLOSURE, NULL, prune_node(AST_CLOSURE_PARAM, "k", NULL))));
    add_child(main_fn->children[0], prune_node(AST_FUNCTION_CALL, "f", NULL));
    add_child(program, main_fn);
    add_child(program, prune_imported(AST_FUNCTION_DEFINITION, "util_f", NULL));
    add_child(program, prune_imported(AST_FUNCTION_DEFINITION, "util_k", NULL));

    module_prune_unreachable(program);

    ASSERT_FALSE(prune_has(program, "util_f"));
    ASSERT_FALSE(prune_has(program, "util_k"));
    free_ast_node(program);
}

TEST_CATEGORY(prune_local_in_one_function_does_not_hide_a_reference_in_another, TEST_CATEGORY_COMPILER) {
    /* glyphs_find binds `record`; main names the glob-imported `record`
     * function as a value. Locals are per function, so main's reference
     * still keeps `ui_record`. */
    ASTNode* program = create_ast_node(AST_PROGRAM, NULL, 0, 0);
    ASTNode* main_fn = prune_main(prune_node(AST_FUNCTION_CALL, "glyphs_find", NULL));
    add_child(main_fn->children[0],
              prune_node(AST_VARIABLE_DECLARATION, "cb", prune_node(AST_IDENTIFIER, "record", NULL)));
    add_child(program, main_fn);
    add_child(program, prune_fn_with_local_named("glyphs_find", "record"));
    add_child(program, prune_imported(AST_BUILDER_FUNCTION, "ui_record", NULL));

    module_prune_unreachable(program);

    ASSERT_TRUE(prune_has(program, "ui_record"));
    free_ast_node(program);
}
