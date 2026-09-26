#include "../runtime/test_harness.h"
#include "../../compiler/parser/lexer.h"
#include "../../compiler/parser/parser.h"
#include "../../compiler/codegen/codegen.h"
#include "../../compiler/analysis/typechecker.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Helper: tokenize source into a heap-allocated Token** array.
static Token** tokenize_source(const char* source, int* out_count) {
    lexer_init(source);
    Token** tokens = malloc(sizeof(Token*) * 256);
    int count = 0;
    Token* tok;
    while ((tok = next_token()) != NULL && tok->type != TOKEN_EOF && count < 255) {
        tokens[count++] = tok;
    }
    if (tok) tokens[count++] = tok;  // include EOF token
    *out_count = count;
    return tokens;
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

TEST(codegen_for_loop_syntax) {
    int count;
    Token** tokens = tokenize_source("main() { for i = 0; i < 3; i++ { print(i) } }", &count);
    Parser* parser = create_parser(tokens, count);
    ASTNode* ast = parse_program(parser);
    ASSERT_NOT_NULL(ast);

    FILE* out = tmpfile();
    ASSERT_NOT_NULL(out);
    CodeGenerator* gen = create_code_generator(out);
    ASSERT_NOT_NULL(gen);
    generate_program(gen, ast);

    char* buf = read_all(out);
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "int main(") != NULL);
    ASSERT_TRUE(strstr(buf, "for (") != NULL);
    free(buf);

    fclose(out);
    free_code_generator(gen);
    free_ast_node(ast);
    free_parser(parser);
    for (int i = 0; i < count; i++) free_token(tokens[i]);
    free(tokens);
}

TEST(codegen_while_loop_syntax) {
    int count;
    Token** tokens = tokenize_source("main() { x = 5\n while x > 0 { x = x - 1 } }", &count);
    Parser* parser = create_parser(tokens, count);
    ASTNode* ast = parse_program(parser);
    ASSERT_NOT_NULL(ast);

    FILE* out = tmpfile();
    ASSERT_NOT_NULL(out);
    CodeGenerator* gen = create_code_generator(out);
    ASSERT_NOT_NULL(gen);
    generate_program(gen, ast);

    char* buf = read_all(out);
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "int main(") != NULL);
    ASSERT_TRUE(strstr(buf, "while (") != NULL);
    free(buf);

    fclose(out);
    free_code_generator(gen);
    free_ast_node(ast);
    free_parser(parser);
    for (int i = 0; i < count; i++) free_token(tokens[i]);
    free(tokens);
}

/* #2210: a `string` argument of a call through a typed function pointer
 * is passed as its bytes (`aether_string_data(arg)`), as an extern call
 * passes one, so a heap string's AetherString header never reaches the C
 * callee. Typechecked first: the pointer's signature and the argument's
 * type are what the checker stamped. One test per call shape. */
static char* generate_typechecked(const char* source) {
    int count;
    Token** tokens = tokenize_source(source, &count);
    Parser* parser = create_parser(tokens, count);
    ASTNode* ast = parse_program(parser);
    if (!ast) return NULL;
    if (!typecheck_program(ast)) return NULL;

    FILE* out = tmpfile();
    CodeGenerator* gen = create_code_generator(out);
    generate_program(gen, ast);
    char* buf = read_all(out);

    fclose(out);
    free_code_generator(gen);
    free_ast_node(ast);
    free_parser(parser);
    for (int i = 0; i < count; i++) free_token(tokens[i]);
    free(tokens);
    return buf;
}

TEST(codegen_fnptr_local_string_arg_passes_bytes) {
    char* buf = generate_typechecked(
        "extern strlen(s: string) -> int\n"
        "main() { prefix = \"lights\"\n"
        "  name = \"${prefix}[0].position\"\n"
        "  f = strlen as fn(string) -> int\n"
        "  println(\"${f(name)}\") }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "(f))(aether_string_data(name))") != NULL);
    free(buf);
}

TEST(codegen_fnptr_param_string_arg_passes_bytes) {
    char* buf = generate_typechecked(
        "through(cb: fn(int, string) -> int, s: string) -> int { return cb(1, s) }\n"
        "main() { }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "(cb))(1, aether_string_data(s))") != NULL);
    free(buf);
}

TEST(codegen_fnptr_struct_field_string_arg_passes_bytes) {
    char* buf = generate_typechecked(
        "struct Ops { measure: fn(string) -> int }\n"
        "by_value(o: Ops, s: string) -> int { return o.measure(s) }\n"
        "by_pointer(p: *Ops, s: string) -> int { return p.measure(s) }\n"
        "main() { }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "(o.measure)(aether_string_data(s))") != NULL);
    ASSERT_TRUE(strstr(buf, "(p->measure)(aether_string_data(s))") != NULL);
    free(buf);
}

TEST(codegen_fnptr_non_string_param_passes_arg_as_written) {
    char* buf = generate_typechecked(
        "through(cb: fn(int, ptr) -> int, n: int, p: ptr) -> int { return cb(n, p) }\n"
        "main() { }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "(cb))(n, p)") != NULL);
    ASSERT_TRUE(strstr(buf, "aether_string_data(p)") == NULL);
    free(buf);
}
