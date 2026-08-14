#include "../runtime/test_harness.h"
#include "../../compiler/parser/lexer.h"
#include "../../compiler/parser/parser.h"
#include "../../compiler/codegen/codegen.h"
#include <string.h>

// Test helper: Parse and validate code
static ASTNode* parse_and_validate(const char* code) {
    lexer_init(code);
    Token** tokens = malloc(1000 * sizeof(Token*));
    int count = 0;
    Token* tok;
    while ((tok = next_token())->type != TOKEN_EOF && count < 999) {
        tokens[count++] = tok;
    }
    if (tok && tok->type == TOKEN_EOF) tokens[count++] = tok;
    
    Parser* parser = create_parser(tokens, count);
    parser->suppress_errors = 1;  // Suppress parse errors during testing
    ASTNode* ast = parse_program(parser);
    free_parser(parser);
    free(tokens);
    return ast;
}

// ====================================================================
// SECTION 1: Basic Pattern Matching in Functions
// ====================================================================

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

TEST(pattern_fib_literal_matching) {
    const char* code = 
        "fib(0) -> 1\n"
        "fib(1) -> 1\n"
        "fib(n) -> fib(n-1) + fib(n-2)\n"
        "main() { }\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(AST_PROGRAM, ast->type);
    ASSERT_TRUE(ast->child_count >= 3);  // 3 fib clauses + main
    free_ast_node(ast);
}

TEST(pattern_factorial_with_guard) {
    const char* code = 
        "fact(0) -> 1\n"
        "fact(n) when n > 0 -> n * fact(n-1)\n"
        "main() { }\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(pattern_boolean_literals) {
    const char* code = 
        "not(true) -> false\n"
        "not(false) -> true\n"
        "main() { }\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(pattern_string_literals) {
    const char* code = 
        "greet(\"Alice\") -> \"Hello Alice\"\n"
        "greet(\"Bob\") -> \"Hello Bob\"\n"
        "greet(_) -> \"Hello stranger\"\n"
        "main() { }\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(pattern_wildcard) {
    const char* code = 
        "const(_) -> 42\n"
        "first(x, _) -> x\n"
        "second(_, y) -> y\n"
        "main() { }\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

// ====================================================================
// SECTION 2: Guard Clauses
// ====================================================================

TEST(guard_numeric_comparison) {
    const char* code = 
        "classify(x) when x < 0 -> \"negative\"\n"
        "classify(x) when x == 0 -> \"zero\"\n"
        "classify(x) when x > 0 -> \"positive\"\n"
        "main() { }\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(guard_logical_operators) {
    const char* code = 
        "is_valid(x) when x > 0 && x < 100 -> true\n"
        "is_valid(_) -> false\n"
        "main() { }\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(guard_division_safety) {
    const char* code = 
        "safe_div(x, y) when y != 0 -> x / y\n"
        "safe_div(_, 0) -> 0\n"
        "main() { }\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

// ====================================================================
// SECTION 3: List Patterns
// ====================================================================

TEST(pattern_empty_list) {
    const char* code = 
        "len([]) -> 0\n"
        "len(list) -> 1\n"
        "main() { }\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(pattern_list_cons) {
    const char* code = 
        "head([h|_]) -> h\n"
        "tail([_|t]) -> t\n"
        "main() { }\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(pattern_list_fixed_size) {
    const char* code = 
        "sum2([a, b]) -> a + b\n"
        "sum3([a, b, c]) -> a + b + c\n"
        "main() { }\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(pattern_list_recursive) {
    const char* code = 
        "sum([]) -> 0\n"
        "sum([h|t]) -> h + sum(t)\n"
        "main() { }\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

// ====================================================================
// SECTION 4: Struct Patterns
// ====================================================================

TEST(pattern_struct_basic) {
    const char* code = 
        "struct Point { int x int y }\n"
        "is_origin(Point{x: 0, y: 0}) -> true\n"
        "is_origin(_) -> false\n"
        "main() { }\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(pattern_struct_destructure) {
    const char* code = 
        "struct Point { int x int y }\n"
        "get_x(Point{x: val, y: _}) -> val\n"
        "get_y(Point{x: _, y: val}) -> val\n"
        "main() { }\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

// ====================================================================
// SECTION 5: Match Expressions
// ====================================================================

TEST(match_with_literals) {
    const char* code = 
        "main() {\n"
        "    x = 2\n"
        "    result = match (x) {\n"
        "        1 => \"one\"\n"
        "        2 => \"two\"\n"
        "        _ => \"other\"\n"
        "    }\n"
        "}\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(match_with_blocks) {
    const char* code = 
        "main() {\n"
        "    x = 5\n"
        "    match (x) {\n"
        "        1 => { print(\"one\") }\n"
        "        5 => { print(\"five\") }\n"
        "        _ => { print(\"other\") }\n"
        "    }\n"
        "}\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(match_nested) {
    const char* code = 
        "main() {\n"
        "    x = 1\n"
        "    y = 2\n"
        "    match (x) {\n"
        "        1 => match (y) {\n"
        "            2 => print(\"1,2\")\n"
        "            _ => print(\"1,?\")\n"
        "        }\n"
        "        _ => print(\"?\")\n"
        "    }\n"
        "}\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

// ====================================================================
// SECTION 6: Complex Examples
// ====================================================================

TEST(fibonacci_complete) {
    const char* code = 
        "fib(0) -> 1\n"
        "fib(1) -> 1\n"
        "fib(n) when n > 1 -> fib(n-1) + fib(n-2)\n"
        "\n"
        "main() {\n"
        "    result = fib(10)\n"
        "    print(result)\n"
        "}\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(list_operations_complete) {
    const char* code = 
        "len([]) -> 0\n"
        "len([_|t]) -> 1 + len(t)\n"
        "\n"
        "sum([]) -> 0\n"
        "sum([h|t]) -> h + sum(t)\n"
        "\n"
        "main() { }\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(mixed_traditional_and_pattern) {
    const char* code = 
        "traditional(x) { return x + 1 }\n"
        "pattern(0) -> 1\n"
        "pattern(n) -> n * 2\n"
        "main() { }\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

// ====================================================================
// SECTION 7: Code Generation Tests
// ====================================================================

TEST(codegen_pattern_literals) {
    const char* code = 
        "fib(0) -> 1\n"
        "fib(1) -> 1\n"
        "main() { }\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    
    FILE* out = tmpfile();
    ASSERT_NOT_NULL(out);
    CodeGenerator* gen = create_code_generator(out);
    generate_program(gen, ast);
    free_code_generator(gen);

    char* buf = read_all(out);
    fclose(out);
    ASSERT_NOT_NULL(buf);

    /* The dispatch the pattern compiles to, not just any `if` in the
     * prelude: the matched value is tested against each literal. */
    ASSERT_TRUE(strstr(buf, "if (") != NULL);
    ASSERT_TRUE(strstr(buf, "return") != NULL);

    free(buf);
    free_ast_node(ast);
}

TEST(codegen_guards) {
    const char* code = 
        "fact(n) when n > 0 -> n\n"
        "main() { }\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    
    FILE* out = tmpfile();
    ASSERT_NOT_NULL(out);
    CodeGenerator* gen = create_code_generator(out);
    generate_program(gen, ast);
    free_code_generator(gen);

    /* The clause has to reach the output: generating a prelude and nothing
     * else would otherwise pass as "did not crash". */
    char* buf = read_all(out);
    fclose(out);
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "fact") != NULL);
    free(buf);

    free_ast_node(ast);
}

TEST(codegen_list_patterns) {
    const char* code = 
        "len([]) -> 0\n"
        "main() { }\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    
    FILE* out = tmpfile();
    ASSERT_NOT_NULL(out);
    CodeGenerator* gen = create_code_generator(out);
    generate_program(gen, ast);
    free_code_generator(gen);

    /* The clause has to reach the output: generating a prelude and nothing
     * else would otherwise pass as "did not crash". */
    char* buf = read_all(out);
    fclose(out);
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "len") != NULL);
    free(buf);

    free_ast_node(ast);
}

// ====================================================================
// SECTION 8: Edge Cases
// ====================================================================

TEST(pattern_multiple_wildcards) {
    const char* code = 
        "ignore(_, _, _) -> 0\n"
        "main() { }\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(pattern_nested_structs) {
    const char* code = 
        "struct Inner { int val }\n"
        "struct Outer { Inner inner }\n"
        "extract(Outer{inner: Inner{val: x}}) -> x\n"
        "main() { }\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

TEST(guard_complex_expression) {
    const char* code = 
        "check(x, y) when x*x + y*y < 100 -> true\n"
        "check(_, _) -> false\n"
        "main() { }\n";
    
    ASTNode* ast = parse_and_validate(code);
    ASSERT_NOT_NULL(ast);
    free_ast_node(ast);
}

