#include "../runtime/test_harness.h"
#include "../../compiler/parser/lexer.h"
#include "../../compiler/parser/parser.h"
#include "../../compiler/codegen/codegen.h"
#include "../../compiler/codegen/codegen_internal.h"
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

/* Parse -> typecheck -> codegen a snippet and return the emitted C, for
 * tests that assert on the generated text. Used by the #2210 typed-fn-pointer
 * tests and the #2206 string-"0"-compare tests below. */
static char* generate_typechecked(const char* source) {
    int count;
    Token** tokens = tokenize_source(source, &count);
    Parser* parser = create_parser(tokens, count);
    ASTNode* ast = parse_program(parser);
    if (!ast) return NULL;
    if (!typecheck_program(ast)) return NULL;

    FILE* out = tmpfile();
    if (!out) return NULL;
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

/* #2206: `s != "0"` must be a content compare, not a pointer compare. Both
 * the integer literal `0` and the string literal `"0"` carry the text `0`;
 * the null-check shortcut in the string-compare codegen used to match either,
 * so the string one skipped strcmp. The string literal is TYPE_STRING out of
 * the parser, which is what the fix keys on. */
TEST(codegen_string_ne_zero_literal_uses_strcmp) {
    char* buf = generate_typechecked(
        "main() { a = \"abc\"\n if a != \"0\" { println(\"ne\") } }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "strcmp(_aether_safe_str(a), _aether_safe_str(\"0\")) != 0") != NULL);
    ASSERT_TRUE(strstr(buf, "a != \"0\"") == NULL);
    free(buf);
}

TEST(codegen_zero_literal_eq_string_uses_strcmp) {
    char* buf = generate_typechecked(
        "main() { a = \"abc\"\n if \"0\" == a { println(\"eq\") } }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "strcmp(_aether_safe_str(\"0\"), _aether_safe_str(a)) == 0") != NULL);
    ASSERT_TRUE(strstr(buf, "\"0\" == a") == NULL);
    free(buf);
}

TEST(codegen_ptr_eq_int_zero_stays_pointer_check) {
    char* buf = generate_typechecked(
        "main() { let p: ptr = null\n if p == 0 { println(\"null\") } }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "p == 0") != NULL);
    ASSERT_TRUE(strstr(buf, "_aether_safe_str(p)") == NULL);
    free(buf);
}

/* #2189: a trailing block (`group("first") { ... }`) inlines as a C `{ ... }`
 * block, so a name it binds is out of scope in a sibling block. A callback in
 * the second block binding its own `ml` must not capture the first block's
 * `ml`: the capture analysis used to treat every trailing block as transparent
 * and emitted `_aether_make_closure_1(root, ml)` against a name that had
 * already gone out of scope. The first block's callback still captures it. */
TEST(codegen_closure_does_not_capture_sibling_trailing_block_local) {
    char* buf = generate_typechecked(
        "group(name: string) -> int { return 1 }\n"
        "run(cb: fn) { cb() }\n"
        "main() {\n"
        "    root = \"/tmp/x\"\n"
        "    group(\"first\") {\n"
        "        ml = \"${root}/a\"\n"
        "        run() callback { println(ml) }\n"
        "    }\n"
        "    group(\"second\") {\n"
        "        run() callback {\n"
        "            ml = \"${root}/b\"\n"
        "            println(ml)\n"
        "        }\n"
        "    }\n"
        "}\n");
    ASSERT_NOT_NULL(buf);
    /* First block's callback reads the outer name: captured. */
    ASSERT_TRUE(strstr(buf, "_aether_make_closure_0(ml)") != NULL);
    /* Second block's callback binds its own: only `root` crosses into it. */
    ASSERT_TRUE(strstr(buf, "_aether_make_closure_1(root)") != NULL);
    ASSERT_TRUE(strstr(buf, "_aether_make_closure_1(root, ml)") == NULL);
    free(buf);
}

/* The companion shape: a binding declared in the SAME trailing block, before
 * the callback, is visible to it and a write in the callback goes through. */
TEST(codegen_closure_captures_same_trailing_block_local_it_mutates) {
    char* buf = generate_typechecked(
        "group(name: string) -> int { return 1 }\n"
        "run(cb: fn) { cb() }\n"
        "main() {\n"
        "    group(\"counted\") {\n"
        "        hits = 0\n"
        "        run() callback { hits = hits + 1 }\n"
        "    }\n"
        "}\n");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "_aether_make_closure_0(hits)") != NULL);
    free(buf);
}

/* #2195: `${}` segments are printf varargs, whose evaluation order C leaves
 * unspecified (gcc goes right to left). Once any segment has a side effect,
 * every segment is evaluated into a `_ad_N` temp in source order before the
 * call; an all-pure interpolation stays inline.
 *
 * The temp counter is process-global and the tests share one process, so
 * the assertions match the shape of each emission and the order of its
 * pieces, never a particular `_ad_N` number. */
static int emitted_in_order(const char* buf, const char* const* pieces, int n) {
    const char* at = buf;
    for (int i = 0; i < n; i++) {
        at = strstr(at, pieces[i]);
        if (!at) return 0;
        at += strlen(pieces[i]);
    }
    return 1;
}

TEST(codegen_interp_impure_segments_hoist_in_source_order) {
    char* buf = generate_typechecked(
        "bump() -> int { return 1 }\n"
        "main() { println(\"${bump()} ${bump()} ${bump()}\") }");
    ASSERT_NOT_NULL(buf);
    const char* const pieces[] = {
        "{ int _ad_", " = (int)(bump()); int _ad_", " = (int)(bump()); int _ad_",
        " = (int)(bump()); printf(\"%d %d %d\", (int)_ad_", ", (int)_ad_", ", (int)_ad_",
        "); }; putchar('\\n');",
    };
    ASSERT_TRUE(emitted_in_order(buf, pieces, 7));
    ASSERT_TRUE(strstr(buf, "(int)bump()") == NULL);
    free(buf);
}

TEST(codegen_interp_value_path_hoists_and_yields_result) {
    char* buf = generate_typechecked(
        "bump() -> int { return 1 }\n"
        "main() { s = \"${bump()}/${bump()}\"\n println(s) }");
    ASSERT_NOT_NULL(buf);
    const char* const pieces[] = {
        "({ int _ad_", " = (int)(bump()); int _ad_", " = (int)(bump()); ",
        "const char* _it_r = _aether_interp(\"%d/%d\", (int)_ad_", ", (int)_ad_", "); _it_r; })",
    };
    ASSERT_TRUE(emitted_in_order(buf, pieces, 6));
    free(buf);
}

TEST(codegen_interp_pure_segment_beside_impure_is_hoisted_too) {
    /* A pure read on either side of the call observes it, so it is hoisted
     * in its source position rather than left as a vararg. */
    char* buf = generate_typechecked(
        "bump() -> int { return 1 }\n"
        "main() { n = 10\n println(\"${n} ${bump()} ${n}\") }");
    ASSERT_NOT_NULL(buf);
    const char* const pieces[] = {
        "{ int _ad_", " = (int)(n); int _ad_", " = (int)(bump()); int _ad_", " = (int)(n); ",
        "printf(\"%d %d %d\", (int)_ad_",
    };
    ASSERT_TRUE(emitted_in_order(buf, pieces, 5));
    ASSERT_TRUE(strstr(buf, "(int)n)") == NULL);
    free(buf);
}

TEST(codegen_interp_all_pure_segments_stay_inline) {
    char* buf = generate_typechecked(
        "main() { name = \"x\"\n age = 3\n println(\"${name} is ${age}\") }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "_ad_") == NULL);
    ASSERT_NOT_NULL(strstr(buf, "printf(\"%s is %d\", _aether_safe_str(name), (int)age)"));
    free(buf);
}

TEST(codegen_interp_single_impure_segment_stays_inline) {
    char* buf = generate_typechecked(
        "bump() -> int { return 1 }\n"
        "main() { println(\"got ${bump()}\") }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "_ad_") == NULL);
    ASSERT_NOT_NULL(strstr(buf, "printf(\"got %d\", (int)bump())"));
    free(buf);
}

TEST(codegen_interp_hoisted_heap_segment_is_freed_after_call) {
    char* buf = generate_typechecked(
        /* The snippet typechecker resolves no std modules, so name the
         * runtime's heap-producing concat directly; the classifier keys on
         * the callee name either way. */
        "extern string_concat(a: string, b: string) -> string\n"
        "bump() -> int { return 1 }\n"
        "main() { name = \"ab\"\n println(\"${string_concat(name, name)} ${bump()}\") }");
    ASSERT_NOT_NULL(buf);
    const char* const pieces[] = {
        "{ const char* _ad_", " = (const char*)(string_concat(name, name)); int _ad_", " = (int)(bump()); ",
        "printf(\"%s %d\", _aether_safe_str(_ad_", "), (int)_ad_", "); aether_heap_str_free(_ad_", "); }",
    };
    ASSERT_TRUE(emitted_in_order(buf, pieces, 7));
    free(buf);
}

TEST(codegen_interp_nested_interp_segment_is_freed_after_call) {
    char* buf = generate_typechecked(
        "main() { x = 3\n s = \"a ${\"${x}-${x}\"} b\"\n println(s) }");
    ASSERT_NOT_NULL(buf);
    const char* const pieces[] = {
        "({ const char* _ad_", " = (const char*)(_aether_interp(\"%d-%d\", (int)x, (int)x)); ",
        "const char* _it_r = _aether_interp(\"a %s b\", _aether_safe_str(_ad_", ")); aether_heap_str_free(_ad_", "); _it_r; })",
    };
    ASSERT_TRUE(emitted_in_order(buf, pieces, 5));
    free(buf);
}

/* The classifiers behind the hoist, on hand-built nodes. */
static ASTNode* interp_text(const char* text) {
    ASTNode* n = create_ast_node(AST_LITERAL, text, 1, 1);
    n->node_type = create_type(TYPE_STRING);
    return n;
}

static ASTNode* interp_int_segment(ASTNodeType kind, const char* value) {
    ASTNode* n = create_ast_node(kind, value, 1, 1);
    n->node_type = create_type(TYPE_INT);
    return n;
}

static ASTNode* interp_of(ASTNode* a, ASTNode* b, ASTNode* c) {
    ASTNode* s = create_ast_node(AST_STRING_INTERP, NULL, 1, 1);
    if (a) add_child(s, a);
    if (b) add_child(s, b);
    if (c) add_child(s, c);
    return s;
}

TEST(interp_order_hoist_boundary_all_pure_is_none) {
    ASTNode* s = interp_of(interp_int_segment(AST_IDENTIFIER, "a"), interp_text(" "),
                           interp_int_segment(AST_IDENTIFIER, "b"));
    ASSERT_EQ(-1, interp_order_hoist_boundary(s));
    free_ast_node(s);
}

TEST(interp_order_hoist_boundary_lone_impure_is_none) {
    ASTNode* s = interp_of(interp_text("got "), interp_int_segment(AST_FUNCTION_CALL, "f"), NULL);
    ASSERT_EQ(-1, interp_order_hoist_boundary(s));
    free_ast_node(s);
}

TEST(interp_order_hoist_boundary_impure_pair_is_last_segment) {
    ASTNode* s = interp_of(interp_int_segment(AST_FUNCTION_CALL, "f"), interp_text(" "),
                           interp_int_segment(AST_FUNCTION_CALL, "f"));
    ASSERT_EQ(2, interp_order_hoist_boundary(s));
    free_ast_node(s);
}

TEST(interp_order_hoist_boundary_pure_after_impure_is_the_pure_one) {
    ASTNode* s = interp_of(interp_int_segment(AST_FUNCTION_CALL, "f"), interp_text(" "),
                           interp_int_segment(AST_IDENTIFIER, "n"));
    ASSERT_EQ(2, interp_order_hoist_boundary(s));
    free_ast_node(s);
}

TEST(interp_order_hoist_boundary_sees_nested_call) {
    ASTNode* sum = interp_int_segment(AST_BINARY_EXPRESSION, "+");
    add_child(sum, interp_int_segment(AST_IDENTIFIER, "n"));
    add_child(sum, interp_int_segment(AST_FUNCTION_CALL, "f"));
    ASTNode* s = interp_of(interp_int_segment(AST_IDENTIFIER, "n"), interp_text(" "), sum);
    ASSERT_EQ(2, interp_order_hoist_boundary(s));
    free_ast_node(s);
}

TEST(interp_segment_is_text_only_for_string_literals) {
    ASTNode* text = interp_text("hi");
    ASTNode* ident = interp_int_segment(AST_IDENTIFIER, "n");
    ASTNode* str_ident = create_ast_node(AST_IDENTIFIER, "s", 1, 1);
    str_ident->node_type = create_type(TYPE_STRING);
    ASSERT_TRUE(interp_segment_is_text(text));
    ASSERT_FALSE(interp_segment_is_text(ident));
    ASSERT_FALSE(interp_segment_is_text(str_ident));
    ASSERT_FALSE(interp_segment_is_text(NULL));
    free_ast_node(text);
    free_ast_node(ident);
    free_ast_node(str_ident);
}

TEST(interp_temp_c_type_matches_vararg_casts) {
    Type* t;
    t = create_type(TYPE_STRING);   ASSERT_TRUE(strcmp("const char*", interp_temp_c_type(t)) == 0); free_type(t);
    t = create_type(TYPE_BOOL);     ASSERT_TRUE(strcmp("int", interp_temp_c_type(t)) == 0);         free_type(t);
    t = create_type(TYPE_INT64);    ASSERT_TRUE(strcmp("long long", interp_temp_c_type(t)) == 0);   free_type(t);
    t = create_type(TYPE_FLOAT);    ASSERT_TRUE(strcmp("double", interp_temp_c_type(t)) == 0);      free_type(t);
    ASSERT_TRUE(strcmp("int", interp_temp_c_type(NULL)) == 0);
    t = create_type(TYPE_STRUCT);   ASSERT_NULL(interp_temp_c_type(t));                              free_type(t);
}
