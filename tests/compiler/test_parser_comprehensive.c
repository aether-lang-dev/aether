#include "../runtime/test_harness.h"
#include "../../compiler/parser/lexer.h"
#include "../../compiler/parser/parser.h"
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

TEST(parser_empty_main) {
    ASTNode* ast = parse_code("main() { }\n");
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(AST_PROGRAM, ast->type);
    free_ast_node(ast);
}

TEST(parser_variable_assignment) {
    ASTNode* ast = parse_code("x = 42\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_multiple_assignments) {
    ASTNode* ast = parse_code("x = 1\ny = 2\nz = 3\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_function_no_params) {
    ASTNode* ast = parse_code("f() { return 42 }\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_function_one_param) {
    ASTNode* ast = parse_code("f(x) { return x }\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_function_multiple_params) {
    ASTNode* ast = parse_code("f(a, b, c) { return a + b + c }\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_if_no_else) {
    ASTNode* ast = parse_code("main() { if (1) { x = 2 } }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_if_with_else) {
    ASTNode* ast = parse_code("main() { if (1) { x = 2 } else { x = 3 } }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_nested_if) {
    ASTNode* ast = parse_code("main() { if (1) { if (2) { x = 3 } } }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_while_loop) {
    ASTNode* ast = parse_code("main() { while (x < 10) { x = x + 1 } }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_for_loop) {
    ASTNode* ast = parse_code("main() { for (i = 0; i < 10; i = i + 1) { } }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_array_literal) {
    ASTNode* ast = parse_code("nums = [1, 2, 3, 4, 5]\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_array_indexing) {
    ASTNode* ast = parse_code("nums = [1, 2, 3]\nx = nums[0]\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_struct_definition) {
    ASTNode* ast = parse_code("struct Point { x = 0; y = 0; }\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_struct_member_access) {
    ASTNode* ast = parse_code("struct Point { x = 0; y = 0; }\np = Point\nv = p.x\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_actor_definition) {
    ASTNode* ast = parse_code("actor Counter { state count = 0\n receive(msg) { } }\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_addition) {
    ASTNode* ast = parse_code("x = 1 + 2\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_multiplication) {
    ASTNode* ast = parse_code("x = 2 * 3\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_operator_precedence) {
    ASTNode* ast = parse_code("x = 1 + 2 * 3\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_parenthesized_expression) {
    ASTNode* ast = parse_code("x = (1 + 2) * 3\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_comparison_less) {
    ASTNode* ast = parse_code("x = 5 < 10\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_comparison_greater) {
    ASTNode* ast = parse_code("x = 10 > 5\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_comparison_equals) {
    ASTNode* ast = parse_code("x = 5 == 5\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_function_call_no_args) {
    ASTNode* ast = parse_code("f() { return 1 }\nmain() { x = f() }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_function_call_one_arg) {
    ASTNode* ast = parse_code("f(x) { return x }\nmain() { y = f(5) }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_function_call_multiple_args) {
    ASTNode* ast = parse_code("f(a, b, c) { return a }\nmain() { x = f(1, 2, 3) }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_return_statement) {
    ASTNode* ast = parse_code("f() { return 42 }\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_string_literal) {
    ASTNode* ast = parse_code("msg = \"Hello\"\nmain() { }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(parser_print_statement) {
    ASTNode* ast = parse_code("main() { print(\"Hello\") }\n");
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

