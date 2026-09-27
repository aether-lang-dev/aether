#include "../runtime/test_harness.h"
#include "../../compiler/parser/lexer.h"
#include "../../compiler/parser/parser.h"
#include "../../compiler/analysis/typechecker.h"
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

TEST(typechecker_basic_types) {
    ASTNode* ast = parse_source("main() { x = 42; }");
    ASSERT_NOT_NULL(ast);
    // typecheck_program returns 1 on success, 0 if there are type errors
    int result = typecheck_program(ast);
    ASSERT_EQ(1, result);
    free_ast_node(ast);
}

TEST(typechecker_loop_conditions) {
    ASTNode* ast = parse_source("main() { i = 0; while i < 5 { i = i + 1; } }");
    ASSERT_NOT_NULL(ast);
    int result = typecheck_program(ast);
    ASSERT_EQ(1, result);
    free_ast_node(ast);
}

/* #2200: helpers for the `cfn` tests below — find the first node of a kind. */
static ASTNode* find_first_node(ASTNode* n, ASTNodeType kind, const char* value) {
    if (!n) return NULL;
    if (n->type == kind && (!value || (n->value && strcmp(n->value, value) == 0))) return n;
    for (int i = 0; i < n->child_count; i++) {
        ASTNode* f = find_first_node(n->children[i], kind, value);
        if (f) return f;
    }
    return NULL;
}

/* A `x: Name` annotation and a `p as Name` cast both resolve to the cfn's
 * signature: the same TYPE_FUNCTION (is_fnptr) that `fn(int, ptr)` gives,
 * and the cast becomes the function-pointer cast node. */
TEST(typechecker_cfn_annotation_and_cast_resolve_to_fnptr_signature) {
    ASTNode* ast = parse_source(
        "extern getp() -> ptr\n"
        "cfn GenBuffers(n: int, ids: ptr)\n"
        "var gl_gen_buffers: GenBuffers = null\n"
        "main() { gl_gen_buffers = getp() as GenBuffers\n"
        "  gl_gen_buffers(1, null) }");
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(1, typecheck_program(ast));
    ASTNode* global = find_first_node(ast, AST_CONST_DECLARATION, "gl_gen_buffers");
    ASSERT_NOT_NULL(global);
    ASSERT_EQ(TYPE_FUNCTION, global->node_type->kind);
    ASSERT_EQ(1, global->node_type->is_fnptr);
    ASSERT_EQ(2, global->node_type->param_count);
    ASSERT_NULL(global->node_type->struct_name);
    ASTNode* cast = find_first_node(ast, AST_PTR_AS_FN_CAST, NULL);
    ASSERT_NOT_NULL(cast);
    ASSERT_EQ(TYPE_PTR, cast->node_type->param_types[1]->kind);
    ASSERT_NULL(find_first_node(ast, AST_VALUE_CAST, NULL));
    free_ast_node(ast);
}

/* A cfn is usable everywhere `fn(...) -> R` is: a parameter, a struct
 * field, a return type, and inside another cfn's signature. */
TEST(typechecker_cfn_in_param_field_return_and_nested_signature) {
    ASTNode* ast = parse_source(
        "struct Thing { value: int }\n"
        "cfn Bump(t: *Thing, by: int) -> *Thing\n"
        "cfn Visit(cb: Bump, t: *Thing)\n"
        "struct Slot { bump: Bump }\n"
        "bump_impl(t: *Thing, by: int) -> *Thing { return t }\n"
        "pick() -> Bump { return bump_impl as Bump }\n"
        "apply(f: Bump, t: *Thing) { f(t, 5) }\n"
        "main() { thing = Thing { value: 0 }\n"
        "  slot = Slot { bump: pick() }\n"
        "  slot.bump(&thing, 1)\n"
        "  apply(slot.bump, &thing) }");
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(1, typecheck_program(ast));
    ASTNode* visit = find_first_node(ast, AST_CFN_TYPE_DEF, "Visit");
    ASSERT_NOT_NULL(visit);
    Type* cb = visit->node_type->param_types[0];
    ASSERT_EQ(TYPE_FUNCTION, cb->kind);
    ASSERT_EQ(1, cb->is_fnptr);
    ASSERT_EQ(2, cb->param_count);
    free_ast_node(ast);
}

TEST(typechecker_cfn_declared_twice_is_an_error) {
    ASTNode* ast = parse_source(
        "cfn Gen(n: int)\n"
        "cfn Gen(n: int, p: ptr)\n"
        "main() { }");
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(0, typecheck_program(ast));
    free_ast_node(ast);
}

/* The operand of `as Name` must be a pointer, as for `as fn(...)`. */
TEST(typechecker_cfn_cast_of_a_string_is_an_error) {
    ASTNode* ast = parse_source(
        "cfn Gen(n: int)\n"
        "main() { s = \"hi\"\n"
        "  g = s as Gen\n"
        "  g(1) }");
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(0, typecheck_program(ast));
    free_ast_node(ast);
}

/* An unknown type name is still an error; a cfn name does not make every
 * bare identifier a function pointer. */
TEST(typechecker_unknown_type_name_still_rejected_beside_a_cfn) {
    ASTNode* ast = parse_source(
        "cfn Gen(n: int)\n"
        "main() { let x: Nope = null }");
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(0, typecheck_program(ast));
    free_ast_node(ast);
}

/* The address of a float- or string-returning function is a valid operand
 * for `as Name` / `as fn(...)`: the check used to infer the bare name, get
 * the RETURN type, and reject anything that was not ptr- or int-shaped. */
TEST(typechecker_float_returning_function_address_casts_to_a_cfn) {
    ASTNode* ast = parse_source(
        "cfn Scale(v: float, s: f32) -> float\n"
        "scale_impl(v: float, s: f32) -> float { return v * s }\n"
        "name_impl(n: int) -> string { return \"x\" }\n"
        "main() { sc = scale_impl as Scale\n"
        "  nm = name_impl as fn(int) -> string\n"
        "  println(\"${sc(2.0, 2.0)} ${nm(1)}\") }");
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(1, typecheck_program(ast));
    free_ast_node(ast);
}
