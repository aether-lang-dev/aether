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

/* #2209: a module's merged symbols are prefixed with its namespace. That is
 * the last path segment unless another loaded module ends the same way, in
 * which case a local module gets its full path with dots as underscores, so
 * `mine.vk` and `gfx.vk` never share `vk_<name>`. #2190: a shipped module
 * (`std.*`, `contrib.*`) keeps its last segment whatever collides with it,
 * because its externs are compiled under that prefix (`math_floor`). */

static AetherModule* ns_register(const char* name) {
    AetherModule* m = module_create(name, "/fake.ae");
    module_register(m);
    return m;
}

TEST_CATEGORY(namespace_is_last_segment_when_unshared, TEST_CATEGORY_COMPILER) {
    module_registry_init();
    ns_register("mine.vk");
    ns_register("std.json");
    ns_register("mathmod");
    module_assign_namespaces();
    ASSERT_STREQ("vk", module_namespace_of("mine.vk"));
    ASSERT_STREQ("json", module_namespace_of("std.json"));
    ASSERT_STREQ("mathmod", module_namespace_of("mathmod"));
    module_registry_shutdown();
}

TEST_CATEGORY(namespace_is_full_path_when_last_segment_shared, TEST_CATEGORY_COMPILER) {
    module_registry_init();
    ns_register("mine.vk");
    ns_register("gfx.vk");
    ns_register("std.json");
    module_assign_namespaces();
    ASSERT_STREQ("mine_vk", module_namespace_of("mine.vk"));
    ASSERT_STREQ("gfx_vk", module_namespace_of("gfx.vk"));
    ASSERT_STREQ("json", module_namespace_of("std.json"));
    module_registry_shutdown();
}

/* #2190: `noise` imports std.math and calls `math.floor`; the program also
 * imports its own `pkg.math`. std.math's `math_floor` is a runtime extern,
 * so std.math must keep `math` and pkg.math is the one that moves. */
TEST_CATEGORY(namespace_std_module_keeps_leaf_when_local_module_collides, TEST_CATEGORY_COMPILER) {
    module_registry_init();
    ns_register("pkg.math");
    ns_register("std.math");
    module_assign_namespaces();
    ASSERT_STREQ("math", module_namespace_of("std.math"));
    ASSERT_STREQ("pkg_math", module_namespace_of("pkg.math"));
    module_registry_shutdown();
}

TEST_CATEGORY(namespace_contrib_module_keeps_leaf_when_local_module_collides, TEST_CATEGORY_COMPILER) {
    module_registry_init();
    ns_register("contrib.vulkan.vk");
    ns_register("mine.vk");
    module_assign_namespaces();
    ASSERT_STREQ("vk", module_namespace_of("contrib.vulkan.vk"));
    ASSERT_STREQ("mine_vk", module_namespace_of("mine.vk"));
    module_registry_shutdown();
}

/* Registration order does not decide who keeps the leaf. */
TEST_CATEGORY(namespace_std_module_keeps_leaf_whichever_loads_first, TEST_CATEGORY_COMPILER) {
    module_registry_init();
    ns_register("std.math");
    ns_register("pkg.math");
    ns_register("other.math");
    module_assign_namespaces();
    ASSERT_STREQ("math", module_namespace_of("std.math"));
    ASSERT_STREQ("pkg_math", module_namespace_of("pkg.math"));
    ASSERT_STREQ("other_math", module_namespace_of("other.math"));
    module_registry_shutdown();
}

TEST_CATEGORY(namespace_of_unregistered_path_is_last_segment, TEST_CATEGORY_COMPILER) {
    module_registry_init();
    module_assign_namespaces();
    ASSERT_STREQ("client", module_namespace_of("std.http.client"));
    ASSERT_STREQ("solo", module_namespace_of("solo"));
    module_registry_shutdown();
}

TEST_CATEGORY(module_find_by_namespace_prefers_assigned_namespace, TEST_CATEGORY_COMPILER) {
    module_registry_init();
    AetherModule* first = ns_register("gfx.vk");
    AetherModule* second = ns_register("mine.vk");
    AetherModule* json = ns_register("std.json");
    module_assign_namespaces();
    ASSERT_TRUE(module_find_by_namespace("mine_vk") == second);
    ASSERT_TRUE(module_find_by_namespace("gfx_vk") == first);
    ASSERT_TRUE(module_find_by_namespace("json") == json);
    ASSERT_TRUE(module_find_by_namespace("std.json") == json);
    /* A bare shared leaf still finds a module, the first registered, for
     * a use that was never rewritten. */
    ASSERT_TRUE(module_find_by_namespace("vk") == first);
    ASSERT_NULL(module_find_by_namespace("nope"));
    module_registry_shutdown();
}

/* #2190: the leaf a shipped module kept is ITS namespace, so `math` finds
 * std.math even when pkg.math registered first. */
TEST_CATEGORY(module_find_by_namespace_leaf_finds_shipped_module_over_renamed_local, TEST_CATEGORY_COMPILER) {
    module_registry_init();
    AetherModule* local = ns_register("pkg.math");
    AetherModule* std_math = ns_register("std.math");
    module_assign_namespaces();
    ASSERT_TRUE(module_find_by_namespace("math") == std_math);
    ASSERT_TRUE(module_find_by_namespace("pkg_math") == local);
    module_registry_shutdown();
}

/* #2255: a module registered WITH its source: one `extern <name>`
 * declaration, or (extern_name NULL) one Aether function. Whether a module
 * keeps its leaf is read from these, not from the std./contrib. prefix. */
static AetherModule* ns_register_with(const char* name, const char* extern_name) {
    AetherModule* m = ns_register(name);
    ASTNode* mod = create_ast_node(AST_PROGRAM, NULL, 0, 0);
    if (extern_name) {
        add_child(mod, create_ast_node(AST_EXTERN_FUNCTION, extern_name, 0, 0));
    } else {
        ASTNode* fn = create_ast_node(AST_FUNCTION_DEFINITION, "helper", 0, 0);
        add_child(fn, create_ast_node(AST_BLOCK, NULL, 0, 0));
        add_child(mod, fn);
    }
    m->ast = mod;
    return m;
}

/* #2255: std.jsonpath and contrib.jq each have a pure-Aether `parser`.
 * Neither's calls are bound to a `parser_` C symbol, so both take their
 * full path rather than share `parser`. */
TEST_CATEGORY(namespace_pure_aether_shipped_modules_sharing_a_leaf_both_move, TEST_CATEGORY_COMPILER) {
    module_registry_init();
    ns_register_with("std.jsonpath.parser", NULL);
    ns_register_with("contrib.jq.parser", NULL);
    module_assign_namespaces();
    ASSERT_STREQ("std_jsonpath_parser", module_namespace_of("std.jsonpath.parser"));
    ASSERT_STREQ("contrib_jq_parser", module_namespace_of("contrib.jq.parser"));
    module_registry_shutdown();
}

/* #2190 read from the source: std.math's `extern math_floor` is what ties it
 * to `math`, and a local module with the same kind of externs is tied the
 * same way; the other module in each pair moves. */
TEST_CATEGORY(namespace_module_with_leaf_prefixed_externs_keeps_leaf, TEST_CATEGORY_COMPILER) {
    module_registry_init();
    ns_register_with("pkg.math", NULL);
    ns_register_with("std.math", "math_floor");
    ns_register_with("mylib.gfx", "gfx_init");
    ns_register_with("other.gfx", NULL);
    module_assign_namespaces();
    ASSERT_STREQ("math", module_namespace_of("std.math"));
    ASSERT_STREQ("pkg_math", module_namespace_of("pkg.math"));
    ASSERT_STREQ("gfx", module_namespace_of("mylib.gfx"));
    ASSERT_STREQ("other_gfx", module_namespace_of("other.gfx"));
    module_registry_shutdown();
}

/* Only a `<leaf>_` extern ties a module to its leaf: contrib.jq.value's
 * externs are `aether_jq_*`, and `valuer_x` merely starts with the leaf. */
TEST_CATEGORY(namespace_extern_without_leaf_prefix_does_not_pin, TEST_CATEGORY_COMPILER) {
    module_registry_init();
    ns_register_with("contrib.jq.value", "aether_jq_num_to_str");
    ns_register_with("std.cfg.value", "valuer_x");
    module_assign_namespaces();
    ASSERT_STREQ("contrib_jq_value", module_namespace_of("contrib.jq.value"));
    ASSERT_STREQ("std_cfg_value", module_namespace_of("std.cfg.value"));
    module_registry_shutdown();
}

/* A module AST holding one function `fn_name` with an empty body. */
static ASTNode* ns_module_ast(const char* fn_name) {
    ASTNode* mod = create_ast_node(AST_PROGRAM, NULL, 0, 0);
    ASTNode* fn = create_ast_node(AST_FUNCTION_DEFINITION, fn_name, 0, 0);
    add_child(fn, create_ast_node(AST_BLOCK, NULL, 0, 0));
    add_child(mod, fn);
    return mod;
}

/* `import <path>` (optionally `as <alias>`) as the parser shapes it. */
static ASTNode* ns_import(const char* path, const char* alias) {
    ASTNode* imp = create_ast_node(AST_IMPORT_STATEMENT, path, 0, 0);
    if (alias) {
        ASTNode* a = create_ast_node(AST_IDENTIFIER, alias, 0, 0);
        a->annotation = strdup("module_alias");
        add_child(imp, a);
    }
    return imp;
}

/* main() { <call> } where the call is written `<qualified>()`; returns the
 * call node so a test can read its rewritten name. */
static ASTNode* ns_main_calling(const char* qualified, ASTNode** call_out) {
    ASTNode* call = create_ast_node(AST_FUNCTION_CALL, qualified, 0, 0);
    ASTNode* block = create_ast_node(AST_BLOCK, NULL, 0, 0);
    add_child(block, call);
    ASTNode* mainfn = create_ast_node(AST_MAIN_FUNCTION, "main", 0, 0);
    add_child(mainfn, block);
    *call_out = call;
    return mainfn;
}

static int ns_program_has_function(ASTNode* program, const char* name) {
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_FUNCTION_DEFINITION && c->value &&
            strcmp(c->value, name) == 0) return 1;
    }
    return 0;
}

TEST_CATEGORY(merge_rewrites_program_alias_to_module_namespace, TEST_CATEGORY_COMPILER) {
    module_registry_init();
    AetherModule* m = ns_register("mine.vk");
    m->ast = ns_module_ast("mine");
    module_assign_namespaces();

    ASTNode* program = create_ast_node(AST_PROGRAM, NULL, 0, 0);
    add_child(program, ns_import("mine.vk", "ownvk"));
    ASTNode* call = NULL;
    add_child(program, ns_main_calling("ownvk.mine", &call));

    module_merge_into_program(program);

    /* An alias-only import is not a selection: the module's functions merge. */
    ASSERT_TRUE(ns_program_has_function(program, "vk_mine"));
    ASSERT_STREQ("vk.mine", call->value);

    free_ast_node(program);
    module_registry_shutdown();
}

TEST_CATEGORY(merge_rewrites_shared_leaf_to_full_path_namespace, TEST_CATEGORY_COMPILER) {
    module_registry_init();
    AetherModule* mine = ns_register("mine.vk");
    mine->ast = ns_module_ast("mine");
    AetherModule* gfx = ns_register("gfx.vk");
    gfx->ast = ns_module_ast("describe");
    module_assign_namespaces();

    ASTNode* program = create_ast_node(AST_PROGRAM, NULL, 0, 0);
    add_child(program, ns_import("mine.vk", NULL));
    ASTNode* call = NULL;
    add_child(program, ns_main_calling("vk.mine", &call));

    module_merge_into_program(program);

    ASSERT_TRUE(ns_program_has_function(program, "mine_vk_mine"));
    ASSERT_FALSE(ns_program_has_function(program, "vk_mine"));
    ASSERT_STREQ("mine_vk.mine", call->value);

    free_ast_node(program);
    module_registry_shutdown();
}

TEST_CATEGORY(merge_rewrites_a_library_import_inside_its_cloned_body, TEST_CATEGORY_COMPILER) {
    module_registry_init();
    AetherModule* mine = ns_register("mine.vk");
    mine->ast = ns_module_ast("mine");
    AetherModule* gfx = ns_register("gfx.vk");
    gfx->ast = ns_module_ast("describe");
    /* Library `pb` imports gfx.vk as `vkapi` and calls `vkapi.describe()`. */
    AetherModule* pb = ns_register("pb");
    pb->ast = create_ast_node(AST_PROGRAM, NULL, 0, 0);
    add_child(pb->ast, ns_import("gfx.vk", "vkapi"));
    ASTNode* pb_fn = create_ast_node(AST_FUNCTION_DEFINITION, "name", 0, 0);
    ASTNode* pb_body = create_ast_node(AST_BLOCK, NULL, 0, 0);
    add_child(pb_body, create_ast_node(AST_FUNCTION_CALL, "vkapi.describe", 0, 0));
    add_child(pb_fn, pb_body);
    add_child(pb->ast, pb_fn);
    module_assign_namespaces();

    ASTNode* program = create_ast_node(AST_PROGRAM, NULL, 0, 0);
    add_child(program, ns_import("mine.vk", NULL));
    add_child(program, ns_import("pb", NULL));
    ASTNode* call = NULL;
    add_child(program, ns_main_calling("pb.name", &call));

    module_merge_into_program(program);

    ASSERT_TRUE(ns_program_has_function(program, "pb_name"));
    ASTNode* merged = NULL;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_FUNCTION_DEFINITION && c->value &&
            strcmp(c->value, "pb_name") == 0) merged = c;
    }
    ASSERT_NOT_NULL(merged);
    ASSERT_STREQ("gfx_vk.describe", merged->children[0]->children[0]->value);
    /* The library's own source is untouched: only the clone is rewritten. */
    ASSERT_STREQ("vkapi.describe", pb_body->children[0]->value);

    free_ast_node(program);
    module_registry_shutdown();
}
