#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <limits.h>
#include "tokens.h"
#include "lexer.h"

#define MAX_IDENTIFIER_LENGTH 256

static const char* source;
static int source_length;
static int current_pos;
static int current_line;
static int current_column;

void lexer_init(const char* src) {
    source = src;
    source_length = strlen(src);
    current_pos = 0;
    current_line = 1;
    current_column = 1;
}

void lexer_save(LexerState* out) {
    out->source        = source;
    out->source_length = source_length;
    out->current_pos   = current_pos;
    out->current_line  = current_line;
    out->current_column = current_column;
}

void lexer_restore(const LexerState* in) {
    source        = in->source;
    source_length = in->source_length;
    current_pos   = in->current_pos;
    current_line  = in->current_line;
    current_column = in->current_column;
}

char peek() {
    if (current_pos >= source_length) return '\0';
    return source[current_pos];
}

char advance() {
    if (current_pos >= source_length) return '\0';
    char c = source[current_pos++];
    if (c == '\n') {
        current_line++;
        current_column = 1;
    } else if (((unsigned char)c & 0xC0) != 0x80) {
        /* Advance the column once per CHARACTER, not per byte. A UTF-8
         * continuation byte (0b10xxxxxx) is the 2nd+ byte of a multibyte
         * codepoint and must not bump the column, or the source-snippet
         * caret drifts right of any token following a multibyte char on
         * the same line (issue #645). Leading bytes (ASCII or the first
         * byte of a sequence) still count. */
        current_column++;
    }
    return c;
}

void skip_whitespace() {
    while (current_pos < source_length) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r') {
            advance();
        } else if (c == '\n') {
            advance();
        } else {
            break;
        }
    }
}

int skip_comment() {
    if (peek() == '/' && current_pos + 1 < source_length && source[current_pos + 1] == '/') {
        // Single line comment
        while (current_pos < source_length && peek() != '\n') {
            advance();
        }
        return 1;
    } else if (peek() == '/' && current_pos + 1 < source_length && source[current_pos + 1] == '*') {
        // Multi-line comment
        advance(); // skip /
        advance(); // skip *
        int found_end = 0;
        while (current_pos < source_length) {
            if (peek() == '*' && current_pos + 1 < source_length && source[current_pos + 1] == '/') {
                advance(); // skip *
                advance(); // skip /
                found_end = 1;
                break;
            }
            advance();
        }
        if (!found_end) {
            fprintf(stderr, "Error: unterminated multi-line comment at line %d\n", current_line);
        }
        return 1;
    }
    return 0;
}

Token* read_string() {
    advance(); // skip opening quote
    int capacity = MAX_IDENTIFIER_LENGTH;
    char* buffer = malloc(capacity);
    int i = 0;
    bool has_interp = false;
    // Depth of ${...} nesting we're currently inside. While > 0, an
    // unescaped '"' opens a NESTED string literal (e.g. ${id("hi")})
    // rather than ending the outer string, and a '}' closes one level
    // of interpolation instead of being ordinary literal text.
    int interp_depth = 0;

    while (current_pos < source_length && (interp_depth > 0 || peek() != '"')) {
        if (i >= capacity - 3) {
            capacity *= 2;
            char* new_buf = realloc(buffer, capacity);
            if (!new_buf) { free(buffer); return create_token(TOKEN_ERROR, "out of memory", current_line, current_column); }
            buffer = new_buf;
        }
        if (peek() == '$' && current_pos + 1 < source_length && source[current_pos + 1] == '{') {
            has_interp = true;
            interp_depth++;
            // Store raw ${...} content without escape processing
            buffer[i++] = advance(); // $
            buffer[i++] = advance(); // {
        } else if (interp_depth > 0 &&
                   (peek() == '"' ||
                    (peek() == '\\' && current_pos + 1 < source_length && source[current_pos + 1] == '"'))) {
            // Nested string literal inside ${...}: copy it through verbatim
            // (including its own escapes) so the parser's re-lex of the
            // interpolation expression sees real Aether source, not text
            // that was only valid in the OUTER string's escaping convention.
            // A leading '\"' is accepted the same as '"' (the natural
            // spelling a developer reaches for since the outer string's own
            // quotes must be escaped) and normalized down to a plain '"' —
            // it's not needed here since a bare '"' no longer terminates
            // the outer string, but rejecting it outright would resurrect
            // the silent-empty trap for whichever spelling isn't the one
            // this branch expects.
            if (peek() == '\\') advance(); // drop the escaping backslash
            buffer[i++] = advance(); // opening "
            bool closed = false;
            while (current_pos < source_length) {
                if (i >= capacity - 3) { capacity *= 2; char* nb = realloc(buffer, capacity); if (!nb) { free(buffer); return create_token(TOKEN_ERROR, "out of memory", current_line, current_column); } buffer = nb; }
                if (peek() == '"') {
                    buffer[i++] = advance(); // closing "
                    closed = true;
                    break;
                }
                if (peek() == '\\' && current_pos + 1 < source_length && source[current_pos + 1] == '"') {
                    advance(); // drop the escaping backslash
                    buffer[i++] = advance(); // closing " (normalized, unescaped)
                    closed = true;
                    break;
                }
                if (peek() == '\\' && current_pos + 1 < source_length) {
                    buffer[i++] = advance(); // backslash
                    buffer[i++] = advance(); // escaped char
                } else {
                    buffer[i++] = advance();
                }
            }
            if (!closed) {
                free(buffer);
                return create_token(TOKEN_ERROR, "unterminated string literal", current_line, current_column);
            }
        } else if (interp_depth > 0 && peek() == '{') {
            interp_depth++;
            buffer[i++] = advance();
        } else if (interp_depth > 0 && peek() == '}') {
            interp_depth--;
            buffer[i++] = advance();
        } else if (peek() == '\\') {
            if (has_interp) {
                // In interpolated strings, keep escape sequences raw so parser can handle them
                buffer[i++] = advance(); // backslash
                if (current_pos < source_length) {
                    char esc = peek();
                    buffer[i++] = advance(); // escaped char (e.g. 'x', '0', 'n')
                    if (esc == 'x') {
                        // Copy up to 2 hex digits so parser sees \xNN together
                        int d = 0;
                        while (d < 2 && current_pos < source_length &&
                               isxdigit((unsigned char)peek())) {
                            if (i >= capacity - 3) { capacity *= 2; char* nb = realloc(buffer, capacity); if (!nb) { free(buffer); return create_token(TOKEN_ERROR, "out of memory", current_line, current_column); } buffer = nb; }
                            buffer[i++] = advance();
                            d++;
                        }
                    } else if (esc >= '0' && esc <= '7') {
                        // Copy up to 2 more octal digits so parser sees \NNN together
                        int d = 0;
                        while (d < 2 && current_pos < source_length &&
                               peek() >= '0' && peek() <= '7') {
                            if (i >= capacity - 3) { capacity *= 2; char* nb = realloc(buffer, capacity); if (!nb) { free(buffer); return create_token(TOKEN_ERROR, "out of memory", current_line, current_column); } buffer = nb; }
                            buffer[i++] = advance();
                            d++;
                        }
                    }
                }
            } else {
                advance(); // skip backslash
                char c = advance();
                switch (c) {
                    case 'n': buffer[i++] = '\n'; break;
                    case 't': buffer[i++] = '\t'; break;
                    case 'r': buffer[i++] = '\r'; break;
                    case '\\': buffer[i++] = '\\'; break;
                    case '"': buffer[i++] = '"'; break;
                    case 'x': {  // \xNN hex escape (1-2 hex digits)
                        int val = 0, digits = 0;
                        while (digits < 2 && current_pos < source_length &&
                               isxdigit((unsigned char)peek())) {
                            char h = advance();
                            val = val * 16 + (h >= 'a' ? h - 'a' + 10 :
                                              h >= 'A' ? h - 'A' + 10 : h - '0');
                            digits++;
                        }
                        if (digits == 0) { buffer[i++] = 'x'; break; }
                        buffer[i++] = (char)val;
                        break;
                    }
                    case '0': case '1': case '2': case '3':
                    case '4': case '5': case '6': case '7': {  // \NNN octal (1-3 digits)
                        int val = c - '0', digits = 1;
                        while (digits < 3 && current_pos < source_length &&
                               peek() >= '0' && peek() <= '7') {
                            val = val * 8 + (advance() - '0');
                            digits++;
                        }
                        buffer[i++] = (char)(val & 0xFF);
                        break;
                    }
                    default: buffer[i++] = c; break;
                }
            }
        } else {
            buffer[i++] = advance();
        }
    }

    if (current_pos < source_length && peek() == '"') {
        advance(); // skip closing quote
    } else {
        // Unterminated string
        free(buffer);
        return create_token(TOKEN_ERROR, "unterminated string literal", current_line, current_column);
    }

    buffer[i] = '\0';
    AeTokenType tok_type = has_interp ? TOKEN_INTERP_STRING : TOKEN_STRING_LITERAL;
    Token* token = create_token(tok_type, buffer, current_line, current_column);
    free(buffer);
    return token;
}

Token* read_number() {
    int capacity = MAX_IDENTIFIER_LENGTH;
    char* buffer = malloc(capacity);
    int i = 0;

    // Check for hex (0x), octal (0o), binary (0b) prefixes
    if (peek() == '0' && current_pos + 1 < source_length) {
        char next = source[current_pos + 1];
        if (next == 'x' || next == 'X') {
            // Hex literal: 0x[0-9a-fA-F]+
            buffer[i++] = advance(); // '0'
            buffer[i++] = advance(); // 'x'
            while (current_pos < source_length && (isxdigit((unsigned char)peek()) || peek() == '_')) {
                if (peek() == '_') { advance(); continue; }
                if (i >= capacity - 1) { capacity *= 2; char* nb = realloc(buffer, capacity); if (!nb) { free(buffer); return create_token(TOKEN_ERROR, "out of memory", current_line, current_column); } buffer = nb; }
                buffer[i++] = advance();
            }
            buffer[i] = '\0';
            Token* token = create_token(TOKEN_NUMBER, buffer, current_line, current_column);
            free(buffer);
            return token;
        } else if (next == 'o' || next == 'O') {
            // Octal literal: 0o[0-7]+
            buffer[i++] = advance(); // '0'
            buffer[i++] = advance(); // 'o'
            while (current_pos < source_length && ((peek() >= '0' && peek() <= '7') || peek() == '_')) {
                if (peek() == '_') { advance(); continue; }
                if (i >= capacity - 1) { capacity *= 2; char* nb = realloc(buffer, capacity); if (!nb) { free(buffer); return create_token(TOKEN_ERROR, "out of memory", current_line, current_column); } buffer = nb; }
                buffer[i++] = advance();
            }
            buffer[i] = '\0';
            Token* token = create_token(TOKEN_NUMBER, buffer, current_line, current_column);
            free(buffer);
            return token;
        } else if (next == 'b' || next == 'B') {
            // Binary literal: 0b[01]+
            buffer[i++] = advance(); // '0'
            buffer[i++] = advance(); // 'b'
            while (current_pos < source_length && (peek() == '0' || peek() == '1' || peek() == '_')) {
                if (peek() == '_') { advance(); continue; }
                if (i >= capacity - 1) { capacity *= 2; char* nb = realloc(buffer, capacity); if (!nb) { free(buffer); return create_token(TOKEN_ERROR, "out of memory", current_line, current_column); } buffer = nb; }
                buffer[i++] = advance();
            }
            buffer[i] = '\0';
            Token* token = create_token(TOKEN_NUMBER, buffer, current_line, current_column);
            free(buffer);
            return token;
        }
    }

    // Decimal literal (with optional dot for floats)
    // Don't consume '.' if followed by another '.' (range operator '..')
    while (current_pos < source_length && (isdigit((unsigned char)peek()) || (peek() == '.' && (current_pos + 1 >= source_length || source[current_pos + 1] != '.')))) {
        if (i >= capacity - 1) {
            capacity *= 2;
            char* new_buf = realloc(buffer, capacity);
            if (!new_buf) { free(buffer); return create_token(TOKEN_ERROR, "out of memory", current_line, current_column); }
            buffer = new_buf;
        }
        buffer[i++] = advance();
    }

    // Duration suffixes: ns, us, ms, s, m, h, d. Compound forms
    // (`2m30s`, `1h15m`) stay a single numeric token so parser
    // precedence remains ordinary literal precedence.
    while (current_pos < source_length) {
        int unit_len = 0;
        if (current_pos + 1 < source_length &&
            ((source[current_pos] == 'n' && source[current_pos + 1] == 's') ||
             (source[current_pos] == 'u' && source[current_pos + 1] == 's') ||
             (source[current_pos] == 'm' && source[current_pos + 1] == 's'))) {
            unit_len = 2;
        } else if (source[current_pos] == 's' || source[current_pos] == 'm' ||
                   source[current_pos] == 'h' || source[current_pos] == 'd') {
            unit_len = 1;
        }

        if (unit_len == 0) break;

        char after_unit = (current_pos + unit_len < source_length) ? source[current_pos + unit_len] : '\0';
        if (isalpha((unsigned char)after_unit) || after_unit == '_') break;

        for (int u = 0; u < unit_len; u++) {
            if (i >= capacity - 1) {
                capacity *= 2;
                char* new_buf = realloc(buffer, capacity);
                if (!new_buf) { free(buffer); return create_token(TOKEN_ERROR, "out of memory", current_line, current_column); }
                buffer = new_buf;
            }
            buffer[i++] = advance();
        }

        while (current_pos < source_length &&
               (isdigit((unsigned char)peek()) || (peek() == '.' && (current_pos + 1 >= source_length || source[current_pos + 1] != '.')))) {
            if (i >= capacity - 1) {
                capacity *= 2;
                char* new_buf = realloc(buffer, capacity);
                if (!new_buf) { free(buffer); return create_token(TOKEN_ERROR, "out of memory", current_line, current_column); }
                buffer = new_buf;
            }
            buffer[i++] = advance();
        }
    }

    buffer[i] = '\0';
    Token* token = create_token(TOKEN_NUMBER, buffer, current_line, current_column);
    free(buffer);
    return token;
}

Token* read_identifier() {
    int capacity = MAX_IDENTIFIER_LENGTH;
    char* buffer = malloc(capacity);
    int i = 0;
    
    while (current_pos < source_length && (isalnum((unsigned char)peek()) || peek() == '_')) {
        if (i >= capacity - 1) {
            capacity *= 2;
            char* new_buf = realloc(buffer, capacity);
            if (!new_buf) { free(buffer); return create_token(TOKEN_ERROR, "out of memory", current_line, current_column); }
            buffer = new_buf;
        }
        buffer[i++] = advance();
    }
    
    buffer[i] = '\0';

    // Check if it's a keyword - create token then free buffer
    Token* token;
    if (strcmp(buffer, "actor") == 0) token = create_token(TOKEN_ACTOR, buffer, current_line, current_column);
    else if (strcmp(buffer, "main") == 0) token = create_token(TOKEN_MAIN, buffer, current_line, current_column);
    else if (strcmp(buffer, "func") == 0) token = create_token(TOKEN_FUNC, buffer, current_line, current_column);
    else if (strcmp(buffer, "let") == 0) token = create_token(TOKEN_LET, buffer, current_line, current_column);
    else if (strcmp(buffer, "var") == 0) token = create_token(TOKEN_VAR, buffer, current_line, current_column);
    else if (strcmp(buffer, "if") == 0) token = create_token(TOKEN_IF, buffer, current_line, current_column);
    else if (strcmp(buffer, "else") == 0) token = create_token(TOKEN_ELSE, buffer, current_line, current_column);
    else if (strcmp(buffer, "for") == 0) token = create_token(TOKEN_FOR, buffer, current_line, current_column);
    else if (strcmp(buffer, "while") == 0) token = create_token(TOKEN_WHILE, buffer, current_line, current_column);
    else if (strcmp(buffer, "switch") == 0) token = create_token(TOKEN_SWITCH, buffer, current_line, current_column);
    else if (strcmp(buffer, "case") == 0) token = create_token(TOKEN_CASE, buffer, current_line, current_column);
    else if (strcmp(buffer, "default") == 0) token = create_token(TOKEN_DEFAULT, buffer, current_line, current_column);
    else if (strcmp(buffer, "break") == 0) token = create_token(TOKEN_BREAK, buffer, current_line, current_column);
    else if (strcmp(buffer, "continue") == 0) token = create_token(TOKEN_CONTINUE, buffer, current_line, current_column);
    else if (strcmp(buffer, "return") == 0) token = create_token(TOKEN_RETURN, buffer, current_line, current_column);
    else if (strcmp(buffer, "defer") == 0) token = create_token(TOKEN_DEFER, buffer, current_line, current_column);
    else if (strcmp(buffer, "builder") == 0) token = create_token(TOKEN_BUILDER, buffer, current_line, current_column);
    else if (strcmp(buffer, "match") == 0) token = create_token(TOKEN_MATCH, buffer, current_line, current_column);
    else if (strcmp(buffer, "when") == 0) token = create_token(TOKEN_WHEN, buffer, current_line, current_column);
    else if (strcmp(buffer, "receive") == 0) token = create_token(TOKEN_RECEIVE, buffer, current_line, current_column);
    else if (strcmp(buffer, "send") == 0) token = create_token(TOKEN_SEND, buffer, current_line, current_column);
    else if (strcmp(buffer, "spawn_actor") == 0) token = create_token(TOKEN_SPAWN_ACTOR, buffer, current_line, current_column);
    else if (strcmp(buffer, "spawn") == 0) token = create_token(TOKEN_SPAWN, buffer, current_line, current_column);
    else if (strcmp(buffer, "make") == 0) token = create_token(TOKEN_MAKE, buffer, current_line, current_column);
    else if (strcmp(buffer, "self") == 0) token = create_token(TOKEN_SELF, buffer, current_line, current_column);
    else if (strcmp(buffer, "state") == 0) token = create_token(TOKEN_STATE, buffer, current_line, current_column);
    else if (strcmp(buffer, "struct") == 0) token = create_token(TOKEN_STRUCT, buffer, current_line, current_column);
    else if (strcmp(buffer, "union") == 0) token = create_token(TOKEN_UNION, buffer, current_line, current_column);
    else if (strcmp(buffer, "import") == 0) token = create_token(TOKEN_IMPORT, buffer, current_line, current_column);
    else if (strcmp(buffer, "as") == 0) token = create_token(TOKEN_AS, buffer, current_line, current_column);
    else if (strcmp(buffer, "export") == 0) token = create_token(TOKEN_EXPORT, buffer, current_line, current_column);
    else if (strcmp(buffer, "exports") == 0) token = create_token(TOKEN_EXPORTS, buffer, current_line, current_column);
    else if (strcmp(buffer, "module") == 0) token = create_token(TOKEN_MODULE, buffer, current_line, current_column);
    else if (strcmp(buffer, "message") == 0) token = create_token(TOKEN_MESSAGE_KEYWORD, buffer, current_line, current_column);
    else if (strcmp(buffer, "reply") == 0) token = create_token(TOKEN_REPLY, buffer, current_line, current_column);
    else if (strcmp(buffer, "extern") == 0) token = create_token(TOKEN_EXTERN, buffer, current_line, current_column);
    else if (strcmp(buffer, "null") == 0) token = create_token(TOKEN_NULL, buffer, current_line, current_column);
    else if (strcmp(buffer, "const") == 0) token = create_token(TOKEN_CONST, buffer, current_line, current_column);
    else if (strcmp(buffer, "in") == 0) token = create_token(TOKEN_IN, buffer, current_line, current_column);
    else if (strcmp(buffer, "after") == 0) token = create_token(TOKEN_AFTER, buffer, current_line, current_column);
    else if (strcmp(buffer, "callback") == 0) token = create_token(TOKEN_CALLBACK, buffer, current_line, current_column);
    else if (strcmp(buffer, "hide") == 0) token = create_token(TOKEN_HIDE, buffer, current_line, current_column);
    else if (strcmp(buffer, "seal") == 0) token = create_token(TOKEN_SEAL, buffer, current_line, current_column);
    else if (strcmp(buffer, "except") == 0) token = create_token(TOKEN_EXCEPT, buffer, current_line, current_column);
    else if (strcmp(buffer, "try") == 0) token = create_token(TOKEN_TRY, buffer, current_line, current_column);
    else if (strcmp(buffer, "catch") == 0) token = create_token(TOKEN_CATCH, buffer, current_line, current_column);
    else if (strcmp(buffer, "panic") == 0) token = create_token(TOKEN_PANIC, buffer, current_line, current_column);
    else if (strcmp(buffer, "requires") == 0) token = create_token(TOKEN_REQUIRES, buffer, current_line, current_column);
    else if (strcmp(buffer, "ensures") == 0) token = create_token(TOKEN_ENSURES, buffer, current_line, current_column);
    else if (strcmp(buffer, "ptr") == 0) token = create_token(TOKEN_PTR, buffer, current_line, current_column);
    else if (strcmp(buffer, "int") == 0) token = create_token(TOKEN_INT, buffer, current_line, current_column);
    else if (strcmp(buffer, "long") == 0) token = create_token(TOKEN_INT64, buffer, current_line, current_column);
    else if (strcmp(buffer, "uint64") == 0) token = create_token(TOKEN_UINT64, buffer, current_line, current_column);
    else if (strcmp(buffer, "Duration") == 0) token = create_token(TOKEN_DURATION, buffer, current_line, current_column);
    else if (strcmp(buffer, "float") == 0) token = create_token(TOKEN_FLOAT, buffer, current_line, current_column);
    else if (strcmp(buffer, "bool") == 0) token = create_token(TOKEN_BOOL, buffer, current_line, current_column);
    else if (strcmp(buffer, "byte") == 0) token = create_token(TOKEN_BYTE, buffer, current_line, current_column);
    else if (strcmp(buffer, "string") == 0) token = create_token(TOKEN_STRING, buffer, current_line, current_column);
    else if (strcmp(buffer, "ActorRef") == 0 || strcmp(buffer, "actor_ref") == 0) token = create_token(TOKEN_ACTOR_REF, buffer, current_line, current_column);
    else if (strcmp(buffer, "Message") == 0) token = create_token(TOKEN_MESSAGE, buffer, current_line, current_column);
    else if (strcmp(buffer, "true") == 0) token = create_token(TOKEN_TRUE, buffer, current_line, current_column);
    else if (strcmp(buffer, "false") == 0) token = create_token(TOKEN_FALSE, buffer, current_line, current_column);
    else if (strcmp(buffer, "print") == 0) token = create_token(TOKEN_PRINT, buffer, current_line, current_column);
    else token = create_token(TOKEN_IDENTIFIER, buffer, current_line, current_column);

    free(buffer);
    return token;
}

/* Raw identifier: `name` — any identifier-shaped token, including a
 * reserved keyword, used verbatim as an ordinary identifier (#867).
 * Lets a C→Aether port keep faithful names like `reply`, `when`, `after`,
 * `ptr`, `message` as parameter / local / struct-field / function names.
 * The opening backtick has already been consumed by the caller. Always
 * emits TOKEN_IDENTIFIER, so every identifier-expecting grammar position
 * accepts it with no parser change. Mirrors read_identifier's buffer
 * handling so the value buffer is owned and freed identically (no leak). */
Token* read_raw_identifier(void) {
    int start_line = current_line, start_col = current_column;
    int capacity = MAX_IDENTIFIER_LENGTH;
    char* buffer = malloc(capacity);
    int i = 0;

    while (current_pos < source_length && (isalnum((unsigned char)peek()) || peek() == '_')) {
        if (i >= capacity - 1) {
            capacity *= 2;
            char* new_buf = realloc(buffer, capacity);
            if (!new_buf) { free(buffer); return create_token(TOKEN_ERROR, "out of memory", start_line, start_col); }
            buffer = new_buf;
        }
        buffer[i++] = advance();
    }
    buffer[i] = '\0';

    if (i == 0) {
        free(buffer);
        return create_token(TOKEN_ERROR, "empty raw identifier, `` is not a valid name", start_line, start_col);
    }
    if (isdigit((unsigned char)buffer[0])) {
        free(buffer);
        return create_token(TOKEN_ERROR, "raw identifier must not start with a digit", start_line, start_col);
    }
    if (current_pos >= source_length || peek() != '`') {
        free(buffer);
        return create_token(TOKEN_ERROR, "unterminated raw identifier, missing closing backtick", start_line, start_col);
    }
    advance(); /* consume the closing backtick */

    Token* token = create_token(TOKEN_IDENTIFIER, buffer, start_line, start_col);
    free(buffer);
    return token;
}

Token* next_token() {
    skip_whitespace();
    
    if (current_pos >= source_length) {
        return create_token(TOKEN_EOF, NULL, current_line, current_column);
    }
    
    char c = peek();
    
    // Handle comments
    if (c == '/' && skip_comment()) {
        return next_token(); // Recursively get next token after comment
    }
    
    // Handle string literals
    if (c == '"') {
        return read_string();
    }

    // Handle raw identifiers: `name` escapes a reserved keyword for use
    // as an ordinary identifier (#867).
    if (c == '`') {
        advance(); // consume the opening backtick
        return read_raw_identifier();
    }

    // Handle numbers
    if (isdigit((unsigned char)c)) {
        return read_number();
    }

    // Handle identifiers and keywords
    if (isalpha((unsigned char)c) || c == '_') {
        return read_identifier();
    }
    
    // Handle operators and delimiters
    switch (c) {
        case '+':
            advance();
            if (peek() == '+') {
                advance();
                return create_token(TOKEN_INCREMENT, "++", current_line, current_column);
            }
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_PLUS_ASSIGN, "+=", current_line, current_column);
            }
            return create_token(TOKEN_PLUS, "+", current_line, current_column);
        case '-':
            advance();
            if (peek() == '>') {
                advance();
                return create_token(TOKEN_ARROW, "->", current_line, current_column);
            }
            if (peek() == '-') {
                advance();
                return create_token(TOKEN_DECREMENT, "--", current_line, current_column);
            }
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_MINUS_ASSIGN, "-=", current_line, current_column);
            }
            return create_token(TOKEN_MINUS, "-", current_line, current_column);
        case '*':
            advance();
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_MULTIPLY_ASSIGN, "*=", current_line, current_column);
            }
            return create_token(TOKEN_MULTIPLY, "*", current_line, current_column);
        case '/':
            advance();
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_DIVIDE_ASSIGN, "/=", current_line, current_column);
            }
            return create_token(TOKEN_DIVIDE, "/", current_line, current_column);
        case '%':
            advance();
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_MODULO_ASSIGN, "%=", current_line, current_column);
            }
            return create_token(TOKEN_MODULO, "%", current_line, current_column);
        case '=':
            advance();
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_EQUALS, "==", current_line, current_column);
            }
            return create_token(TOKEN_ASSIGN, "=", current_line, current_column);
        case '!':
            advance();
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_NOT_EQUALS, "!=", current_line, current_column);
            }
            // In Actor V2, ! is the fire-and-forget operator
            // Also used as logical NOT - context determines usage
            return create_token(TOKEN_EXCLAIM, "!", current_line, current_column);
        case '<':
            advance();
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_LESS_EQUAL, "<=", current_line, current_column);
            }
            if (peek() == '<') {
                advance();
                if (peek() == '=') {
                    advance();
                    return create_token(TOKEN_LSHIFT_ASSIGN, "<<=", current_line, current_column);
                }
                // Heredoc: <<MARKER ... MARKER
                // Content is a literal string (no interpolation).
                // Preserves all whitespace and newlines exactly.
                // Use regular "..." strings for interpolation.
                if (isalpha((unsigned char)peek()) || peek() == '_') {
                    // Read the marker name
                    char marker[MAX_IDENTIFIER_LENGTH];
                    int mlen = 0;
                    while ((isalnum((unsigned char)peek()) || peek() == '_') && mlen < MAX_IDENTIFIER_LENGTH - 1) {
                        marker[mlen++] = advance();
                    }
                    marker[mlen] = '\0';

                    // Skip to end of current line (past the marker)
                    while (peek() != '\n' && peek() != '\0') advance();
                    if (peek() == '\n') advance();

                    // Collect everything until a line that is exactly the marker
                    int start_line = current_line;
                    int buf_capacity = 4096;
                    char* buf = malloc(buf_capacity);
                    if (!buf) return create_token(TOKEN_ERROR, "out of memory", current_line, current_column);
                    int blen = 0;
                    // Close detection (#922). Scan line by line. A line closes
                    // the heredoc only when it is, in full, optional leading
                    // whitespace followed by exactly the marker and then a line
                    // ending or EOF — AND its indentation is at-or-below the
                    // shallowest non-blank body line seen so far. The closing
                    // marker lives at the content's base level: a line that is
                    // *more* indented than the body but happens to equal the
                    // marker is therefore body content, not a terminator, so an
                    // indented marker-like line can never silently truncate the
                    // body. (A lone marker indented PAST the body never matches,
                    // so the scan runs to EOF and reports an unterminated
                    // heredoc rather than dropping content.) The marker must be
                    // the only token on its line — `foo MARK` / `xMARK` stay
                    // content. Body dedent (below) is unchanged: common leading
                    // whitespace, i.e. the least-indented line, like Ruby's
                    // squiggly `<<~`.
                    int min_body_indent = INT_MAX;
                    int closed = 0;
                    while (current_pos < source_length) {
                        // Leading whitespace of the line at current_pos.
                        int ws = 0;
                        while (current_pos + ws < source_length &&
                               (source[current_pos + ws] == ' ' ||
                                source[current_pos + ws] == '\t')) {
                            ws++;
                        }
                        int mpos = current_pos + ws;
                        // Is the rest of the line exactly the marker + EOL/EOF?
                        int is_marker_line = 0;
                        if (mpos + mlen <= source_length &&
                            memcmp(&source[mpos], marker, (size_t)mlen) == 0) {
                            int after = mpos + mlen;
                            if (after >= source_length ||
                                source[after] == '\n' ||
                                (source[after] == '\r' && after + 1 < source_length &&
                                 source[after + 1] == '\n')) {
                                is_marker_line = 1;
                            }
                        }
                        if (is_marker_line && ws <= min_body_indent) {
                            // Consume the marker line + its ending, then stop.
                            for (int i = 0; i < ws + mlen; i++) advance();
                            if (peek() == '\r') advance();
                            if (peek() == '\n') advance();
                            closed = 1;
                            break;
                        }
                        // Body line. A blank line (only whitespace) does not
                        // constrain the base indent, mirroring the dedent pass.
                        int line_is_blank = (mpos >= source_length ||
                                             source[mpos] == '\n' ||
                                             (source[mpos] == '\r' && mpos + 1 < source_length &&
                                              source[mpos + 1] == '\n'));
                        if (!line_is_blank && ws < min_body_indent) min_body_indent = ws;
                        // Copy the line and its newline, normalizing \r\n -> \n.
                        while (peek() != '\n' && peek() != '\0') {
                            char ch = peek();
                            if (ch != '\r' || (current_pos + 1 < source_length &&
                                               source[current_pos + 1] != '\n')) {
                                if (blen >= buf_capacity - 1) {
                                    buf_capacity *= 2;
                                    char* new_buf = realloc(buf, buf_capacity);
                                    if (!new_buf) {
                                        free(buf);
                                        return create_token(TOKEN_ERROR, "heredoc too large", current_line, current_column);
                                    }
                                    buf = new_buf;
                                }
                                buf[blen++] = ch;
                            }
                            advance();
                        }
                        if (peek() == '\n') {
                            if (blen >= buf_capacity - 1) {
                                buf_capacity *= 2;
                                char* new_buf = realloc(buf, buf_capacity);
                                if (!new_buf) {
                                    free(buf);
                                    return create_token(TOKEN_ERROR, "heredoc too large", current_line, current_column);
                                }
                                buf = new_buf;
                            }
                            buf[blen++] = '\n';
                            advance();
                        } else {
                            break;   // EOF mid-line, no closing marker
                        }
                    }
                    if (!closed) {
                        free(buf);
                        char err[MAX_IDENTIFIER_LENGTH + 64];
                        snprintf(err, sizeof(err),
                                 "unterminated heredoc: missing closing marker '%s'",
                                 marker);
                        return create_token(TOKEN_ERROR, err, start_line, current_column);
                    }
                    // Strip the final newline before the marker line.
                    // Content between the markers is preserved exactly,
                    // but the newline immediately before the closing
                    // marker is an artifact of the syntax, not content.
                    if (blen > 0 && buf[blen-1] == '\n') blen--;
                    buf[blen] = '\0';

                    // ---- Common-leading-whitespace dedent ----
                    // Strip the longest run of leading whitespace shared by
                    // every non-blank content line, so a heredoc can be
                    // indented to match its surrounding code without that
                    // indentation leaking into the string. Blank lines
                    // (empty or whitespace-only) don't constrain the common
                    // prefix (a single blank line shouldn't force prefix=0),
                    // but they are still emitted. The prefix match is
                    // character-exact: a space where another line has a tab
                    // (or vice-versa) is an inconsistency that stops the
                    // strip at that column — we never shift past a column
                    // where lines disagree. To keep a literal common indent,
                    // shift one line further left than the rest.
                    {
                        // Pass 1: compute the common whitespace prefix.
                        // `common` points at the first non-blank line's
                        // leading whitespace; `common_len` shrinks as later
                        // lines diverge.
                        const char* common = NULL;
                        int common_len = 0;
                        int li = 0;
                        while (li < blen) {
                            // Measure this line's leading whitespace.
                            int ws = 0;
                            while (li + ws < blen &&
                                   (buf[li + ws] == ' ' || buf[li + ws] == '\t')) {
                                ws++;
                            }
                            int line_end = li + ws;
                            int is_blank = (line_end >= blen || buf[line_end] == '\n');
                            if (!is_blank) {
                                if (common == NULL) {
                                    common = &buf[li];
                                    common_len = ws;
                                } else {
                                    // Trim common_len to the longest exact
                                    // character match with this line's ws.
                                    int k = 0;
                                    while (k < common_len && k < ws &&
                                           common[k] == buf[li + k]) {
                                        k++;
                                    }
                                    common_len = k;
                                }
                            }
                            // Advance to the start of the next line.
                            int p = li;
                            while (p < blen && buf[p] != '\n') p++;
                            if (p < blen) p++;   // skip the '\n'
                            li = p;
                        }

                        // Pass 2: rewrite buf in place, dropping the first
                        // `common_len` chars of each line.
                        if (common_len > 0) {
                            int rd = 0, wr = 0;
                            while (rd < blen) {
                                // Skip up to common_len leading ws chars on
                                // this line (a blank/short line may have
                                // fewer — drop only what it has).
                                int skip = 0;
                                while (skip < common_len && rd < blen &&
                                       (buf[rd] == ' ' || buf[rd] == '\t') &&
                                       buf[rd] != '\n') {
                                    rd++;
                                    skip++;
                                }
                                // Copy the rest of the line including '\n'.
                                while (rd < blen && buf[rd] != '\n') {
                                    buf[wr++] = buf[rd++];
                                }
                                if (rd < blen) {        // copy the newline
                                    buf[wr++] = buf[rd++];
                                }
                            }
                            blen = wr;
                            buf[blen] = '\0';
                        }
                    }

                    Token* tok = create_token(TOKEN_STRING_LITERAL, buf, start_line, current_column);
                    free(buf);
                    return tok;
                }
                return create_token(TOKEN_LSHIFT, "<<", current_line, current_column);
            }
            return create_token(TOKEN_LESS, "<", current_line, current_column);
        case '>':
            advance();
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_GREATER_EQUAL, ">=", current_line, current_column);
            }
            if (peek() == '>') {
                advance();
                if (peek() == '=') {
                    advance();
                    return create_token(TOKEN_RSHIFT_ASSIGN, ">>=", current_line, current_column);
                }
                return create_token(TOKEN_RSHIFT, ">>", current_line, current_column);
            }
            return create_token(TOKEN_GREATER, ">", current_line, current_column);
        case '&':
            advance();
            if (peek() == '&') {
                advance();
                return create_token(TOKEN_AND, "&&", current_line, current_column);
            }
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_AND_ASSIGN, "&=", current_line, current_column);
            }
            return create_token(TOKEN_AMPERSAND, "&", current_line, current_column);
        case '|':
            advance();
            if (peek() == '|') {
                advance();
                return create_token(TOKEN_OR, "||", current_line, current_column);
            }
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_OR_ASSIGN, "|=", current_line, current_column);
            }
            return create_token(TOKEN_PIPE, "|", current_line, current_column);
        case '^':
            advance();
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_XOR_ASSIGN, "^=", current_line, current_column);
            }
            return create_token(TOKEN_CARET, "^", current_line, current_column);
        case '~': advance(); return create_token(TOKEN_TILDE, "~", current_line, current_column);
        case '(': advance(); return create_token(TOKEN_LEFT_PAREN, "(", current_line, current_column);
        case ')': advance(); return create_token(TOKEN_RIGHT_PAREN, ")", current_line, current_column);
        case '{': advance(); return create_token(TOKEN_LEFT_BRACE, "{", current_line, current_column);
        case '}': advance(); return create_token(TOKEN_RIGHT_BRACE, "}", current_line, current_column);
        case '[': advance(); return create_token(TOKEN_LEFT_BRACKET, "[", current_line, current_column);
        case ']': advance(); return create_token(TOKEN_RIGHT_BRACKET, "]", current_line, current_column);
        case ';': advance(); return create_token(TOKEN_SEMICOLON, ";", current_line, current_column);
        case ',': advance(); return create_token(TOKEN_COMMA, ",", current_line, current_column);
        case '.':
            advance();
            if (peek() == '.') {
                advance();
                if (peek() == '.') {
                    advance();
                    return create_token(TOKEN_DOTDOTDOT, "...", current_line, current_column);
                }
                /* #1047: `..=` inclusive and `..<` half-open range operators for
                 * match/switch case labels. Plain `..` stays exclusive (for). */
                if (peek() == '=') {
                    advance();
                    return create_token(TOKEN_DOTDOT_EQ, "..=", current_line, current_column);
                }
                if (peek() == '<') {
                    advance();
                    return create_token(TOKEN_DOTDOT_LT, "..<", current_line, current_column);
                }
                return create_token(TOKEN_DOTDOT, "..", current_line, current_column);
            }
            return create_token(TOKEN_DOT, ".", current_line, current_column);
        case ':': advance(); return create_token(TOKEN_COLON, ":", current_line, current_column);
        case '?':
            advance();
            // #340: `??` null-coalesce, `?.` optional-chain. A lone `?` stays
            // the actor-ask operator.
            if (peek() == '?') { advance(); return create_token(TOKEN_QUESTION_QUESTION, "??", current_line, current_column); }
            if (peek() == '.') { advance(); return create_token(TOKEN_QUESTION_DOT, "?.", current_line, current_column); }
            return create_token(TOKEN_QUESTION, "?", current_line, current_column);
        case '@': advance(); return create_token(TOKEN_AT, "@", current_line, current_column);
        default: {
            advance();
            char err_ch[2] = { c, '\0' };
            return create_token(TOKEN_ERROR, err_ch, current_line, current_column);
        }
    }
}

Token* create_token(AeTokenType type, const char* value, int line, int column) {
    Token* token = malloc(sizeof(Token));
    if (!token) return NULL;
    token->type = type;
    token->line = line;
    token->column = column;
    if (value) {
        size_t len = strlen(value);
        token->value = malloc(len + 1);
        if (!token->value) { free(token); return NULL; }
        memcpy(token->value, value, len + 1);
    } else {
        token->value = NULL;
    }
    return token;
}

void free_token(Token* token) {
    if (token) {
        if (token->value) {
            free(token->value);
        }
        free(token);
    }
}

const char* token_type_to_string(AeTokenType type) {
    switch (type) {
        case TOKEN_ACTOR: return "ACTOR";
        case TOKEN_MAIN: return "MAIN";
        case TOKEN_FUNC: return "FUNC";
        case TOKEN_LET: return "LET";
        case TOKEN_VAR: return "VAR";
        case TOKEN_IF: return "IF";
        case TOKEN_ELSE: return "ELSE";
        case TOKEN_FOR: return "FOR";
        case TOKEN_WHILE: return "WHILE";
        case TOKEN_SWITCH: return "SWITCH";
        case TOKEN_CASE: return "CASE";
        case TOKEN_DEFAULT: return "DEFAULT";
        case TOKEN_BREAK: return "BREAK";
        case TOKEN_CONTINUE: return "CONTINUE";
        case TOKEN_RETURN: return "RETURN";
        case TOKEN_BUILDER: return "BUILDER";
        case TOKEN_MATCH: return "MATCH";
        case TOKEN_WHEN: return "WHEN";
        case TOKEN_RECEIVE: return "RECEIVE";
        case TOKEN_SEND: return "SEND";
        case TOKEN_SPAWN_ACTOR: return "SPAWN_ACTOR";
        case TOKEN_SPAWN: return "SPAWN";
        case TOKEN_SELF: return "SELF";
        case TOKEN_STATE: return "STATE";
        case TOKEN_STRUCT: return "STRUCT";
        case TOKEN_UNION: return "UNION";
        case TOKEN_IMPORT: return "IMPORT";
        case TOKEN_AS: return "AS";
        case TOKEN_EXPORT: return "EXPORT";
        case TOKEN_EXPORTS: return "EXPORTS";
        case TOKEN_MODULE: return "MODULE";
        case TOKEN_MESSAGE_KEYWORD: return "MESSAGE_KEYWORD";
        case TOKEN_REPLY: return "REPLY";
        case TOKEN_INT: return "INT";
        case TOKEN_FLOAT: return "FLOAT";
        case TOKEN_BOOL: return "BOOL";
        case TOKEN_BYTE: return "BYTE";
        case TOKEN_STRING: return "STRING";
        case TOKEN_ACTOR_REF: return "ACTOR_REF";
        case TOKEN_MESSAGE: return "MESSAGE";
        case TOKEN_IDENTIFIER: return "IDENTIFIER";
        case TOKEN_NUMBER: return "NUMBER";
        case TOKEN_STRING_LITERAL: return "STRING_LITERAL";
        case TOKEN_TRUE: return "TRUE";
        case TOKEN_FALSE: return "FALSE";
        case TOKEN_PLUS: return "PLUS";
        case TOKEN_MINUS: return "MINUS";
        case TOKEN_MULTIPLY: return "MULTIPLY";
        case TOKEN_DIVIDE: return "DIVIDE";
        case TOKEN_MODULO: return "MODULO";
        case TOKEN_ASSIGN: return "ASSIGN";
        case TOKEN_EQUALS: return "EQUALS";
        case TOKEN_NOT_EQUALS: return "NOT_EQUALS";
        case TOKEN_LESS: return "LESS";
        case TOKEN_LESS_EQUAL: return "LESS_EQUAL";
        case TOKEN_GREATER: return "GREATER";
        case TOKEN_GREATER_EQUAL: return "GREATER_EQUAL";
        case TOKEN_AND: return "AND";
        case TOKEN_OR: return "OR";
        case TOKEN_NOT: return "NOT";
        case TOKEN_INCREMENT: return "INCREMENT";
        case TOKEN_DECREMENT: return "DECREMENT";
        case TOKEN_AMPERSAND: return "AMPERSAND";
        case TOKEN_CARET: return "CARET";
        case TOKEN_TILDE: return "TILDE";
        case TOKEN_LSHIFT: return "LSHIFT";
        case TOKEN_RSHIFT: return "RSHIFT";
        case TOKEN_PLUS_ASSIGN: return "PLUS_ASSIGN";
        case TOKEN_MINUS_ASSIGN: return "MINUS_ASSIGN";
        case TOKEN_MULTIPLY_ASSIGN: return "MULTIPLY_ASSIGN";
        case TOKEN_DIVIDE_ASSIGN: return "DIVIDE_ASSIGN";
        case TOKEN_MODULO_ASSIGN: return "MODULO_ASSIGN";
        case TOKEN_AND_ASSIGN: return "AND_ASSIGN";
        case TOKEN_OR_ASSIGN: return "OR_ASSIGN";
        case TOKEN_XOR_ASSIGN: return "XOR_ASSIGN";
        case TOKEN_LSHIFT_ASSIGN: return "LSHIFT_ASSIGN";
        case TOKEN_RSHIFT_ASSIGN: return "RSHIFT_ASSIGN";
        case TOKEN_NULL: return "NULL";
        case TOKEN_CALLBACK: return "CALLBACK";
        case TOKEN_IN: return "IN";
        case TOKEN_DOTDOT: return "DOTDOT";
        case TOKEN_DOTDOT_EQ: return "DOTDOT_EQ";
        case TOKEN_DOTDOT_LT: return "DOTDOT_LT";
        case TOKEN_DOTDOTDOT: return "DOTDOTDOT";
        case TOKEN_CONST: return "CONST";
        case TOKEN_TRY: return "TRY";
        case TOKEN_CATCH: return "CATCH";
        case TOKEN_PANIC: return "PANIC";
        case TOKEN_REQUIRES: return "REQUIRES";
        case TOKEN_ENSURES: return "ENSURES";
        case TOKEN_LEFT_PAREN: return "LEFT_PAREN";
        case TOKEN_RIGHT_PAREN: return "RIGHT_PAREN";
        case TOKEN_LEFT_BRACE: return "LEFT_BRACE";
        case TOKEN_RIGHT_BRACE: return "RIGHT_BRACE";
        case TOKEN_LEFT_BRACKET: return "LEFT_BRACKET";
        case TOKEN_RIGHT_BRACKET: return "RIGHT_BRACKET";
        case TOKEN_SEMICOLON: return "SEMICOLON";
        case TOKEN_COMMA: return "COMMA";
        case TOKEN_DOT: return "DOT";
        case TOKEN_COLON: return "COLON";
        case TOKEN_ARROW: return "ARROW";
        case TOKEN_PIPE: return "PIPE";
        case TOKEN_AT: return "AT";
        case TOKEN_EXCLAIM: return "EXCLAIM";
        case TOKEN_QUESTION: return "QUESTION";
        case TOKEN_QUESTION_QUESTION: return "QUESTION_QUESTION";
        case TOKEN_QUESTION_DOT: return "QUESTION_DOT";
        case TOKEN_PRINT: return "PRINT";
        case TOKEN_EOF: return "EOF";
        case TOKEN_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}
