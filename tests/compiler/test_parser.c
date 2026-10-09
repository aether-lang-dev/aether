#include "../runtime/test_harness.h"
#include "../../compiler/parser/lexer.h"
#include "../../compiler/parser/parser.h"
#include <string.h>
#include <stdlib.h>

// Helper: tokenize + parse using current API
static ASTNode* parse_source(const char* source) {
    lexer_init(source);
    Token** tokens = malloc(sizeof(Token*) * 256);
    int count = 0;
    Token* tok;
    while ((tok = next_token()) != NULL && tok->type != TOKEN_EOF && count < 255) {
        tokens[count++] = tok;
    }
    if (tok) tokens[count++] = tok;
    Parser* parser = create_parser(tokens, count);
    ASTNode* ast = parse_program(parser);
    free_parser(parser);
    for (int i = 0; i < count; i++) free_token(tokens[i]);
    free(tokens);
    return ast;
}

TEST(parser_basic_expressions) {
    ASTNode* ast = parse_source("main() { x = 1 + 2 }");
    ASSERT_NOT_NULL(ast);
    ASSERT_TRUE(ast->child_count > 0);
    free_ast_node(ast);
}

TEST(parser_for_loops) {
    ASTNode* ast = parse_source("main() { for i = 0; i < 10; i++ { x = i } }");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_while_loops) {
    ASTNode* ast = parse_source("main() { while x > 0 { x = x - 1 } }");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

/* #2200: `cfn Name(a: T1, b: T2) -> R` declares a named C function-pointer
 * type. The node carries only the signature: parameter names are dropped,
 * `-> R` defaults to void, and a bare type list is accepted as well. */
TEST(parser_cfn_type_def_with_named_params) {
    ASTNode* ast = parse_source("cfn GenBuffers(n: int, ids: ptr)\nmain() { }");
    ASSERT_NOT_NULL(ast);
    ASSERT_TRUE(ast->child_count >= 2);
    ASTNode* def = ast->children[0];
    ASSERT_EQ(AST_CFN_TYPE_DEF, def->type);
    ASSERT_STREQ("GenBuffers", def->value);
    ASSERT_NOT_NULL(def->node_type);
    ASSERT_EQ(TYPE_FUNCTION, def->node_type->kind);
    ASSERT_EQ(1, def->node_type->is_fnptr);
    ASSERT_EQ(2, def->node_type->param_count);
    ASSERT_EQ(TYPE_INT, def->node_type->param_types[0]->kind);
    ASSERT_EQ(TYPE_PTR, def->node_type->param_types[1]->kind);
    ASSERT_EQ(TYPE_VOID, def->node_type->return_type->kind);
    free_ast_node(ast);
}

TEST(parser_cfn_type_def_bare_types_and_return) {
    ASTNode* ast = parse_source("cfn Add(int, long) -> int\nmain() { }");
    ASSERT_NOT_NULL(ast);
    ASTNode* def = ast->children[0];
    ASSERT_EQ(AST_CFN_TYPE_DEF, def->type);
    ASSERT_EQ(2, def->node_type->param_count);
    ASSERT_EQ(TYPE_INT, def->node_type->param_types[0]->kind);
    ASSERT_EQ(TYPE_INT64, def->node_type->param_types[1]->kind);
    ASSERT_EQ(TYPE_INT, def->node_type->return_type->kind);
    free_ast_node(ast);
}

TEST(parser_cfn_type_def_struct_pointer_and_export) {
    ASTNode* ast = parse_source(
        "struct Thing { value: int }\n"
        "export cfn Bump(t: *Thing, by: int) -> *Thing\n"
        "main() { }");
    ASSERT_NOT_NULL(ast);
    ASTNode* exp = ast->children[1];
    ASSERT_EQ(AST_EXPORT_STATEMENT, exp->type);
    ASSERT_TRUE(exp->child_count > 0);
    ASTNode* def = exp->children[0];
    ASSERT_EQ(AST_CFN_TYPE_DEF, def->type);
    ASSERT_STREQ("Bump", def->value);
    ASSERT_EQ(TYPE_PTR, def->node_type->param_types[0]->kind);
    ASSERT_NOT_NULL(def->node_type->param_types[0]->element_type);
    ASSERT_STREQ("Thing", def->node_type->param_types[0]->element_type->struct_name);
    ASSERT_EQ(TYPE_PTR, def->node_type->return_type->kind);
    free_ast_node(ast);
}

/* #2636: a function's return type can be a C function pointer type written
 * out. Its own `-> R` is part of the type; without one it returns nothing.
 * Each function keeps its body, and the next one still parses. */
TEST(parser_written_out_fnptr_return_type) {
    ASTNode* ast = parse_source(
        "get() -> fn(ptr) -> string { return name_of }\n"
        "put() -> fn(int) { return show }\n"
        "main() { }");
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(3, ast->child_count);
    ASTNode* get = ast->children[0];
    ASSERT_EQ(AST_FUNCTION_DEFINITION, get->type);
    ASSERT_STREQ("get", get->value);
    ASSERT_EQ(TYPE_FUNCTION, get->node_type->kind);
    ASSERT_EQ(1, get->node_type->is_fnptr);
    ASSERT_EQ(1, get->node_type->param_count);
    ASSERT_EQ(TYPE_PTR, get->node_type->param_types[0]->kind);
    ASSERT_EQ(TYPE_STRING, get->node_type->return_type->kind);
    ASSERT_EQ(AST_BLOCK, get->children[get->child_count - 1]->type);
    ASTNode* put = ast->children[1];
    ASSERT_EQ(AST_FUNCTION_DEFINITION, put->type);
    ASSERT_EQ(1, put->node_type->is_fnptr);
    ASSERT_EQ(TYPE_VOID, put->node_type->return_type->kind);
    ASSERT_EQ(AST_MAIN_FUNCTION, ast->children[2]->type);
    free_ast_node(ast);
}

/* `cfn` stays a contextual identifier: only `cfn <name> (` is the declaration. */
TEST(parser_cfn_is_still_a_plain_identifier_elsewhere) {
    ASTNode* ast = parse_source("main() { cfn = 3\n println(\"${cfn}\") }");
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(AST_MAIN_FUNCTION, ast->children[0]->type);
    free_ast_node(ast);
}

/* The alias spelling, `type Name = fn(T1, T2) -> R`, is the same declaration
 * as `cfn Name(a: T1, b: T2) -> R`: one node, one resolution. */
TEST(parser_type_alias_of_fn_signature_is_a_cfn_def) {
    ASTNode* ast = parse_source("type GenBuffers = fn(int, ptr)\ntype Add = fn(int, int) -> int\nmain() { }");
    ASSERT_NOT_NULL(ast);
    ASTNode* gen = ast->children[0];
    ASSERT_EQ(AST_CFN_TYPE_DEF, gen->type);
    ASSERT_STREQ("GenBuffers", gen->value);
    ASSERT_EQ(TYPE_FUNCTION, gen->node_type->kind);
    ASSERT_EQ(1, gen->node_type->is_fnptr);
    ASSERT_EQ(2, gen->node_type->param_count);
    ASSERT_EQ(TYPE_VOID, gen->node_type->return_type->kind);
    ASTNode* add = ast->children[1];
    ASSERT_EQ(AST_CFN_TYPE_DEF, add->type);
    ASSERT_EQ(TYPE_INT, add->node_type->return_type->kind);
    free_ast_node(ast);
}

/* A bare `fn` alias has no signature to call through, so it is refused. */
TEST(parser_type_alias_of_bare_fn_is_an_error) {
    ASTNode* ast = parse_source("type Cb = fn\nmain() { }");
    ASSERT_TRUE(ast == NULL || ast->child_count == 0 || ast->children[0]->type != AST_CFN_TYPE_DEF);
    if (ast) free_ast_node(ast);
}
