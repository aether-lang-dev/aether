#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../compiler/parser/lexer.h"
#include "../../compiler/parser/parser.h"
#include "../../compiler/ast.h"
#include "../../compiler/analysis/typechecker.h"
#include "../../compiler/codegen/codegen.h"

// Helper function to create parser with error suppression
static Parser* create_test_parser(Token** tokens, int token_count) {
    Parser* parser = create_parser(tokens, token_count);
    parser->suppress_errors = 1;  // Suppress parse errors during testing
    return parser;
}

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return 0; \
    } \
} while(0)

#define ASSERT_FALSE(cond) do { \
    if (cond) { \
        fprintf(stderr, "FAIL: %s:%d: !(%s)\n", __FILE__, __LINE__, #cond); \
        return 0; \
    } \
} while(0)

#define ASSERT_EQUAL(a, b) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "FAIL: %s:%d: %s != %s (%d != %d)\n", __FILE__, __LINE__, #a, #b, (int)(a), (int)(b)); \
        return 0; \
    } \
} while(0)

// Test array literal parsing
int test_array_literal_parsing(void) {
    const char* code = "main() { let arr = [1, 2, 3]; }";
    
    lexer_init(code);
    Token* tokens[100];
    int token_count = 0;
    
    Token* tok;
    while ((tok = next_token()) && tok->type != TOKEN_EOF && token_count < 100) {
        tokens[token_count++] = tok;
    }
    
    Parser* parser = create_test_parser(tokens, token_count);
    ASTNode* program = parse_program(parser);
    
    ASSERT_TRUE(program != NULL);
    ASSERT_TRUE(program->child_count > 0);
    
    // Find main function
    ASTNode* main_func = program->children[0];
    ASSERT_EQUAL(main_func->type, AST_MAIN_FUNCTION);
    
    // Find block
    ASTNode* block = main_func->children[0];
    ASSERT_EQUAL(block->type, AST_BLOCK);
    
    // Find variable declaration
    ASTNode* var_decl = block->children[0];
    ASSERT_EQUAL(var_decl->type, AST_VARIABLE_DECLARATION);
    
    // Find array literal
    ASTNode* array_lit = var_decl->children[0];
    ASSERT_EQUAL(array_lit->type, AST_ARRAY_LITERAL);
    ASSERT_EQUAL(array_lit->child_count, 3);  // [1, 2, 3]
    
    free_ast_node(program);
    free_parser(parser);
    
    return 1;
}

// Test array indexing parsing
int test_array_indexing_parsing(void) {
    const char* code = "main() { let x = arr[0]; }";
    
    lexer_init(code);
    Token* tokens[100];
    int token_count = 0;
    
    Token* tok;
    while ((tok = next_token()) && tok->type != TOKEN_EOF && token_count < 100) {
        tokens[token_count++] = tok;
    }
    
    Parser* parser = create_test_parser(tokens, token_count);
    parser->suppress_errors = 1;  // Suppress parse errors during testing
    ASTNode* program = parse_program(parser);
    
    ASSERT_TRUE(program != NULL);
    
    // Navigate to array access
    ASTNode* main_func = program->children[0];
    ASTNode* block = main_func->children[0];
    ASTNode* var_decl = block->children[0];
    ASTNode* array_access = var_decl->children[0];
    
    ASSERT_EQUAL(array_access->type, AST_ARRAY_ACCESS);
    ASSERT_EQUAL(array_access->child_count, 2);  // array and index
    
    free_ast_node(program);
    free_parser(parser);
    
    return 1;
}

// Test fixed array type parsing
int test_fixed_array_type(void) {
    const char* code = "main() { let nums = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0]; }";
    
    lexer_init(code);
    Token* tokens[100];
    int token_count = 0;
    
    Token* tok;
    while ((tok = next_token()) && tok->type != TOKEN_EOF && token_count < 100) {
        tokens[token_count++] = tok;
    }
    
    Parser* parser = create_test_parser(tokens, token_count);
    ASTNode* program = parse_program(parser);
    
    ASSERT_TRUE(program != NULL);
    
    // Navigate to variable declaration
    ASTNode* main_func = program->children[0];
    ASTNode* block = main_func->children[0];
    ASTNode* var_decl = block->children[0];
    
    ASSERT_EQUAL(var_decl->type, AST_VARIABLE_DECLARATION);
    
    // Check the array literal
    ASTNode* array_lit = var_decl->children[0];
    ASSERT_EQUAL(array_lit->type, AST_ARRAY_LITERAL);
    ASSERT_EQUAL(array_lit->child_count, 10);
    
    free_ast_node(program);
    free_parser(parser);
    
    return 1;
}

// Test make() for dynamic arrays
int test_make_dynamic_array(void) {
    const char* code = "main() { let buf = make([]int, 100); }";
    
    lexer_init(code);
    Token* tokens[100];
    int token_count = 0;
    
    Token* tok;
    while ((tok = next_token()) && tok->type != TOKEN_EOF && token_count < 100) {
        tokens[token_count++] = tok;
    }
    
    Parser* parser = create_test_parser(tokens, token_count);
    parser->suppress_errors = 1;  // Suppress parse errors during testing
    ASTNode* program = parse_program(parser);
    
    ASSERT_TRUE(program != NULL);
    
    // Navigate to make call
    ASTNode* main_func = program->children[0];
    ASTNode* block = main_func->children[0];
    ASTNode* var_decl = block->children[0];
    ASTNode* make_call = var_decl->children[0];
    
    ASSERT_EQUAL(make_call->type, AST_FUNCTION_CALL);
    ASSERT_TRUE(strcmp(make_call->value, "make") == 0);
    ASSERT_TRUE(make_call->node_type != NULL);
    ASSERT_EQUAL(make_call->node_type->kind, TYPE_ARRAY);
    ASSERT_EQUAL(make_call->node_type->array_size, -1);  // Dynamic
    
    free_ast_node(program);
    free_parser(parser);
    
    return 1;
}

// Test multi-dimensional arrays
int test_multidimensional_arrays(void) {
    const char* code = "main() { let matrix = [[1, 2, 3], [4, 5, 6], [7, 8, 9]]; }";
    
    lexer_init(code);
    Token* tokens[100];
    int token_count = 0;
    
    Token* tok;
    while ((tok = next_token()) && tok->type != TOKEN_EOF && token_count < 100) {
        tokens[token_count++] = tok;
    }
    
    Parser* parser = create_test_parser(tokens, token_count);
    ASTNode* program = parse_program(parser);
    
    ASSERT_TRUE(program != NULL);
    
    // Navigate to variable declaration
    ASTNode* main_func = program->children[0];
    ASTNode* block = main_func->children[0];
    ASTNode* var_decl = block->children[0];
    
    ASSERT_EQUAL(var_decl->type, AST_VARIABLE_DECLARATION);
    
    // Check the outer array literal
    ASTNode* outer_array = var_decl->children[0];
    ASSERT_EQUAL(outer_array->type, AST_ARRAY_LITERAL);
    ASSERT_EQUAL(outer_array->child_count, 3);
    
    // Check that first element is also an array
    ASTNode* inner_array = outer_array->children[0];
    ASSERT_EQUAL(inner_array->type, AST_ARRAY_LITERAL);
    ASSERT_EQUAL(inner_array->child_count, 3);
    
    free_ast_node(program);
    free_parser(parser);
    
    return 1;
}

int main(void) {
    int passed = 0;
    int total = 0;
    
    printf("Running Array Tests...\n");
    printf("=====================\n\n");
    
    #define RUN_TEST(test) do { \
        total++; \
        printf("Running %s... ", #test); \
        if (test()) { \
            printf("PASS\n"); \
            passed++; \
        } else { \
            printf("FAIL\n"); \
        } \
    } while(0)
    
    RUN_TEST(test_array_literal_parsing);
    RUN_TEST(test_array_indexing_parsing);
    RUN_TEST(test_fixed_array_type);
    RUN_TEST(test_make_dynamic_array);
    RUN_TEST(test_multidimensional_arrays);
    
    printf("\n=====================\n");
    printf("Results: %d/%d tests passed\n", passed, total);
    
    return (passed == total) ? 0 : 1;
}

