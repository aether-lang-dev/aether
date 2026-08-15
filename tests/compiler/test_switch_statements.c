#include "../runtime/test_harness.h"
#include "../../compiler/parser/lexer.h"
#include "../../compiler/parser/parser.h"
#include "../../compiler/codegen/codegen.h"
#include "../../compiler/analysis/typechecker.h"
#include <string.h>
#include <stdio.h>

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

/* The generated C starts with a multi-thousand-line runtime prelude, so a
 * fixed-size read of the first few KB never reaches the translated program:
 * an assertion against that prefix either fails for the wrong reason or
 * passes on prelude text. Read the whole stream. */
static char* read_all(FILE* f) {
    if (fseek(f, 0, SEEK_END) != 0) return NULL;
    long size = ftell(f);
    if (size < 0) return NULL;
    rewind(f);
    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf) return NULL;
    size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
    return buf;
}

TEST(switch_parse_basic) {
    const char* code = 
        "main() {\n"
        "    x = 1;\n"
        "    switch (x) {\n"
        "        case 1:\n"
        "            print(\"one\");\n"
        "        case 2:\n"
        "            print(\"two\");\n"
        "        default:\n"
        "            print(\"other\");\n"
        "    }\n"
        "}\n";
    
    ASTNode* ast = parse_code(code);
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(AST_PROGRAM, ast->type);
    
    // Find main function
    ASSERT_TRUE(ast->child_count > 0);
    ASTNode* main_func = ast->children[0];
    ASSERT_EQ(AST_MAIN_FUNCTION, main_func->type);
    
    // Find function body
    ASSERT_TRUE(main_func->child_count > 0);
    ASTNode* body = main_func->children[main_func->child_count - 1];
    
    // Find switch statement (should be second statement after x = 1)
    ASSERT_TRUE(body->child_count >= 2);
    ASTNode* switch_stmt = body->children[1];
    ASSERT_EQ(AST_SWITCH_STATEMENT, switch_stmt->type);
    
    // Switch should have: expression + cases
    ASSERT_TRUE(switch_stmt->child_count >= 2);
    
    free_ast_node(ast);
}

TEST(switch_parse_empty) {
    const char* code = 
        "main() {\n"
        "    switch (x) {\n"
        "    }\n"
        "}\n";
    
    ASTNode* ast = parse_code(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(switch_parse_single_case) {
    const char* code = 
        "main() {\n"
        "    switch (x) {\n"
        "        case 1:\n"
        "            print(\"one\");\n"
        "    }\n"
        "}\n";
    
    ASTNode* ast = parse_code(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(switch_parse_default_only) {
    const char* code = 
        "main() {\n"
        "    switch (x) {\n"
        "        default:\n"
        "            print(\"default\");\n"
        "    }\n"
        "}\n";
    
    ASTNode* ast = parse_code(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(switch_parse_multiple_statements_per_case) {
    const char* code = 
        "main() {\n"
        "    switch (x) {\n"
        "        case 1:\n"
        "            y = 10;\n"
        "            z = 20;\n"
        "            print(y + z);\n"
        "        case 2:\n"
        "            print(\"two\");\n"
        "    }\n"
        "}\n";
    
    ASTNode* ast = parse_code(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(switch_parse_nested_blocks) {
    const char* code = 
        "main() {\n"
        "    switch (x) {\n"
        "        case 1:\n"
        "            if (y > 0) {\n"
        "                print(\"positive\");\n"
        "            }\n"
        "    }\n"
        "}\n";
    
    ASTNode* ast = parse_code(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(switch_codegen_basic) {
    const char* code = 
        "main() {\n"
        "    x = 1;\n"
        "    switch (x) {\n"
        "        case 1:\n"
        "            print(\"one\");\n"
        "        case 2:\n"
        "            print(\"two\");\n"
        "        default:\n"
        "            print(\"other\");\n"
        "    }\n"
        "}\n";
    
    ASTNode* ast = parse_code(code);
    ASSERT_NOT_NULL(ast);
    
    /* tmpfile(): an assertion longjmps out of the test, so a named file
     * would be left behind in whatever directory the suite ran in. */
    FILE* out = tmpfile();
    ASSERT_NOT_NULL(out);

    CodeGenerator* gen = create_code_generator(out);
    generate_program(gen, ast);
    free_code_generator(gen);

    char* buffer = read_all(out);
    fclose(out);
    ASSERT_NOT_NULL(buffer);

    ASSERT_TRUE(strstr(buffer, "switch") != NULL);
    ASSERT_TRUE(strstr(buffer, "case") != NULL);

    free(buffer);
    free_ast_node(ast);
}

TEST(switch_string_cases) {
    // Note: C doesn't support string switch, but we should handle parsing
    const char* code = 
        "main() {\n"
        "    x = \"hello\";\n"
        "    switch (x) {\n"
        "        case \"hello\":\n"
        "            print(\"greeting\");\n"
        "        default:\n"
        "            print(\"unknown\");\n"
        "    }\n"
        "}\n";
    
    ASTNode* ast = parse_code(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(switch_expression_in_condition) {
    const char* code = 
        "main() {\n"
        "    switch (x + y * 2) {\n"
        "        case 10:\n"
        "            print(\"ten\");\n"
        "    }\n"
        "}\n";
    
    ASTNode* ast = parse_code(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(switch_break_statements) {
    const char* code = 
        "main() {\n"
        "    switch (x) {\n"
        "        case 1:\n"
        "            print(\"one\");\n"
        "            break;\n"
        "        case 2:\n"
        "            print(\"two\");\n"
        "            break;\n"
        "    }\n"
        "}\n";
    
    ASTNode* ast = parse_code(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(switch_fallthrough) {
    const char* code = 
        "main() {\n"
        "    switch (x) {\n"
        "        case 1:\n"
        "        case 2:\n"
        "        case 3:\n"
        "            print(\"1, 2, or 3\");\n"
        "    }\n"
        "}\n";
    
    ASTNode* ast = parse_code(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

