#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "parser.h"
#include "../aether_defines.h"

/* Conditional-compilation regions (#1527): shared by the top-level and the
 * statement-level form, both of which appear later than the helpers use each
 * other. */
static int  at_when_region(Parser* parser);
static int  parse_define_cond(Parser* parser, int* ok);
static void skip_braced_region(Parser* parser);
#include "lexer.h"
#include "../aether_error.h"

#define INTERP_MAX_TOKENS 512

Parser* create_parser(Token** tokens, int token_count) {
    Parser* parser = malloc(sizeof(Parser));
    if (!parser) return NULL;
    parser->tokens = tokens;
    parser->token_count = token_count;
    parser->current_token = 0;
    parser->suppress_errors = 0;  // By default, show errors
    parser->parsing_builder = 0;
    parser->in_condition = 0;
    parser->when_top_level = 0;
    return parser;
}

void free_parser(Parser* parser) {
    if (parser) {
        free(parser);
    }
}

Token* peek_token(Parser* parser) {
    if (parser->current_token >= parser->token_count) {
        return NULL;
    }
    return parser->tokens[parser->current_token];
}

Token* peek_ahead(Parser* parser, int offset) {
    int pos = parser->current_token + offset;
    if (pos < 0 || pos >= parser->token_count) {
        return NULL;
    }
    return parser->tokens[pos];
}

// Disambiguate a `|` (or `||`) right after a function call: is it the start
// of a trailing closure `func(args) |params| { ... }` / `func(args) || { ... }`,
// or a bitwise/logical operator `func(args) | EXPR` / `func(args) || EXPR`?
//
// A trailing closure's parameter list is always immediately followed by a
// `{` block or a `->` arrow. So: from the opening `|`, scan a plausible
// parameter list (identifiers / commas / `: type` annotations) to a closing
// `|`, then require `{` or `->`. For `||` (empty params) just require the
// next token to be `{` or `->`. Anything else is an operator and must be
// left for the expression parser (fixes `strlen(s) | 0x80` being misread as
// a closure — see the AES/ChaCha/Ed25519 crypto ports). `offset` is the
// position of the `|`/`||` token relative to parser->current_token.
static int looks_like_trailing_closure(Parser* parser, int offset) {
    Token* t = peek_ahead(parser, offset);
    if (!t) return 0;
    if (t->type == TOKEN_OR) {
        // `||` — empty param list; closure only if followed by `{` or `->`.
        Token* after = peek_ahead(parser, offset + 1);
        return after && (after->type == TOKEN_LEFT_BRACE || after->type == TOKEN_ARROW);
    }
    if (t->type != TOKEN_PIPE) return 0;

    // Scan forward to the matching closing `|`. The decisive test is what
    // follows that closing pipe: a real closure has `|params| {` or
    // `|params| ->`. A bitwise expression `f() | EXPR` has no second `|`
    // before the statement ends, or — for `f() | a | b` — the token after
    // the second `|` is an operand, not `{`/`->`, so it's rejected here.
    //
    // The interior scan is permissive (closure params include type-keyword
    // tokens like `int`/`string`, not just identifiers, so we can't whitelist
    // narrowly) but bails on tokens that cannot appear inside a parameter
    // list and that would instead terminate or nest an expression: another
    // `|`/`||` (handled as the closing pipe / a non-closure), a `{`, `->`,
    // `;`, `)`, `}`, `(`, or EOF.
    int i = offset + 1;
    while (1) {
        Token* tk = peek_ahead(parser, i);
        if (!tk) return 0;
        if (tk->type == TOKEN_PIPE) {
            // Closing pipe — closure iff `{` or `->` follows.
            Token* after = peek_ahead(parser, i + 1);
            return after && (after->type == TOKEN_LEFT_BRACE || after->type == TOKEN_ARROW);
        }
        if (tk->type == TOKEN_OR ||             // `||` mid-list → not a param list
            tk->type == TOKEN_LEFT_BRACE ||     // `{` before a closing `|`
            tk->type == TOKEN_ARROW ||
            tk->type == TOKEN_SEMICOLON ||
            tk->type == TOKEN_LEFT_PAREN ||
            tk->type == TOKEN_RIGHT_PAREN ||
            tk->type == TOKEN_RIGHT_BRACE ||
            tk->type == TOKEN_EOF) {
            return 0;
        }
        i++;
        if (i - offset > 64) return 0;          // runaway guard
    }
}

Token* advance_token(Parser* parser) {
    if (parser->current_token >= parser->token_count) {
        return NULL;
    }
    return parser->tokens[parser->current_token++];
}

// True when `token`'s source text looks like a reserved keyword (all
// alphanumeric/underscore, starts with a letter). Used to generate a
// friendlier error than "Expected IDENTIFIER, got MESSAGE_KEYWORD"
// when a user picks a name that collides with the grammar. Skips
// TOKEN_IDENTIFIER itself and anything whose value is punctuation.
static int token_is_reserved_keyword(Token* token) {
    if (!token || !token->value || token->type == TOKEN_IDENTIFIER) return 0;
    const char* s = token->value;
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_')) return 0;
    for (const char* p = s + 1; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) return 0;
    }
    return 1;
}

// #880: tokens accepted as an ordinary value identifier in name position —
// a parameter / local binding, an assignment target, a tuple-destructure
// slot. `ptr`/`byte` are keywords only in type position, `func` only as a
// declaration head, `state`/`after` only as statement heads; none of those
// roles reaches a value-identifier position, so accept the spelling (carried
// in `->value`) as the name. Two members of the issue's list are deliberately
// left out: `match` (heads a match expression — genuinely ambiguous in value
// position) and `union` (a C keyword, so a value named `union` would emit
// invalid C — supporting it needs value-identifier mangling in codegen, a
// separate change).
static int token_is_value_ident(const Token* token) {
    if (!token) return 0;
    switch (token->type) {
        case TOKEN_IDENTIFIER:
        case TOKEN_STATE:
        case TOKEN_PTR:
        case TOKEN_BYTE:
        case TOKEN_FUNC:
        case TOKEN_AFTER:
            return 1;
        default:
            return 0;
    }
}

// C ABI scalar aliases — recognised exact C type spellings. When a
// type-position identifier matches one, parse_type builds a Type with
// the given `kind` (which governs all typechecking and arithmetic)
// plus c_alias = the name (the spelling codegen emits verbatim, so an
// Aether `extern` prototype matches the C system header).
//
// `out_kind` receives the underlying TypeKind. Returns 1 on a match.
// The set is the fixed C99/POSIX scalar family from
// redis-porting-language-gaps.md "P0: C ABI Scalar Aliases".
static int c_abi_alias_kind(const char* name, TypeKind* out_kind) {
    struct { const char* alias; TypeKind kind; } table[] = {
        /* fixed-width */
        { "int8_t",   TYPE_INT },
        { "uint8_t",  TYPE_UINT8 },
        { "int16_t",  TYPE_INT },
        { "uint16_t", TYPE_UINT16 },
        { "int32_t",  TYPE_INT },
        { "uint32_t", TYPE_UINT32 },
        { "int64_t",  TYPE_INT64 },
        { "uint64_t", TYPE_UINT64 },
        /* pointer-width */
        { "intptr_t",  TYPE_INT64 },
        { "uintptr_t", TYPE_UINT64 },
        /* size / offset */
        { "size_t",  TYPE_UINT64 },
        { "ssize_t", TYPE_INT64 },
        { "off_t",   TYPE_INT64 },
        /* platform scalars */
        { "time_t", TYPE_INT64 },
        { "pid_t",  TYPE_INT },
        { "mode_t", TYPE_INT },
    };
    int n = (int)(sizeof(table) / sizeof(table[0]));
    for (int i = 0; i < n; i++) {
        if (strcmp(name, table[i].alias) == 0) {
            *out_kind = table[i].kind;
            return 1;
        }
    }
    return 0;
}

Token* expect_token(Parser* parser, AeTokenType expected) {
    Token* token = peek_token(parser);
    if (!token || token->type != expected) {
        char error_msg[256];
        if (expected == TOKEN_IDENTIFIER && token_is_reserved_keyword(token)) {
            // User picked a reserved keyword as an identifier name —
            // point at the keyword and suggest a rename so they don't
            // have to guess which grammar slot was wanted.
            snprintf(error_msg, sizeof(error_msg),
                "'%s' is a reserved keyword and cannot be used as an identifier; "
                "rename it (e.g. '%s_' or 'msg'), or escape it as `%s` to use the "
                "name verbatim",
                token->value, token->value, token->value);
            // Emit with a reserved-keyword-specific hint so the `help:`
            // line matches the error (previous default hint was the
            // generic "check for missing parentheses, braces, or
            // keywords", which misled users into hunting for a parse
            // problem that wasn't there).
            char hint[160];
            snprintf(hint, sizeof(hint),
                "rename to '%s_', or write `%s` to keep the name",
                token->value, token->value);
            if (!parser->suppress_errors) {
                aether_error_full(error_msg, token->line, token->column,
                                  hint, NULL, AETHER_ERR_SYNTAX);
            }
            return NULL;
        }
        snprintf(error_msg, sizeof(error_msg),
            "Expected %s, got %s",
            token_type_to_string(expected),
            token ? token_type_to_string(token->type) : "EOF");
        parser_error(parser, error_msg);
        return NULL;
    }
    return advance_token(parser);
}

int is_at_end(Parser* parser) {
    if (parser->current_token >= parser->token_count) return 1;
    Token* t = peek_token(parser);
    return !t || t->type == TOKEN_EOF;
}

int match_token(Parser* parser, AeTokenType type) {
    if (is_at_end(parser)) return 0;
    if (peek_token(parser)->type == type) {
        advance_token(parser);
        return 1;
    }
    return 0;
}

void parser_error(Parser* parser, const char* message) {
    if (parser->suppress_errors) {
        return;
    }
    
    Token* token = peek_token(parser);
    if (token) {
        aether_error_with_code(message, token->line, token->column,
                               AETHER_ERR_SYNTAX);
    } else {
        aether_error_simple(message, 0, 0);
    }
}

// Helper to print warnings and errors (respects suppress_errors flag)
static void parser_message(Parser* parser, const char* message) {
    (void)parser;
    (void)message;
}

static Type* parse_type_unsuffixed(Parser* parser);

// #340: a type may carry a postfix `?` making it optional (`int?`, `string?`,
// `Vec2?`, `*Node?`). Parse the base type, then wrap once per `?`. In type
// position `?` is unambiguous (the actor-ask `?` only appears in expressions).
Type* parse_type(Parser* parser) {
    Type* t = parse_type_unsuffixed(parser);
    if (!t) return NULL;
    /* Nested optionals `T??` are rejected. A double optional parses (as
     * `ae_opt_ae_opt_<T>`), but the rest of the compiler only reasons ONE
     * presence layer deep — `none`/wrap coercion, `== none`, and narrowing
     * all assume a single layer — so it miscompiles silently. Refuse it at
     * the source, matching C3, whose `type_add_optional` likewise won't nest.
     *
     * The lexer greedily fuses `??` into one TOKEN_QUESTION_QUESTION, so
     * `int??` arrives as `int` followed by `??` — the type-suffix loop below
     * never sees a first `?`. So catch the `??`-after-a-type shape here BEFORE
     * consuming a single `?`, and also the (rarer) `int? ?` shape after we do.
     * Consume the stray token(s) so error recovery doesn't stall. */
    int nested_opt = 0;
    if (peek_token(parser) && peek_token(parser)->type == TOKEN_QUESTION_QUESTION) {
        /* `int??` — the whole nested optional, fused. */
        advance_token(parser);
        t = create_optional_type(t);   /* the layer the user did mean */
        nested_opt = 1;
    } else if (peek_token(parser) && peek_token(parser)->type == TOKEN_QUESTION) {
        advance_token(parser);
        t = create_optional_type(t);
        if (peek_token(parser) &&
            (peek_token(parser)->type == TOKEN_QUESTION ||
             peek_token(parser)->type == TOKEN_QUESTION_QUESTION)) {
            /* `int? ?` or `int? ??`. */
            while (peek_token(parser) &&
                   (peek_token(parser)->type == TOKEN_QUESTION ||
                    peek_token(parser)->type == TOKEN_QUESTION_QUESTION))
                advance_token(parser);
            nested_opt = 1;
        }
    }
    if (nested_opt) {
        parser_error(parser,
            "nested optional `T??` is not supported, an optional is a single "
            "presence layer; remove the extra `?`");
    }
    // #913: a postfix `!` makes the type a fallible result (`string!`,
    // `int!`), symmetric with the `?` optional suffix above. In type position
    // `!` is unambiguous (the unwrap/propagate `!` only appears in
    // expressions). `T!` lowers to the existing `(T, string)` (value, err)
    // tuple, so it interops with the stdlib convention.
    if (peek_token(parser) && peek_token(parser)->type == TOKEN_EXCLAIM) {
        advance_token(parser);
        /* A tuple payload — `(A, B)!` — would lower to the NESTED tuple
         * `((A, B), string)`, whose C layout (`{ _tuple_A_B _0; const
         * char* _1; }`) differs from the FLAT `(A, B, string)` tuple the
         * stdlib multi-value fallibles use (`{ A _0; B _1; const char*
         * _2; }`). The two are not ABI-interchangeable, and a 3-way
         * `a, b, e = f()` destructure sees only 2 values (payload +
         * error) and fails with a confusing count mismatch downstream.
         * `T!` is the single-payload fallible spine; a multi-value
         * fallible stays a raw `(A, B, string)` tuple. Reject here with
         * guidance rather than emit the mis-shaped type. */
        if (t && t->kind == TYPE_TUPLE) {
            parser_error(parser,
                "a tuple payload in `T!` is not supported, `(A, B)!` would "
                "nest as `((A, B), string)`, which is not ABI-compatible with "
                "a flat `(A, B, string)` tuple and breaks `a, b, e = f()` "
                "destructuring. Use a raw `(A, B, string)` tuple for a "
                "multi-value fallible return; `T!` is for a single payload.");
        }
        t = create_result_type(t);
    }
    return t;
}

static Type* parse_type_unsuffixed(Parser* parser) {
    Token* token = peek_token(parser);
    if (!token) return NULL;

    Type* type = NULL;

    // #1044 enum-indexed array `[E]T`: a leading `[` introduces the index enum,
    // then the element type. The array has one slot per member of enum E and is
    // indexed by an E value (`labels[Dir.North]`). Ordinary arrays keep the
    // postfix `T[N]` / `T[]` form handled after the switch below. The size is
    // filled in from E by resolve_enum_types once the enum definition is known.
    if (token->type == TOKEN_LEFT_BRACKET) {
        advance_token(parser);  // consume '['
        Type* index_enum = parse_type(parser);
        if (!index_enum) {
            parser_error(parser,
                "expected an enum name inside `[...]` for an enum-indexed array");
            return NULL;
        }
        if (!expect_token(parser, TOKEN_RIGHT_BRACKET)) { free_type(index_enum); return NULL; }
        Type* elem = parse_type(parser);
        if (!elem) {
            parser_error(parser,
                "expected an element type after `[E]` for an enum-indexed array, e.g. [Dir]string");
            free_type(index_enum);
            return NULL;
        }
        Type* arr = create_array_type(elem, -1);  // size resolved at typecheck
        if (index_enum->struct_name)
            arr->index_enum_name = strdup(index_enum->struct_name);
        free_type(index_enum);
        return arr;
    }

    /* `const <type>` — a C-qualified type.  Parsed by recursing on the
     * unqualified type, then stamping a const-prefixed C spelling onto
     * `c_alias` so codegen emits the qualifier verbatim.  The `kind`
     * is untouched, so const-ness affects only the emitted C — enough
     * to match a header prototype like `memcmp(const void *, const
     * void *, size_t)` and silence conflicting-declaration warnings.
     * `const ptr` -> `const void*`; `const *T` -> `const T*`;
     * `const string` -> `const char*`. See
     * redis-porting-language-gaps.md "P0: Typed And Qualified C
     * Pointers". */
    if (token->type == TOKEN_CONST) {
        advance_token(parser);  // consume `const`
        Type* inner = parse_type(parser);
        if (!inner) {
            parser_error(parser, "Expected a type after `const`");
            return NULL;
        }
        /* Compute the unqualified C spelling, then prefix `const `. */
        const char* base = NULL;
        char ptr_buf[128];
        if (inner->c_alias) {
            base = inner->c_alias;
        } else if (inner->kind == TYPE_PTR && inner->element_type &&
                   inner->element_type->kind == TYPE_STRUCT &&
                   inner->element_type->struct_name) {
            snprintf(ptr_buf, sizeof(ptr_buf), "%s*",
                     inner->element_type->struct_name);
            base = ptr_buf;
        } else if (inner->kind == TYPE_PTR) {
            base = "void*";
        } else if (inner->kind == TYPE_STRING) {
            base = "char*";
        } else if (inner->kind == TYPE_BYTE) {
            base = "unsigned char";
        } else if (inner->kind == TYPE_INT) {
            base = "int";
        } else {
            base = NULL;  /* leave codegen's default for exotic kinds */
        }
        if (base) {
            char* qualified = malloc(strlen(base) + 7 /* "const " + NUL */);
            sprintf(qualified, "const %s", base);
            if (inner->c_alias) free(inner->c_alias);
            inner->c_alias = qualified;
        }
        return inner;
    }

    // Pointer-to-struct type: `*StructName`. Lowers to `StructName*` in
    // C. Used as the return type of `expr as *StructName` (the
    // pointer-overlay cast) and accepted in any other type position
    // (variable annotations, function params, return types, struct
    // fields, extern decls). The pointer-ness is part of the spelled
    // type so callers can declare e.g. `process(node: *list_head)`.
    // Plain `ptr` (void*) remains the right type for raw byte addresses;
    // `*T` carries the struct identity through the type system so member
    // access dispatches via `->field` automatically. Lifetime is the
    // operand's — `as` does not allocate or refcount.
    if (token->type == TOKEN_MULTIPLY) {
        advance_token(parser);
        Token* name_tok = expect_token(parser, TOKEN_IDENTIFIER);
        if (!name_tok) return NULL;
        Type* struct_type = create_type(TYPE_STRUCT);
        struct_type->struct_name = strdup(name_tok->value);
        Type* ptr_type = create_type(TYPE_PTR);
        ptr_type->element_type = struct_type;
        return ptr_type;
    }

    // Tuple type: `(T1, T2, ...)` — used in extern return positions for
    // C functions that return a struct by value with the matching shape.
    // See `compiler/codegen/codegen_func.c` for the `_tuple_T1_T2`
    // typedef the codegen synthesises. Issue #271.
    if (token->type == TOKEN_LEFT_PAREN) {
        advance_token(parser);
        Type* tup = create_type(TYPE_TUPLE);
        tup->tuple_count = 0;
        tup->tuple_types = NULL;
        do {
            Type* elem = parse_type(parser);
            if (!elem) {
                parser_error(parser, "Expected type inside tuple");
                free_type(tup);
                return NULL;
            }
            tup->tuple_count++;
            tup->tuple_types = aether_xrealloc(tup->tuple_types,
                                       (size_t)tup->tuple_count * sizeof(Type*));
            tup->tuple_types[tup->tuple_count - 1] = elem;

            /* Per-position heap-ownership annotation (issue #420):
             *
             *   (string @heap, int, string @borrow)
             *
             * Marks the position-0 result as a fresh heap allocation
             * the destructured LHS now owns; the position-2 result
             * as a borrow / non-heap value. Default for unannotated
             * positions is @borrow — preserves the pre-#420 silent
             * behaviour for every existing tuple-returning extern
             * and user function. The flag is later consumed by the
             * AST_TUPLE_DESTRUCTURE codegen path to decide whether
             * to emit `_heap_<lhs> = 1;` after the destructure. */
            if (peek_token(parser) && peek_token(parser)->type == TOKEN_AT) {
                advance_token(parser);  /* consume '@' */
                Token* tag = peek_token(parser);
                int heap = 0;
                if (tag && tag->type == TOKEN_IDENTIFIER && tag->value) {
                    if (strcmp(tag->value, "heap") == 0) {
                        heap = 1; advance_token(parser);
                    } else if (strcmp(tag->value, "borrow") == 0) {
                        heap = 0; advance_token(parser);
                    } else {
                        parser_error(parser,
                            "unknown tuple-position attribute "
                            "(expected @heap or @borrow)");
                    }
                } else {
                    parser_error(parser,
                        "expected @heap or @borrow after '@' on tuple element");
                }
                /* Lazy-allocate the parallel flags array on first
                 * annotation. Trailing positions default to 0 (borrow). */
                if (!tup->tuple_heap_flags) {
                    tup->tuple_heap_flags =
                        (int*)calloc((size_t)tup->tuple_count, sizeof(int));
                } else {
                    /* Grow if a later position is annotated after we'd
                     * already lazy-allocated for an earlier one. */
                    tup->tuple_heap_flags =
                        (int*)realloc(tup->tuple_heap_flags,
                                      (size_t)tup->tuple_count * sizeof(int));
                    /* Newly-grown slot defaults to 0 if not just set. */
                }
                tup->tuple_heap_flags[tup->tuple_count - 1] = heap;
            } else if (tup->tuple_heap_flags) {
                /* Earlier positions were annotated; this one isn't.
                 * Grow the flags array and default to 0 (borrow). */
                tup->tuple_heap_flags =
                    (int*)realloc(tup->tuple_heap_flags,
                                  (size_t)tup->tuple_count * sizeof(int));
                tup->tuple_heap_flags[tup->tuple_count - 1] = 0;
            }
        } while (match_token(parser, TOKEN_COMMA));
        if (!expect_token(parser, TOKEN_RIGHT_PAREN)) {
            free_type(tup);
            return NULL;
        }
        if (tup->tuple_count < 2) {
            parser_error(parser, "tuple type requires at least two element types");
            free_type(tup);
            return NULL;
        }
        return tup;
    }

    switch (token->type) {
        case TOKEN_INT:
            advance_token(parser);
            type = create_type(TYPE_INT);
            break;
        case TOKEN_INT64:
            advance_token(parser);
            type = create_type(TYPE_INT64);
            /* `long long` — the second `long` (also TOKEN_INT64) is
             * consumed and stamps a `long long` C alias so the emitted
             * extern prototype matches a libc/POSIX header that spells
             * the parameter as `long long` (e.g. mstime_t typedef chains,
             * the MT19937 / SHA-x reference impls). The Aether-side type
             * is still TYPE_INT64 (int64_t), so all arithmetic and
             * typechecking behave identically — only the verbatim C
             * spelling differs. Required by aedis core-floor (the
             * "Minor, real, cheap" item). */
            if (peek_token(parser) && peek_token(parser)->type == TOKEN_INT64) {
                advance_token(parser);
                type->c_alias = strdup("long long");
            }
            break;
        case TOKEN_UINT64:
            advance_token(parser);
            type = create_type(TYPE_UINT64);
            break;
        case TOKEN_DURATION:
            advance_token(parser);
            type = create_type(TYPE_DURATION);
            break;
        case TOKEN_FLOAT:
            advance_token(parser);
            type = create_type(TYPE_FLOAT);
            break;
        case TOKEN_BOOL:
            advance_token(parser);
            type = create_type(TYPE_BOOL);
            break;
        case TOKEN_BYTE:
            advance_token(parser);
            type = create_type(TYPE_BYTE);
            break;
        case TOKEN_STRING:
            advance_token(parser);
            type = create_type(TYPE_STRING);
            break;
        case TOKEN_MESSAGE:
            advance_token(parser);
            type = create_type(TYPE_MESSAGE);
            break;
        case TOKEN_PTR:
            advance_token(parser);
            type = create_type(TYPE_PTR);
            break;
        case TOKEN_IDENTIFIER: {
            advance_token(parser);
            // "fn" is the closure/function type.  Two forms:
            //   bare `fn`              — closure, no concrete signature
            //   `fn(T1, T2, ...) -> R` — typed C function pointer:
            //     emits as `void*` storage with a typed C cast injected
            //     at the call site.  Used to declare local variables /
            //     parameters holding a raw `ptr` returned from a C
            //     extern (vtable lookup, signal handler table, qsort
            //     callback handoff) so Aether can `fp(a, b)` directly
            //     instead of routing through bespoke per-signature
            //     `mem.call_fn3_int` shims.
            if (strcmp(token->value, "fn") == 0) {
                type = create_type(TYPE_FUNCTION);
                // Optional signature: `(T1, T2, ...) -> R`.  The
                // signature is OPTIONAL so bare `fn` keeps working
                // for closures whose param types come from inference.
                // When the signature IS provided we treat this as a
                // raw C function pointer (is_fnptr = 1) — storage
                // `void*`, call site emits the matching typed cast.
                // Bare `fn` keeps the closure semantics.
                if (peek_token(parser) && peek_token(parser)->type == TOKEN_LEFT_PAREN) {
                    type->is_fnptr = 1;
                    advance_token(parser);  // consume '('
                    type->param_count = 0;
                    type->param_types = NULL;
                    if (!(peek_token(parser) && peek_token(parser)->type == TOKEN_RIGHT_PAREN)) {
                        do {
                            Type* p = parse_type(parser);
                            if (!p) {
                                parser_error(parser, "Expected type in fn() signature");
                                free_type(type);
                                return NULL;
                            }
                            type->param_count++;
                            type->param_types = aether_xrealloc(type->param_types,
                                (size_t)type->param_count * sizeof(Type*));
                            type->param_types[type->param_count - 1] = p;
                        } while (match_token(parser, TOKEN_COMMA));
                    }
                    if (!expect_token(parser, TOKEN_RIGHT_PAREN)) {
                        free_type(type);
                        return NULL;
                    }
                    // Return type after `->`.  Omitting `-> R` means
                    // void return (consistent with extern/function
                    // syntax elsewhere).
                    if (peek_token(parser) && peek_token(parser)->type == TOKEN_ARROW) {
                        advance_token(parser);  // consume `->`
                        type->return_type = parse_type(parser);
                        if (!type->return_type) {
                            parser_error(parser, "Expected return type after `->` in fn() signature");
                            free_type(type);
                            return NULL;
                        }
                    } else {
                        type->return_type = create_type(TYPE_VOID);
                    }
                }
            } else {
                /* Named types: C string aliases, C ABI scalar
                 * aliases, then a plain struct-name fall-through. */
                TypeKind alias_kind;
                if (strcmp(token->value, "Isolated") == 0 &&
                    peek_token(parser) &&
                    peek_token(parser)->type == TOKEN_LEFT_BRACKET) {
                    /* #479: Isolated[T], a compile-time-only, move-only
                     * (linear) wrapper for actor message payloads. The
                     * identifier was consumed at case entry, so peek is `[`.
                     * TYPE_ISOLATED carries the wrapped T in element_type and
                     * lowers to T's C type with zero runtime cost. */
                    advance_token(parser);  // consume '['
                    Type* inner = parse_type(parser);
                    if (!inner) {
                        parser_error(parser,
                            "Isolated requires a type parameter, e.g. Isolated[T]");
                        return NULL;
                    }
                    if (!expect_token(parser, TOKEN_RIGHT_BRACKET)) {
                        free_type(inner);
                        return NULL;
                    }
                    type = create_type(TYPE_ISOLATED);
                    type->element_type = inner;
                } else if (strcmp(token->value, "bit_set") == 0 &&
                           peek_token(parser) &&
                           peek_token(parser)->type == TOKEN_LEFT_BRACKET) {
                    /* #1046: bit_set[E], a set of members of enum E backed by an
                     * unsigned 64-bit word. The identifier was consumed at case
                     * entry, so peek is `[`. The element type resolves to a
                     * TYPE_ENUM (via resolve_enum_types); it lowers to
                     * `unsigned long long` with zero runtime cost. */
                    advance_token(parser);  // consume '['
                    Type* elem = parse_type(parser);
                    if (!elem) {
                        parser_error(parser,
                            "bit_set requires an enum type parameter, e.g. bit_set[Color]");
                        return NULL;
                    }
                    if (!expect_token(parser, TOKEN_RIGHT_BRACKET)) {
                        free_type(elem);
                        return NULL;
                    }
                    type = create_bitset_type(elem);
                } else if (strcmp(token->value, "cstring") == 0) {
                    /* `cstring` — a string whose emitted C type is the
                     * mutable `char*` (Aether's plain `string` emits
                     * `const char*`). For a C extern whose header has
                     * a non-const `char *` parameter. Behaves as a
                     * `string` for all typechecking. */
                    type = create_type(TYPE_STRING);
                    type->c_alias = strdup("char*");
                } else if (strcmp(token->value, "cstring_const") == 0) {
                    /* `cstring_const` — `const char*` spelled
                     * explicitly. Same emitted type as plain `string`;
                     * provided so an extern signature reads as the C
                     * header does. */
                    type = create_type(TYPE_STRING);
                    type->c_alias = strdup("const char*");
                } else if (strcmp(token->value, "va_list") == 0) {
                    /* #1244: `va_list`, the C type of a forwarded variadic
                     * tail, for the `v*` half of every printf-style pair
                     * (vprintf, vsnprintf, the shape Redis's serverLog and
                     * sdscatprintf need). Typechecks as the opaque `ptr` that
                     * va_start() yields; the C spelling is what matters,
                     * because the call site has to hand the callee the va_list
                     * ITSELF, not the cookie pointing at it. Declaring the
                     * param `ptr` instead compiles and then prints garbage. */
                    type = create_type(TYPE_PTR);
                    type->c_alias = strdup("va_list");
                } else if (strcmp(token->value, "longdouble") == 0) {
                    /* #749: `long double` — the widest C floating type.
                     * An identifier-spelled primitive (no keyword token);
                     * get_c_type emits "long double". Values arrive via
                     * externs (strtold), arithmetic promotes to it. */
                    type = create_type(TYPE_LONGDOUBLE);
                } else if (strcmp(token->value, "f32") == 0) {
                    /* #1033: `f32` — C `float`, 32-bit. Exists so extern
                     * tuple params/returns can match C structs with float
                     * fields (raylib Vector2/Rectangle/Color-adjacent
                     * shapes). Aether's own float stays double; codegen
                     * casts at the FFI boundary. */
                    type = create_type(TYPE_FLOAT32);
                } else if (strcmp(token->value, "uint8") == 0 ||
                           strcmp(token->value, "uint16") == 0 ||
                           strcmp(token->value, "uint32") == 0) {
                    /* #745: short unsigned width names, the siblings of
                     * the `uint64` keyword. Each has a distinct TypeKind
                     * whose C spelling (uint8_t/uint16_t/uint32_t) is
                     * produced by get_c_type / const_array_elem_c_type —
                     * so we set NO c_alias (a bare "uint16" is not a C
                     * type). Needed for module-level const lookup tables
                     * with a pinned element width, e.g.
                     * `const CRC16TAB: uint16[256] = [...]`. */
                    TypeKind k = (token->value[4] == '8') ? TYPE_UINT8
                               : (token->value[5] == '6') ? TYPE_UINT16
                               : TYPE_UINT32;
                    type = create_type(k);
                } else if (c_abi_alias_kind(token->value, &alias_kind)) {
                    /* C ABI scalar aliases — exact C type spellings the
                     * Redis/mquickjs ports need so an Aether `extern`
                     * prototype matches the system header byte-for-byte.
                     * The alias is a normal Type whose `kind` drives all
                     * typechecking/arithmetic; `c_alias` carries the C
                     * spelling codegen emits verbatim. See
                     * redis-porting-language-gaps.md "P0: C ABI Scalar
                     * Aliases". */
                    type = create_type(alias_kind);
                    type->c_alias = strdup(token->value);
                } else if (peek_token(parser) &&
                           peek_token(parser)->type == TOKEN_DOT) {
                    /* #946: qualified type name `mod.Type` in a type
                     * position (param / return / var annotation / field).
                     * The qualified call surface already accepts `mod.fn()`
                     * (#878); this is the type-position analogue. An
                     * exported struct type is usable cross-module by its
                     * BARE name (the merge brings it into the consumer's
                     * struct namespace unprefixed), so the qualifier is a
                     * disambiguator only — resolve `mod.Type` to the bare
                     * `Type`. The leading identifier (`mod`) was already
                     * consumed; consume the `.` and the type name. Allow a
                     * multi-segment path (`a.b.Type`) by looping. */
                    const char* type_name = token->value;
                    while (peek_token(parser) &&
                           peek_token(parser)->type == TOKEN_DOT) {
                        advance_token(parser);  // consume '.'
                        Token* seg = peek_token(parser);
                        if (!seg || (seg->type != TOKEN_IDENTIFIER &&
                                     !token_is_reserved_keyword(seg))) {
                            parser_error(parser,
                                "Expected a type name after '.' in a "
                                "qualified type");
                            return NULL;
                        }
                        advance_token(parser);  // consume the segment
                        type_name = seg->value; // last segment is the type
                    }
                    type = create_type(TYPE_STRUCT);
                    type->struct_name = strdup(type_name);
                } else {
                    // Could be a struct type
                    type = create_type(TYPE_STRUCT);
                    type->struct_name = strdup(token->value);
                }
            }
            break;
        }
        case TOKEN_ACTOR_REF:
            advance_token(parser);
            // Optional type parameter: ActorRef[Type] or bare actor_ref
            if (peek_token(parser) && peek_token(parser)->type == TOKEN_LEFT_BRACKET) {
                advance_token(parser); // consume '['
                Type* actor_type = parse_type(parser);
                if (!expect_token(parser, TOKEN_RIGHT_BRACKET)) return NULL;
                type = create_actor_ref_type(actor_type);
            } else {
                // Bare actor_ref — no type parameter
                type = create_type(TYPE_ACTOR_REF);
            }
            break;
        default:
            return NULL;
    }
    
    // Check for array type
    if (match_token(parser, TOKEN_LEFT_BRACKET)) {
        if (match_token(parser, TOKEN_RIGHT_BRACKET)) {
            // Dynamic array
            type = create_array_type(type, -1);
        } else {
            // Fixed-size array
            Token* size_token = expect_token(parser, TOKEN_NUMBER);
            if (size_token) {
                int size = atoi(size_token->value);
                if (!expect_token(parser, TOKEN_RIGHT_BRACKET)) return NULL;
                type = create_array_type(type, size);
            }
        }
    }
    
    return type;
}

// Parse an interpolated string literal (TOKEN_INTERP_STRING).
// The raw value has literal text intermixed with ${expr} segments.
// Returns AST_STRING_INTERP with alternating children:
//   - AST_LITERAL (TYPE_STRING) for literal text segments
//   - expression nodes for ${...} parts
static ASTNode* parse_interp_string_expr(const char* raw) {
    ASTNode* interp = create_ast_node(AST_STRING_INTERP, NULL, 0, 0);

    const char* p = raw;
    int lit_cap = 256;
    char* lit_buf = malloc(lit_cap);
    int lit_len = 0;

    // Helper lambda (C-style): flush current literal buffer as a child node
    #define FLUSH_LIT() do { \
        lit_buf[lit_len] = '\0'; \
        ASTNode* _lit = create_ast_node(AST_LITERAL, lit_buf, 0, 0); \
        _lit->node_type = create_type(TYPE_STRING); \
        add_child(interp, _lit); \
        lit_len = 0; \
    } while(0)

    while (*p) {
        if (*p == '$' && p[1] == '{') {
            FLUSH_LIT();
            p += 2; // skip ${

            // Collect expression source until matching }. Skip over nested
            // string literals so a '{' or '}' inside one (e.g. ${id("a}b")})
            // doesn't miscount the interpolation's own brace depth.
            int depth = 1;
            const char* expr_start = p;
            while (*p && depth > 0) {
                if (*p == '"') {
                    p++;
                    while (*p && *p != '"') {
                        if (*p == '\\' && p[1]) p++;
                        p++;
                    }
                    if (*p == '"') p++;
                    continue;
                }
                if (*p == '{') depth++;
                else if (*p == '}') { if (--depth == 0) break; }
                p++;
            }
            size_t expr_len = (size_t)(p - expr_start);
            char* expr_src = malloc(expr_len + 1);
            memcpy(expr_src, expr_start, expr_len);
            expr_src[expr_len] = '\0';
            if (*p == '}') p++; // skip }

            // Re-lex the expression (save/restore global lexer state)
            LexerState saved;
            lexer_save(&saved);
            lexer_init(expr_src);

            Token* sub_tokens[INTERP_MAX_TOKENS];
            int sub_count = 0;
            while (sub_count < INTERP_MAX_TOKENS - 1) {
                Token* t = next_token();
                sub_tokens[sub_count++] = t;
                if (t->type == TOKEN_EOF || t->type == TOKEN_ERROR) break;
            }
            lexer_restore(&saved);
            free(expr_src);

            // Exclude trailing EOF from token count for sub-parser
            int n = (sub_count > 0 && sub_tokens[sub_count - 1]->type == TOKEN_EOF)
                    ? sub_count - 1 : sub_count;
            Parser* sub = create_parser(sub_tokens, n);
            ASTNode* expr_node = parse_expression(sub);
            free(sub); // tokens owned by AST nodes; do not free them here

            if (expr_node) add_child(interp, expr_node);
        } else if (*p == '\\' && p[1]) {
            // Escape sequence in literal segment
            if (lit_len >= lit_cap - 2) {
                lit_cap *= 2;
                char* nb = realloc(lit_buf, lit_cap);
                if (!nb) { free(lit_buf); return interp; }
                lit_buf = nb;
            }
            char code = p[1];
            if (code == 'x') {
                // \xNN hex escape (1-2 hex digits)
                p += 2; // skip \x
                int val = 0, digits = 0;
                while (digits < 2 && *p && isxdigit((unsigned char)*p)) {
                    char h = *p++;
                    val = val * 16 + (h >= 'a' ? h - 'a' + 10 :
                                      h >= 'A' ? h - 'A' + 10 : h - '0');
                    digits++;
                }
                lit_buf[lit_len++] = digits > 0 ? (char)val : 'x';
            } else if (code >= '0' && code <= '7') {
                // \NNN octal escape (1-3 digits)
                p++; // skip backslash
                int val = (*p++) - '0', digits = 1;
                while (digits < 3 && *p >= '0' && *p <= '7') {
                    val = val * 8 + (*p++ - '0');
                    digits++;
                }
                lit_buf[lit_len++] = (char)(val & 0xFF);
            } else {
                switch (code) {
                    case 'n':  lit_buf[lit_len++] = '\n'; break;
                    case 't':  lit_buf[lit_len++] = '\t'; break;
                    case 'r':  lit_buf[lit_len++] = '\r'; break;
                    case '\\': lit_buf[lit_len++] = '\\'; break;
                    case '"':  lit_buf[lit_len++] = '"';  break;
                    default:   lit_buf[lit_len++] = code; break;
                }
                p += 2;
            }
        } else {
            if (lit_len >= lit_cap - 2) {
                lit_cap *= 2;
                char* nb = realloc(lit_buf, lit_cap);
                if (!nb) { free(lit_buf); return interp; }
                lit_buf = nb;
            }
            lit_buf[lit_len++] = *p++;
        }
    }
    FLUSH_LIT(); // trailing literal (may be empty string)
    #undef FLUSH_LIT

    free(lit_buf);
    return interp;
}

// Parse closure expression: |params| -> expr  OR  |params| { block }
// Also handles: || { block } (no params, double-pipe)
ASTNode* parse_closure_expression(Parser* parser) {
    Token* start = peek_token(parser);
    int line = start->line, col = start->column;

    ASTNode* closure = create_ast_node(AST_CLOSURE, NULL, line, col);

    if (start->type == TOKEN_OR) {
        // || means empty parameter list
        advance_token(parser); // consume '||'
    } else {
        // TOKEN_PIPE: |param1, param2, ...|
        advance_token(parser); // consume opening '|'

        // Check for empty |  |
        if (!match_token(parser, TOKEN_PIPE)) {
            // Parse parameters
            do {
                Token* param_name = expect_token(parser, TOKEN_IDENTIFIER);
                if (!param_name) {
                    free_ast_node(closure);
                    return NULL;
                }
                ASTNode* param = create_ast_node(AST_CLOSURE_PARAM, param_name->value,
                                                  param_name->line, param_name->column);
                // Optional type annotation: |x: int|
                if (match_token(parser, TOKEN_COLON)) {
                    Type* ptype = parse_type(parser);
                    if (ptype) {
                        param->node_type = ptype;
                    }
                }
                add_child(closure, param);
            } while (match_token(parser, TOKEN_COMMA));

            if (!expect_token(parser, TOKEN_PIPE)) {
                free_ast_node(closure);
                return NULL;
            }
        }
    }

    // Parse body: either -> expr  or  { block }
    Token* next = peek_token(parser);
    if (!next) {
        free_ast_node(closure);
        return NULL;
    }

    if (next->type == TOKEN_ARROW) {
        // Arrow body: |x| -> x * 2
        advance_token(parser); // consume '->'
        if (peek_token(parser) && peek_token(parser)->type == TOKEN_LEFT_BRACE) {
            // |x| -> { multi-statement block }
            ASTNode* body = parse_block(parser);
            add_child(closure, body);
        } else {
            // |x| -> single_expression
            ASTNode* expr = parse_expression(parser);
            if (!expr) {
                free_ast_node(closure);
                return NULL;
            }
            // Wrap in a block with implicit return
            ASTNode* ret = create_ast_node(AST_RETURN_STATEMENT, NULL, expr->line, expr->column);
            add_child(ret, expr);
            ASTNode* body = create_ast_node(AST_BLOCK, NULL, expr->line, expr->column);
            add_child(body, ret);
            add_child(closure, body);
        }
    } else if (next->type == TOKEN_LEFT_BRACE) {
        // Block body: |x| { statements }
        ASTNode* body = parse_block(parser);
        add_child(closure, body);
    } else {
        parser_error(parser, "Expected '->' or '{' after closure parameters");
        free_ast_node(closure);
        return NULL;
    }

    closure->node_type = create_type(TYPE_FUNCTION);
    return closure;
}

ASTNode* parse_primary_expression(Parser* parser) {
    Token* token = peek_token(parser);
    if (!token) return NULL;

    // #340: `none` — the empty-optional literal. A contextual keyword (so it
    // doesn't reserve the spelling globally): only an identifier token whose
    // text is exactly "none" in expression position becomes the literal. Its
    // concrete `T?` type is resolved from context by the typechecker.
    if (token->type == TOKEN_IDENTIFIER && token->value &&
        strcmp(token->value, "none") == 0) {
        advance_token(parser);
        return create_ast_node(AST_NONE_LITERAL, NULL, token->line, token->column);
    }

    switch (token->type) {
        case TOKEN_NUMBER:
        case TOKEN_STRING_LITERAL:
        case TOKEN_TRUE:
        case TOKEN_FALSE:
            return create_literal_node(advance_token(parser));

        case TOKEN_NULL: {
            Token* t = advance_token(parser);
            ASTNode* null_node = create_ast_node(AST_NULL_LITERAL, "null", t->line, t->column);
            null_node->node_type = create_type(TYPE_PTR);
            return null_node;
        }

        case TOKEN_IF: {
            // If-expression: if COND { EXPR } else { EXPR }
            Token* t = advance_token(parser); // consume 'if'
            ASTNode* cond = parse_expression(parser);
            if (!cond) return NULL;
            if (!expect_token(parser, TOKEN_LEFT_BRACE)) return NULL;
            ASTNode* then_expr = parse_expression(parser);
            if (!then_expr) return NULL;
            if (!expect_token(parser, TOKEN_RIGHT_BRACE)) return NULL;
            if (!expect_token(parser, TOKEN_ELSE)) return NULL;
            if (!expect_token(parser, TOKEN_LEFT_BRACE)) return NULL;
            ASTNode* else_expr = parse_expression(parser);
            if (!else_expr) return NULL;
            if (!expect_token(parser, TOKEN_RIGHT_BRACE)) return NULL;

            ASTNode* if_expr = create_ast_node(AST_IF_EXPRESSION, NULL, t->line, t->column);
            if_expr->node_type = create_type(TYPE_UNKNOWN);
            add_child(if_expr, cond);
            add_child(if_expr, then_expr);
            add_child(if_expr, else_expr);
            return if_expr;
        }

        case TOKEN_INTERP_STRING: {
            Token* t = advance_token(parser);
            return parse_interp_string_expr(t->value);
        }
            
        // Type keywords used as namespace names: string.new(), int.parse(), etc.
        case TOKEN_STRING:
        case TOKEN_INT:
        case TOKEN_DURATION:
        case TOKEN_FLOAT:
        case TOKEN_BOOL: {
            // Check if followed by dot - treat as namespace identifier
            Token* next = peek_ahead(parser, 1);
            if (next && next->type == TOKEN_DOT) {
                // Treat type keyword as identifier for namespace access
                return create_identifier_node(advance_token(parser));
            }
            // Otherwise return NULL - type keyword alone in expression is invalid
            return NULL;
        }

        case TOKEN_IDENTIFIER: {
            // Layout builtins: sizeof(TypeName) / offsetof(TypeName, field).
            // Lexed as plain identifiers (not reserved keywords) so user
            // code may still name things `sizeof`/`offsetof` elsewhere;
            // only the `sizeof(` / `offsetof(` call shape triggers these.
            {
                Token* after = peek_ahead(parser, 1);
                if (after && after->type == TOKEN_LEFT_PAREN &&
                    (strcmp(token->value, "sizeof") == 0 ||
                     strcmp(token->value, "offsetof") == 0)) {
                    bool is_offsetof = (strcmp(token->value, "offsetof") == 0);
                    int line = token->line;
                    int column = token->column;
                    advance_token(parser);                       // consume sizeof/offsetof
                    if (!expect_token(parser, TOKEN_LEFT_PAREN)) return NULL;
                    Token* tyname = expect_token(parser, TOKEN_IDENTIFIER);
                    if (!tyname) return NULL;
                    ASTNode* node = create_ast_node(
                        is_offsetof ? AST_OFFSETOF : AST_SIZEOF,
                        tyname->value, line, column);
                    node->node_type = create_type(TYPE_INT);
                    if (is_offsetof) {
                        if (!expect_token(parser, TOKEN_COMMA)) {
                            free_ast_node(node);
                            return NULL;
                        }
                        Token* field = expect_token(parser, TOKEN_IDENTIFIER);
                        if (!field) {
                            free_ast_node(node);
                            return NULL;
                        }
                        ASTNode* field_node = create_ast_node(
                            AST_IDENTIFIER, field->value, field->line, field->column);
                        add_child(node, field_node);
                    }
                    if (!expect_token(parser, TOKEN_RIGHT_PAREN)) {
                        free_ast_node(node);
                        return NULL;
                    }
                    return node;
                }
            }
            // __pure(funcName) — compile-time purity introspection (#522).
            // Folds to a `true`/`false` bool constant after whole-program
            // purity analysis. Same non-reserving treatment as sizeof: only
            // the `__pure(` call shape triggers it.
            {
                Token* after = peek_ahead(parser, 1);
                if (after && after->type == TOKEN_LEFT_PAREN && token->value &&
                    strcmp(token->value, "__pure") == 0) {
                    int line = token->line, column = token->column;
                    advance_token(parser);                       // consume __pure
                    if (!expect_token(parser, TOKEN_LEFT_PAREN)) return NULL;
                    Token* fname = expect_token(parser, TOKEN_IDENTIFIER);
                    if (!fname) return NULL;
                    ASTNode* node = create_ast_node(AST_PURITY_QUERY, fname->value, line, column);
                    node->node_type = create_type(TYPE_BOOL);
                    if (!expect_token(parser, TOKEN_RIGHT_PAREN)) {
                        free_ast_node(node);
                        return NULL;
                    }
                    return node;
                }
            }
            // C variadic-consumer intrinsics: va_start() / va_arg(vap, T)
            // / va_end(vap). Same non-reserving treatment as sizeof —
            // intercepted only on the `va_*(`  call shape. Usable only
            // inside a function declared with a trailing `...` param.
            {
                Token* after = peek_ahead(parser, 1);
                if (after && after->type == TOKEN_LEFT_PAREN && token->value &&
                    (strcmp(token->value, "va_start") == 0 ||
                     strcmp(token->value, "va_arg") == 0 ||
                     strcmp(token->value, "va_end") == 0)) {
                    int line = token->line;
                    int column = token->column;
                    if (strcmp(token->value, "va_start") == 0) {
                        advance_token(parser);                       // va_start
                        if (!expect_token(parser, TOKEN_LEFT_PAREN)) return NULL;
                        if (!expect_token(parser, TOKEN_RIGHT_PAREN)) return NULL;
                        ASTNode* node = create_ast_node(AST_VA_START, NULL, line, column);
                        node->node_type = create_type(TYPE_PTR);  // opaque va_list cookie
                        return node;
                    }
                    if (strcmp(token->value, "va_arg") == 0) {
                        advance_token(parser);                       // va_arg
                        if (!expect_token(parser, TOKEN_LEFT_PAREN)) return NULL;
                        ASTNode* cookie = parse_expression(parser);  // the va_list ptr
                        if (!cookie) return NULL;
                        if (!expect_token(parser, TOKEN_COMMA)) {
                            free_ast_node(cookie);
                            return NULL;
                        }
                        Type* arg_type = parse_type(parser);         // requested C type
                        if (!arg_type) { free_ast_node(cookie); return NULL; }
                        if (!expect_token(parser, TOKEN_RIGHT_PAREN)) {
                            free_ast_node(cookie);
                            free_type(arg_type);
                            return NULL;
                        }
                        ASTNode* node = create_ast_node(AST_VA_ARG, NULL, line, column);
                        node->node_type = arg_type;
                        add_child(node, cookie);
                        return node;
                    }
                    // va_end(vap)
                    advance_token(parser);                           // va_end
                    if (!expect_token(parser, TOKEN_LEFT_PAREN)) return NULL;
                    ASTNode* cookie = parse_expression(parser);
                    if (!cookie) return NULL;
                    if (!expect_token(parser, TOKEN_RIGHT_PAREN)) {
                        free_ast_node(cookie);
                        return NULL;
                    }
                    ASTNode* node = create_ast_node(AST_VA_END, NULL, line, column);
                    node->node_type = create_type(TYPE_VOID);
                    add_child(node, cookie);
                    return node;
                }
            }
            // #1046 bit_set set literal: `bit_set[E]{ E.A, E.B }` (also the
            // bare-member form `{ A, B }`, and the empty set `bit_set[E]{}`).
            // Intercepted on the `bit_set[` shape only, so user code may still
            // name things `bit_set` elsewhere (same non-reserving treatment as
            // sizeof). Each element is normalized to an AST_MEMBER_ACCESS
            // `E.Member` so the enum-resolution pass lowers it to `E_Member`.
            {
                Token* after = peek_ahead(parser, 1);
                if (after && after->type == TOKEN_LEFT_BRACKET && token->value &&
                    strcmp(token->value, "bit_set") == 0) {
                    int line = token->line, column = token->column;
                    advance_token(parser);                       // consume 'bit_set'
                    advance_token(parser);                       // consume '['
                    Type* elem = parse_type(parser);
                    if (!elem) {
                        parser_error(parser,
                            "bit_set requires an enum type parameter, e.g. bit_set[Color]{ Color.Red }");
                        return NULL;
                    }
                    if (!expect_token(parser, TOKEN_RIGHT_BRACKET)) { free_type(elem); return NULL; }
                    if (!expect_token(parser, TOKEN_LEFT_BRACE)) { free_type(elem); return NULL; }
                    const char* enum_name = elem->struct_name;  // element enum's name
                    ASTNode* lit = create_ast_node(AST_BITSET_LITERAL, NULL, line, column);
                    lit->node_type = create_bitset_type(elem);
                    if (!match_token(parser, TOKEN_RIGHT_BRACE)) {
                        do {
                            Token* first = expect_token(parser, TOKEN_IDENTIFIER);
                            if (!first) { free_ast_node(lit); return NULL; }
                            const char* base_name;   // enum name for this element
                            const char* member_name; // member name
                            if (peek_token(parser) && peek_token(parser)->type == TOKEN_DOT) {
                                advance_token(parser);            // consume '.'
                                Token* mem = expect_token(parser, TOKEN_IDENTIFIER);
                                if (!mem) { free_ast_node(lit); return NULL; }
                                base_name = first->value;         // qualified: E.Member
                                member_name = mem->value;
                            } else {
                                if (!enum_name) {
                                    parser_error(parser,
                                        "bare set member needs an enum element type, e.g. bit_set[Color]{ Red }");
                                    free_ast_node(lit);
                                    return NULL;
                                }
                                base_name = enum_name;            // bare: Member
                                member_name = first->value;
                            }
                            ASTNode* ma = create_ast_node(AST_MEMBER_ACCESS, member_name,
                                                          first->line, first->column);
                            ASTNode* base = create_ast_node(AST_IDENTIFIER, base_name,
                                                            first->line, first->column);
                            add_child(ma, base);
                            add_child(lit, ma);
                        } while (match_token(parser, TOKEN_COMMA));
                        if (!expect_token(parser, TOKEN_RIGHT_BRACE)) { free_ast_node(lit); return NULL; }
                    }
                    return lit;
                }
            }
            // #1046 card(s), cardinality (popcount) of a bit_set. Intercepted on
            // the `card(` call shape only (same non-reserving treatment as sizeof).
            {
                Token* after = peek_ahead(parser, 1);
                if (after && after->type == TOKEN_LEFT_PAREN && token->value &&
                    strcmp(token->value, "card") == 0) {
                    int line = token->line, column = token->column;
                    advance_token(parser);                       // consume 'card'
                    if (!expect_token(parser, TOKEN_LEFT_PAREN)) return NULL;
                    ASTNode* arg = parse_expression(parser);
                    if (!arg) return NULL;
                    if (!expect_token(parser, TOKEN_RIGHT_PAREN)) { free_ast_node(arg); return NULL; }
                    ASTNode* node = create_ast_node(AST_BITSET_CARD, NULL, line, column);
                    node->node_type = create_type(TYPE_INT);
                    add_child(node, arg);
                    return node;
                }
            }
            // Could be identifier or struct literal
            Token* next = peek_ahead(parser, 1);

            /* Qualified struct literal: `module.Type { field: value }`.
             * Pattern: IDENTIFIER `.` IDENTIFIER `{` IDENTIFIER `:`
             * (with the empty-struct variant `module.Type{}`). Parse
             * the dotted name into the struct_name so the rest of the
             * struct-literal lowering uses `Type` as the struct (which
             * is what the rename pass produces — modules' struct
             * definitions go into the program AST by their bare name,
             * not module-prefixed). Filed in aether/new_aevg_asks.md
             * ASK 2 from the AeVG port. */
            if (next && next->type == TOKEN_DOT && !parser->in_condition) {
                Token* t_type = peek_ahead(parser, 2);
                Token* t_brace = peek_ahead(parser, 3);
                if (t_type && t_type->type == TOKEN_IDENTIFIER &&
                    t_brace && t_brace->type == TOKEN_LEFT_BRACE) {
                    Token* t_after = peek_ahead(parser, 4);
                    int looks_qualified_struct = 0;
                    if (t_after && t_after->type == TOKEN_RIGHT_BRACE) {
                        looks_qualified_struct = 1;
                    } else if (t_after && token_is_value_ident(t_after)) {
                        // #880: value-identifier keyword as the first field name
                        Token* t_colon = peek_ahead(parser, 5);
                        if (t_colon && t_colon->type == TOKEN_COLON) {
                            looks_qualified_struct = 1;
                        }
                    }
                    if (looks_qualified_struct) {
                        /* Drop the module prefix — module structs land
                         * in the program AST by their bare name. */
                        char* struct_name = strdup(t_type->value);
                        int s_line = token->line;
                        int s_col = token->column;
                        advance_token(parser);  /* module ident */
                        advance_token(parser);  /* . */
                        advance_token(parser);  /* Type ident */
                        advance_token(parser);  /* { */

                        ASTNode* struct_lit = create_ast_node(AST_STRUCT_LITERAL, struct_name, s_line, s_col);
                        if (!match_token(parser, TOKEN_RIGHT_BRACE)) {
                            do {
                                // #880: accept value-identifier keyword field names
                                Token* field_name = peek_token(parser);
                                if (field_name && token_is_value_ident(field_name)) {
                                    advance_token(parser);
                                } else {
                                    field_name = expect_token(parser, TOKEN_IDENTIFIER);
                                    if (!field_name) { free_ast_node(struct_lit); return NULL; }
                                }
                                if (!expect_token(parser, TOKEN_COLON)) { free_ast_node(struct_lit); return NULL; }
                                ASTNode* value_expr = parse_expression(parser);
                                if (!value_expr) { free_ast_node(struct_lit); return NULL; }
                                ASTNode* field_init = create_ast_node(AST_ASSIGNMENT, field_name->value,
                                                                       field_name->line, field_name->column);
                                add_child(field_init, value_expr);
                                add_child(struct_lit, field_init);
                            } while (match_token(parser, TOKEN_COMMA));
                            if (!expect_token(parser, TOKEN_RIGHT_BRACE)) { free_ast_node(struct_lit); return NULL; }
                        }
                        return struct_lit;
                    }
                }
            }
            // Disambiguate: IDENTIFIER { could be a struct literal OR an identifier
            // followed by a block (e.g., while i < n { ... }).
            // A struct literal has the pattern: TypeName { field: value } or TypeName {}
            // A block-preceding identifier has statements (not field:) after the {.
            // Look 2-3 tokens ahead to check for the struct literal pattern.
            //
            // BUT not when we're inside an if/while/for condition — there
            // the `{` after the identifier is the start of the statement's
            // body, not a struct literal. Otherwise `if a == b {}` would
            // greedily parse `b {}` as an empty struct literal and consume
            // the body's braces, leaving `else` orphaned in the outer
            // block. Same shape as the trailing-closure suppression above
            // (parse_call_expression). Caught while writing range_compress
            // for the mquickjs port.
            bool looks_like_struct = false;
            if (next && next->type == TOKEN_LEFT_BRACE && !parser->in_condition) {
                Token* after_brace = peek_ahead(parser, 2);
                if (after_brace && after_brace->type == TOKEN_RIGHT_BRACE) {
                    // TypeName {} — empty struct literal
                    looks_like_struct = true;
                } else if (after_brace && token_is_value_ident(after_brace)) {
                    // #880: a first field named with a value-identifier keyword
                    // (`ptr`/`byte`/`func`/`state`/`after`) still marks this as
                    // a struct literal, not an identifier-then-block.
                    Token* after_field = peek_ahead(parser, 3);
                    if (after_field && after_field->type == TOKEN_COLON) {
                        // TypeName { field: value } — struct literal
                        looks_like_struct = true;
                    }
                }
            }
            if (next && next->type == TOKEN_LEFT_BRACE && looks_like_struct) {
                // Struct literal: TypeName{ field: value, ... }
                char* struct_name = strdup(token->value);
                int line = token->line;
                int column = token->column;
                advance_token(parser); // consume identifier
                advance_token(parser); // consume '{'

                ASTNode* struct_lit = create_ast_node(AST_STRUCT_LITERAL, struct_name, line, column);

                // Parse field initializers
                if (!match_token(parser, TOKEN_RIGHT_BRACE)) {
                    do {
                        // Parse field name. #880: accept the value-identifier
                        // keywords (`ptr`/`byte`/`func`/`state`/`after`) so a
                        // literal can initialise a struct whose fields carry
                        // those (valid-C) names.
                        Token* field_name = peek_token(parser);
                        if (field_name && token_is_value_ident(field_name)) {
                            advance_token(parser);
                        } else {
                            field_name = expect_token(parser, TOKEN_IDENTIFIER);
                            if (!field_name) {
                                free_ast_node(struct_lit);
                                return NULL;
                            }
                        }

                        // Expect colon
                        if (!expect_token(parser, TOKEN_COLON)) {
                            free_ast_node(struct_lit);
                            return NULL;
                        }

                        // Parse field value
                        ASTNode* value_expr = parse_expression(parser);
                        if (!value_expr) {
                            free_ast_node(struct_lit);
                            return NULL;
                        }

                        // Create field init node
                        ASTNode* field_init = create_ast_node(AST_ASSIGNMENT, field_name->value,
                                                              field_name->line, field_name->column);
                        add_child(field_init, value_expr);
                        add_child(struct_lit, field_init);

                    } while (match_token(parser, TOKEN_COMMA));

                    if (!expect_token(parser, TOKEN_RIGHT_BRACE)) {
                        free_ast_node(struct_lit);
                        return NULL;
                    }
                }

                return struct_lit;
            } else {
                // Regular identifier
                return create_identifier_node(advance_token(parser));
            }
        }
            
        case TOKEN_LEFT_PAREN: {
            int line = token->line;
            int column = token->column;
            advance_token(parser);
            ASTNode* expr = parse_expression(parser);
            if (!expr) return NULL;
            /* #1033: `(a, b, ...)` — a comma after the first expression
             * upgrades the grouping to a tuple literal. Only meaningful
             * as an argument to a tuple-typed extern parameter (the
             * typechecker rejects it everywhere else); a plain
             * parenthesized expression `(a)` is unaffected. */
            if (peek_token(parser) && peek_token(parser)->type == TOKEN_COMMA) {
                ASTNode* tup = create_ast_node(AST_TUPLE_LITERAL, NULL, line, column);
                add_child(tup, expr);
                while (match_token(parser, TOKEN_COMMA)) {
                    ASTNode* elem = parse_expression(parser);
                    if (!elem) {
                        free_ast_node(tup);
                        return NULL;
                    }
                    add_child(tup, elem);
                }
                if (!expect_token(parser, TOKEN_RIGHT_PAREN)) {
                    free_ast_node(tup);
                    return NULL;
                }
                return tup;
            }
            if (!expect_token(parser, TOKEN_RIGHT_PAREN)) return NULL;
            return expr;
        }
        
        case TOKEN_LEFT_BRACKET: {
            // Array literal: [1, 2, 3]
            int line = token->line;
            int column = token->column;
            advance_token(parser); // consume '['
            
            ASTNode* array_lit = create_ast_node(AST_ARRAY_LITERAL, NULL, line, column);
            
            // Parse array elements
            if (!match_token(parser, TOKEN_RIGHT_BRACKET)) {
                do {
                    ASTNode* element = parse_expression(parser);
                    if (element) {
                        add_child(array_lit, element);
                    }
                } while (match_token(parser, TOKEN_COMMA));
                
                if (!expect_token(parser, TOKEN_RIGHT_BRACKET)) {
                    free_ast_node(array_lit);
                    return NULL;
                }
            }
            
            return array_lit;
        }
        
        case TOKEN_SELF:
            advance_token(parser);
            return create_ast_node(AST_ACTOR_REF, "self", token->line, token->column);
        
        case TOKEN_MAKE: {
            // make([]type, size) for dynamic arrays
            int line = token->line;
            int column = token->column;
            advance_token(parser); // consume 'make'

            if (!expect_token(parser, TOKEN_LEFT_PAREN)) return NULL;

            // Parse []type syntax
            if (!expect_token(parser, TOKEN_LEFT_BRACKET)) return NULL;
            if (!expect_token(parser, TOKEN_RIGHT_BRACKET)) return NULL;

            // Parse element type
            Type* element_type = parse_type(parser);
            if (!element_type) {
                parser_error(parser, "Expected type after [] in make");
                return NULL;
            }

            // Parse comma
            if (!expect_token(parser, TOKEN_COMMA)) return NULL;

            // Parse size expression
            ASTNode* size_expr = parse_expression(parser);
            if (!size_expr) {
                parser_error(parser, "Expected size expression in make");
                return NULL;
            }

            if (!expect_token(parser, TOKEN_RIGHT_PAREN)) return NULL;

            // Create a function call node: malloc(size * sizeof(type))
            // We'll transform this in codegen
            ASTNode* make_node = create_ast_node(AST_FUNCTION_CALL, "make", line, column);
            make_node->node_type = create_array_type(element_type, -1); // Dynamic array
            add_child(make_node, size_expr);

            return make_node;
        }

        case TOKEN_SPAWN: {
            // spawn(ActorName()) or spawn(ActorName(), core: N)
            int line = token->line;
            int column = token->column;
            advance_token(parser); // consume 'spawn'

            // Expect opening paren: spawn(...)
            if (!expect_token(parser, TOKEN_LEFT_PAREN)) {
                parser_error(parser, "Expected '(' after 'spawn'");
                return NULL;
            }

            Token* actor_name = expect_token(parser, TOKEN_IDENTIFIER);
            if (!actor_name) {
                parser_error(parser, "Expected actor name inside spawn(...)");
                return NULL;
            }

            // Expect () after actor name (constructor args)
            if (!expect_token(parser, TOKEN_LEFT_PAREN)) return NULL;
            if (!expect_token(parser, TOKEN_RIGHT_PAREN)) return NULL;

            // Internal representation: AST_FUNCTION_CALL with spawn_ActorName
            char func_name[256];
            snprintf(func_name, sizeof(func_name), "spawn_%s", actor_name->value);

            ASTNode* spawn_call = create_ast_node(AST_FUNCTION_CALL, func_name, line, column);

            // Optional core placement hint: spawn(Actor(), core: N)
            Token* next = peek_token(parser);
            if (next && next->type == TOKEN_COMMA) {
                advance_token(parser);  // consume ','
                Token* keyword = expect_token(parser, TOKEN_IDENTIFIER);
                if (!keyword || strcmp(keyword->value, "core") != 0) {
                    parser_error(parser, "Expected 'core' keyword in spawn options");
                    return NULL;
                }
                if (!expect_token(parser, TOKEN_COLON)) return NULL;
                ASTNode* core_expr = parse_expression(parser);
                if (!core_expr) return NULL;
                add_child(spawn_call, core_expr);  // child[0] = core expression
            }

            // Expect closing paren for spawn(...)
            if (!expect_token(parser, TOKEN_RIGHT_PAREN)) return NULL;

            return spawn_call;
        }

        case TOKEN_PRINT: {
            // Allow print() as an expression (e.g., in pattern matching bodies)
            int line = token->line;
            int column = token->column;
            advance_token(parser); // consume 'print'

            if (!expect_token(parser, TOKEN_LEFT_PAREN)) {
                parser_error(parser, "Expected '(' after 'print'");
                return NULL;
            }

            ASTNode* print_call = create_ast_node(AST_PRINT_STATEMENT, NULL, line, column);

            // Parse arguments
            if (!match_token(parser, TOKEN_RIGHT_PAREN)) {
                do {
                    ASTNode* arg = parse_expression(parser);
                    if (!arg) {
                        free_ast_node(print_call);
                        return NULL;
                    }
                    add_child(print_call, arg);
                } while (match_token(parser, TOKEN_COMMA));

                if (!expect_token(parser, TOKEN_RIGHT_PAREN)) {
                    free_ast_node(print_call);
                    return NULL;
                }
            }

            return print_call;
        }

        case TOKEN_STATE:
            // Outside actor bodies, 'state' is treated as a regular identifier
        case TOKEN_PTR:
        case TOKEN_BYTE:
        case TOKEN_FUNC:
        case TOKEN_AFTER:
            // #880: these keywords have meaning only in type position
            // (`ptr`/`byte`), as a declaration head (`func`) or as statement
            // heads (`state`/`after`) — none of which reach a primary-
            // expression operand. So in value position (e.g. `return ptr`,
            // `ptr + 1`, `func.field`) accept them as ordinary identifiers.
            // `match` (match-expression head) and `union` (a C keyword that
            // can't be a C identifier) are deliberately NOT here (#880).
            return create_identifier_node(advance_token(parser));

        case TOKEN_PIPE:
        case TOKEN_OR:
            // Closure expression: |params| -> expr  OR  || { block }
            return parse_closure_expression(parser);

        default:
            return NULL;
    }
}

/* Newline boundary recogniser for issue #528. Aether has no semicolon
 * requirement, so a token on a later source line than the previous token
 * starts a fresh statement when it could otherwise be parsed as an infix
 * or postfix continuation. Multiline expressions remain available by
 * placing the continuing operator before the newline:
 *
 *     total = a +
 *         b
 *
 * This mirrors the existing same-line rule for trailing closures and
 * removes the old token-shape heuristics (`*StructName name`, `[a, b]`,
 * etc.) that still left `-x` ambiguous. */
static int operator_starts_newline(Parser* parser, Token* op) {
    if (!op) return 0;

    Token* prev = peek_ahead(parser, -1);
    if (!prev || op->line <= prev->line) return 0;
    return 1;
}

ASTNode* parse_expression(Parser* parser) {
    ASTNode* expr = parse_binary_expression(parser, 0);
    if (!expr) return NULL;
    // #913: postfix error handler `expr or { … }` / `expr or <default>`. `or`
    // is a CONTEXTUAL keyword — still a valid identifier elsewhere (`byte or =
    // …`) — recognised only here, in value position, on the same line (so a
    // bare expression followed by a statement that happens to start with an
    // `or`-named identifier stays two statements). The LHS is a fallible
    // `(value, err)` / `T!` expression; the handler runs when the error slot
    // is non-empty, binding `err`, and yields a value or exits (return/…).
    Token* t = peek_token(parser);
    if (t && t->type == TOKEN_IDENTIFIER && t->value &&
        strcmp(t->value, "or") == 0 && !operator_starts_newline(parser, t)) {
        advance_token(parser);  // consume contextual 'or'
        ASTNode* handler;
        Token* nx = peek_token(parser);
        if (nx && nx->type == TOKEN_LEFT_BRACE) {
            handler = parse_block(parser);
        } else {
            handler = parse_expression(parser);   // bare default value
        }
        if (!handler) return NULL;
        ASTNode* oe = create_ast_node(AST_OR_ELSE, NULL, expr->line, expr->column);
        add_child(oe, expr);
        add_child(oe, handler);
        return oe;
    }
    return expr;
}

ASTNode* parse_binary_expression(Parser* parser, int precedence) {
    ASTNode* left = parse_unary_expression(parser);
    if (!left) return NULL;
    
    int iteration_count = 0;
    const int MAX_BINARY_OPS = 1000;
    
    while (1) {
        if (++iteration_count > MAX_BINARY_OPS) {
            parser_message(parser, "Error: Expression too complex (max 1000 binary operators)");
            break;
        }
        
        Token* operator = peek_token(parser);
        if (!operator) break;

        int op_precedence = get_operator_precedence(operator->type);
        if (op_precedence < 0) break;  // Not an operator
        if (op_precedence < precedence) break;  // Lower precedence, stop

        if (operator_starts_newline(parser, operator)) {
            break;
        }

        advance_token(parser);
        ASTNode* right = parse_binary_expression(parser, op_precedence + 1);  // Left-associative
        if (!right) {
            /* The half-built left operand is owned here alone; dropping it
             * on the error path leaked once per failed expression, which in
             * the long-lived lsp mode grows per keystroke. */
            free_ast_node(left);
            return NULL;
        }

        // #340: `a ?? b` builds a dedicated null-coalesce node (children:
        // [optional, default]) rather than a generic binary expression, so the
        // typechecker/codegen handle it as its own form.
        if (operator->type == TOKEN_QUESTION_QUESTION) {
            ASTNode* nc = create_ast_node(AST_NULL_COALESCE, NULL, operator->line, operator->column);
            add_child(nc, left);
            add_child(nc, right);
            left = nc;
        } else {
            left = create_binary_expression(left, right, operator);
        }
    }

    return left;
}

// Parse postfix expressions like i++ / i-- / obj.field
static ASTNode* parse_postfix_expression(Parser* parser) {
    ASTNode* expr = parse_primary_expression(parser);
    if (!expr) return NULL;
    
    int iteration_count = 0;
    const int MAX_POSTFIX_OPS = 100;
    
    while (1) {
        if (++iteration_count > MAX_POSTFIX_OPS) {
            parser_message(parser, "Error: Too many postfix operations (max 100)");
            break;
        }
        
        Token* op = peek_token(parser);
        if (!op) break;
        
        if (op->type == TOKEN_INCREMENT || op->type == TOKEN_DECREMENT) {
            advance_token(parser);
            expr = create_unary_expression(expr, op);
            continue;
        }

        // #340: optional chaining `opt?.field` -> fieldT? (none-propagating).
        if (op->type == TOKEN_QUESTION_DOT) {
            advance_token(parser);
            Token* field = peek_token(parser);
            if (!field) return NULL;
            int field_ok = (field->type == TOKEN_IDENTIFIER) ||
                           token_is_reserved_keyword(field);
            if (!field_ok) {
                expect_token(parser, TOKEN_IDENTIFIER);
                return NULL;
            }
            advance_token(parser);
            ASTNode* chain = create_ast_node(AST_OPTIONAL_CHAIN, field->value, op->line, op->column);
            add_child(chain, expr);
            expr = chain;
            continue;
        }

        if (op->type == TOKEN_DOT) {
            // Member access: expr.field
            //
            // Accept reserved keywords as field names — `io.print(...)`
            // calls a method named `print` (TOKEN_PRINT in the lexer)
            // on the `io` namespace; same for `obj.match`, `actor.send`,
            // etc. Without this allowance, expect_token(TOKEN_IDENTIFIER)
            // hits the reserved-keyword path (parser.c:71) and emits a
            // spurious "rename it" error for every method call that
            // shares a name with an Aether keyword.
            advance_token(parser);
            Token* field = peek_token(parser);
            if (!field) return NULL;
            int field_ok = (field->type == TOKEN_IDENTIFIER) ||
                           token_is_reserved_keyword(field);
            if (!field_ok) {
                expect_token(parser, TOKEN_IDENTIFIER);  /* trigger the standard error */
                return NULL;
            }
            advance_token(parser);

            ASTNode* member_access = create_ast_node(AST_MEMBER_ACCESS, field->value, op->line, op->column);
            add_child(member_access, expr);
            expr = member_access;
            continue;
        }
        
        if (op->type == TOKEN_LEFT_BRACKET) {
            if (operator_starts_newline(parser, op)) {
                break;
            }
            // Array indexing: expr[index]
            advance_token(parser); // consume '['
            ASTNode* index = parse_expression(parser);
            if (!index) return NULL;
            if (!expect_token(parser, TOKEN_RIGHT_BRACKET)) return NULL;

            ASTNode* array_access = create_ast_node(AST_ARRAY_ACCESS, NULL, op->line, op->column);
            add_child(array_access, expr);  // array expression
            add_child(array_access, index); // index expression
            expr = array_access;
            continue;
        }

        if (op->type == TOKEN_AS) {
            // Pointer-overlay cast.  Two destination shapes:
            //   `expr as *StructName`           — struct overlay
            //   `expr as fn(T1, T2, ...) -> R`  — typed function-ptr
            //
            // Struct form: views a raw `ptr`-typed value as a
            // pointer-to-struct so member access (`view.field`) can
            // reach struct fields. The `ptr` operand's lifetime is the
            // caller's problem — the cast does NOT allocate, refcount,
            // or auto-free. This is the systems-programming escape
            // hatch for FFI shapes that overlay struct headers on raw
            // memory (e.g. QuickJS-style tagged-pointer ports). The
            // leading `*` makes the pointer-ness visible in source;
            // the result type is spelled `*StructName` and matches
            // type annotations on function parameters, struct fields,
            // etc.
            //
            // Fn form: views a raw `ptr`-typed value (typically a
            // function pointer returned by a C extern) as a typed
            // callable.  The signature is carried on the AST node's
            // node_type and consumed at the call site to emit the
            // matching `((R (*)(T1, T2))(p))(a, b)` C cast.  No
            // typedef synthesis, no storage change — locals of fn-type
            // stay `void*`-shaped in the emitted C.  Use case: storing
            // a vtable lookup result then invoking it directly with
            // type checking instead of routing through bespoke
            // per-signature `mem.call_fn3_*` shims.
            //
            // The keyword token TOKEN_AS is shared with `import x as y`
            // aliasing; that's parsed only inside import statements so
            // there's no collision.
            advance_token(parser);  /* consume `as` */
            Token* next = peek_token(parser);
            if (next && next->type == TOKEN_IDENTIFIER && next->value &&
                strcmp(next->value, "fn") == 0) {
                /* `as fn(...) -> R` — reuse parse_type's `fn(...) -> R`
                 * branch so the signature parsing lives in one place. */
                Type* fn_type = parse_type(parser);
                if (!fn_type) return NULL;
                if (fn_type->kind != TYPE_FUNCTION || fn_type->param_count < 0) {
                    parser_error(parser, "Expected fn(T1, T2, ...) -> R after `as`");
                    free_type(fn_type);
                    return NULL;
                }
                ASTNode* cast = create_ast_node(AST_PTR_AS_FN_CAST,
                                                NULL,
                                                op->line, op->column);
                cast->node_type = fn_type;
                add_child(cast, expr);
                expr = cast;
                continue;
            }
            if (!(next && next->type == TOKEN_MULTIPLY)) {
                // Not `as *T`. Parse the target type: a `T[]` is the typed-
                // array view cast; anything else (a scalar, or a named type
                // that may resolve to a distinct type) is a #480 value cast —
                // a zero-cost nominal (un)wrap or numeric conversion.
                Type* target = parse_type(parser);
                if (!target) return NULL;
                if (target->kind == TYPE_ARRAY && target->element_type) {
                    ASTNode* cast = create_ast_node(AST_PTR_AS_ARRAY_CAST,
                                                    NULL,
                                                    op->line, op->column);
                    cast->node_type = target;
                    add_child(cast, expr);
                    expr = cast;
                    continue;
                }
                ASTNode* cast = create_ast_node(AST_VALUE_CAST,
                                                NULL,
                                                op->line, op->column);
                cast->node_type = target;
                add_child(cast, expr);
                expr = cast;
                continue;
            }
            advance_token(parser);  // consume `*` (was just peeked)
            Token* struct_name_tok = expect_token(parser, TOKEN_IDENTIFIER);
            if (!struct_name_tok) return NULL;
            ASTNode* cast = create_ast_node(AST_PTR_AS_STRUCT_CAST,
                                            struct_name_tok->value,
                                            op->line, op->column);
            add_child(cast, expr);
            expr = cast;
            continue;
        }
        
        if (op->type == TOKEN_LEFT_PAREN) {
            // Function call: expr(arg1, arg2, ...)
            //
            // Statement-boundary guard: if the `(` is on a different
            // source line from the previous token, treat it as the
            // start of a NEW statement (a leading `(parenthesized
            // expression)` whose member/cast result the user wants
            // to assign into) rather than as a call applied to the
            // expression we've parsed so far.  Aether doesn't require
            // semicolons; this same "newline acts as statement
            // separator unless the next token clearly continues"
            // heuristic is already used for trailing-block braces
            // (line ~1180 below).  Without this guard,
            //     println("foo")
            //     (raw as *T).field = v
            // parses as `println("foo")(raw as *T).field = v` — one
            // expression — producing a bogus AST_FUNCTION_CALL with
            // expr=AST_BINARY_EXPRESSION as the callee, which then
            // fails typecheck with `Undefined function '?'`.
            int prev_line = (parser->current_token > 0)
                ? parser->tokens[parser->current_token - 1]->line
                : -1;
            if (op->line != prev_line) {
                break;  // not a call — let parse_statement see the `(` as a fresh statement
            }

            // Extract function name - handle both simple and namespaced calls
            const char* func_name = NULL;
            // #928 UFCS shape (a): a method-call on a NON-identifier
            // receiver (a call result, an indexed value, etc. —
            // `expect(5).to_eq(...)`). The receiver subtree is detached
            // here and handed to the typechecker as the implicit first
            // argument; ufcs_recv holds it until the call node exists.
            ASTNode* ufcs_recv = NULL;
            if (expr && expr->type == AST_IDENTIFIER && expr->value) {
                // Simple call: foo()
                func_name = strdup(expr->value);
            } else if (expr && expr->type == AST_MEMBER_ACCESS && expr->value &&
                       expr->child_count > 0 && expr->children[0] &&
                       expr->children[0]->type == AST_IDENTIFIER) {
                // Namespaced call: namespace.func() -> store as "namespace.func"
                // (the receiver MIGHT be a value, not a module — the
                // typechecker tries qualified resolution first and only
                // falls back to UFCS, shape (b), if that fails.)
                char qualified_name[256];
                snprintf(qualified_name, sizeof(qualified_name), "%s.%s",
                         expr->children[0]->value, expr->value);
                func_name = strdup(qualified_name);
            } else if (expr && expr->type == AST_MEMBER_ACCESS && expr->value &&
                       expr->child_count > 0 && expr->children[0]) {
                // #928 UFCS shape (a): receiver is not a bare identifier, so
                // there is no qualified name to form. Carry the bare method
                // name and detach the receiver subtree.
                func_name = strdup(expr->value);
                ufcs_recv = expr->children[0];
                expr->children[0] = NULL;   // detach so free_ast_node(expr) won't reclaim it
            }

            // heap.new(TypeName) — like sizeof, the argument is a *type
            // name*, not a value, so it can't go through the normal
            // argument parser (which would treat `AppCtx` as an undefined
            // variable). Intercept the call shape here and build a
            // dedicated AST_HEAP_NEW node carrying the struct name. (#564)
            // heap.free(p) is an ordinary call — handled at codegen.
            if (func_name && strcmp(func_name, "heap.new") == 0) {
                int hn_line = op->line, hn_col = op->column;
                advance_token(parser); // consume '('
                Token* tyname = expect_token(parser, TOKEN_IDENTIFIER);
                if (!tyname) { free((void*)func_name); free_ast_node(expr); return NULL; }
                ASTNode* heap_new = create_ast_node(AST_HEAP_NEW, tyname->value,
                                                    hn_line, hn_col);
                if (!expect_token(parser, TOKEN_RIGHT_PAREN)) {
                    free((void*)func_name); free_ast_node(expr);
                    free_ast_node(heap_new); return NULL;
                }
                free((void*)func_name);
                free_ast_node(expr);  // discard the `heap.new` member-access subtree
                expr = heap_new;
                continue;             // resume the postfix loop on the new node
            }

            advance_token(parser); // consume '('

            ASTNode* func_call = create_ast_node(AST_FUNCTION_CALL, func_name, op->line, op->column);
            /* create_ast_node strdups its value; this working copy leaked
             * once per parsed call expression. */
            free((void*)func_name);

            // #928 UFCS shape (a): mark the node and seat the detached
            // receiver as the implicit first argument (children[0]). The
            // typechecker rewrites `method(recv, args)` and clears the tag;
            // if no free function matches, the tag is dropped and the
            // standard Undefined-function error fires on `func_name`.
            if (ufcs_recv) {
                func_call->annotation = strdup("ufcs");
                add_child(func_call, ufcs_recv);
            }

            // Parse arguments
            if (!match_token(parser, TOKEN_RIGHT_PAREN)) {
                do {
                    // Check for named argument: IDENTIFIER : expr
                    Token* maybe_name = peek_token(parser);
                    Token* maybe_colon = peek_ahead(parser, 1);
                    if (maybe_name && maybe_name->type == TOKEN_IDENTIFIER &&
                        maybe_colon && maybe_colon->type == TOKEN_COLON) {
                        // Named argument
                        Token* name_tok = advance_token(parser); // consume name
                        advance_token(parser); // consume ':'
                        ASTNode* value = parse_expression(parser);
                        if (!value) {
                            free_ast_node(func_call);
                            return NULL;
                        }
                        ASTNode* named = create_ast_node(AST_NAMED_ARG,
                            name_tok->value, name_tok->line, name_tok->column);
                        add_child(named, value);
                        add_child(func_call, named);
                    } else {
                        // Positional argument
                        ASTNode* arg = parse_expression(parser);
                        if (!arg) {
                            free_ast_node(func_call);
                            return NULL;
                        }
                        add_child(func_call, arg);
                    }
                } while (match_token(parser, TOKEN_COMMA));

                if (!expect_token(parser, TOKEN_RIGHT_PAREN)) {
                    free_ast_node(func_call);
                    return NULL;
                }
            }

            // Capture the closing paren's line for the trailing-block
            // line check below. Both arg-list paths (empty via
            // match_token at the head of the surrounding if; non-empty
            // via expect_token after the do/while) have just consumed
            // the `)`, so it sits at parser->current_token - 1. See #286:
            // a `{` on a later line must NOT be eaten as a trailing
            // closure for this call — it is a separate bare-brace block.
            int paren_close_line = (parser->current_token > 0)
                ? parser->tokens[parser->current_token - 1]->line
                : -1;

            // Check for trailing closure/block after function call
            // func(args) { body }  or  func(args) |x| { body }
            // func(args) callback { body }  or  func(args) callback |x| { body }
            {
                Token* next_tok = peek_token(parser);
                if (next_tok && next_tok->type == TOKEN_CALLBACK) {
                    // Callback trailing block: always a real closure (hoisted, captures vars)
                    // func(args) callback { body }  — zero-param closure
                    // func(args) callback |x| { body }  — parameterized closure
                    advance_token(parser); // consume 'callback'
                    Token* after_cb = peek_token(parser);
                    if (after_cb && (after_cb->type == TOKEN_PIPE || after_cb->type == TOKEN_OR)) {
                        // callback |params| { body }
                        ASTNode* trailing = parse_closure_expression(parser);
                        if (trailing) {
                            add_child(func_call, trailing);
                        }
                    } else if (after_cb && after_cb->type == TOKEN_LEFT_BRACE) {
                        // callback { body } — zero-param closure (NOT a DSL block)
                        ASTNode* trailing = create_ast_node(AST_CLOSURE, NULL,
                                                             after_cb->line, after_cb->column);
                        trailing->node_type = create_type(TYPE_FUNCTION);
                        ASTNode* body = parse_block(parser);
                        add_child(trailing, body);
                        add_child(func_call, trailing);
                    }
                } else if (next_tok && (next_tok->type == TOKEN_PIPE || next_tok->type == TOKEN_OR)
                           && looks_like_trailing_closure(parser, 0)) {
                    // Trailing closure with params: func(args) |x| { ... }
                    // These are real closures (not DSL blocks) — they get hoisted.
                    // The looks_like_trailing_closure guard distinguishes this
                    // from `func(args) | EXPR` (bitwise-or) / `func(args) || EXPR`
                    // (logical-or), where the `|`/`||` is a binary operator and
                    // must be left for the expression parser.
                    ASTNode* trailing = parse_closure_expression(parser);
                    if (trailing) {
                        add_child(func_call, trailing);
                    }
                } else if (next_tok && next_tok->type == TOKEN_LEFT_BRACE &&
                           !parser->in_condition &&
                           next_tok->line == paren_close_line) {
                    // Trailing block without params: func(args) { body }
                    //
                    // Only attached when `{` is on the same source line as
                    // the call's closing `)`. A `{` on a later line is a
                    // separate bare-brace block (handled by the statement
                    // parser via TOKEN_LEFT_BRACE → parse_block). See #286
                    // and docs/closures-and-builder-dsl.md § Same-line rule
                    // for trailing blocks.
                    //
                    // Also suppressed when we're parsing an if/while/for
                    // condition: the `{` there is the start of the
                    // statement's body, not a trailing closure attached to
                    // the rightmost call. Eating it here would swallow the
                    // real body and produce silently wrong code (e.g. an
                    // infinite while loop because the increment statement
                    // becomes the if-body).
                    ASTNode* trailing = create_ast_node(AST_CLOSURE, "trailing",
                                                         next_tok->line, next_tok->column);
                    trailing->node_type = create_type(TYPE_FUNCTION);
                    ASTNode* body = parse_block(parser);
                    add_child(trailing, body);
                    add_child(func_call, trailing);
                } else if (next_tok && next_tok->type == TOKEN_LEFT_BRACE &&
                           !parser->in_condition &&
                           next_tok->line > paren_close_line) {
                    // Common foot-gun (#286): user wrote
                    //     x = call()
                    //     {
                    //         ...
                    //     }
                    // and likely either (a) intended a trailing closure
                    // and put `{` on the wrong line, or (b) intended a
                    // separate bare-brace block. Under the same-line
                    // rule we keep the safe interpretation — leave the
                    // `{` for the statement parser, which will treat it
                    // as a bare block — and emit a hint so users in case
                    // (a) get pointed at the fix without having to debug
                    // an "Undefined variable" later.
                    AetherError w = {NULL, NULL, next_tok->line, next_tok->column,
                        "'{' on this line is parsed as a separate block, not as a trailing closure for the preceding call",
                        "move '{' to the same line as the closing ')' if you intended a trailing closure",
                        NULL, AETHER_ERR_NONE};
                    aether_warning_report(&w);
                    /* fall through — leave the `{` for the statement parser */
                }
            }

            // Free the original identifier node since we've copied its name
            if (expr) free_ast_node(expr);

            expr = func_call;
            continue;
        }

        // `!` is overloaded: actor fire-and-forget (`actor ! Message {...}`)
        // vs unwrap-or-trap (`tuple_call()!`). Disambiguate on the token
        // after `!`: a fire-and-forget is always followed by a message
        // *type* — an uppercase-leading identifier. Anything else (a
        // newline, a binary operator, `,`, `)`, EOF, a lowercase ident,
        // a literal …) is the unwrap suffix. Same lookahead style the
        // `?` actor-ask handler below uses to reject ternary misuse.
        if (op->type == TOKEN_EXCLAIM) {
            Token* after = peek_ahead(parser, 1);
            int is_fire_forget = (after && after->type == TOKEN_IDENTIFIER &&
                                  after->value && after->value[0] >= 'A' &&
                                  after->value[0] <= 'Z');
            if (!is_fire_forget) {
                // Unwrap-or-trap: yields the tuple's first slot, panics
                // if the trailing error slot is non-empty.
                advance_token(parser); // consume '!'
                ASTNode* unwrap = create_ast_node(AST_TUPLE_UNWRAP, NULL,
                                                  op->line, op->column);
                add_child(unwrap, expr);
                expr = unwrap;
                continue;
            }

            // Actor V2 - Fire-and-forget operator: actor ! Message { ... }
            advance_token(parser); // consume '!'

            ASTNode* message = parse_message_constructor(parser);
            if (!message) return NULL;

            ASTNode* send_op = create_ast_node(AST_SEND_FIRE_FORGET, NULL, op->line, op->column);
            add_child(send_op, expr);     // actor reference
            add_child(send_op, message);  // message to send
            expr = send_op;
            continue;
        }
        
        // Actor V2 - Ask operator: result = actor ? Message { ... }
        if (op->type == TOKEN_QUESTION) {
            // Guard against ternary-style usage (? is actor-ask, not ternary).
            // Heuristic: after '?', an actor-ask always names a message type
            // (uppercase identifier). If we see a lowercase identifier, a
            // literal, '(', or '-', it is almost certainly an attempted ternary.
            Token* after_q = peek_ahead(parser, 1); // token after '?'
            if (after_q && (
                    (after_q->type == TOKEN_IDENTIFIER && after_q->value &&
                     after_q->value[0] >= 'a' && after_q->value[0] <= 'z') ||
                    after_q->type == TOKEN_NUMBER     ||
                    after_q->type == TOKEN_LEFT_PAREN ||
                    after_q->type == TOKEN_MINUS      ||
                    after_q->type == TOKEN_STRING)) {
                parser_error(parser,
                    "unexpected `?` in expression: Aether does not have a ternary "
                    "operator - `?` is the actor ask operator (`actor ? Msg { ... }`); "
                    "use if/else blocks for conditional values");
                // Break out of the postfix loop; return expression parsed so far.
                break;
            }

            advance_token(parser); // consume '?'

            ASTNode* message = parse_message_constructor(parser);
            if (!message) return NULL;

            ASTNode* ask_op = create_ast_node(AST_SEND_ASK, NULL, op->line, op->column);
            add_child(ask_op, expr);     // actor reference
            add_child(ask_op, message);  // message to send
            expr = ask_op;
            continue;
        }
        
        break;
    }
    
    return expr;
}

ASTNode* parse_unary_expression(Parser* parser) {
    Token* operator = peek_token(parser);
    if (!operator) return NULL;
    
    if (operator->type == TOKEN_EXCLAIM || operator->type == TOKEN_MINUS ||
        operator->type == TOKEN_TILDE ||
        operator->type == TOKEN_INCREMENT || operator->type == TOKEN_DECREMENT) {
        advance_token(parser);
        ASTNode* operand = parse_unary_expression(parser);
        if (!operand) return NULL;
        return create_unary_expression(operand, operator);
    }

    // #890: prefix `&` is the address-of operator on an lvalue —
    // `&local.field`, `&(p as *T).field`, `&local`, `&arr[i]`. It lowers to
    // C's `&` and is typed as a pointer, so a C extern with a `&struct->field`
    // out-param can be called without raw `mem.long_to_ptr(base + OFFSET)`
    // offset math. A leading `&` here is unambiguous: a *binary* `&` (bitwise
    // AND) is consumed by the binary-expression parser, never reaching the
    // operand position. The operand is parsed at postfix precedence so member
    // access / cast / index bind tighter than `&` (matching C: `&p.b` is
    // `&(p.b)`).
    if (operator->type == TOKEN_AMPERSAND) {
        advance_token(parser);
        ASTNode* operand = parse_unary_expression(parser);
        if (!operand) return NULL;
        return create_unary_expression(operand, operator);
    }

    return parse_postfix_expression(parser);
}

int get_operator_precedence(AeTokenType type) {
    switch (type) {
        case TOKEN_ASSIGN: return 0;  // Lowest precedence (right-associative)
        case TOKEN_QUESTION_QUESTION: return 1;  // #340 `??` null-coalesce (loose, like ||)
        case TOKEN_OR: return 1;      // logical OR
        case TOKEN_AND: return 2;     // logical AND
        case TOKEN_PIPE: return 3;    // bitwise OR
        case TOKEN_CARET: return 4;   // bitwise XOR
        case TOKEN_AMPERSAND: return 5; // bitwise AND
        case TOKEN_EQUALS:
        case TOKEN_NOT_EQUALS: return 6;
        case TOKEN_LESS:
        case TOKEN_LESS_EQUAL:
        case TOKEN_GREATER:
        case TOKEN_GREATER_EQUAL:
        case TOKEN_IN: return 7;  // #1046 `member in bit_set` membership test.
                                  // A leading `IDENT in` in a for-header is
                                  // consumed by parse_for_loop before reaching
                                  // here, so range loops are unaffected.
        case TOKEN_LSHIFT:
        case TOKEN_RSHIFT: return 8;  // shift operators
        case TOKEN_PLUS:
        case TOKEN_MINUS: return 9;
        case TOKEN_MULTIPLY:
        case TOKEN_DIVIDE:
        case TOKEN_MODULO: return 10;
        case TOKEN_INCREMENT:
        case TOKEN_DECREMENT: return 11;
        default: return -1;  // Not an operator
    }
}

ASTNode* parse_statement(Parser* parser) {
    Token* token = peek_token(parser);
    if (!token) return NULL;

    // #893: labeled loop — `label: while ...` / `label: for ...`. A leading
    // identifier followed by `:` and a loop keyword is a loop label; the loop's
    // AST node carries the label name in its `value`, and `break label` /
    // `continue label` inside it target that loop. Disambiguated from a typed
    // declaration (`x: int = ...`) by requiring `while`/`for` after the colon —
    // a type name never appears there.
    if (token->type == TOKEN_IDENTIFIER) {
        Token* colon = peek_ahead(parser, 1);
        Token* kw = peek_ahead(parser, 2);
        if (colon && colon->type == TOKEN_COLON && kw &&
            (kw->type == TOKEN_WHILE || kw->type == TOKEN_FOR)) {
            const char* label = token->value;
            advance_token(parser);   // label identifier
            advance_token(parser);   // ':'
            ASTNode* loop = (kw->type == TOKEN_WHILE)
                          ? parse_while_loop(parser)
                          : parse_for_loop(parser);
            if (loop) {
                if (loop->value) free(loop->value);
                loop->value = label ? strdup(label) : NULL;
            }
            return loop;
        }
    }

    switch (token->type) {
        case TOKEN_AT: {
            // Statement-level binding annotation (#521): `@scoped let buf = ...`
            // (the `let`/`var` keyword is optional). Marks a local whose value
            // must not outlive its lexical block — the typechecker rejects the
            // escape patterns (return / store into another binding or field /
            // closure capture / container insert). The only statement
            // annotation today is `@scoped`.
            advance_token(parser); // consume '@'
            Token* attr = peek_token(parser);
            if (!attr || attr->type != TOKEN_IDENTIFIER || !attr->value ||
                strcmp(attr->value, "scoped") != 0) {
                parser_error(parser, "unknown statement annotation, only `@scoped` is supported on a binding");
                return NULL;
            }
            advance_token(parser); // consume 'scoped'
            if (peek_token(parser) && (peek_token(parser)->type == TOKEN_LET ||
                                       peek_token(parser)->type == TOKEN_VAR)) {
                advance_token(parser); // optional let/var
            }
            ASTNode* decl = parse_python_style_declaration(parser);
            if (decl && decl->type == AST_VARIABLE_DECLARATION) {
                if (decl->annotation) free(decl->annotation);
                decl->annotation = strdup("scoped");
            } else if (decl) {
                parser_error(parser, "`@scoped` applies to a single `let`/`var` binding, not this form");
                return NULL;
            }
            return decl;
        }
        case TOKEN_LET:
        case TOKEN_VAR:
            // Optional 'let' or 'var' - skip it and parse as Python-style
            advance_token(parser);
            return parse_python_style_declaration(parser);

        case TOKEN_CONST: {
            // Local constant: const x = 5 or const arr[] = [1, 2, 3]
            int cline = token->line, ccol = token->column;
            advance_token(parser); // consume 'const'
            Token* cname = expect_token(parser, TOKEN_IDENTIFIER);
            if (!cname) return NULL;

            // Check for array form: const NAME[] = [...]
            int is_array = 0;
            if (peek_token(parser) && peek_token(parser)->type == TOKEN_LEFT_BRACKET) {
                advance_token(parser); // consume '['
                if (!expect_token(parser, TOKEN_RIGHT_BRACKET)) return NULL;
                is_array = 1;
            }

            if (!expect_token(parser, TOKEN_ASSIGN)) return NULL;
            ASTNode* cval = parse_expression(parser);
            if (!cval) return NULL;
            match_token(parser, TOKEN_SEMICOLON);
            ASTNode* node = create_ast_node(AST_CONST_DECLARATION, cname->value, cline, ccol);
            add_child(node, cval);

            if (is_array) {
                node->annotation = strdup("array_const");
                // Infer element type from first child of array literal
                Type* elem_type = NULL;
                if (cval->node_type == NULL && cval->child_count > 0 && cval->children[0]) {
                    if (cval->children[0]->node_type) {
                        elem_type = cval->children[0]->node_type;
                    }
                } else if (cval->node_type && cval->node_type->element_type) {
                    elem_type = cval->node_type->element_type;
                }
                if (elem_type) {
                    node->node_type = create_array_type(clone_type(elem_type), cval->child_count);
                } else {
                    node->node_type = create_array_type(create_type(TYPE_PTR), cval->child_count);
                }
            } else {
                if (cval->node_type) {
                    node->node_type = clone_type(cval->node_type);
                } else {
                    node->node_type = create_type(TYPE_UNKNOWN);
                }
            }
            return node;
        }

        case TOKEN_HIDE: {
            // Scope-level directive: hide name1, name2, ...
            // Position within block doesn't matter — typechecker collects all
            // hide directives in a scope before resolving any other names.
            int hline = token->line, hcol = token->column;
            advance_token(parser); // consume 'hide'
            ASTNode* node = create_ast_node(AST_HIDE_DIRECTIVE, NULL, hline, hcol);
            for (;;) {
                Token* hname = expect_token(parser, TOKEN_IDENTIFIER);
                if (!hname) return NULL;
                ASTNode* id = create_ast_node(AST_IDENTIFIER, hname->value, hname->line, hname->column);
                add_child(node, id);
                if (!match_token(parser, TOKEN_COMMA)) break;
            }
            match_token(parser, TOKEN_SEMICOLON);
            return node;
        }

        case TOKEN_SEAL: {
            // Scope-level directive: seal except name1, name2, ...
            // Hides every outer binding except those listed in the whitelist.
            int sline = token->line, scol = token->column;
            advance_token(parser); // consume 'seal'
            if (!expect_token(parser, TOKEN_EXCEPT)) return NULL;
            ASTNode* node = create_ast_node(AST_SEAL_DIRECTIVE, NULL, sline, scol);
            for (;;) {
                Token* sname = expect_token(parser, TOKEN_IDENTIFIER);
                if (!sname) return NULL;
                ASTNode* id = create_ast_node(AST_IDENTIFIER, sname->value, sname->line, sname->column);
                add_child(node, id);
                if (!match_token(parser, TOKEN_COMMA)) break;
            }
            match_token(parser, TOKEN_SEMICOLON);
            return node;
        }
            
        case TOKEN_INT:
        case TOKEN_INT64:
        case TOKEN_UINT64:
        case TOKEN_DURATION:
        case TOKEN_STRING:
        case TOKEN_FLOAT:
        case TOKEN_BOOL:
        case TOKEN_BYTE: {
            // Check if this is a namespace call: string.func() vs type declaration: string x = ...
            Token* next = peek_ahead(parser, 1);
            if (next && next->type == TOKEN_DOT) {
                // Namespace call like string.release(s) - parse as expression statement
                ASTNode* expr = parse_expression(parser);
                if (expr) {
                    match_token(parser, TOKEN_SEMICOLON);
                    ASTNode* stmt = create_ast_node(AST_EXPRESSION_STATEMENT, NULL, token->line, token->column);
                    add_child(stmt, expr);
                    return stmt;
                }
                return NULL;
            }
            // Explicit type declaration: int x = 42;  byte b = 0x7F;
            return parse_variable_declaration(parser);
        }

        case TOKEN_MULTIPLY: {
            /* `*StructName name = expr` — a typed pointer-to-struct
             * local declaration.  Disambiguated from a deref-store
             * (`*p = v`) by the shape `* IDENT IDENT`: a struct name
             * followed by a variable name.  Used so an FFI handle
             * (`*client c = ...`, `*JSContext ctx = ...`) carries its
             * pointee identity through the type system, exactly like
             * the param/return positions already do. See
             * redis-porting-language-gaps.md "P0: Typed And Qualified
             * C Pointers". */
            Token* t1 = peek_ahead(parser, 1);  // the struct name
            Token* t2 = peek_ahead(parser, 2);  // the variable name
            if (t1 && t1->type == TOKEN_IDENTIFIER &&
                t2 && t2->type == TOKEN_IDENTIFIER) {
                return parse_variable_declaration(parser);
            }
            // Otherwise it's an expression statement (`*p = v`, etc.).
            ASTNode* expr = parse_expression(parser);
            if (!expr) return NULL;
            match_token(parser, TOKEN_SEMICOLON);
            ASTNode* es = create_ast_node(AST_EXPRESSION_STATEMENT, NULL,
                                          token->line, token->column);
            add_child(es, expr);
            return es;
        }

        case TOKEN_IF:
            return parse_if_statement(parser);

        case TOKEN_WHEN: {
            // Compile-time `when` / static-if (#483). The guard use of
            // `when` only appears after a clause's parameter list inside
            // parse_function_definition, never at statement head, so this
            // is unambiguous. Statement-level arms hold statements.
            int saved = parser->when_top_level;
            parser->when_top_level = 0;
            ASTNode* w = parse_when_statement(parser);
            parser->when_top_level = saved;
            return w;
        }

        case TOKEN_FOR:
            return parse_for_loop(parser);
            
        case TOKEN_WHILE:
            return parse_while_loop(parser);
            
        case TOKEN_SWITCH:
            return parse_switch_statement(parser);
            
        case TOKEN_MATCH:
            return parse_match_statement(parser);
            
        case TOKEN_RETURN:
            return parse_return_statement(parser);
            
        case TOKEN_REPLY:
            return parse_reply_statement(parser);
            
        case TOKEN_BREAK:
        case TOKEN_CONTINUE: {
            // #893: optional loop label — `break label` / `continue label`.
            // The label must be on the SAME line as the keyword so a bare
            // `break` followed by an identifier-headed statement on the next
            // line is not mis-read as a labeled break.
            AeTokenType kind = token->type;
            int kline = token->line, kcol = token->column;
            advance_token(parser);
            const char* label = NULL;
            Token* nxt = peek_token(parser);
            if (nxt && nxt->type == TOKEN_IDENTIFIER && nxt->line == kline) {
                label = nxt->value;
                advance_token(parser);
            }
            match_token(parser, TOKEN_SEMICOLON);
            return create_ast_node(
                kind == TOKEN_BREAK ? AST_BREAK_STATEMENT : AST_CONTINUE_STATEMENT,
                label, kline, kcol);
        }
            
        case TOKEN_DEFER:
            return parse_defer_statement(parser);

        case TOKEN_TRY:
            return parse_try_statement(parser);

        case TOKEN_PANIC:
            return parse_panic_statement(parser);

        case TOKEN_PRINT:
            return parse_print_statement(parser);
            
        case TOKEN_SEND:
            return parse_send_statement(parser);
            
        case TOKEN_SPAWN_ACTOR:
            return parse_spawn_actor_statement(parser);
            
        case TOKEN_LEFT_BRACE:
            return parse_block(parser);
            
        case TOKEN_STATE:
            // Outside actor bodies, 'state' is a regular identifier
            // fall through
        case TOKEN_FUNC:
        case TOKEN_AFTER:
            // #880: `func`/`after` have no statement-head role (and are not
            // type keywords), so at the start of a statement they are a value
            // identifier — a local binding (`func = compute()`), an
            // assignment / compound-assign target, or the head of an
            // expression statement. Routed through the identifier path below.
            // (`ptr`/`byte` are type keywords: bare `byte b = ...` stays a
            // typed declaration; the value-name form is `let byte = ...`.)
        case TOKEN_IDENTIFIER: {
            // Check if this is: identifier = expression (Python-style)
            // or tuple destructuring: identifier, identifier = expression
            Token* next = peek_ahead(parser, 1);
            // C-style typed local declaration — two adjacent identifiers
            // (`IDENT IDENT`) at statement start:
            //   `size_t n = ...`      (C ABI alias type, #...)
            //   `Pair p`              (#746: stack-allocated struct local,
            //                          the value sibling of `*Pair p`)
            //   `GeoHashRadius r = make(...)`
            // No other construct has the `IDENT IDENT` shape — exactly the
            // disambiguation the `*StructName name` pointer path already
            // relies on (member access uses `.`, calls use `(`). parse_type
            // lowers a bare struct-name identifier to TYPE_STRUCT, and the
            // declaration may omit the initializer (`Pair p`).
            if (next && next->type == TOKEN_IDENTIFIER) {
                return parse_variable_declaration(parser);
            }
            // #946: C-style typed local with a QUALIFIED type name —
            // `mod.Type name [= expr]` (the dotted analogue of `Type name`
            // above). Shape: IDENT (`.` IDENT)+ IDENT. Disambiguated from a
            // member-access expression statement (`a.b.c`) by the trailing
            // binding identifier — a member chain ends at the last `.field`,
            // never `... field NAME`. parse_variable_declaration → parse_type
            // consumes the dotted type, then the binding name.
            if (next && next->type == TOKEN_DOT) {
                int off = 1;
                /* walk the `.IDENT` chain */
                while (peek_ahead(parser, off) &&
                       peek_ahead(parser, off)->type == TOKEN_DOT &&
                       peek_ahead(parser, off + 1) &&
                       peek_ahead(parser, off + 1)->type == TOKEN_IDENTIFIER) {
                    off += 2;
                }
                /* `off` is past the dotted type name; a binding identifier
                 * here (and not another `.`/`(`) marks a typed declaration. */
                Token* after = peek_ahead(parser, off);
                if (off > 1 && after && after->type == TOKEN_IDENTIFIER) {
                    return parse_variable_declaration(parser);
                }
            }
            if (next && (next->type == TOKEN_ASSIGN || next->type == TOKEN_COMMA)) {
                return parse_python_style_declaration(parser);
            }
            // Check for compound assignment: identifier op= expression
            if (next && (next->type == TOKEN_PLUS_ASSIGN || next->type == TOKEN_MINUS_ASSIGN ||
                         next->type == TOKEN_MULTIPLY_ASSIGN || next->type == TOKEN_DIVIDE_ASSIGN ||
                         next->type == TOKEN_MODULO_ASSIGN || next->type == TOKEN_AND_ASSIGN ||
                         next->type == TOKEN_OR_ASSIGN || next->type == TOKEN_XOR_ASSIGN ||
                         next->type == TOKEN_LSHIFT_ASSIGN || next->type == TOKEN_RSHIFT_ASSIGN)) {
                // Consume identifier
                Token* name = peek_token(parser);
                if (!token_is_value_ident(name)) {
                    parser_error(parser, "Expected identifier");
                    return NULL;
                }
                advance_token(parser);
                // Consume the compound assignment operator
                Token* op = advance_token(parser);
                // Parse RHS expression
                ASTNode* rhs = parse_expression(parser);
                if (!rhs) return NULL;
                // Create AST_COMPOUND_ASSIGNMENT: value = operator string, child[0] = RHS
                ASTNode* node = create_ast_node(AST_COMPOUND_ASSIGNMENT, name->value, name->line, name->column);
                node->node_type = create_type(TYPE_UNKNOWN);
                // Store operator in a child node so codegen knows which op
                ASTNode* op_node = create_ast_node(AST_LITERAL, op->value, op->line, op->column);
                add_child(node, op_node);
                add_child(node, rhs);
                match_token(parser, TOKEN_SEMICOLON);
                return node;
            }
            // Otherwise fall through to expression statement
            ASTNode* expr = parse_expression(parser);
            if (expr) {
                match_token(parser, TOKEN_SEMICOLON);
                ASTNode* stmt = create_ast_node(AST_EXPRESSION_STATEMENT, NULL, token->line, token->column);
                add_child(stmt, expr);
                return stmt;
            }
            return NULL;
        }
            
        default: {
            ASTNode* expr = parse_expression(parser);
            if (expr) {
                match_token(parser, TOKEN_SEMICOLON);
                ASTNode* stmt = create_ast_node(AST_EXPRESSION_STATEMENT, NULL, token->line, token->column);
                add_child(stmt, expr);
                return stmt;
            }
            return NULL;
        }
    }
}

ASTNode* parse_variable_declaration(Parser* parser) {
    return parse_variable_declaration_with_semicolon(parser, true);
}

ASTNode* parse_variable_declaration_with_semicolon(Parser* parser, bool expect_semicolon) {
    // Token is already positioned at type token (int, string, etc.)
    Type* type = parse_type(parser);  // parse_type will advance past type
    Token* name = expect_token(parser, TOKEN_IDENTIFIER);
    if (!name) return NULL;
    
    ASTNode* decl = create_ast_node(AST_VARIABLE_DECLARATION, name->value, name->line, name->column);
    decl->node_type = type;
    
    if (match_token(parser, TOKEN_ASSIGN)) {
        ASTNode* value = parse_expression(parser);
        if (value) {
            add_child(decl, value);
        }
    }
    
    if (expect_semicolon) {
        match_token(parser, TOKEN_SEMICOLON);
    }
    return decl;
}

// Python-style variable declaration: x = 42 (no 'let', type inferred)
ASTNode* parse_python_style_declaration(Parser* parser) {
    // Accept a value-identifier token as the binding name — TOKEN_IDENTIFIER,
    // `state` (a regular identifier outside actors), or the #880 keyword set.
    Token* name = peek_token(parser);
    if (!token_is_value_ident(name)) {
        parser_error(parser, "Expected identifier");
        return NULL;
    }
    advance_token(parser);

    // Check for tuple destructuring: a, b = func()
    Token* after_name = peek_token(parser);
    if (after_name && after_name->type == TOKEN_COMMA) {
        // Tuple destructuring mode
        ASTNode* destructure = create_ast_node(AST_TUPLE_DESTRUCTURE, NULL, name->line, name->column);

        // First lvalue
        ASTNode* first = create_ast_node(AST_VARIABLE_DECLARATION, name->value, name->line, name->column);
        first->node_type = create_type(TYPE_UNKNOWN);
        add_child(destructure, first);

        // Parse remaining lvalues
        while (match_token(parser, TOKEN_COMMA)) {
            Token* next_name = peek_token(parser);
            if (!next_name) break;

            if (next_name->type == TOKEN_IDENTIFIER && strcmp(next_name->value, "_") == 0) {
                // Discard: _ — create a placeholder
                advance_token(parser);
                ASTNode* discard = create_ast_node(AST_VARIABLE_DECLARATION, "_", next_name->line, next_name->column);
                discard->node_type = create_type(TYPE_UNKNOWN);
                add_child(destructure, discard);
            } else if (token_is_value_ident(next_name)) {
                advance_token(parser);
                ASTNode* var = create_ast_node(AST_VARIABLE_DECLARATION, next_name->value, next_name->line, next_name->column);
                var->node_type = create_type(TYPE_UNKNOWN);
                add_child(destructure, var);
            } else {
                parser_error(parser, "Expected identifier in tuple destructuring");
                break;
            }
        }

        if (!expect_token(parser, TOKEN_ASSIGN)) {
            free_ast_node(destructure);
            return NULL;
        }

        // Parse RHS expression
        ASTNode* rhs = parse_expression(parser);
        if (rhs) {
            add_child(destructure, rhs);  // Last child is the RHS
        }

        match_token(parser, TOKEN_SEMICOLON);
        return destructure;
    }

    // Single variable declaration (existing path)
    ASTNode* decl = create_ast_node(AST_VARIABLE_DECLARATION, name->value, name->line, name->column);
    decl->node_type = create_type(TYPE_UNKNOWN);
    decl->type_inferred = 1;  /* #698: bare `x = expr` — type inferred, no
                               * explicit annotation. Survives the pre-
                               * typecheck pass that fills node_type. */

    // Optional `: type` annotation — `let x: int = 5`, `let m: int? = none`.
    // (#340 needs the optional form; the colon annotation is general.)
    if (peek_token(parser) && peek_token(parser)->type == TOKEN_COLON) {
        advance_token(parser);   // consume ':'
        Type* annot = parse_type(parser);
        if (annot) {
            if (decl->node_type) free_type(decl->node_type);
            decl->node_type = annot;
            decl->type_inferred = 0;   // explicit annotation, not inferred
        }
    }

    if (match_token(parser, TOKEN_ASSIGN)) {
        // Check for match-as-expression: x = match val { ... }
        Token* next_tok = peek_token(parser);
        if (next_tok && next_tok->type == TOKEN_MATCH) {
            ASTNode* match_node = parse_match_statement(parser);
            if (match_node) {
                add_child(decl, match_node);
            }
        } else {
            ASTNode* value = parse_expression(parser);
            if (value) {
                add_child(decl, value);
            }
        }
    }

    match_token(parser, TOKEN_SEMICOLON);
    return decl;
}

// Compile-time `when` / static-if (issue #483).
//
//   when target.os == "windows" { ... } else { ... }
//
// `when` already exists as TOKEN_WHEN for function-clause guards
// (`fib(n) when n > 1 -> ...`), but that use only appears AFTER a
// parameter list inside parse_function_definition — never at the head
// of a statement or top-level declaration. So a `when` reached from
// parse_statement / parse_top_level_decl is unambiguously the static-if
// form; the guard parse is untouched.
//
// The condition is evaluated at compile time by a pre-typecheck pass
// (resolve_when_statements, in aetherc.c) which prunes the AST down to
// the selected arm BEFORE type-checking or codegen run on it. The
// UNSELECTED arm parses but is never type-checked or emitted — that is
// what lets a platform-specific extern be gated cleanly.
//
// AST layout mirrors AST_IF_STATEMENT:
//   children[0] = condition expression
//   children[1] = then-arm  (an AST_BLOCK of statements/decls)
//   children[2] = else-arm  (optional AST_BLOCK), present iff `else` given
//
// An arm body is a brace-delimited list. Its items are parsed with the
// same grammar the surrounding context would use: a top-level-only
// construct (extern / import / struct / func / ...) goes through
// parse_top_level_decl, everything else through parse_statement. This
// lets a single `when` form serve both statement bodies and top-level
// declaration groups (including a `when` nested inside another `when`).

// Parse one `{ ... }` arm of a `when`. At top level the items are
// declarations (extern / import / func / struct / ...) parsed via
// parse_top_level_decl, so a platform-gated extern parses identically to a
// bare top-level one. At statement level the items are ordinary statements.
// Returns an AST_BLOCK whose children are the arm items.
static ASTNode* parse_when_arm(Parser* parser) {
    if (!expect_token(parser, TOKEN_LEFT_BRACE)) return NULL;
    int top_level = parser->when_top_level;
    ASTNode* arm = create_ast_node(AST_BLOCK, NULL, 0, 0);
    while (!match_token(parser, TOKEN_RIGHT_BRACE)) {
        if (is_at_end(parser)) break;
        int start_token = parser->current_token;
        ASTNode* item = top_level
            ? parse_top_level_decl(parser)
            : parse_statement(parser);
        if (item) {
            add_child(arm, item);
        } else if (parser->current_token == start_token) {
            // Guarantee forward progress on a malformed item.
            parser_error(parser, "Expected statement or declaration in `when` arm");
            advance_token(parser);
        }
    }
    return arm;
}

ASTNode* parse_when_statement(Parser* parser) {
    Token* when_tok = advance_token(parser);  // consume `when`

    // Parse the compile-time condition. Reuse the `in_condition` guard so a
    // trailing `{` is read as the arm body, not a closure on the condition.
    int saved_in_condition = parser->in_condition;
    parser->in_condition = 1;
    ASTNode* condition = parse_expression(parser);
    parser->in_condition = saved_in_condition;
    if (!condition) return NULL;

    ASTNode* then_arm = parse_when_arm(parser);
    if (!then_arm) return NULL;

    ASTNode* when_stmt = create_ast_node(AST_WHEN_STATEMENT, NULL,
                                         when_tok ? when_tok->line : 0,
                                         when_tok ? when_tok->column : 0);
    add_child(when_stmt, condition);
    add_child(when_stmt, then_arm);

    if (match_token(parser, TOKEN_ELSE)) {
        // `else when ...` chains: an else arm that is itself a `when`.
        if (peek_token(parser) && peek_token(parser)->type == TOKEN_WHEN) {
            ASTNode* else_when = parse_when_statement(parser);
            if (else_when) {
                ASTNode* wrap = create_ast_node(AST_BLOCK, NULL, 0, 0);
                add_child(wrap, else_when);
                add_child(when_stmt, wrap);
            }
        } else {
            ASTNode* else_arm = parse_when_arm(parser);
            if (else_arm) {
                add_child(when_stmt, else_arm);
            }
        }
    }

    return when_stmt;
}

ASTNode* parse_if_statement(Parser* parser) {
    advance_token(parser); // if
    int saved_in_condition = parser->in_condition;
    parser->in_condition = 1;
    ASTNode* condition = parse_expression(parser);
    parser->in_condition = saved_in_condition;
    if (!condition) return NULL;
    
    ASTNode* then_branch = parse_statement(parser);
    if (!then_branch) return NULL;
    
    ASTNode* if_stmt = create_ast_node(AST_IF_STATEMENT, NULL, 0, 0);
    add_child(if_stmt, condition);
    add_child(if_stmt, then_branch);
    
    if (match_token(parser, TOKEN_ELSE)) {
        ASTNode* else_branch = parse_statement(parser);
        if (else_branch) {
            add_child(if_stmt, else_branch);
        }
    }
    
    return if_stmt;
}

ASTNode* parse_for_loop(Parser* parser) {
    advance_token(parser); // for

    // Check for range-based for: for IDENT in EXPR..EXPR { body }
    Token* first = peek_token(parser);
    Token* second = peek_ahead(parser, 1);
    if (first && (first->type == TOKEN_IDENTIFIER || first->type == TOKEN_STATE) &&
        second && second->type == TOKEN_IN) {
        // Range-based for loop
        Token* var_name = advance_token(parser); // consume identifier
        advance_token(parser); // consume 'in'
        ASTNode* start_expr = parse_expression(parser);
        if (!start_expr) return NULL;
        if (!expect_token(parser, TOKEN_DOTDOT)) return NULL;
        // end_expr is terminated by `{` (the loop body) — the same
        // trailing-block ambiguity if/while have. See parse_if_statement
        // for the rationale.
        int saved_in_condition = parser->in_condition;
        parser->in_condition = 1;
        ASTNode* end_expr = parse_expression(parser);
        parser->in_condition = saved_in_condition;
        if (!end_expr) return NULL;

        ASTNode* body = parse_statement(parser);
        if (!body) return NULL;

        // Desugar: for i in start..end { body }
        //       → for (i = start; i < end; i++) { body }
        ASTNode* init = create_ast_node(AST_VARIABLE_DECLARATION, var_name->value, var_name->line, var_name->column);
        init->node_type = create_type(TYPE_UNKNOWN);
        add_child(init, start_expr);

        // Condition: i < end
        Token cond_op = { .type = TOKEN_LESS, .value = "<", .line = var_name->line, .column = var_name->column };
        ASTNode* cond_left = create_ast_node(AST_IDENTIFIER, var_name->value, var_name->line, var_name->column);
        ASTNode* condition = create_binary_expression(cond_left, end_expr, &cond_op);

        // Increment: i++
        Token inc_op = { .type = TOKEN_INCREMENT, .value = "++", .line = var_name->line, .column = var_name->column };
        ASTNode* inc_target = create_ast_node(AST_IDENTIFIER, var_name->value, var_name->line, var_name->column);
        ASTNode* increment = create_unary_expression(inc_target, &inc_op);

        ASTNode* for_loop = create_ast_node(AST_FOR_LOOP, NULL, var_name->line, var_name->column);
        for_loop->children = malloc(4 * sizeof(ASTNode*));
    for_loop->child_capacity = 0;
        if (!for_loop->children) { free_ast_node(for_loop); return NULL; }
        for_loop->child_count = 4;
        for_loop->children[0] = init;
        for_loop->children[1] = condition;
        for_loop->children[2] = increment;
        for_loop->children[3] = body;
        return for_loop;
    }

    if (!expect_token(parser, TOKEN_LEFT_PAREN)) return NULL;

    ASTNode* init = NULL;
    Token* token = peek_token(parser);

    // Check if init is a variable declaration (int i = 1) or expression (i = 1)
    if (token && (token->type == TOKEN_INT || token->type == TOKEN_STRING ||
                  token->type == TOKEN_FLOAT || token->type == TOKEN_BOOL ||
                  token->type == TOKEN_BYTE)) {
        init = parse_variable_declaration_with_semicolon(parser, false);
        match_token(parser, TOKEN_SEMICOLON);
    } else if (token && token->type == TOKEN_IDENTIFIER) {
        // Check for Python-style: i = 0 (treat as variable declaration)
        Token* next = peek_ahead(parser, 1);
        if (next && next->type == TOKEN_ASSIGN) {
            // Parse as variable declaration without consuming semicolon
            Token* name = expect_token(parser, TOKEN_IDENTIFIER);
            if (!name) return NULL;
            init = create_ast_node(AST_VARIABLE_DECLARATION, name->value, name->line, name->column);
            init->node_type = create_type(TYPE_UNKNOWN);
            if (match_token(parser, TOKEN_ASSIGN)) {
                ASTNode* value = parse_expression(parser);
                if (value) {
                    add_child(init, value);
                }
            }
        } else {
            init = parse_expression(parser);
        }
        match_token(parser, TOKEN_SEMICOLON);
    } else if (!match_token(parser, TOKEN_SEMICOLON)) {
        init = parse_expression(parser);
        match_token(parser, TOKEN_SEMICOLON);
    }
    
    ASTNode* condition = NULL;
    if (!match_token(parser, TOKEN_SEMICOLON)) {
        condition = parse_expression(parser);
        match_token(parser, TOKEN_SEMICOLON);
    }
    
    ASTNode* increment = NULL;
    if (!match_token(parser, TOKEN_RIGHT_PAREN)) {
        increment = parse_expression(parser);
        expect_token(parser, TOKEN_RIGHT_PAREN);
    }
    
    ASTNode* body = parse_statement(parser);
    if (!body) return NULL;
    
    ASTNode* for_loop = create_ast_node(AST_FOR_LOOP, NULL, 0, 0);
    // Reserve 4 slots for init, condition, increment, body
    for_loop->children = malloc(4 * sizeof(ASTNode*));
    for_loop->child_capacity = 0;
    if (!for_loop->children) { free_ast_node(for_loop); return NULL; }
    for_loop->child_count = 4;
    for_loop->children[0] = init;
    for_loop->children[1] = condition;
    for_loop->children[2] = increment;
    for_loop->children[3] = body;

    return for_loop;
}

ASTNode* parse_while_loop(Parser* parser) {
    advance_token(parser); // while
    int saved_in_condition = parser->in_condition;
    parser->in_condition = 1;
    ASTNode* condition = parse_expression(parser);
    parser->in_condition = saved_in_condition;
    if (!condition) return NULL;
    
    ASTNode* body = parse_statement(parser);
    if (!body) return NULL;
    
    ASTNode* while_loop = create_ast_node(AST_WHILE_LOOP, NULL, 0, 0);
    add_child(while_loop, condition);
    add_child(while_loop, body);
    
    return while_loop;
}

ASTNode* parse_switch_statement(Parser* parser) {
    advance_token(parser);
    ASTNode* expression = parse_expression(parser);
    if (!expression) return NULL;
    
    expect_token(parser, TOKEN_LEFT_BRACE);
    
    ASTNode* switch_stmt = create_ast_node(AST_SWITCH_STATEMENT, NULL, 0, 0);
    add_child(switch_stmt, expression);
    
    int iteration_count = 0;
    const int MAX_CASES = 1000;
    
    while (!match_token(parser, TOKEN_RIGHT_BRACE) && !is_at_end(parser)) {
        if (++iteration_count > MAX_CASES) {
            parser_message(parser, "Error: Too many cases in switch statement (max 100)");
            return switch_stmt;
        }
        
        ASTNode* case_stmt = parse_case_statement(parser);
        if (case_stmt) {
            add_child(switch_stmt, case_stmt);
        } else {
            parser_error(parser, "Expected 'case' or 'default' in switch statement");
            advance_token(parser);
        }
    }
    
    return switch_stmt;
}

// #1047: parse one match/switch case selector element: a single value, or a
// range `lo..=hi` (inclusive) / `lo..<hi` (half-open). Returns the bare value
// expression, or an AST_MATCH_RANGE with children [lo, hi] and annotation
// "inclusive"/"halfopen".
static ASTNode* parse_one_selector(Parser* parser) {
    ASTNode* lo = parse_expression(parser);
    if (!lo) return NULL;
    Token* t = peek_token(parser);
    if (t && (t->type == TOKEN_DOTDOT_EQ || t->type == TOKEN_DOTDOT_LT)) {
        int inclusive = (t->type == TOKEN_DOTDOT_EQ);
        advance_token(parser);  // consume ..= / ..<
        ASTNode* hi = parse_expression(parser);
        if (!hi) { free_ast_node(lo); return NULL; }
        ASTNode* range = create_ast_node(AST_MATCH_RANGE, NULL, lo->line, lo->column);
        range->annotation = strdup(inclusive ? "inclusive" : "halfopen");
        add_child(range, lo);
        add_child(range, hi);
        return range;
    }
    return lo;
}

// #1047: parse a full case selector: one element, or a comma-list of elements
// (`1, 2, 5..=9`). A single element returns bare (backward compatible); a list
// returns an AST_MATCH_ALT whose children are the elements. Stops at the arm
// terminator (`->` / `:`), so the arm-separator comma is untouched.
static ASTNode* parse_case_selector(Parser* parser) {
    ASTNode* first = parse_one_selector(parser);
    if (!first) return NULL;
    if (!peek_token(parser) || peek_token(parser)->type != TOKEN_COMMA) {
        return first;  // single value / single range
    }
    ASTNode* alt = create_ast_node(AST_MATCH_ALT, NULL, first->line, first->column);
    add_child(alt, first);
    while (peek_token(parser) && peek_token(parser)->type == TOKEN_COMMA) {
        advance_token(parser);  // consume ','
        ASTNode* nxt = parse_one_selector(parser);
        if (!nxt) { free_ast_node(alt); return NULL; }
        add_child(alt, nxt);
    }
    return alt;
}

ASTNode* parse_case_statement(Parser* parser) {
    if (match_token(parser, TOKEN_DEFAULT)) {
        if (!expect_token(parser, TOKEN_COLON)) return NULL;

        ASTNode* case_stmt = create_ast_node(AST_CASE_STATEMENT, "default", 0, 0);
        
        int iteration_count = 0;
        const int MAX_CASE_STMTS = 1000;
        
        while (!is_at_end(parser)) {
            if (++iteration_count > MAX_CASE_STMTS) {
                parser_message(parser, "Error: Too many statements in case block (max 1000)");
                break;
            }
            
            Token* next = peek_token(parser);
            if (!next || next->type == TOKEN_CASE || next->type == TOKEN_DEFAULT || next->type == TOKEN_RIGHT_BRACE) {
                break;
            }
            ASTNode* stmt = parse_statement(parser);
            if (stmt) {
                add_child(case_stmt, stmt);
            } else {
                advance_token(parser);
            }
        }
        return case_stmt;
    }
    
    if (match_token(parser, TOKEN_CASE)) {
        ASTNode* value = parse_case_selector(parser);  // #1047: value / range / comma-list
        if (!value) return NULL;
        if (!expect_token(parser, TOKEN_COLON)) return NULL;
        
        ASTNode* case_stmt = create_ast_node(AST_CASE_STATEMENT, NULL, 0, 0);
        add_child(case_stmt, value);
        
        int iteration_count = 0;
        const int MAX_CASE_STMTS = 1000;
        
        while (!is_at_end(parser)) {
            if (++iteration_count > MAX_CASE_STMTS) {
                parser_message(parser, "Error: Too many statements in case block (max 1000)");
                break;
            }
            
            Token* next = peek_token(parser);
            if (!next || next->type == TOKEN_CASE || next->type == TOKEN_DEFAULT || next->type == TOKEN_RIGHT_BRACE) {
                break;
            }
            ASTNode* stmt = parse_statement(parser);
            if (stmt) {
                add_child(case_stmt, stmt);
            } else {
                advance_token(parser);
            }
        }
        return case_stmt;
    }
    
    return NULL;
}

// Parse match statement (pattern matching)
// Syntax:
//   match (expr) {
//     pattern => expression
//     pattern => { statements }
//     _ => default_case
//   }
ASTNode* parse_match_statement(Parser* parser) {
    advance_token(parser); // consume 'match'

    // Parse the expression to match on (parens optional). When there are
    // no parens, the `{` that follows introduces the match arms — the same
    // trailing-block ambiguity if/while have. Guarding the condition flag
    // keeps `match f(x) { ... }` from eating the arms as a closure on f.
    int has_paren = match_token(parser, TOKEN_LEFT_PAREN);
    int saved_in_condition = parser->in_condition;
    if (!has_paren) parser->in_condition = 1;
    ASTNode* expression = parse_expression(parser);
    parser->in_condition = saved_in_condition;
    if (!expression) return NULL;
    if (has_paren && !expect_token(parser, TOKEN_RIGHT_PAREN)) return NULL;

    if (!expect_token(parser, TOKEN_LEFT_BRACE)) return NULL;
    
    ASTNode* match_stmt = create_ast_node(AST_MATCH_STATEMENT, NULL, 0, 0);
    add_child(match_stmt, expression);
    
    int iteration_count = 0;
    const int MAX_CASES = 1000;
    
    // Parse match arms
    while (!match_token(parser, TOKEN_RIGHT_BRACE) && !is_at_end(parser)) {
        if (++iteration_count > MAX_CASES) {
            parser_message(parser, "Error: Too many match arms (max 1000)");
            return match_stmt;
        }
        
        ASTNode* match_arm = parse_match_case(parser);
        if (match_arm) {
            add_child(match_stmt, match_arm);
        } else {
            parser_message(parser, "Parse error: Expected match arm in match statement");
            advance_token(parser);
        }
    }
    
    return match_stmt;
}

// Parse a single match arm
// pattern => expression
// pattern => { block }
ASTNode* parse_match_case(Parser* parser) {
    Token* current = peek_token(parser);
    if (!current) return NULL;

    // Parse pattern: wildcard, list pattern, or expression
    ASTNode* pattern = NULL;

    if (current->type == TOKEN_IDENTIFIER && strcmp(current->value, "_") == 0) {
        // Wildcard pattern
        advance_token(parser);
        pattern = create_ast_node(AST_LITERAL, "_", current->line, current->column);
        pattern->node_type = create_type(TYPE_WILDCARD);
    } else if (current->type == TOKEN_LEFT_BRACKET) {
        // List pattern: [], [x], [x, y], [h|t]
        pattern = parse_pattern(parser);
        if (!pattern) return NULL;
    } else if (current->type == TOKEN_IDENTIFIER && current->value &&
               strcmp(current->value, "some") == 0 &&
               peek_ahead(parser, 1) &&
               peek_ahead(parser, 1)->type == TOKEN_LEFT_PAREN) {
        // #340: optional `some(binding)` arm — binds the unwrapped value.
        // Modeled as a variable pattern annotated "some_pattern"; the
        // matched `none` arm is the AST_NONE_LITERAL from the branch below.
        advance_token(parser);  // 'some'
        advance_token(parser);  // '('
        Token* bind = expect_token(parser, TOKEN_IDENTIFIER);
        if (!bind) return NULL;
        pattern = create_ast_node(AST_PATTERN_VARIABLE, bind->value,
                                  bind->line, bind->column);
        pattern->annotation = strdup("some_pattern");
        if (!expect_token(parser, TOKEN_RIGHT_PAREN)) { free_ast_node(pattern); return NULL; }
    } else {
        // Expression pattern (literal, identifier, etc.) — includes the
        // `none` arm, which parse_expression yields as AST_NONE_LITERAL.
        // #1047: also accepts a range (`lo..=hi` / `lo..<hi`) and a comma-list
        // of values/ranges in one arm.
        pattern = parse_case_selector(parser);
        if (!pattern) return NULL;
    }
    
    // Expect -> arrow. The pattern is this function's until it is attached to
    // the arm below, so an error here has to release it.
    if (!expect_token(parser, TOKEN_ARROW)) { free_ast_node(pattern); return NULL; }

    // Parse the result (expression, statement, or block)
    ASTNode* result = NULL;
    Token* next = peek_token(parser);

    if (next && next->type == TOKEN_LEFT_BRACE) {
        // Block result
        result = parse_block(parser);
    } else if (next && next->type == TOKEN_PRINT) {
        // print/println is a statement keyword, not an expression
        result = parse_statement(parser);
    } else {
        // Expression result
        result = parse_expression(parser);
    }
    
    if (!result) { free_ast_node(pattern); return NULL; }

    // Optional comma or newline
    Token* separator = peek_token(parser);
    if (separator && separator->type == TOKEN_COMMA) {
        advance_token(parser);
    }
    
    // Create match arm node
    ASTNode* match_arm = create_ast_node(AST_MATCH_ARM, NULL, 0, 0);
    add_child(match_arm, pattern);
    add_child(match_arm, result);
    
    return match_arm;
}

// Parse module declaration
// Syntax: module name.subname
ASTNode* parse_module_declaration(Parser* parser) {
    Token* module_token = advance_token(parser);  // consume 'module'
    
    Token* name_token = expect_token(parser, TOKEN_IDENTIFIER);
    if (!name_token) return NULL;
    
    // Build full module name (handle dotted notation)
    char module_name[256] = {0};
    strncpy(module_name, name_token->value, sizeof(module_name) - 1);
    
    while (match_token(parser, TOKEN_DOT)) {
        Token* part = expect_token(parser, TOKEN_IDENTIFIER);
        if (!part) break;
        strncat(module_name, ".", sizeof(module_name) - strlen(module_name) - 1);
        strncat(module_name, part->value, sizeof(module_name) - strlen(module_name) - 1);
    }
    
    ASTNode* module_decl = create_ast_node(AST_MODULE_DECLARATION, module_name, 
                                          module_token->line, module_token->column);
    return module_decl;
}

// Parse import statement
// Helper: Check if token can be used as a module name part
// Allows identifiers and type keywords (string, int, float, etc.)
/* A segment of a module path is a NAME, not an expression, so any keyword
 * spelled like an identifier is unambiguous there: `import std.message` has
 * to reach std/message/module.ae even though `message` declares an actor
 * message elsewhere. Enumerating the keywords instead, as this once did,
 * meant such a module could only be imported by backtick-escaping it, and the
 * parser reported the failure on whatever followed the import. */
static int is_module_name_token(Token* token) {
    if (!token || !token->value || !token->value[0]) return 0;
    switch (token->type) {
        /* Literals carry identifier-looking text but are not names. */
        case TOKEN_STRING_LITERAL:
        case TOKEN_NUMBER:
            return 0;
        default:
            break;
    }
    const char* v = token->value;
    if (!((v[0] >= 'a' && v[0] <= 'z') || (v[0] >= 'A' && v[0] <= 'Z') || v[0] == '_')) {
        return 0;
    }
    for (const char* p = v; *p; p++) {
        int ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                 (*p >= '0' && *p <= '9') || *p == '_';
        if (!ok) return 0;
    }
    return 1;
}

// Syntax: import module.name
// Syntax: import module.name (symbol1, symbol2)
// Syntax: import module.name as alias
ASTNode* parse_import_statement(Parser* parser) {
    Token* import_token = advance_token(parser);  // consume 'import'

    Token* name_token = peek_token(parser);
    if (!is_module_name_token(name_token)) {
        parser_error(parser, "Expected module name after 'import'");
        return NULL;
    }
    advance_token(parser);  // consume name

    // Build module name (handle dotted notation)
    char module_name[256] = {0};
    strncpy(module_name, name_token->value, sizeof(module_name) - 1);

    while (match_token(parser, TOKEN_DOT)) {
        Token* part = peek_token(parser);
        if (!is_module_name_token(part)) break;
        advance_token(parser);  // consume the part
        strncat(module_name, ".", sizeof(module_name) - strlen(module_name) - 1);
        strncat(module_name, part->value, sizeof(module_name) - strlen(module_name) - 1);
    }
    
    ASTNode* import_stmt = create_ast_node(AST_IMPORT_STATEMENT, module_name,
                                          import_token->line, import_token->column);
    
    // Check for selective import: import mod (a, b, c)
    // Or glob import: import mod (*)
    //
    // The glob form expands at typecheck time to short aliases for every
    // public name (no leading underscore) defined by the imported module.
    // Implemented as a parser-side annotation rather than AST children
    // because the parser doesn't yet know what the module exports —
    // typechecker.c walks the symbol table to register the aliases once
    // the module's symbols are loaded. See issue #171 (P1).
    if (match_token(parser, TOKEN_LEFT_PAREN)) {
        Token* first = peek_token(parser);
        if (first && first->type == TOKEN_MULTIPLY) {
            advance_token(parser);  // consume '*'
            import_stmt->annotation = strdup("glob_import");
            expect_token(parser, TOKEN_RIGHT_PAREN);
        } else {
            do {
                // Tolerate a trailing comma before the closing paren —
                // `import mod (a, b,)`. Without this guard the comma's
                // next loop iteration hits `)` and errors spuriously.
                if (peek_token(parser) &&
                    peek_token(parser)->type == TOKEN_RIGHT_PAREN) break;
                Token* symbol = expect_token(parser, TOKEN_IDENTIFIER);
                if (!symbol) break;

                ASTNode* symbol_node = create_ast_node(AST_IDENTIFIER, symbol->value,
                                                      symbol->line, symbol->column);
                // Per-symbol aliasing: `import M (path as vgpath)` keeps
                // the node's value as the EXPORTED name and carries the
                // local binding in the annotation, so every consumer that
                // classifies names against the module's exports keeps
                // working unchanged.
                if (peek_token(parser) && peek_token(parser)->type == TOKEN_AS) {
                    advance_token(parser);  // consume 'as'
                    Token* alias = expect_token(parser, TOKEN_IDENTIFIER);
                    if (!alias) break;
                    char ann[160];
                    snprintf(ann, sizeof(ann), "select_alias:%s", alias->value);
                    symbol_node->annotation = strdup(ann);
                }
                add_child(import_stmt, symbol_node);
            } while (match_token(parser, TOKEN_COMMA));

            expect_token(parser, TOKEN_RIGHT_PAREN);
        }
    }
    
    // Check for alias: import mod as alias
    Token* next = peek_token(parser);
    if (next && next->type == TOKEN_AS) {
        advance_token(parser);  // consume 'as'
        Token* alias = expect_token(parser, TOKEN_IDENTIFIER);
        if (alias) {
            ASTNode* alias_node = create_ast_node(AST_IDENTIFIER, alias->value,
                                                 alias->line, alias->column);
            // Mark so the typechecker can tell `as`-aliases apart from
            // selective-import symbols, which share AST_IDENTIFIER children
            // of the import statement.
            alias_node->annotation = strdup("module_alias");
            // Store alias as last child
            add_child(import_stmt, alias_node);
        }
    }
    
    return import_stmt;
}

// Parse top-of-file `exports (a, b, c)` list — Erlang-style public-API
// declaration. Replaces the per-function `export <fn>` form for modules
// that prefer to list their public surface in one place. Mutually
// exclusive with `export` at the module-orchestration layer (the parser
// accepts both, the orchestrator errors if both appear in one module).
//
// Grammar:  exports ( IDENT [, IDENT]* )
//
// Children of the resulting AST_EXPORTS_LIST node are AST_IDENTIFIER
// nodes carrying each name. The orchestrator walks this list and
// populates `mod->exports[]` exactly as if each name had been written
// with a per-function `export <name>`.
ASTNode* parse_exports_list(Parser* parser) {
    Token* exports_token = advance_token(parser);  // consume 'exports'
    ASTNode* list = create_ast_node(AST_EXPORTS_LIST, NULL,
                                    exports_token->line, exports_token->column);

    if (!expect_token(parser, TOKEN_LEFT_PAREN)) return list;

    // Allow an empty list `exports ()` as a valid (if unusual) declaration —
    // it pins "this module exports nothing public" explicitly.
    if (peek_token(parser) && peek_token(parser)->type != TOKEN_RIGHT_PAREN) {
        do {
            // Tolerate a trailing comma before the closing paren —
            // `exports (a, b,)`. The multi-line, comma-per-line export
            // list style (one name per line, trailing comma on the last)
            // is common in stdlib modules (e.g. std.http.proxy). Without
            // this guard the trailing comma's next loop iteration hits
            // `)` and emits a spurious "Expected IDENTIFIER, got
            // RIGHT_PAREN" parse error.
            if (peek_token(parser) &&
                peek_token(parser)->type == TOKEN_RIGHT_PAREN) break;
            Token* name = expect_token(parser, TOKEN_IDENTIFIER);
            if (!name) break;
            ASTNode* id = create_ast_node(AST_IDENTIFIER, name->value,
                                          name->line, name->column);
            add_child(list, id);
        } while (match_token(parser, TOKEN_COMMA));
    }

    expect_token(parser, TOKEN_RIGHT_PAREN);
    return list;
}

// Parse export statement
// Syntax: export func_name
// Syntax: export struct Point { ... }
// Syntax: export actor Worker { ... }
ASTNode* parse_export_statement(Parser* parser) {
    Token* export_token = advance_token(parser);  // consume 'export'
    
    ASTNode* export_stmt = create_ast_node(AST_EXPORT_STATEMENT, NULL,
                                          export_token->line, export_token->column);
    
    Token* next = peek_token(parser);
    if (!next) return NULL;
    
    ASTNode* exported_item = NULL;
    
    switch (next->type) {
        case TOKEN_FUNC:
            advance_token(parser);
            exported_item = parse_function_definition(parser);
            break;
        case TOKEN_STRUCT:
            exported_item = parse_struct_definition(parser);
            break;
        case TOKEN_ACTOR:
            exported_item = parse_actor_definition(parser);
            break;
        case TOKEN_CONST:
            exported_item = parse_statement(parser);  // parse const declaration
            break;
        case TOKEN_INT:
        case TOKEN_INT64:
        case TOKEN_UINT64:
        case TOKEN_DURATION:
        case TOKEN_FLOAT:
        case TOKEN_BOOL:
        case TOKEN_BYTE:
        case TOKEN_STRING:
        case TOKEN_PTR: {
            // C-style: export int func_name(...) { ... }
            Token* next2 = peek_ahead(parser, 1);
            Token* next3 = peek_ahead(parser, 2);
            if (next2 && next2->type == TOKEN_IDENTIFIER &&
                next3 && next3->type == TOKEN_LEFT_PAREN) {
                Type* ret_type = parse_type(parser);
                exported_item = parse_function_definition(parser);
                if (exported_item && ret_type) {
                    if (exported_item->node_type) free_type(exported_item->node_type);
                    exported_item->node_type = ret_type;
                } else if (ret_type) {
                    free_type(ret_type);
                }
            } else {
                parser_error(parser, "Expected function definition after type in export");
                return NULL;
            }
            break;
        }
        case TOKEN_IDENTIFIER: {
            // Check if this is a function: export func_name(...)
            Token* after = peek_ahead(parser, 1);
            if (after && after->type == TOKEN_LEFT_PAREN) {
                exported_item = parse_function_definition(parser);
            } else {
                // Export existing symbol: export my_func
                exported_item = create_ast_node(AST_IDENTIFIER, next->value,
                                              next->line, next->column);
                advance_token(parser);
            }
            break;
        }
        default:
            parser_error(parser, "Expected function, struct, actor, or identifier after 'export'");
            return NULL;
    }
    
    if (exported_item) {
        add_child(export_stmt, exported_item);
    }
    
    return export_stmt;
}

ASTNode* parse_return_statement(Parser* parser) {
    Token* ret_tok = peek_token(parser);
    advance_token(parser); // return
    ASTNode* return_stmt = create_ast_node(AST_RETURN_STATEMENT, NULL,
                                           ret_tok ? ret_tok->line : 0,
                                           ret_tok ? ret_tok->column : 0);

    if (!match_token(parser, TOKEN_SEMICOLON)) {
        // #1054: `return match x { ... }`. A `match` is statement-only in the
        // grammar (not reachable from parse_expression), so parse it explicitly
        // here and attach it as the return value; codegen lowers a match in
        // return position to a value-producing form. Without this the `match`
        // was left unconsumed, `return` took no operand, and the match parsed
        // as a dead sibling statement (void return + garbage result).
        ASTNode* value = (peek_token(parser) && peek_token(parser)->type == TOKEN_MATCH)
                         ? parse_match_statement(parser)
                         : parse_expression(parser);
        if (value) {
            add_child(return_stmt, value);
        }

        // Multiple return values: return a, b
        while (peek_token(parser) && peek_token(parser)->type == TOKEN_COMMA) {
            advance_token(parser);  // consume comma
            ASTNode* next_val = parse_expression(parser);
            if (next_val) {
                add_child(return_stmt, next_val);
            }
        }

        /* #1421: `return (a, b)` means the same thing as `return a, b`.
         *
         * `(a, b)` parses as a tuple literal, so the parenthesised spelling
         * arrived here as ONE child where the bare one arrives as two, and
         * codegen flattened the literal into the return slot: the reported
         * symptom was C naming an identifier `NULL0` (from `(null, 0)`) or
         * `bufw` (from `(w.buf, w.off)`), with nothing pointing at the
         * parentheses. Splicing the elements up makes the two spellings the
         * same AST, so every later stage treats them identically by
         * construction rather than by having two paths that agree.
         *
         * Only when the literal is the whole return value: `return (a, b), c`
         * keeps its existing shape rather than silently regrouping. */
        if (return_stmt->child_count == 1 &&
            return_stmt->children[0] &&
            return_stmt->children[0]->type == AST_TUPLE_LITERAL &&
            return_stmt->children[0]->child_count > 1) {
            ASTNode* tup = return_stmt->children[0];
            return_stmt->child_count = 0;
            for (int i = 0; i < tup->child_count; i++) {
                add_child(return_stmt, tup->children[i]);
            }
            /* The elements now belong to the return statement; detach them
             * before freeing the husk, or free_ast_node takes them with it. */
            tup->child_count = 0;
            free_ast_node(tup);
        }

        match_token(parser, TOKEN_SEMICOLON);
    }

    return return_stmt;
}

/* #1140 — `defer`, `defer try` and `defer catch`.
 *
 *   defer       cleanup()    // always, on every exit (the pre-existing form)
 *   defer try   commit()     // only when the function returns SUCCESSFULLY
 *   defer catch rollback()   // only when the function returns an ERROR
 *
 * The qualifier is recorded in `value` ("try" / "catch"; NULL = unconditional),
 * which codegen reads when it drains the defer stack at each exit.
 *
 * This is the transaction shape: acquire, register the rollback, register the
 * commit, then let any error path bail without the acquire leaking and without
 * the half-built result being published.
 *
 * `try` / `catch` here are the ordinary existing keywords — no new tokens. The
 * qualifier is only meaningful in a function that can actually fail (one
 * returning `(value, err)` or `T!`); in any other function a `defer try` is just
 * an unconditional defer and a `defer catch` never fires, which the typechecker
 * warns about rather than silently accepting. */
ASTNode* parse_defer_statement(Parser* parser) {
    Token* defer_token = peek_token(parser);
    advance_token(parser);

    const char* mode = NULL;
    Token* q = peek_token(parser);
    if (q && q->type == TOKEN_TRY) {
        advance_token(parser);
        mode = "try";
    } else if (q && q->type == TOKEN_CATCH) {
        advance_token(parser);
        mode = "catch";
    }

    ASTNode* deferred_stmt = parse_statement(parser);
    if (!deferred_stmt) {
        parser_error(parser, mode
            ? "Expected statement after `defer try` / `defer catch`"
            : "Expected statement after 'defer'");
        return NULL;
    }

    ASTNode* defer_node = create_ast_node(AST_DEFER_STATEMENT, NULL,
                                          defer_token->line, defer_token->column);
    if (mode) defer_node->value = strdup(mode);
    add_child(defer_node, deferred_stmt);

    return defer_node;
}

// try { body } catch name { handler }
//
// Shape:
//   AST_TRY_STATEMENT
//     [0] AST_BLOCK (body)
//     [1] AST_CATCH_CLAUSE, value = bound name
//       [0] AST_BLOCK (handler)
ASTNode* parse_try_statement(Parser* parser) {
    Token* try_token = peek_token(parser);
    advance_token(parser); // consume 'try'

    ASTNode* body = parse_block(parser);
    if (!body) {
        parser_error(parser, "Expected '{ ... }' after 'try'");
        return NULL;
    }

    Token* catch_tok = peek_token(parser);
    if (!catch_tok || catch_tok->type != TOKEN_CATCH) {
        parser_error(parser, "Expected 'catch' after try block");
        return NULL;
    }
    advance_token(parser); // consume 'catch'

    Token* name = expect_token(parser, TOKEN_IDENTIFIER);
    if (!name) {
        parser_error(parser, "Expected identifier after 'catch' to bind the panic reason");
        return NULL;
    }

    ASTNode* handler = parse_block(parser);
    if (!handler) {
        parser_error(parser, "Expected '{ ... }' for catch handler");
        return NULL;
    }

    ASTNode* catch_node = create_ast_node(AST_CATCH_CLAUSE, name->value,
                                          catch_tok->line, catch_tok->column);
    add_child(catch_node, handler);

    ASTNode* try_node = create_ast_node(AST_TRY_STATEMENT, NULL,
                                        try_token->line, try_token->column);
    add_child(try_node, body);
    add_child(try_node, catch_node);
    return try_node;
}

// panic("reason") — a statement form. The argument is parsed as a normal
// expression (so interpolation and variables work) and stored as the
// single child.
ASTNode* parse_panic_statement(Parser* parser) {
    Token* panic_tok = peek_token(parser);
    advance_token(parser); // consume 'panic'

    if (!expect_token(parser, TOKEN_LEFT_PAREN)) return NULL;

    ASTNode* reason_expr = parse_expression(parser);
    if (!reason_expr) {
        parser_error(parser, "Expected reason expression inside panic(...)");
        return NULL;
    }

    if (!expect_token(parser, TOKEN_RIGHT_PAREN)) return NULL;
    match_token(parser, TOKEN_SEMICOLON);

    ASTNode* panic_node = create_ast_node(AST_PANIC_STATEMENT, NULL,
                                          panic_tok->line, panic_tok->column);
    add_child(panic_node, reason_expr);
    return panic_node;
}

// Actor V2 - Message Definition Parsing
// Syntax: message MessageName { field1: type1, field2: type2 }
ASTNode* parse_message_definition(Parser* parser) {
    Token* message_token = peek_token(parser);
    advance_token(parser); // consume 'message'
    
    Token* name = expect_token(parser, TOKEN_IDENTIFIER);
    if (!name) return NULL;
    
    expect_token(parser, TOKEN_LEFT_BRACE);
    
    ASTNode* msg_def = create_ast_node(AST_MESSAGE_DEFINITION, name->value, message_token->line, message_token->column);
    
    // Parse fields: name: type
    while (!match_token(parser, TOKEN_RIGHT_BRACE)) {
        if (is_at_end(parser)) {
            parser_message(parser, "Error: Unexpected end of file in message definition");
            return NULL;
        }
        
        Token* field_name = expect_token(parser, TOKEN_IDENTIFIER);
        if (!field_name) break;

        if (!expect_token(parser, TOKEN_COLON)) break;

        Type* field_type = parse_type(parser);
        if (!field_type) {
            parser_message(parser, "Error: Expected type for message field");
            break;
        }
        
        ASTNode* field = create_ast_node(AST_MESSAGE_FIELD, field_name->value, field_name->line, field_name->column);
        field->node_type = field_type;
        add_child(msg_def, field);
        
        // Optional comma
        if (peek_token(parser) && peek_token(parser)->type == TOKEN_COMMA) {
            advance_token(parser);
        }
    }
    
    return msg_def;
}

// Parse message pattern in receive block
// Syntax: MessageName(field1, field2) or MessageName(field1: var1, field2)
ASTNode* parse_message_pattern(Parser* parser) {
    Token* msg_name = expect_token(parser, TOKEN_IDENTIFIER);
    if (!msg_name) return NULL;

    ASTNode* pattern = create_ast_node(AST_MESSAGE_PATTERN, msg_name->value, msg_name->line, msg_name->column);

    // Check for field destructuring
    if (match_token(parser, TOKEN_LEFT_PAREN)) {
        // Parse pattern fields
        while (!match_token(parser, TOKEN_RIGHT_PAREN)) {
            if (is_at_end(parser)) {
                parser_message(parser, "Error: Unexpected end in message pattern");
                return NULL;
            }
            
            Token* field_name = expect_token(parser, TOKEN_IDENTIFIER);
            if (!field_name) break;

            ASTNode* field_pattern = create_ast_node(AST_PATTERN_FIELD, field_name->value, field_name->line, field_name->column);

            // Check for explicit binding: field: variable
            if (match_token(parser, TOKEN_COLON)) {
                Token* var_name = expect_token(parser, TOKEN_IDENTIFIER);
                if (var_name) {
                    ASTNode* var_node = create_ast_node(AST_PATTERN_VARIABLE, var_name->value, var_name->line, var_name->column);
                    add_child(field_pattern, var_node);
                }
            } else {
                // Implicit binding: use field name as variable name
                ASTNode* var_node = create_ast_node(AST_PATTERN_VARIABLE, field_name->value, field_name->line, field_name->column);
                add_child(field_pattern, var_node);
            }

            add_child(pattern, field_pattern);
            
            if (peek_token(parser) && peek_token(parser)->type == TOKEN_COMMA) {
                advance_token(parser);
            }
        }
    }
    
    return pattern;
}

// Parse reply statement
// Syntax: reply MessageName { field1: expr1, field2: expr2 }
//     or: reply <expression>            (typed scalar reply, #1324)
ASTNode* parse_reply_statement(Parser* parser) {
    Token* reply_token = peek_token(parser);
    advance_token(parser); // consume 'reply'

    // Two-token lookahead: only `IDENT {` is the message-constructor
    // form. Anything else (`reply count`, `reply n * 2`, `reply f()`)
    // is an expression reply delivered to the asker as a typed scalar.
    Token* head = peek_token(parser);
    Token* brace = peek_ahead(parser, 1);
    if (!(head && head->type == TOKEN_IDENTIFIER &&
          brace && brace->type == TOKEN_LEFT_BRACE)) {
        ASTNode* reply_stmt = create_ast_node(AST_REPLY_STATEMENT, NULL,
                                              reply_token->line, reply_token->column);
        ASTNode* value_expr = parse_expression(parser);
        if (!value_expr) {
            parser_message(parser, "Error: expected expression after 'reply'");
            return NULL;
        }
        add_child(reply_stmt, value_expr);
        match_token(parser, TOKEN_SEMICOLON);
        return reply_stmt;
    }

    Token* msg_name = expect_token(parser, TOKEN_IDENTIFIER);
    if (!msg_name) return NULL;

    ASTNode* reply_stmt = create_ast_node(AST_REPLY_STATEMENT, NULL, reply_token->line, reply_token->column);

    // Create message constructor node (codegen expects this structure)
    ASTNode* msg_constructor = create_ast_node(AST_MESSAGE_CONSTRUCTOR, msg_name->value, msg_name->line, msg_name->column);

    // Parse message fields
    if (match_token(parser, TOKEN_LEFT_BRACE)) {
        while (!match_token(parser, TOKEN_RIGHT_BRACE)) {
            if (is_at_end(parser)) {
                parser_message(parser, "Error: Unexpected end in reply statement");
                return NULL;
            }

            Token* field_name = expect_token(parser, TOKEN_IDENTIFIER);
            if (!field_name) break;

            if (!expect_token(parser, TOKEN_COLON)) break;

            ASTNode* field_expr = parse_expression(parser);
            if (!field_expr) break;

            ASTNode* field_init = create_ast_node(AST_FIELD_INIT, field_name->value, field_name->line, field_name->column);
            add_child(field_init, field_expr);
            add_child(msg_constructor, field_init);

            if (peek_token(parser) && peek_token(parser)->type == TOKEN_COMMA) {
                advance_token(parser);
            }
        }
    }

    add_child(reply_stmt, msg_constructor);

    // Optional semicolon (Aether allows statements without semicolons)
    match_token(parser, TOKEN_SEMICOLON);

    return reply_stmt;
}

// Parse message constructor (for send operations)
// Syntax: MessageName { field1: expr1, field2: expr2 }
ASTNode* parse_message_constructor(Parser* parser) {
    Token* msg_name = expect_token(parser, TOKEN_IDENTIFIER);
    if (!msg_name) return NULL;
    
    ASTNode* constructor = create_ast_node(AST_MESSAGE_CONSTRUCTOR, msg_name->value, msg_name->line, msg_name->column);
    
    if (match_token(parser, TOKEN_LEFT_BRACE)) {
        while (!match_token(parser, TOKEN_RIGHT_BRACE)) {
            if (is_at_end(parser)) {
                parser_message(parser, "Error: Unexpected end in message constructor");
                return NULL;
            }
            
            Token* field_name = expect_token(parser, TOKEN_IDENTIFIER);
            if (!field_name) break;

            if (!expect_token(parser, TOKEN_COLON)) break;

            ASTNode* field_expr = parse_expression(parser);
            if (!field_expr) break;

            ASTNode* field_init = create_ast_node(AST_FIELD_INIT, field_name->value, field_name->line, field_name->column);
            add_child(field_init, field_expr);
            add_child(constructor, field_init);
            
            if (peek_token(parser) && peek_token(parser)->type == TOKEN_COMMA) {
                advance_token(parser);
            }
        }
    }
    
    return constructor;
}

ASTNode* parse_print_statement(Parser* parser) {
    advance_token(parser); // print
    expect_token(parser, TOKEN_LEFT_PAREN);
    
    ASTNode* print_stmt = create_ast_node(AST_PRINT_STATEMENT, NULL, 0, 0);
    
    if (!match_token(parser, TOKEN_RIGHT_PAREN)) {
        do {
            ASTNode* arg = parse_expression(parser);
            if (arg) {
                add_child(print_stmt, arg);
            }
        } while (match_token(parser, TOKEN_COMMA));
        
        expect_token(parser, TOKEN_RIGHT_PAREN);
    }
    
    match_token(parser, TOKEN_SEMICOLON);
    return print_stmt;
}

ASTNode* parse_send_statement(Parser* parser) {
    advance_token(parser); // send
    expect_token(parser, TOKEN_LEFT_PAREN);
    
    ASTNode* actor_ref = parse_expression(parser);
    if (!actor_ref) return NULL;
    
    expect_token(parser, TOKEN_COMMA);
    ASTNode* message = parse_expression(parser);
    if (!message) return NULL;
    
    expect_token(parser, TOKEN_RIGHT_PAREN);
    match_token(parser, TOKEN_SEMICOLON);
    
    ASTNode* send_stmt = create_ast_node(AST_SEND_STATEMENT, NULL, 0, 0);
    add_child(send_stmt, actor_ref);
    add_child(send_stmt, message);
    
    return send_stmt;
}

ASTNode* parse_spawn_actor_statement(Parser* parser) {
    advance_token(parser); // spawn_actor
    expect_token(parser, TOKEN_LEFT_PAREN);
    
    ASTNode* actor_type = parse_expression(parser);
    if (!actor_type) return NULL;
    
    expect_token(parser, TOKEN_RIGHT_PAREN);
    match_token(parser, TOKEN_SEMICOLON);
    
    ASTNode* spawn_stmt = create_ast_node(AST_SPAWN_ACTOR_STATEMENT, NULL, 0, 0);
    add_child(spawn_stmt, actor_type);
    
    return spawn_stmt;
}

/* Statement-level sibling of the top-level region. Same rule: the losing
 * branch is never parsed, so a guarded block may call functions that only
 * exist under the same symbol. Declared here because it recurses through
 * parse_statement. */
static void parse_when_region_stmts(Parser* parser, ASTNode* block);

ASTNode* parse_block(Parser* parser) {
    expect_token(parser, TOKEN_LEFT_BRACE);
    
    ASTNode* block = create_ast_node(AST_BLOCK, NULL, 0, 0);
    
    while (!match_token(parser, TOKEN_RIGHT_BRACE)) {
        if (at_when_region(parser)) {
            parse_when_region_stmts(parser, block);
            continue;
        }
        int start_token = parser->current_token;
        ASTNode* stmt = parse_statement(parser);
        if (stmt) {
            add_child(block, stmt);
        } else {
            // Prevent infinite loops on unexpected tokens inside blocks.
            // If the block-head token is a reserved keyword being used as
            // if it were an identifier (e.g. `message = "hello"`), point
            // at it directly instead of the generic "expected statement"
            // that leaves users guessing.
            Token* stmt_head = peek_token(parser);
            if (stmt_head && token_is_reserved_keyword(stmt_head)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "'%s' is a reserved keyword and cannot be used as an identifier; rename it (e.g. '%s_' or 'msg')",
                    stmt_head->value, stmt_head->value);
                char hint[128];
                snprintf(hint, sizeof(hint),
                    "rename to '%s_' or another identifier",
                    stmt_head->value);
                if (!parser->suppress_errors) {
                    aether_error_full(msg, stmt_head->line, stmt_head->column,
                                      hint, NULL, AETHER_ERR_SYNTAX);
                }
            } else {
                parser_error(parser, "Expected statement in block");
            }
            if (parser->current_token == start_token) {
                advance_token(parser);
            }
        }

        if (is_at_end(parser)) break;
    }
    
    return block;
}

ASTNode* parse_actor_definition(Parser* parser) {
    advance_token(parser); // actor
    Token* name = expect_token(parser, TOKEN_IDENTIFIER);
    if (!name) return NULL;
    
    expect_token(parser, TOKEN_LEFT_BRACE);
    
    ASTNode* actor = create_ast_node(AST_ACTOR_DEFINITION, name->value, name->line, name->column);
    
    int iteration_count = 0;
    const int MAX_ACTOR_BODY = 1000;
    
    while (!match_token(parser, TOKEN_RIGHT_BRACE)) {
        if (++iteration_count > MAX_ACTOR_BODY) {
            parser_message(parser, "Error: Too many statements in actor definition (max 1000)");
            break;
        }
        
        if (is_at_end(parser)) {
            parser_message(parser, "Error: Unexpected end of file in actor definition");
            break;
        }
        
        if (match_token(parser, TOKEN_STATE)) {
            // Check if there's an explicit type or Python-style
            Token* next_tok = peek_token(parser);
            ASTNode* state_decl = NULL;
            
            if (next_tok && (next_tok->type == TOKEN_INT || next_tok->type == TOKEN_INT64 ||
                            next_tok->type == TOKEN_UINT64 ||
                            next_tok->type == TOKEN_DURATION ||
                            next_tok->type == TOKEN_FLOAT ||
                            next_tok->type == TOKEN_STRING || next_tok->type == TOKEN_BOOL ||
                            next_tok->type == TOKEN_BYTE)) {
                // Explicit type: state int count = 0  or  state long total = 0
                state_decl = parse_variable_declaration_with_semicolon(parser, false);
            } else if (next_tok && next_tok->type == TOKEN_IDENTIFIER) {
                // Python-style: state count = 0 (no semicolon required in actor)
                Token* name = expect_token(parser, TOKEN_IDENTIFIER);
                if (name) {
                    state_decl = create_ast_node(AST_VARIABLE_DECLARATION, name->value, name->line, name->column);
                    state_decl->node_type = create_type(TYPE_UNKNOWN);
                    
                    if (match_token(parser, TOKEN_ASSIGN)) {
                        ASTNode* value = parse_expression(parser);
                        if (value) {
                            add_child(state_decl, value);
                        }
                    }
                }
            }
            
            if (state_decl) {
                state_decl->type = AST_STATE_DECLARATION;
                add_child(actor, state_decl);
                // Consume optional semicolon after state declaration
                match_token(parser, TOKEN_SEMICOLON);
            }
        } else if (match_token(parser, TOKEN_RECEIVE)) {
            ASTNode* receive_stmt = parse_receive_statement(parser);
            if (receive_stmt) {
                add_child(actor, receive_stmt);
            }
        } else {
            ASTNode* stmt = parse_statement(parser);
            if (stmt) {
                add_child(actor, stmt);
            } else {
                // If we can't parse a statement, advance to avoid infinite loop
                Token* tok = peek_token(parser);
                if (tok) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Unexpected token in actor body: '%s'",
                             tok->value ? tok->value : "?");
                    aether_error_simple(msg, tok->line, tok->column);
                }
                advance_token(parser);
            }
        }
    }
    
    return actor;
}

ASTNode* parse_receive_statement(Parser* parser) {
    // Note: TOKEN_RECEIVE has already been consumed by the caller
    Token* current = peek_token(parser);
    if (!current) return NULL;
    
    // Check for V1 syntax: receive(msg) { ... }
    if (current->type == TOKEN_LEFT_PAREN) {
        // V1 syntax - backward compatibility
        expect_token(parser, TOKEN_LEFT_PAREN);
        Token* param = expect_token(parser, TOKEN_IDENTIFIER);
        if (!param) return NULL;
        expect_token(parser, TOKEN_RIGHT_PAREN);
        
        ASTNode* body = parse_block(parser);
        if (!body) return NULL;
        
        ASTNode* receive_stmt = create_ast_node(AST_RECEIVE_STATEMENT, param->value, param->line, param->column);
        add_child(receive_stmt, body);
        
        return receive_stmt;
    }
    
    // V2 syntax: receive { Pattern -> block, ... }
    if (current->type != TOKEN_LEFT_BRACE) {
        parser_error(parser, "Expected '(' or '{' after 'receive'");
        return NULL;
    }
    
    expect_token(parser, TOKEN_LEFT_BRACE);
    
    ASTNode* receive_stmt = create_ast_node(AST_RECEIVE_STATEMENT, NULL, current->line, current->column);
    
    // Parse receive arms (pattern matching)
    while (!match_token(parser, TOKEN_RIGHT_BRACE)) {
        if (is_at_end(parser)) {
            parser_message(parser, "Error: Unexpected end in receive statement");
            return NULL;
        }
        
        // Parse pattern (message pattern or wildcard)
        ASTNode* pattern = NULL;
        Token* pattern_token = peek_token(parser);
        
        if (pattern_token && pattern_token->type == TOKEN_IDENTIFIER && 
            strcmp(pattern_token->value, "_") == 0) {
            // Wildcard pattern: _
            advance_token(parser);
            pattern = create_ast_node(AST_WILDCARD_PATTERN, "_", pattern_token->line, pattern_token->column);
        } else {
            // Message pattern: MessageName { fields }
            pattern = parse_message_pattern(parser);
            if (!pattern) break;
        }
        
        expect_token(parser, TOKEN_ARROW);
        
        // Parse arm body
        ASTNode* arm_body = NULL;
        Token* body_start = peek_token(parser);
        
        if (body_start && body_start->type == TOKEN_LEFT_BRACE) {
            arm_body = parse_block(parser);
        } else {
            // Single expression or statement
            ASTNode* stmt = parse_statement(parser);
            if (stmt) {
                arm_body = create_ast_node(AST_BLOCK, NULL, body_start->line, body_start->column);
                add_child(arm_body, stmt);
            }
        }
        
        if (!arm_body || !pattern || !pattern_token) break;

        // Create receive arm node
        ASTNode* arm = create_ast_node(AST_RECEIVE_ARM, NULL, pattern_token->line, pattern_token->column);
        add_child(arm, pattern);
        add_child(arm, arm_body);
        add_child(receive_stmt, arm);
        
        // Optional comma between arms
        if (peek_token(parser) && peek_token(parser)->type == TOKEN_COMMA) {
            advance_token(parser);
        }
    }
    
    // Check for timeout clause: } after N -> { body }
    Token* after_tok = peek_token(parser);
    if (after_tok && after_tok->type == TOKEN_AFTER) {
        advance_token(parser);  // consume 'after'

        ASTNode* timeout_expr = parse_expression(parser);
        if (!timeout_expr) {
            parser_error(parser, "Expected timeout expression after 'after'");
            return receive_stmt;
        }

        expect_token(parser, TOKEN_ARROW);

        ASTNode* timeout_body = NULL;
        Token* tbody_start = peek_token(parser);
        if (tbody_start && tbody_start->type == TOKEN_LEFT_BRACE) {
            timeout_body = parse_block(parser);
        } else {
            ASTNode* stmt = parse_statement(parser);
            if (stmt) {
                timeout_body = create_ast_node(AST_BLOCK, NULL, tbody_start->line, tbody_start->column);
                add_child(timeout_body, stmt);
            }
        }

        if (timeout_body) {
            ASTNode* timeout_arm = create_ast_node(AST_TIMEOUT_ARM, NULL, after_tok->line, after_tok->column);
            add_child(timeout_arm, timeout_expr);
            add_child(timeout_arm, timeout_body);
            add_child(receive_stmt, timeout_arm);
        }
    }

    return receive_stmt;
}

// Parse one field inside an `extern struct` body. Three shapes:
//   1. `name: type`             — leaf field. With optional `: NN` bit-width.
//   2. `name: union { ... }`    — compound field, members overlap at the
//                                 field's start offset (C union semantics).
//   3. `name: struct { ... }`   — compound field, members laid out sequen-
//                                 tially (C struct semantics). Typically
//                                 appears as a member of a union to model
//                                 multi-field variants like `u.func.length`
//                                 + `u.func.magic` + ... in mquickjs's
//                                 JSPropDef.
//
// Returns the field AST node, or NULL on parse error (with diagnostics
// already emitted). Caller wires the result into the parent struct's
// children list.
ASTNode* parse_extern_struct_field(Parser* parser) {
    // #880: accept the value-identifier keywords (`ptr`/`byte`/`func`/`state`/
    // `after`) as extern-struct field NAMES — these mirror C struct fields and
    // are all valid C field identifiers. The `name:` form is unambiguous (the
    // name is always first), so no type/name disambiguation is needed here.
    Token* fname = peek_token(parser);
    if (fname && token_is_value_ident(fname)) {
        advance_token(parser);
    } else {
        fname = expect_token(parser, TOKEN_IDENTIFIER);
        if (!fname) return NULL;
    }

    if (!expect_token(parser, TOKEN_COLON)) return NULL;

    Token* peek = peek_token(parser);
    if (peek && (peek->type == TOKEN_UNION || peek->type == TOKEN_STRUCT)) {
        int is_union = (peek->type == TOKEN_UNION);
        advance_token(parser);  // consume `union` / `struct`
        if (!expect_token(parser, TOKEN_LEFT_BRACE)) return NULL;
        ASTNode* compound = create_ast_node(
            is_union ? AST_STRUCT_FIELD_UNION : AST_STRUCT_FIELD_NESTED,
            fname->value, fname->line, fname->column);
        while (!match_token(parser, TOKEN_RIGHT_BRACE)) {
            if (is_at_end(parser)) {
                parser_error(parser, "Unexpected end of union/struct field");
                free_ast_node(compound);
                return NULL;
            }
            ASTNode* sub = parse_extern_struct_field(parser);
            if (!sub) { free_ast_node(compound); return NULL; }
            add_child(compound, sub);
            if (!match_token(parser, TOKEN_COMMA)) {
                match_token(parser, TOKEN_SEMICOLON);
            }
        }
        return compound;
    }

    ASTNode* field = create_ast_node(AST_STRUCT_FIELD, fname->value,
                                     fname->line, fname->column);
    Type* ftype = parse_type(parser);
    if (!ftype) {
        free_ast_node(field);
        return NULL;
    }
    field->node_type = ftype;
    /* Optional bit-width: `: NN` after the type. */
    if (peek_token(parser) && peek_token(parser)->type == TOKEN_COLON) {
        advance_token(parser);  // consume second ':'
        Token* width_tok = expect_token(parser, TOKEN_NUMBER);
        if (!width_tok || !width_tok->value) {
            parser_error(parser, "expected bit width after `:`");
            free_ast_node(field);
            return NULL;
        }
        int w = atoi(width_tok->value);
        if (w <= 0 || w > 64) {
            parser_error(parser, "bit width out of range (1..64)");
            free_ast_node(field);
            return NULL;
        }
        field->bit_width = w;
    }
    return field;
}

/* Trailing `@`-attributes on an extern signature, shared by the bare
 * `extern name(...)` form and the `@extern("c_sym") name(...)` form:
 *
 *   @heap     the `-> string` return is a malloc'd buffer the caller owns.
 *             Recorded as "heap_return", read by is_heap_string_expr in
 *             codegen_stmt.c. Only meaningful on `-> string`; anything else
 *             is a parse error since integers and pointers aren't heap-
 *             tracked and void has nothing to own.
 *   @borrow   the unannotated default. A no-op, accepted so callers can be
 *             explicit and symmetric with the tuple form.
 *   @c_import a C header owns the prototype, so codegen emits no declaration
 *             of its own (#1239). The header's exact typedef spelling ends up
 *             being the only one in the translation unit, so the two cannot
 *             disagree. Same "the header is the sole source of truth"
 *             contract as `extern struct @c_import` and `extern const
 *             @c_import`.
 *
 * Attributes stack and are order-independent; they accumulate as `;`-joined
 * markers via annotation_add_marker, so adding one never drops another (a
 * variadic `@heap` extern stays variadic).
 *
 * Disambiguating from the NEXT declaration: newlines aren't tokens here, so a
 * stray `@` after the return type might be the start of a following
 * `@c_callback` / `@extern("...")`. Two-token lookahead, only consume the
 * `@` when the token after it is one of the names above; anything else is left
 * for the top-level decoration handler.
 */
static void parse_extern_trailing_attrs(Parser* parser, ASTNode* ext) {
    while (peek_token(parser) && peek_token(parser)->type == TOKEN_AT) {
        Token* tag = peek_ahead(parser, 1);
        if (!tag || tag->type != TOKEN_IDENTIFIER || !tag->value) break;

        if (strcmp(tag->value, "heap") == 0) {
            advance_token(parser);  /* consume '@' */
            if (!ext->node_type || ext->node_type->kind != TYPE_STRING) {
                parser_error(parser,
                    "@heap on extern return is only valid on `-> string`");
            } else {
                ext->annotation =
                    annotation_add_marker(ext->annotation, "heap_return");
            }
            advance_token(parser);  /* consume 'heap' */
        } else if (strcmp(tag->value, "borrow") == 0) {
            advance_token(parser);  /* consume '@' */
            advance_token(parser);  /* consume 'borrow' */
        } else if (strcmp(tag->value, "c_import") == 0) {
            advance_token(parser);  /* consume '@' */
            ext->annotation =
                annotation_add_marker(ext->annotation, "c_import");
            advance_token(parser);  /* consume 'c_import' */
        } else {
            break;
        }
    }
}

// Parse extern C function declaration
// Syntax: extern name(param: type, ...) -> return_type
//         extern name(param: type, ...)   (void return)
ASTNode* parse_extern_declaration(Parser* parser) {
    Token* extern_token = expect_token(parser, TOKEN_EXTERN);
    if (!extern_token) return NULL;

    /* `extern struct Name { ... }` — declares a C struct whose
     * layout the user is asserting matches the C side.  The Aether-
     * side emit produces the same C struct declaration so codegen
     * can do `view->field` member access via the standard `*Name`
     * overlay path.  Field decls accept an optional `: NN` bit-width
     * suffix for C bitfield support:
     *
     *     extern struct JSObject {
     *         class_id: byte         // plain field
     *     }
     *
     *     extern struct JSString {
     *         is_unique: int : 1     // bitfield, 1 bit
     *         is_ascii:  int : 1
     *         len:       int : 27
     *     }
     *
     * The user is responsible for ensuring the C-side layout matches
     * — Aether emits the struct definition into its .gen.c file;
     * if the surrounding C code has a competing definition under the
     * same name in the SAME translation unit, that's a duplicate-
     * typedef error.  Typical usage: the C side defines the struct
     * in a private header that the .gen.c does NOT include, so each
     * TU sees exactly one definition.  Layouts agree by construction
     * if the Aether spelling matches the C spelling.
     */
    /* `extern type Name` — an opaque, header-defined C type.  Used as
     * an FFI pointee (`*Name`) for handles whose layout Aether never
     * inspects: Redis `client`, `dictEntry`; mquickjs `JSContext`.
     * Emits a forward typedef `typedef struct Name Name;` (an
     * incomplete type) — `Name*` is a valid pointer, field access is
     * not.  See redis-porting-language-gaps.md "P0: Typed And
     * Qualified C Pointers". */
    if (peek_token(parser) && peek_token(parser)->type == TOKEN_IDENTIFIER &&
        peek_token(parser)->value && strcmp(peek_token(parser)->value, "type") == 0) {
        advance_token(parser);  // consume `type`
        Token* tname = expect_token(parser, TOKEN_IDENTIFIER);
        if (!tname) return NULL;
        ASTNode* td = create_ast_node(AST_STRUCT_DEFINITION, tname->value,
                                      extern_token->line, extern_token->column);
        td->annotation = strdup("extern_opaque");
        return td;
    }

    if (peek_token(parser) && peek_token(parser)->type == TOKEN_STRUCT) {
        advance_token(parser);  // consume `struct`
        Token* sname = expect_token(parser, TOKEN_IDENTIFIER);
        if (!sname) return NULL;
        ASTNode* sd = create_ast_node(AST_STRUCT_DEFINITION, sname->value,
                                      extern_token->line, extern_token->column);
        sd->annotation = strdup("extern");

        /* Optional `@c_import` after the struct name:
         *
         *     extern struct client @c_import { argc: int; argv: ptr }
         *
         * Marks a struct whose layout is *imported from a C header*,
         * not emitted by Aether.  Aether typechecks field access and
         * `*client` overlays against the declared fields, but codegen
         * emits NO `typedef struct client { ... } client;` and NO
         * forward typedef — the included header is the sole source of
         * truth for size, layout and padding.  Without this, Aether's
         * own typedef collides with the header's.  See
         * redis-porting-language-gaps.md "P0: Header-Defined C Struct
         * Interop". */
        /* Zero or more `@`-attributes after the name:
         *   @c_import  — layout imported from a C header (no body emitted)
         *   @packed    — emit the C body with __attribute__((packed)) so
         *                its layout has no inter-field padding (#747): the
         *                Redis sdshdr8/16/32/64 shape. Mutually exclusive
         *                with @c_import (a header-defined struct's packing
         *                is the header's job; @packed only governs a body
         *                Aether emits). */
        int has_c_import = 0, has_packed = 0;
        while (peek_token(parser) && peek_token(parser)->type == TOKEN_AT) {
            advance_token(parser);  // consume '@'
            Token* attr = peek_token(parser);
            if (attr && attr->type == TOKEN_IDENTIFIER && attr->value &&
                strcmp(attr->value, "c_import") == 0) {
                advance_token(parser);
                has_c_import = 1;
            } else if (attr && attr->type == TOKEN_IDENTIFIER && attr->value &&
                       strcmp(attr->value, "packed") == 0) {
                advance_token(parser);
                has_packed = 1;
            } else {
                parser_error(parser,
                    "unknown extern-struct attribute (expected @c_import or @packed)");
                free_ast_node(sd);
                return NULL;
            }
        }
        if (has_c_import && has_packed) {
            parser_error(parser,
                "@packed and @c_import are mutually exclusive, a @c_import "
                "struct's layout (incl. packing) comes from the C header; "
                "use @packed only on a struct whose body Aether emits");
            free_ast_node(sd);
            return NULL;
        }
        if (has_c_import) {
            free(sd->annotation);
            sd->annotation = strdup("extern_c_import");
        } else if (has_packed) {
            free(sd->annotation);
            sd->annotation = strdup("extern_packed");
        }

        if (!expect_token(parser, TOKEN_LEFT_BRACE)) {
            free_ast_node(sd);
            return NULL;
        }
        while (!match_token(parser, TOKEN_RIGHT_BRACE)) {
            if (is_at_end(parser)) {
                parser_error(parser, "Unexpected end of extern struct definition");
                free_ast_node(sd);
                return NULL;
            }
            ASTNode* field = parse_extern_struct_field(parser);
            if (!field) { free_ast_node(sd); return NULL; }
            add_child(sd, field);
            /* Optional separator. */
            if (!match_token(parser, TOKEN_COMMA)) {
                match_token(parser, TOKEN_SEMICOLON);
            }
        }
        return sd;
    }

    /* `extern const NAME: type @c_import` (#702) — import an object-like
     * C macro (EAGAIN, O_NONBLOCK, REDIS_GIT_SHA1, …) as a typed constant.
     * The declaration teaches the typechecker a name and an Aether type;
     * codegen emits the macro name VERBATIM at every use site and emits
     * nothing for the declaration itself — no value, no forward decl. The
     * including TU's headers are the sole source of truth, so per-platform
     * values come out right by construction. Modelled on AST_CONST_DECLARATION
     * with annotation "c_import_const" and NO initializer child: the const
     * hoist and the statement loop both gate on child_count > 0, so a
     * childless node is skipped end to end; use sites resolve to the bare
     * identifier (= the macro name). Type is trusted as declared, same model
     * as extern functions. Object-like macros only — function-like macros
     * (`CPU_SET(i, &set)`) are out of scope. */
    if (peek_token(parser) && peek_token(parser)->type == TOKEN_CONST) {
        advance_token(parser);  // consume 'const'
        Token* cname = expect_token(parser, TOKEN_IDENTIFIER);
        if (!cname) return NULL;
        if (!expect_token(parser, TOKEN_COLON)) return NULL;
        Type* ctype = parse_type(parser);
        if (!ctype) return NULL;
        /* `@c_import` is required: it is the marker that selects the
         * emit-verbatim-macro semantics. Without it there is no value to
         * emit, so the declaration would be meaningless. */
        int has_c_import = 0;
        if (peek_token(parser) && peek_token(parser)->type == TOKEN_AT) {
            advance_token(parser);  // consume '@'
            Token* attr = peek_token(parser);
            if (attr && attr->type == TOKEN_IDENTIFIER && attr->value &&
                strcmp(attr->value, "c_import") == 0) {
                advance_token(parser);  // consume 'c_import'
                has_c_import = 1;
            }
        }
        if (!has_c_import) {
            parser_error(parser,
                "extern const requires @c_import, it imports an object-like "
                "C macro by name (e.g. `extern const EAGAIN: int @c_import`)");
            free_type(ctype);
            return NULL;
        }
        ASTNode* ec = create_ast_node(AST_CONST_DECLARATION, cname->value,
                                      extern_token->line, extern_token->column);
        ec->annotation = strdup("c_import_const");
        ec->node_type = ctype;
        return ec;
    }

    Token* name = expect_token(parser, TOKEN_IDENTIFIER);
    if (!name) return NULL;

    expect_token(parser, TOKEN_LEFT_PAREN);

    ASTNode* extern_func = create_ast_node(AST_EXTERN_FUNCTION, name->value,
                                           extern_token->line, extern_token->column);

    // Parse parameters with types: param: type, param2: type
    // Trailing `...` marks the extern as variadic (C-style, v1):
    //     extern printf(fmt: string, ...) -> int
    // The `...` may appear as the sole "param" or after a comma
    // following the last named parameter.
    if (!match_token(parser, TOKEN_RIGHT_PAREN)) {
        do {
            // Check for trailing `...`
            if (peek_token(parser) && peek_token(parser)->type == TOKEN_DOTDOTDOT) {
                advance_token(parser);  // consume '...'
                extern_func->annotation =
                    annotation_add_marker(extern_func->annotation, "varargs");
                break;
            }
            Token* param_name = expect_token(parser, TOKEN_IDENTIFIER);
            if (!param_name) break;

            ASTNode* param = create_ast_node(AST_IDENTIFIER, param_name->value,
                                            param_name->line, param_name->column);

            // Require type annotation for extern: param: type
            if (match_token(parser, TOKEN_COLON)) {
                /* Zero or more `@<attr>` markers between `:` and the
                 * type. Currently recognised:
                 *
                 *   @aether — param receives an AetherString header
                 *             rather than the unwrapped const char*.
                 *             Codegen suppresses the call-site
                 *             aether_string_data() unwrap so binary
                 *             content with embedded NULs survives
                 *             the boundary intact. See #351.
                 *
                 *   @retain — the function stores / retains the
                 *             pointer beyond the call (think
                 *             `string_list_add`, `map_put_raw`'s
                 *             key, any add/put/insert that captures
                 *             the bytes). Tells the escape walker
                 *             to mark a heap-string arg as escaped
                 *             at this slot, so the heap-string-
                 *             tracker wrapper and function-exit
                 *             defer-free both skip freeing. Without
                 *             it, default `string`-param treatment
                 *             is "read-only" — correct for
                 *             string.length / equals / println but
                 *             a UAF for retainers. See #420 follow-up.
                 *
                 * Multiple annotations stack: `name: @aether @retain string`
                 * is legal. Order is irrelevant; storage is a
                 * comma-separated set on `param->annotation`. */
                while (peek_token(parser) && peek_token(parser)->type == TOKEN_AT) {
                    advance_token(parser);  // consume '@'
                    Token* attr = peek_token(parser);
                    const char* tag = NULL;
                    if (attr && attr->type == TOKEN_IDENTIFIER && attr->value) {
                        if (strcmp(attr->value, "aether") == 0) {
                            tag = "aether_param";
                            advance_token(parser);
                        } else if (strcmp(attr->value, "retain") == 0) {
                            tag = "retain_param";
                            advance_token(parser);
                        }
                    }
                    if (!tag) {
                        parser_error(parser, "unknown extern-param attribute (expected @aether or @retain)");
                        break;
                    }
                    /* Append to the comma-separated set, deduping. */
                    if (!param->annotation) {
                        param->annotation = strdup(tag);
                    } else if (!strstr(param->annotation, tag)) {
                        size_t old_len = strlen(param->annotation);
                        size_t tag_len = strlen(tag);
                        char* combined = (char*)malloc(old_len + 1 + tag_len + 1);
                        memcpy(combined, param->annotation, old_len);
                        combined[old_len] = ',';
                        memcpy(combined + old_len + 1, tag, tag_len);
                        combined[old_len + 1 + tag_len] = '\0';
                        free(param->annotation);
                        param->annotation = combined;
                    }
                }
                Type* param_type = parse_type(parser);
                if (param_type) {
                    param->node_type = param_type;
                } else {
                    parser_error(parser, "Expected type after ':' in extern parameter");
                    param->node_type = create_type(TYPE_INT);  // Fallback for error recovery
                }
            } else {
                // Type annotation required for extern functions
                parser_error(parser, "Type annotation required for extern parameter (use param: type)");
                param->node_type = create_type(TYPE_INT);  // Fallback for error recovery
            }

            add_child(extern_func, param);
        } while (match_token(parser, TOKEN_COMMA));

        expect_token(parser, TOKEN_RIGHT_PAREN);
    }

    // Parse optional return type: -> type
    if (match_token(parser, TOKEN_ARROW)) {
        Type* return_type = parse_type(parser);
        if (return_type) {
            extern_func->node_type = return_type;
        } else {
            extern_func->node_type = create_type(TYPE_INT);
        }
    } else {
        // No return type = void
        extern_func->node_type = create_type(TYPE_VOID);
    }

    /* `@heap` on a single-value return lives on the extern's annotation slot
     * rather than on its Type: the tuple form uses Type.tuple_heap_flags, and
     * there is no equivalent "this whole thing is heap" slot for a non-tuple
     * return. Unannotated `-> string` stays classified non-heap, which is what
     * keeps the hundreds of existing extern declarations unchanged. */
    parse_extern_trailing_attrs(parser, extern_func);

    return extern_func;
}

ASTNode* parse_function_definition(Parser* parser) {
    // Erlang-style pattern matching functions!
    // Syntax: 
    //   fib(0) -> 1
    //   fib(1) -> 1
    //   fib(n) when n > 1 -> fib(n-1) + fib(n-2)
    // Or traditional:
    //   name(param1, param2) { ... }
    
    Token* name = expect_token(parser, TOKEN_IDENTIFIER);
    if (!name) return NULL;
    
    expect_token(parser, TOKEN_LEFT_PAREN);
    
    ASTNode* func = create_ast_node(AST_FUNCTION_DEFINITION, name->value, name->line, name->column);
    
    // Parse parameters - can be patterns!
    // A trailing `...` after the last named param marks the function as
    // C-variadic: codegen emits a trailing `...` in the C signature and
    // the body reads its varargs via va_start()/va_arg()/va_end(). The
    // marker is recorded as annotation "varargs" (same convention as
    // variadic externs).
    if (!match_token(parser, TOKEN_RIGHT_PAREN)) {
        do {
            if (peek_token(parser) && peek_token(parser)->type == TOKEN_DOTDOTDOT) {
                advance_token(parser);  // consume '...'
                if (func->annotation) free(func->annotation);
                func->annotation = strdup("varargs");
                break;
            }
            ASTNode* param = parse_pattern(parser);
            if (!param) {
                // parse_pattern handles type-first C-style params (`int a`),
                // so it only fails here when the parameter NAME position holds
                // a reserved keyword (a faithful C-port name like
                // `reply`/`after`/`ptr`). That otherwise surfaced as a
                // misleading "Expected RIGHT_PAREN" further down — point at the
                // keyword and teach the backtick escape instead (#867).
                Token* pk = peek_token(parser);
                if (pk && token_is_reserved_keyword(pk) && !parser->suppress_errors) {
                    char emsg[256], hint[160];
                    snprintf(emsg, sizeof(emsg),
                        "'%s' is a reserved keyword and cannot be used as a parameter "
                        "name; rename it (e.g. '%s_'), or escape it as `%s` to use the "
                        "name verbatim", pk->value, pk->value, pk->value);
                    snprintf(hint, sizeof(hint),
                        "rename to '%s_', or write `%s` to keep the name",
                        pk->value, pk->value);
                    aether_error_full(emsg, pk->line, pk->column, hint, NULL, AETHER_ERR_SYNTAX);
                    free_ast_node(func);
                    return NULL;
                }
                break;
            }
            add_child(func, param);
            // #525 gradual contracts: `name: T where <cond>` attaches a
            // runtime precondition on the parameter. Lower to an
            // AST_REQUIRES_CLAUSE on the function — checked at entry (the
            // parameter is in scope) by the same machinery as `requires`.
            // `where` is a contextual keyword: a plain identifier elsewhere,
            // recognised only right after a parameter here.
            {
                Token* w = peek_token(parser);
                if (w && w->type == TOKEN_IDENTIFIER && w->value &&
                    strcmp(w->value, "where") == 0) {
                    int wl = w->line, wc = w->column;
                    advance_token(parser);  // consume 'where'
                    ASTNode* cond = parse_expression(parser);
                    if (!cond) {
                        parser_error(parser, "expected a condition expression after `where`");
                        free_ast_node(func);
                        return NULL;
                    }
                    ASTNode* clause = create_ast_node(AST_REQUIRES_CLAUSE, NULL, wl, wc);
                    add_child(clause, cond);
                    add_child(func, clause);
                }
            }
        } while (match_token(parser, TOKEN_COMMA));

        expect_token(parser, TOKEN_RIGHT_PAREN);
    }
    
    // Check for guard clause: when condition
    ASTNode* guard = NULL;
    if (match_token(parser, TOKEN_WHEN)) {
        ASTNode* guard_expr = parse_expression(parser);
        if (guard_expr) {
            guard = create_ast_node(AST_GUARD_CLAUSE, NULL, 0, 0);
            add_child(guard, guard_expr);
            add_child(func, guard);
        }
    }
    
    // Optional return type annotation: -> type (before arrow body)
    Type* return_type = create_type(TYPE_UNKNOWN);
    Token* next = peek_token(parser);
    if (next && next->type == TOKEN_COLON) {
        advance_token(parser);  // consume ':'
        Type* parsed_type = parse_type(parser);
        if (parsed_type) {
            free_type(return_type);
            return_type = parsed_type;
        }
        next = peek_token(parser);
    }
    func->node_type = return_type;

    // Check for 'with factory' clause (builder functions only)
    if (parser->parsing_builder) {
        Token* maybe_with = peek_token(parser);
        if (maybe_with && maybe_with->type == TOKEN_IDENTIFIER &&
            strcmp(maybe_with->value, "with") == 0) {
            advance_token(parser); // consume 'with'
            Token* factory_tok = expect_token(parser, TOKEN_IDENTIFIER);
            if (factory_tok) {
                func->annotation = strdup(factory_tok->value);
            }
        }
    }

    // Check for Erlang-style arrow body: -> expr OR -> { stmts; expr }
    // OR typed return annotation before a traditional block body:
    //   `name(params) -> ReturnType { ... }` — mirrors the `extern`
    //   signature convention (`extern f(...) -> int`).
    if (match_token(parser, TOKEN_ARROW)) {
        Token* peek = peek_token(parser);
        // Disambiguate `-> ReturnType { body }` from `-> expr`:
        //   If peek is a type keyword (int, string, ptr, etc.) OR an
        //   identifier followed by `{` that isn't the start of a
        //   struct literal (i.e. not `Name { field: value }`), treat
        //   the token(s) between `->` and `{` as a return type and
        //   fall through to the traditional block-body path below.
        int is_typed_return = 0;
        if (peek) {
            switch (peek->type) {
                case TOKEN_INT:
                case TOKEN_INT64:
                case TOKEN_UINT64:
                case TOKEN_DURATION:
                case TOKEN_FLOAT:
                case TOKEN_BOOL:
                case TOKEN_BYTE:
                case TOKEN_STRING:
                case TOKEN_MESSAGE:
                case TOKEN_PTR:
                case TOKEN_ACTOR_REF:
                    is_typed_return = 1;
                    break;
                case TOKEN_MULTIPLY:
                    // `-> *StructName { body }` — the pointer-to-struct
                    // return type. parse_type already handles `*Name`
                    // in any other position (param, var annotation,
                    // struct field, extern decl); the disambiguator
                    // here was the only place it slipped through,
                    // making the body parse as `-> expr` (multiplication)
                    // and breaking with a misleading top-level error
                    // at the `*`. Match the parameter-side surface.
                    is_typed_return = 1;
                    break;
                case TOKEN_IDENTIFIER: {
                    // A C ABI scalar alias (size_t, ssize_t, ...) or the
                    // `longdouble` primitive (#749) in return position is
                    // unambiguously a type.
                    TypeKind alias_k;
                    if (peek->value &&
                        (c_abi_alias_kind(peek->value, &alias_k) ||
                         strcmp(peek->value, "longdouble") == 0)) {
                        is_typed_return = 1;
                        break;
                    }
                    // A parametric return type `-> Name[T] { ... }` (e.g.
                    // `bit_set[Color]`, `Isolated[T]`). The bare-name branch
                    // below only looks one token ahead for `{`, so a `[...]`
                    // group between the name and `{` hid the block body and the
                    // signature fell through to the `-> expr` path (which then
                    // mis-parsed the function body). Scan the balanced bracket
                    // group; a `{` after the closing `]` (that isn't a struct-
                    // literal head) marks a typed return with a block body. An
                    // arrow-expr body like `-> arr[i]` has no trailing `{`, so
                    // it stays an expression. (Fixes bracketed return types
                    // generally, not just bit_set.)
                    if (peek_ahead(parser, 1) &&
                        peek_ahead(parser, 1)->type == TOKEN_LEFT_BRACKET) {
                        int off = 1, depth = 0, guard = 0;
                        while (peek_ahead(parser, off) && guard++ < 256) {
                            AeTokenType tt = peek_ahead(parser, off)->type;
                            if (tt == TOKEN_LEFT_BRACKET) depth++;
                            else if (tt == TOKEN_RIGHT_BRACKET) {
                                depth--;
                                if (depth == 0) { off++; break; }
                            }
                            off++;
                        }
                        // `off` now points just past the closing `]`.
                        Token* after_br = peek_ahead(parser, off);
                        if (after_br && after_br->type == TOKEN_LEFT_BRACE) {
                            Token* ab = peek_ahead(parser, off + 1);
                            Token* af = peek_ahead(parser, off + 2);
                            int looks_like_struct_literal =
                                ab && ab->type == TOKEN_IDENTIFIER &&
                                af && af->type == TOKEN_COLON;
                            if (!looks_like_struct_literal) is_typed_return = 1;
                        }
                        break;
                    }
                    // #946: a qualified return type `-> mod.Name { ... }`
                    // (or multi-segment `a.b.Name`). The bare-name branch
                    // below only looks one token ahead for `{`; skip over a
                    // `.IDENT` chain first so the `{` is found at the right
                    // offset. A dotted name in return position is always a
                    // type (a `-> expr` body never starts `ident . ident`
                    // followed by `{`), so accept it directly.
                    if (peek_ahead(parser, 1) &&
                        peek_ahead(parser, 1)->type == TOKEN_DOT) {
                        int off = 1;
                        while (peek_ahead(parser, off) &&
                               peek_ahead(parser, off)->type == TOKEN_DOT &&
                               peek_ahead(parser, off + 1) &&
                               (peek_ahead(parser, off + 1)->type == TOKEN_IDENTIFIER ||
                                token_is_reserved_keyword(peek_ahead(parser, off + 1)))) {
                            off += 2;
                        }
                        /* `off` now points just past the dotted name. A typed
                         * return is one immediately followed by `{` (block
                         * body) or end-of-signature; a struct-literal head
                         * `{ field:` is still excluded. */
                        Token* after_qual = peek_ahead(parser, off);
                        if (after_qual && after_qual->type == TOKEN_LEFT_BRACE) {
                            Token* after_brace = peek_ahead(parser, off + 1);
                            Token* after_field = peek_ahead(parser, off + 2);
                            int looks_like_struct_literal =
                                after_brace &&
                                after_brace->type == TOKEN_IDENTIFIER &&
                                after_field && after_field->type == TOKEN_COLON;
                            if (!looks_like_struct_literal) is_typed_return = 1;
                        } else {
                            is_typed_return = 1;
                        }
                        break;
                    }
                    // `-> Name { ... }` — only a typed return if what
                    // follows `{` is NOT a struct-literal `field:` head.
                    // `->` is already consumed, so the return-type name is
                    // at offset 0 (peek), `{` at offset 1, and the first
                    // body token at offset 2 (#746: these were 2/3/4,
                    // off by one, so `-> Pair { ... }` never matched and
                    // fell through to the `-> expr` path, leaving `{`
                    // dangling → a top-level parse error).
                    Token* after_name = peek_ahead(parser, 1);
                    if (after_name && after_name->type == TOKEN_LEFT_BRACE) {
                        Token* after_brace = peek_ahead(parser, 2);
                        Token* after_field = peek_ahead(parser, 3);
                        int looks_like_struct_literal =
                            after_brace &&
                            after_brace->type == TOKEN_IDENTIFIER &&
                            after_field && after_field->type == TOKEN_COLON;
                        if (!looks_like_struct_literal) is_typed_return = 1;
                    }
                    break;
                }
                case TOKEN_LEFT_PAREN: {
                    // `-> (T1, T2, ...) { ... }` — parenthesised tuple
                    // return type. Mirrors the form already accepted on
                    // `extern f(...) -> (T1, T2)`. Disambiguate from a
                    // parenthesised arrow-body expression `-> (a + b)` by
                    // requiring a type keyword (or identifier-as-typename)
                    // followed by a comma — only the tuple-type form has
                    // that shape.
                    // peek (offset 0) = `(`, so the first inside-paren
                    // token is offset 1, and the comma after it is offset 2.
                    Token* inner = peek_ahead(parser, 1);
                    Token* after_inner = peek_ahead(parser, 2);
                    if (inner && after_inner && after_inner->type == TOKEN_COMMA) {
                        switch (inner->type) {
                            case TOKEN_INT:
                            case TOKEN_INT64:
                            case TOKEN_UINT64:
                            case TOKEN_DURATION:
                            case TOKEN_FLOAT:
                            case TOKEN_BOOL:
                            case TOKEN_BYTE:
                            case TOKEN_STRING:
                            case TOKEN_MESSAGE:
                            case TOKEN_PTR:
                            case TOKEN_ACTOR_REF:
                            case TOKEN_IDENTIFIER:
                                is_typed_return = 1;
                                break;
                            default:
                                break;
                        }
                    }
                    break;
                }
                default:
                    break;
            }
        }
        if (is_typed_return) {
            // Parse the return type annotation.
            Type* parsed_type = parse_type(parser);
            if (parsed_type) {
                free_type(func->node_type);
                func->node_type = parsed_type;
            }
            // Issue #348 — Eiffel-style contracts. Between the
            // typed return and the body, consume zero or more
            // `requires <expr>` and `ensures <expr>` clauses (in
            // any order, freely interleaved). Each becomes an
            // AST_REQUIRES_CLAUSE or AST_ENSURES_CLAUSE child of
            // the function node, with the predicate expression as
            // its single child. Codegen emits an `if (!(<expr>))
            // aether_panic(...)` at the right scope: function
            // entry for `requires`, before each `return` for
            // `ensures`.
            for (;;) {
                Token* clause_peek = peek_token(parser);
                if (!clause_peek) break;
                if (clause_peek->type != TOKEN_REQUIRES &&
                    clause_peek->type != TOKEN_ENSURES) break;
                ASTNodeType kind = (clause_peek->type == TOKEN_REQUIRES)
                    ? AST_REQUIRES_CLAUSE
                    : AST_ENSURES_CLAUSE;
                int line = clause_peek->line;
                int col  = clause_peek->column;
                advance_token(parser);  // consume the keyword
                ASTNode* expr = parse_expression(parser);
                if (!expr) {
                    parser_error(parser,
                        kind == AST_REQUIRES_CLAUSE
                          ? "expected expression after 'requires'"
                          : "expected expression after 'ensures'");
                    break;
                }
                ASTNode* clause = create_ast_node(kind, NULL, line, col);
                add_child(clause, expr);
                add_child(func, clause);
            }
            // Now expect the traditional block body `{ ... }`.
            ASTNode* body = parse_block(parser);
            if (body) {
                add_child(func, body);
            }
        } else if (peek && peek->type == TOKEN_LEFT_BRACE) {
            // Multi-statement arrow body: -> { stmt1; stmt2; expr }
            // Parse as a block, but treat the last expression as implicit return
            ASTNode* body = parse_block(parser);
            if (body && body->child_count > 0) {
                // Check if the last statement is already a return
                ASTNode* last = body->children[body->child_count - 1];
                if (last->type != AST_RETURN_STATEMENT) {
                    // Wrap last statement/expression as implicit return
                    ASTNode* return_stmt = create_ast_node(AST_RETURN_STATEMENT, NULL, 0, 0);
                    add_child(return_stmt, last);
                    body->children[body->child_count - 1] = return_stmt;
                }
            }
            add_child(func, body);
        } else {
            // Single expression arrow body: -> expr
            ASTNode* body_expr = parse_expression(parser);
            if (body_expr) {
                // Wrap in a return statement
                ASTNode* return_stmt = create_ast_node(AST_RETURN_STATEMENT, NULL, 0, 0);
                add_child(return_stmt, body_expr);

                ASTNode* body_block = create_ast_node(AST_BLOCK, NULL, 0, 0);
                add_child(body_block, return_stmt);
                add_child(func, body_block);
            }
        }
    } else {
        // Traditional block body
        ASTNode* body = parse_block(parser);
        if (body) {
            add_child(func, body);
        }
    }
    
    return func;
}

// Parse pattern for function parameters and match expressions
// Supports: literals (0, "foo"), variables (n), wildcards (_), structs
ASTNode* parse_pattern(Parser* parser) {
    Token* token = peek_token(parser);
    if (!token) return NULL;
    
    switch (token->type) {
        case TOKEN_NUMBER: {
            advance_token(parser);
            ASTNode* pattern = create_ast_node(AST_PATTERN_LITERAL, token->value, 
                                              token->line, token->column);
            pattern->node_type = create_type(TYPE_INT);
            return pattern;
        }
        
        case TOKEN_STRING_LITERAL: {
            advance_token(parser);
            ASTNode* pattern = create_ast_node(AST_PATTERN_LITERAL, token->value,
                                              token->line, token->column);
            pattern->node_type = create_type(TYPE_STRING);
            return pattern;
        }
        
        case TOKEN_TRUE:
        case TOKEN_FALSE: {
            advance_token(parser);
            ASTNode* pattern = create_ast_node(AST_PATTERN_LITERAL, token->value,
                                              token->line, token->column);
            pattern->node_type = create_type(TYPE_BOOL);
            return pattern;
        }
        
        // #880: `func`/`state`/`after` are keywords only as declaration /
        // statement heads, never types — so in pattern / parameter-NAME
        // position they are unambiguously a binding name (their `->value`
        // holds the spelling). Accept them here as a variable pattern.
        // (`ptr`/`byte` ARE type keywords, so they share the C-style
        // typed-parameter block below, which disambiguates `byte b` (type)
        // from `byte: int` / `byte` (name) by the next token.) `match`
        // (expression head) and `union` (a C keyword) stay reserved (#880).
        case TOKEN_FUNC:
        case TOKEN_STATE:
        case TOKEN_AFTER:
        case TOKEN_IDENTIFIER: {
            // Check if it's a wildcard _
            if (token->type == TOKEN_IDENTIFIER && strcmp(token->value, "_") == 0) {
                advance_token(parser);
                ASTNode* pattern = create_ast_node(AST_PATTERN_LITERAL, "_",
                                                  token->line, token->column);
                pattern->node_type = create_type(TYPE_WILDCARD);
                return pattern;
            }

            // Check if it's a struct pattern: Point{x: 0, y: 0} (real
            // identifiers only — a keyword token is never a struct name).
            Token* next = peek_ahead(parser, 1);
            if (token->type == TOKEN_IDENTIFIER && next && next->type == TOKEN_LEFT_BRACE) {
                return parse_struct_pattern(parser);
            }

            // Regular variable pattern
            advance_token(parser);
            ASTNode* pattern = create_ast_node(AST_PATTERN_VARIABLE, token->value,
                                              token->line, token->column);

            // Optional type annotation: param: type
            if (match_token(parser, TOKEN_COLON)) {
                Type* param_type = parse_type(parser);
                if (param_type) {
                    pattern->node_type = param_type;
                } else {
                    pattern->node_type = create_type(TYPE_UNKNOWN);
                }
            } else {
                pattern->node_type = create_type(TYPE_UNKNOWN);
            }

            // Optional default value: param: type = expr  (issue #265
            // / Phase A2.1 — default function arguments). The default
            // expression is stored as the first child of the
            // pattern, with annotation="has_default" so consumers
            // (typechecker, codegen) can distinguish it from
            // pattern-children used by struct/list patterns elsewhere.
            // A default expression is evaluated in the enclosing scope, with
            // the other parameters NOT in scope, so a default that names
            // another parameter resolves as an undefined variable (a clear
            // E0300 at type-check) rather than that parameter's value.
            if (match_token(parser, TOKEN_ASSIGN)) {
                ASTNode* default_expr = parse_expression(parser);
                if (default_expr) {
                    add_child(pattern, default_expr);
                    if (pattern->annotation) free(pattern->annotation);
                    pattern->annotation = strdup("has_default");
                }
            }

            return pattern;
        }
        
        case TOKEN_LEFT_BRACKET: {
            // List pattern: [], [x], [H|T]
            return parse_list_pattern(parser);
        }

        // C-style typed parameters: int a, float b, string s, etc.
        case TOKEN_INT:
        case TOKEN_INT64:
        case TOKEN_UINT64:
        case TOKEN_DURATION:
        case TOKEN_FLOAT:
        case TOKEN_BOOL:
        case TOKEN_BYTE:
        case TOKEN_STRING:
        case TOKEN_PTR: {
            // Check if next token is an identifier (type name pattern)
            Token* next = peek_ahead(parser, 1);
            if (next && next->type == TOKEN_IDENTIFIER) {
                Type* param_type = parse_type(parser);  // consume type token
                Token* pname = expect_token(parser, TOKEN_IDENTIFIER);
                if (!pname) { if (param_type) free_type(param_type); return NULL; }
                ASTNode* pattern = create_ast_node(AST_PATTERN_VARIABLE, pname->value,
                                                   pname->line, pname->column);
                pattern->node_type = param_type ? param_type : create_type(TYPE_UNKNOWN);
                // Default value (Phase A2.1) — same shape as the
                // identifier-with-type-annotation branch above.
                if (match_token(parser, TOKEN_ASSIGN)) {
                    ASTNode* default_expr = parse_expression(parser);
                    if (default_expr) {
                        add_child(pattern, default_expr);
                        if (pattern->annotation) free(pattern->annotation);
                        pattern->annotation = strdup("has_default");
                    }
                }
                return pattern;
            }
            // #880: `byte`/`ptr` are type keywords that double as natural value
            // identifiers in C ports. When NOT followed by an identifier they
            // are the parameter NAME, not a type — `f(byte: int)`, `f(ptr)`,
            // `f(ptr = 0)`. (The `<type> <name>` C-style form above already
            // consumed the `next == identifier` case.) The other type keywords
            // in this group (int/float/...) are not value identifiers, so they
            // keep falling through to expression parsing.
            if (token->type == TOKEN_BYTE || token->type == TOKEN_PTR) {
                advance_token(parser);
                ASTNode* pattern = create_ast_node(AST_PATTERN_VARIABLE, token->value,
                                                   token->line, token->column);
                if (match_token(parser, TOKEN_COLON)) {
                    Type* param_type = parse_type(parser);
                    pattern->node_type = param_type ? param_type : create_type(TYPE_UNKNOWN);
                } else {
                    pattern->node_type = create_type(TYPE_UNKNOWN);
                }
                if (match_token(parser, TOKEN_ASSIGN)) {
                    ASTNode* default_expr = parse_expression(parser);
                    if (default_expr) {
                        add_child(pattern, default_expr);
                        if (pattern->annotation) free(pattern->annotation);
                        pattern->annotation = strdup("has_default");
                    }
                }
                return pattern;
            }
            // Fall through to expression parsing
            return parse_expression(parser);
        }

        default:
            // Fallback to expression
            return parse_expression(parser);
    }
}

// Parse struct pattern: Point{x: 0, y: _}
ASTNode* parse_struct_pattern(Parser* parser) {
    Token* name = expect_token(parser, TOKEN_IDENTIFIER);
    if (!name) return NULL;
    
    if (!expect_token(parser, TOKEN_LEFT_BRACE)) return NULL;

    ASTNode* pattern = create_ast_node(AST_PATTERN_STRUCT, name->value,
                                      name->line, name->column);

    if (!match_token(parser, TOKEN_RIGHT_BRACE)) {
        do {
            // #880: accept value-identifier keyword field names when
            // destructuring (`Box{ ptr: p }`).
            Token* field = peek_token(parser);
            if (field && token_is_value_ident(field)) {
                advance_token(parser);
            } else {
                field = expect_token(parser, TOKEN_IDENTIFIER);
                if (!field) break;
            }

            if (!expect_token(parser, TOKEN_COLON)) break;

            ASTNode* field_pattern = parse_pattern(parser);
            if (field_pattern) {
                // Store field name in pattern
                ASTNode* field_node = create_ast_node(AST_ASSIGNMENT, field->value,
                                                     field->line, field->column);
                add_child(field_node, field_pattern);
                add_child(pattern, field_node);
            }
        } while (match_token(parser, TOKEN_COMMA));
        
        expect_token(parser, TOKEN_RIGHT_BRACE);
    }
    
    return pattern;
}

// Parse list pattern: [], [x], [x, y], [H|T]
ASTNode* parse_list_pattern(Parser* parser) {
    Token* bracket = expect_token(parser, TOKEN_LEFT_BRACKET);
    if (!bracket) return NULL;
    
    // Empty list: []
    if (match_token(parser, TOKEN_RIGHT_BRACKET)) {
        ASTNode* pattern = create_ast_node(AST_PATTERN_LIST, "[]", 
                                          bracket->line, bracket->column);
        pattern->node_type = create_array_type(create_type(TYPE_UNKNOWN), -1);
        return pattern;
    }
    
    // Parse first element
    ASTNode* first = parse_pattern(parser);
    if (!first) return NULL;
    
    // Check for cons pattern: [H|T]
    if (match_token(parser, TOKEN_PIPE)) {
        ASTNode* tail = parse_pattern(parser);
        if (!tail) return NULL;
        
        expect_token(parser, TOKEN_RIGHT_BRACKET);
        
        // Create cons pattern node
        ASTNode* cons = create_ast_node(AST_PATTERN_CONS, "[|]", 
                                       bracket->line, bracket->column);
        cons->node_type = create_array_type(create_type(TYPE_UNKNOWN), -1);
        add_child(cons, first);   // Head
        add_child(cons, tail);    // Tail
        return cons;
        }
    
    // Regular list pattern: [x, y, z]
    ASTNode* list = create_ast_node(AST_PATTERN_LIST, "[]",
                                   bracket->line, bracket->column);
    list->node_type = create_array_type(create_type(TYPE_UNKNOWN), -1);
    add_child(list, first);
    
    while (match_token(parser, TOKEN_COMMA)) {
        ASTNode* elem = parse_pattern(parser);
        if (!elem) break;
        add_child(list, elem);
    }
    
    expect_token(parser, TOKEN_RIGHT_BRACKET);
    return list;
}

ASTNode* parse_main_function(Parser* parser) {
    advance_token(parser); // main
    expect_token(parser, TOKEN_LEFT_PAREN);
    expect_token(parser, TOKEN_RIGHT_PAREN);

    ASTNode* main = create_ast_node(AST_MAIN_FUNCTION, "main", 0, 0);
    main->node_type = create_type(TYPE_VOID);

    ASTNode* body = parse_block(parser);
    if (body) {
        add_child(main, body);
    }

    return main;
}

ASTNode* parse_struct_definition(Parser* parser) {
    Token* struct_token = advance_token(parser); // consume 'struct'
    
    Token* name_token = expect_token(parser, TOKEN_IDENTIFIER);
    if (!name_token) return NULL;
    
    ASTNode* struct_def = create_ast_node(AST_STRUCT_DEFINITION, name_token->value, 
                                         struct_token->line, struct_token->column);
    
    /* Past this point the definition (and every field attached to it) belongs
     * to this function until it is returned, so each error path releases it. */
    if (!expect_token(parser, TOKEN_LEFT_BRACE)) { free_ast_node(struct_def); return NULL; }

    // Parse fields (types optional - will be inferred!)
    while (!match_token(parser, TOKEN_RIGHT_BRACE)) {
        if (is_at_end(parser)) {
            parser_error(parser, "Unexpected end of struct definition");
            free_ast_node(struct_def);
            return NULL;
        }

        /* #1048 field injection: `using name: Type` embeds a sub-struct and
         * promotes its fields into the outer namespace (`f.x` resolves through
         * `f.name.x`). Contextual keyword: only `using <ident>` at a field
         * position is intercepted; the rest parses as a normal field, and the
         * field is tagged so member-access resolution can search through it. */
        int is_using_field = 0;
        {
            Token* uw = peek_token(parser);
            if (uw && uw->type == TOKEN_IDENTIFIER && uw->value &&
                strcmp(uw->value, "using") == 0) {
                Token* after = peek_ahead(parser, 1);
                if (after && after->type == TOKEN_IDENTIFIER) {
                    advance_token(parser);   // consume 'using'
                    is_using_field = 1;
                }
            }
        }

        /* Two field syntaxes accepted:
         *   Aether-style: `name: type` (or just `name` for inferred)
         *   C-style:      `int name`, `string name`, etc.
         * The C-style form is convenient for users porting C/C++
         * structs. Without this branch, parse expects an
         * identifier first and `int x` triggers the reserved-
         * keyword error. */
        Token* peek = peek_token(parser);
        Type* c_type = NULL;
        if (peek && (peek->type == TOKEN_INT  || peek->type == TOKEN_INT64 ||
                     peek->type == TOKEN_UINT64 ||
                     peek->type == TOKEN_DURATION ||
                     peek->type == TOKEN_FLOAT || peek->type == TOKEN_BOOL  ||
                     peek->type == TOKEN_BYTE  ||
                     peek->type == TOKEN_STRING || peek->type == TOKEN_PTR)) {
            Token* ahead = peek_ahead(parser, 1);
            if (ahead && ahead->type == TOKEN_IDENTIFIER) {
                c_type = parse_type(parser);
            }
        }

        // #880: accept the value-identifier keywords (`ptr`/`byte`/`func`/
        // `state`/`after`) as field NAMES — C ports routinely have struct
        // fields so named, and each is a valid C struct-field identifier. The
        // `<type> <name>` C-style form above already consumed the case where
        // such a keyword is the field TYPE (e.g. `byte b`), so a keyword
        // remaining here is the name. Anything else falls to the standard
        // identifier error.
        Token* field_name = peek_token(parser);
        if (field_name && token_is_value_ident(field_name)) {
            advance_token(parser);
        } else {
            field_name = expect_token(parser, TOKEN_IDENTIFIER);
            if (!field_name) {
                if (c_type) free_type(c_type);
                free_ast_node(struct_def);
                return NULL;
            }
        }

        // Create field node
        ASTNode* field = create_ast_node(AST_STRUCT_FIELD, field_name->value,
                                        field_name->line, field_name->column);
        if (is_using_field) field->annotation = strdup("using");  // #1048

        if (c_type) {
            field->node_type = c_type;
        } else if (match_token(parser, TOKEN_COLON)) {
            // Aether-style: name: type
            Type* field_type = parse_type(parser);
            if (field_type) {
                field->node_type = field_type;
            }
        } else {
            // No type - will be inferred from usage
            field->node_type = create_type(TYPE_UNKNOWN);
        }
        
        add_child(struct_def, field);
        
        // Optional comma or semicolon
        if (!match_token(parser, TOKEN_COMMA)) {
            match_token(parser, TOKEN_SEMICOLON);  // Optional semicolon
        }
    }
    
    return struct_def;
}

/* #1132 — the width in bits of a bitstruct's backing integer, or 0 if `t` is not
 * a legal backing type. Only the unsigned fixed-width C ABI aliases qualify: the
 * storage must have an exact width AND be unsigned, since the entire point is to
 * escape C bitfields, whose signedness is implementation-defined (a plain `int
 * x : 3` field is signed on gcc, so a 3-bit value of 0b111 reads back as -1).
 * That is the bug this feature exists to kill; permitting a signed backing type
 * would reintroduce it. */
static int bitstruct_backing_bits(const Type* t) {
    if (!t) return 0;
    switch (t->kind) {
        case TYPE_UINT8:  return 8;
        case TYPE_UINT16: return 16;
        case TYPE_UINT32: return 32;
        case TYPE_UINT64: return 64;
        default: return 0;
    }
}

/* #1132 — `bitstruct Name : uint8_t { flag: bool 0, kind: int 1..=3 }`.
 * Called with `bitstruct` already consumed.
 *
 * Each field is `name: type <bits>`, where `<bits>` is either a single index
 * (`0`) or a range. The range may be written inclusively (`1..=3`) or exclusively
 * (`1..<4`) — both denote the same three bits. C3, which this borrows from, hard-
 * codes inclusive ranges and leaves the reader to remember; Aether already has
 * `..=` / `..<` (used by match labels, #1047), so we make the source say which it
 * means and normalise to an inclusive [bit_lo, bit_hi] pair here. Codegen never
 * has to care which spelling was used.
 *
 * Overlapping fields are rejected unless the bitstruct carries `@overlap`, and a
 * range that runs off the end of the backing integer is always an error. */
static ASTNode* parse_bitstruct_definition(Parser* parser) {
    Token* name = expect_token(parser, TOKEN_IDENTIFIER);
    if (!name) return NULL;
    if (!expect_token(parser, TOKEN_COLON)) return NULL;

    Type* backing = parse_type(parser);
    int total_bits = bitstruct_backing_bits(backing);
    if (!total_bits) {
        parser_error(parser,
            "bitstruct backing type must be uint8_t, uint16_t, uint32_t or uint64_t "
            "(an exact-width unsigned integer)");
        if (backing) free_type(backing);
        return NULL;
    }

    ASTNode* def = create_ast_node(AST_BITSTRUCT_DEFINITION, name->value,
                                   name->line, name->column);
    def->node_type = backing;

    /* Optional `@overlap` — permits fields to share bits (a union-like view). */
    int allow_overlap = 0;
    Token* at = peek_token(parser);
    if (at && at->type == TOKEN_AT) {
        Token* a1 = peek_ahead(parser, 1);
        if (a1 && a1->type == TOKEN_IDENTIFIER && a1->value &&
            strcmp(a1->value, "overlap") == 0) {
            advance_token(parser);   // '@'
            advance_token(parser);   // 'overlap'
            allow_overlap = 1;
            def->annotation = strdup("overlap");
        }
    }

    if (!expect_token(parser, TOKEN_LEFT_BRACE)) { free_ast_node(def); return NULL; }

    /* Bit i of the backing integer is claimed by an already-parsed field. Used
     * for the overlap check; 64 bits is the widest backing type. */
    unsigned long long claimed = 0ULL;

    while (peek_token(parser) && peek_token(parser)->type != TOKEN_RIGHT_BRACE) {
        Token* fname = expect_token(parser, TOKEN_IDENTIFIER);
        if (!fname) { free_ast_node(def); return NULL; }
        if (!expect_token(parser, TOKEN_COLON)) { free_ast_node(def); return NULL; }

        Type* ftype = parse_type(parser);
        if (!ftype) {
            parser_error(parser, "expected a field type in bitstruct");
            free_ast_node(def);
            return NULL;
        }
        /* Members are integers or bools only — a float or string has no meaning
         * as a bit range, and a struct field would need a layout of its own. */
        if (!(ftype->kind == TYPE_BOOL || ftype->kind == TYPE_INT ||
              ftype->kind == TYPE_INT64 || ftype->kind == TYPE_UINT8 ||
              ftype->kind == TYPE_UINT16 || ftype->kind == TYPE_UINT32 ||
              ftype->kind == TYPE_UINT64 || ftype->kind == TYPE_ENUM)) {
            parser_error(parser,
                "bitstruct fields must be bool, an integer type, or an enum");
            free_type(ftype);
            free_ast_node(def);
            return NULL;
        }

        Token* lo_tok = expect_token(parser, TOKEN_NUMBER);
        if (!lo_tok || !lo_tok->value) {
            parser_error(parser,
                "expected a bit position or range after the field type "
                "(e.g. `f: bool 0` or `g: int 1..=3`)");
            free_type(ftype); free_ast_node(def);
            return NULL;
        }
        int lo = atoi(lo_tok->value);
        int hi = lo;   /* a bare index is a one-bit field */

        Token* r = peek_token(parser);
        if (r && (r->type == TOKEN_DOTDOT_EQ || r->type == TOKEN_DOTDOT_LT)) {
            int inclusive = (r->type == TOKEN_DOTDOT_EQ);
            advance_token(parser);
            Token* hi_tok = expect_token(parser, TOKEN_NUMBER);
            if (!hi_tok || !hi_tok->value) {
                parser_error(parser, "expected the high bit of the range");
                free_type(ftype); free_ast_node(def);
                return NULL;
            }
            hi = atoi(hi_tok->value);
            /* Normalise the half-open spelling to the inclusive pair we store. */
            if (!inclusive) hi -= 1;
        }

        if (hi < lo) {
            parser_error(parser, "bitstruct field range is empty or inverted "
                                 "(high bit is below the low bit)");
            free_type(ftype); free_ast_node(def);
            return NULL;
        }
        if (lo < 0 || hi >= total_bits) {
            char msg[192];
            snprintf(msg, sizeof(msg),
                     "bitstruct field bits %d..=%d do not fit in the %d-bit backing type",
                     lo, hi, total_bits);
            parser_error(parser, msg);
            free_type(ftype); free_ast_node(def);
            return NULL;
        }
        /* A bool occupies exactly one bit; anything wider is a mistake the
         * programmer wants to hear about, not a silent truncation. */
        if (ftype->kind == TYPE_BOOL && hi != lo) {
            parser_error(parser, "a bool bitstruct field must be exactly one bit wide");
            free_type(ftype); free_ast_node(def);
            return NULL;
        }

        unsigned long long span = (hi - lo + 1 >= 64)
            ? ~0ULL
            : (((1ULL << (hi - lo + 1)) - 1ULL) << lo);
        if (!allow_overlap && (claimed & span)) {
            parser_error(parser,
                "bitstruct fields overlap; annotate the bitstruct with `@overlap` "
                "if that is intended");
            free_type(ftype); free_ast_node(def);
            return NULL;
        }
        claimed |= span;

        ASTNode* field = create_ast_node(AST_BITSTRUCT_FIELD, fname->value,
                                         fname->line, fname->column);
        field->node_type = ftype;
        field->bit_lo = lo;
        field->bit_hi = hi;
        add_child(def, field);

        if (peek_token(parser) && peek_token(parser)->type == TOKEN_COMMA) {
            advance_token(parser);   // optional comma
        }
    }

    if (!expect_token(parser, TOKEN_RIGHT_BRACE)) { free_ast_node(def); return NULL; }
    return def;
}

// Parse a single top-level declaration (module / import / func / struct /
// extern / const / etc.). Returns the parsed node, or NULL when the item
// was consumed by error recovery (the parser has advanced past it). Factored
// out of parse_program so the top-level `when` static-if (issue #483) can
// parse declarations inside its arms with the exact same grammar — an extern
// or import gated behind `when` parses identically to a top-level one.
ASTNode* parse_top_level_decl(Parser* parser) {
    {
        Token* token = peek_token(parser);
        if (!token) return NULL;

        ASTNode* node = NULL;

        // #1132: `bitstruct Name : uint8_t { flag: bool 0, kind: int 1..=3 }`.
        // A layout-exact, endianness-independent replacement for C bitfields.
        // `bitstruct` is a contextual identifier (still usable as a name
        // elsewhere); only the `bitstruct <ident> :` shape here is intercepted.
        if (token->type == TOKEN_IDENTIFIER && token->value &&
            strcmp(token->value, "bitstruct") == 0) {
            Token* n1 = peek_ahead(parser, 1);
            Token* n2 = peek_ahead(parser, 2);
            if (n1 && n1->type == TOKEN_IDENTIFIER && n2 && n2->type == TOKEN_COLON) {
                advance_token(parser);   // consume 'bitstruct'
                return parse_bitstruct_definition(parser);
            }
            /* `bitstruct Name {` — the backing type is not optional. Diagnose it
             * here rather than letting it fall through to be parsed as something
             * else and produce a baffling error. The whole point of the feature is
             * that the storage is named explicitly, so its width and signedness
             * are never implementation-defined (as a C bitfield's are). */
            if (n1 && n1->type == TOKEN_IDENTIFIER &&
                n2 && n2->type == TOKEN_LEFT_BRACE) {
                parser_error(parser,
                    "bitstruct requires an explicit backing type: "
                    "`bitstruct Name : uint8_t { ... }` (uint8_t/uint16_t/uint32_t/uint64_t)");
                return NULL;
            }
        }

        // #1044: `enum Name { A, B = 5, C }`, a first-class named-integer enum.
        // `enum` is a contextual identifier (usable as a name elsewhere); only
        // the `enum <ident> {` shape here is intercepted. Members are separated
        // by commas and/or newlines; an optional `= <expr>` sets an explicit
        // value (otherwise it is the previous value + 1, first defaulting to 0).
        if (token->type == TOKEN_IDENTIFIER && token->value &&
            strcmp(token->value, "enum") == 0) {
            Token* n1 = peek_ahead(parser, 1);
            Token* n2 = peek_ahead(parser, 2);
            if (n1 && n1->type == TOKEN_IDENTIFIER &&
                n2 && n2->type == TOKEN_LEFT_BRACE) {
                advance_token(parser);   // consume 'enum'
                Token* ename = expect_token(parser, TOKEN_IDENTIFIER);
                if (!ename) return NULL;
                if (!expect_token(parser, TOKEN_LEFT_BRACE)) return NULL;
                ASTNode* edef = create_ast_node(AST_ENUM_DEFINITION, ename->value,
                                                ename->line, ename->column);
                while (peek_token(parser) &&
                       peek_token(parser)->type != TOKEN_RIGHT_BRACE) {
                    Token* m = expect_token(parser, TOKEN_IDENTIFIER);
                    if (!m) { free_ast_node(edef); return NULL; }
                    ASTNode* member = create_ast_node(AST_ENUM_MEMBER, m->value,
                                                      m->line, m->column);
                    if (peek_token(parser) && peek_token(parser)->type == TOKEN_ASSIGN) {
                        advance_token(parser);   // consume '='
                        ASTNode* v = parse_expression(parser);
                        if (!v) { free_ast_node(edef); return NULL; }
                        add_child(member, v);
                    }
                    add_child(edef, member);
                    if (peek_token(parser) && peek_token(parser)->type == TOKEN_COMMA) {
                        advance_token(parser);   // optional comma
                    }
                }
                if (!expect_token(parser, TOKEN_RIGHT_BRACE)) {
                    free_ast_node(edef); return NULL;
                }
                return edef;
            }
        }

        // #error-unification P3: `fault NotFound, PermissionDenied, ...` — a
        // set of named error identities. `fault` is a contextual identifier
        // (usable as a name elsewhere); only the `fault <ident>` shape here is
        // intercepted (an identifier immediately following `fault`, which no
        // ordinary statement/decl produces at top level). Members are bare
        // identifiers separated by commas and/or newlines.
        if (token->type == TOKEN_IDENTIFIER && token->value &&
            strcmp(token->value, "fault") == 0) {
            Token* n1 = peek_ahead(parser, 1);
            if (n1 && n1->type == TOKEN_IDENTIFIER) {
                advance_token(parser);   // consume 'fault'
                ASTNode* fdef = create_ast_node(AST_FAULT_DEFINITION, NULL,
                                                token->line, token->column);
                /* Comma-separated member list on one logical line (the lexer
                 * does not surface newlines as tokens, so commas are the
                 * separator — matching the documented `fault A, B, C` form).
                 * A member is a bare identifier; the list ends at the first
                 * token that is not `<comma> <identifier>`. */
                for (;;) {
                    Token* m = peek_token(parser);
                    if (!m || m->type != TOKEN_IDENTIFIER) break;
                    ASTNode* member = create_ast_node(AST_IDENTIFIER, m->value,
                                                      m->line, m->column);
                    /* children[0] holds the member's interned string CONTENT —
                     * its bare name for now; the module-merge namespace pass
                     * rewrites it to the qualified `"<ns>.<name>"`. Codegen
                     * emits `static const char <name>[] = <content>;`. */
                    ASTNode* content = create_ast_node(AST_LITERAL, m->value,
                                                       m->line, m->column);
                    content->node_type = create_type(TYPE_STRING);
                    add_child(member, content);
                    add_child(fdef, member);
                    advance_token(parser);   // consume member name
                    if (peek_token(parser) &&
                        peek_token(parser)->type == TOKEN_COMMA) {
                        advance_token(parser);   // consume ',' and continue
                    } else {
                        break;                   // no comma → member list ended
                    }
                }
                if (fdef->child_count == 0) {
                    parser_error(parser,
                        "`fault` requires at least one member "
                        "(`fault NotFound, PermissionDenied, ...`)");
                    return NULL;
                }
                return fdef;
            }
        }

        // #480: `type Name = distinct Base` — a zero-cost nominal type over
        // Base. `type` and `distinct` are contextual identifiers (usable as
        // names elsewhere); only the `type <ident> = distinct ...` shape here
        // is intercepted.
        if (token->type == TOKEN_IDENTIFIER && token->value &&
            strcmp(token->value, "type") == 0) {
            Token* n1 = peek_ahead(parser, 1);
            Token* n2 = peek_ahead(parser, 2);
            if (n1 && n1->type == TOKEN_IDENTIFIER &&
                n2 && n2->type == TOKEN_ASSIGN) {
                advance_token(parser);                       // consume 'type'
                Token* name = expect_token(parser, TOKEN_IDENTIFIER);
                if (!name) return NULL;
                if (!expect_token(parser, TOKEN_ASSIGN)) return NULL;
                Token* dk = peek_token(parser);
                if (dk && dk->type == TOKEN_IDENTIFIER && dk->value &&
                    strcmp(dk->value, "distinct") == 0) {
                    advance_token(parser);                   // consume 'distinct'
                    Type* base = parse_type(parser);
                    if (!base) {
                        parser_error(parser, "expected a base type after `distinct`");
                        return NULL;
                    }
                    ASTNode* d = create_ast_node(AST_DISTINCT_TYPE_DEF,
                                                 name->value, name->line, name->column);
                    d->node_type = base;
                    match_token(parser, TOKEN_SEMICOLON);
                    return d;
                }
                // #914 sum/variant type: `type Name = A | B | C`. The variants
                // are existing struct type names separated by `|`. At least two
                // are required (a single-name alias is not a supported form).
                if (dk && dk->type == TOKEN_IDENTIFIER) {
                    ASTNode* sum = create_ast_node(AST_SUM_TYPE_DEF,
                                                   name->value, name->line, name->column);
                    int nvar = 0;
                    for (;;) {
                        Token* v = expect_token(parser, TOKEN_IDENTIFIER);
                        if (!v) { free_ast_node(sum); return NULL; }
                        add_child(sum, create_ast_node(AST_IDENTIFIER, v->value,
                                                       v->line, v->column));
                        nvar++;
                        if (peek_token(parser) && peek_token(parser)->type == TOKEN_PIPE) {
                            advance_token(parser);           // consume '|'
                            continue;
                        }
                        break;
                    }
                    if (nvar < 2) {
                        parser_error(parser,
                            "a sum type needs at least two variants "
                            "(`type Name = A | B`); a single-name alias is not supported");
                        free_ast_node(sum);
                        return NULL;
                    }
                    match_token(parser, TOKEN_SEMICOLON);
                    return sum;
                }
                parser_error(parser,
                    "expected `distinct <type>` or a `|`-separated variant list "
                    "after `type Name =`");
                return NULL;
            }
        }

        switch (token->type) {
            case TOKEN_WHEN: {
                // Top-level static-if: its arms hold declarations.
                int saved = parser->when_top_level;
                parser->when_top_level = 1;
                ASTNode* w = parse_when_statement(parser);
                parser->when_top_level = saved;
                return w;
            }
            case TOKEN_MODULE:
                node = parse_module_declaration(parser);
                break;
            case TOKEN_IMPORT:
                node = parse_import_statement(parser);
                break;
            case TOKEN_EXPORT:
                node = parse_export_statement(parser);
                break;
            case TOKEN_EXPORTS:
                node = parse_exports_list(parser);
                break;
            case TOKEN_ACTOR:
                node = parse_actor_definition(parser);
                break;
            case TOKEN_MESSAGE_KEYWORD:
                node = parse_message_definition(parser);
                break;
            case TOKEN_FUNC:
                // 'func' keyword is optional but still supported
                advance_token(parser);
                node = parse_function_definition(parser);
                break;
            case TOKEN_STRUCT:
                node = parse_struct_definition(parser);
                break;
            case TOKEN_EXTERN:
                node = parse_extern_declaration(parser);
                break;
            case TOKEN_AT: {
                // @extern("c_symbol_name") aether_name(params) -> ret
                //
                // Binds an Aether-namespace function name to a chosen
                // C symbol. The Aether name lives in the module's
                // public surface (qualified callers write
                // `module.aether_name(...)`); codegen forwards every
                // call to the named C symbol. No wrapper function is
                // emitted — one annotation, no thunk.
                //
                // Equivalent to writing:
                //     extern c_symbol_name(...)
                //     aether_name(...) { return c_symbol_name(...) }
                // …without the wrapper. Closes #234.
                Token* at_tok = expect_token(parser, TOKEN_AT);
                if (!at_tok) { advance_token(parser); return NULL; }
                // Two attribute forms accepted at top-level:
                //   @extern("c_symbol") aether_name(params) -> ret    (#234)
                //   @c_callback aether_name(params) -> ret { body }   (#235)
                //   @c_callback("c_symbol") aether_name(...) {body}   (#235, explicit name)
                // The lexer classifies `extern` as TOKEN_EXTERN (reserved
                // keyword); `c_callback` is TOKEN_IDENTIFIER.
                Token* attr = peek_token(parser);
                // #891 @c_struct Name { field: type @offset, ... }
                // A pure-Aether typed overlay over a raw ptr. Each field
                // carries an explicit byte offset; access on an
                // `expr as *Name` value lowers to a width-correct
                // mem_get_*/set_* at that offset (no C struct is declared).
                if (attr && attr->type == TOKEN_IDENTIFIER && attr->value &&
                    strcmp(attr->value, "link") == 0) {
                    /* #1259: @link("-lfoo -lbar") — this module's native
                     * link dependencies. Carried as an AST_LINK_DIRECTIVE
                     * whose value is the verbatim flag string; codegen
                     * unions them across the import closure. */
                    advance_token(parser);  // consume 'link'
                    if (!expect_token(parser, TOKEN_LEFT_PAREN)) return NULL;
                    Token* flags = peek_token(parser);
                    if (!flags || flags->type != TOKEN_STRING_LITERAL || !flags->value) {
                        parser_error(parser, "@link expects a string of linker flags, e.g. @link(\"-lsqlite3\")");
                        return NULL;
                    }
                    advance_token(parser);
                    if (!expect_token(parser, TOKEN_RIGHT_PAREN)) return NULL;
                    return create_ast_node(AST_LINK_DIRECTIVE, flags->value,
                                           at_tok->line, at_tok->column);
                }
                                if (attr && attr->type == TOKEN_IDENTIFIER && attr->value &&
                    strcmp(attr->value, "c_struct") == 0) {
                    advance_token(parser);  // consume 'c_struct'
                    Token* name_tok = expect_token(parser, TOKEN_IDENTIFIER);
                    if (!name_tok) return NULL;
                    ASTNode* cdef = create_ast_node(AST_C_STRUCT_DEF,
                        name_tok->value, name_tok->line, name_tok->column);
                    /* Optional `@c_verify` (#1242): a C type of the same name
                     * is in scope, so codegen emits a _Static_assert per field
                     * checking the declared offset and width against the real
                     * header layout. Without it the offsets are asserted by the
                     * author and nothing catches upstream layout drift. */
                    if (peek_token(parser) && peek_token(parser)->type == TOKEN_AT) {
                        Token* vtag = peek_ahead(parser, 1);
                        if (vtag && vtag->type == TOKEN_IDENTIFIER && vtag->value &&
                            strcmp(vtag->value, "c_verify") == 0) {
                            advance_token(parser);  // consume '@'
                            advance_token(parser);  // consume 'c_verify'
                            cdef->annotation = strdup("c_verify");
                        } else {
                            parser_error(parser,
                                "unknown @c_struct attribute (expected @c_verify)");
                            free_ast_node(cdef);
                            return NULL;
                        }
                    }
                    if (!expect_token(parser, TOKEN_LEFT_BRACE)) {
                        free_ast_node(cdef);
                        return NULL;
                    }
                    while (!match_token(parser, TOKEN_RIGHT_BRACE)) {
                        if (is_at_end(parser)) {
                            parser_error(parser, "unterminated @c_struct body (missing `}`)");
                            free_ast_node(cdef);
                            return NULL;
                        }
                        // field name (accept value-ident keywords as names)
                        Token* fn = peek_token(parser);
                        if (fn && token_is_value_ident(fn)) advance_token(parser);
                        else { fn = expect_token(parser, TOKEN_IDENTIFIER);
                               if (!fn) { free_ast_node(cdef); return NULL; } }
                        if (!expect_token(parser, TOKEN_COLON)) { free_ast_node(cdef); return NULL; }
                        ASTNode* field = create_ast_node(AST_STRUCT_FIELD,
                            fn->value, fn->line, fn->column);
                        Type* ft = parse_type(parser);
                        if (!ft) { free_ast_node(field); free_ast_node(cdef); return NULL; }
                        field->node_type = ft;
                        // `@offset` — required explicit byte offset.
                        if (!match_token(parser, TOKEN_AT)) {
                            parser_error(parser, "@c_struct field needs an explicit `@<offset>` (e.g. `length: uint64 @8`)");
                            free_ast_node(field); free_ast_node(cdef); return NULL;
                        }
                        Token* off = expect_token(parser, TOKEN_NUMBER);
                        if (!off || !off->value) {
                            parser_error(parser, "expected a byte offset after `@`");
                            free_ast_node(field); free_ast_node(cdef); return NULL;
                        }
                        field->bit_width = atoi(off->value);  // reuse: byte offset (#891)
                        add_child(cdef, field);
                        if (!match_token(parser, TOKEN_COMMA))
                            match_token(parser, TOKEN_SEMICOLON);
                    }
                    node = cdef;
                    break;
                }
                // @derive(eq[, format, clone, hash]) struct T { ... }
                //
                // Issue #338: synthesize trait-shaped helpers from
                // type definitions. Pre-typecheck pass picks up the
                // annotation and generates the matching helper
                // functions (T_eq / T_format / T_clone / T_hash)
                // before the struct is type-checked.
                if (attr && attr->type == TOKEN_IDENTIFIER &&
                    attr->value && strcmp(attr->value, "derive") == 0) {
                    advance_token(parser);  // consume 'derive'
                    if (!expect_token(parser, TOKEN_LEFT_PAREN)) return NULL;
                    // Parse the comma-separated derive list.
                    char tag[256];
                    snprintf(tag, sizeof(tag), "derive:");
                    size_t tag_len = strlen(tag);
                    int first = 1;
                    while (1) {
                        Token* d = expect_token(parser, TOKEN_IDENTIFIER);
                        if (!d || !d->value) break;
                        size_t dlen = strlen(d->value);
                        if (tag_len + dlen + 2 >= sizeof(tag)) break;
                        if (!first) { tag[tag_len++] = ','; }
                        memcpy(tag + tag_len, d->value, dlen);
                        tag_len += dlen;
                        tag[tag_len] = '\0';
                        first = 0;
                        if (!match_token(parser, TOKEN_COMMA)) break;
                    }
                    if (!expect_token(parser, TOKEN_RIGHT_PAREN)) return NULL;
                    // The next decl should be a struct. Parse it via the
                    // normal struct path, then attach the annotation.
                    Token* next_tok = peek_token(parser);
                    if (next_tok && next_tok->type == TOKEN_STRUCT) {
                        ASTNode* sd = parse_struct_definition(parser);
                        if (sd) {
                            if (sd->annotation) free(sd->annotation);
                            sd->annotation = strdup(tag);
                        }
                        node = sd;
                        break;
                    } else {
                        parser_error(parser, "@derive(...) must precede a `struct` definition");
                        return NULL;
                    }
                }
                if (attr && attr->type == TOKEN_IDENTIFIER &&
                    attr->value && strcmp(attr->value, "c_callback") == 0) {
                    advance_token(parser);  // consume 'c_callback'
                    // Optional ("c_symbol_name") binding. Without it, the
                    // C symbol matches the Aether-side name verbatim.
                    char* explicit_sym = NULL;
                    if (match_token(parser, TOKEN_LEFT_PAREN)) {
                        Token* sym = expect_token(parser, TOKEN_STRING_LITERAL);
                        expect_token(parser, TOKEN_RIGHT_PAREN);
                        if (sym && sym->value) explicit_sym = strdup(sym->value);
                    }
                    // Parse the function definition that follows. Any
                    // existing function-def grammar is fine — the
                    // annotation only changes codegen of a normal
                    // function, it doesn't change the parse shape.
                    ASTNode* fdef = parse_function_definition(parser);
                    if (fdef) {
                        // Tag shape:
                        //   "c_callback:NAME" — explicit @c_callback("NAME")
                        //   "c_callback:"     — bare @c_callback; codegen
                        //                       falls back to fdef->value
                        //                       (post-merge namespace-
                        //                       prefixed when imported).
                        char tag[256];
                        snprintf(tag, sizeof(tag), "c_callback:%s",
                                 explicit_sym ? explicit_sym : "");
                        if (fdef->annotation) free(fdef->annotation);
                        fdef->annotation = strdup(tag);
                    }
                    if (explicit_sym) free(explicit_sym);
                    node = fdef;
                    break;
                }
                // #481 effect tags: `@pure` / `@no_fs` / `@no_net` / `@no_os`
                // declare that the function (transitively) must not touch the
                // named capability. Stackable (`@no_fs @no_net f() {}`);
                // `@pure` forbids all three. A post-typecheck pass walks the
                // call graph and errors if a forbidden capability is reached.
                if (attr && attr->type == TOKEN_IDENTIFIER && attr->value &&
                    (strcmp(attr->value, "pure") == 0 ||
                     strcmp(attr->value, "no_fs") == 0 ||
                     strcmp(attr->value, "no_net") == 0 ||
                     strcmp(attr->value, "no_os") == 0)) {
                    int no_fs = 0, no_net = 0, no_os = 0;
                    for (;;) {
                        if (strcmp(attr->value, "pure") == 0) { no_fs = no_net = no_os = 1; }
                        else if (strcmp(attr->value, "no_fs") == 0) no_fs = 1;
                        else if (strcmp(attr->value, "no_net") == 0) no_net = 1;
                        else if (strcmp(attr->value, "no_os") == 0) no_os = 1;
                        advance_token(parser);  // consume the effect keyword
                        // Another stacked `@effect`?
                        if (peek_token(parser) && peek_token(parser)->type == TOKEN_AT) {
                            advance_token(parser);  // consume '@'
                            attr = peek_token(parser);
                            if (attr && attr->type == TOKEN_IDENTIFIER && attr->value &&
                                (strcmp(attr->value, "pure") == 0 ||
                                 strcmp(attr->value, "no_fs") == 0 ||
                                 strcmp(attr->value, "no_net") == 0 ||
                                 strcmp(attr->value, "no_os") == 0)) {
                                continue;
                            }
                            parser_error(parser, "effect tags (@pure/@no_fs/@no_net/@no_os) must directly precede the function definition");
                            return NULL;
                        }
                        break;
                    }
                    if (peek_token(parser) && peek_token(parser)->type == TOKEN_FUNC)
                        advance_token(parser);  // optional 'func'
                    ASTNode* fdef = parse_function_definition(parser);
                    if (fdef) {
                        if (fdef->annotation) {
                            parser_error(parser, "effect tags cannot combine with another annotation (e.g. a variadic `...` or @c_callback) on the same function");
                            return NULL;
                        }
                        char caps[32] = "";
                        if (no_fs)  strncat(caps, caps[0] ? ",fs"  : "fs",  sizeof(caps) - strlen(caps) - 1);
                        if (no_net) strncat(caps, caps[0] ? ",net" : "net", sizeof(caps) - strlen(caps) - 1);
                        if (no_os)  strncat(caps, caps[0] ? ",os"  : "os",  sizeof(caps) - strlen(caps) - 1);
                        char tag[64];
                        snprintf(tag, sizeof(tag), "effect:%s", caps);
                        fdef->annotation = strdup(tag);
                    }
                    node = fdef;
                    break;
                }
                if (!attr || attr->type != TOKEN_EXTERN) {
                    parser_error(parser, "unknown attribute (expected @extern(\"...\"), @c_callback, @derive, or an effect tag @pure/@no_fs/@no_net/@no_os)");
                    advance_token(parser);
                    return NULL;
                }
                advance_token(parser);
                expect_token(parser, TOKEN_LEFT_PAREN);
                Token* sym = expect_token(parser, TOKEN_STRING_LITERAL);
                expect_token(parser, TOKEN_RIGHT_PAREN);
                if (!sym || !sym->value) { advance_token(parser); return NULL; }
                // Now parse the function declaration that follows.
                // We use parse_extern_declaration's shape (params with
                // mandatory types, no body) but the Aether name comes
                // from the identifier following the annotation rather
                // than after an `extern` keyword. Re-purpose by temporarily
                // pretending `extern` came in: easier to inline the small
                // amount of code than to rework parse_extern_declaration.
                Token* fname = expect_token(parser, TOKEN_IDENTIFIER);
                if (!fname) return NULL;
                expect_token(parser, TOKEN_LEFT_PAREN);
                ASTNode* ext = create_ast_node(AST_EXTERN_FUNCTION, fname->value,
                                               at_tok->line, at_tok->column);
                // Stash the C symbol name with a recognizable prefix so
                // codegen can detect it without colliding with other
                // annotation users (e.g. defer factories).
                char tag[256];
                snprintf(tag, sizeof(tag), "c_symbol:%s", sym->value);
                ext->annotation = strdup(tag);

                if (!match_token(parser, TOKEN_RIGHT_PAREN)) {
                    do {
                        // Trailing `...` — `@extern` may be variadic just
                        // like a bare `extern`.
                        if (peek_token(parser) &&
                            peek_token(parser)->type == TOKEN_DOTDOTDOT) {
                            advance_token(parser);  // consume '...'
                            ext->annotation =
                                annotation_add_marker(ext->annotation, "varargs");
                            break;
                        }
                        Token* pname = expect_token(parser, TOKEN_IDENTIFIER);
                        if (!pname) break;
                        ASTNode* p = create_ast_node(AST_IDENTIFIER, pname->value,
                                                     pname->line, pname->column);
                        if (match_token(parser, TOKEN_COLON)) {
                            /* Same `@aether` / `@retain` per-param annotations as
                             * the bare `extern foo(...)` form. See
                             * parse_extern_declaration for the full table of
                             * supported attributes. Multiple stack via repeated
                             * `@<attr>`. */
                            while (peek_token(parser) && peek_token(parser)->type == TOKEN_AT) {
                                advance_token(parser);
                                Token* pattr = peek_token(parser);
                                const char* tag = NULL;
                                if (pattr && pattr->type == TOKEN_IDENTIFIER && pattr->value) {
                                    if (strcmp(pattr->value, "aether") == 0) {
                                        tag = "aether_param";
                                        advance_token(parser);
                                    } else if (strcmp(pattr->value, "retain") == 0) {
                                        tag = "retain_param";
                                        advance_token(parser);
                                    }
                                }
                                if (!tag) {
                                    parser_error(parser, "unknown extern-param attribute (expected @aether or @retain)");
                                    break;
                                }
                                if (!p->annotation) {
                                    p->annotation = strdup(tag);
                                } else if (!strstr(p->annotation, tag)) {
                                    size_t old_len = strlen(p->annotation);
                                    size_t tag_len = strlen(tag);
                                    char* combined = (char*)malloc(old_len + 1 + tag_len + 1);
                                    memcpy(combined, p->annotation, old_len);
                                    combined[old_len] = ',';
                                    memcpy(combined + old_len + 1, tag, tag_len);
                                    combined[old_len + 1 + tag_len] = '\0';
                                    free(p->annotation);
                                    p->annotation = combined;
                                }
                            }
                            Type* pt = parse_type(parser);
                            p->node_type = pt ? pt : create_type(TYPE_INT);
                        } else {
                            parser_error(parser, "Type annotation required for @extern parameter (use param: type)");
                            p->node_type = create_type(TYPE_INT);
                        }
                        add_child(ext, p);
                    } while (match_token(parser, TOKEN_COMMA));
                    expect_token(parser, TOKEN_RIGHT_PAREN);
                }
                if (match_token(parser, TOKEN_ARROW)) {
                    Type* rt = parse_type(parser);
                    ext->node_type = rt ? rt : create_type(TYPE_INT);
                } else {
                    ext->node_type = create_type(TYPE_VOID);
                }
                parse_extern_trailing_attrs(parser, ext);
                node = ext;
                break;
            }
            case TOKEN_BUILDER: {
                // builder before a function definition = builder function
                Token* next_d = peek_ahead(parser, 1);
                Token* next_d2 = peek_ahead(parser, 2);
                if (next_d && next_d->type == TOKEN_IDENTIFIER &&
                    next_d2 && next_d2->type == TOKEN_LEFT_PAREN) {
                    advance_token(parser); // consume 'builder'
                    parser->parsing_builder = 1;
                    node = parse_function_definition(parser);
                    parser->parsing_builder = 0;
                    if (node) {
                        node->type = AST_BUILDER_FUNCTION;
                    }
                } else {
                    parser_error(parser, "Expected function definition after 'builder' at top level");
                    advance_token(parser);
                    return NULL;
                }
                break;
            }
            case TOKEN_VAR: {
                // Top-level mutable global (#701): var NAME[: type] = const-expr
                // Lowers to a file-scope `static <ctype> NAME = <value>;` in the
                // generated TU. Module-private by design (matches the C statics it
                // replaces — a PRNG seed, a formatted-id cache, etc.); reads and
                // writes from same-module functions are plain identifier access,
                // no accessor indirection. Reuses AST_CONST_DECLARATION with a
                // `global_var` annotation: the node shape is identical (name +
                // initializer + type); the annotation only flips codegen from a
                // `#define` to a mutable static and tailors the const-expr
                // diagnostic. (Same pattern as array constants, which reuse this
                // node with `array_const`.) The initializer is still required to
                // be a compile-time constant — C demands it of a static.
                int vline = token->line, vcol = token->column;
                advance_token(parser); // consume 'var'
                Token* vname = expect_token(parser, TOKEN_IDENTIFIER);
                if (!vname) { advance_token(parser); return NULL; }
                Type* vtype = NULL;
                if (peek_token(parser) && peek_token(parser)->type == TOKEN_COLON) {
                    advance_token(parser); // consume ':'
                    vtype = parse_type(parser);
                }
                if (!expect_token(parser, TOKEN_ASSIGN)) { advance_token(parser); return NULL; }
                ASTNode* vval = parse_expression(parser);
                if (!vval) { advance_token(parser); return NULL; }
                node = create_ast_node(AST_CONST_DECLARATION, vname->value, vline, vcol);
                add_child(node, vval);
                node->annotation = strdup("global_var");
                if (vtype) {
                    node->node_type = vtype;
                } else if (vval->node_type) {
                    node->node_type = clone_type(vval->node_type);
                    /* #929: no explicit type annotation — the width was
                     * INFERRED from the initializer (bare `var x = 0` → 32-bit
                     * int). Mark it so the #698 silent-narrowing guard fires on
                     * a later 64-bit assignment to this global, matching the
                     * local `x = expr` path (an explicit `var x: long`/`: T`
                     * leaves this 0 and is exempt). */
                    node->type_inferred = 1;
                } else {
                    node->node_type = create_type(TYPE_UNKNOWN);
                }
                break;
            }
            case TOKEN_CONST: {
                // Top-level constant: const NAME = value, const arr[] = [...],
                // or (typed) const NAME: T[N] = [...] / const NAME: T = value.
                int cline = token->line, ccol = token->column;
                advance_token(parser); // consume 'const'
                Token* cname = expect_token(parser, TOKEN_IDENTIFIER);
                if (!cname) { advance_token(parser); return NULL; }

                int is_array = 0;
                /* #745: explicit element type for a module-level const
                 * array — `const NAME: T[N] = [...]` lowers to a file-
                 * scope `static const <T> NAME[N] = {...}` lookup table.
                 * The untyped `const NAME[] = [...]` form (below) infers
                 * `int` from the literals; the typed form lets a porter
                 * pin the C element type (uint8/uint16/int/long/uint64)
                 * so e.g. a CRC16 table is `uint16_t[256]`, not `int[]`.
                 * Also accepts the typed scalar form `const NAME: T =
                 * value`. */
                Type* annotated_type = NULL;   // full type from `: T` / `: T[N]`
                if (peek_token(parser) && peek_token(parser)->type == TOKEN_COLON) {
                    advance_token(parser); // consume ':'
                    /* parse_type already folds a trailing `[N]` into a
                     * TYPE_ARRAY (element + size), so `long[3]` arrives
                     * here as array(long, 3). */
                    annotated_type = parse_type(parser);
                    if (!annotated_type) { advance_token(parser); return NULL; }
                    if (annotated_type->kind == TYPE_ARRAY) {
                        is_array = 1;
                    }
                }
                // Untyped array form: const NAME[] = [...]
                else if (peek_token(parser) && peek_token(parser)->type == TOKEN_LEFT_BRACKET) {
                    advance_token(parser); // consume '['
                    if (!expect_token(parser, TOKEN_RIGHT_BRACKET)) { advance_token(parser); return NULL; }
                    is_array = 1;
                }

                if (!expect_token(parser, TOKEN_ASSIGN)) {
                    if (annotated_type) free_type(annotated_type);
                    advance_token(parser); return NULL;
                }
                ASTNode* cval = parse_expression(parser);
                if (!cval) { if (annotated_type) free_type(annotated_type); advance_token(parser); return NULL; }
                node = create_ast_node(AST_CONST_DECLARATION, cname->value, cline, ccol);
                add_child(node, cval);

                if (is_array) {
                    node->annotation = strdup("array_const");
                    if (annotated_type) {
                        /* Explicit element type + size from `: T[N]`
                         * (ownership transferred). If the size was elided
                         * (`T[]`), recover it from the literal length. */
                        if (annotated_type->array_size < 0) {
                            annotated_type->array_size = cval->child_count;
                        }
                        node->node_type = annotated_type;
                    } else {
                        // Untyped `const NAME[] = [...]` — infer element type.
                        Type* elem_type = NULL;
                        if (cval->node_type == NULL && cval->child_count > 0 && cval->children[0]) {
                            if (cval->children[0]->node_type) elem_type = cval->children[0]->node_type;
                        } else if (cval->node_type && cval->node_type->element_type) {
                            elem_type = cval->node_type->element_type;
                        }
                        node->node_type = elem_type
                            ? create_array_type(clone_type(elem_type), cval->child_count)
                            : create_array_type(create_type(TYPE_PTR), cval->child_count);
                    }
                } else if (annotated_type) {
                    // Typed scalar const: `const NAME: T = value`.
                    node->node_type = annotated_type;
                } else {
                    // Infer type from value.
                    node->node_type = cval->node_type ? clone_type(cval->node_type)
                                                       : create_type(TYPE_UNKNOWN);
                }
                break;
            }
            case TOKEN_MAIN:
                node = parse_main_function(parser);
                break;
            case TOKEN_IDENTIFIER: {
                // Check if this is a function: identifier(...)
                Token* next = peek_ahead(parser, 1);
                // `fn name(...)` — `fn` as a function-definition keyword.
                // `fn` is NOT a lexer keyword (it doubles as the fn-pointer
                // type head `fn(...) -> R`), so the definition form is
                // recognised here by shape: `fn` + name + `(`. The std
                // library uses this spelling (std.uuid, std.url). Before,
                // a top-level `fn name()` only survived via parse-error
                // recovery — the "unexpected identifier" error fired on
                // `fn`, recovery skipped it, and `name(` then parsed as a
                // function. That recovery is non-fatal when a module is
                // imported but fatal on a standalone/strict parse, so at
                // full module-graph scale a re-parsed sibling module that
                // used `fn` surfaced the recovery as a spurious top-level
                // parse error in that module (#791). Accepting `fn` here
                // makes the spelling first-class and the parse identical on
                // every path.
                if (token->value && strcmp(token->value, "fn") == 0 &&
                    next && next->type == TOKEN_IDENTIFIER) {
                    Token* after = peek_ahead(parser, 2);
                    if (after && after->type == TOKEN_LEFT_PAREN) {
                        advance_token(parser);  // consume `fn`
                        node = parse_function_definition(parser);
                        break;
                    }
                }
                if (next && next->type == TOKEN_LEFT_PAREN) {
                    // Function without 'func' keyword
                    node = parse_function_definition(parser);
                } else {
                    parser_error(parser, "Unexpected identifier at top level (expected actor, struct, or function)");
                    advance_token(parser);
                    return NULL;
                }
                break;
            }
            // C-style return type prefix: int func_name(...) { ... }
            case TOKEN_INT:
            case TOKEN_INT64:
            case TOKEN_UINT64:
            case TOKEN_DURATION:
            case TOKEN_FLOAT:
            case TOKEN_BOOL:
            case TOKEN_BYTE:
            case TOKEN_STRING:
            case TOKEN_PTR: {
                Token* next = peek_ahead(parser, 1);
                Token* next2 = peek_ahead(parser, 2);
                if (next && next->type == TOKEN_IDENTIFIER &&
                    next2 && next2->type == TOKEN_LEFT_PAREN) {
                    // Parse the return type, then the function definition
                    Type* ret_type = parse_type(parser);
                    node = parse_function_definition(parser);
                    if (node && ret_type) {
                        if (node->node_type) free_type(node->node_type);
                        node->node_type = ret_type;
                    } else if (ret_type) {
                        free_type(ret_type);
                    }
                } else {
                    parser_error(parser, "Expected function definition after type keyword");
                    advance_token(parser);
                    return NULL;
                }
                break;
            }
            default:
                /* If the token is a reserved keyword followed by `(`,
                 * the user almost certainly tried to define a function
                 * with that name (e.g. `send()`, `recv()`, `state()`,
                 * `match()`). Without this check, the parser advances
                 * past the keyword, the `(...)` then re-enters statement
                 * parsing, and the function body is silently dropped at
                 * codegen — call sites compile to expressions referring
                 * to a nonexistent C function. Detect this case here
                 * and emit a clear "reserved keyword" diagnostic that
                 * matches the inner-block handling (parser.c:71). */
                if (token_is_reserved_keyword(token)) {
                    Token* nxt = peek_ahead(parser, 1);
                    if (nxt && nxt->type == TOKEN_LEFT_PAREN) {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                            "'%s' is a reserved keyword and cannot be used as a function name; rename it (e.g. '%s_' or 'do_%s')",
                            token->value, token->value, token->value);
                        char hint[128];
                        snprintf(hint, sizeof(hint),
                            "rename to '%s_' or another identifier",
                            token->value);
                        if (!parser->suppress_errors) {
                            aether_error_full(msg, token->line, token->column,
                                              hint, NULL, AETHER_ERR_SYNTAX);
                        }
                        /* Skip the bogus definition so we don't keep
                         * erroring on every token in its body. */
                        advance_token(parser);  /* the keyword */
                        int paren_depth = 0;
                        while (peek_token(parser)) {
                            Token* t = peek_token(parser);
                            if (t->type == TOKEN_LEFT_PAREN) paren_depth++;
                            else if (t->type == TOKEN_RIGHT_PAREN) {
                                paren_depth--;
                                if (paren_depth == 0) {
                                    advance_token(parser);
                                    break;
                                }
                            }
                            advance_token(parser);
                        }
                        /* Skip an optional `-> type` clause. */
                        if (peek_token(parser) &&
                            peek_token(parser)->type == TOKEN_ARROW) {
                            advance_token(parser);
                            parse_type(parser);
                        }
                        /* Skip the body block. */
                        if (peek_token(parser) &&
                            peek_token(parser)->type == TOKEN_LEFT_BRACE) {
                            int brace_depth = 0;
                            while (peek_token(parser)) {
                                Token* t = peek_token(parser);
                                if (t->type == TOKEN_LEFT_BRACE) brace_depth++;
                                else if (t->type == TOKEN_RIGHT_BRACE) {
                                    brace_depth--;
                                    if (brace_depth == 0) {
                                        advance_token(parser);
                                        break;
                                    }
                                }
                                advance_token(parser);
                            }
                        }
                        return NULL;
                    }
                }
                parser_error(parser, "Expected actor, struct, function, or main");
                advance_token(parser);
                return NULL;
        }

        return node;
    }
}


static void parse_region_stmts_into(Parser* parser, ASTNode* block) {
    if (!match_token(parser, TOKEN_LEFT_BRACE)) {
        parser_message(parser, "Error: expected '{' to open a `when` region");
        return;
    }
    int guard = 0;
    while (!is_at_end(parser) && guard++ < 10000) {
        Token* t = peek_token(parser);
        if (!t || t->type == TOKEN_RIGHT_BRACE) break;
        int start = parser->current_token;
        ASTNode* stmt = parse_statement(parser);
        if (stmt) add_child(block, stmt);
        else if (parser->current_token == start) advance_token(parser);
    }
    if (!match_token(parser, TOKEN_RIGHT_BRACE)) {
        parser_message(parser, "Error: unterminated `when` region");
    }
}

static void parse_when_region_stmts(Parser* parser, ASTNode* block) {
    advance_token(parser);            /* `when` */
    int ok = 1;
    int taken = parse_define_cond(parser, &ok);
    if (!ok) { skip_braced_region(parser); return; }

    if (taken) parse_region_stmts_into(parser, block);
    else       skip_braced_region(parser);

    if (peek_token(parser) && peek_token(parser)->type == TOKEN_ELSE) {
        advance_token(parser);
        if (taken) skip_braced_region(parser);
        else       parse_region_stmts_into(parser, block);
    }
}

/* ---- conditional compilation regions (#1527) --------------------------- *
 *
 * `when defined(NAME) { … } else { … }` around top-level declarations. The
 * losing branch is DROPPED, not wrapped in a preprocessor `#if`: an excluded
 * subsystem should not be in the binary, and code that never reaches codegen
 * does not need the names it mentions to exist. That is what the reporter of
 * #1527 could not get by any other route, and what `select()` structurally
 * cannot become: it chooses a value, and both branches still compile and link.
 *
 * The condition grammar is small on purpose: defined(NAME), !, &&, ||, and
 * parentheses. It is evaluated here, at parse time, against the symbols the
 * build declared.
 */

static int parse_define_primary(Parser* parser, int* ok) {
    Token* t = peek_token(parser);
    if (!t) { *ok = 0; return 0; }

    if (t->type == TOKEN_LEFT_PAREN) {
        advance_token(parser);
        int v = parse_define_cond(parser, ok);
        if (!match_token(parser, TOKEN_RIGHT_PAREN)) {
            parser_message(parser, "Error: expected ')' in a `when` condition");
            *ok = 0;
        }
        return v;
    }

    if (t->type == TOKEN_IDENTIFIER && t->value && strcmp(t->value, "defined") == 0) {
        advance_token(parser);
        if (!match_token(parser, TOKEN_LEFT_PAREN)) {
            parser_message(parser, "Error: expected '(' after `defined`");
            *ok = 0;
            return 0;
        }
        Token* name = peek_token(parser);
        if (!name || name->type != TOKEN_IDENTIFIER || !name->value) {
            parser_message(parser, "Error: expected a symbol name inside `defined(...)`");
            *ok = 0;
            return 0;
        }
        int v = aether_define_is_set(name->value);
        advance_token(parser);
        if (!match_token(parser, TOKEN_RIGHT_PAREN)) {
            parser_message(parser, "Error: expected ')' after the symbol name");
            *ok = 0;
        }
        return v;
    }

    parser_message(parser, "Error: a `when` region condition is defined(NAME), "
                           "optionally combined with !, && and ||");
    *ok = 0;
    return 0;
}

static int parse_define_unary(Parser* parser, int* ok) {
    if (peek_token(parser) &&
        (peek_token(parser)->type == TOKEN_NOT || peek_token(parser)->type == TOKEN_EXCLAIM)) {
        advance_token(parser);
        return !parse_define_unary(parser, ok);
    }
    return parse_define_primary(parser, ok);
}

static int parse_define_cond(Parser* parser, int* ok) {
    int value = parse_define_unary(parser, ok);
    while (*ok && peek_token(parser) &&
           (peek_token(parser)->type == TOKEN_AND || peek_token(parser)->type == TOKEN_OR)) {
        int is_and = peek_token(parser)->type == TOKEN_AND;
        advance_token(parser);
        int rhs = parse_define_unary(parser, ok);
        /* Both sides are always evaluated: they are symbol lookups with no
         * side effects, and short-circuiting would leave the losing side's
         * tokens unconsumed. */
        value = is_and ? (value && rhs) : (value || rhs);
    }
    return value;
}

/* Consumes a brace-delimited region without parsing it. Used for the branch
 * that loses, so its contents never reach the type checker. */
static void skip_braced_region(Parser* parser) {
    if (!match_token(parser, TOKEN_LEFT_BRACE)) return;
    int depth = 1;
    while (depth > 0 && !is_at_end(parser)) {
        Token* t = peek_token(parser);
        if (!t) break;
        if (t->type == TOKEN_LEFT_BRACE) depth++;
        else if (t->type == TOKEN_RIGHT_BRACE) depth--;
        advance_token(parser);
    }
}

/* Parses the declarations of a winning region straight into `program`, so a
 * region leaves no trace in the AST: what it contained is indistinguishable
 * from having been written at top level. */
static void parse_region_into(Parser* parser, ASTNode* program) {
    if (!match_token(parser, TOKEN_LEFT_BRACE)) {
        parser_message(parser, "Error: expected '{' to open a `when` region");
        return;
    }
    int guard = 0;
    while (!is_at_end(parser) && guard++ < 10000) {
        Token* t = peek_token(parser);
        if (!t || t->type == TOKEN_RIGHT_BRACE) break;
        int start = parser->current_token;
        ASTNode* node = parse_top_level_decl(parser);
        if (node) add_child(program, node);
        else if (parser->current_token == start) advance_token(parser);
    }
    if (!match_token(parser, TOKEN_RIGHT_BRACE)) {
        parser_message(parser, "Error: unterminated `when` region");
    }
}

/* True when the next tokens open a conditional-compilation region rather than
 * a function-clause guard: `when` followed by `defined`, `!` or `(`. A guard
 * is always attached to a clause head, so it never appears at top level. */
static int at_when_region(Parser* parser) {
    Token* t = peek_token(parser);
    if (!t || t->type != TOKEN_WHEN) return 0;
    Token* n = peek_ahead(parser, 1);
    if (!n) return 0;
    if (n->type == TOKEN_NOT || n->type == TOKEN_EXCLAIM || n->type == TOKEN_LEFT_PAREN) return 1;
    return n->type == TOKEN_IDENTIFIER && n->value && strcmp(n->value, "defined") == 0;
}

static void parse_when_region(Parser* parser, ASTNode* program) {
    advance_token(parser);            /* `when` */
    int ok = 1;
    int taken = parse_define_cond(parser, &ok);
    if (!ok) { skip_braced_region(parser); return; }

    if (taken) parse_region_into(parser, program);
    else       skip_braced_region(parser);

    if (peek_token(parser) && peek_token(parser)->type == TOKEN_ELSE) {
        advance_token(parser);
        if (taken) skip_braced_region(parser);
        else       parse_region_into(parser, program);
    }
}

ASTNode* parse_program(Parser* parser) {
    ASTNode* program = create_ast_node(AST_PROGRAM, NULL, 0, 0);

    int safety_counter = 0;
    // Safety limit to prevent infinite loops on malformed input
    const int MAX_ITERATIONS = 10000;

    while (!is_at_end(parser) && safety_counter < MAX_ITERATIONS) {
        safety_counter++;

        if (at_when_region(parser)) {
            parse_when_region(parser, program);
            continue;
        }

        int start_token = parser->current_token;
        ASTNode* node = parse_top_level_decl(parser);
        if (node) {
            add_child(program, node);
        } else if (parser->current_token == start_token) {
            // Error recovery in parse_top_level_decl did not consume a token
            // (e.g. NULL produced without advancing); step over to guarantee
            // forward progress and avoid spinning the safety counter.
            advance_token(parser);
        }
    }

    if (safety_counter >= MAX_ITERATIONS) {
        parser_message(parser, "Error: Parser safety limit reached - possible infinite loop");
        return NULL;
    }

    return program;
}
