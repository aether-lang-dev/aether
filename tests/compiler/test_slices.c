/* #1286 first-class slices: parser, coercion, typechecker and codegen units.
 *
 * The runtime helpers (aether_slice.h) are exercised directly too, so a
 * bounds rule is pinned here without building a whole program. */
#include "../runtime/test_harness.h"
#include "../../compiler/parser/lexer.h"
#include "../../compiler/parser/parser.h"
#include "../../compiler/ast.h"
#include "../../compiler/analysis/typechecker.h"
#include "../../compiler/analysis/slice_coerce.h"
#include "../../compiler/codegen/codegen.h"
#include "../../runtime/aether_slice.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static Token** tokenize(const char* source, int* out_count) {
    lexer_init(source);
    Token** tokens = malloc(sizeof(Token*) * 1024);
    int count = 0;
    Token* tok;
    while ((tok = next_token()) != NULL && tok->type != TOKEN_EOF && count < 1023)
        tokens[count++] = tok;
    if (tok) tokens[count++] = tok;
    *out_count = count;
    return tokens;
}

static ASTNode* parse_source(const char* source) {
    int count;
    Token** tokens = tokenize(source, &count);
    Parser* parser = create_parser(tokens, count);
    ASTNode* ast = parse_program(parser);
    free_parser(parser);
    for (int i = 0; i < count; i++) free_token(tokens[i]);
    free(tokens);
    return ast;
}

static ASTNode* find_first(ASTNode* n, ASTNodeType kind, const char* value) {
    if (!n) return NULL;
    if (n->type == kind && (!value || (n->value && strcmp(n->value, value) == 0))) return n;
    for (int i = 0; i < n->child_count; i++) {
        ASTNode* f = find_first(n->children[i], kind, value);
        if (f) return f;
    }
    return NULL;
}

static ASTNode* find_call(ASTNode* n, const char* name) {
    return find_first(n, AST_FUNCTION_CALL, name);
}

static char* generate_c(ASTNode* ast) {
    FILE* out = tmpfile();
    CodeGenerator* gen = create_code_generator(out);
    generate_program(gen, ast);
    fflush(out);
    long n = ftell(out);
    rewind(out);
    char* buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, out);
    buf[got] = '\0';
    fclose(out);
    free_code_generator(gen);
    return buf;
}

/* ---- parser -------------------------------------------------------- */

TEST(slices_parser_sub_slice_shapes) {
    ASTNode* ast = parse_source(
        "main() { arr = [1, 2, 3]\n"
        "  a = arr[1..3]\n  b = arr[1..]\n  c = arr[..2]\n  d = arr[..]\n  e = arr[1] }");
    ASSERT_NOT_NULL(ast);
    ASTNode* a = find_first(ast, AST_SLICE_EXPR, "lo..hi");
    ASSERT_NOT_NULL(a);
    ASSERT_EQ(3, a->child_count);
    ASSERT_EQ(AST_IDENTIFIER, a->children[0]->type);
    ASTNode* b = find_first(ast, AST_SLICE_EXPR, "lo..");
    ASSERT_NOT_NULL(b);
    ASSERT_EQ(2, b->child_count);
    ASTNode* c = find_first(ast, AST_SLICE_EXPR, "..hi");
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(2, c->child_count);
    ASTNode* d = find_first(ast, AST_SLICE_EXPR, "..");
    ASSERT_NOT_NULL(d);
    ASSERT_EQ(1, d->child_count);
    ASTNode* e = find_first(ast, AST_ARRAY_ACCESS, NULL);
    ASSERT_NOT_NULL(e);
    ASSERT_EQ(2, e->child_count);
    free_ast_node(ast);
}

/* ---- type predicates ------------------------------------------------ */

TEST(slices_type_predicates) {
    Type* sized = create_array_type(create_type(TYPE_INT), 4);
    Type* slice = create_array_type(create_type(TYPE_INT), -1);
    Type* by_enum = create_array_type(create_type(TYPE_INT), -1);
    by_enum->index_enum_name = strdup("Dir");
    ASSERT_TRUE(type_is_sized_array(sized));
    ASSERT_FALSE(type_is_slice(sized));
    ASSERT_TRUE(type_is_slice(slice));
    ASSERT_FALSE(type_is_sized_array(slice));
    ASSERT_FALSE(type_is_slice(by_enum));   /* [E]T gets its size later */
    ASSERT_FALSE(type_is_slice(NULL));
    free_type(sized); free_type(slice); free_type(by_enum);
}

/* ---- slice_coerce (pure AST rewriting) ------------------------------ */

static ASTNode* ident_of_type(Type* t) {
    ASTNode* n = create_ast_node(AST_IDENTIFIER, "x", 1, 1);
    n->node_type = t;
    return n;
}

TEST(slices_coerce_sized_array_into_slice_slot_records_length) {
    ASTNode* n = ident_of_type(create_array_type(create_type(TYPE_INT), 5));
    Type* target = create_array_type(create_type(TYPE_INT), -1);
    ASSERT_EQ(1, slice_coerce_slot(&n, target, 0));
    ASSERT_EQ(AST_SLICE_FROM_ARRAY, n->type);
    ASSERT_STREQ("5", n->value);
    ASSERT_TRUE(type_is_slice(n->node_type));
    ASSERT_EQ(TYPE_INT, n->node_type->element_type->kind);
    ASSERT_EQ(0, slice_coerce_slot(&n, target, 0));   /* idempotent */
    ASSERT_EQ(AST_SLICE_FROM_ARRAY, n->type);
    free_ast_node(n); free_type(target);
}

TEST(slices_coerce_null_into_slice_slot_is_empty_slice) {
    ASTNode* n = create_ast_node(AST_NULL_LITERAL, "null", 1, 1);
    n->node_type = create_type(TYPE_PTR);
    Type* target = create_array_type(create_type(TYPE_BYTE), -1);
    ASSERT_EQ(1, slice_coerce_slot(&n, target, 0));
    ASSERT_EQ(AST_SLICE_FROM_ARRAY, n->type);
    ASSERT_STREQ("0", n->value);
    free_ast_node(n); free_type(target);
}

TEST(slices_coerce_slice_into_ptr_and_extern_slots_decays) {
    ASTNode* a = ident_of_type(create_array_type(create_type(TYPE_INT), -1));
    Type* ptr = create_type(TYPE_PTR);
    ASSERT_EQ(1, slice_coerce_slot(&a, ptr, 0));
    ASSERT_EQ(AST_SLICE_TO_PTR, a->type);
    ASSERT_EQ(TYPE_PTR, a->node_type->kind);
    free_ast_node(a); free_type(ptr);

    /* An extern's `T[]` parameter is a C `T*`: the slice decays there too. */
    ASTNode* b = ident_of_type(create_array_type(create_type(TYPE_INT), -1));
    Type* ext_arr = create_array_type(create_type(TYPE_INT), -1);
    ASSERT_EQ(1, slice_coerce_slot(&b, ext_arr, 1));
    ASSERT_EQ(AST_SLICE_TO_PTR, b->type);
    free_ast_node(b);

    /* ...but an Aether `T[]` slot takes the slice as it is. */
    ASTNode* c = ident_of_type(create_array_type(create_type(TYPE_INT), -1));
    ASSERT_EQ(0, slice_coerce_slot(&c, ext_arr, 0));
    ASSERT_EQ(AST_IDENTIFIER, c->type);
    free_ast_node(c); free_type(ext_arr);
}

TEST(slices_coerce_leaves_scalars_and_sized_targets_alone) {
    ASTNode* a = ident_of_type(create_type(TYPE_INT));
    Type* slice = create_array_type(create_type(TYPE_INT), -1);
    ASSERT_EQ(0, slice_coerce_slot(&a, slice, 0));
    ASSERT_EQ(0, slice_coerce_to_ptr(&a));
    free_ast_node(a);
    /* a sized array into a sized-array slot is C's own business */
    ASTNode* b = ident_of_type(create_array_type(create_type(TYPE_INT), 3));
    Type* sized = create_array_type(create_type(TYPE_INT), 3);
    ASSERT_EQ(0, slice_coerce_slot(&b, sized, 0));
    free_ast_node(b); free_type(sized); free_type(slice);
}

/* ---- typechecker ---------------------------------------------------- */

TEST(slices_typechecker_len_is_int_and_sub_slice_is_slice) {
    ASTNode* ast = parse_source(
        "main() { arr = [1, 2, 3]\n  n = arr.len\n  v = arr[1..3]\n  m = v.len }");
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(1, typecheck_program(ast));
    ASTNode* n = find_first(ast, AST_VARIABLE_DECLARATION, "n");
    ASSERT_NOT_NULL(n);
    ASSERT_EQ(TYPE_INT, n->node_type->kind);
    ASTNode* v = find_first(ast, AST_VARIABLE_DECLARATION, "v");
    ASSERT_NOT_NULL(v);
    ASSERT_TRUE(type_is_slice(v->node_type));
    ASSERT_EQ(TYPE_INT, v->node_type->element_type->kind);
    ASTNode* m = find_first(ast, AST_VARIABLE_DECLARATION, "m");
    ASSERT_NOT_NULL(m);
    ASSERT_EQ(TYPE_INT, m->node_type->kind);
    free_ast_node(ast);
}

TEST(slices_typechecker_wraps_array_argument_and_return) {
    ASTNode* ast = parse_source(
        "first(xs: int[]) -> int { return xs[0] }\n"
        "whole(xs: int[]) -> int[] { return xs }\n"
        "main() { arr = [7, 8, 9]\n  a = first(arr)\n  b = whole(arr)\n  c = whole(b) }");
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(1, typecheck_program(ast));
    ASTNode* call = find_call(ast, "first");
    ASSERT_NOT_NULL(call);
    ASSERT_EQ(AST_SLICE_FROM_ARRAY, call->children[0]->type);
    ASSERT_STREQ("3", call->children[0]->value);
    /* a slice into a slice parameter is passed as-is */
    ASTNode* c = find_first(ast, AST_VARIABLE_DECLARATION, "c");
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(AST_IDENTIFIER, c->children[0]->children[0]->type);
    ASSERT_TRUE(type_is_slice(c->node_type));
    free_ast_node(ast);
}

TEST(slices_typechecker_slice_decays_for_extern_ptr_null_and_arithmetic) {
    ASTNode* ast = parse_source(
        "extern free(p: ptr)\n"
        "extern take(p: int[])\n"
        "main() { arr = [1, 2, 3]\n  v = arr[..]\n"
        "  free(v)\n  take(v)\n  z = v == null\n  q = v + 1 }");
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(1, typecheck_program(ast));
    ASTNode* fr = find_call(ast, "free");
    ASSERT_NOT_NULL(fr);
    ASSERT_EQ(AST_SLICE_TO_PTR, fr->children[0]->type);
    ASTNode* tk = find_call(ast, "take");
    ASSERT_NOT_NULL(tk);
    ASSERT_EQ(AST_SLICE_TO_PTR, tk->children[0]->type);
    ASTNode* z = find_first(ast, AST_VARIABLE_DECLARATION, "z");
    ASSERT_NOT_NULL(z);
    ASSERT_EQ(AST_SLICE_TO_PTR, z->children[0]->children[0]->type);
    ASTNode* q = find_first(ast, AST_VARIABLE_DECLARATION, "q");
    ASSERT_NOT_NULL(q);
    ASSERT_EQ(AST_SLICE_TO_PTR, q->children[0]->children[0]->type);
    ASSERT_EQ(TYPE_PTR, q->node_type->kind);
    free_ast_node(ast);
}

TEST(slices_typechecker_rejects_bad_base_and_bound) {
    ASTNode* ast = parse_source("main() { n = 5\n  v = n[0..2] }");
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(0, typecheck_program(ast));
    free_ast_node(ast);
    ast = parse_source("main() { arr = [1, 2]\n  v = arr[0..\"x\"] }");
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(0, typecheck_program(ast));
    free_ast_node(ast);
}

TEST(slices_typechecker_slice_binding_does_not_decay_to_ptr) {
    /* #892 decays a NAMED T[N] array to ptr; a named T[] slice keeps its type. */
    ASTNode* ast = parse_source(
        "main() { arr = [1, 2, 3]\n  p = arr\n  v = arr[..]\n  w = v }");
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(1, typecheck_program(ast));
    ASTNode* p = find_first(ast, AST_VARIABLE_DECLARATION, "p");
    ASSERT_EQ(TYPE_PTR, p->node_type->kind);
    ASTNode* w = find_first(ast, AST_VARIABLE_DECLARATION, "w");
    ASSERT_TRUE(type_is_slice(w->node_type));
    free_ast_node(ast);
}

/* ---- codegen -------------------------------------------------------- */

TEST(slices_codegen_lowers_to_fat_pointer_helpers) {
    ASTNode* ast = parse_source(
        "extern malloc(sz: long) -> ptr\n"
        "extern free(p: ptr)\n"
        "extern raw_ints() -> int[]\n"
        "sum(xs: int[]) -> int { return xs[0] + xs.len }\n"
        "main() { arr = [1, 2, 3]\n  s = sum(arr)\n  v = arr[1..3]\n"
        "  view = malloc(8) as int[]\n  free(view)\n  r = raw_ints() }");
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(1, typecheck_program(ast));
    char* c = generate_c(ast);
    ASSERT_NOT_NULL(strstr(c, "#include \"aether_slice.h\""));
    ASSERT_NOT_NULL(strstr(c, "int sum(AetherSlice xs)"));
    ASSERT_NOT_NULL(strstr(c, "aether_slice_at(xs, (int64_t)(0), sizeof(int)"));
    ASSERT_NOT_NULL(strstr(c, "aether_slice_len(xs)"));
    ASSERT_NOT_NULL(strstr(c, "sum(aether_slice_make((void*)(arr), 3))"));
    ASSERT_NOT_NULL(strstr(c, "aether_slice_sub(aether_slice_make((void*)(arr), 3), (int64_t)(1), (int64_t)(3), sizeof(int)"));
    ASSERT_NOT_NULL(strstr(c, "AetherSlice view = aether_slice_view((void*)(malloc(8)))"));
    ASSERT_NOT_NULL(strstr(c, "free((void*)((int*)(view).ptr))"));
    /* the extern keeps C's shape; the call site wraps the result */
    ASSERT_NOT_NULL(strstr(c, "int* raw_ints("));
    ASSERT_NOT_NULL(strstr(c, "aether_slice_view((void*)(raw_ints()))"));
    free(c);
    free_ast_node(ast);
}

TEST(slices_codegen_extern_param_and_aether_struct_field) {
    ASTNode* ast = parse_source(
        "extern take(p: byte[], n: int)\n"
        "struct Packet { payload: byte[] }\n"
        "main() { buf = [1, 2]\n  p = Packet { payload: buf }\n  take(p.payload, p.payload.len) }");
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(1, typecheck_program(ast));
    char* c = generate_c(ast);
    ASSERT_NOT_NULL(strstr(c, "void take(unsigned char*, int);"));
    ASSERT_NOT_NULL(strstr(c, "AetherSlice payload;"));
    ASSERT_NOT_NULL(strstr(c, ".payload = aether_slice_make((void*)(buf), 2)"));
    ASSERT_NOT_NULL(strstr(c, "take(((unsigned char*)(p.payload).ptr), "));
    free(c);
    free_ast_node(ast);
}

/* ---- runtime helpers ------------------------------------------------ */

TEST(slices_runtime_len_sub_and_from) {
    int backing[5] = {1, 2, 3, 4, 5};
    AetherSlice s = aether_slice_make(backing, 5);
    ASSERT_EQ(5, (int)aether_slice_len(s));
    ASSERT_EQ(4, *(int*)aether_slice_at(s, 3, sizeof(int), "t", 1));
    AetherSlice sub = aether_slice_sub(s, 1, 4, sizeof(int), "t", 2);
    ASSERT_EQ(3, (int)aether_slice_len(sub));
    ASSERT_EQ(2, *(int*)aether_slice_at(sub, 0, sizeof(int), "t", 3));
    *(int*)aether_slice_at(sub, 0, sizeof(int), "t", 4) = 42;
    ASSERT_EQ(42, backing[1]);                       /* shares storage */
    AetherSlice tail = aether_slice_from(s, 4, sizeof(int), "t", 5);
    ASSERT_EQ(1, (int)aether_slice_len(tail));
    AetherSlice empty = aether_slice_sub(s, 5, 5, sizeof(int), "t", 6);
    ASSERT_EQ(0, (int)aether_slice_len(empty));
    AetherSlice view = aether_slice_view(backing);
    ASSERT_EQ(-1, (int)aether_slice_len(view));
    ASSERT_EQ(5, *(int*)aether_slice_at(view, 4, sizeof(int), "t", 7));  /* unchecked */
    AetherSlice bounded = aether_slice_sub(view, 0, 2, sizeof(int), "t", 8);
    ASSERT_EQ(2, (int)aether_slice_len(bounded));
    AetherSlice rest = aether_slice_from(view, 3, sizeof(int), "t", 9);
    ASSERT_EQ(-1, (int)aether_slice_len(rest));      /* still unbounded */
    ASSERT_EQ(4, *(int*)aether_slice_at(rest, 0, sizeof(int), "t", 10));
    AetherSlice fresh = aether_slice_alloc(3, sizeof(int));
    ASSERT_EQ(3, (int)aether_slice_len(fresh));
    ASSERT_EQ(0, *(int*)aether_slice_at(fresh, 2, sizeof(int), "t", 11));   /* zeroed */
    free(fresh.ptr);
}
