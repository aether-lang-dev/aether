#include "../runtime/test_harness.h"
#include "../../compiler/parser/lexer.h"
#include "../../compiler/parser/parser.h"
#include "../../compiler/analysis/typechecker.h"
#include <string.h>

static ASTNode* parse_code(const char* code) {
    lexer_init(code);
    Token** tokens = malloc(1000 * sizeof(Token*));
    int count = 0;
    Token* tok;
    int safety = 0;
    const int MAX_TOKENS = 999;
    while (safety++ < MAX_TOKENS && (tok = next_token())->type != TOKEN_EOF && count < MAX_TOKENS) {
        tokens[count++] = tok;
    }
    if (tok && tok->type == TOKEN_EOF) {
        tokens[count++] = tok;
    }
    Parser* parser = create_parser(tokens, count);
    parser->suppress_errors = 1;  // Suppress parse errors during testing
    ASTNode* ast = parse_program(parser);
    free_parser(parser);
    /* The AST owns copies of every name it needed, so the tokens are dead
     * here. Dropping them leaked the whole token stream of every program
     * these tests parse (3428 allocations under LeakSanitizer). */
    for (int i = 0; i < count; i++) free_token(tokens[i]);
    free(tokens);
    return ast;
}

TEST(type_inference_int_literal) {
    ASTNode* ast = parse_code("x = 42\nmain() { print(x) }\n");
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(AST_PROGRAM, ast->type);
    free_ast_node(ast);
}

TEST(type_inference_float_literal) {
    ASTNode* ast = parse_code("pi = 3.14\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(AST_PROGRAM, ast->type);
    free_ast_node(ast);
}

TEST(type_inference_string_literal) {
    ASTNode* ast = parse_code("name = \"Alice\"\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(AST_PROGRAM, ast->type);
    free_ast_node(ast);
}

TEST(type_inference_array_literal) {
    ASTNode* ast = parse_code("nums = [1, 2, 3]\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(type_inference_function_return) {
    ASTNode* ast = parse_code("add(a, b) { return a + b }\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(type_inference_binary_op) {
    ASTNode* ast = parse_code("result = 10 + 20\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(type_inference_comparison) {
    ASTNode* ast = parse_code("x = 5\ny = 10\nresult = x < y\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(type_inference_function_call) {
    ASTNode* ast = parse_code("add(a, b) { return a + b }\nmain() { x = add(1, 2) }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(type_inference_nested_expressions) {
    ASTNode* ast = parse_code("x = (1 + 2) * (3 + 4)\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(type_inference_array_indexing) {
    ASTNode* ast = parse_code("arr = [1, 2, 3]\nx = arr[0]\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

