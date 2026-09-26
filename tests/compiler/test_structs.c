#include <stdio.h>
#include <string.h>
#include "../runtime/test_harness.h"
#include "../../compiler/parser/tokens.h"
#include "../../compiler/parser/lexer.h"
#include "../../compiler/ast.h"
#include "../../compiler/parser/parser.h"
#include "../../compiler/analysis/typechecker.h"

// Helper function to create parser with error suppression
static Parser* create_test_parser(Token** tokens, int token_count) {
    Parser* parser = create_parser(tokens, token_count);
    parser->suppress_errors = 1;  // Suppress parse errors during testing
    return parser;
}

// Test struct lexing
TEST(struct_keyword) {
    lexer_init("struct");
    Token* token = next_token();
    ASSERT_TRUE(token->type == TOKEN_STRUCT);
    ASSERT_TRUE(strcmp(token->value, "struct") == 0);
    free_token(token);
}

// Test struct parsing
TEST(parse_simple_struct) {
    const char* code = "struct Point { int x; int y; }";
    lexer_init(code);
    
    // Collect tokens
    Token** tokens = NULL;
    int token_count = 0;
    Token* token;
    while ((token = next_token())->type != TOKEN_EOF) {
        tokens = realloc(tokens, (token_count + 1) * sizeof(Token*));
        tokens[token_count++] = token;
    }
    tokens = realloc(tokens, (token_count + 1) * sizeof(Token*));
    tokens[token_count++] = token;
    
    // Parse
    Parser* parser = create_parser(tokens, token_count);
    parser->suppress_errors = 1;  // Suppress parse errors during testing
    ASTNode* struct_def = parse_struct_definition(parser);
    
    ASSERT_TRUE(struct_def != NULL);
    ASSERT_TRUE(struct_def->type == AST_STRUCT_DEFINITION);
    ASSERT_TRUE(strcmp(struct_def->value, "Point") == 0);
    ASSERT_TRUE(struct_def->child_count == 2); // x and y fields
    
    // Check fields
    ASSERT_TRUE(struct_def->children[0]->type == AST_STRUCT_FIELD);
    ASSERT_TRUE(strcmp(struct_def->children[0]->value, "x") == 0);
    ASSERT_TRUE(struct_def->children[0]->node_type->kind == TYPE_INT);
    
    ASSERT_TRUE(struct_def->children[1]->type == AST_STRUCT_FIELD);
    ASSERT_TRUE(strcmp(struct_def->children[1]->value, "y") == 0);
    ASSERT_TRUE(struct_def->children[1]->node_type->kind == TYPE_INT);
    
    free_ast_node(struct_def);
    free_parser(parser);
    for (int i = 0; i < token_count; i++) {
        free_token(tokens[i]);
    }
    free(tokens);
}

// Test struct type checking
TEST(typecheck_struct) {
    const char* code = "struct Player { int health; int score; }";
    lexer_init(code);
    
    Token** tokens = NULL;
    int token_count = 0;
    Token* token;
    while ((token = next_token())->type != TOKEN_EOF) {
        tokens = realloc(tokens, (token_count + 1) * sizeof(Token*));
        tokens[token_count++] = token;
    }
    tokens = realloc(tokens, (token_count + 1) * sizeof(Token*));
    tokens[token_count++] = token;
    
    Parser* parser = create_parser(tokens, token_count);
    ASTNode* struct_def = parse_struct_definition(parser);
    
    SymbolTable* table = create_symbol_table(NULL);
    int result = typecheck_struct_definition(struct_def, table);
    
    ASSERT_TRUE(result == 1); // Should succeed
    
    free_symbol_table(table);
    free_ast_node(struct_def);
    free_parser(parser);
    for (int i = 0; i < token_count; i++) {
        free_token(tokens[i]);
    }
    free(tokens);
}

// Test duplicate field detection
TEST(duplicate_field_detection) {
    const char* code = "struct Bad { int x; int x; }";
    lexer_init(code);
    
    Token** tokens = NULL;
    int token_count = 0;
    Token* token;
    while ((token = next_token())->type != TOKEN_EOF) {
        tokens = realloc(tokens, (token_count + 1) * sizeof(Token*));
        tokens[token_count++] = token;
    }
    tokens = realloc(tokens, (token_count + 1) * sizeof(Token*));
    tokens[token_count++] = token;
    
    Parser* parser = create_parser(tokens, token_count);
    ASTNode* struct_def = parse_struct_definition(parser);
    
    SymbolTable* table = create_symbol_table(NULL);
    int result = typecheck_struct_definition(struct_def, table);
    
    ASSERT_TRUE(result == 0); // Should fail due to duplicate field
    
    free_symbol_table(table);
    free_ast_node(struct_def);
    free_parser(parser);
    for (int i = 0; i < token_count; i++) {
        free_token(tokens[i]);
    }
    free(tokens);
}

// Registration is the TEST() macro's constructor; the harness owns main().

/* #2220 — `struct Name @observable { ... }` carries the marker codegen keys
 * on; any other attribute in that position is a parse error. */
static ASTNode* parse_struct_source(const char* code, Token*** out_tokens,
                                    int* out_count, Parser** out_parser) {
    lexer_init(code);
    Token** tokens = NULL;
    int token_count = 0;
    Token* token;
    while ((token = next_token())->type != TOKEN_EOF) {
        tokens = realloc(tokens, (token_count + 1) * sizeof(Token*));
        tokens[token_count++] = token;
    }
    tokens = realloc(tokens, (token_count + 1) * sizeof(Token*));
    tokens[token_count++] = token;
    Parser* parser = create_parser(tokens, token_count);
    parser->suppress_errors = 1;
    *out_tokens = tokens;
    *out_count = token_count;
    *out_parser = parser;
    return parse_struct_definition(parser);
}

static void free_struct_source(ASTNode* sd, Token** tokens, int count, Parser* parser) {
    if (sd) free_ast_node(sd);
    free_parser(parser);
    for (int i = 0; i < count; i++) free_token(tokens[i]);
    free(tokens);
}

TEST(parse_observable_struct_attribute) {
    Token** tokens; int count; Parser* parser;
    ASTNode* sd = parse_struct_source("struct Model @observable { count: int, status: string }",
                                      &tokens, &count, &parser);
    ASSERT_NOT_NULL(sd);
    ASSERT_TRUE(strcmp(sd->value, "Model") == 0);
    ASSERT_EQ(2, sd->child_count);
    ASSERT_TRUE(annotation_has_marker(sd->annotation, "observable"));
    free_struct_source(sd, tokens, count, parser);
}

TEST(parse_plain_struct_has_no_observable_marker) {
    Token** tokens; int count; Parser* parser;
    ASTNode* sd = parse_struct_source("struct Model { count: int }", &tokens, &count, &parser);
    ASSERT_NOT_NULL(sd);
    ASSERT_FALSE(annotation_has_marker(sd->annotation, "observable"));
    free_struct_source(sd, tokens, count, parser);
}

TEST(parse_unknown_struct_attribute_is_an_error) {
    Token** tokens; int count; Parser* parser;
    ASTNode* sd = parse_struct_source("struct Model @packed { count: int }", &tokens, &count, &parser);
    ASSERT_NULL(sd);
    free_struct_source(sd, tokens, count, parser);
}
