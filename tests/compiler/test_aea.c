/* Unit tests for the compiled module artifact codec (#1746). The codec is
 * pure (bytes in, AST out), so every rule the consumer applies before
 * trusting an artifact is checked here without touching the file system;
 * tests/integration/aea_artifacts drives the resolver end to end. */
#include "../runtime/test_harness.h"
#include "../../compiler/aether_aea.h"
#include "../../compiler/aether_defines.h"
#include "../../compiler/aether_error.h"
#include "../../compiler/parser/lexer.h"
#include "../../compiler/parser/parser.h"
#include <stdlib.h>
#include <string.h>

static const char* const RICH_SOURCE =
    "exports (area, pair, apply, Point)\n"
    "const LIMIT = 10\n"
    "const TABLE: int[3] = [1, 2, 3]\n"
    "struct Point {\n    x: int\n    y: float\n}\n"
    "bitstruct Flags: uint32_t {\n    lo: int 0..<30\n    hi: int 30..<32\n}\n"
    "extern memcpy(dst: ptr, src: ptr, n: int) -> ptr\n"
    "cfn Cmp(a: ptr, b: ptr) -> int\n"
    "pair(a: int) -> (int, string) {\n    return a, \"two words\\nand a line\"\n}\n"
    "area(p: Point) -> float {\n    return p.y * 2.0\n}\n"
    "apply(f: fn(int) -> int, v: int) -> int {\n    return f(v)\n}\n"
    "when defined(AEA_TEST_FLAG) {\n    flagged() -> int { return 1 }\n}\n"
    "main() {\n    n, w = pair(LIMIT)\n    sq = | x: int | -> x * x\n"
    "    println(apply(sq, n))\n}\n";

static const char* const REL = "std/demo/module.ae";

static ASTNode* parse_text(const char* source) {
    int count = 0;
    Token** tokens = lexer_tokenize(source, &count);
    if (!tokens) return NULL;
    Parser* parser = create_parser(tokens, count);
    ASTNode* ast = parse_program(parser);
    free_parser(parser);
    free_tokens(tokens, count);
    if (ast) ast_stamp_source_file(ast, "demo/module.ae");
    return ast;
}

static AeaProducer producer_for(const char* source) {
    AeaProducer p = { "1.2.3", "fe-1", REL, source, strlen(source), NULL, 0 };
    return p;
}

static int never_defined(const char* name) { (void)name; return 0; }
static int always_defined(const char* name) { (void)name; return 1; }

static AeaConsumer consumer_for(const char* source) {
    AeaConsumer c = { "1.2.3", "fe-1", REL, source, strlen(source), never_defined };
    return c;
}

/* Encodes RICH_SOURCE's parse with the default producer. */
static char* encode_rich(size_t* len) {
    ASTNode* ast = parse_text(RICH_SOURCE);
    if (!ast) return NULL;
    AeaProducer p = producer_for(RICH_SOURCE);
    const char* err = NULL;
    char* out = aea_encode(ast, &p, len, &err);
    free_ast_node(ast);
    return out;
}

/* Replaces the first `from` in the artifact header with `to`. */
static char* edit_header(const char* data, size_t len, const char* from,
                         const char* to, size_t* out_len) {
    const char* at = strstr(data, from);
    if (!at) return NULL;
    size_t before = (size_t)(at - data), fl = strlen(from), tl = strlen(to);
    *out_len = len - fl + tl;
    char* out = malloc(*out_len + 1);
    memcpy(out, data, before);
    memcpy(out + before, to, tl);
    memcpy(out + before + tl, at + fl, len - before - fl);
    out[*out_len] = '\0';
    return out;
}

static const char* decode_reason(const char* data, size_t len, const AeaConsumer* c) {
    const char* why = NULL;
    ASTNode* ast = aea_decode(data, len, c, &why);
    if (ast) { free_ast_node(ast); return "decoded"; }
    return why ? why : "no reason";
}

/* ---- hashing and paths ---- */

TEST_CATEGORY(aea_hash_is_fnv1a_64, TEST_CATEGORY_COMPILER) {
    ASSERT_TRUE(aea_hash("", 0) == 0xcbf29ce484222325ULL);
    ASSERT_TRUE(aea_hash("a", 1) == 0xaf63dc4c8601ec8cULL);
}

TEST_CATEGORY(aea_path_for_module_facade, TEST_CATEGORY_COMPILER) {
    char* rel = NULL;
    char* p = aea_artifact_path_for("/opt/ae/share/aether/std/cryptography/md2/module.ae", &rel);
    ASSERT_NOT_NULL(p);
    ASSERT_STREQ("/opt/ae/lib/aether/modules/std/cryptography/md2.aea", p);
    ASSERT_STREQ("std/cryptography/md2/module.ae", rel);
    free(p); free(rel);
}

TEST_CATEGORY(aea_path_for_package_file_keeps_its_name, TEST_CATEGORY_COMPILER) {
    char* rel = NULL;
    char* p = aea_artifact_path_for("/opt/ae/bin/../share/aether/std/jsonpath/parser.ae", &rel);
    ASSERT_NOT_NULL(p);
    ASSERT_STREQ("/opt/ae/bin/../lib/aether/modules/std/jsonpath/parser.aea", p);
    ASSERT_STREQ("std/jsonpath/parser.ae", rel);
    free(p); free(rel);
}

TEST_CATEGORY(aea_path_accepts_windows_separators, TEST_CATEGORY_COMPILER) {
    char* rel = NULL;
    char* p = aea_artifact_path_for("C:\\ae\\bin/../share/aether/std\\math\\module.ae", &rel);
    ASSERT_NOT_NULL(p);
    ASSERT_STREQ("C:\\ae\\bin/../lib/aether/modules/std/math.aea", p);
    ASSERT_STREQ("std/math/module.ae", rel);
    free(p); free(rel);
}

TEST_CATEGORY(aea_path_uses_innermost_install_root, TEST_CATEGORY_COMPILER) {
    char* p = aea_artifact_path_for("/share/aether/x/share/aether/std/a/module.ae", NULL);
    ASSERT_NOT_NULL(p);
    ASSERT_STREQ("/share/aether/x/lib/aether/modules/std/a.aea", p);
    free(p);
}

TEST_CATEGORY(aea_path_none_outside_an_install, TEST_CATEGORY_COMPILER) {
    char* rel = (char*)"untouched";
    ASSERT_NULL(aea_artifact_path_for("std/cryptography/md2/module.ae", &rel));
    ASSERT_NULL(rel);
    ASSERT_NULL(aea_artifact_path_for("/home/me/project/lib/util/module.ae", NULL));
    ASSERT_NULL(aea_artifact_path_for("/opt/ae/share/aether/std/x/module.c", NULL));
    ASSERT_NULL(aea_artifact_path_for("/opt/ae/share/aether/std/../../etc/module.ae", NULL));
    ASSERT_NULL(aea_artifact_path_for("/opt/ae/share/aether/.ae", NULL));
    ASSERT_NULL(aea_artifact_path_for(NULL, NULL));
}

TEST_CATEGORY(aea_module_names_follow_the_import_path, TEST_CATEGORY_COMPILER) {
    char* a = aea_module_name_for("std/cryptography/md2/module.ae");
    char* b = aea_module_name_for("std/jsonpath/parser.ae");
    ASSERT_STREQ("std.cryptography.md2", a);
    ASSERT_STREQ("std.jsonpath.parser", b);
    free(a); free(b);
}

/* ---- encode/decode round trip ---- */

TEST_CATEGORY(aea_round_trip_is_byte_stable, TEST_CATEGORY_COMPILER) {
    int errors_before = aether_error_count();
    size_t len = 0;
    char* first = encode_rich(&len);
    ASSERT_EQ(errors_before, aether_error_count());
    ASSERT_NOT_NULL(first);

    AeaConsumer c = consumer_for(RICH_SOURCE);
    const char* why = NULL;
    ASTNode* back = aea_decode(first, len, &c, &why);
    ASSERT_NOT_NULL(back);
    /* The decoded AST carries no path; the consumer stamps its own. */
    ASSERT_NULL(back->source_file);
    ast_stamp_source_file(back, "elsewhere/module.ae");

    AeaProducer p = producer_for(RICH_SOURCE);
    size_t len2 = 0;
    const char* err = NULL;
    char* second = aea_encode(back, &p, &len2, &err);
    ASSERT_NOT_NULL(second);
    ASSERT_EQ(len, len2);
    ASSERT_TRUE(memcmp(first, second, len) == 0);
    free(first); free(second);
    free_ast_node(back);
}

static ASTNode* find_top(ASTNode* ast, ASTNodeType type, const char* value) {
    for (int i = 0; i < ast->child_count; i++) {
        ASTNode* c = ast->children[i];
        if (c->type == type && c->value && strcmp(c->value, value) == 0) return c;
    }
    return NULL;
}

TEST_CATEGORY(aea_round_trip_keeps_types_positions_and_text, TEST_CATEGORY_COMPILER) {
    ASTNode* orig = parse_text(RICH_SOURCE);
    ASSERT_NOT_NULL(orig);
    size_t len = 0;
    char* data = encode_rich(&len);
    AeaConsumer c = consumer_for(RICH_SOURCE);
    ASTNode* back = aea_decode(data, len, &c, NULL);
    ASSERT_NOT_NULL(back);
    ASSERT_EQ(orig->child_count, back->child_count);

    ASTNode* o = find_top(orig, AST_CFN_TYPE_DEF, "Cmp");
    ASTNode* b = find_top(back, AST_CFN_TYPE_DEF, "Cmp");
    ASSERT_NOT_NULL(o);
    ASSERT_NOT_NULL(b);
    ASSERT_NOT_NULL(b->node_type);
    ASSERT_EQ(o->node_type->kind, b->node_type->kind);
    ASSERT_EQ(o->node_type->is_fnptr, b->node_type->is_fnptr);
    ASSERT_EQ(o->node_type->param_count, b->node_type->param_count);
    ASSERT_EQ(o->line, b->line);
    ASSERT_EQ(o->column, b->column);

    o = find_top(orig, AST_FUNCTION_DEFINITION, "pair");
    b = find_top(back, AST_FUNCTION_DEFINITION, "pair");
    ASSERT_NOT_NULL(b);
    ASSERT_NOT_NULL(b->node_type);
    ASSERT_EQ(o->node_type->tuple_count, b->node_type->tuple_count);
    ASSERT_EQ(2, b->node_type->tuple_count);
    free(data);
    free_ast_node(orig);
    free_ast_node(back);
}

/* ---- encoder refusals ---- */

TEST_CATEGORY(aea_encode_refuses_analysed_asts, TEST_CATEGORY_COMPILER) {
    AeaProducer p = producer_for(RICH_SOURCE);
    size_t len = 0;
    const char* err = NULL;

    ASTNode* ast = parse_text(RICH_SOURCE);
    ast->children[1]->is_imported = 1;
    ASSERT_NULL(aea_encode(ast, &p, &len, &err));
    ASSERT_NOT_NULL(err);
    free_ast_node(ast);

    ast = parse_text(RICH_SOURCE);
    ASTNode* fn = find_top(ast, AST_FUNCTION_DEFINITION, "area");
    ASSERT_NOT_NULL(fn->node_type);
    fn->node_type->compound_node = fn;
    err = NULL;
    ASSERT_NULL(aea_encode(ast, &p, &len, &err));
    ASSERT_NOT_NULL(err);
    fn->node_type->compound_node = NULL;
    free_ast_node(ast);
}

TEST_CATEGORY(aea_encode_refuses_nodes_from_another_file, TEST_CATEGORY_COMPILER) {
    ASTNode* ast = parse_text(RICH_SOURCE);
    free(ast->children[2]->source_file);
    ast->children[2]->source_file = strdup("other.ae");
    AeaProducer p = producer_for(RICH_SOURCE);
    size_t len = 0;
    const char* err = NULL;
    ASSERT_NULL(aea_encode(ast, &p, &len, &err));
    ASSERT_NOT_NULL(err);
    free_ast_node(ast);
}

TEST_CATEGORY(aea_encode_refuses_bad_producer_facts, TEST_CATEGORY_COMPILER) {
    ASTNode* ast = parse_text(RICH_SOURCE);
    size_t len = 0;
    const char* err = NULL;
    static const char* const bad_rels[] = {
        "/abs/std/x/module.ae", "std/../x/module.ae", "std/./x.ae", "std//x.ae",
        "std\\x\\module.ae", "C:std/x.ae", "std/has space.ae", ""
    };
    for (size_t i = 0; i < sizeof(bad_rels) / sizeof(bad_rels[0]); i++) {
        AeaProducer p = producer_for(RICH_SOURCE);
        p.source_rel = bad_rels[i];
        ASSERT_NULL(aea_encode(ast, &p, &len, &err));
    }
    AeaProducer p = producer_for(RICH_SOURCE);
    p.frontend_id = "unknown";
    ASSERT_NULL(aea_encode(ast, &p, &len, &err));
    free_ast_node(ast);
}

/* ---- decoder compatibility rules ---- */

TEST_CATEGORY(aea_decode_rejects_another_toolchain, TEST_CATEGORY_COMPILER) {
    size_t len = 0;
    char* data = encode_rich(&len);
    AeaConsumer c = consumer_for(RICH_SOURCE);
    c.aether_version = "1.2.4";
    ASSERT_STREQ("built by another compiler version", decode_reason(data, len, &c));
    c = consumer_for(RICH_SOURCE);
    c.frontend_id = "fe-2";
    ASSERT_STREQ("built by another compiler front end", decode_reason(data, len, &c));
    c.frontend_id = "unknown";
    ASSERT_STREQ("built by another compiler front end", decode_reason(data, len, &c));

    size_t n = 0;
    char* other = edit_header(data, len, "AEA 1\n", "AEA 2\n", &n);
    c = consumer_for(RICH_SOURCE);
    ASSERT_STREQ("artifact format version differs", decode_reason(other, n, &c));
    free(other);
    free(data);
}

TEST_CATEGORY(aea_decode_rejects_another_or_changed_source, TEST_CATEGORY_COMPILER) {
    size_t len = 0;
    char* data = encode_rich(&len);
    AeaConsumer c = consumer_for(RICH_SOURCE);
    c.source_rel = "std/other/module.ae";
    ASSERT_STREQ("built from another source file", decode_reason(data, len, &c));

    const char* edited = "exports (area)\n";
    c = consumer_for(edited);
    ASSERT_STREQ("source changed since the artifact was built", decode_reason(data, len, &c));
    free(data);
}

TEST_CATEGORY(aea_decode_checks_recorded_build_symbols, TEST_CATEGORY_COMPILER) {
    ASTNode* ast = parse_text(RICH_SOURCE);
    AeaDefine defs[] = { { "AEA_TEST_FLAG", 0 } };
    AeaProducer p = producer_for(RICH_SOURCE);
    p.defines = defs;
    p.define_count = 1;
    size_t len = 0;
    char* data = aea_encode(ast, &p, &len, NULL);
    ASSERT_NOT_NULL(data);
    ASSERT_NOT_NULL(strstr(data, "\ndefine AEA_TEST_FLAG 0\n"));

    AeaConsumer c = consumer_for(RICH_SOURCE);
    ASSERT_STREQ("decoded", decode_reason(data, len, &c));
    c.define_is_set = always_defined;
    ASSERT_STREQ("a build symbol the module tests is set differently",
                 decode_reason(data, len, &c));
    free(data);
    free_ast_node(ast);
}

TEST_CATEGORY(aea_decode_rejects_damage, TEST_CATEGORY_COMPILER) {
    size_t len = 0;
    char* data = encode_rich(&len);
    AeaConsumer c = consumer_for(RICH_SOURCE);

    ASSERT_STREQ("artifact payload is truncated", decode_reason(data, len - 1, &c));

    char* flipped = malloc(len);
    memcpy(flipped, data, len);
    flipped[len - 5] ^= 0x01;
    ASSERT_STREQ("artifact payload is damaged", decode_reason(flipped, len, &c));
    free(flipped);

    ASSERT_STREQ("not an artifact", decode_reason("hello\n", 6, &c));
    ASSERT_STREQ("not an artifact", decode_reason("", 0, &c));

    size_t n = 0;
    char* extra = edit_header(data, len, "module ", "future_key 1\nmodule ", &n);
    ASSERT_STREQ("artifact header has a key this compiler does not know",
                 decode_reason(extra, n, &c));
    free(extra);

    char* missing = edit_header(data, len, "source_hash ", "module_hash ", &n);
    ASSERT_STREQ("artifact header has a key this compiler does not know",
                 decode_reason(missing, n, &c));
    free(missing);
    free(data);
}

TEST_CATEGORY(aea_decode_rejects_a_well_hashed_but_malformed_payload, TEST_CATEGORY_COMPILER) {
    /* A payload whose hash matches but whose grammar does not: the decoder
     * must fail cleanly rather than trust the hash. */
    static const char body[] = "N 1 1 1 0 0 0 0 ~ ~ ~ 5\n";
    char header[512];
    AeaConsumer c = consumer_for(RICH_SOURCE);
    int hn = snprintf(header, sizeof(header),
        "AEA 1\naether_version 1.2.3\nfrontend fe-1\nsource %s\n"
        "source_hash %016llx\npayload_hash %016llx\npayload %zu\n",
        REL, (unsigned long long)aea_hash(RICH_SOURCE, strlen(RICH_SOURCE)),
        (unsigned long long)aea_hash(body, strlen(body)), strlen(body));
    char buf[1024];
    memcpy(buf, header, (size_t)hn);
    memcpy(buf + hn, body, strlen(body));
    ASSERT_STREQ("artifact payload is malformed", decode_reason(buf, (size_t)hn + strlen(body), &c));
}

/* ---- the codec's view of the AST ---- */

TEST_CATEGORY(aea_codec_covers_every_ast_field, TEST_CATEGORY_COMPILER) {
    /* A new field in ASTNode or Type is state the codec does not carry:
     * encode_node/encode_type and decode_node/decode_type must learn it (or
     * refuse it, as they do compound_node) and AEA_FORMAT_VERSION must be
     * bumped. This size check is the tripwire; update it with them.
     *
     * ASTNode grew to 104 with `source_name` (#2292). That field is set only
     * at codegen (NULL after a parse), and an artifact holds a fresh parse,
     * so the codec correctly does not carry it and the wire format — and
     * AEA_FORMAT_VERSION — is unchanged. */
    /* Type grew to 136 with `closure_literal` (#2460), a type-checker
     * back-pointer like compound_node: NULL after a parse and refused by
     * encode_type, so the wire format and AEA_FORMAT_VERSION are unchanged
     * as well. */
    if (sizeof(void*) == 8 && sizeof(int) == 4) {
        ASSERT_EQ(104, (int)sizeof(ASTNode));
        ASSERT_EQ(136, (int)sizeof(Type));
    }
}

/* ---- `when defined` answers recorded during a parse ---- */

TEST_CATEGORY(aea_define_queries_are_recorded_once_with_answers, TEST_CATEGORY_COMPILER) {
    aether_defines_clear();
    aether_define_add("ON");
    aether_define_record_queries(1);
    ASSERT_EQ(1, aether_define_is_set("ON"));
    ASSERT_EQ(0, aether_define_is_set("OFF"));
    ASSERT_EQ(1, aether_define_is_set("ON"));
    aether_define_record_queries(0);
    ASSERT_EQ(0, aether_define_is_set("LATER"));   /* not recording */

    ASSERT_EQ(2, aether_define_query_count());
    ASSERT_FALSE(aether_define_query_overflowed());
    ASSERT_STREQ("ON", aether_define_query_name(0));
    ASSERT_EQ(1, aether_define_query_value(0));
    ASSERT_STREQ("OFF", aether_define_query_name(1));
    ASSERT_EQ(0, aether_define_query_value(1));
    ASSERT_NULL(aether_define_query_name(2));

    aether_define_record_queries(1);   /* a new recording starts empty */
    ASSERT_EQ(0, aether_define_query_count());
    aether_define_record_queries(0);
    aether_defines_clear();
}

TEST_CATEGORY(aea_define_query_log_reports_overflow, TEST_CATEGORY_COMPILER) {
    aether_define_record_queries(1);
    char name[32];
    for (int i = 0; i < 200; i++) {
        snprintf(name, sizeof(name), "SYM_%d", i);
        aether_define_is_set(name);
    }
    aether_define_record_queries(0);
    ASSERT_TRUE(aether_define_query_overflowed());
}

TEST_CATEGORY(aea_parse_of_when_region_asks_the_define_table, TEST_CATEGORY_COMPILER) {
    aether_defines_clear();
    aether_define_record_queries(1);
    ASTNode* ast = parse_text(RICH_SOURCE);
    aether_define_record_queries(0);
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(1, aether_define_query_count());
    ASSERT_STREQ("AEA_TEST_FLAG", aether_define_query_name(0));
    ASSERT_EQ(0, aether_define_query_value(0));
    free_ast_node(ast);
}

/* Rewrites the payload_hash line in place so it matches the (edited)
 * payload, the way a deliberate or unlucky edit would. */
static void rehash_payload(char* data, size_t len) {
    char* ph = strstr(data, "\npayload_hash ") + strlen("\npayload_hash ");
    char* pl = strstr(data, "\npayload ");
    char* body = strchr(pl + 1, '\n') + 1;
    char hex[17];
    snprintf(hex, sizeof(hex), "%016llx",
             (unsigned long long)aea_hash(body, len - (size_t)(body - data)));
    memcpy(ph, hex, 16);
}

TEST_CATEGORY(aea_decode_survives_every_single_byte_corruption, TEST_CATEGORY_COMPILER) {
    /* The payload hash catches accidental damage; this checks the decoder
     * itself stays in bounds when the hash is made to match. Every byte of a
     * small module's payload is replaced in turn; each decode either fails
     * cleanly or yields an AST that frees cleanly (valgrind/ASan runs of the
     * test runner check the "cleanly"). */
    static const char* const SMALL =
        "exports (f)\nstruct P { x: int }\nf(a: int, s: string) -> (int, string) {\n"
        "    return a + 1, s\n}\n";
    ASTNode* ast = parse_text(SMALL);
    ASSERT_NOT_NULL(ast);
    AeaProducer p = producer_for(SMALL);
    size_t len = 0;
    char* data = aea_encode(ast, &p, &len, NULL);
    free_ast_node(ast);
    ASSERT_NOT_NULL(data);
    AeaConsumer c = consumer_for(SMALL);

    size_t body = (size_t)(strchr(strstr(data, "\npayload ") + 1, '\n') + 1 - data);
    static const char replacements[] = { '0', '9', ' ', '~', 'N', 'T', '\n', ':', '-' };
    char* copy = malloc(len + 1);
    int decoded = 0, refused = 0;
    for (size_t i = body; i < len; i++) {
        for (size_t r = 0; r < sizeof(replacements); r++) {
            if (data[i] == replacements[r]) continue;
            memcpy(copy, data, len + 1);
            copy[i] = replacements[r];
            rehash_payload(copy, len);
            const char* why = NULL;
            ASTNode* got = aea_decode(copy, len, &c, &why);
            if (got) { decoded++; free_ast_node(got); }
            else { refused++; ASSERT_NOT_NULL(why); }
        }
    }
    free(copy);
    free(data);
    ASSERT_TRUE(refused > 0);
    ASSERT_TRUE(decoded + refused > 100);
}
