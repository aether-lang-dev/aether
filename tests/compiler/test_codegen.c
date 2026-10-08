#include "../runtime/test_harness.h"
#include "../../compiler/parser/lexer.h"
#include "../../compiler/parser/parser.h"
#include "../../compiler/codegen/codegen.h"
#include "../../compiler/codegen/codegen_internal.h"
#include "../../compiler/analysis/typechecker.h"
#include "../../compiler/analysis/derive.h"
#include "../../compiler/aether_error.h"
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
 * so the string one skipped the content compare. The string literal is
 * TYPE_STRING out of the parser, which is what the fix keys on. The compare
 * is by length and bytes, through string_equals (#2515). */
TEST(codegen_string_ne_zero_literal_compares_content) {
    char* buf = generate_typechecked(
        "main() { a = \"abc\"\n if a != \"0\" { println(\"ne\") } }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "!string_equals(a, \"0\")") != NULL);
    ASSERT_TRUE(strstr(buf, "a != \"0\"") == NULL);
    free(buf);
}

TEST(codegen_zero_literal_eq_string_compares_content) {
    char* buf = generate_typechecked(
        "main() { a = \"abc\"\n if \"0\" == a { println(\"eq\") } }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "string_equals(\"0\", a)") != NULL);
    ASSERT_TRUE(strstr(buf, "\"0\" == a") == NULL);
    free(buf);
}

/* #2515: order on strings is byte order over the whole length, through
 * string_compare; strcmp stopped at the first NUL. */
TEST(codegen_string_order_compares_content) {
    char* buf = generate_typechecked(
        "main() { a = \"abc\"\n if a < \"b\" { println(\"lt\") } }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "(string_compare(a, \"b\") < 0)") != NULL);
    ASSERT_TRUE(strstr(buf, "strcmp(_aether_safe_str(a)") == NULL);
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

/* #2218: a builder declared `-> int` that returns the injected `_builder`
 * (a `void*`) must cast it, or GCC 14 rejects the TU under its default
 * -Werror=int-conversion. The cast is the same one a call argument gets. */
TEST(codegen_builder_int_return_of_builder_casts_pointer) {
    char* buf = generate_typechecked(
        "builder rec(w: int) -> int {\n"
        "  if w > 0 { return _builder }\n"
        "  return w }\n"
        "main() { println(\"${rec(2) {}}\") }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "return (int)(intptr_t)(_builder);") != NULL);
    ASSERT_TRUE(strstr(buf, "return _builder;") == NULL);
    free(buf);
}

TEST(codegen_int_return_of_int_is_not_cast) {
    char* buf = generate_typechecked(
        "builder rec(w: int) -> int { return w }\n"
        "main() { println(\"${rec(2) {}}\") }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "return w;") != NULL);
    ASSERT_TRUE(strstr(buf, "(intptr_t)(w)") == NULL);
    free(buf);
}

TEST(codegen_ptr_return_of_int_casts_to_pointer) {
    char* buf = generate_typechecked(
        "handle(n: int) -> ptr { return n }\n"
        "main() { }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "return (void*)(intptr_t)(n);") != NULL);
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
        " = (int)(bump()); _aether_interp_print(\"%d %d %d\\n\", (int)_ad_", ", (int)_ad_", ", (int)_ad_",
        "); }",
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
        "_aether_interp_print(\"%d %d %d\\n\", (int)_ad_",
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
    /* A string segment goes as held, for the formatter to write by its
     * length, and println's newline ends the format (#2521). */
    ASSERT_NOT_NULL(strstr(buf, "_aether_interp_print(\"%s is %d\\n\", (const void*)(name), (int)age)"));
    free(buf);
}

TEST(codegen_interp_single_impure_segment_stays_inline) {
    char* buf = generate_typechecked(
        "bump() -> int { return 1 }\n"
        "main() { println(\"got ${bump()}\") }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "_ad_") == NULL);
    ASSERT_NOT_NULL(strstr(buf, "_aether_interp_print(\"got %d\\n\", (int)bump())"));
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
        "_aether_interp_print(\"%s %d\\n\", (const void*)(_ad_", "), (int)_ad_", "); aether_heap_str_free(_ad_", "); }",
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
        "const char* _it_r = _aether_interp(\"a %s b\", (const void*)(_ad_", ")); aether_heap_str_free(_ad_", "); _it_r; })",
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

/* #2220 — observable struct models. A field store on a value whose struct is
 * `@observable` is followed by `aether_observe_notify_field(<object>, <field>)`:
 * by address for a value (`&(m)`), by the pointer itself for a `*T` (`(p)`),
 * with the stored field's declaration index (#2299). A nested value store
 * notifies innermost first and then the enclosing value, each with its own
 * field. A store on a struct without the attribute emits no call. */
static int count_occurrences(const char* hay, const char* needle) {
    int n = 0;
    size_t len = strlen(needle);
    for (const char* p = strstr(hay, needle); p; p = strstr(p + len, needle)) n++;
    return n;
}

TEST(codegen_observable_value_store_notifies_by_address) {
    char* buf = generate_typechecked(
        "struct Model @observable { count: int }\n"
        "main() { m = Model { count: 0 }\n  m.count = 1\n  m.count += 1 }");
    ASSERT_NOT_NULL(buf);
    ASSERT_EQ(2, count_occurrences(buf, "aether_observe_notify_field(&(m), 0);"));
    free(buf);
}

TEST(codegen_observable_store_names_the_field_by_declaration_index) {
    char* buf = generate_typechecked(
        "struct Model @observable { count: int, status: string, ratio: float }\n"
        "main() { m = Model { count: 0, status: \"\", ratio: 0.0 }\n"
        "  m.ratio = 1.5\n  m.status = \"ok\"\n  m.count = 2 }");
    ASSERT_NOT_NULL(buf);
    const char* r = strstr(buf, "aether_observe_notify_field(&(m), 2);");
    const char* st = strstr(buf, "aether_observe_notify_field(&(m), 1);");
    const char* c = strstr(buf, "aether_observe_notify_field(&(m), 0);");
    ASSERT_NOT_NULL(r);
    ASSERT_NOT_NULL(st);
    ASSERT_NOT_NULL(c);
    ASSERT_TRUE(r < st && st < c);
    free(buf);
}

TEST(codegen_observable_pointer_store_notifies_pointer) {
    char* buf = generate_typechecked(
        "struct Model @observable { count: int }\n"
        "main() { p = heap.new(Model)\n  p.count = 1\n  heap.free(p) }");
    ASSERT_NOT_NULL(buf);
    ASSERT_EQ(1, count_occurrences(buf, "aether_observe_notify_field((p), 0);"));
    free(buf);
}

TEST(codegen_plain_struct_store_emits_no_notify) {
    char* buf = generate_typechecked(
        "struct Model { count: int }\n"
        "main() { m = Model { count: 0 }\n  m.count = 1\n"
        "  p = heap.new(Model)\n  p.count = 2\n  heap.free(p) }");
    ASSERT_NOT_NULL(buf);
    ASSERT_EQ(0, count_occurrences(buf, "aether_observe_notify_field(&"));
    ASSERT_EQ(0, count_occurrences(buf, "aether_observe_notify_field(("));
    free(buf);
}

TEST(codegen_observable_nested_value_store_notifies_inner_then_outer) {
    char* buf = generate_typechecked(
        "struct Inner @observable { x: int }\n"
        "struct Outer @observable { n: int, inner: Inner }\n"
        "main() { o = Outer { n: 0, inner: Inner { x: 0 } }\n  o.inner.x = 1 }");
    ASSERT_NOT_NULL(buf);
    /* Inner hears its field `x` (0); Outer hears `inner` (1), the field of
     * its own that holds what changed. */
    const char* inner = strstr(buf, "aether_observe_notify_field(&(o.inner), 0);");
    const char* outer = strstr(buf, "aether_observe_notify_field(&(o), 1);");
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(outer);
    ASSERT_TRUE(inner < outer);
    free(buf);
}

TEST(codegen_observable_store_through_pointer_field_notifies_pointee_only) {
    char* buf = generate_typechecked(
        "struct Inner @observable { x: int }\n"
        "struct Outer @observable { n: int, link: *Inner }\n"
        "main() { o = Outer { n: 0, link: heap.new(Inner) }\n  o.link.x = 1\n  heap.free(o.link) }");
    ASSERT_NOT_NULL(buf);
    ASSERT_EQ(1, count_occurrences(buf, "aether_observe_notify_field((o.link), 0);"));
    ASSERT_EQ(0, count_occurrences(buf, "aether_observe_notify_field(&(o)"));
    free(buf);
}

TEST(codegen_observable_survives_derive_pass) {
    int count;
    Token** tokens = tokenize_source(
        "@derive(eq)\nstruct Model @observable { count: int }\n"
        "main() { m = Model { count: 0 }\n  m.count = 1 }", &count);
    Parser* parser = create_parser(tokens, count);
    ASTNode* ast = parse_program(parser);
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(0, derive_synthesize_pass(ast));
    ASSERT_TRUE(typecheck_program(ast));

    FILE* out = tmpfile();
    ASSERT_NOT_NULL(out);
    CodeGenerator* gen = create_code_generator(out);
    generate_program(gen, ast);
    char* buf = read_all(out);
    ASSERT_NOT_NULL(buf);
    ASSERT_EQ(1, count_occurrences(buf, "aether_observe_notify_field(&(m), 0);"));
    ASSERT_TRUE(strstr(buf, "Model_eq") != NULL);   /* derive still ran */
    free(buf);

    fclose(out);
    free_code_generator(gen);
    free_ast_node(ast);
    free_parser(parser);
    for (int i = 0; i < count; i++) free_token(tokens[i]);
    free(tokens);
}

/* #2211: a local holding a function pointer shadows a builtin of the same
 * name. The checker already resolved `release(state)` through the innermost
 * symbol, but codegen's by-name builtin dispatch ran before its typed
 * fn-pointer local branch, so the call lowered as the `release` builtin,
 * printed a type error for the argument, and still produced a binary. */
TEST(codegen_fnptr_local_named_release_shadows_builtin) {
    char* buf = generate_typechecked(
        "struct FreeHook { release: fn(ptr), state: ptr }\n"
        "run(h: *FreeHook) { release = h.release\n"
        "  state = h.state\n"
        "  release(state) }\n"
        "main() { }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "((void(*)(void*))(release))(state)") != NULL);
    ASSERT_TRUE(strstr(buf, "string_release(state)") == NULL);
    ASSERT_TRUE(strstr(buf, "release() type error") == NULL);
    free(buf);
}

/* The same shape with `free`, a builtin that is also a libc symbol: the
 * call must spell the local as its declaration did (`free`), not the
 * libc-avoiding `ae_free`, or the emitted C names a variable that does
 * not exist. */
TEST(codegen_fnptr_local_named_free_shadows_builtin_and_keeps_its_spelling) {
    char* buf = generate_typechecked(
        "struct H { free: fn(ptr), state: ptr }\n"
        "run(h: *H) { free = h.free\n"
        "  state = h.state\n"
        "  free(state) }\n"
        "main() { }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "void* free = h->free;") != NULL);
    ASSERT_TRUE(strstr(buf, "((void(*)(void*))(free))(state)") != NULL);
    ASSERT_TRUE(strstr(buf, "ae_free") == NULL);
    free(buf);
}

/* The other half of #2211: the builtin `release` on a non-string is a
 * reported error, so the driver fails the build, rather than a stderr line
 * beside an exit status of 0. */
TEST(codegen_release_builtin_on_non_string_is_a_reported_error) {
    aether_error_reset_counts();
    char* buf = generate_typechecked(
        "main() { n = 5\n"
        "  release(n) }");
    ASSERT_NOT_NULL(buf);
    ASSERT_EQ(1, aether_error_count());
    aether_error_reset_counts();
    free(buf);
}

/* #2200: a call through a module-level `var` holding a function pointer
 * carries the typed C cast, as a call through a local does. The global is
 * never in the per-function registry, so it used to be emitted bare on a
 * `void*`, which C rejects. */
TEST(codegen_call_through_global_fnptr_var_emits_typed_cast) {
    char* buf = generate_typechecked(
        "extern getp() -> ptr\n"
        "cfn GenBuffers(n: int, ids: ptr)\n"
        "var gl_gen_buffers: GenBuffers = null\n"
        "load() { gl_gen_buffers = getp() as GenBuffers }\n"
        "gen(ids: ptr) { gl_gen_buffers(1, ids) }\n"
        "main() { }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "static void* gl_gen_buffers = NULL;") != NULL);
    ASSERT_TRUE(strstr(buf, "((void(*)(int, void*))(gl_gen_buffers))(1, ids)") != NULL);
    free(buf);
}

/* A cfn-typed local, parameter and struct field lower exactly as their
 * `fn(...)` spellings do: `void*` storage for the local, a real C
 * function-pointer declarator for the parameter and the field, and a typed
 * cast at each call. A cfn emits no typedef of its own. */
TEST(codegen_cfn_local_param_and_field_lower_like_fn_types) {
    char* buf = generate_typechecked(
        "extern getp() -> ptr\n"
        "cfn Scale(v: float, s: f32) -> float\n"
        "struct Ops { scale: Scale }\n"
        "apply(f: Scale, v: float) -> float { return f(v, 2.0) }\n"
        "main() { sc = getp() as Scale\n"
        "  r = sc(1.5, 4.0)\n"
        "  o = Ops { scale: sc }\n"
        "  r2 = o.scale(1.0, 1.0) + apply(sc, r) }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "void* sc = ((void*)(getp()))") != NULL);
    ASSERT_TRUE(strstr(buf, "((double(*)(double, float))(sc))(1.5, 4.0)") != NULL);
    ASSERT_TRUE(strstr(buf, "double (*f)(double, float)") != NULL);
    ASSERT_TRUE(strstr(buf, "double (*scale)(double, float)") != NULL);
    ASSERT_TRUE(strstr(buf, "typedef") == NULL || strstr(buf, "Scale;") == NULL);
    free(buf);
}

/* #2200: a C `bool` return defines only the low byte of the return
 * register, so a call through a `-> bool` function pointer whose value is
 * used reads just that byte; the upper bytes were garbage on Windows x64.
 * A call made as a bare statement is left alone: narrowing a discarded
 * value would be an unused computed value (-Wunused-value, an error under
 * HARDEN=1). Both the pointer-variable and the struct-field call paths. */
TEST(codegen_fnptr_bool_return_reads_low_byte_when_used) {
    char* buf = generate_typechecked(
        "extern getp() -> ptr\n"
        "cfn Ready(u: int) -> bool\n"
        "struct Dev { ready: Ready }\n"
        "main() { r = getp() as Ready\n"
        "  d = Dev { ready: r }\n"
        "  r(1)\n"
        "  d.ready(2)\n"
        "  if r(3) { println(\"a\") }\n"
        "  if d.ready(4) { println(\"b\") } }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "((_Bool)(unsigned char)(((int(*)(int))(r))(3)))") != NULL);
    ASSERT_TRUE(strstr(buf, "((_Bool)(unsigned char)((d.ready)(4)))") != NULL);
    ASSERT_TRUE(strstr(buf, "((_Bool)(unsigned char)(((int(*)(int))(r))(1)") == NULL);
    ASSERT_TRUE(strstr(buf, "((_Bool)(unsigned char)((d.ready)(2)") == NULL);
    ASSERT_TRUE(strstr(buf, "((int(*)(int))(r))(1);") != NULL);
    ASSERT_TRUE(strstr(buf, "(d.ready)(2);") != NULL);
    free(buf);
}

/* #2298 — @derive(schema). The derive pass runs between parse and typecheck,
 * as in aetherc; this runs the same three steps and generates. NULL when the
 * derive pass or the typechecker rejects the source. */
static char* generate_derived(const char* source) {
    int count;
    Token** tokens = tokenize_source(source, &count);
    Parser* parser = create_parser(tokens, count);
    ASTNode* ast = parse_program(parser);
    char* buf = NULL;
    if (ast && derive_synthesize_pass(ast) == 0 && typecheck_program(ast)) {
        FILE* out = tmpfile();
        if (out) {
            CodeGenerator* gen = create_code_generator(out);
            generate_program(gen, ast);
            buf = read_all(out);
            fclose(out);
            free_code_generator(gen);
        }
    }
    if (ast) free_ast_node(ast);
    free_parser(parser);
    for (int i = 0; i < count; i++) free_token(tokens[i]);
    free(tokens);
    return buf;
}

TEST(parser_field_attributes_attach_to_the_field) {
    int count;
    Token** tokens = tokenize_source(
        "struct Bob { rate: float @range(-1, 10.5) @hidden @tip(\"x\") @on(true) }", &count);
    Parser* parser = create_parser(tokens, count);
    ASTNode* ast = parse_program(parser);
    ASSERT_NOT_NULL(ast);
    ASTNode* sd = NULL;
    for (int i = 0; i < ast->child_count; i++)
        if (ast->children[i]->type == AST_STRUCT_DEFINITION) sd = ast->children[i];
    ASSERT_NOT_NULL(sd);
    ASTNode* field = sd->children[0];
    ASSERT_EQ(AST_STRUCT_FIELD, field->type);
    ASSERT_EQ(4, field->child_count);
    ASTNode* range = field->children[0];
    ASSERT_EQ(AST_FIELD_ATTRIBUTE, range->type);
    ASSERT_STREQ("range", range->value);
    ASSERT_EQ(2, range->child_count);
    ASSERT_STREQ("-1", range->children[0]->value);          /* the sign folds in */
    ASSERT_EQ(TYPE_INT, range->children[0]->node_type->kind);
    ASSERT_EQ(TYPE_FLOAT, range->children[1]->node_type->kind);
    ASSERT_EQ(0, field->children[1]->child_count);          /* @hidden */
    ASSERT_EQ(TYPE_STRING, field->children[2]->children[0]->node_type->kind);
    ASSERT_EQ(TYPE_BOOL, field->children[3]->children[0]->node_type->kind);

    free_ast_node(ast);
    free_parser(parser);
    for (int i = 0; i < count; i++) free_token(tokens[i]);
    free(tokens);
}

TEST(codegen_derive_schema_emits_a_static_table) {
    char* buf = generate_derived(
        "struct Vec { x: float }\n"
        "@derive(schema)\n"
        "struct Bob { rate: float @range(0.0, 10.0)\n  pos: Vec\n  name: string }\n"
        "main() { s = Bob_schema()\n  println(\"${s == null}\") }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "#define AETHER_SCHEMA_LAYOUT_ONLY\n#include \"aether_schema.h\"") != NULL);
    /* Offsets and sizes are C's own. */
    ASSERT_TRUE(strstr(buf, "offsetof(struct Bob, rate), sizeof(((struct Bob*)0)->rate)") != NULL);
    /* The attribute and its two float arguments. */
    ASSERT_TRUE(strstr(buf, "_ae_schema_Bob_a0_0[] = { {2, \"0.0\", 0LL, 0.0}, {2, \"10.0\", 10LL, 10.0} };") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"range\", 2, _ae_schema_Bob_a0_0}") != NULL);
    /* A struct reached by value gets its own table, named by its getter. */
    ASSERT_TRUE(strstr(buf, "static const AetherSchema* _ae_schema_Vec(void) {") != NULL);
    ASSERT_TRUE(strstr(buf, "\"pos\", \"Vec\", 15, 0, 0, offsetof(struct Bob, pos)") != NULL);
    /* A string names its ownership flag. */
    ASSERT_TRUE(strstr(buf, "(long)offsetof(struct Bob, _heap_name)") != NULL);
    /* T_schema() returns the table's address. */
    ASSERT_TRUE(strstr(buf, "((void*)_ae_schema_Bob())") != NULL);
    free(buf);
}

TEST(codegen_no_schema_emits_no_table) {
    char* buf = generate_derived(
        "struct Bob { rate: float }\n"
        "main() { b = Bob { rate: 1.0 }\n  println(\"${b.rate}\") }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "aether_schema.h") == NULL);
    ASSERT_TRUE(strstr(buf, "_ae_schema_") == NULL);
    free(buf);
}

/* The derive pass's verdict alone, for the rejected forms. */
static int derive_result(const char* source) {
    int count;
    Token** tokens = tokenize_source(source, &count);
    Parser* parser = create_parser(tokens, count);
    ASTNode* ast = parse_program(parser);
    int rc = ast ? derive_synthesize_pass(ast) : -2;
    if (ast) free_ast_node(ast);
    free_parser(parser);
    for (int i = 0; i < count; i++) free_token(tokens[i]);
    free(tokens);
    return rc;
}

TEST(derive_rejects_field_attributes_without_schema) {
    ASSERT_EQ(-1, derive_result("struct Bob { rate: float @range(0.0, 1.0) }\nmain() { }"));
    ASSERT_EQ(-1, derive_result("@derive(eq)\nstruct Bob { rate: float @range(0.0, 1.0) }\nmain() { }"));
    ASSERT_EQ(0, derive_result("@derive(eq, schema)\nstruct Bob { rate: float @range(0.0, 1.0) }\nmain() { }"));
}

TEST(derive_rejects_an_attribute_named_twice) {
    ASSERT_EQ(-1, derive_result(
        "@derive(schema)\nstruct Bob { rate: float @range(0.0, 1.0) @range(1.0, 2.0) }\nmain() { }"));
}

/* #2509: a message with more than four fields was declared aligned(64), but
 * its payload travels in a malloc'd copy (16-byte aligned) that the receiver
 * reads through a pointer of the message type: undefined behaviour, and an
 * aligned vector move the compiler may choose for it faults. Messages now
 * take their natural alignment; the actor struct keeps its cache line,
 * which the runtime allocates on a 64-byte boundary (#2485). */
TEST(codegen_wide_message_has_natural_alignment) {
    char* buf = generate_typechecked(
        "message Wide { a: int, b: int, c: int, d: int, e: int, f: string }\n"
        "actor Sink {\n"
        "    state n = 0\n"
        "    receive {\n"
        "        Wide(a, b, c, d, e, f) -> { n = n + a + e }\n"
        "    }\n"
        "}\n"
        "main() {\n"
        "    s = spawn(Sink())\n"
        "    s ! Wide { a: 1, b: 2, c: 3, d: 4, e: 5, f: \"x\" }\n"
        "}\n");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "typedef struct Wide {") != NULL);
    int aligned = 0;
    for (const char* p = buf; (p = strstr(p, "aligned(64)")) != NULL; p++) aligned++;
    ASSERT_EQ(1, aligned);  /* the actor struct only */
    free(buf);
}
