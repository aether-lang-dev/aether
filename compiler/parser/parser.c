#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
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
#include <stdarg.h>

/* #3569: `fmt` formatted into a string as long as it needs, malloc'd for the
 * caller: a name the parser joins is as long as its parts. Fixed buffers cut
 * a longer one to a name nothing defines. Out of memory ends the compile, as
 * aether_xrealloc does. */
static char* parser_strf(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
static char* parser_strf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    char* s = (char*)aether_xrealloc(NULL, (size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
}

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
    parser->depth = 0;
    parser->depth_exceeded = 0;
    parser->int_consts = NULL;
    parser->int_const_count = 0;
    parser->int_consts_scanned = 0;
    return parser;
}

typedef struct ParserIntConst {
    const char* name;   /* the token's text, alive for the whole parse */
    long long value;
} ParserIntConst;

void free_parser(Parser* parser) {
    if (parser) {
        free(parser->int_consts);
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

/* #3115: the offset of the `]` that closes the `[` at offset `open` from the
 * current token, on that `[`'s line; -1 when there is none. A declaration's
 * array type may hold an expression (`Pair[N + 1] ps`), so the lookaheads
 * that tell `T[n] name` from an index expression skip the whole group. */
static int closing_bracket_offset(Parser* parser, int open) {
    Token* o = peek_ahead(parser, open);
    if (!o || o->type != TOKEN_LEFT_BRACKET) return -1;
    int depth = 0;
    for (int k = open;; k++) {
        Token* t = peek_ahead(parser, k);
        if (!t || t->line != o->line) return -1;
        if (t->type == TOKEN_LEFT_BRACKET) depth++;
        else if (t->type == TOKEN_RIGHT_BRACKET && --depth == 0) return k;
    }
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
// #340 made `none` the empty-optional literal: any bare identifier spelt `none`
// in expression position IS the literal. That makes the spelling unusable as a
// name, and not in a way the use can warn about -- the use simply reads as the
// literal, and the error arrives two concepts away ("cannot interpolate this
// value", "invalid operation for given types") or, in one shape, as malformed C
// out of codegen, where the variable and the literal both try to own the name
// (#2018). So it is refused where it is BOUND, which is the one place the
// diagnostic can point at something the user actually wrote.
//
// Returns 1 and reports when `token` is the identifier `none`; the caller
// abandons the declaration. Only binding positions call this. A `match` arm
// spelt `none` is the literal pattern and goes through parse_expression, so it
// never arrives here. The backtick escape is deliberately not offered: it yields
// the same identifier token, which expression position would still read as the
// literal.
static int reject_none_binding(Parser* parser, const Token* token, const char* what) {
    if (!token || token->type != TOKEN_IDENTIFIER || !token->value ||
        strcmp(token->value, "none") != 0) return 0;
    if (!parser->suppress_errors) {
        const char* msg = "";
        msg = aether_internf(
            "'none' is the empty-optional literal and cannot be used as a %s: every "
            "later use of it would read as the literal, not the binding", what);
        aether_error_full(msg, token->line, token->column,
                          "rename it (e.g. 'nothing', 'absent' or 'none_')",
                          NULL, AETHER_ERR_SYNTAX);
    }
    return 1;
}

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
        const char* error_msg = "";
        if (expected == TOKEN_IDENTIFIER && token_is_reserved_keyword(token)) {
            // User picked a reserved keyword as an identifier name —
            // point at the keyword and suggest a rename so they don't
            // have to guess which grammar slot was wanted.
            error_msg = aether_internf(
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
        error_msg = aether_internf(
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

/* A syntax error at `token` rather than at the current one. */
static void parser_error_at(Parser* parser, Token* token, const char* message) {
    if (parser->suppress_errors) return;
    if (!token) {
        parser_error(parser, message);
        return;
    }
    aether_error_with_code(message, token->line, token->column, AETHER_ERR_SYNTAX);
}

/* #2998: what had to follow the token `after` (an `=`, an operator, a `(` or
 * a `,`, already consumed) is missing. The expression parser returns NULL
 * without a word at a token that cannot begin an expression (`}`, `;`, `)`,
 * the end of the file), and every place that had consumed such a token took
 * the NULL as "no value": `x = }` declared `x` with nothing in it, `y = x +`
 * dropped the `+`, `int x =` and `x = f(` compiled. So each of them parses
 * what it requires and, when that fails, calls this with the error count from
 * before the attempt: it reports at `after`, naming `what` was expected and
 * what was found instead, unless something below already said why. */
static void missing_operand(Parser* parser, Token* after, int errors_before,
                            const char* what) {
    if (parser->suppress_errors || parser->depth_exceeded) return;
    if (aether_error_count() != errors_before) return;
    Token* found = peek_token(parser);
    if (found && found->type == TOKEN_TRY) {
        /* #3150: `r = try { work() } catch e { ... }`. A `try` is a
         * statement and gives no value. Reported as a keyword used for a
         * name, and with only `try {` taken as the rest of the line, the
         * block's own `}` then closed the function and its `catch` was read
         * at top level: six errors. Say what it is once, and step over the
         * whole statement, its `catch` included, so nothing after it is
         * misread. */
        aether_error_full("`try` is a statement and gives no value",
                          found->line, found->column,
                          "bind the value inside it: `try { r = work() } catch e { ... }`",
                          NULL, AETHER_ERR_SYNTAX);
        advance_token(parser); // `try`
        for (int part = 0; part < 2; part++) {
            if (part == 1) {
                if (!peek_token(parser) || peek_token(parser)->type != TOKEN_CATCH) break;
                advance_token(parser); // `catch`
                if (peek_token(parser) && peek_token(parser)->type == TOKEN_IDENTIFIER) {
                    advance_token(parser); // the name the message binds
                }
            }
            if (!peek_token(parser) || peek_token(parser)->type != TOKEN_LEFT_BRACE) break;
            int depth = 0;
            while (!is_at_end(parser)) {
                Token* t = advance_token(parser);
                if (t->type == TOKEN_LEFT_BRACE) depth++;
                if (t->type == TOKEN_RIGHT_BRACE && --depth == 0) break;
            }
        }
        return;
    }
    if (found && found->value && token_is_reserved_keyword(found) &&
        !token_is_value_ident(found)) {
        /* `rc = send()`: the operand is there, named by a keyword. */
        char kmsg[256], khint[160];
        snprintf(kmsg, sizeof(kmsg),
                 "'%s' is a reserved keyword and cannot be used as an identifier; "
                 "rename it (e.g. '%s_'), or escape it as `%s` to use the name verbatim",
                 found->value, found->value, found->value);
        snprintf(khint, sizeof(khint), "rename to '%s_', or write `%s` to keep the name",
                 found->value, found->value);
        aether_error_full(kmsg, found->line, found->column, khint, NULL, AETHER_ERR_SYNTAX);
        return;
    }
    char seen[96];
    if (!found || found->type == TOKEN_EOF)
        snprintf(seen, sizeof(seen), "the end of the file");
    else if (found->value)
        snprintf(seen, sizeof(seen), "`%.60s`", found->value);
    else
        snprintf(seen, sizeof(seen), "%s", token_type_to_string(found->type));
    const char* op = (after && after->value) ? after->value : "?";
    const char* msg = ""; const char* help = "";
    msg = aether_internf("expected %s after `%s`, found %s", what, op, seen);
    help = aether_internf("write %s after `%s`", what, op);
    int line = after ? after->line : (found ? found->line : 0);
    int column = after ? after->column : (found ? found->column : 0);
    aether_error_full(msg, line, column, help, NULL, AETHER_ERR_SYNTAX);
}

static Type* parse_type_unsuffixed(Parser* parser);
static void parse_param_modes(Parser* parser, char** annotation);
static int reject_unbound_binding(Parser* parser, ASTNode* decl, const char* kw);

/* A struct's name in a type position, its first identifier `first` already
 * consumed: a bare `T`, or `mod.T` / `a.b.T` (#946), the module it comes
 * from written before it. The struct is the last segment and the module
 * path is kept on the type as its `qualifier`: the merge resolves it to
 * that module's struct (struct types are module-scoped), and
 * check_qualified_types holds it to a module that has the type. Shared by
 * `T`, `*T` and `as *T`. */
static Type* parse_struct_type_name(Parser* parser, Token* first) {
    const char* type_name = first->value;
    /* The module path, as long as it is (#3569): 512 bytes cut a longer
     * one to another module's. */
    const char* path = NULL;
    while (peek_token(parser) && peek_token(parser)->type == TOKEN_DOT) {
        advance_token(parser);  // consume '.'
        Token* seg = peek_token(parser);
        if (!seg || (seg->type != TOKEN_IDENTIFIER &&
                     !token_is_reserved_keyword(seg))) {
            parser_error(parser,
                "Expected a type name after '.' in a qualified type");
            return NULL;
        }
        advance_token(parser);  // consume the segment
        /* the segment before this one joins the module path */
        path = path ? aether_internf("%s.%s", path, type_name) : aether_intern(type_name);
        type_name = seg->value; // last segment is the type
    }
    Type* type = create_type(TYPE_STRUCT);
    type->struct_name = strdup(type_name);
    if (path) type->qualifier = path;
    return type;
}

/* #3115: an integer literal's text as the language reads it (`128`, `0x40`,
 * `0b1000`, `0o20`, `1_024`). 0 for anything else, a float among them. */
static int int_literal_text(const char* text, long long* out) {
    char digits[72];
    size_t k = 0;
    for (const char* c = text; c && *c && k + 1 < sizeof digits; c++)
        if (*c != '_') digits[k++] = *c;
    digits[k] = '\0';
    int base = 0;
    const char* s = digits;
    if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) { base = 2; s += 2; }
    else if (s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) { base = 8; s += 2; }
    char* end = NULL;
    unsigned long long v = strtoull(s, &end, base);
    if (!end || *end || end == s || v > (unsigned long long)LLONG_MAX) return 0;
    *out = (long long)v;
    return 1;
}

/* #3115: an integer constant expression over the tokens from `pos`:
 * literals, names among the file's integer constants, unary minus,
 * parentheses, and `* / %`, `+ -`, `<< >>` with C's precedence. It stops at
 * the first token that cannot continue it. `fail` names why it is not one:
 * the first unknown name, or a division by zero. */
typedef struct {
    Parser* p;
    int pos;
    const char* fail;
    Token* fail_at;
} ConstEval;

static Token* ce_peek(ConstEval* ce) {
    return ce->pos < ce->p->token_count ? ce->p->tokens[ce->pos] : NULL;
}

static int ce_lookup(Parser* p, const char* name, long long* out) {
    for (int i = 0; i < p->int_const_count; i++)
        if (strcmp(p->int_consts[i].name, name) == 0) { *out = p->int_consts[i].value; return 1; }
    return 0;
}

/* Every value an array-length expression reaches stays within +-2^31: a
 * length is at most INT_MAX, and inside that bound no `*`, `+`, `-` or
 * `<<` of two such values overflows a long long. */
#define CE_LIMIT (1LL << 31)

static int ce_in_range(ConstEval* ce, long long v, Token* at) {
    if (v >= -CE_LIMIT && v <= CE_LIMIT) return 1;
    if (!ce->fail) { ce->fail = "range"; ce->fail_at = at; }
    return 0;
}

static int ce_shift(ConstEval* ce, long long* out);

static int ce_unary(ConstEval* ce, long long* out) {
    Token* t = ce_peek(ce);
    if (!t) return 0;
    if (t->type == TOKEN_MINUS) {
        ce->pos++;
        if (!ce_unary(ce, out)) return 0;
        *out = -*out;
        return 1;
    }
    if (t->type == TOKEN_LEFT_PAREN) {
        ce->pos++;
        if (!ce_shift(ce, out)) return 0;
        Token* r = ce_peek(ce);
        if (!r || r->type != TOKEN_RIGHT_PAREN) return 0;
        ce->pos++;
        return 1;
    }
    if (t->type == TOKEN_NUMBER) {
        if (!int_literal_text(t->value, out) || !ce_in_range(ce, *out, t)) return 0;
        ce->pos++;
        return 1;
    }
    if (t->type == TOKEN_IDENTIFIER) {
        if (!ce_lookup(ce->p, t->value, out)) {
            if (!ce->fail) { ce->fail = "unknown"; ce->fail_at = t; }
            return 0;
        }
        ce->pos++;
        return 1;
    }
    return 0;
}

static int ce_mul(ConstEval* ce, long long* out) {
    if (!ce_unary(ce, out)) return 0;
    for (;;) {
        Token* t = ce_peek(ce);
        if (!t || (t->type != TOKEN_MULTIPLY && t->type != TOKEN_DIVIDE &&
                   t->type != TOKEN_MODULO)) return 1;
        ce->pos++;
        long long r;
        if (!ce_unary(ce, &r)) return 0;
        if (t->type == TOKEN_MULTIPLY) *out *= r;
        else if (r == 0) {
            if (!ce->fail) { ce->fail = "zero"; ce->fail_at = t; }
            return 0;
        } else if (t->type == TOKEN_DIVIDE) *out /= r;
        else *out %= r;
        if (!ce_in_range(ce, *out, t)) return 0;
    }
}

static int ce_add(ConstEval* ce, long long* out) {
    if (!ce_mul(ce, out)) return 0;
    for (;;) {
        Token* t = ce_peek(ce);
        if (!t || (t->type != TOKEN_PLUS && t->type != TOKEN_MINUS)) return 1;
        ce->pos++;
        long long r;
        if (!ce_mul(ce, &r)) return 0;
        *out = t->type == TOKEN_PLUS ? *out + r : *out - r;
        if (!ce_in_range(ce, *out, t)) return 0;
    }
}

static int ce_shift(ConstEval* ce, long long* out) {
    if (!ce_add(ce, out)) return 0;
    for (;;) {
        Token* t = ce_peek(ce);
        if (!t || (t->type != TOKEN_LSHIFT && t->type != TOKEN_RSHIFT)) return 1;
        ce->pos++;
        long long r;
        if (!ce_add(ce, &r)) return 0;
        /* a shift of a negative value, or by a negative or too-wide count,
         * is no length */
        if (*out < 0 || r < 0 || r > 31) {
            if (!ce->fail) { ce->fail = "range"; ce->fail_at = t; }
            return 0;
        }
        *out = t->type == TOKEN_LSHIFT ? *out << r : *out >> r;
        if (!ce_in_range(ce, *out, t)) return 0;
    }
}

/* #3115: collect the file's integer constants: every top-level
 * `const NAME = e` or `const NAME: T = e` whose whole value is an integer
 * constant expression. A constant built from a later one is found by
 * scanning again, until a pass finds nothing new. */
static void scan_int_consts(Parser* p) {
    p->int_consts_scanned = 1;
    int cap = 0;
    for (int pass = 0; pass < 16; pass++) {
        int found = 0, depth = 0;
        for (int i = 0; i < p->token_count; i++) {
            Token* t = p->tokens[i];
            if (t->type == TOKEN_LEFT_BRACE) { depth++; continue; }
            if (t->type == TOKEN_RIGHT_BRACE) { if (depth > 0) depth--; continue; }
            if (depth != 0 || t->type != TOKEN_CONST || i + 2 >= p->token_count) continue;
            Token* name = p->tokens[i + 1];
            if (name->type != TOKEN_IDENTIFIER) continue;
            long long known;
            if (ce_lookup(p, name->value, &known)) continue;
            /* skip `: T` to the `=` on the same line */
            int j = i + 2;
            while (j < p->token_count && p->tokens[j]->type != TOKEN_ASSIGN &&
                   p->tokens[j]->line == name->line)
                j++;
            if (j >= p->token_count || p->tokens[j]->type != TOKEN_ASSIGN) continue;
            ConstEval ce = { p, j + 1, NULL, NULL };
            long long v;
            if (!ce_shift(&ce, &v) || ce.pos == j + 1) continue;
            /* the value is all of the expression: what follows starts a new
             * statement (a later line, a `;`, the end) */
            Token* last = p->tokens[ce.pos - 1];
            Token* next = ce.pos < p->token_count ? p->tokens[ce.pos] : NULL;
            if (next && next->type != TOKEN_EOF && next->type != TOKEN_SEMICOLON &&
                next->line == last->line)
                continue;
            if (p->int_const_count == cap) {
                cap = cap ? cap * 2 : 16;
                ParserIntConst* grown = realloc(p->int_consts, (size_t)cap * sizeof *grown);
                if (!grown) return;
                p->int_consts = grown;
            }
            p->int_consts[p->int_const_count].name = name->value;
            p->int_consts[p->int_const_count].value = v;
            p->int_const_count++;
            found = 1;
        }
        if (!found) return;
    }
}

/* #3115: a fixed-size array's length, after its `[`: an integer constant
 * expression of at least 1. Reports and returns 0 for anything else. */
static int parse_array_length(Parser* parser) {
    Token* start = peek_token(parser);
    if (!parser->int_consts_scanned) scan_int_consts(parser);
    ConstEval ce = { parser, parser->current_token, NULL, NULL };
    long long v = 0;
    int ok = ce_shift(&ce, &v);
    const char* msg = "";
    if (!ok || ce.pos == parser->current_token) {
        if (ce.fail && strcmp(ce.fail, "zero") == 0) {
            parser_error_at(parser, ce.fail_at, "division by zero in an array length");
        } else if (ce.fail && strcmp(ce.fail, "range") == 0) {
            msg = aether_internf("an array length must be from 1 to %d; this one "
                     "reaches a value outside that", INT_MAX);
            parser_error_at(parser, ce.fail_at, msg);
        } else if (ce.fail) {
            msg = aether_internf(
                "array length '%s' is not an integer constant of this file: a length is "
                "a number, a `const` of the file holding one, or an expression over those",
                ce.fail_at->value);
            parser_error_at(parser, ce.fail_at, msg);
        } else {
            parser_error_at(parser, start,
                "an array length is a number, a `const` of the file holding one, or an "
                "expression over those (`int[4]`, `long[QWORDS]`, `long[NW + 1]`)");
        }
        return 0;
    }
    if (v < 1 || v > INT_MAX) {
        msg = aether_internf("an array length must be from 1 to %d, but this one is %lld",
                 INT_MAX, v);
        parser_error_at(parser, start, msg);
        return 0;
    }
    parser->current_token = ce.pos;
    return (int)v;
}

/* After `actor_ref`, at its `[`: is that the type parameter
 * (`actor_ref[Counter]`, `actor_ref[mod.Counter]`), or the start of an
 * array of untyped references (`actor_ref[]`, `actor_ref[8]`,
 * `actor_ref[MAX]`, `actor_ref[N + 1]`)? A parameter is a name that is not
 * an integer `const` of the file; a size starts with a number, a `(`, a
 * `-` or such a const, and `]` is a dynamic array. Every `[` was read as
 * a parameter, so `state members: actor_ref[8]` was a parse error at the
 * 8 (#3189). */
static int actor_ref_bracket_is_parameter(Parser* parser) {
    int i = parser->current_token + 1;
    if (i >= parser->token_count) return 1;
    Token* t = parser->tokens[i];
    if (t->type == TOKEN_IDENTIFIER) {
        if (!parser->int_consts_scanned) scan_int_consts(parser);
        long long v;
        return !ce_lookup(parser, t->value, &v);
    }
    return t->type != TOKEN_NUMBER && t->type != TOKEN_RIGHT_BRACKET &&
           t->type != TOKEN_LEFT_PAREN && t->type != TOKEN_MINUS;
}

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
        if (inner->c_alias) {
            base = inner->c_alias;
        } else if (inner->kind == TYPE_PTR && inner->element_type &&
                   inner->element_type->kind == TYPE_STRUCT &&
                   inner->element_type->struct_name) {
            /* As long as the struct's name (#3569): 128 bytes cut it. */
            base = aether_internf("%s*", inner->element_type->struct_name);
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
        /* `*mod.T`: the pointer form of the qualified name below. */
        Type* struct_type = parse_struct_type_name(parser, name_tok);
        if (!struct_type) return NULL;
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
                    /* #2980: a parameter's mode, `fn(@keep ptr, float)`. A
                     * signature that names one is a closure's type: what
                     * a function does with a `ptr` is an Aether
                     * function's contract, and a typed C function
                     * pointer's parameters borrow. */
                    /* Each parameter's mode, every one (#3574): past 64 a
                     * `@keep` or `@into` parameter's was dropped. */
                    int* modes = NULL;
                    int any_mode = 0;
                    if (!(peek_token(parser) && peek_token(parser)->type == TOKEN_RIGHT_PAREN)) {
                        do {
                            char* marks = NULL;
                            Token* mode_at = peek_token(parser);
                            parse_param_modes(parser, &marks);
                            int mode = FN_MODE_BORROWED;
                            if (marks) {
                                int keep = annotation_has_marker(marks, "keep_param");
                                int into = annotation_has_marker(marks, "into_param");
                                if (keep && into)
                                    parser_error_at(parser, mode_at,
                                                    "a parameter of a function type is "
                                                    "`@keep` or `@into`, not both");
                                mode = keep ? FN_MODE_KEEP : FN_MODE_INTO;
                                free(marks);
                            }
                            Type* p = parse_type(parser);
                            if (!p) {
                                parser_error(parser, "Expected type in fn() signature");
                                free(modes);
                                free_type(type);
                                return NULL;
                            }
                            type->param_count++;
                            type->param_types = aether_xrealloc(type->param_types,
                                (size_t)type->param_count * sizeof(Type*));
                            type->param_types[type->param_count - 1] = p;
                            modes = (int*)aether_xrealloc(modes, (size_t)type->param_count * sizeof(int));
                            modes[type->param_count - 1] = mode;
                            if (mode != FN_MODE_BORROWED) any_mode = 1;
                        } while (match_token(parser, TOKEN_COMMA));
                    }
                    if (any_mode) {
                        type->is_fnptr = 0;
                        for (int i = 0; i < type->param_count; i++)
                            fn_type_set_param_mode(type, i, modes[i]);
                    }
                    free(modes);
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
                } else if (strcmp(token->value, "void") == 0) {
                    /* `-> void`, the conventional spelling of "returns no
                     * value", is the type an omitted `-> R` gives. It fell
                     * through to the struct-name case below and became a
                     * struct called `void`, which the C spelling hid but the
                     * type checker did not: `fn(ptr, int) -> void` and
                     * `fn(ptr, int)` were two types, and a function written
                     * `-> void` matched neither's slot (#2623). */
                    type = create_type(TYPE_VOID);
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
                } else if (strcmp(token->value, "f32x4") == 0) {
                    /* #2146: four f32 lanes in one register, lowered to a
                     * GCC/Clang vector type. Arithmetic is lane-wise; the
                     * lanes are read as `.x .y .z .w`; `std.lanes` has the
                     * loads, stores, min/max, comparisons and select. */
                    type = create_type(TYPE_F32X4);
                } else if (strcmp(token->value, "f64x2") == 0) {
                    type = create_type(TYPE_F64X2);
                } else if (strcmp(token->value, "i64x2") == 0) {
                    /* The mask an f64x2 comparison yields — two 64-bit lanes,
                     * the width C gives it; an i32x4 there would reinterpret
                     * the register and scramble the lanes. */
                    type = create_type(TYPE_I64X2);
                } else if (strcmp(token->value, "i32x4") == 0) {
                    /* The mask a lane comparison yields (all-ones or zero per
                     * lane), and four 32-bit integer lanes in its own right. */
                    type = create_type(TYPE_I32X4);
                } else if (strcmp(token->value, "i16x8") == 0) {
                    /* #2212: eight 16-bit integer lanes, for integer image /
                     * audio kernels (JPEG IDCT, YCbCr->RGB): 16-bit fixed
                     * point with 32-bit accumulators and a saturating pack. */
                    type = create_type(TYPE_I16X8);
                } else if (strcmp(token->value, "f32x8") == 0) {
                    /* #2428: eight f32 lanes. One AVX register where the
                     * build enables AVX, two SSE/NEON halves where it does
                     * not; `std.lanes` has the eight-wide surface. */
                    type = create_type(TYPE_F32X8);
                } else if (strcmp(token->value, "i32x8") == 0) {
                    /* The mask an f32x8 comparison yields: eight 32-bit
                     * lanes, all-ones or zero, the width of its operands. */
                    type = create_type(TYPE_I32X8);
                } else if (strcmp(token->value, "int64") == 0) {
                    /* The signed sibling of the `uint64` keyword, and the
                     * name the reference uses for the widening `int -> int64`.
                     * Same type as `long`; it was falling through to the
                     * unknown-name path and binding as a struct called
                     * "int64", so `int64 n = 5` was a type mismatch. */
                    type = create_type(TYPE_INT64);
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
                     * (#878); this is the type-position analogue. The
                     * type is `Type` with `mod` as its qualifier, which
                     * the merge resolves to that module's struct (struct
                     * types are module-scoped). The leading identifier (`mod`) was already
                     * consumed; consume the `.` and the type name. Allow a
                     * multi-segment path (`a.b.Type`) by looping. */
                    type = parse_struct_type_name(parser, token);
                    if (!type) return NULL;
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
            // Optional type parameter: ActorRef[Type] or bare actor_ref. A
            // `[` that opens an array instead (`actor_ref[8]`,
            // `actor_ref[MAX]`, `actor_ref[]`) is the array suffix below.
            if (peek_token(parser) && peek_token(parser)->type == TOKEN_LEFT_BRACKET &&
                actor_ref_bracket_is_parameter(parser)) {
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
            /* A slice of slices: each further `[]` wraps once more, so
             * `int[][]` is a slice whose elements are `int[]` -- the type
             * `make([]int[], n)` already builds. Only the empty form
             * repeats; a fixed length after a slice has no C shape. */
            while (peek_token(parser) && peek_token(parser)->type == TOKEN_LEFT_BRACKET &&
                   peek_ahead(parser, 1) && peek_ahead(parser, 1)->type == TOKEN_RIGHT_BRACKET) {
                advance_token(parser);
                advance_token(parser);
                type = create_array_type(type, -1);
            }
        } else {
            // Fixed-size array: the length is an integer constant
            // expression (#3115), reported here when it is not one.
            int size = parse_array_length(parser);
            if (size > 0) {
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
static ASTNode* parse_interp_string_expr(const char* raw, int line, int column) {
    /* Carry the string's own position. It used to be created at 0,0, so any
     * diagnostic about an interpolated expression pointed at line 0 -- or, if
     * it fell back to the operand's node, at wherever that identifier was
     * DECLARED rather than where it was misused. */
    ASTNode* interp = create_ast_node(AST_STRING_INTERP, NULL, line, column);

    const char* p = raw;
    int lit_cap = 256;
    char* lit_buf = malloc(lit_cap);
    int lit_len = 0;

    // Helper lambda (C-style): flush current literal buffer as a child node.
    // By length: a decoded `\0` is a byte of the text (#2520).
    #define FLUSH_LIT() do { \
        lit_buf[lit_len] = '\0'; \
        ASTNode* _lit = create_ast_node(AST_LITERAL, NULL, 0, 0); \
        ast_set_literal_bytes(_lit, lit_buf, lit_len); \
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
            int sub_count = 0;
            Token** sub_tokens = lexer_tokenize(expr_src, &sub_count);
            lexer_restore(&saved);
            free(expr_src);
            if (!sub_tokens) { free(lit_buf); return interp; }

            /* The sub-lexer counts from 1:1 in expr_src. Rebase every token
             * onto the file: the string's line, plus the column of the `${`
             * inside the literal, so a diagnostic about an interpolated
             * expression lands on it and not on line 1 of the file. */
            int expr_col = column + (int)(expr_start - raw);
            for (int ti = 0; ti < sub_count; ti++) {
                Token* t = sub_tokens[ti];
                if (t->line == 1) t->column += expr_col;
                t->line += line - 1;
            }

            // Exclude trailing EOF from token count for sub-parser
            int n = sub_tokens[sub_count - 1]->type == TOKEN_EOF ? sub_count - 1 : sub_count;
            Parser* sub = create_parser(sub_tokens, n);
            ASTNode* expr_node = parse_expression(sub);
            free_parser(sub);
            /* AST nodes copy a token's text (create_ast_node strdups it) and
             * keep no reference to the token itself, so these are ours to
             * release: every `${...}` used to leak its whole token run
             * (#1667). */
            free_tokens(sub_tokens, sub_count);

            if (expr_node) add_child(interp, expr_node);
        } else if (*p == '\\' && p[1]) {
            // Escape sequence in literal segment: the lexer kept every escape
            // of an interpolated string as written, so this is its one
            // decoding, with the same decoder as a plain literal (#2512).
            if (lit_len >= lit_cap - 2) {
                lit_cap *= 2;
                char* nb = realloc(lit_buf, lit_cap);
                if (!nb) { free(lit_buf); return interp; }
                lit_buf = nb;
            }
            char ch;
            p += lexer_decode_escape(p, &ch);
            lit_buf[lit_len++] = ch;
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

static int body_is_assignment(Parser* parser);

/* Whether the last statement of an arrow body `-> { ... }`, a function's or
 * a closure's (#2697), is the value the body returns. Only an expression is
 * (#2694), and a `match`, which has a value when its arms have one (the
 * checker settles that). Any other statement stays one, and the body ends
 * with it:
 *  - control: an `if`, a loop, a `switch`, a `try`, a `defer`, a `panic`,
 *    a `return`, a `break`, a `continue` or a block. Wrapped in a `return`,
 *    `if c { return 1 }` emitted C that did not compile; a body that
 *    returns a value on some path must on every path (#2684).
 *  - a binding or a store: `x = v` (a new local or an existing one),
 *    `x += v`, `p.n = v`, `xs[i] = v`, `a, b = f()`, `const k = v`.
 *    Returned, `count = count + 1` handed back the sum and never stored
 *    it, `a += 3` emitted `return +=3;`, and a new string local did not fit
 *    the int result inferred for it (#2735).
 *  - a send, `a ! M { ... }`, which has no value. */
static int arrow_tail_is_value(ASTNode* last) {
    if (last->type == AST_MATCH_STATEMENT) return 1;
    if (last->type != AST_EXPRESSION_STATEMENT || last->child_count != 1) return 0;
    ASTNode* e = last->children[0];
    if (!e || e->type == AST_SEND_FIRE_FORGET) return 0;
    return !(e->type == AST_BINARY_EXPRESSION && e->value && strcmp(e->value, "=") == 0);
}

/* The return an arrow body's value `-> expr` / `-> { ...; expr }` makes,
 * marked `arrow_value`: whether a call or a `match` has a value is not
 * known here, and a call of a function that returns nothing has none, so
 * the checker turns a marked return of one back into the statement it
 * returns (#2697). */
static ASTNode* arrow_value_return(ASTNode* value, int line, int column) {
    ASTNode* ret = create_ast_node(AST_RETURN_STATEMENT, NULL, line, column);
    ret->annotation = strdup("arrow_value");
    add_child(ret, value);
    return ret;
}

/* The body of a single-expression arrow, `-> expr`, of a function or a
 * closure: the block that returns it. An assignment there, `-> n = n + 1`
 * or `-> p.n += 1`, is the statement `{ n = n + 1 }`, parsed as one (as a
 * match arm's is, #2575), so it stores and returns nothing, as `-> { n =
 * n + 1 }` does (#2735). Returned, a closure's store into a captured local
 * was lost. NULL on a parse error. */
static ASTNode* parse_arrow_expression_body(Parser* parser) {
    Token* at = peek_token(parser);
    if (!at) return NULL;
    ASTNode* body = create_ast_node(AST_BLOCK, NULL, at->line, at->column);
    if (body_is_assignment(parser)) {
        ASTNode* stmt = parse_statement(parser);
        if (!stmt) { free_ast_node(body); return NULL; }
        add_child(body, stmt);
        return body;
    }
    ASTNode* expr = parse_expression(parser);
    if (!expr) { free_ast_node(body); return NULL; }
    add_child(body, arrow_value_return(expr, expr->line, expr->column));
    return body;
}

/* An arrow body `-> { ... }`, of a function or a closure alike (#2697),
 * returns its last statement when that has a value to return. */
static void arrow_block_return_tail(ASTNode* body) {
    if (!body || body->child_count == 0) return;
    ASTNode* last = body->children[body->child_count - 1];
    if (!last || !arrow_tail_is_value(last)) return;
    // Wrap the trailing expression as the implicit return (a trailing
    // statement is not one, #2694). It goes in bare, out of its expression
    // statement: that is the node an explicit `return expr` holds, and the
    // passes that read a return's value (whether it hands over an owned
    // string, which local escapes through it) do not look inside a
    // statement. Wrapped, `msg = "n=${n}"` then `msg` freed msg at scope
    // exit and returned the freed pointer, which -O0 and -O2 printed as
    // different garbage (#2685).
    ASTNode* value = last;
    if (last->type == AST_EXPRESSION_STATEMENT &&
        last->child_count == 1 && last->children[0]) {
        value = last->children[0];
        last->children[0] = NULL;  // detach, so freeing last keeps it
        free_ast_node(last);
    }
    body->children[body->child_count - 1] = arrow_value_return(value, value->line, value->column);
}

/* What a function does with a `ptr` parameter, written before its type:
 * `@keep` (keeps it past the call, #2926), `@into` (it may reach only the
 * function's result, #2990). Each is a marker on the parameter; the type
 * checker refuses a combination, or either one where it does not apply. */
static void parse_param_modes(Parser* parser, char** annotation) {
    while (peek_token(parser) && peek_token(parser)->type == TOKEN_AT &&
           peek_ahead(parser, 1) && peek_ahead(parser, 1)->type == TOKEN_IDENTIFIER &&
           peek_ahead(parser, 1)->value &&
           (strcmp(peek_ahead(parser, 1)->value, "keep") == 0 ||
            strcmp(peek_ahead(parser, 1)->value, "into") == 0 ||
            strcmp(peek_ahead(parser, 1)->value, "frees") == 0 ||
            strcmp(peek_ahead(parser, 1)->value, "moves") == 0)) {
        /* #1300: `@frees` (the function frees what it is given) and
         * `@moves` (it may move the storage the argument holds). */
        const char* word = peek_ahead(parser, 1)->value;
        const char* marker = strcmp(word, "keep") == 0    ? "keep_param"
                             : strcmp(word, "into") == 0  ? "into_param"
                             : strcmp(word, "frees") == 0 ? "frees_param"
                                                          : "moves_param";
        advance_token(parser);
        advance_token(parser);
        *annotation = annotation_add_marker(*annotation, marker);
    }
}

// Parse closure expression: |params| -> expr  OR  |params| { block }
// Also handles: || { block } (no params, double-pipe)
ASTNode* parse_closure_expression(Parser* parser) {
    Token* start = peek_token(parser);
    int line = start->line, col = start->column;

    // Line of the token immediately before `start`, captured before we consume
    // anything, so the #1781 diagnostic below can tell whether this `||` opened
    // its own line (a wrapped condition) vs continued one. -1 if none.
    Token* start_prev = peek_ahead(parser, -1);
    int start_prev_line = start_prev ? start_prev->line : -1;

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
                if (!param_name || reject_none_binding(parser, param_name, "parameter name")) {
                    free_ast_node(closure);
                    return NULL;
                }
                ASTNode* param = create_ast_node(AST_CLOSURE_PARAM, param_name->value,
                                                  param_name->line, param_name->column);
                // Optional type annotation: |x: int|, and a parameter mode
                // (`|el: @keep ptr|`) the type checker rules on.
                if (match_token(parser, TOKEN_COLON)) {
                    parse_param_modes(parser, &param->annotation);
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
            // |x| -> { multi-statement block }: returns its last value, as a
            // function's arrow body does. `|x| { ... }` returns nothing it
            // does not `return`.
            ASTNode* body = parse_block(parser);
            arrow_block_return_tail(body);
            add_child(closure, body);
        } else {
            // |x| -> single_expression
            ASTNode* body = parse_arrow_expression_body(parser);
            if (!body) {
                free_ast_node(closure);
                return NULL;
            }
            add_child(closure, body);
        }
    } else if (next->type == TOKEN_LEFT_BRACE) {
        // Block body: |x| { statements }
        ASTNode* body = parse_block(parser);
        add_child(closure, body);
    } else {
        // #1781: a boolean condition wrapped so the continuation line begins
        // with `||` is misread as closure parameters — the `||` looked like an
        // empty param list, and the operand after it is neither `->` nor `{`.
        // Aether continues an expression only when the operator sits at the END
        // of the previous line (see operator_starts_newline); a leading operator
        // starts a fresh expression, so `|| b == 2 {` parses as a closure. When
        // this `||` opened its own line (its line is past the token before it),
        // that wrapped-condition mistake is by far the likeliest cause — say so,
        // instead of a bare "closure parameters" error far from the real problem.
        int opened_new_line = 0;
        if (start->type == TOKEN_OR && start_prev_line >= 0 &&
            start->line > start_prev_line) {
            opened_new_line = 1;
        }
        if (opened_new_line) {
            parser_error(parser,
                "a line starting with `||` is read as closure parameters; "
                "to continue a wrapped condition, put the operator at the END "
                "of the previous line (`a == 1 ||`) or parenthesise the whole "
                "condition");
        } else {
            parser_error(parser, "Expected '->' or '{' after closure parameters");
        }
        free_ast_node(closure);
        return NULL;
    }

    closure->node_type = create_type(TYPE_FUNCTION);
    return closure;
}

/* docs/quote.md: is the token at `offset` the `quote` of a quoted trailing
 * block? `quote` is not a keyword (std.quote is a module called `quote`, and
 * `quote` stays an ordinary name everywhere else): it means quoting only
 * straight after a call's closing `)`, on the same line, and followed by
 * `|`, `||`, `{` or `->`. Nothing else can follow a call that way, so
 * there is no reading of such code that this takes away. */
static int is_quote_trailing(Parser* parser, int offset, int paren_close_line) {
    Token* t = peek_ahead(parser, offset);
    if (!t || t->type != TOKEN_IDENTIFIER || !t->value || strcmp(t->value, "quote") != 0)
        return 0;
    if (t->line != paren_close_line) return 0;
    Token* after = peek_ahead(parser, offset + 1);
    if (!after) return 0;
    switch (after->type) {
        case TOKEN_PIPE:
        case TOKEN_OR:
        case TOKEN_LEFT_BRACE:
        case TOKEN_ARROW:
            return 1;
        default:
            return 0;
    }
}

/* Each call in a quoted body keeps its name as written (`math.sin`): the
 * type checker rewrites `value` to the resolved C-level name, and the tree
 * records the call as the source spelt it. */
static void quote_keep_call_names(ASTNode* n) {
    if (!n) return;
    if (n->type == AST_FUNCTION_CALL && n->value && !n->source_name)
        n->source_name = strdup(n->value);
    for (int i = 0; i < n->child_count; i++) quote_keep_call_names(n->children[i]);
}

/* `quote |a: T| { body }`, `quote |a: T| -> expr`, `quote { body }` or
 * `quote -> expr`, with the parser at `quote`. An AST_QUOTE whose children
 * are the parameters then the body block, as an AST_CLOSURE's are. */
static ASTNode* parse_quote_trailing(Parser* parser) {
    Token* q = peek_token(parser);
    advance_token(parser);  // consume 'quote'
    Token* next = peek_token(parser);
    ASTNode* quote = NULL;
    if (next->type == TOKEN_PIPE || next->type == TOKEN_OR) {
        quote = parse_closure_expression(parser);
        if (!quote) return NULL;
        quote->type = AST_QUOTE;
        if (quote->node_type) { free_type(quote->node_type); quote->node_type = NULL; }
    } else {
        quote = create_ast_node(AST_QUOTE, NULL, q->line, q->column);
        ASTNode* body = NULL;
        if (next->type == TOKEN_ARROW) {
            advance_token(parser);  // consume '->'
            if (peek_token(parser) && peek_token(parser)->type == TOKEN_LEFT_BRACE) {
                body = parse_block(parser);
                arrow_block_return_tail(body);
            } else {
                body = parse_arrow_expression_body(parser);
            }
        } else {
            body = parse_block(parser);
        }
        if (!body) { free_ast_node(quote); return NULL; }
        add_child(quote, body);
    }
    quote->line = q->line;
    quote->column = q->column;
    quote_keep_call_names(quote);
    return quote;
}

static ASTNode* parse_expression_inner(Parser* parser);
static ASTNode* parse_unary_expression_inner(Parser* parser);
static ASTNode* parse_statement_inner(Parser* parser);

/* The compound-assignment operators (`+=` ... `>>=`). */
static int token_is_compound_assign(Token* t) {
    if (!t) return 0;
    switch (t->type) {
        case TOKEN_PLUS_ASSIGN: case TOKEN_MINUS_ASSIGN: case TOKEN_MULTIPLY_ASSIGN:
        case TOKEN_DIVIDE_ASSIGN: case TOKEN_MODULO_ASSIGN: case TOKEN_AND_ASSIGN:
        case TOKEN_OR_ASSIGN: case TOKEN_XOR_ASSIGN: case TOKEN_LSHIFT_ASSIGN:
        case TOKEN_RSHIFT_ASSIGN:
            return 1;
        default:
            return 0;
    }
}

/* Why this lvalue cannot be evaluated twice without observable effect, or
 * 0 when it can: names, fields, elements, literals and arithmetic over
 * them. LVALUE_CALL: anything with a call in it. LVALUE_WRITE: a write
 * inside it (`a[i++] += 1` desugared to `a[i++] = a[i++] + 1` stepped `i`
 * twice, #2516). */
enum { LVALUE_CALL = 1, LVALUE_WRITE = 2 };
static int lvalue_unrepeatable(ASTNode* n) {
    if (!n) return 0;
    switch (n->type) {
        case AST_IDENTIFIER: case AST_LITERAL: case AST_MEMBER_ACCESS:
        case AST_ARRAY_ACCESS: case AST_BINARY_EXPRESSION: case AST_UNARY_EXPRESSION:
            break;
        default:
            return LVALUE_CALL;
    }
    if (n->value && ((n->type == AST_UNARY_EXPRESSION &&
                      (strcmp(n->value, "++") == 0 || strcmp(n->value, "--") == 0)) ||
                     (n->type == AST_BINARY_EXPRESSION && strcmp(n->value, "=") == 0)))
        return LVALUE_WRITE;
    for (int i = 0; i < n->child_count; i++) {
        int why = lvalue_unrepeatable(n->children[i]);
        if (why) return why;
    }
    return 0;
}
static ASTNode* parse_primary_expression_inner(Parser* parser);

/* CRITICAL: bounds the mutually recursive descent. Exceeding the C stack is a
 * SIGSEGV, not a diagnostic, so the depth is checked on the way in and the
 * error is reported once at the boundary rather than at every frame. */
static int parse_depth_enter(Parser* parser) {
    if (!parser) return 1;
    if (parser->depth >= AETHER_MAX_PARSE_DEPTH) {
        if (!parser->depth_exceeded) {
            parser->depth_exceeded = 1;
            const char* msg = "";
            msg = aether_internf(
                     "nested too deeply (limit %d); simplify the expression or "
                     "split the block into smaller functions",
                     AETHER_MAX_PARSE_DEPTH);
            parser_error(parser, msg);
        }
        return 0;
    }
    parser->depth++;
    return 1;
}

static void parse_depth_leave(Parser* parser) {
    if (parser && parser->depth > 0) parser->depth--;
}

ASTNode* parse_primary_expression(Parser* parser) {
    if (!parse_depth_enter(parser)) return NULL;
    ASTNode* _r = parse_primary_expression_inner(parser);
    parse_depth_leave(parser);
    return _r;
}

static ASTNode* parse_primary_expression_inner(Parser* parser) {
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

    /* #2974: `try_send(ref, Msg { ... })` sends as `ref ! Msg { ... }` does
     * and says whether it did: "" when sent, "the mailbox is full" or "the
     * actor is dead" when not. A send node marked "try_send", so the message
     * is built, owned and released as a send's is. Contextual: only the name
     * followed by `(` is it. */
    if (token->type == TOKEN_IDENTIFIER && token->value &&
        strcmp(token->value, "try_send") == 0 && peek_ahead(parser, 1) &&
        peek_ahead(parser, 1)->type == TOKEN_LEFT_PAREN) {
        advance_token(parser);   // try_send
        advance_token(parser);   // (
        ASTNode* target = parse_expression(parser);
        if (!target) return NULL;
        if (!expect_token(parser, TOKEN_COMMA)) return NULL;
        Token* name = peek_token(parser);
        if (!name || name->type != TOKEN_IDENTIFIER || !name->value ||
            name->value[0] < 'A' || name->value[0] > 'Z') {
            parser_error(parser, "try_send(ref, Msg { ... }) takes a message, as `!` does");
            return NULL;
        }
        ASTNode* message = parse_message_constructor(parser);
        if (!message) return NULL;
        if (!expect_token(parser, TOKEN_RIGHT_PAREN)) return NULL;
        ASTNode* send = create_ast_node(AST_SEND_FIRE_FORGET, NULL, token->line, token->column);
        send->annotation = strdup("try_send");
        add_child(send, target);
        add_child(send, message);
        return send;
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
            // If-expression: if COND { EXPR } else { EXPR }, or
            // `else if COND { EXPR } ... else { EXPR }`: an `else if` is
            // sugar for an else arm that is itself an if-expression, so a
            // chain of any length is nested two-armed if-expressions and
            // types as they do, the last `else` still required.
            Token* t = advance_token(parser); // consume 'if'
            /* #3478: the `{` after the condition opens the then arm, so a
             * call that ends the condition takes no trailing block, as in an
             * `if` statement or a `match` (the in_condition guard). The arms
             * sit inside braces, where a call may take one again. */
            int saved_in_condition = parser->in_condition;
            parser->in_condition = 1;
            ASTNode* cond = parse_expression(parser);
            parser->in_condition = 0;
            ASTNode* then_expr = NULL;
            ASTNode* else_expr = NULL;
            int ok = cond && expect_token(parser, TOKEN_LEFT_BRACE) &&
                     (then_expr = parse_expression(parser)) != NULL &&
                     expect_token(parser, TOKEN_RIGHT_BRACE) &&
                     expect_token(parser, TOKEN_ELSE);
            if (ok) {
                Token* after_else = peek_token(parser);
                if (after_else && after_else->type == TOKEN_IF) {
                    else_expr = parse_primary_expression(parser);
                    ok = else_expr != NULL;
                } else {
                    ok = expect_token(parser, TOKEN_LEFT_BRACE) &&
                         (else_expr = parse_expression(parser)) != NULL &&
                         expect_token(parser, TOKEN_RIGHT_BRACE);
                }
            }
            parser->in_condition = saved_in_condition;
            if (!ok) return NULL;

            ASTNode* if_expr = create_ast_node(AST_IF_EXPRESSION, NULL, t->line, t->column);
            if_expr->node_type = create_type(TYPE_UNKNOWN);
            add_child(if_expr, cond);
            add_child(if_expr, then_expr);
            add_child(if_expr, else_expr);
            return if_expr;
        }

        case TOKEN_INTERP_STRING: {
            Token* t = advance_token(parser);
            return parse_interp_string_expr(t->value, t->line, t->column);
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
                    /* #3174: the operand is any type a declaration takes,
                     * read by the type parser: `sizeof(int)`, `uint32`,
                     * `ptr`, `*Node`, `int[4]`, a struct. A struct named
                     * bare keeps its name as the node's value (the C tag
                     * form codegen and the module merge already handle);
                     * any other type rides in a type-annotation child. Only
                     * an identifier was read before: `sizeof(int)` was
                     * "'int' is a reserved keyword". */
                    Token* operand_tok = peek_token(parser);
                    Type* operand = parse_type(parser);
                    if (!operand) {
                        if (!parser->suppress_errors && operand_tok) {
                            aether_error_full(is_offsetof
                                                  ? "offsetof expects a struct type, then a field"
                                                  : "sizeof expects a type",
                                              operand_tok->line, operand_tok->column,
                                              is_offsetof ? "write offsetof(Point, x)"
                                                          : "write sizeof(int), sizeof(Point) or sizeof(*Node)",
                                              NULL, AETHER_ERR_SYNTAX);
                        }
                        return NULL;
                    }
                    int bare_struct = operand->kind == TYPE_STRUCT && operand->struct_name &&
                                      !operand->qualifier;
                    ASTNode* node = create_ast_node(
                        is_offsetof ? AST_OFFSETOF : AST_SIZEOF,
                        bare_struct ? operand->struct_name : NULL, line, column);
                    if (bare_struct && is_offsetof) {
                        free_type(operand);
                    } else {
                        /* A sizeof keeps the type even for a bare name, which
                         * may turn out to be an enum or a distinct type once
                         * names are resolved: codegen spells a struct by its
                         * tag and anything else as its C type. */
                        ASTNode* ty = create_ast_node(AST_TYPE_ANNOTATION, NULL,
                                                      operand_tok->line, operand_tok->column);
                        ty->node_type = operand;
                        add_child(node, ty);
                    }
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
             * (with the empty-struct variant `module.Type{}`). The
             * literal names `Type`, and keeps `module` on its parsed
             * type for the merge to resolve to that module's struct.
             * Filed in aether/new_aevg_asks.md ASK 2 from the AeVG
             * port. */
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
                        char* struct_name = strdup(t_type->value);
                        int s_line = token->line;
                        int s_col = token->column;
                        advance_token(parser);  /* module ident */
                        advance_token(parser);  /* . */
                        advance_token(parser);  /* Type ident */
                        advance_token(parser);  /* { */

                        ASTNode* struct_lit = create_ast_node(AST_STRUCT_LITERAL, struct_name, s_line, s_col);
                        /* The module stays on the literal's parsed type, as
                         * on `as *mod.T`: the merge resolves `mod.T` to that
                         * module's struct (struct types are module-scoped),
                         * and check_qualified_types holds `mod` to a module
                         * that has it. The checker sets the literal's own
                         * type in its place. */
                        struct_lit->node_type = create_type(TYPE_STRUCT);
                        struct_lit->node_type->struct_name = strdup(struct_name);
                        struct_lit->node_type->qualifier = aether_intern(token->value);
                        /* create_ast_node keeps its own copy, so this one is
                         * dead the moment it returns. Leaked once per struct
                         * literal in the program before this. */
                        free(struct_name);
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
                                Token* colon = expect_token(parser, TOKEN_COLON);
                                if (!colon) { free_ast_node(struct_lit); return NULL; }
                                int errors_before = aether_error_count();
                                ASTNode* value_expr = parse_expression(parser);
                                if (!value_expr) {
                                    missing_operand(parser, colon, errors_before, "a value");
                                    free_ast_node(struct_lit);
                                    return NULL;
                                }
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
                free(struct_name);

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
                        Token* colon = expect_token(parser, TOKEN_COLON);
                        if (!colon) {
                            free_ast_node(struct_lit);
                            return NULL;
                        }

                        // Parse field value
                        int errors_before = aether_error_count();
                        ASTNode* value_expr = parse_expression(parser);
                        if (!value_expr) {
                            missing_operand(parser, colon, errors_before, "a value");
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
            int errors_before = aether_error_count();
            ASTNode* expr = parse_expression(parser);
            if (!expr) {
                missing_operand(parser, token, errors_before, "an expression");
                return NULL;
            }
            /* #1033: `(a, b, ...)` — a comma after the first expression
             * upgrades the grouping to a tuple literal. Only meaningful
             * as an argument to a tuple-typed extern parameter (the
             * typechecker rejects it everywhere else); a plain
             * parenthesized expression `(a)` is unaffected. */
            if (peek_token(parser) && peek_token(parser)->type == TOKEN_COMMA) {
                ASTNode* tup = create_ast_node(AST_TUPLE_LITERAL, NULL, line, column);
                add_child(tup, expr);
                while (match_token(parser, TOKEN_COMMA)) {
                    Token* comma = peek_ahead(parser, -1);
                    errors_before = aether_error_count();
                    ASTNode* elem = parse_expression(parser);
                    if (!elem) {
                        missing_operand(parser, comma, errors_before, "an element");
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
                    /* A trailing comma, `[1, 2,]`, ends the list. Anywhere
                     * else an element has to follow its `,`: `[1, , 2]` was
                     * `[1, 2]` (#2998). */
                    Token* sep = peek_ahead(parser, -1);
                    if (array_lit->child_count > 0 && peek_token(parser) &&
                        peek_token(parser)->type == TOKEN_RIGHT_BRACKET)
                        break;
                    int errors_before = aether_error_count();
                    ASTNode* element = parse_expression(parser);
                    if (!element) {
                        missing_operand(parser, sep, errors_before,
                                        array_lit->child_count == 0
                                            ? "an element or `]`" : "an element");
                        free_ast_node(array_lit);
                        return NULL;
                    }
                    add_child(array_lit, element);
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
            char* func_name = parser_strf("spawn_%s", actor_name->value);
            ASTNode* spawn_call = create_ast_node(AST_FUNCTION_CALL, func_name, line, column);
            free(func_name);

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

        case TOKEN_MATCH: {
            /* #2993: a `match` is an expression wherever a value goes: an
             * operand, an argument, a condition, an interpolation. It was
             * no expression here, so `b = b + match i { ... }` parsed as
             * far as `b +`, failed without a word, and the declaration was
             * kept with no value while the `match` ran as the next
             * statement: `b` never changed, and `c = 1 + match ...`
             * declared `c` uninitialized. */
            Token* t = peek_token(parser);
            ASTNode* m = parse_match_statement(parser);
            if (m && m->line == 0) {
                m->line = t->line;
                m->column = t->column;
            }
            return m;
        }

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
    if (!parse_depth_enter(parser)) return NULL;
    ASTNode* _r = parse_expression_inner(parser);
    parse_depth_leave(parser);
    return _r;
}

static ASTNode* parse_expression_inner(Parser* parser) {
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
        int errors_before = aether_error_count();
        advance_token(parser);  // consume contextual 'or'
        ASTNode* handler;
        Token* nx = peek_token(parser);
        if (nx && nx->type == TOKEN_LEFT_BRACE) {
            handler = parse_block(parser);
        } else {
            handler = parse_expression(parser);   // bare default value
        }
        if (!handler) {
            missing_operand(parser, t, errors_before, "a default value or a block");
            free_ast_node(expr);
            return NULL;
        }
        ASTNode* oe = create_ast_node(AST_OR_ELSE, NULL, expr->line, expr->column);
        add_child(oe, expr);
        add_child(oe, handler);
        return oe;
    }
    return expr;
}

/* #2907: one more link of an operator chain parsed by a loop, `links` so far.
 * Past AETHER_MAX_EXPR_CHAIN it reports, once, at `at`, naming the limit, and
 * stops the parse as the depth guard does: the tree is as deep as the chain
 * is long, and every later pass walks it recursively. Returns 1 then. */
static int expr_chain_too_long(Parser* parser, int links, Token* at, const char* what) {
    if (links <= AETHER_MAX_EXPR_CHAIN) return 0;
    if (!parser->depth_exceeded) {
        parser->depth_exceeded = 1;
        if (!parser->suppress_errors && at) {
            const char* msg = "";
            msg = aether_internf("%s chains more than %d operations, the limit",
                     what, AETHER_MAX_EXPR_CHAIN);
            aether_error_full(msg, at->line, at->column,
                              "split it with intermediate locals", NULL, AETHER_ERR_SYNTAX);
        }
    }
    return 1;
}

ASTNode* parse_binary_expression(Parser* parser, int precedence) {
    ASTNode* left = parse_unary_expression(parser);
    if (!left) return NULL;

    /* Each pass consumes its operator, so the loop ends. It stopped after
     * 1000 operators at one level with nothing said, and the rest of the
     * expression was read as statements (#2907); the limit is reported now. */
    int links = 0;
    while (1) {
        Token* operator = peek_token(parser);
        if (!operator) break;

        int op_precedence = get_operator_precedence(operator->type);
        if (op_precedence < 0) break;  // Not an operator
        if (op_precedence < precedence) break;  // Lower precedence, stop

        if (operator_starts_newline(parser, operator)) {
            break;
        }
        if (expr_chain_too_long(parser, ++links, operator, "this expression")) {
            free_ast_node(left);
            return NULL;
        }

        advance_token(parser);
        int errors_before = aether_error_count();
        ASTNode* right = parse_binary_expression(parser, op_precedence + 1);  // Left-associative
        if (!right) {
            missing_operand(parser, operator, errors_before,
                            operator->type == TOKEN_ASSIGN ? "a value" : "an operand");
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

    /* A pass that consumed nothing ends the loop (each one that continues
     * has consumed its operator). It stopped after 100 postfix operations,
     * `a.b.c...` or `x[i][j]...`, with nothing said (#2907); a chain is
     * limited as a binary one is, and the limit is reported. */
    int last = -1;
    int links = 0;
    while (1) {
        if (parser->current_token == last) break;
        if (last >= 0 && expr_chain_too_long(parser, ++links, peek_token(parser),
                                             "this operand")) {
            free_ast_node(expr);
            return NULL;
        }
        last = parser->current_token;

        Token* op = peek_token(parser);
        if (!op) break;
        
        if (op->type == TOKEN_INCREMENT || op->type == TOKEN_DECREMENT) {
            /* A `++` on the next line is a prefix `++x` starting the next
             * statement (`z--` then `++z`), not a second postfix on this
             * operand. */
            Token* prev = peek_ahead(parser, -1);
            if (prev && prev->line != op->line) break;
            advance_token(parser);
            expr = create_unary_expression(expr, op);
            /* #2457: `i++` is the same node as `++i` but marked postfix, so
             * codegen emits C's `i++` and a used value is the old one. */
            expr->annotation = annotation_add_marker(expr->annotation, "postfix");
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
            // Array indexing: expr[index] — or a sub-slice (#1286):
            // expr[lo..hi], expr[lo..], expr[..hi], expr[..]. The range
            // end is exclusive, as in `for i in 0..n`.
            advance_token(parser); // consume '['
            ASTNode* lo = NULL;
            Token* after_open = peek_token(parser);
            if (!(after_open && after_open->type == TOKEN_DOTDOT)) {
                lo = parse_expression(parser);
                if (!lo) return NULL;
            }
            Token* sep = peek_token(parser);
            if (sep && sep->type == TOKEN_DOTDOT) {
                advance_token(parser); // consume '..'
                ASTNode* hi = NULL;
                Token* after_dots = peek_token(parser);
                if (!(after_dots && after_dots->type == TOKEN_RIGHT_BRACKET)) {
                    hi = parse_expression(parser);
                    if (!hi) return NULL;
                }
                if (!expect_token(parser, TOKEN_RIGHT_BRACKET)) return NULL;
                const char* shape = lo ? (hi ? "lo..hi" : "lo..") : (hi ? "..hi" : "..");
                ASTNode* slice = create_ast_node(AST_SLICE_EXPR, shape, op->line, op->column);
                add_child(slice, expr);
                if (lo) add_child(slice, lo);
                if (hi) add_child(slice, hi);
                expr = slice;
                continue;
            }
            if (!expect_token(parser, TOKEN_RIGHT_BRACKET)) return NULL;

            ASTNode* array_access = create_ast_node(AST_ARRAY_ACCESS, NULL, op->line, op->column);
            add_child(array_access, expr);  // array expression
            add_child(array_access, lo);    // index expression
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
            /* `as *mod.T`: the struct is `T`; the cast carries the
             * spelled `*mod.T` as its type until the checker sets its
             * own, so check_qualified_types sees the module. */
            Type* target = parse_struct_type_name(parser, struct_name_tok);
            if (!target) return NULL;
            ASTNode* cast = create_ast_node(AST_PTR_AS_STRUCT_CAST,
                                            target->struct_name,
                                            op->line, op->column);
            if (target->qualifier) {
                cast->node_type = create_type(TYPE_PTR);
                cast->node_type->element_type = target;
            } else {
                free_type(target);
            }
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
            /* #3099: any other callee is a value, called as a closure local
             * or field is (`call(callee, args)`, the form the checker gives
             * those). The callee was dropped and the call left nameless:
             * `fs[1]()` was "Undefined function '?'". */
            ASTNode* value_callee = NULL;
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
                // As long as the two names are (#3569): a 256-byte buffer
                // cut a longer one to a name nothing defines.
                func_name = parser_strf("%s.%s", expr->children[0]->value, expr->value);
            } else if (expr && expr->type == AST_MEMBER_ACCESS && expr->value &&
                       expr->child_count > 0 && expr->children[0]) {
                // #928 UFCS shape (a): receiver is not a bare identifier, so
                // there is no qualified name to form. Carry the bare method
                // name and detach the receiver subtree.
                func_name = strdup(expr->value);
                ufcs_recv = expr->children[0];
                expr->children[0] = NULL;   // detach so free_ast_node(expr) won't reclaim it
            } else if (expr) {
                func_name = strdup("call");
                value_callee = expr;
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

            /* Anchor the call on its CALLEE, not on the '('. The caret for
             * "Undefined function 'f'" pointed at the parenthesis after the
             * name, so the one token the message is about was the one token
             * not underlined. */
            int call_line = expr ? expr->line   : op->line;
            int call_col  = expr ? expr->column : op->column;
            ASTNode* func_call = create_ast_node(AST_FUNCTION_CALL, func_name, call_line, call_col);
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
            if (value_callee) {
                add_child(func_call, value_callee);
                func_call->annotation = annotation_add_marker(func_call->annotation, "value_callee");
            }

            // Parse arguments
            /* Inside the parentheses a `{` cannot open the body of an if, a
             * while or a match whose condition the call is part of, so the
             * arguments parse as they do anywhere: a struct literal, a call
             * with a trailing block (#3478 review; the `if` expression and the
             * statements alike). */
            int saved_args_in_condition = parser->in_condition;
            parser->in_condition = 0;
            if (!match_token(parser, TOKEN_RIGHT_PAREN)) {
                /* #2998: the `(` or `,` an argument has to follow, for the
                 * error when it is missing: `x = f(` used to compile. */
                Token* before_arg = peek_ahead(parser, -1);
                do {
                    int errors_before = aether_error_count();
                    // Check for named argument: IDENTIFIER : expr
                    Token* maybe_name = peek_token(parser);
                    Token* maybe_colon = peek_ahead(parser, 1);
                    if (maybe_name && maybe_name->type == TOKEN_IDENTIFIER &&
                        maybe_colon && maybe_colon->type == TOKEN_COLON) {
                        // Named argument
                        Token* name_tok = advance_token(parser); // consume name
                        Token* colon = advance_token(parser); // consume ':'
                        ASTNode* value = parse_expression(parser);
                        if (!value) {
                            missing_operand(parser, colon, errors_before, "a value");
                            if (peek_token(parser) &&
                                peek_token(parser)->type == TOKEN_RIGHT_PAREN)
                                advance_token(parser);   /* as below */
                            parser->in_condition = saved_args_in_condition;
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
                            missing_operand(parser, before_arg, errors_before,
                                            before_arg && before_arg->type == TOKEN_LEFT_PAREN
                                                ? "an argument or `)`" : "an argument");
                            /* #2998 review: the `)` that came instead closes
                             * this call; left for the block, a `)` on a line
                             * of its own was a second error, "Expected
                             * statement in block". */
                            if (peek_token(parser) &&
                                peek_token(parser)->type == TOKEN_RIGHT_PAREN)
                                advance_token(parser);
                            parser->in_condition = saved_args_in_condition;
                            free_ast_node(func_call);
                            return NULL;
                        }
                        add_child(func_call, arg);
                    }
                    before_arg = peek_token(parser);
                } while (match_token(parser, TOKEN_COMMA));

                if (!expect_token(parser, TOKEN_RIGHT_PAREN)) {
                    parser->in_condition = saved_args_in_condition;
                    free_ast_node(func_call);
                    return NULL;
                }
            }
            parser->in_condition = saved_args_in_condition;

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
                if (is_quote_trailing(parser, 0, paren_close_line)) {
                    // func(args) quote |x: T| { body } — the block is handed
                    // over as a tree, not run (docs/quote.md).
                    ASTNode* quoted = parse_quote_trailing(parser);
                    if (!quoted) {
                        free_ast_node(func_call);
                        return NULL;
                    }
                    add_child(func_call, quoted);
                } else if (next_tok && next_tok->type == TOKEN_CALLBACK) {
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

            // Free the original identifier node since we've copied its name;
            // a value callee is not freed, it is the call's first argument.
            if (expr && expr != value_callee) free_ast_node(expr);

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
    if (!parse_depth_enter(parser)) return NULL;
    ASTNode* _r = parse_unary_expression_inner(parser);
    parse_depth_leave(parser);
    return _r;
}

static ASTNode* parse_unary_expression_inner(Parser* parser) {
    Token* operator = peek_token(parser);
    if (!operator) return NULL;
    
    if (operator->type == TOKEN_EXCLAIM || operator->type == TOKEN_MINUS ||
        operator->type == TOKEN_TILDE ||
        operator->type == TOKEN_INCREMENT || operator->type == TOKEN_DECREMENT) {
        advance_token(parser);
        int errors_before = aether_error_count();
        ASTNode* operand = parse_unary_expression(parser);
        if (!operand) {
            missing_operand(parser, operator, errors_before, "an operand");
            return NULL;
        }
        return create_unary_expression(operand, operator);
    }

    // #2975: `move x` hands x's value over and leaves x moved-from. `move` is
    // contextual, not reserved: it is the operator only when the next token,
    // on the same line, is a name. Everything else spelled `move` stays a
    // name, so `fs.move(a, b)`, a glob-imported `move(dst, src, n)` and a
    // program's own `move()` keep working.
    if (operator->type == TOKEN_IDENTIFIER && operator->value &&
        strcmp(operator->value, "move") == 0) {
        Token* next = peek_ahead(parser, 1);
        if (next && next->type == TOKEN_IDENTIFIER && next->line == operator->line) {
            advance_token(parser);
            int errors_before = aether_error_count();
            ASTNode* operand = parse_unary_expression(parser);
            if (!operand) {
                missing_operand(parser, operator, errors_before, "a name to move");
                return NULL;
            }
            return create_unary_expression(operand, operator);
        }
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
        int errors_before = aether_error_count();
        ASTNode* operand = parse_unary_expression(parser);
        if (!operand) {
            missing_operand(parser, operator, errors_before, "an operand");
            return NULL;
        }
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

/* Does the match arm or arrow body at the cursor assign: a name, followed
 * by fields and indexes, then `=` or a compound assignment? */
static int body_is_assignment(Parser* parser) {
    Token* t = peek_ahead(parser, 0);
    if (!t || t->type != TOKEN_IDENTIFIER) return 0;
    int i = 1;
    for (;;) {
        t = peek_ahead(parser, i);
        if (!t) return 0;
        if (t->type == TOKEN_DOT) {
            Token* name = peek_ahead(parser, i + 1);
            if (!name || name->type != TOKEN_IDENTIFIER) return 0;
            i += 2;
        } else if (t->type == TOKEN_LEFT_BRACKET) {
            int depth = 0;
            do {
                t = peek_ahead(parser, i++);
                if (!t || t->type == TOKEN_EOF) return 0;
                if (t->type == TOKEN_LEFT_BRACKET) depth++;
                else if (t->type == TOKEN_RIGHT_BRACKET) depth--;
            } while (depth > 0);
        } else {
            return t->type == TOKEN_ASSIGN || token_is_compound_assign(t);
        }
    }
}

ASTNode* parse_statement(Parser* parser) {
    if (!parse_depth_enter(parser)) return NULL;
    ASTNode* _r = parse_statement_inner(parser);
    parse_depth_leave(parser);
    return _r;
}

static ASTNode* parse_statement_inner(Parser* parser) {
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
            if (reject_unbound_binding(parser, decl, "@scoped")) {
                free_ast_node(decl);
                return NULL;
            }
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
        case TOKEN_VAR: {
            const char* kw = token->type == TOKEN_LET ? "let" : "var";
            advance_token(parser);
            ASTNode* decl = parse_python_style_declaration(parser);
            if (reject_unbound_binding(parser, decl, kw)) {
                free_ast_node(decl);
                return NULL;
            }
            return decl;
        }

        case TOKEN_CONST: {
            // Local constant: const x = 5 or const arr[] = [1, 2, 3]
            int cline = token->line, ccol = token->column;
            advance_token(parser); // consume 'const'
            Token* cname = expect_token(parser, TOKEN_IDENTIFIER);
            if (!cname) return NULL;
            if (reject_none_binding(parser, cname, "constant name")) return NULL;

            // Check for array form: const NAME[] = [...]
            int is_array = 0;
            if (peek_token(parser) && peek_token(parser)->type == TOKEN_LEFT_BRACKET) {
                advance_token(parser); // consume '['
                if (!expect_token(parser, TOKEN_RIGHT_BRACKET)) return NULL;
                is_array = 1;
            }

            Token* ceq = expect_token(parser, TOKEN_ASSIGN);
            if (!ceq) return NULL;
            int cerrors = aether_error_count();
            ASTNode* cval = parse_expression(parser);
            if (!cval) {
                missing_operand(parser, ceq, cerrors, "a value");
                return NULL;
            }
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
                    /* Unknown, not `ptr`, as at module level: an element with
                     * no type yet (a struct literal, `-1`) is typed by
                     * inference, and the declaration takes the literal's
                     * type once it is (its array_fix). As `ptr`, `[XY {...}]`
                     * was refused as structs in an array of pointers. */
                    node->node_type = create_array_type(create_type(TYPE_UNKNOWN), cval->child_count);
                    node->type_inferred = 1;
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

        case TOKEN_PTR: {
            /* #2465: `ptr p = null` is a typed declaration, like `int x`. The
             * keyword is also usable as a value name (token_is_value_ident),
             * so only the `ptr NAME` shape on one line declares; anything
             * else (`ptr = q`, `ptr.f`) stays an expression statement. */
            Token* next = peek_ahead(parser, 1);
            if (next && next->type == TOKEN_IDENTIFIER && next->line == token->line) {
                return parse_variable_declaration(parser);
            }
            ASTNode* expr = parse_expression(parser);
            if (!expr) return NULL;
            match_token(parser, TOKEN_SEMICOLON);
            ASTNode* stmt = create_ast_node(AST_EXPRESSION_STATEMENT, NULL, token->line, token->column);
            add_child(stmt, expr);
            return stmt;
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
            int off = 2;
            /* `*mod.T name`: walk the `.IDENT` chain of a qualified name
             * (a deref-store `*p.f = v` ends at `=`, not at a name) */
            while (peek_ahead(parser, off) &&
                   peek_ahead(parser, off)->type == TOKEN_DOT &&
                   peek_ahead(parser, off + 1) &&
                   peek_ahead(parser, off + 1)->type == TOKEN_IDENTIFIER) {
                off += 2;
            }
            Token* t2 = peek_ahead(parser, off);  // the variable name
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
            // The binding name is on the type's line: `foo` on a line of its
            // own followed by `bar = 2` is two statements, and used to parse
            // as the declaration `foo bar = 2` — a "Type mismatch in
            // variable initialization" at `bar`.
            if (next && next->type == TOKEN_IDENTIFIER && next->line == token->line) {
                return parse_variable_declaration(parser);
            }
            // The optional form, `Point? q = ...` (a struct's optional;
            // `int? o` reaches parse_variable_declaration through the keyword
            // case), shape IDENT `?` IDENT `=` on one line. An ask has the
            // first three when its message has no braces (`d ? GetCount`,
            // with its `{` on the next line or none), even when the first
            // name is also a type's (`counter ? GetCount` beside `struct
            // counter`), but never a `=` after the message, so that is what
            // makes the declaration. One with no value is spelled `let r:
            // Point? = none`.
            if (next && next->type == TOKEN_QUESTION && next->line == token->line) {
                Token* nm = peek_ahead(parser, 2);
                Token* after = peek_ahead(parser, 3);
                if (nm && nm->type == TOKEN_IDENTIFIER && nm->line == token->line &&
                    after && after->line == nm->line && after->type == TOKEN_ASSIGN) {
                    return parse_variable_declaration(parser);
                }
            }
            // The array form of the same declaration, with an element
            // type spelled by an identifier: `uint32[4] xs = [...]`,
            // `uint16[2] hs`, `Pair[N + 1] ps`. Shape IDENT `[` length? `]`
            // IDENT on one line — an index expression statement (`xs[i]`)
            // is never followed by an identifier on its own line, exactly
            // as `IDENT IDENT` above; the line check keeps `xs[1]` followed
            // by a statement on the next line an expression. `int[4] xs`
            // reaches parse_variable_declaration through the keyword case;
            // without this branch `uint32[4] xs` parsed as an index into an
            // undefined variable `uint32`.
            if (next && next->type == TOKEN_LEFT_BRACKET) {
                int rbo = closing_bracket_offset(parser, 1);
                Token* rb = rbo > 0 ? peek_ahead(parser, rbo) : NULL;
                Token* nm = rbo > 0 ? peek_ahead(parser, rbo + 1) : NULL;
                if (rb && nm && nm->type == TOKEN_IDENTIFIER && nm->line == rb->line) {
                    return parse_variable_declaration(parser);
                }
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
                if (off > 1 && after && after->type == TOKEN_IDENTIFIER &&
                    after->line == token->line) {
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
                int errors_before = aether_error_count();
                ASTNode* rhs = parse_expression(parser);
                if (!rhs) {
                    missing_operand(parser, op, errors_before, "a value");
                    return NULL;
                }
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
                // `p.n += rhs` / `xs[i] -= rhs`: a compound assignment whose
                // target is a field or an element. It is `p.n = p.n + rhs`
                // and is built as that (the plain `=` form the typechecker
                // and codegen already handle for these targets). Only a
                // target the language can evaluate twice qualifies, a
                // chain of names, fields, indexes and arithmetic, with no
                // call and no write in it, since the desugaring repeats
                // it. Before, the statement stopped at the operator:
                // "Expected statement in block".
                Token* cop = peek_token(parser);
                if (cop && token_is_compound_assign(cop) &&
                    (expr->type == AST_MEMBER_ACCESS || expr->type == AST_ARRAY_ACCESS ||
                     expr->type == AST_FUNCTION_CALL)) {
                    int why = lvalue_unrepeatable(expr);
                    if (why) {
                        /* #2481: `f(x) += v` stopped at the operator with
                         * "Expected statement in block". #2516: `a[i++] += v`
                         * stepped `i` twice. */
                        parser_error(parser, expr->type == AST_FUNCTION_CALL
                            ? "cannot assign to the result of a call: it is a temporary, so the write would be lost; bind it to a variable first"
                            : why == LVALUE_WRITE
                            ? "the target of a compound assignment is read and written, so a write inside it (`i++`, `i = v`) would run twice; step the variable in a statement of its own first"
                            : "the target of a compound assignment must be a variable, a field or an element with no call in it; write `target = target op value` with a temporary instead");
                        /* Skip the rest of the statement's line so the
                         * operator and its operand are not reported again. */
                        int eline = cop->line;
                        while (peek_token(parser) && peek_token(parser)->line == eline &&
                               peek_token(parser)->type != TOKEN_RIGHT_BRACE &&
                               peek_token(parser)->type != TOKEN_EOF) advance_token(parser);
                        free_ast_node(expr);
                        return NULL;
                    }
                    advance_token(parser);
                    int errors_before = aether_error_count();
                    ASTNode* rhs = parse_expression(parser);
                    if (!rhs) {
                        missing_operand(parser, cop, errors_before, "a value");
                        free_ast_node(expr);
                        return NULL;
                    }
                    char base[8];
                    snprintf(base, sizeof(base), "%.*s", (int)strlen(cop->value) - 1, cop->value);
                    ASTNode* op_node = create_ast_node(AST_BINARY_EXPRESSION, base, cop->line, cop->column);
                    add_child(op_node, clone_ast_node(expr));
                    add_child(op_node, rhs);
                    ASTNode* assign = create_ast_node(AST_BINARY_EXPRESSION, "=", cop->line, cop->column);
                    add_child(assign, expr);
                    add_child(assign, op_node);
                    match_token(parser, TOKEN_SEMICOLON);
                    ASTNode* stmt = create_ast_node(AST_EXPRESSION_STATEMENT, NULL, token->line, token->column);
                    add_child(stmt, assign);
                    return stmt;
                }
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
    if (reject_none_binding(parser, name, "variable name")) return NULL;
    
    ASTNode* decl = create_ast_node(AST_VARIABLE_DECLARATION, name->value, name->line, name->column);
    decl->node_type = type;

    Token* eq = peek_token(parser);
    if (match_token(parser, TOKEN_ASSIGN)) {
        /* #2790, #2803: `int k = match n { ... }` binds the match's value,
         * as `k = match` and `let k: int = match` do. A `match` is an
         * expression (#2993), so one with more after it (`int k = match n
         * { ... } + 1`) is an operand of that. */
        int errors_before = aether_error_count();
        ASTNode* value = parse_expression(parser);
        if (!value) {
            /* #2998: `int x =` with nothing after the `=`. */
            missing_operand(parser, eq, errors_before, "a value");
            free_ast_node(decl);
            return NULL;
        }
        add_child(decl, value);
    }
    
    if (expect_semicolon) {
        match_token(parser, TOKEN_SEMICOLON);
    }
    return decl;
}

// Python-style variable declaration: x = 42 (no 'let', type inferred)
/* `let` / `var` bind, so a binding with nothing bound is a typo rather than a
 * declaration: `let mut x = 0` used to parse as a bare `mut` plus a separate
 * `x = 0`, leaving the program a stray variable named after a keyword Aether
 * does not have (#1644). Reports, drops the rest of the statement so its
 * remains are not reported a second time, and returns 1 when `decl` is
 * rejected. */
static int reject_unbound_binding(Parser* parser, ASTNode* decl, const char* kw) {
    if (!decl || decl->type != AST_VARIABLE_DECLARATION || decl->child_count > 0)
        return 0;
    if (!parser->suppress_errors) {
        Token* stray = peek_token(parser);
        const char* msg = "";
        msg = aether_internf("`%s %s` binds nothing: expected `= <expression>`",
                 kw, decl->value ? decl->value : "?");
        if (stray && stray->type == TOKEN_IDENTIFIER && stray->value) {
            const char* hint = "";
            hint = aether_internf(
                     "a typed declaration is written without `%s` (`%s %s = ...`); "
                     "an inferred one names only the variable (`%s %s = ...`)",
                     kw, decl->value ? decl->value : "?", stray->value, kw, stray->value);
            aether_error_with_suggestion(msg, decl->line, decl->column, hint);
        } else {
            aether_error_with_code(msg, decl->line, decl->column, AETHER_ERR_SYNTAX);
        }
    }
    int line = decl->line;
    while (!is_at_end(parser)) {
        Token* rest = peek_token(parser);
        if (!rest || rest->type == TOKEN_RIGHT_BRACE || rest->line > line) break;
        advance_token(parser);
        if (rest->type == TOKEN_SEMICOLON) break;
    }
    return 1;
}

ASTNode* parse_python_style_declaration(Parser* parser) {
    // Accept a value-identifier token as the binding name — TOKEN_IDENTIFIER,
    // `state` (a regular identifier outside actors), or the #880 keyword set.
    Token* name = peek_token(parser);
    if (!token_is_value_ident(name)) {
        parser_error(parser, "Expected identifier");
        return NULL;
    }
    if (reject_none_binding(parser, name, "variable name")) return NULL;
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
                if (reject_none_binding(parser, next_name, "variable name")) {
                    free_ast_node(destructure);
                    return NULL;
                }
                advance_token(parser);
                ASTNode* var = create_ast_node(AST_VARIABLE_DECLARATION, next_name->value, next_name->line, next_name->column);
                var->node_type = create_type(TYPE_UNKNOWN);
                add_child(destructure, var);
            } else {
                parser_error(parser, "Expected identifier in tuple destructuring");
                break;
            }
        }

        Token* eq = expect_token(parser, TOKEN_ASSIGN);
        if (!eq) {
            free_ast_node(destructure);
            return NULL;
        }

        // Parse RHS expression
        int errors_before = aether_error_count();
        ASTNode* rhs = parse_expression(parser);
        if (!rhs) {
            /* #2998: `a, b =` with nothing after the `=`. */
            missing_operand(parser, eq, errors_before, "a value");
            free_ast_node(destructure);
            return NULL;
        }
        add_child(destructure, rhs);  // Last child is the RHS

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

    Token* eq = peek_token(parser);
    if (match_token(parser, TOKEN_ASSIGN)) {
        // `x = match val { ... }` binds the match's value; a `match` with
        // more after it is an operand of that (#2993).
        int errors_before = aether_error_count();
        ASTNode* value = parse_expression(parser);
        if (!value) {
            /* #2998: `x = }` declared `x` with no value and compiled. */
            missing_operand(parser, eq, errors_before, "a value");
            free_ast_node(decl);
            return NULL;
        }
        add_child(decl, value);
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
    Token* if_tok = peek_token(parser);
    int if_line = if_tok ? if_tok->line : 0;
    int if_col  = if_tok ? if_tok->column : 0;
    advance_token(parser); // if
    int saved_in_condition = parser->in_condition;
    parser->in_condition = 1;
    ASTNode* condition = parse_expression(parser);
    parser->in_condition = saved_in_condition;
    if (!condition) return NULL;
    
    ASTNode* then_branch = parse_statement(parser);
    if (!then_branch) return NULL;
    
    ASTNode* if_stmt = create_ast_node(AST_IF_STATEMENT, NULL, if_line, if_col);
    add_child(if_stmt, condition);
    add_child(if_stmt, then_branch);

    /* The else block needs a position of its own. Codegen emits a #line before
     * `} else {`, and with no position there the C line inherits the count
     * drifting on from the then-branch and lands on the first line of the ELSE
     * BODY. gcov then charges the branch-decision counter to a statement that
     * never ran, and an untaken else reports as covered. */
    Token* else_tok = peek_token(parser);
    int else_line = else_tok ? else_tok->line : 0;
    int else_col  = else_tok ? else_tok->column : 0;
    if (match_token(parser, TOKEN_ELSE)) {
        ASTNode* else_branch = parse_statement(parser);
        if (else_branch) {
            if (else_branch->line <= 0) {
                else_branch->line = else_line;
                else_branch->column = else_col;
            }
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
        // `{` ends this expression when it is a collection (`for x in xs {`),
        // the same trailing-block ambiguity if/while have; a range's start
        // (`0..n`) is unaffected.
        int saved_in_cond_start = parser->in_condition;
        parser->in_condition = 1;
        ASTNode* start_expr = parse_expression(parser);
        parser->in_condition = saved_in_cond_start;
        if (!start_expr) return NULL;
        Token* after_start = peek_token(parser);
        if (after_start && after_start->type != TOKEN_DOTDOT) {
            // Element loop: for x in xs { body }. Lowered to the index loop
            //   { _fi_xs = xs; _fi_n = _fi_xs.len
            //     for (_fi_i = 0; _fi_i < _fi_n; _fi_i++) { x = _fi_xs[_fi_i]; body } }
            // so `xs` is evaluated once, `x` is bound by value, its type is
            // inferred from the element, and break/continue are the plain
            // loop's. Works for any slice or fixed array. A bare name or a
            // field path is indexed in place: binding a named fixed array to
            // a new name decays it to a `ptr`, which has no length, and
            // reading a name twice has no side effect to repeat.
            ASTNode* body = parse_statement(parser);
            if (!body) return NULL;
            static int for_in_counter = 0;
            char xs_name[48], idx_name[48], n_name[48];
            int id = for_in_counter++;
            snprintf(xs_name, sizeof xs_name, "_fi_xs_%d", id);
            snprintf(idx_name, sizeof idx_name, "_fi_i_%d", id);
            snprintf(n_name, sizeof n_name, "_fi_n_%d", id);
            int ln = var_name->line, col = var_name->column;

            int in_place = 1;
            for (ASTNode* e = start_expr; e; e = e->child_count ? e->children[0] : NULL) {
                if (e->type == AST_IDENTIFIER) break;
                if (e->type != AST_MEMBER_ACCESS || e->child_count != 1) { in_place = 0; break; }
            }
            #define FI_XS() (in_place ? clone_ast_node(start_expr) \
                                      : create_ast_node(AST_IDENTIFIER, xs_name, ln, col))

            ASTNode* wrap = create_ast_node(AST_BLOCK, NULL, ln, col);
            if (!in_place) {
                ASTNode* xs_decl = create_ast_node(AST_VARIABLE_DECLARATION, xs_name, ln, col);
                xs_decl->node_type = create_type(TYPE_UNKNOWN);
                xs_decl->type_inferred = 1;
                add_child(xs_decl, start_expr);
                add_child(wrap, xs_decl);
            }
            ASTNode* n_decl = create_ast_node(AST_VARIABLE_DECLARATION, n_name, ln, col);
            n_decl->node_type = create_type(TYPE_UNKNOWN);
            n_decl->type_inferred = 1;
            ASTNode* len = create_ast_node(AST_MEMBER_ACCESS, "len", ln, col);
            add_child(len, FI_XS());
            add_child(n_decl, len);
            add_child(wrap, n_decl);

            ASTNode* init = create_ast_node(AST_VARIABLE_DECLARATION, idx_name, ln, col);
            init->node_type = create_type(TYPE_UNKNOWN);
            ASTNode* zero = create_ast_node(AST_LITERAL, "0", ln, col);
            zero->node_type = create_type(TYPE_INT);
            add_child(init, zero);

            Token lt_op = { .type = TOKEN_LESS, .value = "<", .line = ln, .column = col };
            ASTNode* condition = create_binary_expression(
                create_ast_node(AST_IDENTIFIER, idx_name, ln, col),
                create_ast_node(AST_IDENTIFIER, n_name, ln, col), &lt_op);

            Token inc_op = { .type = TOKEN_INCREMENT, .value = "++", .line = ln, .column = col };
            ASTNode* increment = create_unary_expression(
                create_ast_node(AST_IDENTIFIER, idx_name, ln, col), &inc_op);

            ASTNode* elem = create_ast_node(AST_VARIABLE_DECLARATION, var_name->value, ln, col);
            elem->node_type = create_type(TYPE_UNKNOWN);
            ASTNode* at = create_ast_node(AST_ARRAY_ACCESS, NULL, ln, col);
            add_child(at, FI_XS());
            add_child(at, create_ast_node(AST_IDENTIFIER, idx_name, ln, col));
            #undef FI_XS
            add_child(elem, at);

            // Element binding goes first in the body block.
            ASTNode* loop_body = body;
            if (body->type != AST_BLOCK) {
                loop_body = create_ast_node(AST_BLOCK, NULL, ln, col);
                add_child(loop_body, body);
            }
            add_child(loop_body, elem);
            for (int k = loop_body->child_count - 1; k > 0; k--)
                loop_body->children[k] = loop_body->children[k - 1];
            loop_body->children[0] = elem;

            ASTNode* for_loop = create_ast_node(AST_FOR_LOOP, NULL, ln, col);
            for_loop->children = malloc(4 * sizeof(ASTNode*));
            for_loop->child_capacity = 0;
            if (!for_loop->children) { free_ast_node(for_loop); return NULL; }
            for_loop->child_count = 4;
            for_loop->children[0] = init;
            for_loop->children[1] = condition;
            for_loop->children[2] = increment;
            for_loop->children[3] = loop_body;

            add_child(wrap, for_loop);
            return wrap;
        }
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
            /* #2734: a bare `x = expr`, as anywhere else: it assigns a
             * variable already bound there. The checker clears the mark
             * for a new name, which the loop declares as its own, as it
             * does a typed init's (`int i = 0`) or a range-for's. */
            init->type_inferred = 1;
            Token* eq = peek_token(parser);
            if (match_token(parser, TOKEN_ASSIGN)) {
                int errors_before = aether_error_count();
                ASTNode* value = parse_expression(parser);
                if (!value) {
                    missing_operand(parser, eq, errors_before, "a value");
                    free_ast_node(init);
                    return NULL;
                }
                add_child(init, value);
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
        /* #2742: an increment that binds a variable (`s = f(s)`, `i += 2`)
         * is the statement it would be at the end of the body, the
         * binding a while-loop rewrite makes there, so it gets the same
         * node and, in codegen, the same ownership: a string the binding
         * replaces is freed, a cell is stored through. Parsed as an
         * expression, it was a raw C assignment in the header. Any other
         * increment (`i++`, a call) stays an expression. */
        Token* step = peek_token(parser);
        Token* op = peek_ahead(parser, 1);
        if (step && step->type == TOKEN_IDENTIFIER && op &&
            (op->type == TOKEN_ASSIGN || token_is_compound_assign(op))) {
            increment = parse_statement(parser);
        } else {
            increment = parse_expression(parser);
        }
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
    
    /* No cap on the cases: each pass consumes a case or a token. It stopped
     * after 1000 cases (saying "max 100", through a message that printed
     * nothing), and the rest of the switch was read as statements (#2907). */
    while (!match_token(parser, TOKEN_RIGHT_BRACE) && !is_at_end(parser)) {
        int start = parser->current_token;
        int errors_before = aether_error_count();
        ASTNode* case_stmt = parse_case_statement(parser);
        if (case_stmt) {
            add_child(switch_stmt, case_stmt);
        } else {
            if (aether_error_count() == errors_before)
                parser_error(parser, "Expected 'case' or 'default' in switch statement");
            if (parser->current_token == start) advance_token(parser);
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
    /* `1 | 2 | 3 ->` reads as alternatives and is the bitwise OR: it matched
     * only 3, silently. Alternatives are a comma-list here, so a `|` at the
     * top of a selector is refused with the spelling that was meant. */
    if (lo->type == AST_BINARY_EXPRESSION && lo->value && strcmp(lo->value, "|") == 0) {
        if (!parser->suppress_errors) {
            aether_error_full(
                "`|` in a match/switch selector is the bitwise OR, not an alternative: "
                "`1 | 2 | 3` matches only 3",
                lo->line, lo->column,
                "write alternatives as a comma-list: `1, 2, 3 ->`",
                NULL, AETHER_ERR_SYNTAX);
        }
        free_ast_node(lo);
        return NULL;
    }
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
        
        /* No cap on the statements: each pass consumes a statement or a
         * token. It stopped after 1000 with nothing said (#2907). */
        while (!is_at_end(parser)) {
            Token* next = peek_token(parser);
            if (!next || next->type == TOKEN_CASE || next->type == TOKEN_DEFAULT || next->type == TOKEN_RIGHT_BRACE) {
                break;
            }
            int start = parser->current_token;
            int errors_before = aether_error_count();
            ASTNode* stmt = parse_statement(parser);
            if (stmt) {
                add_child(case_stmt, stmt);
            } else {
                /* A token no statement starts with was skipped without a
                 * word. */
                if (aether_error_count() == errors_before)
                    parser_error(parser, "Expected statement in case block");
                if (parser->current_token == start) advance_token(parser);
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
        
        /* No cap on the statements: each pass consumes a statement or a
         * token. It stopped after 1000 with nothing said (#2907). */
        while (!is_at_end(parser)) {
            Token* next = peek_token(parser);
            if (!next || next->type == TOKEN_CASE || next->type == TOKEN_DEFAULT || next->type == TOKEN_RIGHT_BRACE) {
                break;
            }
            int start = parser->current_token;
            int errors_before = aether_error_count();
            ASTNode* stmt = parse_statement(parser);
            if (stmt) {
                add_child(case_stmt, stmt);
            } else {
                /* A token no statement starts with was skipped without a
                 * word. */
                if (aether_error_count() == errors_before)
                    parser_error(parser, "Expected statement in case block");
                if (parser->current_token == start) advance_token(parser);
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
    
    /* The arms sit inside the match's braces, so a `match` that is part of
     * a condition (#2993) does not parse them as one: a call in an arm may
     * take a trailing block. No cap on the arms: each pass consumes an arm
     * or a token. It stopped after 1000 arms, and a token no arm starts with
     * was skipped, both through a message that printed nothing (#2907). */
    parser->in_condition = 0;
    // Parse match arms
    while (!match_token(parser, TOKEN_RIGHT_BRACE) && !is_at_end(parser)) {
        int start = parser->current_token;
        int errors_before = aether_error_count();
        ASTNode* match_arm = parse_match_case(parser);
        if (match_arm) {
            add_child(match_stmt, match_arm);
        } else {
            if (aether_error_count() == errors_before)
                parser_error(parser, "Expected a match arm, `pattern -> value`");
            if (parser->current_token == start) advance_token(parser);
        }
    }
    parser->in_condition = saved_in_condition;
    
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
    } else if (current->type == TOKEN_IDENTIFIER && peek_ahead(parser, 1) &&
               peek_ahead(parser, 1)->type == TOKEN_LEFT_BRACE) {
        // #2992: struct pattern `Pt { x: 0, y: _ }`, as a clause head takes
        // one (#2988). Read as an expression it was a struct literal, whose
        // `_` was an undefined variable.
        pattern = parse_struct_pattern(parser);
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
    Token* arrow = expect_token(parser, TOKEN_ARROW);
    if (!arrow) { free_ast_node(pattern); return NULL; }

    // Parse the result (expression, statement, or block)
    ASTNode* result = NULL;
    Token* next = peek_token(parser);
    int errors_before = aether_error_count();

    if (next && next->type == TOKEN_LEFT_BRACE) {
        // Block result
        result = parse_block(parser);
    } else if (body_is_assignment(parser)) {
        /* #2575: `1 -> o.f = v` / `1 -> x = v` is the statement `{ o.f = v }`
         * and parsed as one, so every pass sees the tree a braced arm has.
         * Parsed as an expression it was emitted as a bare C assignment:
         * a field store neither freed what it replaced nor took ownership,
         * and a local's reassignment skipped its tracker. */
        Token* at = next;
        ASTNode* stmt = parse_statement(parser);
        if (stmt) {
            result = create_ast_node(AST_BLOCK, NULL, at->line, at->column);
            add_child(result, stmt);
        }
    } else if (next && next->type == TOKEN_PRINT) {
        // print/println is a statement keyword, not an expression
        result = parse_statement(parser);
    } else {
        // Expression result
        result = parse_expression(parser);
        /* #2993: an arm that is a `match` (`0 -> match b { ... }`) is the
         * block `{ match b { ... } }`, so every pass sees the tree a braced
         * arm has: the inner match yields for this one. */
        if (result && result->type == AST_MATCH_STATEMENT) {
            ASTNode* block = create_ast_node(AST_BLOCK, NULL, result->line, result->column);
            add_child(block, result);
            result = block;
        }
    }
    
    if (!result) {
        /* `1 -> }`: the arm has nothing after its `->`. */
        missing_operand(parser, arrow, errors_before, "a value or a block");
        free_ast_node(pattern);
        return NULL;
    }

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
    /* As long as its parts (#3569): 256 bytes cut it to another name. */
    const char* module_name = aether_intern(name_token->value);
    
    while (match_token(parser, TOKEN_DOT)) {
        Token* part = expect_token(parser, TOKEN_IDENTIFIER);
        if (!part) break;
        module_name = aether_internf("%s.%s", module_name, part->value);
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
    /* As long as its parts (#3569): 256 bytes cut it to another name. */
    const char* module_name = aether_intern(name_token->value);

    while (match_token(parser, TOKEN_DOT)) {
        Token* part = peek_token(parser);
        if (!is_module_name_token(part)) break;
        advance_token(parser);  // consume the part
        module_name = aether_internf("%s.%s", module_name, part->value);
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
                    /* As long as the alias (#3569): 160 bytes cut it. */
                    symbol_node->annotation = parser_strf("select_alias:%s", alias->value);
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

/* #2200: `cfn Name(a: T1, b: T2) -> R` — a named C function-pointer type,
 * the typedef form of `fn(T1, T2) -> R`. OpenGL past 1.1 and all of Vulkan
 * are reached through pointers fetched at run time (wglGetProcAddress,
 * vkGetDeviceProcAddr, dlsym); naming each signature once lets a program
 * declare `var gen_buffers: GenBuffers = null`, load it with
 * `p as GenBuffers`, and call it with C's calling convention.
 *
 * The parameter list takes the extern spelling (`name: type`) so a
 * declaration can be pasted from the C prototype, and also a bare type
 * list (`cfn Add(int, int) -> int`) to match the anonymous `fn(...)`
 * form. Omitting `-> R` means void. Only the types survive: the node is
 * `value` = Name with `node_type` = the TYPE_FUNCTION (is_fnptr=1). */
ASTNode* parse_cfn_type_def(Parser* parser) {
    Token* name = expect_token(parser, TOKEN_IDENTIFIER);
    if (!name) return NULL;
    if (!expect_token(parser, TOKEN_LEFT_PAREN)) return NULL;

    Type* sig = create_type(TYPE_FUNCTION);
    sig->is_fnptr = 1;
    if (!match_token(parser, TOKEN_RIGHT_PAREN)) {
        do {
            /* `name: type` when an identifier is followed by `:`; otherwise
             * the token starts a bare type (`int`, `ptr`, `*Thing`, ...). */
            Token* t0 = peek_token(parser);
            Token* t1 = peek_ahead(parser, 1);
            if (t0 && t0->type == TOKEN_IDENTIFIER && t1 && t1->type == TOKEN_COLON) {
                advance_token(parser);   // parameter name
                advance_token(parser);   // ':'
            }
            Type* p = parse_type(parser);
            if (!p) {
                parser_error(parser, "Expected a parameter type in cfn signature "
                                     "(`cfn Name(n: int, ids: ptr) -> int`)");
                free_type(sig);
                return NULL;
            }
            sig->param_count++;
            sig->param_types = aether_xrealloc(sig->param_types,
                (size_t)sig->param_count * sizeof(Type*));
            sig->param_types[sig->param_count - 1] = p;
        } while (match_token(parser, TOKEN_COMMA));
        if (!expect_token(parser, TOKEN_RIGHT_PAREN)) {
            free_type(sig);
            return NULL;
        }
    }
    if (peek_token(parser) && peek_token(parser)->type == TOKEN_ARROW) {
        advance_token(parser);   // consume '->'
        sig->return_type = parse_type(parser);
        if (!sig->return_type) {
            parser_error(parser, "Expected a return type after `->` in cfn signature");
            free_type(sig);
            return NULL;
        }
    } else {
        sig->return_type = create_type(TYPE_VOID);
    }

    ASTNode* def = create_ast_node(AST_CFN_TYPE_DEF, name->value, name->line, name->column);
    def->node_type = sig;
    match_token(parser, TOKEN_SEMICOLON);
    return def;
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
            Token* after2 = peek_ahead(parser, 2);
            if (next->value && strcmp(next->value, "cfn") == 0 &&
                after && after->type == TOKEN_IDENTIFIER &&
                after2 && after2->type == TOKEN_LEFT_PAREN) {
                // #2200: `export cfn Name(...) -> R`
                advance_token(parser);   // consume 'cfn'
                exported_item = parse_cfn_type_def(parser);
            } else if (after && after->type == TOKEN_LEFT_PAREN) {
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
        /* #3150: `return try { ... } catch e { ... }` on the return's line
         * is a `try` given as the value, which it cannot be. Read on, it
         * was a bare `return` (an E0200 in a function with a result) and a
         * try statement after it. */
        Token* tv = peek_token(parser);
        if (tv && tv->type == TOKEN_TRY && ret_tok && tv->line == ret_tok->line) {
            missing_operand(parser, ret_tok, aether_error_count(), "a value");
            free_ast_node(return_stmt);
            return NULL;
        }
        // #1054: `return match x { ... }`; codegen lowers a match in return
        // position to a value-producing form. A `match` is an expression
        // (#2993), so `return match x { ... } + 1` returns the sum.
        ASTNode* value = parse_expression(parser);
        if (value) {
            add_child(return_stmt, value);
        }

        // Multiple return values: return a, b
        while (value && peek_token(parser) && peek_token(parser)->type == TOKEN_COMMA) {
            Token* comma = advance_token(parser);  // consume comma
            int errors_before = aether_error_count();
            ASTNode* next_val = parse_expression(parser);
            if (!next_val) {
                /* #2998: `return a,` returned `a` alone. */
                missing_operand(parser, comma, errors_before, "a value");
                free_ast_node(return_stmt);
                return NULL;
            }
            add_child(return_stmt, next_val);
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
/* #385: `protocol Name { from -> Message -> to ... }`, one transition per
 * line (a comma may separate them too). `protocol` is a contextual word: the
 * caller has seen `protocol <ident> {`. */
static ASTNode* parse_protocol_definition(Parser* parser) {
    Token* kw = peek_token(parser);
    advance_token(parser);   // consume 'protocol'
    Token* name = expect_token(parser, TOKEN_IDENTIFIER);
    if (!name) return NULL;
    if (!expect_token(parser, TOKEN_LEFT_BRACE)) return NULL;
    ASTNode* proto = create_ast_node(AST_PROTOCOL_DEFINITION, name->value, kw->line, kw->column);
    while (!match_token(parser, TOKEN_RIGHT_BRACE)) {
        if (is_at_end(parser)) {
            parser_error(parser, "Unexpected end of file in protocol definition: its `}` is missing");
            return proto;
        }
        Token* from = expect_token(parser, TOKEN_IDENTIFIER);
        if (!from) return proto;
        if (!expect_token(parser, TOKEN_ARROW)) return proto;
        Token* msg = expect_token(parser, TOKEN_IDENTIFIER);
        if (!msg) return proto;
        if (!expect_token(parser, TOKEN_ARROW)) return proto;
        Token* to = expect_token(parser, TOKEN_IDENTIFIER);
        if (!to) return proto;
        ASTNode* t = create_ast_node(AST_PROTOCOL_TRANSITION, msg->value, from->line, from->column);
        add_child(t, create_ast_node(AST_IDENTIFIER, from->value, from->line, from->column));
        add_child(t, create_ast_node(AST_IDENTIFIER, to->value, to->line, to->column));
        add_child(proto, t);
        match_token(parser, TOKEN_COMMA);
    }
    return proto;
}

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
            parser_error(parser, "Unexpected end of file in message definition");
            return NULL;
        }
        
        Token* field_name = expect_token(parser, TOKEN_IDENTIFIER);
        if (!field_name) break;

        if (!expect_token(parser, TOKEN_COLON)) break;

        Type* field_type = parse_type(parser);
        if (!field_type) {
            parser_error(parser, "Expected type for message field");
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
                parser_error(parser, "Unexpected end in message pattern");
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
        int errors_before = aether_error_count();
        ASTNode* value_expr = parse_expression(parser);
        if (!value_expr) {
            missing_operand(parser, reply_token, errors_before, "a value or a message");
            return NULL;
        }
        ASTNode* reply_stmt = create_ast_node(AST_REPLY_STATEMENT, NULL,
                                              reply_token->line, reply_token->column);
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
                parser_error(parser, "Unexpected end in reply statement");
                return NULL;
            }

            Token* field_name = expect_token(parser, TOKEN_IDENTIFIER);
            if (!field_name) break;

            Token* colon = expect_token(parser, TOKEN_COLON);
            if (!colon) break;

            int errors_before = aether_error_count();
            ASTNode* field_expr = parse_expression(parser);
            if (!field_expr) {
                missing_operand(parser, colon, errors_before, "a value");
                break;
            }

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
                parser_error(parser, "Unexpected end in message constructor");
                return NULL;
            }
            
            Token* field_name = expect_token(parser, TOKEN_IDENTIFIER);
            if (!field_name) break;

            Token* colon = expect_token(parser, TOKEN_COLON);
            if (!colon) break;

            int errors_before = aether_error_count();
            ASTNode* field_expr = parse_expression(parser);
            if (!field_expr) {
                missing_operand(parser, colon, errors_before, "a value");
                break;
            }

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

/* Consumes the `}` that closes `block`, recording where it is: control that
 * falls off the end of a function body leaves there (#2684). */
static int block_closes(Parser* parser, ASTNode* block) {
    Token* close = peek_token(parser);
    if (!match_token(parser, TOKEN_RIGHT_BRACE)) return 0;
    block->end_line = close->line;
    block->end_column = close->column;
    return 1;
}

/* #3167: is the statement at the current token a destructure with a place
 * among its slots (`names[0], _ = pair()`, `p.x, n = pair()`)? Its slots,
 * at bracket depth 0 before a `=` on the statement's line, are each a name
 * or `_` (one token), or a name followed by `[` or `.`, and one of them is
 * the latter. A destructure of names only is the ordinary form
 * (parse_python_style_declaration); anything else is not a destructure. */
static int at_place_destructure(Parser* parser) {
    Token* first = peek_token(parser);
    if (!first) return 0;
    int line = first->line, depth = 0, commas = 0, places = 0, slot_len = 0;
    int slot_ok = 1;
    for (int i = parser->current_token; i < parser->token_count; i++) {
        Token* t = parser->tokens[i];
        if (depth == 0 && t->line != line) return 0;
        if (depth == 0 && (t->type == TOKEN_COMMA || t->type == TOKEN_ASSIGN)) {
            if (slot_len == 0 || !slot_ok) return 0;
            if (slot_len > 1) places++;
            if (t->type == TOKEN_ASSIGN) return commas > 0 && places > 0;
            commas++;
            slot_len = 0;
            slot_ok = 1;
            continue;
        }
        if (slot_len == 0 && !token_is_value_ident(t)) return 0;
        if (slot_len == 1 && t->type != TOKEN_LEFT_BRACKET && t->type != TOKEN_DOT) slot_ok = 0;
        if (t->type == TOKEN_LEFT_PAREN || t->type == TOKEN_LEFT_BRACKET) depth++;
        else if (t->type == TOKEN_RIGHT_PAREN || t->type == TOKEN_RIGHT_BRACKET) depth--;
        else if (depth == 0 && (t->type == TOKEN_LEFT_BRACE || t->type == TOKEN_RIGHT_BRACE ||
                                t->type == TOKEN_SEMICOLON))
            return 0;
        if (depth < 0) return 0;
        slot_len++;
    }
    return 0;
}

/* #3167: a destructure into places. Each element or field slot binds a
 * hidden local of the destructure, and a store `place = local` follows it
 * in the block, so the place takes the value as any store does, ownership
 * included, and the names among the slots bind in the block's scope as an
 * ordinary destructure's do. Appends the statements to `block`. */
static int g_place_destructure_seq = 0;

/* The index operands of a place slot (`a[i]`, `rows[i][j]`, `o.items[i]`),
 * outermost first, each bound to a hidden local appended to `block` and
 * read from it by the place. They are evaluated before the destructure
 * binds anything, as Go does: `xs[i], i = pair()` stores into the element `i`
 * named before the statement, not the one the statement gives `i`, and an
 * index with a side effect runs once, before the value. A literal index
 * reads the same either way and stays. */
static void hoist_place_indices(ASTNode* place, ASTNode* block, int seq, int* k) {
    if (!place) return;
    if (place->type == AST_ARRAY_ACCESS && place->child_count >= 2) {
        hoist_place_indices(place->children[0], block, seq, k);
        ASTNode* idx = place->children[1];
        if (!idx || idx->type == AST_LITERAL) return;
        char name[48];
        snprintf(name, sizeof(name), "_ae_dix%d_%d", seq, (*k)++);
        ASTNode* decl = create_ast_node(AST_VARIABLE_DECLARATION, name, idx->line, idx->column);
        decl->node_type = create_type(TYPE_UNKNOWN);
        decl->type_inferred = 1;
        add_child(decl, idx);
        add_child(block, decl);
        place->children[1] = create_ast_node(AST_IDENTIFIER, name, idx->line, idx->column);
    } else if (place->type == AST_MEMBER_ACCESS && place->child_count >= 1) {
        hoist_place_indices(place->children[0], block, seq, k);
    }
}

static void parse_place_destructure(Parser* parser, ASTNode* block) {
    Token* head = peek_token(parser);
    ASTNode* destructure = create_ast_node(AST_TUPLE_DESTRUCTURE, NULL, head->line, head->column);
    /* Every place the destructure names (#3574): past 64 the next was
     * refused as no destructure slot at all. */
    ASTNode** places = NULL;
    char** hidden = NULL;
    int np = 0, place_cap = 0;
    int seq = ++g_place_destructure_seq;
    for (;;) {
        Token* t = peek_token(parser);
        Token* after = peek_ahead(parser, 1);
        if (t && token_is_value_ident(t) && after &&
            (after->type == TOKEN_COMMA || after->type == TOKEN_ASSIGN)) {
            if (strcmp(t->value, "_") != 0 && reject_none_binding(parser, t, "variable name")) break;
            advance_token(parser);
            ASTNode* var = create_ast_node(AST_VARIABLE_DECLARATION, t->value, t->line, t->column);
            var->node_type = create_type(TYPE_UNKNOWN);
            add_child(destructure, var);
        } else {
            ASTNode* place = parse_unary_expression(parser);
            if (!place || (place->type != AST_ARRAY_ACCESS && place->type != AST_MEMBER_ACCESS)) {
                if (!parser->suppress_errors && t) {
                    aether_error_with_code(
                        "a destructure slot is a name, `_`, an element (`a[i]`) or a field (`o.f`)",
                        t->line, t->column, AETHER_ERR_SYNTAX);
                }
                if (place) free_ast_node(place);
                break;
            }
            char name[48];
            snprintf(name, sizeof(name), "_ae_dslot%d_%d", seq, np);
            ASTNode* var = create_ast_node(AST_VARIABLE_DECLARATION, name, place->line, place->column);
            var->node_type = create_type(TYPE_UNKNOWN);
            add_child(destructure, var);
            if (np == place_cap) {
                place_cap = place_cap ? place_cap * 2 : 8;
                places = (ASTNode**)aether_xrealloc(places, sizeof(ASTNode*) * (size_t)place_cap);
                hidden = (char**)aether_xrealloc(hidden, sizeof(char*) * (size_t)place_cap);
            }
            places[np] = place;
            hidden[np] = strdup(name);
            np++;
        }
        if (!match_token(parser, TOKEN_COMMA)) break;
    }
    Token* eq = peek_token(parser);
    ASTNode* rhs = NULL;
    if (eq && eq->type == TOKEN_ASSIGN) {
        advance_token(parser);
        int errors_before = aether_error_count();
        rhs = parse_expression(parser);
        if (!rhs) missing_operand(parser, eq, errors_before, "a value");
    } else if (!parser->suppress_errors && eq) {
        aether_error_with_code("expected `=` after a destructure's slots", eq->line, eq->column,
                               AETHER_ERR_SYNTAX);
    }
    if (!rhs) {
        /* Recover past the rest of the statement's line. */
        while (peek_token(parser) && peek_token(parser)->line == head->line &&
               peek_token(parser)->type != TOKEN_RIGHT_BRACE &&
               peek_token(parser)->type != TOKEN_EOF) advance_token(parser);
        free_ast_node(destructure);
        for (int i = 0; i < np; i++) { free_ast_node(places[i]); free(hidden[i]); }
        free(places);
        free(hidden);
        return;
    }
    add_child(destructure, rhs);  // Last child is the RHS
    match_token(parser, TOKEN_SEMICOLON);
    int hoisted = 0;
    for (int i = 0; i < np; i++) hoist_place_indices(places[i], block, seq, &hoisted);
    add_child(block, destructure);
    for (int i = 0; i < np; i++) {
        ASTNode* assign = create_ast_node(AST_BINARY_EXPRESSION, "=", places[i]->line, places[i]->column);
        add_child(assign, places[i]);
        add_child(assign, create_ast_node(AST_IDENTIFIER, hidden[i], places[i]->line, places[i]->column));
        ASTNode* stmt = create_ast_node(AST_EXPRESSION_STATEMENT, NULL, places[i]->line, places[i]->column);
        add_child(stmt, assign);
        add_child(block, stmt);
        free(hidden[i]);
    }
    free(places);
    free(hidden);
}

ASTNode* parse_block(Parser* parser) {
    expect_token(parser, TOKEN_LEFT_BRACE);

    ASTNode* block = create_ast_node(AST_BLOCK, NULL, 0, 0);

    while (!block_closes(parser, block)) {
        /* CRITICAL: the depth guard returns without consuming a token, so the
         * force-advance below would otherwise emit one error per remaining
         * token. The file is already known unparseable at this point. */
        if (parser->depth_exceeded) break;
        if (at_when_region(parser)) {
            parse_when_region_stmts(parser, block);
            continue;
        }
        /* `;` is an optional statement terminator: every simple statement
         * consumes one after itself, but a block-ending statement (`if x {
         * ... }`) did not, so `if x { a } ; b` failed at the `;` with
         * "expected statement" (#2073). A `;` at statement position is the
         * same terminator, belonging to whatever came before. */
        if (match_token(parser, TOKEN_SEMICOLON)) continue;
        int start_token = parser->current_token;
        int start_errors = aether_error_count();
        /* `when = 0.0`: a keyword that heads its own statement form, used as
         * an assignment target. Left to parse_statement, the keyword's own
         * parser consumed it and stopped at the `=` with the generic
         * "expected statement" — pointing at the operator and naming
         * nothing (#2073). A keyword followed by an assignment operator is
         * the reserved-name mistake; say so at the keyword. */
        {
            Token* head = peek_token(parser);
            Token* after = peek_ahead(parser, 1);
            if (head && after && token_is_reserved_keyword(head) &&
                !token_is_value_ident(head) &&
                (after->type == TOKEN_ASSIGN || after->type == TOKEN_PLUS_ASSIGN ||
                 after->type == TOKEN_MINUS_ASSIGN)) {
                const char* msg = "";
                msg = aether_internf(
                    "'%s' is a reserved keyword and cannot be used as an identifier; rename it (e.g. '%s_' or 'msg')",
                    head->value, head->value);
                const char* hint = "";
                hint = aether_internf("rename to '%s_' or another identifier", head->value);
                if (!parser->suppress_errors) {
                    aether_error_full(msg, head->line, head->column, hint, NULL, AETHER_ERR_SYNTAX);
                }
                /* Recover past the rest of the statement, but not past a
                 * `}` on the same line: that brace closes the block. */
                while (!is_at_end(parser) && peek_token(parser) &&
                       peek_token(parser)->line == head->line &&
                       peek_token(parser)->type != TOKEN_RIGHT_BRACE) {
                    advance_token(parser);
                }
                continue;
            }
        }
        if (at_place_destructure(parser)) {
            parse_place_destructure(parser, block);
            continue;
        }
        ASTNode* stmt = parse_statement(parser);
        if (stmt) {
            add_child(block, stmt);
        } else if (aether_error_count() > start_errors &&
                   parser->current_token > start_token) {
            /* The statement parser already said what was wrong and consumed
             * its input; a generic "expected statement" on the remains points
             * at the next good line and reads as a second, phantom defect.
             * The rest of the line it stopped on is its remains too (`[1, ,
             * 2]` stops at the second `,`, and `, 2]` read as statements
             * gave two more errors), up to a `}` that closes this block. */
            Token* last = parser->tokens[parser->current_token - 1];
            int line = last ? last->line : -1;
            int braces = 0;
            while (!is_at_end(parser)) {
                Token* rest = peek_token(parser);
                if (!rest || rest->line != line) break;
                if (rest->type == TOKEN_LEFT_BRACE) braces++;
                if (rest->type == TOKEN_RIGHT_BRACE && braces-- == 0) break;
                advance_token(parser);
            }
        } else {
            // Prevent infinite loops on unexpected tokens inside blocks.
            // If the block-head token is a reserved keyword being used as
            // if it were an identifier (e.g. `message = "hello"`), point
            // at it directly instead of the generic "expected statement"
            // that leaves users guessing.
            Token* stmt_head = peek_token(parser);
            if (stmt_head && token_is_reserved_keyword(stmt_head)) {
                const char* msg = "";
                msg = aether_internf(
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

    /* #385: `actor Name implements Protocol {`, carried on the actor as
     * its annotation `implements:Protocol`. `implements` is contextual. */
    char* implements = NULL;
    Token* impl = peek_token(parser);
    if (impl && impl->type == TOKEN_IDENTIFIER && impl->value &&
        strcmp(impl->value, "implements") == 0) {
        advance_token(parser);
        Token* proto = expect_token(parser, TOKEN_IDENTIFIER);
        if (!proto) return NULL;
        size_t n = strlen(proto->value) + sizeof("implements:");
        implements = malloc(n);
        if (implements) snprintf(implements, n, "implements:%s", proto->value);
    }

    expect_token(parser, TOKEN_LEFT_BRACE);

    ASTNode* actor = create_ast_node(AST_ACTOR_DEFINITION, name->value, name->line, name->column);
    actor->annotation = implements;
    
    /* No cap on the body: each pass consumes a declaration or a token. It
     * stopped after 1000 with nothing said, and an actor left open at the
     * end of the file was not reported either (#2907). */
    while (!match_token(parser, TOKEN_RIGHT_BRACE)) {
        if (is_at_end(parser)) {
            parser_error(parser, "Unexpected end of file in actor definition: its `}` is missing");
            break;
        }
        int body_start = parser->current_token;
        int body_errors = aether_error_count();

        if (match_token(parser, TOKEN_STATE)) {
            // Check if there's an explicit type or Python-style
            Token* next_tok = peek_token(parser);
            ASTNode* state_decl = NULL;
            /* #2465: a type spelled by an identifier (`uint8`, `uint16`,
             * `uint32`, `int64`, `f32`, `longdouble`, a C ABI alias, a struct
             * name) is recognised by the same shape as a typed local
             * declaration (parse_statement): `IDENT IDENT`, or the array form
             * `IDENT [N] IDENT`, on one line. Without it `state uint8 b8 = 1`
             * became a field named `uint8` and `b8 = 1` a stray statement. */
            int ident_typed = 0;
            if (next_tok && next_tok->type == TOKEN_IDENTIFIER) {
                Token* t1 = peek_ahead(parser, 1);
                if (t1 && t1->type == TOKEN_IDENTIFIER && t1->line == next_tok->line) {
                    ident_typed = 1;
                } else if (t1 && t1->type == TOKEN_LEFT_BRACKET) {
                    int rbo = closing_bracket_offset(parser, 1);
                    Token* rb = rbo > 0 ? peek_ahead(parser, rbo) : NULL;
                    Token* nm = rbo > 0 ? peek_ahead(parser, rbo + 1) : NULL;
                    ident_typed = rb && nm && nm->type == TOKEN_IDENTIFIER &&
                                  nm->line == rb->line;
                }
            }

            if (ident_typed ||
                (next_tok && (next_tok->type == TOKEN_INT || next_tok->type == TOKEN_INT64 ||
                            next_tok->type == TOKEN_UINT64 ||
                            next_tok->type == TOKEN_DURATION ||
                            next_tok->type == TOKEN_FLOAT ||
                            next_tok->type == TOKEN_STRING || next_tok->type == TOKEN_BOOL ||
                            next_tok->type == TOKEN_BYTE || next_tok->type == TOKEN_PTR ||
                            /* #3163: `state actor_ref next` */
                            next_tok->type == TOKEN_ACTOR_REF ||
                            /* #2921: a tuple type, `state (string, int) v` */
                            next_tok->type == TOKEN_LEFT_PAREN))) {
                // Explicit type: state int count = 0  or  state long total = 0
                state_decl = parse_variable_declaration_with_semicolon(parser, false);
            } else if (next_tok && next_tok->type == TOKEN_IDENTIFIER) {
                // Python-style: state count = 0 (no semicolon required in actor)
                Token* name = expect_token(parser, TOKEN_IDENTIFIER);
                if (name) {
                    state_decl = create_ast_node(AST_VARIABLE_DECLARATION, name->value, name->line, name->column);
                    state_decl->node_type = create_type(TYPE_UNKNOWN);

                    /* #3163: `state next: actor_ref = null`, the type
                     * written as a struct field, a parameter, `let` and a
                     * module `var` write it, so a state field takes every
                     * type a field takes. The declaration is the one the
                     * type-first form makes, `state int n = 0`: the written
                     * type, then the value checked against it as a typed
                     * binding's is. It was three "Unexpected token in actor
                     * body" errors, and an actor_ref could be held only as
                     * a bare `state next = null`. */
                    if (match_token(parser, TOKEN_COLON)) {
                        Type* written = parse_type(parser);
                        if (written) {
                            free_type(state_decl->node_type);
                            state_decl->node_type = written;
                        } else if (!parser->suppress_errors) {
                            parser_error(parser, "expected a type after `:` in a state declaration");
                        }
                    }

                    Token* eq = peek_token(parser);
                    if (match_token(parser, TOKEN_ASSIGN)) {
                        int errors_before = aether_error_count();
                        ASTNode* value = parse_expression(parser);
                        if (value) {
                            add_child(state_decl, value);
                        } else {
                            /* #2998: `state n =` with nothing after the `=`. */
                            missing_operand(parser, eq, errors_before, "a value");
                            free_ast_node(state_decl);
                            state_decl = NULL;
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
                Token* tok = peek_token(parser);
                if (tok && aether_error_count() == body_errors) {
                    const char* msg = "";
                    msg = aether_internf("Unexpected token in actor body: '%.60s'",
                             tok->value ? tok->value : "?");
                    parser_error(parser, msg);
                }
            }
        }
        /* Forward progress, whatever the pass did. */
        if (parser->current_token == body_start) advance_token(parser);
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
            parser_error(parser, "Unexpected end in receive statement");
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

/* The `@<attr>` markers between an extern parameter's `:` and its type,
 * then the type. Shared by the bare `extern name(...)` form and the
 * `@extern("c_sym") name(...)` form, so both accept the same set:
 *
 *   @aether   the param receives an AetherString header rather than the
 *             unwrapped const char*. Codegen suppresses the call-site
 *             aether_string_data() unwrap so binary content with embedded
 *             NULs survives the boundary intact. See #351.
 *
 *   @retain   the function stores / retains the pointer beyond the call
 *             (think `string_list_add`, `map_put_raw`'s key, any
 *             add/put/insert that captures the bytes). Tells the escape
 *             walker to mark a heap-string arg as escaped at this slot, so
 *             the heap-string-tracker wrapper and function-exit defer-free
 *             both skip freeing. Without it, default `string`-param
 *             treatment is "read-only", correct for string.length /
 *             equals / println but a UAF for retainers. See #420 follow-up.
 *
 *   @noescape the function uses the argument only during the call: it
 *             neither stores it, hands it to another thread, nor frees
 *             it. For a closure passed to a `ptr` or `fn` parameter the
 *             caller then keeps the environment and releases it after the
 *             call, as it does for an Aether callee that keeps nothing,
 *             and a `ptr` slot gets a box on the caller's stack instead of
 *             a heap one (#2523). Only meaningful on `ptr` and `fn`.
 *
 *   @into     the C function's result points into or holds the `ptr`
 *             argument, which may go nowhere else (#2990). Trusted, as
 *             `@retain` is; a call is checked as one to an Aether
 *             function's `@into` parameter.
 *
 *   @keep     an Aether function's `@keep ptr` behind a C symbol: the stub
 *             `ae` writes for a binary import (#3013). A call is checked as
 *             one to that function (a borrowed `ptr` is E0902, a string goes
 *             only through `string.keep(s)`), which `@retain` is not: R4
 *             hands `@retain` a string's characters with a reference of its
 *             own, right for C and not what the Aether function expects.
 *
 *   @holds    the `ptr` argument is where the function keeps what its
 *             `@retain` parameters are given (#3086): std's list and map
 *             stores declare their container so. A keep through such a
 *             call outlives the caller only when that argument's memory
 *             does: a container the caller makes with an `@fresh` extern
 *             and lets go nowhere keeps nothing past it. Only meaningful
 *             on `ptr`.
 *
 * Multiple annotations stack: `name: @aether @retain string` is legal.
 * Order is irrelevant; storage is a comma-separated set on
 * `param->annotation`, tested by substring. */
static void parse_extern_param_attrs_and_type(Parser* parser, ASTNode* param) {
    while (peek_token(parser) && peek_token(parser)->type == TOKEN_AT) {
        advance_token(parser);  // consume '@'
        Token* attr = peek_token(parser);
        const char* tag = NULL;
        if (attr && attr->type == TOKEN_IDENTIFIER && attr->value) {
            if (strcmp(attr->value, "aether") == 0) {
                tag = "aether_param";
            } else if (strcmp(attr->value, "retain") == 0) {
                tag = "retain_param";
            } else if (strcmp(attr->value, "noescape") == 0) {
                tag = "noescape_param";
            } else if (strcmp(attr->value, "into") == 0) {
                tag = "into_param";
            } else if (strcmp(attr->value, "keep") == 0) {
                tag = "keep_param";
            } else if (strcmp(attr->value, "holds") == 0) {
                tag = "holder_param";
            } else if (strcmp(attr->value, "frees") == 0) {
                tag = "frees_param";   /* #1300 */
            } else if (strcmp(attr->value, "moves") == 0) {
                tag = "moves_param";   /* #1300 */
            }
            if (tag) advance_token(parser);
        }
        if (!tag) {
            parser_error(parser, "unknown extern-param attribute (expected @aether, @retain, "
                                 "@noescape, @into, @keep, @holds, @frees or @moves)");
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
    /* #2523: a string parameter is borrowed unless `@retain` says
     * otherwise, and an integer has no lifetime, so `@noescape` on either
     * would say nothing: refuse it rather than let it look meaningful. */
    if (param->annotation && strstr(param->annotation, "noescape_param") &&
        param->node_type->kind != TYPE_PTR && param->node_type->kind != TYPE_FUNCTION) {
        parser_error(parser, "@noescape on an extern parameter is only valid on `ptr` or `fn`");
    }
    if (param->annotation && strstr(param->annotation, "holder_param") &&
        param->node_type->kind != TYPE_PTR) {
        parser_error(parser, "@holds on an extern parameter is only valid on `ptr`");
    }
}

/* Trailing `@`-attributes on an extern signature, shared by the bare
 * `extern name(...)` form and the `@extern("c_sym") name(...)` form:
 *
 *   @heap     the `-> string` return is a malloc'd buffer the caller owns,
 *             and a `-> fn` result a reference the caller owns (#3547).
 *             Recorded as "heap_return", read by is_heap_string_expr and
 *             closure_value_is_fresh in codegen_stmt.c. Only meaningful on
 *             `-> string`, `-> ptr` and `-> fn`; anything else is a parse
 *             error since integers aren't heap-tracked and void has
 *             nothing to own.
 *   @fresh    the `-> ptr` result is memory the call allocates, which nothing
 *             else holds (#3086): std's list and map constructors. A
 *             container bound from one is a holder (`@holds`) that keeps
 *             nothing past its function unless it escapes.
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
            /* A `ptr` result is a counted string returned untyped
             * (string.from_int and the like) or a `*StringSeq`: `@heap`
             * says the caller owns a reference, as for a `string` result
             * (R1). So does a closure (`-> fn`), an Aether library's
             * export handing its caller the reference its return gives
             * (#3547): a C function's closure is lent (R4). A typed C
             * function pointer holds no reference. */
            int closure_result = ext->node_type && ext->node_type->kind == TYPE_FUNCTION &&
                                 !ext->node_type->is_fnptr;
            if (!ext->node_type || (ext->node_type->kind != TYPE_STRING &&
                                    ext->node_type->kind != TYPE_PTR && !closure_result)) {
                parser_error(parser,
                    "@heap on extern return is only valid on `-> string`, `-> ptr` or `-> fn`");
            } else {
                ext->annotation =
                    annotation_add_marker(ext->annotation, "heap_return");
            }
            advance_token(parser);  /* consume 'heap' */
        } else if (strcmp(tag->value, "fresh") == 0) {
            advance_token(parser);  /* consume '@' */
            if (!ext->node_type || ext->node_type->kind != TYPE_PTR) {
                parser_error(parser, "@fresh on extern return is only valid on `-> ptr`");
            } else {
                ext->annotation = annotation_add_marker(ext->annotation, "fresh_return");
            }
            advance_token(parser);  /* consume 'fresh' */
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

    if (peek_token(parser) && (peek_token(parser)->type == TOKEN_STRUCT ||
                               peek_token(parser)->type == TOKEN_UNION)) {
        /* #2561: `extern union Name @c_import { ... }` declares a union the
         * header defines, spelled `union Name` in the C. Only imported: its
         * fields share storage, a layout Aether does not emit. */
        int is_union = peek_token(parser)->type == TOKEN_UNION;
        advance_token(parser);  // consume `struct` / `union`
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
        if (is_union && !has_c_import) {
            parser_error(parser,
                "an extern union needs @c_import: its fields share storage, "
                "a layout only the C header that defines it can give "
                "(`extern union Name @c_import { ... }`)");
            free_ast_node(sd);
            return NULL;
        }
        if (has_c_import) {
            free(sd->annotation);
            sd->annotation = strdup(is_union ? "extern_c_import_union" : "extern_c_import");
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
        if (reject_none_binding(parser, cname, "constant name")) return NULL;
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
                /* `@<attr>` markers between `:` and the type, then the
                 * type: see parse_extern_param_attrs_and_type. */
                parse_extern_param_attrs_and_type(parser, param);
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

/* True when the tokens at the cursor are a return type followed by the body,
 * `-> (T1, T2) { ... }` or `-> fn(ptr) -> string { ... }`, rather than an
 * arrow-body expression, `-> (a + b)` or `-> fn(x)`.
 *
 * This used to be a shape check of its own: a type keyword or a name, then a
 * comma. Any first element spelled with more than one token (`*Foo`,
 * `mod.Name`, `fn(int) -> int`, `int?`) failed it, so `-> (*Foo, *Foo) {` fell
 * to the `-> expr` path and broke at top level (#2626), although parse_type
 * reads that tuple, as an extern return does. The type parser is the one
 * authority on what a type is, so it decides: parse the group as a type with
 * errors suppressed, rewind, and call it a typed return when it parsed and
 * the body or a contract clause follows. An expression in parentheses does
 * not parse as a type (a one-element `(a)` is refused too), and a tuple
 * literal is no arrow body the checker accepts. A written-out function
 * pointer type is decided the same way (#2636): its own `-> R` is part of
 * the type, and a call `fn(x)` as an arrow body has no `{` after it. */
static int starts_return_type_then_body(Parser* parser) {
    int saved_pos = parser->current_token;
    int saved_suppress = parser->suppress_errors;
    parser->suppress_errors = 1;
    Type* t = parse_type(parser);
    Token* after = peek_token(parser);
    int typed = t && after &&
                (after->type == TOKEN_LEFT_BRACE ||
                 after->type == TOKEN_REQUIRES ||
                 after->type == TOKEN_ENSURES);
    if (t) free_type(t);
    parser->current_token = saved_pos;
    parser->suppress_errors = saved_suppress;
    return typed;
}

/* True when the tokens at the cursor are a result type followed by an arrow
 * body, `-> Point -> p` or `-> (int, int) -> (a, b)` (#3158). An arrow-body
 * expression is never followed by a second `->`, so a type that parses with
 * one after it is the written result type. */
static int starts_return_type_then_arrow(Parser* parser) {
    int saved_pos = parser->current_token;
    int saved_suppress = parser->suppress_errors;
    parser->suppress_errors = 1;
    Type* t = parse_type(parser);
    Token* after = peek_token(parser);
    int typed = t && after && after->type == TOKEN_ARROW;
    if (t) free_type(t);
    parser->current_token = saved_pos;
    parser->suppress_errors = saved_suppress;
    return typed;
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
                    char emsg[256];
                    const char* hint = "";
                    snprintf(emsg, sizeof(emsg),
                        "'%s' is a reserved keyword and cannot be used as a parameter "
                        "name; rename it (e.g. '%s_'), or escape it as `%s` to use the "
                        "name verbatim", pk->value, pk->value, pk->value);
                    hint = aether_internf(
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
        if (peek && starts_return_type_then_arrow(parser)) {
            is_typed_return = 1;
        } else if (peek) {
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
                    // #2636: a C function pointer return type written out,
                    // `-> fn(ptr) -> string { ... }`. The bare-name branch
                    // below sees `(` after `fn`, not `{`, and sent it to the
                    // `-> expr` path, which read `fn(ptr)` as a call.
                    if (peek->value && strcmp(peek->value, "fn") == 0 &&
                        peek_ahead(parser, 1) &&
                        peek_ahead(parser, 1)->type == TOKEN_LEFT_PAREN) {
                        is_typed_return = starts_return_type_then_body(parser);
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
                        int off = 1, depth = 0;
                        while (peek_ahead(parser, off)) {
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
                         * body), a contract clause or a type suffix; a
                         * struct-literal head `{ field:` is still excluded.
                         * Anything else, an operator, `=`, or the next
                         * definition, follows an expression body: `-> p.n`,
                         * `-> p.n = v`. Read as a type, both were a parse
                         * error ("Expected LEFT_BRACE"). */
                        Token* after_qual = peek_ahead(parser, off);
                        if (after_qual && after_qual->type == TOKEN_LEFT_BRACE) {
                            Token* after_brace = peek_ahead(parser, off + 1);
                            Token* after_field = peek_ahead(parser, off + 2);
                            int looks_like_struct_literal =
                                after_brace &&
                                after_brace->type == TOKEN_IDENTIFIER &&
                                after_field && after_field->type == TOKEN_COLON;
                            if (!looks_like_struct_literal) is_typed_return = 1;
                        } else if (after_qual) {
                            AeTokenType at = after_qual->type;
                            is_typed_return = at == TOKEN_REQUIRES || at == TOKEN_ENSURES ||
                                              at == TOKEN_QUESTION || at == TOKEN_QUESTION_QUESTION ||
                                              at == TOKEN_EXCLAIM || at == TOKEN_LEFT_BRACKET;
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
                    /* #2809: an optional result type, `-> Point? { ... }`:
                     * the `?` sits between the name and the body. A `-> expr`
                     * body never puts `{` (or a contract clause) right after
                     * a postfix `?`. */
                    if (after_name && after_name->type == TOKEN_QUESTION) {
                        Token* after_q = peek_ahead(parser, 2);
                        if (after_q && (after_q->type == TOKEN_LEFT_BRACE ||
                                        after_q->type == TOKEN_REQUIRES ||
                                        after_q->type == TOKEN_ENSURES))
                            is_typed_return = 1;
                        break;
                    }
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
                case TOKEN_LEFT_PAREN:
                    // `-> (T1, T2, ...) { ... }`, a parenthesised tuple
                    // return type, as `extern f(...) -> (T1, T2)` takes.
                    is_typed_return = starts_return_type_then_body(parser);
                    break;
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
            if (match_token(parser, TOKEN_ARROW)) {
                /* #3158: an arrow body after a written result type,
                 * `square(x: int) -> int -> x * x` or `-> int -> { ... }`.
                 * The body is the one an untyped arrow body is, and its
                 * value is returned against the written type, as a block
                 * body's `return` is. Read as a block body, it was
                 * "Expected LEFT_BRACE, got ARROW", and the block parser
                 * then took the rest of the file as the missing block's
                 * statements: nine errors, the last ones in later
                 * functions. */
                if (peek_token(parser) && peek_token(parser)->type == TOKEN_LEFT_BRACE) {
                    ASTNode* body = parse_block(parser);
                    arrow_block_return_tail(body);
                    add_child(func, body);
                } else {
                    ASTNode* body_block = parse_arrow_expression_body(parser);
                    if (body_block) {
                        add_child(func, body_block);
                    }
                }
            } else {
                // Now expect the traditional block body `{ ... }`.
                ASTNode* body = parse_block(parser);
                if (body) {
                    add_child(func, body);
                }
            }
        } else if (peek && peek->type == TOKEN_LEFT_BRACE) {
            // Multi-statement arrow body: -> { stmt1; stmt2; expr }
            // Parse as a block, but treat the last expression as implicit return
            ASTNode* body = parse_block(parser);
            arrow_block_return_tail(body);
            add_child(func, body);
        } else {
            // Single expression arrow body: -> expr
            ASTNode* body_block = parse_arrow_expression_body(parser);
            if (body_block) {
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

    /* #2684: with no result type written, the result is what inference
     * finds in the body's `return`s, not something the signature claims. */
    func->type_inferred = !func->node_type || func->node_type->kind == TYPE_UNKNOWN;

    return func;
}

/* A parameter's optional default value, `= expr` (issue #265 / Phase A2.1,
 * default function arguments). The expression is stored as the pattern's
 * first child, with annotation="has_default" so consumers (typechecker,
 * codegen) can distinguish it from the pattern-children struct and list
 * patterns use. An `=` with nothing after it is an error (#2998); it was a
 * parameter with no default. */
static void parse_param_default(Parser* parser, ASTNode* pattern) {
    Token* eq = peek_token(parser);
    if (!match_token(parser, TOKEN_ASSIGN)) return;
    int errors_before = aether_error_count();
    ASTNode* default_expr = parse_expression(parser);
    if (!default_expr) {
        missing_operand(parser, eq, errors_before, "a default value");
        return;
    }
    add_child(pattern, default_expr);
    pattern->annotation = annotation_add_marker(pattern->annotation, "has_default");
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
            /* A pattern holding a NUL matches by all its bytes (#2520). */
            if (token->value_len > 0) ast_set_literal_bytes(pattern, token->value, token->value_len);
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
            if (reject_none_binding(parser, token, "parameter name")) return NULL;
            advance_token(parser);
            ASTNode* pattern = create_ast_node(AST_PATTERN_VARIABLE, token->value,
                                              token->line, token->column);

            // Optional type annotation: param: type, `param: @keep ptr` for a
            // pointer the function keeps past the call (#2926), `param:
            // @into ptr` for one that may reach only its result (#2990).
            if (match_token(parser, TOKEN_COLON)) {
                parse_param_modes(parser, &pattern->annotation);
                Type* param_type = parse_type(parser);
                if (param_type) {
                    pattern->node_type = param_type;
                } else {
                    pattern->node_type = create_type(TYPE_UNKNOWN);
                }
            } else {
                pattern->node_type = create_type(TYPE_UNKNOWN);
            }

            // Optional default value: param: type = expr. A default
            // expression is evaluated in the enclosing scope, with
            // the other parameters NOT in scope, so a default that names
            // another parameter resolves as an undefined variable (a clear
            // E0300 at type-check) rather than that parameter's value.
            parse_param_default(parser, pattern);

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
                parse_param_default(parser, pattern);
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
                parse_param_default(parser, pattern);
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

/* A token spelled like an identifier: an attribute name may be a keyword
 * (`@default(2.0)`), since it is only ever read as text. */
static int token_spells_identifier(const Token* t) {
    if (!t || !t->value || !t->value[0]) return 0;
    if (!(isalpha((unsigned char)t->value[0]) || t->value[0] == '_')) return 0;
    for (const char* c = t->value; *c; c++) {
        if (!(isalnum((unsigned char)*c) || *c == '_')) return 0;
    }
    return 1;
}

/* One attribute argument: a number (optionally negated), a string, or
 * true/false. Returns an AST_LITERAL typed TYPE_INT, TYPE_FLOAT, TYPE_STRING
 * or TYPE_BOOL, or NULL after reporting the error. */
static ASTNode* parse_field_attribute_arg(Parser* parser) {
    Token* t = peek_token(parser);
    int negative = 0;
    if (t && t->type == TOKEN_MINUS) {
        advance_token(parser);
        negative = 1;
        t = peek_token(parser);
        if (!t || t->type != TOKEN_NUMBER) {
            parser_error(parser, "expected a number after `-` in a field attribute");
            return NULL;
        }
    }
    if (!t || !(t->type == TOKEN_NUMBER || t->type == TOKEN_STRING_LITERAL ||
                t->type == TOKEN_TRUE || t->type == TOKEN_FALSE)) {
        parser_error(parser,
            "a field attribute's arguments are literals: a number, a string, true or false");
        return NULL;
    }
    advance_token(parser);
    Type* ty = NULL;
    char* text = NULL;
    if (t->type == TOKEN_NUMBER) {
        const char* v = t->value ? t->value : "0";
        int radix = v[0] == '0' && (v[1] == 'x' || v[1] == 'X' || v[1] == 'o' ||
                                    v[1] == 'O' || v[1] == 'b' || v[1] == 'B');
        int is_float = 0;
        for (const char* c = v; *c; c++) {
            if (*c == '.' || (!radix && (*c == 'e' || *c == 'E'))) is_float = 1;
            else if (!radix && isalpha((unsigned char)*c) && *c != 'e' && *c != 'E') {
                parser_error(parser, "a duration is not a field attribute argument; "
                                     "write the number of nanoseconds");
                return NULL;
            }
        }
        ty = create_type(is_float ? TYPE_FLOAT : TYPE_INT);
        size_t n = strlen(v);
        text = (char*)malloc(n + 2);
        if (!text) { free_type(ty); return NULL; }
        snprintf(text, n + 2, "%s%s", negative ? "-" : "", v);
    } else if (t->type == TOKEN_STRING_LITERAL) {
        ty = create_type(TYPE_STRING);
    } else {
        ty = create_type(TYPE_BOOL);
    }
    ASTNode* lit = create_ast_node(AST_LITERAL, text ? text : t->value, t->line, t->column);
    free(text);
    lit->node_type = ty;
    return lit;
}

/* Zero or more `@name` / `@name(args)` after a struct field's type (#2298),
 * attached to `field` as AST_FIELD_ATTRIBUTE children. Returns 0 after
 * reporting a syntax error. */
static int parse_field_attributes(Parser* parser, ASTNode* field) {
    while (peek_token(parser) && peek_token(parser)->type == TOKEN_AT) {
        Token* at = advance_token(parser);
        Token* name = peek_token(parser);
        if (!token_spells_identifier(name)) {
            parser_error(parser, "expected an attribute name after `@` on a struct field");
            return 0;
        }
        advance_token(parser);
        ASTNode* attr = create_ast_node(AST_FIELD_ATTRIBUTE, name->value, at->line, at->column);
        if (match_token(parser, TOKEN_LEFT_PAREN)) {
            if (!match_token(parser, TOKEN_RIGHT_PAREN)) {
                for (;;) {
                    ASTNode* arg = parse_field_attribute_arg(parser);
                    if (!arg) { free_ast_node(attr); return 0; }
                    add_child(attr, arg);
                    if (match_token(parser, TOKEN_COMMA)) continue;
                    if (!expect_token(parser, TOKEN_RIGHT_PAREN)) { free_ast_node(attr); return 0; }
                    break;
                }
            }
        }
        add_child(field, attr);
    }
    return 1;
}

ASTNode* parse_struct_definition(Parser* parser) {
    Token* struct_token = advance_token(parser); // consume 'struct'
    
    Token* name_token = expect_token(parser, TOKEN_IDENTIFIER);
    if (!name_token) return NULL;
    
    ASTNode* struct_def = create_ast_node(AST_STRUCT_DEFINITION, name_token->value, 
                                         struct_token->line, struct_token->column);
    
    /* Past this point the definition (and every field attached to it) belongs
     * to this function until it is returned, so each error path releases it. */

    /* Zero or more `@`-attributes after the name, the same position an
     * `extern struct` takes `@packed` (#747):
     *   @observable — every field store on a value of this struct is
     *                 followed by a runtime notification to the closures
     *                 registered on that object through std.observe (#2220).
     * Carried as a `;`-joined marker so a leading `@derive(...)` on the same
     * struct keeps it. */
    while (peek_token(parser) && peek_token(parser)->type == TOKEN_AT) {
        advance_token(parser);  // consume '@'
        Token* attr = peek_token(parser);
        if (attr && attr->type == TOKEN_IDENTIFIER && attr->value &&
            strcmp(attr->value, "observable") == 0) {
            advance_token(parser);
            struct_def->annotation = annotation_add_marker(struct_def->annotation,
                                                           "observable");
        } else {
            parser_error(parser,
                "unknown struct attribute (expected @observable)");
            free_ast_node(struct_def);
            return NULL;
        }
    }

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

        /* #2298: `@name` or `@name(literal, ...)` after the type, any number
         * of them: metadata a `@derive(schema)` table carries per field
         * (`rate: float @range(0.0, 10.0) @default(2.0)`). The compiler does
         * not interpret them; the derive pass rejects them on a struct with
         * no schema to carry them. */
        if (!parse_field_attributes(parser, field)) {
            free_ast_node(field);
            free_ast_node(struct_def);
            return NULL;
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
            const char* msg = "";
            msg = aether_internf(
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

        // #2200: `cfn Name(a: T1, b: T2) -> R` — a named C function-pointer
        // type. `cfn` is a contextual identifier (still usable as a name
        // elsewhere); only the `cfn <ident> (` shape here is intercepted.
        if (token->type == TOKEN_IDENTIFIER && token->value &&
            strcmp(token->value, "cfn") == 0) {
            Token* n1 = peek_ahead(parser, 1);
            Token* n2 = peek_ahead(parser, 2);
            if (n1 && n1->type == TOKEN_IDENTIFIER &&
                n2 && n2->type == TOKEN_LEFT_PAREN) {
                advance_token(parser);   // consume 'cfn'
                return parse_cfn_type_def(parser);
            }
        }

        // #385: `protocol Name { ... }`. `protocol` is a contextual word:
        // only the `protocol <ident> {` shape here is intercepted.
        if (token->type == TOKEN_IDENTIFIER && token->value &&
            strcmp(token->value, "protocol") == 0) {
            Token* n1 = peek_ahead(parser, 1);
            Token* n2 = peek_ahead(parser, 2);
            if (n1 && n1->type == TOKEN_IDENTIFIER && n2 && n2->type == TOKEN_LEFT_BRACE)
                return parse_protocol_definition(parser);
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
                /* #3142: `type Name = owned ptr drop f`, a pointer with one
                 * owner, released by `f` when its owner lets go. A distinct
                 * type over `ptr` that carries `owned_drop:<f>`. `owned` and
                 * `drop` are words only here. */
                if (dk && dk->type == TOKEN_IDENTIFIER && dk->value &&
                    strcmp(dk->value, "owned") == 0) {
                    advance_token(parser);                   // consume 'owned'
                    Type* base = parse_type(parser);
                    if (!base) {
                        parser_error(parser, "expected `ptr` after `owned`: `type Name = owned ptr drop f`");
                        return NULL;
                    }
                    Token* dw = peek_token(parser);
                    if (!dw || dw->type != TOKEN_IDENTIFIER || !dw->value ||
                        strcmp(dw->value, "drop") != 0) {
                        parser_error(parser, "an owned type names the function that releases it: "
                                             "`type Name = owned ptr drop f`");
                        free_type(base);
                        return NULL;
                    }
                    advance_token(parser);                   // consume 'drop'
                    Token* fn = expect_token(parser, TOKEN_IDENTIFIER);
                    if (!fn) { free_type(base); return NULL; }
                    /* The drop function's name, as long as it is (#3569):
                     * 256 bytes cut a qualified one to another name. */
                    const char* qname = aether_intern(fn->value);
                    while (peek_token(parser) && peek_token(parser)->type == TOKEN_DOT &&
                           peek_ahead(parser, 1) && peek_ahead(parser, 1)->type == TOKEN_IDENTIFIER) {
                        advance_token(parser);
                        Token* part = expect_token(parser, TOKEN_IDENTIFIER);
                        qname = aether_internf("%s.%s", qname, part->value);
                    }
                    ASTNode* d = create_ast_node(AST_DISTINCT_TYPE_DEF,
                                                 name->value, name->line, name->column);
                    d->node_type = base;
                    d->annotation = parser_strf("owned_drop:%s", qname);
                    match_token(parser, TOKEN_SEMICOLON);
                    return d;
                }
                // #2200: `type Name = fn(T1, T2) -> R` — a named C function-
                // pointer type, the alias form of `cfn Name(a: T1, b: T2) -> R`
                // (no new word; the signature is written as a use site would
                // write it). Same node, same resolution.
                if (dk && dk->type == TOKEN_IDENTIFIER && dk->value &&
                    strcmp(dk->value, "fn") == 0) {
                    Token* paren = peek_ahead(parser, 1);
                    if (!paren || paren->type != TOKEN_LEFT_PAREN) {
                        parser_error(parser,
                            "a function-pointer type alias needs a signature: "
                            "`type Name = fn(T1, T2) -> R`");
                        return NULL;
                    }
                    Type* sig = parse_type(parser);
                    if (!sig) return NULL;
                    if (fn_type_has_modes(sig)) {
                        /* #2980: the alias is a C function pointer's. */
                        parser_error_at(parser, name,
                            "a function-pointer type alias is a C function pointer, whose "
                            "parameters borrow; `@keep` and `@into` go in a closure's type, "
                            "written where it is used");
                        free_type(sig);
                        return NULL;
                    }
                    ASTNode* d = create_ast_node(AST_CFN_TYPE_DEF,
                                                 name->value, name->line, name->column);
                    d->node_type = sig;
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
                    strcmp(attr->value, "target") == 0) {
                    /* #2607: @target("avx2") import m as alias: a second
                     * copy of module `m`, compiled for that instruction set
                     * beside the build's own, so one program can carry both
                     * widths of its lane code and choose at run time. The
                     * target rides on the import as a `lane_target:` marker
                     * until orchestration registers the copy as a module of
                     * its own (module_bind_target_import). AVX2 is the one
                     * target: it is what changes an f32x8's width. */
                    advance_token(parser);  // consume 'target'
                    if (!expect_token(parser, TOKEN_LEFT_PAREN)) return NULL;
                    Token* isa = peek_token(parser);
                    if (!isa || isa->type != TOKEN_STRING_LITERAL || !isa->value) {
                        parser_error(parser, "@target expects the instruction set a module is compiled for again: @target(\"avx2\")");
                        return NULL;
                    }
                    int known = strcmp(isa->value, "avx2") == 0;
                    advance_token(parser);
                    if (!expect_token(parser, TOKEN_RIGHT_PAREN)) return NULL;
                    if (!peek_token(parser) || peek_token(parser)->type != TOKEN_IMPORT) {
                        parser_error(parser, "@target(\"avx2\") must precede an `import m as alias`");
                        return NULL;
                    }
                    /* The import is parsed either way, so an error here does
                     * not leave the parser inside it. */
                    ASTNode* imp = parse_import_statement(parser);
                    if (!imp) return NULL;
                    if (!known) {
                        parser_error(parser, "@target expects the instruction set a module is compiled for again: \"avx2\", the one that changes an f32x8's width");
                        return imp;
                    }
                    if (imp->annotation) {
                        parser_error(parser, "@target(\"avx2\") imports a module whole, under its `as` name: not as a glob import");
                        return imp;
                    }
                    imp->annotation = strdup("lane_target:avx2");
                    node = imp;
                    break;
                }
                if (attr && attr->type == TOKEN_IDENTIFIER && attr->value &&
                    strcmp(attr->value, "c_include") == 0) {
                    /* #1986: @c_include("aether_collections.h") — a header
                     * the generated C includes whenever this module is in
                     * the import closure. The name is written as C would
                     * write it; the include path is the build's (`ae` puts
                     * the stdlib's directories on it, and a project adds its
                     * own with `[build] cflags`). */
                    advance_token(parser);  // consume 'c_include'
                    if (!expect_token(parser, TOKEN_LEFT_PAREN)) return NULL;
                    Token* hdr = peek_token(parser);
                    if (!hdr || hdr->type != TOKEN_STRING_LITERAL || !hdr->value ||
                        !hdr->value[0]) {
                        parser_error(parser, "@c_include expects a header name, e.g. @c_include(\"api.h\")");
                        return NULL;
                    }
                    advance_token(parser);
                    if (!expect_token(parser, TOKEN_RIGHT_PAREN)) return NULL;
                    return create_ast_node(AST_C_INCLUDE_DIRECTIVE, hdr->value,
                                           at_tok->line, at_tok->column);
                }
                if (attr && attr->type == TOKEN_IDENTIFIER && attr->value &&
                    strcmp(attr->value, "source") == 0) {
                    /* #2125: @source("lanes.c") — a C file this module ships,
                     * named relative to the module's directory. Carried as an
                     * AST_SOURCE_DIRECTIVE whose value is the path as written;
                     * codegen resolves it against the file the node was
                     * stamped with and lists it for the build. */
                    advance_token(parser);  // consume 'source'
                    if (!expect_token(parser, TOKEN_LEFT_PAREN)) return NULL;
                    Token* path = peek_token(parser);
                    if (!path || path->type != TOKEN_STRING_LITERAL || !path->value ||
                        !path->value[0]) {
                        parser_error(parser, "@source expects the path of one C file, e.g. @source(\"lanes.c\")");
                        return NULL;
                    }
                    advance_token(parser);
                    if (!expect_token(parser, TOKEN_RIGHT_PAREN)) return NULL;
                    return create_ast_node(AST_SOURCE_DIRECTIVE, path->value,
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
                // @mailbox(bound: N) actor A { ... } (#2974): at most N
                // messages accepted for A and not yet taken by its step; a
                // send past it is refused, never blocked on. Carried as the
                // actor's "mailbox_bound:N" annotation.
                if (attr && attr->type == TOKEN_IDENTIFIER && attr->value &&
                    strcmp(attr->value, "mailbox") == 0) {
                    advance_token(parser);  // consume 'mailbox'
                    if (!expect_token(parser, TOKEN_LEFT_PAREN)) return NULL;
                    Token* key = peek_token(parser);
                    if (!key || key->type != TOKEN_IDENTIFIER || !key->value ||
                        strcmp(key->value, "bound") != 0) {
                        parser_error(parser, "@mailbox takes a bound, as in @mailbox(bound: 1000)");
                        return NULL;
                    }
                    advance_token(parser);  // consume 'bound'
                    if (!expect_token(parser, TOKEN_COLON)) return NULL;
                    Token* num = peek_token(parser);
                    char* end = NULL;
                    long bound = (num && num->type == TOKEN_NUMBER && num->value)
                                 ? strtol(num->value, &end, 10) : 0;
                    if (!end || *end || bound < 1 || bound > 1000000000L) {
                        parser_error(parser, "@mailbox(bound: N) takes a whole number N from 1 to 1000000000");
                        return NULL;
                    }
                    advance_token(parser);  // consume the number
                    if (!expect_token(parser, TOKEN_RIGHT_PAREN)) return NULL;
                    Token* next_tok = peek_token(parser);
                    if (!next_tok || next_tok->type != TOKEN_ACTOR) {
                        parser_error(parser, "@mailbox(bound: N) must precede an `actor` definition");
                        return NULL;
                    }
                    ASTNode* ad = parse_actor_definition(parser);
                    if (ad) {
                        char tag[48];
                        snprintf(tag, sizeof(tag), "mailbox_bound:%ld", bound);
                        free(ad->annotation);
                        ad->annotation = strdup(tag);
                    }
                    node = ad;
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
                    /* As long as the list (#3569): one past 256 bytes
                     * stopped the list there. */
                    const char* tag = "derive:";
                    int first = 1;
                    while (1) {
                        Token* d = expect_token(parser, TOKEN_IDENTIFIER);
                        if (!d || !d->value) break;
                        tag = aether_internf("%s%s%s", tag, first ? "" : ",", d->value);
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
                            /* `derive:` has to lead the marker set (derive.c
                             * keys on the prefix); markers the struct's own
                             * attributes added (`@observable`) follow it. */
                            char* own = sd->annotation;
                            sd->annotation = strdup(tag);
                            if (own) {
                                sd->annotation = annotation_add_marker(sd->annotation, own);
                                free(own);
                            }
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
                        if (fdef->annotation) free(fdef->annotation);
                        fdef->annotation =
                            parser_strf("c_callback:%s", explicit_sym ? explicit_sym : "");
                    }
                    if (explicit_sym) free(explicit_sym);
                    node = fdef;
                    break;
                }
                // `@quotable` (docs/quote.md, version 2): an ordinary function
                // a quote may call. Its body is also lowered to a tree, which
                // travels with each quote that reaches it, so a back end can
                // write it out as a function of its own (a GLSL function).
                if (attr && attr->type == TOKEN_IDENTIFIER && attr->value &&
                    strcmp(attr->value, "quotable") == 0) {
                    advance_token(parser);  // consume 'quotable'
                    if (peek_token(parser) && peek_token(parser)->type == TOKEN_FUNC)
                        advance_token(parser);  // optional 'func'
                    ASTNode* fdef = parse_function_definition(parser);
                    if (fdef) {
                        if (fdef->annotation) {
                            parser_error(parser, "@quotable cannot combine with another annotation (a variadic `...`, @c_callback or an effect tag) on the same function");
                            return NULL;
                        }
                        fdef->annotation = strdup("quotable");
                        /* Its body becomes a tree too, which names each call
                         * as written (`dot`, not the merged `shaderlib_dot`),
                         * as a quote's does. */
                        quote_keep_call_names(fdef);
                    }
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
                    parser_error(parser, "unknown attribute (expected @extern(\"...\"), @c_callback, @derive, @mailbox, @quotable, or an effect tag @pure/@no_fs/@no_net/@no_os)");
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
                ext->annotation = parser_strf("c_symbol:%s", sym->value);   /* #3569: whole */

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
                            /* The same per-param annotations as the bare
                             * `extern foo(...)` form, then the type: see
                             * parse_extern_param_attrs_and_type. */
                            parse_extern_param_attrs_and_type(parser, p);
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
                if (reject_none_binding(parser, vname, "variable name")) { advance_token(parser); return NULL; }
                Type* vtype = NULL;
                if (peek_token(parser) && peek_token(parser)->type == TOKEN_COLON) {
                    advance_token(parser); // consume ':'
                    vtype = parse_type(parser);
                }
                ASTNode* vval = NULL;
                if (vtype && vtype->kind == TYPE_OPTIONAL &&
                    !(peek_token(parser) && peek_token(parser)->type == TOKEN_ASSIGN)) {
                    /* #2800: an optional with no initialiser starts as
                     * `none`, as a local `let a: int?` does. */
                    vval = create_ast_node(AST_NONE_LITERAL, NULL, vline, vcol);
                } else {
                    Token* veq = expect_token(parser, TOKEN_ASSIGN);
                    if (!veq) { advance_token(parser); return NULL; }
                    int verrors = aether_error_count();
                    vval = parse_expression(parser);
                    if (!vval) missing_operand(parser, veq, verrors, "a value");
                }
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
                if (reject_none_binding(parser, cname, "constant name")) { advance_token(parser); return NULL; }

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

                Token* ceq = expect_token(parser, TOKEN_ASSIGN);
                if (!ceq) {
                    if (annotated_type) free_type(annotated_type);
                    advance_token(parser); return NULL;
                }
                int cerrors = aether_error_count();
                ASTNode* cval = parse_expression(parser);
                if (!cval) {
                    missing_operand(parser, ceq, cerrors, "a value");
                    if (annotated_type) free_type(annotated_type);
                    advance_token(parser); return NULL;
                }
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
                            : create_array_type(create_type(TYPE_UNKNOWN), cval->child_count);
                        /* Only a guess here: an element that is not a bare
                         * literal (`-1`, `0 - 1`, `~1`) has no type yet. The
                         * guess was `ptr`, so `[-1, 2, 3]` was an array of
                         * pointers, emitted as `const void*[]` (clang refuses
                         * the ints in it; gcc only warned) and read as
                         * pointers. Unknown, inference types it from its
                         * elements; marked inferred, the typechecker gives the
                         * declaration the literal's type once they are typed
                         * (a declaration's array_fix). */
                        node->type_inferred = 1;
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
                /* `fn main()`: `main` is a keyword token, not an identifier,
                 * so the shape above never matched it and `fn main()` was the
                 * one function `fn` could not spell ("unexpected identifier
                 * at top level"). */
                if (token->value && strcmp(token->value, "fn") == 0 &&
                    next && next->type == TOKEN_MAIN) {
                    Token* after = peek_ahead(parser, 2);
                    if (after && after->type == TOKEN_LEFT_PAREN) {
                        advance_token(parser);  // consume `fn`
                        node = parse_main_function(parser);
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
                        const char* msg = "";
                        msg = aether_internf(
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
        parser_error(parser, "expected '{' to open a `when` region");
        return;
    }
    /* #2907: no cap on the statements (it was 10000, then silence); each
     * pass consumes at least one token. */
    while (!is_at_end(parser)) {
        Token* t = peek_token(parser);
        if (!t || t->type == TOKEN_RIGHT_BRACE) break;
        int start = parser->current_token;
        int errors_before = aether_error_count();
        ASTNode* stmt = parse_statement(parser);
        if (stmt) add_child(block, stmt);
        else if (aether_error_count() == errors_before)
            parser_error(parser, "Expected statement in a `when` region");
        if (parser->current_token == start) advance_token(parser);
    }
    if (!match_token(parser, TOKEN_RIGHT_BRACE)) {
        parser_error(parser, "unterminated `when` region");
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
            parser_error(parser, "expected ')' in a `when` condition");
            *ok = 0;
        }
        return v;
    }

    if (t->type == TOKEN_IDENTIFIER && t->value && strcmp(t->value, "defined") == 0) {
        advance_token(parser);
        if (!match_token(parser, TOKEN_LEFT_PAREN)) {
            parser_error(parser, "expected '(' after `defined`");
            *ok = 0;
            return 0;
        }
        Token* name = peek_token(parser);
        if (!name || name->type != TOKEN_IDENTIFIER || !name->value) {
            parser_error(parser, "expected a symbol name inside `defined(...)`");
            *ok = 0;
            return 0;
        }
        int v = aether_define_is_set(name->value);
        advance_token(parser);
        if (!match_token(parser, TOKEN_RIGHT_PAREN)) {
            parser_error(parser, "expected ')' after the symbol name");
            *ok = 0;
        }
        return v;
    }

    parser_error(parser, "a `when` region condition is defined(NAME), "
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
        parser_error(parser, "expected '{' to open a `when` region");
        return;
    }
    /* #2907: no cap on the declarations (it was 10000, then silence);
     * each pass consumes at least one token. */
    while (!is_at_end(parser)) {
        Token* t = peek_token(parser);
        if (!t || t->type == TOKEN_RIGHT_BRACE) break;
        int start = parser->current_token;
        ASTNode* node = parse_top_level_decl(parser);
        if (node) add_child(program, node);
        if (parser->current_token == start) advance_token(parser);
    }
    if (!match_token(parser, TOKEN_RIGHT_BRACE)) {
        parser_error(parser, "unterminated `when` region");
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

    /* #2907: no count of the declarations. A program of 10000 or more
     * stopped at a "safety limit" whose message printed nothing, and aetherc
     * exited 1 without a word. What the limit guarded against, a pass that
     * consumes nothing and so repeats forever, is guarded directly: every
     * pass consumes at least one token. */
    while (!is_at_end(parser)) {
        if (parser->depth_exceeded) break;
        int start_token = parser->current_token;

        if (at_when_region(parser)) {
            parse_when_region(parser, program);
        } else {
            ASTNode* node = parse_top_level_decl(parser);
            if (node) add_child(program, node);
        }
        // Error recovery that consumed nothing: step over the token.
        if (parser->current_token == start_token) advance_token(parser);
    }

    return program;
}
