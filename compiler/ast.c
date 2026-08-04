#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "ast.h"
#include <limits.h>
#include "../std/mem/aether_grow.h"

// Type functions
Type* create_type(TypeKind kind) {
    Type* type = malloc(sizeof(Type));
    type->kind = kind;
    type->element_type = NULL;
    type->array_size = -1;
    type->struct_name = NULL;
    type->c_alias = NULL;
    type->distinct_name = NULL;
    type->index_enum_name = NULL;   // #1044 enum-indexed array marker
    type->tuple_types = NULL;
    type->tuple_count = 0;
    type->tuple_heap_flags = NULL;
    type->param_types = NULL;
    type->param_count = 0;
    type->return_type = NULL;
    /* Default to closure (`_AeClosure`-shaped). The fnptr-cast parser
     * path (`expr as fn(...)`) flips this on a freshly-created
     * TYPE_FUNCTION; closure types created via other paths (typechecker
     * builtins like `unbox_closure`, function-definition return types)
     * must NOT inherit garbage from malloc. */
    type->is_fnptr = 0;
    type->compound_node = NULL;
    type->is_result = 0;   /* #913 `T!` marker — must not inherit malloc garbage */
    return type;
}

Type* create_array_type(Type* element_type, int size) {
    Type* type = create_type(TYPE_ARRAY);
    type->element_type = element_type;
    type->array_size = size;
    return type;
}

// #340 `T?` — optional over `inner`. `inner` is adopted (not cloned).
Type* create_optional_type(Type* inner) {
    Type* type = create_type(TYPE_OPTIONAL);
    type->element_type = inner;
    return type;
}

Type* create_actor_ref_type(Type* actor_type) {
    Type* type = create_type(TYPE_ACTOR_REF);
    type->element_type = actor_type;
    return type;
}

// #1046 `bit_set[E]`, a set of members of enum `element_enum`, backed by an
// unsigned 64-bit word. `element_enum` is adopted (not cloned). Nominal;
// clone_type/free_type handle element_type generically.
Type* create_bitset_type(Type* element_enum) {
    Type* type = create_type(TYPE_BITSET);
    type->element_type = element_enum;
    return type;
}

// #1132 `bitstruct Name : uint8_t { ... }`. `struct_name` is the bitstruct's
// name; `element_type` is the backing integer type (adopted, not cloned).
// Nominal — see types_equal below. clone_type/free_type already handle both
// slots generically.
Type* create_bitstruct_type(const char* name, Type* backing) {
    Type* type = create_type(TYPE_BITSTRUCT);
    type->struct_name = name ? strdup(name) : NULL;
    type->element_type = backing;
    return type;
}

Type* create_tuple_type(int count, ...) {
    Type* type = create_type(TYPE_TUPLE);
    type->tuple_count = count;
    type->tuple_types = malloc(count * sizeof(Type*));
    va_list args;
    va_start(args, count);
    for (int i = 0; i < count; i++) {
        type->tuple_types[i] = va_arg(args, Type*);
    }
    va_end(args);
    return type;
}

// #913 fallible result `T!`. Represented as the existing `(T, string)`
// (value, err) tuple so it is ABI-interchangeable with the stdlib convention;
// the `is_result` flag distinguishes it from a plain 2-tuple so `expr!`
// propagates (not panics) in a result-returning function and `or {}` applies.
Type* create_result_type(Type* inner) {
    Type* t = create_tuple_type(2, inner, create_type(TYPE_STRING));
    t->is_result = 1;
    return t;
}

// #914 sum/variant type. `struct_name` holds the sum's name; the variant
// Types are filled into `tuple_types[]` by the typechecker once the named
// variant structs resolve. clone_type/free_type already handle both fields
// generically, so a TYPE_SUM round-trips like any other Type.
Type* create_sum_type(const char* name) {
    Type* type = create_type(TYPE_SUM);
    if (name) type->struct_name = strdup(name);
    type->tuple_types = NULL;
    type->tuple_count = 0;
    return type;
}

Type* create_function_type(int param_count, Type** param_types, Type* return_type) {
    Type* type = create_type(TYPE_FUNCTION);
    type->param_count = param_count;
    if (param_count > 0 && param_types) {
        type->param_types = malloc(param_count * sizeof(Type*));
        for (int i = 0; i < param_count; i++) {
            type->param_types[i] = param_types[i];
        }
    }
    type->return_type = return_type;
    return type;
}

void free_type(Type* type) {
    if (type) {
        if (type->element_type) {
            free_type(type->element_type);
        }
        if (type->struct_name) {
            free(type->struct_name);
        }
        if (type->c_alias) {
            free(type->c_alias);
        }
        if (type->distinct_name) {
            free(type->distinct_name);
        }
        if (type->index_enum_name) {
            free(type->index_enum_name);
        }
        if (type->tuple_types) {
            for (int i = 0; i < type->tuple_count; i++) {
                free_type(type->tuple_types[i]);
            }
            free(type->tuple_types);
        }
        /* Per-tuple-element heap-ownership flags (issue #420). NULL
         * unless the parser saw at least one `@heap` / `@borrow`
         * marker on the tuple type. */
        if (type->tuple_heap_flags) {
            free(type->tuple_heap_flags);
        }
        if (type->param_types) {
            for (int i = 0; i < type->param_count; i++) {
                free_type(type->param_types[i]);
            }
            free(type->param_types);
        }
        if (type->return_type) {
            free_type(type->return_type);
        }
        free(type);
    }
}

const char* type_to_string(Type* type) {
    if (!type) return "UNKNOWN";
    
    switch (type->kind) {
        case TYPE_INT: return "int";
        case TYPE_INT64: return "long";
        case TYPE_UINT64: return "uint64";
        case TYPE_DURATION: return "Duration";
        case TYPE_FLOAT: return "float";
        case TYPE_LONGDOUBLE: return "longdouble";
        case TYPE_FLOAT32: return "f32";
        case TYPE_BOOL: return "bool";
        case TYPE_BYTE: return "byte";
        case TYPE_STRING: return "string";
        case TYPE_VOID: return "void";
        case TYPE_PTR: return "ptr";
        case TYPE_MESSAGE: return "Message";
        case TYPE_STRUCT: {
            static char buffer[256];
            snprintf(buffer, sizeof(buffer), "struct %s", 
                    type->struct_name ? type->struct_name : "unnamed");
            return buffer;
        }
        case TYPE_ARRAY: {
            static char buffer[256];
            if (type->index_enum_name) {
                // #1044 enum-indexed array `[E]T`
                snprintf(buffer, sizeof(buffer), "[%s]%s",
                        type->index_enum_name,
                        type->element_type ? type_to_string(type->element_type) : "?");
            } else {
                snprintf(buffer, sizeof(buffer), "%s[%d]",
                        type_to_string(type->element_type),
                        type->array_size);
            }
            return buffer;
        }
        case TYPE_ACTOR_REF: {
            static char buffer[256];
            snprintf(buffer, sizeof(buffer), "ActorRef<%s>", 
                    type_to_string(type->element_type));
            return buffer;
        }
        case TYPE_TUPLE: {
            static char buffer[512];
            int pos = snprintf(buffer, sizeof(buffer), "(");
            for (int i = 0; i < type->tuple_count && pos < (int)sizeof(buffer) - 10; i++) {
                if (i > 0) pos += snprintf(buffer + pos, sizeof(buffer) - pos, ", ");
                pos += snprintf(buffer + pos, sizeof(buffer) - pos, "%s",
                               type_to_string(type->tuple_types[i]));
            }
            snprintf(buffer + pos, sizeof(buffer) - pos, ")");
            return buffer;
        }
        case TYPE_FUNCTION: {
            static char buffer[512];
            int pos = snprintf(buffer, sizeof(buffer), "|");
            for (int i = 0; i < type->param_count && pos < (int)sizeof(buffer) - 20; i++) {
                if (i > 0) pos += snprintf(buffer + pos, sizeof(buffer) - pos, ", ");
                pos += snprintf(buffer + pos, sizeof(buffer) - pos, "%s",
                               type->param_types ? type_to_string(type->param_types[i]) : "?");
            }
            pos += snprintf(buffer + pos, sizeof(buffer) - pos, "| -> %s",
                           type->return_type ? type_to_string(type->return_type) : "void");
            return buffer;
        }
        case TYPE_OPTIONAL: {
            static char buffer[256];
            snprintf(buffer, sizeof(buffer), "%s?",
                     type->element_type ? type_to_string(type->element_type) : "?");
            return buffer;
        }
        case TYPE_SUM:
            return type->struct_name ? type->struct_name : "sum";
        case TYPE_ENUM:
            return type->struct_name ? type->struct_name : "enum";
        case TYPE_BITSTRUCT:
            return type->struct_name ? type->struct_name : "bitstruct";
        case TYPE_ISOLATED: {
            static char buffer[256];
            snprintf(buffer, sizeof(buffer), "Isolated[%s]",
                     type->element_type ? type_to_string(type->element_type) : "?");
            return buffer;
        }
        case TYPE_BITSET: {
            static char buffer[256];
            snprintf(buffer, sizeof(buffer), "bit_set[%s]",
                     type->element_type ? type_to_string(type->element_type) : "?");
            return buffer;
        }
        default: return "UNKNOWN";
    }
}

int types_equal(Type* a, Type* b) {
    if (!a || !b) return a == b;
    if (a->kind != b->kind) return 0;
    if (a->array_size != b->array_size) return 0;

    // #480 distinct types: a distinct type equals only the same-named distinct
    // type; a distinct type and its base (one name set, the other NULL) are
    // not equal even when `kind` matches.
    {
        const char* da = a->distinct_name;
        const char* db = b->distinct_name;
        if (da || db) {
            if (!da || !db) return 0;
            return strcmp(da, db) == 0;
        }
    }
    
    // For struct types, compare names
    if (a->kind == TYPE_STRUCT) {
        if (!a->struct_name || !b->struct_name) return 0;
        return strcmp(a->struct_name, b->struct_name) == 0;
    }

    // #914 sum types are nominal: equal iff they name the same sum.
    if (a->kind == TYPE_SUM) {
        if (!a->struct_name || !b->struct_name) return 0;
        return strcmp(a->struct_name, b->struct_name) == 0;
    }

    // #1044 enums are nominal: equal iff they name the same enum.
    if (a->kind == TYPE_ENUM) {
        if (!a->struct_name || !b->struct_name) return 0;
        return strcmp(a->struct_name, b->struct_name) == 0;
    }

    // #1132 bitstructs are nominal too: equal iff they name the same bitstruct.
    // Two bitstructs over the same backing integer are still different types.
    if (a->kind == TYPE_BITSTRUCT) {
        if (!a->struct_name || !b->struct_name) return 0;
        return strcmp(a->struct_name, b->struct_name) == 0;
    }

    // For function types, compare param types and return type
    if (a->kind == TYPE_FUNCTION) {
        if (a->param_count != b->param_count) return 0;
        for (int i = 0; i < a->param_count; i++) {
            if (!types_equal(a->param_types[i], b->param_types[i])) return 0;
        }
        return types_equal(a->return_type, b->return_type);
    }

    // #1044 enum-indexed arrays are distinguished by their index enum: a
    // `[Dir]string` is neither a plain `string[]` nor a `[Color]string`.
    if (a->index_enum_name || b->index_enum_name) {
        if (!a->index_enum_name || !b->index_enum_name) return 0;
        if (strcmp(a->index_enum_name, b->index_enum_name) != 0) return 0;
    }

    return types_equal(a->element_type, b->element_type);
}

Type* clone_type(Type* type) {
    if (!type) return NULL;

    Type* new_type = create_type(type->kind);
    new_type->array_size = type->array_size;
    
    if (type->element_type) {
        new_type->element_type = clone_type(type->element_type);
    }
    
    if (type->struct_name) {
        new_type->struct_name = strdup(type->struct_name);
    }
    if (type->c_alias) {
        new_type->c_alias = strdup(type->c_alias);
    }
    if (type->distinct_name) {
        new_type->distinct_name = strdup(type->distinct_name);
    }
    if (type->index_enum_name) {
        new_type->index_enum_name = strdup(type->index_enum_name);
    }

    if (type->tuple_types && type->tuple_count > 0) {
        new_type->tuple_count = type->tuple_count;
        new_type->tuple_types = malloc(type->tuple_count * sizeof(Type*));
        for (int i = 0; i < type->tuple_count; i++) {
            new_type->tuple_types[i] = clone_type(type->tuple_types[i]);
        }
        /* Clone per-position heap flags (issue #420). The flags
         * array is parallel to tuple_types; allocated lazily, so
         * NULL on the source means "all unannotated, default to
         * borrow" — clone preserves that NULL. */
        if (type->tuple_heap_flags) {
            new_type->tuple_heap_flags = malloc(type->tuple_count * sizeof(int));
            for (int i = 0; i < type->tuple_count; i++) {
                new_type->tuple_heap_flags[i] = type->tuple_heap_flags[i];
            }
        }
    }

    if (type->param_types && type->param_count > 0) {
        new_type->param_count = type->param_count;
        new_type->param_types = malloc(type->param_count * sizeof(Type*));
        for (int i = 0; i < type->param_count; i++) {
            new_type->param_types[i] = clone_type(type->param_types[i]);
        }
    }
    if (type->return_type) {
        new_type->return_type = clone_type(type->return_type);
    }
    new_type->is_fnptr = type->is_fnptr;
    new_type->compound_node = type->compound_node;  // borrowed; AST owns it.
    new_type->is_result = type->is_result;          // #913 `T!` marker

    return new_type;
}

int is_string_seq_ptr_type(const Type* t) {
    return t && t->kind == TYPE_PTR && t->element_type &&
           t->element_type->kind == TYPE_STRUCT &&
           t->element_type->struct_name &&
           strcmp(t->element_type->struct_name, "StringSeq") == 0;
}

Type* make_string_seq_ptr_type(void) {
    Type* t = create_type(TYPE_PTR);
    t->element_type = create_type(TYPE_STRUCT);
    t->element_type->struct_name = strdup("StringSeq");
    return t;
}

void* aether_xrealloc(void* ptr, size_t size) {
    void* grown = realloc(ptr, size);
    if (!grown && size != 0) {
        fprintf(stderr, "Fatal: out of memory (requested %zu bytes)\n", size);
        free(ptr);
        exit(1);
    }
    return grown;
}

// AST Node functions
ASTNode* create_ast_node(ASTNodeType type, const char* value, int line, int column) {
    ASTNode* node = malloc(sizeof(ASTNode));
    node->type = type;
    node->value = value ? strdup(value) : NULL;
    node->node_type = NULL;
    node->children = NULL;
    node->child_count = 0;
    node->child_capacity = 0;
    node->line = line;
    node->column = column;
    node->annotation = NULL;
    node->is_imported = 0;
    node->bit_width = 0;
    node->bit_lo = 0;
    node->bit_hi = 0;
    node->source_file = NULL;
    node->type_inferred = 0;
    return node;
}

void add_child(ASTNode* parent, ASTNode* child) {
    if (!parent || !child) return;
    
    if (parent->child_count >= parent->child_capacity) {
        /* Reallocating to exactly count+1 on every append made building a
         * node with n children O(n^2); this is the compiler's hottest
         * allocation path. */
        size_t want = aether_buf_grow_capacity((size_t)parent->child_capacity, 4,
                                               (size_t)parent->child_count + 1,
                                               sizeof(ASTNode*));
        if (want == 0 || want > (size_t)INT_MAX) {
            fprintf(stderr, "Fatal: AST child array too large\n");
            exit(1);
        }
        ASTNode** new_children = realloc(parent->children, want * sizeof(ASTNode*));
        if (!new_children) {
            fprintf(stderr, "Fatal: out of memory adding AST child\n");
            exit(1);
        }
        parent->children = new_children;
        parent->child_capacity = (int)want;
    }
    parent->children[parent->child_count] = child;
    parent->child_count++;
}

ASTNode* clone_ast_node(ASTNode* node) {
    if (!node) return NULL;

    ASTNode* clone = create_ast_node(node->type, node->value, node->line, node->column);
    clone->node_type = clone_type(node->node_type);
    clone->annotation = node->annotation ? strdup(node->annotation) : NULL;
    clone->is_imported = node->is_imported;
    clone->bit_width = node->bit_width;
    clone->bit_lo = node->bit_lo;
    clone->bit_hi = node->bit_hi;
    clone->source_file = node->source_file ? strdup(node->source_file) : NULL;
    clone->type_inferred = node->type_inferred;

    for (int i = 0; i < node->child_count; i++) {
        add_child(clone, clone_ast_node(node->children[i]));
    }

    return clone;
}

void free_ast_node(ASTNode* node) {
    if (!node) return;
    
    if (node->value) {
        free(node->value);
    }

    if (node->annotation) {
        free(node->annotation);
    }

    if (node->source_file) {
        free(node->source_file);
    }

    if (node->node_type) {
        free_type(node->node_type);
    }
    
    for (int i = 0; i < node->child_count; i++) {
        free_ast_node(node->children[i]);
    }
    
    if (node->children) {
        free(node->children);
    }

    free(node);
}

void ast_stamp_source_file(ASTNode* node, const char* path) {
    if (!node || !path) return;
    if (!node->source_file) {
        node->source_file = strdup(path);
    }
    for (int i = 0; i < node->child_count; i++) {
        ast_stamp_source_file(node->children[i], path);
    }
}

int annotation_has_marker(const char* annotation, const char* marker) {
    if (!annotation || !marker || !*marker) return 0;
    size_t n = strlen(marker);
    const char* p = annotation;
    while ((p = strstr(p, marker)) != NULL) {
        int left_ok  = (p == annotation) || (p[-1] == ';');
        int right_ok = (p[n] == '\0') || (p[n] == ';');
        if (left_ok && right_ok) return 1;
        p += n;
    }
    return 0;
}

char* annotation_add_marker(char* annotation, const char* marker) {
    if (!marker || !*marker) return annotation;
    if (!annotation) return strdup(marker);
    if (annotation_has_marker(annotation, marker)) return annotation;

    size_t cur = strlen(annotation);
    size_t n = strlen(marker);
    char* combined = (char*)malloc(cur + 1 + n + 1);
    if (!combined) return annotation;
    memcpy(combined, annotation, cur);
    combined[cur] = ';';
    memcpy(combined + cur + 1, marker, n);
    combined[cur + 1 + n] = '\0';
    free(annotation);
    return combined;
}

void print_ast(ASTNode* node, int indent) {
    if (!node) return;
    
    for (int i = 0; i < indent; i++) {
        printf("  ");
    }
    
    printf("%s", ast_node_type_to_string(node->type));
    
    if (node->value) {
        printf(": %s", node->value);
    }
    
    if (node->node_type) {
        printf(" [%s]", type_to_string(node->node_type));
    }
    
    printf(" (line %d, col %d)\n", node->line, node->column);
    
    for (int i = 0; i < node->child_count; i++) {
        print_ast(node->children[i], indent + 1);
    }
}

const char* ast_node_type_to_string(ASTNodeType type) {
    switch (type) {
        case AST_PROGRAM: return "PROGRAM";
        case AST_MODULE_DECLARATION: return "MODULE_DECLARATION";
        case AST_IMPORT_STATEMENT: return "IMPORT_STATEMENT";
        case AST_EXPORT_STATEMENT: return "EXPORT_STATEMENT";
        case AST_ACTOR_DEFINITION: return "ACTOR_DEFINITION";
        case AST_FUNCTION_DEFINITION: return "FUNCTION_DEFINITION";
        case AST_FUNCTION_CLAUSE: return "FUNCTION_CLAUSE";
        case AST_MAIN_FUNCTION: return "MAIN_FUNCTION";
        case AST_STRUCT_DEFINITION: return "STRUCT_DEFINITION";
        case AST_C_STRUCT_DEF: return "C_STRUCT_DEF";
        case AST_STRUCT_FIELD: return "STRUCT_FIELD";
        case AST_STRUCT_FIELD_UNION: return "STRUCT_FIELD_UNION";
        case AST_STRUCT_FIELD_NESTED: return "STRUCT_FIELD_NESTED";
        case AST_BLOCK: return "BLOCK";
        case AST_VARIABLE_DECLARATION: return "VARIABLE_DECLARATION";
        case AST_TUPLE_DESTRUCTURE: return "TUPLE_DESTRUCTURE";
        case AST_ASSIGNMENT: return "ASSIGNMENT";
        case AST_COMPOUND_ASSIGNMENT: return "COMPOUND_ASSIGNMENT";
        case AST_IF_STATEMENT: return "IF_STATEMENT";
        case AST_WHEN_STATEMENT: return "WHEN_STATEMENT";
        case AST_FOR_LOOP: return "FOR_LOOP";
        case AST_WHILE_LOOP: return "WHILE_LOOP";
        case AST_SWITCH_STATEMENT: return "SWITCH_STATEMENT";
        case AST_CASE_STATEMENT: return "CASE_STATEMENT";
        case AST_RETURN_STATEMENT: return "RETURN_STATEMENT";
        case AST_BREAK_STATEMENT: return "BREAK_STATEMENT";
        case AST_CONTINUE_STATEMENT: return "CONTINUE_STATEMENT";
        case AST_EXPRESSION_STATEMENT: return "EXPRESSION_STATEMENT";
        case AST_MATCH_STATEMENT: return "MATCH_STATEMENT";
        case AST_MATCH_ARM: return "MATCH_ARM";
        case AST_PATTERN_LITERAL: return "PATTERN_LITERAL";
        case AST_PATTERN_VARIABLE: return "PATTERN_VARIABLE";
        case AST_PATTERN_STRUCT: return "PATTERN_STRUCT";
        case AST_PATTERN_LIST: return "PATTERN_LIST";
        case AST_PATTERN_CONS: return "PATTERN_CONS";
        case AST_GUARD_CLAUSE: return "GUARD_CLAUSE";
        case AST_RECEIVE_STATEMENT: return "RECEIVE_STATEMENT";
        case AST_SEND_STATEMENT: return "SEND_STATEMENT";
        case AST_SPAWN_ACTOR_STATEMENT: return "SPAWN_ACTOR_STATEMENT";
        case AST_STATE_DECLARATION: return "STATE_DECLARATION";
        case AST_HIDE_DIRECTIVE: return "HIDE_DIRECTIVE";
        case AST_SEAL_DIRECTIVE: return "SEAL_DIRECTIVE";
        case AST_TRY_STATEMENT: return "TRY_STATEMENT";
        case AST_CATCH_CLAUSE: return "CATCH_CLAUSE";
        case AST_PANIC_STATEMENT: return "PANIC_STATEMENT";
        case AST_MESSAGE_DEFINITION: return "MESSAGE_DEFINITION";
        case AST_MESSAGE_FIELD: return "MESSAGE_FIELD";
        case AST_RECEIVE_ARM: return "RECEIVE_ARM";
        case AST_MESSAGE_PATTERN: return "MESSAGE_PATTERN";
        case AST_PATTERN_FIELD: return "PATTERN_FIELD";
        case AST_WILDCARD_PATTERN: return "WILDCARD_PATTERN";
        case AST_REPLY_STATEMENT: return "REPLY_STATEMENT";
        case AST_MESSAGE_CONSTRUCTOR: return "MESSAGE_CONSTRUCTOR";
        case AST_FIELD_INIT: return "FIELD_INIT";
        case AST_SEND_FIRE_FORGET: return "SEND_FIRE_FORGET";
        case AST_SEND_ASK: return "SEND_ASK";
        case AST_BINARY_EXPRESSION: return "BINARY_EXPRESSION";
        case AST_UNARY_EXPRESSION: return "UNARY_EXPRESSION";
        case AST_TUPLE_UNWRAP: return "TUPLE_UNWRAP";
        case AST_HEAP_NEW: return "HEAP_NEW";
        case AST_FUNCTION_CALL: return "FUNCTION_CALL";
        case AST_ACTOR_REF: return "ACTOR_REF";
        case AST_IDENTIFIER: return "IDENTIFIER";
        case AST_LITERAL: return "LITERAL";
        case AST_ARRAY_LITERAL: return "ARRAY_LITERAL";
        case AST_ARRAY_ACCESS: return "ARRAY_ACCESS";
        case AST_MEMBER_ACCESS: return "MEMBER_ACCESS";
        case AST_STRUCT_LITERAL: return "STRUCT_LITERAL";
        case AST_TYPE_ANNOTATION: return "TYPE_ANNOTATION";
        case AST_ACTOR_REF_TYPE: return "ACTOR_REF_TYPE";
        case AST_ARRAY_TYPE: return "ARRAY_TYPE";
        case AST_PRINT_STATEMENT: return "PRINT_STATEMENT";
        case AST_NULL_LITERAL: return "NULL_LITERAL";
        case AST_IF_EXPRESSION: return "IF_EXPRESSION";
        case AST_SIZEOF: return "SIZEOF";
        case AST_OFFSETOF: return "OFFSETOF";
        case AST_VA_START: return "VA_START";
        case AST_VA_ARG: return "VA_ARG";
        case AST_VA_END: return "VA_END";
        case AST_CONST_DECLARATION: return "CONST_DECLARATION";
        case AST_STRING_INTERP: return "STRING_INTERP";
        case AST_EXTERN_FUNCTION: return "EXTERN_FUNCTION";
        case AST_DEFER_STATEMENT: return "DEFER_STATEMENT";
        case AST_CLOSURE: return "CLOSURE";
        case AST_CLOSURE_PARAM: return "CLOSURE_PARAM";
        case AST_NAMED_ARG: return "NAMED_ARG";
        case AST_NONE_LITERAL: return "NONE_LITERAL";       // #340
        case AST_NULL_COALESCE: return "NULL_COALESCE";     // #340
        case AST_OPTIONAL_CHAIN: return "OPTIONAL_CHAIN";   // #340
        case AST_SUM_TYPE_DEF: return "SUM_TYPE_DEF";       // #914
        default: return "UNKNOWN";
    }
}

// Utility functions
ASTNode* create_literal_node(Token* token) {
    Type* type = NULL;
    
    switch (token->type) {
        case TOKEN_NUMBER:
            // Let type inference determine if it's int or float
            type = create_type(TYPE_UNKNOWN);
            break;
        case TOKEN_STRING_LITERAL:
            type = create_type(TYPE_STRING);
            break;
        case TOKEN_TRUE:
        case TOKEN_FALSE:
            type = create_type(TYPE_BOOL);
            break;
        default:
            type = create_type(TYPE_UNKNOWN);
            break;
    }
    
    ASTNode* node = create_ast_node(AST_LITERAL, token->value, token->line, token->column);
    node->node_type = type;
    return node;
}

ASTNode* create_identifier_node(Token* token) {
    ASTNode* node = create_ast_node(AST_IDENTIFIER, token->value, token->line, token->column);
    node->node_type = create_type(TYPE_UNKNOWN); // Will be resolved during type checking
    return node;
}

ASTNode* create_binary_expression(ASTNode* left, ASTNode* right, Token* operator) {
    ASTNode* node = create_ast_node(AST_BINARY_EXPRESSION, operator->value, operator->line, operator->column);
    add_child(node, left);
    add_child(node, right);
    return node;
}

ASTNode* create_unary_expression(ASTNode* operand, Token* operator) {
    ASTNode* node = create_ast_node(AST_UNARY_EXPRESSION, operator->value, operator->line, operator->column);
    add_child(node, operand);
    return node;
}
